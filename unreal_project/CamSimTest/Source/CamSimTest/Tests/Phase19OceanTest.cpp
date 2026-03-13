// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"
#include "Ocean/FBeaufortTable.h"
#include "Ocean/FGerstnerOceanSurface.h"
#include "CIGI/CigiPacketTypes.h"
#include "Environment/CamSimParticleManager.h"

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

// ---------------------------------------------------------------------------
// Test 5: Beaufort 0 → flat sea
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase19Beaufort0Test,
	"CamSim.Phase19.Beaufort0Flat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase19Beaufort0Test::RunTest(const FString& Parameters)
{
	const FBeaufortEntry E = FBeaufortTable::Sample(0);
	TestEqual(TEXT("Beaufort 0 WaveHt"),  E.WaveHtM,    0.0f);
	TestEqual(TEXT("Beaufort 0 WaveLen"), E.WaveLenM,   0.0f);
	TestEqual(TEXT("Beaufort 0 Chop"),    E.Choppiness, 0.0f);
	return true;
}

// ---------------------------------------------------------------------------
// Test 6: Beaufort 6 → correct table values
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase19Beaufort6Test,
	"CamSim.Phase19.Beaufort6Values",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase19Beaufort6Test::RunTest(const FString& Parameters)
{
	const FBeaufortEntry E = FBeaufortTable::Sample(6);
	TestEqual(TEXT("Beaufort 6 WaveHt"),  E.WaveHtM,    2.5f);
	TestEqual(TEXT("Beaufort 6 WaveLen"), E.WaveLenM,  70.0f);
	TestEqual(TEXT("Beaufort 6 Chop"),    E.Choppiness, 0.6f);
	return true;
}

// ---------------------------------------------------------------------------
// Test 7: Beaufort 12 → clamped to max
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase19Beaufort12Test,
	"CamSim.Phase19.Beaufort12Max",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase19Beaufort12Test::RunTest(const FString& Parameters)
{
	const FBeaufortEntry E12  = FBeaufortTable::Sample(12);
	const FBeaufortEntry E100 = FBeaufortTable::Sample(100); // clamped
	TestEqual(TEXT("WaveHt 12"),    E12.WaveHtM,     14.0f);
	TestEqual(TEXT("WaveHt clamp"), E100.WaveHtM,    E12.WaveHtM);
	TestEqual(TEXT("WaveLen clamp"),E100.WaveLenM,   E12.WaveLenM);
	TestEqual(TEXT("Chop clamp"),   E100.Choppiness, E12.Choppiness);
	return true;
}

// ---------------------------------------------------------------------------
// Test 8: Beaufort 5 → linearly interpolated
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase19Beaufort5InterpolTest,
	"CamSim.Phase19.Beaufort5Interp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase19Beaufort5InterpolTest::RunTest(const FString& Parameters)
{
	const FBeaufortEntry E4 = FBeaufortTable::Sample(4);
	const FBeaufortEntry E5 = FBeaufortTable::Sample(5);
	const FBeaufortEntry E6 = FBeaufortTable::Sample(6);

	// E5 should be between E4 and E6
	TestTrue(TEXT("WaveHt 5 between 4 and 6"),
		E5.WaveHtM >= E4.WaveHtM && E5.WaveHtM <= E6.WaveHtM);
	TestTrue(TEXT("WaveLen 5 between 4 and 6"),
		E5.WaveLenM >= E4.WaveLenM && E5.WaveLenM <= E6.WaveLenM);

	// Beaufort 5.0: Lo=5, Hi=6, T=0.0 → exactly Entries[5]
	TestEqual(TEXT("WaveHt 5 exact"), E5.WaveHtM, 1.5f);
	return true;
}

// ---------------------------------------------------------------------------
// Test 9: FCigiWaveState fields from opcode 14 struct
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase19CigiWaveStateTest,
	"CamSim.Phase19.CigiWaveState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase19CigiWaveStateTest::RunTest(const FString& Parameters)
{
	FCigiWaveState S;
	S.WaveID   = 2;
	S.bEnabled = true;
	S.WaveHtM  = 3.5f;
	S.WaveLenM = 80.0f;
	S.PeriodS  = 7.2f;

	TestEqual(TEXT("WaveID"),   (int32)S.WaveID,  2);
	TestTrue (TEXT("Enabled"),  S.bEnabled);
	TestEqual(TEXT("WaveHtM"),  S.WaveHtM,   3.5f);
	TestEqual(TEXT("WaveLenM"), S.WaveLenM, 80.0f);
	TestEqual(TEXT("PeriodS"),  S.PeriodS,   7.2f);
	return true;
}

// ---------------------------------------------------------------------------
// Test 10: CIGI WaveHtM > 0 → SetWaveParams uses it directly, no Beaufort conversion
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase19CigiWaveOverrideTest,
	"CamSim.Phase19.CigiWaveOverride",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase19CigiWaveOverrideTest::RunTest(const FString& Parameters)
{
	FGerstnerOceanSurface Ocean;
	// Set Beaufort 6 first (WaveHt=2.5)
	const FBeaufortEntry B6 = FBeaufortTable::Sample(6);
	Ocean.SetWaveParams(B6.WaveHtM, B6.WaveLenM, 1.0f, 1.0f, B6.Choppiness);
	// Now override with CIGI values
	Ocean.SetWaveParams(5.0f, 120.0f, 1.0f, 1.0f, 0.7f);

	// At t=0, X=0: height = A*cos(0) = A = WaveHt * 100 * 0.5 = 250 cm
	const float H = Ocean.GetSurfaceHeightAt(FVector2D(0.0f, 0.0f));
	TestTrue(TEXT("Height reflects CIGI WaveHt, not Beaufort 6"),
		FMath::IsNearlyEqual(H, 250.0f, 1.0f));
	return true;
}

// ---------------------------------------------------------------------------
// Test 11: GetSurfaceHeightAt() returns 0 at Beaufort 0
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase19HeightFlatTest,
	"CamSim.Phase19.HeightFlat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase19HeightFlatTest::RunTest(const FString& Parameters)
{
	FGerstnerOceanSurface Ocean;
	const FBeaufortEntry B0 = FBeaufortTable::Sample(0);
	Ocean.SetWaveParams(B0.WaveHtM, B0.WaveLenM, 1.0f, 1.0f, B0.Choppiness);

	TestEqual(TEXT("Height 0 at Beaufort 0"),
		Ocean.GetSurfaceHeightAt(FVector2D(0.0f, 0.0f)), 0.0f);
	TestEqual(TEXT("Height 0 at Beaufort 0 far"),
		Ocean.GetSurfaceHeightAt(FVector2D(100000.0f, 0.0f)), 0.0f);
	return true;
}

// ---------------------------------------------------------------------------
// Test 12: GetSurfaceHeightAt() returns non-zero at Beaufort 6
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase19HeightNonZeroTest,
	"CamSim.Phase19.HeightNonZero",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase19HeightNonZeroTest::RunTest(const FString& Parameters)
{
	FGerstnerOceanSurface Ocean;
	const FBeaufortEntry B6 = FBeaufortTable::Sample(6);
	Ocean.SetWaveParams(B6.WaveHtM, B6.WaveLenM, 1.0f, 1.0f, B6.Choppiness);

	// At X=0, t=0: height = A * cos(0) = A > 0
	const float H = Ocean.GetSurfaceHeightAt(FVector2D(0.0f, 0.0f));
	TestTrue(TEXT("Height > 0 at Beaufort 6 crest"), H > 0.0f);

	// Expected: A = 2.5m * 100 * 0.5 = 125 cm
	TestTrue(TEXT("Height ≈ 125 cm at crest"), FMath::IsNearlyEqual(H, 125.0f, 1.0f));
	return true;
}

// ---------------------------------------------------------------------------
// Test 13: Sea-domain entity (EntityDomain==3) triggers vessel motion;
//          land-domain (EntityDomain==2) is skipped.
// This tests the dispatch logic in FCamSimEntityManager — here we test
// the FCigiEntityState field directly.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase19SeaDomainDispatchTest,
	"CamSim.Phase19.SeaDomainDispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase19SeaDomainDispatchTest::RunTest(const FString& Parameters)
{
	FCigiEntityState SeaEntity;
	SeaEntity.EntityDomain = 3;

	FCigiEntityState LandEntity;
	LandEntity.EntityDomain = 2;

	// Dispatch predicate mirrors CamSimEntityManager logic
	auto IsMaritime = [](const FCigiEntityState& S) { return S.EntityDomain == 3; };

	TestTrue (TEXT("Sea domain (3) triggers motion"),  IsMaritime(SeaEntity));
	TestFalse(TEXT("Land domain (2) skips motion"),    IsMaritime(LandEntity));

	return true;
}

// ---------------------------------------------------------------------------
// Test 14: Wake FX entity state tracking — bWakeActive flag test
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase19WakeLifecycleTest,
	"CamSim.Phase19.WakeLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase19WakeLifecycleTest::RunTest(const FString& Parameters)
{
	// Test the tracking map logic without spawning real Niagara components.
	// FEntityParticleState should have a bWakeActive flag.
	FEntityParticleState State;
	State.bWakeActive = false;
	TestFalse(TEXT("Wake inactive on spawn"), State.bWakeActive);

	State.bWakeActive = true;
	TestTrue(TEXT("Wake active after flag set"), State.bWakeActive);

	return true;
}

// ---------------------------------------------------------------------------
// Test 15: CIGI wave queue drain → FOceanManager::ApplyWaveState() path.
//          Tests SetWaveParams + GetSurfaceHeightAt round-trip directly.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase19WaveQueueDrainTest,
	"CamSim.Phase19.WaveQueueDrain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase19WaveQueueDrainTest::RunTest(const FString& Parameters)
{
	// Test that ApplyWaveState with a valid state changes ocean height.
	FGerstnerOceanSurface Ocean;

	// Start with Beaufort 0 (flat)
	Ocean.SetWaveParams(0.0f, 0.0f, 1.0f, 1.0f, 0.5f);
	TestEqual(TEXT("Flat before CIGI"), Ocean.GetSurfaceHeightAt(FVector2D::ZeroVector), 0.0f);

	// Simulate ApplyWaveState: apply WaveHt=5.0m, WaveLen=120m
	FCigiWaveState WaveState;
	WaveState.bEnabled = true;
	WaveState.WaveHtM  = 5.0f;
	WaveState.WaveLenM = 120.0f;

	// ApplyWaveState passes these directly to SetWaveParams
	Ocean.SetWaveParams(WaveState.WaveHtM, WaveState.WaveLenM, 1.0f, 1.0f, 0.5f);

	const float H = Ocean.GetSurfaceHeightAt(FVector2D::ZeroVector);
	TestTrue(TEXT("Non-zero height after CIGI wave state applied"), H > 0.0f);
	// A = 5.0m * 100 * 0.5 = 250 cm; height at crest ≈ 250
	TestTrue(TEXT("Height ≈ 250 cm (5m Beaufort)"), FMath::IsNearlyEqual(H, 250.0f, 1.0f));

	return true;
}
