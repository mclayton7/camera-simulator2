// Copyright CamSim Contributors. All Rights Reserved.

#include "GroundTruth/SeaSurfacePlane.h"
#include "GroundTruth/AnnotationTypes.h"
#include "Ocean/OceanSurface.h"

namespace CamSimGroundTruth
{
	FSeaSurfacePlane ComputeSeaSurfacePlane(const FOceanSurface& Ocean, double Lat, double Lon, FGeoToWorldFn GeoToWorld)
	{
		FSeaSurfacePlane Out;
		if (!FMath::IsFinite(Lat) || !FMath::IsFinite(Lon)) return Out;
		const TOptional<double> Sea = Ocean.SurfaceHeightM(Lat, Lon);
		if (!Sea.IsSet() || !FMath::IsFinite(*Sea)) return Out;
		FVector P, Up;
		if (!GeoToWorld(Lat, Lon, *Sea, P) || !GeoToWorld(Lat, Lon, *Sea + 100.0, Up)) return Out;
		const FVector N = (Up - P).GetSafeNormal();
		if (N.IsNearlyZero()) return Out;
		Out.bValid = true;
		Out.Point  = P;
		Out.Normal = N;
		return Out;
	}

	TArray<FEntityWaterPlane> BuildEntityWaterPlanes(const TArray<FEntityAnnotationData>& Entities,
		const FOceanSurface& Ocean, FGeoToWorldFn GeoToWorld)
	{
		TArray<FEntityWaterPlane> Out;
		for (const FEntityAnnotationData& E : Entities)
		{
			if (!E.bWaterSurface || E.StencilValue == 0 || !E.bHasGeo) continue;
			const FSeaSurfacePlane P = ComputeSeaSurfacePlane(Ocean, E.Lat, E.Lon, GeoToWorld);
			if (P.bValid) Out.Add({ E.StencilValue, P.Point, P.Normal });
		}
		return Out;
	}
}
