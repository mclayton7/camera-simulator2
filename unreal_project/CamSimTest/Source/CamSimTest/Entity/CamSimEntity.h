// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Sim/Commands.h"
#include "Geospatial/CigiFrames.h"
#include "Entity/SurfaceClamp.h"
#include "Thermal/EntityThermal.h"
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
class ISurfaceProbe;
class UCamSimSubsystem;
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

	// Entity identity — set by EntityManager at spawn time. Key is the
	// entity's identity (source + ID); EntityId is its ID within the source,
	// truncated to 16 bits, for logs and ground-truth labels.
	FEntityKey Key;
	uint16 EntityId   = 0;
	uint16 EntityType = 0;
	uint32 AnnotationId = 0;  // session-unique ground-truth ID, set at spawn

	/** Custom-depth stencil value (1..255) the GPU ground-truth pass reads; 0 = untagged. Applies to meshes only. */
	void  SetGroundTruthStencil(uint8 Value);
	uint8 GetGroundTruthStencil() const { return GroundTruthStencil; }

	/**
	 * Ground-truth box3d (ROADMAP 2.7): the union of the actor-space (cm, actor scale removed) bounds of the meshes
	 * SetGroundTruthStencil tags, counting only visible ones with a mesh asset. Particles (rotor wash, smoke,
	 * wakes), lights and empty mesh slots never count. Invalid (IsValid == 0) when no mesh is shown yet.
	 */
	FBox GetGroundTruthLocalBox() const { return ComputeMeshLocalBox(*this); }
	/** GetGroundTruthLocalBox for any actor (testable without an ACamSimEntity). */
	static FBox ComputeMeshLocalBox(const AActor& Actor);

	/** True for surface vessels (the entity's surface placement is water: DIS domain 3). */
	bool IsWaterSurfaceVessel() const { return SurfaceMode == ESurfaceMode::Water; }

	/**
	 * Land or sea vehicle (thermal: +8 K default offset, ROADMAP 4A): a DIS/CIGI platform in the land or surface domain
	 * (the domains CamSim::Dis::SurfaceModeFor places on the surface), or a type whose entity_category is
	 * truck/boat/ground/sea.
	 */
	bool IsSurfaceVehicle() const;
	/** IsSurfaceVehicle's rule for a classification and an entity_category (case-insensitive). Pure. */
	static bool IsSurfaceVehicleClass(const FEntityClassification& Class, const FString& EntityCategory);

	/** Inject the surface probe (owned by the entity manager); null disables surface placement. */
	void SetSurfaceProbe(const ISurfaceProbe* InProbe) { SurfaceProbe = InProbe; }

	/** Inject the type table — must be called before SetEntityType(). */
	void SetEntityTypeTable(const FEntityTypeTable* Table);

	/**
	 * Load mesh assets for the given CIGI type ID.
	 * Looks up FEntityTypeEntry and assigns the mesh to the appropriate component.
	 */
	void SetEntityType(uint16 Type);

	/**
	 * Snap position and orientation from an entity command (and take its
	 * motion model, if it carries one). For an attached (child) entity this
	 * records the offset from its parent; the entity manager then places it
	 * every tick via ApplyGeoPose().
	 */
	void ApplyCommand(const FEntityCommand& Command);

	/** Place the entity at a resolved geodetic pose (orientation in local NEU). */
	void ApplyGeoPose(const CamSimFrames::FGeoPose& Pose);

	/** Current geodetic pose, read back from the globe anchor. */
	bool GetGeoPose(CamSimFrames::FGeoPose& OutPose) const;

	/** Attached to a parent entity (e.g. CIGI Attach State = Attach). */
	bool     IsAttached() const              { return bAttached; }
	const FEntityKey& GetParentKey() const   { return AttachParent; }
	FVector  GetAttachOffsetFrd() const      { return AttachOffsetFrd; }
	FRotator GetAttachRotation() const       { return AttachRotation; }  // Pitch, Yaw, Roll relative to parent

	/** How the entity moves (dead reckoning) between pose updates. */
	void SetMotion(const FMotionModel& Motion);

	/** Apply an articulated part offset/rotation to the skeletal mesh. */
	void ApplyArticulation(const FArticulationCommand& P);

	/** Handle component control (lights, damage state, etc.). */
	void ApplyComponent(const FComponentCommand& C);

	/** Apply runtime culling and tick-rate controls for large scene scaling. */
	void ApplyScaleControls(float MaxDrawDistanceM, float TickRateHz);

	/** Enable or disable shadow casting on all mesh components (Phase 24A). */
	void SetShadowCasting(bool bCast);

	/** Current damage state (0=intact, 1=damaged, 2=destroyed). */
	uint8 GetDamageState() const { return DamageState; }
	/** Commanded thermal inputs (ROADMAP 4C): Component Control 11 power plant, 12 flaming, 10 damage. */
	const FEntityThermalCommanded& GetThermalCommanded() const { return ThermalCmd; }
	/**
	 * Step the thermal state (ROADMAP 4C) to sim time SimSec: speed from this entity's ECEF position, targets from the
	 * environment (bHasEnv false: D = 0). The entity manager calls it once per tick after every pose is final.
	 */
	void StepThermal(double SimSec, const FEntityThermalSettings& Settings);
	/** Latch an IR frame's T_air and this entity's baseline B (its stencil at that frame); ignored when invalid. */
	void LatchThermalEnv(bool bEnvValid, float TairK, float BaselineK) { CamSimEntityThermal::LatchEnv(ThermalEnvLatch, bEnvValid, TairK, BaselineK); }
	const FEntityThermalState& GetThermalState() const { return ThermalState; }

	// AActor interface
	virtual void Tick(float DeltaTime) override;

protected:
	virtual void BeginPlay() override;

private:
	uint8 GroundTruthStencil = 0;

	/** Last non-empty classification a command carried (CIGI Conformal Clamp carries none and keeps it). */
	FEntityClassification Classification;

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
		FMotionModel Motion;
		bool    bHasMotion = false;
	} DR;

	// CIGI attachment: while attached, pose follows the parent and dead
	// reckoning is suspended.
	bool     bAttached       = false;
	FEntityKey AttachParent;
	FVector  AttachOffsetFrd = FVector::ZeroVector;    // metres, parent body frame
	FRotator AttachRotation  = FRotator::ZeroRotator;  // relative to parent axes

	// Strobe state
	bool  bStrobeEnabled = false;
	float StrobeAccum    = 0.0f;

	// Damage state (0=intact, 1=damaged, 2=destroyed)
	uint8 DamageState = 0;
	// ROADMAP 4C: host-commanded thermal inputs (power plant, flaming; damage mirrors DamageState)
	FEntityThermalCommanded ThermalCmd;
	FEntityThermalState     ThermalState;
	FEntitySpeedTracker     ThermalSpeed;
	FEntityThermalLatch     ThermalEnvLatch;

	// Set to true after the first ApplyPose — suppresses the one-time world-location log
	bool bPoseLogged = false;

	// Injected by FCamSimEntityManager at spawn time; lifetime guaranteed by UCamSimSubsystem
	const FEntityTypeTable* TypeTable = nullptr;

	void UpdateDeadReckoning(float Dt);

	/** Write a pose to the globe anchor, placed on the surface for SurfaceMode. */
	void CommitPose(const CamSimFrames::FGeoPose& SenderPose);
	void GetFootprintHalfSizesM(double& OutHalfLengthM, double& OutHalfBeamM) const;

	/** Cached at BeginPlay (mirrors ACamSimCamera/ACamSimEnvironment). Null if not found. */
	UCamSimSubsystem* GetCamSimSubsystem() const { return Subsystem.Get(); }
	TWeakObjectPtr<UCamSimSubsystem> Subsystem;

	// Surface placement: the DR base stays the sender's pose; the clamp is
	// applied on top at every commit (host command and dead reckoning alike).
	const ISurfaceProbe*        SurfaceProbe = nullptr;
	ESurfaceMode                SurfaceMode  = ESurfaceMode::None;
	CamSimSurface::FClampState  SurfaceState;
	double                      LastCommitTimeSec = -1.0;
	CamSimFrames::FGeoPose      LastCommitSender;   // valid while LastCommitTimeSec >= 0

	/** Forget the surface: the next commit traces the full span and snaps (mode change, attach/detach, teleport). */
	void ResetSurfacePlacement();
};
