// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * FGroundSpeedEstimator
 *
 * Ground speed from successive geodetic fixes, using the host's time for each
 * fix rather than the IG's frame time (the two differ whenever the host and
 * IG run at different rates). The last estimate is held between fixes.
 *
 * Fixes with a non-positive time step are ignored; implausible jumps
 * (teleports, or a host switching time bases) reset the estimate to zero.
 */
class FGroundSpeedEstimator
{
public:
	/** Anything faster is treated as a discontinuity, not motion. */
	static constexpr double MaxPlausibleMps = 2000.0;

	void AddFix(double LatDeg, double LonDeg, double TimeSec)
	{
		if (!bHavePrev)
		{
			SetPrev(LatDeg, LonDeg, TimeSec);
			return;
		}
		const double Dt = TimeSec - PrevTimeSec;
		if (Dt <= 0.0) return;  // duplicate or out-of-order fix: hold

		const double Speed = DistanceM(PrevLatDeg, PrevLonDeg, LatDeg, LonDeg) / Dt;
		SpeedMps = (Speed <= MaxPlausibleMps) ? static_cast<float>(Speed) : 0.0f;
		SetPrev(LatDeg, LonDeg, TimeSec);
	}

	float GetSpeedMps() const { return SpeedMps; }

	/** Local-tangent-plane distance between two nearby geodetic points, metres. */
	static double DistanceM(double Lat0Deg, double Lon0Deg, double Lat1Deg, double Lon1Deg)
	{
		constexpr double EarthRadiusM = 6371008.8;  // mean radius
		const double DLat = FMath::DegreesToRadians(Lat1Deg - Lat0Deg);
		double DLonDeg = Lon1Deg - Lon0Deg;
		if (DLonDeg > 180.0) DLonDeg -= 360.0;       // antimeridian
		if (DLonDeg < -180.0) DLonDeg += 360.0;
		const double MeanLat = FMath::DegreesToRadians((Lat0Deg + Lat1Deg) * 0.5);
		const double Dx = FMath::DegreesToRadians(DLonDeg) * FMath::Cos(MeanLat) * EarthRadiusM;
		const double Dy = DLat * EarthRadiusM;
		return FMath::Sqrt(Dx * Dx + Dy * Dy);
	}

private:
	void SetPrev(double LatDeg, double LonDeg, double TimeSec)
	{
		PrevLatDeg = LatDeg; PrevLonDeg = LonDeg; PrevTimeSec = TimeSec;
		bHavePrev = true;
	}

	double PrevLatDeg  = 0.0;
	double PrevLonDeg  = 0.0;
	double PrevTimeSec = 0.0;
	float  SpeedMps    = 0.0f;
	bool   bHavePrev   = false;
};
