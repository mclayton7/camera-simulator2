// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"

class FLandCoverTileCache;

/**
 * Point land-cover codes for the vegetation scatter (REALISM R4 slice): its own MaxTiles LRU (~5.8 MB), so it never
 * spends FLandCoverCodeLookup's per-tick game-thread budget. Same GlobalCell + CellToTile pick as the thermal window and
 * the CIGI lookup. Code() is thread-safe (the scatter calls it on worker tasks); a cache miss decodes on the caller.
 */
class CAMSIMTEST_API FVegetationLandCover
{
public:
	static constexpr int32 MaxTiles = 16;

	/** Game thread (the tile cache loads the ImageWrapper module). Dir as thermal.land_cover.dir (relative = project dir). */
	explicit FVegetationLandCover(const FString& Dir);
	~FVegetationLandCover();

	bool IsValid() const;
	const FString& GetError() const;
	/** The code of the cell holding (Lat, Lon), or 0 (no data, invalid, non-finite). Any thread. */
	uint8 Code(double LatDeg, double LonDeg);
	/** The tile cache's summary warnings since the last call. Any thread. */
	TArray<FString> TakeWarnings();

private:
	TUniquePtr<FLandCoverTileCache> Cache;
	FString Error;
};
