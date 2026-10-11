// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

class IImageWrapperModule;

/**
 * Building footprint mask for the tree scatter (REALISM R3b, docs/vegetation.md): the scene package's
 * features/buildings/mask/ (camsim-buildings-mask-1: the land-cover tile grid at 8 x its resolution, 4800^2 1-bit PNGs,
 * row 0 = north, a cell set where its centre is inside a footprint). Tiles decode on the caller (the scatter worker) into
 * bitsets (~2.9 MB) held in a MaxTiles LRU (~23 MB), separate from the land-cover sampler. A tile missing from the index,
 * missing on disk or unreadable (a git LFS pointer) means "no buildings" there; each such tile logs one warning, once.
 * IsMasked / IsClear are thread-safe. No network.
 */
class CAMSIMTEST_API FVegetationBuildingMask
{
public:
	static constexpr int32  TilePx = 4800;
	static constexpr double TileDeg = 0.05;
	static constexpr double CellsPerDeg = 96000.0;   // 12000 WorldCover cells per degree x 8
	static constexpr int32  MaxTiles = 8;
	static constexpr int32  WordsPerTile = TilePx * TilePx / 64;   // 360000 uint64

	static bool ParseIndex(const FString& Json, TMap<FIntPoint, FString>& OutFiles, FString& OutError);
	/** 1-bit (as written) or 8-bit greyscale TilePx^2 PNG -> bitset, bit (row * TilePx + column); nonzero = building. */
	static bool DecodeMaskPng(IImageWrapperModule& Module, TConstArrayView<uint8> Png, TArray<uint64>& OutBits, FString& OutError);
	/** The mask cell holding (Lat, Lon): OutTile = (LatIndex, LonIndex), OutInTile = (column, row), as camsim-scene grids it. */
	static void CellOf(double LatDeg, double LonDeg, FIntPoint& OutTile, FIntPoint& OutInTile);
	/** The 5-point rule: clear when neither the point nor the 4 points ClearanceM east, west, north and south of it are
	 *  masked. Axis probes only: a footprint corner diagonally within ClearanceM is not seen. */
	static bool IsClearOf(double LatDeg, double LonDeg, double ClearanceM, TFunctionRef<bool(double, double)> IsMaskedAt);

	/** Game thread (loads the ImageWrapper module). Dir = Cfg.Scene.BuildingsMaskDir (relative = project dir). */
	explicit FVegetationBuildingMask(const FString& Dir);

	bool IsValid() const { return bValid; }
	const FString& GetError() const { return Error; }
	/** Any thread. False with an invalid mask, a non-finite point or no tile there. */
	bool IsMasked(double LatDeg, double LonDeg);
	/** Any thread. IsClearOf with this mask. */
	bool IsClear(double LatDeg, double LonDeg, double ClearanceM);
	int32 NumCached() const;
	/** Tiles decoded and kept so far (hits and a concurrent duplicate decode don't count). */
	int32 NumLoads() const;

private:
	struct FTile
	{
		TArray<uint64> Bits;   // WordsPerTile
	};
	struct FEntry
	{
		TSharedPtr<const FTile, ESPMode::ThreadSafe> Tile;
		uint64 LastUse = 0;
	};

	TSharedPtr<const FTile, ESPMode::ThreadSafe> Get(FIntPoint Key);

	FString Dir;
	FString Error;
	bool    bValid = false;
	IImageWrapperModule* ImageWrapper = nullptr;
	TMap<FIntPoint, FString> Files;   // immutable after construction

	mutable FCriticalSection Lock;    // guards everything below; never held across file I/O or decode
	TMap<FIntPoint, FEntry> Cache;
	TSet<FIntPoint> Failed;           // indexed tiles that could not be read: no buildings, warned once
	uint64 UseCounter = 0;
	int32  Loads = 0;
};
