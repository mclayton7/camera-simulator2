// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Environment/Ephemeris.h"

namespace
{
	struct FEphemRef { int32 Y, Mo, D, H, Mi; double Lat, Lon, SunAz, SunEl, MoonAz, MoonEl, Illum, MoonKm, Last; };
	// From scripts/sky/make_ephemeris_refs.py (JPL DE421, skyfield, apparent = with aberration, no refraction).
	const FEphemRef Refs[] = {
	{2026,10,16,6,0, 33.3, -117.35, 312.6906, -57.2202, 246.7443, -16.7400, 0.2800, 406154, 357.4261},
	{2026,10,26,7,0, 33.3, -117.35, 337.9491, -67.8194, 153.1094, 72.1309, 0.9982, 362405, 22.3236},
	{2026,11,24,8,0, 37.77, -122.42, 2.9229, -72.7814, 204.0726, 75.5026, 0.9969, 355309, 60.8785},
	{2026,12,1,4,30, 30.46, -86.55, 296.8660, -73.0438, 74.0881, -12.6847, 0.5088, 378739, 51.0045},
	{2027,3,22,10,0, 32.65, -114.6, 50.4250, -44.0245, 229.6138, 39.8970, 0.9991, 368287, 215.0879},
	{2026,6,21,12,0, 78.22, 15.65, 197.0215, 34.7579, 110.1330, 3.2140, 0.4583, 385691, 105.3515},
	{2026,12,21,0,0, -33.87, 151.21, 74.5200, 63.2207, 122.1706, -69.8726, 0.8656, 373023, 240.7928},
	{2025,1,13,22,0, 33.3, -117.35, 212.3464, 28.1572, 29.3863, -25.2529, 0.9984, 384033, 326.2813},
	{2026,9,26,3,0, 51.48, 0.0, 54.6975, -26.1155, 241.2462, 20.6287, 0.9957, 379484, 49.9401},
	{2026,7,4,2,15, 64.84, -147.72, 257.3091, 31.1880, 22.0328, -36.9089, 0.8502, 399237, 168.1447},
	{2024,4,8,18,18, 23.0, -105.0, 144.0552, 71.3638, 143.9309, 71.3881, 0.0000, 353753, 6.9962},
	};
	double AngDiff(double A, double B) { return FMath::Abs(FMath::Fmod(A - B + 540.0, 360.0) - 180.0); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEphemerisMatchesJplTest, "CamSim.Environment.Ephemeris.MatchesJpl",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEphemerisMatchesJplTest::RunTest(const FString&)
{
	for (const FEphemRef& R : Refs)
	{
		const FEphemerisResult E = FEphemeris::Compute(FDateTime(R.Y, R.Mo, R.D, R.H, R.Mi), R.Lat, R.Lon, 0.0);
		const FString At = FString::Printf(TEXT("%04d-%02d-%02d %02d:%02d %.2f,%.2f"), R.Y, R.Mo, R.D, R.H, R.Mi, R.Lat, R.Lon);
		TestTrue(At + TEXT(" sun el"), FMath::Abs(E.SunElDeg - R.SunEl) < 0.05);
		if (R.SunEl > -85.0) TestTrue(At + TEXT(" sun az"), AngDiff(E.SunAzDeg, R.SunAz) * FMath::Cos(FMath::DegreesToRadians(R.SunEl)) < 0.05);
		TestTrue(At + TEXT(" moon el"), FMath::Abs(E.MoonElDeg - R.MoonEl) < 0.2);
		TestTrue(At + TEXT(" moon az"), AngDiff(E.MoonAzDeg, R.MoonAz) * FMath::Cos(FMath::DegreesToRadians(R.MoonEl)) < 0.2);
		TestTrue(At + TEXT(" illum"), FMath::Abs(E.MoonIlluminatedFraction - R.Illum) < 0.01);
		TestTrue(At + TEXT(" moon km"), FMath::Abs(E.MoonDistanceKm - R.MoonKm) < 300.0);
		TestTrue(At + TEXT(" LAST"), AngDiff(E.LocalSiderealDeg, R.Last) < 0.01);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEphemerisEnuTest, "CamSim.Environment.Ephemeris.EnuMatchesAzEl",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEphemerisEnuTest::RunTest(const FString&)
{
	const FVector3d N = FEphemeris::AzElToEnu(0.0, 0.0);
	TestTrue(TEXT("north"), N.Equals(FVector3d(0, 1, 0), 1e-12));
	const FVector3d E = FEphemeris::AzElToEnu(90.0, 0.0);
	TestTrue(TEXT("east"), E.Equals(FVector3d(1, 0, 0), 1e-12));
	const FVector3d Z = FEphemeris::AzElToEnu(123.0, 90.0);
	TestTrue(TEXT("zenith"), Z.Equals(FVector3d(0, 0, 1), 1e-12));
	const FEphemerisResult R = FEphemeris::Compute(FDateTime(2026, 10, 16, 6, 0), 33.3, -117.35, 0.0);
	TestTrue(TEXT("moon enu"), R.MoonEnu.Equals(FEphemeris::AzElToEnu(R.MoonAzDeg, R.MoonElDeg), 1e-9));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEphemerisPolarTest, "CamSim.Environment.Ephemeris.PolarNoNaN",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEphemerisPolarTest::RunTest(const FString&)
{
	for (double Lat : { 90.0, -90.0, 89.9999, -89.9999 })
	{
		for (int32 H = 0; H < 24; H += 6)
		{
			const FEphemerisResult R = FEphemeris::Compute(FDateTime(2026, 6, 21, H, 0), Lat, 10.0, 0.0);
			const double Vals[] = { R.SunAzDeg, R.SunElDeg, R.MoonAzDeg, R.MoonElDeg, R.MoonIlluminatedFraction,
				R.SunEnu.X, R.SunEnu.Y, R.SunEnu.Z, R.MoonEnu.X, R.MoonEnu.Y, R.MoonEnu.Z, R.LocalSiderealDeg };
			for (double V : Vals) TestTrue(FString::Printf(TEXT("finite lat %.4f h %d"), Lat, H), FMath::IsFinite(V));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEphemerisDeltaTTest, "CamSim.Environment.Ephemeris.DeltaT",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEphemerisDeltaTTest::RunTest(const FString&)
{
	// Espenak–Meeus 2005–2050: ΔT = 62.92 + 0.32217 t + 0.005589 t², t = y − 2000 (y = year + (month − 0.5)/12).
	TestTrue(TEXT("2026"), FMath::Abs(FEphemeris::DeltaTSeconds(2026, 1) - 75.0) < 2.0);
	TestTrue(TEXT("J2000 JD"), FMath::Abs(FEphemeris::JulianDayUtc(FDateTime(2000, 1, 1, 12, 0)) - 2451545.0) < 1e-9);
	return true;
}
