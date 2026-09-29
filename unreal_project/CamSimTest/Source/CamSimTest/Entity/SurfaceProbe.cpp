// Copyright CamSim Contributors. All Rights Reserved.

#include "Entity/SurfaceProbe.h"
#include "Cesium3DTileset.h"
#include "Engine/World.h"
#include "Geospatial/CamSimGeospatialProvider.h"
#include "Geospatial/Geoid.h"

FCesiumSurfaceProbe::FCesiumSurfaceProbe(UWorld* InWorld, const FCamSimGeospatialProvider* InGeo)
	: World(InWorld), Geo(InGeo)
{
}

TOptional<double> FCesiumSurfaceProbe::TraceHeight(double LatDeg, double LonDeg, double TopAltM, double BottomAltM) const
{
	UWorld* W = World.Get();
	if (!W || !Geo) return {};

	FVector Top, Bottom;
	if (!Geo->GeoToWorld(W, LatDeg, LonDeg, TopAltM, Top) || !Geo->GeoToWorld(W, LatDeg, LonDeg, BottomAltM, Bottom))
	{
		return {};
	}

	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(CamSimSurfaceProbe), /*bTraceComplex=*/true);
	if (!W->LineTraceSingleByChannel(Hit, Top, Bottom, ECC_Visibility, Params)) return {};
	if (!Hit.GetActor() || !Hit.GetActor()->IsA<ACesium3DTileset>()) return {};

	double HitLat, HitLon, HitAlt;
	if (!Geo->WorldToGeo(W, Hit.Location, HitLat, HitLon, HitAlt) || !FMath::IsFinite(HitAlt)) return {};
	return HitAlt;
}

namespace CamSimSurface
{
	CamSimFrames::FGeoPose PlaceOnSurface(ESurfaceMode Mode, const CamSimFrames::FGeoPose& Sender,
		double HalfLengthM, double HalfBeamM, double DtSec, const ISurfaceProbe& Probe, FClampState& State)
	{
		if (Mode == ESurfaceMode::None) return Sender;

		const FTraceSpan Span = GetTraceSpan(State);
		if (Mode == ESurfaceMode::Water)
		{
			const TOptional<double> Hit = Probe.TraceHeight(Sender.Lat, Sender.Lon, Span.TopM, Span.BottomM);
			const TOptional<double> Sea = Hit.IsSet() ? TOptional<double>() : CamSim::Geospatial::GetGeoidUndulation(Sender.Lat, Sender.Lon);
			return ClampWater(Sender, Hit, Sea, DtSec, State);
		}

		const FFootprint F = GetFootprint(Sender.Lat, Sender.Lon, Sender.Neu.Rotator().Yaw, HalfLengthM, HalfBeamM);
		FGroundHits Hits;
		Hits.Bow   = Probe.TraceHeight(F.Lat[0], F.Lon[0], Span.TopM, Span.BottomM);
		Hits.Stern = Probe.TraceHeight(F.Lat[1], F.Lon[1], Span.TopM, Span.BottomM);
		Hits.Port  = Probe.TraceHeight(F.Lat[2], F.Lon[2], Span.TopM, Span.BottomM);
		Hits.Stbd  = Probe.TraceHeight(F.Lat[3], F.Lon[3], Span.TopM, Span.BottomM);
		return ClampGround(Sender, Hits, HalfLengthM, HalfBeamM, DtSec, State);
	}
}
