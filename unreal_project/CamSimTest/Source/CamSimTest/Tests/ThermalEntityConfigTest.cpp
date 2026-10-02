// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"

// CamSim.Thermal.Entity.Config.*: thermal.entity yaml, env and validation (ROADMAP 4C).

namespace
{
	int32 EntityErrors(const FCamSimConfig& Cfg)
	{
		int32 N = 0;
		for (const FString& E : Cfg.Validate()) N += E.Contains(TEXT("thermal.entity")) ? 1 : 0;
		return N;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalEntityConfigDefaultsTest, "CamSim.Thermal.Entity.Config.Defaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalEntityConfigDefaultsTest::RunTest(const FString& Parameters)
{
	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT("thermal: {}\n"));
	const FEntityThermalSettings& E = Cfg.Thermal.Entity;
	TestTrue(TEXT("enabled"), E.bEnabled);
	TestEqual(TEXT("idle hold"), E.IdleHoldS, 120.0f);
	TestEqual(TEXT("engine delta"), E.Kind(EEntityThermalPartKind::Engine).DeltaK, 45.0f);
	TestEqual(TEXT("exhaust temp"), E.Kind(EEntityThermalPartKind::Exhaust).TempK, 450.0f);
	TestEqual(TEXT("gear k"), E.Kind(EEntityThermalPartKind::RunningGear).KPerMps, 1.5f);
	TestEqual(TEXT("no thermal.entity validation errors"), EntityErrors(Cfg), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalEntityConfigYamlTest, "CamSim.Thermal.Entity.Config.YamlAndEnv",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalEntityConfigYamlTest::RunTest(const FString& Parameters)
{
	const TCHAR* Yaml =
		TEXT("thermal:\n")
		TEXT("  entity:\n")
		TEXT("    enabled: false\n")
		TEXT("    moving_mps: 1.0\n")
		TEXT("    idle_hold_s: 30\n")
		TEXT("    skin_running_k: 2\n")
		TEXT("    convection_v0_mps: 5\n")
		TEXT("    skin_tau_s: 100\n")
		TEXT("    burn_k: 650\n")
		TEXT("    burn_s: 60\n")
		TEXT("    hull_cool_tau_s: 900\n")
		TEXT("    engine: { delta_k: 30, tau_up_s: 100, tau_down_s: 200 }\n")
		TEXT("    exhaust: { temp_k: 500, tau_up_s: 10, tau_down_s: 40 }\n")
		TEXT("    running_gear: { k_per_mps: 2, max_k: 20, tau_up_s: 50, tau_down_s: 70 }\n");
	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(Yaml);
	const FEntityThermalSettings& E = Cfg.Thermal.Entity;
	TestFalse(TEXT("enabled"), E.bEnabled);
	TestEqual(TEXT("moving"), E.MovingMps, 1.0f);
	TestEqual(TEXT("hold"), E.IdleHoldS, 30.0f);
	TestEqual(TEXT("skin run"), E.SkinRunningK, 2.0f);
	TestEqual(TEXT("v0"), E.ConvectionV0Mps, 5.0f);
	TestEqual(TEXT("skin tau"), E.SkinTauS, 100.0f);
	TestEqual(TEXT("burn k"), E.BurnK, 650.0f);
	TestEqual(TEXT("burn s"), E.BurnS, 60.0f);
	TestEqual(TEXT("hull"), E.HullCoolTauS, 900.0f);
	TestEqual(TEXT("engine delta"), E.Kind(EEntityThermalPartKind::Engine).DeltaK, 30.0f);
	TestEqual(TEXT("engine up"), E.Kind(EEntityThermalPartKind::Engine).TauUpS, 100.0f);
	TestEqual(TEXT("engine down"), E.Kind(EEntityThermalPartKind::Engine).TauDownS, 200.0f);
	TestEqual(TEXT("exhaust temp"), E.Kind(EEntityThermalPartKind::Exhaust).TempK, 500.0f);
	TestEqual(TEXT("exhaust up"), E.Kind(EEntityThermalPartKind::Exhaust).TauUpS, 10.0f);
	TestEqual(TEXT("gear k"), E.Kind(EEntityThermalPartKind::RunningGear).KPerMps, 2.0f);
	TestEqual(TEXT("gear max"), E.Kind(EEntityThermalPartKind::RunningGear).MaxK, 20.0f);
	TestEqual(TEXT("gear down"), E.Kind(EEntityThermalPartKind::RunningGear).TauDownS, 70.0f);
	TestEqual(TEXT("no unknown keys"), Cfg.UnknownYamlKeys.Num(), 0);

	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_THERMAL_ENTITY_ENABLED"), TEXT("1"));
	const FCamSimConfig Env = FCamSimConfig::LoadFromYamlString(Yaml);
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_THERMAL_ENTITY_ENABLED"), TEXT(""));
	TestTrue(TEXT("env overrides enabled"), Env.Thermal.Entity.bEnabled);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalEntityConfigValidateTest, "CamSim.Thermal.Entity.Config.Validation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalEntityConfigValidateTest::RunTest(const FString& Parameters)
{
	auto ErrorsFor = [](TFunctionRef<void(FEntityThermalSettings&)> Edit)
	{
		FCamSimConfig Cfg;
		Edit(Cfg.Thermal.Entity);
		return EntityErrors(Cfg);
	};
	TestEqual(TEXT("defaults"), ErrorsFor([](FEntityThermalSettings&) {}), 0);
	TestEqual(TEXT("burn_k above LUT"), ErrorsFor([](FEntityThermalSettings& E) { E.BurnK = 1200.0f; }), 1);
	TestEqual(TEXT("exhaust below LUT"), ErrorsFor([](FEntityThermalSettings& E) { E.Kind(EEntityThermalPartKind::Exhaust).TempK = 100.0f; }), 1);
	TestEqual(TEXT("tau 0"), ErrorsFor([](FEntityThermalSettings& E) { E.SkinTauS = 0.0f; }), 1);
	TestEqual(TEXT("tau NaN"), ErrorsFor([](FEntityThermalSettings& E) { E.Kind(EEntityThermalPartKind::Engine).TauUpS = NAN; }), 1);
	TestEqual(TEXT("negative hold"), ErrorsFor([](FEntityThermalSettings& E) { E.IdleHoldS = -1.0f; }), 1);
	TestEqual(TEXT("v0 0"), ErrorsFor([](FEntityThermalSettings& E) { E.ConvectionV0Mps = 0.0f; }), 1);
	TestEqual(TEXT("gear k negative"), ErrorsFor([](FEntityThermalSettings& E) { E.Kind(EEntityThermalPartKind::RunningGear).KPerMps = -1.0f; }), 1);
	return true;
}
