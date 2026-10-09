// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"

// REALISM R0 offline profile: scene.* and cesium.imagery.url parse from YAML and env.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneConfigParseTest,
	"CamSim.Scene.Config.Parse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneConfigParseTest::RunTest(const FString& Parameters)
{
	{
		const FCamSimConfig Cfg;
		TestTrue(TEXT("scene.dir empty by default"), Cfg.Scene.Dir.IsEmpty());
		TestFalse(TEXT("scene.offline off by default"), Cfg.Scene.bOffline);
		TestTrue(TEXT("imagery.url empty by default"), Cfg.CesiumBackend.Imagery.Url.IsEmpty());
	}
	{
		const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT(
			"scene:\n"
			"  offline: true\n"
			"cesium:\n"
			"  imagery:\n"
			"    source: tms\n"
			"    url: \"file:///data/pkg/imagery/tilemapresource.xml\"\n"));
		TestTrue(TEXT("yaml scene.offline"), Cfg.Scene.bOffline);
		TestEqual(TEXT("yaml imagery.source"), Cfg.CesiumBackend.Imagery.Source, FString(TEXT("tms")));
		TestEqual(TEXT("yaml imagery.url"), Cfg.CesiumBackend.Imagery.Url,
			FString(TEXT("file:///data/pkg/imagery/tilemapresource.xml")));
		TestEqual(TEXT("no unknown keys"), Cfg.UnknownYamlKeys.Num(), 0);
	}
	{
		ON_SCOPE_EXIT
		{
			FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_SCENE_OFFLINE"), TEXT(""));
			FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_CESIUM_IMAGERY_URL"), TEXT(""));
		};
		FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_SCENE_OFFLINE"), TEXT("true"));
		FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_CESIUM_IMAGERY_URL"), TEXT("file:///x/tilemapresource.xml"));
		const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT("{}"));
		TestTrue(TEXT("env scene.offline"), Cfg.Scene.bOffline);
		TestEqual(TEXT("env imagery.url"), Cfg.CesiumBackend.Imagery.Url, FString(TEXT("file:///x/tilemapresource.xml")));
	}
	return true;
}
