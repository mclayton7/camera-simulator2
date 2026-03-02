// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "CamSimGimbalComponent.generated.h"

class FCigiReceiver;
struct FCamSimConfig;

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

	/** Returns the current gimbal orientation as a Rotator (Pitch, Yaw, Roll). */
	FRotator GetGimbalRelativeRotation() const
	{
		return FRotator(GimbalPitch, GimbalYaw, GimbalRoll);
	}

	float GetGimbalYaw()   const { return GimbalYaw;   }
	float GetGimbalPitch() const { return GimbalPitch; }
	float GetGimbalRoll()  const { return GimbalRoll;  }

private:
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
