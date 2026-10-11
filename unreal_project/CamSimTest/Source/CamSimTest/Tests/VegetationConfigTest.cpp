// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"

#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegConfigDefaultsTest, "CamSim.Vegetation.Config.Defaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegConfigDefaultsTest::RunTest(const FString& Parameters)
{
	const FCamSimConfig::FVegetationConfig V;
	TestTrue (TEXT("enabled"), V.bEnabled);
	TestEqual(TEXT("library_dir"), V.LibraryDir, FString());
	TestEqual(TEXT("radius_m"), V.RadiusM, 1000.0f);
	TestEqual(TEXT("density_scale"), V.DensityScale, 1.0f);
	TestTrue (TEXT("cast_shadows"), V.bCastShadows);
	TestEqual(TEXT("cell_size_m"), V.CellSizeM, 100.0f);
	TestEqual(TEXT("seed"), V.Seed, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegConfigYamlEnvTest, "CamSim.Vegetation.Config.YamlAndEnv",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegConfigYamlEnvTest::RunTest(const FString& Parameters)
{
	const FString Yaml = TEXT(
		"vegetation:\n"
		"  enabled: false\n"
		"  library_dir: \"/mnt/veg\"\n"
		"  radius_m: 1500\n"
		"  density_scale: 0.5\n"
		"  cast_shadows: false\n"
		"  cell_size_m: 50\n"
		"  seed: 42\n");
	FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(Yaml);
	TestTrue (TEXT("parsed"), Cfg.bLoadedSuccessfully);
	TestFalse(TEXT("enabled"), Cfg.Vegetation.bEnabled);
	TestEqual(TEXT("library_dir"), Cfg.Vegetation.LibraryDir, FString(TEXT("/mnt/veg")));
	TestEqual(TEXT("radius"), Cfg.Vegetation.RadiusM, 1500.0f);
	TestEqual(TEXT("density"), Cfg.Vegetation.DensityScale, 0.5f);
	TestFalse(TEXT("shadows"), Cfg.Vegetation.bCastShadows);
	TestEqual(TEXT("cell"), Cfg.Vegetation.CellSizeM, 50.0f);
	TestEqual(TEXT("seed"), Cfg.Vegetation.Seed, 42);

	const TCHAR* Vars[][2] = {
		{ TEXT("CAMSIM_VEGETATION_ENABLED"), TEXT("true") }, { TEXT("CAMSIM_VEGETATION_LIBRARY_DIR"), TEXT("/opt/veg") },
		{ TEXT("CAMSIM_VEGETATION_RADIUS_M"), TEXT("500") }, { TEXT("CAMSIM_VEGETATION_DENSITY_SCALE"), TEXT("0.25") },
		{ TEXT("CAMSIM_VEGETATION_CAST_SHADOWS"), TEXT("1") }, { TEXT("CAMSIM_VEGETATION_CELL_SIZE_M"), TEXT("200") },
		{ TEXT("CAMSIM_VEGETATION_SEED"), TEXT("7") } };
	for (const auto& V : Vars) FPlatformMisc::SetEnvironmentVar(V[0], V[1]);
	Cfg = FCamSimConfig::LoadFromYamlString(Yaml);
	for (const auto& V : Vars) FPlatformMisc::SetEnvironmentVar(V[0], TEXT(""));
	TestTrue (TEXT("env enabled"), Cfg.Vegetation.bEnabled);
	TestEqual(TEXT("env library_dir"), Cfg.Vegetation.LibraryDir, FString(TEXT("/opt/veg")));
	TestEqual(TEXT("env radius"), Cfg.Vegetation.RadiusM, 500.0f);
	TestEqual(TEXT("env density"), Cfg.Vegetation.DensityScale, 0.25f);
	TestTrue (TEXT("env shadows"), Cfg.Vegetation.bCastShadows);
	TestEqual(TEXT("env cell"), Cfg.Vegetation.CellSizeM, 200.0f);
	TestEqual(TEXT("env seed"), Cfg.Vegetation.Seed, 7);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegConfigValidateTest, "CamSim.Vegetation.Config.Validate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegConfigValidateTest::RunTest(const FString& Parameters)
{
	auto Has = [](const TArray<FString>& E, const TCHAR* S) { return E.ContainsByPredicate([S](const FString& X) { return X.Contains(S); }); };
	auto With = [](TFunction<void(FCamSimConfig::FVegetationConfig&)> Set) { FCamSimConfig C; Set(C.Vegetation); return C.Validate(); };
	const float NaN = std::numeric_limits<float>::quiet_NaN();
	TestFalse(TEXT("defaults valid"), Has(FCamSimConfig().Validate(), TEXT("vegetation.")));
	TestTrue (TEXT("radius 0"),     Has(With([](auto& V) { V.RadiusM = 0.0f; }), TEXT("vegetation.radius_m")));
	TestTrue (TEXT("radius 6000"),  Has(With([](auto& V) { V.RadiusM = 6000.0f; }), TEXT("vegetation.radius_m")));
	TestTrue (TEXT("radius NaN"),   Has(With([NaN](auto& V) { V.RadiusM = NaN; }), TEXT("vegetation.radius_m")));
	TestFalse(TEXT("radius 5000"),  Has(With([](auto& V) { V.RadiusM = 5000.0f; }), TEXT("vegetation.radius_m")));
	TestTrue (TEXT("density -0.1"), Has(With([](auto& V) { V.DensityScale = -0.1f; }), TEXT("vegetation.density_scale")));
	TestTrue (TEXT("density 4.5"),  Has(With([](auto& V) { V.DensityScale = 4.5f; }), TEXT("vegetation.density_scale")));
	TestFalse(TEXT("density 0"),    Has(With([](auto& V) { V.DensityScale = 0.0f; }), TEXT("vegetation.density_scale")));
	TestTrue (TEXT("cell 10"),      Has(With([](auto& V) { V.CellSizeM = 10.0f; }), TEXT("vegetation.cell_size_m")));
	TestTrue (TEXT("cell 600"),     Has(With([](auto& V) { V.CellSizeM = 600.0f; }), TEXT("vegetation.cell_size_m")));
	TestTrue (TEXT("seed -1"),      Has(With([](auto& V) { V.Seed = -1; }), TEXT("vegetation.seed")));
	return true;
}
