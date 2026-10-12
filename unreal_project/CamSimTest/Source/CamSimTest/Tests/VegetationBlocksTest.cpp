// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Vegetation/VegetationBlocks.h"

using namespace CamSimVegetation;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegBlocksGeometryTest, "CamSim.Vegetation.Blocks.Geometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegBlocksGeometryTest::RunTest(const FString& Parameters)
{
	const FVegCellId C = CellOf(33.3, -117.4, 100.0);
	const FVegBlockId B = BlockOf(C, 4, 100.0);
	const TArray<FVegCellId> Cells = CellsOfBlock(B, 4, 100.0);
	TestTrue(*FString::Printf(TEXT("about 16 cells (%d)"), Cells.Num()), Cells.Num() >= 12 && Cells.Num() <= 20);
	TestTrue(TEXT("contains its cell"), Cells.Contains(C));
	for (const FVegCellId& X : Cells) TestTrue(TEXT("every cell maps back"), BlockOf(X, 4, 100.0) == B);
	TestEqual(TEXT("negative rows floor"), BlockOf({ -1, 0 }, 4, 100.0).Row, -1);
	TestTrue(TEXT("row 4, col 3 -> block (1, 0)"), BlockOf({ 4, 3 }, 4, 100.0) == FVegBlockId{ 1, 0 });
	// Blocks stay compact far from 180 W (the grid's columns shift row to row): every cell centre within ~0.75 block of
	// its block centre, at Pendleton and east of Greenwich.
	for (const double Lon : { -117.4, 120.0 })
	{
		const FVegBlockId X = BlockOf(CellOf(33.3, Lon, 100.0), 4, 100.0);
		double BLa, BLo; BlockCentre(X, 4, 100.0, BLa, BLo);
		double Worst = 0.0;
		int32 Count = 0;
		for (const FVegCellId& Cc : CellsOfBlock(X, 4, 100.0))
		{
			double CLa, CLo; CellCentre(Cc, 100.0, CLa, CLo);
			Worst = FMath::Max(Worst, GroundDistanceM(BLa, BLo, CLa, CLo));
			++Count;
		}
		TestTrue(*FString::Printf(TEXT("lon %.1f: compact block (%d cells, worst %.0f m)"), Lon, Count, Worst), Worst < 300.0 && Count >= 12);
	}
	double La, Lo; BlockCentre(B, 4, 100.0, La, Lo);
	TestTrue(TEXT("centre within 300 m of the cell"), GroundDistanceM(La, Lo, 33.3, -117.4) < 300.0);
	const TArray<FVegBlockId> W = BlocksWithin(33.3, -117.4, 4000.0, 100.0, 4);
	TestTrue(*FString::Printf(TEXT("~pi*40^2/16 = 314 blocks (%d)"), W.Num()), W.Num() > 250 && W.Num() < 380);
	double PrevD = -1.0; bool bSorted = true, bInside = true;
	for (const FVegBlockId& X : W)
	{
		double A, O; BlockCentre(X, 4, 100.0, A, O);
		const double D = GroundDistanceM(33.3, -117.4, A, O);
		bSorted &= D >= PrevD; bInside &= D <= 4000.0;
		PrevD = D;
	}
	TestTrue(TEXT("nearest first"), bSorted);
	TestTrue(TEXT("all within the radius"), bInside);
	// Column widths differ per row, so a block's cells shear (~60 m per row at 117 W): its centroid is near, not always nearest.
	TestTrue(TEXT("the camera's block is among the first four"), W.IndexOfByKey(B) >= 0 && W.IndexOfByKey(B) < 4);
	const TArray<FVegBlockId> Dateline = BlocksWithin(10.0, 179.999, 1500.0, 100.0, 4);
	bool bBothSides = false;
	for (const FVegBlockId& X : Dateline) { double A, O; BlockCentre(X, 4, 100.0, A, O); bBothSides |= O < -179.0; }
	TestTrue(TEXT("wraps the antimeridian"), bBothSides);
	TestEqual(TEXT("none past 85 deg"), BlocksWithin(88.0, 0.0, 1000.0, 100.0, 4).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegTierHysteresisTest, "CamSim.Vegetation.Blocks.TierHysteresis",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegTierHysteresisTest::RunTest(const FString& Parameters)
{
	FVegetationTierPlanner P({});
	const FVegBlockId Home = BlockOf(CellOf(33.3, -117.4, 100.0), 4, 100.0);
	double La, Lo; BlockCentre(Home, 4, 100.0, La, Lo);
	P.Update(La, Lo);
	TestTrue(TEXT("home near"), P.GetTier(Home) == TOptional<EVegTier>(EVegTier::Near));
	TestTrue(TEXT("its cells are wanted near"), P.GetNearCells().Contains(CellOf(33.3, -117.4, 100.0)));
	const double DLat = 1.0 / MetresPerDegLat;
	P.Update(La + 1100.0 * DLat, Lo);   // 1.1 km: past NearM, inside 1.2 NearM
	TestTrue(TEXT("still near (hysteresis)"), P.GetTier(Home) == TOptional<EVegTier>(EVegTier::Near));
	P.Update(La + 1300.0 * DLat, Lo);
	TestTrue(TEXT("far past 1.2 NearM"), P.GetTier(Home) == TOptional<EVegTier>(EVegTier::Far));
	TestTrue(TEXT("listed far"), P.GetFarBlocks().Contains(Home));
	TestFalse(TEXT("its cells no longer wanted near"), P.GetNearCells().Contains(CellOf(33.3, -117.4, 100.0)));
	P.Update(La + 1050.0 * DLat, Lo);
	TestTrue(TEXT("far stays far until inside NearM"), P.GetTier(Home) == TOptional<EVegTier>(EVegTier::Far));
	P.Update(La + 4300.0 * DLat, Lo);
	TestTrue(TEXT("far inside 1.1 FarM"), P.GetTier(Home) == TOptional<EVegTier>(EVegTier::Far));
	const TArray<FVegetationTierPlanner::FChange> Dropped = P.Update(La + 4500.0 * DLat, Lo);
	TestFalse(TEXT("dropped past 1.1 FarM"), P.GetTier(Home).IsSet());
	TestTrue(TEXT("change reported"), Dropped.ContainsByPredicate([&](const FVegetationTierPlanner::FChange& C) { return C.Block == Home && !C.To.IsSet(); }));
	P.Update(La, Lo);
	TestTrue(TEXT("back: near"), P.GetTier(Home) == TOptional<EVegTier>(EVegTier::Near));

	FVegetationTierPlanner::FParams NoFar; NoFar.FarM = 0.0;
	FVegetationTierPlanner Q(NoFar);
	Q.Update(La, Lo);
	Q.Update(La + 1300.0 * DLat, Lo);
	TestFalse(TEXT("no far tier: dropped"), Q.GetTier(Home).IsSet());
	TestEqual(TEXT("no far blocks"), Q.GetFarBlocks().Num(), 0);
	Q.Update(La + 50000.0 * DLat, Lo);   // a 50 km cut
	TestFalse(TEXT("nothing near the old place"), Q.GetNearCells().ContainsByPredicate([&](const FVegCellId& C) { return BlockOf(C, 4, 100.0) == Home; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegTierThrottleTest, "CamSim.Vegetation.Blocks.UpdateOnlyAfterMoving",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegTierThrottleTest::RunTest(const FString& Parameters)
{
	FVegetationTierPlanner P({});
	TestTrue(TEXT("first update plans"), P.Update(33.3, -117.4).Num() > 0);
	TestEqual(TEXT("a 10 m move changes nothing"), P.Update(33.3 + 10.0 / MetresPerDegLat, -117.4).Num(), 0);
	const int32 Near = P.GetNearCells().Num();
	TestTrue(TEXT("near cells kept"), Near > 0);
	P.Update(33.3 + 2000.0 / MetresPerDegLat, -117.4);
	TestTrue(TEXT("a 2 km move replans"), P.GetNearCells().Num() > 0 && !P.GetNearCells().Contains(CellOf(33.3, -117.4, 100.0)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegTierSlantTest, "CamSim.Vegetation.Blocks.SlantDistance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegTierSlantTest::RunTest(const FString& Parameters)
{
	// Tiers by 3D distance from a camera HeightM above the ground: what is planned is within its cull distance.
	const FVegBlockId Home = BlockOf(CellOf(33.3, -117.4, 100.0), 4, 100.0);
	double La, Lo; BlockCentre(Home, 4, 100.0, La, Lo);
	const double DLat = 1.0 / MetresPerDegLat;
	FVegetationTierPlanner Low({});
	Low.Update(La, Lo, 0.0);
	TestTrue(TEXT("on the ground: near"), Low.GetTier(Home) == TOptional<EVegTier>(EVegTier::Near));
	FVegetationTierPlanner High({});
	High.Update(La, Lo, 1500.0);
	TestTrue(TEXT("1.5 km above: the block below is far, not near"), High.GetTier(Home) == TOptional<EVegTier>(EVegTier::Far));
	TestEqual(TEXT("1.5 km above: no near cells"), High.GetNearCells().Num(), 0);
	int32 TooFar = 0;
	for (const FVegBlockId& B : High.GetFarBlocks())
	{
		double A, O; BlockCentre(B, 4, 100.0, A, O);
		TooFar += FMath::Sqrt(FMath::Square(GroundDistanceM(La, Lo, A, O)) + FMath::Square(1500.0)) >= 4000.0;
	}
	TestEqual(TEXT("every far block within 4 km in 3D"), TooFar, 0);
	FVegetationTierPlanner Mid({});
	Mid.Update(La + 900.0 * DLat, Lo, 600.0);   // 900 m ground, 1082 m slant
	TestTrue(TEXT("900 m away 600 m up: far"), Mid.GetTier(Home) == TOptional<EVegTier>(EVegTier::Far));
	TestEqual(TEXT("a height change alone replans"), High.Update(La, Lo, 5000.0).Num() > 0, true);
	TestFalse(TEXT("5 km above: beyond the far tier"), High.GetTier(Home).IsSet());
	TestEqual(TEXT("5 km above: nothing planned"), High.GetFarBlocks().Num() + High.GetNearCells().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegHeightAboveGroundTest, "CamSim.Vegetation.Blocks.HeightAboveGround",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegHeightAboveGroundTest::RunTest(const FString& Parameters)
{
	double Ground = 0.0;
	TestEqual(TEXT("nadir hit"), HeightAboveGround(1000.0, 120.0, {}, Ground), 880.0);
	TestEqual(TEXT("remembered"), Ground, 120.0);
	TestEqual(TEXT("no nadir: frame centre"), HeightAboveGround(1000.0, {}, 200.0, Ground), 800.0);
	TestEqual(TEXT("neither: the last ground"), HeightAboveGround(1000.0, {}, {}, Ground), 800.0);
	TestEqual(TEXT("below the ground reads 0"), HeightAboveGround(150.0, 300.0, {}, Ground), 0.0);
	return true;
}
