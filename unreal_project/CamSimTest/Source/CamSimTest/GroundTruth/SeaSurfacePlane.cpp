// Copyright CamSim Contributors. All Rights Reserved.

#include "GroundTruth/SeaSurfacePlane.h"
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
}
