// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Config/CamSimConfig.h"
#include "Thermal/ThermalMaterials.h"

#include <limits>

// CamSim.Thermal.Config.*: the thermal: section and the material table (ROADMAP 4A).

namespace
{
	bool HasError(const TArray<FString>& Errors, const TCHAR* Needle)
	{
		return Errors.ContainsByPredicate([Needle](const FString& E) { return E.Contains(Needle); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalConfigDefaultsTest, "CamSim.Thermal.Config.Defaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalConfigDefaultsTest::RunTest(const FString& Parameters)
{
	const FCamSimConfig::FThermalConfig T;
	TestTrue (TEXT("enabled"), T.bEnabled);
	TestEqual(TEXT("air temperature"), T.AirTemperatureC, 15.0f);
	TestEqual(TEXT("diurnal swing"), T.AirDiurnalSwingK, 8.0f);
	TestEqual(TEXT("MWIR extinction"), T.ExtinctionPerKmMwir, 0.15f);
	TestEqual(TEXT("LWIR extinction"), T.ExtinctionPerKmLwir, 0.10f);
	TestEqual(TEXT("fog factor"), T.FogIrFactor, 0.4f);
	TestEqual(TEXT("no material overrides"), T.Materials.Num(), 0);
	TestFalse(TEXT("defaults valid"), HasError(FCamSimConfig().Validate(), TEXT("thermal")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalConfigYamlTest, "CamSim.Thermal.Config.YamlAndEnv",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalConfigYamlTest::RunTest(const FString& Parameters)
{
	const FString Yaml = TEXT(
		"thermal:\n"
		"  enabled: false\n"
		"  air_temperature_c: 22.5\n"
		"  air_diurnal_swing_k: 12\n"
		"  extinction_per_km:\n"
		"    mwir: 0.3\n"
		"    lwir: 0.2\n"
		"  fog_ir_factor: 0.6\n"
		"  materials:\n"
		"    asphalt:\n"
		"      albedo: 0.08\n"
		"      k_fast: 0.025\n"
		"    gravel:\n"
		"      emissivity: 0.93\n"
		"      thermal_inertia: 1000\n"
		"      convection_w_m2k: 11\n"
		"      temperature: model\n");
	FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(Yaml);
	TestTrue (TEXT("parsed"), Cfg.bLoadedSuccessfully);
	TestEqual(TEXT("no unknown keys"), Cfg.UnknownYamlKeys.Num(), 0);
	TestFalse(TEXT("enabled"), Cfg.Thermal.bEnabled);
	TestEqual(TEXT("air"), Cfg.Thermal.AirTemperatureC, 22.5f);
	TestEqual(TEXT("swing"), Cfg.Thermal.AirDiurnalSwingK, 12.0f);
	TestEqual(TEXT("mwir"), Cfg.Thermal.ExtinctionPerKmMwir, 0.3f);
	TestEqual(TEXT("lwir"), Cfg.Thermal.ExtinctionPerKmLwir, 0.2f);
	TestEqual(TEXT("fog"), Cfg.Thermal.FogIrFactor, 0.6f);
	if (TestEqual(TEXT("two material specs"), Cfg.Thermal.Materials.Num(), 2))
	{
		const FThermalMaterialSpec& A = Cfg.Thermal.Materials[0];
		TestEqual(TEXT("asphalt name"), A.Name, FString(TEXT("asphalt")));
		TestTrue (TEXT("asphalt albedo set"), A.Albedo.IsSet() && *A.Albedo == 0.08f);
		TestFalse(TEXT("asphalt emissivity not set"), A.Emissivity.IsSet());
		const FThermalMaterialSpec& G = Cfg.Thermal.Materials[1];
		TestEqual(TEXT("gravel temperature"), G.Temperature, FString(TEXT("model")));
		TestTrue (TEXT("gravel inertia"), G.ThermalInertia.IsSet() && *G.ThermalInertia == 1000.0f);
	}
	TestTrue(TEXT("a typo is an unknown key"),
		FCamSimConfig::LoadFromYamlString(TEXT("thermal:\n  materials:\n    asphalt:\n      albedoo: 0.1\n")).UnknownYamlKeys
			.Contains(TEXT("thermal.materials.asphalt.albedoo")));

	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_THERMAL_ENABLED"), TEXT("true"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_THERMAL_AIR_TEMPERATURE_C"), TEXT("5"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_THERMAL_AIR_DIURNAL_SWING_K"), TEXT("4"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_THERMAL_EXTINCTION_MWIR"), TEXT("0.5"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_THERMAL_EXTINCTION_LWIR"), TEXT("0.25"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_THERMAL_FOG_IR_FACTOR"), TEXT("1.5"));
	Cfg = FCamSimConfig::LoadFromYamlString(Yaml);
	for (const TCHAR* K : { TEXT("CAMSIM_THERMAL_ENABLED"), TEXT("CAMSIM_THERMAL_AIR_TEMPERATURE_C"), TEXT("CAMSIM_THERMAL_AIR_DIURNAL_SWING_K"),
		TEXT("CAMSIM_THERMAL_EXTINCTION_MWIR"), TEXT("CAMSIM_THERMAL_EXTINCTION_LWIR"), TEXT("CAMSIM_THERMAL_FOG_IR_FACTOR") })
	{
		FPlatformMisc::SetEnvironmentVar(K, TEXT(""));
	}
	TestTrue (TEXT("env enabled"), Cfg.Thermal.bEnabled);
	TestEqual(TEXT("env air"), Cfg.Thermal.AirTemperatureC, 5.0f);
	TestEqual(TEXT("env swing"), Cfg.Thermal.AirDiurnalSwingK, 4.0f);
	TestEqual(TEXT("env mwir"), Cfg.Thermal.ExtinctionPerKmMwir, 0.5f);
	TestEqual(TEXT("env lwir"), Cfg.Thermal.ExtinctionPerKmLwir, 0.25f);
	TestEqual(TEXT("env fog"), Cfg.Thermal.FogIrFactor, 1.5f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalConfigValidateTest, "CamSim.Thermal.Config.Validate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalConfigValidateTest::RunTest(const FString& Parameters)
{
	const float NaN = std::numeric_limits<float>::quiet_NaN();
	auto With = [](TFunction<void(FCamSimConfig::FThermalConfig&)> Set) { FCamSimConfig C; Set(C.Thermal); return C.Validate(); };
	TestTrue (TEXT("air -100"),  HasError(With([](auto& T) { T.AirTemperatureC = -100.0f; }), TEXT("thermal.air_temperature_c")));
	TestTrue (TEXT("air NaN"),   HasError(With([NaN](auto& T) { T.AirTemperatureC = NaN; }), TEXT("thermal.air_temperature_c")));
	TestTrue (TEXT("swing 40"),  HasError(With([](auto& T) { T.AirDiurnalSwingK = 40.0f; }), TEXT("thermal.air_diurnal_swing_k")));
	TestTrue (TEXT("mwir < 0"),  HasError(With([](auto& T) { T.ExtinctionPerKmMwir = -0.1f; }), TEXT("thermal.extinction_per_km.mwir")));
	TestTrue (TEXT("lwir NaN"),  HasError(With([NaN](auto& T) { T.ExtinctionPerKmLwir = NaN; }), TEXT("thermal.extinction_per_km.lwir")));
	TestTrue (TEXT("fog 3"),     HasError(With([](auto& T) { T.FogIrFactor = 3.0f; }), TEXT("thermal.fog_ir_factor")));
	auto WithSpec = [&](TFunction<void(FThermalMaterialSpec&)> Set)
	{
		return With([Set](FCamSimConfig::FThermalConfig& T) { FThermalMaterialSpec S; S.Name = TEXT("asphalt"); Set(S); T.Materials.Add(S); });
	};
	TestTrue (TEXT("albedo 1"),        HasError(WithSpec([](auto& S) { S.Albedo = 1.0f; }), TEXT("thermal.materials.asphalt.albedo")));
	TestTrue (TEXT("emissivity 0"),    HasError(WithSpec([](auto& S) { S.Emissivity = 0.0f; }), TEXT("thermal.materials.asphalt.emissivity")));
	TestTrue (TEXT("inertia -1"),      HasError(WithSpec([](auto& S) { S.ThermalInertia = -1.0f; }), TEXT("thermal.materials.asphalt.thermal_inertia")));
	TestTrue (TEXT("convection 0"),    HasError(WithSpec([](auto& S) { S.ConvectionWm2K = 0.0f; }), TEXT("thermal.materials.asphalt.convection_w_m2k")));
	TestTrue (TEXT("k_fast 0.5"),      HasError(WithSpec([](auto& S) { S.KFast = 0.5f; }), TEXT("thermal.materials.asphalt.k_fast")));
	TestTrue (TEXT("temperature lava"), HasError(WithSpec([](auto& S) { S.Temperature = TEXT("lava"); }), TEXT("thermal.materials.asphalt.temperature")));
	TestTrue (TEXT("bad name"),        HasError(With([](auto& T) { FThermalMaterialSpec S; S.Name = TEXT("Bad Name"); T.Materials.Add(S); }), TEXT("thermal.materials")));
	TestFalse(TEXT("valid spec"),      HasError(WithSpec([](auto& S) { S.Albedo = 0.1f; S.Temperature = TEXT("water"); }), TEXT("thermal")));
	TestTrue (TEXT("too many classes"), HasError(With([](FCamSimConfig::FThermalConfig& T)
	{
		for (int32 I = 0; I < FThermalMaterialTable::MaxClasses; ++I) { FThermalMaterialSpec S; S.Name = FString::Printf(TEXT("class_%d"), I); T.Materials.Add(S); }
	}), TEXT("thermal.materials")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalMaterialTableTest, "CamSim.Thermal.Config.MaterialTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalMaterialTableTest::RunTest(const FString& Parameters)
{
	FThermalMaterialTable T;
	TestEqual(TEXT("terrain_default index"), T.Find(TEXT("terrain_default")), FThermalMaterialTable::TerrainDefault);
	TestEqual(TEXT("water index"), T.Find(TEXT("water")), FThermalMaterialTable::Water);
	TestEqual(TEXT("vehicle_paint index"), T.Find(TEXT("vehicle_paint")), FThermalMaterialTable::VehiclePaint);
	TestTrue (TEXT("water is fixed"), T.Get(FThermalMaterialTable::Water).Source == EThermalTemperatureSource::Water);
	TestEqual(TEXT("asphalt k_fast (spec)"), T.Get(T.Find(TEXT("asphalt"))).KFast, 0.02f);
	TestEqual(TEXT("vegetation k_fast (spec)"), T.Get(T.Find(TEXT("vegetation"))).KFast, 0.008f);
	TestEqual(TEXT("vehicle_paint k_fast (spec)"), T.Get(FThermalMaterialTable::VehiclePaint).KFast, 0.04f);
	TestEqual(TEXT("unknown"), T.Find(TEXT("unobtainium")), static_cast<int32>(INDEX_NONE));

	const uint32 V0 = T.GetVersion();
	TArray<FThermalMaterialSpec> Specs;
	{ FThermalMaterialSpec S; S.Name = TEXT("asphalt"); S.Albedo = 0.05f; Specs.Add(S); }
	{ FThermalMaterialSpec S; S.Name = TEXT("gravel"); S.Emissivity = 0.93f; Specs.Add(S); }
	{ FThermalMaterialSpec S; S.Name = TEXT("broken"); S.Albedo = 2.0f; Specs.Add(S); }
	const TArray<FString> Errors = T.Build(Specs);
	TestTrue (TEXT("version changed"), T.GetVersion() != V0);
	TestEqual(TEXT("one error (the broken spec)"), Errors.Num(), 1);
	TestEqual(TEXT("broken skipped"), T.Find(TEXT("broken")), static_cast<int32>(INDEX_NONE));
	const FThermalMaterial& A = T.Get(T.Find(TEXT("asphalt")));
	TestEqual(TEXT("asphalt albedo overridden"), A.Albedo, 0.05f);
	TestEqual(TEXT("asphalt emissivity kept"), A.Emissivity, 0.95f);
	const int32 Gi = T.Find(TEXT("gravel"));
	if (TestTrue(TEXT("gravel added"), Gi >= FThermalMaterialTable::BuiltIns().Num()))
	{
		const FThermalMaterial& G = T.Get(Gi);
		TestEqual(TEXT("gravel emissivity"), G.Emissivity, 0.93f);
		TestEqual(TEXT("gravel inherits terrain_default inertia"), G.ThermalInertia, T.Get(FThermalMaterialTable::TerrainDefault).ThermalInertia);
	}
	TestEqual(TEXT("built-in indices unchanged"), T.Find(TEXT("water")), FThermalMaterialTable::Water);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalConfigCanonicalTest, "CamSim.Thermal.Config.CanonicalConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalConfigCanonicalTest::RunTest(const FString& Parameters)
{
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../../deploy/camsim_config.yaml")));
	FString Yaml;
	if (!TestTrue(FString::Printf(TEXT("read %s"), *Path), FFileHelper::LoadFileToString(Yaml, *Path))) return false;
	TestTrue(TEXT("canonical yaml has a thermal: section"), Yaml.Contains(TEXT("\nthermal:")));
	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(Yaml, Path);
	const FCamSimConfig::FThermalConfig D;
	TestEqual(TEXT("enabled = default"), Cfg.Thermal.bEnabled, D.bEnabled);
	TestEqual(TEXT("air = default"), Cfg.Thermal.AirTemperatureC, D.AirTemperatureC);
	TestEqual(TEXT("swing = default"), Cfg.Thermal.AirDiurnalSwingK, D.AirDiurnalSwingK);
	TestEqual(TEXT("mwir = default"), Cfg.Thermal.ExtinctionPerKmMwir, D.ExtinctionPerKmMwir);
	TestEqual(TEXT("lwir = default"), Cfg.Thermal.ExtinctionPerKmLwir, D.ExtinctionPerKmLwir);
	TestEqual(TEXT("fog = default"), Cfg.Thermal.FogIrFactor, D.FogIrFactor);
	TestFalse(TEXT("no thermal validation errors"), HasError(Cfg.Validate(), TEXT("thermal")));
	return true;
}
