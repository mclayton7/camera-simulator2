// Copyright CamSim Contributors. All Rights Reserved.

#include "Camera/CamSimGimbalComponent.h"
#include "CamSimTest.h"
#include "CIGI/CigiPacketTypes.h"
#include "CIGI/CigiReceiver.h"
#include "Config/CamSimConfig.h"

UCamSimGimbalComponent::UCamSimGimbalComponent()
{
	PrimaryComponentTick.bCanEverTick = false;  // driven explicitly by ACamSimCamera
}

// -------------------------------------------------------------------------
// TickGimbal — drain CIGI queues and update gimbal orientation
// -------------------------------------------------------------------------

void UCamSimGimbalComponent::TickGimbal(float DeltaTime, FCigiReceiver* Receiver, const FCamSimConfig& Config)
{
	if (!Receiver) return;

	// -----------------------------------------------------------------------
	// View Control (opcode 16) → direct gimbal override (no slew)
	// -----------------------------------------------------------------------
	{
		FCigiViewControl ViewCtrl;
		while (Receiver->DequeueViewControl(ViewCtrl))
		{
			ApplyViewControl(ViewCtrl, Config);
		}
	}

	// -----------------------------------------------------------------------
	// Camera ArtPart (opcode 6 for camera entity) → slew-rate-limited target.
	// Coalesce into a single Apply call so a burst of packets only slews once
	// per tick (the previous behaviour). Pass the coalesced packet through
	// ApplyArtPart so the per-axis-limit and slew-rate logic stays in one place.
	// -----------------------------------------------------------------------
	{
		FCigiArtPartControl Art;
		FCigiArtPartControl Coalesced;
		Coalesced.bArtPartEn = false;

		while (Receiver->DequeueCameraArtPart(Art))
		{
			if (!Art.bArtPartEn) continue;
			Coalesced.bArtPartEn = true;
			if (Art.bYawEn)   { Coalesced.bYawEn   = true; Coalesced.Yaw   = Art.Yaw;   }
			if (Art.bPitchEn) { Coalesced.bPitchEn = true; Coalesced.Pitch = Art.Pitch; }
			if (Art.bRollEn)  { Coalesced.bRollEn  = true; Coalesced.Roll  = Art.Roll;  }
		}

		if (Coalesced.bArtPartEn)
		{
			ApplyArtPart(Coalesced, DeltaTime, Config);
		}
	}
}

// -------------------------------------------------------------------------
// ApplyViewControl — snap with limits (no slew), update FPS-view bookkeeping
// -------------------------------------------------------------------------

void UCamSimGimbalComponent::ApplyViewControl(const FCigiViewControl& ViewCtrl, const FCamSimConfig& Config)
{
	if (ViewCtrl.bYawEn)   GimbalYaw   = ViewCtrl.Yaw;
	if (ViewCtrl.bPitchEn) GimbalPitch = ViewCtrl.Pitch;
	if (ViewCtrl.bRollEn)  GimbalRoll  = ViewCtrl.Roll;

	// Clamp to physical envelope so a CIGI host can't drive the gimbal outside
	// the sensor's mechanical limits. ArtPart already did this; ViewControl now
	// matches.
	GimbalPitch = FMath::Clamp(GimbalPitch, Config.GimbalPitchMin, Config.GimbalPitchMax);
	GimbalYaw   = FMath::Clamp(GimbalYaw,   Config.GimbalYawMin,   Config.GimbalYawMax);

	// Phase 22G: Capture FPS view activation from ViewControl EntityId
	if (ViewCtrl.EntityId != 0)
	{
		LastViewControlEntityId = ViewCtrl.EntityId;
		if (ViewCtrl.bXOffEn) LastViewControlOffset.X = ViewCtrl.XOff;
		if (ViewCtrl.bYOffEn) LastViewControlOffset.Y = ViewCtrl.YOff;
		if (ViewCtrl.bZOffEn) LastViewControlOffset.Z = ViewCtrl.ZOff;
	}
	else if (LastViewControlEntityId != 0)
	{
		// EntityId=0 deactivates FPS mode
		LastViewControlEntityId = 0;
		LastViewControlOffset = FVector::ZeroVector;
	}

	UE_LOG(LogCamSim, Verbose,
		TEXT("UCamSimGimbalComponent: ViewCtrl -> yaw=%.1f pitch=%.1f roll=%.1f entity=%u"),
		GimbalYaw, GimbalPitch, GimbalRoll, ViewCtrl.EntityId);
}

// -------------------------------------------------------------------------
// ApplyArtPart — slew-rate-limited target with axis clamps
// -------------------------------------------------------------------------

void UCamSimGimbalComponent::ApplyArtPart(const FCigiArtPartControl& Art, float DeltaTime, const FCamSimConfig& Config)
{
	if (!Art.bArtPartEn) return;

	const float TargetYaw   = Art.bYawEn   ? Art.Yaw   : GimbalYaw;
	const float TargetPitch = Art.bPitchEn ? Art.Pitch : GimbalPitch;
	const float TargetRoll  = Art.bRollEn  ? Art.Roll  : GimbalRoll;

	ApplyGimbalSlew(TargetYaw, TargetPitch, TargetRoll, DeltaTime, Config);
}

// -------------------------------------------------------------------------
// ApplyGimbalSlew — move gimbal toward target, respecting rate and limits
// -------------------------------------------------------------------------

void UCamSimGimbalComponent::ApplyGimbalSlew(
	float TargetYaw, float TargetPitch, float TargetRoll,
	float DeltaTime, const FCamSimConfig& Config)
{
	const float MaxRate = Config.GimbalMaxSlewRateDegPerSec;

	auto SlewAngle = [MaxRate, DeltaTime](float Current, float Target) -> float
	{
		if (MaxRate <= 0.0f) return Target;  // unlimited — snap instantly
		// UnwindDegrees is O(1) via fmod instead of the while-loop form below,
		// and folds the [-180,180] shortest-path normalisation into one call.
		const float Delta    = FMath::UnwindDegrees(Target - Current);
		const float MaxDelta = MaxRate * DeltaTime;
		return Current + FMath::Clamp(Delta, -MaxDelta, MaxDelta);
	};

	GimbalYaw   = SlewAngle(GimbalYaw,   TargetYaw);
	GimbalPitch = SlewAngle(GimbalPitch, TargetPitch);
	GimbalRoll  = SlewAngle(GimbalRoll,  TargetRoll);

	// Clamp to physical axis limits
	GimbalPitch = FMath::Clamp(GimbalPitch, Config.GimbalPitchMin, Config.GimbalPitchMax);
	GimbalYaw   = FMath::Clamp(GimbalYaw,   Config.GimbalYawMin,   Config.GimbalYawMax);
}
