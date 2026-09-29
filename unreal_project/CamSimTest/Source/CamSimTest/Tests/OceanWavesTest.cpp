// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Ocean/OceanWaves.h"
#include "Ocean/FBeaufortTable.h"
#include "Geospatial/EcefFrames.h"
#include "Geospatial/CigiFrames.h"

namespace
{
	FOceanWave Wave(double H, double L, double From, double Q = 0.0, double T = 0.0)
	{
		FOceanWave W; W.HeightM = H; W.LengthM = L; W.FromDeg = From; W.Steepness = Q; W.PeriodS = T; return W;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanWavesBeaufortTest, "CamSim.Ocean.Waves.BeaufortSignificantHeight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanWavesBeaufortTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Beaufort 0 → no waves"), FOceanWaves::FromBeaufort(0.0, 270.0, 0.5).Num(), 0);
	for (int32 B = 1; B <= 12; ++B)
	{
		FOceanWaves W;
		W.SetWaves(FOceanWaves::FromBeaufort(B, 270.0, 0.5));
		const double Table = FBeaufortTable::Sample(B).WaveHtM;
		TestEqual(*FString::Printf(TEXT("Beaufort %d: 4 waves"), B), W.GetWaves().Num(), 4);
		TestEqual(*FString::Printf(TEXT("Beaufort %d: Hs"), B), W.SignificantHeight(), Table, Table * 0.01);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanWavesSteepnessTest, "CamSim.Ocean.Waves.SteepnessBound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanWavesSteepnessTest::RunTest(const FString& Parameters)
{
	for (double C : { 0.0, 0.5, 1.0, 3.0 })
	{
		FOceanWaves W;
		W.SetWaves(FOceanWaves::FromBeaufort(6.0, 200.0, C));
		double Sum = 0.0;
		for (int32 i = 0; i < W.GetWaves().Num(); ++i)
		{
			Sum += W.GetWaves()[i].Steepness * W.WaveNumber(i) * W.GetWaves()[i].HeightM * 0.5;
		}
		TestTrue(*FString::Printf(TEXT("choppiness %.1f: sum Q k a = %.4f <= 1"), C, Sum), Sum <= 1.0 + 1e-9);
		TestEqual(*FString::Printf(TEXT("choppiness %.1f: sum = min(C, 1)"), C), Sum, FMath::Min(C, 1.0), 1e-9);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanWavesDispersionTest, "CamSim.Ocean.Waves.Dispersion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanWavesDispersionTest::RunTest(const FString& Parameters)
{
	FOceanWaves W;
	W.SetWaves({ Wave(1.0, 100.0, 0.0), Wave(1.0, 100.0, 0.0, 0.0, 8.0) });
	TestEqual(TEXT("deep water: w = sqrt(g k)"), W.AngularFrequency(0), FMath::Sqrt(FOceanWaves::G * 2.0 * PI / 100.0), 1e-12);
	TestEqual(TEXT("host period: w = 2 pi / T"), W.AngularFrequency(1), 2.0 * PI / 8.0, 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanWavesDirectionTest, "CamSim.Ocean.Waves.DirectionFrom270MovesEast",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanWavesDirectionTest::RunTest(const FString& Parameters)
{
	FOceanWaves W;
	W.SetWaves({ Wave(2.0, 60.0, 270.0) });
	const FVector2D D = W.TravelDir(0);
	TestEqual(TEXT("travel north component"), D.X, 0.0, 1e-12);
	TestEqual(TEXT("travel east component"), D.Y, 1.0, 1e-12);
	const double C = W.AngularFrequency(0) / W.WaveNumber(0);   // phase speed
	W.SetTime(1000.0);
	const double H0 = W.HeightAtPlane(0.0, 10.0);
	W.SetTime(1002.0);
	TestEqual(TEXT("crest pattern moved east by c*dt"), W.HeightAtPlane(0.0, 10.0 + 2.0 * C), H0, 1e-6);
	TestTrue(TEXT("not west"), FMath::Abs(W.HeightAtPlane(0.0, 10.0 - 2.0 * C) - H0) > 1e-3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanWavesPlaneTest, "CamSim.Ocean.Waves.PlaneCoordsAreEcefProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanWavesPlaneTest::RunTest(const FString& Parameters)
{
	FOceanWaves W;
	W.SetAnchor(37.8, -122.45);
	TestTrue(TEXT("anchor set"), W.HasAnchor());
	// A point ~200 km NE: plane coords = (ECEF - anchor ECEF) . axes.
	double Lat, Lon, Alt;
	CamSimFrames::OffsetGeodetic(37.8, -122.45, 0.0, FVector(140000.0, 140000.0, 0.0), Lat, Lon, Alt);
	const FVector Rel = CamSimFrames::GeodeticToEcef(Lat, Lon, -30.0) - W.GetAnchorEcef();
	const FVector2D P = W.PlaneCoords(Lat, Lon, -30.0);
	TestEqual(TEXT("north"), P.X, FVector::DotProduct(Rel, W.GetAxisNorthEcef()), 1e-3);
	TestEqual(TEXT("east"),  P.Y, FVector::DotProduct(Rel, W.GetAxisEastEcef()),  1e-3);
	TestTrue(TEXT("roughly 140 km north"), FMath::Abs(P.X - 140000.0) < 2000.0);
	TestEqual(TEXT("axes orthonormal"), FVector::DotProduct(W.GetAxisNorthEcef(), W.GetAxisEastEcef()), 0.0, 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanWavesInvertTest, "CamSim.Ocean.Waves.HeightMatchesBruteForce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanWavesInvertTest::RunTest(const FString& Parameters)
{
	for (const double Chop : { 0.8, 1.0 })
	{
		FOceanWaves W;
		W.SetWaves(FOceanWaves::FromBeaufort(6.0, 240.0, Chop));
		W.SetTime(1234.5);
		const double Tol = Chop < 0.9 ? 0.01 : 0.05;
		double MaxErr = 0.0;
		for (int32 s = 0; s < 40; ++s)
		{
			const double N = 3.1 * s, E = -2.3 * s;
			// Brute force: search the parameter point whose displaced position lands on (N, E).
			double Best = TNumericLimits<double>::Max(), BestZ = 0.0;
			for (double pn = N - 6.0; pn <= N + 6.0; pn += 0.05)
			for (double pe = E - 6.0; pe <= E + 6.0; pe += 0.05)
			{
				const FVector D = W.Displacement(pn, pe);
				const double Err = FMath::Square(pn + D.X - N) + FMath::Square(pe + D.Y - E);
				if (Err < Best) { Best = Err; BestZ = D.Z; }
			}
			MaxErr = FMath::Max(MaxErr, FMath::Abs(W.HeightAtPlane(N, E) - BestZ));
		}
		TestTrue(*FString::Printf(TEXT("choppiness %.1f: max |height - brute| = %.4f m <= %.2f"), Chop, MaxErr, Tol), MaxErr <= Tol);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanWavesNormalTest, "CamSim.Ocean.Waves.NormalSingleWave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanWavesNormalTest::RunTest(const FString& Parameters)
{
	// Q = 0 sine along north: z = a cos(k N + phase); slope dz/dN = -a k sin(...).
	FOceanWaves W;
	W.SetWaves({ Wave(2.0, 40.0, 180.0) });   // from south → travels north
	W.SetTime(0.0);
	const double k = W.WaveNumber(0), a = 1.0;
	for (double N : { 0.0, 3.0, 7.5, 12.0 })
	{
		const FVector Nrm = W.NormalAtPlane(N, 0.0);
		const double Slope = -a * k * FMath::Sin(k * N + W.Phase(0));
		const FVector Expected = FVector(-Slope, 0.0, 1.0).GetSafeNormal();
		TestTrue(*FString::Printf(TEXT("normal at N=%.1f"), N), Nrm.Equals(Expected, 1e-9));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanWavesTimeTest, "CamSim.Ocean.Waves.PhaseFromSimTime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanWavesTimeTest::RunTest(const FString& Parameters)
{
	FOceanWaves W;
	W.SetWaves({ Wave(1.0, 30.0, 90.0) });
	W.SetTime(1.7e9);   // Unix-epoch sim seconds: phase stays precise
	const double P0 = W.Phase(0);
	TestTrue(TEXT("phase wrapped"), P0 >= 0.0 && P0 < 2.0 * PI);
	W.SetTime(1.7e9);
	TestEqual(TEXT("frozen time → same surface"), W.Phase(0), P0, 0.0);
	W.SetTime(1.7e9 + 1.0);
	const double Expected = FMath::Fmod(P0 - W.AngularFrequency(0) + 4.0 * PI, 2.0 * PI);
	TestEqual(TEXT("1 s later: phase - w"), W.Phase(0), Expected, 1e-5);
	return true;
}
