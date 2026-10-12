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

	namespace
	{
		/** The file exists, is not a git LFS pointer and has the given sha256. */
		bool CheckFile(const FString& Name, const FString& Path, const FString& Sha, FString& OutError)
		{
			TArray<uint8> Bytes;
			if (!FPaths::FileExists(Path) || !FFileHelper::LoadFileToArray(Bytes, *Path))
			{
				OutError = FString::Printf(TEXT("type '%s': cannot read %s"), *Name, *Path); return false;
			}
			// A git LFS pointer is a short text file; compare the prefix within the bytes actually loaded (the buffer is not NUL-terminated).
			static const ANSICHAR LfsPrefix[] = "version https://git-lfs";
			constexpr int32 LfsPrefixLen = static_cast<int32>(sizeof(LfsPrefix) - 1);
			if (Bytes.Num() < 200 && Bytes.Num() >= LfsPrefixLen && FMemory::Memcmp(Bytes.GetData(), LfsPrefix, LfsPrefixLen) == 0)
			{
				OutError = FString::Printf(TEXT("type '%s': %s is a git LFS pointer (run git lfs pull)"), *Name, *Path); return false;
			}
			const FString Got = Sha256Hex(Bytes);
			if (Got != Sha)
			{
				OutError = FString::Printf(TEXT("type '%s': %s sha256 %s != library.json %s"), *Name, *Path, *Got, *Sha); return false;
			}
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
		Root->TryGetStringField(TEXT("format"), Format);
		const bool bV2 = Format == TEXT("camsim-vegetation-2");
		if (!bV2 && Format != TEXT("camsim-vegetation-1"))
		{
			OutError = FString::Printf(TEXT("library.json format '%s' is not camsim-vegetation-1 or -2"), *Format); return false;
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
			if (FindType(Out, T.Name) != INDEX_NONE) { OutError = FString::Printf(TEXT("duplicate type name '%s'"), *T.Name); return false; }
			FString Role;
			if (bV2 && (*O)->TryGetStringField(TEXT("role"), Role) && Role != TEXT("tree"))
			{
				if (Role != TEXT("shrub")) { OutError = FString::Printf(TEXT("type '%s': role '%s' is not tree or shrub"), *T.Name, *Role); return false; }
				T.Role = EVegRole::Shrub;
			}
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
			double Crown = 0.0;
			if (bV2 && (*O)->TryGetNumberField(TEXT("crown_radius_m"), Crown))
			{
				if (!(Crown > 0.0 && Crown <= 30.0)) { OutError = FString::Printf(TEXT("type '%s': crown_radius_m %g not in (0, 30]"), *T.Name, Crown); return false; }
				T.CrownRadiusM = static_cast<float>(Crown);
			}
			else
			{
				T.CrownRadiusM = (T.Role == EVegRole::Shrub ? 0.5f : 0.3f) * T.HeightM;
			}
			FString FarGltf;
			if (bV2 && (*O)->TryGetStringField(TEXT("far_gltf"), FarGltf) && !FarGltf.IsEmpty())
			{
				if (T.Role == EVegRole::Shrub) { OutError = FString::Printf(TEXT("type '%s': shrubs have no far mesh"), *T.Name); return false; }
				if (!(*O)->TryGetStringField(TEXT("far_sha256"), T.FarSha256) || !IsHex64(T.FarSha256))
				{
					OutError = FString::Printf(TEXT("type '%s': far_gltf needs far_sha256 (64 lowercase hex digits)"), *T.Name); return false;
				}
				T.FarGltfPath = FPaths::IsRelative(FarGltf) ? FPaths::Combine(Dir, FarGltf) : FarGltf;
			}
			Out.Types.Add(MoveTemp(T));
		}
		return true;
	}

	int32 FindType(const FVegetationLibrary& Library, const FString& Name)
	{
		return Library.Types.IndexOfByPredicate([&Name](const FVegetationType& T) { return T.Name == Name; });
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
			if (!CheckFile(T.Name, T.GltfPath, T.Sha256, OutError)) return false;
			if (T.HasFarMesh() && !CheckFile(T.Name, T.FarGltfPath, T.FarSha256, OutError)) return false;
		}
		return true;
	}
}
