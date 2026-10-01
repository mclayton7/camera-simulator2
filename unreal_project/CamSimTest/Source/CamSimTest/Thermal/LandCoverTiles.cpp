// Copyright CamSim Contributors. All Rights Reserved.

#include "Thermal/LandCoverTiles.h"
#include "Dom/JsonObject.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	const TCHAR* IndexFormat = TEXT("camsim-landcover-1");

	bool GetIndexField(const FJsonObject& O, const TCHAR* Key, int32 Lo, int32 Hi, int32& Out)
	{
		double V = 0.0;
		if (!O.TryGetNumberField(Key, V) || !FMath::IsFinite(V) || V != FMath::FloorToDouble(V) || V < Lo || V > Hi) return false;
		Out = static_cast<int32>(V);
		return true;
	}
}

FString FLandCoverTileCache::ResolveDir(const FString& InDir)
{
	const FString Trimmed = InDir.TrimStartAndEnd();
	if (Trimmed.IsEmpty()) return FString();
	return FPaths::IsRelative(Trimmed) ? FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), Trimmed)) : Trimmed;
}

bool FLandCoverTileCache::ParseIndex(const FString& Json, FLandCoverIndex& Out, FString& OutError)
{
	Out = FLandCoverIndex();
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		OutError = TEXT("index.json is not valid JSON");
		return false;
	}
	FString Format;
	if (!Root->TryGetStringField(TEXT("format"), Format) || Format != IndexFormat)
	{
		OutError = FString::Printf(TEXT("index.json format '%s' is not %s"), *Format, IndexFormat);
		return false;
	}
	double Deg = 0.0, Px = 0.0;
	if (!Root->TryGetNumberField(TEXT("tile_deg"), Deg) || FMath::Abs(Deg - TileDeg) > 1e-9
		|| !Root->TryGetNumberField(TEXT("tile_px"), Px) || Px != static_cast<double>(TilePx))
	{
		OutError = FString::Printf(TEXT("index.json tile_deg / tile_px must be %.2f / %d"), TileDeg, TilePx);
		return false;
	}
	Root->TryGetStringField(TEXT("attribution"), Out.Attribution);
	Root->TryGetStringField(TEXT("licence"), Out.Licence);
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
		Out.Files.Add(FIntPoint(Lat, Lon), File);
	}
	return true;
}

bool FLandCoverTileCache::DecodeTilePng(IImageWrapperModule& Module, TConstArrayView<uint8> Png, TArray<uint8>& OutCodes, FString& OutError)
{
	const TSharedPtr<IImageWrapper> W = Module.CreateImageWrapper(EImageFormat::PNG);
	if (!W.IsValid() || Png.Num() == 0 || !W->SetCompressed(Png.GetData(), Png.Num()))
	{
		OutError = TEXT("not a PNG (a git LFS pointer? run git lfs pull)");
		return false;
	}
	if (W->GetWidth() != TilePx || W->GetHeight() != TilePx || W->GetFormat() != ERGBFormat::Gray || W->GetBitDepth() != 8)
	{
		OutError = FString::Printf(TEXT("%lldx%lld, %d-bit: expected a %dx%d 8-bit greyscale PNG"),
			static_cast<long long>(W->GetWidth()), static_cast<long long>(W->GetHeight()), W->GetBitDepth(), TilePx, TilePx);
		return false;
	}
	TArray64<uint8> Raw;
	if (!W->GetRaw(ERGBFormat::Gray, 8, Raw) || Raw.Num() != static_cast<int64>(TilePx) * TilePx)
	{
		OutError = TEXT("PNG decode failed");
		return false;
	}
	OutCodes.SetNumUninitialized(TilePx * TilePx);
	FMemory::Memcpy(OutCodes.GetData(), Raw.GetData(), OutCodes.Num());
	return true;
}

std::atomic<double> FLandCoverTileCache::TestLoadDelaySeconds{ 0.0 };

FLandCoverTileCache::FLandCoverTileCache(const FString& InDir, int32 InMaxTiles)
	: Dir(ResolveDir(InDir))
	, MaxTiles(FMath::Max(InMaxTiles, 4))
{
	check(IsInGameThread());
	ImageWrapper = &FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	const FString IndexPath = FPaths::Combine(Dir, TEXT("index.json"));
	FString Json;
	if (Dir.IsEmpty() || !FFileHelper::LoadFileToString(Json, *IndexPath))
	{
		Error = FString::Printf(TEXT("no land-cover index at %s"), *IndexPath);
		return;
	}
	FString ParseError;
	if (!ParseIndex(Json, Index, ParseError))
	{
		Error = IndexPath + TEXT(": ") + ParseError;
		return;
	}
	bValid = true;
}

TSharedPtr<const FLandCoverTile, ESPMode::ThreadSafe> FLandCoverTileCache::Get(int32 LatIndex, int32 LonIndex)
{
	const FIntPoint Key(LatIndex, LonIndex);
	const FString* File = Index.Files.Find(Key);   // Index is immutable after construction
	if (!bValid || !File) return nullptr;
	{
		FScopeLock L(&Lock);
		if (FEntry* E = Cache.Find(Key))
		{
			E->LastUse = ++UseCounter;
			return E->Tile;
		}
		if (Failed.Contains(Key)) return nullptr;
	}

	// File I/O and decode run unlocked, so TakeWarnings / NumCached (game thread) never wait for a load. Two threads
	// loading the same tile both decode; the first insert wins and the other result is dropped.
	const FString Path = FPaths::Combine(Dir, *File);
	TArray<uint8> Png, Codes;
	FString Why;
	if (const double Delay = TestLoadDelaySeconds.load(); Delay > 0.0) FPlatformProcess::Sleep(static_cast<float>(Delay));
	if (!FFileHelper::LoadFileToArray(Png, *Path, FILEREAD_Silent))
	{
		Why = TEXT("missing (git lfs pull?)");
	}
	else
	{
		DecodeTilePng(*ImageWrapper, Png, Codes, Why);
	}

	FScopeLock L(&Lock);
	if (FEntry* E = Cache.Find(Key))   // inserted by a concurrent load meanwhile
	{
		E->LastUse = ++UseCounter;
		return E->Tile;
	}
	if (Codes.Num() != TilePx * TilePx)
	{
		bool bAlreadyFailed = false;
		Failed.Add(Key, &bAlreadyFailed);
		if (!bAlreadyFailed) Warnings.Add(FString::Printf(TEXT("land-cover tile %s: %s; treated as no data"), *Path, *Why));
		return nullptr;
	}
	++Loads;
	TSharedRef<FLandCoverTile, ESPMode::ThreadSafe> Tile = MakeShared<FLandCoverTile, ESPMode::ThreadSafe>();
	Tile->LatIndex = LatIndex;
	Tile->LonIndex = LonIndex;
	Tile->Codes = MoveTemp(Codes);
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

TArray<FString> FLandCoverTileCache::TakeWarnings()
{
	FScopeLock L(&Lock);
	return MoveTemp(Warnings);
}

int32 FLandCoverTileCache::NumCached() const
{
	FScopeLock L(&Lock);
	return Cache.Num();
}

int32 FLandCoverTileCache::NumLoads() const
{
	FScopeLock L(&Lock);
	return Loads;
}
