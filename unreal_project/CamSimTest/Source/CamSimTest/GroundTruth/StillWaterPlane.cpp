// Copyright CamSim Contributors. All Rights Reserved.

#include "GroundTruth/StillWaterPlane.h"
#include "Ocean/OceanSurface.h"
#include "Ocean/OceanMeshBuilder.h"

namespace CamSimGroundTruth
{
	FStillWaterPlane ComputeStillWaterPlane(const FOceanSurface& Ocean, double NadirLat, double NadirLon,
		double CameraAltM, double FcLat, double FcLon, bool bFrameCentreValid, double MaxRadiusKm, FGeoToWorldFn GeoToWorld)
	{
		FStillWaterPlane Out;
		if (!FMath::IsFinite(NadirLat) || !FMath::IsFinite(NadirLon) || !FMath::IsFinite(CameraAltM)) return Out;
		// As FOceanManager::Tick: the centre limit uses the camera's height above the sea at its nadir.
		const double AltAboveSea = CameraAltM - Ocean.SeaLevelM(NadirLat, NadirLon).Get(0.0);
		double Lat = NadirLat, Lon = NadirLon;
		CamSimOcean::ChooseCentre(NadirLat, NadirLon, FcLat, FcLon, bFrameCentreValid, Lat, Lon);
		CamSimOcean::LimitCentreToMesh(NadirLat, NadirLon, AltAboveSea, MaxRadiusKm, Lat, Lon);
		const TOptional<double> Sea = Ocean.SeaLevelM(Lat, Lon);
		if (!Sea.IsSet()) return Out;
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
