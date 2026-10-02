// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "CamSimGimbalComponent.generated.h"

struct FCamSimConfig;
struct FCigiViewControl;
struct FCigiArtPartControl;

/**
 * UCamSimGimbalComponent
 *
 * Owns the gimbal state (yaw / pitch / roll) for ACamSimCamera.
 * Each tick it applies the frame's camera packets (FCigiCameraFrame):
 *   - View Control (opcode 16)            — absolute gimbal override (snap)
 *   - Articulated Part on the camera entity — rate-limited slew target
 *
 * Slew rate and axis limits come from FCamSimConfig. Yaw is unwound to
 * [-180, 180] before the limits, so CIGI's 0-360 range works (270 = -90).
 * When the yaw limits span the full circle the slew takes the shortest way
 * round, across +/-180; otherwise it stays inside the limits.
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
	 * Apply this frame's View Control and camera Art Part packets (oldest
	 * first), or keep slewing toward the last Art Part target when there are
	 * none. Call from ACamSimCamera each game tick before applying the result
	 * to the scene capture.
	 */
	void TickGimbal(float DeltaTime, TConstArrayView<FCigiViewControl> ViewControls,
		TConstArrayView<FCigiArtPartControl> ArtParts, const FCamSimConfig& Config);

	/**
	 * Apply a single ViewControl packet (opcode 16). Snaps yaw/pitch/roll
	 * to the packet's values, gated by its enable flags, unwinds yaw and roll
	 * to [-180, 180] (CIGI sends yaw as 0-360), then clamps the result to the
	 * configured gimbal axis limits. No slew.
	 *
	 * Exposed so unit tests can drive the gimbal without an FCigiReceiver.
	 */
	void ApplyViewControl(const FCigiViewControl& ViewCtrl, const FCamSimConfig& Config);

	/**
	 * Apply a single ArtPart packet (opcode 6) as a slew target. Rate-limited
	 * by Config.GimbalMaxSlewRateDegPerSec and clamped to axis limits. The
	 * target persists: AdvanceSlew() keeps moving toward it on later ticks.
	 *
	 * Exposed so unit tests can drive the gimbal without an FCigiReceiver.
	 */
	void ApplyArtPart(const FCigiArtPartControl& Art, float DeltaTime, const FCamSimConfig& Config);

	/** Continue slewing toward the last ArtPart target (ticks with no new packet). */
	void AdvanceSlew(float DeltaTime, const FCamSimConfig& Config);

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

	/** Slew target from the last ArtPart packet; equals the current angles otherwise. */
	float SlewTargetYaw   =   0.0f;
	float SlewTargetPitch = -90.0f;
	float SlewTargetRoll  =   0.0f;

	/**
	 * Slew current angles toward target, respecting max slew rate and axis limits.
	 * If MaxRate <= 0, snaps instantly (unlimited).
	 */
	void ApplyGimbalSlew(float TargetYaw, float TargetPitch, float TargetRoll,
	                     float DeltaTime, const FCamSimConfig& Config);
};
