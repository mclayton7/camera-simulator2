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
		Cfg.SensorModeConfigs.Add(ESensorMode::NVG, NvgCfg);
	}

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
			UE_LOG(LogCamSim, Warning, TEXT("Failed to parse %s: %hs - using defaults"), *YamlPath, Ex.what());
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
		YamlBool  (Root, "gpu_sensor_effects", Cfg.bGpuSensorEffects);
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

		YamlFloat (Root, "tile_preload_fov_scale",     Cfg.TilePreloadFovScale);
		YamlInt   (Root, "max_simultaneous_tile_loads", Cfg.MaxSimultaneousTileLoads);
		YamlFloat (Root, "maximum_screen_space_error", Cfg.MaximumScreenSpaceError);
		YamlInt   (Root, "maximum_cached_bytes_mb",    Cfg.MaximumCachedBytesMB);
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
		if (Root.has_child("sensor_fov_presets"))
		{
			ryml::ConstNodeRef PresetsNode = Root["sensor_fov_presets"];
			if (PresetsNode.is_seq())
			{
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

						Cfg.ScenarioEntities.Add(Spec);
					}
				}
			}
		}

		// Legacy flat keys.
		YamlBool (Root, "scenario_enabled",    Cfg.bScenarioEnabled);
		YamlFloat(Root, "scenario_time_scale", Cfg.ScenarioTimeScale);

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
	Cfg.TilePreloadFovScale     = GetEnvFloat(TEXT("CAMSIM_TILE_FOV_SCALE"),       Cfg.TilePreloadFovScale);
	Cfg.MaxSimultaneousTileLoads = GetEnvInt(TEXT("CAMSIM_MAX_TILE_LOADS"),        Cfg.MaxSimultaneousTileLoads);
	Cfg.MaximumScreenSpaceError = GetEnvFloat(TEXT("CAMSIM_MAX_SSE"),             Cfg.MaximumScreenSpaceError);
	Cfg.MaximumCachedBytesMB    = GetEnvInt(TEXT("CAMSIM_MAX_CACHED_MB"),         Cfg.MaximumCachedBytesMB);
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

	Cfg.GroundTruth.bEnabled = GetEnvInt(TEXT("CAMSIM_GROUND_TRUTH_ENABLED"), Cfg.GroundTruth.bEnabled ? 1 : 0) != 0;
	Cfg.GroundTruth.OutputPath = GetEnv(TEXT("CAMSIM_GROUND_TRUTH_PATH"), Cfg.GroundTruth.OutputPath);
	Cfg.GroundTruth.IntervalFrames = FMath::Max(1, GetEnvInt(TEXT("CAMSIM_GROUND_TRUTH_INTERVAL_FRAMES"), Cfg.GroundTruth.IntervalFrames));
	Cfg.EntityScale.MaxDrawDistanceM = GetEnvFloat(TEXT("CAMSIM_ENTITY_MAX_DRAW_DISTANCE_M"), Cfg.EntityScale.MaxDrawDistanceM);
	Cfg.EntityScale.TickRateHz = GetEnvFloat(TEXT("CAMSIM_ENTITY_TICK_RATE_HZ"), Cfg.EntityScale.TickRateHz);
	Cfg.EntityScale.DefaultMaxUpdateRateHz = GetEnvFloat(
		TEXT("CAMSIM_ENTITY_DEFAULT_MAX_UPDATE_RATE_HZ"), Cfg.EntityScale.DefaultMaxUpdateRateHz);
	Cfg.bScenarioEnabled = GetEnvInt(TEXT("CAMSIM_SCENARIO_ENABLED"), Cfg.bScenarioEnabled ? 1 : 0) != 0;
	Cfg.ScenarioTimeScale = GetEnvFloat(TEXT("CAMSIM_SCENARIO_TIME_SCALE"), Cfg.ScenarioTimeScale);

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
		Cfg.MaximumScreenSpaceError, Cfg.MaximumCachedBytesMB, Cfg.MaxEntities, Cfg.bGpuSensorEffects ? 1 : 0,
		*Cfg.SensorQualityPreset, *Cfg.TerrainProvider, *Cfg.ImageryProvider,
		Cfg.GroundTruth.bEnabled ? 1 : 0,
		Cfg.EntityScale.MaxDrawDistanceM, Cfg.EntityScale.TickRateHz, Cfg.EntityScale.DefaultMaxUpdateRateHz,
		Cfg.bScenarioEnabled ? 1 : 0, Cfg.ScenarioEntities.Num(), Cfg.ScenarioTimeScale);
}
