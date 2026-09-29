// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Entity/SurfaceClamp.h"
#include "Sim/Commands.h"

class UWorld;
class FCamSimGeospatialProvider;

/** Downward surface trace: ellipsoid height of the first surface between Top and Bottom, if any. */
class ISurfaceProbe
{
public:
	virtual ~ISurfaceProbe() = default;
	virtual TOptional<double> TraceHeight(double LatDeg, double LonDeg, double TopAltM, double BottomAltM) const = 0;
};

/** Traces the rendered Cesium tiles (needs create_physics_meshes); other actors are ignored. */
class FCesiumSurfaceProbe final : public ISurfaceProbe
{
public:
	FCesiumSurfaceProbe(UWorld* InWorld, const FCamSimGeospatialProvider* InGeo);
	virtual TOptional<double> TraceHeight(double LatDeg, double LonDeg, double TopAltM, double BottomAltM) const override;

private:
	TWeakObjectPtr<UWorld>           World;
	const FCamSimGeospatialProvider* Geo = nullptr;
};

namespace CamSimSurface
{
	/** Trace the surface for Mode and clamp the sender's pose (None returns it unchanged). */
	CamSimFrames::FGeoPose PlaceOnSurface(ESurfaceMode Mode, const CamSimFrames::FGeoPose& Sender,
		double HalfLengthM, double HalfBeamM, double DtSec, const ISurfaceProbe& Probe, FClampState& State);
}
