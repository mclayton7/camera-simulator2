// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class APlayerController;

namespace CamSimRender
{
	/**
	 * True when the view moved too far in one frame for TSR to reproject
	 * (teleport, origin rebase, view snap). Locations in UE units (cm).
	 */
	inline bool ShouldCutCamera(const FVector& PrevLocCm, const FQuat& PrevRot,
	                            const FVector& CurLocCm, const FQuat& CurRot,
	                            double CutDistanceM, double CutAngleDeg)
	{
		const double MovedM = FVector::Dist(PrevLocCm, CurLocCm) / 100.0;
		const double TurnedDeg = FMath::RadiansToDegrees(PrevRot.AngularDistance(CurRot));
		return MovedM > CutDistanceM || TurnedDeg > CutAngleDeg;
	}

	/**
	 * The view's AutoExposureBias for a compensation relative to the engine's
	 * default bias (r.DefaultFeature.AutoExposure.Bias, +1 in UE 5.8): 0 keeps
	 * UE's metering, -1 is one stop darker. Setting the bias directly would
	 * replace that default, making -1 two stops darker.
	 */
	inline float AutoExposureBias(float EngineDefaultBias, float CompensationEV)
	{
		return EngineDefaultBias + CompensationEV;
	}

	/**
	 * Re-run the player camera update with the sensor's final pose. The engine
	 * updates player cameras before TG_PostUpdateWork, where CamSim applies
	 * gimbal, FOV and post-process; without this the primary view renders the
	 * previous frame's pose while the KLV describes this frame's. Keeps a
	 * camera cut requested this frame.
	 */
	void RefreshPlayerView(APlayerController* PC, float DeltaSeconds);
}
