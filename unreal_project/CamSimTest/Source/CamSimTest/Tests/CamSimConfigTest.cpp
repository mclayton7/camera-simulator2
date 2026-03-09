// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"

// -------------------------------------------------------------------------
// CamSimConfig Automation Tests
//
// Validates default values, config loading success flag, and env var
// override application.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCamSimConfigDefaultsTest,
	"CamSim.Config.Defaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCamSimConfigDefaultsTest::RunTest(const FString& Parameters)
{
	// Verify default member values are sane
	FCamSimConfig Cfg;

	TestEqual(TEXT("Default CIGI port"), Cfg.CigiPort, 8888);
	TestEqual(TEXT("Default multicast port"), Cfg.MulticastPort, 5004);
	TestEqual(TEXT("Default capture width"), Cfg.CaptureWidth, 1920);
	TestEqual(TEXT("Default capture height"), Cfg.CaptureHeight, 1080);
	TestTrue(TEXT("Default frame rate > 0"), Cfg.FrameRate > 0.0f);
	TestEqual(TEXT("Default bitrate"), Cfg.VideoBitrate, 4'000'000);
	TestEqual(TEXT("Default max entities"), Cfg.MaxEntities, 500);
	TestTrue(TEXT("Default HFovDeg > 0"), Cfg.HFovDeg > 0.0f);
	TestEqual(TEXT("Default watchdog policy"), static_cast<uint8>(Cfg.EncoderWatchdogPolicy),
		static_cast<uint8>(FCamSimConfig::EEncoderWatchdogPolicy::Reconnect));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCamSimConfigSensorModeDefaultsTest,
	"CamSim.Config.SensorModeDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCamSimConfigSensorModeDefaultsTest::RunTest(const FString& Parameters)
{
	// Load config (will use defaults if no file found)
	FCamSimConfig Cfg = FCamSimConfig::Load();

	// Should have all three sensor mode configs
	TestTrue(TEXT("Has EO config"), Cfg.SensorModeConfigs.Contains(ESensorMode::EO));
	TestTrue(TEXT("Has IR config"), Cfg.SensorModeConfigs.Contains(ESensorMode::IR));
	TestTrue(TEXT("Has NVG config"), Cfg.SensorModeConfigs.Contains(ESensorMode::NVG));

	// EO should have zero NETD
	if (const FSensorModeConfig* Eo = Cfg.SensorModeConfigs.Find(ESensorMode::EO))
	{
		TestTrue(TEXT("EO NETD == 0"), FMath::IsNearlyZero(Eo->NETD));
	}

	// IR should have nonzero NETD
	if (const FSensorModeConfig* Ir = Cfg.SensorModeConfigs.Find(ESensorMode::IR))
	{
		TestTrue(TEXT("IR NETD > 0"), Ir->NETD > 0.0f);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCamSimConfigLoadSuccessTest,
	"CamSim.Config.LoadSuccess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCamSimConfigLoadSuccessTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg = FCamSimConfig::Load();

	// bLoadedSuccessfully should be true even with defaults (defaults are valid)
	TestTrue(TEXT("Config loaded successfully"), Cfg.bLoadedSuccessfully);

	// Quality profiles should be populated
	TestTrue(TEXT("Has medium quality profile"),
		Cfg.SensorQualityProfiles.Contains(TEXT("medium")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCamSimConfigFovPresetsTest,
	"CamSim.Config.FovPresets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCamSimConfigFovPresetsTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg = FCamSimConfig::Load();

	// Default FOV presets should be populated
	TestTrue(TEXT("FOV presets not empty"), Cfg.SensorFovPresets.Num() > 0);

	// Presets should be in descending order (wide -> narrow)
	bool bDescending = true;
	for (int32 i = 1; i < Cfg.SensorFovPresets.Num(); ++i)
	{
		if (Cfg.SensorFovPresets[i] >= Cfg.SensorFovPresets[i - 1])
		{
			bDescending = false;
			break;
		}
	}
	TestTrue(TEXT("FOV presets are descending (wide->narrow)"), bDescending);

	return true;
}
