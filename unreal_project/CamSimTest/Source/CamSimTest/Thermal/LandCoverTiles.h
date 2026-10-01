// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

class IImageWrapperModule;

/** One 0.05 x 0.05 degree land-cover tile: WorldCover codes, row 0 = north edge, column 0 = west edge (ROADMAP 4B). */
struct FLandCoverTile
{
	int32 LatIndex = 0;     // south edge = 0.05 * LatIndex degrees
	int32 LonIndex = 0;     // west edge  = 0.05 * LonIndex degrees
	TArray<uint8> Codes;    // FLandCoverTileCache::TilePx^2
};

/** index.json of a land-cover directory (scripts/landcover/fetch_worldcover.py). */
struct FLandCoverIndex
{
	FString Attribution;
	FString Licence;
	TMap<FIntPoint, FString> Files;   // (LatIndex, LonIndex) -> file name inside the directory
};

/**
 * Local land-cover tiles (ROADMAP 4B): index.json plus 8-bit greyscale PNGs whose values are WorldCover codes, decoded
 * with ImageWrapper and kept in an LRU of MaxTiles. Get() is thread-safe (the window build calls it on a task thread).
 * A tile listed in the index whose file is missing, not a PNG (e.g. a git LFS pointer) or the wrong size is reported once
 * (TakeWarnings) and treated as no data from then on. No network: only Dir is read.
 */
class CAMSIMTEST_API FLandCoverTileCache
{
public:
	static constexpr int32  TilePx      = 600;       // 0.05 deg at 1/12000 deg per WorldCover cell
	static constexpr double TileDeg     = 0.05;
	static constexpr double CellsPerDeg = 12000.0;

	/** Parses index.json text. Wrong format / tile_deg / tile_px or a malformed entry fails with OutError. */
	static bool ParseIndex(const FString& Json, FLandCoverIndex& Out, FString& OutError);
	/** Decodes a PNG into TilePx^2 codes; false (with OutError) unless it is an 8-bit greyscale PNG of that size. */
	static bool DecodeTilePng(IImageWrapperModule& Module, TConstArrayView<uint8> Png, TArray<uint8>& OutCodes, FString& OutError);
	/** Absolute paths are kept; relative ones are taken from the project directory (Content/NonUFS/... stages as loose files). */
	static FString ResolveDir(const FString& Dir);

	/** Game thread (loads the ImageWrapper module). An unreadable or invalid index leaves the cache invalid (GetError). */
	explicit FLandCoverTileCache(const FString& Dir, int32 MaxTiles = 64);

	bool IsValid() const { return bValid; }
	const FString& GetDir() const { return Dir; }
	const FString& GetError() const { return Error; }
	const FLandCoverIndex& GetIndex() const { return Index; }
	bool HasTile(int32 LatIndex, int32 LonIndex) const { return Index.Files.Contains(FIntPoint(LatIndex, LonIndex)); }

	/** The tile, or null when it is not in the index or failed to load. Any thread. */
	TSharedPtr<const FLandCoverTile, ESPMode::ThreadSafe> Get(int32 LatIndex, int32 LonIndex);
	/** Warnings since the last call (each failed tile once). Any thread. */
	TArray<FString> TakeWarnings();
	int32 NumCached() const;
	/** Decodes so far (cache hits don't count). */
	int32 NumLoads() const;

private:
	struct FEntry
	{
		TSharedPtr<const FLandCoverTile, ESPMode::ThreadSafe> Tile;
		uint64 LastUse = 0;
	};

	FString Dir;
	FString Error;
	bool    bValid = false;
	int32   MaxTiles = 64;
	IImageWrapperModule* ImageWrapper = nullptr;
	FLandCoverIndex Index;

	mutable FCriticalSection Lock;   // guards everything below
	TMap<FIntPoint, FEntry> Cache;
	TSet<FIntPoint> Failed;
	TArray<FString> Warnings;
	uint64 UseCounter = 0;
	int32  Loads = 0;
};
