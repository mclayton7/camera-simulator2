// Copyright CamSim Contributors. All Rights Reserved.

#include "Entity/CamSimEntityManager.h"
#include "Entity/CamSimEntity.h"
#include "Camera/CamSimCamera.h"
#include "Entity/EntityTypeTable.h"
#include "Thermal/ThermalFrameBuilder.h"   // FThermalStencilEntity
#include "Entity/SurfaceProbe.h"
#include "Entity/StencilSlotAllocator.h"
#include "GroundTruth/FEntityProjection.h"
#include "Subsystem/CamSimSubsystem.h"
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
#include "Time/SimClock.h"

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
	StencilOf.Empty();
	LastPoseApplySeconds.Empty();
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

	// ROADMAP 4C: entity thermal state on sim time, after every pose is final for this tick.
	StepEntityThermal(static_cast<double>(FSimClock::Get().NowMicros()) * 1e-6);

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

	if (Entity)
	{
		Entity->ApplyComponent(Command);
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
	else if (const uint16 NewType = ResolveEntityTypeId(Entity->EntityType, C); NewType != Entity->EntityType)
	{
		bTypeChanged = true;
		UE_LOG(LogCamSim, Log, TEXT("EntityManager: entity %s type change %u -> %u"),
			*C.Key.ToString(), Entity->EntityType, NewType);
		Entity->SetEntityType(NewType);
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
}

void FCamSimEntityManager::ForgetEntity(const FEntityKey& Key)
{
	EntityMap.Remove(Key);
	uint8 Stencil = 0;
	if (StencilOf.RemoveAndCopyValue(Key, Stencil))
	{
		StencilSlots.Release(Stencil, GFrameCounter);
	}
	LastPoseApplySeconds.Remove(Key);
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
	// Tagged only when the instance-ID pass or ThermalCS runs (decided once at startup): an untagged entity costs no
	// custom-depth draw and gets the projected box.
	if (Subsystem && Subsystem->IsEntityStencilTaggingEnabled())
	{
		// A stale entry for this key (actor died before PurgeStaleEntities ran) must not leak its value.
		uint8 Old = 0;
		if (StencilOf.RemoveAndCopyValue(C.Key, Old))
		{
			StencilSlots.Release(Old, GFrameCounter);
		}
		const uint8 Stencil = StencilSlots.Allocate(GFrameCounter);
		if (Stencil == 0 && !bLoggedStencilExhausted)
		{
			bLoggedStencilExhausted = true;
			UE_LOG(LogCamSim, Warning, TEXT("EntityManager: more than 255 stencil-tagged entities; extra entities get projected ground-truth boxes ")
				TEXT("and render as terrain in thermal IR (logged once)"));
		}
		if (Stencil != 0)
		{
			StencilOf.Add(C.Key, Stencil);
		}
		Entity->SetGroundTruthStencil(Stencil);
	}
	Entity->SetEntityTypeTable(TypeTable);
	if (!SurfaceProbe && Subsystem)
	{
		SurfaceProbe = MakeUnique<FCesiumSurfaceProbe>(World, Subsystem->GetGeospatialProvider(),
			Subsystem->GetConfig().bCreatePhysicsMeshes);
	}
	Entity->SetSurfaceProbe(SurfaceProbe.Get());
	Entity->SetEntityType(C.TypeId);
	if (Subsystem)
	{
		const FCamSimConfig& Cfg = Subsystem->GetConfig();
		Entity->ApplyScaleControls(Cfg.EntityScale.MaxDrawDistanceM, Cfg.EntityScale.TickRateHz);
		Entity->SetShadowCasting(Cfg.RenderingQuality.bEntityShadows);  // 24A
	}
	Entity->ApplyCommand(C);

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

		// The shown meshes only (particles, lights and empty mesh slots excluded): box3d and the projected
		// fallback box both come from it. No mesh shown yet: nothing is drawn, nothing to label.
		const FBox LocalBox = Entity->GetGroundTruthLocalBox();
		if (!LocalBox.IsValid || LocalBox.GetExtent().IsNearlyZero()) continue;
		const FBox WorldAABB = LocalBox.TransformBy(Entity->GetActorTransform());

		FBox2D ScreenBBox(ForceInit);
		bool   bTruncated = false;
		const bool bVisible = FEntityProjection::ProjectAABB(
			WorldAABB, ViewProj.ViewProjectionMatrix,
			ViewProj.ImageWidth, ViewProj.ImageHeight,
			ScreenBBox, bTruncated);

		Data.bVisible   = bVisible;
		Data.bTruncated = bTruncated;
		Data.ScreenBBox = ScreenBBox;
		// Thermal can tag entities without the instance-ID pass: only report the stencil when masks are measured.
		Data.StencilValue = (Subsystem && Subsystem->IsGroundTruthMaskAvailable()) ? Entity->GetGroundTruthStencil() : 0;
		Data.bWaterSurface = Entity->IsWaterSurfaceVessel();
		const FProjectedBox3D P = FEntityProjection::ProjectOrientedBox(LocalBox, Entity->GetActorTransform(),
			ViewProj.ViewProjectionMatrix, ViewProj.ImageWidth, ViewProj.ImageHeight, ViewProj.FocalPx, ViewProj.K1, ViewProj.K2);
		Data.bHasBox3D     = true;
		Data.Box3DSizeM    = LocalBox.GetSize() * Entity->GetActorScale3D() / 100.0;
		Data.bCornersValid = P.bValid;
		FMemory::Memcpy(Data.CornersPx, P.Corners, sizeof(P.Corners));
		Data.Truncation    = P.Truncation;
		if (P.bValid) Data.bTruncated = P.Truncation > 0.01;
		CamSimFrames::FGeoPose Geo;
		if (Entity->GetGeoPose(Geo))
		{
			Data.bHasGeo = true;
			Data.Lat = Geo.Lat; Data.Lon = Geo.Lon; Data.AltM = Geo.Alt;
			const FRotator R = Geo.Neu.Rotator();
			Data.YawDeg = FRotator::ClampAxis(R.Yaw); Data.PitchDeg = R.Pitch; Data.RollDeg = R.Roll;
		}
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

void FCamSimEntityManager::GetThermalStencilEntities(TArray<FThermalStencilEntity>& Out) const
{
	Out.Reset();
	for (const TPair<FEntityKey, uint8>& KV : StencilOf)
	{
		const ACamSimEntity* E = FindEntity(KV.Key);   // null for a pending-kill actor
		if (!E || KV.Value == 0) continue;
		FThermalStencilEntity& T = Out.AddDefaulted_GetRef();
		T.Stencil = KV.Value;
		// ROADMAP 4C: pose, part volumes and the stepped excess temperatures
		const FEntityThermalState& St = E->GetThermalState();
		T.bHasThermalState = St.bInitialized;
		T.OriginWorld      = E->GetActorLocation();
		T.RotationWorld    = E->GetActorQuat();
		T.SkinExcessK      = St.SkinExcessK;
		if (const FEntityTypeEntry* Type = TypeTable ? TypeTable->FindEntry(E->EntityType) : nullptr)
		{
			T.ThermalMaterial = Type->ThermalMaterial;
			T.ThermalOffsetK  = Type->ThermalOffsetK;
			for (int32 K = 0; K < Type->ThermalParts.Num() && K < FEntityThermalSettings::MaxParts; ++K)
			{
				T.Parts.Add(Type->ThermalParts[K]);
				T.PartExcessK[K] = St.PartExcessK[K];
			}
		}
		T.bSurfaceVehicle = E->IsSurfaceVehicle();
	}
}

void FCamSimEntityManager::SetEntityThermalEnv(const FEntityThermalEnv& Env)
{
	if (!ThermalEnv) ThermalEnv = MakeUnique<FEntityThermalEnv>();
	*ThermalEnv = Env;
}

void FCamSimEntityManager::StepEntityThermal(double SimSec)
{
	if (!Subsystem) return;
	const FEntityThermalSettings& Settings = Subsystem->GetConfig().Thermal.Entity;
	if (!Settings.bEnabled) return;
	const bool bEnv = ThermalEnv.IsValid() && ThermalEnv->bValid;
	for (const TPair<FEntityKey, ACamSimEntity*>& KV : EntityMap)
	{
		ACamSimEntity* E = KV.Value;
		if (!IsValid(E)) continue;
		const uint8* Stencil = StencilOf.Find(KV.Key);
		const bool bTagged = bEnv && Stencil && *Stencil != 0;
		E->StepThermal(SimSec, Settings, bTagged, bTagged ? ThermalEnv->TairK : 0.0f, bTagged ? ThermalEnv->BaselineK[*Stencil] : 0.0f);
	}
}

ACamSimEntity* FCamSimEntityManager::FindEntity(const FEntityKey& Key) const
{
	ACamSimEntity* const* Found = EntityMap.Find(Key);
	return (Found && IsValid(*Found)) ? *Found : nullptr;
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
