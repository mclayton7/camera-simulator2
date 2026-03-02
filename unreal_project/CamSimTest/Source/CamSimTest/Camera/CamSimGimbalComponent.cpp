// Copyright CamSim Contributors. All Rights Reserved.

#include "Camera/CamSimGimbalComponent.h"
#include "CamSimTest.h"
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
			if (ViewCtrl.bYawEn)   GimbalYaw   = ViewCtrl.Yaw;
			if (ViewCtrl.bPitchEn) GimbalPitch = ViewCtrl.Pitch;
			if (ViewCtrl.bRollEn)  GimbalRoll  = ViewCtrl.Roll;
			UE_LOG(LogCamSim, Verbose,
				TEXT("UCamSimGimbalComponent: ViewCtrl -> yaw=%.1f pitch=%.1f roll=%.1f"),
				GimbalYaw, GimbalPitch, GimbalRoll);
		}
	}

	// -----------------------------------------------------------------------
	// Camera ArtPart (opcode 6 for camera entity) → slew-rate-limited target
	// -----------------------------------------------------------------------
	{
		FCigiArtPartControl Art;
		float TargetYaw   = GimbalYaw;
		float TargetPitch = GimbalPitch;
		float TargetRoll  = GimbalRoll;
		bool  bGotArt     = false;

		while (Receiver->DequeueCameraArtPart(Art))
		{
			if (Art.bArtPartEn)
			{
				if (Art.bYawEn)   TargetYaw   = Art.Yaw;
				if (Art.bPitchEn) TargetPitch = Art.Pitch;
				if (Art.bRollEn)  TargetRoll  = Art.Roll;
				bGotArt = true;
			}
		}

		if (bGotArt)
		{
			ApplyGimbalSlew(TargetYaw, TargetPitch, TargetRoll, DeltaTime, Config);
		}
	}
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
		float Delta = Target - Current;
		// Shortest path wrap
		while (Delta >  180.0f) Delta -= 360.0f;
		while (Delta < -180.0f) Delta += 360.0f;
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
