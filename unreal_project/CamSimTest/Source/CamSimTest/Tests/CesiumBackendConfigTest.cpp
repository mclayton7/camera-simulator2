// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"
#include "Geospatial/CamSimGeospatialProvider.h"
#include "Geospatial/CesiumWorldSetup.h"
#include "Cesium3DTileset.h"
#include "CesiumGeoreference.h"
#include "CesiumTileMapServiceRasterOverlay.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "EngineUtils.h"

// -------------------------------------------------------------------------
// Cesium Backend Configuration — Automation Tests
// -------------------------------------------------------------------------

// 1. FCesiumBackendConfig defaults
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCesiumBackendConfigDefaultsTest,
	"CamSim.CesiumBackend.ConfigDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCesiumBackendConfigDefaultsTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;

	TestEqual(TEXT("IonPortalUrl default"), Cfg.CesiumBackend.IonPortalUrl, TEXT("https://ion.cesium.com"));
	TestEqual(TEXT("IonApiUrl default"),    Cfg.CesiumBackend.IonApiUrl,    TEXT("https://api.cesium.com"));
	TestTrue (TEXT("IonToken empty by default"), Cfg.CesiumBackend.IonToken.IsEmpty());

	TestEqual(TEXT("Terrain.Source default"),     Cfg.CesiumBackend.Terrain.Source,     TEXT("cesium_ion"));
	TestEqual(TEXT("Terrain.IonAssetId default"),  Cfg.CesiumBackend.Terrain.IonAssetId, 1);
	TestTrue (TEXT("Terrain.Url empty by default"), Cfg.CesiumBackend.Terrain.Url.IsEmpty());

	TestEqual(TEXT("Imagery.Source default"),       Cfg.CesiumBackend.Imagery.Source,       TEXT("cesium_ion"));
	TestEqual(TEXT("Imagery.IonAssetId default"),    Cfg.CesiumBackend.Imagery.IonAssetId,   2);
	TestTrue (TEXT("Imagery.WmsUrl empty by default"), Cfg.CesiumBackend.Imagery.WmsUrl.IsEmpty());
	TestEqual(TEXT("Imagery.WmsTileWidth default"),  Cfg.CesiumBackend.Imagery.WmsTileWidth,  256);
	TestEqual(TEXT("Imagery.WmsTileHeight default"), Cfg.CesiumBackend.Imagery.WmsTileHeight, 256);

	return true;
}

// The level's terrain tileset: Cesium World Terrain wherever it sits, else the
// first tileset. Main.umap's second tileset (OSM Buildings, 96188) used to be
// overwritten into a duplicate terrain.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCesiumBackendSelectTerrainTilesetTest,
	"CamSim.CesiumBackend.SelectTerrainTileset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCesiumBackendSelectTerrainTilesetTest::RunTest(const FString& Parameters)
{
	using CamSim::Geospatial::SelectTerrainTileset;
	TestEqual(TEXT("Main.umap order: terrain first"), SelectTerrainTileset({1, 96188}), 0);
	TestEqual(TEXT("terrain after buildings"), SelectTerrainTileset({96188, 1}), 1);
	TestEqual(TEXT("no World Terrain: first"), SelectTerrainTileset({-1, 96188}), 0);
	TestEqual(TEXT("single URL tileset"), SelectTerrainTileset({-1}), 0);
	TestEqual(TEXT("none"), SelectTerrainTileset({}), INDEX_NONE);
	return true;
}

// REALISM R0: configured before BeginPlay, the level's other tilesets are gone and the
// terrain has its final source before it ever loads. Tilesets are spawned FromEllipsoid so
// the test makes no request and touches no file.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCesiumBackendSetUpBeforeBeginPlayTest,
	"CamSim.CesiumBackend.SetUpBeforeBeginPlay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCesiumBackendSetUpBeforeBeginPlayTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	};
	// Cesium's camera lookup on spawn: a test world has no game viewport.
	AddExpectedMessage(TEXT("No game viewport was found"), EAutomationExpectedErrorFlags::Contains, 0);
	World->GetWorldSettings()->bEnableWorldBoundsChecks = false;   // as in Main.umap (Cesium warns otherwise)
	World->SpawnActor<ACesiumGeoreference>();

	auto SpawnEllipsoidTileset = [World]()
	{
		ACesium3DTileset* T = World->SpawnActorDeferred<ACesium3DTileset>(
			ACesium3DTileset::StaticClass(), FTransform::Identity);
		T->SetTilesetSource(ETilesetSource::FromEllipsoid);
		T->FinishSpawning(FTransform::Identity);
		return T;
	};
	ACesium3DTileset* Terrain = SpawnEllipsoidTileset();
	SpawnEllipsoidTileset();   // stands in for Main.umap's OSM Buildings

	FCamSimConfig Cfg;
	Cfg.Render.OriginShiftDistanceM = 20000.0;
	Cfg.CesiumBackend.Terrain.Source = TEXT("url");
	Cfg.CesiumBackend.Terrain.Url    = TEXT("file:///nonexistent/terrain/layer.json");
	Cfg.CesiumBackend.Imagery.Source = TEXT("tms");
	Cfg.CesiumBackend.Imagery.Url    = TEXT("file:///nonexistent/imagery/tilemapresource.xml");

	TestFalse(TEXT("world has not begun play"), World->HasBegunPlay());
	CamSim::Geospatial::SetUpCesiumWorld(World, Cfg);

	TArray<ACesium3DTileset*> Left;
	for (TActorIterator<ACesium3DTileset> It(World); It; ++It) { Left.Add(*It); }
	TestEqual(TEXT("one tileset left"), Left.Num(), 1);
	TestTrue(TEXT("the first one is kept"), Left.Num() == 1 && Left[0] == Terrain);
	TestFalse(TEXT("terrain has not begun play"), Terrain->HasActorBegunPlay());
	TestTrue(TEXT("terrain source is FromUrl"), Terrain->GetTilesetSource() == ETilesetSource::FromUrl);
	TestEqual(TEXT("terrain url"), Terrain->GetUrl(), Cfg.CesiumBackend.Terrain.Url);
	TestTrue(TEXT("movable for origin shift"), Terrain->GetRootComponent()->Mobility == EComponentMobility::Movable);

	TArray<UCesiumTileMapServiceRasterOverlay*> Overlays;
	Terrain->GetComponents<UCesiumTileMapServiceRasterOverlay>(Overlays);
	TestEqual(TEXT("one TMS overlay"), Overlays.Num(), 1);
	TestTrue(TEXT("TMS url"), Overlays.Num() == 1 && Overlays[0]->Url == Cfg.CesiumBackend.Imagery.Url);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCesiumBackendSetupLatchTest,
	"CamSim.CesiumBackend.SetupLatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCesiumBackendSetupLatchTest::RunTest(const FString& Parameters)
{
	UWorld* A = UWorld::CreateWorld(EWorldType::Game, false);
	UWorld* B = UWorld::CreateWorld(EWorldType::Game, false);
	ON_SCOPE_EXIT { A->DestroyWorld(false); B->DestroyWorld(false); };

	CamSim::Geospatial::FCesiumWorldSetupLatch Latch;
	TestFalse(TEXT("null world"), Latch.TryBegin(nullptr));
	TestTrue(TEXT("first call for A"), Latch.TryBegin(A));
	TestFalse(TEXT("second call for A (game mode, then camera)"), Latch.TryBegin(A));
	TestTrue(TEXT("a new world runs again"), Latch.TryBegin(B));
	return true;
}

// Which worlds the subsystem sets up from OnPostWorldInitialization: game worlds of its own
// game instance (or none yet), never editor or preview worlds.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCesiumBackendShouldSetUpWorldTest,
	"CamSim.CesiumBackend.ShouldSetUpWorld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCesiumBackendShouldSetUpWorldTest::RunTest(const FString& Parameters)
{
	using CamSim::Geospatial::ShouldSetUpCesiumWorld;
	UWorld* Game = UWorld::CreateWorld(EWorldType::Game, false);
	UWorld* Editor = UWorld::CreateWorld(EWorldType::EditorPreview, false);
	ON_SCOPE_EXIT { Game->DestroyWorld(false); Editor->DestroyWorld(false); };

	UGameInstance* Ours = NewObject<UGameInstance>(GetTransientPackage());
	UGameInstance* Other = NewObject<UGameInstance>(GetTransientPackage());
	TestFalse(TEXT("null world"), ShouldSetUpCesiumWorld(nullptr, Ours));
	TestTrue(TEXT("game world, no game instance yet"), ShouldSetUpCesiumWorld(Game, Ours));
	TestFalse(TEXT("editor preview world"), ShouldSetUpCesiumWorld(Editor, Ours));
	Game->SetGameInstance(Ours);
	TestTrue(TEXT("game world of our game instance"), ShouldSetUpCesiumWorld(Game, Ours));
	Game->SetGameInstance(Other);
	TestFalse(TEXT("game world of another game instance"), ShouldSetUpCesiumWorld(Game, Ours));
	Game->SetGameInstance(nullptr);
	return true;
}
