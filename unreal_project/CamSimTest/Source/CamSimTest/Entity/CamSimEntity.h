// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CIGI/CigiPacketTypes.h"
#include "CamSimEntity.generated.h"

class UCesiumGlobeAnchorComponent;
class UStaticMeshComponent;
class UPoseableMeshComponent;  // USkinnedMeshComponent subclass with per-bone API
class UPointLightComponent;
class FEntityTypeTable;

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

	// Dead-reckoning state
	struct FDRState
	{
		double Lat = 0.0, Lon = 0.0;
		float  Alt = 0.0f;
		float  Yaw = 0.0f, Pitch = 0.0f, Roll = 0.0f;
		float  XRate = 0.0f, YRate = 0.0f, ZRate = 0.0f;  // m/s body-frame
		float  YawRate = 0.0f, PitchRate = 0.0f, RollRate = 0.0f; // deg/s
		bool   bHasRate = false;
	} DR;

	// Strobe state
	bool  bStrobeEnabled = false;
	float StrobeAccum    = 0.0f;

	// Damage state (0=intact, 1=damaged, 2=destroyed)
	uint8 DamageState = 0;

	// Set to true after the first ApplyPose — suppresses the one-time world-location log
	bool bPoseLogged = false;

	// Injected by FCamSimEntityManager at spawn time; lifetime guaranteed by UCamSimSubsystem
	const FEntityTypeTable* TypeTable = nullptr;

	void UpdateDeadReckoning(float Dt);
};
