// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Tickable.h"
#include "CIGI/CigiPacketTypes.h"

class UCamSimSubsystem;
class ACamSimEntity;
class UWorld;
class FEntityTypeTable;

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
class FCamSimEntityManager : public FTickableGameObject
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

private:
	UCamSimSubsystem*       Subsystem  = nullptr;
	const FEntityTypeTable* TypeTable  = nullptr;

	// Live entity actors, keyed by CIGI EntityId
	TMap<uint16, ACamSimEntity*> EntityMap;

	// Drain helpers — called from Tick()
	void ProcessEntityStates();
	void ProcessRateControls();
	void ProcessArtPartControls();
	void ProcessComponentControls();

	// Spawn a new ACamSimEntity with the given initial state
	ACamSimEntity* SpawnEntity(const FCigiEntityState& S);

	// Remove a stale (pending-kill) entry from EntityMap
	void PurgeStaleEntities();
};
