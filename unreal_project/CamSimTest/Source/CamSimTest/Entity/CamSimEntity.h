// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CIGI/CigiPacketTypes.h"
#include "Ocean/IOceanSurface.h"
// StreamableManager gives us the complete FStreamableHandle type — needed so
// the UHT-generated CamSimEntity.gen.cpp can destruct TSharedPtr<FStreamableHandle>.
#include "Engine/StreamableManager.h"
#include "CamSimEntity.generated.h"

class UCesiumGlobeAnchorComponent;
class UStaticMeshComponent;
class USkeletalMeshComponent;
class UPoseableMeshComponent;  // USkinnedMeshComponent subclass with per-bone API
class UPointLightComponent;
class UStaticMesh;
class USkeletalMesh;
class FEntityTypeTable;
struct FEntityTypeEntry;
// FStreamableHandle is provided by the Engine/StreamableManager.h include above.

/**
 * ACamSimEntity
 *
 * Represents a non-camera CIGI entity (aircraft, vehicle, etc.) in the scene.
 * Driven by FCamSimEntityManager which drains the CIGI entity/rate/art-part/
 * component queues each tick.
 *
 * Position is set via CesiumGlobeAnchorComponent (WGS-84 lat/lon/alt).
 * Dead-reckoning is applied in Tick() when rate data has been received.
 *
 * Mesh loading is synchronous. Paths ending in .gltf/.glb are loaded via the
 * glTFRuntime plugin from {repo_root}/entities/; /Game/... paths use the UE
 * content browser. Async loading can be layered on in a later phase.
 */
UCLASS()
class CAMSIMTEST_API ACamSimEntity : public AActor
{
	GENERATED_BODY()

public:
	ACamSimEntity();

	// Entity identity — set by EntityManager at spawn time
	uint16 EntityId   = 0;
	uint16 EntityType = 0;

	/** Inject the type table — must be called before SetEntityType(). */
	void SetEntityTypeTable(const FEntityTypeTable* Table);

	/**
	 * Load mesh assets for the given CIGI type ID.
	 * Looks up FEntityTypeEntry and assigns the mesh to the appropriate component.
	 */
	void SetEntityType(uint16 Type);

	/** Snap position and orientation from an incoming CIGI packet. */
	void ApplyPose(const FCigiEntityState& S);

	/** Store linear/angular rates for dead-reckoning between CIGI updates. */
	void SetRateControl(const FCigiRateControl& R);

	/** Apply an articulated part offset/rotation to the skeletal mesh. */
	void ApplyArtPart(const FCigiArtPartControl& P);

	/** Handle component control (lights, damage state, etc.). */
	void ApplyComponentControl(const FCigiComponentControl& C);

	/** Apply runtime culling and tick-rate controls for large scene scaling. */
	void ApplyScaleControls(float MaxDrawDistanceM, float TickRateHz);

	/** Enable or disable shadow casting on all mesh components (Phase 24A). */
	void SetShadowCasting(bool bCast);

	/** Current damage state (0=intact, 1=damaged, 2=destroyed). */
	uint8 GetDamageState() const { return DamageState; }

	/** Configure gradual damage interpolation (Phase 22C). */
	void SetDamageInterpolation(bool bEnabled, float RateSec);

	/**
	 * Apply pitch/roll/heave from ocean surface to this vessel entity.
	 * Called from FCamSimEntityManager::Tick() for sea-domain entities only.
	 * Ocean must be non-null. TypeEntry provides HalfLength/HalfBeam geometry.
	 */
	void ApplyVesselMotion(IOceanSurface* Ocean,
	                       float HalfLengthCm, float HalfBeamCm,
	                       float MotionScale);

	// AActor interface
	virtual void Tick(float DeltaTime) override;

protected:
	virtual void BeginPlay() override;

private:
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UCesiumGlobeAnchorComponent> GlobeAnchor;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UStaticMeshComponent> StaticMeshComp;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UPoseableMeshComponent> SkelMeshComp;  // allows SetBoneTransformByName

	// Navigation lights (hidden by default; enabled via Component Control)
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UPointLightComponent> NavLightRed;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UPointLightComponent> NavLightGreen;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UPointLightComponent> NavLightWhite;

	// Anti-collision strobe (1Hz, 50% duty cycle)
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UPointLightComponent> StrobeLight;

	// Landing lights (component control CompId=2)
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UPointLightComponent> LandingLight;

	// Phase 22D: Animated character skeletal mesh
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USkeletalMeshComponent> AnimMeshComp;

	float CachedGroundSpeed = 0.0f;
	void InitAnimatedCharacter(const FEntityTypeEntry& Entry);

	/** In-flight async mesh load handle — resetting cancels the request. */
	TSharedPtr<FStreamableHandle> PendingMeshHandle_;

	// Async-load helpers — keep the public API stable; the call chain is
	// SetEntityType → Request*Mesh → (async) → ApplyLoaded*Mesh.
	void RequestAsyncStaticMesh(const FEntityTypeEntry& Entry, uint16 Type);
	void RequestAsyncSkeletalMesh(const FEntityTypeEntry& Entry, uint16 Type);
	void ApplyLoadedStaticMesh(UStaticMesh* Mesh, const FEntityTypeEntry& Entry, uint16 Type);
	void ApplyLoadedSkeletalMesh(USkeletalMesh* Mesh, const FEntityTypeEntry& Entry, uint16 Type);

	// Dead-reckoning state. Orientation is stored as FQuat to integrate body-frame
	// angular velocity without passing through the FRotator→FQuat singularity at
	// pitch = ±90° — Euler-only integration produced discontinuous heading for
	// aircraft in a steep climb or dive.
	struct FDRState
	{
		double  Lat = 0.0, Lon = 0.0;
		float   Alt = 0.0f;
		FQuat   Orientation = FQuat::Identity;                 // single source of truth
		float   XRate = 0.0f, YRate = 0.0f, ZRate = 0.0f;     // m/s body-frame
		float   YawRate = 0.0f, PitchRate = 0.0f, RollRate = 0.0f; // deg/s body-frame
		bool    bHasRate = false;
	} DR;

	// Strobe state
	bool  bStrobeEnabled = false;
	float StrobeAccum    = 0.0f;

	// Damage state (0=intact, 1=damaged, 2=destroyed)
	uint8 DamageState = 0;

	// Phase 22C: Damage transition interpolation
	uint8 TargetDamageState    = 0;
	float DamageBlendAlpha     = 1.0f;
	bool  bDamageInterpolating = false;
	float DamageInterpolationRate = 1.0f;

	// Set to true after the first ApplyPose — suppresses the one-time world-location log
	bool bPoseLogged = false;

	// Injected by FCamSimEntityManager at spawn time; lifetime guaranteed by UCamSimSubsystem
	const FEntityTypeTable* TypeTable = nullptr;

	void UpdateDeadReckoning(float Dt);
};
