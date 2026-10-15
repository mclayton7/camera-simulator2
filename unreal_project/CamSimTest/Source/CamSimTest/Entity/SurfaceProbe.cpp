// Copyright CamSim Contributors. All Rights Reserved.

#include "Entity/SurfaceProbe.h"
#include "CamSimTest.h"
#include "Cesium3DTileset.h"
#include "Engine/World.h"
#include "Geospatial/CamSimGeospatialProvider.h"
#include "Geospatial/Geoid.h"
#include "Geospatial/SceneBuildings.h"

FCesiumSurfaceProbe::FCesiumSurfaceProbe(UWorld* InWorld, const FCamSimGeospatialProvider* InGeo, bool bInPhysicsMeshes)
	: World(InWorld), Geo(InGeo), bPhysicsMeshes(bInPhysicsMeshes)
{
}

FCollisionQueryParams CamSimSurface::MakeProbeQueryParams(const AActor* IgnoredBuildings, const AActor* IgnoredBridges)
{
	FCollisionQueryParams Params(SCENE_QUERY_STAT(CamSimSurfaceProbe), /*bTraceComplex=*/true);
	if (IgnoredBuildings)
	{
		Params.AddIgnoredActor(IgnoredBuildings);
	}
	if (IgnoredBridges)
	{
		Params.AddIgnoredActor(IgnoredBridges);
	}
	return Params;
}

TOptional<double> FCesiumSurfaceProbe::TraceOnce(UWorld* W, double LatDeg, double LonDeg, double TopAltM,
	double BottomAltM, bool bIgnoreBridges) const
{
	FVector Top, Bottom;
	if (!Geo->GeoToWorld(W, LatDeg, LonDeg, TopAltM, Top) || !Geo->GeoToWorld(W, LatDeg, LonDeg, BottomAltM, Bottom))
	{
		return {};
	}
	FHitResult Hit;
	const FCollisionQueryParams Params = CamSimSurface::MakeProbeQueryParams(
		Buildings.Get(), bIgnoreBridges ? Bridges.Get() : nullptr);
	if (!W->LineTraceSingleByChannel(Hit, Top, Bottom, ECC_Visibility, Params)) return {};
	if (!Hit.GetActor() || !Hit.GetActor()->IsA<ACesium3DTileset>()) return {};

	double HitLat, HitLon, HitAlt;
	if (!Geo->WorldToGeo(W, Hit.Location, HitLat, HitLon, HitAlt) || !FMath::IsFinite(HitAlt)) return {};
	return HitAlt;
}

TOptional<double> FCesiumSurfaceProbe::TraceHeight(double LatDeg, double LonDeg, double TopAltM, double BottomAltM,
	double DeckCeilingAltM) const
{
	if (!bPhysicsMeshes && !bWarnedNoPhysicsMeshes)
	{
		bWarnedNoPhysicsMeshes = true;   // first Ground/Water clamp attempt
		UE_LOG(LogCamSim, Warning, TEXT("SurfaceProbe: create_physics_meshes is off — surface traces can't hit the terrain; ground entities use the sender's height, surface entities EGM96 sea level"));
	}
	UWorld* W = World.Get();
	if (!W || !Geo) return {};

	if (!bLookedUpBuildings || Buildings.IsStale() || Bridges.IsStale())
	{
		Buildings = CamSim::Geospatial::FindBuildingsTileset(W);
		Bridges = CamSim::Geospatial::FindBridgesTileset(W);
		bLookedUpBuildings = true;
	}
	if (!Bridges.IsValid())
	{
		return TraceOnce(W, LatDeg, LonDeg, TopAltM, BottomAltM, /*bIgnoreBridges=*/false);   // no decks: one trace
	}
	// REALISM R3 bridges: the terrain alone over the span, then terrain + decks from the deck ceiling down.
	TOptional<double> Best = TraceOnce(W, LatDeg, LonDeg, TopAltM, BottomAltM, /*bIgnoreBridges=*/true);
	const double DeckTop = FMath::Min(TopAltM, DeckCeilingAltM);
	if (DeckTop > BottomAltM)
	{
		const TOptional<double> WithDecks = TraceOnce(W, LatDeg, LonDeg, DeckTop, BottomAltM, /*bIgnoreBridges=*/false);
		if (WithDecks.IsSet() && (!Best.IsSet() || *WithDecks > *Best)) Best = WithDecks;
	}
	return Best;
}

namespace CamSimSurface
{
	double DeckCeiling(ESurfaceMode Mode, const FClampState& State)
	{
		if (Mode != ESurfaceMode::Ground) return -TNumericLimits<double>::Max();
		return State.bHasSurface ? State.Height + DeckStepUpM : TNumericLimits<double>::Max();
	}

	CamSimFrames::FGeoPose PlaceOnSurface(ESurfaceMode Mode, const CamSimFrames::FGeoPose& Sender,
		double HalfLengthM, double HalfBeamM, double DtSec, const ISurfaceProbe& Probe, FClampState& State,
		const FWaterInput& Water)
	{
		if (Mode == ESurfaceMode::None) return Sender;

		// After a hit the span is narrow (last height +50/−500 m) so an overpass above
		// doesn't capture the entity. If every trace misses there — the surface rose
		// above the span, or the tiles under it were evicted — retry once over the
		// first-trace span so the entity can't stay buried.
		const FTraceSpan Narrow = GetTraceSpan(State);
		const FTraceSpan Full   = { FirstTraceTopM, FirstTraceBottomM };
		if (Mode == ESurfaceMode::Water)
		{
			const double Ceiling = DeckCeiling(Mode, State);   // a boat never takes a deck
			TOptional<double> Hit = Probe.TraceHeight(Sender.Lat, Sender.Lon, Narrow.TopM, Narrow.BottomM, Ceiling);
			if (!Hit.IsSet() && State.bHasSurface)
			{
				Hit = Probe.TraceHeight(Sender.Lat, Sender.Lon, Full.TopM, Full.BottomM, Ceiling);
			}
			const TOptional<double> Sea = Hit.IsSet() || State.bHasSurface
				? TOptional<double>() : CamSim::Geospatial::GetGeoidUndulation(Sender.Lat, Sender.Lon);
			return ClampWater(Sender, Hit, Sea, DtSec, State, Water, HalfLengthM, HalfBeamM);
		}

		const FFootprint F = GetFootprint(Sender.Lat, Sender.Lon, Sender.Neu.Rotator().Yaw, HalfLengthM, HalfBeamM);
		// REALISM R3 bridges: a deck is driven onto from its approach, never taken from below (DeckCeiling).
		const double Ceiling = DeckCeiling(Mode, State);
		auto TraceFootprint = [&F, &Probe, Ceiling](const FTraceSpan& Span)
		{
			FGroundHits Hits;
			Hits.Bow   = Probe.TraceHeight(F.Lat[0], F.Lon[0], Span.TopM, Span.BottomM, Ceiling);
			Hits.Stern = Probe.TraceHeight(F.Lat[1], F.Lon[1], Span.TopM, Span.BottomM, Ceiling);
			Hits.Port  = Probe.TraceHeight(F.Lat[2], F.Lon[2], Span.TopM, Span.BottomM, Ceiling);
			Hits.Stbd  = Probe.TraceHeight(F.Lat[3], F.Lon[3], Span.TopM, Span.BottomM, Ceiling);
			return Hits;
		};
		FGroundHits Hits = TraceFootprint(Narrow);
		const bool bAllMissed = !Hits.Bow.IsSet() && !Hits.Stern.IsSet() && !Hits.Port.IsSet() && !Hits.Stbd.IsSet();
		if (bAllMissed && State.bHasSurface)
		{
			Hits = TraceFootprint(Full);
		}
		return ClampGround(Sender, Hits, HalfLengthM, HalfBeamM, DtSec, State);
	}
}
