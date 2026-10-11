// Copyright CamSim Contributors. All Rights Reserved.

#include "Vegetation/VegetationLibrary.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Thermal/LandCoverTiles.h"

extern "C"
{
#include "libavutil/mem.h"
#include "libavutil/sha.h"
}

namespace CamSimVegetation
{
	namespace
	{
		bool IsHex64(const FString& S)
		{
			if (S.Len() != 64) return false;
			for (TCHAR C : S) if (!FChar::IsHexDigit(C) || FChar::IsUpper(C)) return false;
			return true;
		}
	}

	FString Sha256Hex(TConstArrayView<uint8> Bytes)
	{
		AVSHA* Ctx = av_sha_alloc();
		av_sha_init(Ctx, 256);
		if (Bytes.Num() > 0) av_sha_update(Ctx, Bytes.GetData(), static_cast<size_t>(Bytes.Num()));
		uint8 Digest[32];
		av_sha_final(Ctx, Digest);
		av_free(Ctx);
		return BytesToHex(Digest, 32).ToLower();
	}

	bool ParseLibrary(const FString& Json, const FString& Dir, FVegetationLibrary& Out, FString& OutError)
	{
		Out = FVegetationLibrary();
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
		{
			OutError = TEXT("library.json is not valid JSON"); return false;
		}
		FString Format;
		if (!Root->TryGetStringField(TEXT("format"), Format) || Format != TEXT("camsim-vegetation-1"))
		{
			OutError = FString::Printf(TEXT("library.json format '%s' is not camsim-vegetation-1"), *Format); return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Types = nullptr;
		if (!Root->TryGetArrayField(TEXT("types"), Types) || Types->Num() == 0)
		{
			OutError = TEXT("library.json has no types"); return false;
		}
		for (int32 I = 0; I < Types->Num(); ++I)
		{
			const TSharedPtr<FJsonObject>* O = nullptr;
			if (!(*Types)[I]->TryGetObject(O)) { OutError = FString::Printf(TEXT("types[%d] is not an object"), I); return false; }
			FVegetationType T;
			double H = 0.0;
			(*O)->TryGetStringField(TEXT("name"), T.Name);
			(*O)->TryGetNumberField(TEXT("height_m"), H);
			(*O)->TryGetStringField(TEXT("licence"), T.Licence);
			(*O)->TryGetStringField(TEXT("source_url"), T.SourceUrl);
			(*O)->TryGetBoolField(TEXT("procedural"), T.bProcedural);
			if (!(*O)->TryGetStringField(TEXT("thermal_class"), T.ThermalClass)) T.ThermalClass = TEXT("tree_canopy");
			T.HeightM = static_cast<float>(H);
			if (T.Name.IsEmpty()) { OutError = FString::Printf(TEXT("types[%d]: name missing"), I); return false; }
			if (!(H > 0.5 && H <= 80.0)) { OutError = FString::Printf(TEXT("type '%s': height_m %g not in (0.5, 80]"), *T.Name, H); return false; }
			if (T.Licence.IsEmpty()) { OutError = FString::Printf(TEXT("type '%s': licence missing"), *T.Name); return false; }
			if (!T.bProcedural)
			{
				FString Gltf;
				if (!(*O)->TryGetStringField(TEXT("gltf"), Gltf) || Gltf.IsEmpty()) { OutError = FString::Printf(TEXT("type '%s': gltf missing"), *T.Name); return false; }
				if (!(*O)->TryGetStringField(TEXT("sha256"), T.Sha256) || !IsHex64(T.Sha256))
				{
					OutError = FString::Printf(TEXT("type '%s': sha256 must be 64 lowercase hex digits"), *T.Name); return false;
				}
				T.GltfPath = FPaths::IsRelative(Gltf) ? FPaths::Combine(Dir, Gltf) : Gltf;
			}
			Out.Types.Add(MoveTemp(T));
		}
		return true;
	}

	FString ResolveLibraryDir(const FString& ConfigDir)
	{
		return FLandCoverTileCache::ResolveDir(ConfigDir.IsEmpty() ? FString(TEXT("Content/NonUFS/Vegetation")) : ConfigDir);
	}

	bool LoadLibrary(const FString& Dir, FVegetationLibrary& Out, FString& OutError)
	{
		FString Json;
		if (!FFileHelper::LoadFileToString(Json, *FPaths::Combine(Dir, TEXT("library.json"))))
		{
			OutError = FString::Printf(TEXT("no library.json in '%s'"), *Dir); return false;
		}
		if (!ParseLibrary(Json, Dir, Out, OutError)) return false;
		for (const FVegetationType& T : Out.Types)
		{
			if (T.bProcedural) continue;
			TArray<uint8> Bytes;
			if (!FPaths::FileExists(T.GltfPath) || !FFileHelper::LoadFileToArray(Bytes, *T.GltfPath))
			{
				OutError = FString::Printf(TEXT("type '%s': cannot read %s"), *T.Name, *T.GltfPath); return false;
			}
			// A git LFS pointer is a short text file; compare the prefix within the bytes actually loaded (the buffer is not NUL-terminated).
			static const ANSICHAR LfsPrefix[] = "version https://git-lfs";
			constexpr int32 LfsPrefixLen = static_cast<int32>(sizeof(LfsPrefix) - 1);
			if (Bytes.Num() < 200 && Bytes.Num() >= LfsPrefixLen && FMemory::Memcmp(Bytes.GetData(), LfsPrefix, LfsPrefixLen) == 0)
			{
				OutError = FString::Printf(TEXT("type '%s': %s is a git LFS pointer (run git lfs pull)"), *T.Name, *T.GltfPath); return false;
			}
			const FString Got = Sha256Hex(Bytes);
			if (Got != T.Sha256)
			{
				OutError = FString::Printf(TEXT("type '%s': %s sha256 %s != library.json %s"), *T.Name, *T.GltfPath, *Got, *T.Sha256); return false;
			}
		}
		return true;
	}
}
