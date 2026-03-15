// Copyright CamSim Contributors. All Rights Reserved.

#include "Config/CamSimConfig.h"
#include "CamSimTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshadow"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wold-style-cast"
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wundef"
#endif
// UE5 CoreDefines.h defines DEFAULTS as 0, which collides with a ryml enum member.
#pragma push_macro("DEFAULTS")
#undef DEFAULTS
#include "ryml/ryml_all.hpp"
#pragma pop_macro("DEFAULTS")
#ifdef __clang__
#pragma clang diagnostic pop
#endif

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static FString GetEnv(const TCHAR* Key, const FString& Default)
{
	FString Value = FPlatformMisc::GetEnvironmentVariable(Key);
	return Value.IsEmpty() ? Default : Value;
}

static int32 GetEnvInt(const TCHAR* Key, int32 Default)
{
	FString Value = FPlatformMisc::GetEnvironmentVariable(Key);
	return Value.IsEmpty() ? Default : FCString::Atoi(*Value);
}

static double GetEnvDouble(const TCHAR* Key, double Default)
{
	FString Value = FPlatformMisc::GetEnvironmentVariable(Key);
	return Value.IsEmpty() ? Default : FCString::Atod(*Value);
}

static float GetEnvFloat(const TCHAR* Key, float Default)
{
	FString Value = FPlatformMisc::GetEnvironmentVariable(Key);
	return Value.IsEmpty() ? Default : FCString::Atof(*Value);
}

static bool GetEnvBool(const TCHAR* Key, bool Default)
{
	FString Value = FPlatformMisc::GetEnvironmentVariable(Key);
	return Value.IsEmpty() ? Default : FCString::Atoi(*Value) != 0;
}

static FCamSimConfig::EReadbackFormat ParseReadbackFormat(const FString& Value)
{
	const FString Lower = Value.ToLower();
	if (Lower == TEXT("bgra")) return FCamSimConfig::EReadbackFormat::BGRA;
	if (Lower == TEXT("rgba")) return FCamSimConfig::EReadbackFormat::RGBA;
	if (Lower == TEXT("argb")) return FCamSimConfig::EReadbackFormat::ARGB;
	if (Lower == TEXT("abgr")) return FCamSimConfig::EReadbackFormat::ABGR;
	return FCamSimConfig::EReadbackFormat::Auto;
}

static FCamSimConfig::EEncoderWatchdogPolicy ParseWatchdogPolicy(const FString& Value)
{
	const FString Lower = Value.ToLower();
	if (Lower == TEXT("log_only")) return FCamSimConfig::EEncoderWatchdogPolicy::LogOnly;
	if (Lower == TEXT("fail_fast")) return FCamSimConfig::EEncoderWatchdogPolicy::FailFast;
	return FCamSimConfig::EEncoderWatchdogPolicy::Reconnect;
}

static FString NormalizeQualityPreset(const FString& Value)
{
	return Value.TrimStartAndEnd().ToLower();
}

static void ResolveActiveSensorQuality(FCamSimConfig& Cfg)
{
	const FString Key = NormalizeQualityPreset(Cfg.SensorQualityPreset);
	if (const FSensorQualityConfig* Found = Cfg.SensorQualityProfiles.Find(Key))
	{
		Cfg.ActiveSensorQuality = *Found;
		return;
	}

	UE_LOG(LogCamSim, Warning,
		TEXT("Config: unknown sensor_quality.preset '%s' (known: low|medium|high|ultra|custom) — using medium"),
		*Cfg.SensorQualityPreset);
	Cfg.SensorQualityPreset = TEXT("medium");
	Cfg.ActiveSensorQuality = Cfg.SensorQualityProfiles.FindRef(TEXT("medium"));
}

// ---------------------------------------------------------------------------
// ryml YAML helpers — mirror the old TryGet*Field call pattern
// ---------------------------------------------------------------------------

static FString RymlToFString(c4::csubstr S)
{
	return FString(static_cast<int32>(S.len), UTF8_TO_TCHAR(S.str));
}

static bool YamlString(ryml::ConstNodeRef Node, c4::csubstr Key, FString& Out)
{
	if (!Node.has_child(Key)) return false;
	ryml::ConstNodeRef Child = Node[Key];
	if (!Child.has_val()) return false;
	Out = RymlToFString(Child.val());
	return true;
}

static bool YamlInt(ryml::ConstNodeRef Node, c4::csubstr Key, int32& Out)
{
	if (!Node.has_child(Key)) return false;
	ryml::ConstNodeRef Child = Node[Key];
	if (!Child.has_val()) return false;
	FString Str = RymlToFString(Child.val());
	Out = FCString::Atoi(*Str);
	return true;
}

static bool YamlFloat(ryml::ConstNodeRef Node, c4::csubstr Key, float& Out)
{
	if (!Node.has_child(Key)) return false;
	ryml::ConstNodeRef Child = Node[Key];
	if (!Child.has_val()) return false;
	FString Str = RymlToFString(Child.val());
	Out = FCString::Atof(*Str);
	return true;
}

static bool YamlDouble(ryml::ConstNodeRef Node, c4::csubstr Key, double& Out)
{
	if (!Node.has_child(Key)) return false;
	ryml::ConstNodeRef Child = Node[Key];
	if (!Child.has_val()) return false;
	FString Str = RymlToFString(Child.val());
	Out = FCString::Atod(*Str);
	return true;
}

static bool YamlBool(ryml::ConstNodeRef Node, c4::csubstr Key, bool& Out)
{
	if (!Node.has_child(Key)) return false;
	ryml::ConstNodeRef Child = Node[Key];
	if (!Child.has_val()) return false;
	c4::csubstr Val = Child.val();
	// YAML booleans: true/false/yes/no/on/off (case insensitive)
	Out = (Val == "true" || Val == "True" || Val == "TRUE" ||
	       Val == "yes"  || Val == "Yes"  || Val == "YES"  ||
	       Val == "on"   || Val == "On"   || Val == "ON"   ||
	       Val == "1");
	return true;
}

static float YamlFloatVal(ryml::ConstNodeRef Node)
{
	FString Str = RymlToFString(Node.val());
	return FCString::Atof(*Str);
}

static double YamlDoubleVal(ryml::ConstNodeRef Node)
{
	FString Str = RymlToFString(Node.val());
	return FCString::Atod(*Str);
}

// ---------------------------------------------------------------------------
// Config file path resolution
// ---------------------------------------------------------------------------

FString FCamSimConfig::GetConfigFilePath()
{
	FString YamlPath = FPaths::Combine(FPaths::ProjectDir(), TEXT("camsim_config.yaml"));
	if (!FPaths::FileExists(YamlPath))
	{
		YamlPath = FPaths::Combine(FPlatformProcess::BaseDir(), TEXT("camsim_config.yaml"));
	}
	return YamlPath;
}

// ---------------------------------------------------------------------------
// FCamSimConfig
// ---------------------------------------------------------------------------

FCamSimConfig FCamSimConfig::Load()
{
	FCamSimConfig Cfg; // default values from member initialisers

	// Apply default sensor mode configs (overwritten by YAML if present)
	{
		FSensorModeConfig EoCfg;
		EoCfg.NETD              = 0.0f;
		EoCfg.FixedPatternNoise = 0.0f;
		EoCfg.Vignetting        = 0.10f;
		EoCfg.bScanLines        = false;
		EoCfg.Contrast          = 1.0f;
		EoCfg.BrightnessBias    = 0.0f;
		EoCfg.BlurRadius        = 0;
		EoCfg.ColorTemperatureK = 6500.0f;
		// Phase 16 Sprint 2 defaults (EO)
		EoCfg.RollingShutterStrength = 0.0f;
		EoCfg.SunGlintIntensity      = 0.0f;
		EoCfg.SunGlintThreshold      = 220.0f;
		EoCfg.SunGlintSpread         = 2.0f;
		Cfg.SensorModeConfigs.Add(ESensorMode::EO, EoCfg);

		FSensorModeConfig IrCfg;
		IrCfg.NETD              = 0.01f;
		IrCfg.FixedPatternNoise = 0.005f;
		IrCfg.Vignetting        = 0.20f;
		IrCfg.bScanLines        = false;
		IrCfg.IRExtinctionCoeff = 1e-5f;
		IrCfg.AtmosphericVisibilityM = 12000.0f;
		IrCfg.AtmosphereStrength = 0.75f;
		IrCfg.Contrast          = 1.1f;
		IrCfg.BrightnessBias    = -0.03f;
		// Phase 16 defaults
		IrCfg.bAGCEnabled        = true;
		IrCfg.AGCLowPercentile   = 0.01f;
		IrCfg.AGCHighPercentile  = 0.99f;
		IrCfg.AGCManualLevel     = -1.0f;
		IrCfg.DefectPixelCount   = 150;
		IrCfg.DefectHotRatio     = 0.6f;
		IrCfg.DefectSeed         = 42;
		IrCfg.GaussianSigma      = 0.5f;
		IrCfg.ACBandingAmplitude = 4.0f;
		IrCfg.ACBandingFrequency = 3.0f;
		// Phase 16 Sprint 2 defaults (IR)
		IrCfg.bThermalDriftEnabled = true;
		IrCfg.ThermalDriftRate     = 0.5f;
		IrCfg.NUCIntervalSec       = 30.0f;
		IrCfg.AGCLagFrames         = 2;
		IrCfg.GainJitter           = 0.005f;
		IrCfg.OffsetJitter         = 1.0f;
		Cfg.SensorModeConfigs.Add(ESensorMode::IR, IrCfg);

		FSensorModeConfig NvgCfg;
		NvgCfg.NETD              = 0.03f;
		NvgCfg.FixedPatternNoise = 0.0f;
		NvgCfg.Vignetting        = 0.35f;
		NvgCfg.bScanLines        = false;
		NvgCfg.ScanLineStrength  = 0.05f;
		NvgCfg.AtmosphericVisibilityM = 8000.0f;
		NvgCfg.AtmosphereStrength = 0.9f;
		NvgCfg.Contrast          = 1.2f;
		NvgCfg.BrightnessBias    = 0.02f;
		NvgCfg.ColorTemperatureK = 5200.0f;
		// Phase 16 defaults
		NvgCfg.bAGCEnabled       = true;
		NvgCfg.AGCLowPercentile  = 0.02f;
		NvgCfg.AGCHighPercentile = 0.98f;
		NvgCfg.AGCManualLevel    = -1.0f;
		NvgCfg.AGCLagFrames      = 1;
		Cfg.SensorModeConfigs.Add(ESensorMode::NVG, NvgCfg);
	}

	// Default FOV presets (wide → narrow); YAML values replace these if present
	Cfg.SensorFovPresets = { 60.0f, 20.0f, 5.0f };

	{
		FSensorQualityConfig Low;
		Low.NoiseScale      = 0.75f;
		Low.VignettingScale = 0.8f;
		Low.ScanLineScale   = 0.8f;
		Low.AtmosphereScale = 0.7f;
		Low.BlurRadius      = 0;
		Low.Contrast        = 0.95f;
		Low.BrightnessBias  = 0.0f;
		Cfg.SensorQualityProfiles.Add(TEXT("low"), Low);

		FSensorQualityConfig Medium;
		Medium.NoiseScale      = 1.0f;
		Medium.VignettingScale = 1.0f;
		Medium.ScanLineScale   = 1.0f;
		Medium.AtmosphereScale = 1.0f;
		Medium.BlurRadius      = 0;
		Medium.Contrast        = 1.0f;
		Medium.BrightnessBias  = 0.0f;
		Cfg.SensorQualityProfiles.Add(TEXT("medium"), Medium);

		FSensorQualityConfig High;
		High.NoiseScale      = 1.25f;
		High.VignettingScale = 1.15f;
		High.ScanLineScale   = 1.15f;
		High.AtmosphereScale = 1.15f;
		High.BlurRadius      = 1;
		High.Contrast        = 1.05f;
		High.BrightnessBias  = 0.0f;
		Cfg.SensorQualityProfiles.Add(TEXT("high"), High);

		FSensorQualityConfig Ultra;
		Ultra.NoiseScale      = 1.5f;
		Ultra.VignettingScale = 1.25f;
		Ultra.ScanLineScale   = 1.25f;
		Ultra.AtmosphereScale = 1.25f;
		Ultra.BlurRadius      = 2;
		Ultra.Contrast        = 1.1f;
		Ultra.BrightnessBias  = 0.0f;
		Cfg.SensorQualityProfiles.Add(TEXT("ultra"), Ultra);

		Cfg.SensorQualityProfiles.Add(TEXT("custom"), Medium);
		Cfg.ActiveSensorQuality = Medium;
	}

	// Attempt to read YAML config
	FString YamlPath = GetConfigFilePath();

	FString YamlContent;
	if (FFileHelper::LoadFileToString(YamlContent, *YamlPath))
	{
		// Convert FString (UTF-16) to UTF-8 std::string for ryml
		FTCHARToUTF8 Utf8(*YamlContent);
		c4::csubstr Src(Utf8.Get(), Utf8.Length());

		ryml::Tree Tree;
		try
		{
			Tree = ryml::parse_in_arena(Src);
		}
		catch (const std::exception& Ex)
		{
			UE_LOG(LogCamSim, Error, TEXT("Failed to parse %s: %hs - using defaults"), *YamlPath, Ex.what());
			Cfg.bLoadedSuccessfully = false;
			ApplyEnvOverrides(Cfg);
			return Cfg;
		}

		ryml::ConstNodeRef Root = Tree.rootref();

		YamlString(Root, "cigi_bind_addr",      Cfg.CigiBindAddr);
		YamlInt   (Root, "cigi_port",           Cfg.CigiPort);
		YamlString(Root, "cigi_response_addr",  Cfg.CigiResponseAddr);
		YamlInt   (Root, "cigi_response_port",  Cfg.CigiResponsePort);
		YamlString(Root, "multicast_addr",   Cfg.MulticastAddr);
		YamlInt   (Root, "multicast_port",   Cfg.MulticastPort);
		YamlInt   (Root, "video_bitrate",    Cfg.VideoBitrate);
		YamlString(Root, "h264_preset",      Cfg.H264Preset);
		YamlString(Root, "h264_tune",        Cfg.H264Tune);
		YamlInt   (Root, "capture_width",    Cfg.CaptureWidth);
		YamlInt   (Root, "capture_height",   Cfg.CaptureHeight);
		YamlFloat (Root, "frame_rate",       Cfg.FrameRate);
		YamlBool  (Root, "swap_rb_readback", Cfg.bSwapRBReadback);
		YamlInt   (Root, "readback_ready_polls", Cfg.ReadbackReadyPolls);
		{
			FString ReadbackFmt;
			if (YamlString(Root, "readback_format", ReadbackFmt))
			{
				Cfg.ReadbackFormat = ParseReadbackFormat(ReadbackFmt);
			}
		}
		{
			FString WatchdogPolicy;
			if (YamlString(Root, "encoder_watchdog_policy", WatchdogPolicy))
			{
				Cfg.EncoderWatchdogPolicy = ParseWatchdogPolicy(WatchdogPolicy);
			}
		}
		YamlInt   (Root, "encoder_watchdog_interval_ticks", Cfg.EncoderWatchdogIntervalTicks);
		YamlInt   (Root, "watchdog_max_reconnects", Cfg.WatchdogMaxReconnects);
		YamlString(Root, "encoder", Cfg.Encoder);
		YamlInt   (Root, "max_entities", Cfg.MaxEntities);
		YamlBool  (Root, "use_instanced_rendering", Cfg.bUseInstancedRendering);
		YamlFloat (Root, "hfov_deg",         Cfg.HFovDeg);
		YamlString(Root, "terrain_provider", Cfg.TerrainProvider);
		YamlString(Root, "imagery_provider", Cfg.ImageryProvider);

		if (Root.has_child("terrain"))
		{
			ryml::ConstNodeRef TerrainNode = Root["terrain"];
			YamlString(TerrainNode, "provider", Cfg.TerrainProvider);
		}
		if (Root.has_child("imagery"))
		{
			ryml::ConstNodeRef ImageryNode = Root["imagery"];
			YamlString(ImageryNode, "provider", Cfg.ImageryProvider);
		}

		YamlBool  (Root, "log_tile_stats",             Cfg.bLogTileSelectionStats);
		YamlFloat (Root, "tile_preload_fov_scale",     Cfg.TilePreloadFovScale);
		YamlInt   (Root, "max_simultaneous_tile_loads", Cfg.MaxSimultaneousTileLoads);
		YamlFloat (Root, "maximum_screen_space_error", Cfg.MaximumScreenSpaceError);
		YamlInt   (Root, "maximum_cached_bytes_mb",    Cfg.MaximumCachedBytesMB);
		YamlInt  (Root, "loading_descendant_limit", Cfg.LoadingDescendantLimit);
		YamlBool (Root, "use_lod_transitions",      Cfg.bUseLodTransitions);
		YamlFloat(Root, "lod_transition_length",    Cfg.LodTransitionLength);
		YamlDouble(Root, "start_latitude",   Cfg.StartLatitude);
		YamlDouble(Root, "start_longitude",  Cfg.StartLongitude);
		YamlDouble(Root, "start_altitude",   Cfg.StartAltitude);
		YamlFloat (Root, "start_yaw",        Cfg.StartYaw);
		YamlFloat (Root, "start_pitch",      Cfg.StartPitch);
		YamlFloat (Root, "start_roll",       Cfg.StartRoll);
		YamlFloat (Root, "start_hour",       Cfg.StartHour);
		YamlInt   (Root, "camera_entity_id",           Cfg.CameraEntityId);
		YamlFloat (Root, "gimbal_max_slew_rate",       Cfg.GimbalMaxSlewRateDegPerSec);
		YamlFloat (Root, "gimbal_pitch_min",           Cfg.GimbalPitchMin);
		YamlFloat (Root, "gimbal_pitch_max",           Cfg.GimbalPitchMax);
		YamlFloat (Root, "gimbal_yaw_min",             Cfg.GimbalYawMin);
		YamlFloat (Root, "gimbal_yaw_max",             Cfg.GimbalYawMax);

		// FOV presets: optional YAML array of floats (wide -> narrow)
		// Replaces the defaults set above when present in config.
		if (Root.has_child("sensor_fov_presets"))
		{
			ryml::ConstNodeRef PresetsNode = Root["sensor_fov_presets"];
			if (PresetsNode.is_seq())
			{
				Cfg.SensorFovPresets.Empty();
				for (ryml::ConstNodeRef Val : PresetsNode)
				{
					if (Val.has_val())
					{
						Cfg.SensorFovPresets.Add(YamlFloatVal(Val));
					}
				}
			}
		}

		// -------------------------------------------------------------------
		// sensor_modes: per-waveband simulation parameters (Phase 11)
		// Overwrites the defaults set above with YAML values where present.
		// -------------------------------------------------------------------
		if (Root.has_child("sensor_modes"))
		{
			ryml::ConstNodeRef ModesNode = Root["sensor_modes"];

			auto ParseMode = [&](c4::csubstr Key, ESensorMode M)
			{
				if (!ModesNode.has_child(Key)) return;
				ryml::ConstNodeRef ModeNode = ModesNode[Key];

				FSensorModeConfig& MC = Cfg.SensorModeConfigs.FindOrAdd(M);
				YamlFloat(ModeNode, "noise_netd",          MC.NETD);
				YamlFloat(ModeNode, "fixed_pattern_noise",  MC.FixedPatternNoise);
				YamlFloat(ModeNode, "vignetting",           MC.Vignetting);
				YamlBool (ModeNode, "scan_lines",           MC.bScanLines);
				YamlFloat(ModeNode, "scan_line_strength",   MC.ScanLineStrength);
				YamlFloat(ModeNode, "ir_extinction_coeff",  MC.IRExtinctionCoeff);
				YamlFloat(ModeNode, "atmospheric_visibility_m", MC.AtmosphericVisibilityM);
				YamlFloat(ModeNode, "atmosphere_strength",   MC.AtmosphereStrength);
				YamlFloat(ModeNode, "color_temperature_k",   MC.ColorTemperatureK);
				YamlFloat(ModeNode, "contrast",              MC.Contrast);
				YamlFloat(ModeNode, "brightness_bias",       MC.BrightnessBias);
				{
					int32 BlurVal = MC.BlurRadius;
					if (YamlInt(ModeNode, "blur_radius", BlurVal))
					{
						MC.BlurRadius = FMath::Max(0, BlurVal);
					}
				}
				// Phase 16 fields
				YamlBool (ModeNode, "agc_enabled",          MC.bAGCEnabled);
				YamlFloat(ModeNode, "agc_low_percentile",   MC.AGCLowPercentile);
				YamlFloat(ModeNode, "agc_high_percentile",  MC.AGCHighPercentile);
				YamlFloat(ModeNode, "agc_manual_level",     MC.AGCManualLevel);
				YamlFloat(ModeNode, "agc_manual_gain",      MC.AGCManualGain);
				{
					int32 QBits = MC.QuantizationBits;
					if (YamlInt(ModeNode, "quantization_bits", QBits))
						MC.QuantizationBits = FMath::Clamp(QBits, 1, 14);
				}
				YamlBool (ModeNode, "quantization_dither",  MC.bQuantizationDither);
				{
					int32 DefCount = MC.DefectPixelCount;
					if (YamlInt(ModeNode, "defect_pixel_count", DefCount))
						MC.DefectPixelCount = FMath::Max(0, DefCount);
				}
				YamlFloat(ModeNode, "defect_hot_ratio",     MC.DefectHotRatio);
				{
					int32 DefSeed = MC.DefectSeed;
					if (YamlInt(ModeNode, "defect_seed", DefSeed))
						MC.DefectSeed = DefSeed;
				}
				YamlFloat(ModeNode, "gaussian_sigma",       MC.GaussianSigma);
				YamlFloat(ModeNode, "ac_banding_amplitude",  MC.ACBandingAmplitude);
				YamlFloat(ModeNode, "ac_banding_frequency",  MC.ACBandingFrequency);
				YamlBool (ModeNode, "ir_pointer_enabled",    MC.bIRPointerEnabled);
				YamlFloat(ModeNode, "ir_pointer_x",          MC.IRPointerX);
				YamlFloat(ModeNode, "ir_pointer_y",          MC.IRPointerY);
				YamlFloat(ModeNode, "ir_pointer_radius",     MC.IRPointerRadius);
				YamlFloat(ModeNode, "ir_pointer_intensity",  MC.IRPointerIntensity);
				// Phase 16 Sprint 2
				YamlBool (ModeNode, "thermal_drift_enabled",    MC.bThermalDriftEnabled);
				YamlFloat(ModeNode, "thermal_drift_rate",       MC.ThermalDriftRate);
				YamlFloat(ModeNode, "nuc_interval_sec",         MC.NUCIntervalSec);
				{
					int32 LagVal = MC.AGCLagFrames;
					if (YamlInt(ModeNode, "agc_lag_frames", LagVal))
						MC.AGCLagFrames = FMath::Clamp(LagVal, 0, 10);
				}
				YamlFloat(ModeNode, "rolling_shutter_strength", MC.RollingShutterStrength);
				YamlFloat(ModeNode, "vibration_amplitude",      MC.VibrationAmplitude);
				YamlFloat(ModeNode, "gain_jitter",              MC.GainJitter);
				YamlFloat(ModeNode, "offset_jitter",            MC.OffsetJitter);
				YamlFloat(ModeNode, "sun_glint_intensity",      MC.SunGlintIntensity);
				YamlFloat(ModeNode, "sun_glint_threshold",      MC.SunGlintThreshold);
				YamlFloat(ModeNode, "sun_glint_spread",         MC.SunGlintSpread);
			};

			ParseMode("eo",  ESensorMode::EO);
			ParseMode("ir",  ESensorMode::IR);
			ParseMode("nvg", ESensorMode::NVG);
		}

		// Optional user-defined sensor quality profiles.
		if (Root.has_child("sensor_quality_profiles"))
		{
			ryml::ConstNodeRef ProfilesNode = Root["sensor_quality_profiles"];
			if (ProfilesNode.is_map())
			{
				for (ryml::ConstNodeRef ProfileChild : ProfilesNode)
				{
					const FString PresetKey = NormalizeQualityPreset(RymlToFString(ProfileChild.key()));
					if (!ProfileChild.is_map()) continue;

					FSensorQualityConfig Profile = Cfg.SensorQualityProfiles.FindRef(TEXT("medium"));
					YamlFloat(ProfileChild, "noise_scale",      Profile.NoiseScale);
					YamlFloat(ProfileChild, "vignetting_scale", Profile.VignettingScale);
					YamlFloat(ProfileChild, "scan_line_scale",  Profile.ScanLineScale);
					YamlFloat(ProfileChild, "atmosphere_scale", Profile.AtmosphereScale);
					{
						int32 BlurVal = Profile.BlurRadius;
						if (YamlInt(ProfileChild, "blur_radius", BlurVal))
							Profile.BlurRadius = FMath::Max(0, BlurVal);
					}
					YamlFloat(ProfileChild, "contrast",         Profile.Contrast);
					YamlFloat(ProfileChild, "brightness_bias",  Profile.BrightnessBias);
					Cfg.SensorQualityProfiles.Add(PresetKey, Profile);
				}
			}
		}

		// Active sensor quality preset and optional inline overrides.
		if (Root.has_child("sensor_quality"))
		{
			ryml::ConstNodeRef QualityNode = Root["sensor_quality"];

			FString Preset;
			if (YamlString(QualityNode, "preset", Preset))
			{
				Cfg.SensorQualityPreset = NormalizeQualityPreset(Preset);
			}
			ResolveActiveSensorQuality(Cfg);

			YamlFloat(QualityNode, "noise_scale",      Cfg.ActiveSensorQuality.NoiseScale);
			YamlFloat(QualityNode, "vignetting_scale",  Cfg.ActiveSensorQuality.VignettingScale);
			YamlFloat(QualityNode, "scan_line_scale",   Cfg.ActiveSensorQuality.ScanLineScale);
			YamlFloat(QualityNode, "atmosphere_scale",  Cfg.ActiveSensorQuality.AtmosphereScale);
			{
				int32 BlurVal = Cfg.ActiveSensorQuality.BlurRadius;
				if (YamlInt(QualityNode, "blur_radius", BlurVal))
					Cfg.ActiveSensorQuality.BlurRadius = FMath::Max(0, BlurVal);
			}
			YamlFloat(QualityNode, "contrast",         Cfg.ActiveSensorQuality.Contrast);
			YamlFloat(QualityNode, "brightness_bias",  Cfg.ActiveSensorQuality.BrightnessBias);
			YamlFloat(QualityNode, "gaussian_sigma_scale", Cfg.ActiveSensorQuality.GaussianSigmaScale);
		}
		else
		{
			ResolveActiveSensorQuality(Cfg);
		}

		// Optional multi-stream output views.
		if (Root.has_child("output_views"))
		{
			ryml::ConstNodeRef ViewsNode = Root["output_views"];
			if (ViewsNode.is_seq())
			{
				Cfg.OutputViews.Reset();
				int32 DefaultViewId = 0;
				for (ryml::ConstNodeRef ViewNode : ViewsNode)
				{
					if (!ViewNode.is_map()) continue;

					FCamSimConfig::FOutputViewConfig ViewCfg;
					ViewCfg.ViewId = DefaultViewId++;
					ViewCfg.MulticastAddr = Cfg.MulticastAddr;
					ViewCfg.MulticastPort = Cfg.MulticastPort;
					ViewCfg.VideoBitrate = Cfg.VideoBitrate;
					ViewCfg.H264Preset = Cfg.H264Preset;
					ViewCfg.H264Tune = Cfg.H264Tune;
					ViewCfg.HFovDeg = 0.0f;

					YamlInt   (ViewNode, "view_id",        ViewCfg.ViewId);
					YamlBool  (ViewNode, "enabled",        ViewCfg.bEnabled);
					YamlString(ViewNode, "multicast_addr", ViewCfg.MulticastAddr);
					YamlInt   (ViewNode, "multicast_port", ViewCfg.MulticastPort);
					YamlInt   (ViewNode, "video_bitrate",  ViewCfg.VideoBitrate);
					YamlString(ViewNode, "h264_preset",    ViewCfg.H264Preset);
					YamlString(ViewNode, "h264_tune",      ViewCfg.H264Tune);
					YamlFloat (ViewNode, "hfov_deg",       ViewCfg.HFovDeg);

					Cfg.OutputViews.Add(ViewCfg);
				}
			}
		}

		// Optional ground-truth sidecar output.
		if (Root.has_child("ground_truth"))
		{
			ryml::ConstNodeRef GTNode = Root["ground_truth"];
			YamlBool  (GTNode, "enabled",         Cfg.GroundTruth.bEnabled);
			YamlString(GTNode, "output_path",     Cfg.GroundTruth.OutputPath);
			{
				int32 IntervalVal = Cfg.GroundTruth.IntervalFrames;
				if (YamlInt(GTNode, "interval_frames", IntervalVal))
					Cfg.GroundTruth.IntervalFrames = FMath::Max(1, IntervalVal);
			}
		}
		YamlBool  (Root, "ground_truth_enabled", Cfg.GroundTruth.bEnabled);
		YamlString(Root, "ground_truth_path",    Cfg.GroundTruth.OutputPath);
		{
			int32 GTInterval = Cfg.GroundTruth.IntervalFrames;
			if (YamlInt(Root, "ground_truth_interval_frames", GTInterval))
			{
				Cfg.GroundTruth.IntervalFrames = FMath::Max(1, GTInterval);
			}
		}

		// ML Training Data Generation (Phase 17).
		if (Root.has_child("ml_training"))
		{
			ryml::ConstNodeRef MLNode = Root["ml_training"];
			YamlBool  (MLNode, "enabled",                  Cfg.MLTraining.bEnabled);
			YamlString(MLNode, "output_dir",               Cfg.MLTraining.OutputDir);
			YamlBool  (MLNode, "depth_map",                Cfg.MLTraining.bDepthMap);
			YamlBool  (MLNode, "bounding_boxes",           Cfg.MLTraining.bBoundingBoxes);
			YamlBool  (MLNode, "coco_export",              Cfg.MLTraining.bCocoExport);
			YamlBool  (MLNode, "voc_export",               Cfg.MLTraining.bVocExport);
			YamlFloat (MLNode, "depth_far_plane_m",        Cfg.MLTraining.DepthFarPlaneM);
			{
				int32 Interval = Cfg.MLTraining.AnnotationIntervalFrames;
				if (YamlInt(MLNode, "annotation_interval_frames", Interval))
					Cfg.MLTraining.AnnotationIntervalFrames = FMath::Max(1, Interval);
			}
		}

		// Entity runtime scale controls (LOD/culling/update throttling).
		if (Root.has_child("entity_scale"))
		{
			ryml::ConstNodeRef ScaleNode = Root["entity_scale"];
			YamlFloat(ScaleNode, "max_draw_distance_m",      Cfg.EntityScale.MaxDrawDistanceM);
			YamlFloat(ScaleNode, "tick_rate_hz",              Cfg.EntityScale.TickRateHz);
			YamlFloat(ScaleNode, "default_max_update_rate_hz", Cfg.EntityScale.DefaultMaxUpdateRateHz);

			if (ScaleNode.has_child("max_update_rate_hz_overrides"))
			{
				ryml::ConstNodeRef OverridesNode = ScaleNode["max_update_rate_hz_overrides"];
				if (OverridesNode.is_map())
				{
					for (ryml::ConstNodeRef Override : OverridesNode)
					{
						FString KeyStr = RymlToFString(Override.key());
						int32 EntityId = FCString::Atoi(*KeyStr);
						if (Override.has_val())
						{
							float RateHz = YamlFloatVal(Override);
							Cfg.EntityScale.MaxUpdateRateHzOverrides.Add(EntityId, RateHz);
						}
					}
				}
			}
		}

		// Legacy flat keys (kept for backwards compatibility).
		YamlFloat(Root, "entity_max_draw_distance_m",         Cfg.EntityScale.MaxDrawDistanceM);
		YamlFloat(Root, "entity_tick_rate_hz",                Cfg.EntityScale.TickRateHz);
		YamlFloat(Root, "entity_default_max_update_rate_hz",  Cfg.EntityScale.DefaultMaxUpdateRateHz);

		// Optional scenario entity orchestration block.
		if (Root.has_child("scenario"))
		{
			ryml::ConstNodeRef ScenarioNode = Root["scenario"];
			YamlBool (ScenarioNode, "enabled",    Cfg.bScenarioEnabled);
			YamlFloat(ScenarioNode, "time_scale", Cfg.ScenarioTimeScale);
			YamlFloat(ScenarioNode, "start_hour", Cfg.ScenarioStartHour);

			if (ScenarioNode.has_child("entities"))
			{
				ryml::ConstNodeRef EntitiesNode = ScenarioNode["entities"];
				if (EntitiesNode.is_seq())
				{
					Cfg.ScenarioEntities.Reset();
					for (ryml::ConstNodeRef EntityNode : EntitiesNode)
					{
						if (!EntityNode.is_map()) continue;

						FCamSimConfig::FScenarioEntityConfig Spec;
						YamlInt   (EntityNode, "entity_id",       Spec.EntityId);
						YamlInt   (EntityNode, "entity_type",     Spec.EntityType);
						YamlDouble(EntityNode, "start_latitude",  Spec.StartLatitude);
						YamlDouble(EntityNode, "start_longitude", Spec.StartLongitude);
						YamlDouble(EntityNode, "start_altitude",  Spec.StartAltitude);
						YamlFloat (EntityNode, "start_yaw",       Spec.StartYaw);
						YamlFloat (EntityNode, "start_pitch",     Spec.StartPitch);
						YamlFloat (EntityNode, "start_roll",      Spec.StartRoll);
						YamlFloat (EntityNode, "spawn_time_sec",  Spec.SpawnTimeSec);
						YamlFloat (EntityNode, "despawn_time_sec", Spec.DespawnTimeSec);
						YamlFloat (EntityNode, "update_rate_hz",  Spec.UpdateRateHz);
						YamlFloat (EntityNode, "north_rate_mps",  Spec.NorthRateMps);
						YamlFloat (EntityNode, "east_rate_mps",   Spec.EastRateMps);
						YamlFloat (EntityNode, "up_rate_mps",     Spec.UpRateMps);
						YamlFloat (EntityNode, "yaw_rate_dps",    Spec.YawRateDegPerSec);
						YamlFloat (EntityNode, "pitch_rate_dps",  Spec.PitchRateDegPerSec);
						YamlFloat (EntityNode, "roll_rate_dps",   Spec.RollRateDegPerSec);

						// Phase 23A: Waypoint trajectories
						YamlBool (EntityNode, "loop_waypoints", Spec.bLoopWaypoints);
						YamlFloat(EntityNode, "base_speed_mps", Spec.BaseSpeedMps);
						if (EntityNode.has_child("waypoints"))
						{
							ryml::ConstNodeRef WpNode = EntityNode["waypoints"];
							if (WpNode.is_seq())
							{
								for (ryml::ConstNodeRef Wp : WpNode)
								{
									if (!Wp.is_map()) continue;
									FCamSimConfig::FWaypointConfig WpCfg;
									YamlDouble(Wp, "lat",         WpCfg.Latitude);
									YamlDouble(Wp, "lon",         WpCfg.Longitude);
									YamlFloat (Wp, "alt",         WpCfg.Altitude);
									YamlFloat (Wp, "speed_mps",   WpCfg.SpeedMps);
									YamlFloat (Wp, "heading_deg", WpCfg.HeadingDeg);
									YamlFloat (Wp, "pause_sec",   WpCfg.PauseSec);
									Spec.Waypoints.Add(WpCfg);
								}
							}
						}

						// Phase 23C: Activity schedule
						YamlString(EntityNode, "activity_profile", Spec.ActivityProfile);
						if (EntityNode.has_child("activity_schedule"))
						{
							ryml::ConstNodeRef ActNode = EntityNode["activity_schedule"];
							if (ActNode.is_seq())
							{
								for (ryml::ConstNodeRef Act : ActNode)
								{
									if (!Act.is_map()) continue;
									FCamSimConfig::FActivityScheduleEntry Entry;
									YamlFloat(Act, "start_hour",      Entry.StartHour);
									YamlFloat(Act, "end_hour",        Entry.EndHour);
									YamlInt  (Act, "waypoint_path",   Entry.WaypointPathIndex);
									YamlFloat(Act, "spawn_jitter_sec", Entry.SpawnJitterSec);
									YamlFloat(Act, "path_deviation_m", Entry.PathDeviationM);
									Spec.ActivitySchedule.Add(Entry);
								}
							}
						}

						Cfg.ScenarioEntities.Add(Spec);
					}
				}
			}

			// Phase 23B: Scenario triggers
			if (ScenarioNode.has_child("triggers"))
			{
				ryml::ConstNodeRef TriggersNode = ScenarioNode["triggers"];
				if (TriggersNode.is_seq())
				{
					Cfg.ScenarioTriggers.Reset();
					for (ryml::ConstNodeRef TrigNode : TriggersNode)
					{
						if (!TrigNode.is_map()) continue;
						FCamSimConfig::FScenarioTrigger Trig;
						YamlString(TrigNode, "name", Trig.Name);
						YamlBool  (TrigNode, "repeat", Trig.bRepeat);
						YamlFloat (TrigNode, "cooldown_sec", Trig.CooldownSec);

						if (TrigNode.has_child("condition"))
						{
							ryml::ConstNodeRef CondNode = TrigNode["condition"];
							FString CondType;
							YamlString(CondNode, "type", CondType);
							if (CondType == TEXT("time_sec"))       Trig.Condition.Type = FCamSimConfig::EScenarioConditionType::TimeSec;
							else if (CondType == TEXT("entity_in_area"))  Trig.Condition.Type = FCamSimConfig::EScenarioConditionType::EntityInArea;
							else if (CondType == TEXT("entity_proximity")) Trig.Condition.Type = FCamSimConfig::EScenarioConditionType::EntityProximity;
							else if (CondType == TEXT("frame_count"))     Trig.Condition.Type = FCamSimConfig::EScenarioConditionType::FrameCount;

							YamlFloat (CondNode, "time_sec",        Trig.Condition.TimeSec);
							YamlInt   (CondNode, "entity_id_a",     Trig.Condition.EntityIdA);
							YamlInt   (CondNode, "entity_id_b",     Trig.Condition.EntityIdB);
							YamlDouble(CondNode, "area_latitude",   Trig.Condition.AreaLatitude);
							YamlDouble(CondNode, "area_longitude",  Trig.Condition.AreaLongitude);
							YamlFloat (CondNode, "radius_m",        Trig.Condition.RadiusM);
							YamlInt   (CondNode, "frame_threshold", Trig.Condition.FrameThreshold);
						}

						if (TrigNode.has_child("action"))
						{
							ryml::ConstNodeRef ActNode = TrigNode["action"];
							FString ActType;
							YamlString(ActNode, "type", ActType);
							if (ActType == TEXT("spawn_entity"))       Trig.Action.Type = FCamSimConfig::EScenarioActionType::SpawnEntity;
							else if (ActType == TEXT("despawn_entity")) Trig.Action.Type = FCamSimConfig::EScenarioActionType::DespawnEntity;
							else if (ActType == TEXT("set_damage_state")) Trig.Action.Type = FCamSimConfig::EScenarioActionType::SetDamageState;
							else if (ActType == TEXT("change_speed"))   Trig.Action.Type = FCamSimConfig::EScenarioActionType::ChangeSpeed;
							else if (ActType == TEXT("log_message"))    Trig.Action.Type = FCamSimConfig::EScenarioActionType::LogMessage;

							YamlInt   (ActNode, "target_entity_id",   Trig.Action.TargetEntityId);
							YamlInt   (ActNode, "target_entity_type", Trig.Action.TargetEntityType);
							YamlDouble(ActNode, "spawn_latitude",     Trig.Action.SpawnLatitude);
							YamlDouble(ActNode, "spawn_longitude",    Trig.Action.SpawnLongitude);
							YamlFloat (ActNode, "spawn_altitude",     Trig.Action.SpawnAltitude);
							{
								int32 DmgState = 0;
								if (YamlInt(ActNode, "damage_state", DmgState))
									Trig.Action.DamageState = static_cast<uint8>(FMath::Clamp(DmgState, 0, 2));
							}
							YamlFloat (ActNode, "new_speed_mps",     Trig.Action.NewSpeedMps);
							YamlString(ActNode, "message",           Trig.Action.Message);
						}

						Cfg.ScenarioTriggers.Add(Trig);
					}
				}
			}
		}

		// Legacy flat keys.
		YamlBool (Root, "scenario_enabled",    Cfg.bScenarioEnabled);
		YamlFloat(Root, "scenario_time_scale", Cfg.ScenarioTimeScale);

		// Phase 22C: Damage transition FX
		if (Root.has_child("damage_transition"))
		{
			ryml::ConstNodeRef DmgNode = Root["damage_transition"];
			YamlBool (DmgNode, "enabled",           Cfg.DamageTransition.bDamageTransitionFX);
			YamlBool (DmgNode, "gradual",            Cfg.DamageTransition.bGradualDamage);
			YamlFloat(DmgNode, "interpolation_sec",  Cfg.DamageTransition.DamageInterpolationSec);
			YamlFloat(DmgNode, "scorch_darkening",   Cfg.DamageTransition.DamageScorchDarkening);
		}

		// Security metadata (MISB ST 0102, Phase 12A)
		if (Root.has_child("security_metadata"))
		{
			ryml::ConstNodeRef SecNode = Root["security_metadata"];
			YamlString(SecNode, "classification",       Cfg.SecurityMetadata.Classification);
			YamlString(SecNode, "classifying_country",   Cfg.SecurityMetadata.ClassifyingCountry);
			YamlString(SecNode, "object_country_codes",  Cfg.SecurityMetadata.ObjectCountryCodes);
			YamlString(SecNode, "caveats",               Cfg.SecurityMetadata.Caveats);
			YamlString(SecNode, "releasing_instructions", Cfg.SecurityMetadata.ReleasingInstructions);
		}

		// Video codec (Phase 12B)
		YamlString(Root, "video_codec", Cfg.VideoCodec);

		// Prometheus metrics (Phase 12D)
		YamlString(Root, "prometheus_metrics_path", Cfg.PrometheusMetricsPath);

		// Recording & playback (Phase 12E)
		if (Root.has_child("recording"))
		{
			ryml::ConstNodeRef RecNode = Root["recording"];
			YamlString(RecNode, "cigi_record_path",   Cfg.Recording.CigiRecordPath);
			YamlString(RecNode, "video_record_path",  Cfg.Recording.VideoRecordPath);
			YamlString(RecNode, "cigi_playback_path", Cfg.Recording.CigiPlaybackPath);
		}

		// Optical realism (Phase 15)
		if (Root.has_child("optical_realism"))
		{
			ryml::ConstNodeRef OptNode = Root["optical_realism"];
			YamlBool (OptNode, "enabled",                        Cfg.OpticalRealism.bEnabled);
			YamlBool (OptNode, "motion_blur",                    Cfg.OpticalRealism.bMotionBlur);
			YamlFloat(OptNode, "motion_blur_amount",             Cfg.OpticalRealism.MotionBlurAmount);
			YamlInt  (OptNode, "motion_blur_max",                Cfg.OpticalRealism.MotionBlurMax);
			YamlBool (OptNode, "lens_distortion",                Cfg.OpticalRealism.bLensDistortion);
			YamlFloat(OptNode, "distortion_k1",                  Cfg.OpticalRealism.DistortionK1);
			YamlFloat(OptNode, "distortion_k2",                  Cfg.OpticalRealism.DistortionK2);
			YamlBool (OptNode, "bloom",                          Cfg.OpticalRealism.bBloom);
			YamlFloat(OptNode, "bloom_intensity",                Cfg.OpticalRealism.BloomIntensity);
			YamlFloat(OptNode, "bloom_threshold",                Cfg.OpticalRealism.BloomThreshold);
			YamlBool (OptNode, "chromatic_aberration",           Cfg.OpticalRealism.bChromaticAberration);
			YamlFloat(OptNode, "chromatic_aberration_intensity", Cfg.OpticalRealism.ChromaticAberrationIntensity);
			YamlBool (OptNode, "depth_of_field",                 Cfg.OpticalRealism.bDepthOfField);
			YamlFloat(OptNode, "focal_distance",                 Cfg.OpticalRealism.FocalDistance);
			YamlFloat(OptNode, "aperture_fstop",                 Cfg.OpticalRealism.ApertureFStop);
			YamlFloat(OptNode, "sensor_width",                   Cfg.OpticalRealism.SensorWidth);
			YamlBool (OptNode, "lens_flare",                     Cfg.OpticalRealism.bLensFlare);
			YamlFloat(OptNode, "lens_flare_intensity",           Cfg.OpticalRealism.LensFlareIntensity);
			YamlFloat(OptNode, "lens_flare_bokeh_size",          Cfg.OpticalRealism.LensFlareBokehSize);
			YamlFloat(OptNode, "lens_flare_threshold",           Cfg.OpticalRealism.LensFlareThreshold);
		}

		// Phase 18: weather, atmosphere & particle effects
		if (Root.has_child("phase18"))
		{
			ryml::ConstNodeRef P18 = Root["phase18"];
			YamlBool (P18, "second_fog",                Cfg.Phase18.bSecondFog);
			YamlFloat(P18, "fog_density",               Cfg.Phase18.FogDensity);
			YamlFloat(P18, "fog_height_falloff",        Cfg.Phase18.FogHeightFalloff);
			YamlBool (P18, "precipitation",             Cfg.Phase18.bPrecipitation);
			YamlFloat(P18, "rain_intensity",            Cfg.Phase18.RainIntensity);
			YamlFloat(P18, "snow_intensity",            Cfg.Phase18.SnowIntensity);
			YamlBool (P18, "god_rays",                  Cfg.Phase18.bGodRays);
			YamlFloat(P18, "god_ray_intensity",         Cfg.Phase18.GodRayIntensity);
			YamlBool (P18, "atmospheric_scattering",    Cfg.Phase18.bAtmosphericScattering);
			YamlFloat(P18, "rayleigh_scattering",       Cfg.Phase18.RayleighScattering);
			YamlFloat(P18, "mie_scattering",            Cfg.Phase18.MieScattering);
			YamlBool (P18, "dynamic_ir_extinction",     Cfg.Phase18.bDynamicIRExtinction);
			YamlFloat(P18, "visibility_range_m",        Cfg.Phase18.VisibilityRangeM);
			// 18A/18B
			YamlBool (P18, "volumetric_clouds",          Cfg.Phase18.bVolumetricClouds);
			YamlFloat(P18, "cloud_shadow_strength",       Cfg.Phase18.CloudShadowStrength);
			// 18L Weather zones -- array of {id, lat, lon, radius_m}
			YamlBool (P18, "weather_zones",               Cfg.Phase18.bWeatherZones);
			if (P18.has_child("zone_positions") && P18["zone_positions"].is_seq())
			{
				for (const ryml::ConstNodeRef& ZNode : P18["zone_positions"])
				{
					if (Cfg.Phase18.WeatherZoneConfigs.Num() >= 16)
					{
						UE_LOG(LogCamSim, Warning, TEXT("zone_positions: exceeded 16-zone CIGI limit; extra entries ignored"));
						break;
					}
					FCamSimConfig::FPhase18Config::FWeatherZoneConfig ZCfg;
					YamlInt   (ZNode, "id",       ZCfg.ZoneID);
					YamlDouble(ZNode, "lat",      ZCfg.LatDeg);
					YamlDouble(ZNode, "lon",      ZCfg.LonDeg);
					YamlFloat (ZNode, "radius_m", ZCfg.RadiusM);
					Cfg.Phase18.WeatherZoneConfigs.Add(ZCfg);
				}
			}
			// 18F/G/H Niagara
			YamlString(P18, "niagara_rotor_wash",         Cfg.Phase18.NiagaraRotorWash);
			YamlString(P18, "niagara_smoke",               Cfg.Phase18.NiagaraSmoke);
			YamlString(P18, "niagara_fire",                Cfg.Phase18.NiagaraFire);
			YamlString(P18, "niagara_contrail",            Cfg.Phase18.NiagaraContrail);
			YamlFloat (P18, "contrail_alt_m",              Cfg.Phase18.ContrailAltM);
			YamlFloat (P18, "contrail_speed_ms",           Cfg.Phase18.ContrailSpeedMs);
			YamlInt   (P18, "smoke_component_id",          Cfg.Phase18.SmokeComponentID);
			YamlInt   (P18, "fire_component_id",           Cfg.Phase18.FireComponentID);
			// 18I
			YamlString(P18, "crater_decal_material",       Cfg.Phase18.CraterDecalMaterial);
			YamlInt   (P18, "crater_impact_component_id",  Cfg.Phase18.CraterImpactComponentID);
			YamlInt   (P18, "max_craters",                 Cfg.Phase18.MaxCraters);
			YamlFloat (P18, "crater_default_radius_m",     Cfg.Phase18.CraterDefaultRadiusM);
		}

		if (Root.has_child("rendering_quality"))
		{
			ryml::ConstNodeRef RQNode = Root["rendering_quality"];
			FRenderingQualityConfig& RQ = Cfg.RenderingQuality;
			YamlBool (RQNode, "entity_shadows",         RQ.bEntityShadows);
			YamlBool (RQNode, "contact_shadows",        RQ.bContactShadows);
			YamlFloat(RQNode, "contact_shadow_length",  RQ.ContactShadowLength);
			YamlFloat(RQNode, "ao_intensity",           RQ.AOIntensity);
			YamlFloat(RQNode, "ao_radius",              RQ.AORadius);
			YamlBool (RQNode, "rt_enabled",             RQ.bRayTracingEnabled);
			YamlBool (RQNode, "rt_reflections",         RQ.bRayTracedReflections);
			YamlFloat(RQNode, "shadow_distance_scale",  RQ.ShadowDistanceScale);
			YamlInt  (RQNode, "vsm_resolution_bias",    RQ.VSMResolutionBias);
			YamlInt  (RQNode, "vsm_max_physical_pages", RQ.VSMMaxPhysicalPages);
			YamlInt  (RQNode, "tsr_screen_percentage",  RQ.TSRScreenPercentage);
		}

		// Phase 27: Performance
		if (Root.has_child("performance"))
		{
			ryml::ConstNodeRef PerfNode = Root["performance"];
			FPerformanceConfig& Perf = Cfg.Performance;
			YamlBool (PerfNode, "track_frame_drops_by_category",        Perf.bTrackFrameDropsByCategory);
			YamlBool (PerfNode, "hot_reload_config",                    Perf.bHotReloadConfig);
			YamlFloat(PerfNode, "hot_reload_poll_interval_sec",         Perf.HotReloadPollIntervalSec);
			YamlFloat(PerfNode, "tile_prefetch_slew_threshold_deg_per_sec", Perf.TilePrefetchSlewThresholdDegPerSec);
			YamlFloat(PerfNode, "tile_prefetch_fov_boost",              Perf.TilePrefetchFovBoost);
			YamlInt  (PerfNode, "tile_prefetch_boost_frames",           Perf.TilePrefetchBoostFrames);
			YamlFloat(PerfNode, "render_frame_rate_hz",                 Perf.RenderFrameRateHz);
			YamlFloat(PerfNode, "output_frame_rate_hz",                 Perf.OutputFrameRateHz);
			YamlInt  (PerfNode, "texture_pool_budget_mb",               Perf.TexturePoolBudgetMB);
			YamlBool (PerfNode, "gpu_sensor_effects",                   Perf.bGpuSensorEffects);
			YamlString(PerfNode, "gpu_sensor_material_path",            Perf.GpuSensorMaterialPath);
			YamlString(PerfNode, "gpu_sensor_mpc_path",                 Perf.GpuSensorMpcPath);
			YamlBool (PerfNode, "adaptive_sse",                         Perf.bAdaptiveSSE);
			YamlFloat(PerfNode, "adaptive_sse_min",                     Perf.AdaptiveSSEMin);
			YamlFloat(PerfNode, "adaptive_sse_max",                     Perf.AdaptiveSSEMax);
		}

		if (Root.has_child("phase19"))
		{
			ryml::ConstNodeRef P19 = Root["phase19"];
			YamlBool  (P19, "ocean_enabled",            Cfg.Phase19.bOceanEnabled);
			YamlInt   (P19, "beaufort_state",            Cfg.Phase19.BeaufortState);
			YamlFloat (P19, "wave_amplitude_scale",      Cfg.Phase19.WaveAmplitudeScale);
			YamlFloat (P19, "wave_frequency_scale",      Cfg.Phase19.WaveFrequencyScale);
			YamlFloat (P19, "wave_choppiness",           Cfg.Phase19.WaveChoppiness);
			YamlString(P19, "ocean_material_path",       Cfg.Phase19.OceanMaterialPath);
			YamlBool  (P19, "vessel_wakes_enabled",      Cfg.Phase19.bVesselWakesEnabled);
			YamlString(P19, "niagara_vessel_wake",       Cfg.Phase19.NiagaraVesselWake);
			YamlFloat (P19, "wake_fade_time",            Cfg.Phase19.WakeFadeTime);
			YamlBool  (P19, "vessel_motion_enabled",     Cfg.Phase19.bVesselMotionEnabled);
			YamlFloat (P19, "vessel_motion_scale",       Cfg.Phase19.VesselMotionScale);
			YamlBool  (P19, "ocean_reflections_enabled", Cfg.Phase19.bOceanReflectionsEnabled);
			YamlFloat (P19, "ssr_intensity",             Cfg.Phase19.SSRIntensity);
			YamlFloat (P19, "reflection_capture_radius", Cfg.Phase19.ReflectionCaptureRadius);
		}

		// Cesium backend: ion server, terrain source, imagery overlay
		if (Root.has_child("cesium"))
		{
			ryml::ConstNodeRef Cs = Root["cesium"];
			YamlString(Cs, "ion_portal_url", Cfg.CesiumBackend.IonPortalUrl);
			YamlString(Cs, "ion_api_url",    Cfg.CesiumBackend.IonApiUrl);
			YamlString(Cs, "ion_token",      Cfg.CesiumBackend.IonToken);

			if (Cs.has_child("terrain"))
			{
				ryml::ConstNodeRef Tr = Cs["terrain"];
				YamlString(Tr, "source",       Cfg.CesiumBackend.Terrain.Source);
				YamlInt   (Tr, "ion_asset_id", Cfg.CesiumBackend.Terrain.IonAssetId);
				YamlString(Tr, "url",          Cfg.CesiumBackend.Terrain.Url);
			}

			if (Cs.has_child("imagery"))
			{
				ryml::ConstNodeRef Im = Cs["imagery"];
				YamlString(Im, "source",          Cfg.CesiumBackend.Imagery.Source);
				YamlInt   (Im, "ion_asset_id",    Cfg.CesiumBackend.Imagery.IonAssetId);
				YamlString(Im, "wms_url",         Cfg.CesiumBackend.Imagery.WmsUrl);
				YamlString(Im, "wms_layers",      Cfg.CesiumBackend.Imagery.WmsLayers);
				YamlInt   (Im, "wms_tile_width",  Cfg.CesiumBackend.Imagery.WmsTileWidth);
				YamlInt   (Im, "wms_tile_height", Cfg.CesiumBackend.Imagery.WmsTileHeight);
				YamlDouble(Im, "maximum_screen_space_error",     Cfg.CesiumBackend.Imagery.MaximumScreenSpaceError);
				YamlInt   (Im, "maximum_texture_size",           Cfg.CesiumBackend.Imagery.MaximumTextureSize);
				YamlInt   (Im, "maximum_simultaneous_tile_loads", Cfg.CesiumBackend.Imagery.MaximumSimultaneousTileLoads);
			}
		}

		// Phase 20: overlay HUD/OSD
		if (Root.has_child("overlay"))
		{
			ryml::ConstNodeRef Ov = Root["overlay"];

			// Preset seeds config defaults before individual keys override
			{
				FString PresetName;
				if (YamlString(Ov, "preset", PresetName) && !PresetName.IsEmpty())
					FHudOverlay::LoadPreset(PresetName, Cfg.OverlayConfig);
			}

			YamlBool  (Ov, "enabled",         Cfg.OverlayConfig.bEnabled);
			YamlBool  (Ov, "crosshair",       Cfg.OverlayConfig.ElementCrosshair.bEnabled);
			YamlBool  (Ov, "az_el_readout",   Cfg.OverlayConfig.ElementAzEl.bEnabled);
			YamlBool  (Ov, "fov_indicator",   Cfg.OverlayConfig.ElementFov.bEnabled);
			YamlBool  (Ov, "slant_range",     Cfg.OverlayConfig.ElementSlantRange.bEnabled);
			YamlBool  (Ov, "timestamp",       Cfg.OverlayConfig.ElementTimestamp.bEnabled);
			YamlBool  (Ov, "class_banner",    Cfg.OverlayConfig.ElementClassBanner.bEnabled);
			YamlBool  (Ov, "compass_rose",    Cfg.OverlayConfig.ElementCompassRose.bEnabled);
			{
				FString LabelText;
				if (YamlString(Ov, "platform_label", LabelText))
				{
					Cfg.OverlayConfig.PlatformLabelText = LabelText;
					Cfg.OverlayConfig.ElementPlatformLabel.bEnabled = !LabelText.IsEmpty();
				}
			}
			{
				int32 V = Cfg.OverlayConfig.TextScale;
				if (YamlInt(Ov, "text_scale", V))
					Cfg.OverlayConfig.TextScale = FMath::Clamp(V, 1, 4);
			}
			{
				int32 V = Cfg.OverlayConfig.EdgeMarginPx;
				if (YamlInt(Ov, "edge_margin_px", V))
					Cfg.OverlayConfig.EdgeMarginPx = FMath::Clamp(V, 0, 100);
			}
			{
				FString StyleStr;
				if (YamlString(Ov, "crosshair_style", StyleStr))
				{
					StyleStr = StyleStr.TrimStartAndEnd().ToLower();
					if      (StyleStr == TEXT("mil_dot")) Cfg.OverlayConfig.CrosshairStyle = ECrosshairStyle::MilDot;
					else if (StyleStr == TEXT("circle"))  Cfg.OverlayConfig.CrosshairStyle = ECrosshairStyle::CircleCross;
					else                                  Cfg.OverlayConfig.CrosshairStyle = ECrosshairStyle::SimpleCross;
				}
			}
			YamlString(Ov, "classification_text", Cfg.OverlayConfig.ClassificationText);

			// Per-element color overrides (hex string "RRGGBB"; empty = sensor-mode default)
			// Helper: parse "RRGGBB" or "#RRGGBB" -> FColor(R,G,B,255); no-op if empty/invalid
			auto YamlHexColor = [&](c4::csubstr Key, FColor& OutColor)
			{
				FString Hex;
				if (!YamlString(Ov, Key, Hex) || Hex.IsEmpty()) return;
				if (Hex.StartsWith(TEXT("#"))) Hex = Hex.RightChop(1);
				if (Hex.Len() == 6)
				{
					const uint32 V = FParse::HexNumber(*Hex);
					OutColor = FColor((V >> 16) & 0xFF, (V >> 8) & 0xFF, V & 0xFF, 255);
				}
			};
			YamlHexColor("compass_rose_color",   Cfg.OverlayConfig.ElementCompassRose.Color);
			YamlHexColor("platform_label_color", Cfg.OverlayConfig.ElementPlatformLabel.Color);

			// Per-element position overrides (-1 = use default anchor)
			YamlInt(Ov, "az_el_x",          Cfg.OverlayConfig.ElementAzEl.X);
			YamlInt(Ov, "az_el_y",          Cfg.OverlayConfig.ElementAzEl.Y);
			YamlInt(Ov, "fov_x",            Cfg.OverlayConfig.ElementFov.X);
			YamlInt(Ov, "fov_y",            Cfg.OverlayConfig.ElementFov.Y);
			YamlInt(Ov, "slant_range_x",    Cfg.OverlayConfig.ElementSlantRange.X);
			YamlInt(Ov, "slant_range_y",    Cfg.OverlayConfig.ElementSlantRange.Y);
			YamlInt(Ov, "timestamp_x",      Cfg.OverlayConfig.ElementTimestamp.X);
			YamlInt(Ov, "timestamp_y",      Cfg.OverlayConfig.ElementTimestamp.Y);
			YamlInt(Ov, "compass_rose_x",   Cfg.OverlayConfig.ElementCompassRose.X);
			YamlInt(Ov, "compass_rose_y",   Cfg.OverlayConfig.ElementCompassRose.Y);
			YamlInt(Ov, "platform_label_x", Cfg.OverlayConfig.ElementPlatformLabel.X);
			YamlInt(Ov, "platform_label_y", Cfg.OverlayConfig.ElementPlatformLabel.Y);
		}

		// Phase 21: DIS protocol config
		if (Root.has_child("dis"))
		{
			ryml::ConstNodeRef D = Root["dis"];
			YamlBool  (D, "enabled",               Cfg.DIS.bEnabled);
			YamlString(D, "bind_addr",              Cfg.DIS.BindAddr);
			YamlInt   (D, "port",                   Cfg.DIS.Port);
			YamlString(D, "multicast_group",        Cfg.DIS.MulticastGroup);
			YamlInt   (D, "exercise_id",            Cfg.DIS.ExerciseId);
			YamlInt   (D, "site_id",                Cfg.DIS.SiteId);
			YamlInt   (D, "application_id",         Cfg.DIS.ApplicationId);
			YamlFloat (D, "heartbeat_timeout_sec",  Cfg.DIS.HeartbeatTimeoutSec);
			YamlInt   (D, "id_base_offset",         Cfg.DIS.IdBaseOffset);
			YamlInt   (D, "default_entity_type_id", Cfg.DIS.DefaultEntityTypeId);

			// Entity type mappings: dis.entity_type_map
			if (D.has_child("entity_type_map"))
			{
				ryml::ConstNodeRef MapNode = D["entity_type_map"];
				if (MapNode.is_map())
				{
					for (ryml::ConstNodeRef Entry : MapNode)
					{
						if (Entry.has_key() && Entry.has_val())
						{
							FString Key = RymlToFString(Entry.key());
							FString ValStr = RymlToFString(Entry.val());
							const int32 TypeId = FCString::Atoi(*ValStr);
							Cfg.DIS.EntityTypeMappings.Add(Key, static_cast<uint16>(TypeId));
						}
					}
				}
			}
		}

		// Phase 21 Sprint 2: streaming config
		if (Root.has_child("streaming"))
		{
			ryml::ConstNodeRef S = Root["streaming"];
			YamlBool  (S, "cot_enabled",       Cfg.Streaming.bCotEnabled);
			YamlString(S, "cot_addr",          Cfg.Streaming.CotAddr);
			YamlInt   (S, "cot_port",          Cfg.Streaming.CotPort);
			YamlFloat (S, "cot_interval_sec",  Cfg.Streaming.CotIntervalSec);
			YamlString(S, "cot_uid",           Cfg.Streaming.CotUid);
			YamlString(S, "cot_type",          Cfg.Streaming.CotType);
			YamlString(S, "cot_callsign",      Cfg.Streaming.CotCallsign);
			YamlBool  (S, "atak_view_enabled", Cfg.Streaming.bAtakViewEnabled);
			YamlString(S, "atak_addr",         Cfg.Streaming.AtakAddr);
			YamlInt   (S, "atak_port",         Cfg.Streaming.AtakPort);
			YamlInt   (S, "atak_bitrate",      Cfg.Streaming.AtakBitrate);
			YamlBool  (S, "rover_compat",      Cfg.Streaming.bRoverCompat);
			YamlInt   (S, "rover_video_pid",   Cfg.Streaming.RoverVideoPid);
			YamlInt   (S, "rover_klv_pid",     Cfg.Streaming.RoverKlvPid);
		}

		// Phase 21 Sprint 2: laser designator config
		if (Root.has_child("laser_designator"))
		{
			ryml::ConstNodeRef L = Root["laser_designator"];
			YamlBool  (L, "enabled",          Cfg.LaserDesignator.bEnabled);
			YamlFloat (L, "spot_x",           Cfg.LaserDesignator.SpotX);
			YamlFloat (L, "spot_y",           Cfg.LaserDesignator.SpotY);
			YamlFloat (L, "spot_radius",      Cfg.LaserDesignator.SpotRadius);
			YamlFloat (L, "spot_intensity",   Cfg.LaserDesignator.SpotIntensity);
			YamlInt   (L, "designator_code",  Cfg.LaserDesignator.DesignatorCode);
		}

		UE_LOG(LogCamSim, Log, TEXT("Loaded config from %s"), *YamlPath);
	}
	else
	{
		UE_LOG(LogCamSim, Log, TEXT("No config file found at %s - using defaults"), *YamlPath);
	}

	ApplyEnvOverrides(Cfg);
	return Cfg;
}

void FCamSimConfig::ApplyEnvOverrides(FCamSimConfig& Cfg)
{
	const FString MulticastAddrEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("CAMSIM_MULTICAST_ADDR"));
	const FString MulticastPortEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("CAMSIM_MULTICAST_PORT"));
	const bool bHasMulticastAddrEnv = !MulticastAddrEnv.IsEmpty();
	const bool bHasMulticastPortEnv = !MulticastPortEnv.IsEmpty();

	Cfg.CigiBindAddr     = GetEnv(TEXT("CAMSIM_CIGI_BIND_ADDR"),      Cfg.CigiBindAddr);
	Cfg.CigiPort         = GetEnvInt(TEXT("CAMSIM_CIGI_PORT"),        Cfg.CigiPort);
	Cfg.CigiResponseAddr = GetEnv(TEXT("CAMSIM_CIGI_RESPONSE_ADDR"),  Cfg.CigiResponseAddr);
	Cfg.CigiResponsePort = GetEnvInt(TEXT("CAMSIM_CIGI_RESPONSE_PORT"), Cfg.CigiResponsePort);
	Cfg.MulticastAddr  = GetEnv(TEXT("CAMSIM_MULTICAST_ADDR"),   Cfg.MulticastAddr);
	Cfg.MulticastPort  = GetEnvInt(TEXT("CAMSIM_MULTICAST_PORT"),Cfg.MulticastPort);
	Cfg.VideoBitrate   = GetEnvInt(TEXT("CAMSIM_VIDEO_BITRATE"),  Cfg.VideoBitrate);
	Cfg.H264Preset     = GetEnv(TEXT("CAMSIM_H264_PRESET"),      Cfg.H264Preset);
	Cfg.bSwapRBReadback = GetEnvInt(TEXT("CAMSIM_SWAP_RB_READBACK"),
		Cfg.bSwapRBReadback ? 1 : 0) != 0;
	{
		const FString ReadbackEnv = GetEnv(TEXT("CAMSIM_READBACK_FORMAT"), TEXT(""));
		if (!ReadbackEnv.IsEmpty())
		{
			Cfg.ReadbackFormat = ParseReadbackFormat(ReadbackEnv);
		}
	}
	Cfg.ReadbackReadyPolls = FMath::Max(1, GetEnvInt(TEXT("CAMSIM_READBACK_READY_POLLS"), Cfg.ReadbackReadyPolls));
	{
		const FString WatchdogPolicy = GetEnv(TEXT("CAMSIM_ENCODER_WATCHDOG_POLICY"), TEXT(""));
		if (!WatchdogPolicy.IsEmpty())
		{
			Cfg.EncoderWatchdogPolicy = ParseWatchdogPolicy(WatchdogPolicy);
		}
	}
	Cfg.EncoderWatchdogIntervalTicks = FMath::Max(30, GetEnvInt(
		TEXT("CAMSIM_ENCODER_WATCHDOG_INTERVAL_TICKS"), Cfg.EncoderWatchdogIntervalTicks));
	Cfg.bLogTileSelectionStats  = GetEnvBool(TEXT("CAMSIM_LOG_TILE_STATS"),        Cfg.bLogTileSelectionStats);
	Cfg.TilePreloadFovScale     = GetEnvFloat(TEXT("CAMSIM_TILE_FOV_SCALE"),       Cfg.TilePreloadFovScale);
	Cfg.MaxSimultaneousTileLoads = GetEnvInt(TEXT("CAMSIM_MAX_TILE_LOADS"),        Cfg.MaxSimultaneousTileLoads);
	Cfg.MaximumScreenSpaceError = GetEnvFloat(TEXT("CAMSIM_MAX_SSE"),             Cfg.MaximumScreenSpaceError);
	Cfg.MaximumCachedBytesMB    = GetEnvInt(TEXT("CAMSIM_MAX_CACHED_MB"),         Cfg.MaximumCachedBytesMB);
	Cfg.LoadingDescendantLimit  = GetEnvInt  (TEXT("CAMSIM_LOADING_DESCENDANT_LIMIT"), Cfg.LoadingDescendantLimit);
	Cfg.bUseLodTransitions      = GetEnvBool (TEXT("CAMSIM_USE_LOD_TRANSITIONS"),      Cfg.bUseLodTransitions);
	Cfg.LodTransitionLength     = GetEnvFloat(TEXT("CAMSIM_LOD_TRANSITION_LENGTH"),    Cfg.LodTransitionLength);
	Cfg.Encoder = GetEnv(TEXT("CAMSIM_ENCODER"), Cfg.Encoder);
	Cfg.MaxEntities = GetEnvInt(TEXT("CAMSIM_MAX_ENTITIES"), Cfg.MaxEntities);
	Cfg.TerrainProvider = GetEnv(TEXT("CAMSIM_TERRAIN_PROVIDER"), Cfg.TerrainProvider).TrimStartAndEnd().ToLower();
	Cfg.ImageryProvider = GetEnv(TEXT("CAMSIM_IMAGERY_PROVIDER"), Cfg.ImageryProvider).TrimStartAndEnd().ToLower();
	Cfg.StartLatitude  = GetEnvDouble(TEXT("CAMSIM_START_LAT"),   Cfg.StartLatitude);
	Cfg.StartLongitude = GetEnvDouble(TEXT("CAMSIM_START_LON"),   Cfg.StartLongitude);
	Cfg.StartAltitude  = GetEnvDouble(TEXT("CAMSIM_START_ALT"),   Cfg.StartAltitude);
	Cfg.StartYaw       = GetEnvFloat(TEXT("CAMSIM_START_YAW"),    Cfg.StartYaw);
	Cfg.StartPitch     = GetEnvFloat(TEXT("CAMSIM_START_PITCH"),  Cfg.StartPitch);
	Cfg.StartRoll      = GetEnvFloat(TEXT("CAMSIM_START_ROLL"),   Cfg.StartRoll);
	Cfg.StartHour      = GetEnvFloat(TEXT("CAMSIM_START_HOUR"),   Cfg.StartHour);

	{
		const FString Preset = GetEnv(TEXT("CAMSIM_SENSOR_QUALITY_PRESET"), TEXT(""));
		if (!Preset.IsEmpty())
		{
			Cfg.SensorQualityPreset = NormalizeQualityPreset(Preset);
		}
	}
	ResolveActiveSensorQuality(Cfg);
	Cfg.ActiveSensorQuality.NoiseScale = GetEnvFloat(TEXT("CAMSIM_SENSOR_QUALITY_NOISE_SCALE"), Cfg.ActiveSensorQuality.NoiseScale);
	Cfg.ActiveSensorQuality.VignettingScale = GetEnvFloat(TEXT("CAMSIM_SENSOR_QUALITY_VIGNETTING_SCALE"), Cfg.ActiveSensorQuality.VignettingScale);
	Cfg.ActiveSensorQuality.ScanLineScale = GetEnvFloat(TEXT("CAMSIM_SENSOR_QUALITY_SCANLINE_SCALE"), Cfg.ActiveSensorQuality.ScanLineScale);
	Cfg.ActiveSensorQuality.AtmosphereScale = GetEnvFloat(TEXT("CAMSIM_SENSOR_QUALITY_ATMOSPHERE_SCALE"), Cfg.ActiveSensorQuality.AtmosphereScale);
	Cfg.ActiveSensorQuality.BlurRadius = FMath::Max(0, GetEnvInt(TEXT("CAMSIM_SENSOR_QUALITY_BLUR_RADIUS"), Cfg.ActiveSensorQuality.BlurRadius));
	Cfg.ActiveSensorQuality.Contrast = GetEnvFloat(TEXT("CAMSIM_SENSOR_QUALITY_CONTRAST"), Cfg.ActiveSensorQuality.Contrast);
	Cfg.ActiveSensorQuality.BrightnessBias = GetEnvFloat(TEXT("CAMSIM_SENSOR_QUALITY_BRIGHTNESS_BIAS"), Cfg.ActiveSensorQuality.BrightnessBias);
	Cfg.ActiveSensorQuality.GaussianSigmaScale = GetEnvFloat(TEXT("CAMSIM_SENSOR_QUALITY_GAUSSIAN_SIGMA_SCALE"), Cfg.ActiveSensorQuality.GaussianSigmaScale);

	// Phase 16 Sprint 2: per-mode env var overrides (applied to whichever mode has the feature)
	if (FSensorModeConfig* IrM = Cfg.SensorModeConfigs.Find(ESensorMode::IR))
	{
		IrM->bThermalDriftEnabled = GetEnvInt(TEXT("CAMSIM_IR_THERMAL_DRIFT_ENABLED"), IrM->bThermalDriftEnabled ? 1 : 0) != 0;
		IrM->ThermalDriftRate     = GetEnvFloat(TEXT("CAMSIM_IR_THERMAL_DRIFT_RATE"), IrM->ThermalDriftRate);
		IrM->NUCIntervalSec       = GetEnvFloat(TEXT("CAMSIM_IR_NUC_INTERVAL_SEC"), IrM->NUCIntervalSec);
		IrM->AGCLagFrames         = FMath::Clamp(GetEnvInt(TEXT("CAMSIM_IR_AGC_LAG_FRAMES"), IrM->AGCLagFrames), 0, 10);
		IrM->GainJitter           = GetEnvFloat(TEXT("CAMSIM_IR_GAIN_JITTER"), IrM->GainJitter);
		IrM->OffsetJitter         = GetEnvFloat(TEXT("CAMSIM_IR_OFFSET_JITTER"), IrM->OffsetJitter);
		IrM->VibrationAmplitude   = GetEnvFloat(TEXT("CAMSIM_IR_VIBRATION_AMPLITUDE"), IrM->VibrationAmplitude);
	}
	if (FSensorModeConfig* EoM = Cfg.SensorModeConfigs.Find(ESensorMode::EO))
	{
		EoM->RollingShutterStrength = GetEnvFloat(TEXT("CAMSIM_EO_ROLLING_SHUTTER_STRENGTH"), EoM->RollingShutterStrength);
		EoM->SunGlintIntensity      = GetEnvFloat(TEXT("CAMSIM_EO_SUN_GLINT_INTENSITY"), EoM->SunGlintIntensity);
		EoM->SunGlintThreshold      = GetEnvFloat(TEXT("CAMSIM_EO_SUN_GLINT_THRESHOLD"), EoM->SunGlintThreshold);
		EoM->SunGlintSpread         = GetEnvFloat(TEXT("CAMSIM_EO_SUN_GLINT_SPREAD"), EoM->SunGlintSpread);
		EoM->VibrationAmplitude     = GetEnvFloat(TEXT("CAMSIM_EO_VIBRATION_AMPLITUDE"), EoM->VibrationAmplitude);
	}
	if (FSensorModeConfig* NvgM = Cfg.SensorModeConfigs.Find(ESensorMode::NVG))
	{
		NvgM->AGCLagFrames        = FMath::Clamp(GetEnvInt(TEXT("CAMSIM_NVG_AGC_LAG_FRAMES"), NvgM->AGCLagFrames), 0, 10);
		NvgM->VibrationAmplitude  = GetEnvFloat(TEXT("CAMSIM_NVG_VIBRATION_AMPLITUDE"), NvgM->VibrationAmplitude);
	}

	Cfg.GroundTruth.bEnabled = GetEnvInt(TEXT("CAMSIM_GROUND_TRUTH_ENABLED"), Cfg.GroundTruth.bEnabled ? 1 : 0) != 0;
	Cfg.GroundTruth.OutputPath = GetEnv(TEXT("CAMSIM_GROUND_TRUTH_PATH"), Cfg.GroundTruth.OutputPath);
	Cfg.GroundTruth.IntervalFrames = FMath::Max(1, GetEnvInt(TEXT("CAMSIM_GROUND_TRUTH_INTERVAL_FRAMES"), Cfg.GroundTruth.IntervalFrames));

	Cfg.MLTraining.bEnabled = GetEnvInt(TEXT("CAMSIM_ML_ENABLED"), Cfg.MLTraining.bEnabled ? 1 : 0) != 0;
	Cfg.MLTraining.OutputDir = GetEnv(TEXT("CAMSIM_ML_OUTPUT_DIR"), Cfg.MLTraining.OutputDir);
	Cfg.MLTraining.bDepthMap = GetEnvInt(TEXT("CAMSIM_ML_DEPTH_ENABLED"), Cfg.MLTraining.bDepthMap ? 1 : 0) != 0;
	Cfg.MLTraining.bBoundingBoxes = GetEnvInt(TEXT("CAMSIM_ML_BBOX_ENABLED"), Cfg.MLTraining.bBoundingBoxes ? 1 : 0) != 0;
	Cfg.MLTraining.bCocoExport = GetEnvInt(TEXT("CAMSIM_ML_COCO_ENABLED"), Cfg.MLTraining.bCocoExport ? 1 : 0) != 0;
	Cfg.MLTraining.bVocExport = GetEnvInt(TEXT("CAMSIM_ML_VOC_ENABLED"), Cfg.MLTraining.bVocExport ? 1 : 0) != 0;
	Cfg.MLTraining.AnnotationIntervalFrames = FMath::Max(1, GetEnvInt(TEXT("CAMSIM_ML_INTERVAL_FRAMES"), Cfg.MLTraining.AnnotationIntervalFrames));
	Cfg.MLTraining.DepthFarPlaneM = GetEnvFloat(TEXT("CAMSIM_ML_DEPTH_FAR_PLANE_M"), Cfg.MLTraining.DepthFarPlaneM);
	Cfg.EntityScale.MaxDrawDistanceM = GetEnvFloat(TEXT("CAMSIM_ENTITY_MAX_DRAW_DISTANCE_M"), Cfg.EntityScale.MaxDrawDistanceM);
	Cfg.EntityScale.TickRateHz = GetEnvFloat(TEXT("CAMSIM_ENTITY_TICK_RATE_HZ"), Cfg.EntityScale.TickRateHz);
	Cfg.EntityScale.DefaultMaxUpdateRateHz = GetEnvFloat(
		TEXT("CAMSIM_ENTITY_DEFAULT_MAX_UPDATE_RATE_HZ"), Cfg.EntityScale.DefaultMaxUpdateRateHz);
	Cfg.bScenarioEnabled = GetEnvInt(TEXT("CAMSIM_SCENARIO_ENABLED"), Cfg.bScenarioEnabled ? 1 : 0) != 0;
	Cfg.ScenarioTimeScale = GetEnvFloat(TEXT("CAMSIM_SCENARIO_TIME_SCALE"), Cfg.ScenarioTimeScale);
	Cfg.ScenarioStartHour = GetEnvFloat(TEXT("CAMSIM_SCENARIO_START_HOUR"), Cfg.ScenarioStartHour);

	// Phase 22C: Damage transition env overrides
	Cfg.DamageTransition.bDamageTransitionFX = GetEnvInt(TEXT("CAMSIM_DAMAGE_TRANSITION_FX"),
		Cfg.DamageTransition.bDamageTransitionFX ? 1 : 0) != 0;
	Cfg.DamageTransition.bGradualDamage = GetEnvInt(TEXT("CAMSIM_DAMAGE_GRADUAL"),
		Cfg.DamageTransition.bGradualDamage ? 1 : 0) != 0;
	Cfg.DamageTransition.DamageInterpolationSec = GetEnvFloat(TEXT("CAMSIM_DAMAGE_INTERPOLATION_SEC"),
		Cfg.DamageTransition.DamageInterpolationSec);

	// Phase 12A: security metadata env overrides
	Cfg.SecurityMetadata.Classification = GetEnv(TEXT("CAMSIM_SECURITY_CLASSIFICATION"), Cfg.SecurityMetadata.Classification);
	Cfg.SecurityMetadata.ClassifyingCountry = GetEnv(TEXT("CAMSIM_SECURITY_CLASSIFYING_COUNTRY"), Cfg.SecurityMetadata.ClassifyingCountry);
	Cfg.SecurityMetadata.ObjectCountryCodes = GetEnv(TEXT("CAMSIM_SECURITY_OBJECT_COUNTRY"), Cfg.SecurityMetadata.ObjectCountryCodes);

	// Phase 12B: video codec
	Cfg.VideoCodec = GetEnv(TEXT("CAMSIM_VIDEO_CODEC"), Cfg.VideoCodec);

	// Phase 12D: Prometheus
	Cfg.PrometheusMetricsPath = GetEnv(TEXT("CAMSIM_PROMETHEUS_METRICS_PATH"), Cfg.PrometheusMetricsPath);

	// Phase 12E: recording/playback
	Cfg.Recording.CigiRecordPath = GetEnv(TEXT("CAMSIM_CIGI_RECORD_PATH"), Cfg.Recording.CigiRecordPath);
	Cfg.Recording.VideoRecordPath = GetEnv(TEXT("CAMSIM_VIDEO_RECORD_PATH"), Cfg.Recording.VideoRecordPath);
	Cfg.Recording.CigiPlaybackPath = GetEnv(TEXT("CAMSIM_CIGI_PLAYBACK_PATH"), Cfg.Recording.CigiPlaybackPath);

	// Phase 15: optical realism env overrides
	Cfg.OpticalRealism.bEnabled = GetEnvInt(TEXT("CAMSIM_OPTICAL_REALISM_ENABLED"),
		Cfg.OpticalRealism.bEnabled ? 1 : 0) != 0;
	Cfg.OpticalRealism.MotionBlurAmount = GetEnvFloat(TEXT("CAMSIM_MOTION_BLUR_AMOUNT"), Cfg.OpticalRealism.MotionBlurAmount);
	Cfg.OpticalRealism.DistortionK1 = GetEnvFloat(TEXT("CAMSIM_DISTORTION_K1"), Cfg.OpticalRealism.DistortionK1);
	Cfg.OpticalRealism.DistortionK2 = GetEnvFloat(TEXT("CAMSIM_DISTORTION_K2"), Cfg.OpticalRealism.DistortionK2);
	Cfg.OpticalRealism.FocalDistance = GetEnvFloat(TEXT("CAMSIM_FOCAL_DISTANCE"), Cfg.OpticalRealism.FocalDistance);
	Cfg.OpticalRealism.ApertureFStop = GetEnvFloat(TEXT("CAMSIM_APERTURE_FSTOP"), Cfg.OpticalRealism.ApertureFStop);

	// Phase 18: weather, atmosphere & particle effects env overrides
	Cfg.Phase18.bSecondFog       = GetEnvInt(TEXT("CAMSIM_SECOND_FOG"),       Cfg.Phase18.bSecondFog       ? 1 : 0) != 0;
	Cfg.Phase18.bPrecipitation   = GetEnvInt(TEXT("CAMSIM_PRECIPITATION"),    Cfg.Phase18.bPrecipitation   ? 1 : 0) != 0;
	Cfg.Phase18.RainIntensity    = GetEnvFloat(TEXT("CAMSIM_RAIN_INTENSITY"), Cfg.Phase18.RainIntensity);
	Cfg.Phase18.SnowIntensity    = GetEnvFloat(TEXT("CAMSIM_SNOW_INTENSITY"), Cfg.Phase18.SnowIntensity);
	Cfg.Phase18.bGodRays         = GetEnvInt(TEXT("CAMSIM_GOD_RAYS"),        Cfg.Phase18.bGodRays         ? 1 : 0) != 0;
	Cfg.Phase18.bDynamicIRExtinction = GetEnvInt(TEXT("CAMSIM_DYNAMIC_IR_EXTINCTION"),
		Cfg.Phase18.bDynamicIRExtinction ? 1 : 0) != 0;
	Cfg.Phase18.VisibilityRangeM = GetEnvFloat(TEXT("CAMSIM_VISIBILITY_RANGE_M"), Cfg.Phase18.VisibilityRangeM);
	Cfg.Phase18.bVolumetricClouds   = GetEnvInt(TEXT("CAMSIM_VOLUMETRIC_CLOUDS"),      Cfg.Phase18.bVolumetricClouds   ? 1 : 0) != 0;
	Cfg.Phase18.CloudShadowStrength = GetEnvFloat(TEXT("CAMSIM_CLOUD_SHADOW_STRENGTH"),Cfg.Phase18.CloudShadowStrength);
	Cfg.Phase18.bWeatherZones       = GetEnvInt(TEXT("CAMSIM_WEATHER_ZONES"),          Cfg.Phase18.bWeatherZones       ? 1 : 0) != 0;
	Cfg.Phase18.ContrailAltM        = GetEnvFloat(TEXT("CAMSIM_CONTRAIL_ALT_M"),       Cfg.Phase18.ContrailAltM);
	Cfg.Phase18.MaxCraters          = GetEnvInt(TEXT("CAMSIM_MAX_CRATERS"),            Cfg.Phase18.MaxCraters);
	Cfg.Phase18.ContrailSpeedMs       = GetEnvFloat(TEXT("CAMSIM_CONTRAIL_SPEED_MS"),        Cfg.Phase18.ContrailSpeedMs);
	Cfg.Phase18.SmokeComponentID      = GetEnvInt  (TEXT("CAMSIM_SMOKE_COMPONENT_ID"),        Cfg.Phase18.SmokeComponentID);
	Cfg.Phase18.FireComponentID       = GetEnvInt  (TEXT("CAMSIM_FIRE_COMPONENT_ID"),         Cfg.Phase18.FireComponentID);
	Cfg.Phase18.CraterImpactComponentID = GetEnvInt(TEXT("CAMSIM_CRATER_IMPACT_COMPONENT_ID"),Cfg.Phase18.CraterImpactComponentID);
	Cfg.Phase18.CraterDefaultRadiusM  = GetEnvFloat(TEXT("CAMSIM_CRATER_DEFAULT_RADIUS_M"),   Cfg.Phase18.CraterDefaultRadiusM);

	// Phase 19 — Ocean
	Cfg.Phase19.bOceanEnabled           = GetEnvInt  (TEXT("CAMSIM_OCEAN_ENABLED"),            Cfg.Phase19.bOceanEnabled            ? 1 : 0) != 0;
	Cfg.Phase19.BeaufortState           = GetEnvInt  (TEXT("CAMSIM_OCEAN_BEAUFORT"),            Cfg.Phase19.BeaufortState);
	Cfg.Phase19.WaveAmplitudeScale      = GetEnvFloat(TEXT("CAMSIM_OCEAN_AMP_SCALE"),           Cfg.Phase19.WaveAmplitudeScale);
	Cfg.Phase19.WaveFrequencyScale      = GetEnvFloat(TEXT("CAMSIM_OCEAN_FREQ_SCALE"),          Cfg.Phase19.WaveFrequencyScale);
	Cfg.Phase19.WaveChoppiness          = GetEnvFloat(TEXT("CAMSIM_OCEAN_CHOPPINESS"),          Cfg.Phase19.WaveChoppiness);
	Cfg.Phase19.bVesselWakesEnabled     = GetEnvInt  (TEXT("CAMSIM_OCEAN_WAKES_ENABLED"),       Cfg.Phase19.bVesselWakesEnabled      ? 1 : 0) != 0;
	Cfg.Phase19.WakeFadeTime            = GetEnvFloat(TEXT("CAMSIM_OCEAN_WAKE_FADE"),           Cfg.Phase19.WakeFadeTime);
	Cfg.Phase19.bVesselMotionEnabled    = GetEnvInt  (TEXT("CAMSIM_OCEAN_MOTION_ENABLED"),      Cfg.Phase19.bVesselMotionEnabled     ? 1 : 0) != 0;
	Cfg.Phase19.VesselMotionScale       = GetEnvFloat(TEXT("CAMSIM_OCEAN_MOTION_SCALE"),        Cfg.Phase19.VesselMotionScale);
	Cfg.Phase19.bOceanReflectionsEnabled = GetEnvInt (TEXT("CAMSIM_OCEAN_REFLECTIONS_ENABLED"), Cfg.Phase19.bOceanReflectionsEnabled ? 1 : 0) != 0;
	Cfg.Phase19.SSRIntensity            = GetEnvFloat(TEXT("CAMSIM_OCEAN_SSR_INTENSITY"),       Cfg.Phase19.SSRIntensity);
	Cfg.Phase19.ReflectionCaptureRadius = GetEnvFloat(TEXT("CAMSIM_OCEAN_REFLECTION_RADIUS"),   Cfg.Phase19.ReflectionCaptureRadius);

	// Phase 24: rendering quality env var overrides
	{
		FRenderingQualityConfig& RQ = Cfg.RenderingQuality;
		RQ.bEntityShadows        = GetEnvInt  (TEXT("CAMSIM_ENTITY_SHADOWS"),          RQ.bEntityShadows        ? 1 : 0) != 0;
		RQ.bContactShadows       = GetEnvInt  (TEXT("CAMSIM_CONTACT_SHADOWS"),         RQ.bContactShadows       ? 1 : 0) != 0;
		RQ.ContactShadowLength   = GetEnvFloat(TEXT("CAMSIM_CONTACT_SHADOW_LENGTH"),   RQ.ContactShadowLength);
		RQ.AOIntensity           = GetEnvFloat(TEXT("CAMSIM_AO_INTENSITY"),            RQ.AOIntensity);
		RQ.AORadius              = GetEnvFloat(TEXT("CAMSIM_AO_RADIUS"),               RQ.AORadius);
		RQ.bRayTracingEnabled    = GetEnvInt  (TEXT("CAMSIM_RT_ENABLED"),              RQ.bRayTracingEnabled    ? 1 : 0) != 0;
		RQ.bRayTracedReflections = GetEnvInt  (TEXT("CAMSIM_RT_REFLECTIONS"),          RQ.bRayTracedReflections ? 1 : 0) != 0;
		RQ.ShadowDistanceScale   = GetEnvFloat(TEXT("CAMSIM_SHADOW_DISTANCE_SCALE"),   RQ.ShadowDistanceScale);
		RQ.VSMResolutionBias     = GetEnvInt  (TEXT("CAMSIM_VSM_RESOLUTION_BIAS"),     RQ.VSMResolutionBias);
		RQ.VSMMaxPhysicalPages   = GetEnvInt  (TEXT("CAMSIM_VSM_MAX_PAGES"),           RQ.VSMMaxPhysicalPages);
		RQ.TSRScreenPercentage   = GetEnvInt  (TEXT("CAMSIM_TSR_SCREEN_PERCENTAGE"),   RQ.TSRScreenPercentage);
	}

	// Cesium backend env var overrides — IonToken is never logged
	Cfg.CesiumBackend.IonPortalUrl = GetEnv(TEXT("CAMSIM_CESIUM_ION_PORTAL_URL"), Cfg.CesiumBackend.IonPortalUrl);
	Cfg.CesiumBackend.IonApiUrl    = GetEnv(TEXT("CAMSIM_CESIUM_ION_API_URL"),    Cfg.CesiumBackend.IonApiUrl);
	Cfg.CesiumBackend.IonToken     = GetEnv(TEXT("CAMSIM_CESIUM_ION_TOKEN"),      Cfg.CesiumBackend.IonToken);
	Cfg.CesiumBackend.Terrain.Source     = GetEnv(TEXT("CAMSIM_CESIUM_TERRAIN_SOURCE"), Cfg.CesiumBackend.Terrain.Source).TrimStartAndEnd().ToLower();
	Cfg.CesiumBackend.Terrain.IonAssetId = GetEnvInt(TEXT("CAMSIM_CESIUM_TERRAIN_ION_ASSET_ID"), Cfg.CesiumBackend.Terrain.IonAssetId);
	Cfg.CesiumBackend.Terrain.Url        = GetEnv(TEXT("CAMSIM_CESIUM_TERRAIN_URL"), Cfg.CesiumBackend.Terrain.Url);
	Cfg.CesiumBackend.Imagery.Source     = GetEnv(TEXT("CAMSIM_CESIUM_IMAGERY_SOURCE"), Cfg.CesiumBackend.Imagery.Source).TrimStartAndEnd().ToLower();
	Cfg.CesiumBackend.Imagery.IonAssetId = GetEnvInt(TEXT("CAMSIM_CESIUM_IMAGERY_ION_ASSET_ID"), Cfg.CesiumBackend.Imagery.IonAssetId);
	Cfg.CesiumBackend.Imagery.WmsUrl     = GetEnv(TEXT("CAMSIM_CESIUM_IMAGERY_WMS_URL"),    Cfg.CesiumBackend.Imagery.WmsUrl);
	Cfg.CesiumBackend.Imagery.WmsLayers  = GetEnv(TEXT("CAMSIM_CESIUM_IMAGERY_WMS_LAYERS"), Cfg.CesiumBackend.Imagery.WmsLayers);
	Cfg.CesiumBackend.Imagery.WmsTileWidth  = GetEnvInt(TEXT("CAMSIM_CESIUM_IMAGERY_WMS_TILE_WIDTH"),  Cfg.CesiumBackend.Imagery.WmsTileWidth);
	Cfg.CesiumBackend.Imagery.WmsTileHeight = GetEnvInt(TEXT("CAMSIM_CESIUM_IMAGERY_WMS_TILE_HEIGHT"), Cfg.CesiumBackend.Imagery.WmsTileHeight);
	Cfg.CesiumBackend.Imagery.MaximumScreenSpaceError      = GetEnvDouble(TEXT("CAMSIM_CESIUM_IMAGERY_MAX_SSE"),      Cfg.CesiumBackend.Imagery.MaximumScreenSpaceError);
	Cfg.CesiumBackend.Imagery.MaximumTextureSize          = GetEnvInt(TEXT("CAMSIM_CESIUM_IMAGERY_MAX_TEXTURE_SIZE"), Cfg.CesiumBackend.Imagery.MaximumTextureSize);
	Cfg.CesiumBackend.Imagery.MaximumSimultaneousTileLoads = GetEnvInt(TEXT("CAMSIM_CESIUM_IMAGERY_MAX_TILE_LOADS"),  Cfg.CesiumBackend.Imagery.MaximumSimultaneousTileLoads);

	// Phase 20: overlay HUD/OSD env var overrides
	// Apply preset first so individual env var overrides win
	{
		const FString PresetEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("CAMSIM_OVERLAY_PRESET"));
		if (!PresetEnv.IsEmpty())
			FHudOverlay::LoadPreset(PresetEnv, Cfg.OverlayConfig);
	}
	Cfg.OverlayConfig.bEnabled                        = GetEnvInt(TEXT("CAMSIM_OVERLAY_ENABLED"),     Cfg.OverlayConfig.bEnabled                       ? 1 : 0) != 0;
	Cfg.OverlayConfig.ElementCrosshair.bEnabled       = GetEnvInt(TEXT("CAMSIM_OVERLAY_CROSSHAIR"),   Cfg.OverlayConfig.ElementCrosshair.bEnabled       ? 1 : 0) != 0;
	Cfg.OverlayConfig.ElementAzEl.bEnabled            = GetEnvInt(TEXT("CAMSIM_OVERLAY_AZEL"),        Cfg.OverlayConfig.ElementAzEl.bEnabled            ? 1 : 0) != 0;
	Cfg.OverlayConfig.ElementFov.bEnabled             = GetEnvInt(TEXT("CAMSIM_OVERLAY_FOV"),         Cfg.OverlayConfig.ElementFov.bEnabled             ? 1 : 0) != 0;
	Cfg.OverlayConfig.ElementSlantRange.bEnabled      = GetEnvInt(TEXT("CAMSIM_OVERLAY_SLANT_RANGE"), Cfg.OverlayConfig.ElementSlantRange.bEnabled      ? 1 : 0) != 0;
	Cfg.OverlayConfig.ElementTimestamp.bEnabled       = GetEnvInt(TEXT("CAMSIM_OVERLAY_TIMESTAMP"),   Cfg.OverlayConfig.ElementTimestamp.bEnabled       ? 1 : 0) != 0;
	Cfg.OverlayConfig.ElementClassBanner.bEnabled     = GetEnvInt(TEXT("CAMSIM_OVERLAY_CLASS_BANNER"),Cfg.OverlayConfig.ElementClassBanner.bEnabled     ? 1 : 0) != 0;
	Cfg.OverlayConfig.ElementCompassRose.bEnabled     = GetEnvInt(TEXT("CAMSIM_OVERLAY_COMPASS_ROSE"),Cfg.OverlayConfig.ElementCompassRose.bEnabled     ? 1 : 0) != 0;
	{
		const FString ClassTxt = FPlatformMisc::GetEnvironmentVariable(TEXT("CAMSIM_OVERLAY_CLASS_TEXT"));
		if (!ClassTxt.IsEmpty()) Cfg.OverlayConfig.ClassificationText = ClassTxt;
	}
	{
		const FString LabelTxt = FPlatformMisc::GetEnvironmentVariable(TEXT("CAMSIM_OVERLAY_PLATFORM_LABEL"));
		if (!LabelTxt.IsEmpty())
		{
			Cfg.OverlayConfig.PlatformLabelText = LabelTxt;
			Cfg.OverlayConfig.ElementPlatformLabel.bEnabled = true;
		}
	}

	if (Cfg.OutputViews.Num() > 0 && (bHasMulticastAddrEnv || bHasMulticastPortEnv))
	{
		for (FOutputViewConfig& ViewCfg : Cfg.OutputViews)
		{
			if (bHasMulticastAddrEnv)
			{
				ViewCfg.MulticastAddr = Cfg.MulticastAddr;
			}
			if (bHasMulticastPortEnv)
			{
				ViewCfg.MulticastPort = Cfg.MulticastPort;
			}
		}
	}

	UE_LOG(LogCamSim, Log,
		TEXT("Config: CIGI=%s:%d Out=udp://%s:%d Bitrate=%d Preset=%s Encoder=%s ReadbackReadyPolls=%d WatchdogInterval=%d ")
		TEXT("SSE=%.1f CacheMB=%d MaxEntities=%d GpuSensorFX=%d ")
		TEXT("SensorQuality=%s TerrainProvider=%s ImageryProvider=%s GroundTruth=%d ")
		TEXT("EntityScale(draw=%.1fm tick=%.1fHz pose_cap=%.1fHz) Scenario=%d entities=%d time_scale=%.2f"),
		*Cfg.CigiBindAddr, Cfg.CigiPort,
		*Cfg.MulticastAddr, Cfg.MulticastPort,
		Cfg.VideoBitrate, *Cfg.H264Preset, *Cfg.Encoder,
		Cfg.ReadbackReadyPolls, Cfg.EncoderWatchdogIntervalTicks,
		Cfg.MaximumScreenSpaceError, Cfg.MaximumCachedBytesMB, Cfg.MaxEntities, Cfg.Performance.bGpuSensorEffects ? 1 : 0,
		*Cfg.SensorQualityPreset, *Cfg.TerrainProvider, *Cfg.ImageryProvider,
		Cfg.GroundTruth.bEnabled ? 1 : 0,
		Cfg.EntityScale.MaxDrawDistanceM, Cfg.EntityScale.TickRateHz, Cfg.EntityScale.DefaultMaxUpdateRateHz,
		Cfg.bScenarioEnabled ? 1 : 0, Cfg.ScenarioEntities.Num(), Cfg.ScenarioTimeScale);

	// Phase 27: Performance env overrides
	{
		FPerformanceConfig& Perf = Cfg.Performance;
		Perf.bTrackFrameDropsByCategory          = GetEnvBool (TEXT("CAMSIM_PERF_TRACK_DROPS"),         Perf.bTrackFrameDropsByCategory);
		Perf.bHotReloadConfig                    = GetEnvBool (TEXT("CAMSIM_PERF_HOT_RELOAD"),          Perf.bHotReloadConfig);
		Perf.HotReloadPollIntervalSec            = GetEnvFloat(TEXT("CAMSIM_PERF_POLL_INTERVAL"),       Perf.HotReloadPollIntervalSec);
		Perf.TilePrefetchSlewThresholdDegPerSec  = GetEnvFloat(TEXT("CAMSIM_PERF_TILE_SLEW_THRESHOLD"), Perf.TilePrefetchSlewThresholdDegPerSec);
		Perf.TilePrefetchFovBoost                = GetEnvFloat(TEXT("CAMSIM_PERF_TILE_FOV_BOOST"),      Perf.TilePrefetchFovBoost);
		Perf.TilePrefetchBoostFrames             = GetEnvInt  (TEXT("CAMSIM_PERF_TILE_BOOST_FRAMES"),   Perf.TilePrefetchBoostFrames);
		Perf.RenderFrameRateHz                   = GetEnvFloat(TEXT("CAMSIM_PERF_RENDER_FPS"),          Perf.RenderFrameRateHz);
		Perf.OutputFrameRateHz                   = GetEnvFloat(TEXT("CAMSIM_PERF_OUTPUT_FPS"),          Perf.OutputFrameRateHz);
		Perf.TexturePoolBudgetMB                 = GetEnvInt  (TEXT("CAMSIM_PERF_TEXTURE_POOL_MB"),     Perf.TexturePoolBudgetMB);
		Perf.bGpuSensorEffects                   = GetEnvBool (TEXT("CAMSIM_PERF_GPU_SENSOR"),          Perf.bGpuSensorEffects);
		Perf.bAdaptiveSSE                        = GetEnvBool (TEXT("CAMSIM_PERF_ADAPTIVE_SSE"),         Perf.bAdaptiveSSE);
		Perf.AdaptiveSSEMin                      = GetEnvFloat(TEXT("CAMSIM_PERF_ADAPTIVE_SSE_MIN"),     Perf.AdaptiveSSEMin);
		Perf.AdaptiveSSEMax                      = GetEnvFloat(TEXT("CAMSIM_PERF_ADAPTIVE_SSE_MAX"),     Perf.AdaptiveSSEMax);
	}

	// Phase 21: DIS protocol env var overrides
	{
		FDisConfig& D = Cfg.DIS;
		D.bEnabled            = GetEnvInt  (TEXT("CAMSIM_DIS_ENABLED"),         D.bEnabled            ? 1 : 0) != 0;
		D.BindAddr            = GetEnv     (TEXT("CAMSIM_DIS_BIND_ADDR"),       D.BindAddr);
		D.Port                = GetEnvInt  (TEXT("CAMSIM_DIS_PORT"),            D.Port);
		D.MulticastGroup      = GetEnv     (TEXT("CAMSIM_DIS_MULTICAST_GROUP"), D.MulticastGroup);
		D.ExerciseId          = GetEnvInt  (TEXT("CAMSIM_DIS_EXERCISE_ID"),     D.ExerciseId);
		D.SiteId              = GetEnvInt  (TEXT("CAMSIM_DIS_SITE_ID"),         D.SiteId);
		D.ApplicationId       = GetEnvInt  (TEXT("CAMSIM_DIS_APP_ID"),          D.ApplicationId);
		D.HeartbeatTimeoutSec = GetEnvFloat(TEXT("CAMSIM_DIS_HEARTBEAT_TIMEOUT"), D.HeartbeatTimeoutSec);
		D.IdBaseOffset        = GetEnvInt  (TEXT("CAMSIM_DIS_ID_BASE_OFFSET"),  D.IdBaseOffset);
		D.DefaultEntityTypeId = GetEnvInt  (TEXT("CAMSIM_DIS_DEFAULT_ENTITY_TYPE"), D.DefaultEntityTypeId);
	}

	// Phase 21 Sprint 2: streaming env var overrides
	{
		FStreamingConfig& S = Cfg.Streaming;
		S.bCotEnabled      = GetEnvInt  (TEXT("CAMSIM_COT_ENABLED"),       S.bCotEnabled      ? 1 : 0) != 0;
		S.CotAddr          = GetEnv     (TEXT("CAMSIM_COT_ADDR"),          S.CotAddr);
		S.CotPort          = GetEnvInt  (TEXT("CAMSIM_COT_PORT"),          S.CotPort);
		S.CotIntervalSec   = GetEnvFloat(TEXT("CAMSIM_COT_INTERVAL"),      S.CotIntervalSec);
		S.CotUid           = GetEnv     (TEXT("CAMSIM_COT_UID"),           S.CotUid);
		S.CotType          = GetEnv     (TEXT("CAMSIM_COT_TYPE"),          S.CotType);
		S.CotCallsign      = GetEnv     (TEXT("CAMSIM_COT_CALLSIGN"),      S.CotCallsign);
		S.bAtakViewEnabled = GetEnvInt  (TEXT("CAMSIM_ATAK_VIEW_ENABLED"), S.bAtakViewEnabled ? 1 : 0) != 0;
		S.AtakAddr         = GetEnv     (TEXT("CAMSIM_ATAK_ADDR"),         S.AtakAddr);
		S.AtakPort         = GetEnvInt  (TEXT("CAMSIM_ATAK_PORT"),         S.AtakPort);
		S.AtakBitrate      = GetEnvInt  (TEXT("CAMSIM_ATAK_BITRATE"),      S.AtakBitrate);
		S.bRoverCompat     = GetEnvInt  (TEXT("CAMSIM_ROVER_COMPAT"),      S.bRoverCompat     ? 1 : 0) != 0;
		S.RoverVideoPid    = GetEnvInt  (TEXT("CAMSIM_ROVER_VIDEO_PID"),    S.RoverVideoPid);
		S.RoverKlvPid      = GetEnvInt  (TEXT("CAMSIM_ROVER_KLV_PID"),     S.RoverKlvPid);
	}

	// Phase 21 Sprint 2: laser designator env var overrides
	{
		FLaserDesignatorConfig& L = Cfg.LaserDesignator;
		L.bEnabled       = GetEnvInt  (TEXT("CAMSIM_LASER_ENABLED"),   L.bEnabled       ? 1 : 0) != 0;
		L.SpotX          = GetEnvFloat(TEXT("CAMSIM_LASER_X"),         L.SpotX);
		L.SpotY          = GetEnvFloat(TEXT("CAMSIM_LASER_Y"),         L.SpotY);
		L.SpotRadius     = GetEnvFloat(TEXT("CAMSIM_LASER_RADIUS"),    L.SpotRadius);
		L.SpotIntensity  = GetEnvFloat(TEXT("CAMSIM_LASER_INTENSITY"), L.SpotIntensity);
		L.DesignatorCode = GetEnvInt  (TEXT("CAMSIM_LASER_CODE"),      L.DesignatorCode);
	}

	// Log FOV presets so operators can confirm sensor gain→zoom mapping
	if (Cfg.SensorFovPresets.Num() > 0)
	{
		FString PresetStr;
		for (int32 i = 0; i < Cfg.SensorFovPresets.Num(); ++i)
		{
			if (i > 0) PresetStr += TEXT(", ");
			PresetStr += FString::Printf(TEXT("%.1f"), Cfg.SensorFovPresets[i]);
		}
		UE_LOG(LogCamSim, Log, TEXT("Config: SensorFovPresets=[%s] (%d levels)"),
			*PresetStr, Cfg.SensorFovPresets.Num());
	}
}
