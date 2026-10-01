// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Tickable.h"
#include "Containers/Set.h"
#include "Config/CamSimConfig.h"
#include "Sim/CommandSink.h"
#include "GroundTruth/AnnotationTypes.h"
#include "Entity/StencilSlotAllocator.h"

class UCamSimSubsystem;
class ACamSimEntity;
class UWorld;
class FEntityTypeTable;
class FCigiHostAdapter;
class FCesiumSurfaceProbe;
struct FThermalStencilEntity;

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

	/** A live entity actor by key, or nullptr. */
	ACamSimEntity* FindEntity(const FEntityKey& Key) const;

	/**
	 * The live stencil-tagged entities with their thermal class (ROADMAP 4A), rebuilt per call. StencilOf holds
	 * exactly the live tagged entities (values are released 4 frames late), so a reused value maps to its current owner.
	 */
	void GetThermalStencilEntities(TArray<FThermalStencilEntity>& Out) const;

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

	// Live entity actors. Each source (CIGI, DIS) has its own IDs.
	TMap<FEntityKey, ACamSimEntity*> EntityMap;

	TUniquePtr<FCigiHostAdapter> CigiAdapter;

	/** Terrain / water traces for surface-clamped entities; created with the first entity. */
	TUniquePtr<FCesiumSurfaceProbe> SurfaceProbe;

	/**
	 * Draw each preloaded glTF model once, far below the world, at startup: the editor
	 * binary compiles material shaders on first use (~100 ms on the game and render
	 * threads), which would otherwise hitch the stream when the first vehicle appears.
	 */
	void WarmUpModels(UWorld* World, double NowSeconds);
	bool   bModelsWarmed = false;
	double WarmUpEndSeconds = 0.0;
	TArray<TWeakObjectPtr<AActor>> WarmUpActors;

	/** Ground-truth annotation IDs, handed out at spawn (never repeated within a session). */
	FAnnotationIdAllocator AnnotationIds;

	/** Custom-depth stencil values for ground-truth masks; released in ForgetEntity (every removal path). */
	FStencilSlotAllocator StencilSlots;
	TMap<FEntityKey, uint8> StencilOf;
	bool bLoggedStencilExhausted = false;

	/** Place attached (child) entities relative to their parents' current poses. */
	void ResolveAttachedEntities();
	static constexpr int32 MaxAttachDepth = 8;
	void ApplyEntityCommand(const FEntityCommand& Command, double NowSeconds, bool bBypassRateLimit);
	float GetEntityMaxUpdateRateHz(const FEntityKey& Key) const;

	// Spawn a new ACamSimEntity with the given initial state
	ACamSimEntity* SpawnEntity(const FEntityCommand& Command);

	// Remove a stale (pending-kill) entry from EntityMap
	void PurgeStaleEntities();
	void ForgetEntity(const FEntityKey& Key);

	// Runtime update throttling to reduce transform churn under large-entity loads.
	TMap<FEntityKey, double> LastPoseApplySeconds;
};
