// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"

#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanConfigDefaultsTest, "CamSim.Ocean.Config.Defaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanConfigDefaultsTest::RunTest(const FString& Parameters)
{
	const FCamSimConfig::FOceanConfig O;
	TestTrue (TEXT("enabled"), O.bEnabled);
	TestEqual(TEXT("beaufort"), O.Beaufort, 3.0f);
	TestEqual(TEXT("direction"), O.WaveDirectionDeg, 270.0f);
	TestEqual(TEXT("choppiness"), O.Choppiness, 0.5f);
	TestTrue (TEXT("vessel motion"), O.bVesselMotion);
	TestEqual(TEXT("motion scale"), O.VesselMotionScale, 1.0f);
	TestEqual(TEXT("max radius"), O.MaxRadiusKm, 400.0f);
	TestEqual(TEXT("material"), O.MaterialPath, FString(TEXT("/Game/Ocean/M_Ocean")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanConfigYamlTest, "CamSim.Ocean.Config.YamlAndEnv",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanConfigYamlTest::RunTest(const FString& Parameters)
{
	const FString Yaml = TEXT(
		"ocean:\n"
		"  enabled: false\n"
		"  beaufort: 6.5\n"
		"  wave_direction_deg: 45\n"
		"  choppiness: 0.9\n"
		"  vessel_motion: false\n"
		"  vessel_motion_scale: 0.5\n"
		"  max_radius_km: 100\n"
		"  material: \"/Game/X/M_Y\"\n");

	FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(Yaml);
	TestTrue(TEXT("parsed"), Cfg.bLoadedSuccessfully);
	TestFalse(TEXT("enabled"), Cfg.Ocean.bEnabled);
	TestEqual(TEXT("beaufort"), Cfg.Ocean.Beaufort, 6.5f);
	TestEqual(TEXT("direction"), Cfg.Ocean.WaveDirectionDeg, 45.0f);
	TestEqual(TEXT("choppiness"), Cfg.Ocean.Choppiness, 0.9f);
	TestFalse(TEXT("motion"), Cfg.Ocean.bVesselMotion);
	TestEqual(TEXT("scale"), Cfg.Ocean.VesselMotionScale, 0.5f);
	TestEqual(TEXT("radius"), Cfg.Ocean.MaxRadiusKm, 100.0f);
	TestEqual(TEXT("material"), Cfg.Ocean.MaterialPath, FString(TEXT("/Game/X/M_Y")));

	// FCamSimConfig::ApplyEnvOverrides is private; LoadFromYamlString applies
	// env overrides internally (see FConfigEnvOverridesYamlTest for the pattern).
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_OCEAN_BEAUFORT"), TEXT("2.5"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_OCEAN_WAVE_DIR"), TEXT("90"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_OCEAN_ENABLED"), TEXT("true"));
	Cfg = FCamSimConfig::LoadFromYamlString(Yaml);
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_OCEAN_BEAUFORT"), TEXT(""));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_OCEAN_WAVE_DIR"), TEXT(""));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_OCEAN_ENABLED"), TEXT(""));
	TestEqual(TEXT("env beaufort (fractional)"), Cfg.Ocean.Beaufort, 2.5f);
	TestEqual(TEXT("env direction"), Cfg.Ocean.WaveDirectionDeg, 90.0f);
	TestTrue (TEXT("env enabled accepts true"), Cfg.Ocean.bEnabled);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanConfigValidateTest, "CamSim.Ocean.Config.Validate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanConfigValidateTest::RunTest(const FString& Parameters)
{
	auto Has = [](const TArray<FString>& E, const TCHAR* S) { return E.ContainsByPredicate([S](const FString& X) { return X.Contains(S); }); };
	auto With = [](TFunction<void(FCamSimConfig::FOceanConfig&)> Set) { FCamSimConfig C; Set(C.Ocean); return C.Validate(); };
	const float NaN = std::numeric_limits<float>::quiet_NaN();
	const float Inf = std::numeric_limits<float>::infinity();

	TestFalse(TEXT("defaults valid"), Has(FCamSimConfig().Validate(), TEXT("ocean.")));

	TestTrue (TEXT("max_radius_km 0"),    Has(With([](auto& O) { O.MaxRadiusKm = 0.0f; }), TEXT("ocean.max_radius_km")));
	TestTrue (TEXT("max_radius_km < 0"),  Has(With([](auto& O) { O.MaxRadiusKm = -5.0f; }), TEXT("ocean.max_radius_km")));
	TestTrue (TEXT("max_radius_km NaN"),  Has(With([NaN](auto& O) { O.MaxRadiusKm = NaN; }), TEXT("ocean.max_radius_km")));
	TestTrue (TEXT("max_radius_km inf"),  Has(With([Inf](auto& O) { O.MaxRadiusKm = Inf; }), TEXT("ocean.max_radius_km")));
	TestFalse(TEXT("max_radius_km 0.5"),  Has(With([](auto& O) { O.MaxRadiusKm = 0.5f; }), TEXT("ocean.max_radius_km")));

	TestTrue (TEXT("beaufort -1"),  Has(With([](auto& O) { O.Beaufort = -1.0f; }), TEXT("ocean.beaufort")));
	TestTrue (TEXT("beaufort 13"),  Has(With([](auto& O) { O.Beaufort = 13.0f; }), TEXT("ocean.beaufort")));
	TestTrue (TEXT("beaufort NaN"), Has(With([NaN](auto& O) { O.Beaufort = NaN; }), TEXT("ocean.beaufort")));
	TestFalse(TEXT("beaufort 0"),   Has(With([](auto& O) { O.Beaufort = 0.0f; }), TEXT("ocean.beaufort")));
	TestFalse(TEXT("beaufort 12"),  Has(With([](auto& O) { O.Beaufort = 12.0f; }), TEXT("ocean.beaufort")));

	TestTrue (TEXT("choppiness -0.1"), Has(With([](auto& O) { O.Choppiness = -0.1f; }), TEXT("ocean.choppiness")));
	TestTrue (TEXT("choppiness 1.5"),  Has(With([](auto& O) { O.Choppiness = 1.5f; }), TEXT("ocean.choppiness")));
	TestTrue (TEXT("choppiness NaN"),  Has(With([NaN](auto& O) { O.Choppiness = NaN; }), TEXT("ocean.choppiness")));
	TestFalse(TEXT("choppiness 1"),    Has(With([](auto& O) { O.Choppiness = 1.0f; }), TEXT("ocean.choppiness")));

	// Hot reload rejects it too (ValidateHotReload = Validate of the config as it would run).
	FCamSimConfig Reloaded = FCamSimConfig::LoadFromYamlString(TEXT("ocean:\n  max_radius_km: 0\n"));
	TestTrue(TEXT("hot reload rejects max_radius_km 0"), Has(FCamSimConfig::ValidateHotReload(FCamSimConfig(), Reloaded), TEXT("ocean.max_radius_km")));
	return true;
}
