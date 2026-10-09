// Copyright CamSim Contributors. All Rights Reserved.

#include "Config/ScenePackage.h"
#include "Config/CamSimConfig.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace CamSimScene
{
namespace
{
	bool IsUnreserved(uint8 C)
	{
		return (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9')
			|| C == '-' || C == '.' || C == '_' || C == '~' || C == '/' || C == ':';
	}

	int32 HexValue(ANSICHAR C)
	{
		if (C >= '0' && C <= '9') return C - '0';
		if (C >= 'a' && C <= 'f') return C - 'a' + 10;
		if (C >= 'A' && C <= 'F') return C - 'A' + 10;
		return -1;
	}
}

FString PathToFileUrl(const FString& AbsolutePath)
{
	const FString Path = AbsolutePath.Replace(TEXT("\\"), TEXT("/"));
	FString Out = Path.StartsWith(TEXT("/")) ? TEXT("file://") : TEXT("file:///");
	const FTCHARToUTF8 Utf8(*Path);
	for (int32 i = 0; i < Utf8.Length(); ++i)
	{
		const uint8 C = static_cast<uint8>(Utf8.Get()[i]);
		if (IsUnreserved(C))
		{
			Out.AppendChar(static_cast<TCHAR>(C));
		}
		else
		{
			Out += FString::Printf(TEXT("%%%02X"), C);
		}
	}
	return Out;
}

bool FileUrlToPath(const FString& Url, FString& OutPath)
{
	static const FString Prefix = TEXT("file:///");
	if (!Url.StartsWith(Prefix, ESearchCase::IgnoreCase))
	{
		return false;
	}
	const FTCHARToUTF8 Utf8(*Url.RightChop(Prefix.Len() - 1));   // keep the path's leading '/'
	const ANSICHAR* S = Utf8.Get();
	const int32 N = Utf8.Length();
	TArray<ANSICHAR> Bytes;
	Bytes.Reserve(N + 1);
	for (int32 i = 0; i < N; ++i)
	{
		const int32 Hi = (S[i] == '%' && i + 2 < N) ? HexValue(S[i + 1]) : -1;
		const int32 Lo = Hi >= 0 ? HexValue(S[i + 2]) : -1;
		if (Lo >= 0)
		{
			Bytes.Add(static_cast<ANSICHAR>(Hi * 16 + Lo));
			i += 2;
		}
		else
		{
			Bytes.Add(S[i]);
		}
	}
	Bytes.Add('\0');
	OutPath = UTF8_TO_TCHAR(Bytes.GetData());
	// "/C:/x" -> "C:/x"
	if (OutPath.Len() >= 3 && OutPath[0] == TEXT('/') && FChar::IsAlpha(OutPath[1]) && OutPath[2] == TEXT(':'))
	{
		OutPath.RightChopInline(1);
	}
	return true;
}

void ResolvePackage(FCamSimConfig& Cfg)
{
	FCamSimConfig::FSceneConfig& Scene = Cfg.Scene;
	Scene.PackageName.Reset();
	Scene.ResolveErrors.Reset();
	Scene.ResolveWarnings.Reset();

	FString Dir = Scene.Dir.TrimStartAndEnd();
	if (Dir.IsEmpty())
	{
		return;
	}
	FPaths::NormalizeDirectoryName(Dir);   // '/' separators, no trailing slash
	if (FPaths::IsRelative(Dir))
	{
		Scene.ResolveErrors.Add(FString::Printf(TEXT("scene.dir '%s' must be an absolute path"), *Scene.Dir));
		return;
	}

	FString ManifestText;
	if (!FFileHelper::LoadFileToString(ManifestText, *(Dir / TEXT("manifest.json"))))
	{
		Scene.ResolveErrors.Add(FString::Printf(TEXT("scene.dir '%s' has no manifest.json (not a scene package)"), *Dir));
		return;
	}
	TSharedPtr<FJsonObject> Manifest;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ManifestText), Manifest) || !Manifest.IsValid())
	{
		Scene.ResolveErrors.Add(FString::Printf(TEXT("scene.dir '%s': manifest.json is not valid JSON"), *Dir));
		return;
	}
	int32 Schema = 0;
	if (!Manifest->TryGetNumberField(TEXT("schema_version"), Schema) || Schema != SupportedSchemaVersion)
	{
		Scene.ResolveErrors.Add(FString::Printf(
			TEXT("scene.dir '%s': manifest.json schema_version %d is not supported (this build reads %d)"),
			*Dir, Schema, SupportedSchemaVersion));
		return;
	}
	Manifest->TryGetStringField(TEXT("name"), Scene.PackageName);

	const FCamSimConfig Defaults;
	FCamSimConfig::FCesiumBackendConfig& Cs = Cfg.CesiumBackend;

	const FString TerrainFile = Dir / TEXT("terrain/layer.json");
	if (FPaths::FileExists(TerrainFile))
	{
		if (Cs.Terrain.Source != Defaults.CesiumBackend.Terrain.Source || !Cs.Terrain.Url.IsEmpty())
		{
			Scene.ResolveWarnings.Add(FString::Printf(
				TEXT("scene.dir overrides cesium.terrain (source '%s', url '%s')"), *Cs.Terrain.Source, *Cs.Terrain.Url));
		}
		Cs.Terrain.Source = TEXT("url");
		Cs.Terrain.Url    = PathToFileUrl(TerrainFile);
	}

	const FString ImageryFile = Dir / TEXT("imagery/tilemapresource.xml");
	if (FPaths::FileExists(ImageryFile))
	{
		if (Cs.Imagery.Source != Defaults.CesiumBackend.Imagery.Source || !Cs.Imagery.Url.IsEmpty())
		{
			Scene.ResolveWarnings.Add(FString::Printf(
				TEXT("scene.dir overrides cesium.imagery (source '%s', url '%s')"), *Cs.Imagery.Source, *Cs.Imagery.Url));
		}
		Cs.Imagery.Source = TEXT("tms");
		Cs.Imagery.Url    = PathToFileUrl(ImageryFile);
	}

	if (FPaths::FileExists(Dir / TEXT("landcover/index.json")))
	{
		if (Cfg.Thermal.LandCover.Dir != Defaults.Thermal.LandCover.Dir)
		{
			Scene.ResolveWarnings.Add(FString::Printf(
				TEXT("scene.dir overrides thermal.land_cover.dir ('%s')"), *Cfg.Thermal.LandCover.Dir));
		}
		Cfg.Thermal.LandCover.Dir = Dir / TEXT("landcover");
	}
}

TArray<FString> ValidateSources(const FCamSimConfig& Cfg)
{
	TArray<FString> Errors;
	const FCamSimConfig::FCesiumBackendConfig& Cs = Cfg.CesiumBackend;
	if (Cs.Imagery.Source == TEXT("tms") && Cs.Imagery.Url.IsEmpty())
	{
		Errors.Add(TEXT("cesium.imagery.source 'tms' needs cesium.imagery.url (the tilemapresource.xml URL)"));
	}
	if (!Cfg.Scene.bOffline)
	{
		return Errors;
	}

	auto CheckLocal = [&Errors](const TCHAR* Key, const FString& Url)
	{
		FString Path;
		if (!FileUrlToPath(Url, Path))
		{
			Errors.Add(FString::Printf(TEXT("scene.offline: %s '%s' is not a file:/// URL"), Key, *Url));
		}
		else if (!FPaths::FileExists(Path))
		{
			Errors.Add(FString::Printf(TEXT("scene.offline: %s file '%s' does not exist"), Key, *Path));
		}
	};

	if (Cs.Terrain.Source == TEXT("url"))
	{
		CheckLocal(TEXT("cesium.terrain.url"), Cs.Terrain.Url);
	}
	else if (Cs.Terrain.Source != TEXT("flat"))
	{
		Errors.Add(FString::Printf(
			TEXT("scene.offline: cesium.terrain.source '%s' needs the network (use url with a file:/// URL, or flat)"),
			*Cs.Terrain.Source));
	}

	if (Cs.Imagery.Source == TEXT("tms"))
	{
		if (!Cs.Imagery.Url.IsEmpty())
		{
			CheckLocal(TEXT("cesium.imagery.url"), Cs.Imagery.Url);
		}
	}
	else if (Cs.Imagery.Source != TEXT("none"))
	{
		Errors.Add(FString::Printf(
			TEXT("scene.offline: cesium.imagery.source '%s' needs the network (use tms with a file:/// URL, or none)"),
			*Cs.Imagery.Source));
	}
	return Errors;
}
} // namespace CamSimScene
