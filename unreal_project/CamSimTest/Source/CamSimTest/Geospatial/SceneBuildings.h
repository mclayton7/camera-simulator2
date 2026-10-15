// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class AActor;
class ACesium3DTileset;
class UWorld;
struct FCamSimConfig;

/**
 * The scene package's feature tilesets, spawned at world init after the terrain set-up's destroy loop and recognised
 * everywhere by their actor tags: the buildings (REALISM R3: CIGI material code 230, the DIS surface probe's ignore
 * list) and the bridge decks (REALISM R3 bridges: CIGI hits report the land-cover code, the DIS probe's deck ceiling).
 * Neither is the terrain (IsFeatureTilesetActor).
 */
namespace CamSim::Geospatial
{
	extern CAMSIMTEST_API const FName BuildingsTag;
	extern CAMSIMTEST_API const FName BridgesTag;

	CAMSIMTEST_API bool IsBuildingsActor(const AActor* Actor);
	CAMSIMTEST_API bool IsBridgesActor(const AActor* Actor);
	/** Buildings or bridges: a package feature tileset, never the terrain. */
	CAMSIMTEST_API bool IsFeatureTilesetActor(const AActor* Actor);
	CAMSIMTEST_API ACesium3DTileset* FindBuildingsTileset(UWorld* World);
	CAMSIMTEST_API ACesium3DTileset* FindBridgesTileset(UWorld* World);

	/**
	 * Spawn (once) the tileset at Cfg.Scene.BuildingsUrl with the terrain's tuning, then the buildings' own SSE and
	 * physics; Movable when origin shifting is on; no raster overlays. nullptr when CamSimScene::ShouldSpawnBuildings
	 * is false. Returns the existing one on a repeat call.
	 */
	CAMSIMTEST_API ACesium3DTileset* SpawnBuildingsTileset(UWorld* World, const FCamSimConfig& Cfg);

	/** As SpawnBuildingsTileset, for Cfg.Scene.BridgesUrl with the scene.bridges.* settings (CamSimScene::ShouldSpawnBridges). */
	CAMSIMTEST_API ACesium3DTileset* SpawnBridgesTileset(UWorld* World, const FCamSimConfig& Cfg);
}
