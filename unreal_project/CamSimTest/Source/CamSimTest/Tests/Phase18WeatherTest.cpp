// Copyright CamSim Contributors. All Rights Reserved.
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18CloudConfigDefaultsTest,
    "CamSim.Phase18.CloudConfigDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18CloudConfigDefaultsTest::RunTest(const FString& Parameters)
{
    FCamSimConfig Cfg;
    TestFalse(TEXT("bVolumetricClouds off by default"),       Cfg.Phase18.bVolumetricClouds);
    TestNearlyEqual(TEXT("CloudShadowStrength default 0.6"),  Cfg.Phase18.CloudShadowStrength, 0.6f, 0.001f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18WeatherZoneConfigDefaultsTest,
    "CamSim.Phase18.WeatherZoneConfigDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18WeatherZoneConfigDefaultsTest::RunTest(const FString& Parameters)
{
    FCamSimConfig Cfg;
    TestFalse(TEXT("bWeatherZones off by default"), Cfg.Phase18.bWeatherZones);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18ParticleConfigDefaultsTest,
    "CamSim.Phase18.ParticleConfigDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18ParticleConfigDefaultsTest::RunTest(const FString& Parameters)
{
    FCamSimConfig Cfg;
    TestEqual(TEXT("ContrailAltM default 8000"),   Cfg.Phase18.ContrailAltM,    8000.0f);
    TestEqual(TEXT("ContrailSpeedMs default 100"), Cfg.Phase18.ContrailSpeedMs, 100.0f);
    TestEqual(TEXT("SmokeComponentID default 1"),  Cfg.Phase18.SmokeComponentID,          1);
    TestEqual(TEXT("FireComponentID default 2"),   Cfg.Phase18.FireComponentID,            2);
    TestEqual(TEXT("CraterComponentID default 10"),Cfg.Phase18.CraterImpactComponentID,   10);
    TestEqual(TEXT("MaxCraters default 32"),        Cfg.Phase18.MaxCraters,               32);
    return true;
}

// ─── 18L: Weather zone blending math ─────────────────────────────────────────

#include "Environment/CamSimEnvironment.h"

// ─── 18F/G/H/I: Particle state defaults ──────────────────────────────────────

#include "Environment/CamSimParticleManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18ParticleStateDefaultsTest,
    "CamSim.Phase18.ParticleState_Defaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18ParticleStateDefaultsTest::RunTest(const FString& Parameters)
{
    FEntityParticleState State;
    TestNull(TEXT("RotorWashComp null"),   State.RotorWashComp);
    TestNull(TEXT("SmokeComp null"),       State.SmokeComp);
    TestNull(TEXT("FireComp null"),        State.FireComp);
    TestNull(TEXT("ContrailComp null"),    State.ContrailComp);
    TestFalse(TEXT("Smoke inactive"),      State.bSmokeActive);
    TestFalse(TEXT("Fire inactive"),       State.bFireActive);
    TestFalse(TEXT("Contrail inactive"),   State.bContrailActive);
    return true;
}

