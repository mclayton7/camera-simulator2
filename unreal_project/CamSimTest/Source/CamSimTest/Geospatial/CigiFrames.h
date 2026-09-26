// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Orientation conversions between CIGI and Cesium/UE frames.
 *
 * CIGI gives heading (clockwise from true north), pitch (nose up +) and roll
 * (right wing down +), relative to the local horizon at the entity.
 *
 * NEU  — local North-East-Up frame. With UE's left-handed conventions,
 *        FRotator(Pitch, Heading, Roll) is exactly the CIGI orientation here.
 *        Body-frame vectors (X forward, Y right, Z up) rotate into (N, E, U).
 * ESU  — Cesium's local East-South-Up frame, used by
 *        UCesiumGlobeAnchorComponent::SetEastSouthUpRotation(). North is -Y,
 *        so ESU yaw = heading - 90.
 *
 * Never pass CIGI angles to SetActorRotation(): UE world axes only match ESU
 * at the georeference origin and tilt away from it with Earth's curvature.
 */
namespace CamSimFrames
{
	inline FQuat CigiToNeu(double HeadingDeg, double PitchDeg, double RollDeg)
	{
		return FRotator(PitchDeg, HeadingDeg, RollDeg).Quaternion();
	}

	inline FQuat NeuToEastSouthUp(const FQuat& Neu)
	{
		static const FQuat NeuToEsu = FRotator(0.0, -90.0, 0.0).Quaternion();
		return NeuToEsu * Neu;
	}

	inline FQuat EastSouthUpToNeu(const FQuat& Esu)
	{
		static const FQuat EsuToNeu = FRotator(0.0, 90.0, 0.0).Quaternion();
		return EsuToNeu * Esu;
	}

	inline FQuat CigiToEastSouthUp(double HeadingDeg, double PitchDeg, double RollDeg)
	{
		return NeuToEastSouthUp(CigiToNeu(HeadingDeg, PitchDeg, RollDeg));
	}

	/** CIGI heading/pitch/roll as FRotator(Pitch, Heading, Roll), heading in [0, 360). */
	inline FRotator EastSouthUpToCigi(const FQuat& Esu)
	{
		FRotator R = EastSouthUpToNeu(Esu).Rotator();
		R.Yaw = FRotator::ClampAxis(R.Yaw);
		return R;
	}

	/**
	 * CIGI body-frame velocity (X forward, Y right, Z down) to local
	 * (North, East, Up) velocity for an entity with the given NEU orientation.
	 */
	inline FVector BodyVelocityToNeu(const FQuat& Neu, const FVector& BodyForwardRightDown)
	{
		const FVector BodyUp(BodyForwardRightDown.X, BodyForwardRightDown.Y, -BodyForwardRightDown.Z);
		return Neu.RotateVector(BodyUp);
	}
}
