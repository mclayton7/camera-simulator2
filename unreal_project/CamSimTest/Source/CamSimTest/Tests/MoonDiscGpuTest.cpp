// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "RHI.h"
#include "UObject/StrongObjectPtr.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "ShaderCompiler.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Environment/CelestialEnables.h"
#include "Environment/Ephemeris.h"
#include "Environment/MoonSurface.h"
#include "Environment/NightIllumination.h"
#include "Environment/SkyDomeDriver.h"

// CamSim.GPU.Sky.MoonDiscMatchesCpu (6A fix round): M_SkyDome's moon disc (CamSimMoonDisc in CamSimSky.ush, the albedo
// texture, MPC_Sky) against its CPU mirror FMoonDisc::Radiance. A transient world holds only the sky dome; a 0.6 deg
// scene capture looks at a gibbous moon (phase 50) at the zenith (constant transmittance) and records HDR scene colour.
// The rendered disc must match LumScale x Radiance x T pixel for pixel, up to one exposure scale (fitted and reported),
// within 3 % rms of the disc's mean inside 0.95 R; a mirrored or rotated albedo map would differ by ~30 %.

namespace
{
	constexpr int32 CapturePx = 512;
	constexpr double FovDeg = 0.6;

	struct FTransientWorld
	{
		UWorld* World = nullptr;
		FTransientWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld=*/false, TEXT("CamSimMoonDiscGpuTest"));
			FWorldContext& Ctx = GEngine->CreateNewWorldContext(EWorldType::Game);
			Ctx.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
		}
		~FTransientWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
			CollectGarbage(RF_NoFlags, true);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoonDiscGpuMatchesCpuTest, "CamSim.GPU.Sky.MoonDiscMatchesCpu",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMoonDiscGpuMatchesCpuTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI)
	{
		AddInfo(TEXT("skipped: NullRHI (run scripts/run_gpu_tests.sh)"));
		return true;
	}
	const TStrongObjectPtr<UMaterialInterface> Material(LoadObject<UMaterialInterface>(nullptr, FSkyDomeDriver::MaterialPath));
	const TStrongObjectPtr<UMaterialParameterCollection> Mpc(LoadObject<UMaterialParameterCollection>(nullptr, FSkyDomeDriver::MpcPath));
	if (!TestNotNull(TEXT("M_SkyDome loaded (scripts/sky/make_sky_material.sh)"), Material.Get())) return false;
	if (!TestNotNull(TEXT("MPC_Sky loaded"), Mpc.Get())) return false;

	FTransientWorld Tw;   // MPC loaded first: the world creates its MPC instances at init
	UWorld* World = Tw.World;
	if (!TestNotNull(TEXT("world"), World) || !TestNotNull(TEXT("world scene"), World->Scene)) return false;

	AActor* Owner = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("owner actor"), Owner)) return false;
	USceneComponent* Root = NewObject<USceneComponent>(Owner, TEXT("Root"));
	Owner->SetRootComponent(Root);
	Root->RegisterComponent();

	// The sky dome as the environment drives it: identity East-South-Up frame at the origin.
	FSkyDomeDriver Driver;
	FCamSimConfig::FCelestialConfig Cfg;
	Driver.Init(Owner, Cfg);
	if (!TestTrue(TEXT("dome initialised"), Driver.IsInitialized())) return false;
	if (Driver.GetMoonAlbedo().IsEmpty()) AddWarning(TEXT("moon_albedo.pgm not loaded: a uniform disc is compared"));

	FEphemerisResult E;
	const double Alpha = 50.0;
	E.MoonEnu = FVector3d(0, 0, 1);
	E.SunEnu = FVector3d(FMath::Sin(FMath::DegreesToRadians(180.0 - Alpha)), 0.0, FMath::Cos(FMath::DegreesToRadians(180.0 - Alpha)));
	E.MoonElDeg = 90.0;
	E.SunElDeg = 90.0 - (180.0 - Alpha);
	E.MoonPhaseAngleDeg = Alpha;
	E.MoonDistanceKm = 384400.0;
	E.LocalSiderealDeg = 100.0;
	FNightIlluminationResult I;
	I.MoonToaLux = 0.1;
	I.FloorRadianceCdM2 = 0.0;
	FCelestialEnables En;
	En.bMoon = true;
	En.bStars = false;
	En.StarScale = 0.0f;
	const FMatrix Esu = FMatrix::Identity;
	const double LatDeg = 33.0, Centuries = 0.268;
	Driver.Update(E, I, En, Esu, FVector::ZeroVector, LatDeg, Centuries);

	// CPU mirror of the same frame.
	FVector Rows[3];
	FSkyDomeDriver::SkyRotationRows(Esu, LatDeg, E.LocalSiderealDeg, Centuries, Rows);
	const FMoonDiscFrame Frame = FSkyDomeDriver::MoonFrame(E, Esu, Rows, FSkyDomeDriver::DaysTtFromCenturiesUt(Centuries));
	const double Radius = FMath::Asin(FSkyDomeDriver::MoonRadiusKm / E.MoonDistanceKm);
	const double Omega = 2.0 * UE_DOUBLE_PI * (1.0 - FMath::Cos(Radius));
	const double LumScale = I.MoonToaLux / (Omega * FMoonDisc::MeanRadiance(Frame, Driver.GetMoonAlbedo()));
	const double T = FNightIllumination::Transmittance(90.0);
	FVector3d Right, Up;
	FMoonDisc::DiscBasis(Frame, Right, Up);

	const TStrongObjectPtr<UTextureRenderTarget2D> Target(NewObject<UTextureRenderTarget2D>(GetTransientPackage()));
	Target->RenderTargetFormat = RTF_RGBA32f;
	Target->ClearColor = FLinearColor::Black;
	Target->InitAutoFormat(CapturePx, CapturePx);
	Target->UpdateResourceImmediate(true);

	const FVector MoonWorld = FVector(Frame.MoonDir);
	ASceneCapture2D* CapActor = World->SpawnActor<ASceneCapture2D>(FVector::ZeroVector, FRotationMatrix::MakeFromXZ(MoonWorld, FVector(1, 0, 0)).Rotator());
	if (!TestNotNull(TEXT("capture actor"), CapActor)) return false;
	USceneCaptureComponent2D* Cap = CapActor->GetCaptureComponent2D();
	Cap->FOVAngle = float(FovDeg);
	Cap->CaptureSource = ESceneCaptureSource::SCS_SceneColorHDR;
	Cap->bCaptureEveryFrame = false;
	Cap->bCaptureOnMovement = false;
	Cap->TextureTarget = Target.Get();
	Cap->ShowFlags.SetTemporalAA(false);
	Cap->ShowFlags.SetAntiAliasing(false);
	Cap->ShowFlags.SetMotionBlur(false);
	Cap->ShowFlags.SetEyeAdaptation(false);
	Cap->ShowFlags.SetBloom(false);

	Cap->CaptureScene();
	FlushRenderingCommands();
	if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
	Cap->CaptureScene();
	FlushRenderingCommands();

	TArray<FVector4f> Pixels;
	FTextureRenderTargetResource* Res = Target->GameThread_GetRenderTargetResource();
	if (!TestNotNull(TEXT("render target resource"), Res)) return false;
	ENQUEUE_RENDER_COMMAND(CamSimMoonDiscGpuTestReadback)([Res, &Pixels](FRHICommandListImmediate& RHICmdList)
	{
		FRHIGPUTextureReadback Rb(TEXT("CamSimTestMoonDisc"));
		Rb.EnqueueCopy(RHICmdList, Res->GetRenderTargetTexture());
		RHICmdList.SubmitAndBlockUntilGPUIdle();
		if (!Rb.IsReady()) return;
		int32 Pitch = 0;
		const FVector4f* Data = static_cast<const FVector4f*>(Rb.Lock(Pitch));
		Pixels.SetNumUninitialized(CapturePx * CapturePx);
		for (int32 Y = 0; Y < CapturePx; ++Y) FMemory::Memcpy(&Pixels[Y * CapturePx], Data + static_cast<int64>(Y) * Pitch, CapturePx * sizeof(FVector4f));
		Rb.Unlock();
	});
	FlushRenderingCommands();
	if (!TestEqual(TEXT("readback pixel count"), Pixels.Num(), CapturePx * CapturePx)) return false;

	// Pixel centre -> view direction through the capture's own basis.
	const FVector Fwd = Cap->GetForwardVector(), CRight = Cap->GetRightVector(), CUp = Cap->GetUpVector();
	const double TanHalf = FMath::Tan(FMath::DegreesToRadians(FovDeg * 0.5));
	TArray<double> Gpu, Cpu;
	for (int32 Y = 0; Y < CapturePx; ++Y)
	{
		for (int32 X = 0; X < CapturePx; ++X)
		{
			const double Nx = ((X + 0.5) / CapturePx) * 2.0 - 1.0, Ny = 1.0 - ((Y + 0.5) / CapturePx) * 2.0;
			const FVector3d V = (FVector3d(Fwd) + Nx * TanHalf * FVector3d(CRight) + Ny * TanHalf * FVector3d(CUp)).GetSafeNormal();
			const double U = V.Dot(Right) / FMath::Sin(Radius), W = V.Dot(Up) / FMath::Sin(Radius);
			if (U * U + W * W > 0.95 * 0.95) continue;   // the limb's pixels straddle the disc edge
			const FVector4f& P = Pixels[Y * CapturePx + X];
			Gpu.Add(0.2126 * P.X + 0.7152 * P.Y + 0.0722 * P.Z);   // the dome writes a grey disc: any channel
			Cpu.Add(LumScale * FMoonDisc::Radiance(Frame, Driver.GetMoonAlbedo(), U, W) * T);
		}
	}
	double Sgc = 0.0, Scc = 0.0, MeanC = 0.0;
	for (int32 K = 0; K < Gpu.Num(); ++K) { Sgc += Gpu[K] * Cpu[K]; Scc += Cpu[K] * Cpu[K]; MeanC += Cpu[K]; }
	if (!TestTrue(TEXT("disc pixels in frame"), Gpu.Num() > 1000 && Scc > 0.0)) return false;
	MeanC /= Gpu.Num();
	const double Scale = Sgc / Scc;   // the capture's exposure (1 when the scene capture applies none)
	double Sse = 0.0, MaxErr = 0.0;
	for (int32 K = 0; K < Gpu.Num(); ++K)
	{
		const double Err = Gpu[K] / Scale - Cpu[K];
		Sse += Err * Err;
		MaxErr = FMath::Max(MaxErr, FMath::Abs(Err));
	}
	const double Rms = FMath::Sqrt(Sse / Gpu.Num()) / MeanC;
	AddInfo(FString::Printf(TEXT("%d disc pixels; GPU / CPU scale %.4f; rms error %.2f %% of the disc mean, max %.1f %%"),
		Gpu.Num(), Scale, 100.0 * Rms, 100.0 * MaxErr / MeanC));
	TestTrue(*FString::Printf(TEXT("GPU disc = CPU mirror within 3 %% rms (%.2f %%)"), 100.0 * Rms), Rms < 0.03);
	TestTrue(*FString::Printf(TEXT("absolute level: GPU / CPU %.4f within 2 %% (LumScale x T, no capture exposure)"), Scale),
		FMath::Abs(Scale - 1.0) < 0.02);
	return true;
}
