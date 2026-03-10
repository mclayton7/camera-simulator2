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
    TestEqual(TEXT("CloudShadowStrength default 0.6"),  Cfg.Phase18.CloudShadowStrength, 0.6f);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18WeatherZoneAddTest,
    "CamSim.Phase18.WeatherZone_Add",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18WeatherZoneAddTest::RunTest(const FString& Parameters)
{
    TArray<FWeatherZone> Zones;
    FWeatherZone Z; Z.ZoneID = 1; Z.LatDeg = 37.0; Z.LonDeg = -122.0; Z.RadiusM = 5000.0f;
    Z.Params.FogDensity = 0.8f;
    Zones.Add(Z);
    TestEqual(TEXT("One zone stored"), Zones.Num(), 1);
    TestNearlyEqual(TEXT("Zone fog density"), Zones[0].Params.FogDensity, 0.8f, 0.001f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18WeatherZoneRemoveTest,
    "CamSim.Phase18.WeatherZone_Remove",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18WeatherZoneRemoveTest::RunTest(const FString& Parameters)
{
    TArray<FWeatherZone> Zones;
    FWeatherZone Z; Z.ZoneID = 5;
    Zones.Add(Z);
    Zones.RemoveAll([](const FWeatherZone& W){ return W.ZoneID == 5; });
    TestEqual(TEXT("Zone removed"), Zones.Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18WeatherZoneBlendInsideTest,
    "CamSim.Phase18.WeatherZoneBlend_Inside",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18WeatherZoneBlendInsideTest::RunTest(const FString& Parameters)
{
    const float Dist = 0.0f, Radius = 5000.0f;
    const float Alpha = FMath::Clamp(1.0f - (Dist / Radius), 0.0f, 1.0f);
    TestNearlyEqual(TEXT("Alpha at center = 1"), Alpha, 1.0f, 0.001f);
    const float Result = FMath::Lerp(0.02f, 0.8f, Alpha);
    TestNearlyEqual(TEXT("Fog blended to zone value"), Result, 0.8f, 0.001f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18WeatherZoneBlendOutsideTest,
    "CamSim.Phase18.WeatherZoneBlend_Outside",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18WeatherZoneBlendOutsideTest::RunTest(const FString& Parameters)
{
    const float Dist = 10000.0f, Radius = 5000.0f;
    const float Alpha = FMath::Clamp(1.0f - (Dist / Radius), 0.0f, 1.0f);
    TestNearlyEqual(TEXT("Alpha outside = 0"), Alpha, 0.0f, 0.001f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18WeatherZoneCapTest,
    "CamSim.Phase18.WeatherZone_Cap16",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18WeatherZoneCapTest::RunTest(const FString& Parameters)
{
    TArray<FWeatherZone> Zones;
    for (int32 i = 0; i < 17; ++i)
    {
        if (Zones.Num() >= 16) { Zones.RemoveAt(0); }
        FWeatherZone Z; Z.ZoneID = i;
        Zones.Add(Z);
    }
    TestEqual(TEXT("Max 16 zones"), Zones.Num(), 16);
    TestEqual(TEXT("ID=0 evicted"), Zones[0].ZoneID, 1);
    return true;
}

// ─── 18A/18B: Coverage-to-shadow mapping ────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18CoverageToShadowTest,
    "CamSim.Phase18.CoverageToShadowMapping",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18CoverageToShadowTest::RunTest(const FString& Parameters)
{
    // CIGI Coverage field is percent 0-100; normalise to [0,1] and apply threshold
    const float CoveragePercent = 75.0f;
    const float Coverage01      = FMath::Clamp(CoveragePercent / 100.0f, 0.0f, 1.0f);
    const bool  bShadowOn       = (Coverage01 > 0.1f);
    TestNearlyEqual(TEXT("Coverage01 from 75%"), Coverage01, 0.75f, 0.001f);
    TestTrue(TEXT("Shadow enabled at 75% coverage"), bShadowOn);

    const float LowCoverage01 = FMath::Clamp(5.0f / 100.0f, 0.0f, 1.0f);
    TestFalse(TEXT("Shadow disabled at 5% coverage"), LowCoverage01 > 0.1f);
    return true;
}

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
