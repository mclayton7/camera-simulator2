// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Vegetation/VegetationBlocks.h"
#include "Vegetation/VegetationFarTier.h"
#include "Vegetation/VegetationManager.h"

using namespace CamSimVegetation;

namespace
{
	constexpr int32 BlockN = 2;
	constexpr double CellM = 100.0;

	/** Near and far fakes: generation and heights complete on Flush (async), spawns recorded per id. */
	struct FTwoTierFake
	{
		TArray<TFunction<void()>> Pending;
		TMap<FVegCellId, FVegCellSpawn> NearLive;
		TMap<FVegBlockId, FVegFarSpawn> FarLive;
		int32 NearSpawns = 0, NearDestroys = 0, FarSpawns = 0, FarDestroys = 0, FarGenerates = 0, FarHeightCalls = 0, LastGridPoints = 0;
		int32 FarTreesPerBlock = 3;
		TSet<FVegBlockId> EmptyFarBlocks;
		int32 FailGridPoint = -1;
		TArray<FString> Events;

		FVegetationHooks NearHooks()
		{
			FVegetationHooks H;
			H.Generate = [this](FVegCellId Id, TFunction<void(FCellContent)> Done)
			{
				Pending.Add([Id, Done]()
				{
					FCellContent C;
					double La, Lo; CellCentre(Id, CellM, La, Lo);
					for (int32 K = 0; K < 4; ++K) C.Trees.Add({ La + K * 1e-5, Lo, 10.0f * K, 1.0f, 0 });
					Done(MoveTemp(C));
				});
			};
			H.SampleHeights = [this](const TArray<FVector>& P, TFunction<void(TArray<TOptional<double>>)> Done)
			{
				const int32 N = P.Num();
				Pending.Add([N, Done]() { TArray<TOptional<double>> R; for (int32 K = 0; K < N; ++K) R.Add(100.0); Done(MoveTemp(R)); });
			};
			H.Spawn = [this](FVegCellId Id, const FVegCellSpawn& S) { ++NearSpawns; NearLive.Add(Id, S); };
			H.Destroy = [this](FVegCellId Id) { ++NearDestroys; NearLive.Remove(Id); };
			H.OnEvent = [this](const FString& Line) { Events.Add(Line); };
			return H;
		}

		FVegetationFarHooks FarHooks()
		{
			FVegetationFarHooks H;
			H.Generate = [this](FVegBlockId B, TFunction<void(TArray<FFarTree>)> Done)
			{
				++FarGenerates;
				const bool bEmpty = EmptyFarBlocks.Contains(B);
				Pending.Add([this, B, bEmpty, Done]()
				{
					TArray<FFarTree> T;
					double La, Lo; BlockCentre(B, BlockN, CellM, La, Lo);
					for (int32 K = 0; K < (bEmpty ? 0 : FarTreesPerBlock); ++K) { FFarTree F; F.Tree = { La + K * 1e-5, Lo, 0.0f, 1.3f, 0 }; F.Bucket = uint8(K % 4); T.Add(F); }
					Done(MoveTemp(T));
				});
			};
			H.SampleHeights = [this](const TArray<FVector>& P, TFunction<void(TArray<TOptional<double>>)> Done)
			{
				++FarHeightCalls;
				LastGridPoints = P.Num();
				const int32 N = P.Num(), Fail = FailGridPoint;
				Pending.Add([N, Fail, Done]()
				{
					TArray<TOptional<double>> R;
					for (int32 K = 0; K < N; ++K) R.Add(K == Fail ? TOptional<double>() : TOptional<double>(100.0));
					Done(MoveTemp(R));
				});
			};
			H.Spawn = [this](FVegBlockId B, const FVegFarSpawn& S) { ++FarSpawns; FarLive.Add(B, S); };
			H.Destroy = [this](FVegBlockId B) { ++FarDestroys; FarLive.Remove(B); };
			return H;
		}

		/** The shrub ring: shrub-only cells (2 shrubs each), their own actors. */
		TMap<FVegCellId, FVegCellSpawn> ShrubLive;
		int32 ShrubSpawns = 0;
		FVegetationHooks ShrubHooks()
		{
			FVegetationHooks H = NearHooks();
			H.Generate = [this](FVegCellId Id, TFunction<void(FCellContent)> Done)
			{
				Pending.Add([Id, Done]()
				{
					FCellContent C;
					double La, Lo; CellCentre(Id, CellM, La, Lo);
					for (int32 K = 0; K < 2; ++K) C.Shrubs.Add({ La - K * 1e-5, Lo, 5.0f * K, 1.0f, 1 });
					Done(MoveTemp(C));
				});
			};
			H.Spawn = [this](FVegCellId Id, const FVegCellSpawn& S) { ++ShrubSpawns; ShrubLive.Add(Id, S); };
			H.Destroy = [this](FVegCellId Id) { ShrubLive.Remove(Id); };
			H.OnEvent = nullptr;
			return H;
		}

		void Flush() { TArray<TFunction<void()>> P = MoveTemp(Pending); for (auto& F : P) F(); }

		bool HasActor(FVegBlockId B) const
		{
			if (FarLive.Contains(B)) return true;
			for (const auto& Kv : NearLive) if (BlockOf(Kv.Key, BlockN, CellM) == B) return true;
			return false;
		}
	};

	FVegetationTierPlanner::FParams Tiers(double FarM = 1500.0)
	{
		FVegetationTierPlanner::FParams T;
		T.NearM = 300.0; T.FarM = FarM; T.CellSizeM = CellM; T.BlockCells = BlockN;
		return T;
	}

	constexpr double DLat = 1.0 / MetresPerDegLat;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegFarTierTiersTest, "CamSim.Vegetation.FarTier.SpawnsBeyondNear",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegFarTierTiersTest::RunTest(const FString& Parameters)
{
	FTwoTierFake F;
	TSharedRef<FVegetationManager> M = MakeShared<FVegetationManager>(Tiers(), F.NearHooks(), F.FarHooks());
	if (!TestNotNull(TEXT("far tier exists"), M->GetFarTier())) return true;
	for (int32 I = 0; I < 400; ++I) { M->Tick(33.3, -117.4); F.Flush(); }
	TestTrue(TEXT("near cells spawned"), F.NearLive.Num() > 0);
	TestTrue(TEXT("far blocks spawned"), F.FarLive.Num() > 0);
	for (const auto& Kv : F.FarLive)
	{
		TestTrue(TEXT("far block is tiered far"), M->GetPlanner().GetTier(Kv.Key) == TOptional<EVegTier>(EVegTier::Far));
		TestFalse(TEXT("no near cell in a far block"), F.NearLive.Contains(CellsOfBlock(Kv.Key, BlockN, CellM)[0]));
	}
	for (const auto& Kv : F.NearLive)
		TestTrue(TEXT("near cell's block is near"), M->GetPlanner().GetTier(BlockOf(Kv.Key, BlockN, CellM)) == TOptional<EVegTier>(EVegTier::Near));
	TestEqual(TEXT("one grid batch per far block"), F.FarHeightCalls, F.FarSpawns);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegFarTierNoGapTest, "CamSim.Vegetation.FarTier.NoGapOnSwitch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegFarTierNoGapTest::RunTest(const FString& Parameters)
{
	FTwoTierFake F;
	TSharedRef<FVegetationManager> M = MakeShared<FVegetationManager>(Tiers(), F.NearHooks(), F.FarHooks());
	const FVegBlockId B = BlockOf(CellOf(33.3, -117.4, CellM), BlockN, CellM);
	double La, Lo; BlockCentre(B, BlockN, CellM, La, Lo);
	bool bSeen = false; int32 Gaps = 0;
	auto Step = [&](double Lat)
	{
		M->Tick(Lat, Lo); F.Flush();
		const bool bHas = F.HasActor(B);
		bSeen |= bHas;
		Gaps += bSeen && !bHas;
	};
	for (int32 I = 0; I < 60; ++I) Step(La);                                    // near, settled
	for (int32 D = 0; D <= 1000; D += 20) for (int32 I = 0; I < 3; ++I) Step(La + D * DLat);   // out to 1 km: near -> far
	TestTrue(TEXT("far at 1 km"), F.FarLive.Contains(B));
	for (int32 I = 0; I < 60; ++I) Step(La + 1000.0 * DLat);
	for (int32 D = 1000; D >= 0; D -= 20) for (int32 I = 0; I < 3; ++I) Step(La + D * DLat);  // back: far -> near
	for (int32 I = 0; I < 60; ++I) Step(La);
	TestTrue(TEXT("block seen"), bSeen);
	TestEqual(TEXT("never without actors once seen"), Gaps, 0);
	TestFalse(TEXT("far actor gone once near again"), F.FarLive.Contains(B));
	TestTrue(TEXT("events logged"), F.Events.ContainsByPredicate([](const FString& E) { return E.Contains(TEXT("\"ev\":\"tier\"")); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegFarTierEmptyTest, "CamSim.Vegetation.FarTier.EmptyTierDoesNotBlock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegFarTierEmptyTest::RunTest(const FString& Parameters)
{
	FTwoTierFake F;
	const FVegBlockId B = BlockOf(CellOf(33.3, -117.4, CellM), BlockN, CellM);
	F.EmptyFarBlocks.Add(B);
	TSharedRef<FVegetationManager> M = MakeShared<FVegetationManager>(Tiers(), F.NearHooks(), F.FarHooks());
	double La, Lo; BlockCentre(B, BlockN, CellM, La, Lo);
	for (int32 I = 0; I < 60; ++I) { M->Tick(La, Lo); F.Flush(); }
	TestTrue(TEXT("near spawned"), F.HasActor(B));
	for (int32 I = 0; I < 120; ++I) { M->Tick(La + 800.0 * DLat, Lo); F.Flush(); }
	TestFalse(TEXT("near cells dropped although the far block is empty"), F.HasActor(B));
	TestTrue(TEXT("far settled empty"), M->GetFarTier()->IsSettled(B));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegFarTierTeleportTest, "CamSim.Vegetation.FarTier.TeleportClears",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegFarTierTeleportTest::RunTest(const FString& Parameters)
{
	FTwoTierFake F;
	TSharedRef<FVegetationManager> M = MakeShared<FVegetationManager>(Tiers(), F.NearHooks(), F.FarHooks());
	for (int32 I = 0; I < 400; ++I) { M->Tick(33.3, -117.4); F.Flush(); }
	TSet<FVegCellId> OldNear; for (const auto& Kv : F.NearLive) OldNear.Add(Kv.Key);
	TSet<FVegBlockId> OldFar; for (const auto& Kv : F.FarLive) OldFar.Add(Kv.Key);
	TestTrue(TEXT("settled with both tiers"), OldNear.Num() > 0 && OldFar.Num() > 0);
	for (int32 I = 0; I < 3; ++I) { M->Tick(33.3 + 50000.0 * DLat, -117.4); F.Flush(); }
	int32 Left = 0;
	for (const FVegCellId& C : OldNear) Left += F.NearLive.Contains(C);
	for (const FVegBlockId& B : OldFar) Left += F.FarLive.Contains(B);
	TestEqual(TEXT("everything of the old place destroyed within 3 ticks"), Left, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegFarTierOffTest, "CamSim.Vegetation.FarTier.OffEqualsNear",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegFarTierOffTest::RunTest(const FString& Parameters)
{
	FTwoTierFake F;
	TSharedRef<FVegetationManager> M = MakeShared<FVegetationManager>(Tiers(0.0), F.NearHooks(), F.FarHooks());
	for (int32 I = 0; I < 100; ++I) { M->Tick(33.3, -117.4); F.Flush(); }
	TestNull(TEXT("no far tier"), M->GetFarTier());
	TestEqual(TEXT("far never generated"), F.FarGenerates, 0);
	TestTrue(TEXT("near spawned"), F.NearLive.Num() > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegFarTierBudgetTest, "CamSim.Vegetation.FarTier.BudgetsShared",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegFarTierBudgetTest::RunTest(const FString& Parameters)
{
	FTwoTierFake F;
	TSharedRef<FVegetationManager> M = MakeShared<FVegetationManager>(Tiers(), F.NearHooks(), F.FarHooks());
	int32 MaxInFlight = 0, MaxSpawns = 0;
	for (int32 I = 0; I < 300; ++I)
	{
		const int32 Before = F.NearSpawns + F.FarSpawns;
		M->Tick(33.3, -117.4);
		MaxSpawns = FMath::Max(MaxSpawns, F.NearSpawns + F.FarSpawns - Before);
		MaxInFlight = FMath::Max(MaxInFlight, M->GetStreaming().NumInFlight() + M->GetFarTier()->NumInFlight());
		F.Flush();
	}
	TestTrue(*FString::Printf(TEXT("at most 4 in flight (%d)"), MaxInFlight), MaxInFlight <= 4 && MaxInFlight > 0);
	TestTrue(*FString::Printf(TEXT("at most 2 spawns per tick (%d)"), MaxSpawns), MaxSpawns <= 2 && MaxSpawns > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegFarTierGridTest, "CamSim.Vegetation.FarTier.GridHeights",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegFarTierGridTest::RunTest(const FString& Parameters)
{
	FTwoTierFake F;
	F.FarTreesPerBlock = 3;
	TSharedRef<FVegetationManager> M = MakeShared<FVegetationManager>(Tiers(), F.NearHooks(), F.FarHooks());
	for (int32 I = 0; I < 300; ++I) { M->Tick(33.3, -117.4); F.Flush(); }
	if (!TestTrue(TEXT("far spawned"), F.FarLive.Num() > 0)) return true;
	const auto& Any = *F.FarLive.CreateConstIterator();
	const FFarHeightGrid G = FFarHeightGrid::ForBlock(Any.Key, BlockN, CellM, FVegetationFarTier::GridSpacingM);
	TestEqual(TEXT("grid points per far block"), F.LastGridPoints, G.N * G.N);
	TestTrue(*FString::Printf(TEXT("a coarse grid: %d points for a 200 m block"), F.LastGridPoints), F.LastGridPoints <= 49);
	for (const double H : Any.Value.HeightsM) TestEqual(TEXT("height = grid - 0.3 m"), H, 100.0 - FVegetationFarTier::SinkM, 1e-9);
	TestEqual(TEXT("local transforms"), Any.Value.Local.Num(), Any.Value.Trees.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegShrubRingRadiusTest, "CamSim.Vegetation.ShrubRing.OnlyWithinRadius",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegShrubRingRadiusTest::RunTest(const FString& Parameters)
{
	FTwoTierFake F;
	FVegetationShrubRing Ring; Ring.RadiusM = 150.0; Ring.Hooks = F.ShrubHooks();
	TSharedRef<FVegetationManager> M = MakeShared<FVegetationManager>(Tiers(), F.NearHooks(), F.FarHooks(), MoveTemp(Ring));
	if (!TestNotNull(TEXT("ring exists"), M->GetShrubRing())) return true;
	for (int32 I = 0; I < 400; ++I) { M->Tick(33.3, -117.4); F.Flush(); }
	TestTrue(TEXT("idle"), M->IsIdle());
	TestTrue(TEXT("shrub cells spawned"), F.ShrubLive.Num() > 0);
	const double ReachM = 150.0 + 0.5 * CellM * UE_SQRT_2 + 1.0;
	int32 Beyond = 0;
	for (const auto& Kv : F.ShrubLive)
	{
		double La, Lo; CellCentre(Kv.Key, CellM, La, Lo);
		Beyond += GroundDistanceM(33.3, -117.4, La, Lo) > ReachM;
		TestEqual(TEXT("shrub cell: no trees"), Kv.Value.Instances.Num(), 0);
		TestEqual(TEXT("shrub cell: its shrubs"), Kv.Value.Shrubs.Num(), 2);
	}
	TestEqual(TEXT("no shrub cell beyond the ring"), Beyond, 0);
	TestTrue(*FString::Printf(TEXT("tree cells reach further (%d) than shrub cells (%d)"), F.NearLive.Num(), F.ShrubLive.Num()),
		F.NearLive.Num() > F.ShrubLive.Num());
	for (const FVegCellId& C : CellsWithin(33.3, -117.4, 150.0, CellM))
		TestTrue(TEXT("every cell within the radius has shrubs"), F.ShrubLive.Contains(C));
	for (int32 I = 0; I < 3; ++I) { M->Tick(33.3 + 50000.0 * DLat, -117.4); F.Flush(); }
	int32 Left = 0;
	for (const auto& Kv : F.ShrubLive) { double La, Lo; CellCentre(Kv.Key, CellM, La, Lo); Left += GroundDistanceM(33.3, -117.4, La, Lo) < 1000.0; }
	TestEqual(TEXT("teleport drops the old ring"), Left, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegShrubRingBudgetTest, "CamSim.Vegetation.ShrubRing.BudgetsShared",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegShrubRingBudgetTest::RunTest(const FString& Parameters)
{
	FTwoTierFake F;
	FVegetationShrubRing Ring; Ring.RadiusM = 250.0; Ring.Hooks = F.ShrubHooks();
	TSharedRef<FVegetationManager> M = MakeShared<FVegetationManager>(Tiers(), F.NearHooks(), F.FarHooks(), MoveTemp(Ring));
	if (!TestNotNull(TEXT("ring exists"), M->GetShrubRing())) return true;
	int32 MaxInFlight = 0, MaxSpawns = 0;
	bool bIdleWhileRingBusy = false;
	for (int32 I = 0; I < 300; ++I)
	{
		const int32 Before = F.NearSpawns + F.FarSpawns + F.ShrubSpawns;
		M->Tick(33.3, -117.4);
		MaxSpawns = FMath::Max(MaxSpawns, F.NearSpawns + F.FarSpawns + F.ShrubSpawns - Before);
		const int32 RingInFlight = M->GetShrubRing()->GetStreaming().NumInFlight();
		MaxInFlight = FMath::Max(MaxInFlight, M->GetStreaming().NumInFlight() + M->GetFarTier()->NumInFlight() + RingInFlight);
		bIdleWhileRingBusy |= M->IsIdle() && RingInFlight > 0;
		F.Flush();
	}
	TestTrue(TEXT("the ring spawned"), F.ShrubSpawns > 0);
	TestTrue(*FString::Printf(TEXT("at most 4 in flight across near, ring and far (%d)"), MaxInFlight), MaxInFlight <= 4 && MaxInFlight > 0);
	TestTrue(*FString::Printf(TEXT("at most 2 spawns per tick across near, ring and far (%d)"), MaxSpawns), MaxSpawns <= 2 && MaxSpawns > 0);
	TestFalse(TEXT("not idle while the ring is busy"), bIdleWhileRingBusy);
	return true;
}
