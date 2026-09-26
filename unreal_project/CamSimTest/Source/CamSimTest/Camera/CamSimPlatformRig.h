// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Geospatial/CigiFrames.h"
#include "Geospatial/GroundSpeedEstimator.h"

class AActor;
class UCesiumGlobeAnchorComponent;
class UCamSimSubsystem;
class FCamSimTelemetryAssembler;
struct FCamSimConfig;

/**
 * FCamSimPlatformRig
 *
 * The sensor platform's geodetic pose for ACamSimCamera (the gimbal on top of
 * it is UCamSimGimbalComponent): CIGI Entity Control for the camera entity,
 * attachment to a parent entity, and the first-person view that rides on a
 * tracked entity. Moves the Cesium globe anchor and writes pose and ground
 * speed into the telemetry. Game thread only.
 */
class FCamSimPlatformRig
{
public:
	/** Place the platform at the configured start pose. */
	void Initialize(AActor* InOwner, UCesiumGlobeAnchorComponent* InAnchor, UCamSimSubsystem* InSubsystem,
		FCamSimTelemetryAssembler* InTelemetry, const FCamSimConfig& Cfg);

	/**
	 * Apply this frame's host state (CIGI Entity Control for the camera
	 * entity). Runs once per frame: FCamSimEntityManager calls it before
	 * resolving attachments; the camera's Tick calls it too in case nothing did.
	 */
	void ApplyHostPlatformState();

	/** If the platform is attached to an entity, follow it. Once per frame, after the parent is placed. */
	void FollowAttachParent();

	/**
	 * First-person view: sit at eye height on a tracked entity. Activated by
	 * config (fps_entity_id) or by a CIGI View Control naming an entity
	 * (ViewControlEntityId; 0 ends a CIGI-activated view).
	 */
	void UpdateFirstPersonView(uint16 ViewControlEntityId);

	/** Current platform pose (orientation in local NEU). */
	bool GetPose(CamSimFrames::FGeoPose& OutPose) const;

private:
	void ApplyPose(const CamSimFrames::FGeoPose& Pose, double TimeSec);

	AActor*                      Owner     = nullptr;
	UCesiumGlobeAnchorComponent* Anchor    = nullptr;
	UCamSimSubsystem*            Subsystem = nullptr;
	FCamSimTelemetryAssembler*   Telemetry = nullptr;

	// Ground speed (KLV Tag 56) from position deltas over host time
	FGroundSpeedEstimator GroundSpeed;

	// CIGI attachment to a parent entity
	bool     bAttached       = false;
	uint16   AttachParentId  = 0;
	FVector  AttachOffsetFrd = FVector::ZeroVector;
	FRotator AttachRotation  = FRotator::ZeroRotator;

	// GFrameCounter of the last ApplyHostPlatformState / FollowAttachParent run
	uint64 HostStateFrame   = MAX_uint64;
	uint64 AttachFollowFrame = MAX_uint64;

	// Phase 22G first-person view
	uint16 FpsEntityId   = 0;     // 0 = inactive
	float  FpsEyeHeightM = 1.7f;
	bool   bFpsFromCigi  = false; // activated by CIGI View Control rather than config
};
