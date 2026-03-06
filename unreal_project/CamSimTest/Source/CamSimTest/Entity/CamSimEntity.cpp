// Copyright CamSim Contributors. All Rights Reserved.

#include "Entity/CamSimEntity.h"
#include "Entity/EntityTypeTable.h"
#include "CamSimTest.h"

#include "Components/StaticMeshComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "CesiumGlobeAnchorComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
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

	SkelMeshComp = CreateDefaultSubobject<UPoseableMeshComponent>(TEXT("SkelMesh"));
	SkelMeshComp->SetupAttachment(Root);
	SkelMeshComp->SetVisibility(false);
	SkelMeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);

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

	if (Entry->bSkeletal)
	{
		USkeletalMesh* Mesh = LoadSkeletalMeshFromPath(Entry->AssetPath);
		if (Mesh)
		{
			SkelMeshComp->SetSkinnedAsset(Mesh);
			SkelMeshComp->SetRelativeRotation(Entry->ModelRotation);
			SkelMeshComp->SetRelativeScale3D(FVector(Entry->ModelScale));
			SkelMeshComp->SetVisibility(true);
			StaticMeshComp->SetVisibility(false);
			UE_LOG(LogCamSim, Log, TEXT("ACamSimEntity[%u]: loaded skeletal mesh '%s' (type %u, scale=%.3f)"),
				EntityId, *Entry->AssetPath, Type, Entry->ModelScale);
		}
		else
		{
			UE_LOG(LogCamSim, Warning, TEXT("ACamSimEntity[%u]: failed to load skeletal mesh '%s'"),
				EntityId, *Entry->AssetPath);
		}
	}
	else
	{
		UStaticMesh* Mesh = LoadStaticMeshFromPath(Entry->AssetPath);
		if (Mesh)
		{
			StaticMeshComp->SetStaticMesh(Mesh);
			StaticMeshComp->SetRelativeRotation(Entry->ModelRotation);
			StaticMeshComp->SetRelativeScale3D(FVector(Entry->ModelScale));
			StaticMeshComp->SetVisibility(true);
			SkelMeshComp->SetVisibility(false);
			UE_LOG(LogCamSim, Log, TEXT("ACamSimEntity[%u]: loaded static mesh '%s' (type %u, scale=%.3f)"),
				EntityId, *Entry->AssetPath, Type, Entry->ModelScale);
		}
		else
		{
			UE_LOG(LogCamSim, Warning, TEXT("ACamSimEntity[%u]: failed to load static mesh '%s'"),
				EntityId, *Entry->AssetPath);
		}
	}
}

// -------------------------------------------------------------------------
// ApplyPose — snap position + orientation from CIGI packet
// -------------------------------------------------------------------------

void ACamSimEntity::ApplyPose(const FCigiEntityState& S)
{
	if (!GlobeAnchor) return;

	// Update DR base so dead-reckoning doesn't drift after a new packet
	DR.Lat   = S.Latitude;
	DR.Lon   = S.Longitude;
	DR.Alt   = S.Altitude;
	DR.Yaw   = S.Yaw;
	DR.Pitch = S.Pitch;
	DR.Roll  = S.Roll;

	GlobeAnchor->MoveToLongitudeLatitudeHeight(
		FVector(S.Longitude, S.Latitude, S.Altitude));
	SetActorRotation(FRotator(S.Pitch, S.Yaw, S.Roll));

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

	case 10: // Damage state — swap mesh asset
		{
			DamageState = C.CompState;
			static const TCHAR* DamageNames[] = { TEXT("intact"), TEXT("damaged"), TEXT("destroyed") };
			UE_LOG(LogCamSim, Log, TEXT("ACamSimEntity[%u]: damage state -> %u (%s)"),
				EntityId, DamageState,
				DamageState <= 2 ? DamageNames[DamageState] : TEXT("unknown"));

			const FEntityTypeEntry* Entry = TypeTable ? TypeTable->FindEntry(EntityType) : nullptr;
			if (!Entry) break;

			FString AssetPath;
			if (DamageState == 1 && !Entry->DamagedAssetPath.IsEmpty())
			{
				AssetPath = Entry->DamagedAssetPath;
			}
			else if (DamageState == 2 && !Entry->DestroyedAssetPath.IsEmpty())
			{
				AssetPath = Entry->DestroyedAssetPath;
			}
			else
			{
				AssetPath = Entry->AssetPath; // revert to intact
			}

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
	if (!DR.bHasRate || !GlobeAnchor) return;

	// Integrate angular rates
	float NewYaw   = DR.Yaw   + DR.YawRate   * Dt;
	float NewPitch = DR.Pitch + DR.PitchRate  * Dt;
	float NewRoll  = DR.Roll  + DR.RollRate   * Dt;

	// Body → NED velocity: X=forward(North), Y=right(East), Z=down
	FQuat Q = FQuat(FRotator(DR.Pitch, DR.Yaw, DR.Roll));
	FVector WorldVel = Q.RotateVector(FVector(DR.XRate, DR.YRate, DR.ZRate));

	// Integrate position in WGS-84 (linear approximation valid for small Dt)
	double NewLat = DR.Lat + (WorldVel.X * Dt) / 111320.0;
	double NewLon = DR.Lon + (WorldVel.Y * Dt) /
		(111320.0 * FMath::Cos(DR.Lat * PI / 180.0));
	float  NewAlt = DR.Alt - WorldVel.Z * Dt;  // CIGI Z=down, alt increases upward

	// Update DR state
	DR.Yaw   = NewYaw;
	DR.Pitch = NewPitch;
	DR.Roll  = NewRoll;
	DR.Lat   = NewLat;
	DR.Lon   = NewLon;
	DR.Alt   = NewAlt;

	GlobeAnchor->MoveToLongitudeLatitudeHeight(FVector(NewLon, NewLat, NewAlt));
	SetActorRotation(FRotator(NewPitch, NewYaw, NewRoll));
}
