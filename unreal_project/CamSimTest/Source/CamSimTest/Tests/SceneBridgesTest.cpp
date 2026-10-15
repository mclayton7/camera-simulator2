// Copyright CamSim Contributors. All Rights Reserved.

// REALISM R3 bridges: the package's features/bridges/ tileset at runtime (config, resolve, spawn, "not terrain").

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Config/CamSimConfig.h"
#include "Config/ScenePackage.h"
#include "Geospatial/CamSimGeospatialProvider.h"
#include "Geospatial/CesiumWorldSetup.h"
#include "Geospatial/SceneBuildings.h"
#include "Cesium3DTileset.h"
#include "CesiumGeoreference.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "EngineUtils.h"

namespace
{
	FString WriteEmptyTileset(const TCHAR* Sub)
	{
		const FString D = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("Scene"), TEXT("bridges_spawn")));
		IFileManager::Get().MakeDirectory(*(D / Sub), true);
		const FString File = D / Sub / TEXT("tileset.json");
		FFileHelper::SaveStringToFile(TEXT(
			"{\"asset\":{\"version\":\"1.1\"},\"geometricError\":0,\"root\":{\"boundingVolume\":{\"region\":"
			"[-2.05,0.579,-2.049,0.58,0,10]},\"geometricError\":0,\"refine\":\"ADD\"}}"), *File);
		return CamSimScene::PathToFileUrl(File);
	}

	struct FBridgesWorld
	{
		UWorld* World = nullptr;
		FBridgesWorld()
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
		~FBridgesWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}
		TArray<ACesium3DTileset*> Tilesets() const
		{
			TArray<ACesium3DTileset*> Out;
			for (TActorIterator<ACesium3DTileset> It(World); It; ++It) { Out.Add(*It); }
			return Out;
		}
	};

	FCamSimConfig BridgesConfig()
	{
		FCamSimConfig Cfg;
		Cfg.Render.OriginShiftDistanceM = 20000.0;
		Cfg.CesiumBackend.Terrain.Source = TEXT("flat");
		Cfg.CesiumBackend.Imagery.Source = TEXT("none");
		Cfg.Scene.BuildingsUrl = WriteEmptyTileset(TEXT("features/buildings"));
		Cfg.Scene.BridgesUrl = WriteEmptyTileset(TEXT("features/bridges"));
		Cfg.Scene.BridgesMaximumScreenSpaceError = 48.0;
		Cfg.Scene.BridgesMaximumCachedBytesMB = 100;
		return Cfg;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneBridgesSpawnTest,
	"CamSim.Scene.Bridges.SpawnTagsAndTunes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneBridgesSpawnTest::RunTest(const FString& Parameters)
{
	AddExpectedMessage(TEXT("No game viewport was found"), EAutomationExpectedErrorFlags::Contains, 0);
	FBridgesWorld W;
	const FCamSimConfig Cfg = BridgesConfig();
	CamSim::Geospatial::SetUpCesiumWorld(W.World, Cfg);
	TestEqual(TEXT("terrain + buildings + bridges"), W.Tilesets().Num(), 3);
	ACesium3DTileset* B = CamSim::Geospatial::FindBridgesTileset(W.World);
	if (!TestNotNull(TEXT("bridges tileset"), B)) return false;
	TestTrue(TEXT("tagged bridges"), CamSim::Geospatial::IsBridgesActor(B));
	TestFalse(TEXT("not buildings"), CamSim::Geospatial::IsBuildingsActor(B));
	TestTrue(TEXT("a feature tileset"), CamSim::Geospatial::IsFeatureTilesetActor(B));
	TestTrue(TEXT("buildings are a feature tileset too"),
		CamSim::Geospatial::IsFeatureTilesetActor(CamSim::Geospatial::FindBuildingsTileset(W.World)));
	TestEqual(TEXT("url"), B->GetUrl(), Cfg.Scene.BridgesUrl);
	TestEqual(TEXT("own sse"), B->MaximumScreenSpaceError, 48.0);
	TestTrue(TEXT("physics (CIGI and the DIS clamp hit decks)"), B->GetCreatePhysicsMeshes());
	TestEqual(TEXT("own cache cap"), B->MaximumCachedBytes, int64(100) * 1024 * 1024);
	TestTrue(TEXT("movable"), B->GetRootComponent()->Mobility == EComponentMobility::Movable);
	TestTrue(TEXT("idempotent"), CamSim::Geospatial::SpawnBridgesTileset(W.World, Cfg) == B);
	TestEqual(TEXT("still three"), W.Tilesets().Num(), 3);
	TestFalse(TEXT("null actor is not a feature tileset"), CamSim::Geospatial::IsFeatureTilesetActor(nullptr));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneBridgesDisabledTest,
	"CamSim.Scene.Bridges.DisabledSpawnsNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneBridgesDisabledTest::RunTest(const FString& Parameters)
{
	AddExpectedMessage(TEXT("No game viewport was found"), EAutomationExpectedErrorFlags::Contains, 0);
	FBridgesWorld W;
	FCamSimConfig Cfg = BridgesConfig();
	Cfg.Scene.bBridgesEnabled = false;
	CamSim::Geospatial::SetUpCesiumWorld(W.World, Cfg);
	TestEqual(TEXT("terrain + buildings"), W.Tilesets().Num(), 2);
	TestNull(TEXT("no bridges"), CamSim::Geospatial::FindBridgesTileset(W.World));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneBridgesNotTerrainTest,
	"CamSim.Scene.Bridges.NotTerrain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneBridgesNotTerrainTest::RunTest(const FString& Parameters)
{
	AddExpectedMessage(TEXT("No game viewport was found"), EAutomationExpectedErrorFlags::Contains, 0);
	FBridgesWorld W;
	FCamSimConfig Cfg = BridgesConfig();
	Cfg.Scene.BuildingsUrl.Reset();
	CamSim::Geospatial::SpawnBridgesTileset(W.World, Cfg);
	const TArray<ACesium3DTileset*> All = W.Tilesets();
	ACesium3DTileset* Terrain = nullptr;
	for (ACesium3DTileset* T : All) { if (!CamSim::Geospatial::IsBridgesActor(T)) Terrain = T; }
	TestEqual(TEXT("untagged terrain + bridges"), All.Num(), 2);
	TestTrue(TEXT("the lone non-feature tileset is the terrain"), CamSim::Geospatial::PickTerrainTileset(All) == Terrain);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneBridgesConfigTest,
	"CamSim.Scene.Bridges.Config",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneBridgesConfigTest::RunTest(const FString& Parameters)
{
	{
		const FCamSimConfig Cfg;
		TestTrue(TEXT("default enabled"), Cfg.Scene.bBridgesEnabled);
		TestEqual(TEXT("default sse"), Cfg.Scene.BridgesMaximumScreenSpaceError, 64.0);
		TestTrue(TEXT("default physics"), Cfg.Scene.bBridgesCreatePhysicsMeshes);
		TestEqual(TEXT("default cache MB"), Cfg.Scene.BridgesMaximumCachedBytesMB, 128);
	}
	{
		const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT(
			"scene:\n  bridges:\n    enabled: false\n    maximum_screen_space_error: 32\n    create_physics_meshes: false\n"
			"    maximum_cached_bytes_mb: 300\n"));
		TestFalse(TEXT("yaml enabled"), Cfg.Scene.bBridgesEnabled);
		TestEqual(TEXT("yaml sse"), Cfg.Scene.BridgesMaximumScreenSpaceError, 32.0);
		TestFalse(TEXT("yaml physics"), Cfg.Scene.bBridgesCreatePhysicsMeshes);
		TestEqual(TEXT("yaml cache MB"), Cfg.Scene.BridgesMaximumCachedBytesMB, 300);
	}
	{
		ON_SCOPE_EXIT
		{
			FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_SCENE_BRIDGES_ENABLED"), TEXT(""));
			FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_SCENE_BRIDGES_MAXIMUM_SCREEN_SPACE_ERROR"), TEXT(""));
			FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_SCENE_BRIDGES_CREATE_PHYSICS_MESHES"), TEXT(""));
			FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_SCENE_BRIDGES_MAXIMUM_CACHED_BYTES_MB"), TEXT(""));
		};
		FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_SCENE_BRIDGES_ENABLED"), TEXT("0"));
		FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_SCENE_BRIDGES_MAXIMUM_SCREEN_SPACE_ERROR"), TEXT("8"));
		FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_SCENE_BRIDGES_CREATE_PHYSICS_MESHES"), TEXT("0"));
		FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_SCENE_BRIDGES_MAXIMUM_CACHED_BYTES_MB"), TEXT("64"));
		const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT("{}"));
		TestFalse(TEXT("env enabled"), Cfg.Scene.bBridgesEnabled);
		TestEqual(TEXT("env sse"), Cfg.Scene.BridgesMaximumScreenSpaceError, 8.0);
		TestFalse(TEXT("env physics"), Cfg.Scene.bBridgesCreatePhysicsMeshes);
		TestEqual(TEXT("env cache MB"), Cfg.Scene.BridgesMaximumCachedBytesMB, 64);
	}
	{
		FCamSimConfig Cfg;
		Cfg.Scene.BridgesMaximumScreenSpaceError = 0.0;
		TestTrue(TEXT("sse 0 rejected"), Cfg.Validate().ContainsByPredicate(
			[](const FString& E) { return E.Contains(TEXT("scene.bridges.maximum_screen_space_error")); }));
	}
	for (const int32 Mb : { 0, 65537 })
	{
		FCamSimConfig Cfg;
		Cfg.Scene.BridgesMaximumCachedBytesMB = Mb;
		TestTrue(FString::Printf(TEXT("cache %d MB rejected"), Mb), Cfg.Validate().ContainsByPredicate(
			[](const FString& E) { return E.Contains(TEXT("scene.bridges.maximum_cached_bytes_mb")); }));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneResolveBridgesTest,
	"CamSim.Scene.Resolve.Bridges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneResolveBridgesTest::RunTest(const FString& Parameters)
{
	const FString D = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("Scene"), TEXT("bridges_resolve")));
	IFileManager::Get().DeleteDirectory(*D, false, true);
	IFileManager::Get().MakeDirectory(*D, true);
	FFileHelper::SaveStringToFile(TEXT("{\"schema_version\": 1, \"name\": \"testpkg\"}"), *(D / TEXT("manifest.json")));
	IFileManager::Get().MakeDirectory(*(D / TEXT("terrain")), true);
	FFileHelper::SaveStringToFile(TEXT("{}"), *(D / TEXT("terrain/layer.json")));
	FCamSimConfig Cfg;
	Cfg.Scene.Dir = D;
	CamSimScene::ResolvePackage(Cfg);
	TestTrue(TEXT("no tileset: no url"), Cfg.Scene.BridgesUrl.IsEmpty());
	TestFalse(TEXT("no tileset: no spawn"), CamSimScene::ShouldSpawnBridges(Cfg));
	IFileManager::Get().MakeDirectory(*(D / TEXT("features/bridges")), true);
	FFileHelper::SaveStringToFile(TEXT("{}"), *(D / TEXT("features/bridges/tileset.json")));
	CamSimScene::ResolvePackage(Cfg);
	TestEqual(TEXT("url"), Cfg.Scene.BridgesUrl, CamSimScene::PathToFileUrl(D / TEXT("features/bridges/tileset.json")));
	TestTrue(TEXT("spawn"), CamSimScene::ShouldSpawnBridges(Cfg));
	Cfg.Scene.bBridgesEnabled = false;
	TestFalse(TEXT("disabled: no spawn"), CamSimScene::ShouldSpawnBridges(Cfg));
	Cfg.Scene.Dir.Reset();
	CamSimScene::ResolvePackage(Cfg);
	TestTrue(TEXT("cleared when scene.dir is unset"), Cfg.Scene.BridgesUrl.IsEmpty());
	return true;
}
