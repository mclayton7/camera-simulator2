// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"

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
