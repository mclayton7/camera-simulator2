// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "SensorGraph.h"
#include "Sensor/SensorController.h"
#include "Sensor/SensorOptics.h"
#include "Sensor/SensorPresets.h"
#include "Sensor/SensorReference.h"
#include "Sensor/SensorTypes.h"

// CamSim.GPU.Sensor.*: the RDG sensor graph (CamSimSensor.usf) against the CPU reference
// CamSimSensorRef::Run. Noise is on: the PCG hash makes both noise fields bit-identical, so the
// only differences are float math (Metal fast-math ulps). Tolerances (binding): NV12 Y <= 1 DN,
// UV <= 2 DN; histogram totals equal and every bin within max(2, 0.001 N).

namespace
{
	struct FGpuResult { TArray<uint8> Nv12; FSensorHistogram Histogram; bool bOk = false; };

	/** A float RGBA texture of Extent texels, of which Rect is the view rect handed to the graph. */
	struct FGpuImage
	{
		TArray<FLinearColor> Texels;
		FIntPoint Extent = FIntPoint::ZeroValue;
		FIntRect  Rect;

		static FGpuImage Whole(const TArray<FLinearColor>& T, int32 W, int32 H)
		{
			FGpuImage I; I.Texels = T; I.Extent = FIntPoint(W, H); I.Rect = FIntRect(0, 0, W, H); return I;
		}
		/** The view rect's texels, row-major: what the reference sees. */
		TArray<FLinearColor> RectTexels() const
		{
			TArray<FLinearColor> Out;
			for (int32 Y = Rect.Min.Y; Y < Rect.Max.Y; ++Y)
				for (int32 X = Rect.Min.X; X < Rect.Max.X; ++X) Out.Add(Texels[Y * Extent.X + X]);
			return Out;
		}
	};

	FTextureRHIRef Upload(FRHICommandListImmediate& RHICmdList, const FGpuImage& I, const TCHAR* Name)
	{
		const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(Name, I.Extent.X, I.Extent.Y, PF_A32B32G32R32F)
			.SetFlags(ETextureCreateFlags::ShaderResource).SetInitialState(ERHIAccess::SRVMask);
		FTextureRHIRef Tex = RHICmdList.CreateTexture(Desc);
		RHICmdList.UpdateTexture2D(Tex, 0, FUpdateTextureRegion2D(0, 0, 0, 0, I.Extent.X, I.Extent.Y), I.Extent.X * sizeof(FLinearColor),
			reinterpret_cast<const uint8*>(I.Texels.GetData()));
		return Tex;
	}

	/** Upload Scene (+ Bloom), run AddSensorPasses, read back NV12 + histogram synchronously. */
	FGpuResult RunOnGpu(const FGpuImage& Scene, const FGpuImage* Bloom, FIntPoint OutSize, const FSensorFrameParams& P)
	{
		FGpuResult Result;
		ENQUEUE_RENDER_COMMAND(CamSimSensorGpuTest)([&](FRHICommandListImmediate& RHICmdList)
		{
			FTextureRHIRef SceneTex = Upload(RHICmdList, Scene, TEXT("CamSimTestScene"));
			FTextureRHIRef BloomTex = Bloom ? Upload(RHICmdList, *Bloom, TEXT("CamSimTestBloom")) : nullptr;

			FRHIGPUBufferReadback Nv12Rb(TEXT("CamSimTestNv12"));
			FRHIGPUBufferReadback HistRb(TEXT("CamSimTestHist"));
			uint32 Nv12Bytes = 0;
			{
				FRDGBuilder GraphBuilder(RHICmdList);
				FSensorGraphInputs In;
				In.SceneColor = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(SceneTex, TEXT("CamSimTestScene")));
				In.SceneViewRect = Scene.Rect;
				if (Bloom)
				{
					In.Bloom = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(BloomTex, TEXT("CamSimTestBloom")));
					In.BloomViewRect = Bloom->Rect;
				}
				In.OutputSize = OutSize;
				const FSensorGraphOutputs Out = AddSensorPasses(GraphBuilder, In, P);
				Nv12Bytes = Out.Nv12Bytes;
				AddEnqueueCopyPass(GraphBuilder, &Nv12Rb, Out.Nv12, Nv12Bytes);
				AddEnqueueCopyPass(GraphBuilder, &HistRb, Out.Histogram, FSensorHistogram::NumBins * sizeof(uint32));
				GraphBuilder.Execute();
			}
			RHICmdList.SubmitAndBlockUntilGPUIdle();
			if (!Nv12Rb.IsReady() || !HistRb.IsReady()) return;
			Result.Nv12.SetNumUninitialized(Nv12Bytes);
			FMemory::Memcpy(Result.Nv12.GetData(), Nv12Rb.Lock(Nv12Bytes), Nv12Bytes);
			Nv12Rb.Unlock();
			FMemory::Memcpy(Result.Histogram.Bins.GetData(), HistRb.Lock(FSensorHistogram::NumBins * sizeof(uint32)),
				FSensorHistogram::NumBins * sizeof(uint32));
			HistRb.Unlock();
			Result.bOk = true;
		});
		FlushRenderingCommands();
		return Result;
	}

	/** NV12 within Y <= 1 / UV <= 2 DN and histograms within tolerance (the binding GPU-vs-reference criteria). */
	void CompareResults(FAutomationTestBase& T, const FGpuResult& G, const CamSimSensorRef::FResult& R, FIntPoint OutSize,
		const TCHAR* Label)
	{
		const int32 N = OutSize.X * OutSize.Y;
		if (!T.TestTrue(FString::Printf(TEXT("%sGPU readback"), Label), G.bOk && G.Nv12.Num() == R.Nv12.Num())) return;
		int32 MaxY = 0, MaxC = 0, FirstY = -1;
		for (int32 I = 0; I < N; ++I)
		{
			const int32 D = FMath::Abs((int32)G.Nv12[I] - (int32)R.Nv12[I]);
			if (D > 1 && FirstY < 0) FirstY = I;
			MaxY = FMath::Max(MaxY, D);
		}
		for (int32 I = N; I < R.Nv12.Num(); ++I) MaxC = FMath::Max(MaxC, FMath::Abs((int32)G.Nv12[I] - (int32)R.Nv12[I]));
		const uint64 GTotal = G.Histogram.Total(), RTotal = R.Histogram.Total();
		const int64 BinTol = FMath::Max<int64>(2, static_cast<int64>(0.001 * N));
		int64 MaxBin = 0;
		int32 WorstBin = 0;
		for (int32 B = 0; B < FSensorHistogram::NumBins; ++B)
		{
			const int64 D = FMath::Abs((int64)G.Histogram.Bins[B] - (int64)R.Histogram.Bins[B]);
			if (D > MaxBin) { MaxBin = D; WorstBin = B; }
		}
		T.AddInfo(FString::Printf(TEXT("%sGPU vs reference: max |dY| %d DN, max |dUV| %d DN, histogram max |dbin| %lld (bin %d, tol %lld), totals %llu/%llu"),
			Label, MaxY, MaxC, MaxBin, WorstBin, BinTol, GTotal, RTotal));
		if (FirstY >= 0)
		{
			T.AddInfo(FString::Printf(TEXT("%sfirst |dY| > 1 at (%d, %d): GPU %d, reference %d"), Label,
				FirstY % OutSize.X, FirstY / OutSize.X, G.Nv12[FirstY], R.Nv12[FirstY]));
		}
		T.TestTrue(FString::Printf(TEXT("%sY within 1 DN (max %d)"), Label, MaxY), MaxY <= 1);
		T.TestTrue(FString::Printf(TEXT("%sUV within 2 DN (max %d)"), Label, MaxC), MaxC <= 2);
		T.TestTrue(FString::Printf(TEXT("%shistogram counts every pixel (GPU %llu, reference %llu)"), Label, GTotal, RTotal),
			GTotal == RTotal && RTotal == static_cast<uint64>(N));
		T.TestTrue(FString::Printf(TEXT("%shistogram bins within %lld (max %lld at bin %d)"), Label, BinTol, MaxBin, WorstBin), MaxBin <= BinTol);
	}

	/** GPU and reference on the same inputs; the reference sees only the view rects' texels. */
	void RunAndCompare(FAutomationTestBase& T, const FGpuImage& Scene, const FGpuImage* Bloom, FIntPoint OutSize,
		const FSensorFrameParams& P, const TCHAR* Label = TEXT(""))
	{
		const TArray<FLinearColor> SceneRect = Scene.RectTexels();
		const TArray<FLinearColor> BloomRect = Bloom ? Bloom->RectTexels() : TArray<FLinearColor>();
		CamSimSensorRef::FImage RefScene, RefBloom;
		RefScene.Texels = &SceneRect; RefScene.W = Scene.Rect.Width(); RefScene.H = Scene.Rect.Height();
		if (Bloom) { RefBloom.Texels = &BloomRect; RefBloom.W = Bloom->Rect.Width(); RefBloom.H = Bloom->Rect.Height(); }
		const CamSimSensorRef::FResult R = CamSimSensorRef::Run(RefScene, RefBloom, OutSize.X, OutSize.Y, P);
		const FGpuResult G = RunOnGpu(Scene, Bloom, OutSize, P);

		CompareResults(T, G, R, OutSize, Label);
	}

	/** Grey pixels at histogram bin centres covering all 256 bins (a 32-stop log gradient). */
	TArray<FLinearColor> LogGradient(int32 W, int32 H)
	{
		TArray<FLinearColor> A; A.SetNumUninitialized(W * H);
		for (int32 I = 0; I < W * H; ++I)
		{
			const float V = FMath::Exp2(FSensorHistogram::BinCentreLog2(I % FSensorHistogram::NumBins));
			A[I] = FLinearColor(V, V, V, 1);
		}
		return A;
	}

	/** Smooth coloured structure in [0.1, 4.1]: edges for the blur, colour for chroma, room for the knee. */
	TArray<FLinearColor> Pattern(int32 W, int32 H)
	{
		TArray<FLinearColor> A; A.SetNumUninitialized(W * H);
		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				const float V = 0.1f + 4.0f * (0.5f + 0.5f * FMath::Sin(0.7f * X) * FMath::Cos(0.5f * Y));
				const float Edge = (X / 8 + Y / 8) % 2 ? 1.0f : 0.25f;
				A[Y * W + X] = FLinearColor(V * Edge, 0.7f * V, 0.4f * V * Edge, 1);
			}
		}
		return A;
	}

	/** Detector fields of a sensor-class preset, mapped as FSensorController does. */
	FSensorFrameParams PresetParams(const TCHAR* Preset, ESensorGraphMode Mode)
	{
		FSensorModeConfig M;
		verify(CamSimSensorPresets::Apply(Preset, M));
		const FSensorDetectorConfig& D = M.Detector;
		FSensorFrameParams P;
		P.Mode          = Mode;
		P.DetectorType  = static_cast<uint32>(D.Type);
		P.FullWellE     = D.FullWellE;
		P.ReadNoiseE    = D.ReadNoiseE;
		P.Prnu          = D.Prnu;
		P.DsnuE         = D.DsnuE;
		P.DarkE         = D.DarkCurrentEs / 30.0f;
		P.TemporalNoise = D.TemporalNoise;
		P.PixelFpn      = D.PixelFpn;
		P.ColumnFpn     = D.ColumnFpn;
		P.RowFpn        = D.RowFpn;
		P.AdcMax        = static_cast<float>((1u << D.AdcBits) - 1u);
		P.HotFraction   = D.HotPixelFraction;
		P.DeadFraction  = D.DeadPixelFraction;
		P.FrameIndex    = 7;
		P.Seed          = FSensorController::ModeSeed(Mode, 3);   // full 32-bit seeds, as live
		return P;
	}

	/** Optics on: 60 deg HFOV, barrel K1 -0.1, cos^4, optical sigma 1.2 px. */
	void SetTestOptics(FSensorFrameParams& P, int32 W)
	{
		P.FocalPx = CamSimOptics::FocalPx(W, 60.0f);
		P.K1 = -0.1f;
		P.VignettingExponent = 4.0f;
		CamSimOptics::SetPsf(P, 1.2f);
	}

	bool SkipWithoutGpu(FAutomationTestBase& T)
	{
		if (GUsingNullRHI) { T.AddInfo(TEXT("skipped: NullRHI (run scripts/run_gpu_tests.sh)")); return true; }
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorGpuEoTest, "CamSim.GPU.Sensor.EoLogGradient",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorGpuEoTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutGpu(*this)) return true;
	constexpr int32 W = 64, H = 32;
	FSensorFrameParams P; P.PhotonGain = FMath::Exp2(-4.0f);
	RunAndCompare(*this, FGpuImage::Whole(LogGradient(W, H), W, H), nullptr, FIntPoint(W, H), P);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorGpuIrTest, "CamSim.GPU.Sensor.IrStepEdgeBlackHot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorGpuIrTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutGpu(*this)) return true;
	constexpr int32 W = 64, H = 32;
	TArray<FLinearColor> Scene; Scene.SetNumUninitialized(W * H);
	for (int32 I = 0; I < W * H; ++I) { const float V = (I % W) < W / 2 ? 0.5f : 8.0f; Scene[I] = FLinearColor(V, V, V, 1); }
	FSensorFrameParams P; P.Mode = ESensorGraphMode::IR; P.PhotonGain = 0.05f; P.DisplayGain = 2.0f; P.DisplayOffset = 0.05f; P.bBlackHot = 1;
	RunAndCompare(*this, FGpuImage::Whole(Scene, W, H), nullptr, FIntPoint(W, H), P);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorGpuSanitizeTest, "CamSim.GPU.Sensor.SanitizesNaNInfNegative",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorGpuSanitizeTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutGpu(*this)) return true;
	constexpr int32 W = 64, H = 32;
	TArray<FLinearColor> Scene = LogGradient(W, H);
	for (int32 I = 0; I < W * H; I += 7) Scene[I] = FLinearColor(NAN, NAN, NAN, 1);
	for (int32 I = 3; I < W * H; I += 11) Scene[I] = FLinearColor(INFINITY, INFINITY, INFINITY, 1);
	for (int32 I = 4; I < W * H; I += 17) Scene[I] = FLinearColor(-INFINITY, -INFINITY, -INFINITY, 1);
	for (int32 I = 5; I < W * H; I += 13) Scene[I] = FLinearColor(-1, -1, -1, 1);
	FSensorFrameParams P; P.PhotonGain = FMath::Exp2(-4.0f);
	RunAndCompare(*this, FGpuImage::Whole(Scene, W, H), nullptr, FIntPoint(W, H), P, TEXT("optics off: "));
	// Through the bilinear resample and blur as well: a bad texel must not spread NaN.
	SetTestOptics(P, W);
	RunAndCompare(*this, FGpuImage::Whole(Scene, W, H), nullptr, FIntPoint(W, H), P, TEXT("optics on: "));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorGpuScaledTest, "CamSim.GPU.Sensor.GpuMatchesReferenceScaled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorGpuScaledTest::RunTest(const FString& Parameters)
{
	// View 2x the capture (Retina) in a larger texture: the graph resamples bilinearly from the view rect.
	if (SkipWithoutGpu(*this)) return true;
	constexpr int32 W = 32, H = 16;
	FGpuImage Big;
	Big.Extent = FIntPoint(2 * W + 6, 2 * H + 4);
	Big.Rect = FIntRect(3, 2, 3 + 2 * W, 2 + 2 * H);
	Big.Texels.Init(FLinearColor(1000, 0, 1000, 1), Big.Extent.X * Big.Extent.Y);   // outside the rect: never sampled
	const TArray<FLinearColor> Inner = Pattern(2 * W, 2 * H);
	for (int32 Y = 0; Y < 2 * H; ++Y) for (int32 X = 0; X < 2 * W; ++X) Big.Texels[(Y + 2) * Big.Extent.X + X + 3] = Inner[Y * 2 * W + X];
	FSensorFrameParams P; P.PhotonGain = 0.25f;
	RunAndCompare(*this, Big, nullptr, FIntPoint(W, H), P, TEXT("optics off: "));
	SetTestOptics(P, W);
	RunAndCompare(*this, Big, nullptr, FIntPoint(W, H), P, TEXT("optics on: "));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorGpuDetectorTest, "CamSim.GPU.Sensor.DetectorMatchesReference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorGpuDetectorTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutGpu(*this)) return true;
	constexpr int32 W = 64, H = 32;
	const FGpuImage Scene = FGpuImage::Whole(LogGradient(W, H), W, H);
	FSensorFrameParams P = PresetParams(TEXT("eo_hd_cmos"), ESensorGraphMode::EO);
	P.PhotonGain = FMath::Exp2(-4.0f);
	P.AnalogGain = 2.0f;
	RunAndCompare(*this, Scene, nullptr, FIntPoint(W, H), P, TEXT("eo_hd_cmos: "));
	// Defects at 1e-5 miss a 2048-pixel frame; 2% each exercises the hot and dead paths.
	P.HotFraction = 0.02f; P.DeadFraction = 0.02f;
	RunAndCompare(*this, Scene, nullptr, FIntPoint(W, H), P, TEXT("2% defects: "));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorGpuMwirTest, "CamSim.GPU.Sensor.IrMwirMatchesReference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorGpuMwirTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutGpu(*this)) return true;
	constexpr int32 W = 64, H = 32;
	FSensorFrameParams P = PresetParams(TEXT("mwir_cooled"), ESensorGraphMode::IR);
	P.PhotonGain = 0.25f; P.DisplayGain = 1.5f; P.DisplayOffset = -0.05f;
	RunAndCompare(*this, FGpuImage::Whole(Pattern(W, H), W, H), nullptr, FIntPoint(W, H), P, TEXT("white hot: "));
	P.bBlackHot = 1;
	RunAndCompare(*this, FGpuImage::Whole(Pattern(W, H), W, H), nullptr, FIntPoint(W, H), P, TEXT("black hot: "));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorGpuBolometerTest, "CamSim.GPU.Sensor.IrBolometerMatchesReference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorGpuBolometerTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutGpu(*this)) return true;
	constexpr int32 W = 64, H = 32;
	// Flat 0.5 scene, AGC stretched 20x around it: the pixel, column and row FPN are the image.
	TArray<FLinearColor> Flat; Flat.Init(FLinearColor(0.5f, 0.5f, 0.5f, 1), W * H);
	FSensorFrameParams P = PresetParams(TEXT("lwir_uncooled"), ESensorGraphMode::IR);
	P.DisplayGain = 20.0f; P.DisplayOffset = 0.5f - 20.0f * 0.5f;
	RunAndCompare(*this, FGpuImage::Whole(Flat, W, H), nullptr, FIntPoint(W, H), P);

	// The reference image must actually show column structure (else the test proves nothing).
	const CamSimSensorRef::FResult R = CamSimSensorRef::Run(Flat, W, H, P);
	double ColMin = 1e9, ColMax = -1e9;
	for (int32 X = 0; X < W; ++X)
	{
		double Sum = 0.0;
		for (int32 Y = 0; Y < H; ++Y) Sum += R.Nv12[Y * W + X];
		ColMin = FMath::Min(ColMin, Sum / H); ColMax = FMath::Max(ColMax, Sum / H);
	}
	AddInfo(FString::Printf(TEXT("column means span %.1f DN"), ColMax - ColMin));
	TestTrue(TEXT("column FPN visible (column means span > 3 DN)"), ColMax - ColMin > 3.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorGpuOpticsTest, "CamSim.GPU.Sensor.OpticsMatchesReference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorGpuOpticsTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutGpu(*this)) return true;
	constexpr int32 W = 64, H = 32;
	FSensorFrameParams P = PresetParams(TEXT("eo_hd_cmos"), ESensorGraphMode::EO);
	P.PhotonGain = 0.25f;
	SetTestOptics(P, W);
	TestEqual(TEXT("sigma 1.2 -> radius 5 (6 taps)"), P.NumPsfTaps, 6u);
	RunAndCompare(*this, FGpuImage::Whole(Pattern(W, H), W, H), nullptr, FIntPoint(W, H), P, TEXT("EO: "));
	// Preset-sized PSFs use the small-tile shader (radius <= 3): eo_hd_cmos sigma_o ~0.32 (R 2), IR ~0.45 (R 3).
	CamSimOptics::SetPsf(P, 0.32f);
	TestEqual(TEXT("sigma 0.32 -> radius 2"), P.NumPsfTaps, 3u);
	RunAndCompare(*this, FGpuImage::Whole(Pattern(W, H), W, H), nullptr, FIntPoint(W, H), P, TEXT("EO radius 2: "));
	CamSimOptics::SetPsf(P, 0.45f);
	TestEqual(TEXT("sigma 0.45 -> radius 3"), P.NumPsfTaps, 4u);
	RunAndCompare(*this, FGpuImage::Whole(Pattern(W, H), W, H), nullptr, FIntPoint(W, H), P, TEXT("EO radius 3: "));
	CamSimOptics::SetPsf(P, 1.2f);
	P.Mode = ESensorGraphMode::IR; P.DisplayGain = 1.2f;
	RunAndCompare(*this, FGpuImage::Whole(Pattern(W, H), W, H), nullptr, FIntPoint(W, H), P, TEXT("IR: "));
	// Largest radius: 8 (sigma 3 -> ceil(9) + 1 clamped).
	CamSimOptics::SetPsf(P, 3.0f);
	TestEqual(TEXT("sigma 3 -> 9 taps"), P.NumPsfTaps, 9u);
	RunAndCompare(*this, FGpuImage::Whole(Pattern(W, H), W, H), nullptr, FIntPoint(W, H), P, TEXT("IR radius 8: "));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorGpuPartialGroupsTest, "CamSim.GPU.Sensor.PartialThreadGroups",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorGpuPartialGroupsTest::RunTest(const FString& Parameters)
{
	// 68 % 16 != 0 and 34 % 16 != 0 (SensorCS groups: partial tiles, blur aprons, NV12 blocks), 34 / 2 = 17 block rows.
	if (SkipWithoutGpu(*this)) return true;
	constexpr int32 W = 68, H = 34;
	FSensorFrameParams P = PresetParams(TEXT("eo_hd_cmos"), ESensorGraphMode::EO);
	P.PhotonGain = 0.25f;
	SetTestOptics(P, W);
	RunAndCompare(*this, FGpuImage::Whole(Pattern(W, H), W, H), nullptr, FIntPoint(W, H), P, TEXT("radius 5: "));
	CamSimOptics::SetPsf(P, 0.45f);
	RunAndCompare(*this, FGpuImage::Whole(Pattern(W, H), W, H), nullptr, FIntPoint(W, H), P, TEXT("radius 3: "));
	CamSimOptics::SetPsf(P, 0.0f);
	RunAndCompare(*this, FGpuImage::Whole(Pattern(W, H), W, H), nullptr, FIntPoint(W, H), P, TEXT("no blur: "));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorGpuBloomTest, "CamSim.GPU.Sensor.BloomMatchesReference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorGpuBloomTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutGpu(*this)) return true;
	constexpr int32 W = 64, H = 32;
	// Half-resolution bloom (32x16) inside a 40x24 texture at (4, 3): a texel checker on ramps, so a
	// mis-registered bloom sample shows. Outside its view rect the texture holds 1000, so any sample
	// leaking past the rect clamp would blow the comparison.
	FGpuImage Bloom;
	Bloom.Extent = FIntPoint(40, 24);
	Bloom.Rect = FIntRect(4, 3, 4 + W / 2, 3 + H / 2);
	Bloom.Texels.Init(FLinearColor(1000, 1000, 1000, 1), Bloom.Extent.X * Bloom.Extent.Y);
	for (int32 Y = 0; Y < H / 2; ++Y)
		for (int32 X = 0; X < W / 2; ++X)
			Bloom.Texels[(Y + 3) * Bloom.Extent.X + X + 4] = FLinearColor(0.2f + 1.5f * ((X + Y) % 2), 0.1f + 0.05f * X, 0.3f + 0.05f * Y, 1);
	const FGpuImage Scene = FGpuImage::Whole(Pattern(W, H), W, H);
	FSensorFrameParams P = PresetParams(TEXT("eo_hd_cmos"), ESensorGraphMode::EO);
	P.PhotonGain = 0.25f;
	RunAndCompare(*this, Scene, &Bloom, FIntPoint(W, H), P, TEXT("optics off: "));
	SetTestOptics(P, W);
	RunAndCompare(*this, Scene, &Bloom, FIntPoint(W, H), P, TEXT("optics on: "));
	return true;
}

// ---------------------------------------------------------------------------
// ROADMAP 4A: radiance input (ThermalCS output, R32F): no pre-exposure, no bloom, weights (1, 0, 0)
// ---------------------------------------------------------------------------

namespace
{
	FTextureRHIRef UploadR32(FRHICommandListImmediate& RHICmdList, const TArray<float>& Texels, FIntPoint Ext)
	{
		const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(TEXT("CamSimTestRadiance"), Ext.X, Ext.Y, PF_R32_FLOAT)
			.SetFlags(ETextureCreateFlags::ShaderResource).SetInitialState(ERHIAccess::SRVMask);
		FTextureRHIRef Tex = RHICmdList.CreateTexture(Desc);
		RHICmdList.UpdateTexture2D(Tex, 0, FUpdateTextureRegion2D(0, 0, 0, 0, Ext.X, Ext.Y), Ext.X * sizeof(float),
			reinterpret_cast<const uint8*>(Texels.GetData()));
		return Tex;
	}

	FGpuResult RunRadianceOnGpu(const TArray<float>& Radiance, FIntPoint Size, const FGpuImage& Bloom, const FSensorFrameParams& P)
	{
		FGpuResult Result;
		ENQUEUE_RENDER_COMMAND(CamSimSensorRadianceGpuTest)([&](FRHICommandListImmediate& RHICmdList)
		{
			FTextureRHIRef SceneTex = UploadR32(RHICmdList, Radiance, Size);
			FTextureRHIRef BloomTex = Upload(RHICmdList, Bloom, TEXT("CamSimTestBloom"));
			FRHIGPUBufferReadback Nv12Rb(TEXT("CamSimTestNv12"));
			FRHIGPUBufferReadback HistRb(TEXT("CamSimTestHist"));
			uint32 Nv12Bytes = 0;
			{
				FRDGBuilder GraphBuilder(RHICmdList);
				FSensorGraphInputs In;
				In.SceneColor = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(SceneTex, TEXT("CamSimTestRadiance")));
				In.SceneViewRect = FIntRect(FIntPoint::ZeroValue, Size);
				In.Bloom = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(BloomTex, TEXT("CamSimTestBloom")));
				In.BloomViewRect = Bloom.Rect;
				In.bRadianceInput = true;
				In.OutputSize = Size;
				const FSensorGraphOutputs Out = AddSensorPasses(GraphBuilder, In, P);
				Nv12Bytes = Out.Nv12Bytes;
				AddEnqueueCopyPass(GraphBuilder, &Nv12Rb, Out.Nv12, Nv12Bytes);
				AddEnqueueCopyPass(GraphBuilder, &HistRb, Out.Histogram, FSensorHistogram::NumBins * sizeof(uint32));
				GraphBuilder.Execute();
			}
			RHICmdList.SubmitAndBlockUntilGPUIdle();
			if (!Nv12Rb.IsReady() || !HistRb.IsReady()) return;
			Result.Nv12.SetNumUninitialized(Nv12Bytes);
			FMemory::Memcpy(Result.Nv12.GetData(), Nv12Rb.Lock(Nv12Bytes), Nv12Bytes);
			Nv12Rb.Unlock();
			FMemory::Memcpy(Result.Histogram.Bins.GetData(), HistRb.Lock(FSensorHistogram::NumBins * sizeof(uint32)),
				FSensorHistogram::NumBins * sizeof(uint32));
			HistRb.Unlock();
			Result.bOk = true;
		});
		FlushRenderingCommands();
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorGpuRadianceInputTest, "CamSim.GPU.Sensor.RadianceInputIgnoresBloom",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorGpuRadianceInputTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutGpu(*this)) return true;
	constexpr int32 W = 64, H = 32;
	TArray<float> Radiance;
	TArray<FLinearColor> AsRgb;
	for (int32 I = 0; I < W * H; ++I)
	{
		const float L = 1.5f + 0.5f * FMath::Sin(0.3f * (I % W)) * FMath::Cos(0.2f * (I / W));   // MWIR-like, W m^-2 sr^-1
		Radiance.Add(L);
		AsRgb.Add(FLinearColor(L, 0.0f, 0.0f, 1.0f));   // an R32F texel loads as (L, 0, 0)
	}
	TArray<FLinearColor> BloomTexels;
	BloomTexels.Init(FLinearColor(1e4f, 1e4f, 1e4f, 1.0f), W * H);   // would saturate everything if it were added
	FSensorFrameParams P = PresetParams(TEXT("mwir_cooled"), ESensorGraphMode::IR);
	P.SignalWeights = FVector3f(1.0f, 0.0f, 0.0f);
	P.InputScale = 0.5f;   // 1 / B(300 K) at runtime
	P.PhotonGain = 0.5f;
	P.DisplayGain = 6.0f;
	P.DisplayOffset = -2.0f;
	const CamSimSensorRef::FResult R = CamSimSensorRef::Run(AsRgb, W, H, P);   // reference: no bloom
	const FGpuResult G = RunRadianceOnGpu(Radiance, FIntPoint(W, H), FGpuImage::Whole(BloomTexels, W, H), P);
	CompareResults(*this, G, R, FIntPoint(W, H), TEXT("radiance input: "));
	return true;
}
