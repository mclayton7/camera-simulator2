// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class FOceanSurface;

/** The still-water sea plane in UE world space (cm, doubles): Point on it, unit Normal = local up. */
struct FStillWaterPlane
{
	bool    bValid = false;
	FVector Point  = FVector::ZeroVector;
	FVector Normal = FVector::UpVector;
};

namespace CamSimGroundTruth
{
	/** Geodetic (deg, deg, m HAE) to UE world (cm); false when unavailable (no georeference). */
	using FGeoToWorldFn = TFunctionRef<bool(double Lat, double Lon, double AltM, FVector& OutWorld)>;

	/**
	 * Ground truth (ROADMAP 2.7, final review I2): the plane InstanceIdCS cuts the submerged hull with. It is the
	 * tangent plane of still water (sea level = EGM96 geoid + CIGI tide, no waves) at the ocean mesh's centre —
	 * the same frame-centre/nadir choice FOceanManager makes (ChooseCentre + LimitCentreToMesh) — normal = local
	 * up. Invalid when there is no sea level there (no geoid grid) or GeoToWorld fails.
	 */
	CAMSIMTEST_API FStillWaterPlane ComputeStillWaterPlane(const FOceanSurface& Ocean, double NadirLat, double NadirLon,
		double CameraAltM, double FcLat, double FcLon, bool bFrameCentreValid, double MaxRadiusKm, FGeoToWorldFn GeoToWorld);
}
