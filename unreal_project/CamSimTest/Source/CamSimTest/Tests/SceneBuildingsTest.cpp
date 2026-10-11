// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Config/CamSimConfig.h"
#include "Config/ScenePackage.h"
#include "Geospatial/CesiumTuning.h"
#include "Geospatial/CesiumWorldSetup.h"
#include "Geospatial/SceneBuildings.h"
#include "Cesium3DTileset.h"
#include "CesiumGeoreference.h"
#include "CesiumRasterOverlay.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "EngineUtils.h"

namespace
{
	/** A valid, empty 3D Tiles 1.1 tileset on disk, so Cesium loads it without a request or an error. */
	FString WriteEmptyTileset()
	{
		const FString D = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("Scene"), TEXT("buildings_spawn")));
		IFileManager::Get().MakeDirectory(*(D / TEXT("features/buildings")), true);
		const FString File = D / TEXT("features/buildings/tileset.json");
		FFileHelper::SaveStringToFile(TEXT(
			"{\"asset\":{\"version\":\"1.1\"},\"geometricError\":0,\"root\":{\"boundingVolume\":{\"region\":"
			"[-2.05,0.579,-2.049,0.58,0,10]},\"geometricError\":0,\"refine\":\"ADD\"}}"), *File);
		return CamSimScene::PathToFileUrl(File);
	}

	struct FTestWorld
	{
		UWorld* World = nullptr;
		FTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, false);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->GetWorldSettings()->bEnableWorldBoundsChecks = false;
			World->SpawnActor<ACesiumGeoreference>();
			ACesium3DTileset* T = World->SpawnActorDeferred<ACesium3DTileset>(
				ACesium3DTileset::StaticClass(), FTransform::Identity);
			T->SetTilesetSource(ETilesetSource::FromEllipsoid);
			T->FinishSpawning(FTransform::Identity);
		}
		~FTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}
		int32 CountTilesets() const
		{
			int32 N = 0;
			for (TActorIterator<ACesium3DTileset> It(World); It; ++It) { ++N; }
			return N;
		}
	};

	FCamSimConfig BuildingsConfig(const FString& Url)
	{
		FCamSimConfig Cfg;
		Cfg.Render.OriginShiftDistanceM = 20000.0;
		Cfg.CesiumBackend.Terrain.Source = TEXT("flat");
		Cfg.CesiumBackend.Imagery.Source = TEXT("none");
		Cfg.Scene.BuildingsUrl = Url;
		Cfg.Scene.BuildingsMaximumScreenSpaceError = 24.0;
		Cfg.Scene.bBuildingsCreatePhysicsMeshes = true;
		return Cfg;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneBuildingsSpawnTest,
	"CamSim.Scene.Buildings.SpawnAfterDestroyLoop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneBuildingsSpawnTest::RunTest(const FString& Parameters)
{
	AddExpectedMessage(TEXT("No game viewport was found"), EAutomationExpectedErrorFlags::Contains, 0);
	FTestWorld W;
	const FCamSimConfig Cfg = BuildingsConfig(WriteEmptyTileset());
	CamSim::Geospatial::SetUpCesiumWorld(W.World, Cfg);
	TestEqual(TEXT("terrain + buildings"), W.CountTilesets(), 2);
	ACesium3DTileset* B = CamSim::Geospatial::FindBuildingsTileset(W.World);
	if (!TestNotNull(TEXT("buildings tileset"), B)) return false;
	TestTrue(TEXT("tagged"), CamSim::Geospatial::IsBuildingsActor(B));
	TestTrue(TEXT("FromUrl"), B->GetTilesetSource() == ETilesetSource::FromUrl);
	TestEqual(TEXT("url"), B->GetUrl(), Cfg.Scene.BuildingsUrl);
	TestEqual(TEXT("own sse"), B->MaximumScreenSpaceError, 24.0);
	TestTrue(TEXT("physics"), B->GetCreatePhysicsMeshes());
	TestEqual(TEXT("culled sse never finer than its own (terrain 16, buildings 24)"), B->CulledScreenSpaceError, 24.0);
	TestTrue(TEXT("movable"), B->GetRootComponent()->Mobility == EComponentMobility::Movable);
	TArray<UCesiumRasterOverlay*> Overlays;
	B->GetComponents<UCesiumRasterOverlay>(Overlays);
	TestEqual(TEXT("no overlays"), Overlays.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneBuildingsIdempotentTest,
	"CamSim.Scene.Buildings.SpawnIsIdempotent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneBuildingsIdempotentTest::RunTest(const FString& Parameters)
{
	AddExpectedMessage(TEXT("No game viewport was found"), EAutomationExpectedErrorFlags::Contains, 0);
	FTestWorld W;
	const FCamSimConfig Cfg = BuildingsConfig(WriteEmptyTileset());
	ACesium3DTileset* First = CamSim::Geospatial::SpawnBuildingsTileset(W.World, Cfg);
	ACesium3DTileset* Second = CamSim::Geospatial::SpawnBuildingsTileset(W.World, Cfg);
	TestTrue(TEXT("same actor"), First != nullptr && First == Second);
	TestEqual(TEXT("one buildings tileset"), W.CountTilesets(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneBuildingsDisabledTest,
	"CamSim.Scene.Buildings.DisabledSpawnsNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneBuildingsDisabledTest::RunTest(const FString& Parameters)
{
	AddExpectedMessage(TEXT("No game viewport was found"), EAutomationExpectedErrorFlags::Contains, 0);
	FTestWorld W;
	FCamSimConfig Cfg = BuildingsConfig(WriteEmptyTileset());
	Cfg.Scene.bBuildingsEnabled = false;
	CamSim::Geospatial::SetUpCesiumWorld(W.World, Cfg);
	TestEqual(TEXT("terrain only"), W.CountTilesets(), 1);
	TestNull(TEXT("none found"), CamSim::Geospatial::FindBuildingsTileset(W.World));
	TestFalse(TEXT("null actor is not buildings"), CamSim::Geospatial::IsBuildingsActor(nullptr));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneBuildingsCulledSseTest,
	"CamSim.Scene.Buildings.CulledSse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneBuildingsCulledSseTest::RunTest(const FString& Parameters)
{
	using CamSim::Geospatial::ResolveTilesetCulledScreenSpaceError;
	TestEqual(TEXT("terrain keeps the terrain value"), ResolveTilesetCulledScreenSpaceError(16.0, false, 2.0), 16.0);
	TestEqual(TEXT("terrain keeps it even when coarser than its max"),
		ResolveTilesetCulledScreenSpaceError(4.0, false, 16.0), 4.0);
	TestEqual(TEXT("buildings coarser than terrain use their own SSE"),
		ResolveTilesetCulledScreenSpaceError(16.0, true, 24.0), 24.0);
	TestEqual(TEXT("buildings finer than the (FOV-scaled) terrain value follow the terrain"),
		ResolveTilesetCulledScreenSpaceError(48.0, true, 24.0), 48.0);
	return true;
}
