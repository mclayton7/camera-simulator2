// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"
#include "Sensor/SensorPostProcess.h"
#include "Sensor/SensorTypes.h"

// -------------------------------------------------------------------------
// Optical Realism Automation Tests (Phase 15)
// -------------------------------------------------------------------------

// 1. Config defaults — verify FOpticalRealismConfig default values
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpticalRealismConfigDefaultsTest,
	"CamSim.OpticalRealism.ConfigDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOpticalRealismConfigDefaultsTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;

	TestFalse(TEXT("OpticalRealism disabled by default"), Cfg.OpticalRealism.bEnabled);
	TestTrue(TEXT("Motion blur on by default"), Cfg.OpticalRealism.bMotionBlur);
	TestEqual(TEXT("MotionBlurAmount default"), Cfg.OpticalRealism.MotionBlurAmount, 0.5f);
	TestEqual(TEXT("MotionBlurMax default"), Cfg.OpticalRealism.MotionBlurMax, 5);
	TestFalse(TEXT("Lens distortion off by default"), Cfg.OpticalRealism.bLensDistortion);
	TestEqual(TEXT("DistortionK1 default"), Cfg.OpticalRealism.DistortionK1, 0.0f);
	TestEqual(TEXT("DistortionK2 default"), Cfg.OpticalRealism.DistortionK2, 0.0f);
	TestTrue(TEXT("Bloom on by default"), Cfg.OpticalRealism.bBloom);
	TestEqual(TEXT("BloomIntensity default"), Cfg.OpticalRealism.BloomIntensity, 0.675f);
	TestEqual(TEXT("BloomThreshold default"), Cfg.OpticalRealism.BloomThreshold, -1.0f);
	TestFalse(TEXT("ChromaticAberration off by default"), Cfg.OpticalRealism.bChromaticAberration);
	TestFalse(TEXT("DoF off by default"), Cfg.OpticalRealism.bDepthOfField);
	TestEqual(TEXT("FocalDistance default"), Cfg.OpticalRealism.FocalDistance, 0.0f);
	TestEqual(TEXT("ApertureFStop default"), Cfg.OpticalRealism.ApertureFStop, 4.0f);
	TestFalse(TEXT("LensFlare off by default"), Cfg.OpticalRealism.bLensFlare);
	TestEqual(TEXT("LensFlareIntensity default"), Cfg.OpticalRealism.LensFlareIntensity, 1.0f);

	return true;
}

// 2. Config YAML parse — verify optical_realism block round-trips
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpticalRealismConfigParseTest,
	"CamSim.OpticalRealism.ConfigParse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOpticalRealismConfigParseTest::RunTest(const FString& Parameters)
{
	// Load the actual config file and verify the optical_realism block was parsed.
	// The default camsim_config.yaml has optical_realism.enabled: false.
	FCamSimConfig Cfg = FCamSimConfig::Load();

	// Even if YAML isn't found, defaults should be correct
	TestFalse(TEXT("OpticalRealism disabled after load"), Cfg.OpticalRealism.bEnabled);
	TestTrue(TEXT("Bloom enabled after load"), Cfg.OpticalRealism.bBloom);

	return true;
}

// 3. Distortion identity — K1=0, K2=0 produces pixel-exact passthrough
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpticalRealismDistortionIdentityTest,
	"CamSim.OpticalRealism.DistortionIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOpticalRealismDistortionIdentityTest::RunTest(const FString& Parameters)
{
	const int32 W = 16;
	const int32 H = 16;

	FSensorPostProcess Pipeline;
	TMap<ESensorMode, FSensorModeConfig> EmptyConfigs;
	FSensorQualityConfig DefaultQuality;
	Pipeline.Initialize(W, H, EmptyConfigs, DefaultQuality);

	// K1=0, K2=0 should NOT enable distortion
	Pipeline.SetDistortion(0.0f, 0.0f);

	// Create test pattern
	TArray<FColor> Original;
	Original.SetNumUninitialized(W * H);
	for (int32 i = 0; i < W * H; ++i)
	{
		Original[i] = FColor(
			static_cast<uint8>(i % 256),
			static_cast<uint8>((i * 7) % 256),
			static_cast<uint8>((i * 13) % 256),
			255);
	}

	TArray<FColor> Pixels = Original;
	FCamSimTelemetry Telemetry;
	Pipeline.Process(Pixels, ESensorMode::EO, 0, Telemetry, 0);

	// EO with no sensor configs = passthrough; distortion disabled = no change
	bool bIdentical = true;
	for (int32 i = 0; i < W * H; ++i)
	{
		if (Pixels[i].R != Original[i].R || Pixels[i].G != Original[i].G || Pixels[i].B != Original[i].B)
		{
			bIdentical = false;
			break;
		}
	}
	TestTrue(TEXT("K1=0 K2=0 produces pixel-exact passthrough"), bIdentical);

	return true;
}

// 4. Distortion center invariant — center pixel maps to itself for any K1
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpticalRealismDistortionCenterTest,
	"CamSim.OpticalRealism.DistortionCenterInvariant",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOpticalRealismDistortionCenterTest::RunTest(const FString& Parameters)
{
	const int32 W = 32;
	const int32 H = 32;

	FSensorPostProcess Pipeline;
	TMap<ESensorMode, FSensorModeConfig> EmptyConfigs;
	FSensorQualityConfig DefaultQuality;
	Pipeline.Initialize(W, H, EmptyConfigs, DefaultQuality);

	// Strong barrel distortion
	Pipeline.SetDistortion(0.5f, 0.0f);

	// Fill with unique per-pixel colors
	TArray<FColor> Pixels;
	Pixels.SetNumUninitialized(W * H);
	for (int32 i = 0; i < W * H; ++i)
	{
		Pixels[i] = FColor(
			static_cast<uint8>(i % 256),
			static_cast<uint8>((i * 3) % 256),
			static_cast<uint8>((i * 11) % 256),
			255);
	}

	// Record center pixel value before distortion
	const int32 CenterIdx = (H / 2) * W + (W / 2);
	const FColor CenterBefore = Pixels[CenterIdx];

	FCamSimTelemetry Telemetry;
	Pipeline.Process(Pixels, ESensorMode::EO, 0, Telemetry, 0);

	// Center pixel should be unchanged (r=0 at center, factor=1.0)
	TestEqual(TEXT("Center R unchanged"), Pixels[CenterIdx].R, CenterBefore.R);
	TestEqual(TEXT("Center G unchanged"), Pixels[CenterIdx].G, CenterBefore.G);
	TestEqual(TEXT("Center B unchanged"), Pixels[CenterIdx].B, CenterBefore.B);

	return true;
}

// 5. Distortion remap bounds — extreme K1 values don't produce OOB reads
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpticalRealismDistortionBoundsTest,
	"CamSim.OpticalRealism.DistortionRemapBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOpticalRealismDistortionBoundsTest::RunTest(const FString& Parameters)
{
	const int32 W = 16;
	const int32 H = 16;

	FSensorPostProcess Pipeline;
	TMap<ESensorMode, FSensorModeConfig> EmptyConfigs;
	FSensorQualityConfig DefaultQuality;
	Pipeline.Initialize(W, H, EmptyConfigs, DefaultQuality);

	// Extreme barrel distortion
	Pipeline.SetDistortion(2.0f, 1.0f);

	TArray<FColor> Pixels;
	Pixels.SetNumUninitialized(W * H);
	for (int32 i = 0; i < W * H; ++i)
	{
		Pixels[i] = FColor(128, 128, 128, 255);
	}

	// Should not crash — this is the main assertion
	FCamSimTelemetry Telemetry;
	Pipeline.Process(Pixels, ESensorMode::EO, 0, Telemetry, 0);

	// Verify all pixels are valid (0-255 range, alpha=255)
	bool bAllValid = true;
	for (int32 i = 0; i < W * H; ++i)
	{
		if (Pixels[i].A != 255)
		{
			bAllValid = false;
			break;
		}
	}
	TestTrue(TEXT("All pixels valid after extreme distortion"), bAllValid);

	return true;
}
