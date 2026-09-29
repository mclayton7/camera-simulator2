// Copyright CamSim Contributors. All Rights Reserved.

#include "Entity/CamSimEntityManager.h"
#include "Entity/CamSimEntity.h"
#include "Camera/CamSimCamera.h"
#include "Entity/EntityTypeTable.h"
#include "Entity/SurfaceProbe.h"
#include "GroundTruth/FEntityProjection.h"
#include "Subsystem/CamSimSubsystem.h"
#include "Environment/CamSimParticleManager.h"
#include "Scenario/ScenarioEngine.h"
#include "Time/SimClock.h"
#include "Scenario/ScenarioRandomizer.h"
#include "CIGI/CigiReceiver.h"
#include "DIS/DisEntityAdapter.h"
#include "Hosts/CigiCommands.h"
#include "Hosts/CigiHostAdapter.h"
#include "CamSimTest.h"

#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Actor.h"

#include "Geospatial/CigiFrames.h"

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
	// One ordered pass: host states (camera platform included), then
	// attachments parent-first, then the camera if it is itself attached.
	// ACamSimCamera ticks later (TG_PostUpdateWork) and captures the result.
	ACamSimCamera* Camera = Subsystem ? Subsystem->GetCamera() : nullptr;
	if (Subsystem && Subsystem->GetGameInstance())
	{
		WarmUpModels(Subsystem->GetGameInstance()->GetWorld(), FPlatformTime::Seconds());
	}
	PurgeStaleEntities();

	// Host adapters submit this frame's commands (each source has its own entity IDs).
	if (!CigiAdapter && Subsystem && Subsystem->GetCigiReceiver())
	{
		CigiAdapter = MakeUnique<FCigiHostAdapter>(Subsystem->GetCigiReceiver());
	}
	if (CigiAdapter) CigiAdapter->PollEntities(*this);
	if (FDisEntityAdapter* DisAdapter = Subsystem ? Subsystem->GetDisAdapter() : nullptr)
	{
		DisAdapter->Poll(*this);
	}

	if (Camera) Camera->ApplyHostPlatformState();
	ResolveAttachedEntities();
	if (Camera) Camera->FollowAttachParent();
	ProcessScenarioEntities();

	// Drive CIGI query handler and sender flush (SOF + HAT/HOT + LOS responses)
	if (Subsystem)
	{
		Subsystem->Tick(DeltaTime);
	}
}

// -------------------------------------------------------------------------
// CIGI attachment
// -------------------------------------------------------------------------

void FCamSimEntityManager::ResolveAttachedEntities()
{
	if (!Subsystem) return;

	// Parents before children: repeatedly place children whose parent is
	// top-level or already placed this tick. Bounded by the hierarchy depth.
	TSet<FEntityKey> Placed;
	TArray<ACamSimEntity*> Pending;
	for (const TPair<FEntityKey, ACamSimEntity*>& Pair : EntityMap)
	{
		if (IsValid(Pair.Value) && Pair.Value->IsAttached())
		{
			Pending.Add(Pair.Value);
		}
	}

	for (int32 Pass = 0; Pass < MaxAttachDepth && Pending.Num() > 0; ++Pass)
	{
		for (int32 i = Pending.Num() - 1; i >= 0; --i)
		{
			ACamSimEntity* Child = Pending[i];
			const FEntityKey& ParentKey = Child->GetParentKey();
			const ACamSimEntity* Parent = FindEntity(ParentKey);
			const bool bParentReady = !IsValid(Parent) || !Parent->IsAttached() || Placed.Contains(ParentKey);
			if (!bParentReady) continue;

			CamSimFrames::FGeoPose ParentPose;
			if (ParentKey != Child->Key && Subsystem->GetEntityGeoPose(ParentKey, ParentPose))
			{
				const FRotator Rel = Child->GetAttachRotation();
				Child->ApplyGeoPose(CamSimFrames::AttachedChildPose(
					ParentPose, Child->GetAttachOffsetFrd(), Rel.Yaw, Rel.Pitch, Rel.Roll));
			}
			// else: parent missing — hold the child's last pose
			Placed.Add(Child->Key);
			Pending.RemoveAtSwap(i);
		}
	}
	for (const ACamSimEntity* Orphan : Pending)
	{
		UE_LOG(LogCamSim, Verbose, TEXT("EntityManager: entity %s attachment chain too deep or cyclic"),
			*Orphan->Key.ToString());
	}
}

// -------------------------------------------------------------------------
// ISimCommandSink
// -------------------------------------------------------------------------

void FCamSimEntityManager::Submit(const FEntityCommand& Command)
{
	ApplyEntityCommand(Command, FPlatformTime::Seconds(), false);
}

void FCamSimEntityManager::Submit(const FEntityMotionCommand& Command)
{
	if (ACamSimEntity* Entity = FindEntity(Command.Key))
	{
		Entity->SetMotion(Command.Motion);
	}
}

void FCamSimEntityManager::Submit(const FArticulationCommand& Command)
{
	if (ACamSimEntity* Entity = FindEntity(Command.Key))
	{
		Entity->ApplyArticulation(Command);
	}
}

void FCamSimEntityManager::Submit(const FComponentCommand& Command)
{
	ACamSimEntity* Entity = FindEntity(Command.Key);

	// Phase 22C: component 10 of class 0 is the damage state
	const bool bDamage = Command.ComponentClass == 0 && Command.ComponentId == 10;
	const uint8 OldDamageState = (Entity && bDamage) ? Entity->GetDamageState() : 0;

	if (Entity)
	{
		Entity->ApplyComponent(Command);
	}
	if (FCamSimParticleManager* PM = Subsystem ? Subsystem->GetParticleManager() : nullptr)
	{
		PM->OnComponentControl(Command.Key, Entity, Command);
		if (Entity && bDamage)
		{
			const uint8 NewDamageState = FMath::Min(Command.State, static_cast<uint8>(2));
			if (OldDamageState != NewDamageState)
			{
				PM->OnDamageStateChanged(Command.Key, Entity, OldDamageState, NewDamageState);
			}
		}
	}
}

void FCamSimEntityManager::ApplyEntityCommand(const FEntityCommand& C, double NowSeconds, bool bBypassRateLimit)
{
	ACamSimEntity* Entity = FindEntity(C.Key);

	if (C.Lifecycle == EEntityLifecycle::Remove)
	{
		if (Entity)
		{
			Entity->Destroy();
		}
		ForgetEntity(C.Key);
		UE_LOG(LogCamSim, Log, TEXT("EntityManager: removed entity %s"), *C.Key.ToString());
		return;
	}

	if (C.Lifecycle == EEntityLifecycle::Hidden)
	{
		if (Entity)
		{
			Entity->SetActorHiddenInGame(true);
		}
		return;
	}

	bool bTypeChanged = false;
	const bool bJustSpawned = (Entity == nullptr);
	if (bJustSpawned)
	{
		Entity = SpawnEntity(C);
		if (!Entity)
		{
			UE_LOG(LogCamSim, Warning, TEXT("EntityManager: failed to spawn entity %s"), *C.Key.ToString());
			return;
		}
		EntityMap.Add(C.Key, Entity);
		UE_LOG(LogCamSim, Log, TEXT("EntityManager: spawned entity %s (type %u)"), *C.Key.ToString(), C.TypeId);
	}
	else if (Entity->EntityType != C.TypeId)
	{
		bTypeChanged = true;
		UE_LOG(LogCamSim, Log, TEXT("EntityManager: entity %s type change %u -> %u"),
			*C.Key.ToString(), Entity->EntityType, C.TypeId);
		Entity->SetEntityType(C.TypeId);
	}

	bool bApplyPose = true;
	if (!bBypassRateLimit && !bTypeChanged)
	{
		const float MaxHz = GetEntityMaxUpdateRateHz(C.Key);
		const double* LastApply = LastPoseApplySeconds.Find(C.Key);
		if (MaxHz > 0.0f && LastApply && (NowSeconds - *LastApply) < 1.0 / static_cast<double>(MaxHz))
		{
			bApplyPose = false;
		}
	}
	if (bApplyPose)
	{
		Entity->ApplyCommand(C);
		LastPoseApplySeconds.Add(C.Key, NowSeconds);
	}
	Entity->SetActorHiddenInGame(false);

	if (!bJustSpawned)
	{
		if (FCamSimParticleManager* PM = Subsystem ? Subsystem->GetParticleManager() : nullptr)
		{
			PM->OnEntityUpdated(C.Key, Entity, C);
		}
	}
	// Phase 19C: vessel motion for sea-domain entities
	if (OceanSurface && C.Classification.Domain == 3 && Subsystem->GetConfig().Phase19.bVesselMotionEnabled)
	{
		const FEntityTypeEntry* TypeEntry = TypeTable->FindEntry(C.TypeId);
		Entity->ApplyVesselMotion(OceanSurface,
			TypeEntry ? TypeEntry->HalfLengthCm : 0.0f, TypeEntry ? TypeEntry->HalfBeamCm : 0.0f,
			Subsystem->GetConfig().Phase19.VesselMotionScale);
	}
}

void FCamSimEntityManager::ForgetEntity(const FEntityKey& Key)
{
	if (FCamSimParticleManager* PM = Subsystem ? Subsystem->GetParticleManager() : nullptr)
	{
		PM->OnEntityRemoved(Key);
	}
	EntityMap.Remove(Key);
	LastPoseApplySeconds.Remove(Key);
	if (Key.Source == EHostSource::Scenario)
	{
		LastScenarioUpdateSeconds.Remove(static_cast<uint16>(Key.Id));
	}
}

// -------------------------------------------------------------------------
// SpawnEntity
// -------------------------------------------------------------------------

ACamSimEntity* FCamSimEntityManager::SpawnEntity(const FEntityCommand& C)
{
	// Enforce entity budget (Phase 4)
	if (Subsystem)
	{
		const int32 MaxEntities = Subsystem->GetConfig().MaxEntities;
		if (MaxEntities > 0 && EntityMap.Num() >= MaxEntities)
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("EntityManager: entity budget exhausted (%d/%d), rejecting entity %s"),
				EntityMap.Num(), MaxEntities, *C.Key.ToString());
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

	Entity->Key      = C.Key;
	Entity->EntityId = static_cast<uint16>(C.Key.Id & 0xFFFF);
	Entity->AnnotationId = AnnotationIds.Allocate();
	Entity->SetEntityTypeTable(TypeTable);
	if (!SurfaceProbe && Subsystem)
	{
		SurfaceProbe = MakeUnique<FCesiumSurfaceProbe>(World, Subsystem->GetGeospatialProvider());
		if (!Subsystem->GetConfig().bCreatePhysicsMeshes)
		{
			UE_LOG(LogCamSim, Warning, TEXT("EntityManager: create_physics_meshes is off — surface traces can't hit the terrain; ground entities use the sender's height, surface entities EGM96 sea level"));
		}
	}
	Entity->SetSurfaceProbe(SurfaceProbe.Get());
	Entity->SetEntityType(C.TypeId);
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
	Entity->ApplyCommand(C);
	if (FCamSimParticleManager* PM = Subsystem ? Subsystem->GetParticleManager() : nullptr)
	{
		PM->OnEntitySpawned(C.Key, Entity, C);
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
		Data.EntityId   = Entity->AnnotationId;
		Data.EntityType = Entity->EntityType;
		CamSimGroundTruth::SplitSourceKey(Entity->Key.ToString(), Data.Source, Data.SourceId);

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
	TArray<FEntityKey> ToRemove;
	for (const auto& Pair : EntityMap)
	{
		if (!IsValid(Pair.Value))
		{
			ToRemove.Add(Pair.Key);
		}
	}
	for (const FEntityKey& Key : ToRemove)
	{
		ForgetEntity(Key);
	}
	if (!ToRemove.IsEmpty())
	{
		UE_LOG(LogCamSim, Log, TEXT("EntityManager: purged %d stale entity actor(s)"), ToRemove.Num());
	}
}

float FCamSimEntityManager::GetEntityMaxUpdateRateHz(const FEntityKey& Key) const
{
	if (!Subsystem) return 0.0f;
	const FCamSimConfig& Cfg = Subsystem->GetConfig();
	// Overrides are keyed by the entity's ID within its source.
	if (const float* OverrideHz = Cfg.EntityScale.MaxUpdateRateHzOverrides.Find(static_cast<int32>(Key.Id)))
	{
		return FMath::Max(0.0f, *OverrideHz);
	}
	return FMath::Max(0.0f, Cfg.EntityScale.DefaultMaxUpdateRateHz);
}

ACamSimEntity* FCamSimEntityManager::FindEntity(const FEntityKey& Key) const
{
	ACamSimEntity* const* Found = EntityMap.Find(Key);
	return (Found && IsValid(*Found)) ? *Found : nullptr;
}

void FCamSimEntityManager::ProcessScenarioEntities()
{
	if (!Subsystem) return;
	const FCamSimConfig& Cfg = Subsystem->GetConfig();
	if (!Cfg.bScenarioEnabled || Cfg.ScenarioEntities.IsEmpty()) return;

	// Wall time only for per-entity update rate limiting.
	const double NowSeconds = FPlatformTime::Seconds();

	// Scenario time is sim time (ROADMAP 2.1): it pauses, scales and steps
	// with the sim clock, and its time of day matches the sun's.
	FSimClock& Clock = FSimClock::Get();
	if (!ScenarioEngine)
	{
		// Phase 23E: Apply randomization to a config copy before initializing
		FCamSimConfig WorkCfg = Cfg;
		if (WorkCfg.Randomization.bEnabled)
		{
			FScenarioRandomizer::Randomize(WorkCfg);
		}

		// scenario.start_hour is local solar time at the start position.
		if (WorkCfg.ScenarioStartHour >= 0.0f)
		{
			const double UtcHour = FMath::Fmod(WorkCfg.ScenarioStartHour - WorkCfg.StartLongitude / 15.0 + 48.0, 24.0);
			Clock.SetUtc(Clock.NowUtc().GetDate() + FTimespan::FromHours(UtcHour));
		}
		if (!FMath::IsNearlyEqual(WorkCfg.ScenarioTimeScale, 1.0f))
		{
			Clock.SetRate(Clock.GetRate() * FMath::Max(0.0f, WorkCfg.ScenarioTimeScale));
		}

		ScenarioEngine = MakeUnique<FScenarioEngine>();
		ScenarioEngine->Initialize(WorkCfg);
		ScenarioStartMicros = LastScenarioMicros = Clock.NowMicros();
		ScenarioLongitude = WorkCfg.StartLongitude;
		UE_LOG(LogCamSim, Log, TEXT("EntityManager: scenario orchestration enabled (%d entities, %d triggers) at sim time %s, clock rate %.2f"),
			WorkCfg.ScenarioEntities.Num(), WorkCfg.ScenarioTriggers.Num(), *Clock.NowUtc().ToIso8601(), Clock.GetRate());
	}

	const uint64 SimMicros = Clock.NowMicros();
	const double ScenarioElapsed = (SimMicros - ScenarioStartMicros) / 1e6;
	const float  DeltaTime = static_cast<float>((SimMicros - FMath::Min(LastScenarioMicros, SimMicros)) / 1e6);
	LastScenarioMicros = SimMicros;
	// Local solar time of day, for pattern-of-life schedules.
	const FDateTime SimNow = FSimClock::FromMicros(SimMicros);
	const float TOD = static_cast<float>(FMath::Fmod(
		SimNow.GetTimeOfDay().GetTotalHours() + ScenarioLongitude / 15.0 + 48.0, 24.0));

	// Despawn check (still handled here for rate-limiting integration)
	for (const FCamSimConfig::FScenarioEntityConfig& Spec : Cfg.ScenarioEntities)
	{
		const uint16 ScenarioEntityId = static_cast<uint16>(FMath::Clamp(Spec.EntityId, 0, 65535));
		const bool bShouldDespawn =
			(Spec.DespawnTimeSec > Spec.SpawnTimeSec) &&
			(ScenarioElapsed > static_cast<double>(Spec.DespawnTimeSec));

		if (bShouldDespawn && !ScenarioRemovedEntities.Contains(ScenarioEntityId))
		{
			FEntityCommand Remove;
			Remove.Key = FEntityKey(EHostSource::Scenario, ScenarioEntityId);
			Remove.Lifecycle = EEntityLifecycle::Remove;
			ApplyEntityCommand(Remove, NowSeconds, true);
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
		// The scenario engine still speaks CIGI entity states internally
		// (ROADMAP 2.3 phase 7); its entities live in the scenario namespace.
		FEntityCommand Command = CamSim::Cigi::ToEntityCommand(S);
		Command.Key = FEntityKey(EHostSource::Scenario, EId);
		ApplyEntityCommand(Command, NowSeconds, false);
	}

	// Process removals from triggers
	for (uint16 RemoveId : ScenarioEngine->GetPendingRemovals())
	{
		FEntityCommand Remove;
		Remove.Key = FEntityKey(EHostSource::Scenario, RemoveId);
		Remove.Lifecycle = EEntityLifecycle::Remove;
		ApplyEntityCommand(Remove, NowSeconds, true);
	}
}

// -------------------------------------------------------------------------
// Model warm-up
// -------------------------------------------------------------------------

void FCamSimEntityManager::WarmUpModels(UWorld* World, double NowSeconds)
{
	constexpr double WarmUpHoldSeconds = 30.0;
	if (bModelsWarmed)
	{
		if (WarmUpActors.Num() > 0 && NowSeconds >= WarmUpEndSeconds)
		{
			for (const TWeakObjectPtr<AActor>& A : WarmUpActors)
			{
				if (A.IsValid()) A->Destroy();
			}
			WarmUpActors.Empty();
		}
		return;
	}
	if (!World || !World->GetCurrentLevel() || !World->HasBegunPlay() || !TypeTable) return;
	bModelsWarmed = true;
	WarmUpEndSeconds = NowSeconds + WarmUpHoldSeconds;

	// 50 km below the world origin: outside every view, but the component registers,
	// creates its scene proxy and precaches its PSOs, which compiles the shaders.
	const FVector Hidden(0.0, 0.0, -5.0e6);
	for (UStaticMesh* Mesh : TypeTable->GetPreloadedStaticMeshes())
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(Hidden), Params);
		if (!Actor) continue;
		UStaticMeshComponent* Comp = NewObject<UStaticMeshComponent>(Actor);
		Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Comp->SetCastShadow(false);
		Comp->SetStaticMesh(Mesh);
		Actor->SetRootComponent(Comp);
		Comp->RegisterComponent();
		Comp->SetWorldLocation(Hidden);
		WarmUpActors.Add(Actor);
	}
	UE_LOG(LogCamSim, Log, TEXT("EntityManager: warming up %d entity model(s) for %.0f s"),
		WarmUpActors.Num(), WarmUpHoldSeconds);
}
