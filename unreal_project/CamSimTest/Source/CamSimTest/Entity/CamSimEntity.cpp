// Copyright CamSim Contributors. All Rights Reserved.

#include "Entity/CamSimEntity.h"
#include "Entity/EntityTypeTable.h"
#include "Entity/EntityMeshLoader.h"
#include "Entity/SurfaceProbe.h"
#include "Ocean/IOceanSurface.h"
#include "CamSimTest.h"

#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Entity/CamSimAnimInstance.h"
#include "CesiumGlobeAnchorComponent.h"
#include "Geospatial/CigiFrames.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"

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

void ACamSimEntity::SetDamageInterpolation(bool bEnabled, float RateSec)
{
	bDamageInterpolating  = bEnabled;
	DamageInterpolationRate = FMath::Max(0.01f, RateSec);
}

// -------------------------------------------------------------------------
// SetEntityType — load mesh by type ID
// -------------------------------------------------------------------------

void ACamSimEntity::SetEntityType(uint16 Type)
{
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

	UE_LOG(LogCamSim, Log, TEXT("ACamSimEntity[%u]: initialized animated character '%s'"),
		EntityId, *Entry.AssetPath);
}

// -------------------------------------------------------------------------
// ApplyPose — snap position + orientation from CIGI packet
// -------------------------------------------------------------------------

void ACamSimEntity::ApplyCommand(const FEntityCommand& Command)
{
	if (!GlobeAnchor) return;

	if (Command.Motion.IsSet())
	{
		SetMotion(*Command.Motion);
	}

	SurfaceMode = Command.SurfaceMode;

	bAttached = Command.Attachment.IsSet();
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
		// Mesh bounds in model space, times the entry's scale (same fallback as ApplyVesselMotion).
		const FVector Ext = StaticMeshComp->GetStaticMesh()->GetBounds().BoxExtent * StaticMeshComp->GetRelativeScale3D();
		if (OutHalfLengthM <= 0.0) OutHalfLengthM = Ext.X / 100.0;
		if (OutHalfBeamM   <= 0.0) OutHalfBeamM   = Ext.Y / 100.0;
	}
}

void ACamSimEntity::CommitPose(const CamSimFrames::FGeoPose& SenderPose)
{
	CamSimFrames::FGeoPose Pose = SenderPose;
	if (SurfaceMode != ESurfaceMode::None && SurfaceProbe && !bAttached)
	{
		const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
		const double Dt  = LastCommitTimeSec < 0.0 ? 0.0 : FMath::Max(0.0, Now - LastCommitTimeSec);
		LastCommitTimeSec = Now;
		double HalfLength, HalfBeam;
		GetFootprintHalfSizesM(HalfLength, HalfBeam);
		Pose = CamSimSurface::PlaceOnSurface(SurfaceMode, SenderPose, HalfLength, HalfBeam, Dt, *SurfaceProbe, SurfaceState);
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
// ApplyComponentControl — lights, damage state
// -------------------------------------------------------------------------

void ACamSimEntity::ApplyComponent(const FComponentCommand& C)
{
	if (C.ComponentClass != 0) return; // only handle entity-class components

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

	case 10: // Damage state — swap mesh asset (Phase 22C: gradual interpolation)
		{
			const uint8 OldDamageState = DamageState;
			const uint8 NewDamageState = FMath::Min(C.State, static_cast<uint8>(2));
			static const TCHAR* DamageNames[] = { TEXT("intact"), TEXT("damaged"), TEXT("destroyed") };
			UE_LOG(LogCamSim, Log, TEXT("ACamSimEntity[%u]: damage state -> %u (%s)"),
				EntityId, NewDamageState,
				NewDamageState <= 2 ? DamageNames[NewDamageState] : TEXT("unknown"));

			if (bDamageInterpolating && DamageInterpolationRate > 0.0f && OldDamageState != NewDamageState)
			{
				// Gradual damage: start interpolation, defer mesh swap
				TargetDamageState = NewDamageState;
				DamageBlendAlpha  = 0.0f;
			}
			else
			{
				// Immediate swap
				DamageState = NewDamageState;
				TargetDamageState = NewDamageState;

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

	// Phase 22C: Gradual damage blend
	if (bDamageInterpolating && DamageState != TargetDamageState && DamageInterpolationRate > 0.0f)
	{
		DamageBlendAlpha += DeltaTime / DamageInterpolationRate;
		if (DamageBlendAlpha >= 1.0f)
		{
			DamageBlendAlpha = 1.0f;
			DamageState = TargetDamageState;

			// Complete mesh swap
			const FEntityTypeEntry* Entry = TypeTable ? TypeTable->FindEntry(EntityType) : nullptr;
			if (Entry)
			{
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
// ApplyVesselMotion — pitch/roll/heave from ocean surface
// -------------------------------------------------------------------------

void ACamSimEntity::ApplyVesselMotion(IOceanSurface* Ocean,
                                       float HalfLengthCm, float HalfBeamCm,
                                       float MotionScale)
{
	if (!Ocean) return;

	// Resolve half-dimensions: fall back to mesh bounding box if not configured
	if (HalfLengthCm <= 0.0f || HalfBeamCm <= 0.0f)
	{
		UStaticMeshComponent* MC = FindComponentByClass<UStaticMeshComponent>();
		if (!MC || !MC->GetStaticMesh()) return; // mesh not loaded yet — skip this tick

		const FBoxSphereBounds Bounds = MC->GetStaticMesh()->GetBounds();
		if (HalfLengthCm <= 0.0f) HalfLengthCm = Bounds.BoxExtent.X;
		if (HalfBeamCm   <= 0.0f) HalfBeamCm   = Bounds.BoxExtent.Y;
	}

	if (HalfLengthCm <= 0.0f || HalfBeamCm <= 0.0f) return;

	const FVector  Loc     = GetActorLocation();     // UE units, cm
	const FRotator Rot     = GetActorRotation();
	const FVector  Forward = Rot.Vector();
	const FVector  Right   = FRotationMatrix(Rot).GetScaledAxis(EAxis::Y);

	const FVector BowPos   = Loc + Forward * HalfLengthCm;
	const FVector SternPos = Loc - Forward * HalfLengthCm;
	const FVector PortPos  = Loc - Right   * HalfBeamCm;
	const FVector StbdPos  = Loc + Right   * HalfBeamCm;

	// Sample ocean height at each point (UE units, cm)
	const float hBow   = Ocean->GetSurfaceHeightAt(FVector2D(BowPos.X,   BowPos.Y));
	const float hStern = Ocean->GetSurfaceHeightAt(FVector2D(SternPos.X, SternPos.Y));
	const float hPort  = Ocean->GetSurfaceHeightAt(FVector2D(PortPos.X,  PortPos.Y));
	const float hStbd  = Ocean->GetSurfaceHeightAt(FVector2D(StbdPos.X,  StbdPos.Y));

	// All units consistent (cm/cm) — atan2 result in radians
	const float PitchRad = FMath::Atan2(hBow - hStern, HalfLengthCm * 2.0f) * MotionScale;
	const float RollRad  = FMath::Atan2(hStbd - hPort, HalfBeamCm   * 2.0f) * MotionScale;
	const float HeaveZ   = (hBow + hStern + hPort + hStbd) * 0.25f * MotionScale;

	// Pitch/roll the hull about its own axes, on top of the commanded pose.
	// (Adding them to the world FRotator would tilt about UE world axes, which
	// are not the local horizon away from the georeference origin.)
	const FQuat WaveTilt = FRotator(FMath::RadiansToDegrees(PitchRad), 0.0, FMath::RadiansToDegrees(RollRad)).Quaternion();
	SetActorRotation(GetActorQuat() * WaveTilt);

	FVector NewLoc = Loc;
	NewLoc.Z += HeaveZ;
	SetActorLocation(NewLoc);
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
