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
static constexpr double METRES_PER_DEGREE_LAT = 111320.0;

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
	LastPoseApplySeconds.Empty();
	LastScenarioUpdateSeconds.Empty();
	ScenarioRemovedEntities.Empty();
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
	ProcessScenarioEntities();

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

	const double NowSeconds = FPlatformTime::Seconds();
	for (const auto& Pair : FrameStates)
	{
		ApplyEntityState(Pair.Value, NowSeconds, false);
	}
}

void FCamSimEntityManager::ApplyEntityState(const FCigiEntityState& S, double NowSeconds, bool bBypassRateLimit)
{
	if (S.EntityState == CIGI_ENTITY_ACTIVE)
	{
		ACamSimEntity** ExistingPtr = EntityMap.Find(S.EntityId);
		ACamSimEntity*  Entity = ExistingPtr ? *ExistingPtr : nullptr;
		bool bTypeChanged = false;

		if (!IsValid(Entity))
		{
			Entity = SpawnEntity(S);
			if (!Entity)
			{
				UE_LOG(LogCamSim, Warning, TEXT("EntityManager: failed to spawn entity %u"), S.EntityId);
				return;
			}
			EntityMap.Add(S.EntityId, Entity);
			UE_LOG(LogCamSim, Log, TEXT("EntityManager: spawned entity %u (type %u)"),
				S.EntityId, S.EntityType);
		}
		else if (Entity->EntityType != S.EntityType)
		{
			bTypeChanged = true;
			UE_LOG(LogCamSim, Log, TEXT("EntityManager: entity %u type change %u -> %u"),
				S.EntityId, Entity->EntityType, S.EntityType);
			Entity->SetEntityType(S.EntityType);
		}

		bool bApplyPose = true;
		if (!bBypassRateLimit && !bTypeChanged)
		{
			const float MaxHz = GetEntityMaxUpdateRateHz(S.EntityId);
			if (MaxHz > 0.0f)
			{
				const double MinInterval = 1.0 / static_cast<double>(MaxHz);
				if (const double* LastApply = LastPoseApplySeconds.Find(S.EntityId))
				{
					if ((NowSeconds - *LastApply) < MinInterval)
					{
						bApplyPose = false;
					}
				}
			}
		}

		if (bApplyPose)
		{
			Entity->ApplyPose(S);
			LastPoseApplySeconds.Add(S.EntityId, NowSeconds);
		}
		Entity->SetActorHiddenInGame(false);
		return;
	}

	if (S.EntityState == CIGI_ENTITY_STANDBY)
	{
		ACamSimEntity** EntityPtr = EntityMap.Find(S.EntityId);
		if (EntityPtr && IsValid(*EntityPtr))
		{
			(*EntityPtr)->SetActorHiddenInGame(true);
		}
		return;
	}

	if (S.EntityState == CIGI_ENTITY_REMOVE)
	{
		ACamSimEntity** EntityPtr = EntityMap.Find(S.EntityId);
		if (EntityPtr && IsValid(*EntityPtr))
		{
			(*EntityPtr)->Destroy();
		}
		EntityMap.Remove(S.EntityId);
		LastPoseApplySeconds.Remove(S.EntityId);
		LastScenarioUpdateSeconds.Remove(S.EntityId);
		UE_LOG(LogCamSim, Log, TEXT("EntityManager: removed entity %u"), S.EntityId);
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
	if (Subsystem)
	{
		const FCamSimConfig& Cfg = Subsystem->GetConfig();
		Entity->ApplyScaleControls(Cfg.EntityScale.MaxDrawDistanceM, Cfg.EntityScale.TickRateHz);
	}
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
		LastPoseApplySeconds.Remove(Id);
		LastScenarioUpdateSeconds.Remove(Id);
	}
	if (!ToRemove.IsEmpty())
	{
		UE_LOG(LogCamSim, Log, TEXT("EntityManager: purged %d stale entity actor(s)"), ToRemove.Num());
	}
}

float FCamSimEntityManager::GetEntityMaxUpdateRateHz(uint16 EntityId) const
{
	if (!Subsystem) return 0.0f;
	const FCamSimConfig& Cfg = Subsystem->GetConfig();
	if (const float* OverrideHz = Cfg.EntityScale.MaxUpdateRateHzOverrides.Find(static_cast<int32>(EntityId)))
	{
		return FMath::Max(0.0f, *OverrideHz);
	}
	return FMath::Max(0.0f, Cfg.EntityScale.DefaultMaxUpdateRateHz);
}

FCigiEntityState FCamSimEntityManager::BuildScenarioState(
	const FCamSimConfig::FScenarioEntityConfig& Spec,
	double ScenarioElapsedSeconds) const
{
	const double TimeSinceSpawn = FMath::Max(0.0, ScenarioElapsedSeconds - static_cast<double>(Spec.SpawnTimeSec));
	const double LatRad = FMath::DegreesToRadians(Spec.StartLatitude);
	const double CosLat = FMath::Max(0.01, FMath::Abs(FMath::Cos(LatRad)));

	FCigiEntityState Out;
	Out.EntityId = static_cast<uint16>(FMath::Clamp(Spec.EntityId, 0, 65535));
	Out.EntityType = static_cast<uint16>(FMath::Clamp(Spec.EntityType, 0, 65535));
	Out.EntityState = CIGI_ENTITY_ACTIVE;
	Out.Latitude = Spec.StartLatitude + (static_cast<double>(Spec.NorthRateMps) * TimeSinceSpawn) / METRES_PER_DEGREE_LAT;
	Out.Longitude = Spec.StartLongitude + (static_cast<double>(Spec.EastRateMps) * TimeSinceSpawn) / (METRES_PER_DEGREE_LAT * CosLat);
	Out.Altitude = static_cast<float>(Spec.StartAltitude + static_cast<double>(Spec.UpRateMps) * TimeSinceSpawn);
	Out.Yaw = Spec.StartYaw + Spec.YawRateDegPerSec * static_cast<float>(TimeSinceSpawn);
	Out.Pitch = Spec.StartPitch + Spec.PitchRateDegPerSec * static_cast<float>(TimeSinceSpawn);
	Out.Roll = Spec.StartRoll + Spec.RollRateDegPerSec * static_cast<float>(TimeSinceSpawn);
	return Out;
}

void FCamSimEntityManager::ProcessScenarioEntities()
{
	if (!Subsystem) return;
	const FCamSimConfig& Cfg = Subsystem->GetConfig();
	if (!Cfg.bScenarioEnabled || Cfg.ScenarioEntities.IsEmpty()) return;

	const double NowSeconds = FPlatformTime::Seconds();
	if (ScenarioStartSeconds <= 0.0)
	{
		ScenarioStartSeconds = NowSeconds;
		UE_LOG(LogCamSim, Log, TEXT("EntityManager: scenario orchestration enabled (%d entities, time_scale=%.2f)"),
			Cfg.ScenarioEntities.Num(), Cfg.ScenarioTimeScale);
	}

	const double ScenarioElapsed = (NowSeconds - ScenarioStartSeconds) * FMath::Max(0.0f, Cfg.ScenarioTimeScale);
	for (const FCamSimConfig::FScenarioEntityConfig& Spec : Cfg.ScenarioEntities)
	{
		const uint16 ScenarioEntityId = static_cast<uint16>(FMath::Clamp(Spec.EntityId, 0, 65535));
		if (ScenarioElapsed < static_cast<double>(Spec.SpawnTimeSec))
		{
			continue;
		}

		const bool bShouldDespawn =
			(Spec.DespawnTimeSec > Spec.SpawnTimeSec) &&
			(ScenarioElapsed > static_cast<double>(Spec.DespawnTimeSec));

		if (bShouldDespawn)
		{
			FCigiEntityState RemoveState;
			RemoveState.EntityId = ScenarioEntityId;
			RemoveState.EntityState = CIGI_ENTITY_REMOVE;
			if (!ScenarioRemovedEntities.Contains(ScenarioEntityId))
			{
				ApplyEntityState(RemoveState, NowSeconds, true);
				ScenarioRemovedEntities.Add(ScenarioEntityId);
			}
			continue;
		}
		ScenarioRemovedEntities.Remove(ScenarioEntityId);

		const float ScenarioUpdateHz = FMath::Max(0.0f, Spec.UpdateRateHz);
		if (ScenarioUpdateHz > 0.0f)
		{
			const double MinInterval = 1.0 / static_cast<double>(ScenarioUpdateHz);
			if (const double* LastUpdate = LastScenarioUpdateSeconds.Find(ScenarioEntityId))
			{
				if ((NowSeconds - *LastUpdate) < MinInterval)
				{
					continue;
				}
			}
		}
		LastScenarioUpdateSeconds.Add(ScenarioEntityId, NowSeconds);

		const FCigiEntityState ScenarioState = BuildScenarioState(Spec, ScenarioElapsed);
		ApplyEntityState(ScenarioState, NowSeconds, false);
	}
}
