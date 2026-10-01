// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "InstanceIdPass.h"
#include "SensorFrameParams.h"
#include "Sensor/SensorOptics.h"

// CamSim.GPU.GroundTruth.InstanceId.*: InstanceIdCS (CamSimInstanceId.usf) on synthetic depth/stencil
// textures. Each output pixel holds visible | amodal << 8 (uint16, two per uint32, pixel 2k in the low half);
// its source texel follows the sensor's distortion resample (CamSimSensorRef::Optics).

namespace
{
	/** Synthetic depth/stencil inputs: Extent texels; stencil replicated in all four channels. */
	struct FIdTextures
	{
		FIntPoint Extent = FIntPoint::ZeroValue;
		TArray<float> SceneDepth;
		TArray<float> CustomDepth;
		TArray<uint8> Stencil;

		FIdTextures(int32 W, int32 H, float SceneZ)
		{
			Extent = FIntPoint(W, H);
			SceneDepth.Init(SceneZ, W * H);
			CustomDepth.Init(0.0f, W * H);
			Stencil.Init(0, W * H);
		}
		/** A tagged-entity texel: stencil Id, custom depth CustomZ, scene depth SceneZ. */
		void Tag(int32 X, int32 Y, uint8 Id, float CustomZ, float SceneZ)
		{
			const int32 I = Y * Extent.X + X;
			Stencil[I] = Id; CustomDepth[I] = CustomZ; SceneDepth[I] = SceneZ;
		}
	};

	FTextureRHIRef UploadFloat(FRHICommandListImmediate& RHICmdList, const TArray<float>& Texels, FIntPoint Extent, const TCHAR* Name)
	{
		const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(Name, Extent.X, Extent.Y, PF_R32_FLOAT)
			.SetFlags(ETextureCreateFlags::ShaderResource).SetInitialState(ERHIAccess::SRVMask);
		FTextureRHIRef Tex = RHICmdList.CreateTexture(Desc);
		RHICmdList.UpdateTexture2D(Tex, 0, FUpdateTextureRegion2D(0, 0, 0, 0, Extent.X, Extent.Y), Extent.X * sizeof(float),
			reinterpret_cast<const uint8*>(Texels.GetData()));
		return Tex;
	}

	/** PF_R8G8B8A8_UINT with the stencil value in every channel: STENCIL_COMPONENT_SWIZZLE (.x on Metal/Vulkan,
	 *  .g elsewhere) reads it whichever channel it names. */
	FTextureRHIRef UploadStencil(FRHICommandListImmediate& RHICmdList, const TArray<uint8>& Ids, FIntPoint Extent)
	{
		const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(TEXT("CamSimTestStencil"), Extent.X, Extent.Y, PF_R8G8B8A8_UINT)
			.SetFlags(ETextureCreateFlags::ShaderResource).SetInitialState(ERHIAccess::SRVMask);
		FTextureRHIRef Tex = RHICmdList.CreateTexture(Desc);
		TArray<uint8> Rgba; Rgba.SetNumUninitialized(Ids.Num() * 4);
		for (int32 I = 0; I < Ids.Num(); ++I) { for (int32 C = 0; C < 4; ++C) Rgba[I * 4 + C] = Ids[I]; }
		RHICmdList.UpdateTexture2D(Tex, 0, FUpdateTextureRegion2D(0, 0, 0, 0, Extent.X, Extent.Y), Extent.X * 4, Rgba.GetData());
		return Tex;
	}

	/** The still-water cut's inputs (off by default). */
	struct FWaterCut
	{
		bool       bOn = false;
		FVector4f  Plane = FVector4f(0.0f, 0.0f, 1.0f, 0.0f);
		FMatrix44f ClipToTranslatedWorld = FMatrix44f::Identity;
	};

	/** Upload, run AddInstanceIdPass over Rect of the textures, read the packed ids back. Empty on failure. */
	TArray<uint32> RunInstanceIdOnGpu(const FIdTextures& Tex, FIntRect Rect, FIntPoint OutSize, const FSensorFrameParams& P,
		const FWaterCut& Water = FWaterCut())
	{
		TArray<uint32> Result;
		ENQUEUE_RENDER_COMMAND(CamSimInstanceIdGpuTest)([&](FRHICommandListImmediate& RHICmdList)
		{
			FTextureRHIRef SceneTex   = UploadFloat(RHICmdList, Tex.SceneDepth, Tex.Extent, TEXT("CamSimTestSceneDepth"));
			FTextureRHIRef CustomTex  = UploadFloat(RHICmdList, Tex.CustomDepth, Tex.Extent, TEXT("CamSimTestCustomDepth"));
			FTextureRHIRef StencilTex = UploadStencil(RHICmdList, Tex.Stencil, Tex.Extent);

			FRHIGPUBufferReadback Rb(TEXT("CamSimTestInstanceIds"));
			const uint32 Bytes = static_cast<uint32>(OutSize.X * OutSize.Y / 2) * sizeof(uint32);
			{
				FRDGBuilder GraphBuilder(RHICmdList);
				FInstanceIdInputs In;
				In.SceneDepth  = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(SceneTex, TEXT("CamSimTestSceneDepth")));
				In.CustomDepth = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(CustomTex, TEXT("CamSimTestCustomDepth")));
				FRDGTextureRef Stencil = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(StencilTex, TEXT("CamSimTestStencil")));
				In.CustomStencil = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(Stencil));
				In.DepthViewRect = Rect;
				In.OutputSize = OutSize;
				In.bWaterCut = Water.bOn;
				In.WaterPlane = Water.Plane;
				In.ClipToTranslatedWorld = Water.ClipToTranslatedWorld;
				const FRDGBufferRef Ids = AddInstanceIdPass(GraphBuilder, In, P);
				AddEnqueueCopyPass(GraphBuilder, &Rb, Ids, Bytes);
				GraphBuilder.Execute();
			}
			RHICmdList.SubmitAndBlockUntilGPUIdle();
			if (!Rb.IsReady()) return;
			Result.SetNumUninitialized(Bytes / sizeof(uint32));
			FMemory::Memcpy(Result.GetData(), Rb.Lock(Bytes), Bytes);
			Rb.Unlock();
		});
		FlushRenderingCommands();
		return Result;
	}

	/** Output pixel (X, Y)'s 16-bit value: visible | amodal << 8. */
	uint32 PixelOf(const TArray<uint32>& Words, int32 OutW, int32 X, int32 Y)
	{
		const uint32 W = Words[(Y * OutW + X) / 2];
		return (X % 2 == 0) ? (W & 0xFFFFu) : (W >> 16);
	}

	bool SkipWithoutGpu(FAutomationTestBase& T)
	{
		if (GUsingNullRHI) { T.AddInfo(TEXT("skipped: NullRHI (run scripts/run_gpu_tests.sh)")); return true; }
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstanceIdSyntheticTest, "CamSim.GPU.GroundTruth.InstanceId.Synthetic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FInstanceIdSyntheticTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutGpu(*this)) return true;
	constexpr int32 W = 8, H = 4;
	FIdTextures Tex(W, H, 0.1f);
	Tex.Tag(1, 1, 5, 0.5f, 0.5f);   // the entity is the front surface
	Tex.Tag(2, 1, 5, 0.5f, 0.9f);   // an occluder in front of it
	Tex.Tag(5, 2, 7, 0.5f, 0.5f);
	FSensorFrameParams P;   // FocalPx 0: optics off
	const TArray<uint32> Words = RunInstanceIdOnGpu(Tex, FIntRect(0, 0, W, H), FIntPoint(W, H), P);
	if (!TestEqual(TEXT("buffer is 16 words"), Words.Num(), 16)) return true;
	for (int32 Y = 0; Y < H; ++Y)
	{
		for (int32 X = 0; X < W; ++X)
		{
			uint32 Expect = 0;
			if (X == 1 && Y == 1) Expect = 5u | (5u << 8);
			if (X == 2 && Y == 1) Expect = 0u | (5u << 8);
			if (X == 5 && Y == 2) Expect = 7u | (7u << 8);
			TestEqual(*FString::Printf(TEXT("pixel (%d, %d)"), X, Y), PixelOf(Words, W, X, Y), Expect);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstanceIdViewRectTest, "CamSim.GPU.GroundTruth.InstanceId.ViewRectOffset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FInstanceIdViewRectTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutGpu(*this)) return true;
	constexpr int32 W = 8, H = 4;
	FIdTextures Tex(10, 6, 0.1f);
	Tex.Tag(1, 1, 3, 0.5f, 0.5f);   // the view rect's first texel -> output (0, 0)
	Tex.Tag(0, 0, 4, 0.5f, 0.5f);   // outside the view rect: must never appear
	FSensorFrameParams P;
	const TArray<uint32> Words = RunInstanceIdOnGpu(Tex, FIntRect(1, 1, 9, 5), FIntPoint(W, H), P);
	if (!TestEqual(TEXT("buffer is 16 words"), Words.Num(), 16)) return true;
	TestEqual(TEXT("pixel (0, 0) = texel (1, 1)"), PixelOf(Words, W, 0, 0), 3u | (3u << 8));
	for (int32 Y = 0; Y < H; ++Y)
	{
		for (int32 X = 0; X < W; ++X)
		{
			if (X == 0 && Y == 0) continue;
			TestEqual(*FString::Printf(TEXT("pixel (%d, %d) empty"), X, Y), PixelOf(Words, W, X, Y), 0u);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstanceIdDistortionTest, "CamSim.GPU.GroundTruth.InstanceId.DistortionMatchesSensor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FInstanceIdDistortionTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutGpu(*this)) return true;
	constexpr int32 W = 64, H = 48;
	const FIntPoint T(52, 40);
	FIdTextures Tex(W, H, 0.1f);
	Tex.Tag(T.X, T.Y, 9, 0.5f, 0.5f);
	FSensorFrameParams P;
	P.FocalPx = CamSimOptics::FocalPx(W, 60.0f);
	P.K1 = -0.2f;
	P.K2 = 0.05f;
	const TArray<uint32> Words = RunInstanceIdOnGpu(Tex, FIntRect(0, 0, W, H), FIntPoint(W, H), P);
	if (!TestEqual(TEXT("buffer size"), Words.Num(), W * H / 2)) return true;

	// The CPU source texel of each output pixel, with the expressions of CamSimSensorRef::Optics
	// (same size: scale 1).
	const float F = P.FocalPx, HalfW = 0.5f * W, HalfH = 0.5f * H, ScaleX = 1.0f, ScaleY = 1.0f;
	int32 Expected = 0, Mismatches = 0;
	for (int32 Py = 0; Py < H; ++Py)
	{
		for (int32 Px = 0; Px < W; ++Px)
		{
			const float Xd = (Px + 0.5f - HalfW) / F, Yd = (Py + 0.5f - HalfH) / F;
			const float Rd = FMath::Sqrt(Xd * Xd + Yd * Yd);
			float S = 1.0f;
			if (Rd > 0.0f)
			{
				float Ru;
				CamSimOptics::UndistortRadius(Rd, P.K1, P.K2, Ru);
				S = Ru / Rd;
			}
			const float Xu = Xd * S, Yu = Yd * S;
			const float Sx = (Xu * F + HalfW) * ScaleX - 0.5f, Sy = (Yu * F + HalfH) * ScaleY - 0.5f;
			bool bHitsT = false;
			if (Sx >= -0.5f && Sx <= W - 0.5f && Sy >= -0.5f && Sy <= H - 0.5f)
			{
				const int32 Tx = FMath::Clamp(FMath::FloorToInt32(Sx + 0.5f), 0, W - 1);
				const int32 Ty = FMath::Clamp(FMath::FloorToInt32(Sy + 0.5f), 0, H - 1);
				bHitsT = (Tx == T.X && Ty == T.Y);
			}
			const bool bGpu = (PixelOf(Words, W, Px, Py) >> 8) == 9u;
			Expected += bHitsT ? 1 : 0;
			if (bGpu != bHitsT)
			{
				++Mismatches;
				AddError(FString::Printf(TEXT("output (%d, %d): GPU amodal 9 = %d, CPU source texel is T = %d (Sx %.6f, Sy %.6f)"),
					Px, Py, bGpu ? 1 : 0, bHitsT ? 1 : 0, Sx, Sy));
			}
		}
	}
	AddInfo(FString::Printf(TEXT("%d output pixels sample texel (%d, %d); %d mismatches"), Expected, T.X, T.Y, Mismatches));
	TestTrue(TEXT("some output pixel samples T"), Expected > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstanceIdScaledDistortedTest, "CamSim.GPU.GroundTruth.InstanceId.ScaledDistortedMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FInstanceIdScaledDistortedTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutGpu(*this)) return true;
	// A 32x24 depth view rect offset inside a 40x30 texture, resampled to a 64x48 distorted output:
	// stencil encodes the rect-relative column (pass 0) then row (pass 1), so every output pixel's full
	// source texel (scale, offset, distortion, x/y) is pinned against the CPU mapping.
	constexpr int32 W = 64, H = 48;
	const FIntRect Rect(5, 3, 37, 27);
	const int32 SrcW = Rect.Width(), SrcH = Rect.Height();
	FSensorFrameParams P;
	P.FocalPx = CamSimOptics::FocalPx(W, 60.0f);
	P.K1 = -0.2f;
	P.K2 = 0.05f;
	// CamSimSensorRef::Optics's expressions (SrcScale = float(SrcW) / W, as AddSensorPasses).
	const float F = P.FocalPx, HalfW = 0.5f * W, HalfH = 0.5f * H;
	const float ScaleX = static_cast<float>(SrcW) / W, ScaleY = static_cast<float>(SrcH) / H;
	auto CpuTexel = [&](int32 Px, int32 Py, FIntPoint& OutT) -> bool
	{
		const float Xd = (Px + 0.5f - HalfW) / F, Yd = (Py + 0.5f - HalfH) / F;
		const float Rd = FMath::Sqrt(Xd * Xd + Yd * Yd);
		float S = 1.0f;
		if (Rd > 0.0f)
		{
			float Ru;
			CamSimOptics::UndistortRadius(Rd, P.K1, P.K2, Ru);
			S = Ru / Rd;
		}
		const float Xu = Xd * S, Yu = Yd * S;
		const float Sx = (Xu * F + HalfW) * ScaleX - 0.5f, Sy = (Yu * F + HalfH) * ScaleY - 0.5f;
		if (!(Sx >= -0.5f && Sx <= SrcW - 0.5f && Sy >= -0.5f && Sy <= SrcH - 0.5f)) return false;
		OutT = FIntPoint(FMath::Clamp(FMath::FloorToInt32(Sx + 0.5f), 0, SrcW - 1), FMath::Clamp(FMath::FloorToInt32(Sy + 0.5f), 0, SrcH - 1));
		return true;
	};

	for (int32 Axis = 0; Axis < 2; ++Axis)
	{
		FIdTextures Tex(40, 30, 0.1f);
		for (int32 Y = Rect.Min.Y; Y < Rect.Max.Y; ++Y)
		{
			for (int32 X = Rect.Min.X; X < Rect.Max.X; ++X)
			{
				const int32 V = (Axis == 0 ? X - Rect.Min.X : Y - Rect.Min.Y) + 1;
				Tex.Tag(X, Y, static_cast<uint8>(V), 0.5f, 0.5f);   // custom = scene: visible everywhere
			}
		}
		const TArray<uint32> Words = RunInstanceIdOnGpu(Tex, Rect, FIntPoint(W, H), P);
		if (!TestEqual(TEXT("buffer size"), Words.Num(), W * H / 2)) return true;
		int32 Mismatches = 0, InRange = 0;
		for (int32 Py = 0; Py < H; ++Py)
		{
			for (int32 Px = 0; Px < W; ++Px)
			{
				FIntPoint T;
				uint32 Expect = 0;
				if (CpuTexel(Px, Py, T))
				{
					const uint32 V = static_cast<uint32>((Axis == 0 ? T.X : T.Y) + 1);
					Expect = V | (V << 8);
					++InRange;
				}
				const uint32 Got = PixelOf(Words, W, Px, Py);
				if (Got != Expect && ++Mismatches <= 20)
				{
					AddError(FString::Printf(TEXT("%s pass, output (%d, %d): GPU 0x%04x, CPU 0x%04x"),
						Axis == 0 ? TEXT("column") : TEXT("row"), Px, Py, Got, Expect));
				}
			}
		}
		AddInfo(FString::Printf(TEXT("%s pass: %d in-range pixels, %d mismatches"), Axis == 0 ? TEXT("column") : TEXT("row"), InRange, Mismatches));
		TestEqual(*FString::Printf(TEXT("%s pass mismatches"), Axis == 0 ? TEXT("column") : TEXT("row")), Mismatches, 0);
		TestTrue(TEXT("most output pixels map inside the rect"), InRange > W * H / 2);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstanceIdWaterCutTest, "CamSim.GPU.GroundTruth.InstanceId.StillWaterCut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FInstanceIdWaterCutTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutGpu(*this)) return true;
	// Final review I2. A camera at the translated-world origin looking along +X (UE axes), reversed-Z infinite
	// perspective; a tagged wall 10 m ahead spans every row, so it straddles the still-water plane z = -Zcut,
	// put on the boundary between rows 15 and 16. Columns 8..15 are visible, 16..23 hidden behind an occluder.
	// Expected: hidden texels below the plane leave amodal; visible texels below it stay in both channels;
	// with the cut off nothing changes.
	constexpr int32 W = 32, H = 24, Split = 16;
	const double HalfFov = FMath::DegreesToRadians(30.0), MinZ = 10.0, D = 1000.0;
	const FMatrix UeToView(FPlane(0, 0, 1, 0), FPlane(1, 0, 0, 0), FPlane(0, 1, 0, 0), FPlane(0, 0, 0, 1));
	const FMatrix ViewProj = UeToView * FReversedZPerspectiveMatrix(HalfFov, double(W), double(H), MinZ);
	const FMatrix ClipToWorld = ViewProj.Inverse();
	const float WallZ = static_cast<float>(MinZ / D);   // device Z of a point D ahead (reversed, infinite far)
	const double TanY = FMath::Tan(HalfFov) * H / W;
	const double Zcut = -(1.0 - 2.0 * Split / H) * D * TanY;   // world z of the row-16 top edge (> 0 here)

	FIdTextures Tex(W, H, 0.0f);
	for (int32 Y = 0; Y < H; ++Y)
	{
		for (int32 X = 8; X < 24; ++X) Tex.Tag(X, Y, 5, WallZ, X < 16 ? WallZ : 0.5f);   // 0.5: occluder nearer
	}
	FSensorFrameParams P;
	FWaterCut Cut;
	Cut.bOn = true;
	Cut.Plane = FVector4f(0.0f, 0.0f, 1.0f, static_cast<float>(Zcut));   // height = z + Zcut
	Cut.ClipToTranslatedWorld = FMatrix44f(ClipToWorld);
	const TArray<uint32> On = RunInstanceIdOnGpu(Tex, FIntRect(0, 0, W, H), FIntPoint(W, H), P, Cut);
	const TArray<uint32> Off = RunInstanceIdOnGpu(Tex, FIntRect(0, 0, W, H), FIntPoint(W, H), P);
	if (!TestEqual(TEXT("buffer size (on)"), On.Num(), W * H / 2) || !TestEqual(TEXT("buffer size (off)"), Off.Num(), W * H / 2)) return true;

	int32 Dropped = 0, VisibleBelowKept = 0, Mismatches = 0;
	for (int32 Y = 0; Y < H; ++Y)
	{
		for (int32 X = 0; X < W; ++X)
		{
			// CPU reconstruction (doubles) agrees with the hard-coded split.
			const FVector4 Hc = ClipToWorld.TransformFVector4(FVector4((X + 0.5) / W * 2.0 - 1.0, 1.0 - (Y + 0.5) / H * 2.0, WallZ, 1.0));
			const bool bBelow = Hc.Z / Hc.W + Zcut < 0.0;
			TestEqual(*FString::Printf(TEXT("row %d below the plane on the CPU"), Y), bBelow, Y >= Split);
			const bool bTagged = X >= 8 && X < 24, bVisible = X < 16;
			const uint32 OffExpect = !bTagged ? 0u : ((bVisible ? 5u : 0u) | (5u << 8));
			const uint32 OnExpect = (bTagged && !bVisible && Y >= Split) ? 0u : OffExpect;
			Dropped += (OffExpect != 0 && OnExpect == 0) ? 1 : 0;
			VisibleBelowKept += (bTagged && bVisible && Y >= Split) ? 1 : 0;
			const uint32 GotOn = PixelOf(On, W, X, Y), GotOff = PixelOf(Off, W, X, Y);
			if ((GotOn != OnExpect || GotOff != OffExpect) && ++Mismatches <= 20)
			{
				AddError(FString::Printf(TEXT("pixel (%d, %d): cut on 0x%04x (want 0x%04x), off 0x%04x (want 0x%04x)"),
					X, Y, GotOn, OnExpect, GotOff, OffExpect));
			}
		}
	}
	AddInfo(FString::Printf(TEXT("%d hidden texels below still water dropped; %d visible ones below it kept; %d mismatches"),
		Dropped, VisibleBelowKept, Mismatches));
	TestEqual(TEXT("mismatches"), Mismatches, 0);
	TestEqual(TEXT("hidden-below texels"), Dropped, 8 * (H - Split));
	return true;
}
