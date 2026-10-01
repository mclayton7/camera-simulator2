// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include <limits>
#include "Config/CamSimConfig.h"
#include "Sensor/SensorOptics.h"
#include "Sensor/SensorTypes.h"
#include "SensorFrameParams.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorExposureYamlTest,
	"CamSim.Sensor.Config.ExposureFromYaml",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorExposureYamlTest::RunTest(const FString& Parameters)
{
	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT(
		"sensor_modes:\n"
		"  ir:\n"
		"    signal_weight_r: 0.6\n"
		"    signal_weight_g: 0.3\n"
		"    signal_weight_b: 0.1\n"
		"    exposure:\n"
		"      auto: false\n"
		"      min_gain_ev: -18\n"
		"      max_photon_gain_ev: 7\n"
		"      target_grey: 0.3\n"
		"      highlight_percentile: 0.95\n"
		"      lag_frames: 4\n"
		"      manual_gain_ev: -2\n"));
	const FSensorModeConfig& N = Cfg.SensorModeConfigs.FindChecked(ESensorMode::IR);
	TestEqual(TEXT("weight r"), N.SignalWeights.X, 0.6f);
	TestEqual(TEXT("weight b"), N.SignalWeights.Z, 0.1f);
	TestFalse(TEXT("auto"), N.Exposure.bAuto);
	TestEqual(TEXT("min"), N.Exposure.MinGainEv, -18.0f);
	TestEqual(TEXT("max"), N.Exposure.MaxPhotonGainEv, 7.0f);
	TestEqual(TEXT("grey"), N.Exposure.TargetGrey, 0.3f);
	TestEqual(TEXT("hi pct"), N.Exposure.HighlightPercentile, 0.95f);
	TestEqual(TEXT("lag"), N.Exposure.LagFrames, 4);
	TestEqual(TEXT("manual"), N.Exposure.ManualGainEv, -2.0f);
	TestEqual(TEXT("no unknown keys"), Cfg.UnknownYamlKeys.Num(), 0);
	return true;
}

// 3B.2: the legacy CPU sensor effect fields (and the quality-preset system) are
// gone. A yaml that still sets them must report each as an unknown key instead
// of silently doing nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorLegacyEffectKeysUnknownTest,
	"CamSim.Sensor.Config.LegacyEffectKeysUnknown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorLegacyEffectKeysUnknownTest::RunTest(const FString& Parameters)
{
	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT(
		"sensor_quality:\n"
		"  preset: high\n"
		"optical_realism:\n"
		"  lens_distortion: true\n"
		"  chromatic_aberration: true\n"
		"sensor_modes:\n"
		"  eo:\n"
		"    noise_netd: 0.02\n"
		"    fixed_pattern_noise: 0.02\n"
		"    vignetting: 0.3\n"
		"    scan_lines: true\n"
		"    gaussian_sigma: 0.5\n"
		"    defect_pixel_count: 10\n"
		"    quantization_bits: 6\n"
		"    ac_banding_amplitude: 2.0\n"
		"    thermal_drift_enabled: true\n"
		"    sun_glint_intensity: 1.0\n"
		"    contrast: 1.2\n"));

	auto HasUnknown = [&Cfg](const TCHAR* Needle)
	{
		return Cfg.UnknownYamlKeys.ContainsByPredicate(
			[Needle](const FString& K) { return K.Contains(Needle); });
	};

	TestTrue(TEXT("sensor_quality unknown"),        HasUnknown(TEXT("sensor_quality")));
	// The whole optical_realism section is gone, so it is reported once (not per key).
	TestTrue(TEXT("optical_realism unknown"),        HasUnknown(TEXT("optical_realism")));
	TestTrue(TEXT("noise_netd unknown"),             HasUnknown(TEXT("noise_netd")));
	TestTrue(TEXT("fixed_pattern_noise unknown"),    HasUnknown(TEXT("fixed_pattern_noise")));
	TestTrue(TEXT("vignetting unknown"),             HasUnknown(TEXT("vignetting")));
	TestTrue(TEXT("scan_lines unknown"),             HasUnknown(TEXT("scan_lines")));
	TestTrue(TEXT("gaussian_sigma unknown"),         HasUnknown(TEXT("gaussian_sigma")));
	TestTrue(TEXT("defect_pixel_count unknown"),     HasUnknown(TEXT("defect_pixel_count")));
	TestTrue(TEXT("quantization_bits unknown"),      HasUnknown(TEXT("quantization_bits")));
	TestTrue(TEXT("ac_banding_amplitude unknown"),   HasUnknown(TEXT("ac_banding_amplitude")));
	TestTrue(TEXT("thermal_drift_enabled unknown"),  HasUnknown(TEXT("thermal_drift_enabled")));
	TestTrue(TEXT("sun_glint_intensity unknown"),    HasUnknown(TEXT("sun_glint_intensity")));
	TestTrue(TEXT("contrast unknown"),               HasUnknown(TEXT("contrast")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorNoNvgTest,
	"CamSim.Sensor.Config.NvgRemoved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorNoNvgTest::RunTest(const FString& Parameters)
{
	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT("sensor_modes:\n  nvg:\n    target_grey: 0.3\n"));
	TestEqual(TEXT("only EO and IR modes"), Cfg.SensorModeConfigs.Num(), 2);
	TestTrue(TEXT("nvg block reported as unknown"), Cfg.UnknownYamlKeys.ContainsByPredicate(
		[](const FString& K) { return K.Contains(TEXT("nvg")); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorExposureDefaultsTest,
	"CamSim.Sensor.Config.ExposureDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorExposureDefaultsTest::RunTest(const FString& Parameters)
{
	const FSensorModeConfig Def;
	TestTrue(TEXT("auto by default"), Def.Exposure.bAuto);
	TestEqual(TEXT("BT.709 luminance weights sum to 1"),
		Def.SignalWeights.X + Def.SignalWeights.Y + Def.SignalWeights.Z, 1.0f, 1e-5f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorNv12DimsTest,
	"CamSim.Sensor.Config.Nv12DimensionsValidated",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorNv12DimsTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	Cfg.CaptureWidth = 1282;   // even, but not a multiple of 4
	Cfg.CaptureHeight = 720;
	auto HasError = [](const TArray<FString>& Errors, const TCHAR* Needle)
	{
		return Errors.ContainsByPredicate([Needle](const FString& E) { return E.Contains(Needle); });
	};
	TestTrue(TEXT("width % 4 reported"), HasError(Cfg.Validate(), TEXT("multiple of 4")));
	Cfg.CaptureWidth = 1280;
	TestFalse(TEXT("1280 accepted"), HasError(Cfg.Validate(), TEXT("multiple of 4")));

	// The GPU sensor graph is the only path: the check is unconditional (3B.2).
	FCamSimConfig Wide;
	Wide.CaptureWidth = 1366;
	Wide.CaptureHeight = 768;
	TestTrue(TEXT("1366 reported"), HasError(Wide.Validate(), TEXT("multiple of 4")));

	FSensorModeConfig& Eo = Cfg.SensorModeConfigs.FindOrAdd(ESensorMode::EO);
	Eo.Exposure.MinGainEv = 3.0f;
	Eo.Exposure.MaxPhotonGainEv = 2.0f;
	TestTrue(TEXT("min > max reported"), HasError(Cfg.Validate(), TEXT("min_gain_ev")));
	Eo.Exposure.MinGainEv = -20.0f;
	Eo.Exposure.HighlightPercentile = 1.5f;
	TestTrue(TEXT("percentile range reported"), HasError(Cfg.Validate(), TEXT("highlight_percentile")));
	Eo.Exposure.HighlightPercentile = 0.99f;
	Eo.AGCLowPercentile = 0.9f;
	Eo.AGCHighPercentile = 0.1f;
	TestTrue(TEXT("agc low >= high reported"), HasError(Cfg.Validate(), TEXT("agc_low_percentile")));
	return true;
}

namespace
{
	/** ROADMAP 3B.1 calibrated per-mode exposure (deploy/camsim_config.yaml). */
	void TestCalibratedExposure(FAutomationTestBase& T, const FCamSimConfig& Cfg, const TCHAR* Source)
	{
		struct FExpect { ESensorMode Mode; const TCHAR* Name; float Min, Max, Grey, HiPct; };
		const FExpect Expected[] = {
			{ ESensorMode::EO,  TEXT("eo"),  -20.0f, -12.5f, 0.18f, 0.99f },
			{ ESensorMode::IR,  TEXT("ir"),  -20.0f,  -6.0f, 0.18f, 0.99f },
		};
		for (const FExpect& E : Expected)
		{
			const FSensorModeConfig* M = Cfg.SensorModeConfigs.Find(E.Mode);
			if (!T.TestNotNull(FString::Printf(TEXT("%s: %s present"), Source, E.Name), M)) continue;
			const FSensorExposureConfig& X = M->Exposure;
			T.TestTrue (FString::Printf(TEXT("%s: %s auto"), Source, E.Name), X.bAuto);
			T.TestEqual(FString::Printf(TEXT("%s: %s min_gain_ev"), Source, E.Name), X.MinGainEv, E.Min);
			T.TestEqual(FString::Printf(TEXT("%s: %s max_photon_gain_ev"), Source, E.Name), X.MaxPhotonGainEv, E.Max);
			T.TestEqual(FString::Printf(TEXT("%s: %s target_grey"), Source, E.Name), X.TargetGrey, E.Grey);
			T.TestEqual(FString::Printf(TEXT("%s: %s highlight_percentile"), Source, E.Name), X.HighlightPercentile, E.HiPct);
			T.TestEqual(FString::Printf(TEXT("%s: %s lag_frames"), Source, E.Name), X.LagFrames, 2);
			T.TestEqual(FString::Printf(TEXT("%s: %s manual_gain_ev"), Source, E.Name), X.ManualGainEv, -12.0f);
		}
	}
}

// Built-in defaults and the canonical yaml must both carry the calibrated
// per-mode exposure, so neither silently falls back to the neutral struct defaults.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPerModeExposureDefaultsTest,
	"CamSim.Sensor.Config.PerModeExposureDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorPerModeExposureDefaultsTest::RunTest(const FString& Parameters)
{
	TestCalibratedExposure(*this, FCamSimConfig::LoadFromYamlString(TEXT("")), TEXT("built-in"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPerModeExposureCanonicalTest,
	"CamSim.Sensor.Config.PerModeExposureCanonicalConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorPerModeExposureCanonicalTest::RunTest(const FString& Parameters)
{
	const FString Path = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectDir(), TEXT("../../deploy/camsim_config.yaml")));
	FString Yaml;
	if (!TestTrue(FString::Printf(TEXT("read %s"), *Path), FFileHelper::LoadFileToString(Yaml, *Path)))
	{
		return false;
	}
	TestCalibratedExposure(*this, FCamSimConfig::LoadFromYamlString(Yaml, Path), TEXT("deploy/camsim_config.yaml"));
	return true;
}

// ROADMAP 3B.2 Task 5: sensor-class presets with optics/detector config.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPresetDefaultsTest, "CamSim.Sensor.Config.PresetDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorPresetDefaultsTest::RunTest(const FString& Parameters)
{
	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT(""));
	const FSensorModeConfig& Eo = Cfg.SensorModeConfigs.FindChecked(ESensorMode::EO);
	const FSensorModeConfig& Ir = Cfg.SensorModeConfigs.FindChecked(ESensorMode::IR);
	TestEqual(TEXT("eo preset"), Eo.Preset, FString(TEXT("eo_hd_cmos")));
	TestEqual(TEXT("eo full well"), Eo.Detector.FullWellE, 10000.0f);
	TestEqual(TEXT("eo pitch"), Eo.Optics.PixelPitchUm, 2.9f);
	TestEqual(TEXT("eo adc"), Eo.Detector.AdcBits, 12);
	TestEqual(TEXT("eo analog gain cap"), Eo.Detector.MaxAnalogGainDb, 30.0f);
	TestEqual(TEXT("ir preset"), Ir.Preset, FString(TEXT("mwir_cooled")));
	TestTrue(TEXT("ir photon"), Ir.Detector.Type == ESensorDetectorType::Photon);
	TestEqual(TEXT("ir full well"), Ir.Detector.FullWellE, 7000000.0f);
	TestEqual(TEXT("ir read noise"), Ir.Detector.ReadNoiseE, 400.0f);
	TestEqual(TEXT("ir wavelength"), Ir.Optics.WavelengthUm, 4.0f);
	TestEqual(TEXT("ir adc"), Ir.Detector.AdcBits, 14);
	TestEqual(TEXT("ir: no analog gain (AGC)"), Ir.Detector.MaxAnalogGainDb, 0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPresetOverrideTest, "CamSim.Sensor.Config.PresetOverride",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorPresetOverrideTest::RunTest(const FString& Parameters)
{
	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT(
		"sensor_modes:\n  ir:\n    preset: lwir_uncooled\n    seed: 7\n"
		"    detector:\n      column_fpn: 0.002\n    optics:\n      k1: -0.05\n"));
	const FSensorModeConfig& Ir = Cfg.SensorModeConfigs.FindChecked(ESensorMode::IR);
	TestTrue(TEXT("microbolometer"), Ir.Detector.Type == ESensorDetectorType::Microbolometer);
	TestEqual(TEXT("preset value kept"), Ir.Detector.TemporalNoise, 0.004f);
	TestEqual(TEXT("override wins"), Ir.Detector.ColumnFpn, 0.002f);
	TestEqual(TEXT("lwir: no analog gain"), Ir.Detector.MaxAnalogGainDb, 0.0f);
	TestEqual(TEXT("optics override"), Ir.Optics.K1, -0.05f);
	TestEqual(TEXT("seed"), Ir.Seed, 7u);
	TestEqual(TEXT("no unknown keys"), Cfg.UnknownYamlKeys.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPresetValidationTest, "CamSim.Sensor.Config.PresetValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorPresetValidationTest::RunTest(const FString& Parameters)
{
	auto Errors = [](const TCHAR* Yaml) { return FCamSimConfig::LoadFromYamlString(Yaml).Validate(); };
	auto Has = [](const TArray<FString>& E, const TCHAR* S) { return E.ContainsByPredicate([S](const FString& X) { return X.Contains(S); }); };
	TestTrue(TEXT("unknown preset"), Has(Errors(TEXT("sensor_modes:\n  eo:\n    preset: hd55\n")), TEXT("preset")));
	TestTrue(TEXT("full well"), Has(Errors(TEXT("sensor_modes:\n  eo:\n    detector:\n      full_well_e: 0\n")), TEXT("full_well_e")));
	TestTrue(TEXT("adc bits"), Has(Errors(TEXT("sensor_modes:\n  eo:\n    detector:\n      adc_bits: 20\n")), TEXT("adc_bits")));
	TestTrue(TEXT("negative noise"), Has(Errors(TEXT("sensor_modes:\n  eo:\n    detector:\n      read_noise_e: -1\n")), TEXT("read_noise_e")));
	TestTrue(TEXT("defect fraction"), Has(Errors(TEXT("sensor_modes:\n  eo:\n    detector:\n      hot_pixel_fraction: 0.5\n")), TEXT("hot_pixel_fraction")));
	TestTrue(TEXT("k1 range"), Has(Errors(TEXT("sensor_modes:\n  eo:\n    optics:\n      k1: 2\n")), TEXT("k1")));
	TestTrue(TEXT("f-number"), Has(Errors(TEXT("sensor_modes:\n  eo:\n    optics:\n      f_number: 0\n")), TEXT("f_number")));

	// Final-review F2: unknown detector type, vignetting exponent range, AE floor.
	const TArray<FString> BadType = Errors(TEXT("sensor_modes:\n  ir:\n    detector:\n      type: bolometer\n"));
	TestTrue(TEXT("unknown detector.type rejected"), Has(BadType, TEXT("detector.type")) && Has(BadType, TEXT("bolometer")));
	TestFalse(TEXT("detector.type microbolometer accepted"), Has(Errors(TEXT("sensor_modes:\n  ir:\n    detector:\n      type: microbolometer\n")), TEXT("detector.type")));
	TestFalse(TEXT("detector.type photon accepted"), Has(Errors(TEXT("sensor_modes:\n  eo:\n    detector:\n      type: Photon\n")), TEXT("detector.type")));
	TestTrue(TEXT("vignetting exponent > 8"), Has(Errors(TEXT("sensor_modes:\n  eo:\n    optics:\n      vignetting_exponent: 9\n")), TEXT("vignetting_exponent")));
	TestTrue(TEXT("negative vignetting exponent"), Has(Errors(TEXT("sensor_modes:\n  eo:\n    optics:\n      vignetting_exponent: -1\n")), TEXT("vignetting_exponent")));
	TestFalse(TEXT("vignetting exponent 8 accepted"), Has(Errors(TEXT("sensor_modes:\n  eo:\n    optics:\n      vignetting_exponent: 8\n")), TEXT("vignetting_exponent")));
	TestTrue(TEXT("min_gain_ev below -40"), Has(Errors(TEXT("sensor_modes:\n  eo:\n    exposure:\n      min_gain_ev: -200\n")), TEXT("min_gain_ev")));
	TestFalse(TEXT("min_gain_ev -40 accepted"), Has(Errors(TEXT("sensor_modes:\n  eo:\n    exposure:\n      min_gain_ev: -40\n")), TEXT("min_gain_ev")));
	TestEqual(TEXT("defaults valid"), FCamSimConfig().Validate().Num(), 0);

	// NaN never passes a range check (every rule is written !(x in range)).
	const float NaN = std::numeric_limits<float>::quiet_NaN();
	struct FNanCase { const TCHAR* Field; TFunction<void(FSensorModeConfig&)> Set; };
	const FNanCase Cases[] = {
		{ TEXT("f_number"),            [NaN](FSensorModeConfig& M) { M.Optics.FNumber = NaN; } },
		{ TEXT("pixel_pitch_um"),      [NaN](FSensorModeConfig& M) { M.Optics.PixelPitchUm = NaN; } },
		{ TEXT("wavelength_um"),       [NaN](FSensorModeConfig& M) { M.Optics.WavelengthUm = NaN; } },
		{ TEXT("extra_blur_px"),       [NaN](FSensorModeConfig& M) { M.Optics.ExtraBlurPx = NaN; } },
		{ TEXT("vignetting_exponent"), [NaN](FSensorModeConfig& M) { M.Optics.VignettingExponent = NaN; } },
		{ TEXT("k1"),                  [NaN](FSensorModeConfig& M) { M.Optics.K1 = NaN; } },
		{ TEXT("k2"),                  [NaN](FSensorModeConfig& M) { M.Optics.K2 = NaN; } },
		{ TEXT("full_well_e"),         [NaN](FSensorModeConfig& M) { M.Detector.FullWellE = NaN; } },
		{ TEXT("read_noise_e"),        [NaN](FSensorModeConfig& M) { M.Detector.ReadNoiseE = NaN; } },
		{ TEXT("prnu"),                [NaN](FSensorModeConfig& M) { M.Detector.Prnu = NaN; } },
		{ TEXT("dsnu_e"),              [NaN](FSensorModeConfig& M) { M.Detector.DsnuE = NaN; } },
		{ TEXT("dark_current_e_s"),    [NaN](FSensorModeConfig& M) { M.Detector.DarkCurrentEs = NaN; } },
		{ TEXT("max_analog_gain_db"),  [NaN](FSensorModeConfig& M) { M.Detector.MaxAnalogGainDb = NaN; } },
		{ TEXT("temporal_noise"),      [NaN](FSensorModeConfig& M) { M.Detector.TemporalNoise = NaN; } },
		{ TEXT("pixel_fpn"),           [NaN](FSensorModeConfig& M) { M.Detector.PixelFpn = NaN; } },
		{ TEXT("column_fpn"),          [NaN](FSensorModeConfig& M) { M.Detector.ColumnFpn = NaN; } },
		{ TEXT("row_fpn"),             [NaN](FSensorModeConfig& M) { M.Detector.RowFpn = NaN; } },
		{ TEXT("hot_pixel_fraction"),  [NaN](FSensorModeConfig& M) { M.Detector.HotPixelFraction = NaN; } },
		{ TEXT("dead_pixel_fraction"), [NaN](FSensorModeConfig& M) { M.Detector.DeadPixelFraction = NaN; } },
		{ TEXT("min_gain_ev"),         [NaN](FSensorModeConfig& M) { M.Exposure.MinGainEv = NaN; } },
	};
	for (const FNanCase& Case : Cases)
	{
		FCamSimConfig C;
		Case.Set(C.SensorModeConfigs.FindOrAdd(ESensorMode::EO));
		TestTrue(FString::Printf(TEXT("NaN %s rejected"), Case.Field), Has(C.Validate(), Case.Field));
	}
	return true;
}

// ROADMAP 3B.2 Task 10: the GPU runs a fixed Newton recurrence, so a lens whose inverse does not
// converge somewhere in the frame (rd over [0, corner]) at the configured HFOV is a config error.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorDistortionMustConvergeTest, "CamSim.Sensor.Config.DistortionMustConverge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorDistortionMustConvergeTest::RunTest(const FString& Parameters)
{
	auto Errors = [](const TCHAR* Yaml) { return FCamSimConfig::LoadFromYamlString(Yaml).Validate(); };
	auto Converge = [](const TArray<FString>& E)
	{
		return !E.ContainsByPredicate([](const FString& X) { return X.Contains(TEXT("does not converge")); });
	};
	const TArray<FString> Bad = Errors(TEXT("sensor_modes:\n  eo:\n    optics:\n      k1: -1.0\n"));
	TestFalse(TEXT("k1 -1.0 rejected"), Converge(Bad));
	TestTrue(TEXT("error names k1"), Bad.ContainsByPredicate([](const FString& X)
	{
		return X.Contains(TEXT("does not converge")) && X.Contains(TEXT("k1")) && X.Contains(TEXT("sensor_modes[0]"));
	}));
	TestTrue(TEXT("k1 -0.3 accepted"), Converge(Errors(TEXT("sensor_modes:\n  eo:\n    optics:\n      k1: -0.3\n"))));
	TestTrue(TEXT("IR mode checked too"), !Converge(Errors(TEXT("sensor_modes:\n  ir:\n    optics:\n      k1: -1.0\n"))));
	// The check is at the configured HFOV: a narrow field keeps -1.0 inside the invertible range.
	TestTrue(TEXT("narrow HFOV accepts k1 -1.0"), Converge(Errors(TEXT("hfov_deg: 10.0\nsensor_modes:\n  eo:\n    optics:\n      k1: -1.0\n"))));
	TestTrue(TEXT("defaults converge"), Converge(FCamSimConfig().Validate()));

	// The helper scans rd over [0, corner] (64 samples), not only the corner.
	TestTrue(TEXT("helper: -0.3 converges"), CamSimOptics::DistortionConverges(1280, 720, 60.0f, -0.3f, 0.0f));
	TestFalse(TEXT("helper: -1.0 fails"), CamSimOptics::DistortionConverges(1280, 720, 60.0f, -1.0f, 0.0f));
	TestTrue(TEXT("helper: no distortion"), CamSimOptics::DistortionConverges(1280, 720, 170.0f, 0.0f, 0.0f));
	return true;
}

// A PSF radius past the fast (R <= 3) groupshared tile costs more than the 1080p GPU budget: warned, not rejected.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorBlurBudgetWarningTest, "CamSim.Sensor.Config.BlurBudgetWarning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorBlurBudgetWarningTest::RunTest(const FString& Parameters)
{
	const FCamSimConfig Defaults;
	TestEqual(TEXT("defaults: no warnings"), Defaults.ValidateWarnings().Num(), 0);
	const FCamSimConfig Canonical = FCamSimConfig::LoadFromYamlString(TEXT("sensor_modes:\n  eo:\n    preset: eo_hd_cmos\n  ir:\n    preset: lwir_uncooled\n"));
	TestEqual(TEXT("presets: no warnings"), Canonical.ValidateWarnings().Num(), 0);

	const FCamSimConfig Wide = FCamSimConfig::LoadFromYamlString(TEXT("sensor_modes:\n  eo:\n    optics:\n      extra_blur_px: 3.0\n"));
	const TArray<FString> W = Wide.ValidateWarnings();
	TestTrue(TEXT("large blur warned"), W.ContainsByPredicate([](const FString& X)
	{
		return X.Contains(TEXT("sensor_modes[0]")) && X.Contains(TEXT("budget"));
	}));
	TestFalse(TEXT("large blur is not an error"), Wide.Validate().ContainsByPredicate([](const FString& X) { return X.Contains(TEXT("budget")); }));
	return true;
}

// UpdateSensorParams' optics fill: focal length from the live HFOV, optical sigma (not the full quadrature),
// taps from it; a live FOV where the lens does not converge falls back to no distortion (vignetting kept).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorOpticsParamsTest, "CamSim.Sensor.Config.OpticsFrameParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorOpticsParamsTest::RunTest(const FString& Parameters)
{
	FSensorOpticsConfig O;
	O.FNumber = 4.0f; O.PixelPitchUm = 2.9f; O.WavelengthUm = 0.55f; O.ExtraBlurPx = 1.0f;
	O.VignettingExponent = 4.0f; O.K1 = -0.3f; O.K2 = 0.01f;
	FSensorFrameParams P;
	TestTrue(TEXT("converges at 60 deg"), CamSimOptics::SetOptics(P, O, 1920, 1080, 60.0f));
	TestEqual(TEXT("focal"), P.FocalPx, CamSimOptics::FocalPx(1920, 60.0f));
	TestEqual(TEXT("k1"), P.K1, -0.3f);
	TestEqual(TEXT("k2"), P.K2, 0.01f);
	TestEqual(TEXT("vignetting"), P.VignettingExponent, 4.0f);
	TestEqual(TEXT("optical sigma"), P.PsfSigmaPx, CamSimOptics::PsfOpticalSigmaPx(O));
	TestTrue(TEXT("not the quadrature sigma"), P.PsfSigmaPx < CamSimOptics::PsfSigmaPx(O));
	TArray<float> Taps;
	CamSimOptics::PsfTaps(CamSimOptics::PsfOpticalSigmaPx(O), Taps);
	TestEqual(TEXT("taps count"), static_cast<int32>(P.NumPsfTaps), Taps.Num());
	TestEqual(TEXT("centre tap"), P.PsfTaps[0], Taps[0]);

	O.K1 = -1.0f; O.K2 = 0.0f;
	TestFalse(TEXT("wide live FOV does not converge"), CamSimOptics::SetOptics(P, O, 1920, 1080, 90.0f));
	TestEqual(TEXT("fallback k1 0"), P.K1, 0.0f);
	TestEqual(TEXT("fallback k2 0"), P.K2, 0.0f);
	TestEqual(TEXT("fallback keeps focal"), P.FocalPx, CamSimOptics::FocalPx(1920, 90.0f));
	TestEqual(TEXT("fallback keeps vignetting"), P.VignettingExponent, 4.0f);
	TestTrue(TEXT("zoomed in converges"), CamSimOptics::SetOptics(P, O, 1920, 1080, 5.0f));
	TestEqual(TEXT("zoomed k1 kept"), P.K1, -1.0f);
	return true;
}
