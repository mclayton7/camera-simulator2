// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"
#include "Sensor/SensorPresets.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCelestialConfigDefaultsTest, "CamSim.Environment.Config.Defaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FCelestialConfigDefaultsTest::RunTest(const FString&)
{
	const FCamSimConfig::FCelestialConfig C;
	TestTrue(TEXT("moon"), C.bMoonEnabled);
	TestTrue(TEXT("stars default"), C.bStarsDefault);
	TestEqual(TEXT("floor"), C.NightFloorLux, 0.002f);
	TestEqual(TEXT("day ev"), C.CachedExposure.DayEv, 8.0f);
	TestEqual(TEXT("night ev"), C.CachedExposure.NightEv, -6.0f);
	TestEqual(TEXT("switch"), C.CachedExposure.SwitchMedianLog2, 0.0f);
	TestEqual(TEXT("hyst"), C.CachedExposure.HysteresisEv, 2.0f);
	FSensorModeConfig Eo; CamSimSensorPresets::Apply(TEXT("eo_hd_cmos"), Eo);
	TestEqual(TEXT("eo transmission"), Eo.Optics.Transmission, 0.9f);
	TestEqual(TEXT("eo qe"), Eo.Detector.Qe, 0.6f);
	FSensorModeConfig Ir; CamSimSensorPresets::Apply(TEXT("mwir_cooled"), Ir);
	TestEqual(TEXT("mwir not radiometric"), Ir.Detector.Qe, 0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCelestialConfigYamlTest, "CamSim.Environment.Config.YamlAndEnv",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FCelestialConfigYamlTest::RunTest(const FString&)
{
	const FString Yaml = TEXT(
		"environment:\n"
		"  celestial:\n"
		"    moon_enabled: false\n"
		"    stars_default: false\n"
		"    night_floor_lux: 0.001\n"
		"    star_catalog: /tmp/x.dat\n"
		"    cached_exposure:\n"
		"      day_ev: 7\n"
		"      night_ev: -5\n"
		"      switch_median_log2: 1\n"
		"      hysteresis_ev: 3\n"
		"sensor_modes:\n"
		"  eo:\n"
		"    optics:\n"
		"      transmission: 0.8\n"
		"    detector:\n"
		"      qe: 0.5\n"
		"      integration_max_ms: 20\n"
		"      integration_min_us: 5\n");
	FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(Yaml);
	TestTrue(TEXT("parsed"), Cfg.bLoadedSuccessfully);
	TestTrue(TEXT("no unknown keys"), Cfg.UnknownYamlKeys.IsEmpty());
	TestFalse(TEXT("moon"), Cfg.Celestial.bMoonEnabled);
	TestFalse(TEXT("stars"), Cfg.Celestial.bStarsDefault);
	TestEqual(TEXT("floor"), Cfg.Celestial.NightFloorLux, 0.001f);
	TestEqual(TEXT("catalog"), Cfg.Celestial.StarCatalog, FString(TEXT("/tmp/x.dat")));
	TestEqual(TEXT("day"), Cfg.Celestial.CachedExposure.DayEv, 7.0f);
	TestEqual(TEXT("night"), Cfg.Celestial.CachedExposure.NightEv, -5.0f);
	TestEqual(TEXT("switch"), Cfg.Celestial.CachedExposure.SwitchMedianLog2, 1.0f);
	TestEqual(TEXT("hyst"), Cfg.Celestial.CachedExposure.HysteresisEv, 3.0f);
	const FSensorModeConfig& Eo = Cfg.SensorModeConfigs.FindChecked(ESensorMode::EO);
	TestEqual(TEXT("transmission"), Eo.Optics.Transmission, 0.8f);
	TestEqual(TEXT("qe"), Eo.Detector.Qe, 0.5f);
	TestEqual(TEXT("tmax"), Eo.Detector.IntegrationMaxMs, 20.0f);
	TestEqual(TEXT("tmin"), Eo.Detector.IntegrationMinUs, 5.0f);
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_NIGHT_FLOOR_LUX"), TEXT("0.004"));
	Cfg = FCamSimConfig::LoadFromYamlString(Yaml);
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_NIGHT_FLOOR_LUX"), TEXT(""));
	TestEqual(TEXT("env floor"), Cfg.Celestial.NightFloorLux, 0.004f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCelestialConfigValidateTest, "CamSim.Environment.Config.Validate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FCelestialConfigValidateTest::RunTest(const FString&)
{
	FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT("environment:\n  celestial:\n    night_floor_lux: -1\n    cached_exposure:\n      night_ev: 20\n"));
	TArray<FString> Errors = Cfg.Validate();
	TestTrue(TEXT("negative floor rejected"), Errors.ContainsByPredicate([](const FString& E) { return E.Contains(TEXT("night_floor_lux")); }));
	TestTrue(TEXT("night_ev out of [-16, 16] or >= day_ev rejected"), Errors.ContainsByPredicate([](const FString& E) { return E.Contains(TEXT("cached_exposure")); }));
	return true;
}
