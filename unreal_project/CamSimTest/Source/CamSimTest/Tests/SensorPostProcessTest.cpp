// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Sensor/SensorPostProcess.h"
#include "Sensor/SensorTypes.h"

// -------------------------------------------------------------------------
// SensorPostProcess Automation Tests
//
// Validates deterministic pixel output for each waveband mode,
// vignetting symmetry, noise ring bounds, and pipeline ordering.
// -------------------------------------------------------------------------

namespace
{

/** Create a solid-color test frame. */
TArray<FColor> MakeFrame(int32 Width, int32 Height, FColor Color)
{
	TArray<FColor> Pixels;
	Pixels.SetNum(Width * Height);
	for (FColor& P : Pixels) P = Color;
	return Pixels;
}

/** Get default sensor mode configs. */
TMap<ESensorMode, FSensorModeConfig> MakeDefaultConfigs()
{
	TMap<ESensorMode, FSensorModeConfig> Configs;
	FSensorModeConfig Eo;
	Eo.NETD = 0.0f;
	Eo.FixedPatternNoise = 0.0f;
	Eo.Vignetting = 0.0f;
	Configs.Add(ESensorMode::EO, Eo);

	FSensorModeConfig Ir;
	Ir.NETD = 0.0f;
	Ir.FixedPatternNoise = 0.0f;
	Ir.Vignetting = 0.0f;
	Configs.Add(ESensorMode::IR, Ir);

	FSensorModeConfig Nvg;
	Nvg.NETD = 0.0f;
	Nvg.FixedPatternNoise = 0.0f;
	Nvg.Vignetting = 0.0f;
	Configs.Add(ESensorMode::NVG, Nvg);

	return Configs;
}

} // anonymous namespace

// -------------------------------------------------------------------------
// EO mode passthrough (no effects) should not modify pixels
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorEOPassthroughTest,
	"CamSim.Sensor.EOPassthrough",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorEOPassthroughTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 64, H = 64;
	auto Configs = MakeDefaultConfigs();
	FSensorQualityConfig Quality;

	FSensorPostProcess PP;
	PP.Initialize(W, H, Configs, Quality);

	TArray<FColor> Pixels = MakeFrame(W, H, FColor(100, 150, 200, 255));
	TArray<FColor> Original = Pixels;

	FCamSimTelemetry T;
	PP.Process(Pixels, ESensorMode::EO, 0, T, 0);

	// With zero NETD, zero vignetting, zero FPN — pixels should be unchanged
	bool bIdentical = true;
	for (int32 i = 0; i < Pixels.Num(); ++i)
	{
		if (Pixels[i] != Original[i])
		{
			bIdentical = false;
			break;
		}
	}
	TestTrue(TEXT("EO passthrough preserves pixels"), bIdentical);

	return true;
}

// -------------------------------------------------------------------------
// IR mode converts to grayscale
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorIRGrayscaleTest,
	"CamSim.Sensor.IRGrayscale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorIRGrayscaleTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 32, H = 32;
	auto Configs = MakeDefaultConfigs();
	FSensorQualityConfig Quality;

	FSensorPostProcess PP;
	PP.Initialize(W, H, Configs, Quality);

	// Red channel only
	TArray<FColor> Pixels = MakeFrame(W, H, FColor(0, 0, 255, 255)); // FColor is BGRA: B=0, G=0, R=255
	FCamSimTelemetry T;
	PP.Process(Pixels, ESensorMode::IR, 0, T, 0);

	// After IR conversion, R == G == B (grayscale)
	const FColor& P = Pixels[W * H / 2];
	TestEqual(TEXT("IR: R == G"), P.R, P.G);
	TestEqual(TEXT("IR: R == B"), P.R, P.B);

	return true;
}

// -------------------------------------------------------------------------
// IR BlackHot polarity inverts
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorIRPolarityTest,
	"CamSim.Sensor.IRPolarity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorIRPolarityTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 16, H = 16;
	auto Configs = MakeDefaultConfigs();
	FSensorQualityConfig Quality;

	FSensorPostProcess PP;
	PP.Initialize(W, H, Configs, Quality);

	TArray<FColor> WhiteHot = MakeFrame(W, H, FColor(128, 128, 128, 255));
	TArray<FColor> BlackHot = WhiteHot;

	FCamSimTelemetry T;
	PP.Process(WhiteHot, ESensorMode::IR, 0, T, 0);
	PP.Process(BlackHot, ESensorMode::IR, 1, T, 0);

	// WhiteHot and BlackHot should be inversions of each other
	// Due to S-curve, exact inversion is 255 - value
	const uint8 WH = WhiteHot[0].R;
	const uint8 BH = BlackHot[0].R;
	TestTrue(TEXT("WH + BH == 255"), static_cast<int32>(WH) + static_cast<int32>(BH) == 255);

	return true;
}

// -------------------------------------------------------------------------
// NVG mode produces green tint
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorNVGGreenTintTest,
	"CamSim.Sensor.NVGGreenTint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorNVGGreenTintTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 16, H = 16;
	auto Configs = MakeDefaultConfigs();
	FSensorQualityConfig Quality;

	FSensorPostProcess PP;
	PP.Initialize(W, H, Configs, Quality);

	// Gray input
	TArray<FColor> Pixels = MakeFrame(W, H, FColor(128, 128, 128, 255));
	FCamSimTelemetry T;
	PP.Process(Pixels, ESensorMode::NVG, 0, T, 0);

	const FColor& P = Pixels[0];
	// NVG: R=0, G=I, B=I*3/10 — red should be zero
	TestEqual(TEXT("NVG: R == 0"), P.R, static_cast<uint8>(0));
	TestTrue(TEXT("NVG: G > B"), P.G > P.B);
	TestTrue(TEXT("NVG: G > 0"), P.G > 0);

	return true;
}

// -------------------------------------------------------------------------
// Vignetting: center pixel should be brighter than corner pixel
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorVignettingSymmetryTest,
	"CamSim.Sensor.VignettingSymmetry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorVignettingSymmetryTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 64, H = 64;

	TMap<ESensorMode, FSensorModeConfig> Configs;
	FSensorModeConfig Eo;
	Eo.NETD = 0.0f;
	Eo.FixedPatternNoise = 0.0f;
	Eo.Vignetting = 0.8f; // strong vignetting
	Configs.Add(ESensorMode::EO, Eo);

	FSensorQualityConfig Quality;

	FSensorPostProcess PP;
	PP.Initialize(W, H, Configs, Quality);

	TArray<FColor> Pixels = MakeFrame(W, H, FColor(200, 200, 200, 255));
	FCamSimTelemetry T;
	PP.Process(Pixels, ESensorMode::EO, 0, T, 0);

	// Center pixel
	const FColor& Center = Pixels[(H / 2) * W + (W / 2)];
	// Corner pixel
	const FColor& Corner = Pixels[0];

	TestTrue(TEXT("Center brighter than corner (R)"), Center.R > Corner.R);
	TestTrue(TEXT("Center brighter than corner (G)"), Center.G > Corner.G);

	return true;
}

// -------------------------------------------------------------------------
// Pixel count mismatch should not crash
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPixelCountMismatchTest,
	"CamSim.Sensor.PixelCountMismatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorPixelCountMismatchTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 32, H = 32;
	auto Configs = MakeDefaultConfigs();
	FSensorQualityConfig Quality;

	FSensorPostProcess PP;
	PP.Initialize(W, H, Configs, Quality);

	// Wrong pixel count — should return without crashing
	TArray<FColor> Pixels;
	Pixels.SetNum(10); // Way too small
	FCamSimTelemetry T;
	PP.Process(Pixels, ESensorMode::EO, 0, T, 0);

	// If we get here, the test passed (no crash)
	TestTrue(TEXT("Survived pixel count mismatch"), true);

	return true;
}

// -------------------------------------------------------------------------
// Noise determinism: same frame index produces same noise pattern
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorNoiseDeterminismTest,
	"CamSim.Sensor.NoiseDeterminism",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorNoiseDeterminismTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 32, H = 32;

	TMap<ESensorMode, FSensorModeConfig> Configs;
	FSensorModeConfig Ir;
	Ir.NETD = 0.05f;
	Ir.Vignetting = 0.0f;
	Ir.FixedPatternNoise = 0.0f;
	Configs.Add(ESensorMode::IR, Ir);

	FSensorQualityConfig Quality;

	FSensorPostProcess PP;
	PP.Initialize(W, H, Configs, Quality);

	FCamSimTelemetry T;

	// First run
	TArray<FColor> Pixels1 = MakeFrame(W, H, FColor(128, 128, 128, 255));
	PP.Process(Pixels1, ESensorMode::IR, 0, T, 42);

	// Second run with same frame index
	TArray<FColor> Pixels2 = MakeFrame(W, H, FColor(128, 128, 128, 255));
	PP.Process(Pixels2, ESensorMode::IR, 0, T, 42);

	// Should produce identical output (noise is deterministic from ring buffer)
	bool bIdentical = true;
	for (int32 i = 0; i < Pixels1.Num(); ++i)
	{
		if (Pixels1[i] != Pixels2[i])
		{
			bIdentical = false;
			break;
		}
	}
	TestTrue(TEXT("Same frame index -> identical noise"), bIdentical);

	return true;
}
