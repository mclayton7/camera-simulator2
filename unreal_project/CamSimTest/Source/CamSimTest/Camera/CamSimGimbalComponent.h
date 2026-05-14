// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "CamSimGimbalComponent.generated.h"

class FCigiReceiver;
struct FCamSimConfig;
struct FCigiViewControl;
struct FCigiArtPartControl;

/**
 * UCamSimGimbalComponent
 *
 * Owns the gimbal state (yaw / pitch / roll) for ACamSimCamera.
 * Each Tick it drains:
 *   - FCigiReceiver::ViewCtrlQueue   — absolute gimbal override
 *   - FCigiReceiver::CameraArtPartQueue — rate-limited slew target
 *
 * Slew rate and axis limits come from FCamSimConfig.
 *
 * Exposes GetGimbalRelativeRotation() so ACamSimCamera can apply
 * the result to the SceneCaptureComponent2D each frame.
 */
UCLASS(ClassGroup=CamSim, meta=(BlueprintSpawnableComponent))
class CAMSIMTEST_API UCamSimGimbalComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCamSimGimbalComponent();

	/**
	 * Drive gimbal from CIGI queues.  Call from ACamSimCamera::ApplyCigiState()
	 * each game tick before applying the result to the scene capture.
	 */
	void TickGimbal(float DeltaTime, FCigiReceiver* Receiver, const FCamSimConfig& Config);

	/**
	 * Apply a single ViewControl packet (opcode 16). Snaps yaw/pitch/roll
	 * to the packet's values, gated by its enable flags, then clamps the
	 * result to the configured gimbal axis limits. No slew.
	 *
	 * Exposed so unit tests can drive the gimbal without an FCigiReceiver.
	 */
	void ApplyViewControl(const FCigiViewControl& ViewCtrl, const FCamSimConfig& Config);

	/**
	 * Apply a single ArtPart packet (opcode 6) as a slew target. Rate-limited
	 * by Config.GimbalMaxSlewRateDegPerSec and clamped to axis limits.
	 *
	 * Exposed so unit tests can drive the gimbal without an FCigiReceiver.
	 */
	void ApplyArtPart(const FCigiArtPartControl& Art, float DeltaTime, const FCamSimConfig& Config);

	/** Returns the current gimbal orientation as a Rotator (Pitch, Yaw, Roll). */
	FRotator GetGimbalRelativeRotation() const
	{
		return FRotator(GimbalPitch, GimbalYaw, GimbalRoll);
	}

	float GetGimbalYaw()   const { return GimbalYaw;   }
	float GetGimbalPitch() const { return GimbalPitch; }
	float GetGimbalRoll()  const { return GimbalRoll;  }

	/** Phase 22G: Last EntityId seen in a ViewControl packet (0 = none). */
	uint16 GetLastViewControlEntityId() const { return LastViewControlEntityId; }

	/** Phase 22G: Last body-frame offset from ViewControl (metres). */
	FVector GetLastViewControlOffset() const { return LastViewControlOffset; }

private:
	uint16  LastViewControlEntityId = 0;
	FVector LastViewControlOffset   = FVector::ZeroVector;
	/** Current gimbal orientation relative to platform body frame (degrees).
	 *  Pitch defaults to -90 (nadir) — sensor mounts below the aircraft and
	 *  looks straight down when boresighted.  CIGI ArtPart/ViewCtrl overrides. */
	float GimbalYaw   =   0.0f;
	float GimbalPitch = -90.0f;
	float GimbalRoll  =   0.0f;

	/**
	 * Slew current angles toward target, respecting max slew rate and axis limits.
	 * If MaxRate <= 0, snaps instantly (unlimited).
	 */
	void ApplyGimbalSlew(float TargetYaw, float TargetPitch, float TargetRoll,
	                     float DeltaTime, const FCamSimConfig& Config);
};
