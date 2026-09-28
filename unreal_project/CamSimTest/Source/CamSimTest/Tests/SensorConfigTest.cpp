// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"
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
		"      max_gain_ev: 7\n"
		"      target_grey: 0.3\n"
		"      highlight_percentile: 0.95\n"
		"      lag_frames: 4\n"
		"      manual_gain_ev: -2\n"));
	const FSensorModeConfig& N = Cfg.SensorModeConfigs.FindChecked(ESensorMode::IR);
	TestEqual(TEXT("weight r"), N.SignalWeights.X, 0.6f);
	TestEqual(TEXT("weight b"), N.SignalWeights.Z, 0.1f);
	TestFalse(TEXT("auto"), N.Exposure.bAuto);
	TestEqual(TEXT("min"), N.Exposure.MinGainEv, -18.0f);
	TestEqual(TEXT("max"), N.Exposure.MaxGainEv, 7.0f);
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
	TestTrue(TEXT("lens_distortion unknown"),        HasUnknown(TEXT("lens_distortion")));
	TestTrue(TEXT("chromatic_aberration unknown"),   HasUnknown(TEXT("chromatic_aberration")));
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
	Eo.Exposure.MaxGainEv = 2.0f;
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
			T.TestEqual(FString::Printf(TEXT("%s: %s max_gain_ev"), Source, E.Name), X.MaxGainEv, E.Max);
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
