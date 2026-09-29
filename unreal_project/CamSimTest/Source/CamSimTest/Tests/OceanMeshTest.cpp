// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Ocean/OceanMeshBuilder.h"
#include "Ocean/OceanSurface.h"
#include "Geospatial/EcefFrames.h"
#include "Geospatial/CigiFrames.h"

using namespace CamSimOcean;

namespace
{
	// UE world stand-in: ECEF in centimetres (any rigid transform would do).
	FVector EcefCm(double Lat, double Lon, double Alt) { return CamSimFrames::GeodeticToEcef(Lat, Lon, Alt) * 100.0; }
	double GeoidAt(double Lat, double Lon) { return -30.0 + 0.001 * Lat * Lon; }   // smooth, non-constant
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanMeshWarpTest, "CamSim.Ocean.Mesh.WarpCentreCellAndMonotonic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanMeshWarpTest::RunTest(const FString& Parameters)
{
	for (double R : { 400000.0, 25000.0, 1000.0 })
	{
		const double A = SolveWarpAlpha(R);
		const double Cell = WarpDistance(1.0 / (GridN / 2), R, A);
		const double Expected = FMath::Min(CentreCellM, R / (GridN / 2));   // small R: uniform grid
		TestEqual(*FString::Printf(TEXT("R %.0f: centre cell"), R), Cell, Expected, Expected * 0.05);
		TestEqual(*FString::Printf(TEXT("R %.0f: edge"), R), WarpDistance(1.0, R, A), R, 1e-6 * R);
		TestEqual(*FString::Printf(TEXT("R %.0f: odd"), R), WarpDistance(-0.3, R, A), -WarpDistance(0.3, R, A), 1e-9);
		double Prev = -1.0;
		bool bMono = true;
		for (int32 i = 0; i <= GridN / 2; ++i) { const double D = WarpDistance(double(i) / (GridN / 2), R, A); bMono &= D > Prev; Prev = D; }
		TestTrue(*FString::Printf(TEXT("R %.0f: monotonic"), R), bMono);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanMeshRadiusTest, "CamSim.Ocean.Mesh.HorizonRadius",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanMeshRadiusTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("1000 m alt, centre at nadir"), HorizonRadiusM(0.0, 1000.0, 400.0), 3570.0 * FMath::Sqrt(1000.0) * 1.1, 1.0);
	TestEqual(TEXT("adds centre offset"), HorizonRadiusM(5000.0, 1000.0, 400.0), 5000.0 + 3570.0 * FMath::Sqrt(1000.0) * 1.1, 1.0);
	TestEqual(TEXT("cap"), HorizonRadiusM(0.0, 20000.0, 400.0), 400000.0, 1e-6);
	TestEqual(TEXT("sea level floor 2 m"), HorizonRadiusM(0.0, -5.0, 400.0), 3570.0 * FMath::Sqrt(2.0) * 1.1, 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanMeshGeoidTest, "CamSim.Ocean.Mesh.VerticesOnGeoid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanMeshGeoidTest::RunTest(const FString& Parameters)
{
	FOceanSurface S([](double Lat, double Lon) { return TOptional<double>(GeoidAt(Lat, Lon)); });
	FOceanMeshData M;
	TestTrue(TEXT("built"), BuildOceanMesh(37.8, -122.45, 200000.0, S, &EcefCm, M));
	TestEqual(TEXT("vertex count"), M.Positions.Num(), (GridN + 1) * (GridN + 1));
	TestEqual(TEXT("triangle indices"), M.Triangles.Num(), GridN * GridN * 6);
	TestEqual(TEXT("cell sizes"), M.CellSize.Num(), M.Positions.Num());
	const int32 Mid = GridN / 2;
	for (const int32 Idx : { Mid * (GridN + 1) + Mid, (Mid + 64) * (GridN + 1) + Mid + 30, GridN * (GridN + 1) + GridN })
	{
		const FVector Ecef = (M.OriginWorld + M.Positions[Idx]) / 100.0;
		double Lat, Lon, Alt;
		CamSimFrames::EcefToGeodetic(Ecef, Lat, Lon, Alt);
		TestEqual(*FString::Printf(TEXT("vertex %d on geoid"), Idx), Alt, GeoidAt(Lat, Lon), 0.01);
	}
	TestTrue(TEXT("centre vertex at the origin"), M.Positions[Mid * (GridN + 1) + Mid].Size() < 1.0);

	FOceanSurface NoGrid([](double, double) { return TOptional<double>(); });
	TestFalse(TEXT("no geoid -> no mesh"), BuildOceanMesh(37.8, -122.45, 200000.0, NoGrid, &EcefCm, M));

	// Informational timing: build at R = 400 km, target < 10 ms (not asserted, just reported).
	const double StartSeconds = FPlatformTime::Seconds();
	FOceanMeshData TimedMesh;
	const bool bTimedBuilt = BuildOceanMesh(37.8, -122.45, 400000.0, S, &EcefCm, TimedMesh);
	const double ElapsedMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	TestTrue(TEXT("timed build succeeded"), bTimedBuilt);
	AddInfo(FString::Printf(TEXT("BuildOceanMesh at R=400km: %.3f ms (target < 10 ms)"), ElapsedMs));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanMeshRebuildTest, "CamSim.Ocean.Mesh.RebuildPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanMeshRebuildTest::RunTest(const FString& Parameters)
{
	FRebuildPolicy L; L.bHasMesh = false;
	TestTrue(TEXT("first build"), NeedsRebuild(L, 37.8, -122.45, 100000.0));
	L = { 37.8, -122.45, 100000.0, true };
	double Lat, Lon, Alt;
	CamSimFrames::OffsetGeodetic(37.8, -122.45, 0.0, FVector(400.0, 0.0, 0.0), Lat, Lon, Alt);   // 0.4% of R
	TestFalse(TEXT("small move"), NeedsRebuild(L, Lat, Lon, 100000.0));
	CamSimFrames::OffsetGeodetic(37.8, -122.45, 0.0, FVector(600.0, 0.0, 0.0), Lat, Lon, Alt);   // 0.6% of R
	TestTrue(TEXT("move > 0.5% R"), NeedsRebuild(L, Lat, Lon, 100000.0));
	TestFalse(TEXT("R +20%"), NeedsRebuild(L, 37.8, -122.45, 120000.0));
	TestTrue(TEXT("R +30%"), NeedsRebuild(L, 37.8, -122.45, 130000.0));
	TestTrue(TEXT("R -30%"), NeedsRebuild(L, 37.8, -122.45, 70000.0));
	return true;
}
