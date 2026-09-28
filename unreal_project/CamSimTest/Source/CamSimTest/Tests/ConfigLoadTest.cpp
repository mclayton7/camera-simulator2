// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"

// -------------------------------------------------------------------------
// Settings reach FCamSimConfig through the real loader: every value below
// differs from its default, so a wrong key name or a dropped Yaml* call
// fails here instead of silently leaving the default in place.
// -------------------------------------------------------------------------

namespace
{
	const TCHAR* YamlBoolText(bool b) { return b ? TEXT("true") : TEXT("false"); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FConfigYamlSectionsTest,
	"CamSim.Config.YamlSectionsApplied",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConfigYamlSectionsTest::RunTest(const FString& Parameters)
{
	const FCamSimConfig D;  // defaults

	const FString Yaml = FString::Printf(TEXT(
		"cigi_port: 9123\n"
		"multicast_addr: \"239.9.9.9\"\n"
		"capture_width: 1920\n"
		"phase18:\n"
		"  second_fog: %s\n"
		"phase19:\n"
		"  ocean_enabled: %s\n"
		"  beaufort_state: 7\n"
		"cesium:\n"
		"  ion_api_url: \"https://example.test/api\"\n"
		"dis:\n"
		"  enabled: %s\n"
		"  bind_addr: \"10.1.2.3\"\n"
		"ml_training:\n"
		"  enabled: %s\n"
		"  output_dir: \"/tmp/camsim-ml\"\n"
		"optical_realism:\n"
		"  enabled: %s\n"
		"streaming:\n"
		"  cot_enabled: %s\n"
		"  cot_addr: \"10.4.5.6\"\n"
		"performance:\n"
		"  hot_reload_config: %s\n"
		"damage_transition:\n"
		"  gradual: %s\n"
		"terrain_gate:\n"
		"  min_load_progress: 87.5\n"
		"recording:\n"
		"  cigi_record_path: \"/tmp/cigi.rec\"\n"
		"scenario:\n"
		"  time_scale: 3.5\n"
		"ground_truth:\n"
		"  output_path: \"/tmp/gt\"\n"
		"operational:\n"
		"  health_http_enabled: %s\n"
		"  health_http_port: 18080\n"),
		YamlBoolText(!D.Phase18.bSecondFog),
		YamlBoolText(!D.Phase19.bOceanEnabled),
		YamlBoolText(!D.DIS.bEnabled),
		YamlBoolText(!D.MLTraining.bEnabled),
		YamlBoolText(!D.OpticalRealism.bEnabled),
		YamlBoolText(!D.Streaming.bCotEnabled),
		YamlBoolText(!D.Performance.bHotReloadConfig),
		YamlBoolText(!D.DamageTransition.bGradualDamage),
		YamlBoolText(!D.Operational.bHealthHttpEnabled));

	const FCamSimConfig C = FCamSimConfig::LoadFromYamlString(Yaml);

	TestTrue(TEXT("loaded"), C.bLoadedSuccessfully);
	TestEqual(TEXT("no unknown keys in the fixture"), C.UnknownYamlKeys.Num(), 0);

	TestEqual(TEXT("cigi_port"), C.CigiPort, 9123);
	TestEqual(TEXT("multicast_addr"), C.MulticastAddr, FString(TEXT("239.9.9.9")));
	TestEqual(TEXT("capture_width"), C.CaptureWidth, 1920);
	TestEqual(TEXT("phase18.second_fog"), C.Phase18.bSecondFog, !D.Phase18.bSecondFog);
	TestEqual(TEXT("phase19.ocean_enabled"), C.Phase19.bOceanEnabled, !D.Phase19.bOceanEnabled);
	TestEqual(TEXT("phase19.beaufort_state"), C.Phase19.BeaufortState, 7);
	TestEqual(TEXT("cesium.ion_api_url"), C.CesiumBackend.IonApiUrl, FString(TEXT("https://example.test/api")));
	TestEqual(TEXT("dis.enabled"), C.DIS.bEnabled, !D.DIS.bEnabled);
	TestEqual(TEXT("dis.bind_addr"), C.DIS.BindAddr, FString(TEXT("10.1.2.3")));
	TestEqual(TEXT("ml_training.enabled"), C.MLTraining.bEnabled, !D.MLTraining.bEnabled);
	TestEqual(TEXT("ml_training.output_dir"), C.MLTraining.OutputDir, FString(TEXT("/tmp/camsim-ml")));
	TestEqual(TEXT("optical_realism.enabled"), C.OpticalRealism.bEnabled, !D.OpticalRealism.bEnabled);
	TestEqual(TEXT("streaming.cot_enabled"), C.Streaming.bCotEnabled, !D.Streaming.bCotEnabled);
	TestEqual(TEXT("streaming.cot_addr"), C.Streaming.CotAddr, FString(TEXT("10.4.5.6")));
	TestEqual(TEXT("performance.hot_reload_config"), C.Performance.bHotReloadConfig, !D.Performance.bHotReloadConfig);
	TestEqual(TEXT("damage_transition.gradual"), C.DamageTransition.bGradualDamage, !D.DamageTransition.bGradualDamage);
	TestEqual(TEXT("terrain_gate.min_load_progress"), C.TerrainGate.MinLoadProgressPct, 87.5f);
	TestEqual(TEXT("recording.cigi_record_path"), C.Recording.CigiRecordPath, FString(TEXT("/tmp/cigi.rec")));
	TestEqual(TEXT("scenario.time_scale"), C.ScenarioTimeScale, 3.5f);
	TestEqual(TEXT("ground_truth.output_path"), C.GroundTruth.OutputPath, FString(TEXT("/tmp/gt")));
	TestEqual(TEXT("operational.health_http_enabled"), C.Operational.bHealthHttpEnabled, !D.Operational.bHealthHttpEnabled);
	TestEqual(TEXT("operational.health_http_port"), C.Operational.HealthHttpPort, 18080);
	return true;
}

// -------------------------------------------------------------------------
// Environment variables override YAML, and boolean env vars accept the
// spellings people actually write.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FConfigEnvOverridesYamlTest,
	"CamSim.Config.EnvOverridesYaml",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConfigEnvOverridesYamlTest::RunTest(const FString& Parameters)
{
	const TCHAR* PortVar = TEXT("CAMSIM_CIGI_PORT");
	const TCHAR* HttpVar = TEXT("CAMSIM_HEALTH_HTTP_ENABLED");
	ON_SCOPE_EXIT
	{
		FPlatformMisc::SetEnvironmentVar(PortVar, TEXT(""));
		FPlatformMisc::SetEnvironmentVar(HttpVar, TEXT(""));
	};

	const FString Yaml = TEXT("cigi_port: 9123\noperational:\n  health_http_enabled: false\n");

	FPlatformMisc::SetEnvironmentVar(PortVar, TEXT("7777"));
	TestEqual(TEXT("env beats YAML"), FCamSimConfig::LoadFromYamlString(Yaml).CigiPort, 7777);
	FPlatformMisc::SetEnvironmentVar(PortVar, TEXT(""));
	TestEqual(TEXT("empty env leaves YAML"), FCamSimConfig::LoadFromYamlString(Yaml).CigiPort, 9123);

	for (const TCHAR* True : { TEXT("1"), TEXT("true"), TEXT("TRUE"), TEXT("yes"), TEXT("on") })
	{
		FPlatformMisc::SetEnvironmentVar(HttpVar, True);
		TestTrue(FString::Printf(TEXT("%s=%s enables"), HttpVar, True),
			FCamSimConfig::LoadFromYamlString(Yaml).Operational.bHealthHttpEnabled);
	}
	const FString YamlOn = TEXT("operational:\n  health_http_enabled: true\n");
	for (const TCHAR* False : { TEXT("0"), TEXT("false"), TEXT("no"), TEXT("off") })
	{
		FPlatformMisc::SetEnvironmentVar(HttpVar, False);
		TestFalse(FString::Printf(TEXT("%s=%s disables"), HttpVar, False),
			FCamSimConfig::LoadFromYamlString(YamlOn).Operational.bHealthHttpEnabled);
	}
	return true;
}
