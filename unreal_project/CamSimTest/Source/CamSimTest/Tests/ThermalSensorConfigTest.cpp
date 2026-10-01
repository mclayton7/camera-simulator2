// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"
#include "Entity/EntityTypeTable.h"
#include "Sensor/SensorPresets.h"

#include <limits>

// CamSim.Thermal.Config.*: the sensor, ocean and entity-type keys the thermal path reads (ROADMAP 4A).

namespace
{
	bool HasError(const TArray<FString>& Errors, const TCHAR* Needle)
	{
		return Errors.ContainsByPredicate([Needle](const FString& E) { return E.Contains(Needle); });
	}
	const FSensorModeConfig& Ir(const FCamSimConfig& C) { return C.SensorModeConfigs.FindChecked(ESensorMode::IR); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalPresetBandsTest, "CamSim.Thermal.Config.PresetBands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalPresetBandsTest::RunTest(const FString& Parameters)
{
	struct FExpect { const TCHAR* Preset; float Lo, Hi; };
	for (const FExpect& E : { FExpect{ TEXT("eo_hd_cmos"), 0.4f, 0.7f }, FExpect{ TEXT("mwir_cooled"), 3.0f, 5.0f }, FExpect{ TEXT("lwir_uncooled"), 8.0f, 12.0f } })
	{
		FSensorModeConfig M;
		CamSimSensorPresets::Apply(E.Preset, M);
		TestEqual(*FString::Printf(TEXT("%s band_lo_um"), E.Preset), M.Detector.BandLoUm, E.Lo);
		TestEqual(*FString::Printf(TEXT("%s band_hi_um"), E.Preset), M.Detector.BandHiUm, E.Hi);
	}
	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT(
		"sensor_modes:\n  ir:\n    preset: mwir_cooled\n    detector:\n      band_lo_um: 3.4\n      band_hi_um: 4.9\n"));
	TestEqual(TEXT("override lo"), Ir(Cfg).Detector.BandLoUm, 3.4f);
	TestEqual(TEXT("override hi"), Ir(Cfg).Detector.BandHiUm, 4.9f);
	TestEqual(TEXT("no unknown keys"), Cfg.UnknownYamlKeys.Num(), 0);
	FCamSimConfig Bad = FCamSimConfig::LoadFromYamlString(TEXT("sensor_modes:\n  ir:\n    detector:\n      band_lo_um: 5\n      band_hi_um: 3\n"));
	TestTrue(TEXT("lo >= hi rejected"), HasError(Bad.Validate(), TEXT("band_lo_um")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalExposureConfigTest, "CamSim.Thermal.Config.ThermalExposureAndAgcCap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalExposureConfigTest::RunTest(const FString& Parameters)
{
	const FSensorModeConfig D;
	TestEqual(TEXT("agc_max_display_gain default"), D.AGCMaxDisplayGain, 40.0f);
	TestTrue (TEXT("thermal auto"), D.ThermalExposure.bAuto);
	TestEqual(TEXT("thermal min"), D.ThermalExposure.MinGainEv, -8.0f);
	TestEqual(TEXT("thermal max photon"), D.ThermalExposure.MaxPhotonGainEv, 0.0f);
	TestEqual(TEXT("thermal grey (300 K mid-range)"), D.ThermalExposure.TargetGrey, 0.5f);
	TestEqual(TEXT("thermal highlight"), D.ThermalExposure.HighlightPercentile, 0.99f);
	TestEqual(TEXT("thermal lag"), D.ThermalExposure.LagFrames, 2);
	TestEqual(TEXT("thermal manual"), D.ThermalExposure.ManualGainEv, -1.0f);
	TestEqual(TEXT("luminance exposure untouched"), D.Exposure.MaxPhotonGainEv, -6.0f);

	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT(
		"sensor_modes:\n"
		"  ir:\n"
		"    agc_max_display_gain: 25\n"
		"    thermal_exposure:\n"
		"      auto: false\n"
		"      min_gain_ev: -6\n"
		"      max_photon_gain_ev: -0.5\n"
		"      target_grey: 0.4\n"
		"      highlight_percentile: 0.98\n"
		"      lag_frames: 3\n"
		"      manual_gain_ev: -2\n"));
	TestEqual(TEXT("no unknown keys"), Cfg.UnknownYamlKeys.Num(), 0);
	TestEqual(TEXT("cap"), Ir(Cfg).AGCMaxDisplayGain, 25.0f);
	TestFalse(TEXT("auto"), Ir(Cfg).ThermalExposure.bAuto);
	TestEqual(TEXT("min"), Ir(Cfg).ThermalExposure.MinGainEv, -6.0f);
	TestEqual(TEXT("max"), Ir(Cfg).ThermalExposure.MaxPhotonGainEv, -0.5f);
	TestEqual(TEXT("grey"), Ir(Cfg).ThermalExposure.TargetGrey, 0.4f);
	TestEqual(TEXT("hi pct"), Ir(Cfg).ThermalExposure.HighlightPercentile, 0.98f);
	TestEqual(TEXT("lag"), Ir(Cfg).ThermalExposure.LagFrames, 3);
	TestEqual(TEXT("manual"), Ir(Cfg).ThermalExposure.ManualGainEv, -2.0f);

	TestTrue(TEXT("cap < 1 rejected"), HasError(FCamSimConfig::LoadFromYamlString(TEXT("sensor_modes:\n  ir:\n    agc_max_display_gain: 0.5\n")).Validate(),
		TEXT("agc_max_display_gain")));
	TestTrue(TEXT("thermal min > max rejected"), HasError(FCamSimConfig::LoadFromYamlString(TEXT(
		"sensor_modes:\n  ir:\n    thermal_exposure:\n      min_gain_ev: 1\n      max_photon_gain_ev: 0\n")).Validate(), TEXT("thermal_exposure")));
	TestFalse(TEXT("built-in defaults valid"), HasError(FCamSimConfig::LoadFromYamlString(TEXT("")).Validate(), TEXT("thermal_exposure")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalIrPresetEnvTest, "CamSim.Thermal.Config.IrPresetAndCaptureEnv",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalIrPresetEnvTest::RunTest(const FString& Parameters)
{
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_IR_PRESET"), TEXT("lwir_uncooled"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_CAPTURE_WIDTH"), TEXT("1920"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_CAPTURE_HEIGHT"), TEXT("1080"));
	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT("capture_width: 1280\ncapture_height: 720\nsensor_modes:\n  ir:\n    preset: mwir_cooled\n"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_IR_PRESET"), TEXT(""));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_CAPTURE_WIDTH"), TEXT(""));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_CAPTURE_HEIGHT"), TEXT(""));
	TestEqual(TEXT("preset name"), Ir(Cfg).Preset, FString(TEXT("lwir_uncooled")));
	TestTrue (TEXT("microbolometer"), Ir(Cfg).Detector.Type == ESensorDetectorType::Microbolometer);
	TestEqual(TEXT("LWIR band lo"), Ir(Cfg).Detector.BandLoUm, 8.0f);
	TestEqual(TEXT("width"), Cfg.CaptureWidth, 1920);
	TestEqual(TEXT("height"), Cfg.CaptureHeight, 1080);
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_IR_PRESET"), TEXT("mwir_typo"));
	const FCamSimConfig Bad = FCamSimConfig::LoadFromYamlString(TEXT(""));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_IR_PRESET"), TEXT(""));
	TestTrue(TEXT("unknown env preset reported"), HasError(Bad.Validate(), TEXT("mwir_typo")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalWaterTemperatureTest, "CamSim.Thermal.Config.WaterTemperature",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalWaterTemperatureTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("default"), FCamSimConfig::FOceanConfig().WaterTemperatureC, 15.0f);
	FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT("ocean:\n  water_temperature_c: 9.5\n"));
	TestEqual(TEXT("yaml"), Cfg.Ocean.WaterTemperatureC, 9.5f);
	TestEqual(TEXT("no unknown keys"), Cfg.UnknownYamlKeys.Num(), 0);
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_OCEAN_WATER_TEMPERATURE_C"), TEXT("21"));
	Cfg = FCamSimConfig::LoadFromYamlString(TEXT("ocean:\n  water_temperature_c: 9.5\n"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_OCEAN_WATER_TEMPERATURE_C"), TEXT(""));
	TestEqual(TEXT("env"), Cfg.Ocean.WaterTemperatureC, 21.0f);
	FCamSimConfig Hot; Hot.Ocean.WaterTemperatureC = 45.0f;
	TestTrue(TEXT("45 C rejected"), HasError(Hot.Validate(), TEXT("ocean.water_temperature_c")));
	FCamSimConfig NaNCfg; NaNCfg.Ocean.WaterTemperatureC = std::numeric_limits<float>::quiet_NaN();
	TestTrue(TEXT("NaN rejected"), HasError(NaNCfg.Validate(), TEXT("ocean.water_temperature_c")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalEntityTypeKeysTest, "CamSim.Thermal.Config.EntityTypeKeys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalEntityTypeKeysTest::RunTest(const FString& Parameters)
{
	FEntityTypeTable Table;
	Table.LoadFromYamlString(TEXT(
		"entity_types:\n"
		"  \"2001\":\n"
		"    mesh: truck/ural_4320.glb\n"
		"    thermal_material: asphalt\n"
		"    thermal_offset_k: 12.5\n"
		"  \"3001\":\n"
		"    mesh: boat/mako_655.glb\n"
		"    thermal_offset_k: 9999\n"));
	const FEntityTypeEntry* Truck = Table.FindEntry(2001);
	const FEntityTypeEntry* Boat = Table.FindEntry(3001);
	if (!TestNotNull(TEXT("truck"), Truck) || !TestNotNull(TEXT("boat"), Boat)) return false;
	TestEqual(TEXT("truck material"), Truck->ThermalMaterial, FString(TEXT("asphalt")));
	TestTrue (TEXT("truck offset"), Truck->ThermalOffsetK.IsSet() && *Truck->ThermalOffsetK == 12.5f);
	TestTrue (TEXT("boat material empty (vehicle_paint)"), Boat->ThermalMaterial.IsEmpty());
	TestFalse(TEXT("out-of-range offset ignored"), Boat->ThermalOffsetK.IsSet());
	return true;
}
