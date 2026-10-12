// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"
#include "Templates/UniquePtr.h"

class FLandCoverTileCache;

struct FCanopySample
{
	float CoverFrac = 0.0f;   // 0..1
	float HeightM = 0.0f;
};

/**
 * Canopy cover and height for the vegetation scatter (REALISM R4, docs/vegetation.md): the scene package's canopy/cover
 * and canopy/height tile sets (camsim-canopy-1, the land-cover grid), each in its own FLandCoverTileCache of MaxTiles.
 * Sample() is thread-safe (the scatter calls it on worker tasks); a cache miss decodes on the caller. A bad tile reads as
 * no canopy there (one summary warning per TakeWarnings, as the land cover's).
 */
class CAMSIMTEST_API FVegetationCanopy
{
public:
	static constexpr int32 MaxTiles = 16;

	/** Game thread (the tile caches load the ImageWrapper module). CanopyDir holds cover/ and height/. */
	explicit FVegetationCanopy(const FString& CanopyDir);
	~FVegetationCanopy();

	bool IsValid() const;
	const FString& GetError() const;
	/** Unset where either set has no tile for the point (outside the package bbox), a tile is unreadable, or the point is
	 *  not finite. Any thread. */
	TOptional<FCanopySample> Sample(double LatDeg, double LonDeg);
	/** Both caches' summary warnings since the last call. Any thread. */
	TArray<FString> TakeWarnings();

private:
	TUniquePtr<FLandCoverTileCache> Cover;
	TUniquePtr<FLandCoverTileCache> Height;
	FString Error;
};
