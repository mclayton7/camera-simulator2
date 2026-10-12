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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegScatterNoMaskIsR4Test, "CamSim.Vegetation.Scatter.NoMaskIsBitIdenticalToR4",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegScatterNoMaskIsR4Test::RunTest(const FString& Parameters)
{
	// Pinned from the pre-R3b GenerateCell (R4 slice), captured before the overload was added: the 25 cells around
	// Pendleton, seed 0, with a sampler that is code 10 on ~60 % of the ground.
	static const uint32 R4Hashes[25] = { 0x3B0CCCB9u, 0x218F41F0u, 0x44B1197Cu, 0x7893A39Au, 0x93F2DC51u, 0xE94F4D3Au, 0x9D3C76DBu, 0xA51C0CDFu, 0x833D41B6u, 0xD2BCFCC4u, 0xD561B735u, 0x16E3966Au, 0x983837ABu, 0xE986C955u, 0x74A96E5Cu, 0xB4414F44u, 0xDBB23958u, 0x8AF9E5D6u, 0x0ADD048Au, 0x3486C49Eu, 0x65375980u, 0x0EDA8898u, 0x8F4509E0u, 0xE9D03CADu, 0x524C4F9Eu };
	const FScatterParams P;
	const uint32 Seed = VegetationSeed(0u);
	const FVegCellId Origin = CellOf(33.302, -117.355, P.CellSizeM);
	auto Mixed = [](double Lat, double Lon) -> uint8 { return FMath::Frac(Lat * 3000.0 + Lon * 2000.0) < 0.6 ? 10 : 30; };
	auto Always = [](double, double) { return true; };
	for (int32 K = 0; K < 25; ++K)
	{
		const FVegCellId C{ Origin.Row + K / 5, Origin.Col + K % 5 };
		const TArray<FTreeInstance> NoPredicate = GenerateCell(Seed, C, P, Mixed);
		const TArray<FTreeInstance> AlwaysClear = GenerateCell(Seed, C, P, Mixed, Always);
		TestEqual(*FString::Printf(TEXT("cell %d: 4-argument hash == R4"), K), HashInstances(NoPredicate), R4Hashes[K]);
		TestEqual(*FString::Printf(TEXT("cell %d: always-clear hash == R4"), K), HashInstances(AlwaysClear), R4Hashes[K]);
		TestTrue(TEXT("always-clear == 4-argument"), AlwaysClear == NoPredicate);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegScatterPredicateTest, "CamSim.Vegetation.Scatter.ClearPredicateDropsOnlyRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegScatterPredicateTest::RunTest(const FString& Parameters)
{
	const FScatterParams P;
	const uint32 Seed = VegetationSeed(42u);
	const FVegCellId C = CellOf(33.302, -117.355, P.CellSizeM);
	const FVegCellBounds B = CellBounds(C, P.CellSizeM);
	const double Mid = 0.5 * (B.WestDeg + B.EastDeg);
	const TArray<FTreeInstance> All = GenerateCell(Seed, C, P, AllTrees);
	const TArray<FTreeInstance> East = GenerateCell(Seed, C, P, AllTrees, [Mid](double, double Lon) { return Lon >= Mid; });
	TArray<FTreeInstance> Want;
	for (const FTreeInstance& T : All) if (T.LonDeg >= Mid) Want.Add(T);
	TestTrue(TEXT("some trees on each side"), East.Num() > 0 && East.Num() < All.Num());
	TestTrue(TEXT("exactly the unfiltered east trees, same bits and order"), East == Want);
	TestEqual(TEXT("reject all"), GenerateCell(Seed, C, P, AllTrees, [](double, double) { return false; }).Num(), 0);
	return true;
}
