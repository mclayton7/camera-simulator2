// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"

// ---------------------------------------------------------------------------
// Test 1: FPhase19Config default values
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase19ConfigDefaultsTest,
	"CamSim.Phase19.ConfigDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase19ConfigDefaultsTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;

	TestFalse(TEXT("OceanEnabled off by default"),           Cfg.Phase19.bOceanEnabled);
	TestFalse(TEXT("VesselWakesEnabled off by default"),     Cfg.Phase19.bVesselWakesEnabled);
	TestFalse(TEXT("VesselMotionEnabled off by default"),    Cfg.Phase19.bVesselMotionEnabled);
	TestFalse(TEXT("OceanReflectionsEnabled off by default"),Cfg.Phase19.bOceanReflectionsEnabled);
	TestEqual(TEXT("BeaufortState default 0"),   Cfg.Phase19.BeaufortState,      0);
	TestEqual(TEXT("AmpScale default 1.0"),      Cfg.Phase19.WaveAmplitudeScale, 1.0f);
	TestEqual(TEXT("FreqScale default 1.0"),     Cfg.Phase19.WaveFrequencyScale, 1.0f);
	TestEqual(TEXT("Choppiness default 0.5"),    Cfg.Phase19.WaveChoppiness,     0.5f);
	TestEqual(TEXT("WakeFadeTime default 8.0"),  Cfg.Phase19.WakeFadeTime,       8.0f);
	TestEqual(TEXT("VesselMotionScale default"), Cfg.Phase19.VesselMotionScale,  1.0f);
	TestEqual(TEXT("SSRIntensity default 1.0"),  Cfg.Phase19.SSRIntensity,       1.0f);

	return true;
}

// ---------------------------------------------------------------------------
// Test 2: FPhase19Config fields can all be set and read back (struct field test)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase19FieldsTest,
	"CamSim.Phase19.Fields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase19FieldsTest::RunTest(const FString& Parameters)
{
	FCamSimConfig::FPhase19Config Cfg;
	Cfg.bOceanEnabled             = true;
	Cfg.BeaufortState             = 7;
	Cfg.WaveAmplitudeScale        = 1.5f;
	Cfg.WaveFrequencyScale        = 0.8f;
	Cfg.WaveChoppiness            = 0.7f;
	Cfg.bVesselWakesEnabled       = true;
	Cfg.WakeFadeTime              = 12.0f;
	Cfg.bVesselMotionEnabled      = true;
	Cfg.VesselMotionScale         = 0.9f;
	Cfg.bOceanReflectionsEnabled  = true;
	Cfg.SSRIntensity              = 0.6f;
	Cfg.ReflectionCaptureRadius   = 5000.0f;

	TestTrue (TEXT("OceanEnabled"),            Cfg.bOceanEnabled);
	TestEqual(TEXT("BeaufortState"),           Cfg.BeaufortState,          7);
	TestEqual(TEXT("WaveAmplitudeScale"),      Cfg.WaveAmplitudeScale,     1.5f);
	TestEqual(TEXT("WaveFrequencyScale"),      Cfg.WaveFrequencyScale,     0.8f);
	TestEqual(TEXT("WaveChoppiness"),          Cfg.WaveChoppiness,         0.7f);
	TestTrue (TEXT("VesselWakesEnabled"),      Cfg.bVesselWakesEnabled);
	TestEqual(TEXT("WakeFadeTime"),            Cfg.WakeFadeTime,          12.0f);
	TestTrue (TEXT("VesselMotionEnabled"),     Cfg.bVesselMotionEnabled);
	TestEqual(TEXT("VesselMotionScale"),       Cfg.VesselMotionScale,      0.9f);
	TestTrue (TEXT("OceanReflectionsEnabled"), Cfg.bOceanReflectionsEnabled);
	TestEqual(TEXT("SSRIntensity"),            Cfg.SSRIntensity,           0.6f);
	TestEqual(TEXT("ReflectionCaptureRadius"), Cfg.ReflectionCaptureRadius, 5000.0f);

	return true;
}

// ---------------------------------------------------------------------------
// Test 3: CAMSIM_OCEAN_BEAUFORT env var overrides config
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase19EnvBeaufortTest,
	"CamSim.Phase19.EnvVarBeaufort",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase19EnvBeaufortTest::RunTest(const FString& Parameters)
{
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_OCEAN_BEAUFORT"), TEXT("9"));

	const FCamSimConfig Cfg = FCamSimConfig::Load();
	TestEqual(TEXT("Env var sets BeaufortState to 9"), Cfg.Phase19.BeaufortState, 9);

	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_OCEAN_BEAUFORT"), TEXT(""));
	return true;
}

// ---------------------------------------------------------------------------
// Test 4: CAMSIM_OCEAN_AMP_SCALE env var overrides config
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase19EnvAmpScaleTest,
	"CamSim.Phase19.EnvVarAmpScale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase19EnvAmpScaleTest::RunTest(const FString& Parameters)
{
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_OCEAN_AMP_SCALE"), TEXT("2.5"));

	const FCamSimConfig Cfg = FCamSimConfig::Load();
	TestEqual(TEXT("Env var sets WaveAmplitudeScale to 2.5"), Cfg.Phase19.WaveAmplitudeScale, 2.5f);

	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_OCEAN_AMP_SCALE"), TEXT(""));
	return true;
}
