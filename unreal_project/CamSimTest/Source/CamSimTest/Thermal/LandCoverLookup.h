// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"

class FLandCoverTileCache;

/**
 * Point lookup of the land-cover code under a geodetic position (REALISM R2): the material code CIGI HAT/HOT and LOS
 * extended responses report. Uses the same cell GlobalCell + CellToTile pick as the thermal window's resample, through its
 * own small tile cache. Game thread. Warnings (a failed Configure, tile-load summaries) collect in TakeWarnings.
 */
class CAMSIMTEST_API FLandCoverCodeLookup
{
public:
	static constexpr int32 MaxTiles = 16;

	FLandCoverCodeLookup();
	~FLandCoverCodeLookup();

	/** Game thread. An empty Dir, or one with no valid index.json, makes every Code() 0 (one warning in TakeWarnings). */
	void Configure(const FString& Dir);
	/** The land-cover code of the cell holding (Lat, Lon), or 0 (no data / no tile / not configured). Game thread. */
	uint8 Code(double LatDeg, double LonDeg);
	bool IsAvailable() const;
	TArray<FString> TakeWarnings();

private:
	TUniquePtr<FLandCoverTileCache> Cache;
	FString ConfiguredDir;
	TArray<FString> Warnings;
};

namespace CamSimLandCover
{
	inline constexpr uint8 SeaSurfaceCode = 80;   // WorldCover water
	/** REALISM R2 rule for CIGI HAT/HOT and LOS extended material codes. Lookup may be null (land cover off). */
	CAMSIMTEST_API uint32 QueryMaterialCode(bool bValid, bool bWater, bool bEntityHit, double LatDeg, double LonDeg,
		FLandCoverCodeLookup* Lookup);
}
