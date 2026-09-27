// Copyright CamSim Contributors. All Rights Reserved.

#include "Sensor/SensorPath.h"
#include "Config/CamSimConfig.h"

namespace
{
	/** Enabled effects that 3B.1's GPU path doesn't implement. Shrinks in 3B.2/3B.3. */
	void CollectUnported(const FCamSimConfig& Cfg, TArray<FString>& Out)
	{
		static const TCHAR* ModeNames[] = { TEXT("eo"), TEXT("ir"), TEXT("nvg") };
		for (const TPair<ESensorMode, FSensorModeConfig>& Pair : Cfg.SensorModeConfigs)
		{
			const FSensorModeConfig& M = Pair.Value;
			const TCHAR* Mode = ModeNames[FMath::Clamp(static_cast<int32>(Pair.Key), 0, 2)];
			auto Add = [&](bool bOn, const TCHAR* Key) { if (bOn) Out.Add(FString::Printf(TEXT("%s.%s"), Mode, Key)); };
			Add(M.NETD > 0.0f,                     TEXT("noise_netd"));
			Add(M.FixedPatternNoise > 0.0f,        TEXT("fixed_pattern_noise"));
			Add(M.Vignetting > 0.0f,               TEXT("vignetting"));
			Add(M.bScanLines,                      TEXT("scan_lines"));
			Add(M.IRExtinctionCoeff > 0.0f,        TEXT("ir_extinction_coeff"));
			Add(M.AtmosphericVisibilityM > 0.0f,   TEXT("atmospheric_visibility_m"));
			Add(M.ColorTemperatureK > 0.0f && !FMath::IsNearlyEqual(M.ColorTemperatureK, 6500.0f), TEXT("color_temperature_k"));
			Add(!FMath::IsNearlyEqual(M.Contrast, 1.0f), TEXT("contrast"));
			Add(M.BrightnessBias != 0.0f,          TEXT("brightness_bias"));
			Add(M.BlurRadius > 0,                  TEXT("blur_radius"));
			Add(M.AGCManualLevel >= 0.0f,          TEXT("agc_manual_level"));
			Add(M.QuantizationBits < 8 || M.bQuantizationDither, TEXT("quantization"));
			Add(M.DefectPixelCount > 0,            TEXT("defect_pixel_count"));
			Add(M.GaussianSigma > 0.0f,            TEXT("gaussian_sigma"));
			Add(M.ACBandingAmplitude > 0.0f,       TEXT("ac_banding_amplitude"));
			Add(M.bIRPointerEnabled,               TEXT("ir_pointer_enabled"));
			Add(M.bThermalDriftEnabled,            TEXT("thermal_drift_enabled"));
			Add(M.RollingShutterStrength > 0.0f,   TEXT("rolling_shutter_strength"));
			Add(M.VibrationAmplitude > 0.0f,       TEXT("vibration_amplitude"));
			Add(M.GainJitter > 0.0f || M.OffsetJitter > 0.0f, TEXT("gain_offset_jitter"));
			Add(M.SunGlintIntensity > 0.0f,        TEXT("sun_glint_intensity"));
		}
		if (Cfg.OpticalRealism.bEnabled && Cfg.OpticalRealism.bLensDistortion) Out.Add(TEXT("lens_distortion"));
		if (Cfg.Phase18.bPrecipitation)        Out.Add(TEXT("precipitation"));
		if (Cfg.Phase18.bDynamicIRExtinction)  Out.Add(TEXT("dynamic_ir_extinction"));
		if (Cfg.OverlayConfig.bEnabled)        Out.Add(TEXT("overlay"));
		if (Cfg.LaserDesignator.bEnabled)      Out.Add(TEXT("laser_designator"));
		if (Cfg.Performance.bGpuSensorEffects) Out.Add(TEXT("gpu_sensor_effects"));

		// ActiveSensorQuality: NoiseScale/VignettingScale/ScanLineScale/AtmosphereScale/
		// GaussianSigmaScale are multiplicative on a per-mode base that's already listed
		// above when non-zero, so they're no-ops here. Contrast, BrightnessBias and
		// BlurRadius are independent of the per-mode base (Contrast multiplies, but a
		// non-1 quality Contrast still changes the effective value even when the
		// per-mode Contrast is neutral at 1.0; BrightnessBias/BlurRadius are additive)
		// and quality presets ("high"/"ultra") set them non-neutral, so they must be
		// checked on their own.
		const FSensorQualityConfig& Q = Cfg.ActiveSensorQuality;
		if (!FMath::IsNearlyEqual(Q.Contrast, 1.0f)) Out.Add(TEXT("quality.contrast"));
		if (Q.BrightnessBias != 0.0f)                Out.Add(TEXT("quality.brightness_bias"));
		if (Q.BlurRadius > 0)                        Out.Add(TEXT("quality.blur_radius"));
	}
}

FSensorPathDecision FSensorPathSelector::Decide(const FCamSimConfig& Cfg)
{
	using ESP = FCamSimConfig::FRenderConfig::ESensorPath;
	FSensorPathDecision D;
	CollectUnported(Cfg, D.Unported);
	const FString List = FString::Join(D.Unported, TEXT(", "));
	if (!Cfg.Render.IsPrimary())
	{
		D.Path = ESensorPipelinePath::Legacy;
		D.Reason = TEXT("sensor path: legacy (view_source is scene_capture)");
	}
	else if (Cfg.Render.SensorPathMode == ESP::Legacy)
	{
		D.Path = ESensorPipelinePath::Legacy;
		D.Reason = TEXT("sensor path: legacy (render.sensor_path = legacy)");
	}
	else if (Cfg.Render.SensorPathMode == ESP::Gpu)
	{
		D.Path = ESensorPipelinePath::Gpu;
		D.Reason = D.Unported.Num() == 0 ? FString(TEXT("sensor path: gpu (forced)"))
			: FString::Printf(TEXT("sensor path: gpu (forced; ignored unported effects: %s)"), *List);
	}
	else
	{
		D.Path = D.Unported.Num() == 0 ? ESensorPipelinePath::Gpu : ESensorPipelinePath::Legacy;
		D.Reason = D.Unported.Num() == 0 ? FString(TEXT("sensor path: gpu"))
			: FString::Printf(TEXT("sensor path: legacy (unported: %s)"), *List);
	}
	return D;
}
