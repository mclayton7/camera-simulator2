// Copyright CamSim Contributors. All Rights Reserved.

#include "Geospatial/SceneBuildings.h"
#include "CamSimTest.h"
#include "Config/CamSimConfig.h"
#include "Config/ScenePackage.h"
#include "Geospatial/CesiumTuning.h"

#include "Cesium3DTileset.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace CamSim::Geospatial
{
const FName BuildingsTag(TEXT("CamSimBuildings"));
const FName BridgesTag(TEXT("CamSimBridges"));

bool IsBuildingsActor(const AActor* Actor)
{
	return Actor != nullptr && Actor->ActorHasTag(BuildingsTag);
}

bool IsBridgesActor(const AActor* Actor)
{
	return Actor != nullptr && Actor->ActorHasTag(BridgesTag);
}

bool IsFeatureTilesetActor(const AActor* Actor)
{
	return IsBuildingsActor(Actor) || IsBridgesActor(Actor);
}

namespace
{
	ACesium3DTileset* FindTagged(UWorld* World, const FName& Tag)
	{
		if (!World) return nullptr;
		for (TActorIterator<ACesium3DTileset> It(World); It; ++It)
		{
			if (It->ActorHasTag(Tag)) return *It;
		}
		return nullptr;
	}

	struct FFeatureTilesetSpec
	{
		const FName& Tag;
		const TCHAR* Name;
		const TCHAR* Label;   // the log's "<label> tileset" (scripts grep "buildings tileset")
		const FString& Url;
		double MaximumScreenSpaceError;
		int32 MaximumCachedBytesMB;
		bool bPhysicsMeshes;
	};

	/** One feature tileset: the terrain's tuning, then its own SSE, cache cap and physics; Movable when origin shifting
	 *  is on; no raster overlays. Returns the existing one on a repeat call. */
	ACesium3DTileset* SpawnFeatureTileset(UWorld* World, const FCamSimConfig& Cfg, const FFeatureTilesetSpec& S)
	{
		if (ACesium3DTileset* Existing = FindTagged(World, S.Tag)) return Existing;

		FActorSpawnParameters Params;
		Params.Name = S.Name;
		Params.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
		Params.bDeferConstruction = true;
		ACesium3DTileset* T = World->SpawnActor<ACesium3DTileset>(ACesium3DTileset::StaticClass(), FTransform::Identity, Params);
		if (!T) return nullptr;
		T->Tags.Add(S.Tag);
		T->SetTilesetSource(ETilesetSource::FromUrl);
		T->SetUrl(S.Url);
		TuneTileset(*T, Cfg);
		T->MaximumScreenSpaceError = S.MaximumScreenSpaceError;
		// Its own cache cap: TuneTileset gave it the terrain's (2 GiB), a second full cache over a built-up flight.
		T->MaximumCachedBytes = static_cast<int64>(S.MaximumCachedBytesMB) * 1024LL * 1024LL;
		T->SetCreatePhysicsMeshes(S.bPhysicsMeshes);
		// The streaming controller re-applies this per frame as the FOV-scaled terrain value changes.
		T->CulledScreenSpaceError = ResolveTilesetCulledScreenSpaceError(
			ResolveCulledScreenSpaceError(Cfg), true, S.MaximumScreenSpaceError);
		T->FinishSpawning(FTransform::Identity);
		if (Cfg.Render.OriginShiftDistanceM > 0.0)
		{
			if (USceneComponent* Root = T->GetRootComponent()) Root->SetMobility(EComponentMobility::Movable);
		}
		UE_LOG(LogCamSim, Log,
			TEXT("CamSim: %s tileset '%s' %s (maxLoads=%d SSE=%.1f culledSSE=%.0f cacheMB=%d descLimit=%d frustumCull=%d lodBlend=%d logStats=%d physicsMeshes=%d)"),
			S.Label, *T->GetName(), *S.Url, T->MaximumSimultaneousTileLoads, T->MaximumScreenSpaceError,
			T->CulledScreenSpaceError, S.MaximumCachedBytesMB, T->LoadingDescendantLimit, (int)T->EnableFrustumCulling,
			(int)T->GetUseLodTransitions(), (int)T->LogSelectionStats, (int)T->GetCreatePhysicsMeshes());
		return T;
	}
}

ACesium3DTileset* FindBuildingsTileset(UWorld* World)
{
	return FindTagged(World, BuildingsTag);
}

ACesium3DTileset* FindBridgesTileset(UWorld* World)
{
	return FindTagged(World, BridgesTag);
}

ACesium3DTileset* SpawnBuildingsTileset(UWorld* World, const FCamSimConfig& Cfg)
{
	if (!World || !CamSimScene::ShouldSpawnBuildings(Cfg)) return nullptr;
	return SpawnFeatureTileset(World, Cfg, { BuildingsTag, TEXT("CamSimBuildings"), TEXT("buildings"), Cfg.Scene.BuildingsUrl,
		Cfg.Scene.BuildingsMaximumScreenSpaceError, Cfg.Scene.BuildingsMaximumCachedBytesMB,
		Cfg.Scene.bBuildingsCreatePhysicsMeshes });
}

ACesium3DTileset* SpawnBridgesTileset(UWorld* World, const FCamSimConfig& Cfg)
{
	if (!World || !CamSimScene::ShouldSpawnBridges(Cfg)) return nullptr;
	return SpawnFeatureTileset(World, Cfg, { BridgesTag, TEXT("CamSimBridges"), TEXT("bridges"), Cfg.Scene.BridgesUrl,
		Cfg.Scene.BridgesMaximumScreenSpaceError, Cfg.Scene.BridgesMaximumCachedBytesMB,
		Cfg.Scene.bBridgesCreatePhysicsMeshes });
}
}
