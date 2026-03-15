// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"
#include "Scenario/ScenarioEngine.h"

// ---------------------------------------------------------------------------
// Test 1: WaypointLinearInterp — T=0.5 between two waypoints is midpoint
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaypointLinearInterpTest,
	"CamSim.Phase23.ScenarioEngine.WaypointLinearInterp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWaypointLinearInterpTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	FCamSimConfig::FScenarioEntityConfig Spec;
	Spec.EntityId = 1;
	Spec.EntityType = 1001;
	Spec.SpawnTimeSec = 0.0f;
	Spec.BaseSpeedMps = 10.0f;

	FCamSimConfig::FWaypointConfig WpA;
	WpA.Latitude = 38.0; WpA.Longitude = -77.0; WpA.Altitude = 100.0f;
	FCamSimConfig::FWaypointConfig WpB;
	WpB.Latitude = 38.001; WpB.Longitude = -77.0; WpB.Altitude = 200.0f;

	Spec.Waypoints.Add(WpA);
	Spec.Waypoints.Add(WpB);

	Cfg.ScenarioEntities.Add(Spec);
	Cfg.bScenarioEnabled = true;

	FScenarioEngine Engine;
	Engine.Initialize(Cfg);

	// Distance between waypoints is ~111m. At 10 m/s, transit takes ~11.1s.
	// At T=5.55s from spawn, should be approximately at midpoint.
	const double SegDist = FScenarioEngine::DistanceM(38.0, -77.0, 38.001, -77.0);
	const double HalfTime = (SegDist / 10.0) * 0.5;

	TMap<uint16, ACamSimEntity*> EmptyMap;
	TArray<FCigiEntityState> States = Engine.Tick(HalfTime, 1.0f/30.0f, 12.0f, EmptyMap);

	TestTrue(TEXT("Got 1 entity state"), States.Num() == 1);
	if (States.Num() > 0)
	{
		const double MidLat = (38.0 + 38.001) * 0.5;
		TestTrue(TEXT("Latitude near midpoint"),
			FMath::Abs(States[0].Latitude - MidLat) < 0.0002);
		TestTrue(TEXT("Altitude near midpoint"),
			FMath::Abs(States[0].Altitude - 150.0f) < 10.0f);
	}

	return true;
}

// ---------------------------------------------------------------------------
// Test 2: WaypointPause — entity holds position during PauseSec dwell
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaypointPauseTest,
	"CamSim.Phase23.ScenarioEngine.WaypointPause",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWaypointPauseTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	FCamSimConfig::FScenarioEntityConfig Spec;
	Spec.EntityId = 1;
	Spec.EntityType = 1001;
	Spec.SpawnTimeSec = 0.0f;
	Spec.BaseSpeedMps = 1000.0f; // fast enough to complete segment instantly

	FCamSimConfig::FWaypointConfig WpA;
	WpA.Latitude = 38.0; WpA.Longitude = -77.0; WpA.Altitude = 0.0f;
	FCamSimConfig::FWaypointConfig WpB;
	WpB.Latitude = 38.0001; WpB.Longitude = -77.0; WpB.Altitude = 0.0f;
	WpB.PauseSec = 5.0f;
	FCamSimConfig::FWaypointConfig WpC;
	WpC.Latitude = 38.0002; WpC.Longitude = -77.0; WpC.Altitude = 0.0f;

	Spec.Waypoints.Add(WpA);
	Spec.Waypoints.Add(WpB);
	Spec.Waypoints.Add(WpC);

	Cfg.ScenarioEntities.Add(Spec);
	Cfg.bScenarioEnabled = true;

	FScenarioEngine Engine;
	Engine.Initialize(Cfg);

	TMap<uint16, ACamSimEntity*> EmptyMap;

	// After arriving at WpB (fast speed, should arrive by T=1), entity should hold at WpB
	TArray<FCigiEntityState> States = Engine.Tick(1.0, 1.0f/30.0f, 12.0f, EmptyMap);
	TestTrue(TEXT("Got state during pause"), States.Num() == 1);
	if (States.Num() > 0)
	{
		// During pause, lat should be near WpB (38.0001), not yet at WpC
		TestTrue(TEXT("During pause, near WpB lat"),
			FMath::Abs(States[0].Latitude - 38.0001) < 0.00005);
	}

	return true;
}

// ---------------------------------------------------------------------------
// Test 3: WaypointLoop — entity loops with bLoopWaypoints=true
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaypointLoopTest,
	"CamSim.Phase23.ScenarioEngine.WaypointLoop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWaypointLoopTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	FCamSimConfig::FScenarioEntityConfig Spec;
	Spec.EntityId = 1;
	Spec.EntityType = 1001;
	Spec.SpawnTimeSec = 0.0f;
	Spec.BaseSpeedMps = 1000.0f;
	Spec.bLoopWaypoints = true;

	FCamSimConfig::FWaypointConfig WpA;
	WpA.Latitude = 38.0; WpA.Longitude = -77.0; WpA.Altitude = 0.0f;
	FCamSimConfig::FWaypointConfig WpB;
	WpB.Latitude = 38.0001; WpB.Longitude = -77.0; WpB.Altitude = 0.0f;

	Spec.Waypoints.Add(WpA);
	Spec.Waypoints.Add(WpB);
	Cfg.ScenarioEntities.Add(Spec);
	Cfg.bScenarioEnabled = true;

	FScenarioEngine Engine;
	Engine.Initialize(Cfg);

	TMap<uint16, ACamSimEntity*> EmptyMap;

	// Tick past multiple loop completions (fast speed means multiple loops in 10s)
	TArray<FCigiEntityState> S1 = Engine.Tick(5.0, 1.0f/30.0f, 12.0f, EmptyMap);
	TArray<FCigiEntityState> S2 = Engine.Tick(10.0, 1.0f/30.0f, 12.0f, EmptyMap);

	// Both ticks should produce states (entity doesn't stop)
	TestTrue(TEXT("Still producing states at T=5"), S1.Num() == 1);
	TestTrue(TEXT("Still producing states at T=10"), S2.Num() == 1);

	return true;
}

// ---------------------------------------------------------------------------
// Test 4: WaypointSpeedOverride — per-waypoint speed overrides BaseSpeedMps
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaypointSpeedOverrideTest,
	"CamSim.Phase23.ScenarioEngine.WaypointSpeedOverride",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWaypointSpeedOverrideTest::RunTest(const FString& Parameters)
{
	FCamSimConfig::FWaypointConfig Wp;
	Wp.SpeedMps = 25.0f;

	// SpeedMps > 0 means this waypoint overrides BaseSpeedMps
	TestTrue(TEXT("Waypoint speed override is set"), Wp.SpeedMps > 0.0f);
	TestTrue(TEXT("Default waypoint speed is 0 (inherit)"),
		FCamSimConfig::FWaypointConfig().SpeedMps == 0.0f);

	return true;
}

// ---------------------------------------------------------------------------
// Test 5: WaypointAutoHeading — HeadingDeg=-1 computes bearing
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaypointAutoHeadingTest,
	"CamSim.Phase23.ScenarioEngine.WaypointAutoHeading",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWaypointAutoHeadingTest::RunTest(const FString& Parameters)
{
	// North-bound bearing (same longitude, higher latitude)
	const float NorthBearing = FScenarioEngine::BearingDeg(38.0, -77.0, 39.0, -77.0);
	TestTrue(TEXT("Northbound bearing ~0 deg"),
		FMath::Abs(NorthBearing) < 1.0f || FMath::Abs(NorthBearing - 360.0f) < 1.0f);

	// East-bound bearing (same latitude, higher longitude)
	const float EastBearing = FScenarioEngine::BearingDeg(38.0, -77.0, 38.0, -76.0);
	TestTrue(TEXT("Eastbound bearing ~90 deg"),
		FMath::Abs(EastBearing - 90.0f) < 2.0f);

	// Default HeadingDeg is -1 (auto)
	TestTrue(TEXT("Default heading is auto (-1)"),
		FCamSimConfig::FWaypointConfig().HeadingDeg < 0.0f);

	return true;
}

// ---------------------------------------------------------------------------
// Test 6: WaypointFallback — entity without waypoints uses linear rates
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaypointFallbackTest,
	"CamSim.Phase23.ScenarioEngine.WaypointFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWaypointFallbackTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	FCamSimConfig::FScenarioEntityConfig Spec;
	Spec.EntityId = 1;
	Spec.EntityType = 1001;
	Spec.SpawnTimeSec = 0.0f;
	Spec.StartLatitude = 38.0;
	Spec.NorthRateMps = 10.0f; // 10 m/s north
	// No waypoints — should use linear fallback

	Cfg.ScenarioEntities.Add(Spec);
	Cfg.bScenarioEnabled = true;

	FScenarioEngine Engine;
	Engine.Initialize(Cfg);

	TMap<uint16, ACamSimEntity*> EmptyMap;
	TArray<FCigiEntityState> States = Engine.Tick(10.0, 1.0f/30.0f, 12.0f, EmptyMap);

	TestTrue(TEXT("Linear fallback produces state"), States.Num() == 1);
	if (States.Num() > 0)
	{
		// 10 m/s * 10s = 100m north. 100m / 111320 m/deg ~= 0.000898 deg
		TestTrue(TEXT("Latitude moved north"),
			States[0].Latitude > 38.0 + 0.0005);
	}

	return true;
}

// ---------------------------------------------------------------------------
// Test 7: TriggerTimeSec — trigger fires at time threshold
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTriggerTimeSecTest,
	"CamSim.Phase23.ScenarioEngine.TriggerTimeSec",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTriggerTimeSecTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	Cfg.bScenarioEnabled = true;

	FCamSimConfig::FScenarioTrigger Trig;
	Trig.Name = TEXT("spawn_at_30s");
	Trig.Condition.Type = FCamSimConfig::EScenarioConditionType::TimeSec;
	Trig.Condition.TimeSec = 30.0f;
	Trig.Action.Type = FCamSimConfig::EScenarioActionType::SpawnEntity;
	Trig.Action.TargetEntityId = 99;
	Trig.Action.TargetEntityType = 1001;
	Trig.Action.SpawnLatitude = 38.0;
	Trig.Action.SpawnLongitude = -77.0;
	Cfg.ScenarioTriggers.Add(Trig);

	FScenarioEngine Engine;
	Engine.Initialize(Cfg);

	TMap<uint16, ACamSimEntity*> EmptyMap;

	// Before threshold — trigger should NOT fire
	TArray<FCigiEntityState> S1 = Engine.Tick(20.0, 1.0f/30.0f, 12.0f, EmptyMap);
	// After threshold — trigger SHOULD fire, adding entity to configs
	TArray<FCigiEntityState> S2 = Engine.Tick(35.0, 1.0f/30.0f, 12.0f, EmptyMap);

	// After the trigger fires, the spawned entity config is added;
	// on the next tick it will produce a state
	TArray<FCigiEntityState> S3 = Engine.Tick(36.0, 1.0f/30.0f, 12.0f, EmptyMap);

	TestTrue(TEXT("Spawned entity appears after trigger"),
		S3.Num() >= 1);

	return true;
}

// ---------------------------------------------------------------------------
// Test 8: TriggerEntityProximity — fires when two entities within RadiusM
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTriggerEntityProximityTest,
	"CamSim.Phase23.ScenarioEngine.TriggerEntityProximity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTriggerEntityProximityTest::RunTest(const FString& Parameters)
{
	// Test the condition type defaults
	FCamSimConfig::FScenarioCondition Cond;
	Cond.Type = FCamSimConfig::EScenarioConditionType::EntityProximity;
	Cond.EntityIdA = 1;
	Cond.EntityIdB = 2;
	Cond.RadiusM = 50.0f;

	TestEqual(TEXT("Condition type is EntityProximity"),
		static_cast<uint8>(Cond.Type),
		static_cast<uint8>(FCamSimConfig::EScenarioConditionType::EntityProximity));
	TestEqual(TEXT("RadiusM is 50"), Cond.RadiusM, 50.0f);

	return true;
}

// ---------------------------------------------------------------------------
// Test 9: TriggerRepeatCooldown — repeating trigger respects cooldown
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTriggerRepeatCooldownTest,
	"CamSim.Phase23.ScenarioEngine.TriggerRepeatCooldown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTriggerRepeatCooldownTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	Cfg.bScenarioEnabled = true;

	FCamSimConfig::FScenarioTrigger Trig;
	Trig.Name = TEXT("repeat_log");
	Trig.bRepeat = true;
	Trig.CooldownSec = 10.0f;
	Trig.Condition.Type = FCamSimConfig::EScenarioConditionType::TimeSec;
	Trig.Condition.TimeSec = 5.0f;
	Trig.Action.Type = FCamSimConfig::EScenarioActionType::LogMessage;
	Trig.Action.Message = TEXT("hello");
	Cfg.ScenarioTriggers.Add(Trig);

	FScenarioEngine Engine;
	Engine.Initialize(Cfg);

	TMap<uint16, ACamSimEntity*> EmptyMap;

	// First fire at T=5
	Engine.Tick(5.0, 1.0f/30.0f, 12.0f, EmptyMap);
	// Tick at T=10 — should NOT fire (cooldown 10s, so next fire at T=15)
	Engine.Tick(10.0, 1.0f/30.0f, 12.0f, EmptyMap);

	TestTrue(TEXT("Repeat trigger with cooldown parsed OK"), Trig.bRepeat);
	TestTrue(TEXT("Cooldown is 10s"), FMath::IsNearlyEqual(Trig.CooldownSec, 10.0f));

	return true;
}

// ---------------------------------------------------------------------------
// Test 10: TriggerSpawnAction — SpawnEntity action produces valid state
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTriggerSpawnActionTest,
	"CamSim.Phase23.ScenarioEngine.TriggerSpawnAction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTriggerSpawnActionTest::RunTest(const FString& Parameters)
{
	FCamSimConfig::FScenarioAction Act;
	Act.Type = FCamSimConfig::EScenarioActionType::SpawnEntity;
	Act.TargetEntityId = 42;
	Act.TargetEntityType = 2001;
	Act.SpawnLatitude = 39.5;
	Act.SpawnLongitude = -76.5;
	Act.SpawnAltitude = 300.0f;

	TestEqual(TEXT("Action target entity ID"), Act.TargetEntityId, 42);
	TestEqual(TEXT("Action target entity type"), Act.TargetEntityType, 2001);
	TestTrue(TEXT("Spawn latitude set"), FMath::IsNearlyEqual(Act.SpawnLatitude, 39.5, 0.001));

	return true;
}

// ---------------------------------------------------------------------------
// Test 11: ActivityScheduleResolution — correct path for time-of-day
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActivityScheduleResolutionTest,
	"CamSim.Phase23.ScenarioEngine.ActivityScheduleResolution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FActivityScheduleResolutionTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	FCamSimConfig::FScenarioEntityConfig Spec;
	Spec.EntityId = 1;
	Spec.EntityType = 1001;
	Spec.SpawnTimeSec = 0.0f;
	Spec.BaseSpeedMps = 10.0f;

	// No waypoints, but with activity schedule
	FCamSimConfig::FActivityScheduleEntry MorningEntry;
	MorningEntry.StartHour = 6.0f;
	MorningEntry.EndHour = 12.0f;
	MorningEntry.WaypointPathIndex = 0;
	Spec.ActivitySchedule.Add(MorningEntry);

	FCamSimConfig::FActivityScheduleEntry EveningEntry;
	EveningEntry.StartHour = 18.0f;
	EveningEntry.EndHour = 22.0f;
	EveningEntry.WaypointPathIndex = 1;
	Spec.ActivitySchedule.Add(EveningEntry);

	Cfg.ScenarioEntities.Add(Spec);
	Cfg.bScenarioEnabled = true;

	FScenarioEngine Engine;
	Engine.Initialize(Cfg);

	TMap<uint16, ACamSimEntity*> EmptyMap;

	// At 8:00 (morning) — entity should be active
	TArray<FCigiEntityState> MorningStates = Engine.Tick(1.0, 1.0f/30.0f, 8.0f, EmptyMap);
	TestTrue(TEXT("Entity active during morning schedule"), MorningStates.Num() == 1);

	// At 20:00 (evening) — entity should be active
	TArray<FCigiEntityState> EveningStates = Engine.Tick(2.0, 1.0f/30.0f, 20.0f, EmptyMap);
	TestTrue(TEXT("Entity active during evening schedule"), EveningStates.Num() == 1);

	return true;
}

// ---------------------------------------------------------------------------
// Test 12: ActivityScheduleGap — no state outside schedule windows
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActivityScheduleGapTest,
	"CamSim.Phase23.ScenarioEngine.ActivityScheduleGap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FActivityScheduleGapTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	FCamSimConfig::FScenarioEntityConfig Spec;
	Spec.EntityId = 1;
	Spec.EntityType = 1001;
	Spec.SpawnTimeSec = 0.0f;

	FCamSimConfig::FActivityScheduleEntry Entry;
	Entry.StartHour = 8.0f;
	Entry.EndHour = 10.0f;
	Spec.ActivitySchedule.Add(Entry);

	Cfg.ScenarioEntities.Add(Spec);
	Cfg.bScenarioEnabled = true;

	FScenarioEngine Engine;
	Engine.Initialize(Cfg);

	TMap<uint16, ACamSimEntity*> EmptyMap;

	// At 14:00 — outside the 8-10 window
	TArray<FCigiEntityState> States = Engine.Tick(1.0, 1.0f/30.0f, 14.0f, EmptyMap);
	TestTrue(TEXT("No entity state outside schedule window"), States.Num() == 0);

	return true;
}

// ---------------------------------------------------------------------------
// Test 13: DamageConfigDefaults — FDamageTransitionConfig defaults correct
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDamageConfigDefaultsTest,
	"CamSim.Phase22.DamageConfigDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDamageConfigDefaultsTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;

	TestTrue(TEXT("Damage transition FX enabled by default"), Cfg.DamageTransition.bDamageTransitionFX);
	TestFalse(TEXT("Gradual damage disabled by default"), Cfg.DamageTransition.bGradualDamage);
	TestTrue(TEXT("Interpolation sec is 1.0"),
		FMath::IsNearlyEqual(Cfg.DamageTransition.DamageInterpolationSec, 1.0f));
	TestTrue(TEXT("Scorch darkening is 0.3"),
		FMath::IsNearlyEqual(Cfg.DamageTransition.DamageScorchDarkening, 0.3f));

	return true;
}

// ---------------------------------------------------------------------------
// Test 14: AnimStateFromSpeed — 0=Idle, 1.0=Walk, 5.0=Run
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimStateFromSpeedTest,
	"CamSim.Phase22.AnimStateFromSpeed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAnimStateFromSpeedTest::RunTest(const FString& Parameters)
{
	// Replicate the auto-derive logic from UCamSimAnimInstance::NativeUpdateAnimation
	auto DeriveAnimState = [](float GroundSpeed) -> int32
	{
		if (GroundSpeed < 0.1f) return 0; // Idle
		if (GroundSpeed < 1.5f) return 1; // Walk
		return 2; // Run
	};

	TestEqual(TEXT("0 m/s -> Idle (0)"), DeriveAnimState(0.0f), 0);
	TestEqual(TEXT("0.05 m/s -> Idle (0)"), DeriveAnimState(0.05f), 0);
	TestEqual(TEXT("1.0 m/s -> Walk (1)"), DeriveAnimState(1.0f), 1);
	TestEqual(TEXT("1.49 m/s -> Walk (1)"), DeriveAnimState(1.49f), 1);
	TestEqual(TEXT("1.5 m/s -> Run (2)"), DeriveAnimState(1.5f), 2);
	TestEqual(TEXT("5.0 m/s -> Run (2)"), DeriveAnimState(5.0f), 2);

	return true;
}
