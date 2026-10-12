// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Vegetation/VegetationBlocks.h"
#include "Vegetation/VegetationFar.h"
#include "Vegetation/VegetationMix.h"
#include "Vegetation/VegetationScatter.h"

using namespace CamSimVegetation;

namespace
{
	TArray<FTreeInstance> SomeTrees(FVegCellId C)
	{
		FScatterParams P; P.bShrubs = false;
		FVegetationMix Mix = SingleTypeMix(10, 0, 9.0f, 2.7f);
		Mix.Codes[10].Trees = { { 0, 0.5f }, { 1, 1.0f } };
		Mix.TypeHeightM = { 9.0f, 16.0f }; Mix.TypeCrownRadiusM = { 2.7f, 2.5f };
		auto All = [](double, double) { FVegPoint V; V.Code = 10; return V; };
		return GenerateCellContent(VegetationSeed(11u), C, P, Mix, All).Trees;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegFarSubsetTest, "CamSim.Vegetation.Far.Subset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegFarSubsetTest::RunTest(const FString& Parameters)
{
	FFarParams P; P.TypeHasFar = { true, true };
	const FVegCellId O = CellOf(30.5, -86.5, 100.0);
	int64 In = 0, Kept = 0;
	int64 Buckets[4] = { 0, 0, 0, 0 };
	for (int32 K = 0; K < 400; ++K)
	{
		const FVegCellId C{ O.Row + K / 20, O.Col + K % 20 };
		const TArray<FTreeInstance> Trees = SomeTrees(C);
		const TArray<FFarTree> Far = FarFilter(VegetationSeed(11u), C, Trees, P);
		In += Trees.Num(); Kept += Far.Num();
		for (const FFarTree& F : Far)
		{
			const FTreeInstance* Src = Trees.FindByPredicate([&](const FTreeInstance& T) { return T.LatDeg == F.Tree.LatDeg && T.LonDeg == F.Tree.LonDeg; });
			if (!Src) { AddError(TEXT("far tree not in the near set")); continue; }
			if (Src->Type != F.Tree.Type || Src->YawDeg != F.Tree.YawDeg) AddError(TEXT("type/yaw differ"));
			if (F.Tree.Scale != Src->Scale * 1.3f) AddError(TEXT("scale x far_scale"));
			if (F.Bucket >= 4) AddError(TEXT("bucket range"));
			else ++Buckets[F.Bucket];
		}
		if (K == 0) TestTrue(TEXT("deterministic"), FarFilter(VegetationSeed(11u), C, Trees, P) == Far);
	}
	TestTrue(*FString::Printf(TEXT("keep %.3f ~ 0.35 (+-0.02)"), double(Kept) / In), FMath::Abs(double(Kept) / In - 0.35) <= 0.02);
	for (int32 B = 0; B < 4; ++B)
		TestTrue(*FString::Printf(TEXT("bucket %d %.3f ~ 0.25"), B, double(Buckets[B]) / Kept), FMath::Abs(double(Buckets[B]) / Kept - 0.25) <= 0.02);
	FFarParams NoPines = P; NoPines.TypeHasFar = { true, false };
	for (const FFarTree& F : FarFilter(VegetationSeed(11u), O, SomeTrees(O), NoPines))
		if (F.Tree.Type == 1) AddError(TEXT("a type without a far mesh is never drawn far"));
	FFarParams All = P; All.Keep = 1.0f;
	TestEqual(TEXT("keep 1: every tree"), FarFilter(VegetationSeed(11u), O, SomeTrees(O), All).Num(), SomeTrees(O).Num());
	TestTrue(TEXT("hash covers bucket"), HashFarTrees(FarFilter(VegetationSeed(11u), O, SomeTrees(O), P)) != HashFarTrees(FarFilter(VegetationSeed(12u), O, SomeTrees(O), P)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegFarGridTest, "CamSim.Vegetation.Far.HeightGrid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegFarGridTest::RunTest(const FString& Parameters)
{
	const FVegBlockId B = BlockOf(CellOf(33.3, -117.4, 100.0), 4, 100.0);
	FFarHeightGrid G = FFarHeightGrid::ForBlock(B, 4, 100.0, 25.0);
	TestTrue(*FString::Printf(TEXT("~25 m spacing over the block's cells (~500 m): N %d"), G.N), G.N >= 17 && G.N <= 23);
	const TArray<FVector> Pts = G.LonLatPoints();
	TestEqual(TEXT("N*N points"), Pts.Num(), G.N * G.N);
	G.H.SetNum(Pts.Num());
	for (int32 K = 0; K < Pts.Num(); ++K) G.H[K] = 100.0 + 2000.0 * (Pts[K].Y - 33.0) + 500.0 * (Pts[K].X + 117.0);   // a plane
	double MaxErr = 0.0;
	for (const FVegCellId& C : CellsOfBlock(B, 4, 100.0))
	{
		double La, Lo; CellCentre(C, 100.0, La, Lo);
		const TOptional<double> H = G.Sample(La, Lo);
		if (!TestTrue(TEXT("inside: set"), H.IsSet())) continue;
		MaxErr = FMath::Max(MaxErr, FMath::Abs(*H - (100.0 + 2000.0 * (La - 33.0) + 500.0 * (Lo + 117.0))));
	}
	TestTrue(*FString::Printf(TEXT("plane reproduced (%.6f m)"), MaxErr), MaxErr < 1e-3);
	TestFalse(TEXT("outside: unset"), G.Sample(G.SouthDeg - 0.01, G.WestDeg).IsSet());
	G.H[0].Reset();
	TestFalse(TEXT("failed corner: unset"), G.Sample(G.SouthDeg + 0.1 * G.DLatDeg, G.WestDeg + 0.1 * G.DLonDeg).IsSet());
	TestTrue(TEXT("a quad without the failed corner: set"), G.Sample(G.SouthDeg + 1.5 * G.DLatDeg, G.WestDeg + 1.5 * G.DLonDeg).IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegFarBucketCullTest, "CamSim.Vegetation.Far.BucketCull",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegFarBucketCullTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("b0"), BucketCullM(4000.0f, 0, 4), 2800.0f);
	TestEqual(TEXT("b1"), BucketCullM(4000.0f, 1, 4), 3200.0f);
	TestEqual(TEXT("b2"), BucketCullM(4000.0f, 2, 4), 3600.0f);
	TestEqual(TEXT("b3"), BucketCullM(4000.0f, 3, 4), 4000.0f);
	TestEqual(TEXT("one bucket"), BucketCullM(4000.0f, 0, 1), 4000.0f);
	return true;
}
