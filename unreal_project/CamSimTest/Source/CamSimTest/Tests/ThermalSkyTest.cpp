// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Thermal/BandRadiance.h"
#include "Thermal/ThermalSky.h"

// CamSim.Thermal.Sky.*: effective sky temperature (ROADMAP 4A).

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalSkyZenithTest, "CamSim.Thermal.Sky.ZenithColderThanHorizon",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalSkyZenithTest::RunTest(const FString& Parameters)
{
	const double Ta = 288.15;
	TestNearlyEqual(TEXT("Swinbank clear zenith"), FThermalSky::ClearZenithK(Ta), 270.002, 0.01);
	TestNearlyEqual(TEXT("zenith = clear zenith"), FThermalSky::SkyTemperatureK(Ta, 0.0, 1.0), FThermalSky::ClearZenithK(Ta), 1e-6);
	TestNearlyEqual(TEXT("horizon ~ T_air"), FThermalSky::SkyTemperatureK(Ta, 0.0, 0.05), Ta, 0.01);
	double Prev = FThermalSky::SkyTemperatureK(Ta, 0.0, 0.05);
	for (int32 I = 1; I <= 10; ++I)
	{
		const double SinEl = 0.05 + I * 0.095;
		const double T = FThermalSky::SkyTemperatureK(Ta, 0.0, SinEl);
		TestTrue(*FString::Printf(TEXT("colder toward the zenith (sin el %.3f)"), SinEl), T < Prev);
		Prev = T;
	}
	TestNearlyEqual(TEXT("below the horizon clamps to MinSinEl"), FThermalSky::SkyTemperatureK(Ta, 0.0, -0.7),
		FThermalSky::SkyTemperatureK(Ta, 0.0, FThermalSky::MinSinEl), 1e-9);
	TestEqual(TEXT("zenith emissivity clamps to 1 for a very hot day"), FThermalSky::ZenithEmissivity(400.0), 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalSkyOvercastTest, "CamSim.Thermal.Sky.OvercastIsAirTemperature",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalSkyOvercastTest::RunTest(const FString& Parameters)
{
	const double Ta = 283.0;
	for (const double SinEl : { -0.2, 0.05, 0.5, 1.0 })
	{
		TestNearlyEqual(*FString::Printf(TEXT("overcast at sin el %.2f"), SinEl), FThermalSky::SkyTemperatureK(Ta, 1.0, SinEl), Ta, 1e-9);
	}
	TestNearlyEqual(TEXT("overcast downwelling = sigma T^4"), FThermalSky::DownwellingIrradiance(Ta, 1.0), FThermalSky::Sigma * Ta * Ta * Ta * Ta, 1e-6);
	FBandRadiance Band;
	Band.Build(8.0, 12.0);
	const double B = Band.Radiance(static_cast<float>(Ta));
	TestNearlyEqual(TEXT("overcast hemisphere radiance = B(T_air)"), FThermalSky::HemisphereBandRadiance(Band, Ta, 1.0), B, B * 1e-5);
	TestTrue(TEXT("clear hemisphere radiance below B(T_air)"), FThermalSky::HemisphereBandRadiance(Band, Ta, 0.0) < B);
	TestTrue(TEXT("half cloud between"), FThermalSky::HemisphereBandRadiance(Band, Ta, 0.5) > FThermalSky::HemisphereBandRadiance(Band, Ta, 0.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalSkyHemisphereTest, "CamSim.Thermal.Sky.HemisphericEmissivity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalSkyHemisphereTest::RunTest(const FString& Parameters)
{
	const double Ta = 288.15;
	const double EpsZ = FThermalSky::ZenithEmissivity(Ta);
	const double EpsH = FThermalSky::HemisphericEmissivity(Ta);
	TestNearlyEqual(TEXT("zenith emissivity"), EpsZ, 0.77089, 1e-4);
	TestTrue(TEXT("eps_z < eps_hemi < 1"), EpsZ < EpsH && EpsH < 1.0);
	TestNearlyEqual(TEXT("eps_hemi (64-sample cosine-weighted mean)"), EpsH, 0.8827, 0.005);
	TestNearlyEqual(TEXT("clear downwelling"), FThermalSky::DownwellingIrradiance(Ta, 0.0), FThermalSky::Sigma * EpsH * Ta * Ta * Ta * Ta, 1e-6);
	return true;
}
