// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"
#include "Thermal/LandCoverClasses.h"
#include "Thermal/ThermalMaterials.h"

#include <limits>

// CamSim.Thermal.Config.LandCover*: the thermal.land_cover section and the code -> class table (ROADMAP 4B).

namespace
{
	bool HasError(const TArray<FString>& Errors, const TCHAR* Needle)
	{
		return Errors.ContainsByPredicate([Needle](const FString& E) { return E.Contains(Needle); });
	}

	FLandCoverClassSpec Spec(int32 Code, const TCHAR* Material)
	{
		FLandCoverClassSpec S;
		S.Key = FString::FromInt(Code);
		S.Code = Code;
		S.Material = Material;
		return S;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalLandCoverConfigDefaultsTest, "CamSim.Thermal.Config.LandCoverDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalLandCoverConfigDefaultsTest::RunTest(const FString& Parameters)
{
	const FCamSimConfig::FThermalConfig::FLandCoverConfig L;
	TestTrue (TEXT("enabled"), L.bEnabled);
	TestEqual(TEXT("dir"), L.Dir, FString(TEXT("Content/NonUFS/LandCover")));
	TestEqual(TEXT("window"), L.WindowTexels, 2048);
	TestEqual(TEXT("recentre"), L.RecentreFraction, 0.25f);
	TestEqual(TEXT("veg lo"), L.VegIndexLo, 0.05f);
	TestEqual(TEXT("veg hi"), L.VegIndexHi, 0.20f);
	TestEqual(TEXT("asphalt luma"), L.AsphaltMaxLuma, 0.12f);
	TestEqual(TEXT("no class overrides"), L.Classes.Num(), 0);
	TestFalse(TEXT("defaults valid"), HasError(FCamSimConfig().Validate(), TEXT("land_cover")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalLandCoverConfigYamlTest, "CamSim.Thermal.Config.LandCoverYamlAndEnv",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalLandCoverConfigYamlTest::RunTest(const FString& Parameters)
{
	const FString Yaml = TEXT(
		"thermal:\n"
		"  land_cover:\n"
		"    enabled: false\n"
		"    dir: /data/landcover\n"
		"    window_texels: 1024\n"
		"    recentre_fraction: 0.1\n"
		"    veg_index_lo: 0.02\n"
		"    veg_index_hi: 0.3\n"
		"    asphalt_max_luma: 0.2\n"
		"    classes:\n"
		"      10: vegetation\n"
		"      50: concrete\n");
	FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(Yaml);
	const auto& L = Cfg.Thermal.LandCover;
	TestTrue (TEXT("parsed"), Cfg.bLoadedSuccessfully);
	TestEqual(TEXT("no unknown keys"), Cfg.UnknownYamlKeys.Num(), 0);
	TestFalse(TEXT("enabled"), L.bEnabled);
	TestEqual(TEXT("dir"), L.Dir, FString(TEXT("/data/landcover")));
	TestEqual(TEXT("window"), L.WindowTexels, 1024);
	TestEqual(TEXT("recentre"), L.RecentreFraction, 0.1f);
	TestEqual(TEXT("veg lo"), L.VegIndexLo, 0.02f);
	TestEqual(TEXT("veg hi"), L.VegIndexHi, 0.3f);
	TestEqual(TEXT("luma"), L.AsphaltMaxLuma, 0.2f);
	if (TestEqual(TEXT("two class specs"), L.Classes.Num(), 2))
	{
		TestEqual(TEXT("code"), L.Classes[0].Code, 10);
		TestEqual(TEXT("material"), L.Classes[0].Material, FString(TEXT("vegetation")));
		TestEqual(TEXT("second code"), L.Classes[1].Code, 50);
	}
	TestEqual(TEXT("a non-numeric key keeps Code -1"),
		FCamSimConfig::LoadFromYamlString(TEXT("thermal:\n  land_cover:\n    classes:\n      trees: vegetation\n")).Thermal.LandCover.Classes[0].Code, -1);
	TestTrue(TEXT("a typo is an unknown key"),
		FCamSimConfig::LoadFromYamlString(TEXT("thermal:\n  land_cover:\n    window_texel: 512\n")).UnknownYamlKeys
			.Contains(TEXT("thermal.land_cover.window_texel")));

	const TCHAR* Keys[] = { TEXT("CAMSIM_THERMAL_LAND_COVER_ENABLED"), TEXT("CAMSIM_THERMAL_LAND_COVER_DIR"),
		TEXT("CAMSIM_THERMAL_LAND_COVER_WINDOW_TEXELS"), TEXT("CAMSIM_THERMAL_LAND_COVER_RECENTRE_FRACTION"),
		TEXT("CAMSIM_THERMAL_LAND_COVER_VEG_INDEX_LO"), TEXT("CAMSIM_THERMAL_LAND_COVER_VEG_INDEX_HI"),
		TEXT("CAMSIM_THERMAL_LAND_COVER_ASPHALT_MAX_LUMA") };
	const TCHAR* Values[] = { TEXT("1"), TEXT("Content/Other"), TEXT("512"), TEXT("0.02"), TEXT("0.01"), TEXT("0.4"), TEXT("0.15") };
	for (int32 K = 0; K < UE_ARRAY_COUNT(Keys); ++K) FPlatformMisc::SetEnvironmentVar(Keys[K], Values[K]);
	Cfg = FCamSimConfig::LoadFromYamlString(Yaml);
	for (const TCHAR* K : Keys) FPlatformMisc::SetEnvironmentVar(K, TEXT(""));
	TestTrue (TEXT("env enabled"), Cfg.Thermal.LandCover.bEnabled);
	TestEqual(TEXT("env dir"), Cfg.Thermal.LandCover.Dir, FString(TEXT("Content/Other")));
	TestEqual(TEXT("env window"), Cfg.Thermal.LandCover.WindowTexels, 512);
	TestEqual(TEXT("env recentre"), Cfg.Thermal.LandCover.RecentreFraction, 0.02f);
	TestEqual(TEXT("env veg lo"), Cfg.Thermal.LandCover.VegIndexLo, 0.01f);
	TestEqual(TEXT("env veg hi"), Cfg.Thermal.LandCover.VegIndexHi, 0.4f);
	TestEqual(TEXT("env luma"), Cfg.Thermal.LandCover.AsphaltMaxLuma, 0.15f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalLandCoverConfigValidateTest, "CamSim.Thermal.Config.LandCoverValidate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalLandCoverConfigValidateTest::RunTest(const FString& Parameters)
{
	using FLc = FCamSimConfig::FThermalConfig::FLandCoverConfig;
	const float NaN = std::numeric_limits<float>::quiet_NaN();
	auto With = [](TFunction<void(FLc&)> Set) { FCamSimConfig C; Set(C.Thermal.LandCover); return C.Validate(); };
	TestTrue (TEXT("window 128"),     HasError(With([](FLc& L) { L.WindowTexels = 128; }), TEXT("thermal.land_cover.window_texels")));
	TestTrue (TEXT("window odd"),     HasError(With([](FLc& L) { L.WindowTexels = 1001; }), TEXT("thermal.land_cover.window_texels")));
	TestTrue (TEXT("window 16386"),    HasError(With([](FLc& L) { L.WindowTexels = 16386; }), TEXT("thermal.land_cover.window_texels")));
	TestTrue (TEXT("window huge"),     HasError(With([](FLc& L) { L.WindowTexels = 2000000000; }), TEXT("thermal.land_cover.window_texels")));
	TestTrue (TEXT("window negative"), HasError(With([](FLc& L) { L.WindowTexels = -2048; }), TEXT("thermal.land_cover.window_texels")));
	TestFalse(TEXT("window 256 ok"),   HasError(With([](FLc& L) { L.WindowTexels = 256; }), TEXT("land_cover")));
	TestFalse(TEXT("window 16384 ok"), HasError(With([](FLc& L) { L.WindowTexels = 16384; }), TEXT("land_cover")));
	TestTrue (TEXT("recentre 0.5"),   HasError(With([](FLc& L) { L.RecentreFraction = 0.5f; }), TEXT("thermal.land_cover.recentre_fraction")));
	TestTrue (TEXT("recentre NaN"),   HasError(With([NaN](FLc& L) { L.RecentreFraction = NaN; }), TEXT("thermal.land_cover.recentre_fraction")));
	TestTrue (TEXT("lo >= hi"),       HasError(With([](FLc& L) { L.VegIndexLo = 0.3f; L.VegIndexHi = 0.3f; }), TEXT("thermal.land_cover.veg_index")));
	TestTrue (TEXT("hi NaN"),         HasError(With([NaN](FLc& L) { L.VegIndexHi = NaN; }), TEXT("thermal.land_cover.veg_index")));
	TestTrue (TEXT("luma 1.5"),       HasError(With([](FLc& L) { L.AsphaltMaxLuma = 1.5f; }), TEXT("thermal.land_cover.asphalt_max_luma")));
	TestTrue (TEXT("empty dir"),      HasError(With([](FLc& L) { L.Dir = TEXT("  "); }), TEXT("thermal.land_cover.dir")));
	TestFalse(TEXT("empty dir is fine when disabled"), HasError(With([](FLc& L) { L.bEnabled = false; L.Dir = TEXT(""); }), TEXT("land_cover")));
	TestTrue (TEXT("key 'trees'"),    HasError(With([](FLc& L) { FLandCoverClassSpec S; S.Key = TEXT("trees"); S.Material = TEXT("vegetation"); L.Classes.Add(S); }),
		TEXT("key 'trees' is not a WorldCover code")));
	TestTrue (TEXT("code 300"),       HasError(With([](FLc& L) { L.Classes.Add(Spec(300, TEXT("vegetation"))); }), TEXT("not a WorldCover code")));
	TestTrue (TEXT("bad name"),       HasError(With([](FLc& L) { L.Classes.Add(Spec(10, TEXT("Tree Canopy"))); }), TEXT("thermal.land_cover.classes.10")));
	TestTrue (TEXT("listed twice"),   HasError(With([](FLc& L) { L.Classes.Add(Spec(10, TEXT("vegetation"))); L.Classes.Add(Spec(10, TEXT("grassland"))); }), TEXT("listed twice")));
	TestFalse(TEXT("valid override"), HasError(With([](FLc& L) { L.Classes.Add(Spec(10, TEXT("vegetation"))); }), TEXT("land_cover")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalLandCoverClassTableTest, "CamSim.Thermal.Config.LandCoverClassTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalLandCoverClassTableTest::RunTest(const FString& Parameters)
{
	using T = FThermalMaterialTable;
	FThermalMaterialTable Materials;
	FLandCoverClassTable Table;
	TestEqual(TEXT("defaults: no warnings"), Table.Build({}, Materials).Num(), 0);
	const int32 Expected[][2] = { { 0, T::TerrainDefault }, { 10, T::TreeCanopy }, { 20, T::Shrubland }, { 30, T::Grassland },
		{ 40, T::Cropland }, { 50, T::BuiltUp }, { 60, T::BareSoil }, { 70, T::SnowIce }, { 80, T::Water }, { 90, T::Wetland },
		{ 95, T::Wetland }, { 100, T::Grassland }, { 200, T::TerrainDefault } };
	for (const auto& E : Expected) TestEqual(*FString::Printf(TEXT("code %d"), E[0]), static_cast<int32>(Table.Class[E[0]]), E[1]);
	TestEqual(TEXT("10 is vegetation family"), Table.Family[10], static_cast<uint8>(ELandCoverFamily::Vegetation));
	TestEqual(TEXT("95 is vegetation family"), Table.Family[95], static_cast<uint8>(ELandCoverFamily::Vegetation));
	TestEqual(TEXT("50 is built-up"), Table.Family[50], static_cast<uint8>(ELandCoverFamily::BuiltUp));
	TestEqual(TEXT("60 is bare"), Table.Family[60], static_cast<uint8>(ELandCoverFamily::Bare));
	TestEqual(TEXT("80 has no refinement"), Table.Family[80], static_cast<uint8>(ELandCoverFamily::None));
	TestEqual(TEXT("0 has no refinement"), Table.Family[0], static_cast<uint8>(ELandCoverFamily::None));

	TestEqual(TEXT("override: no warnings"), Table.Build({ Spec(10, TEXT("vegetation")) }, Materials).Num(), 0);
	TestEqual(TEXT("override applied"), static_cast<int32>(Table.Class[10]), T::Vegetation);
	TestEqual(TEXT("family stays by code"), Table.Family[10], static_cast<uint8>(ELandCoverFamily::Vegetation));
	const TArray<FString> W = Table.Build({ Spec(10, TEXT("lava")) }, Materials);
	TestTrue(TEXT("unknown material warned"), W.Num() == 1 && W[0].Contains(TEXT("lava")));
	TestEqual(TEXT("default kept"), static_cast<int32>(Table.Class[10]), T::TreeCanopy);

	FThermalMaterialSpec Gravel;
	Gravel.Name = TEXT("gravel");
	Materials.Build({ Gravel });
	Table.Build({ Spec(60, TEXT("gravel")) }, Materials);
	TestEqual(TEXT("user material reachable"), static_cast<int32>(Table.Class[60]), Materials.Find(TEXT("gravel")));
	int32 OutOfRange = 0;
	for (int32 C = 0; C < 256; ++C) OutOfRange += Table.Class[C] >= Materials.Num() ? 1 : 0;
	TestEqual(TEXT("every entry is a valid class index"), OutOfRange, 0);
	return true;
}
