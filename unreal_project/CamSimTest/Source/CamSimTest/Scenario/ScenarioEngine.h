// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Config/CamSimConfig.h"
#include "CIGI/CigiPacketTypes.h"

class ACamSimEntity;

/**
 * FScenarioEngine
 *
 * Drives multi-waypoint trajectories, event triggers, and pattern-of-life
 * scheduling for scenario entities. Replaces the linear rate-based model
 * in FCamSimEntityManager::BuildScenarioState().
 *
 * Called once per game tick from FCamSimEntityManager::ProcessScenarioEntities().
 */
class FScenarioEngine
{
public:
	FScenarioEngine() = default;

	void Initialize(const FCamSimConfig& Config);

	/**
	 * Advance scenario clock and produce entity states.
	 * Returns an array of FCigiEntityState for all active scenario entities.
	 */
	TArray<FCigiEntityState> Tick(double ScenarioElapsedSec, float DeltaTimeSec,
	                              float TimeOfDayHours,
	                              const TMap<uint16, ACamSimEntity*>& EntityMap);

	/** Entity IDs that should be removed (from DespawnEntity triggers). */
	TArray<uint16> GetPendingRemovals() const;

	/** Runtime speed override (from ChangeSpeed trigger action). */
	void SetEntitySpeed(uint16 EntityId, float NewSpeedMps);

	/** Reset all state (for hot-reload). */
	void Reset();

	// Exposed for testing
	static float  BearingDeg(double Lat1, double Lon1, double Lat2, double Lon2);
	static double DistanceM(double Lat1, double Lon1, double Lat2, double Lon2);

private:
	struct FWaypointState
	{
		int32  CurrentIdx       = 0;
		double SegmentStartTime = 0.0;
		float  PauseRemaining   = 0.0f;
		bool   bPaused          = false;
		bool   bFinished        = false;
	};

	struct FTriggerState
	{
		bool   bFired          = false;
		double LastFireTimeSec = 0.0;
	};

	TArray<FCamSimConfig::FScenarioEntityConfig> EntityConfigs;
	TArray<FCamSimConfig::FScenarioTrigger>      Triggers;
	float TimeScale = 1.0f;
	float StartHour = 6.0f;
	int32 FrameCount = 0;

	TMap<uint16, FWaypointState> WaypointStates;
	TArray<FTriggerState>        TriggerStates;
	TMap<uint16, float>          SpeedOverrides;
	TArray<uint16>               PendingRemovals;

	// 23D: Formation flying — leader last-known states for hold-on-loss
	TMap<uint16, FCigiEntityState> LastKnownStates;

	// 23A: Waypoint interpolation
	FCigiEntityState InterpolateWaypoints(
	    const FCamSimConfig::FScenarioEntityConfig& Spec,
	    FWaypointState& State, double ScenarioElapsedSec) const;

	// 23A: Linear rate fallback (entities without waypoints)
	FCigiEntityState BuildLinearState(
	    const FCamSimConfig::FScenarioEntityConfig& Spec,
	    double ScenarioElapsedSec) const;

	// 23B: Trigger evaluation
	void EvaluateTriggers(double ScenarioElapsedSec,
	                      const TMap<uint16, ACamSimEntity*>& EntityMap);
	bool CheckCondition(const FCamSimConfig::FScenarioCondition& C,
	                    double ScenarioElapsedSec,
	                    const TMap<uint16, ACamSimEntity*>& EntityMap) const;
	void ExecuteAction(const FCamSimConfig::FScenarioAction& A, double ScenarioElapsedSec);

	// 23C: Activity schedule resolution
	int32 ResolveActiveSchedule(const FCamSimConfig::FScenarioEntityConfig& Spec,
	                             float TimeOfDayHours) const;

	// 23D: Formation flying
	void ApplyFormationOffsets(TArray<FCigiEntityState>& Output);
	void SortByFormationDependency();
	const FCamSimConfig::FScenarioEntityConfig* FindSpecById(uint16 EId) const;
};
