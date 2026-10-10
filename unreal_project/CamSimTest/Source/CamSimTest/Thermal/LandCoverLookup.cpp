// Copyright CamSim Contributors. All Rights Reserved.

#include "Thermal/LandCoverLookup.h"
#include "Thermal/LandCoverGeometry.h"
#include "Thermal/LandCoverTiles.h"

FLandCoverCodeLookup::FLandCoverCodeLookup() = default;
FLandCoverCodeLookup::~FLandCoverCodeLookup() = default;

void FLandCoverCodeLookup::Configure(const FString& Dir)
{
	if (Dir == ConfiguredDir && (Cache.IsValid() || Dir.IsEmpty())) return;
	ConfiguredDir = Dir;
	Cache.Reset();
	if (Dir.IsEmpty()) return;
	TUniquePtr<FLandCoverTileCache> C = MakeUnique<FLandCoverTileCache>(Dir, MaxTiles);
	if (!C->IsValid())
	{
		Warnings.Add(FString::Printf(TEXT("CIGI material codes off: land cover %s: %s"), *Dir, *C->GetError()));
		return;
	}
	Cache = MoveTemp(C);
}

bool FLandCoverCodeLookup::IsAvailable() const { return Cache.IsValid(); }

uint8 FLandCoverCodeLookup::Code(double LatDeg, double LonDeg)
{
	if (!Cache.IsValid() || !FMath::IsFinite(LatDeg) || !FMath::IsFinite(LonDeg)) return 0;
	FIntPoint Tile, In;   // Tile = (LatIndex, LonIndex); In = (column, row), row 0 = north
	CamSimLandCover::CellToTile(CamSimLandCover::GlobalCell(LatDeg, LonDeg), Tile, In);
	const TSharedPtr<const FLandCoverTile, ESPMode::ThreadSafe> T = Cache->Get(Tile.X, Tile.Y);
	Warnings.Append(Cache->TakeWarnings());
	if (!T.IsValid()) return 0;
	return T->Codes[In.Y * FLandCoverTileCache::TilePx + In.X];
}

TArray<FString> FLandCoverCodeLookup::TakeWarnings() { return MoveTemp(Warnings); }

uint32 CamSimLandCover::QueryMaterialCode(bool bValid, bool bWater, bool bEntityHit, double LatDeg, double LonDeg,
	FLandCoverCodeLookup* Lookup)
{
	if (!bValid || bEntityHit) return 0;
	if (bWater) return SeaSurfaceCode;
	return Lookup ? Lookup->Code(LatDeg, LonDeg) : 0;
}
