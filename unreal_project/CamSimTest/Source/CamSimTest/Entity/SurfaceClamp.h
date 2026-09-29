// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Geospatial/CigiFrames.h"

/**
 * Surface placement math (pure; the traces are done by ISurfaceProbe).
 * Heights are WGS-84 ellipsoid metres. Angles follow CIGI: pitch nose-up
 * positive, roll right-side-down positive.
 */
namespace CamSimSurface
{
	constexpr double FirstTraceTopM      = 9000.0;
	constexpr double FirstTraceBottomM   = -500.0;
	constexpr double TraceAboveM         = 50.0;   // above the last surface: an overpass stays above
	constexpr double TraceBelowM         = 500.0;
	constexpr double EaseTimeConstantSec = 0.2;    // hides tile refinement shifts
	constexpr double SnapThresholdM      = 5.0;    // first hit, teleports

	struct FClampState
	{
		bool   bHasSurface = false;
		double Height      = 0.0;
		double PitchDeg    = 0.0;
		double RollDeg     = 0.0;
	};

	struct FTraceSpan { double TopM = 0.0; double BottomM = 0.0; };
	FTraceSpan GetTraceSpan(const FClampState& State);

	/** Probe points, in order bow, stern, port, starboard. */
	struct FFootprint { double Lat[4] = {}; double Lon[4] = {}; };
	FFootprint GetFootprint(double LatDeg, double LonDeg, double HeadingDeg, double HalfLengthM, double HalfBeamM);

	struct FGroundHits { TOptional<double> Bow, Stern, Port, Stbd; };

	/** Ground vehicles: height from the hits, pitch/roll from the footprint, heading from the sender. */
	CamSimFrames::FGeoPose ClampGround(const CamSimFrames::FGeoPose& Sender, const FGroundHits& Hits,
		double HalfLengthM, double HalfBeamM, double DtSec, FClampState& State);

	/** Surface vessels: height from the water hit, else EGM96 sea level, else the sender; attitude from the sender. */
	CamSimFrames::FGeoPose ClampWater(const CamSimFrames::FGeoPose& Sender, TOptional<double> CentreHit,
		TOptional<double> SeaLevelM, double DtSec, FClampState& State);
}
