// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CollisionQueryParams.h"
#include "Entity/SurfaceClamp.h"
#include "Sim/Commands.h"

class UWorld;
class AActor;
class FCamSimGeospatialProvider;

/** Downward surface trace: ellipsoid height of the first surface between Top and Bottom, if any. A bridge deck
 *  (REALISM R3 bridges) counts only at or below DeckCeilingAltM (+inf: any deck, -inf: never a deck). */
class ISurfaceProbe
{
public:
	virtual ~ISurfaceProbe() = default;
	virtual TOptional<double> TraceHeight(double LatDeg, double LonDeg, double TopAltM, double BottomAltM,
		double DeckCeilingAltM) const = 0;
};

/** Traces the rendered Cesium tiles except the buildings tileset (needs create_physics_meshes); other actors are
 *  ignored. With a bridges tileset: one trace of the terrain alone over the span, one of terrain + decks over the span
 *  cut at the deck ceiling; the higher hit wins. */
class FCesiumSurfaceProbe final : public ISurfaceProbe
{
public:
	/** bPhysicsMeshes: create_physics_meshes; when off, the first trace (a Ground/Water clamp) logs a warning. */
	FCesiumSurfaceProbe(UWorld* InWorld, const FCamSimGeospatialProvider* InGeo, bool bPhysicsMeshes);
	virtual TOptional<double> TraceHeight(double LatDeg, double LonDeg, double TopAltM, double BottomAltM,
		double DeckCeilingAltM) const override;

private:
	TWeakObjectPtr<UWorld>           World;
	const FCamSimGeospatialProvider* Geo = nullptr;
	bool                             bPhysicsMeshes = true;
	mutable bool                     bWarnedNoPhysicsMeshes = false;
	mutable TWeakObjectPtr<AActor>   Buildings;
	mutable TWeakObjectPtr<AActor>   Bridges;
	mutable bool                     bLookedUpBuildings = false;

	TOptional<double> TraceOnce(UWorld* W, double LatDeg, double LonDeg, double TopAltM, double BottomAltM,
		bool bIgnoreBridges) const;
};

namespace CamSimSurface
{
	/** Deck ceiling for terrain-only traces (vegetation heights, the camera's height above the terrain). */
	inline constexpr double NoDecks = -TNumericLimits<double>::Max();

	/** The probe's trace parameters: complex collision, and the scene package's buildings tileset (REALISM R3)
	 *  ignored, so ground vehicles clamp to terrain; IgnoredBridges too for the terrain-only trace. */
	FCollisionQueryParams MakeProbeQueryParams(const AActor* IgnoredBuildings, const AActor* IgnoredBridges = nullptr);

	/** The highest bridge deck a trace may take (ISurfaceProbe): Water never (-inf); Ground the last surface (or the
	 *  height it fell from, FClampState::FellFromM) + DeckStepUpM once placed, any deck (+inf) for the first placement
	 *  (REALISM R3 bridges). */
	double DeckCeiling(ESurfaceMode Mode, const FClampState& State);

	/** Trace the surface for Mode and clamp the sender's pose (None returns it unchanged). */
	CamSimFrames::FGeoPose PlaceOnSurface(ESurfaceMode Mode, const CamSimFrames::FGeoPose& Sender,
		double HalfLengthM, double HalfBeamM, double DtSec, const ISurfaceProbe& Probe, FClampState& State,
		const FWaterInput& Water = FWaterInput());
}
