// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Environment/NightIllumination.h"

namespace
{
	FNightIlluminationInput MoonAt(double PhaseDeg, double ElDeg)
	{
		FNightIlluminationInput In;
		In.Ephem.SunElDeg = -40.0;
		In.Ephem.MoonElDeg = ElDeg;
		In.Ephem.MoonPhaseAngleDeg = PhaseDeg;
		In.Ephem.MoonDistanceKm = 384400.0;
		In.Ephem.SunDistanceAu = 1.0;
		In.NightFloorLux = 0.0;
		return In;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNightIllumMoonLevelsTest, "CamSim.Environment.Illumination.MoonLevels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FNightIllumMoonLevelsTest::RunTest(const FString&)
{
	// K&S: full moon TOA 0.313 lux at mean distance; at zenith k_V = 0.172 → T = 0.854 → 0.267 lux.
	TestTrue(TEXT("full TOA"), FMath::IsNearlyEqual(FNightIllumination::MoonToaLux(0.0, 384400.0), 0.313, 0.005));
	const FNightIlluminationResult Full = FNightIllumination::Compute(MoonAt(0.0, 90.0));
	TestTrue(TEXT("full zenith ground 0.25-0.30"), Full.MoonGroundLux > 0.25 && Full.MoonGroundLux < 0.30);
	const FNightIlluminationResult Quarter = FNightIllumination::Compute(MoonAt(90.0, 90.0));
	TestTrue(TEXT("quarter ~0.03"), Quarter.MoonGroundLux > 0.02 && Quarter.MoonGroundLux < 0.04);
	const FNightIlluminationResult New = FNightIllumination::Compute(MoonAt(178.0, 90.0));
	TestTrue(TEXT("new ~0"), New.MoonGroundLux < 1e-4);
	const FNightIlluminationResult Below = FNightIllumination::Compute(MoonAt(0.0, -5.0));
	TestEqual(TEXT("below horizon ground"), Below.MoonGroundLux, 0.0);
	TestTrue(TEXT("perigee brighter"), FNightIllumination::MoonToaLux(0.0, 357000.0) > FNightIllumination::MoonToaLux(0.0, 406000.0) * 1.25);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNightIllumSunTest, "CamSim.Environment.Illumination.SunAndFloor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FNightIllumSunTest::RunTest(const FString&)
{
	FNightIlluminationInput In;
	In.Ephem.SunElDeg = 60.0; In.Ephem.SunDistanceAu = 1.0; In.Ephem.MoonElDeg = -30.0;
	FNightIlluminationResult R = FNightIllumination::Compute(In);
	TestTrue(TEXT("sun TOA 127.5 klx"), FMath::IsNearlyEqual(R.SunToaLux, 127500.0, 1.0));
	In.Ephem.SunDistanceAu = 0.9833;   // perihelion
	TestTrue(TEXT("perihelion"), FNightIllumination::Compute(In).SunToaLux > 131000.0);
	In.Ephem.SunElDeg = -20.0;
	R = FNightIllumination::Compute(In);
	TestEqual(TEXT("sun ground at -20"), R.SunGroundLux, 0.0);
	TestTrue(TEXT("floor = 0.002 / (pi <T>)"), FMath::IsNearlyEqual(R.FloorRadianceCdM2,
		0.002 / (UE_DOUBLE_PI * FNightIllumination::FloorTransmittanceMean()), 1e-9));
	In.CloudCover01 = 1.0;
	TestTrue(TEXT("overcast dims floor"), FNightIllumination::Compute(In).FloorRadianceCdM2 < R.FloorRadianceCdM2 * 0.3);
	In.bSunEnabled = false; In.Ephem.SunElDeg = 60.0;
	TestEqual(TEXT("sun disabled"), FNightIllumination::Compute(In).SunToaLux, 0.0);
	// Air mass: 1 at zenith, ~38 at the horizon, monotonic.
	TestTrue(TEXT("X zenith"), FMath::IsNearlyEqual(FNightIllumination::KastenYoungAirMass(90.0), 1.0, 1e-3));
	TestTrue(TEXT("X horizon"), FNightIllumination::KastenYoungAirMass(0.0) > 30.0);
	double Prev = 0.0;
	for (double El = 90.0; El >= 0.0; El -= 1.0) { const double X = FNightIllumination::KastenYoungAirMass(El); TestTrue(TEXT("monotonic"), X >= Prev); Prev = X; }
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoonDiscNormaliserTest, "CamSim.Environment.MoonDisc.Normaliser",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMoonDiscNormaliserTest::RunTest(const FString&)
{
	// Numerically integrate Shade/MeanShading over the unit disc: the mean must be 1 (so E is conserved).
	for (double Alpha : { 0.0, 45.0, 90.0, 135.0 })
	{
		const int32 N = 801;
		double Sum = 0.0; int32 Count = 0;
		for (int32 J = 0; J < N; ++J) for (int32 I = 0; I < N; ++I)
		{
			const double U = -1.0 + 2.0 * (I + 0.5) / N, V = -1.0 + 2.0 * (J + 0.5) / N;
			if (U * U + V * V > 1.0) continue;
			Sum += FMoonDisc::Shade(U, V, Alpha); ++Count;
		}
		const double Mean = Sum / Count;
		TestTrue(FString::Printf(TEXT("mean shading α=%.0f"), Alpha), FMath::Abs(Mean / FMoonDisc::MeanShading(Alpha) - 1.0) < 0.01);
		// The lit limb is on the sun's side (+u).
		TestTrue(FString::Printf(TEXT("lit limb α=%.0f"), Alpha), FMoonDisc::Shade(0.9, 0.0, Alpha) > FMoonDisc::Shade(-0.9, 0.0, Alpha) - 1e-12);
	}
	TestTrue(TEXT("full mean = 2/3"), FMath::IsNearlyEqual(FMoonDisc::MeanShading(0.0), 2.0 / 3.0, 1e-6));
	return true;
}

// The dome draws the floor as L * T(view elevation) (M_SkyDome): its radiance is normalised so that the horizontal
// illuminance it delivers at the ground is night_floor_lux, the value FNightIllumination's ground figures and
// night_check.py N2 assume (it was 0.755 x that with L = E / pi).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNightIllumFloorDeliveredTest, "CamSim.Environment.Illumination.FloorDeliversNightFloorLux",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FNightIllumFloorDeliveredTest::RunTest(const FString&)
{
	FNightIlluminationInput In;
	In.Ephem.SunElDeg = -40.0; In.Ephem.MoonElDeg = -40.0;
	In.NightFloorLux = 0.002;
	const double L = FNightIllumination::Compute(In).FloorRadianceCdM2;
	// E = integral over the upper hemisphere of L T(el) sin(el) dOmega, dOmega = cos(el) d(el) d(az)
	double E = 0.0;
	const int32 N = 20000;
	for (int32 I = 0; I < N; ++I)
	{
		const double El = (I + 0.5) / N * 90.0;
		const double Rad = FMath::DegreesToRadians(El);
		E += L * FNightIllumination::Transmittance(El) * FMath::Sin(Rad) * FMath::Cos(Rad) * (UE_DOUBLE_PI / 2.0 / N) * 2.0 * UE_DOUBLE_PI;
	}
	AddInfo(FString::Printf(TEXT("floor radiance %.4g cd/m2 delivers %.5f lux (mean T %.4f)"), L, E, FNightIllumination::FloorTransmittanceMean()));
	TestTrue(TEXT("ground illuminance = night_floor_lux within 0.5 %"), FMath::Abs(E / 0.002 - 1.0) < 0.005);
	return true;
}

// The scene-exposure cap: the brightest moon disc (full, at perigee, at the zenith) times 2^cap is 16384 (1/4 of fp16's
// max), and no phase, distance or elevation gives a brighter disc than MaxMoonDiscLuminanceCdM2.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNightIllumExposureCapTest, "CamSim.Environment.Illumination.SceneExposureCapKeepsMoonInFp16",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FNightIllumExposureCapTest::RunTest(const FString&)
{
	const double LMax = FNightIllumination::MaxMoonDiscLuminanceCdM2();
	const double Cap = FNightIllumination::SceneExposureCapEv();
	AddInfo(FString::Printf(TEXT("max moon disc %.0f cd/m2, cap %.2f EV"), LMax, Cap));
	TestTrue(TEXT("disc x 2^cap = 16384"), FMath::IsNearlyEqual(LMax * FMath::Pow(2.0, Cap), 16384.0, 1.0));
	TestTrue(TEXT("a few thousand cd/m2"), LMax > 4000.0 && LMax < 12000.0);
	// The dome's peak: LumScale = TOA / Omega / MeanShading(alpha), times shade <= 1 and T(el).
	for (double Alpha : {0.0, 5.0, 30.0, 90.0})
	{
		for (double Km : {356500.0, 384400.0, 406700.0})
		{
			const double R = FMath::Asin(1737.4 / Km);
			const double Omega = 2.0 * UE_DOUBLE_PI * (1.0 - FMath::Cos(R));
			const double Peak = FNightIllumination::MoonToaLux(Alpha, Km) / Omega / FMoonDisc::MeanShading(Alpha)
				* FNightIllumination::Transmittance(90.0);
			TestTrue(FString::Printf(TEXT("alpha %.0f, %.0f km: %.0f <= max"), Alpha, Km, Peak), Peak <= LMax * 1.0001);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoonCloudScatterTest, "CamSim.Environment.Sky.MoonLightsCloudsOnlyAtNight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMoonCloudScatterTest::RunTest(const FString&)
{
	// UE treats a second atmosphere light on the clouds as expensive (~0.4 ms on the night bench): only while it matters.
	TestEqual(TEXT("night, moon up: white"), FNightIllumination::MoonCloudScatterScale(-12.0, true), FLinearColor::White);
	TestEqual(TEXT("day: black"), FNightIllumination::MoonCloudScatterScale(20.0, true), FLinearColor::Black);
	TestEqual(TEXT("sun just up: black"), FNightIllumination::MoonCloudScatterScale(0.5, true), FLinearColor::Black);
	TestEqual(TEXT("moon down: black"), FNightIllumination::MoonCloudScatterScale(-30.0, false), FLinearColor::Black);
	return true;
}
