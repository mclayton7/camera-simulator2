// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class AActor;
class ACesium3DTileset;
class UWorld;
struct FCamSimConfig;

/**
 * The scene package's buildings tileset (REALISM R3): one ACesium3DTileset spawned at world init after the
 * terrain set-up's destroy loop, recognised everywhere by its actor tag (CIGI material code 230, the DIS surface
 * probe's ignore list).
 */
namespace CamSim::Geospatial
{
	extern CAMSIMTEST_API const FName BuildingsTag;

	CAMSIMTEST_API bool IsBuildingsActor(const AActor* Actor);
	CAMSIMTEST_API ACesium3DTileset* FindBuildingsTileset(UWorld* World);

	/**
	 * Spawn (once) the tileset at Cfg.Scene.BuildingsUrl with the terrain's tuning, then the buildings' own SSE and
	 * physics; Movable when origin shifting is on; no raster overlays. nullptr when CamSimScene::ShouldSpawnBuildings
	 * is false. Returns the existing one on a repeat call.
	 */
	CAMSIMTEST_API ACesium3DTileset* SpawnBuildingsTileset(UWorld* World, const FCamSimConfig& Cfg);
}
