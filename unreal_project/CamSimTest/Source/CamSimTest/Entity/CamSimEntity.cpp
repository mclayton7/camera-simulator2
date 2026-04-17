// Copyright CamSim Contributors. All Rights Reserved.

#include "Entity/CamSimEntity.h"
#include "Entity/EntityTypeTable.h"
#include "Ocean/IOceanSurface.h"
#include "CamSimTest.h"

#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Entity/CamSimAnimInstance.h"
#include "CesiumGlobeAnchorComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "glTFRuntimeFunctionLibrary.h"
#include "glTFRuntimeAsset.h"

// -------------------------------------------------------------------------
// Mesh loading helpers (glTF or UE content asset)
// -------------------------------------------------------------------------

namespace
{
	// Returns true for .gltf / .glb file paths (relative or absolute)
	bool IsGltfPath(const FString& Path)
	{
		return Path.EndsWith(TEXT(".gltf"), ESearchCase::IgnoreCase) ||
		       Path.EndsWith(TEXT(".glb"),  ESearchCase::IgnoreCase);
	}

	// Resolve a config-relative glTF path to an absolute filesystem path.
	// Config paths are relative to {repo_root}/entities/.
	// FPaths::ProjectDir() is {repo_root}/unreal_project/CamSimTest/.
	FString ResolveGltfPath(const FString& RelPath)
	{
		FString Base = FPaths::Combine(FPaths::ProjectDir(), TEXT("../../entities/"));
		return FPaths::ConvertRelativePathToFull(Base + RelPath);
	}

	UStaticMesh* LoadStaticMeshFromPath(const FString& Path)
	{
		if (IsGltfPath(Path))
		{
			FString AbsPath = ResolveGltfPath(Path);
			UglTFRuntimeAsset* Asset = UglTFRuntimeFunctionLibrary::glTFLoadAssetFromFilename(
				AbsPath, false, FglTFRuntimeConfig());
			if (!Asset) return nullptr;
			return Asset->LoadStaticMeshRecursive(TEXT(""), {}, FglTFRuntimeStaticMeshConfig());
		}
		return Cast<UStaticMesh>(FSoftObjectPath(Path).TryLoad());
	}

	USkeletalMesh* LoadSkeletalMeshFromPath(const FString& Path)
	{
		if (IsGltfPath(Path))
		{
			FString AbsPath = ResolveGltfPath(Path);
			UglTFRuntimeAsset* Asset = UglTFRuntimeFunctionLibrary::glTFLoadAssetFromFilename(
				AbsPath, false, FglTFRuntimeConfig());
			if (!Asset) return nullptr;
			return Asset->LoadSkeletalMeshRecursive(TEXT(""), {}, FglTFRuntimeSkeletalMeshConfig());
		}
		return Cast<USkeletalMesh>(FSoftObjectPath(Path).TryLoad());
	}
} // namespace

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
		if (IsGltfPath(Entry->AssetPath))
		{
			Mesh = LoadSkeletalMeshFromPath(Entry->AssetPath);
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
		if (IsGltfPath(Entry->AssetPath))
		{
			Mesh = LoadStaticMeshFromPath(Entry->AssetPath);
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
	USkeletalMesh* Mesh = LoadSkeletalMeshFromPath(Entry.AssetPath);
	if (!Mesh)
	{
		UE_LOG(LogCamSim, Warning, TEXT("ACamSimEntity[%u]: failed to load animated skeletal mesh '%s'"),
			EntityId, *Entry.AssetPath);
		return;
	}

	AnimMeshComp->SetSkeletalMesh(Mesh);
	AnimMeshComp->SetRelativeRotation(Entry.ModelRotation);
	AnimMeshComp->SetRelativeScale3D(FVector(Entry.ModelScale));

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

void ACamSimEntity::ApplyPose(const FCigiEntityState& S)
{
	if (!GlobeAnchor) return;

	// Update DR base so dead-reckoning doesn't drift after a new packet
	DR.Lat = S.Latitude;
	DR.Lon = S.Longitude;
	DR.Alt = S.Altitude;
	const FRotator PoseRot(S.Pitch, S.Yaw, S.Roll);
	DR.Orientation = PoseRot.Quaternion();

	GlobeAnchor->MoveToLongitudeLatitudeHeight(
		FVector(S.Longitude, S.Latitude, S.Altitude));
	SetActorRotation(PoseRot);

	// One-time log to confirm the entity reached a real UE world position.
	// (If this prints 0,0,0 the GlobeAnchor has not found a CesiumGeoreference.)
	if (!bPoseLogged)
	{
		bPoseLogged = true;
		UE_LOG(LogCamSim, Log,
			TEXT("ACamSimEntity[%u]: first pose lat=%.4f lon=%.4f alt=%.0f -> UE world %s"),
			EntityId, S.Latitude, S.Longitude, S.Altitude,
			*GetActorLocation().ToString());
	}
}

// -------------------------------------------------------------------------
// SetRateControl — store rates for dead-reckoning
// -------------------------------------------------------------------------

void ACamSimEntity::SetRateControl(const FCigiRateControl& R)
{
	if (R.bApplyToArtPart) return; // art-part rates handled separately

	DR.XRate     = R.XRate;
	DR.YRate     = R.YRate;
	DR.ZRate     = R.ZRate;
	DR.YawRate   = R.YawRate;
	DR.PitchRate = R.PitchRate;
	DR.RollRate  = R.RollRate;
	DR.bHasRate  = true;
}

// -------------------------------------------------------------------------
// ApplyArtPart — set bone transform on skeletal mesh
// -------------------------------------------------------------------------

void ACamSimEntity::ApplyArtPart(const FCigiArtPartControl& P)
{
	if (!P.bArtPartEn || !SkelMeshComp || !SkelMeshComp->GetSkinnedAsset()) return;

	// Bone naming: ArtPart_XX where XX is zero-padded decimal ArtPartId
	FName BoneName = FName(*FString::Printf(TEXT("ArtPart_%02d"), P.ArtPartId));

	int32 BoneIdx = SkelMeshComp->GetBoneIndex(BoneName);
	if (BoneIdx == INDEX_NONE) return;

	// UPoseableMeshComponent uses ComponentSpace; read current transform to preserve
	// DOFs that are not enabled in this packet.
	FTransform BoneTM = SkelMeshComp->GetBoneTransformByName(BoneName, EBoneSpaces::ComponentSpace);
	FVector   Loc = BoneTM.GetLocation();
	FRotator  Rot = BoneTM.GetRotation().Rotator();

	if (P.bXOffEn)  Loc.X     = P.XOff;
	if (P.bYOffEn)  Loc.Y     = P.YOff;
	if (P.bZOffEn)  Loc.Z     = P.ZOff;
	if (P.bRollEn)  Rot.Roll  = P.Roll;
	if (P.bPitchEn) Rot.Pitch = P.Pitch;
	if (P.bYawEn)   Rot.Yaw   = P.Yaw;

	FTransform NewTM(Rot, Loc);
	SkelMeshComp->SetBoneTransformByName(BoneName, NewTM, EBoneSpaces::ComponentSpace);
}

// -------------------------------------------------------------------------
// ApplyComponentControl — lights, damage state
// -------------------------------------------------------------------------

void ACamSimEntity::ApplyComponentControl(const FCigiComponentControl& C)
{
	if (C.CompClass != 0) return; // only handle entity-class components

	switch (C.CompId)
	{
	case 0: // Nav lights (red/green/white)
		{
			const bool bOn = (C.CompState == 1);
			if (NavLightRed)   NavLightRed->SetVisibility(bOn);
			if (NavLightGreen) NavLightGreen->SetVisibility(bOn);
			if (NavLightWhite) NavLightWhite->SetVisibility(bOn);
			UE_LOG(LogCamSim, Log, TEXT("ACamSimEntity[%u]: nav lights %s"),
				EntityId, bOn ? TEXT("ON") : TEXT("OFF"));
		}
		break;

	case 1: // Anti-collision strobe
		bStrobeEnabled = (C.CompState == 1);
		if (!bStrobeEnabled && StrobeLight)
		{
			StrobeLight->SetVisibility(false);
		}
		UE_LOG(LogCamSim, Log, TEXT("ACamSimEntity[%u]: strobe %s"),
			EntityId, bStrobeEnabled ? TEXT("ON") : TEXT("OFF"));
		break;

	case 2: // Landing lights
		{
			const bool bOn = (C.CompState == 1);
			if (LandingLight) LandingLight->SetVisibility(bOn);
			UE_LOG(LogCamSim, Log, TEXT("ACamSimEntity[%u]: landing lights %s"),
				EntityId, bOn ? TEXT("ON") : TEXT("OFF"));
		}
		break;

	case 10: // Damage state — swap mesh asset (Phase 22C: gradual interpolation)
		{
			const uint8 OldDamageState = DamageState;
			const uint8 NewDamageState = FMath::Min(C.CompState, static_cast<uint8>(2));
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
					USkeletalMesh* Mesh = LoadSkeletalMeshFromPath(AssetPath);
					if (Mesh) SkelMeshComp->SetSkinnedAsset(Mesh);
				}
				else
				{
					UStaticMesh* Mesh = LoadStaticMeshFromPath(AssetPath);
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
				Anim->AnimStateIndex = FMath::Clamp(static_cast<int32>(C.CompState), 0, 4);
				Anim->bManualState = (C.CompState >= 3); // manual for crouch/prone
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
					USkeletalMesh* Mesh = LoadSkeletalMeshFromPath(AssetPath);
					if (Mesh) SkelMeshComp->SetSkinnedAsset(Mesh);
				}
				else
				{
					UStaticMesh* Mesh = LoadStaticMeshFromPath(AssetPath);
					if (Mesh) StaticMeshComp->SetStaticMesh(Mesh);
				}
			}
		}
	}

	// Phase 22D: Push ground speed to animation instance
	if (AnimMeshComp && AnimMeshComp->IsVisible())
	{
		CachedGroundSpeed = FMath::Sqrt(DR.XRate * DR.XRate + DR.YRate * DR.YRate);
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

	// Apply as delta on top of current CIGI-commanded pose
	FRotator NewRot = Rot;
	NewRot.Pitch += FMath::RadiansToDegrees(PitchRad);
	NewRot.Roll  += FMath::RadiansToDegrees(RollRad);
	SetActorRotation(NewRot);

	FVector NewLoc = Loc;
	NewLoc.Z += HeaveZ;
	SetActorLocation(NewLoc);
}

// -------------------------------------------------------------------------
// UpdateDeadReckoning (private)
// -------------------------------------------------------------------------

void ACamSimEntity::UpdateDeadReckoning(float Dt)
{
	if (!DR.bHasRate || !GlobeAnchor) return;

	// Integrate body-frame angular velocity directly on the quaternion.
	// Omega axes match UE's FRotator convention: X=Roll, Y=Pitch, Z=Yaw.
	// Building the delta quaternion from an axis-angle pair (rather than from
	// a FRotator) dodges the FRotator→FQuat gimbal-lock singularity at
	// pitch = ±90° that would otherwise produce discontinuous heading.
	const FVector Omega(
		FMath::DegreesToRadians(DR.RollRate),
		FMath::DegreesToRadians(DR.PitchRate),
		FMath::DegreesToRadians(DR.YawRate));
	const float OmegaMag = Omega.Size();
	const FQuat DeltaRot = (OmegaMag > SMALL_NUMBER)
		? FQuat(Omega / OmegaMag, OmegaMag * Dt)
		: FQuat::Identity;
	DR.Orientation = (DR.Orientation * DeltaRot).GetNormalized();

	// Body → NED linear velocity: X=forward(North), Y=right(East), Z=down
	const FVector WorldVel =
		DR.Orientation.RotateVector(FVector(DR.XRate, DR.YRate, DR.ZRate));

	// Integrate position in WGS-84 (linear approximation valid for small Dt)
	const double NewLat = DR.Lat + (WorldVel.X * Dt) / 111320.0;
	const double NewLon = DR.Lon + (WorldVel.Y * Dt) /
		(111320.0 * FMath::Cos(DR.Lat * PI / 180.0));
	const float  NewAlt = DR.Alt - WorldVel.Z * Dt;  // CIGI Z=down, alt increases upward

	DR.Lat = NewLat;
	DR.Lon = NewLon;
	DR.Alt = NewAlt;

	GlobeAnchor->MoveToLongitudeLatitudeHeight(FVector(NewLon, NewLat, NewAlt));
	SetActorRotation(DR.Orientation.Rotator());
}
