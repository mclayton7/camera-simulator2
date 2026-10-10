// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"

class FLandCoverTileCache;

/** Cost of FLandCoverCodeLookup::Code calls since the last TakeStats (game-thread wall time). */
struct FLandCoverLookupStats
{
	int32  Calls = 0;
	int32  Loads = 0;            // calls that decoded a tile (cache misses)
	int32  Deferred = 0;         // calls answered 0 because the tick's load budget was spent
	double TotalSec = 0.0;
	double MaxHitSec = 0.0;      // slowest call served from the cache
	double MaxLoadSec = 0.0;     // slowest call that decoded a tile
};

/**
 * Point lookup of the land-cover code under a geodetic position (REALISM R2): the material code CIGI HAT/HOT and LOS
 * extended responses report. Uses the same cell GlobalCell + CellToTile pick as the thermal window's resample, through its
 * own tile cache. Game thread. Warnings (a failed Configure, tile-load summaries) collect in TakeWarnings.
 *
 * A cache miss decodes a 600^2 PNG on the game thread (2-4 ms, M1 Pro), so at most MaxLoadsPerTick misses load per tick
 * (BeginTick); a query past that budget answers 0 until a later query loads its tile. MaxTiles (~360 KB each) covers about
 * 0.4 deg square before tiles are evicted.
 */
class CAMSIMTEST_API FLandCoverCodeLookup
{
public:
	static constexpr int32 MaxTiles = 64;
	static constexpr int32 MaxLoadsPerTick = 2;

	FLandCoverCodeLookup();
	~FLandCoverCodeLookup();

	/** Game thread. An empty Dir, or one with no valid index.json, makes every Code() 0 (one warning in TakeWarnings). */
	void Configure(const FString& Dir);
	/** Starts a new tick's tile-load budget (MaxLoadsPerTick). Game thread, once per tick before the queries. */
	void BeginTick() { LoadsThisTick = 0; }
	/** The land-cover code of the cell holding (Lat, Lon), or 0 (no data / no tile / not configured). Game thread. */
	uint8 Code(double LatDeg, double LonDeg);
	bool IsAvailable() const;
	TArray<FString> TakeWarnings();
	/** Timing of the Code() calls since the last call, then resets it. Game thread. */
	FLandCoverLookupStats TakeStats();

private:
	TUniquePtr<FLandCoverTileCache> Cache;
	FString ConfiguredDir;
	TArray<FString> Warnings;
	FLandCoverLookupStats Stats;
	int32 LoadsThisTick = 0;
};

namespace CamSimLandCover
{
	inline constexpr uint8 SeaSurfaceCode = 80;   // WorldCover water
	/** REALISM R2 rule for CIGI HAT/HOT and LOS extended material codes. Lookup may be null (land cover off). */
	CAMSIMTEST_API uint32 QueryMaterialCode(bool bValid, bool bWater, bool bEntityHit, double LatDeg, double LonDeg,
		FLandCoverCodeLookup* Lookup);
}
