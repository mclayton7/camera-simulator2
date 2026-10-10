// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Tests/LandCoverTestTiles.h"
#include "Thermal/LandCoverGeometry.h"
#include "Thermal/LandCoverLookup.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverLookupCodeTest, "CamSim.Thermal.LandCover.CodeLookup.Codes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverLookupCodeTest::RunTest(const FString& Parameters)
{
	using namespace CamSimLandCoverTest;
	const FString Dir = TempDir(TEXT("CodeLookup"));
	// Tile (666, -2350) = +33.30_-117.50, PatternCodes: (row + 3 col) % 101
	WriteTileDir(Dir, { FTestTile{ 666, -2350, PatternCodes() } });
	FLandCoverCodeLookup L;
	L.Configure(Dir);
	TestTrue(TEXT("available"), L.IsAvailable());
	// cell (row 0, col 0) = north-west corner of the tile
	const double Lat = 33.35 - 0.5 / 12000.0, Lon = -117.50 + 0.5 / 12000.0;
	TestEqual(TEXT("NW cell"), static_cast<int32>(L.Code(Lat, Lon)), 0);
	TestEqual(TEXT("row 1 col 2"), static_cast<int32>(L.Code(Lat - 1.0 / 12000.0, Lon + 2.0 / 12000.0)), (1 + 3 * 2) % 101);
	TestEqual(TEXT("no tile -> 0"), static_cast<int32>(L.Code(10.0, 10.0)), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverLookupEdgesTest, "CamSim.Thermal.LandCover.CodeLookup.EdgesMatchResample",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverLookupEdgesTest::RunTest(const FString& Parameters)
{
	using namespace CamSimLandCoverTest;
	const FString Dir = TempDir(TEXT("CodeLookupEdges"));
	WriteTileDir(Dir, { FTestTile{ 666, -2350, PatternCodes() }, FTestTile{ 666, -2349, PatternCodes() },
	                    FTestTile{ 0, 3599, PatternCodes() }, FTestTile{ 0, -3600, PatternCodes() } });
	FLandCoverCodeLookup L;
	L.Configure(Dir);
	// Exactly on a tile boundary and the antimeridian: the same cell GlobalCell + CellToTile pick for the window resample.
	const double Pts[][2] = { { 33.35, -117.45 }, { 33.30, -117.45 }, { 0.025, 180.0 }, { 0.025, -180.0 }, { 0.025, 179.99999 } };
	for (const auto& P : Pts)
	{
		L.BeginTick();   // one point per tick: the points span four tiles, past the per-tick load budget
		FIntPoint Tile, In;
		CamSimLandCover::CellToTile(CamSimLandCover::GlobalCell(P[0], P[1]), Tile, In);
		const bool bHasTile = (Tile == FIntPoint(666, -2350)) || (Tile == FIntPoint(666, -2349)) || (Tile == FIntPoint(0, 3599)) || (Tile == FIntPoint(0, -3600));
		const int32 Expected = bHasTile ? (In.Y + 3 * In.X) % 101 : 0;
		TestEqual(*FString::Printf(TEXT("(%.5f, %.5f)"), P[0], P[1]), static_cast<int32>(L.Code(P[0], P[1])), Expected);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverLookupLoadBudgetTest, "CamSim.Thermal.LandCover.CodeLookup.LoadBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverLookupLoadBudgetTest::RunTest(const FString& Parameters)
{
	using namespace CamSimLandCoverTest;
	const FString Dir = TempDir(TEXT("CodeLookupBudget"));
	TArray<uint8> Road; Road.Init(200, TilePx * TilePx);
	// Three tiles side by side along 33.30 N: -117.50, -117.45, -117.40.
	WriteTileDir(Dir, { FTestTile{ 666, -2350, Road }, FTestTile{ 666, -2349, Road }, FTestTile{ 666, -2348, Road } });
	FLandCoverCodeLookup L;
	L.Configure(Dir);
	static_assert(FLandCoverCodeLookup::MaxLoadsPerTick == 2, "the test spends a budget of two loads");
	const double Lat = 33.32, Lons[] = { -117.48, -117.43, -117.38 };
	L.BeginTick();
	TestEqual(TEXT("1st load"), static_cast<int32>(L.Code(Lat, Lons[0])), 200);
	TestEqual(TEXT("2nd load"), static_cast<int32>(L.Code(Lat, Lons[1])), 200);
	TestEqual(TEXT("3rd tile deferred"), static_cast<int32>(L.Code(Lat, Lons[2])), 0);
	TestEqual(TEXT("cached tiles still answer"), static_cast<int32>(L.Code(Lat, Lons[0])), 200);
	TestEqual(TEXT("no tile: no load, 0"), static_cast<int32>(L.Code(10.0, 10.0)), 0);
	const FLandCoverLookupStats S = L.TakeStats();
	TestEqual(TEXT("loads"), S.Loads, 2);
	TestEqual(TEXT("deferred"), S.Deferred, 1);
	L.BeginTick();
	TestEqual(TEXT("next tick loads it"), static_cast<int32>(L.Code(Lat, Lons[2])), 200);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverLookupMissingTest, "CamSim.Thermal.LandCover.CodeLookup.MissingDir",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverLookupMissingTest::RunTest(const FString& Parameters)
{
	FLandCoverCodeLookup L;
	L.Configure(CamSimLandCoverTest::TempDir(TEXT("CodeLookupEmpty")));   // no index.json
	TestFalse(TEXT("not available"), L.IsAvailable());
	TestEqual(TEXT("code 0"), static_cast<int32>(L.Code(33.3, -117.5)), 0);
	TestEqual(TEXT("one warning"), L.TakeWarnings().Num(), 1);
	L.Code(33.3, -117.5);
	TestEqual(TEXT("no repeat warning"), L.TakeWarnings().Num(), 0);
	FLandCoverCodeLookup Off;
	Off.Configure(FString());
	TestEqual(TEXT("empty dir: code 0, no warning"), static_cast<int32>(Off.Code(33.3, -117.5)) + Off.TakeWarnings().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverQueryMaterialTest, "CamSim.CIGI.MaterialCode.Rule",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverQueryMaterialTest::RunTest(const FString& Parameters)
{
	using namespace CamSimLandCoverTest;
	const FString Dir = TempDir(TEXT("QueryMaterial"));
	TArray<uint8> Road; Road.Init(200, TilePx * TilePx);
	WriteTileDir(Dir, { FTestTile{ 666, -2350, Road } });
	FLandCoverCodeLookup L;
	L.Configure(Dir);
	using CamSimLandCover::QueryMaterialCode;
	TestEqual(TEXT("invalid"), QueryMaterialCode(false, false, false, 33.32, -117.48, &L), 0u);
	TestEqual(TEXT("sea surface"), QueryMaterialCode(true, true, false, 33.32, -117.48, &L), 80u);
	TestEqual(TEXT("entity"), QueryMaterialCode(true, false, true, 33.32, -117.48, &L), 0u);
	TestEqual(TEXT("terrain"), QueryMaterialCode(true, false, false, 33.32, -117.48, &L), 200u);
	TestEqual(TEXT("no lookup"), QueryMaterialCode(true, false, false, 33.32, -117.48, nullptr), 0u);
	TestEqual(TEXT("sea without lookup"), QueryMaterialCode(true, true, false, 33.32, -117.48, nullptr), 80u);
	return true;
}
