// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"
#include "Sensor/SensorPostProcess.h"
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
	TestFalse(TEXT("Precipitation off by default"),        Cfg.Phase18.bPrecipitation);
	TestFalse(TEXT("GodRays off by default"),              Cfg.Phase18.bGodRays);
	TestFalse(TEXT("AtmosphericScattering off by default"),Cfg.Phase18.bAtmosphericScattering);
	TestFalse(TEXT("DynamicIRExtinction off by default"),  Cfg.Phase18.bDynamicIRExtinction);

	TestEqual(TEXT("RainIntensity default"),     Cfg.Phase18.RainIntensity,    0.0f);
	TestEqual(TEXT("SnowIntensity default"),     Cfg.Phase18.SnowIntensity,    0.0f);
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

// 3. ApplyPrecipitation — no-op when intensities are zero
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18PrecipNoOpTest,
	"CamSim.Phase18.Precipitation.NoOp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18PrecipNoOpTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 8, H = 8;

	FSensorPostProcess FX;
	TMap<ESensorMode, FSensorModeConfig> Configs;
	// FSensorModeConfig defaults Vignetting=0.15, which would otherwise
	// trip the fused per-pixel path and darken corner pixels. Zero it so
	// this test exercises *only* the precipitation toggle.
	FSensorModeConfig EoCfg;
	EoCfg.Vignetting = 0.0f;
	Configs.Add(ESensorMode::EO, EoCfg);
	FSensorQualityConfig Quality;
	FX.Initialize(W, H, Configs, Quality);

	// All-grey source image
	TArray<FColor> Pixels;
	Pixels.Init(FColor(128, 128, 128, 255), W * H);
	const TArray<FColor> Original = Pixels;

	FCamSimConfig::FPhase18Config P18;
	P18.bPrecipitation = false;  // off
	P18.RainIntensity  = 0.0f;
	P18.SnowIntensity  = 0.0f;
	FX.SetPhase18Config(P18);

	FCamSimTelemetry Telemetry;
	FX.Process(Pixels, ESensorMode::EO, 0, Telemetry, 0);

	// With precipitation off the pixel buffer must be unchanged
	bool bIdentical = true;
	for (int32 i = 0; i < Pixels.Num(); ++i)
	{
		if (Pixels[i].R != Original[i].R || Pixels[i].G != Original[i].G || Pixels[i].B != Original[i].B)
		{
			bIdentical = false;
			break;
		}
	}
	TestTrue(TEXT("No-op precipitation leaves pixels unchanged"), bIdentical);

	return true;
}

// 4. ApplyPrecipitation — rain modifies at least one pixel
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18PrecipRainTest,
	"CamSim.Phase18.Precipitation.RainModifiesPixels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18PrecipRainTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 1920, H = 1080;

	FSensorPostProcess FX;
	TMap<ESensorMode, FSensorModeConfig> Configs;
	Configs.Add(ESensorMode::EO, FSensorModeConfig{});
	FSensorQualityConfig Quality;
	FX.Initialize(W, H, Configs, Quality);

	TArray<FColor> Pixels;
	Pixels.Init(FColor(0, 0, 0, 255), W * H);  // all-black so any rain brightens

	FCamSimConfig::FPhase18Config P18;
	P18.bPrecipitation = true;
	P18.RainIntensity  = 1.0f;  // max intensity
	FX.SetPhase18Config(P18);

	FCamSimTelemetry Telemetry;
	FX.Process(Pixels, ESensorMode::EO, 0, Telemetry, 0);

	int32 ModifiedCount = 0;
	for (const FColor& P : Pixels)
	{
		if (P.R > 0 || P.G > 0 || P.B > 0)
		{
			++ModifiedCount;
		}
	}
	TestTrue(TEXT("Rain at max intensity brightens at least 10 pixels"), ModifiedCount >= 10);

	return true;
}

// 5. ApplyDynamicIRExtinction — higher visibility = less extinction
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18DynamicIRExtinctionTest,
	"CamSim.Phase18.DynamicIRExtinction.HigherVisLessExtinction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18DynamicIRExtinctionTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 64, H = 64;

	auto RunIR = [&](float VisibilityM) -> int32
	{
		FSensorPostProcess FX;
		TMap<ESensorMode, FSensorModeConfig> Configs;
		FSensorModeConfig IRMode;
		IRMode.IRExtinctionCoeff = 0.0f;  // disable static extinction; let dynamic drive it
		Configs.Add(ESensorMode::IR, IRMode);
		FSensorQualityConfig Quality;
		FX.Initialize(W, H, Configs, Quality);

		FCamSimConfig::FPhase18Config P18;
		P18.bDynamicIRExtinction = true;
		P18.VisibilityRangeM     = VisibilityM;
		FX.SetPhase18Config(P18);

		// All-white source pixels (max brightness)
		TArray<FColor> Pixels;
		Pixels.Init(FColor(255, 255, 255, 255), W * H);

		FCamSimTelemetry Telemetry;
		Telemetry.SlantRangeM = 5000.0;  // 5 km slant range into fog
		FX.Process(Pixels, ESensorMode::IR, 0, Telemetry, 0);

		// Return average brightness after extinction
		int64 Sum = 0;
		for (const FColor& P : Pixels)
		{
			Sum += P.R;
		}
		return static_cast<int32>(Sum / Pixels.Num());
	};

	const int32 AvgLowVis  = RunIR(500.0f);    // 500 m — heavy fog, lots of extinction
	const int32 AvgHighVis = RunIR(50000.0f);  // 50 km — clear air, minimal extinction

	TestTrue(TEXT("Higher visibility produces brighter IR output"), AvgHighVis > AvgLowVis);

	return true;
}

// 6. Koschmieder coefficient — 3.912 / VisM sanity check
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
