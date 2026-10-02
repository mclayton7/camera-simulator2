// Copyright CamSim Contributors. All Rights Reserved.

#include "Camera/CamSimGimbalComponent.h"
#include "CamSimTest.h"
#include "CIGI/CigiPacketTypes.h"
#include "Config/CamSimConfig.h"

UCamSimGimbalComponent::UCamSimGimbalComponent()
{
	PrimaryComponentTick.bCanEverTick = false;  // driven explicitly by ACamSimCamera
}

// -------------------------------------------------------------------------
// TickGimbal — apply this frame's CIGI packets and update gimbal orientation
// -------------------------------------------------------------------------

void UCamSimGimbalComponent::TickGimbal(float DeltaTime, TConstArrayView<FCigiViewControl> ViewControls,
	TConstArrayView<FCigiArtPartControl> ArtParts, const FCamSimConfig& Config)
{
	// -----------------------------------------------------------------------
	// View Control (opcode 16) → direct gimbal override (no slew)
	// -----------------------------------------------------------------------
	for (const FCigiViewControl& ViewCtrl : ViewControls)
	{
		ApplyViewControl(ViewCtrl, Config);
	}

	// -----------------------------------------------------------------------
	// Camera ArtPart (opcode 6 for camera entity) → slew-rate-limited target.
	// Coalesce into a single Apply call so a burst of packets only slews once
	// per tick (the previous behaviour). Pass the coalesced packet through
	// ApplyArtPart so the per-axis-limit and slew-rate logic stays in one place.
	// -----------------------------------------------------------------------
	FCigiArtPartControl Coalesced;
	Coalesced.bArtPartEn = false;
	for (const FCigiArtPartControl& Art : ArtParts)
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
	else
	{
		// Hosts often send ArtPart slower than the frame rate: keep moving.
		AdvanceSlew(DeltaTime, Config);
	}
}

// -------------------------------------------------------------------------
// ApplyViewControl — snap with limits (no slew), update FPS-view bookkeeping
// -------------------------------------------------------------------------

void UCamSimGimbalComponent::ApplyViewControl(const FCigiViewControl& ViewCtrl, const FCamSimConfig& Config)
{
	// CIGI 3.3 sends View Control yaw as 0-360: unwind before the clamp, or
	// 270 (look left) would clamp to 180 (look back). HITL.md gap 1.
	if (ViewCtrl.bYawEn)   GimbalYaw   = FMath::UnwindDegrees(ViewCtrl.Yaw);
	if (ViewCtrl.bPitchEn) GimbalPitch = ViewCtrl.Pitch;
	if (ViewCtrl.bRollEn)  GimbalRoll  = FMath::UnwindDegrees(ViewCtrl.Roll);

	// Clamp to physical envelope so a CIGI host can't drive the gimbal outside
	// the sensor's mechanical limits. ArtPart already did this; ViewControl now
	// matches.
	GimbalPitch = FMath::Clamp(GimbalPitch, Config.GimbalPitchMin, Config.GimbalPitchMax);
	GimbalYaw   = FMath::Clamp(GimbalYaw,   Config.GimbalYawMin,   Config.GimbalYawMax);

	// A snap replaces any ArtPart slew in progress.
	SlewTargetYaw   = GimbalYaw;
	SlewTargetPitch = GimbalPitch;
	SlewTargetRoll  = GimbalRoll;

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

	// Unwound like View Control, so a 0-360 yaw lands inside -180..180 limits.
	if (Art.bYawEn)   SlewTargetYaw   = FMath::UnwindDegrees(Art.Yaw);
	if (Art.bPitchEn) SlewTargetPitch = Art.Pitch;
	if (Art.bRollEn)  SlewTargetRoll  = FMath::UnwindDegrees(Art.Roll);

	ApplyGimbalSlew(SlewTargetYaw, SlewTargetPitch, SlewTargetRoll, DeltaTime, Config);
}

void UCamSimGimbalComponent::AdvanceSlew(float DeltaTime, const FCamSimConfig& Config)
{
	ApplyGimbalSlew(SlewTargetYaw, SlewTargetPitch, SlewTargetRoll, DeltaTime, Config);
}

// -------------------------------------------------------------------------
// ApplyGimbalSlew — move gimbal toward target, respecting rate and limits
// -------------------------------------------------------------------------

void UCamSimGimbalComponent::ApplyGimbalSlew(
	float TargetYaw, float TargetPitch, float TargetRoll,
	float DeltaTime, const FCamSimConfig& Config)
{
	const float MaxRate = Config.GimbalMaxSlewRateDegPerSec;

	// Wrapping axis: shortest way round, result unwound to [-180, 180].
	auto SlewWrapped = [MaxRate, DeltaTime](float Current, float Target) -> float
	{
		if (MaxRate <= 0.0f) return FMath::UnwindDegrees(Target);  // unlimited — snap instantly
		const float Delta    = FMath::UnwindDegrees(Target - Current);
		const float MaxDelta = MaxRate * DeltaTime;
		return FMath::UnwindDegrees(Current + FMath::Clamp(Delta, -MaxDelta, MaxDelta));
	};
	// Limited axis: straight toward the target, never through the stops.
	auto SlewLinear = [MaxRate, DeltaTime](float Current, float Target) -> float
	{
		if (MaxRate <= 0.0f) return Target;
		const float MaxDelta = MaxRate * DeltaTime;
		return Current + FMath::Clamp(Target - Current, -MaxDelta, MaxDelta);
	};

	// Yaw wraps only when its limits allow continuous rotation; a gimbal with
	// stops (e.g. -170..170) must go the long way round instead of through
	// +/-180, where it used to stick against the limit.
	const bool bContinuousYaw = (Config.GimbalYawMax - Config.GimbalYawMin) >= 360.0f - KINDA_SMALL_NUMBER;
	GimbalYaw = bContinuousYaw
		? SlewWrapped(GimbalYaw, TargetYaw)
		: SlewLinear(GimbalYaw, FMath::Clamp(TargetYaw, Config.GimbalYawMin, Config.GimbalYawMax));
	GimbalPitch = SlewLinear(GimbalPitch, TargetPitch);
	GimbalRoll  = SlewWrapped(GimbalRoll, TargetRoll);

	// Clamp to physical axis limits
	GimbalPitch = FMath::Clamp(GimbalPitch, Config.GimbalPitchMin, Config.GimbalPitchMax);
	GimbalYaw   = FMath::Clamp(GimbalYaw,   Config.GimbalYawMin,   Config.GimbalYawMax);
}
