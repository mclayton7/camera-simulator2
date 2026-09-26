// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Earth-Centred Earth-Fixed (WGS-84) conversions, for DIS.
 *
 * ECEF: X through (0N, 0E), Y through (0N, 90E), Z through the north pole.
 * NED:  local North-East-Down at a geodetic point (right-handed).
 *
 * DIS (IEEE 1278.1) gives entity orientation as Euler angles psi, theta, phi
 * (radians): rotations about Z, then Y, then X that take the ECEF axes to the
 * entity body axes (X forward, Y right, Z down). CIGI gives heading, pitch,
 * roll the same way but from local NED. The two differ by the rotation from
 * NED to ECEF at the entity's position.
 */
namespace CamSimFrames
{
	/** Row-major 3x3 rotation. Columns of a "B to A" matrix are B's axes in A. */
	struct FRot3
	{
		double M[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };

		FVector operator*(const FVector& V) const
		{
			return FVector(M[0][0] * V.X + M[0][1] * V.Y + M[0][2] * V.Z,
			               M[1][0] * V.X + M[1][1] * V.Y + M[1][2] * V.Z,
			               M[2][0] * V.X + M[2][1] * V.Y + M[2][2] * V.Z);
		}
		FRot3 operator*(const FRot3& B) const
		{
			FRot3 R;
			for (int32 i = 0; i < 3; ++i)
				for (int32 j = 0; j < 3; ++j)
					R.M[i][j] = M[i][0] * B.M[0][j] + M[i][1] * B.M[1][j] + M[i][2] * B.M[2][j];
			return R;
		}
		FRot3 Transposed() const
		{
			FRot3 R;
			for (int32 i = 0; i < 3; ++i)
				for (int32 j = 0; j < 3; ++j)
					R.M[i][j] = M[j][i];
			return R;
		}

		/** Z-Y-X (yaw, pitch, roll) Euler angles in radians, body to parent frame. */
		static FRot3 FromEulerZYX(double Yaw, double Pitch, double Roll)
		{
			const double cy = FMath::Cos(Yaw),   sy = FMath::Sin(Yaw);
			const double cp = FMath::Cos(Pitch), sp = FMath::Sin(Pitch);
			const double cr = FMath::Cos(Roll),  sr = FMath::Sin(Roll);
			FRot3 R;
			R.M[0][0] = cy * cp; R.M[0][1] = cy * sp * sr - sy * cr; R.M[0][2] = cy * sp * cr + sy * sr;
			R.M[1][0] = sy * cp; R.M[1][1] = sy * sp * sr + cy * cr; R.M[1][2] = sy * sp * cr - cy * sr;
			R.M[2][0] = -sp;     R.M[2][1] = cp * sr;                R.M[2][2] = cp * cr;
			return R;
		}

		/** Inverse of FromEulerZYX (radians). */
		void ToEulerZYX(double& OutYaw, double& OutPitch, double& OutRoll) const
		{
			OutPitch = FMath::Asin(FMath::Clamp(-M[2][0], -1.0, 1.0));
			OutYaw   = FMath::Atan2(M[1][0], M[0][0]);
			OutRoll  = FMath::Atan2(M[2][1], M[2][2]);
		}
	};

	namespace Wgs84
	{
		constexpr double A  = 6378137.0;           // semi-major axis (m)
		constexpr double B  = 6356752.314245;      // semi-minor axis (m)
		constexpr double E2 = 6.69437999014e-3;    // first eccentricity squared
	}

	inline FVector GeodeticToEcef(double LatDeg, double LonDeg, double Alt)
	{
		const double Lat = FMath::DegreesToRadians(LatDeg);
		const double Lon = FMath::DegreesToRadians(LonDeg);
		const double N = Wgs84::A / FMath::Sqrt(1.0 - Wgs84::E2 * FMath::Square(FMath::Sin(Lat)));
		return FVector((N + Alt) * FMath::Cos(Lat) * FMath::Cos(Lon),
		               (N + Alt) * FMath::Cos(Lat) * FMath::Sin(Lon),
		               (N * (1.0 - Wgs84::E2) + Alt) * FMath::Sin(Lat));
	}

	/** ECEF to geodetic (iterative; sub-millimetre after a few iterations). */
	inline void EcefToGeodetic(const FVector& Ecef, double& OutLatDeg, double& OutLonDeg, double& OutAlt)
	{
		const double P = FMath::Sqrt(Ecef.X * Ecef.X + Ecef.Y * Ecef.Y);
		double Lat = FMath::Atan2(Ecef.Z, P * (1.0 - Wgs84::E2));
		for (int32 i = 0; i < 5; ++i)
		{
			const double S = FMath::Sin(Lat);
			const double N = Wgs84::A / FMath::Sqrt(1.0 - Wgs84::E2 * S * S);
			Lat = FMath::Atan2(Ecef.Z + Wgs84::E2 * N * S, P);
		}
		const double S = FMath::Sin(Lat), C = FMath::Cos(Lat);
		const double N = Wgs84::A / FMath::Sqrt(1.0 - Wgs84::E2 * S * S);
		OutAlt    = (FMath::Abs(C) > 1e-10) ? P / C - N : FMath::Abs(Ecef.Z) - Wgs84::B;
		OutLatDeg = FMath::RadiansToDegrees(Lat);
		OutLonDeg = FMath::RadiansToDegrees(FMath::Atan2(Ecef.Y, Ecef.X));
	}

	/** Local NED axes expressed in ECEF (NED to ECEF) at a geodetic point. */
	inline FRot3 NedToEcef(double LatDeg, double LonDeg)
	{
		const double sl = FMath::Sin(FMath::DegreesToRadians(LatDeg)), cl = FMath::Cos(FMath::DegreesToRadians(LatDeg));
		const double so = FMath::Sin(FMath::DegreesToRadians(LonDeg)), co = FMath::Cos(FMath::DegreesToRadians(LonDeg));
		FRot3 R;  // columns: North, East, Down
		R.M[0][0] = -sl * co; R.M[0][1] = -so; R.M[0][2] = -cl * co;
		R.M[1][0] = -sl * so; R.M[1][1] =  co; R.M[1][2] = -cl * so;
		R.M[2][0] =  cl;      R.M[2][1] = 0.0; R.M[2][2] = -sl;
		return R;
	}

	/** An ECEF vector (e.g. DIS world velocity) in local (North, East, Down). */
	inline FVector EcefVectorToNed(const FVector& V, double LatDeg, double LonDeg)
	{
		return NedToEcef(LatDeg, LonDeg).Transposed() * V;
	}

	/**
	 * DIS orientation (psi, theta, phi radians, ECEF-referenced) at a geodetic
	 * point → CIGI heading/pitch/roll in degrees as FRotator(Pitch, Heading, Roll),
	 * heading in [0, 360).
	 */
	inline FRotator DisEulerToCigi(double Psi, double Theta, double Phi, double LatDeg, double LonDeg)
	{
		const FRot3 BodyToNed = NedToEcef(LatDeg, LonDeg).Transposed() * FRot3::FromEulerZYX(Psi, Theta, Phi);
		double H, P, R;
		BodyToNed.ToEulerZYX(H, P, R);
		return FRotator(FMath::RadiansToDegrees(P), FRotator::ClampAxis(FMath::RadiansToDegrees(H)),
		                FMath::RadiansToDegrees(R));
	}

	/** Inverse of DisEulerToCigi: CIGI heading/pitch/roll (degrees) → DIS psi/theta/phi (radians). */
	inline void CigiToDisEuler(double HeadingDeg, double PitchDeg, double RollDeg, double LatDeg, double LonDeg,
		double& OutPsi, double& OutTheta, double& OutPhi)
	{
		const FRot3 BodyToEcef = NedToEcef(LatDeg, LonDeg) * FRot3::FromEulerZYX(
			FMath::DegreesToRadians(HeadingDeg), FMath::DegreesToRadians(PitchDeg), FMath::DegreesToRadians(RollDeg));
		BodyToEcef.ToEulerZYX(OutPsi, OutTheta, OutPhi);
	}
}
