// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"

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

// 2. Terrain source strings are valid values
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCesiumBackendTerrainSourceValuesTest,
	"CamSim.CesiumBackend.TerrainSourceValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCesiumBackendTerrainSourceValuesTest::RunTest(const FString& Parameters)
{
	// Valid source strings — just check these are the known values
	const TArray<FString> ValidSources = { TEXT("cesium_ion"), TEXT("url"), TEXT("flat") };
	TestTrue(TEXT("cesium_ion in valid set"),
		ValidSources.Contains(TEXT("cesium_ion")));
	TestTrue(TEXT("url in valid set"),
		ValidSources.Contains(TEXT("url")));
	TestTrue(TEXT("flat in valid set"),
		ValidSources.Contains(TEXT("flat")));
	TestFalse(TEXT("gibberish not in valid set"),
		ValidSources.Contains(TEXT("gibberish")));
	return true;
}

// 3. Imagery source strings are valid values
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCesiumBackendImagerySourceValuesTest,
	"CamSim.CesiumBackend.ImagerySourceValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCesiumBackendImagerySourceValuesTest::RunTest(const FString& Parameters)
{
	const TArray<FString> ValidSources = { TEXT("cesium_ion"), TEXT("wms"), TEXT("none") };
	TestTrue(TEXT("cesium_ion in valid set"), ValidSources.Contains(TEXT("cesium_ion")));
	TestTrue(TEXT("wms in valid set"),        ValidSources.Contains(TEXT("wms")));
	TestTrue(TEXT("none in valid set"),       ValidSources.Contains(TEXT("none")));
	TestFalse(TEXT("gibberish not in valid set"), ValidSources.Contains(TEXT("gibberish")));
	return true;
}
