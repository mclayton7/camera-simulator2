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

	/** Geodetic position plus orientation in the local NEU frame. */
	struct FGeoPose
	{
		double Lat = 0.0;  // degrees
		double Lon = 0.0;  // degrees
		double Alt = 0.0;  // metres above the WGS-84 ellipsoid
		FQuat  Neu = FQuat::Identity;
	};

	/** Move a geodetic point by a local (North, East, Up) displacement in metres. */
	inline void OffsetGeodetic(double Lat, double Lon, double Alt, const FVector& NeuM,
	                           double& OutLat, double& OutLon, double& OutAlt)
	{
		constexpr double A  = 6378137.0;           // WGS-84 semi-major axis
		constexpr double E2 = 6.69437999014e-3;    // first eccentricity squared
		const double LatRad = FMath::DegreesToRadians(Lat);
		const double S2     = FMath::Square(FMath::Sin(LatRad));
		const double W      = FMath::Sqrt(1.0 - E2 * S2);
		const double Rm     = A * (1.0 - E2) / (W * W * W);   // meridional radius
		const double Rn     = A / W;                          // prime-vertical radius
		OutLat = Lat + FMath::RadiansToDegrees(NeuM.X / (Rm + Alt));
		OutLon = Lon + FMath::RadiansToDegrees(NeuM.Y / ((Rn + Alt) * FMath::Max(FMath::Cos(LatRad), 1e-9)));
		OutAlt = Alt + NeuM.Z;
	}

	/** A point given in an entity's body frame (X forward, Y right, Z down), in geodetic. */
	inline void BodyOffsetToGeodetic(const FGeoPose& Entity, const FVector& OffsetFrdM,
	                                 double& OutLat, double& OutLon, double& OutAlt)
	{
		OffsetGeodetic(Entity.Lat, Entity.Lon, Entity.Alt,
			BodyVelocityToNeu(Entity.Neu, OffsetFrdM), OutLat, OutLon, OutAlt);
	}

	/**
	 * Pose of a CIGI child entity (Attach State = Attach): offset in the parent's
	 * body frame, and yaw/pitch/roll relative to the parent's axes.
	 */
	inline FGeoPose AttachedChildPose(const FGeoPose& Parent, const FVector& OffsetFrdM,
	                                  double RelYawDeg, double RelPitchDeg, double RelRollDeg)
	{
		FGeoPose Child;
		BodyOffsetToGeodetic(Parent, OffsetFrdM, Child.Lat, Child.Lon, Child.Alt);
		Child.Neu = Parent.Neu * FRotator(RelPitchDeg, RelYawDeg, RelRollDeg).Quaternion();
		return Child;
	}

	/**
	 * One dead-reckoning step for CIGI Rate Control (ICD 3.3 section 4.1.8).
	 *
	 * Linear rates are metres/second and angular rates degrees/second, as
	 * (X, Y, Z) and (Roll, Pitch, Yaw). With bLocalFrame (Coordinate System =
	 * Local, and always for CIGI 3.0/3.1) they are in the entity's body frame:
	 * X forward, Y right, Z down, rotating about the body axes. Otherwise
	 * (World/Parent, the 3.2+ default, for a top-level entity) the linear rates
	 * are North/East/Down along the geoid and the angular rates are rates of
	 * change of heading, pitch and roll.
	 */
	inline void IntegrateRates(FGeoPose& Pose, const FVector& LinearRate, const FVector& RollPitchYawRate,
	                           bool bLocalFrame, double Dt)
	{
		FVector NeuVelocity;
		if (bLocalFrame)
		{
			// Axis-angle on the quaternion avoids the FRotator gimbal-lock
			// singularity at pitch = +/-90 deg. Omega axes match UE's FRotator
			// convention: X = roll, Y = pitch, Z = yaw.
			const FVector Omega(
				FMath::DegreesToRadians(RollPitchYawRate.X),
				FMath::DegreesToRadians(RollPitchYawRate.Y),
				FMath::DegreesToRadians(RollPitchYawRate.Z));
			const double OmegaMag = Omega.Size();
			if (OmegaMag > UE_SMALL_NUMBER)
			{
				Pose.Neu = (Pose.Neu * FQuat(Omega / OmegaMag, OmegaMag * Dt)).GetNormalized();
			}
			NeuVelocity = BodyVelocityToNeu(Pose.Neu, LinearRate);
		}
		else
		{
			FRotator Hpr = Pose.Neu.Rotator();
			Hpr.Roll  += RollPitchYawRate.X * Dt;
			Hpr.Pitch += RollPitchYawRate.Y * Dt;
			Hpr.Yaw   += RollPitchYawRate.Z * Dt;
			Pose.Neu = Hpr.Quaternion();
			NeuVelocity = FVector(LinearRate.X, LinearRate.Y, -LinearRate.Z);
		}
		OffsetGeodetic(Pose.Lat, Pose.Lon, Pose.Alt, NeuVelocity * Dt, Pose.Lat, Pose.Lon, Pose.Alt);
	}

	/**
	 * An entity-relative LOS vector (azimuth from the entity's +X axis, positive
	 * elevation towards its -Z/up axis) as true-north azimuth and elevation.
	 */
	inline void BodyAzElToTrueAzEl(const FQuat& EntityNeu, double AzDeg, double ElDeg,
	                               double& OutAzDeg, double& OutElDeg)
	{
		const double Az = FMath::DegreesToRadians(AzDeg);
		const double El = FMath::DegreesToRadians(ElDeg);
		const FVector BodyFrd(FMath::Cos(El) * FMath::Cos(Az), FMath::Cos(El) * FMath::Sin(Az), -FMath::Sin(El));
		const FVector Neu = BodyVelocityToNeu(EntityNeu, BodyFrd);
		OutAzDeg = FRotator::ClampAxis(FMath::RadiansToDegrees(FMath::Atan2(Neu.Y, Neu.X)));
		OutElDeg = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(Neu.Z, -1.0, 1.0)));
	}
}
