// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Vegetation/VegetationGrid.h"
#include "Vegetation/VegetationLibrary.h"
#include "Vegetation/VegetationMix.h"
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

namespace
{
	// R4's cell hashes (captured 2026-10-11 from the R4 slice's scatter, before scatter v2): REALISM R4 bit-identity.
	constexpr uint32 GoldenSeed = 0x5EEDu;
	constexpr uint32 GoldenHash[3] = { 1394846886u, 504439746u, 3376489160u };
	void GoldenCells(FVegCellId (&Out)[3])
	{
		const FVegCellId C0 = CellOf(33.307, -117.354, 100.0);
		Out[0] = C0; Out[1] = { C0.Row + 1, C0.Col }; Out[2] = { C0.Row, C0.Col + 7 };
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegScatterR4IdentityTest, "CamSim.Vegetation.Scatter.R4Identity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegScatterR4IdentityTest::RunTest(const FString& Parameters)
{
	FScatterParams P;
	FVegCellId Cells[3]; GoldenCells(Cells);
	for (int32 K = 0; K < 3; ++K)
		TestEqual(*FString::Printf(TEXT("cell %d legacy == golden"), K), HashInstances(GenerateCell(VegetationSeed(GoldenSeed), Cells[K], P, AllTrees)), GoldenHash[K]);
	// Scatter v2 with the built-in default mix (tree_broadleaf_a at index 0), no canopy, shrubs off: R4's trees.
	FVegetationLibrary L;
	FVegetationType T; T.Name = TEXT("tree_broadleaf_a"); T.Role = EVegRole::Tree; T.HeightM = 9.0f; T.CrownRadiusM = 2.7f;
	L.Types.Add(T);
	const FVegetationMix Mix = DefaultMix(L);
	FScatterParams NoShrubs = P; NoShrubs.bShrubs = false;
	auto NoCanopy = [](double, double) { FVegPoint V; V.Code = 10; return V; };
	for (int32 K = 0; K < 3; ++K)
		TestEqual(*FString::Printf(TEXT("cell %d v2 == golden"), K),
			HashInstances(GenerateCellContent(VegetationSeed(GoldenSeed), Cells[K], NoShrubs, Mix, NoCanopy).Trees), GoldenHash[K]);
	return true;
}

namespace
{
	FVegPoint TreesNoCanopy(double, double) { FVegPoint V; V.Code = 10; return V; }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegScatterSpeciesTest, "CamSim.Vegetation.Scatter.SpeciesFollowMix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegScatterSpeciesTest::RunTest(const FString& Parameters)
{
	FScatterParams P; P.bShrubs = false;
	FVegetationMix Mix = SingleTypeMix(10, 0, 9.0f, 2.7f);
	Mix.Codes[10].Trees = { { 0, 0.25f }, { 1, 1.0f } };
	Mix.TypeHeightM = { 9.0f, 16.0f }; Mix.TypeCrownRadiusM = { 2.7f, 2.5f };
	int64 N[2] = { 0, 0 };
	const FVegCellId O = CellOf(30.5, -86.5, P.CellSizeM);
	for (int32 K = 0; K < 400; ++K)
		for (const FTreeInstance& T : GenerateCellContent(VegetationSeed(3u), { O.Row + K / 20, O.Col + K % 20 }, P, Mix, TreesNoCanopy).Trees) ++N[T.Type];
	const double Total = double(N[0] + N[1]);
	const double Chi2 = FMath::Square(N[0] - 0.25 * Total) / (0.25 * Total) + FMath::Square(N[1] - 0.75 * Total) / (0.75 * Total);
	TestTrue(*FString::Printf(TEXT("chi2 %.2f < 10.83 (1 dof, p 0.001), n=%.0f"), Chi2, Total), Chi2 < 10.83);
	TestTrue(TEXT("enough trees to test"), Total > 10000.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegScatterCanopyTest, "CamSim.Vegetation.Scatter.CanopyCoverMatches",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegScatterCanopyTest::RunTest(const FString& Parameters)
{
	FScatterParams P; P.bShrubs = false;
	const FVegetationMix Mix = SingleTypeMix(10, 0, 9.0f, 2.7f);
	// Below saturation only: one crown per 34.6 m^2 candidate caps crown cover at pi r^2 / 34.6 (0.66 at r 2.7 m).
	for (const float Cover : { 0.1f, 0.3f, 0.5f })
	{
		auto Sample = [Cover](double, double) { FVegPoint V; V.Code = 10; V.bHasCanopy = true; V.CoverFrac = Cover; V.HeightM = 9.0f; return V; };
		double CrownM2 = 0.0, AreaM2 = 0.0;
		const FVegCellId O = CellOf(38.3, -76.4, P.CellSizeM);
		for (int32 K = 0; K < 10000; ++K)
		{
			const FVegCellId C{ O.Row + K / 100, O.Col + K % 100 };
			const FVegCellBounds B = CellBounds(C, P.CellSizeM);
			const double LatC = 0.5 * (B.SouthDeg + B.NorthDeg);
			AreaM2 += GroundDistanceM(B.SouthDeg, B.WestDeg, B.NorthDeg, B.WestDeg) * GroundDistanceM(LatC, B.WestDeg, LatC, B.EastDeg);
			for (const FTreeInstance& T : GenerateCellContent(VegetationSeed(9u), C, P, Mix, Sample).Trees)
				CrownM2 += PI * FMath::Square(2.7 * T.Scale);
		}
		TestTrue(*FString::Printf(TEXT("cover %.1f: crown fraction %.3f within 0.02"), Cover, CrownM2 / AreaM2), FMath::Abs(CrownM2 / AreaM2 - Cover) <= 0.02);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegScatterCanopyEdgesTest, "CamSim.Vegetation.Scatter.CanopyEdgeCases",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegScatterCanopyEdgesTest::RunTest(const FString& Parameters)
{
	FScatterParams P; float S = 0.0f;
	TestEqual(TEXT("below 3 m: none"), CanopyAccept(P, 0.8f, 2.9f, 9.0f, 2.7f, S), 0.0f);
	TestEqual(TEXT("no cover: none"), CanopyAccept(P, 0.0f, 12.0f, 9.0f, 2.7f, S), 0.0f);
	TestEqual(TEXT("full cover at the type's height: 34.6 / 22.9 m^2 clamps to 1"), CanopyAccept(P, 1.0f, 9.0f, 9.0f, 2.7f, S), 1.0f);
	TestEqual(TEXT("cover above 1 clamps too"), CanopyAccept(P, 1.5f, 9.0f, 9.0f, 2.7f, S), 1.0f);
	const float A255 = CanopyAccept(P, 1.0f, 255.0f, 9.0f, 2.7f, S);
	TestEqual(TEXT("scale clamp high"), S, 2.0f);
	TestTrue(TEXT("255 m: the crown at scale 2 lowers p (34.6 / 91.6)"), FMath::Abs(A255 - 0.3777f) < 0.001f);
	CanopyAccept(P, 0.5f, 3.0f, 16.0f, 2.5f, S);
	TestEqual(TEXT("scale clamp low"), S, 0.5f);
	const FVegCellId C = CellOf(30.5, -86.5, 100.0);
	auto Tall = [](double, double) { FVegPoint V; V.Code = 10; V.bHasCanopy = true; V.CoverFrac = 1.0f; V.HeightM = 255.0f; return V; };
	const TArray<FTreeInstance> TallTrees = GenerateCellContent(VegetationSeed(1u), C, P, SingleTypeMix(10, 0, 9.0f, 2.7f), Tall).Trees;
	TestTrue(TEXT("tall canopy: trees"), TallTrees.Num() > 0);
	for (const FTreeInstance& T : TallTrees)
		TestTrue(TEXT("scale in [2*0.9, 2*1.1]"), T.Scale >= 1.8f - 1e-5f && T.Scale <= 2.2f + 1e-5f);
	auto Short = [](double, double) { FVegPoint V; V.Code = 10; V.bHasCanopy = true; V.CoverFrac = 0.9f; V.HeightM = 1.0f; return V; };
	TestEqual(TEXT("short canopy: no trees"), GenerateCellContent(VegetationSeed(1u), C, P, SingleTypeMix(10, 0, 9.0f, 2.7f), Short).Trees.Num(), 0);
	auto Bare = [](double, double) { FVegPoint V; V.Code = 10; V.bHasCanopy = true; V.CoverFrac = 0.0f; V.HeightM = 12.0f; return V; };
	TestEqual(TEXT("no cover: no trees"), GenerateCellContent(VegetationSeed(1u), C, P, SingleTypeMix(10, 0, 9.0f, 2.7f), Bare).Trees.Num(), 0);
	auto OtherCode = [](double, double) { FVegPoint V; V.Code = 50; V.bHasCanopy = true; V.CoverFrac = 0.5f; V.HeightM = 12.0f; return V; };
	TestEqual(TEXT("canopy on a code without trees in the mix: none"), GenerateCellContent(VegetationSeed(1u), C, P, SingleTypeMix(10, 0, 9.0f, 2.7f), OtherCode).Trees.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegScatterShrubsTest, "CamSim.Vegetation.Scatter.Shrubs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegScatterShrubsTest::RunTest(const FString& Parameters)
{
	FScatterParams P;
	FVegetationMix Mix;
	Mix.TypeHeightM = { 9.0f, 1.8f }; Mix.TypeCrownRadiusM = { 2.7f, 0.9f };
	FCodeMix Scrub; Scrub.Shrubs = { { 1, 1.0f } }; Scrub.ShrubsPer100M2 = 8.0f;
	Mix.Codes.Add(20, Scrub);
	auto HalfHalf = [](double, double Lon) { FVegPoint V; V.Code = FMath::Frac(Lon * 1000.0) < 0.5 ? 20 : 30; return V; };
	int64 On20 = 0, On30 = 0, Trees = 0;
	const FVegCellId O = CellOf(32.7, -114.6, P.CellSizeM);
	for (int32 K = 0; K < 100; ++K)
	{
		const FCellContent C = GenerateCellContent(VegetationSeed(5u), { O.Row + K / 10, O.Col + K % 10 }, P, Mix, HalfHalf);
		Trees += C.Trees.Num();
		for (const FTreeInstance& S : C.Shrubs)
		{
			(FMath::Frac(S.LonDeg * 1000.0) < 0.5 ? On20 : On30)++;
			if (!(S.Scale >= 0.7f && S.Scale <= 1.3f)) AddError(TEXT("shrub scale out of range"));
			if (S.Type != 1) AddError(TEXT("shrub type"));
		}
	}
	TestEqual(TEXT("no trees (no tree list)"), Trees, int64(0));
	TestEqual(TEXT("none on 30"), On30, int64(0));
	const double Expected = 8.0 / 100.0 * 0.5 * 100 * 100.0 * 100.0;   // per m^2 x half of 100 cells of ~1 ha
	TestTrue(*FString::Printf(TEXT("%lld shrubs vs ~%.0f (+-10%%)"), On20, Expected), FMath::Abs(On20 - Expected) <= 0.10 * Expected);
	FScatterParams Off = P; Off.bShrubs = false;
	TestEqual(TEXT("bShrubs false"), GenerateCellContent(VegetationSeed(5u), O, Off, Mix, HalfHalf).Shrubs.Num(), 0);
	auto NeverFn = [](double, double) { return false; };
	FClearPredicate Never = NeverFn;
	TestEqual(TEXT("mask drops shrubs"), GenerateCellContent(VegetationSeed(5u), O, P, Mix, HalfHalf, &Never).Shrubs.Num(), 0);
	const FCellContent A = GenerateCellContent(VegetationSeed(5u), O, P, Mix, HalfHalf);
	const FCellContent B = GenerateCellContent(VegetationSeed(5u), O, P, Mix, HalfHalf);
	TestTrue(TEXT("shrubs deterministic"), A.Shrubs == B.Shrubs);
	return true;
}
