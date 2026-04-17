// Copyright CamSim Contributors. All Rights Reserved.

#include "Entity/CamSimEntityManager.h"
#include "Entity/CamSimEntity.h"
#include "Entity/EntityTypeTable.h"
#include "GroundTruth/FEntityProjection.h"
#include "Subsystem/CamSimSubsystem.h"
#include "Environment/CamSimParticleManager.h"
#include "Scenario/ScenarioEngine.h"
#include "Scenario/ScenarioRandomizer.h"
#include "CIGI/CigiReceiver.h"
#include "DIS/DisEntityAdapter.h"
#include "CamSimTest.h"

#include "Engine/World.h"

// CCL EntityState enum values (from CigiBaseEntityCtrl.h)
static constexpr uint8 CIGI_ENTITY_STANDBY = 0;
static constexpr uint8 CIGI_ENTITY_ACTIVE  = 1;
static constexpr uint8 CIGI_ENTITY_REMOVE  = 2;
#include "Geospatial/GeoConstants.h"

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

void FCamSimEntityManager::SetOceanSurface(IOceanSurface* Ocean)
{
	OceanSurface = Ocean;
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
	ProcessEntityStates(DeltaTime);
	ProcessConfClampEntities();
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

void FCamSimEntityManager::ProcessEntityStates(float DeltaTime)
{
	// Collect last state per entity ID this frame (last write wins)
	TMap<uint16, FCigiEntityState> FrameStates;
	FCigiEntityState State;

	// Drain CIGI entity states
	FCigiReceiver* Receiver = Subsystem ? Subsystem->GetCigiReceiver() : nullptr;
	if (Receiver)
	{
		while (Receiver->DequeueEntityState(State))
		{
			FrameStates.Add(State.EntityId, State);
		}
	}

	// Drain DIS entity states (Phase 21) — tick adapter first to process PDUs
	FDisEntityAdapter* DisAdapter = Subsystem ? Subsystem->GetDisAdapter() : nullptr;
	if (DisAdapter)
	{
		DisAdapter->Tick(DeltaTime);
		while (DisAdapter->DequeueEntityState(State))
		{
			FrameStates.Add(State.EntityId, State);
		}
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
		bool bTypeChanged  = false;
		const bool bJustSpawned = !IsValid(Entity);

		if (bJustSpawned)
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
		if (!bJustSpawned)
		{
			if (FCamSimParticleManager* PM = Subsystem ? Subsystem->GetParticleManager() : nullptr)
			{
				PM->OnEntityUpdated(S.EntityId, Entity, S);
			}
		}
		// Phase 19C: vessel motion for sea-domain entities
		if (OceanSurface && S.EntityDomain == 3 && Subsystem->GetConfig().Phase19.bVesselMotionEnabled)
		{
			const FEntityTypeEntry* TypeEntry = TypeTable->FindEntry(S.EntityType);
			const float HalfLen = TypeEntry ? TypeEntry->HalfLengthCm : 0.0f;
			const float HalfBm  = TypeEntry ? TypeEntry->HalfBeamCm   : 0.0f;
			Entity->ApplyVesselMotion(OceanSurface, HalfLen, HalfBm,
			                          Subsystem->GetConfig().Phase19.VesselMotionScale);
		}
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
		if (FCamSimParticleManager* PM = Subsystem ? Subsystem->GetParticleManager() : nullptr)
		{
			PM->OnEntityRemoved(S.EntityId);
		}
		EntityMap.Remove(S.EntityId);
		LastPoseApplySeconds.Remove(S.EntityId);
		LastScenarioUpdateSeconds.Remove(S.EntityId);
		UE_LOG(LogCamSim, Log, TEXT("EntityManager: removed entity %u"), S.EntityId);
	}
}

void FCamSimEntityManager::ProcessConfClampEntities()
{
	FCigiReceiver* Receiver = Subsystem ? Subsystem->GetCigiReceiver() : nullptr;
	if (!Receiver) return;

	FCigiConfClampEntityState Clamp;
	const double NowSeconds = FPlatformTime::Seconds();

	while (Receiver->DequeueConfClampEntity(Clamp))
	{
		// Convert to standard entity state: Active, clamped to terrain (pitch=0, roll=0)
		FCigiEntityState State;
		State.EntityId    = Clamp.EntityId;
		State.EntityState = CIGI_ENTITY_ACTIVE;
		State.Latitude    = Clamp.Latitude;
		State.Longitude   = Clamp.Longitude;
		State.Yaw         = Clamp.Yaw;
		State.Pitch       = 0.0f;
		State.Roll        = 0.0f;

		// Altitude: conformal clamped entities sit on terrain surface.
		// Set to 0 (MSL); Cesium globe anchor + terrain mesh will place the
		// entity visually on the terrain.  A more accurate implementation
		// would perform a downward line trace to get exact terrain height.
		State.Altitude = 0.0f;

		ApplyEntityState(State, NowSeconds, false);
	}
}

void FCamSimEntityManager::ProcessRateControls()
{
	FCigiRateControl Rate;

	// Drain CIGI rate controls
	FCigiReceiver* Receiver = Subsystem ? Subsystem->GetCigiReceiver() : nullptr;
	if (Receiver)
	{
		while (Receiver->DequeueRateControl(Rate))
		{
			ACamSimEntity** EntityPtr = EntityMap.Find(Rate.EntityId);
			if (EntityPtr && IsValid(*EntityPtr))
			{
				(*EntityPtr)->SetRateControl(Rate);
			}
		}
	}

	// Drain DIS rate controls (Phase 21)
	FDisEntityAdapter* DisAdapter = Subsystem ? Subsystem->GetDisAdapter() : nullptr;
	if (DisAdapter)
	{
		while (DisAdapter->DequeueRateControl(Rate))
		{
			ACamSimEntity** EntityPtr = EntityMap.Find(Rate.EntityId);
			if (EntityPtr && IsValid(*EntityPtr))
			{
				(*EntityPtr)->SetRateControl(Rate);
			}
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
		ACamSimEntity*  Entity = (EntityPtr && IsValid(*EntityPtr)) ? *EntityPtr : nullptr;

		// Phase 22C: track old damage state for FX callback
		uint8 OldDamageState = 0;
		if (Entity && Comp.CompClass == 0 && Comp.CompId == 10)
		{
			OldDamageState = Entity->GetDamageState();
		}

		if (Entity)
		{
			Entity->ApplyComponentControl(Comp);
		}
		if (FCamSimParticleManager* PM = Subsystem ? Subsystem->GetParticleManager() : nullptr)
		{
			PM->OnComponentControl(Comp.EntityId,
				Entity ? static_cast<AActor*>(Entity) : nullptr, Comp);

			// Phase 22C: damage transition FX
			if (Entity && Comp.CompClass == 0 && Comp.CompId == 10)
			{
				const uint8 NewDamageState = FMath::Min(Comp.CompState, static_cast<uint8>(2));
				if (OldDamageState != NewDamageState)
				{
					PM->OnDamageStateChanged(Comp.EntityId, Entity, OldDamageState, NewDamageState);
				}
			}
		}
	}
}

// -------------------------------------------------------------------------
// SpawnEntity
// -------------------------------------------------------------------------

ACamSimEntity* FCamSimEntityManager::SpawnEntity(const FCigiEntityState& S)
{
	// Enforce entity budget (Phase 4)
	if (Subsystem)
	{
		const int32 MaxEntities = Subsystem->GetConfig().MaxEntities;
		if (MaxEntities > 0 && EntityMap.Num() >= MaxEntities)
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("EntityManager: entity budget exhausted (%d/%d), rejecting entity %u"),
				EntityMap.Num(), MaxEntities, S.EntityId);
			return nullptr;
		}
	}

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
		Entity->SetShadowCasting(Cfg.RenderingQuality.bEntityShadows);  // 24A
		// Phase 22C: configure gradual damage interpolation
		if (Cfg.DamageTransition.bGradualDamage)
		{
			Entity->SetDamageInterpolation(true, Cfg.DamageTransition.DamageInterpolationSec);
		}
	}
	Entity->ApplyPose(S);
	if (FCamSimParticleManager* PM = Subsystem ? Subsystem->GetParticleManager() : nullptr)
	{
		PM->OnEntitySpawned(S.EntityId, Entity, S);
	}

	return Entity;
}

// -------------------------------------------------------------------------
// GetEntitySnapshot — game-thread entity annotation capture (Phase 17D)
// -------------------------------------------------------------------------

void FCamSimEntityManager::GetEntitySnapshot(
    const FViewProjectionData& ViewProj,
    TArray<FEntityAnnotationData>& OutSnapshot) const
{
	// Reset() preserves allocated capacity so the caller's buffer amortises
	// across frames — avoids one ~N-entity allocation per tick.
	OutSnapshot.Reset(EntityMap.Num());

	const bool bUseConeCull = (ViewProj.CullConeHalfAngleCos > 0.0f)
		&& !ViewProj.CameraForward.IsNearlyZero();
	const FVector CamFwd = bUseConeCull ? ViewProj.CameraForward.GetSafeNormal() : FVector::ZeroVector;

	for (const auto& Pair : EntityMap)
	{
		const ACamSimEntity* Entity = Pair.Value;
		if (!IsValid(Entity)) continue;

		// Cheap visibility pre-filter: skip actors UE has recently culled
		if (!Entity->WasRecentlyRendered(0.1f)) continue;

		// Cone-cull: reject entities whose direction from the camera falls
		// outside the inflated view cone before paying for AABB projection.
		// Entities closer than 1 m get a free pass (the direction vector is
		// unstable and they're almost certainly inside the frustum anyway).
		if (bUseConeCull)
		{
			const FVector ToEntity = Entity->GetActorLocation() - ViewProj.CameraLocation;
			const double  DistSq   = ToEntity.SizeSquared();
			constexpr double MinDistSqCm2 = 100.0 * 100.0;  // 1 m in UE cm units
			if (DistSq > MinDistSqCm2)
			{
				const FVector ToEntityDir = ToEntity / FMath::Sqrt(DistSq);
				if (FVector::DotProduct(ToEntityDir, CamFwd) < ViewProj.CullConeHalfAngleCos)
				{
					continue;
				}
			}
		}

		FEntityAnnotationData Data;
		Data.EntityId   = Entity->EntityId;
		Data.EntityType = Entity->EntityType;

		// Look up class label from entity type table
		if (TypeTable)
		{
			if (const FEntityTypeEntry* Entry = TypeTable->FindEntry(Entity->EntityType))
				Data.ClassName = Entry->ClassName;
		}
		if (Data.ClassName.IsEmpty())
			Data.ClassName = FString::Printf(TEXT("type_%u"), Entity->EntityType);

		// World-space AABB from all components (non-colliding meshes included)
		FVector Origin, Extent;
		Entity->GetActorBounds(/*bOnlyCollidingComponents=*/false, Origin, Extent);
		if (Extent.IsNearlyZero()) continue;

		const FBox WorldAABB(Origin - Extent, Origin + Extent);

		FBox2D ScreenBBox(ForceInit);
		bool   bTruncated = false;
		const bool bVisible = FEntityProjection::ProjectAABB(
			WorldAABB, ViewProj.ViewProjectionMatrix,
			ViewProj.ImageWidth, ViewProj.ImageHeight,
			ScreenBBox, bTruncated);

		Data.bVisible   = bVisible;
		Data.bTruncated = bTruncated;
		Data.ScreenBBox = ScreenBBox;
		OutSnapshot.Add(MoveTemp(Data));
	}
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
		if (FCamSimParticleManager* PM = Subsystem ? Subsystem->GetParticleManager() : nullptr)
		{
			PM->OnEntityRemoved(Id);
		}
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

ACamSimEntity* FCamSimEntityManager::FindEntity(uint16 EntityId) const
{
	ACamSimEntity* const* Found = EntityMap.Find(EntityId);
	return (Found && IsValid(*Found)) ? *Found : nullptr;
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

		// Phase 23E: Apply randomization to a config copy before initializing
		FCamSimConfig WorkCfg = Cfg;
		if (WorkCfg.Randomization.bEnabled)
		{
			FScenarioRandomizer::Randomize(WorkCfg);
		}

		ScenarioEngine = MakeUnique<FScenarioEngine>();
		ScenarioEngine->Initialize(WorkCfg);
		UE_LOG(LogCamSim, Log, TEXT("EntityManager: scenario orchestration enabled (%d entities, %d triggers, time_scale=%.2f)"),
			WorkCfg.ScenarioEntities.Num(), WorkCfg.ScenarioTriggers.Num(), WorkCfg.ScenarioTimeScale);
	}

	const double ScenarioElapsed = (NowSeconds - ScenarioStartSeconds) * FMath::Max(0.0f, Cfg.ScenarioTimeScale);
	const float  DeltaTime = 1.0f / 30.0f; // fixed frame rate
	const float  TOD = FMath::Fmod(Cfg.ScenarioStartHour + static_cast<float>(ScenarioElapsed / 3600.0), 24.0f);

	// Despawn check (still handled here for rate-limiting integration)
	for (const FCamSimConfig::FScenarioEntityConfig& Spec : Cfg.ScenarioEntities)
	{
		const uint16 ScenarioEntityId = static_cast<uint16>(FMath::Clamp(Spec.EntityId, 0, 65535));
		const bool bShouldDespawn =
			(Spec.DespawnTimeSec > Spec.SpawnTimeSec) &&
			(ScenarioElapsed > static_cast<double>(Spec.DespawnTimeSec));

		if (bShouldDespawn && !ScenarioRemovedEntities.Contains(ScenarioEntityId))
		{
			FCigiEntityState RemoveState;
			RemoveState.EntityId = ScenarioEntityId;
			RemoveState.EntityState = CIGI_ENTITY_REMOVE;
			ApplyEntityState(RemoveState, NowSeconds, true);
			ScenarioRemovedEntities.Add(ScenarioEntityId);
		}
	}

	// Delegate to ScenarioEngine for entity state production
	TArray<FCigiEntityState> States = ScenarioEngine->Tick(ScenarioElapsed, DeltaTime, TOD, EntityMap);
	for (const FCigiEntityState& S : States)
	{
		const uint16 EId = S.EntityId;
		if (ScenarioRemovedEntities.Contains(EId)) continue;

		// Rate limiting
		const FCamSimConfig::FScenarioEntityConfig* FoundSpec = nullptr;
		for (const auto& Spec : Cfg.ScenarioEntities)
		{
			if (static_cast<uint16>(FMath::Clamp(Spec.EntityId, 0, 65535)) == EId)
			{
				FoundSpec = &Spec;
				break;
			}
		}
		if (FoundSpec)
		{
			const float ScenarioUpdateHz = FMath::Max(0.0f, FoundSpec->UpdateRateHz);
			if (ScenarioUpdateHz > 0.0f)
			{
				const double MinInterval = 1.0 / static_cast<double>(ScenarioUpdateHz);
				if (const double* LastUpdate = LastScenarioUpdateSeconds.Find(EId))
				{
					if ((NowSeconds - *LastUpdate) < MinInterval) continue;
				}
			}
		}
		LastScenarioUpdateSeconds.Add(EId, NowSeconds);
		ApplyEntityState(S, NowSeconds, false);
	}

	// Process removals from triggers
	for (uint16 RemoveId : ScenarioEngine->GetPendingRemovals())
	{
		FCigiEntityState RemoveState;
		RemoveState.EntityId = RemoveId;
		RemoveState.EntityState = CIGI_ENTITY_REMOVE;
		ApplyEntityState(RemoveState, NowSeconds, true);
	}
}
