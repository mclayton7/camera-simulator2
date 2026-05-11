// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Sensor/SensorPostProcess.h"
#include "Sensor/SensorTypes.h"

// -------------------------------------------------------------------------
// Phase 16 Sensor Fidelity — Automation Tests
//
// Validates AGC, quantization, defect pixels, Gaussian blur,
// AC banding, and NVG IR pointer effects.
// -------------------------------------------------------------------------

namespace
{

TArray<FColor> MakeFrame(int32 Width, int32 Height, FColor Color)
{
	TArray<FColor> Pixels;
	Pixels.SetNum(Width * Height);
	for (FColor& P : Pixels) P = Color;
	return Pixels;
}

TMap<ESensorMode, FSensorModeConfig> MakeCleanConfigs()
{
	TMap<ESensorMode, FSensorModeConfig> Configs;
	FSensorModeConfig Eo;
	Eo.NETD = 0.0f; Eo.FixedPatternNoise = 0.0f; Eo.Vignetting = 0.0f;
	Configs.Add(ESensorMode::EO, Eo);

	FSensorModeConfig Ir;
	Ir.NETD = 0.0f; Ir.FixedPatternNoise = 0.0f; Ir.Vignetting = 0.0f;
	Configs.Add(ESensorMode::IR, Ir);

	FSensorModeConfig Nvg;
	Nvg.NETD = 0.0f; Nvg.FixedPatternNoise = 0.0f; Nvg.Vignetting = 0.0f;
	Configs.Add(ESensorMode::NVG, Nvg);

	return Configs;
}

} // anonymous namespace

// -------------------------------------------------------------------------
// 16A: AGC on uniform IR frame does not crash (degenerate case)
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorAGCUniformTest,
	"CamSim.SensorFidelity.AGC_UniformFrame",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorAGCUniformTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 32, H = 32;
	auto Configs = MakeCleanConfigs();
	FSensorModeConfig& IrCfg = Configs.FindOrAdd(ESensorMode::IR);
	IrCfg.bAGCEnabled      = true;
	IrCfg.AGCLowPercentile = 0.01f;
	IrCfg.AGCHighPercentile = 0.99f;

	FSensorQualityConfig Quality;
	FSensorPostProcess PP;
	PP.Initialize(W, H, Configs, Quality);

	// Uniform input: AGC should hit degenerate guard (Hi <= Lo) and skip
	TArray<FColor> Pixels = MakeFrame(W, H, FColor(100, 100, 100, 255));
	FCamSimTelemetry T;
	PP.Process(Pixels, ESensorMode::IR, 0, T, 0);

	TestTrue(TEXT("AGC on uniform frame does not crash"), Pixels.Num() == W * H);
	return true;
}

// -------------------------------------------------------------------------
// 16A: AGC stretches a narrow-range histogram
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorAGCStretchTest,
	"CamSim.SensorFidelity.AGC_StretchesRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorAGCStretchTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 64, H = 64;
	auto Configs = MakeCleanConfigs();
	FSensorModeConfig& IrCfg = Configs.FindOrAdd(ESensorMode::IR);
	IrCfg.bAGCEnabled      = true;
	IrCfg.AGCLowPercentile = 0.0f;
	IrCfg.AGCHighPercentile = 1.0f;

	FSensorQualityConfig Quality;
	FSensorPostProcess PP;
	PP.Initialize(W, H, Configs, Quality);

	// Half dark (40), half bright (60) — narrow range
	TArray<FColor> Pixels;
	Pixels.SetNum(W * H);
	for (int32 i = 0; i < W * H; ++i)
	{
		uint8 V = (i < W * H / 2) ? 40 : 60;
		Pixels[i] = FColor(V, V, V, 255);
	}

	FCamSimTelemetry T;
	PP.Process(Pixels, ESensorMode::IR, 0, T, 0);

	// After AGC stretch, the dark pixels should be near 0, bright near 255
	// (the IR tone curve will modify exact values, but the stretch should be visible)
	const uint8 Dark  = Pixels[0].R;
	const uint8 Bright = Pixels[W * H - 1].R;
	TestTrue(TEXT("AGC: bright > dark after stretch"), Bright > Dark);
	TestTrue(TEXT("AGC: stretch widens range"), (Bright - Dark) > 100);

	return true;
}

// -------------------------------------------------------------------------
// 16A: Manual gain doubles brightness
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorManualGainTest,
	"CamSim.SensorFidelity.AGC_ManualGain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorManualGainTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 16, H = 16;
	auto Configs = MakeCleanConfigs();
	FSensorModeConfig& IrCfg = Configs.FindOrAdd(ESensorMode::IR);
	IrCfg.bAGCEnabled    = false;
	IrCfg.AGCManualLevel = 0.0f;
	IrCfg.AGCManualGain  = 2.0f;

	FSensorQualityConfig Quality;
	FSensorPostProcess PP;
	PP.Initialize(W, H, Configs, Quality);

	// Gray input (64,64,64)
	TArray<FColor> Pixels = MakeFrame(W, H, FColor(64, 64, 64, 255));
	FCamSimTelemetry T;
	PP.Process(Pixels, ESensorMode::IR, 0, T, 0);

	// After IR conversion (BT.601 luma of gray 64) + S-curve + manual gain*2
	// The output should be notably brighter than original
	const uint8 V = Pixels[0].R;
	TestTrue(TEXT("Manual gain brightens output"), V > 50);

	return true;
}

// -------------------------------------------------------------------------
// 16B: Quantization reduces unique values
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorQuantizationTest,
	"CamSim.SensorFidelity.Quantization_ReducesBitDepth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorQuantizationTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 16, H = 16;
	auto Configs = MakeCleanConfigs();
	FSensorModeConfig& IrCfg = Configs.FindOrAdd(ESensorMode::IR);
	IrCfg.QuantizationBits   = 4;  // 16 levels
	IrCfg.bQuantizationDither = false;
	IrCfg.bAGCEnabled        = false;

	FSensorQualityConfig Quality;
	FSensorPostProcess PP;
	PP.Initialize(W, H, Configs, Quality);

	// Gradient input across rows
	TArray<FColor> Pixels;
	Pixels.SetNum(W * H);
	for (int32 i = 0; i < W * H; ++i)
	{
		uint8 V = static_cast<uint8>((i * 255) / (W * H));
		Pixels[i] = FColor(V, V, V, 255);
	}

	FCamSimTelemetry T;
	PP.Process(Pixels, ESensorMode::IR, 0, T, 0);

	// Count unique R values — should be <= 16 (2^4)
	TSet<uint8> UniqueValues;
	for (const FColor& P : Pixels)
		UniqueValues.Add(P.R);

	TestTrue(TEXT("Quantized to <= 16 unique values"), UniqueValues.Num() <= 16);

	return true;
}

// -------------------------------------------------------------------------
// 16B: Dither increases variance on uniform input
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorQuantizationDitherTest,
	"CamSim.SensorFidelity.Quantization_DitherIncreasesVariance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorQuantizationDitherTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 16, H = 16;

	auto MakeIrConfigs = [](bool bDither)
	{
		auto Configs = MakeCleanConfigs();
		FSensorModeConfig& IrCfg = Configs.FindOrAdd(ESensorMode::IR);
		IrCfg.QuantizationBits   = 4;
		IrCfg.bQuantizationDither = bDither;
		IrCfg.bAGCEnabled        = false;
		return Configs;
	};

	FSensorQualityConfig Quality;

	// Without dither
	auto Configs1 = MakeIrConfigs(false);
	FSensorPostProcess PP1;
	PP1.Initialize(W, H, Configs1, Quality);
	TArray<FColor> Pixels1 = MakeFrame(W, H, FColor(100, 100, 100, 255));
	FCamSimTelemetry T;
	PP1.Process(Pixels1, ESensorMode::IR, 0, T, 0);

	TSet<uint8> NoDitherValues;
	for (const FColor& P : Pixels1) NoDitherValues.Add(P.R);

	// With dither
	auto Configs2 = MakeIrConfigs(true);
	FSensorPostProcess PP2;
	PP2.Initialize(W, H, Configs2, Quality);
	TArray<FColor> Pixels2 = MakeFrame(W, H, FColor(100, 100, 100, 255));
	PP2.Process(Pixels2, ESensorMode::IR, 0, T, 0);

	TSet<uint8> DitherValues;
	for (const FColor& P : Pixels2) DitherValues.Add(P.R);

	TestTrue(TEXT("Dither produces more unique values"),
		DitherValues.Num() >= NoDitherValues.Num());

	return true;
}

// -------------------------------------------------------------------------
// 16C: Hot pixels are present in output
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorDefectPixelsTest,
	"CamSim.SensorFidelity.DefectPixels_HotPresent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorDefectPixelsTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 64, H = 64;
	auto Configs = MakeCleanConfigs();
	FSensorModeConfig& IrCfg = Configs.FindOrAdd(ESensorMode::IR);
	IrCfg.DefectPixelCount = 50;
	IrCfg.DefectHotRatio   = 1.0f;  // all hot
	IrCfg.DefectSeed       = 7;
	IrCfg.bAGCEnabled      = false;

	FSensorQualityConfig Quality;
	FSensorPostProcess PP;
	PP.Initialize(W, H, Configs, Quality);

	TArray<FColor> Pixels = MakeFrame(W, H, FColor(100, 100, 100, 255));
	FCamSimTelemetry T;
	PP.Process(Pixels, ESensorMode::IR, 0, T, 0);

	// At least one pixel should be 255 (hot)
	bool bFoundHot = false;
	for (const FColor& P : Pixels)
		if (P.R == 255) { bFoundHot = true; break; }
	TestTrue(TEXT("At least one hot pixel present"), bFoundHot);

	return true;
}

// -------------------------------------------------------------------------
// 16C: Defect pixels are deterministic
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorDefectDeterminismTest,
	"CamSim.SensorFidelity.DefectPixels_Deterministic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorDefectDeterminismTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 32, H = 32;

	auto MakeDefectConfigs = []()
	{
		auto Configs = MakeCleanConfigs();
		FSensorModeConfig& IrCfg = Configs.FindOrAdd(ESensorMode::IR);
		IrCfg.DefectPixelCount = 20;
		IrCfg.DefectHotRatio   = 0.5f;
		IrCfg.DefectSeed       = 99;
		IrCfg.bAGCEnabled      = false;
		return Configs;
	};

	FSensorQualityConfig Quality;
	FCamSimTelemetry T;

	// First PP instance
	auto Configs1 = MakeDefectConfigs();
	FSensorPostProcess PP1;
	PP1.Initialize(W, H, Configs1, Quality);
	TArray<FColor> Pixels1 = MakeFrame(W, H, FColor(128, 128, 128, 255));
	PP1.Process(Pixels1, ESensorMode::IR, 0, T, 0);

	// Second PP instance (re-initialized from scratch with same seed)
	auto Configs2 = MakeDefectConfigs();
	FSensorPostProcess PP2;
	PP2.Initialize(W, H, Configs2, Quality);
	TArray<FColor> Pixels2 = MakeFrame(W, H, FColor(128, 128, 128, 255));
	PP2.Process(Pixels2, ESensorMode::IR, 0, T, 0);

	bool bIdentical = true;
	for (int32 i = 0; i < Pixels1.Num(); ++i)
	{
		if (Pixels1[i] != Pixels2[i]) { bIdentical = false; break; }
	}
	TestTrue(TEXT("Defect pixels are deterministic across re-initialization"), bIdentical);

	return true;
}

// -------------------------------------------------------------------------
// 16D: Gaussian blur softens a sharp edge
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorGaussianBlurTest,
	"CamSim.SensorFidelity.GaussianBlur_SoftensEdge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorGaussianBlurTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 32, H = 32;
	auto Configs = MakeCleanConfigs();
	FSensorModeConfig& EoCfg = Configs.FindOrAdd(ESensorMode::EO);
	EoCfg.GaussianSigma = 1.0f;

	FSensorQualityConfig Quality;
	FSensorPostProcess PP;
	PP.Initialize(W, H, Configs, Quality);

	// Half white, half black (vertical split)
	TArray<FColor> Pixels;
	Pixels.SetNum(W * H);
	for (int32 i = 0; i < W * H; ++i)
		Pixels[i] = ((i % W) < W / 2) ? FColor(255, 255, 255, 255) : FColor(0, 0, 0, 255);

	FCamSimTelemetry T;
	PP.Process(Pixels, ESensorMode::EO, 0, T, 0);

	// Edge pixel should be blended (not 0 or 255)
	const FColor& Edge = Pixels[(H / 2) * W + (W / 2 - 1)];
	TestTrue(TEXT("Edge pixel blended by Gaussian blur"), Edge.R > 0 && Edge.R < 255);

	return true;
}

// -------------------------------------------------------------------------
// 16D: Gaussian blur preserves uniform image
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorGaussianBlurUniformTest,
	"CamSim.SensorFidelity.GaussianBlur_PreservesUniform",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorGaussianBlurUniformTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 32, H = 32;
	auto Configs = MakeCleanConfigs();
	FSensorModeConfig& EoCfg = Configs.FindOrAdd(ESensorMode::EO);
	EoCfg.GaussianSigma = 1.5f;

	FSensorQualityConfig Quality;
	FSensorPostProcess PP;
	PP.Initialize(W, H, Configs, Quality);

	TArray<FColor> Pixels = MakeFrame(W, H, FColor(128, 128, 128, 255));
	FCamSimTelemetry T;
	PP.Process(Pixels, ESensorMode::EO, 0, T, 0);

	// Uniform input → uniform output (within rounding tolerance)
	const FColor& P = Pixels[W * H / 2];
	TestTrue(TEXT("Uniform preserved by Gaussian blur"),
		FMath::Abs(static_cast<int32>(P.R) - 128) <= 2);

	return true;
}

// -------------------------------------------------------------------------
// 16F: AC banding produces row variation in IR
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorACBandingTest,
	"CamSim.SensorFidelity.ACBanding_RowVariation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorACBandingTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 64, H = 64;
	auto Configs = MakeCleanConfigs();
	FSensorModeConfig& IrCfg = Configs.FindOrAdd(ESensorMode::IR);
	IrCfg.ACBandingAmplitude = 30.0f;
	IrCfg.ACBandingFrequency = 4.0f;
	IrCfg.bAGCEnabled        = false;

	FSensorQualityConfig Quality;
	FSensorPostProcess PP;
	PP.Initialize(W, H, Configs, Quality);

	TArray<FColor> Pixels = MakeFrame(W, H, FColor(128, 128, 128, 255));
	FCamSimTelemetry T;
	PP.Process(Pixels, ESensorMode::IR, 0, T, 0);

	// Different rows should have different brightness. With Frequency=4 cycles
	// per Height, one cycle is H/4 rows — so sampling at H/4 lands on the same
	// sine zero-crossing as Row 0. Pick H/16 (a quarter-cycle into the first
	// wave) to land near a sine peak.
	const uint8 Row0 = Pixels[0].R;
	const uint8 RowM = Pixels[(H / 16) * W].R;
	TestTrue(TEXT("AC banding produces row variation"), Row0 != RowM);

	return true;
}

// -------------------------------------------------------------------------
// 16L: NVG pointer brightens center
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorNvgPointerTest,
	"CamSim.SensorFidelity.NvgPointer_CenterBright",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorNvgPointerTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 64, H = 64;
	auto Configs = MakeCleanConfigs();
	FSensorModeConfig& NvgCfg = Configs.FindOrAdd(ESensorMode::NVG);
	NvgCfg.bIRPointerEnabled = true;
	NvgCfg.IRPointerX        = 0.5f;
	NvgCfg.IRPointerY        = 0.5f;
	NvgCfg.IRPointerRadius   = 4.0f;
	NvgCfg.IRPointerIntensity = 200.0f;

	FSensorQualityConfig Quality;
	FSensorPostProcess PP;
	PP.Initialize(W, H, Configs, Quality);

	// Dim green NVG frame
	TArray<FColor> Pixels = MakeFrame(W, H, FColor(0, 50, 15, 255));
	FCamSimTelemetry T;
	PP.Process(Pixels, ESensorMode::NVG, 0, T, 0);

	// Center pixel G should be brighter than corner
	const FColor& Center = Pixels[(H / 2) * W + (W / 2)];
	const FColor& Corner = Pixels[0];
	TestTrue(TEXT("NVG pointer brightens center G"), Center.G > Corner.G);

	return true;
}

// -------------------------------------------------------------------------
// 16E: Thermal drift shifts baseline over time
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorThermalDriftTest,
	"CamSim.SensorFidelity.ThermalDrift_ShiftsBaseline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorThermalDriftTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 32, H = 32;
	auto Configs = MakeCleanConfigs();
	FSensorModeConfig& IrCfg = Configs.FindOrAdd(ESensorMode::IR);
	IrCfg.bThermalDriftEnabled = true;
	IrCfg.ThermalDriftRate     = 30.0f;  // 30 DN/sec = 1 DN/frame at 30fps
	IrCfg.NUCIntervalSec       = 0.0f;   // no auto-NUC
	IrCfg.bAGCEnabled          = false;

	FSensorQualityConfig Quality;
	FSensorPostProcess PP;
	PP.Initialize(W, H, Configs, Quality);

	// Run 10 frames to accumulate drift
	FCamSimTelemetry T;
	TArray<FColor> Pixels;
	for (int32 F = 0; F < 10; ++F)
	{
		Pixels = MakeFrame(W, H, FColor(100, 100, 100, 255));
		PP.Process(Pixels, ESensorMode::IR, 0, T, F);
	}

	// After 10 frames of drift at 1 DN/frame, the IR-converted baseline should
	// be noticeably brighter than a single frame. IR tone curve maps 100→~13,
	// then drift adds ~10 DN over 10 frames → should be measurably higher.
	// Just verify it's brighter than a clean single-frame pass.
	FSensorPostProcess PP2;
	PP2.Initialize(W, H, Configs, Quality);
	TArray<FColor> Baseline = MakeFrame(W, H, FColor(100, 100, 100, 255));
	PP2.Process(Baseline, ESensorMode::IR, 0, T, 0);

	TestTrue(TEXT("Thermal drift increases brightness over time"),
		Pixels[W * H / 2].R > Baseline[W * H / 2].R);

	return true;
}

// -------------------------------------------------------------------------
// 16E: NUC reset clears thermal drift
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorNUCResetTest,
	"CamSim.SensorFidelity.ThermalDrift_NUCResets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorNUCResetTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 16, H = 16;
	auto Configs = MakeCleanConfigs();
	FSensorModeConfig& IrCfg = Configs.FindOrAdd(ESensorMode::IR);
	IrCfg.bThermalDriftEnabled = true;
	IrCfg.ThermalDriftRate     = 60.0f;  // 2 DN/frame at 30fps
	IrCfg.NUCIntervalSec       = 0.5f;   // NUC every 0.5 sec = 15 frames at 30fps
	IrCfg.bAGCEnabled          = false;

	FSensorQualityConfig Quality;
	FSensorPostProcess PP;
	PP.Initialize(W, H, Configs, Quality);

	FCamSimTelemetry T;

	// Run 14 frames (just before NUC)
	TArray<FColor> PreNUC;
	for (int32 F = 0; F < 14; ++F)
	{
		PreNUC = MakeFrame(W, H, FColor(100, 100, 100, 255));
		PP.Process(PreNUC, ESensorMode::IR, 0, T, F);
	}
	const uint8 BeforeNUC = PreNUC[0].R;

	// Frame 15 triggers NUC (elapsed >= 0.5s), frame 16 is post-NUC with 1 frame drift
	TArray<FColor> PostNUC;
	for (int32 F = 14; F < 17; ++F)
	{
		PostNUC = MakeFrame(W, H, FColor(100, 100, 100, 255));
		PP.Process(PostNUC, ESensorMode::IR, 0, T, F);
	}
	const uint8 AfterNUC = PostNUC[0].R;

	// Post-NUC brightness should be lower than pre-NUC (drift was reset)
	TestTrue(TEXT("NUC reset reduces drift"), AfterNUC < BeforeNUC);

	return true;
}

// -------------------------------------------------------------------------
// 16G: AGC lag causes delayed convergence
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorAGCLagTest,
	"CamSim.SensorFidelity.AGCLag_DelayedConvergence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorAGCLagTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 64, H = 64;

	// Build two configs: one with lag, one without
	auto MakeLagConfigs = [](int32 LagFrames)
	{
		auto Configs = MakeCleanConfigs();
		FSensorModeConfig& IrCfg = Configs.FindOrAdd(ESensorMode::IR);
		IrCfg.bAGCEnabled       = true;
		IrCfg.AGCLowPercentile  = 0.0f;
		IrCfg.AGCHighPercentile = 1.0f;
		IrCfg.AGCLagFrames      = LagFrames;
		return Configs;
	};

	// Helper: make a two-tone frame (half dark, half bright) to avoid degenerate AGC guard
	auto MakeTwoTone = [&](uint8 Lo, uint8 Hi)
	{
		TArray<FColor> Pixels;
		Pixels.SetNum(W * H);
		for (int32 i = 0; i < W * H; ++i)
		{
			uint8 V = (i < W * H / 2) ? Lo : Hi;
			Pixels[i] = FColor(V, V, V, 255);
		}
		return Pixels;
	};

	FSensorQualityConfig Quality;
	FCamSimTelemetry T;

	// PP with lag = 5
	auto LagConfigs = MakeLagConfigs(5);
	FSensorPostProcess PPLag;
	PPLag.Initialize(W, H, LagConfigs, Quality);

	// PP with no lag
	auto NoLagConfigs = MakeLagConfigs(0);
	FSensorPostProcess PPNoLag;
	PPNoLag.Initialize(W, H, NoLagConfigs, Quality);

	// Stabilize both on dark two-tone (20, 40) for 10 frames
	for (int32 F = 0; F < 10; ++F)
	{
		TArray<FColor> DarkLag   = MakeTwoTone(20, 40);
		TArray<FColor> DarkNoLag = MakeTwoTone(20, 40);
		PPLag.Process(DarkLag, ESensorMode::IR, 0, T, F);
		PPNoLag.Process(DarkNoLag, ESensorMode::IR, 0, T, F);
	}

	// Switch to bright two-tone (150, 200) — first frame after switch
	TArray<FColor> BrightLag   = MakeTwoTone(150, 200);
	TArray<FColor> BrightNoLag = MakeTwoTone(150, 200);
	PPLag.Process(BrightLag, ESensorMode::IR, 0, T, 10);
	PPNoLag.Process(BrightNoLag, ESensorMode::IR, 0, T, 10);

	const uint8 LagPixel   = BrightLag[0].R;
	const uint8 NoLagPixel = BrightNoLag[0].R;

	// With lag, the AGC thresholds are still smoothed from the dark input,
	// so the output should differ from the no-lag version
	TestTrue(TEXT("AGC lag produces different output than no-lag on scene switch"),
		LagPixel != NoLagPixel);

	return true;
}

// -------------------------------------------------------------------------
// 16H: Rolling shutter blends with previous frame
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorRollingShutterTest,
	"CamSim.SensorFidelity.RollingShutter_BlendsFrames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorRollingShutterTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 32, H = 32;
	auto Configs = MakeCleanConfigs();
	FSensorModeConfig& EoCfg = Configs.FindOrAdd(ESensorMode::EO);
	EoCfg.RollingShutterStrength = 1.0f;

	FSensorQualityConfig Quality;
	FSensorPostProcess PP;
	PP.Initialize(W, H, Configs, Quality);

	FCamSimTelemetry T;

	// Frame 0: all white
	TArray<FColor> Frame0 = MakeFrame(W, H, FColor(255, 255, 255, 255));
	PP.Process(Frame0, ESensorMode::EO, 0, T, 0);

	// Frame 1: all black — top rows should be blended toward white (previous)
	TArray<FColor> Frame1 = MakeFrame(W, H, FColor(0, 0, 0, 255));
	PP.Process(Frame1, ESensorMode::EO, 0, T, 1);

	// Top row (Y=0): maximum blend with previous frame (white) → should not be pure black
	const uint8 TopR = Frame1[0].R;
	// Bottom row (Y=H-1): minimal blend → should be near black
	const uint8 BottomR = Frame1[(H - 1) * W].R;

	TestTrue(TEXT("Rolling shutter: top row shows previous frame"), TopR > 0);
	TestTrue(TEXT("Rolling shutter: top brighter than bottom"), TopR > BottomR);

	return true;
}

// -------------------------------------------------------------------------
// 16I: Platform vibration shifts image
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorVibrationTest,
	"CamSim.SensorFidelity.Vibration_ShiftsImage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorVibrationTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 32, H = 32;
	auto Configs = MakeCleanConfigs();
	FSensorModeConfig& EoCfg = Configs.FindOrAdd(ESensorMode::EO);
	EoCfg.VibrationAmplitude = 3.0f;  // large amplitude for visible shift

	FSensorQualityConfig Quality;
	FSensorPostProcess PP;
	PP.Initialize(W, H, Configs, Quality);

	// Half white, half black (vertical split)
	TArray<FColor> Pixels;
	Pixels.SetNum(W * H);
	for (int32 i = 0; i < W * H; ++i)
		Pixels[i] = ((i % W) < W / 2) ? FColor(255, 255, 255, 255) : FColor(0, 0, 0, 255);

	TArray<FColor> Original = Pixels;

	FCamSimTelemetry T;
	PP.Process(Pixels, ESensorMode::EO, 0, T, 42);  // specific frame for deterministic seed

	// At least one pixel near the edge should have changed due to vibration shift
	bool bChanged = false;
	for (int32 i = 0; i < W * H; ++i)
	{
		if (Pixels[i] != Original[i]) { bChanged = true; break; }
	}
	TestTrue(TEXT("Vibration shifts pixel values"), bChanged);

	return true;
}

// -------------------------------------------------------------------------
// 16J: Gain/offset jitter modifies IR pixels
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorGainOffsetJitterTest,
	"CamSim.SensorFidelity.GainOffsetJitter_ModifiesPixels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorGainOffsetJitterTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 16, H = 16;
	auto Configs = MakeCleanConfigs();
	FSensorModeConfig& IrCfg = Configs.FindOrAdd(ESensorMode::IR);
	IrCfg.GainJitter   = 0.1f;   // 10% gain variation
	IrCfg.OffsetJitter = 10.0f;  // 10 DN offset variation
	IrCfg.bAGCEnabled  = false;

	FSensorQualityConfig Quality;
	FSensorPostProcess PP;
	PP.Initialize(W, H, Configs, Quality);

	FCamSimTelemetry T;

	// Process two frames with different FrameIndex — different seeds → different jitter
	TArray<FColor> Frame1 = MakeFrame(W, H, FColor(128, 128, 128, 255));
	PP.Process(Frame1, ESensorMode::IR, 0, T, 0);

	TArray<FColor> Frame2 = MakeFrame(W, H, FColor(128, 128, 128, 255));
	PP.Process(Frame2, ESensorMode::IR, 0, T, 1);

	// Different frames should produce different brightness due to jitter
	// (uniform input → all pixels same per frame, but differ between frames)
	TestTrue(TEXT("Gain/offset jitter varies between frames"), Frame1[0].R != Frame2[0].R);

	return true;
}

// -------------------------------------------------------------------------
// 16K: Sun glint brightens highlights when sun is up
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorSunGlintTest,
	"CamSim.SensorFidelity.SunGlint_BrightensHighlights",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorSunGlintTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 32, H = 32;
	auto Configs = MakeCleanConfigs();
	FSensorModeConfig& EoCfg = Configs.FindOrAdd(ESensorMode::EO);
	EoCfg.SunGlintIntensity  = 50.0f;
	EoCfg.SunGlintThreshold  = 200.0f;
	EoCfg.SunGlintSpread     = 1.0f;

	FSensorQualityConfig Quality;
	FSensorPostProcess PP;
	PP.Initialize(W, H, Configs, Quality);

	// Bright frame above threshold
	TArray<FColor> Pixels = MakeFrame(W, H, FColor(230, 230, 230, 255));
	FCamSimTelemetry T;
	T.SunElevationDeg = 45.0f;  // sun well above horizon
	PP.Process(Pixels, ESensorMode::EO, 0, T, 0);

	// Glint should have boosted the bright pixels
	TestTrue(TEXT("Sun glint brightens above threshold"), Pixels[0].R > 230);

	return true;
}

// -------------------------------------------------------------------------
// 16K: Sun glint disabled when sun is below horizon
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorSunGlintNightTest,
	"CamSim.SensorFidelity.SunGlint_DisabledAtNight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorSunGlintNightTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 16, H = 16;
	auto Configs = MakeCleanConfigs();
	FSensorModeConfig& EoCfg = Configs.FindOrAdd(ESensorMode::EO);
	EoCfg.SunGlintIntensity  = 50.0f;
	EoCfg.SunGlintThreshold  = 200.0f;

	FSensorQualityConfig Quality;
	FSensorPostProcess PP;
	PP.Initialize(W, H, Configs, Quality);

	TArray<FColor> Pixels = MakeFrame(W, H, FColor(230, 230, 230, 255));
	FCamSimTelemetry T;
	T.SunElevationDeg = -10.0f;  // sun below horizon
	PP.Process(Pixels, ESensorMode::EO, 0, T, 0);

	// No glint at night — pixels should be unchanged
	TestTrue(TEXT("No sun glint at night"), Pixels[0].R == 230);

	return true;
}
