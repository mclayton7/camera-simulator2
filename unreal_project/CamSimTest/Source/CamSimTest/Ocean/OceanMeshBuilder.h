// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FOceanSurface;

/**
 * Pure (no UObject) builder for the drawn sea: one (N+1)^2 warped grid
 * centred on a point, vertices on the WGS-84 ellipsoid + EGM96 geoid.
 * Task 9 uploads the result to a UProceduralMeshComponent; Task 8's
 * material reads UV1.x (FOceanMeshData::CellSize) as the cell size in
 * metres for its ripple/foam tiling.
 */
namespace CamSimOcean
{
	/** Grid resolution: (GridN+1)^2 vertices, GridN^2 quads. */
	constexpr int32 GridN = 256;
	/** Target cell size at the grid centre, in metres. */
	constexpr double CentreCellM = 2.0;

	/**
	 * Solve for the exponential warp rate alpha such that the centre cell of a
	 * radius-RadiusM, N-wide grid is CentreCellM across. Returns 0 (uniform
	 * grid) when a uniform grid's cell is already <= CentreCellM.
	 */
	double SolveWarpAlpha(double RadiusM, int32 N = GridN, double CentreCellM = 2.0);

	/**
	 * Warp a normalised grid coordinate U in [-1, 1] to a signed distance in
	 * metres from the grid centre, out to +/-RadiusM at U = +/-1. Alpha = 0 is
	 * a uniform (linear) mapping; increasing alpha packs more cells near the
	 * centre. Odd in U.
	 */
	double WarpDistance(double U, double RadiusM, double Alpha);

	/**
	 * Radius (metres) from the sensor's nadir point out to the visible sea
	 * horizon, plus the offset from the grid centre to that nadir point,
	 * capped at MaxRadiusKm. AltAboveSeaM is floored at 2 m so a sensor at or
	 * below sea level still gets a usable horizon distance.
	 */
	double HorizonRadiusM(double CentreToNadirM, double AltAboveSeaM, double MaxRadiusKm);

	/** The built sea mesh, in a form ready for UProceduralMeshComponent::CreateMeshSection. */
	struct FOceanMeshData
	{
		FVector OriginWorld = FVector::ZeroVector;   // UE world (cm) of the grid centre vertex
		TArray<FVector> Positions;                   // relative to OriginWorld, UE world (cm)
		TArray<int32> Triangles;
		TArray<FVector2D> CellSize;                  // X = local cell size (m); UV1.x for the material
		TArray<FVector> Normals;
		double CentreLat = 0.0, CentreLon = 0.0, RadiusM = 0.0, Alpha = 0.0;
	};

	/** Converts geodetic (Lat deg, Lon deg, AltM above the WGS-84 ellipsoid) to UE world position, cm. */
	using FGeoToWorldFn = TFunction<FVector(double Lat, double Lon, double AltM)>;

	/**
	 * Build a warped (N+1)^2 grid of RadiusM around (CentreLat, CentreLon),
	 * with vertices on the ellipsoid + Ocean's sea level (geoid + tide) at
	 * each vertex's geodetic position. Returns false and leaves Out untouched
	 * if Ocean has no sea level at the centre point (e.g. no geoid grid loaded).
	 */
	bool BuildOceanMesh(double CentreLat, double CentreLon, double RadiusM, const FOceanSurface& Ocean,
		const FGeoToWorldFn& GeoToWorld, FOceanMeshData& Out);

	/** Last-built mesh parameters, for deciding whether a rebuild is needed. */
	struct FRebuildPolicy
	{
		double LastLat = 0.0, LastLon = 0.0, LastRadiusM = 0.0;
		bool bHasMesh = false;
	};

	/**
	 * True if the mesh should be rebuilt: no mesh yet, the centre has moved
	 * more than 0.5% of the last radius, or the radius has changed by more
	 * than 25%.
	 */
	bool NeedsRebuild(const FRebuildPolicy& Last, double Lat, double Lon, double RadiusM);

	/** Mesh centre: the frame centre when valid (finite, set), else the nadir. */
	CAMSIMTEST_API void ChooseCentre(double NadirLat, double NadirLon, double FcLat, double FcLon, bool bFrameCentreValid, double& OutLat, double& OutLon);
	/** Wave anchor moves only when unset, on a teleport, or > 200 km from the centre (a one-off re-phase). */
	CAMSIMTEST_API bool NeedsReanchor(bool bHasAnchor, double AnchorLat, double AnchorLon, double Lat, double Lon, bool bTeleport);
}
