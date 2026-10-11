// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Vegetation/VegetationGrid.h"
#include "Vegetation/VegetationManager.h"
#include "Vegetation/VegetationScatter.h"

using namespace CamSimVegetation;

namespace
{
	/** Fake hooks: generation and heights complete when Flush() is called (simulates async), spawns are recorded. */
	struct FFake
	{
		TArray<TFunction<void()>> Pending;
		TMap<FVegCellId, FVegCellSpawn> Live;
		TArray<FVegCellId> SpawnLog;   // every Spawn call, in order
		int32 Spawns = 0, Destroys = 0;
		int32 TreesPerCell = 6;
		bool bHeightsFail = false;
		bool bLoseGenerations = false;   // Done never runs (a task lost at shutdown)
		bool bLoseHeights = false;       // Done never runs (Cesium drops the callback when the tileset actor is destroyed)
		int32 HeightCalls = 0;

		FVegetationHooks Hooks()
		{
			FVegetationHooks H;
			H.Generate = [this](FVegCellId Id, TFunction<void(TArray<FTreeInstance>)> Done)
			{
				if (bLoseGenerations) return;
				Pending.Add([this, Id, Done]()
				{
					TArray<FTreeInstance> T;
					double La, Lo; CellCentre(Id, 100.0, La, Lo);
					for (int32 K = 0; K < TreesPerCell; ++K) T.Add({ La + K * 1e-5, Lo, 10.0f * K, 1.0f, 0 });
					Done(MoveTemp(T));
				});
			};
			H.SampleHeights = [this](const TArray<FVector>& P, TFunction<void(TArray<TOptional<double>>)> Done)
			{
				++HeightCalls;
				if (bLoseHeights) return;
				const int32 N = P.Num();
				Pending.Add([this, N, Done]()
				{
					TArray<TOptional<double>> R;
					for (int32 K = 0; K < N; ++K) R.Add(bHeightsFail ? TOptional<double>() : TOptional<double>(100.0 + K));
					Done(MoveTemp(R));
				});
			};
			H.Spawn = [this](FVegCellId Id, const FVegCellSpawn& S) { ++Spawns; SpawnLog.Add(Id); Live.Add(Id, S); };
			H.Destroy = [this](FVegCellId Id) { ++Destroys; Live.Remove(Id); };
			return H;
		}
		void Flush() { TArray<TFunction<void()>> P = MoveTemp(Pending); for (auto& F : P) F(); }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegManagerPipelineTest, "CamSim.Vegetation.Manager.Pipeline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegManagerPipelineTest::RunTest(const FString& Parameters)
{
	FFake F;
	TSharedRef<FVegetationManager> M = MakeShared<FVegetationManager>(FVegetationStreaming::FParams{}, F.Hooks());
	for (int32 T = 0; T < 600; ++T) { const int32 Before = F.Spawns; M->Tick(33.302, -117.355); TestTrue(TEXT("<= 2 spawns per tick"), F.Spawns - Before <= 2); F.Flush(); }
	const int32 Want = CellsWithin(33.302, -117.355, 1000.0, 100.0).Num();
	TestEqual(TEXT("every wanted cell spawned"), F.Live.Num(), Want);
	const FVegCellSpawn* S = F.Live.Find(CellOf(33.302, -117.355, 100.0));
	TestNotNull(TEXT("camera cell spawned"), S);
	if (S)
	{
		TestEqual(TEXT("instances grounded"), S->LocalInstances.Num(), 6);
		TestEqual(TEXT("base = first height"), S->BaseHeightM, 100.0);
		TestEqual(TEXT("relative height of instance 3 (cm)"), S->LocalInstances[3].GetTranslation().Z, 300.0, 1e-6);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegManagerStaleTest, "CamSim.Vegetation.Manager.StaleResultsIgnored",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegManagerStaleTest::RunTest(const FString& Parameters)
{
	FFake F;
	TSharedRef<FVegetationManager> M = MakeShared<FVegetationManager>(FVegetationStreaming::FParams{}, F.Hooks());
	for (int32 T = 0; T < 600; ++T) { M->Tick(33.302, -117.355); F.Flush(); }
	TestTrue(TEXT("old place populated"), F.Live.Num() > 100);

	// Move 300 m north and tick WITHOUT flushing: new cells start generating and their completions stay pending.
	M->Tick(33.302 + 300.0 / MetresPerDegLat, -117.355);
	const int32 PendingAtOldPlace = F.Pending.Num();
	TestTrue(TEXT("generations are really pending at the old place"), PendingAtOldPlace > 0);
	const int32 SpawnsBefore = F.Spawns;

	// Teleport 600 km away: every actor goes, and the pending generations now belong to dropped cells.
	M->Tick(37.7749, -122.4194);
	TestEqual(TEXT("all old actors destroyed"), F.Live.Num(), 0);
	TestTrue(TEXT("the stale completions are still queued"), F.Pending.Num() >= PendingAtOldPlace);

	// Deliver the stale completions (generation, and any height request a wrong implementation would have made).
	for (int32 K = 0; K < 3; ++K) F.Flush();
	TestEqual(TEXT("a stale result spawned nothing"), F.Spawns, SpawnsBefore);

	// Then let the new place fill in: only cells near the new place may ever spawn.
	for (int32 T = 0; T < 600; ++T) { M->Tick(37.7749, -122.4194); F.Flush(); }
	TestTrue(TEXT("the new place populated"), F.Live.Num() > 100);
	for (int32 K = SpawnsBefore; K < F.SpawnLog.Num(); ++K)
	{
		double La, Lo; CellCentre(F.SpawnLog[K], 100.0, La, Lo);
		if (GroundDistanceM(37.7749, -122.4194, La, Lo) > 1200.0) { AddError(TEXT("a cell spawned at the old place after the teleport")); break; }
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegManagerFailedHeightsTest, "CamSim.Vegetation.Manager.FailedHeightsMakeEmpty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegManagerFailedHeightsTest::RunTest(const FString& Parameters)
{
	FFake F;
	F.bHeightsFail = true;
	TSharedRef<FVegetationManager> M = MakeShared<FVegetationManager>(FVegetationStreaming::FParams{}, F.Hooks());
	for (int32 T = 0; T < 600; ++T) { M->Tick(33.302, -117.355); F.Flush(); }
	TestEqual(TEXT("nothing spawned"), F.Spawns, 0);
	TestEqual(TEXT("no cell stuck in flight"), M->GetStreaming().NumInFlight(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegManagerEmptyTest, "CamSim.Vegetation.Manager.EmptyCellsCostNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegManagerEmptyTest::RunTest(const FString& Parameters)
{
	FFake F;
	F.TreesPerCell = 0;
	int32 HeightCalls = 0;
	FVegetationHooks H = F.Hooks();
	TFunction<void(const TArray<FVector>&, TFunction<void(TArray<TOptional<double>>)>)> Inner = H.SampleHeights;
	H.SampleHeights = [&HeightCalls, Inner](const TArray<FVector>& P, TFunction<void(TArray<TOptional<double>>)> D) { ++HeightCalls; Inner(P, D); };
	TSharedRef<FVegetationManager> M = MakeShared<FVegetationManager>(FVegetationStreaming::FParams{}, H);
	for (int32 T = 0; T < 200; ++T) { M->Tick(33.302, -117.355); F.Flush(); }
	TestEqual(TEXT("no spawns"), F.Spawns, 0);
	TestEqual(TEXT("no height requests for empty cells"), HeightCalls, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegManagerLocalFrameTest, "CamSim.Vegetation.Manager.ToCellLocal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegManagerLocalFrameTest::RunTest(const FString& Parameters)
{
	const double Lat0 = 33.3, Lon0 = -117.4;
	TArray<FTreeInstance> T = { { Lat0 + 10.0 / MetresPerDegLat, Lon0, 90.0f, 1.1f, 0 },   // 10 m north
	                            { Lat0, Lon0 + 20.0 / (MetresPerDegLat * FMath::Cos(FMath::DegreesToRadians(Lat0))), 0.0f, 1.0f, 0 } }; // 20 m east
	const TArray<FTransform> X = ToCellLocal(Lat0, Lon0, 50.0, T, { 52.0, 49.0 });
	TestEqual(TEXT("north -> -Y (ESU)"), X[0].GetTranslation().Y, -1000.0, 5.0);
	TestEqual(TEXT("north: no east"), X[0].GetTranslation().X, 0.0, 5.0);
	TestEqual(TEXT("north: +2 m up"), X[0].GetTranslation().Z, 200.0, 1e-6);
	TestEqual(TEXT("east -> +X"), X[1].GetTranslation().X, 2000.0, 5.0);
	TestEqual(TEXT("east: -1 m up"), X[1].GetTranslation().Z, -100.0, 1e-6);
	TestEqual(TEXT("scale"), X[0].GetScale3D().X, 1.1, 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegManagerLostCallbacksTest, "CamSim.Vegetation.Manager.LostCallbacksTimeOut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegManagerLostCallbacksTest::RunTest(const FString& Parameters)
{
	// One timeout warning per manager (the third manager's generations time out too), never one per cell.
	AddExpectedMessagePlain(TEXT("left empty (logged once)"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 3);
	// Heights never answer (the terrain tileset was destroyed with requests outstanding): the cells must not hold the
	// in-flight budget forever.
	{
		FFake F;
		F.bLoseHeights = true;
		TSharedRef<FVegetationManager> M = MakeShared<FVegetationManager>(FVegetationStreaming::FParams{}, F.Hooks());
		for (int32 T = 0; T < 10; ++T) { M->Tick(33.302, -117.355); F.Flush(); }
		TestEqual(TEXT("budget full of unanswered height requests"), M->GetStreaming().NumInFlight(), 4);
		const int32 CallsBefore = F.HeightCalls;
		for (int32 T = 0; T < FVegetationManager::StuckTicks + 2; ++T) { M->Tick(33.302, -117.355); F.Flush(); }
		TestTrue(TEXT("the budget was released and new cells went to heights"), F.HeightCalls > CallsBefore);
		TestEqual(TEXT("timed-out cells are empty"), M->GetStreaming().GetState(CellOf(33.302, -117.355, 100.0)),
			TOptional<EVegCellState>(EVegCellState::Empty));
		TestEqual(TEXT("nothing spawned"), F.Spawns, 0);
	}
	// Generation never answers.
	{
		FFake F;
		F.bLoseGenerations = true;
		TSharedRef<FVegetationManager> M = MakeShared<FVegetationManager>(FVegetationStreaming::FParams{}, F.Hooks());
		for (int32 T = 0; T < FVegetationManager::StuckTicks + 2; ++T) { M->Tick(33.302, -117.355); F.Flush(); }
		TestEqual(TEXT("first generations timed out to empty"), M->GetStreaming().GetState(CellOf(33.302, -117.355, 100.0)),
			TOptional<EVegCellState>(EVegCellState::Empty));
		TestTrue(TEXT("more cells than the first budget were started"), M->GetStreaming().NumTracked() > 4);
	}
	// A late answer after the timeout is ignored.
	{
		FFake F;
		TSharedRef<FVegetationManager> M = MakeShared<FVegetationManager>(FVegetationStreaming::FParams{}, F.Hooks());
		M->Tick(33.302, -117.355);
		TArray<TFunction<void()>> Late = MoveTemp(F.Pending);   // hold the first four generations back
		F.bLoseGenerations = true;
		for (int32 T = 0; T < FVegetationManager::StuckTicks + 2; ++T) M->Tick(33.302, -117.355);
		F.bLoseGenerations = false;
		for (auto& Fn : Late) Fn();
		for (int32 K = 0; K < 3; ++K) F.Flush();
		TestEqual(TEXT("late generations started no height request"), F.HeightCalls, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegManagerDuplicateTest, "CamSim.Vegetation.Manager.DuplicateResultsAfterReRequest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegManagerDuplicateTest::RunTest(const FString& Parameters)
{
	// Cells start, are dropped while generating, then wanted again: two generation results arrive for each.
	FFake F;
	TSharedRef<FVegetationManager> M = MakeShared<FVegetationManager>(FVegetationStreaming::FParams{}, F.Hooks());
	M->Tick(33.302, -117.355);
	TestEqual(TEXT("four generations pending"), F.Pending.Num(), 4);
	M->Tick(37.7749, -122.4194);   // drop them (and start four at the far place)
	M->Tick(33.302, -117.355);     // drop the far ones, re-request the same four
	TestEqual(TEXT("eight generation results pending for the four cells (+4 far)"), F.Pending.Num(), 12);
	F.Flush();                     // all generations, old and new
	TestEqual(TEXT("one height request per re-requested cell"), F.HeightCalls, 4);
	for (int32 T = 0; T < 4; ++T) { M->Tick(33.302, -117.355); F.Flush(); }
	TSet<FVegCellId> Seen;
	for (const FVegCellId& Id : F.SpawnLog)
	{
		if (Seen.Contains(Id)) { AddError(TEXT("a cell spawned twice")); break; }
		Seen.Add(Id);
	}
	TestTrue(TEXT("the re-requested camera cell spawned"), Seen.Contains(CellOf(33.302, -117.355, 100.0)));
	TestEqual(TEXT("spawn count matches live actors"), F.Spawns, F.Live.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegManagerSeedTest, "CamSim.Vegetation.Manager.SeedFold",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegManagerSeedTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("package seed wins"), ResolveSeed(true, 1234, 99), VegetationSeed(1234u));
	TestEqual(TEXT("config seed without a package seed"), ResolveSeed(false, 1234, 99), VegetationSeed(99u));
	TestEqual(TEXT("negative config seed: two's complement bits"), ResolveSeed(false, 0, -1), VegetationSeed(0xFFFFFFFFu));
	TestEqual(TEXT("int64 package seed folds high into low"), ResolveSeed(true, (int64(5) << 32) | 7, 0), VegetationSeed(7u ^ 5u));
	TestNotEqual(TEXT("high bits matter"), ResolveSeed(true, (int64(1) << 32) | 7, 0), ResolveSeed(true, 7, 0));
	return true;
}
