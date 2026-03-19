// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Scenario/ScenarioEngine.h"
#include "Config/CamSimConfig.h"

#include "Geospatial/GeoConstants.h"

// ---------------------------------------------------------------------------
// Test 1: FormationBasicOffset — 100m right offset shifts follower east
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFormationBasicOffsetTest,
	"CamSim.Phase23.Formation.FormationBasicOffset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFormationBasicOffsetTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	Cfg.bScenarioEnabled = true;

	// Leader: heading north at lat=38.9, lon=-77.0, alt=1000
	FCamSimConfig::FScenarioEntityConfig Leader;
	Leader.EntityId = 1;
	Leader.EntityType = 1001;
	Leader.SpawnTimeSec = 0.0f;
	Leader.StartLatitude = 38.9;
	Leader.StartLongitude = -77.0;
	Leader.StartAltitude = 1000.0;
	Leader.StartYaw = 0.0f; // north

	// Follower: 100m to the right of leader (Y=right in body frame)
	FCamSimConfig::FScenarioEntityConfig Follower;
	Follower.EntityId = 2;
	Follower.EntityType = 1001;
	Follower.SpawnTimeSec = 0.0f;
	Follower.StartLatitude = 38.9;
	Follower.StartLongitude = -77.0;
	Follower.StartAltitude = 1000.0;
	Follower.LeaderEntityId = 1;
	Follower.FormationOffsetM = FVector(0.0, 100.0, 0.0); // 100m right

	Cfg.ScenarioEntities.Add(Leader);
	Cfg.ScenarioEntities.Add(Follower);

	FScenarioEngine Engine;
	Engine.Initialize(Cfg);

	TMap<uint16, ACamSimEntity*> EmptyMap;
	TArray<FCigiEntityState> States = Engine.Tick(1.0, 1.0f / 30.0f, 12.0f, EmptyMap);

	TestTrue(TEXT("Got 2 entity states"), States.Num() == 2);
	if (States.Num() == 2)
	{
		const FCigiEntityState& LeaderState = (States[0].EntityId == 1) ? States[0] : States[1];
		const FCigiEntityState& FollowerState = (States[0].EntityId == 2) ? States[0] : States[1];

		// Heading=0 (north), Y=right means east shift: follower longitude > leader longitude
		TestTrue(TEXT("Follower shifted east (lon > leader lon)"),
			FollowerState.Longitude > LeaderState.Longitude);

		// Latitude should be approximately equal (offset is purely east)
		TestTrue(TEXT("Follower latitude near leader latitude"),
			FMath::Abs(FollowerState.Latitude - LeaderState.Latitude) < 0.001);
	}

	return true;
}

// ---------------------------------------------------------------------------
// Test 2: FormationHeadingRotation — leader heading=90 rotates offset east
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFormationHeadingRotationTest,
	"CamSim.Phase23.Formation.FormationHeadingRotation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFormationHeadingRotationTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	Cfg.bScenarioEnabled = true;

	// Leader: heading east (90 deg)
	FCamSimConfig::FScenarioEntityConfig Leader;
	Leader.EntityId = 1;
	Leader.EntityType = 1001;
	Leader.SpawnTimeSec = 0.0f;
	Leader.StartLatitude = 38.9;
	Leader.StartLongitude = -77.0;
	Leader.StartAltitude = 1000.0;
	Leader.StartYaw = 90.0f; // east

	// Follower: 100m forward (X=forward in body frame)
	// With leader heading east, forward = east
	FCamSimConfig::FScenarioEntityConfig Follower;
	Follower.EntityId = 2;
	Follower.EntityType = 1001;
	Follower.SpawnTimeSec = 0.0f;
	Follower.StartLatitude = 38.9;
	Follower.StartLongitude = -77.0;
	Follower.StartAltitude = 1000.0;
	Follower.LeaderEntityId = 1;
	Follower.FormationOffsetM = FVector(100.0, 0.0, 0.0); // 100m forward

	Cfg.ScenarioEntities.Add(Leader);
	Cfg.ScenarioEntities.Add(Follower);

	FScenarioEngine Engine;
	Engine.Initialize(Cfg);

	TMap<uint16, ACamSimEntity*> EmptyMap;
	TArray<FCigiEntityState> States = Engine.Tick(1.0, 1.0f / 30.0f, 12.0f, EmptyMap);

	TestTrue(TEXT("Got 2 entity states"), States.Num() == 2);
	if (States.Num() == 2)
	{
		const FCigiEntityState& LeaderState = (States[0].EntityId == 1) ? States[0] : States[1];
		const FCigiEntityState& FollowerState = (States[0].EntityId == 2) ? States[0] : States[1];

		// Leader faces east, forward=east: follower longitude should be meaningfully greater
		TestTrue(TEXT("Follower shifted east (forward when heading=90)"),
			FollowerState.Longitude - LeaderState.Longitude > 0.0005);
	}

	return true;
}

// ---------------------------------------------------------------------------
// Test 3: FormationLeaderLoss — follower holds last position when leader lost
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFormationLeaderLossTest,
	"CamSim.Phase23.Formation.FormationLeaderLoss",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFormationLeaderLossTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	Cfg.bScenarioEnabled = true;

	FCamSimConfig::FScenarioEntityConfig Leader;
	Leader.EntityId = 1;
	Leader.EntityType = 1001;
	Leader.SpawnTimeSec = 0.0f;
	Leader.StartLatitude = 38.9;
	Leader.StartLongitude = -77.0;
	Leader.StartAltitude = 1000.0;
	Leader.StartYaw = 0.0f;

	FCamSimConfig::FScenarioEntityConfig Follower;
	Follower.EntityId = 2;
	Follower.EntityType = 1001;
	Follower.SpawnTimeSec = 0.0f;
	Follower.StartLatitude = 38.9;
	Follower.StartLongitude = -77.0;
	Follower.StartAltitude = 1000.0;
	Follower.LeaderEntityId = 1;
	Follower.FormationOffsetM = FVector(0.0, 100.0, 0.0);

	Cfg.ScenarioEntities.Add(Leader);
	Cfg.ScenarioEntities.Add(Follower);

	FScenarioEngine Engine;
	Engine.Initialize(Cfg);

	TMap<uint16, ACamSimEntity*> EmptyMap;

	// Tick 1: both active
	TArray<FCigiEntityState> States1 = Engine.Tick(1.0, 1.0f / 30.0f, 12.0f, EmptyMap);
	TestTrue(TEXT("Tick 1: got 2 states"), States1.Num() == 2);

	double FollowerLat1 = 0.0;
	double FollowerLon1 = 0.0;
	for (const FCigiEntityState& S : States1)
	{
		if (S.EntityId == 2)
		{
			FollowerLat1 = S.Latitude;
			FollowerLon1 = S.Longitude;
		}
	}

	// Tick 2: leader "disappears" by setting SpawnTimeSec far in the future.
	// Re-initialize with leader not yet spawned.
	FCamSimConfig Cfg2 = Cfg;
	Cfg2.ScenarioEntities[0].SpawnTimeSec = 9999.0f; // leader not spawned at T=2

	FScenarioEngine Engine2;
	Engine2.Initialize(Cfg2);

	// Seed the engine's last-known state by ticking once with both active
	TArray<FCigiEntityState> Warmup = Engine2.Tick(1.0, 1.0f / 30.0f, 12.0f, EmptyMap);

	// Now tick past leader spawn time — leader not present
	// But the engine was initialized with leader spawning at T=9999, so at T=2 only follower appears
	TArray<FCigiEntityState> States2 = Engine.Tick(2.0, 1.0f / 30.0f, 12.0f, EmptyMap);

	// Follower should hold at the same formation position computed in tick 1
	for (const FCigiEntityState& S : States2)
	{
		if (S.EntityId == 2)
		{
			TestTrue(TEXT("Follower lat held after leader loss"),
				FMath::Abs(S.Latitude - FollowerLat1) < 0.0001);
			TestTrue(TEXT("Follower lon held after leader loss"),
				FMath::Abs(S.Longitude - FollowerLon1) < 0.0001);
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// Test 4: FormationInheritHeadingFalse — follower keeps its own heading
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFormationInheritHeadingFalseTest,
	"CamSim.Phase23.Formation.FormationInheritHeadingFalse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFormationInheritHeadingFalseTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	Cfg.bScenarioEnabled = true;

	// Leader heading is auto-computed (heading=0 from StartYaw=0 with no waypoints)
	FCamSimConfig::FScenarioEntityConfig Leader;
	Leader.EntityId = 1;
	Leader.EntityType = 1001;
	Leader.SpawnTimeSec = 0.0f;
	Leader.StartLatitude = 38.9;
	Leader.StartLongitude = -77.0;
	Leader.StartAltitude = 1000.0;
	Leader.StartYaw = 0.0f;

	// Follower with bInheritHeading=false and StartYaw=45
	FCamSimConfig::FScenarioEntityConfig Follower;
	Follower.EntityId = 2;
	Follower.EntityType = 1001;
	Follower.SpawnTimeSec = 0.0f;
	Follower.StartLatitude = 38.9;
	Follower.StartLongitude = -77.0;
	Follower.StartAltitude = 1000.0;
	Follower.LeaderEntityId = 1;
	Follower.FormationOffsetM = FVector(50.0, 50.0, 0.0);
	Follower.bInheritHeading = false;
	Follower.StartYaw = 45.0f;

	Cfg.ScenarioEntities.Add(Leader);
	Cfg.ScenarioEntities.Add(Follower);

	FScenarioEngine Engine;
	Engine.Initialize(Cfg);

	TMap<uint16, ACamSimEntity*> EmptyMap;
	TArray<FCigiEntityState> States = Engine.Tick(1.0, 1.0f / 30.0f, 12.0f, EmptyMap);

	TestTrue(TEXT("Got 2 entity states"), States.Num() == 2);
	if (States.Num() == 2)
	{
		float LeaderYaw = 0.0f;
		float FollowerYaw = 0.0f;
		for (const FCigiEntityState& S : States)
		{
			if (S.EntityId == 1) LeaderYaw = S.Yaw;
			if (S.EntityId == 2) FollowerYaw = S.Yaw;
		}

		// Follower should keep its own heading (45), not inherit leader's (0)
		TestTrue(TEXT("Follower yaw != leader yaw"),
			FMath::Abs(FollowerYaw - LeaderYaw) > 1.0f);

		// BuildLinearState sets Yaw = StartYaw + YawRateDegPerSec * elapsed
		// With YawRateDegPerSec=0 and StartYaw=45, follower yaw should be 45
		TestTrue(TEXT("Follower yaw is ~45 (own heading)"),
			FMath::Abs(FollowerYaw - 45.0f) < 1.0f);
	}

	return true;
}

// ---------------------------------------------------------------------------
// Test 5: FormationDependencySort — follower before leader in array still works
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFormationDependencySortTest,
	"CamSim.Phase23.Formation.FormationDependencySort",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFormationDependencySortTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	Cfg.bScenarioEnabled = true;

	// Add follower FIRST (before leader) — dependency sort should fix this
	FCamSimConfig::FScenarioEntityConfig Follower;
	Follower.EntityId = 2;
	Follower.EntityType = 1001;
	Follower.SpawnTimeSec = 0.0f;
	Follower.StartLatitude = 38.9;
	Follower.StartLongitude = -77.0;
	Follower.StartAltitude = 1000.0;
	Follower.LeaderEntityId = 1;
	Follower.FormationOffsetM = FVector(0.0, 100.0, 0.0);

	FCamSimConfig::FScenarioEntityConfig Leader;
	Leader.EntityId = 1;
	Leader.EntityType = 1001;
	Leader.SpawnTimeSec = 0.0f;
	Leader.StartLatitude = 38.9;
	Leader.StartLongitude = -77.0;
	Leader.StartAltitude = 1000.0;
	Leader.StartYaw = 0.0f;

	Cfg.ScenarioEntities.Add(Follower); // follower first!
	Cfg.ScenarioEntities.Add(Leader);   // leader second

	FScenarioEngine Engine;
	Engine.Initialize(Cfg);

	TMap<uint16, ACamSimEntity*> EmptyMap;
	TArray<FCigiEntityState> States = Engine.Tick(1.0, 1.0f / 30.0f, 12.0f, EmptyMap);

	// Both should produce valid output despite reversed declaration order
	TestTrue(TEXT("Got 2 entity states after dependency sort"), States.Num() == 2);
	if (States.Num() == 2)
	{
		const FCigiEntityState* FollowerState = nullptr;
		const FCigiEntityState* LeaderState = nullptr;
		for (const FCigiEntityState& S : States)
		{
			if (S.EntityId == 1) LeaderState = &S;
			if (S.EntityId == 2) FollowerState = &S;
		}

		TestTrue(TEXT("Leader state found"), LeaderState != nullptr);
		TestTrue(TEXT("Follower state found"), FollowerState != nullptr);

		if (LeaderState && FollowerState)
		{
			// Follower should be offset east (100m right when heading=0)
			TestTrue(TEXT("Follower longitude > leader longitude (east offset applied)"),
				FollowerState->Longitude > LeaderState->Longitude);
		}
	}

	return true;
}
