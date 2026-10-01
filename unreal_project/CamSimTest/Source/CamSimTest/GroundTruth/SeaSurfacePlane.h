// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class FOceanSurface;

/** A horizontal water plane in UE world space (cm, doubles): Point on it, unit Normal = local up. */
struct FSeaSurfacePlane
{
	bool    bValid = false;
	FVector Point  = FVector::ZeroVector;
	FVector Normal = FVector::UpVector;
};

/** The plane InstanceIdCS cuts one entity's hidden silhouette with (UE world, doubles). */
struct FEntityWaterPlane
{
	uint8   Stencil = 0;
	FVector Point   = FVector::ZeroVector;
	FVector Normal  = FVector::UpVector;
};

namespace CamSimGroundTruth
{
	/** Geodetic (deg, deg, m HAE) to UE world (cm); false when unavailable (no georeference). */
	using FGeoToWorldFn = TFunctionRef<bool(double Lat, double Lon, double AltM, FVector& OutWorld)>;

	/**
	 * Ground truth (ROADMAP 2.7, final review I2): the water plane under one entity — the tangent plane of the
	 * sea surface (EGM96 geoid + CIGI tide + the active waves, FOceanSurface::SurfaceHeightM: the same surface a
	 * floating boat is placed on) at the entity's position, normal = local up. InstanceIdCS drops the entity's
	 * hidden silhouette pixels below it (its submerged hull). Invalid when there is no sea surface there (no
	 * geoid grid) or GeoToWorld fails.
	 */
	CAMSIMTEST_API FSeaSurfacePlane ComputeSeaSurfacePlane(const FOceanSurface& Ocean, double Lat, double Lon,
		FGeoToWorldFn GeoToWorld);
}
