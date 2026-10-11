// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Global vegetation cell grid (REALISM R4 slice, docs/vegetation.md). Rows of fixed angular height
 * CellM / 111,320 deg from 90 S; each row has columns of CellM / (111,320 cos(row centre)) deg from 180 W (the last
 * column of a row is narrower). A cell's id depends only on the position, never on the camera. Pure; any thread.
 */
namespace CamSimVegetation
{
	inline constexpr double MetresPerDegLat = 111320.0;
	inline constexpr double MaxLatDeg = 85.0;   // no vegetation closer to the poles

	struct FVegCellId
	{
		int32 Row = 0;
		int32 Col = 0;
		bool operator==(const FVegCellId&) const = default;
		friend uint32 GetTypeHash(const FVegCellId& C) { return HashCombine(::GetTypeHash(C.Row), ::GetTypeHash(C.Col)); }
	};

	struct FVegCellBounds
	{
		double SouthDeg = 0.0, NorthDeg = 0.0, WestDeg = 0.0, EastDeg = 0.0;
	};

	CAMSIMTEST_API bool IsLatAllowed(double LatDeg);
	CAMSIMTEST_API double CellDLatDeg(double CellM);
	CAMSIMTEST_API double CellDLonDeg(int32 Row, double CellM);
	CAMSIMTEST_API int32 NumCols(int32 Row, double CellM);
	CAMSIMTEST_API FVegCellId CellOf(double LatDeg, double LonDeg, double CellM);
	CAMSIMTEST_API FVegCellBounds CellBounds(FVegCellId Id, double CellM);
	CAMSIMTEST_API void CellCentre(FVegCellId Id, double CellM, double& OutLatDeg, double& OutLonDeg);
	/** Equirectangular ground distance (metres); the longitude difference wraps to (-180, 180]. Fine below ~10 km. */
	CAMSIMTEST_API double GroundDistanceM(double Lat1, double Lon1, double Lat2, double Lon2);
	/** Cells whose centre is within RadiusM of the point, nearest first (ties: Row, then Col). Empty past MaxLatDeg. */
	CAMSIMTEST_API TArray<FVegCellId> CellsWithin(double LatDeg, double LonDeg, double RadiusM, double CellM);
}
