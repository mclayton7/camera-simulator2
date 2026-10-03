// Copyright CamSim Contributors. All Rights Reserved.

#include "Entity/CamSimEntity.h"
#include "Entity/EntityTypeTable.h"
#include "Entity/EntityMeshLoader.h"
#include "Entity/SurfaceProbe.h"
#include "Hosts/DisCommands.h"       // CamSim::Dis::SurfaceModeFor (IsSurfaceVehicle)
#include "Subsystem/CamSimSubsystem.h"
#include "CamSimTest.h"

#include "Components/MeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Entity/CamSimAnimInstance.h"
#include "CesiumGlobeAnchorComponent.h"
#include "Geospatial/CigiFrames.h"
#include "Geospatial/EcefFrames.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "Engine/GameInstance.h"

// -------------------------------------------------------------------------
// Constructor
// -------------------------------------------------------------------------

ACamSimEntity::ACamSimEntity()
{
	PrimaryActorTick.bCanEverTick = true;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	GlobeAnchor = CreateDefaultSubobject<UCesiumGlobeAnchorComponent>(TEXT("GlobeAnchor"));

	StaticMeshComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("StaticMesh"));
	StaticMeshComp->SetupAttachment(Root);
	StaticMeshComp->SetVisibility(false);
	StaticMeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	StaticMeshComp->CastShadow          = true;  // 24A: explicit shadow casting
	StaticMeshComp->bCastDynamicShadow  = true;

	SkelMeshComp = CreateDefaultSubobject<UPoseableMeshComponent>(TEXT("SkelMesh"));
	SkelMeshComp->SetupAttachment(Root);
	SkelMeshComp->SetVisibility(false);
	SkelMeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SkelMeshComp->CastShadow            = true;  // 24A: explicit shadow casting
	SkelMeshComp->bCastDynamicShadow    = true;

	// Nav lights — created but hidden; enabled via Component Control
	NavLightRed = CreateDefaultSubobject<UPointLightComponent>(TEXT("NavLightRed"));
	NavLightRed->SetupAttachment(Root);
	NavLightRed->SetVisibility(false);
	NavLightRed->SetLightColor(FLinearColor::Red);
	NavLightRed->Intensity = 5000.0f;

	NavLightGreen = CreateDefaultSubobject<UPointLightComponent>(TEXT("NavLightGreen"));
	NavLightGreen->SetupAttachment(Root);
	NavLightGreen->SetVisibility(false);
	NavLightGreen->SetLightColor(FLinearColor::Green);
	NavLightGreen->Intensity = 5000.0f;

	NavLightWhite = CreateDefaultSubobject<UPointLightComponent>(TEXT("NavLightWhite"));
	NavLightWhite->SetupAttachment(Root);
	NavLightWhite->SetVisibility(false);
	NavLightWhite->SetLightColor(FLinearColor::White);
	NavLightWhite->Intensity = 5000.0f;

	StrobeLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("StrobeLight"));
	StrobeLight->SetupAttachment(Root);
	StrobeLight->SetVisibility(false);
	StrobeLight->SetLightColor(FLinearColor::White);
	StrobeLight->Intensity = 20000.0f;

	LandingLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("LandingLight"));
	LandingLight->SetupAttachment(Root);
	LandingLight->SetVisibility(false);
	LandingLight->SetLightColor(FLinearColor::White);
	LandingLight->Intensity = 30000.0f;

	// Phase 22D: Animated character mesh (hidden by default)
	AnimMeshComp = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("AnimMesh"));
	AnimMeshComp->SetupAttachment(Root);
	AnimMeshComp->SetVisibility(false);
	AnimMeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	AnimMeshComp->CastShadow         = true;
	AnimMeshComp->bCastDynamicShadow = true;
}

// -------------------------------------------------------------------------
// BeginPlay
// -------------------------------------------------------------------------

void ACamSimEntity::BeginPlay()
{
	Super::BeginPlay();

	if (UGameInstance* GI = GetGameInstance())
	{
		Subsystem = GI->GetSubsystem<UCamSimSubsystem>();
	}
}

// -------------------------------------------------------------------------
// SetEntityTypeTable — inject dependency before first SetEntityType call
// -------------------------------------------------------------------------

void ACamSimEntity::SetEntityTypeTable(const FEntityTypeTable* Table)
{
	TypeTable = Table;
}

void ACamSimEntity::ApplyScaleControls(float MaxDrawDistanceM, float TickRateHz)
{
	const float MaxDistCm = (MaxDrawDistanceM > 0.0f) ? MaxDrawDistanceM * 100.0f : 0.0f;
	if (StaticMeshComp) StaticMeshComp->LDMaxDrawDistance = MaxDistCm;
	if (SkelMeshComp)   SkelMeshComp->LDMaxDrawDistance = MaxDistCm;
	if (NavLightRed)    NavLightRed->SetMaxDrawDistance(MaxDistCm);
	if (NavLightGreen)  NavLightGreen->SetMaxDrawDistance(MaxDistCm);
	if (NavLightWhite)  NavLightWhite->SetMaxDrawDistance(MaxDistCm);
	if (StrobeLight)    StrobeLight->SetMaxDrawDistance(MaxDistCm);
	if (LandingLight)   LandingLight->SetMaxDrawDistance(MaxDistCm);

	const float TickInterval = (TickRateHz > 0.0f) ? (1.0f / TickRateHz) : 0.0f;
	SetActorTickInterval(TickInterval);
}

// -------------------------------------------------------------------------
// SetShadowCasting — Phase 24A
// -------------------------------------------------------------------------

void ACamSimEntity::SetShadowCasting(bool bCast)
{
	if (StaticMeshComp) { StaticMeshComp->CastShadow = bCast; StaticMeshComp->bCastDynamicShadow = bCast; }
	if (SkelMeshComp)   { SkelMeshComp->CastShadow   = bCast; SkelMeshComp->bCastDynamicShadow   = bCast; }
	if (AnimMeshComp)   { AnimMeshComp->CastShadow    = bCast; AnimMeshComp->bCastDynamicShadow   = bCast; }
}

// -------------------------------------------------------------------------
// SetEntityType — load mesh by type ID
// -------------------------------------------------------------------------

void ACamSimEntity::SetEntityType(uint16 Type)
{
	if (Type != EntityType)   // ROADMAP 4C: other parts, other class: other state and baseline
	{
		ThermalState = FEntityThermalState();
		ThermalEnvLatch = FEntityThermalLatch();
	}
	EntityType = Type;

	const FEntityTypeEntry* Entry = TypeTable ? TypeTable->FindEntry(Type) : nullptr;
	if (!Entry || Entry->AssetPath.IsEmpty())
	{
		UE_LOG(LogCamSim, Warning, TEXT("ACamSimEntity[%u]: no asset for type %u"), EntityId, Type);
		return;
	}

	// Phase 22D: Animated character entities use a separate path
	if (Entry->bAnimated)
	{
		InitAnimatedCharacter(*Entry);
		return;
	}

	// Keep entity invisible until the mesh is resident — avoids the visual
	// artefact where a burst of spawns would otherwise show zero-bounds actors
	// at (0,0,0) while synchronous mesh cook/stream drains the game thread.
	SkelMeshComp->SetVisibility(false);
	StaticMeshComp->SetVisibility(false);

	if (Entry->bSkeletal)
	{
		// Check mesh cache first (Phase 4)
		USkeletalMesh* Mesh = TypeTable ? TypeTable->GetCachedSkeletalMesh(Type) : nullptr;
		if (Mesh)
		{
			ApplyLoadedSkeletalMesh(Mesh, *Entry, Type);
			return;
		}
		// glTF path: must go through the runtime plugin — stays synchronous.
		if (CamSimMeshLoader::IsGltfPath(Entry->AssetPath))
		{
			Mesh = CamSimMeshLoader::LoadSkeletalMesh(Entry->AssetPath);
			if (Mesh && TypeTable) TypeTable->SetCachedSkeletalMesh(Type, Mesh);
			if (Mesh) ApplyLoadedSkeletalMesh(Mesh, *Entry, Type);
			else UE_LOG(LogCamSim, Warning,
				TEXT("ACamSimEntity[%u]: failed to load skeletal glTF '%s'"),
				EntityId, *Entry->AssetPath);
			return;
		}
		// UE content path — async load so a spawn burst doesn't stall the tick.
		RequestAsyncSkeletalMesh(*Entry, Type);
	}
	else
	{
		UStaticMesh* Mesh = TypeTable ? TypeTable->GetCachedStaticMesh(Type) : nullptr;
		if (Mesh)
		{
			ApplyLoadedStaticMesh(Mesh, *Entry, Type);
			return;
		}
		if (CamSimMeshLoader::IsGltfPath(Entry->AssetPath))
		{
			Mesh = CamSimMeshLoader::LoadStaticMesh(Entry->AssetPath);
			if (Mesh && TypeTable) TypeTable->SetCachedStaticMesh(Type, Mesh);
			if (Mesh) ApplyLoadedStaticMesh(Mesh, *Entry, Type);
			else UE_LOG(LogCamSim, Warning,
				TEXT("ACamSimEntity[%u]: failed to load static glTF '%s'"),
				EntityId, *Entry->AssetPath);
			return;
		}
		RequestAsyncStaticMesh(*Entry, Type);
	}
}

// -------------------------------------------------------------------------
// Async mesh loading (UE content paths) — callbacks run on the game thread
// when FStreamableManager finishes streaming the asset in.
// -------------------------------------------------------------------------

void ACamSimEntity::SetGroundTruthStencil(uint8 Value)
{
	GroundTruthStencil = Value;
	// Meshes only: particle systems (smoke, wakes) must not join the vehicle's mask.
	TInlineComponentArray<UMeshComponent*> Meshes(this);
	for (UMeshComponent* M : Meshes)
	{
		M->SetRenderCustomDepth(Value != 0);
		M->SetCustomDepthStencilValue(Value);
	}
}

namespace
{
	/** A mesh slot that draws something: static/skinned meshes need their asset; other mesh types (procedural) count. */
	bool HasMeshAsset(const UMeshComponent* M)
	{
		if (const UStaticMeshComponent* S = Cast<UStaticMeshComponent>(M)) return S->GetStaticMesh() != nullptr;
		if (const USkinnedMeshComponent* K = Cast<USkinnedMeshComponent>(M)) return K->GetSkinnedAsset() != nullptr;
		return true;
	}
}

FBox ACamSimEntity::ComputeMeshLocalBox(const AActor& Actor)
{
	FBox Box(ForceInit);
	const FTransform ActorToWorld = Actor.GetActorTransform();
	TInlineComponentArray<UMeshComponent*> Meshes(&Actor);   // the set SetGroundTruthStencil tags
	for (const UMeshComponent* M : Meshes)
	{
		if (!M || !M->IsRegistered() || !M->IsVisible() || M->bHiddenInGame || !HasMeshAsset(M)) continue;
		// Component -> actor through any nesting (the component's world transform relative to the actor's).
		Box += M->CalcBounds(M->GetComponentTransform().GetRelativeTransform(ActorToWorld)).GetBox();
	}
	return Box;
}

void ACamSimEntity::ApplyLoadedSkeletalMesh(USkeletalMesh* Mesh,
                                             const FEntityTypeEntry& Entry,
                                             uint16 Type)
{
	if (!Mesh || !SkelMeshComp) return;
	SkelMeshComp->SetSkinnedAsset(Mesh);
	SkelMeshComp->SetRelativeRotation(Entry.ModelRotation);
	SkelMeshComp->SetRelativeScale3D(FVector(Entry.ModelScale));
	SkelMeshComp->SetRelativeLocation(FVector(0.0, 0.0, Entry.ModelZOffsetCm));
	SkelMeshComp->SetVisibility(true);
	StaticMeshComp->SetVisibility(false);
	SetGroundTruthStencil(GroundTruthStencil);
	UE_LOG(LogCamSim, Log,
		TEXT("ACamSimEntity[%u]: loaded skeletal mesh '%s' (type %u, scale=%.3f)"),
		EntityId, *Entry.AssetPath, Type, Entry.ModelScale);
}

void ACamSimEntity::ApplyLoadedStaticMesh(UStaticMesh* Mesh,
                                           const FEntityTypeEntry& Entry,
                                           uint16 Type)
{
	if (!Mesh || !StaticMeshComp) return;
	StaticMeshComp->SetStaticMesh(Mesh);
	StaticMeshComp->SetRelativeRotation(Entry.ModelRotation);
	StaticMeshComp->SetRelativeScale3D(FVector(Entry.ModelScale));
	StaticMeshComp->SetRelativeLocation(FVector(0.0, 0.0, Entry.ModelZOffsetCm));
	StaticMeshComp->SetVisibility(true);
	SkelMeshComp->SetVisibility(false);
	SetGroundTruthStencil(GroundTruthStencil);
	UE_LOG(LogCamSim, Log,
		TEXT("ACamSimEntity[%u]: loaded static mesh '%s' (type %u, scale=%.3f)"),
		EntityId, *Entry.AssetPath, Type, Entry.ModelScale);
}

void ACamSimEntity::RequestAsyncSkeletalMesh(const FEntityTypeEntry& Entry, uint16 Type)
{
	const FSoftObjectPath SoftPath(Entry.AssetPath);
	if (!SoftPath.IsValid())
	{
		UE_LOG(LogCamSim, Warning,
			TEXT("ACamSimEntity[%u]: invalid skeletal mesh path '%s'"),
			EntityId, *Entry.AssetPath);
		return;
	}
	const uint16 PendingType = Type;
	const FEntityTypeEntry EntryCopy = Entry;  // callback runs later; capture by value
	FStreamableManager& Mgr = UAssetManager::GetStreamableManager();
	PendingMeshHandle_ = Mgr.RequestAsyncLoad(
		SoftPath,
		FStreamableDelegate::CreateWeakLambda(this, [this, SoftPath, EntryCopy, PendingType]()
		{
			USkeletalMesh* Mesh = Cast<USkeletalMesh>(SoftPath.ResolveObject());
			if (Mesh && TypeTable) TypeTable->SetCachedSkeletalMesh(PendingType, Mesh);
			if (Mesh) ApplyLoadedSkeletalMesh(Mesh, EntryCopy, PendingType);
			else UE_LOG(LogCamSim, Warning,
				TEXT("ACamSimEntity[%u]: async skeletal load failed for '%s'"),
				EntityId, *EntryCopy.AssetPath);
		}),
		FStreamableManager::AsyncLoadHighPriority);
}

void ACamSimEntity::RequestAsyncStaticMesh(const FEntityTypeEntry& Entry, uint16 Type)
{
	const FSoftObjectPath SoftPath(Entry.AssetPath);
	if (!SoftPath.IsValid())
	{
		UE_LOG(LogCamSim, Warning,
			TEXT("ACamSimEntity[%u]: invalid static mesh path '%s'"),
			EntityId, *Entry.AssetPath);
		return;
	}
	const uint16 PendingType = Type;
	const FEntityTypeEntry EntryCopy = Entry;
	FStreamableManager& Mgr = UAssetManager::GetStreamableManager();
	PendingMeshHandle_ = Mgr.RequestAsyncLoad(
		SoftPath,
		FStreamableDelegate::CreateWeakLambda(this, [this, SoftPath, EntryCopy, PendingType]()
		{
			UStaticMesh* Mesh = Cast<UStaticMesh>(SoftPath.ResolveObject());
			if (Mesh && TypeTable) TypeTable->SetCachedStaticMesh(PendingType, Mesh);
			if (Mesh) ApplyLoadedStaticMesh(Mesh, EntryCopy, PendingType);
			else UE_LOG(LogCamSim, Warning,
				TEXT("ACamSimEntity[%u]: async static load failed for '%s'"),
				EntityId, *EntryCopy.AssetPath);
		}),
		FStreamableManager::AsyncLoadHighPriority);
}

// -------------------------------------------------------------------------
// InitAnimatedCharacter — Phase 22D
// -------------------------------------------------------------------------

void ACamSimEntity::InitAnimatedCharacter(const FEntityTypeEntry& Entry)
{
	if (!AnimMeshComp) return;

	// Load skeletal mesh (same path resolution as SetEntityType)
	USkeletalMesh* Mesh = CamSimMeshLoader::LoadSkeletalMesh(Entry.AssetPath);
	if (!Mesh)
	{
		UE_LOG(LogCamSim, Warning, TEXT("ACamSimEntity[%u]: failed to load animated skeletal mesh '%s'"),
			EntityId, *Entry.AssetPath);
		return;
	}

	AnimMeshComp->SetSkeletalMesh(Mesh);
	AnimMeshComp->SetRelativeRotation(Entry.ModelRotation);
	AnimMeshComp->SetRelativeScale3D(FVector(Entry.ModelScale));
	AnimMeshComp->SetRelativeLocation(FVector(0.0, 0.0, Entry.ModelZOffsetCm));

	// Load AnimBlueprint class
	if (!Entry.AnimBlueprintPath.IsEmpty())
	{
		UClass* AnimBPClass = FSoftClassPath(Entry.AnimBlueprintPath).TryLoadClass<UAnimInstance>();
		if (AnimBPClass)
		{
			AnimMeshComp->SetAnimInstanceClass(AnimBPClass);
		}
		else
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("ACamSimEntity[%u]: failed to load AnimBlueprint '%s'"),
				EntityId, *Entry.AnimBlueprintPath);
		}
	}

	// Show AnimMeshComp, hide StaticMesh + PoseableMesh
	AnimMeshComp->SetVisibility(true);
	StaticMeshComp->SetVisibility(false);
	SkelMeshComp->SetVisibility(false);
	SetGroundTruthStencil(GroundTruthStencil);

	UE_LOG(LogCamSim, Log, TEXT("ACamSimEntity[%u]: initialized animated character '%s'"),
		EntityId, *Entry.AssetPath);
}

// -------------------------------------------------------------------------
// ApplyPose — snap position + orientation from CIGI packet
// -------------------------------------------------------------------------

bool ACamSimEntity::IsSurfaceVehicleClass(const FEntityClassification& Class, const FString& EntityCategory)
{
	if (CamSim::Dis::SurfaceModeFor(Class.Kind, Class.Domain, /*bClampToSurface=*/true) != ESurfaceMode::None) return true;
	for (const TCHAR* Category : { TEXT("truck"), TEXT("boat"), TEXT("ground"), TEXT("sea") })
	{
		if (EntityCategory.Equals(Category, ESearchCase::IgnoreCase)) return true;
	}
	return false;
}

bool ACamSimEntity::IsSurfaceVehicle() const
{
	const FEntityTypeEntry* Entry = TypeTable ? TypeTable->FindEntry(EntityType) : nullptr;
	return IsSurfaceVehicleClass(Classification, Entry ? Entry->EntityCategory : FString());
}

void ACamSimEntity::ApplyCommand(const FEntityCommand& Command)
{
	if (!GlobeAnchor) return;

	if (Command.Motion.IsSet())
	{
		SetMotion(*Command.Motion);
	}

	if (Command.Classification.Kind != 0 || Command.Classification.Domain != 0 || Command.Classification.Category != 0)
	{
		Classification = Command.Classification;
	}

	const bool bNowAttached = Command.Attachment.IsSet();
	if (Command.SurfaceMode != SurfaceMode || bNowAttached != bAttached)
	{
		ResetSurfacePlacement();
	}
	SurfaceMode = Command.SurfaceMode;

	bAttached = bNowAttached;
	if (bAttached)
	{
		// The manager resolves the pose from the parent each tick.
		AttachParent    = Command.Attachment->Parent;
		AttachOffsetFrd = Command.Attachment->OffsetFrd;
		AttachRotation  = Command.Attachment->Rotation;
		return;
	}

	ApplyGeoPose(Command.Pose);

	// One-time log to confirm the entity reached a real UE world position.
	// (If this prints 0,0,0 the GlobeAnchor has not found a CesiumGeoreference.)
	if (!bPoseLogged)
	{
		bPoseLogged = true;
		UE_LOG(LogCamSim, Log,
			TEXT("ACamSimEntity[%u]: first pose lat=%.4f lon=%.4f alt=%.0f -> UE world %s"),
			EntityId, Command.Pose.Lat, Command.Pose.Lon, Command.Pose.Alt,
			*GetActorLocation().ToString());
	}
}

void ACamSimEntity::ApplyGeoPose(const CamSimFrames::FGeoPose& Pose)
{
	if (!GlobeAnchor) return;

	// Update DR base so dead-reckoning doesn't drift after a new packet
	DR.Lat = Pose.Lat;
	DR.Lon = Pose.Lon;
	DR.Alt = static_cast<float>(Pose.Alt);
	DR.Orientation = Pose.Neu;

	CommitPose(Pose);
}

void ACamSimEntity::GetFootprintHalfSizesM(double& OutHalfLengthM, double& OutHalfBeamM) const
{
	OutHalfLengthM = OutHalfBeamM = 0.0;
	if (const FEntityTypeEntry* Entry = TypeTable ? TypeTable->FindEntry(EntityType) : nullptr)
	{
		OutHalfLengthM = Entry->HalfLengthCm / 100.0;
		OutHalfBeamM   = Entry->HalfBeamCm / 100.0;
	}
	if ((OutHalfLengthM <= 0.0 || OutHalfBeamM <= 0.0) && StaticMeshComp && StaticMeshComp->GetStaticMesh())
	{
		// Mesh bounds in model space, times the entry's scale.
		const FVector Ext = StaticMeshComp->GetStaticMesh()->GetBounds().BoxExtent * StaticMeshComp->GetRelativeScale3D();
		if (OutHalfLengthM <= 0.0) OutHalfLengthM = Ext.X / 100.0;
		if (OutHalfBeamM   <= 0.0) OutHalfBeamM   = Ext.Y / 100.0;
	}
}

void ACamSimEntity::ResetSurfacePlacement()
{
	SurfaceState      = CamSimSurface::FClampState();
	LastCommitTimeSec = -1.0;
}

void ACamSimEntity::CommitPose(const CamSimFrames::FGeoPose& SenderPose)
{
	CamSimFrames::FGeoPose Pose = SenderPose;
	if (SurfaceMode != ESurfaceMode::None && SurfaceProbe && !bAttached)
	{
		if (LastCommitTimeSec >= 0.0 && CamSimSurface::IsHorizontalJump(LastCommitSender, SenderPose))
		{
			ResetSurfacePlacement();   // teleport: the old height/span says nothing about here
		}
		const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
		const double Dt  = LastCommitTimeSec < 0.0 ? 0.0 : FMath::Max(0.0, Now - LastCommitTimeSec);
		LastCommitTimeSec = Now;
		LastCommitSender  = SenderPose;
		double HalfLength, HalfBeam;
		GetFootprintHalfSizesM(HalfLength, HalfBeam);

		CamSimSurface::FWaterInput Water;
		if (UCamSimSubsystem* Sub = GetCamSimSubsystem())
		{
			Water.Ocean       = Sub->GetOceanSurface();
			Water.bMotion     = Sub->GetConfig().Ocean.bVesselMotion;
			Water.MotionScale = Sub->GetConfig().Ocean.VesselMotionScale;
		}
		Pose = CamSimSurface::PlaceOnSurface(SurfaceMode, SenderPose, HalfLength, HalfBeam, Dt, *SurfaceProbe, SurfaceState, Water);
	}
	GlobeAnchor->MoveToLongitudeLatitudeHeight(FVector(Pose.Lon, Pose.Lat, Pose.Alt));
	GlobeAnchor->SetEastSouthUpRotation(CamSimFrames::NeuToEastSouthUp(Pose.Neu));
}

bool ACamSimEntity::GetGeoPose(CamSimFrames::FGeoPose& OutPose) const
{
	if (!GlobeAnchor) return false;
	const FVector Llh = GlobeAnchor->GetLongitudeLatitudeHeight();
	OutPose.Lon = Llh.X;
	OutPose.Lat = Llh.Y;
	OutPose.Alt = Llh.Z;
	OutPose.Neu = CamSimFrames::EastSouthUpToNeu(GlobeAnchor->GetEastSouthUpRotation());
	return true;
}

// -------------------------------------------------------------------------
// SetRateControl — store rates for dead-reckoning
// -------------------------------------------------------------------------

void ACamSimEntity::SetMotion(const FMotionModel& Motion)
{
	DR.Motion     = Motion;
	DR.bHasMotion = true;
}

// -------------------------------------------------------------------------
// ApplyArtPart — set bone transform on skeletal mesh
// -------------------------------------------------------------------------

void ACamSimEntity::ApplyArticulation(const FArticulationCommand& P)
{
	if (!P.bEnabled || !SkelMeshComp || !SkelMeshComp->GetSkinnedAsset()) return;

	// Bone naming: ArtPart_XX where XX is zero-padded decimal ArtPartId
	FName BoneName = FName(*FString::Printf(TEXT("ArtPart_%02d"), P.PartId));

	int32 BoneIdx = SkelMeshComp->GetBoneIndex(BoneName);
	if (BoneIdx == INDEX_NONE) return;

	// UPoseableMeshComponent uses ComponentSpace; read current transform to preserve
	// DOFs that are not enabled in this packet.
	FTransform BoneTM = SkelMeshComp->GetBoneTransformByName(BoneName, EBoneSpaces::ComponentSpace);
	FVector   Loc = BoneTM.GetLocation();
	FRotator  Rot = BoneTM.GetRotation().Rotator();

	if (P.bXEn)     Loc.X     = P.Offset.X;
	if (P.bYEn)     Loc.Y     = P.Offset.Y;
	if (P.bZEn)     Loc.Z     = P.Offset.Z;
	if (P.bRollEn)  Rot.Roll  = P.Rotation.Roll;
	if (P.bPitchEn) Rot.Pitch = P.Rotation.Pitch;
	if (P.bYawEn)   Rot.Yaw   = P.Rotation.Yaw;

	FTransform NewTM(Rot, Loc);
	SkelMeshComp->SetBoneTransformByName(BoneName, NewTM, EBoneSpaces::ComponentSpace);
}

// -------------------------------------------------------------------------
// StepThermal — entity thermal state (ROADMAP 4C)
// -------------------------------------------------------------------------

void ACamSimEntity::StepThermal(double SimSec, const FEntityThermalSettings& Settings)
{
	CamSimFrames::FGeoPose Pose;
	float SpeedMps = ThermalSpeed.GetSpeedMps();
	if (GetGeoPose(Pose))
	{
		SpeedMps = ThermalSpeed.Update(CamSimFrames::GeodeticToEcef(Pose.Lat, Pose.Lon, Pose.Alt), SimSec);
	}
	if (CamSimEntityThermal::ShouldDeferFirstStep(ThermalState, ThermalSpeed)) return;   // spawn at the real state's targets
	FEntityThermalInputs In = CamSimEntityThermal::InputsFromLatch(ThermalEnvLatch, SimSec);
	In.Cmd = ThermalCmd;
	In.Cmd.Damage = DamageState;   // Component Control 10 sets both; DamageState is the one the mesh swap uses
	In.SpeedMps = SpeedMps;
	const FEntityTypeEntry* Entry = TypeTable ? TypeTable->FindEntry(EntityType) : nullptr;
	const TConstArrayView<FEntityThermalPartSpec> Parts = Entry ? TConstArrayView<FEntityThermalPartSpec>(Entry->ThermalParts)
		: TConstArrayView<FEntityThermalPartSpec>();
	CamSimEntityThermal::Step(ThermalState, In, Settings, Parts);
}

// -------------------------------------------------------------------------
// ApplyComponentControl — lights, damage state
// -------------------------------------------------------------------------

void ACamSimEntity::ApplyComponent(const FComponentCommand& C)
{
	if (C.ComponentClass != 0) return; // only handle entity-class components

	// ROADMAP 4C: 10 damage, 11 power plant, 12 flaming feed the thermal state (10 also swaps meshes below).
	CamSimEntityThermal::ApplyComponent(ThermalCmd, C.ComponentId, C.State);

	switch (C.ComponentId)
	{
	case 0: // Nav lights (red/green/white)
		{
			const bool bOn = (C.State == 1);
			if (NavLightRed)   NavLightRed->SetVisibility(bOn);
			if (NavLightGreen) NavLightGreen->SetVisibility(bOn);
			if (NavLightWhite) NavLightWhite->SetVisibility(bOn);
			UE_LOG(LogCamSim, Log, TEXT("ACamSimEntity[%u]: nav lights %s"),
				EntityId, bOn ? TEXT("ON") : TEXT("OFF"));
		}
		break;

	case 1: // Anti-collision strobe
		bStrobeEnabled = (C.State == 1);
		if (!bStrobeEnabled && StrobeLight)
		{
			StrobeLight->SetVisibility(false);
		}
		UE_LOG(LogCamSim, Log, TEXT("ACamSimEntity[%u]: strobe %s"),
			EntityId, bStrobeEnabled ? TEXT("ON") : TEXT("OFF"));
		break;

	case 2: // Landing lights
		{
			const bool bOn = (C.State == 1);
			if (LandingLight) LandingLight->SetVisibility(bOn);
			UE_LOG(LogCamSim, Log, TEXT("ACamSimEntity[%u]: landing lights %s"),
				EntityId, bOn ? TEXT("ON") : TEXT("OFF"));
		}
		break;

	case 10: // Damage state — swap mesh asset
		{
			DamageState = FMath::Min(C.State, static_cast<uint8>(2));
			static const TCHAR* DamageNames[] = { TEXT("intact"), TEXT("damaged"), TEXT("destroyed") };
			UE_LOG(LogCamSim, Log, TEXT("ACamSimEntity[%u]: damage state -> %u (%s)"),
				EntityId, DamageState, DamageNames[DamageState]);

			const FEntityTypeEntry* Entry = TypeTable ? TypeTable->FindEntry(EntityType) : nullptr;
			if (!Entry) break;

			FString AssetPath;
			if (DamageState == 1 && !Entry->DamagedAssetPath.IsEmpty())
				AssetPath = Entry->DamagedAssetPath;
			else if (DamageState == 2 && !Entry->DestroyedAssetPath.IsEmpty())
				AssetPath = Entry->DestroyedAssetPath;
			else
				AssetPath = Entry->AssetPath;

			if (Entry->bSkeletal)
			{
				USkeletalMesh* Mesh = CamSimMeshLoader::LoadSkeletalMesh(AssetPath);
				if (Mesh) SkelMeshComp->SetSkinnedAsset(Mesh);
			}
			else
			{
				UStaticMesh* Mesh = CamSimMeshLoader::LoadStaticMesh(AssetPath);
				if (Mesh) StaticMeshComp->SetStaticMesh(Mesh);
			}
		}
		break;

	case 20: // Phase 22D: Character stance override (3=Crouch, 4=Prone)
		if (AnimMeshComp && AnimMeshComp->IsVisible())
		{
			if (UCamSimAnimInstance* Anim = Cast<UCamSimAnimInstance>(AnimMeshComp->GetAnimInstance()))
			{
				Anim->AnimStateIndex = FMath::Clamp(static_cast<int32>(C.State), 0, 4);
				Anim->bManualState = (C.State >= 3); // manual for crouch/prone
				UE_LOG(LogCamSim, Log, TEXT("ACamSimEntity[%u]: anim stance -> %d"),
					EntityId, Anim->AnimStateIndex);
			}
		}
		break;

	case 11: // Power plant (ROADMAP 4C, thermal only)
	case 12: // Flaming (ROADMAP 4C, thermal only)
		UE_LOG(LogCamSim, Log, TEXT("ACamSimEntity[%u]: %s %s"), EntityId,
			C.ComponentId == 11 ? TEXT("power plant") : TEXT("flaming"), C.State != 0 ? TEXT("ON") : TEXT("OFF"));
		break;

	default:
		break;
	}
}

// -------------------------------------------------------------------------
// Tick — dead-reckoning + strobe
// -------------------------------------------------------------------------

void ACamSimEntity::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	UpdateDeadReckoning(DeltaTime);

	// A water entity with no motion model still rides the waves (ROADMAP 2.6).
	if (SurfaceMode == ESurfaceMode::Water && !DR.bHasMotion && !bAttached && LastCommitTimeSec >= 0.0)
	{
		if (UCamSimSubsystem* Sub = GetCamSimSubsystem(); Sub && Sub->GetOceanSurface())
		{
			CommitPose(LastCommitSender);
		}
	}

	// Phase 22D: Push ground speed to animation instance
	if (AnimMeshComp && AnimMeshComp->IsVisible())
	{
		CachedGroundSpeed = static_cast<float>(DR.Motion.Velocity.Size2D());
		if (UCamSimAnimInstance* Anim = Cast<UCamSimAnimInstance>(AnimMeshComp->GetAnimInstance()))
		{
			Anim->GroundSpeed = CachedGroundSpeed;
		}
	}

	// 1Hz strobe with 50% duty cycle
	if (bStrobeEnabled && StrobeLight)
	{
		StrobeAccum += DeltaTime;
		if (StrobeAccum >= 1.0f) StrobeAccum -= 1.0f;
		StrobeLight->SetVisibility(StrobeAccum < 0.5f);
	}
}

// -------------------------------------------------------------------------
// UpdateDeadReckoning (private)
// -------------------------------------------------------------------------

void ACamSimEntity::UpdateDeadReckoning(float Dt)
{
	if (!DR.bHasMotion || !GlobeAnchor || bAttached) return;  // children follow their parent

	CamSimFrames::FGeoPose Pose;
	Pose.Lat = DR.Lat;
	Pose.Lon = DR.Lon;
	Pose.Alt = DR.Alt;
	Pose.Neu = DR.Orientation;
	CamSimFrames::IntegrateRates(Pose, DR.Motion.Velocity, DR.Motion.AngularRate,
		DR.Motion.LinearFrame == FMotionModel::EFrame::Body, DR.Motion.AngularFrame == FMotionModel::EFrame::Body, Dt);

	DR.Lat = Pose.Lat;
	DR.Lon = Pose.Lon;
	DR.Alt = static_cast<float>(Pose.Alt);
	DR.Orientation = Pose.Neu;

	CommitPose(Pose);
}
