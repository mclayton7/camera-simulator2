// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Vegetation/VegetationGrid.h"
#include "Vegetation/VegetationScatter.h"

using namespace CamSimVegetation;

namespace
{
	uint8 AllTrees(double, double) { return 10; }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegScatterDeterminismTest, "CamSim.Vegetation.Scatter.OrderIndependent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegScatterDeterminismTest::RunTest(const FString& Parameters)
{
	const FScatterParams P;
	const uint32 Seed = VegetationSeed(1234u);
	const FVegCellId Origin = CellOf(33.302, -117.355, P.CellSizeM);
	TArray<FVegCellId> Cells;
	for (int32 R = -2; R <= 2; ++R) for (int32 C = -2; C <= 2; ++C) Cells.Add({ Origin.Row + R, Origin.Col + C });

	TMap<FVegCellId, TArray<FTreeInstance>> Forward, Reverse, Shuffled;
	for (const FVegCellId& C : Cells) Forward.Add(C, GenerateCell(Seed, C, P, AllTrees));
	for (int32 I = Cells.Num() - 1; I >= 0; --I) Reverse.Add(Cells[I], GenerateCell(Seed, Cells[I], P, AllTrees));
	const int32 Order[] = { 7, 19, 3, 24, 0, 11, 15, 2, 22, 9, 4, 17, 13, 1, 20, 6, 23, 10, 16, 5, 21, 8, 14, 18, 12 };
	for (int32 I : Order) Shuffled.Add(Cells[I], GenerateCell(Seed, Cells[I], P, AllTrees));

	for (const FVegCellId& C : Cells)
	{
		TestTrue(TEXT("forward == reverse"), Forward[C] == Reverse[C]);
		TestTrue(TEXT("forward == shuffled"), Forward[C] == Shuffled[C]);
		TestEqual(TEXT("hash stable"), HashInstances(Forward[C]), HashInstances(Shuffled[C]));
	}
	TestFalse(TEXT("different seed, different trees"), GenerateCell(Seed + 1, Origin, P, AllTrees) == Forward[Origin]);
	TestFalse(TEXT("neighbour cells differ"), Forward[Cells[0]] == Forward[Cells[1]]);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegScatterDensityTest, "CamSim.Vegetation.Scatter.Density",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegScatterDensityTest::RunTest(const FString& Parameters)
{
	for (const float Density : { 0.25f, 0.5f, 1.0f })
	{
		FScatterParams P;
		P.DensityScale = Density;
		const uint32 Seed = VegetationSeed(7u);
		const FVegCellId Origin = CellOf(33.302, -117.355, P.CellSizeM);
		int64 OnTrees = 0, OnGrass = 0;
		double TreeAreaM2 = 0.0;
		for (int32 K = 0; K < 100; ++K)
		{
			const FVegCellId C{ Origin.Row + K / 10, Origin.Col + K % 10 };
			const FVegCellBounds B = CellBounds(C, P.CellSizeM);
			const double MidLon = 0.5 * (B.WestDeg + B.EastDeg);
			// West half tree cover (10), east half grassland (30).
			auto HalfHalf = [MidLon](double, double Lon) -> uint8 { return Lon < MidLon ? 10 : 30; };
			for (const FTreeInstance& T : GenerateCell(Seed, C, P, HalfHalf)) { (T.LonDeg < MidLon ? OnTrees : OnGrass)++; }
			const double LatC = 0.5 * (B.SouthDeg + B.NorthDeg);
			TreeAreaM2 += GroundDistanceM(B.SouthDeg, B.WestDeg, B.NorthDeg, B.WestDeg) * GroundDistanceM(LatC, B.WestDeg, LatC, MidLon);
		}
		const double Expected = ExpectedTreesPerM2(P) * TreeAreaM2;
		TestEqual(*FString::Printf(TEXT("density %.2f: none on grassland"), Density), OnGrass, int64(0));
		TestTrue(*FString::Printf(TEXT("density %.2f: %lld trees vs expected %.0f (+-10%%)"), Density, OnTrees, Expected),
			FMath::Abs(OnTrees - Expected) <= 0.10 * Expected);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegScatterRangesTest, "CamSim.Vegetation.Scatter.InsideCellAndRanges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegScatterRangesTest::RunTest(const FString& Parameters)
{
	const FScatterParams P;
	const FVegCellId C = CellOf(33.302, -117.355, P.CellSizeM);
	const FVegCellBounds B = CellBounds(C, P.CellSizeM);
	const TArray<FTreeInstance> T = GenerateCell(VegetationSeed(0u), C, P, AllTrees);
	TestTrue(TEXT("some trees"), T.Num() > 0);
	TestTrue(TEXT("at most one per candidate"), T.Num() <= CandidatesPerAxis(P) * CandidatesPerAxis(P));
	for (const FTreeInstance& I : T)
	{
		if (!(I.LatDeg > B.SouthDeg && I.LatDeg < B.NorthDeg && I.LonDeg > B.WestDeg && I.LonDeg < B.EastDeg)) { AddError(TEXT("outside its cell")); break; }
		if (!(I.YawDeg >= 0.0f && I.YawDeg < 360.0f)) { AddError(TEXT("yaw range")); break; }
		if (!(I.Scale >= 0.8f && I.Scale <= 1.2f)) { AddError(TEXT("scale range")); break; }
		if (I.Type != 0) { AddError(TEXT("type 0 only")); break; }
	}
	FScatterParams Zero = P; Zero.DensityScale = 0.0f;
	TestEqual(TEXT("density 0 gives nothing"), GenerateCell(VegetationSeed(0u), C, Zero, AllTrees).Num(), 0);
	return true;
}
