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
	// Winding UE draws from above (seen in the running app, Task 9): A -> east -> north.
	const int32 V1 = GridN + 1;
	TestTrue(TEXT("first quad winding"), M.Triangles.Num() >= 6 && M.Triangles[0] == 0 && M.Triangles[1] == 1 && M.Triangles[2] == V1
		&& M.Triangles[3] == 1 && M.Triangles[4] == V1 + 1 && M.Triangles[5] == V1);
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

	// Corner vertex has only one neighbour cell in each direction: its reported
	// size should equal that outermost cell's actual width, not half of it.
	{
		const double AlphaWarp = SolveWarpAlpha(200000.0);
		const double OuterCellWidth = WarpDistance(1.0, 200000.0, AlphaWarp)
			- WarpDistance(2.0 * (GridN - 1) / GridN - 1.0, 200000.0, AlphaWarp);
		const int32 CornerIdx = GridN * (GridN + 1) + GridN;
		TestEqual(TEXT("corner cell size equals the outermost cell width"), M.CellSize[CornerIdx].X, OuterCellWidth, 1e-6);
	}

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanMeshUniformGridTest, "CamSim.Ocean.Mesh.UniformGridBelowThreshold",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanMeshUniformGridTest::RunTest(const FString& Parameters)
{
	// R / (GridN/2) = 200 / 128 = 1.5625 m <= CentreCellM (2 m): the uniform-grid
	// branch of SolveWarpAlpha (Alpha == 0) is untested elsewhere in this file.
	const double R = 200.0;
	const double Alpha = SolveWarpAlpha(R);
	TestEqual(TEXT("alpha is exactly 0 (uniform)"), Alpha, 0.0, 0.0);

	for (double U : { -0.7, -0.2, 0.0, 0.35, 1.0 })
	{
		TestEqual(*FString::Printf(TEXT("linear at u=%.2f"), U), WarpDistance(U, R, Alpha), U * R, 1e-9);
	}

	FOceanSurface S([](double Lat, double Lon) { return TOptional<double>(GeoidAt(Lat, Lon)); });
	FOceanMeshData M;
	TestTrue(TEXT("built"), BuildOceanMesh(37.8, -122.45, R, S, &EcefCm, M));

	const double ExpectedCell = R / (GridN / 2);   // 1.5625 m, uniform everywhere
	const int32 V = GridN + 1;
	const int32 Mid = GridN / 2;
	TestEqual(TEXT("centre vertex cell size"), M.CellSize[Mid * V + Mid].X, ExpectedCell, 1e-6);
	TestEqual(TEXT("edge vertex cell size (r=0, c=mid)"), M.CellSize[0 * V + Mid].X, ExpectedCell, 1e-6);
	TestEqual(TEXT("corner vertex cell size"), M.CellSize[0].X, ExpectedCell, 1e-6);

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanMeshWindingTest, "CamSim.Ocean.Mesh.FrontFaceUp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanMeshWindingTest::RunTest(const FString& Parameters)
{
	// UE world as Cesium lays it out at the georeference origin: X = East, Y = South, Z = Up (cm).
	auto UeLike = [](double Lat, double Lon, double AltM)
	{
		const FVector Neu = CamSimFrames::GeodeticDeltaToNeu(37.8, -122.45, 0.0, Lat, Lon, AltM);
		return FVector(Neu.Y, -Neu.X, Neu.Z) * 100.0;
	};
	FOceanSurface S([](double, double) { return TOptional<double>(0.0); });
	FOceanMeshData M;
	TestTrue(TEXT("built"), BuildOceanMesh(37.8, -122.45, 1000.0, S, UeLike, M));
	const int32 Mid = GridN / 2;
	for (const int32 Tri : { 0, 1 })
	{
		const int32 Base = (Mid * GridN + Mid) * 6 + Tri * 3;
		const FVector& A = M.Positions[M.Triangles[Base]];
		const FVector& B = M.Positions[M.Triangles[Base + 1]];
		const FVector& C = M.Positions[M.Triangles[Base + 2]];
		// The winding seen from above in the running app (Task 9): with UE's cross product
		// in its left-handed frame, (B-A)^(C-A) of a front face points AWAY from the viewer.
		const double Dot = ((B - A) ^ (C - A)) | M.Normals[M.Triangles[Base]];
		TestTrue(*FString::Printf(TEXT("triangle %d front face up (dot %.3g < 0)"), Tri, Dot), Dot < 0.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanMeshCentreTest, "CamSim.Ocean.Mesh.CentreAndAnchorPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanMeshCentreTest::RunTest(const FString& Parameters)
{
	double Lat, Lon;
	ChooseCentre(37.80, -122.45, 37.82, -122.40, /*bFrameCentreValid=*/true, Lat, Lon);
	TestEqual(TEXT("frame centre used"), Lat, 37.82, 1e-12);
	ChooseCentre(37.80, -122.45, 0.0, 0.0, /*bFrameCentreValid=*/false, Lat, Lon);
	TestEqual(TEXT("nadir fallback"), Lon, -122.45, 1e-12);
	ChooseCentre(37.80, -122.45, NAN, 0.0, true, Lat, Lon);
	TestEqual(TEXT("NaN frame centre → nadir"), Lat, 37.80, 1e-12);

	TestTrue (TEXT("no anchor yet"), NeedsReanchor(false, 0, 0, 37.8, -122.45, false));
	TestFalse(TEXT("near anchor"), NeedsReanchor(true, 37.8, -122.45, 37.9, -122.45, false));
	TestTrue (TEXT("> 200 km"), NeedsReanchor(true, 37.8, -122.45, 39.8, -122.45, false));
	TestTrue (TEXT("teleport"), NeedsReanchor(true, 37.8, -122.45, 37.8, -122.45, true));

	// A far frame centre falls back to the nadir: at 10 km the horizon is ~393 km, so a
	// 400 km mesh can't be centred 450 km away and still reach the horizon round the camera.
	double FarLat, FarLon, Alt;
	CamSimFrames::OffsetGeodetic(37.80, -122.45, 0.0, FVector(450000.0, 0.0, 0.0), FarLat, FarLon, Alt);
	Lat = FarLat; Lon = FarLon;
	LimitCentreToMesh(37.80, -122.45, 10000.0, 400.0, Lat, Lon);
	TestEqual(TEXT("10 km alt, centre 450 km away -> nadir lat"), Lat, 37.80, 1e-12);
	TestEqual(TEXT("10 km alt, centre 450 km away -> nadir lon"), Lon, -122.45, 1e-12);
	double NearLat, NearLon;
	CamSimFrames::OffsetGeodetic(37.80, -122.45, 0.0, FVector(5000.0, 0.0, 0.0), NearLat, NearLon, Alt);
	Lat = NearLat; Lon = NearLon;
	LimitCentreToMesh(37.80, -122.45, 300.0, 400.0, Lat, Lon);
	TestEqual(TEXT("300 m alt, centre 5 km away kept"), Lat, NearLat, 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanMeshAnchorTest, "CamSim.Ocean.Mesh.AnchorFromCameraOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanMeshAnchorTest::RunTest(const FString& Parameters)
{
	// No world, mesh or material: the anchor must still follow the camera (boats need it).
	FOceanSurface S([](double Lat, double Lon) { return TOptional<double>(GeoidAt(Lat, Lon)); });
	FCentreTracker T;
	FCentreDecision D = UpdateCentreAndAnchor(T, S, 37.81, -122.42, 1000.0, 37.815, -122.42, true, 400.0);
	TestTrue(TEXT("anchored on the first tick"), S.GetWaves().HasAnchor());
	TestEqual(TEXT("anchor at the frame centre"), S.GetWaves().GetAnchorLat(), 37.815, 1e-12);
	TestFalse(TEXT("first tick is not a teleport"), D.bTeleport);
	TestTrue(TEXT("plane coords non-zero away from the anchor"),
		FMath::Abs(S.GetWaves().PlaneCoords(37.825, -122.42, 0.0).X) > 1000.0);

	D = UpdateCentreAndAnchor(T, S, 37.82, -122.42, 1000.0, 37.825, -122.42, true, 400.0);   // ~1 km move
	TestFalse(TEXT("1 km move: no teleport"), D.bTeleport);
	TestEqual(TEXT("1 km move: anchor held"), S.GetWaves().GetAnchorLat(), 37.815, 1e-12);

	D = UpdateCentreAndAnchor(T, S, 37.92, -122.42, 1000.0, 37.925, -122.42, true, 400.0);   // ~11 km hop
	TestTrue(TEXT("11 km hop: teleport"), D.bTeleport);
	TestEqual(TEXT("teleport re-anchors"), S.GetWaves().GetAnchorLat(), 37.925, 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanMeshRateLimitTest, "CamSim.Ocean.Mesh.RebuildRateLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanMeshRateLimitTest::RunTest(const FString& Parameters)
{
	const FRebuildPolicy L{ 37.8, -122.45, 100000.0, true };
	const double Moved = 37.8 + 0.01;   // ~1.1 km: past 0.5% of R
	TestFalse(TEXT("moved, 0.1 s after the last build: wait"), NeedsRebuild(L, Moved, -122.45, 100000.0, 0.1, false));
	TestTrue (TEXT("moved, 0.3 s after: rebuild"), NeedsRebuild(L, Moved, -122.45, 100000.0, 0.3, false));
	TestFalse(TEXT("not moved, 1 s after: no rebuild"), NeedsRebuild(L, 37.8, -122.45, 100000.0, 1.0, false));
	TestTrue (TEXT("forced (teleport / origin shift) at 0 s"), NeedsRebuild(L, 37.8, -122.45, 100000.0, 0.0, true));
	TestTrue (TEXT("first build at 0 s"), NeedsRebuild(FRebuildPolicy(), 37.8, -122.45, 100000.0, 0.0, false));
	return true;
}
