// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Vegetation/VegetationGrid.h"

using namespace CamSimVegetation;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegGridRoundTripTest, "CamSim.Vegetation.Grid.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegGridRoundTripTest::RunTest(const FString& Parameters)
{
	const double Pts[][2] = { { 33.302, -117.355 }, { 0.0, 0.0 }, { -45.5, 170.25 }, { 84.9, -10.0 }, { 37.7749, -122.4194 } };
	for (const auto& P : Pts)
	{
		const FVegCellId C = CellOf(P[0], P[1], 100.0);
		const FVegCellBounds B = CellBounds(C, 100.0);
		TestTrue(*FString::Printf(TEXT("(%f,%f) inside its cell"), P[0], P[1]),
			P[0] >= B.SouthDeg && P[0] < B.NorthDeg && P[1] >= B.WestDeg && P[1] < B.EastDeg);
		double CLat, CLon; CellCentre(C, 100.0, CLat, CLon);
		TestTrue(TEXT("centre maps to the same cell"), CellOf(CLat, CLon, 100.0) == C);
		TestTrue(TEXT("cell is ~100 m N-S"), FMath::IsNearlyEqual((B.NorthDeg - B.SouthDeg) * MetresPerDegLat, 100.0, 1e-6));
		const double EwM = GroundDistanceM(CLat, B.WestDeg, CLat, B.EastDeg);
		TestTrue(*FString::Printf(TEXT("cell is ~100 m E-W (%f)"), EwM), FMath::Abs(EwM - 100.0) < 0.5);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegGridNeighboursTest, "CamSim.Vegetation.Grid.NoGapsAcrossRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegGridNeighboursTest::RunTest(const FString& Parameters)
{
	// Sample a 2 km square every 7 m: every sample belongs to exactly one cell whose bounds contain it (no gaps, no overlap).
	int32 Bad = 0;
	for (double N = -1000.0; N <= 1000.0; N += 7.0)
		for (double E = -1000.0; E <= 1000.0; E += 7.0)
		{
			const double Lat = 33.3 + N / MetresPerDegLat, Lon = -117.4 + E / (MetresPerDegLat * FMath::Cos(FMath::DegreesToRadians(33.3)));
			const FVegCellBounds B = CellBounds(CellOf(Lat, Lon, 100.0), 100.0);
			Bad += !(Lat >= B.SouthDeg && Lat < B.NorthDeg && Lon >= B.WestDeg && Lon < B.EastDeg);
		}
	TestEqual(TEXT("samples outside their cell"), Bad, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegGridAntimeridianTest, "CamSim.Vegetation.Grid.Antimeridian",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegGridAntimeridianTest::RunTest(const FString& Parameters)
{
	const FVegCellId East = CellOf(10.0, 179.9999, 100.0), West = CellOf(10.0, -179.9999, 100.0), Wrapped = CellOf(10.0, 180.0001, 100.0);
	TestEqual(TEXT("last column"), East.Col, NumCols(East.Row, 100.0) - 1);
	TestEqual(TEXT("first column"), West.Col, 0);
	TestTrue(TEXT("180.0001 wraps to -179.9999's cell"), Wrapped == West);
	const TArray<FVegCellId> Cells = CellsWithin(10.0, 179.9995, 500.0, 100.0);
	TSet<FVegCellId> Unique(Cells);
	TestEqual(TEXT("no duplicates across the seam"), Unique.Num(), Cells.Num());
	bool bBoth = false, bLow = false, bHigh = false;
	for (const FVegCellId& C : Cells) { bLow |= C.Col == 0; bHigh |= C.Col == NumCols(C.Row, 100.0) - 1; }
	bBoth = bLow && bHigh;
	TestTrue(TEXT("cells on both sides of the seam"), bBoth);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegGridPolarTest, "CamSim.Vegetation.Grid.PolarCutoff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegGridPolarTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("85.1 refused"), IsLatAllowed(85.1));
	TestTrue(TEXT("85.0 allowed"), IsLatAllowed(85.0));
	TestEqual(TEXT("no cells near the pole"), CellsWithin(89.0, 0.0, 1000.0, 100.0).Num(), 0);
	TestFalse(TEXT("NaN refused"), IsLatAllowed(std::numeric_limits<double>::quiet_NaN()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegGridWithinTest, "CamSim.Vegetation.Grid.CellsWithin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegGridWithinTest::RunTest(const FString& Parameters)
{
	const TArray<FVegCellId> Cells = CellsWithin(33.302, -117.355, 1000.0, 100.0);
	// pi * 10^2 ~ 314 cells of 100 m within 1 km (centre test).
	TestTrue(*FString::Printf(TEXT("about 314 cells (%d)"), Cells.Num()), Cells.Num() > 290 && Cells.Num() < 340);
	double Prev = -1.0;
	bool bSorted = true, bInside = true;
	for (const FVegCellId& C : Cells)
	{
		double Lat, Lon; CellCentre(C, 100.0, Lat, Lon);
		const double D = GroundDistanceM(33.302, -117.355, Lat, Lon);
		bSorted &= D >= Prev; Prev = D; bInside &= D <= 1000.0;
	}
	TestTrue(TEXT("nearest first"), bSorted);
	TestTrue(TEXT("all centres within the radius"), bInside);
	TestTrue(TEXT("first is the camera's cell"), Cells.Num() > 0 && Cells[0] == CellOf(33.302, -117.355, 100.0));
	TestTrue(TEXT("same list twice (deterministic)"), Cells == CellsWithin(33.302, -117.355, 1000.0, 100.0));
	return true;
}
