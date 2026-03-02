// Copyright CamSim Contributors. All Rights Reserved.

#include "Entity/CamSimEntityManager.h"
#include "Entity/CamSimEntity.h"
#include "Entity/EntityTypeTable.h"
#include "Subsystem/CamSimSubsystem.h"
#include "CIGI/CigiReceiver.h"
#include "CamSimTest.h"

#include "Engine/World.h"

// CCL EntityState enum values (from CigiBaseEntityCtrl.h)
static constexpr uint8 CIGI_ENTITY_STANDBY = 0;
static constexpr uint8 CIGI_ENTITY_ACTIVE  = 1;
static constexpr uint8 CIGI_ENTITY_REMOVE  = 2;

// -------------------------------------------------------------------------
// Constructor / Destructor
// -------------------------------------------------------------------------

FCamSimEntityManager::FCamSimEntityManager(UCamSimSubsystem* InSubsystem,
                                           const FEntityTypeTable* InTypeTable)
	: Subsystem(InSubsystem)
	, TypeTable(InTypeTable)
{
}

FCamSimEntityManager::~FCamSimEntityManager()
{
	// Destroy any remaining entity actors
	for (auto& Pair : EntityMap)
	{
		if (IsValid(Pair.Value))
		{
			Pair.Value->Destroy();
		}
	}
	EntityMap.Empty();
}

// -------------------------------------------------------------------------
// FTickableGameObject
// -------------------------------------------------------------------------

TStatId FCamSimEntityManager::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(FCamSimEntityManager, STATGROUP_Tickables);
}

void FCamSimEntityManager::Tick(float DeltaTime)
{
	PurgeStaleEntities();
	ProcessEntityStates();
	ProcessRateControls();
	ProcessArtPartControls();
	ProcessComponentControls();

	// Drive CIGI query handler and sender flush (SOF + HAT/HOT + LOS responses)
	if (Subsystem)
	{
		Subsystem->Tick(DeltaTime);
	}
}

// -------------------------------------------------------------------------
// Drain helpers
// -------------------------------------------------------------------------

void FCamSimEntityManager::ProcessEntityStates()
{
	FCigiReceiver* Receiver = Subsystem ? Subsystem->GetCigiReceiver() : nullptr;
	if (!Receiver) return;

	// Collect last state per entity ID this frame (last write wins)
	TMap<uint16, FCigiEntityState> FrameStates;
	FCigiEntityState State;
	while (Receiver->DequeueEntityState(State))
	{
		FrameStates.Add(State.EntityId, State);
	}

	for (const auto& Pair : FrameStates)
	{
		const FCigiEntityState& S = Pair.Value;

		if (S.EntityState == CIGI_ENTITY_ACTIVE)
		{
			// Spawn if new, otherwise update pose
			ACamSimEntity** ExistingPtr = EntityMap.Find(S.EntityId);
			ACamSimEntity*  Entity = ExistingPtr ? *ExistingPtr : nullptr;

			if (!IsValid(Entity))
			{
				Entity = SpawnEntity(S);
				if (!Entity)
				{
					UE_LOG(LogCamSim, Warning, TEXT("EntityManager: failed to spawn entity %u"), S.EntityId);
					continue;
				}
				EntityMap.Add(S.EntityId, Entity);
				UE_LOG(LogCamSim, Log, TEXT("EntityManager: spawned entity %u (type %u)"),
					S.EntityId, S.EntityType);
			}
			else
			{
				// Update mesh if type has changed
				if (Entity->EntityType != S.EntityType)
				{
					UE_LOG(LogCamSim, Log, TEXT("EntityManager: entity %u type change %u -> %u"),
						S.EntityId, Entity->EntityType, S.EntityType);
					Entity->SetEntityType(S.EntityType);
				}
				Entity->ApplyPose(S);
			}
			Entity->SetActorHiddenInGame(false);
		}
		else if (S.EntityState == CIGI_ENTITY_STANDBY)
		{
			// Hide but don't destroy
			ACamSimEntity** EntityPtr = EntityMap.Find(S.EntityId);
			if (EntityPtr && IsValid(*EntityPtr))
			{
				(*EntityPtr)->SetActorHiddenInGame(true);
				UE_LOG(LogCamSim, Log, TEXT("EntityManager: entity %u standby (hidden)"), S.EntityId);
			}
		}
		else if (S.EntityState == CIGI_ENTITY_REMOVE)
		{
			ACamSimEntity** EntityPtr = EntityMap.Find(S.EntityId);
			if (EntityPtr && IsValid(*EntityPtr))
			{
				(*EntityPtr)->Destroy();
			}
			EntityMap.Remove(S.EntityId);
			UE_LOG(LogCamSim, Log, TEXT("EntityManager: removed entity %u"), S.EntityId);
		}
	}
}

void FCamSimEntityManager::ProcessRateControls()
{
	FCigiReceiver* Receiver = Subsystem ? Subsystem->GetCigiReceiver() : nullptr;
	if (!Receiver) return;

	FCigiRateControl Rate;
	while (Receiver->DequeueRateControl(Rate))
	{
		ACamSimEntity** EntityPtr = EntityMap.Find(Rate.EntityId);
		if (EntityPtr && IsValid(*EntityPtr))
		{
			(*EntityPtr)->SetRateControl(Rate);
		}
	}
}

void FCamSimEntityManager::ProcessArtPartControls()
{
	FCigiReceiver* Receiver = Subsystem ? Subsystem->GetCigiReceiver() : nullptr;
	if (!Receiver) return;

	FCigiArtPartControl Art;
	while (Receiver->DequeueArtPart(Art))
	{
		ACamSimEntity** EntityPtr = EntityMap.Find(Art.EntityId);
		if (EntityPtr && IsValid(*EntityPtr))
		{
			(*EntityPtr)->ApplyArtPart(Art);
		}
	}
}

void FCamSimEntityManager::ProcessComponentControls()
{
	FCigiReceiver* Receiver = Subsystem ? Subsystem->GetCigiReceiver() : nullptr;
	if (!Receiver) return;

	FCigiComponentControl Comp;
	while (Receiver->DequeueCompCtrl(Comp))
	{
		ACamSimEntity** EntityPtr = EntityMap.Find(Comp.EntityId);
		if (EntityPtr && IsValid(*EntityPtr))
		{
			(*EntityPtr)->ApplyComponentControl(Comp);
		}
	}
}

// -------------------------------------------------------------------------
// SpawnEntity
// -------------------------------------------------------------------------

ACamSimEntity* FCamSimEntityManager::SpawnEntity(const FCigiEntityState& S)
{
	UWorld* World = Subsystem ? Subsystem->GetGameInstance()->GetWorld() : nullptr;
	if (!World || !World->GetCurrentLevel()) return nullptr;

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	ACamSimEntity* Entity = World->SpawnActor<ACamSimEntity>(
		ACamSimEntity::StaticClass(), FTransform::Identity, Params);

	if (!Entity) return nullptr;

	Entity->EntityId = S.EntityId;
	Entity->SetEntityTypeTable(TypeTable);
	Entity->SetEntityType(S.EntityType);
	Entity->ApplyPose(S);

	return Entity;
}

// -------------------------------------------------------------------------
// PurgeStaleEntities — remove pending-kill entries from map
// -------------------------------------------------------------------------

void FCamSimEntityManager::PurgeStaleEntities()
{
	TArray<uint16> ToRemove;
	for (const auto& Pair : EntityMap)
	{
		if (!IsValid(Pair.Value))
		{
			ToRemove.Add(Pair.Key);
		}
	}
	for (uint16 Id : ToRemove)
	{
		EntityMap.Remove(Id);
	}
	if (!ToRemove.IsEmpty())
	{
		UE_LOG(LogCamSim, Log, TEXT("EntityManager: purged %d stale entity actor(s)"), ToRemove.Num());
	}
}
