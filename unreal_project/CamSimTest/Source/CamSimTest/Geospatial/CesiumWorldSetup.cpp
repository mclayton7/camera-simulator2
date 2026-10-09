// Copyright CamSim Contributors. All Rights Reserved.

#include "Geospatial/CesiumWorldSetup.h"
#include "Config/CamSimConfig.h"
#include "Geospatial/CamSimGeospatialProvider.h"
#include "Geospatial/CesiumTuning.h"

#include "Cesium3DTileset.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace CamSim::Geospatial
{
UCesiumIonServer* SetUpCesiumWorld(UWorld* World, const FCamSimConfig& Cfg)
{
	if (!World)
	{
		return nullptr;
	}
	if (Cfg.Render.OriginShiftDistanceM > 0.0)
	{
		// ChangeCesiumGeoreference moves tilesets, so they must be Movable.
		for (TActorIterator<ACesium3DTileset> It(World); It; ++It)
		{
			if (USceneComponent* TilesetRoot = It->GetRootComponent())
			{
				TilesetRoot->SetMobility(EComponentMobility::Movable);
			}
		}
	}
	ApplyCesiumTilesetTuning(World, Cfg);
	return ::ApplyCesiumBackendConfig(World, Cfg.CesiumBackend);   // declared at global scope
}

bool ShouldSetUpCesiumWorld(const UWorld* World, const UGameInstance* Ours)
{
	if (!World || !World->IsGameWorld())
	{
		return false;
	}
	const UGameInstance* Owner = World->GetGameInstance();
	return Owner == nullptr || Owner == Ours;
}
} // namespace CamSim::Geospatial
