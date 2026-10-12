// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Vegetation/VegetationGrid.h"
#include "Vegetation/VegetationStreaming.h"

using namespace CamSimVegetation;

namespace
{
	/** Drives a planner with instant generation/heights (every cell gets N trees), keeping Spawned (the live actor set) current. */
	void Drive(FVegetationStreaming& S, double Lat, double Lon, int32 Ticks, TSet<FVegCellId>& Spawned, int32 N = 5)
	{
		for (int32 T = 0; T < Ticks; ++T)
		{
			const FVegetationStreaming::FActions A = S.Tick(Lat, Lon);
			for (const FVegCellId& C : A.ToDrop) Spawned.Remove(C);
			for (const FVegCellId& C : A.ToSpawn) Spawned.Add(C);
			for (const FVegCellId& C : A.ToGenerate) { S.OnGenerated(C, N); S.OnHeightsSampled(C, N); }
		}
	}

	/** Drive() from a fresh planner's point of view: returns only the cells spawned during these ticks. */
	TSet<FVegCellId> RunToSteadyState(FVegetationStreaming& S, double Lat, double Lon, int32 Ticks, int32 N = 5)
	{
		TSet<FVegCellId> Spawned;
		Drive(S, Lat, Lon, Ticks, Spawned, N);
		return Spawned;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegStreamBudgetsTest, "CamSim.Vegetation.Streaming.Budgets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegStreamBudgetsTest::RunTest(const FString& Parameters)
{
	FVegetationStreaming S({});
	const FVegetationStreaming::FActions A = S.Tick(33.302, -117.355);
	TestEqual(TEXT("first tick starts MaxCellsInFlight"), A.ToGenerate.Num(), 4);
	TestTrue(TEXT("nearest first"), A.ToGenerate.Num() > 0 && A.ToGenerate[0] == CellOf(33.302, -117.355, 100.0));
	TestEqual(TEXT("in flight"), S.NumInFlight(), 4);
	TestEqual(TEXT("nothing more while full"), S.Tick(33.302, -117.355).ToGenerate.Num(), 0);
	for (const FVegCellId& C : A.ToGenerate) { S.OnGenerated(C, 10); S.OnHeightsSampled(C, 10); }
	const FVegetationStreaming::FActions B = S.Tick(33.302, -117.355);
	TestEqual(TEXT("at most 2 spawns per tick"), B.ToSpawn.Num(), 2);
	TestEqual(TEXT("slots refilled"), B.ToGenerate.Num(), 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegStreamEmptyTest, "CamSim.Vegetation.Streaming.EmptyCellsFreeSlots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegStreamEmptyTest::RunTest(const FString& Parameters)
{
	FVegetationStreaming S({});
	int32 Ticks = 0, Spawns = 0;
	while (Ticks < 200)
	{
		const FVegetationStreaming::FActions A = S.Tick(33.302, -117.355);
		Spawns += A.ToSpawn.Num();
		for (const FVegCellId& C : A.ToGenerate) S.OnGenerated(C, 0);
		++Ticks;
		if (A.ToGenerate.Num() == 0 && S.NumInFlight() == 0) break;
	}
	TestEqual(TEXT("no spawns for empty cells"), Spawns, 0);
	TestTrue(*FString::Printf(TEXT("all ~314 cells resolved in %d ticks"), Ticks), Ticks < 100);
	TestEqual(TEXT("all tracked as empty"), S.NumTracked(), CellsWithin(33.302, -117.355, 1000.0, 100.0).Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegStreamPathIndependentTest, "CamSim.Vegetation.Streaming.PathIndependent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegStreamPathIndependentTest::RunTest(const FString& Parameters)
{
	// Arrive at the same point from the north and from the south: the same cells end up spawned.
	// A and B track every live actor over the whole path: cells spawned on the approach stay Spawned and are not re-reported.
	FVegetationStreaming North({}), South({});
	TSet<FVegCellId> A, B;
	for (int32 K = 0; K <= 20; ++K) Drive(North, 33.302 + 0.02 * (20 - K) / 20.0, -117.355, 5, A);
	for (int32 K = 0; K <= 20; ++K) Drive(South, 33.302 - 0.02 * (20 - K) / 20.0, -117.355, 5, B);
	Drive(North, 33.302, -117.355, 400, A);
	Drive(South, 33.302, -117.355, 400, B);
	TSet<FVegCellId> Want(CellsWithin(33.302, -117.355, 1000.0, 100.0));
	TestTrue(TEXT("north path covers the wanted set"), Want.Difference(A).Num() == 0);
	TestTrue(TEXT("south path covers the wanted set"), Want.Difference(B).Num() == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegStreamHysteresisTest, "CamSim.Vegetation.Streaming.Hysteresis",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegStreamHysteresisTest::RunTest(const FString& Parameters)
{
	FVegetationStreaming S({});
	RunToSteadyState(S, 33.302, -117.355, 400);
	const int32 Before = S.NumTracked();
	// Move 150 m north: cells 1000-1200 m behind stay (hysteresis); none is dropped.
	const FVegetationStreaming::FActions A = S.Tick(33.302 + 150.0 / MetresPerDegLat, -117.355);
	TestEqual(TEXT("nothing dropped inside 1.2 R"), A.ToDrop.Num(), 0);
	TestTrue(TEXT("still tracked"), S.NumTracked() >= Before);
	// Move 400 m north: the southern edge (now > 1200 m away) drops.
	const FVegetationStreaming::FActions B = S.Tick(33.302 + 400.0 / MetresPerDegLat, -117.355);
	TestTrue(TEXT("far cells dropped"), B.ToDrop.Num() > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegStreamTeleportTest, "CamSim.Vegetation.Streaming.TeleportDropsAll",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegStreamTeleportTest::RunTest(const FString& Parameters)
{
	FVegetationStreaming S({});
	TSet<FVegCellId> Spawned = RunToSteadyState(S, 33.302, -117.355, 400);
	TestTrue(TEXT("steady state spawned something"), Spawned.Num() > 100);
	TestEqual(TEXT("steady state has nothing in flight"), S.NumInFlight(), 0);

	// Leave work genuinely in flight at the old place: move 300 m north (new cells enter the ring), tick once, and do NOT deliver
	// their results. One of them gets its generation result (so it sits in Sampling), the others stay Generating.
	const double NorthLat = 33.302 + 300.0 / MetresPerDegLat;
	const FVegetationStreaming::FActions Move = S.Tick(NorthLat, -117.355);
	for (const FVegCellId& C : Move.ToDrop) Spawned.Remove(C);
	for (const FVegCellId& C : Move.ToSpawn) Spawned.Add(C);
	TestTrue(TEXT("new cells started and not flushed"), Move.ToGenerate.Num() > 0 && S.NumInFlight() == Move.ToGenerate.Num());
	if (Move.ToGenerate.Num() == 0) return false;
	S.OnGenerated(Move.ToGenerate[0], 5);
	TestEqual(TEXT("one cell is Sampling"), static_cast<int32>(*S.GetState(Move.ToGenerate[0])), static_cast<int32>(EVegCellState::Sampling));
	const TArray<FVegCellId> InFlightOld = Move.ToGenerate;

	// Teleport 600 km away.
	const FVegetationStreaming::FActions A = S.Tick(37.7749, -122.4194);
	TestEqual(TEXT("every spawned cell dropped in one tick"), A.ToDrop.Num(), Spawned.Num());
	for (const FVegCellId& C : InFlightOld) TestFalse(TEXT("in-flight old cell is untracked after the teleport"), S.GetState(C).IsSet());
	for (const FVegCellId& C : Spawned) TestFalse(TEXT("spawned old cell is untracked after the teleport"), S.GetState(C).IsSet());

	// Results of the dropped work arrive late: they must change nothing.
	for (const FVegCellId& C : InFlightOld) { S.OnGenerated(C, 5); S.OnHeightsSampled(C, 5); }
	int32 OldSpawned = 0, OldTracked = 0;
	TSet<FVegCellId> Old(InFlightOld);
	Old.Append(Spawned);
	for (int32 T = 0; T < 5; ++T)
	{
		const FVegetationStreaming::FActions B = S.Tick(37.7749, -122.4194);
		for (const FVegCellId& C : B.ToSpawn) OldSpawned += Old.Contains(C);
		for (const FVegCellId& C : B.ToGenerate) OldSpawned += Old.Contains(C);
	}
	for (const FVegCellId& C : Old) OldTracked += S.GetState(C).IsSet();
	TestEqual(TEXT("no stale result spawns or restarts an old cell"), OldSpawned, 0);
	TestEqual(TEXT("no old cell tracked"), OldTracked, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegStreamingTickWantedTest, "CamSim.Vegetation.Streaming.TickWanted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegStreamingTickWantedTest::RunTest(const FString& Parameters)
{
	using namespace CamSimVegetation;
	FVegetationStreaming S({});
	TArray<FVegCellId> Wanted;
	for (int32 K = 0; K < 6; ++K) Wanted.Add({ 100, K });
	auto Always = [](FVegCellId) { return true; };
	auto Never = [](FVegCellId) { return false; };

	FVegetationStreaming::FActions A = S.TickWanted(Wanted, Always, 4, 2);
	TestEqual(TEXT("in-flight cap 4"), A.ToGenerate.Num(), 4);
	TestTrue(TEXT("in Wanted's order"), A.ToGenerate.Num() == 4 && A.ToGenerate[0] == Wanted[0] && A.ToGenerate[3] == Wanted[3]);
	for (const FVegCellId& C : A.ToGenerate) { S.OnGenerated(C, 5); S.OnHeightsSampled(C, 5); }
	A = S.TickWanted(Wanted, Always, 4, 2);
	TestEqual(TEXT("spawn cap 2"), A.ToSpawn.Num(), 2);
	TestTrue(TEXT("spawns in Wanted's order"), A.ToSpawn.Num() == 2 && A.ToSpawn[0] == Wanted[0] && A.ToSpawn[1] == Wanted[1]);
	TestEqual(TEXT("remaining two start"), A.ToGenerate.Num(), 2);
	TestTrue(TEXT("spawned settled"), S.IsSettled(Wanted[0]));
	TestFalse(TEXT("ready not settled"), S.IsSettled(Wanted[2]));
	TestFalse(TEXT("generating not settled"), S.IsSettled(Wanted[4]));
	TestFalse(TEXT("untracked not settled"), S.IsSettled({ 1, 1 }));
	S.OnGenerated(Wanted[5], 0);
	TestTrue(TEXT("empty settled"), S.IsSettled(Wanted[5]));

	const TArray<FVegCellId> Fewer = { Wanted[2], Wanted[3], Wanted[4], Wanted[5] };
	A = S.TickWanted(Fewer, Never, 4, 2);
	TestEqual(TEXT("MayDrop false: nothing dropped"), A.ToDrop.Num(), 0);
	TestTrue(TEXT("still tracked"), S.GetState(Wanted[0]).IsSet());
	TestFalse(TEXT("a kept, unwanted cell is never re-spawned"), A.ToSpawn.Contains(Wanted[0]) || A.ToSpawn.Contains(Wanted[1]));
	A = S.TickWanted(Fewer, Always, 4, 2);
	TestEqual(TEXT("both spawned cells dropped"), A.ToDrop.Num(), 2);
	TestFalse(TEXT("untracked after drop"), S.GetState(Wanted[0]).IsSet());
	TestEqual(TEXT("a zero cap starts nothing"), S.TickWanted({ FVegCellId{ 7, 7 } }, Always, 0, 0).ToGenerate.Num(), 0);
	return true;
}
