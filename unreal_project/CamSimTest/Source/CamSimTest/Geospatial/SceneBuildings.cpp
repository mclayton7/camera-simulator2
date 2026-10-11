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

bool IsBuildingsActor(const AActor* Actor)
{
	return Actor != nullptr && Actor->ActorHasTag(BuildingsTag);
}

ACesium3DTileset* FindBuildingsTileset(UWorld* World)
{
	if (!World) return nullptr;
	for (TActorIterator<ACesium3DTileset> It(World); It; ++It)
	{
		if (IsBuildingsActor(*It)) return *It;
	}
	return nullptr;
}

ACesium3DTileset* SpawnBuildingsTileset(UWorld* World, const FCamSimConfig& Cfg)
{
	if (!World || !CamSimScene::ShouldSpawnBuildings(Cfg)) return nullptr;
	if (ACesium3DTileset* Existing = FindBuildingsTileset(World)) return Existing;

	FActorSpawnParameters Params;
	Params.Name = TEXT("CamSimBuildings");
	Params.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
	Params.bDeferConstruction = true;
	ACesium3DTileset* T = World->SpawnActor<ACesium3DTileset>(ACesium3DTileset::StaticClass(), FTransform::Identity, Params);
	if (!T) return nullptr;
	T->Tags.Add(BuildingsTag);
	T->SetTilesetSource(ETilesetSource::FromUrl);
	T->SetUrl(Cfg.Scene.BuildingsUrl);
	TuneTileset(*T, Cfg);
	T->MaximumScreenSpaceError = Cfg.Scene.BuildingsMaximumScreenSpaceError;
	T->SetCreatePhysicsMeshes(Cfg.Scene.bBuildingsCreatePhysicsMeshes);
	T->FinishSpawning(FTransform::Identity);
	if (Cfg.Render.OriginShiftDistanceM > 0.0)
	{
		if (USceneComponent* Root = T->GetRootComponent()) Root->SetMobility(EComponentMobility::Movable);
	}
	UE_LOG(LogCamSim, Log, TEXT("CamSim: buildings tileset %s (SSE=%.1f physicsMeshes=%d)"),
		*Cfg.Scene.BuildingsUrl, Cfg.Scene.BuildingsMaximumScreenSpaceError, (int)Cfg.Scene.bBuildingsCreatePhysicsMeshes);
	return T;
}
}
