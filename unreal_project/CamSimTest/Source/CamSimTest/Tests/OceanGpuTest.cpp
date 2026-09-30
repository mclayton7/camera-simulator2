// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "RHI.h"
#include "RenderingThread.h"
#include "ShaderCompiler.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Ocean/FOceanManager.h"
#include "Ocean/OceanMesh.h"
#include "Ocean/OceanMeshBuilder.h"
#include "Ocean/OceanSurface.h"
#include "Ocean/OceanWaves.h"
#include "Geospatial/EcefFrames.h"

// CamSim.GPU.Ocean.MatchesCpu: M_Ocean's world-position offset (CamSimOcean.ush) against the CPU
// wave model FOceanWaves. A transient world holds only the ocean mesh; an orthographic scene
// capture straight down records scene depth, and the rendered surface height at a 9x9 pixel grid
// must match FOceanWaves::HeightAtPlane at the same world XY within 2 cm.

namespace
{
	constexpr int32  CapturePx     = 128;
	constexpr double CameraHeightM = 100.0;
	constexpr double OrthoWidthM   = 100.0;

	/**
	 * ECEF (m) -> UE (cm) with the anchor at the origin and UE X = North, Y = East, Z = Up.
	 * (N, E, Up) is a proper rotation of Cesium's (East, South, Up) layout (90 deg about Up),
	 * so the mesh keeps its front faces up (CamSim.Ocean.Mesh.FrontFaceUp).
	 * FMatrix is row-vector: TransformPosition(P) = P * M, so row i holds ECEF axis i's image.
	 */
	FMatrix MakeEcefToUe(const FOceanWaves& W)
	{
		const FVector N = W.GetAxisNorthEcef(), E = W.GetAxisEastEcef(), U = W.GetAxisUpEcef();
		const FVector A = W.GetAnchorEcef();
		FMatrix M;
		for (int32 i = 0; i < 3; ++i)
		{
			M.M[i][0] = N[i] * 100.0; M.M[i][1] = E[i] * 100.0; M.M[i][2] = U[i] * 100.0; M.M[i][3] = 0.0;
		}
		M.M[3][0] = -(A | N) * 100.0; M.M[3][1] = -(A | E) * 100.0; M.M[3][2] = -(A | U) * 100.0; M.M[3][3] = 1.0;
		return M;
	}

	/** Owns a transient game world for the test's lifetime. */
	struct FTransientWorld
	{
		UWorld* World = nullptr;
		FTransientWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld=*/false, TEXT("CamSimOceanGpuTest"));
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanGpuMatchesCpuTest, "CamSim.GPU.Ocean.MatchesCpu",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOceanGpuMatchesCpuTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI)
	{
		AddInfo(TEXT("skipped: NullRHI (run scripts/run_gpu_tests.sh)"));
		return true;
	}

	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Ocean/M_Ocean.M_Ocean"));
	UMaterialParameterCollection* Mpc = LoadObject<UMaterialParameterCollection>(nullptr, TEXT("/Game/Ocean/MPC_Ocean.MPC_Ocean"));
	if (!TestNotNull(TEXT("M_Ocean loaded (scripts/ocean/make_ocean_material.sh)"), Material)) return false;
	if (!TestNotNull(TEXT("MPC_Ocean loaded"), Mpc)) return false;

	// The sea: constant geoid 0, anchor (0, 0), one host wave H 2 m, L 40 m, from 180 deg, t = 3 s.
	FOceanSurface Ocean([](double, double) { return TOptional<double>(0.0); });
	FOceanWave Wave;
	Wave.HeightM = 2.0; Wave.LengthM = 40.0; Wave.FromDeg = 180.0;
	Ocean.SetHostWave(0, Wave);
	Ocean.SetAnchor(0.0, 0.0);
	Ocean.SetTime(3.0);
	const FOceanWaves& W = Ocean.GetWaves();
	if (!TestEqual(TEXT("one wave"), W.GetWaves().Num(), 1)) return false;
	const FMatrix EcefToUe = MakeEcefToUe(W);

	FTransientWorld Tw;   // MPC loaded first: the world creates its MPC instances at init
	UWorld* World = Tw.World;
	if (!TestNotNull(TEXT("world"), World) || !TestNotNull(TEXT("world scene"), World->Scene)) return false;

	// Mesh: R = 200 m is a uniform 1.5625 m grid, well under lambda/8 = 5 m (no fade).
	CamSimOcean::FOceanMeshData Data;
	auto GeoToWorld = [&EcefToUe](double Lat, double Lon, double Alt)
		{ return EcefToUe.TransformPosition(CamSimFrames::GeodeticToEcef(Lat, Lon, Alt)); };
	if (!TestTrue(TEXT("mesh built"), CamSimOcean::BuildOceanMesh(0.0, 0.0, 200.0, Ocean, GeoToWorld, Data))) return false;
	AddInfo(FString::Printf(TEXT("mesh alpha %.3g, origin (%.3f, %.3f, %.3f) cm"), Data.Alpha,
		Data.OriginWorld.X, Data.OriginWorld.Y, Data.OriginWorld.Z));

	AActor* Owner = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("owner actor"), Owner)) return false;
	FOceanMesh Mesh;
	Mesh.Init(Owner, Material);
	Mesh.Upload(Data);
	Mesh.SetDisplacementPadding(3.0 * W.Amplitude(0) + 10.0);
	CamSimOcean::WriteMpc(World, Mpc, Ocean, Mesh.GetWorldLocation(), EcefToUe);

	// Depth target and an orthographic capture straight down from 100 m, 100 m wide.
	UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
	Target->RenderTargetFormat = RTF_R32f;
	Target->ClearColor = FLinearColor(1e9f, 0.f, 0.f, 1.f);
	Target->InitAutoFormat(CapturePx, CapturePx);
	Target->UpdateResourceImmediate(true);

	ASceneCapture2D* CapActor = World->SpawnActor<ASceneCapture2D>(FVector(0.0, 0.0, CameraHeightM * 100.0), FRotator(-90.0, 0.0, 0.0));
	if (!TestNotNull(TEXT("capture actor"), CapActor)) return false;
	USceneCaptureComponent2D* Cap = CapActor->GetCaptureComponent2D();
	Cap->ProjectionType = ECameraProjectionMode::Orthographic;
	Cap->OrthoWidth = float(OrthoWidthM * 100.0);
	Cap->bAutoCalculateOrthoPlanes = false;   // these move the ortho view origin, which would bias depth
	Cap->bUpdateOrthoPlanes = false;
	Cap->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;
	Cap->bCaptureEveryFrame = false;
	Cap->bCaptureOnMovement = false;
	Cap->TextureTarget = Target;
	// No sub-pixel jitter: a 0.78 m pixel shifted by half a pixel is several cm of wave height.
	Cap->ShowFlags.SetTemporalAA(false);
	Cap->ShowFlags.SetAntiAliasing(false);
	Cap->ShowFlags.SetMotionBlur(false);

	// First capture requests M_Ocean's shaders for this vertex factory; wait for them, then capture
	// for real (a still-compiling material renders as the default material, without WPO).
	Cap->CaptureScene();
	FlushRenderingCommands();
	if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
	Cap->CaptureScene();
	FlushRenderingCommands();

	TArray<FLinearColor> Pixels;
	FTextureRenderTargetResource* Res = Target->GameThread_GetRenderTargetResource();
	if (!TestNotNull(TEXT("render target resource"), Res)) return false;
	if (!TestTrue(TEXT("read depth"), Res->ReadLinearColorPixels(Pixels)) || !TestEqual(TEXT("pixel count"), Pixels.Num(), CapturePx * CapturePx))
	{
		return false;
	}

	// Pixel centre -> world: camera right = +Y, camera up = +X (pitch -90, yaw 0), rows go down.
	const double PxM = OrthoWidthM / CapturePx;
	double MaxDiff = 0.0, SumDiff = 0.0, MinH = 1e9, MaxH = -1e9;
	int32 Count = 0;
	FString Worst;
	for (int32 i = 0; i < 9; ++i)
	for (int32 j = 0; j < 9; ++j)
	{
		const int32 Row = 8 + 14 * i, Col = 8 + 14 * j;
		const double X = (CapturePx * 0.5 - (Row + 0.5)) * PxM;   // North, m
		const double Y = ((Col + 0.5) - CapturePx * 0.5) * PxM;   // East, m
		const double DepthCm = Pixels[Row * CapturePx + Col].R;
		const double Rendered = CameraHeightM - DepthCm / 100.0;
		const double Expected = W.HeightAtPlane(X, Y);
		const double Diff = Rendered - Expected;
		SumDiff += Diff; ++Count;
		MinH = FMath::Min(MinH, Rendered); MaxH = FMath::Max(MaxH, Rendered);
		if (FMath::Abs(Diff) > MaxDiff || !FMath::IsFinite(Diff))
		{
			MaxDiff = FMath::IsFinite(Diff) ? FMath::Abs(Diff) : 1e9;
			Worst = FString::Printf(TEXT("px (%d, %d) N %.2f E %.2f: rendered %.4f m, CPU %.4f m"), Col, Row, X, Y, Rendered, Expected);
		}
	}
	AddInfo(FString::Printf(TEXT("max |diff| %.4f m (mean diff %.4f m, rendered range %.3f..%.3f m); worst %s"),
		MaxDiff, SumDiff / Count, MinH, MaxH, *Worst));
	TestTrue(*FString::Printf(TEXT("GPU surface within 2 cm of FOceanWaves (max |diff| %.4f m)"), MaxDiff), MaxDiff <= 0.02);
	return true;
}
