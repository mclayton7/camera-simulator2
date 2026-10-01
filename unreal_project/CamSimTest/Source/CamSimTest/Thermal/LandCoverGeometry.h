// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Geometry of the camera-centred land-cover window (ROADMAP 4B). A window is Texels x Texels texels of TexelM metres on a
 * local East/North grid about its centre; texel (x, y) is at E = (x + 0.5 - Texels/2) TexelM, N = (Texels/2 - y - 0.5) TexelM
 * (row 0 = north). The small-area mapping lat = lat0 + N / M(lat0), lon = lon0 + E / (N(lat0) cos lat0) (radians) is used
 * both by the resample and by FThermalFrameBuilder's camera offset, so the two agree exactly. Pure functions; any thread.
 */
namespace CamSimLandCover
{
	inline constexpr double MaxWindowLatDeg = 89.0;   // windows within 1 deg of a pole fall back to terrain_default
	inline constexpr int32  GlobalRows = 2160000;     // 180 deg * 12000 WorldCover cells per degree
	inline constexpr int32  GlobalCols = 4320000;     // 360 deg * 12000

	struct FWindowSpec
	{
		double CentreLatDeg = 0.0;
		double CentreLonDeg = 0.0;
		int32  Texels = 2048;   // square, even
		float  TexelM = 10.0f;
	};

	/** WGS-84 meridional (M) and prime-vertical (N) radii of curvature, metres. */
	CAMSIMTEST_API double MeridionalRadiusM(double LatDeg);
	CAMSIMTEST_API double PrimeVerticalRadiusM(double LatDeg);
	/** (East, North) metres of a point from the window centre; the longitude difference wraps to (-180, 180]. */
	CAMSIMTEST_API FVector2D GeodeticToWindowEN(const FWindowSpec& W, double LatDeg, double LonDeg);
	/** Inverse of GeodeticToWindowEN (longitude not wrapped; GlobalCell wraps it). */
	CAMSIMTEST_API void WindowENToGeodetic(const FWindowSpec& W, double EastM, double NorthM, double& OutLatDeg, double& OutLonDeg);
	/** Global WorldCover cell: X = column from 180 W (wrapped), Y = row from 90 N (clamped). */
	CAMSIMTEST_API FIntPoint GlobalCell(double LatDeg, double LonDeg);
	/** Tile (LatIndex, LonIndex) holding a global cell, and the cell's (column, row) inside it. */
	CAMSIMTEST_API void CellToTile(FIntPoint Cell, FIntPoint& OutTile, FIntPoint& OutInTile);
	CAMSIMTEST_API bool IsWindowAllowed(double CentreLatDeg);
	/** The camera is more than RecentreFraction of the window size from its centre, East or North. */
	CAMSIMTEST_API bool NeedsRecentre(const FWindowSpec& Current, double CamLatDeg, double CamLonDeg, float RecentreFraction);

	/** A tile's TilePx^2 codes (row 0 = north), or null for no data. */
	using FTileCodes = TFunctionRef<const uint8*(int32 LatIndex, int32 LonIndex)>;
	/** Nearest-cell resample into Texels^2 codes (row 0 = north, column 0 = west); missing tiles give 0. Returns the
	 *  number of nonzero texels. Latitude depends only on the row and longitude only on the column, so each is computed
	 *  once per row / column. */
	CAMSIMTEST_API int64 Resample(const FWindowSpec& W, FTileCodes Tiles, TArray<uint8>& OutCodes);
}
