// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Thermal/LandCoverTiles.h"

/** Synthetic land-cover tile directories for CamSim.Thermal.LandCover.* (ROADMAP 4B). Test code only. */
namespace CamSimLandCoverTest
{
	inline constexpr int32 TilePx = FLandCoverTileCache::TilePx;

	/** An empty directory under Saved/Automation/LandCover (absolute). */
	inline FString TempDir(const TCHAR* Name)
	{
		const FString D = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("LandCover"), Name));
		IFileManager::Get().DeleteDirectory(*D, false, true);
		IFileManager::Get().MakeDirectory(*D, true);
		return D;
	}

	inline TArray64<uint8> EncodeGreyPng(const TArray<uint8>& Codes, int32 W, int32 H)
	{
		IImageWrapperModule& M = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		const TSharedPtr<IImageWrapper> Wr = M.CreateImageWrapper(EImageFormat::PNG);
		Wr->SetRaw(Codes.GetData(), Codes.Num(), W, H, ERGBFormat::Gray, 8);
		return Wr->GetCompressed();
	}

	/** TilePx^2 codes (row + 3 col) % 101: every value 0..100, no two neighbours equal along a row. */
	inline TArray<uint8> PatternCodes()
	{
		TArray<uint8> C;
		C.SetNumUninitialized(TilePx * TilePx);
		for (int32 Y = 0; Y < TilePx; ++Y)
			for (int32 X = 0; X < TilePx; ++X) C[Y * TilePx + X] = static_cast<uint8>((Y + 3 * X) % 101);
		return C;
	}

	/** What a PNG looks like after a clone without `git lfs pull`. */
	inline TArray<uint8> LfsPointerBytes()
	{
		const FTCHARToUTF8 S(TEXT("version https://git-lfs.github.com/spec/v1\noid sha256:0123456789abcdef\nsize 81234\n"));
		return TArray<uint8>(reinterpret_cast<const uint8*>(S.Get()), S.Length());
	}

	struct FTestTile
	{
		int32 LatIndex = 0;
		int32 LonIndex = 0;
		TArray<uint8> Codes;   // TilePx^2
	};

	inline FTestTile Uniform(int32 LatIndex, int32 LonIndex, uint8 Code)
	{
		FTestTile T;
		T.LatIndex = LatIndex;
		T.LonIndex = LonIndex;
		T.Codes.Init(Code, TilePx * TilePx);
		return T;
	}

	inline FString TileFile(int32 LatIndex, int32 LonIndex) { return FString::Printf(TEXT("t_%d_%d.png"), LatIndex, LonIndex); }

	/** PNGs + index.json (format camsim-landcover-1) in Dir. */
	inline void WriteTileDir(const FString& Dir, const TArray<FTestTile>& Tiles)
	{
		FString Entries;
		for (const FTestTile& T : Tiles)
		{
			const FString File = TileFile(T.LatIndex, T.LonIndex);
			FFileHelper::SaveArrayToFile(EncodeGreyPng(T.Codes, TilePx, TilePx), *FPaths::Combine(Dir, File));
			Entries += FString::Printf(TEXT("%s{\"file\":\"%s\",\"lat_index\":%d,\"lon_index\":%d}"),
				Entries.IsEmpty() ? TEXT("") : TEXT(","), *File, T.LatIndex, T.LonIndex);
		}
		FFileHelper::SaveStringToFile(FString::Printf(
			TEXT("{\"format\":\"camsim-landcover-1\",\"tile_deg\":0.05,\"tile_px\":600,\"licence\":\"CC BY 4.0\",")
			TEXT("\"attribution\":\"test tiles\",\"tiles\":[%s]}"), *Entries), *FPaths::Combine(Dir, TEXT("index.json")));
	}
}
