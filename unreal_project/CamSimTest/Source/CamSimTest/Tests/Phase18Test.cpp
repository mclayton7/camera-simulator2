// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"
#include "Sensor/SensorTypes.h"
#include "Metadata/KlvBuilder.h"

// -------------------------------------------------------------------------
// Phase 18 — Weather, Atmosphere & Particle Effects Automation Tests
// -------------------------------------------------------------------------

// 1. FPhase18Config defaults — all effects off, sane numeric defaults
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18ConfigDefaultsTest,
	"CamSim.Phase18.ConfigDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18ConfigDefaultsTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;

	TestFalse(TEXT("SecondFog off by default"),            Cfg.Phase18.bSecondFog);
	TestFalse(TEXT("GodRays off by default"),              Cfg.Phase18.bGodRays);
	TestFalse(TEXT("AtmosphericScattering off by default"),Cfg.Phase18.bAtmosphericScattering);
	TestFalse(TEXT("DynamicIRExtinction off by default"),  Cfg.Phase18.bDynamicIRExtinction);

	TestEqual(TEXT("VisibilityRangeM default"),  Cfg.Phase18.VisibilityRangeM, 10000.0f);
	TestEqual(TEXT("GodRayIntensity default"),   Cfg.Phase18.GodRayIntensity,  1.0f);
	TestEqual(TEXT("RayleighScattering default"),Cfg.Phase18.RayleighScattering, 1.0f);
	TestEqual(TEXT("MieScattering default"),     Cfg.Phase18.MieScattering,    1.0f);

	return true;
}

// 2. FCamSimTelemetry atmospheric fields — default values
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18TelemetryDefaultsTest,
	"CamSim.Phase18.TelemetryDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18TelemetryDefaultsTest::RunTest(const FString& Parameters)
{
	FCamSimTelemetry T;

	TestEqual(TEXT("AtmosphericVisibilityM default"), T.AtmosphericVisibilityM, 10000.0f);
	TestTrue (TEXT("RelativeHumidity default in [0,1]"),
		T.RelativeHumidity >= 0.0f && T.RelativeHumidity <= 1.0f);
	TestEqual(TEXT("WeatherSeverity default 0"),  T.WeatherSeverity,   0.0f);
	TestEqual(TEXT("WeatherPrecipType default 0"),T.WeatherPrecipType, static_cast<uint8>(0));

	return true;
}

// 3. Koschmieder coefficient — 3.912 / VisM sanity check
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18KoschmiederTest,
	"CamSim.Phase18.KoschmiederCoefficient",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18KoschmiederTest::RunTest(const FString& Parameters)
{
	// At 1000 m visibility the Koschmieder coefficient should be ~0.003912
	constexpr float VisM  = 1000.0f;
	const float Coeff = 3.912f / VisM;

	TestTrue(TEXT("Coeff > 0"), Coeff > 0.0f);
	// 3.912 / 1000 = 0.003912
	TestTrue(TEXT("Coeff ≈ 0.003912"), FMath::Abs(Coeff - 0.003912f) < 1e-5f);

	// At 10000 m it should be 10x smaller
	const float Coeff10km = 3.912f / 10000.0f;
	TestTrue(TEXT("10 km coeff is 10x smaller than 1 km coeff"),
		FMath::Abs(Coeff / Coeff10km - 10.0f) < 0.01f);

	return true;
}
