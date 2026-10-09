// Copyright CamSim Contributors. All Rights Reserved.

#include "Geospatial/CamSimGeospatialProvider.h"
#include "CamSimTest.h"

#include "CesiumGeoreference.h"
#include "Cesium3DTileset.h"
#include "CesiumIonServer.h"
#include "CesiumIonRasterOverlay.h"
#include "CesiumWebMapServiceRasterOverlay.h"
#include "CesiumTileMapServiceRasterOverlay.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace CamSim::Geospatial
{
int32 SelectTerrainTileset(TConstArrayView<int64> IonAssetIds)
{
	for (int32 i = 0; i < IonAssetIds.Num(); ++i)
	{
		if (IonAssetIds[i] == CesiumWorldTerrainAssetId)
		{
			return i;
		}
	}
	return IonAssetIds.IsEmpty() ? INDEX_NONE : 0;
}
} // namespace CamSim::Geospatial

namespace
{
FString NormalizeProvider(const FString& Value)
{
	return Value.TrimStartAndEnd().ToLower();
}

ACesiumGeoreference* ResolveCesiumGeoreference(UWorld* World)
{
	return World ? ACesiumGeoreference::GetDefaultGeoreference(World) : nullptr;
}
}

FCamSimGeospatialProvider::FCamSimGeospatialProvider(const FCamSimConfig& InConfig)
{
	ProviderName = NormalizeProvider(InConfig.TerrainProvider);
	if (ProviderName.IsEmpty())
	{
		ProviderName = TEXT("cesium");
	}

	if (ProviderName != TEXT("cesium"))
	{
		UE_LOG(LogCamSim, Warning,
			TEXT("GeospatialProvider: unsupported terrain provider '%s' -> falling back to 'cesium'"),
			*ProviderName);
		ProviderName = TEXT("cesium");
	}

	Capabilities.bSupportsGeoreferenceTransforms = true;
	Capabilities.bSupportsTerrainLineTraceQueries = true;
}

bool FCamSimGeospatialProvider::IsAvailable(UWorld* World) const
{
	if (ProviderName == TEXT("cesium"))
	{
		return ResolveCesiumGeoreference(World) != nullptr;
	}
	return false;
}

bool FCamSimGeospatialProvider::GeoToWorld(
	UWorld* World, double Lat, double Lon, double AltM, FVector& OutWorld) const
{
	if (ProviderName == TEXT("cesium"))
	{
		if (ACesiumGeoreference* GeoRef = ResolveCesiumGeoreference(World))
		{
			OutWorld = GeoRef->TransformLongitudeLatitudeHeightPositionToUnreal(FVector(Lon, Lat, AltM));
			return true;
		}
	}
	return false;
}

bool FCamSimGeospatialProvider::WorldToGeo(
	UWorld* World, const FVector& WorldPos, double& OutLat, double& OutLon, double& OutAltM) const
{
	if (ProviderName == TEXT("cesium"))
	{
		if (ACesiumGeoreference* GeoRef = ResolveCesiumGeoreference(World))
		{
			const FVector LLH = GeoRef->TransformUnrealPositionToLongitudeLatitudeHeight(WorldPos);
			OutLon = LLH.X;
			OutLat = LLH.Y;
			OutAltM = LLH.Z;
			return true;
		}
	}
	return false;
}

UCesiumIonServer* ApplyCesiumBackendConfig(
	UWorld* World, const FCamSimConfig::FCesiumBackendConfig& Config)
{
	if (!World)
	{
		return nullptr;
	}

	// --- Step 1: Ion server override ---
	// Skip if all three ion settings are at their default values.
	const bool bServerIsDefault =
		Config.IonPortalUrl == TEXT("https://ion.cesium.com") &&
		Config.IonApiUrl    == TEXT("https://api.cesium.com") &&
		Config.IonToken.IsEmpty();

	UCesiumIonServer* CustomServer = nullptr;
	if (!bServerIsDefault)
	{
		CustomServer = NewObject<UCesiumIonServer>(GetTransientPackage());
		CustomServer->ServerUrl             = Config.IonPortalUrl;
		CustomServer->ApiUrl                = Config.IonApiUrl;
		CustomServer->DefaultIonAccessToken = Config.IonToken;
		// Note: IonToken is intentionally not logged here or anywhere in this function.
	}

	// --- Steps 2 & 3: Terrain source + imagery overlay on the terrain tileset ---
	// CamSim streams one terrain tileset. Others in the level (Main.umap also
	// carries Cesium OSM Buildings) used to be overwritten with the terrain
	// config, i.e. a second copy of the terrain at twice the streaming,
	// collision and game-thread cost; they are removed instead.
	TArray<ACesium3DTileset*> Tilesets;
	TArray<int64> IonAssetIds;
	for (TActorIterator<ACesium3DTileset> It(World); It; ++It)
	{
		Tilesets.Add(*It);
		IonAssetIds.Add(It->GetTilesetSource() == ETilesetSource::FromCesiumIon ? It->GetIonAssetID() : -1);
	}
	const bool bFoundAnyTileset = Tilesets.Num() > 0;
	const int32 TerrainIndex = CamSim::Geospatial::SelectTerrainTileset(IonAssetIds);
	for (int32 i = 0; i < Tilesets.Num(); ++i)
	{
		if (i != TerrainIndex)
		{
			UE_LOG(LogCamSim, Log,
				TEXT("ApplyCesiumBackendConfig: removing extra tileset '%s' (ion asset %lld); terrain is '%s'"),
				*Tilesets[i]->GetName(), IonAssetIds[i], *Tilesets[TerrainIndex]->GetName());
			Tilesets[i]->Destroy();
		}
	}

	if (ACesium3DTileset* Tileset = Tilesets.IsValidIndex(TerrainIndex) ? Tilesets[TerrainIndex] : nullptr)
	{
		// Apply custom ion server to this tileset
		if (CustomServer)
		{
			Tileset->SetCesiumIonServer(CustomServer);
		}

		// Apply terrain source
		const FString& TerrainSrc = Config.Terrain.Source;
		if (TerrainSrc == TEXT("cesium_ion"))
		{
			Tileset->SetTilesetSource(ETilesetSource::FromCesiumIon);
			Tileset->SetIonAssetID(static_cast<int64>(Config.Terrain.IonAssetId));
		}
		else if (TerrainSrc == TEXT("url"))
		{
			Tileset->SetTilesetSource(ETilesetSource::FromUrl);
			Tileset->SetUrl(Config.Terrain.Url);
		}
		else if (TerrainSrc == TEXT("flat"))
		{
			Tileset->SetActorHiddenInGame(true);
			Tileset->SetActorEnableCollision(false);
		}
		else
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("ApplyCesiumBackendConfig: unknown terrain source '%s', using cesium_ion"),
				*TerrainSrc);
			Tileset->SetTilesetSource(ETilesetSource::FromCesiumIon);
			Tileset->SetIonAssetID(static_cast<int64>(Config.Terrain.IonAssetId));
		}

		// Refresh after terrain property changes once the tileset has begun play (it may have loaded).
		// Skip refresh for flat — the tileset is just hidden, no tile loading change needed.
		// Before BeginPlay (ACamSimGameMode::StartPlay) nothing has loaded yet and the first
		// load uses the new source; a refresh would only load it twice.
		if (TerrainSrc != TEXT("flat") && Tileset->HasActorBegunPlay())
		{
			Tileset->RefreshTileset();
		}

		// Remove any existing raster overlay components (always, including flat)
		TArray<UCesiumRasterOverlay*> ExistingOverlays;
		Tileset->GetComponents<UCesiumRasterOverlay>(ExistingOverlays);
		for (UCesiumRasterOverlay* O : ExistingOverlays)
		{
			O->Deactivate();
			O->DestroyComponent();
		}

		// Add new imagery overlay (skip for flat terrain or source: none)
		const bool bTerrainIsFlat = (TerrainSrc == TEXT("flat"));
		const FString& ImagerySrc = Config.Imagery.Source;

		if (bTerrainIsFlat)
		{
			UE_LOG(LogCamSim, Log,
				TEXT("ApplyCesiumBackendConfig: imagery source ignored — terrain is flat"));
		}
		else if (ImagerySrc == TEXT("cesium_ion"))
		{
			UCesiumIonRasterOverlay* O =
				NewObject<UCesiumIonRasterOverlay>(Tileset, TEXT("CamSimImagery"));
			O->IonAssetID = static_cast<int64>(Config.Imagery.IonAssetId);
			if (CustomServer) { O->CesiumIonServer = CustomServer; }
			O->SetMaximumScreenSpaceError(Config.Imagery.MaximumScreenSpaceError);
			if (Config.Imagery.MaximumTextureSize > 0)
			{
				O->SetMaximumTextureSize(Config.Imagery.MaximumTextureSize);
			}
			O->SetMaximumSimultaneousTileLoads(Config.Imagery.MaximumSimultaneousTileLoads);
			O->RegisterComponent();
			O->Activate(false); // false = activate without resetting internal state
		}
		else if (ImagerySrc == TEXT("wms"))
		{
			UCesiumWebMapServiceRasterOverlay* O =
				NewObject<UCesiumWebMapServiceRasterOverlay>(Tileset, TEXT("CamSimImagery"));
			O->BaseUrl    = Config.Imagery.WmsUrl;
			O->Layers     = Config.Imagery.WmsLayers;
			O->TileWidth  = Config.Imagery.WmsTileWidth;
			O->TileHeight = Config.Imagery.WmsTileHeight;
			O->SetMaximumScreenSpaceError(Config.Imagery.MaximumScreenSpaceError);
			if (Config.Imagery.MaximumTextureSize > 0)
			{
				O->SetMaximumTextureSize(Config.Imagery.MaximumTextureSize);
			}
			O->SetMaximumSimultaneousTileLoads(Config.Imagery.MaximumSimultaneousTileLoads);
			O->RegisterComponent();
			O->Activate(false);
		}
		else if (ImagerySrc == TEXT("tms"))
		{
			// Url names tilemapresource.xml itself; for file:// a directory URL fails.
			UCesiumTileMapServiceRasterOverlay* O =
				NewObject<UCesiumTileMapServiceRasterOverlay>(Tileset, TEXT("CamSimImagery"));
			O->Url = Config.Imagery.Url;
			O->SetMaximumScreenSpaceError(Config.Imagery.MaximumScreenSpaceError);
			if (Config.Imagery.MaximumTextureSize > 0)
			{
				O->SetMaximumTextureSize(Config.Imagery.MaximumTextureSize);
			}
			O->SetMaximumSimultaneousTileLoads(Config.Imagery.MaximumSimultaneousTileLoads);
			O->RegisterComponent();
			O->Activate(false);
		}
		else if (ImagerySrc != TEXT("none"))
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("ApplyCesiumBackendConfig: unknown imagery source '%s', treating as none"),
				*ImagerySrc);
		}
	}

	if (!bFoundAnyTileset && Config.Terrain.Source == TEXT("flat"))
	{
		UE_LOG(LogCamSim, Warning,
			TEXT("ApplyCesiumBackendConfig: terrain source is 'flat' but no ACesium3DTileset actors found in world"));
	}

	return CustomServer;
}

