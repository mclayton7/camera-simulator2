// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Tickable.h"
#include "Containers/Set.h"
#include "Config/CamSimConfig.h"
#include "Sim/CommandSink.h"
#include "GroundTruth/AnnotationTypes.h"
#include "Ocean/IOceanSurface.h"

class UCamSimSubsystem;
class ACamSimEntity;
class UWorld;
class FEntityTypeTable;
class FScenarioEngine;
class FCigiHostAdapter;

/**
 * FCamSimEntityManager
 *
 * Plain C++ FTickableGameObject that owns the lifecycle of all non-camera
 * CIGI entities.  Owned by UCamSimSubsystem as a raw pointer.
 *
 * Each game tick it:
 *   1. Drains FCigiReceiver::EntityStateQueue — spawns/moves/destroys entities.
 *   2. Drains FCigiReceiver::RateCtrlQueue   — forwards rates to entities.
 *   3. Drains FCigiReceiver::ArtPartQueue    — forwards art-part controls.
 *   4. Drains FCigiReceiver::CompCtrlQueue   — forwards component controls.
 *
 * Entity states are keyed by EntityId; last packet per frame wins.
 * CCL enum: Standby=0, Active=1, Remove=2.
 */
class FCamSimEntityManager : public FTickableGameObject, public ISimCommandSink
{
public:
	explicit FCamSimEntityManager(UCamSimSubsystem* InSubsystem,
	                               const FEntityTypeTable* InTypeTable);
	virtual ~FCamSimEntityManager() override;

	// FTickableGameObject interface
	virtual void   Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool   IsTickable() const override { return true; }
	virtual bool   IsTickableInEditor() const override { return false; }

	/**
	 * Capture a snapshot of all active entity annotations for the current game tick.
	 * Called on the game thread before CaptureAndEncode().
	 *
	 * Out-param form: fills the caller's array in place (Reset()-reused so the
	 * allocation amortises). Cheap cone cull runs before the per-entity AABB
	 * projection so a 500-entity scene with a narrow camera only pays for the
	 * handful of entities plausibly inside the frustum.
	 */
	void GetEntitySnapshot(const FViewProjectionData& ViewProj,
	                       TArray<FEntityAnnotationData>& OutSnapshot) const;

	/** Backwards-compatible convenience form — copies into a fresh TArray. */
	TArray<FEntityAnnotationData> GetEntitySnapshot(
	    const FViewProjectionData& ViewProj) const
	{
		TArray<FEntityAnnotationData> Out;
		GetEntitySnapshot(ViewProj, Out);
		return Out;
	}

	/** Called from FOceanManager::Init() after ocean surface is created. */
	void SetOceanSurface(IOceanSurface* Ocean);

	/** A live entity actor by key, or nullptr. */
	ACamSimEntity* FindEntity(const FEntityKey& Key) const;

	/** Current number of live entity actors (for /metrics camsim_entity_count). */
	int32 GetEntityCount() const { return EntityMap.Num(); }

	// ISimCommandSink — applied immediately
	virtual void Submit(const FEntityCommand& Command) override;
	virtual void Submit(const FEntityMotionCommand& Command) override;
	virtual void Submit(const FArticulationCommand& Command) override;
	virtual void Submit(const FComponentCommand& Command) override;

private:
	UCamSimSubsystem*       Subsystem  = nullptr;
	const FEntityTypeTable* TypeTable  = nullptr;

	// Live entity actors. Each source (CIGI, DIS, scenario) has its own IDs.
	TMap<FEntityKey, ACamSimEntity*> EntityMap;

	TUniquePtr<FCigiHostAdapter> CigiAdapter;

	/** Place attached (child) entities relative to their parents' current poses. */
	void ResolveAttachedEntities();
	static constexpr int32 MaxAttachDepth = 8;
	void ProcessScenarioEntities();
	void ApplyEntityCommand(const FEntityCommand& Command, double NowSeconds, bool bBypassRateLimit);
	float GetEntityMaxUpdateRateHz(const FEntityKey& Key) const;

	// Spawn a new ACamSimEntity with the given initial state
	ACamSimEntity* SpawnEntity(const FEntityCommand& Command);

	// Remove a stale (pending-kill) entry from EntityMap
	void PurgeStaleEntities();
	void ForgetEntity(const FEntityKey& Key);

	IOceanSurface* OceanSurface = nullptr;

	// Runtime update throttling to reduce transform churn under large-entity loads.
	TMap<FEntityKey, double> LastPoseApplySeconds;
	TMap<uint16, double> LastScenarioUpdateSeconds;   // scenario entity IDs
	uint64 ScenarioStartMicros = 0;   // sim time the scenario started
	uint64 LastScenarioMicros  = 0;   // sim time of the previous scenario tick
	double ScenarioLongitude   = 0.0; // for local solar time of day
	TSet<uint16> ScenarioRemovedEntities;

	// Phase 23: Scenario engine (waypoints, triggers, pattern-of-life)
	TUniquePtr<FScenarioEngine> ScenarioEngine;
};
