// Copyright CamSim Contributors. All Rights Reserved.

#include "Vegetation/VegetationCanopy.h"

#include "Misc/Paths.h"
#include "Thermal/LandCoverGeometry.h"
#include "Thermal/LandCoverTiles.h"

FVegetationCanopy::FVegetationCanopy(const FString& CanopyDir)
{
	if (CanopyDir.IsEmpty()) { Error = TEXT("no canopy directory"); return; }
	Cover = MakeUnique<FLandCoverTileCache>(FPaths::Combine(CanopyDir, TEXT("cover")), MaxTiles, TEXT("camsim-canopy-1"));
	Height = MakeUnique<FLandCoverTileCache>(FPaths::Combine(CanopyDir, TEXT("height")), MaxTiles, TEXT("camsim-canopy-1"));
	if (!Cover->IsValid()) Error = Cover->GetError();
	else if (!Height->IsValid()) Error = Height->GetError();
}

FVegetationCanopy::~FVegetationCanopy() = default;

bool FVegetationCanopy::IsValid() const { return Cover.IsValid() && Height.IsValid() && Cover->IsValid() && Height->IsValid(); }

const FString& FVegetationCanopy::GetError() const { return Error; }

TOptional<FCanopySample> FVegetationCanopy::Sample(double LatDeg, double LonDeg)
{
	if (!IsValid() || !FMath::IsFinite(LatDeg) || !FMath::IsFinite(LonDeg)) return {};
	FIntPoint Tile, In;   // Tile = (LatIndex, LonIndex); In = (column, row), row 0 = north
	CamSimLandCover::CellToTile(CamSimLandCover::GlobalCell(LatDeg, LonDeg), Tile, In);
	const TSharedPtr<const FLandCoverTile, ESPMode::ThreadSafe> C = Cover->Get(Tile.X, Tile.Y);
	const TSharedPtr<const FLandCoverTile, ESPMode::ThreadSafe> H = C.IsValid() ? Height->Get(Tile.X, Tile.Y) : nullptr;
	if (!C.IsValid() || !H.IsValid()) return {};
	const int32 K = In.Y * FLandCoverTileCache::TilePx + In.X;
	if (C->Codes[K] == NoDataCover) return {};   // no CHM pixel there: the land-cover rule, as outside the layer
	FCanopySample S;
	S.CoverFrac = FMath::Min<int32>(C->Codes[K], 100) / 100.0f;
	S.HeightM = static_cast<float>(H->Codes[K]);
	return S;
}

TArray<FString> FVegetationCanopy::TakeWarnings()
{
	TArray<FString> W;
	if (Cover.IsValid()) W.Append(Cover->TakeWarnings());
	if (Height.IsValid()) W.Append(Height->TakeWarnings());
	return W;
}
