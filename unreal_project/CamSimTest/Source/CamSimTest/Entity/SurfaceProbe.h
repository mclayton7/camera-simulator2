// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CollisionQueryParams.h"
#include "Entity/SurfaceClamp.h"
#include "Sim/Commands.h"

class UWorld;
class AActor;
class FCamSimGeospatialProvider;

/** Downward surface trace: ellipsoid height of the first surface between Top and Bottom, if any. */
class ISurfaceProbe
{
public:
	virtual ~ISurfaceProbe() = default;
	virtual TOptional<double> TraceHeight(double LatDeg, double LonDeg, double TopAltM, double BottomAltM) const = 0;
};

/** Traces the rendered Cesium tiles except the buildings tileset (needs create_physics_meshes); other actors are ignored. */
class FCesiumSurfaceProbe final : public ISurfaceProbe
{
public:
	/** bPhysicsMeshes: create_physics_meshes; when off, the first trace (a Ground/Water clamp) logs a warning. */
	FCesiumSurfaceProbe(UWorld* InWorld, const FCamSimGeospatialProvider* InGeo, bool bPhysicsMeshes);
	virtual TOptional<double> TraceHeight(double LatDeg, double LonDeg, double TopAltM, double BottomAltM) const override;

private:
	TWeakObjectPtr<UWorld>           World;
	const FCamSimGeospatialProvider* Geo = nullptr;
	bool                             bPhysicsMeshes = true;
	mutable bool                     bWarnedNoPhysicsMeshes = false;
	mutable TWeakObjectPtr<AActor>   Buildings;
	mutable bool                     bLookedUpBuildings = false;
};

namespace CamSimSurface
{
	/** The probe's trace parameters: complex collision, and the scene package's buildings tileset (REALISM R3)
	 *  ignored, so ground vehicles clamp to terrain (bridges, when they come, are a separate tileset). */
	FCollisionQueryParams MakeProbeQueryParams(const AActor* IgnoredBuildings);

	/** Trace the surface for Mode and clamp the sender's pose (None returns it unchanged). */
	CamSimFrames::FGeoPose PlaceOnSurface(ESurfaceMode Mode, const CamSimFrames::FGeoPose& Sender,
		double HalfLengthM, double HalfBeamM, double DtSec, const ISurfaceProbe& Probe, FClampState& State,
		const FWaterInput& Water = FWaterInput());
}
