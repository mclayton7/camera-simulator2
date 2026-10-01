// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class APlayerController;
struct FPostProcessSettings;
struct FEngineShowFlags;

namespace CamSimRender
{
	/**
	 * True when the view moved too far in one frame for TSR to reproject
	 * (teleport, origin rebase, view snap). Locations in UE units (cm). A
	 * non-positive threshold skips that check (Validate() reports it) rather
	 * than cutting every frame, which would turn TSR into no anti-aliasing.
	 */
	inline bool ShouldCutCamera(const FVector& PrevLocCm, const FQuat& PrevRot,
	                            const FVector& CurLocCm, const FQuat& CurRot,
	                            double CutDistanceM, double CutAngleDeg)
	{
		const double MovedM = FVector::Dist(PrevLocCm, CurLocCm) / 100.0;
		const double TurnedDeg = FMath::RadiansToDegrees(PrevRot.AngularDistance(CurRot));
		return (CutDistanceM > 0.0 && MovedM > CutDistanceM)
		    || (CutAngleDeg > 0.0 && TurnedDeg > CutAngleDeg);
	}

	/**
	 * Re-run the player camera update with the sensor's final pose. The engine
	 * updates player cameras before TG_PostUpdateWork, where CamSim applies
	 * gimbal, FOV and post-process; without this the primary view renders the
	 * previous frame's pose while the KLV describes this frame's. Keeps a
	 * camera cut requested this frame.
	 */
	void RefreshPlayerView(APlayerController* PC, float DeltaSeconds);

	/**
	 * Motion blur explicitly off. Left alone, UE's default (on, amount 0.5)
	 * smears every gimbal slew.
	 */
	void DisableMotionBlur(FPostProcessSettings& PP, FEngineShowFlags& Flags);
}
