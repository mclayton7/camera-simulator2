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
	const double T0 = FPlatformTime::Seconds();
	const int32 Loads0 = Cache->NumLoads();
	FIntPoint Tile, In;   // Tile = (LatIndex, LonIndex); In = (column, row), row 0 = north
	CamSimLandCover::CellToTile(CamSimLandCover::GlobalCell(LatDeg, LonDeg), Tile, In);
	if (!Cache->IsResident(Tile.X, Tile.Y))
	{
		if (LoadsThisTick >= MaxLoadsPerTick)
		{
			++Stats.Calls;
			++Stats.Deferred;
			return 0;
		}
		++LoadsThisTick;
	}
	const TSharedPtr<const FLandCoverTile, ESPMode::ThreadSafe> T = Cache->Get(Tile.X, Tile.Y);
	Warnings.Append(Cache->TakeWarnings());
	const uint8 Result = T.IsValid() ? T->Codes[In.Y * FLandCoverTileCache::TilePx + In.X] : 0;

	const double Dt = FPlatformTime::Seconds() - T0;
	const bool bLoaded = Cache->NumLoads() != Loads0;
	++Stats.Calls;
	Stats.TotalSec += Dt;
	if (bLoaded)
	{
		++Stats.Loads;
		Stats.MaxLoadSec = FMath::Max(Stats.MaxLoadSec, Dt);
	}
	else
	{
		Stats.MaxHitSec = FMath::Max(Stats.MaxHitSec, Dt);
	}
	return Result;
}

FLandCoverLookupStats FLandCoverCodeLookup::TakeStats()
{
	const FLandCoverLookupStats Out = Stats;
	Stats = FLandCoverLookupStats();
	return Out;
}

TArray<FString> FLandCoverCodeLookup::TakeWarnings() { return MoveTemp(Warnings); }

uint32 CamSimLandCover::QueryMaterialCode(bool bValid, bool bWater, bool bEntityHit, bool bBuildingHit,
	double LatDeg, double LonDeg, FLandCoverCodeLookup* Lookup)
{
	if (!bValid || bEntityHit) return 0;
	if (bWater) return SeaSurfaceCode;
	if (bBuildingHit) return BuildingCode;
	return Lookup ? Lookup->Code(LatDeg, LonDeg) : 0;
}
