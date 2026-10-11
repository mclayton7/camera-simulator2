// Copyright CamSim Contributors. All Rights Reserved.

#include "Vegetation/VegetationBuildingMask.h"

#include "CamSimTest.h"
#include "Dom/JsonObject.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Thermal/LandCoverGeometry.h"
#include "Thermal/LandCoverTiles.h"

namespace CamSimBuildingMaskDetail
{
	const TCHAR* const MaskFormat = TEXT("camsim-buildings-mask-1");
	constexpr int32  Px = FVegetationBuildingMask::TilePx;
	constexpr int32  GlobalRows = 17280000;   // 180 deg x 96000
	constexpr int32  GlobalCols = 34560000;   // 360 deg x 96000
	constexpr int32  TileRowsToEquator = GlobalRows / Px / 2;   // 1800
	constexpr int32  TileColsToMeridian = GlobalCols / Px / 2;  // 3600
	constexpr double CellEps = 1e-6;   // cells: absorbs round-off at exact grid lines (as CamSimLandCover::GlobalCell)

	bool GetIndexField(const FJsonObject& O, const TCHAR* Key, int32 Lo, int32 Hi, int32& Out)
	{
		double V = 0.0;
		if (!O.TryGetNumberField(Key, V) || !FMath::IsFinite(V) || V != FMath::FloorToDouble(V) || V < Lo || V > Hi) return false;
		Out = static_cast<int32>(V);
		return true;
	}
}

bool FVegetationBuildingMask::ParseIndex(const FString& Json, TMap<FIntPoint, FString>& OutFiles, FString& OutError)
{
	using namespace CamSimBuildingMaskDetail;
	OutFiles.Reset();
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		OutError = TEXT("index.json is not valid JSON");
		return false;
	}
	FString Format;
	if (!Root->TryGetStringField(TEXT("format"), Format) || Format != MaskFormat)
	{
		OutError = FString::Printf(TEXT("index.json format '%s' is not %s"), *Format, MaskFormat);
		return false;
	}
	double Deg = 0.0, TilePxField = 0.0;
	if (!Root->TryGetNumberField(TEXT("tile_deg"), Deg) || FMath::Abs(Deg - TileDeg) > 1e-9
		|| !Root->TryGetNumberField(TEXT("tile_px"), TilePxField) || TilePxField != static_cast<double>(TilePx))
	{
		OutError = FString::Printf(TEXT("index.json tile_deg / tile_px must be %.2f / %d"), TileDeg, TilePx);
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Tiles = nullptr;
	if (!Root->TryGetArrayField(TEXT("tiles"), Tiles))
	{
		OutError = TEXT("index.json has no tiles array");
		return false;
	}
	for (int32 K = 0; K < Tiles->Num(); ++K)
	{
		const TSharedPtr<FJsonObject>* T = nullptr;
		FString File;
		int32 Lat = 0, Lon = 0;
		const bool bOk = (*Tiles)[K].IsValid() && (*Tiles)[K]->TryGetObject(T) && T && (*T).IsValid()
			&& (*T)->TryGetStringField(TEXT("file"), File) && File.EndsWith(TEXT(".png"))
			&& !File.Contains(TEXT("/")) && !File.Contains(TEXT("\\")) && !File.Contains(TEXT(".."))
			&& GetIndexField(**T, TEXT("lat_index"), -1800, 1799, Lat) && GetIndexField(**T, TEXT("lon_index"), -3600, 3599, Lon);
		if (!bOk)
		{
			OutError = FString::Printf(TEXT("index.json tiles[%d] is malformed"), K);
			return false;
		}
		OutFiles.Add(FIntPoint(Lat, Lon), File);
	}
	return true;
}

bool FVegetationBuildingMask::DecodeMaskPng(IImageWrapperModule& Module, TConstArrayView<uint8> Png, TArray<uint64>& OutBits, FString& OutError)
{
	using namespace CamSimBuildingMaskDetail;
	const TSharedPtr<IImageWrapper> W = Module.CreateImageWrapper(EImageFormat::PNG);
	if (!W.IsValid() || Png.Num() == 0 || !W->SetCompressed(Png.GetData(), Png.Num()))
	{
		OutError = TEXT("not a PNG (a git LFS pointer? run git lfs pull)");
		return false;
	}
	const int32 Depth = W->GetBitDepth();
	if (W->GetWidth() != Px || W->GetHeight() != Px || W->GetFormat() != ERGBFormat::Gray || (Depth != 1 && Depth != 8))
	{
		OutError = FString::Printf(TEXT("%lldx%lld, %d-bit: expected a %dx%d 1-bit greyscale PNG"),
			static_cast<long long>(W->GetWidth()), static_cast<long long>(W->GetHeight()), Depth, Px, Px);
		return false;
	}
	TArray64<uint8> Raw;   // libpng expands 1-bit grey to 0 / 255
	if (!W->GetRaw(ERGBFormat::Gray, 8, Raw) || Raw.Num() != static_cast<int64>(Px) * Px)
	{
		OutError = TEXT("PNG decode failed");
		return false;
	}
	OutBits.SetNumZeroed(WordsPerTile);
	for (int64 K = 0; K < Raw.Num(); ++K)
	{
		if (Raw[K] != 0) OutBits[K >> 6] |= uint64(1) << (K & 63);
	}
	return true;
}

void FVegetationBuildingMask::CellOf(double LatDeg, double LonDeg, FIntPoint& OutTile, FIntPoint& OutInTile)
{
	using namespace CamSimBuildingMaskDetail;
	const int32 Row = FMath::Clamp(FMath::FloorToInt32((90.0 - LatDeg) * CellsPerDeg + CellEps), 0, GlobalRows - 1);
	int64 Col = FMath::FloorToInt64((LonDeg + 180.0) * CellsPerDeg + CellEps) % GlobalCols;
	if (Col < 0) Col += GlobalCols;
	const int32 C = static_cast<int32>(Col);
	OutTile = FIntPoint(TileRowsToEquator - 1 - Row / Px, C / Px - TileColsToMeridian);
	OutInTile = FIntPoint(C % Px, Row % Px);
}

bool FVegetationBuildingMask::IsClearOf(double LatDeg, double LonDeg, double ClearanceM, TFunctionRef<bool(double, double)> IsMaskedAt)
{
	if (IsMaskedAt(LatDeg, LonDeg)) return false;
	if (!(ClearanceM > 0.0)) return true;
	const double DLat = FMath::RadiansToDegrees(ClearanceM / CamSimLandCover::MeridionalRadiusM(LatDeg));
	const double DLon = FMath::RadiansToDegrees(ClearanceM
		/ (CamSimLandCover::PrimeVerticalRadiusM(LatDeg) * FMath::Max(FMath::Cos(FMath::DegreesToRadians(LatDeg)), 1e-6)));
	return !IsMaskedAt(LatDeg, LonDeg + DLon) && !IsMaskedAt(LatDeg, LonDeg - DLon)
		&& !IsMaskedAt(LatDeg + DLat, LonDeg) && !IsMaskedAt(LatDeg - DLat, LonDeg);
}

FVegetationBuildingMask::FVegetationBuildingMask(const FString& InDir)
	: Dir(FLandCoverTileCache::ResolveDir(InDir))
{
	check(IsInGameThread());
	ImageWrapper = &FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	const FString IndexPath = FPaths::Combine(Dir, TEXT("index.json"));
	FString Json;
	if (Dir.IsEmpty() || !FFileHelper::LoadFileToString(Json, *IndexPath))
	{
		Error = FString::Printf(TEXT("no building mask index at %s"), *IndexPath);
		return;
	}
	FString ParseError;
	if (!ParseIndex(Json, Files, ParseError))
	{
		Error = IndexPath + TEXT(": ") + ParseError;
		return;
	}
	bValid = true;
}

bool FVegetationBuildingMask::IsMasked(double LatDeg, double LonDeg)
{
	using namespace CamSimBuildingMaskDetail;
	if (!bValid || !FMath::IsFinite(LatDeg) || !FMath::IsFinite(LonDeg)) return false;
	FIntPoint Tile, In;
	CellOf(LatDeg, LonDeg, Tile, In);
	const TSharedPtr<const FTile, ESPMode::ThreadSafe> T = Get(Tile);
	if (!T.IsValid()) return false;
	const int64 K = static_cast<int64>(In.Y) * Px + In.X;
	return ((T->Bits[K >> 6] >> (K & 63)) & 1u) != 0;
}

bool FVegetationBuildingMask::IsClear(double LatDeg, double LonDeg, double ClearanceM)
{
	return IsClearOf(LatDeg, LonDeg, ClearanceM, [this](double La, double Lo) { return IsMasked(La, Lo); });
}

TSharedPtr<const FVegetationBuildingMask::FTile, ESPMode::ThreadSafe> FVegetationBuildingMask::Get(FIntPoint Key)
{
	const FString* File = Files.Find(Key);
	if (!File) return nullptr;
	{
		FScopeLock L(&Lock);
		if (FEntry* E = Cache.Find(Key))
		{
			E->LastUse = ++UseCounter;
			return E->Tile;
		}
		if (Failed.Contains(Key)) return nullptr;
	}
	// Unlocked I/O and decode; two threads loading the same tile both decode and the first insert wins.
	const FString Path = FPaths::Combine(Dir, *File);
	TArray<uint8> Png;
	TArray<uint64> Bits;
	FString Why;
	if (!FFileHelper::LoadFileToArray(Png, *Path, FILEREAD_Silent)) Why = TEXT("missing (git lfs pull?)");
	else DecodeMaskPng(*ImageWrapper, Png, Bits, Why);

	FScopeLock L(&Lock);
	if (FEntry* E = Cache.Find(Key))
	{
		E->LastUse = ++UseCounter;
		return E->Tile;
	}
	if (Bits.Num() != WordsPerTile)
	{
		bool bAlready = false;
		Failed.Add(Key, &bAlready);
		if (!bAlready)
		{
			// Once per bad tile: Failed keeps it from being read (or reported) again.
			UE_LOG(LogCamSim, Warning, TEXT("Vegetation: building mask tile %s unreadable (%s); treated as no buildings"), *Path, *Why);
		}
		return nullptr;
	}
	++Loads;
	TSharedRef<FTile, ESPMode::ThreadSafe> Tile = MakeShared<FTile, ESPMode::ThreadSafe>();
	Tile->Bits = MoveTemp(Bits);
	if (Cache.Num() >= MaxTiles)
	{
		FIntPoint Oldest = FIntPoint::ZeroValue;
		uint64 OldestUse = MAX_uint64;
		for (const TPair<FIntPoint, FEntry>& P : Cache)
		{
			if (P.Value.LastUse < OldestUse) { OldestUse = P.Value.LastUse; Oldest = P.Key; }
		}
		Cache.Remove(Oldest);
	}
	Cache.Add(Key, FEntry{ Tile, ++UseCounter });
	return Tile;
}

int32 FVegetationBuildingMask::NumCached() const
{
	FScopeLock L(&Lock);
	return Cache.Num();
}

int32 FVegetationBuildingMask::NumLoads() const
{
	FScopeLock L(&Lock);
	return Loads;
}
