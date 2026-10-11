// Copyright CamSim Contributors. All Rights Reserved.

#include "Vegetation/VegetationLandCover.h"

#include "Thermal/LandCoverGeometry.h"
#include "Thermal/LandCoverTiles.h"

FVegetationLandCover::FVegetationLandCover(const FString& Dir)
{
	if (Dir.IsEmpty()) { Error = TEXT("no land-cover directory"); return; }
	Cache = MakeUnique<FLandCoverTileCache>(FLandCoverTileCache::ResolveDir(Dir), MaxTiles);
	if (!Cache->IsValid()) { Error = Cache->GetError(); }
}

FVegetationLandCover::~FVegetationLandCover() = default;

bool FVegetationLandCover::IsValid() const { return Cache.IsValid() && Cache->IsValid(); }

const FString& FVegetationLandCover::GetError() const { return Error; }

uint8 FVegetationLandCover::Code(double LatDeg, double LonDeg)
{
	if (!IsValid() || !FMath::IsFinite(LatDeg) || !FMath::IsFinite(LonDeg)) return 0;
	FIntPoint Tile, In;   // Tile = (LatIndex, LonIndex); In = (column, row), row 0 = north
	CamSimLandCover::CellToTile(CamSimLandCover::GlobalCell(LatDeg, LonDeg), Tile, In);
	const TSharedPtr<const FLandCoverTile, ESPMode::ThreadSafe> T = Cache->Get(Tile.X, Tile.Y);
	return T.IsValid() ? T->Codes[In.Y * FLandCoverTileCache::TilePx + In.X] : 0;
}

TArray<FString> FVegetationLandCover::TakeWarnings()
{
	return Cache.IsValid() ? Cache->TakeWarnings() : TArray<FString>();
}
