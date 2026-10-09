// Copyright CamSim Contributors. All Rights Reserved.

#include "Config/CamSimConfig.h"
#include "CamSimTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Sensor/SensorOptics.h"
#include "Sensor/SensorPresets.h"
#include "Thermal/LandCoverClasses.h"
#include "Thermal/ThermalMaterials.h"

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
	Value.TrimStartAndEndInline();
	if (Value.IsEmpty()) return Default;
	// Accept true/yes/on as well as numbers ("true" used to parse as 0 = off).
	return Value.Equals(TEXT("true"), ESearchCase::IgnoreCase)
		|| Value.Equals(TEXT("yes"), ESearchCase::IgnoreCase)
		|| Value.Equals(TEXT("on"), ESearchCase::IgnoreCase)
		|| FCString::Atoi(*Value) != 0;
}

static FCamSimConfig::EEncoderWatchdogPolicy ParseWatchdogPolicy(const FString& Value)
{
	const FString Lower = Value.ToLower();
	if (Lower == TEXT("log_only")) return FCamSimConfig::EEncoderWatchdogPolicy::LogOnly;
	if (Lower == TEXT("fail_fast")) return FCamSimConfig::EEncoderWatchdogPolicy::FailFast;
	return FCamSimConfig::EEncoderWatchdogPolicy::Reconnect;
}

static FCamSimConfig::EEncoderPreference ParseEncoderPreference(const FString& Value)
{
	const FString Lower = Value.ToLower().TrimStartAndEnd();
	if (Lower == TEXT("nvenc"))   return FCamSimConfig::EEncoderPreference::Nvenc;
	if (Lower == TEXT("libx264")) return FCamSimConfig::EEncoderPreference::LibX264;
	if (Lower == TEXT("libx265")) return FCamSimConfig::EEncoderPreference::LibX265;
	if (Lower == TEXT("videotoolbox")) return FCamSimConfig::EEncoderPreference::VideoToolbox;
	if (Lower == TEXT("auto") || Lower.IsEmpty())
		return FCamSimConfig::EEncoderPreference::Auto;
	UE_LOG(LogCamSim, Warning,
		TEXT("Unknown Encoder preference '%s' — defaulting to Auto"), *Value);
	return FCamSimConfig::EEncoderPreference::Auto;
}

// ---------------------------------------------------------------------------
// ryml YAML helpers — mirror the old TryGet*Field call pattern
// ---------------------------------------------------------------------------

// Records which YAML nodes a setting looked up, so keys nobody reads (typos,
// stale settings) can be reported instead of silently falling back to defaults.
struct FYamlKeyTracker
{
	TSet<ryml::id_type> Read;         // nodes found by YamlHas
	TSet<ryml::id_type> DataParents;  // maps whose keys are data (entity types, preset names)
	TSet<ryml::id_type> Elsewhere;    // subtrees another module parses from the same file
};
static FYamlKeyTracker* GYamlKeyTracker = nullptr;  // set for the duration of one load (game thread)

static bool YamlHas(ryml::ConstNodeRef Node, c4::csubstr Key)
{
	const ryml::id_type Child = Node.tree()->find_child(Node.id(), Key);
	if (Child == ryml::NONE) return false;
	if (GYamlKeyTracker) GYamlKeyTracker->Read.Add(Child);
	return true;
}

/** Mark a map whose keys are names chosen by the user rather than settings. */
static void YamlKeysAreData(ryml::ConstNodeRef Node)
{
	if (GYamlKeyTracker) GYamlKeyTracker->DataParents.Add(Node.id());
}

/** Mark a top-level section as parsed by another module (not checked here). */
static void YamlReadElsewhere(ryml::ConstNodeRef Root, c4::csubstr Key)
{
	const ryml::id_type Child = Root.tree()->find_child(Root.id(), Key);
	if (Child != ryml::NONE && GYamlKeyTracker)
	{
		GYamlKeyTracker->Read.Add(Child);
		GYamlKeyTracker->Elsewhere.Add(Child);
	}
}

/** Dotted paths of map keys that no setting read. Unknown subtrees are reported once. */
static void CollectUnknownYamlKeys(ryml::ConstNodeRef Node, const FString& Path,
                                   const FYamlKeyTracker& Tracker, TArray<FString>& Out)
{
	int32 Index = 0;
	for (ryml::ConstNodeRef Child : Node.children())
	{
		FString ChildPath;
		if (Node.is_map())
		{
			const FString Key = FString(static_cast<int32>(Child.key().len), UTF8_TO_TCHAR(Child.key().str));
			ChildPath = Path.IsEmpty() ? Key : Path + TEXT(".") + Key;
			if (!Tracker.Read.Contains(Child.id()) && !Tracker.DataParents.Contains(Node.id()))
			{
				Out.Add(ChildPath);
				continue;
			}
		}
		else
		{
			ChildPath = FString::Printf(TEXT("%s[%d]"), *Path, Index++);
		}
		if ((Child.is_map() || Child.is_seq()) && !Tracker.Elsewhere.Contains(Child.id()))
		{
			CollectUnknownYamlKeys(Child, ChildPath, Tracker, Out);
		}
	}
}

static FString RymlToFString(c4::csubstr S)
{
	return FString(static_cast<int32>(S.len), UTF8_TO_TCHAR(S.str));
}

static bool YamlString(ryml::ConstNodeRef Node, c4::csubstr Key, FString& Out)
{
	if (!YamlHas(Node, Key)) return false;
	ryml::ConstNodeRef Child = Node[Key];
	if (!Child.has_val()) return false;
	Out = RymlToFString(Child.val());
	return true;
}

static bool YamlInt(ryml::ConstNodeRef Node, c4::csubstr Key, int32& Out)
{
	if (!YamlHas(Node, Key)) return false;
	ryml::ConstNodeRef Child = Node[Key];
	if (!Child.has_val()) return false;
	FString Str = RymlToFString(Child.val());
	Out = FCString::Atoi(*Str);
	return true;
}

static bool YamlFloat(ryml::ConstNodeRef Node, c4::csubstr Key, float& Out)
{
	if (!YamlHas(Node, Key)) return false;
	ryml::ConstNodeRef Child = Node[Key];
	if (!Child.has_val()) return false;
	FString Str = RymlToFString(Child.val());
	Out = FCString::Atof(*Str);
	return true;
}

static bool YamlDouble(ryml::ConstNodeRef Node, c4::csubstr Key, double& Out)
{
	if (!YamlHas(Node, Key)) return false;
	ryml::ConstNodeRef Child = Node[Key];
	if (!Child.has_val()) return false;
	FString Str = RymlToFString(Child.val());
	Out = FCString::Atod(*Str);
	return true;
}

static bool YamlBool(ryml::ConstNodeRef Node, c4::csubstr Key, bool& Out)
{
	if (!YamlHas(Node, Key)) return false;
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
	const FString YamlPath = GetConfigFilePath();
	FString YamlContent;
	if (!FFileHelper::LoadFileToString(YamlContent, *YamlPath))
	{
		UE_LOG(LogCamSim, Log, TEXT("No config file found at %s - using defaults"), *YamlPath);
		return LoadFromYaml(nullptr, YamlPath);
	}
	return LoadFromYaml(&YamlContent, YamlPath);
}

FCamSimConfig FCamSimConfig::LoadFromYamlString(const FString& YamlContent, const FString& SourceName)
{
	return LoadFromYaml(&YamlContent, SourceName);
}

FCamSimConfig FCamSimConfig::LoadFromYaml(const FString* YamlContent, const FString& YamlPath)
{
	FCamSimConfig Cfg; // default values from member initialisers

	// Apply default sensor mode configs (overwritten by YAML if present)
	{
		FSensorModeConfig EoCfg;
		// ROADMAP 3B.1 calibrated exposure (must match deploy/camsim_config.yaml;
		// CamSim.Sensor.Config.PerModeExposureDefaults checks both).
		EoCfg.Exposure.MinGainEv           = -20.0f;
		EoCfg.Exposure.MaxPhotonGainEv     = -12.5f;
		EoCfg.Exposure.TargetGrey          = 0.18f;
		EoCfg.Exposure.HighlightPercentile = 0.99f;
		EoCfg.Exposure.LagFrames           = 2;
		EoCfg.Exposure.ManualGainEv        = -12.0f;
		// ROADMAP 3B.2 Task 5: sensor-class preset (1080p industrial CMOS).
		EoCfg.Preset = TEXT("eo_hd_cmos");
		CamSimSensorPresets::Apply(EoCfg.Preset, EoCfg);
		Cfg.SensorModeConfigs.Add(ESensorMode::EO, EoCfg);

		FSensorModeConfig IrCfg;
		// Phase 16 defaults: radiance-based AGC (percentile stretch)
		IrCfg.bAGCEnabled        = true;
		IrCfg.AGCLowPercentile   = 0.01f;
		IrCfg.AGCHighPercentile  = 0.99f;
		IrCfg.AGCLagFrames       = 2;
		IrCfg.Exposure.MinGainEv           = -20.0f;
		IrCfg.Exposure.MaxPhotonGainEv     = -6.0f;
		IrCfg.Exposure.TargetGrey          = 0.18f;
		IrCfg.Exposure.HighlightPercentile = 0.99f;
		IrCfg.Exposure.LagFrames           = 2;
		IrCfg.Exposure.ManualGainEv        = -12.0f;
		// ROADMAP 3B.2 Task 5: sensor-class preset (default: cooled MWIR).
		IrCfg.Preset = TEXT("mwir_cooled");
		CamSimSensorPresets::Apply(IrCfg.Preset, IrCfg);
		Cfg.SensorModeConfigs.Add(ESensorMode::IR, IrCfg);
	}

	// Default FOV presets (wide → narrow); YAML values replace these if present
	Cfg.SensorFovPresets = { 60.0f, 20.0f, 5.0f };

	if (YamlContent)
	{
		// Convert FString (UTF-16) to UTF-8 std::string for ryml
		FTCHARToUTF8 Utf8(**YamlContent);
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
		FYamlKeyTracker KeyTracker;
		GYamlKeyTracker = &KeyTracker;
		ON_SCOPE_EXIT { GYamlKeyTracker = nullptr; };

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
		YamlInt   (Root, "readback_ready_polls", Cfg.ReadbackReadyPolls);
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
		Cfg.EncoderPref = ParseEncoderPreference(Cfg.Encoder);
		YamlInt   (Root, "max_entities", Cfg.MaxEntities);
		YamlFloat (Root, "hfov_deg",         Cfg.HFovDeg);
		YamlString(Root, "terrain_provider", Cfg.TerrainProvider);
		YamlString(Root, "imagery_provider", Cfg.ImageryProvider);

		if (YamlHas(Root, "terrain"))
		{
			ryml::ConstNodeRef TerrainNode = Root["terrain"];
			YamlString(TerrainNode, "provider", Cfg.TerrainProvider);
		}
		if (YamlHas(Root, "imagery"))
		{
			ryml::ConstNodeRef ImageryNode = Root["imagery"];
			YamlString(ImageryNode, "provider", Cfg.ImageryProvider);
		}

		YamlBool  (Root, "log_tile_stats",             Cfg.bLogTileSelectionStats);
		YamlFloat (Root, "tile_preload_fov_scale",     Cfg.TilePreloadFovScale);
		YamlInt   (Root, "max_simultaneous_tile_loads", Cfg.MaxSimultaneousTileLoads);
		YamlFloat (Root, "maximum_screen_space_error", Cfg.MaximumScreenSpaceError);
		YamlFloat (Root, "culled_screen_space_error",  Cfg.CulledScreenSpaceError);
		YamlBool  (Root, "frustum_culling",            Cfg.bFrustumCulling);
		YamlInt   (Root, "maximum_cached_bytes_mb",    Cfg.MaximumCachedBytesMB);
		YamlInt  (Root, "loading_descendant_limit", Cfg.LoadingDescendantLimit);
		YamlBool (Root, "use_lod_transitions",      Cfg.bUseLodTransitions);
		YamlBool (Root, "create_physics_meshes",    Cfg.bCreatePhysicsMeshes);
		YamlFloat(Root, "lod_transition_length",    Cfg.LodTransitionLength);
		YamlDouble(Root, "start_latitude",   Cfg.StartLatitude);
		YamlDouble(Root, "start_longitude",  Cfg.StartLongitude);
		YamlDouble(Root, "start_altitude",   Cfg.StartAltitude);
		YamlFloat (Root, "start_yaw",        Cfg.StartYaw);
		YamlFloat (Root, "start_pitch",      Cfg.StartPitch);
		YamlFloat (Root, "start_roll",       Cfg.StartRoll);
		YamlFloat (Root, "start_hour",       Cfg.StartHour);
		YamlString(Root, "start_datetime",   Cfg.StartDatetime);
		YamlFloat (Root, "sim_time_rate",    Cfg.SimTimeRate);
		YamlInt   (Root, "camera_entity_id",           Cfg.CameraEntityId);
		YamlFloat (Root, "gimbal_max_slew_rate",       Cfg.GimbalMaxSlewRateDegPerSec);
		YamlFloat (Root, "gimbal_pitch_min",           Cfg.GimbalPitchMin);
		YamlFloat (Root, "gimbal_pitch_max",           Cfg.GimbalPitchMax);
		YamlFloat (Root, "gimbal_yaw_min",             Cfg.GimbalYawMin);
		YamlFloat (Root, "gimbal_yaw_max",             Cfg.GimbalYawMax);

		// FOV presets: optional YAML array of floats (wide -> narrow)
		// Replaces the defaults set above when present in config.
		if (YamlHas(Root, "sensor_fov_presets"))
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
		if (YamlHas(Root, "sensor_modes"))
		{
			ryml::ConstNodeRef ModesNode = Root["sensor_modes"];

			auto ParseMode = [&](c4::csubstr Key, ESensorMode M)
			{
				if (!YamlHas(ModesNode, Key)) return;
				ryml::ConstNodeRef ModeNode = ModesNode[Key];

				FSensorModeConfig& MC = Cfg.SensorModeConfigs.FindOrAdd(M);
				// Radiance-Based AGC (GPU sensor path, IR percentile stretch)
				YamlBool (ModeNode, "agc_enabled",          MC.bAGCEnabled);
				YamlFloat(ModeNode, "agc_low_percentile",   MC.AGCLowPercentile);
				YamlFloat(ModeNode, "agc_high_percentile",  MC.AGCHighPercentile);
				{
					int32 LagVal = MC.AGCLagFrames;
					if (YamlInt(ModeNode, "agc_lag_frames", LagVal))
						MC.AGCLagFrames = FMath::Clamp(LagVal, 0, 10);
				}
				// ROADMAP 3B.1: detector spectral response + auto-exposure
				YamlFloat(ModeNode, "signal_weight_r", MC.SignalWeights.X);
				YamlFloat(ModeNode, "signal_weight_g", MC.SignalWeights.Y);
				YamlFloat(ModeNode, "signal_weight_b", MC.SignalWeights.Z);
				auto ParseExposure = [](ryml::ConstNodeRef ENode, FSensorExposureConfig& X)
				{
					YamlBool (ENode, "auto",                 X.bAuto);
					YamlFloat(ENode, "min_gain_ev",          X.MinGainEv);
					YamlFloat(ENode, "max_photon_gain_ev",   X.MaxPhotonGainEv);
					YamlFloat(ENode, "target_grey",          X.TargetGrey);
					YamlFloat(ENode, "highlight_percentile", X.HighlightPercentile);
					YamlInt  (ENode, "lag_frames",           X.LagFrames);
					YamlFloat(ENode, "manual_gain_ev",       X.ManualGainEv);
				};
				if (YamlHas(ModeNode, "exposure")) ParseExposure(ModeNode["exposure"], MC.Exposure);
				// ROADMAP 4A: thermal radiance AE + AGC cap
				if (YamlHas(ModeNode, "thermal_exposure")) ParseExposure(ModeNode["thermal_exposure"], MC.ThermalExposure);
				YamlFloat(ModeNode, "agc_max_display_gain", MC.AGCMaxDisplayGain);

				// ---------------------------------------------------------------
				// ROADMAP 3B.2 Task 5: sensor-class preset + optics/detector.
				// Parse order: preset (applies its Optics/Detector defaults, or
				// leaves the built-in preset defaults alone if the name is
				// unknown — Validate() reports it), then per-field overrides.
				// ---------------------------------------------------------------
				YamlString(ModeNode, "preset", MC.Preset);
				CamSimSensorPresets::Apply(MC.Preset, MC);
				{
					int32 SeedVal = static_cast<int32>(MC.Seed);
					if (YamlInt(ModeNode, "seed", SeedVal))
						MC.Seed = static_cast<uint32>(SeedVal);
				}
				if (YamlHas(ModeNode, "optics"))
				{
					ryml::ConstNodeRef ONode = ModeNode["optics"];
					YamlFloat(ONode, "f_number",            MC.Optics.FNumber);
					YamlFloat(ONode, "pixel_pitch_um",      MC.Optics.PixelPitchUm);
					YamlFloat(ONode, "wavelength_um",       MC.Optics.WavelengthUm);
					YamlFloat(ONode, "extra_blur_px",       MC.Optics.ExtraBlurPx);
					YamlFloat(ONode, "vignetting_exponent", MC.Optics.VignettingExponent);
					YamlFloat(ONode, "k1",                  MC.Optics.K1);
					YamlFloat(ONode, "k2",                  MC.Optics.K2);
				}
				if (YamlHas(ModeNode, "detector"))
				{
					ryml::ConstNodeRef DNode = ModeNode["detector"];
					FString TypeStr;
					if (YamlString(DNode, "type", TypeStr))
					{
						MC.DetectorTypeName = TypeStr;   // Validate() rejects an unknown name
						if (TypeStr.Equals(TEXT("microbolometer"), ESearchCase::IgnoreCase))
							MC.Detector.Type = ESensorDetectorType::Microbolometer;
						else if (TypeStr.Equals(TEXT("photon"), ESearchCase::IgnoreCase))
							MC.Detector.Type = ESensorDetectorType::Photon;
					}
					YamlFloat(DNode, "full_well_e",       MC.Detector.FullWellE);
					YamlFloat(DNode, "read_noise_e",       MC.Detector.ReadNoiseE);
					YamlFloat(DNode, "prnu",               MC.Detector.Prnu);
					YamlFloat(DNode, "dsnu_e",              MC.Detector.DsnuE);
					YamlFloat(DNode, "dark_current_e_s",    MC.Detector.DarkCurrentEs);
					YamlFloat(DNode, "max_analog_gain_db",  MC.Detector.MaxAnalogGainDb);
					YamlFloat(DNode, "temporal_noise",      MC.Detector.TemporalNoise);
					YamlFloat(DNode, "pixel_fpn",           MC.Detector.PixelFpn);
					YamlFloat(DNode, "column_fpn",          MC.Detector.ColumnFpn);
					YamlFloat(DNode, "row_fpn",              MC.Detector.RowFpn);
					YamlInt  (DNode, "adc_bits",             MC.Detector.AdcBits);
					YamlFloat(DNode, "hot_pixel_fraction",   MC.Detector.HotPixelFraction);
					YamlFloat(DNode, "dead_pixel_fraction",  MC.Detector.DeadPixelFraction);
					YamlFloat(DNode, "band_lo_um",           MC.Detector.BandLoUm);
					YamlFloat(DNode, "band_hi_um",           MC.Detector.BandHiUm);
				}
			};

			ParseMode("eo",  ESensorMode::EO);
			ParseMode("ir",  ESensorMode::IR);
		}

		// Optional multi-stream output views.
		if (YamlHas(Root, "output_views"))
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

		// ML Training Data Generation (Phase 17).
		if (YamlHas(Root, "ml_training"))
		{
			ryml::ConstNodeRef MLNode = Root["ml_training"];
			YamlBool  (MLNode, "enabled",                  Cfg.MLTraining.bEnabled);
			YamlString(MLNode, "output_dir",               Cfg.MLTraining.OutputDir);
			YamlBool  (MLNode, "depth_map",                Cfg.MLTraining.bDepthMap);
			YamlBool  (MLNode, "bounding_boxes",           Cfg.MLTraining.bBoundingBoxes);
			YamlBool  (MLNode, "coco_export",              Cfg.MLTraining.bCocoExport);
			YamlFloat (MLNode, "depth_far_plane_m",        Cfg.MLTraining.DepthFarPlaneM);
			{
				int32 Interval = Cfg.MLTraining.AnnotationIntervalFrames;
				if (YamlInt(MLNode, "annotation_interval_frames", Interval))
					Cfg.MLTraining.AnnotationIntervalFrames = FMath::Max(1, Interval);
			}
			{
				int32 MinPx = Cfg.MLTraining.MinVisiblePixels;
				if (YamlInt(MLNode, "min_visible_pixels", MinPx))
					Cfg.MLTraining.MinVisiblePixels = FMath::Max(1, MinPx);
			}
			YamlBool  (MLNode, "segmentation",             Cfg.MLTraining.bSegmentation);
		}

		// Entity runtime scale controls (LOD/culling/update throttling).
		if (YamlHas(Root, "entity_scale"))
		{
			ryml::ConstNodeRef ScaleNode = Root["entity_scale"];
			YamlFloat(ScaleNode, "max_draw_distance_m",      Cfg.EntityScale.MaxDrawDistanceM);
			YamlFloat(ScaleNode, "tick_rate_hz",              Cfg.EntityScale.TickRateHz);
			YamlFloat(ScaleNode, "default_max_update_rate_hz", Cfg.EntityScale.DefaultMaxUpdateRateHz);

			if (YamlHas(ScaleNode, "max_update_rate_hz_overrides"))
			{
				ryml::ConstNodeRef OverridesNode = ScaleNode["max_update_rate_hz_overrides"];
				if (OverridesNode.is_map())
				{
					YamlKeysAreData(OverridesNode);
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

		// Security metadata (MISB ST 0102, Phase 12A)
		if (YamlHas(Root, "security_metadata"))
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

		// Recording & playback (Phase 12E)
		if (YamlHas(Root, "recording"))
		{
			ryml::ConstNodeRef RecNode = Root["recording"];
			YamlString(RecNode, "cigi_record_path",   Cfg.Recording.CigiRecordPath);
			YamlString(RecNode, "video_record_path",  Cfg.Recording.VideoRecordPath);
			YamlString(RecNode, "cigi_playback_path", Cfg.Recording.CigiPlaybackPath);
		}

		// Phase 18: weather & atmosphere
		if (YamlHas(Root, "phase18"))
		{
			ryml::ConstNodeRef P18 = Root["phase18"];
			YamlBool (P18, "second_fog",                Cfg.Phase18.bSecondFog);
			YamlFloat(P18, "fog_density",               Cfg.Phase18.FogDensity);
			YamlFloat(P18, "fog_height_falloff",        Cfg.Phase18.FogHeightFalloff);
			YamlFloat(P18, "visibility_range_m",        Cfg.Phase18.VisibilityRangeM);
			// 18A/18B
			YamlBool (P18, "volumetric_clouds",          Cfg.Phase18.bVolumetricClouds);
			YamlFloat(P18, "cloud_shadow_strength",       Cfg.Phase18.CloudShadowStrength);
		}

		if (YamlHas(Root, "rendering_quality"))
		{
			ryml::ConstNodeRef RQNode = Root["rendering_quality"];
			FRenderingQualityConfig& RQ = Cfg.RenderingQuality;
			YamlBool (RQNode, "entity_shadows",         RQ.bEntityShadows);
			YamlBool (RQNode, "contact_shadows",        RQ.bContactShadows);
			YamlFloat(RQNode, "contact_shadow_length",  RQ.ContactShadowLength);
			YamlFloat(RQNode, "ao_intensity",           RQ.AOIntensity);
			YamlFloat(RQNode, "ao_radius",              RQ.AORadius);
			YamlBool (RQNode, "rt_enabled",             RQ.bRayTracingEnabled);
			YamlFloat(RQNode, "shadow_distance_scale",  RQ.ShadowDistanceScale);
			YamlInt  (RQNode, "vsm_resolution_bias",    RQ.VSMResolutionBias);
			YamlInt  (RQNode, "vsm_max_physical_pages", RQ.VSMMaxPhysicalPages);
			YamlInt  (RQNode, "tsr_screen_percentage",  RQ.TSRScreenPercentage);
		}

		// Phase 27: Performance
		if (YamlHas(Root, "performance"))
		{
			ryml::ConstNodeRef PerfNode = Root["performance"];
			FPerformanceConfig& Perf = Cfg.Performance;
			YamlInt  (PerfNode, "texture_pool_budget_mb",               Perf.TexturePoolBudgetMB);
			YamlBool (PerfNode, "track_pipeline_latency",              Perf.bTrackPipelineLatency);
		}

		if (YamlHas(Root, "ocean"))
		{
			ryml::ConstNodeRef O = Root["ocean"];
			YamlBool  (O, "enabled",             Cfg.Ocean.bEnabled);
			YamlFloat (O, "beaufort",            Cfg.Ocean.Beaufort);
			YamlFloat (O, "wave_direction_deg",  Cfg.Ocean.WaveDirectionDeg);
			YamlFloat (O, "choppiness",          Cfg.Ocean.Choppiness);
			YamlBool  (O, "vessel_motion",       Cfg.Ocean.bVesselMotion);
			YamlFloat (O, "vessel_motion_scale", Cfg.Ocean.VesselMotionScale);
			YamlFloat (O, "max_radius_km",       Cfg.Ocean.MaxRadiusKm);
			YamlFloat (O, "water_temperature_c", Cfg.Ocean.WaterTemperatureC);
			YamlString(O, "material",            Cfg.Ocean.MaterialPath);
		}

				// Thermal radiance for IR (ROADMAP 4A)
		if (YamlHas(Root, "thermal"))
		{
			ryml::ConstNodeRef T = Root["thermal"];
			YamlBool (T, "enabled",             Cfg.Thermal.bEnabled);
			YamlFloat(T, "air_temperature_c",   Cfg.Thermal.AirTemperatureC);
			YamlFloat(T, "air_diurnal_swing_k", Cfg.Thermal.AirDiurnalSwingK);
			if (YamlHas(T, "extinction_per_km"))
			{
				ryml::ConstNodeRef X = T["extinction_per_km"];
				YamlFloat(X, "mwir", Cfg.Thermal.ExtinctionPerKmMwir);
				YamlFloat(X, "lwir", Cfg.Thermal.ExtinctionPerKmLwir);
			}
			YamlFloat(T, "fog_ir_factor",       Cfg.Thermal.FogIrFactor);
			if (YamlHas(T, "materials"))
			{
				ryml::ConstNodeRef Ms = T["materials"];
				if (Ms.is_map())
				{
					YamlKeysAreData(Ms);   // class names; their fields are still checked
					for (ryml::ConstNodeRef MNode : Ms)
					{
						FThermalMaterialSpec Spec;
						Spec.Name = RymlToFString(MNode.key());
						if (MNode.is_map())
						{
							float V = 0.0f;
							if (YamlFloat(MNode, "albedo", V))           Spec.Albedo = V;
							if (YamlFloat(MNode, "emissivity", V))       Spec.Emissivity = V;
							if (YamlFloat(MNode, "thermal_inertia", V))  Spec.ThermalInertia = V;
							if (YamlFloat(MNode, "convection_w_m2k", V)) Spec.ConvectionWm2K = V;
							if (YamlFloat(MNode, "k_fast", V))           Spec.KFast = V;
							YamlString(MNode, "temperature", Spec.Temperature);
						}
						Cfg.Thermal.Materials.Add(MoveTemp(Spec));
					}
				}
			}
			if (YamlHas(T, "land_cover"))   // ROADMAP 4B
			{
				ryml::ConstNodeRef L = T["land_cover"];
				FCamSimConfig::FThermalConfig::FLandCoverConfig& LC = Cfg.Thermal.LandCover;
				YamlBool  (L, "enabled",           LC.bEnabled);
				YamlString(L, "dir",               LC.Dir);
				YamlInt   (L, "window_texels",     LC.WindowTexels);
				YamlFloat (L, "recentre_fraction", LC.RecentreFraction);
				YamlFloat (L, "veg_index_lo",      LC.VegIndexLo);
				YamlFloat (L, "veg_index_hi",      LC.VegIndexHi);
				YamlFloat (L, "asphalt_max_luma",  LC.AsphaltMaxLuma);
				YamlFloat (L, "warp_amplitude_m",  LC.WarpAmplitudeM);
				YamlFloat (L, "warp_cell_m",       LC.WarpCellM);
				YamlFloat (L, "veg_blur_m",        LC.VegBlurM);
				if (YamlHas(L, "classes"))
				{
					ryml::ConstNodeRef Cs = L["classes"];
					if (Cs.is_map())
					{
						YamlKeysAreData(Cs);   // WorldCover codes
						for (ryml::ConstNodeRef C : Cs)
						{
							FLandCoverClassSpec Spec;
							Spec.Key = RymlToFString(C.key());
							bool bDigits = Spec.Key.Len() > 0 && Spec.Key.Len() <= 3;
							for (const TCHAR Ch : Spec.Key) bDigits &= (Ch >= TEXT('0') && Ch <= TEXT('9'));
							Spec.Code = bDigits ? FCString::Atoi(*Spec.Key) : -1;
							if (C.has_val()) Spec.Material = RymlToFString(C.val());
							LC.Classes.Add(MoveTemp(Spec));
						}
					}
					else
					{
						Cfg.UnknownYamlKeys.AddUnique(TEXT("thermal.land_cover.classes (must be a map of code: material)"));
					}
				}
			}
			if (YamlHas(T, "entity"))   // ROADMAP 4C
			{
				ryml::ConstNodeRef En = T["entity"];
				FEntityThermalSettings& E = Cfg.Thermal.Entity;
				YamlBool (En, "enabled",           E.bEnabled);
				YamlFloat(En, "moving_mps",        E.MovingMps);
				YamlFloat(En, "idle_hold_s",       E.IdleHoldS);
				YamlFloat(En, "skin_running_k",    E.SkinRunningK);
				YamlFloat(En, "convection_v0_mps", E.ConvectionV0Mps);
				YamlFloat(En, "skin_tau_s",        E.SkinTauS);
				YamlFloat(En, "burn_k",            E.BurnK);
				YamlFloat(En, "burn_s",            E.BurnS);
				YamlFloat(En, "hull_cool_tau_s",   E.HullCoolTauS);
				struct FKindKey { c4::csubstr Key; EEntityThermalPartKind Kind; };
				const FKindKey Kinds[] = { { "engine", EEntityThermalPartKind::Engine }, { "exhaust", EEntityThermalPartKind::Exhaust },
					{ "running_gear", EEntityThermalPartKind::RunningGear } };
				for (const FKindKey& K : Kinds)
				{
					if (!YamlHas(En, K.Key)) continue;
					ryml::ConstNodeRef KN = En[K.Key];
					FEntityThermalKindParams& P = E.Kind(K.Kind);
					YamlFloat(KN, "delta_k",    P.DeltaK);
					YamlFloat(KN, "temp_k",     P.TempK);
					YamlFloat(KN, "k_per_mps",  P.KPerMps);
					YamlFloat(KN, "max_k",      P.MaxK);
					YamlFloat(KN, "tau_up_s",   P.TauUpS);
					YamlFloat(KN, "tau_down_s", P.TauDownS);
				}
			}
		}

		// Cesium backend: ion server, terrain source, imagery overlay
		if (YamlHas(Root, "cesium"))
		{
			ryml::ConstNodeRef Cs = Root["cesium"];
			YamlString(Cs, "ion_portal_url", Cfg.CesiumBackend.IonPortalUrl);
			YamlString(Cs, "ion_api_url",    Cfg.CesiumBackend.IonApiUrl);
			YamlString(Cs, "ion_token",      Cfg.CesiumBackend.IonToken);

			if (YamlHas(Cs, "terrain"))
			{
				ryml::ConstNodeRef Tr = Cs["terrain"];
				YamlString(Tr, "source",       Cfg.CesiumBackend.Terrain.Source);
				YamlInt   (Tr, "ion_asset_id", Cfg.CesiumBackend.Terrain.IonAssetId);
				YamlString(Tr, "url",          Cfg.CesiumBackend.Terrain.Url);
			}

			if (YamlHas(Cs, "imagery"))
			{
				ryml::ConstNodeRef Im = Cs["imagery"];
				YamlString(Im, "source",          Cfg.CesiumBackend.Imagery.Source);
				YamlInt   (Im, "ion_asset_id",    Cfg.CesiumBackend.Imagery.IonAssetId);
				YamlString(Im, "wms_url",         Cfg.CesiumBackend.Imagery.WmsUrl);
				YamlString(Im, "wms_layers",      Cfg.CesiumBackend.Imagery.WmsLayers);
				YamlInt   (Im, "wms_tile_width",  Cfg.CesiumBackend.Imagery.WmsTileWidth);
				YamlInt   (Im, "wms_tile_height", Cfg.CesiumBackend.Imagery.WmsTileHeight);
				YamlString(Im, "url",             Cfg.CesiumBackend.Imagery.Url);
				YamlDouble(Im, "maximum_screen_space_error",     Cfg.CesiumBackend.Imagery.MaximumScreenSpaceError);
				YamlInt   (Im, "maximum_texture_size",           Cfg.CesiumBackend.Imagery.MaximumTextureSize);
				YamlInt   (Im, "maximum_simultaneous_tile_loads", Cfg.CesiumBackend.Imagery.MaximumSimultaneousTileLoads);
			}
		}

		// REALISM R0: scene package
		if (YamlHas(Root, "scene"))
		{
			ryml::ConstNodeRef Sc = Root["scene"];
			YamlString(Sc, "dir",     Cfg.Scene.Dir);
			YamlBool  (Sc, "offline", Cfg.Scene.bOffline);
		}

		// Phase 21: DIS protocol config
		if (YamlHas(Root, "dis"))
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
			YamlInt   (D, "default_entity_type_id", Cfg.DIS.DefaultEntityTypeId);
			YamlBool  (D, "clamp_to_surface",       Cfg.DIS.bClampToSurface);

			// Entity type mappings: dis.entity_type_map
			if (YamlHas(D, "entity_type_map"))
			{
				ryml::ConstNodeRef MapNode = D["entity_type_map"];
				if (MapNode.is_map())
				{
					YamlKeysAreData(MapNode);
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

		// Phase 26: standards compliance config
		if (YamlHas(Root, "phase26"))
		{
			ryml::ConstNodeRef P = Root["phase26"];
			YamlString(P, "platform_tail_number",     Cfg.Phase26.PlatformTailNumber);
			YamlString(P, "mission_id",               Cfg.Phase26.MissionId);
			YamlString(P, "platform_designation",     Cfg.Phase26.PlatformDesignation);
			YamlString(P, "platform_call_sign",       Cfg.Phase26.PlatformCallSign);
			YamlBool  (P, "klv_full_range_attitude",  Cfg.Phase26.bKlvFullRangeAttitude);
			YamlFloat (P, "target_track_gate_width",  Cfg.Phase26.TargetTrackGateWidth);
			YamlFloat (P, "target_track_gate_height", Cfg.Phase26.TargetTrackGateHeight);
		}

		// Terrain readiness gate
		if (YamlHas(Root, "terrain_gate"))
		{
			ryml::ConstNodeRef G = Root["terrain_gate"];
			float TeleportM = static_cast<float>(Cfg.TerrainGate.TeleportDistanceM);
			YamlBool  (G, "enabled",              Cfg.TerrainGate.bEnabled);
			YamlFloat (G, "min_load_progress",    Cfg.TerrainGate.MinLoadProgressPct);
			YamlFloat (G, "timeout_sec",          Cfg.TerrainGate.TimeoutSec);
			YamlFloat (G, "teleport_distance_m",  TeleportM);
			Cfg.TerrainGate.TeleportDistanceM = TeleportM;
		}

		// Phase 22G: First-person view
		YamlInt  (Root, "fps_entity_id",    Cfg.FpsEntityId);
		YamlFloat(Root, "fps_eye_height_m", Cfg.FpsEyeHeightM);

		// Phase 28: operational config
		if (YamlHas(Root, "operational"))
		{
			ryml::ConstNodeRef OpNode = Root["operational"];
			YamlString(OpNode, "structured_log_path", Cfg.Operational.StructuredLogPath);
			YamlInt(OpNode, "structured_log_max_mb", Cfg.Operational.StructuredLogMaxMB);
			YamlBool(OpNode, "health_http_enabled", Cfg.Operational.bHealthHttpEnabled);
			YamlInt(OpNode, "health_http_port", Cfg.Operational.HealthHttpPort);
			YamlBool  (OpNode, "snapshot_endpoint_enabled", Cfg.Operational.bSnapshotEndpointEnabled);
			YamlString(OpNode, "frame_stats_path",          Cfg.Operational.FrameStatsPath);
		}

		// ROADMAP 3A: render path
		if (YamlHas(Root, "render"))
		{
			ryml::ConstNodeRef RNode = Root["render"];
			YamlFloat (RNode, "camera_cut_distance_m",   Cfg.Render.CameraCutDistanceM);
			YamlFloat (RNode, "camera_cut_angle_deg",    Cfg.Render.CameraCutAngleDeg);
			YamlDouble(RNode, "origin_shift_distance_m", Cfg.Render.OriginShiftDistanceM);
		}

		YamlReadElsewhere(Root, "entity_types");  // FEntityTypeTable

		if (Root.is_map())
		{
			CollectUnknownYamlKeys(Root, FString(), KeyTracker, Cfg.UnknownYamlKeys);
		}
		for (const FString& Key : Cfg.UnknownYamlKeys)
		{
			UE_LOG(LogCamSim, Warning, TEXT("Config: unknown key '%s' in %s is ignored (typo or removed setting?)"),
				*Key, *YamlPath);
		}

		UE_LOG(LogCamSim, Log, TEXT("Loaded config from %s"), *YamlPath);
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
	Cfg.CulledScreenSpaceError  = GetEnvFloat(TEXT("CAMSIM_CULLED_SSE"),          Cfg.CulledScreenSpaceError);
	Cfg.bFrustumCulling         = GetEnvBool (TEXT("CAMSIM_FRUSTUM_CULLING"),     Cfg.bFrustumCulling);
	Cfg.MaximumCachedBytesMB    = GetEnvInt(TEXT("CAMSIM_MAX_CACHED_MB"),         Cfg.MaximumCachedBytesMB);
	Cfg.LoadingDescendantLimit  = GetEnvInt  (TEXT("CAMSIM_LOADING_DESCENDANT_LIMIT"), Cfg.LoadingDescendantLimit);
	Cfg.bUseLodTransitions      = GetEnvBool (TEXT("CAMSIM_USE_LOD_TRANSITIONS"),      Cfg.bUseLodTransitions);
	Cfg.bCreatePhysicsMeshes    = GetEnvBool (TEXT("CAMSIM_CREATE_PHYSICS_MESHES"),    Cfg.bCreatePhysicsMeshes);
	Cfg.LodTransitionLength     = GetEnvFloat(TEXT("CAMSIM_LOD_TRANSITION_LENGTH"),    Cfg.LodTransitionLength);
	Cfg.Encoder = GetEnv(TEXT("CAMSIM_ENCODER"), Cfg.Encoder);
	Cfg.EncoderPref = ParseEncoderPreference(Cfg.Encoder);
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
	Cfg.StartDatetime  = GetEnv     (TEXT("CAMSIM_START_DATETIME"), Cfg.StartDatetime);
	Cfg.SimTimeRate    = GetEnvFloat(TEXT("CAMSIM_SIM_TIME_RATE"),  Cfg.SimTimeRate);

	// Phase 16 Sprint 2: per-mode env var overrides (applied to whichever mode has the feature)
	if (FSensorModeConfig* IrM = Cfg.SensorModeConfigs.Find(ESensorMode::IR))
	{
		IrM->AGCLagFrames = FMath::Clamp(GetEnvInt(TEXT("CAMSIM_IR_AGC_LAG_FRAMES"), IrM->AGCLagFrames), 0, 10);
	}

	Cfg.MLTraining.bEnabled = GetEnvInt(TEXT("CAMSIM_ML_ENABLED"), Cfg.MLTraining.bEnabled ? 1 : 0) != 0;
	Cfg.MLTraining.OutputDir = GetEnv(TEXT("CAMSIM_ML_OUTPUT_DIR"), Cfg.MLTraining.OutputDir);
	Cfg.MLTraining.bDepthMap = GetEnvInt(TEXT("CAMSIM_ML_DEPTH_ENABLED"), Cfg.MLTraining.bDepthMap ? 1 : 0) != 0;
	Cfg.MLTraining.bBoundingBoxes = GetEnvInt(TEXT("CAMSIM_ML_BBOX_ENABLED"), Cfg.MLTraining.bBoundingBoxes ? 1 : 0) != 0;
	Cfg.MLTraining.bCocoExport = GetEnvInt(TEXT("CAMSIM_ML_COCO_ENABLED"), Cfg.MLTraining.bCocoExport ? 1 : 0) != 0;
	Cfg.MLTraining.AnnotationIntervalFrames = FMath::Max(1, GetEnvInt(TEXT("CAMSIM_ML_INTERVAL_FRAMES"), Cfg.MLTraining.AnnotationIntervalFrames));
	Cfg.MLTraining.DepthFarPlaneM = GetEnvFloat(TEXT("CAMSIM_ML_DEPTH_FAR_PLANE_M"), Cfg.MLTraining.DepthFarPlaneM);
	Cfg.MLTraining.MinVisiblePixels = FMath::Max(1, GetEnvInt(TEXT("CAMSIM_ML_MIN_VISIBLE_PIXELS"), Cfg.MLTraining.MinVisiblePixels));
	Cfg.MLTraining.bSegmentation = GetEnvInt(TEXT("CAMSIM_ML_SEGMENTATION_ENABLED"), Cfg.MLTraining.bSegmentation ? 1 : 0) != 0;
	Cfg.EntityScale.MaxDrawDistanceM = GetEnvFloat(TEXT("CAMSIM_ENTITY_MAX_DRAW_DISTANCE_M"), Cfg.EntityScale.MaxDrawDistanceM);
	Cfg.EntityScale.TickRateHz = GetEnvFloat(TEXT("CAMSIM_ENTITY_TICK_RATE_HZ"), Cfg.EntityScale.TickRateHz);
	Cfg.EntityScale.DefaultMaxUpdateRateHz = GetEnvFloat(
		TEXT("CAMSIM_ENTITY_DEFAULT_MAX_UPDATE_RATE_HZ"), Cfg.EntityScale.DefaultMaxUpdateRateHz);

	// Camera platform and gimbal (HITL profile without editing the yaml)
	Cfg.CameraEntityId             = GetEnvInt  (TEXT("CAMSIM_CAMERA_ENTITY_ID"),     Cfg.CameraEntityId);
	Cfg.GimbalMaxSlewRateDegPerSec = GetEnvFloat(TEXT("CAMSIM_GIMBAL_MAX_SLEW_RATE"), Cfg.GimbalMaxSlewRateDegPerSec);
	Cfg.GimbalPitchMin             = GetEnvFloat(TEXT("CAMSIM_GIMBAL_PITCH_MIN"),     Cfg.GimbalPitchMin);
	Cfg.GimbalPitchMax             = GetEnvFloat(TEXT("CAMSIM_GIMBAL_PITCH_MAX"),     Cfg.GimbalPitchMax);
	Cfg.GimbalYawMin               = GetEnvFloat(TEXT("CAMSIM_GIMBAL_YAW_MIN"),       Cfg.GimbalYawMin);
	Cfg.GimbalYawMax               = GetEnvFloat(TEXT("CAMSIM_GIMBAL_YAW_MAX"),       Cfg.GimbalYawMax);
	{
		// Comma-separated degrees, wide to narrow ("60,20,5"); "none", "off" or "[]" disables
		// the presets (Sensor Control Gain ignored; FOV from View Definition only).
		FString Presets = GetEnv(TEXT("CAMSIM_SENSOR_FOV_PRESETS"), FString()).TrimStartAndEnd();
		if (!Presets.IsEmpty())
		{
			Presets.RemoveFromStart(TEXT("["));
			Presets.RemoveFromEnd(TEXT("]"));
			Presets.TrimStartAndEndInline();
			Cfg.SensorFovPresets.Empty();
			if (!Presets.Equals(TEXT("none"), ESearchCase::IgnoreCase) && !Presets.Equals(TEXT("off"), ESearchCase::IgnoreCase))
			{
				TArray<FString> Items;
				Presets.ParseIntoArray(Items, TEXT(","), /*bCullEmpty=*/true);
				for (const FString& Item : Items)
				{
					Cfg.SensorFovPresets.Add(FCString::Atof(*Item.TrimStartAndEnd()));
				}
			}
		}
	}

	// Phase 22G: FPS view
	Cfg.FpsEntityId   = GetEnvInt  (TEXT("CAMSIM_FPS_ENTITY_ID"),    Cfg.FpsEntityId);
	Cfg.FpsEyeHeightM = GetEnvFloat(TEXT("CAMSIM_FPS_EYE_HEIGHT_M"), Cfg.FpsEyeHeightM);

	// Phase 12A: security metadata env overrides
	Cfg.SecurityMetadata.Classification = GetEnv(TEXT("CAMSIM_SECURITY_CLASSIFICATION"), Cfg.SecurityMetadata.Classification);
	Cfg.SecurityMetadata.ClassifyingCountry = GetEnv(TEXT("CAMSIM_SECURITY_CLASSIFYING_COUNTRY"), Cfg.SecurityMetadata.ClassifyingCountry);
	Cfg.SecurityMetadata.ObjectCountryCodes = GetEnv(TEXT("CAMSIM_SECURITY_OBJECT_COUNTRY"), Cfg.SecurityMetadata.ObjectCountryCodes);

	// Phase 12B: video codec
	Cfg.VideoCodec = GetEnv(TEXT("CAMSIM_VIDEO_CODEC"), Cfg.VideoCodec);

	// Phase 12E: recording/playback
	Cfg.Recording.CigiRecordPath = GetEnv(TEXT("CAMSIM_CIGI_RECORD_PATH"), Cfg.Recording.CigiRecordPath);
	Cfg.Recording.VideoRecordPath = GetEnv(TEXT("CAMSIM_VIDEO_RECORD_PATH"), Cfg.Recording.VideoRecordPath);
	Cfg.Recording.CigiPlaybackPath = GetEnv(TEXT("CAMSIM_CIGI_PLAYBACK_PATH"), Cfg.Recording.CigiPlaybackPath);

	// Phase 18: weather & atmosphere env overrides
	Cfg.Phase18.bSecondFog       = GetEnvInt(TEXT("CAMSIM_SECOND_FOG"),       Cfg.Phase18.bSecondFog       ? 1 : 0) != 0;
	Cfg.Phase18.VisibilityRangeM = GetEnvFloat(TEXT("CAMSIM_VISIBILITY_RANGE_M"), Cfg.Phase18.VisibilityRangeM);
	Cfg.Phase18.bVolumetricClouds   = GetEnvInt(TEXT("CAMSIM_VOLUMETRIC_CLOUDS"),      Cfg.Phase18.bVolumetricClouds   ? 1 : 0) != 0;
	Cfg.Phase18.CloudShadowStrength = GetEnvFloat(TEXT("CAMSIM_CLOUD_SHADOW_STRENGTH"),Cfg.Phase18.CloudShadowStrength);

	// Ocean (ROADMAP 2.6)
	Cfg.Ocean.bEnabled          = GetEnvBool (TEXT("CAMSIM_OCEAN_ENABLED"),        Cfg.Ocean.bEnabled);
	Cfg.Ocean.Beaufort          = GetEnvFloat(TEXT("CAMSIM_OCEAN_BEAUFORT"),       Cfg.Ocean.Beaufort);
	Cfg.Ocean.WaveDirectionDeg  = GetEnvFloat(TEXT("CAMSIM_OCEAN_WAVE_DIR"),       Cfg.Ocean.WaveDirectionDeg);
	Cfg.Ocean.Choppiness        = GetEnvFloat(TEXT("CAMSIM_OCEAN_CHOPPINESS"),     Cfg.Ocean.Choppiness);
	Cfg.Ocean.bVesselMotion     = GetEnvBool (TEXT("CAMSIM_OCEAN_MOTION_ENABLED"), Cfg.Ocean.bVesselMotion);
	Cfg.Ocean.VesselMotionScale = GetEnvFloat(TEXT("CAMSIM_OCEAN_MOTION_SCALE"),   Cfg.Ocean.VesselMotionScale);
	Cfg.Ocean.MaxRadiusKm       = GetEnvFloat(TEXT("CAMSIM_OCEAN_MAX_RADIUS_KM"),  Cfg.Ocean.MaxRadiusKm);

		// Thermal (ROADMAP 4A)
	Cfg.Thermal.bEnabled            = GetEnvBool (TEXT("CAMSIM_THERMAL_ENABLED"),             Cfg.Thermal.bEnabled);
	Cfg.Thermal.AirTemperatureC     = GetEnvFloat(TEXT("CAMSIM_THERMAL_AIR_TEMPERATURE_C"),   Cfg.Thermal.AirTemperatureC);
	Cfg.Thermal.AirDiurnalSwingK    = GetEnvFloat(TEXT("CAMSIM_THERMAL_AIR_DIURNAL_SWING_K"), Cfg.Thermal.AirDiurnalSwingK);
	Cfg.Thermal.ExtinctionPerKmMwir = GetEnvFloat(TEXT("CAMSIM_THERMAL_EXTINCTION_MWIR"),     Cfg.Thermal.ExtinctionPerKmMwir);
	Cfg.Thermal.ExtinctionPerKmLwir = GetEnvFloat(TEXT("CAMSIM_THERMAL_EXTINCTION_LWIR"),     Cfg.Thermal.ExtinctionPerKmLwir);
	Cfg.Thermal.FogIrFactor         = GetEnvFloat(TEXT("CAMSIM_THERMAL_FOG_IR_FACTOR"),       Cfg.Thermal.FogIrFactor);
	// Land cover (ROADMAP 4B)
	FCamSimConfig::FThermalConfig::FLandCoverConfig& LC = Cfg.Thermal.LandCover;
	// Entity thermal state (ROADMAP 4C)
	Cfg.Thermal.Entity.bEnabled = GetEnvBool(TEXT("CAMSIM_THERMAL_ENTITY_ENABLED"), Cfg.Thermal.Entity.bEnabled);
	LC.bEnabled         = GetEnvBool (TEXT("CAMSIM_THERMAL_LAND_COVER_ENABLED"),           LC.bEnabled);
	LC.Dir              = GetEnv     (TEXT("CAMSIM_THERMAL_LAND_COVER_DIR"),               LC.Dir);
	LC.WindowTexels     = GetEnvInt  (TEXT("CAMSIM_THERMAL_LAND_COVER_WINDOW_TEXELS"),     LC.WindowTexels);
	LC.RecentreFraction = GetEnvFloat(TEXT("CAMSIM_THERMAL_LAND_COVER_RECENTRE_FRACTION"), LC.RecentreFraction);
	LC.VegIndexLo       = GetEnvFloat(TEXT("CAMSIM_THERMAL_LAND_COVER_VEG_INDEX_LO"),      LC.VegIndexLo);
	LC.VegIndexHi       = GetEnvFloat(TEXT("CAMSIM_THERMAL_LAND_COVER_VEG_INDEX_HI"),      LC.VegIndexHi);
	LC.AsphaltMaxLuma   = GetEnvFloat(TEXT("CAMSIM_THERMAL_LAND_COVER_ASPHALT_MAX_LUMA"),  LC.AsphaltMaxLuma);
	LC.WarpAmplitudeM   = GetEnvFloat(TEXT("CAMSIM_THERMAL_LAND_COVER_WARP_AMPLITUDE_M"),  LC.WarpAmplitudeM);
	LC.WarpCellM        = GetEnvFloat(TEXT("CAMSIM_THERMAL_LAND_COVER_WARP_CELL_M"),       LC.WarpCellM);
	LC.VegBlurM         = GetEnvFloat(TEXT("CAMSIM_THERMAL_LAND_COVER_VEG_BLUR_M"),        LC.VegBlurM);

	// ROADMAP 4A acceptance: MWIR/LWIR and 1080p runs without editing the yaml.
	Cfg.CaptureWidth  = GetEnvInt(TEXT("CAMSIM_CAPTURE_WIDTH"),  Cfg.CaptureWidth);
	Cfg.CaptureHeight = GetEnvInt(TEXT("CAMSIM_CAPTURE_HEIGHT"), Cfg.CaptureHeight);
	Cfg.Ocean.WaterTemperatureC = GetEnvFloat(TEXT("CAMSIM_OCEAN_WATER_TEMPERATURE_C"), Cfg.Ocean.WaterTemperatureC);
	{
		const FString IrPreset = GetEnv(TEXT("CAMSIM_IR_PRESET"), FString());
		FSensorModeConfig* IrM = Cfg.SensorModeConfigs.Find(ESensorMode::IR);
		if (!IrPreset.IsEmpty() && IrM)
		{
			// Re-applies the whole preset (yaml optics:/detector: overrides are dropped); an unknown
			// name keeps the current values and Validate() reports it.
			IrM->Preset = IrPreset;
			CamSimSensorPresets::Apply(IrPreset, *IrM);
		}
	}

	// Phase 24: rendering quality env var overrides
	{
		FRenderingQualityConfig& RQ = Cfg.RenderingQuality;
		RQ.bEntityShadows        = GetEnvInt  (TEXT("CAMSIM_ENTITY_SHADOWS"),          RQ.bEntityShadows        ? 1 : 0) != 0;
		RQ.bContactShadows       = GetEnvInt  (TEXT("CAMSIM_CONTACT_SHADOWS"),         RQ.bContactShadows       ? 1 : 0) != 0;
		RQ.ContactShadowLength   = GetEnvFloat(TEXT("CAMSIM_CONTACT_SHADOW_LENGTH"),   RQ.ContactShadowLength);
		RQ.AOIntensity           = GetEnvFloat(TEXT("CAMSIM_AO_INTENSITY"),            RQ.AOIntensity);
		RQ.AORadius              = GetEnvFloat(TEXT("CAMSIM_AO_RADIUS"),               RQ.AORadius);
		RQ.bRayTracingEnabled    = GetEnvInt  (TEXT("CAMSIM_RT_ENABLED"),              RQ.bRayTracingEnabled    ? 1 : 0) != 0;
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
	Cfg.CesiumBackend.Imagery.Url        = GetEnv(TEXT("CAMSIM_CESIUM_IMAGERY_URL"), Cfg.CesiumBackend.Imagery.Url);
	Cfg.Scene.Dir      = GetEnv    (TEXT("CAMSIM_SCENE_DIR"),     Cfg.Scene.Dir);
	Cfg.Scene.bOffline = GetEnvBool(TEXT("CAMSIM_SCENE_OFFLINE"), Cfg.Scene.bOffline);
	Cfg.CesiumBackend.Imagery.MaximumScreenSpaceError      = GetEnvDouble(TEXT("CAMSIM_CESIUM_IMAGERY_MAX_SSE"),      Cfg.CesiumBackend.Imagery.MaximumScreenSpaceError);
	Cfg.CesiumBackend.Imagery.MaximumTextureSize          = GetEnvInt(TEXT("CAMSIM_CESIUM_IMAGERY_MAX_TEXTURE_SIZE"), Cfg.CesiumBackend.Imagery.MaximumTextureSize);
	Cfg.CesiumBackend.Imagery.MaximumSimultaneousTileLoads = GetEnvInt(TEXT("CAMSIM_CESIUM_IMAGERY_MAX_TILE_LOADS"),  Cfg.CesiumBackend.Imagery.MaximumSimultaneousTileLoads);

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
		TEXT("SSE=%.1f CacheMB=%d MaxEntities=%d ")
		TEXT("TerrainProvider=%s ImageryProvider=%s ")
		TEXT("EntityScale(draw=%.1fm tick=%.1fHz pose_cap=%.1fHz)"),
		*Cfg.CigiBindAddr, Cfg.CigiPort,
		*Cfg.MulticastAddr, Cfg.MulticastPort,
		Cfg.VideoBitrate, *Cfg.H264Preset, *Cfg.Encoder,
		Cfg.ReadbackReadyPolls, Cfg.EncoderWatchdogIntervalTicks,
		Cfg.MaximumScreenSpaceError, Cfg.MaximumCachedBytesMB, Cfg.MaxEntities,
		*Cfg.TerrainProvider, *Cfg.ImageryProvider,
		Cfg.EntityScale.MaxDrawDistanceM, Cfg.EntityScale.TickRateHz, Cfg.EntityScale.DefaultMaxUpdateRateHz);

	// Phase 27: Performance env overrides
	{
		FPerformanceConfig& Perf = Cfg.Performance;
		Perf.TexturePoolBudgetMB                 = GetEnvInt  (TEXT("CAMSIM_PERF_TEXTURE_POOL_MB"),     Perf.TexturePoolBudgetMB);
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
		D.DefaultEntityTypeId = GetEnvInt  (TEXT("CAMSIM_DIS_DEFAULT_ENTITY_TYPE"), D.DefaultEntityTypeId);
		D.bClampToSurface     = GetEnvInt  (TEXT("CAMSIM_DIS_CLAMP_TO_SURFACE"),  D.bClampToSurface ? 1 : 0) != 0;
	}

	// Phase 26: standards compliance env var overrides
	{
		FPhase26Config& P = Cfg.Phase26;
		P.PlatformTailNumber    = GetEnv     (TEXT("CAMSIM_PLATFORM_TAIL_NUMBER"),      P.PlatformTailNumber);
		P.MissionId             = GetEnv     (TEXT("CAMSIM_MISSION_ID"),                P.MissionId);
		P.PlatformDesignation   = GetEnv     (TEXT("CAMSIM_PLATFORM_DESIGNATION"),      P.PlatformDesignation);
		P.PlatformCallSign      = GetEnv     (TEXT("CAMSIM_PLATFORM_CALL_SIGN"),        P.PlatformCallSign);
		P.bKlvFullRangeAttitude = GetEnvBool (TEXT("CAMSIM_KLV_FULL_RANGE_ATTITUDE"),   P.bKlvFullRangeAttitude);
		P.TargetTrackGateWidth  = GetEnvFloat(TEXT("CAMSIM_TARGET_TRACK_GATE_WIDTH"),   P.TargetTrackGateWidth);
		P.TargetTrackGateHeight = GetEnvFloat(TEXT("CAMSIM_TARGET_TRACK_GATE_HEIGHT"),  P.TargetTrackGateHeight);
	}

	// Terrain readiness gate env var overrides
	{
		FCamSimConfig::FTerrainGateConfig& G = Cfg.TerrainGate;
		G.bEnabled           = GetEnvBool (TEXT("CAMSIM_TERRAIN_GATE_ENABLED"),           G.bEnabled);
		G.MinLoadProgressPct = GetEnvFloat(TEXT("CAMSIM_TERRAIN_GATE_MIN_LOAD_PROGRESS"), G.MinLoadProgressPct);
		G.TimeoutSec         = GetEnvFloat(TEXT("CAMSIM_TERRAIN_GATE_TIMEOUT_SEC"),       G.TimeoutSec);
		G.TeleportDistanceM  = GetEnvFloat(TEXT("CAMSIM_TERRAIN_GATE_TELEPORT_M"),
		                                   static_cast<float>(G.TeleportDistanceM));
	}

	// Phase 28: operational env var overrides
	Cfg.Operational.StructuredLogPath     = GetEnv    (TEXT("CAMSIM_STRUCTURED_LOG_PATH"),   Cfg.Operational.StructuredLogPath);
	Cfg.Operational.StructuredLogMaxMB    = GetEnvInt (TEXT("CAMSIM_STRUCTURED_LOG_MAX_MB"), Cfg.Operational.StructuredLogMaxMB);
	Cfg.Operational.bHealthHttpEnabled    = GetEnvBool(TEXT("CAMSIM_HEALTH_HTTP_ENABLED"),   Cfg.Operational.bHealthHttpEnabled);
	Cfg.Operational.HealthHttpPort        = GetEnvInt (TEXT("CAMSIM_HEALTH_HTTP_PORT"),      Cfg.Operational.HealthHttpPort);
	Cfg.Operational.bSnapshotEndpointEnabled = GetEnvBool(TEXT("CAMSIM_SNAPSHOT_ENDPOINT_ENABLED"), Cfg.Operational.bSnapshotEndpointEnabled);
	Cfg.Operational.FrameStatsPath           = GetEnv    (TEXT("CAMSIM_FRAME_STATS_PATH"),          Cfg.Operational.FrameStatsPath);
	// A relative path means the directory CamSim was launched from; UE would
	// otherwise resolve it against the engine's Binaries directory.
	if (!Cfg.Operational.FrameStatsPath.IsEmpty() && FPaths::IsRelative(Cfg.Operational.FrameStatsPath))
	{
		Cfg.Operational.FrameStatsPath = FPaths::ConvertRelativePathToFull(FPaths::LaunchDir(), Cfg.Operational.FrameStatsPath);
	}

	// ROADMAP 3A: render path env overrides.
	Cfg.Render.CameraCutDistanceM   = GetEnvFloat (TEXT("CAMSIM_RENDER_CAMERA_CUT_DISTANCE_M"),   Cfg.Render.CameraCutDistanceM);
	Cfg.Render.CameraCutAngleDeg    = GetEnvFloat (TEXT("CAMSIM_RENDER_CAMERA_CUT_ANGLE_DEG"),    Cfg.Render.CameraCutAngleDeg);
	Cfg.Render.OriginShiftDistanceM = GetEnvDouble(TEXT("CAMSIM_RENDER_ORIGIN_SHIFT_DISTANCE_M"), Cfg.Render.OriginShiftDistanceM);

	Cfg.Performance.bTrackPipelineLatency = GetEnvBool(TEXT("CAMSIM_TRACK_PIPELINE_LATENCY"), Cfg.Performance.bTrackPipelineLatency);

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

// ---------------------------------------------------------------------------
// Phase 28D: Config Validation
// ---------------------------------------------------------------------------

TArray<FString> FCamSimConfig::Validate() const
{
	TArray<FString> Errors;

	auto RangeCheckInt = [&](const TCHAR* Name, int32 Value, int32 Min, int32 Max)
	{
		if (Value < Min || Value > Max)
		{
			Errors.Add(FString::Printf(TEXT("%s=%d out of range [%d, %d]"), Name, Value, Min, Max));
		}
	};

	auto RangeCheckFloat = [&](const TCHAR* Name, float Value, float Min, float Max)
	{
		if (Value < Min || Value > Max)
		{
			Errors.Add(FString::Printf(TEXT("%s=%.2f out of range [%.1f, %.1f]"), Name, Value, Min, Max));
		}
	};

	// Resolution — must be in range AND even (H.264 requires even dimensions)
	RangeCheckInt(TEXT("CaptureWidth"), CaptureWidth, 64, 7680);
	RangeCheckInt(TEXT("CaptureHeight"), CaptureHeight, 64, 4320);
	if (CaptureWidth % 2 != 0)
	{
		Errors.Add(FString::Printf(TEXT("CaptureWidth=%d must be even (H.264 requirement)"), CaptureWidth));
	}
	if (CaptureHeight % 2 != 0)
	{
		Errors.Add(FString::Printf(TEXT("CaptureHeight=%d must be even (H.264 requirement)"), CaptureHeight));
	}

	// NV12 packing writes 4 luma bytes per uint (ROADMAP 3B): the sensor graph needs width % 4 == 0.
	if (CaptureWidth % 4 != 0)
	{
		Errors.Add(FString::Printf(TEXT("CaptureWidth=%d must be a multiple of 4 (NV12 packing, GPU sensor graph)"), CaptureWidth));
	}
	for (const TPair<ESensorMode, FSensorModeConfig>& Pair : SensorModeConfigs)
	{
		const FSensorModeConfig& M = Pair.Value;
		const int32 ModeId = static_cast<int32>(Pair.Key);
		// log2 floor: 2^-40 keeps the gain (and AutoExposureBias) finite; NaN fails too.
		if (!(M.Exposure.MinGainEv >= -40.0f))
		{
			Errors.Add(FString::Printf(TEXT("sensor_modes[%d].exposure: min_gain_ev (%.1f) must be >= -40"),
				ModeId, M.Exposure.MinGainEv));
		}
		if (M.Exposure.MinGainEv > M.Exposure.MaxPhotonGainEv)
		{
			Errors.Add(FString::Printf(TEXT("sensor_modes[%d].exposure: min_gain_ev (%.1f) > max_photon_gain_ev (%.1f)"),
				ModeId, M.Exposure.MinGainEv, M.Exposure.MaxPhotonGainEv));
		}
		if (M.Exposure.HighlightPercentile <= 0.0f || M.Exposure.HighlightPercentile > 1.0f)
		{
			Errors.Add(FString::Printf(TEXT("sensor_modes[%d].exposure: highlight_percentile=%.3f out of (0, 1]"),
				ModeId, M.Exposure.HighlightPercentile));
		}
		// ROADMAP 4A: thermal radiance AE, AGC cap, band
		if (!(M.ThermalExposure.MinGainEv >= -40.0f))
			Errors.Add(FString::Printf(TEXT("sensor_modes[%d].thermal_exposure: min_gain_ev (%.1f) must be >= -40"), ModeId, M.ThermalExposure.MinGainEv));
		if (!(M.ThermalExposure.MaxPhotonGainEv >= M.ThermalExposure.MinGainEv))
			Errors.Add(FString::Printf(TEXT("sensor_modes[%d].thermal_exposure: min_gain_ev (%.1f) > max_photon_gain_ev (%.1f)"),
				ModeId, M.ThermalExposure.MinGainEv, M.ThermalExposure.MaxPhotonGainEv));
		if (!(M.ThermalExposure.HighlightPercentile > 0.0f && M.ThermalExposure.HighlightPercentile <= 1.0f))
			Errors.Add(FString::Printf(TEXT("sensor_modes[%d].thermal_exposure: highlight_percentile=%.3f out of (0, 1]"), ModeId, M.ThermalExposure.HighlightPercentile));
		if (!(M.AGCMaxDisplayGain >= 1.0f) || !FMath::IsFinite(M.AGCMaxDisplayGain))
			Errors.Add(FString::Printf(TEXT("sensor_modes[%d].agc_max_display_gain=%.2f must be finite and >= 1"), ModeId, M.AGCMaxDisplayGain));
		if (!(M.Detector.BandLoUm > 0.0f && M.Detector.BandHiUm > M.Detector.BandLoUm && M.Detector.BandHiUm <= 30.0f))
			Errors.Add(FString::Printf(TEXT("sensor_modes[%d].detector: band_lo_um (%.2f) / band_hi_um (%.2f) must satisfy 0 < lo < hi <= 30"),
				ModeId, M.Detector.BandLoUm, M.Detector.BandHiUm));
		if (M.AGCLowPercentile < 0.0f || M.AGCHighPercentile > 1.0f || M.AGCLowPercentile >= M.AGCHighPercentile)
		{
			Errors.Add(FString::Printf(TEXT("sensor_modes[%d]: agc_low_percentile (%.3f) / agc_high_percentile (%.3f) must satisfy 0 <= low < high <= 1"),
				ModeId, M.AGCLowPercentile, M.AGCHighPercentile));
		}

		// ROADMAP 3B.2 Task 5: preset name + optics/detector ranges.
		if (!M.Preset.IsEmpty())
		{
			FSensorModeConfig Scratch;
			if (!CamSimSensorPresets::Apply(M.Preset, Scratch))
			{
				Errors.Add(FString::Printf(
					TEXT("sensor_modes[%d].preset '%s' is not a known preset (eo_hd_cmos, mwir_cooled, lwir_uncooled)"),
					ModeId, *M.Preset));
			}
		}
		// Range checks are written !(in range) so a NaN (e.g. ".nan" in yaml) is rejected, never passed.
		if (!M.DetectorTypeName.IsEmpty()
			&& !M.DetectorTypeName.Equals(TEXT("photon"), ESearchCase::IgnoreCase)
			&& !M.DetectorTypeName.Equals(TEXT("microbolometer"), ESearchCase::IgnoreCase))
		{
			Errors.Add(FString::Printf(TEXT("sensor_modes[%d].detector.type '%s' is not a known detector type (photon, microbolometer)"),
				ModeId, *M.DetectorTypeName));
		}
		if (!(M.Optics.FNumber > 0.0f))
		{
			Errors.Add(FString::Printf(TEXT("sensor_modes[%d].optics.f_number=%.2f must be > 0"), ModeId, M.Optics.FNumber));
		}
		if (!(FMath::Abs(M.Optics.K1) <= 1.0f))
		{
			Errors.Add(FString::Printf(TEXT("sensor_modes[%d].optics.k1=%.3f must be in [-1, 1]"), ModeId, M.Optics.K1));
		}
		if (!(FMath::Abs(M.Optics.K2) <= 1.0f))
		{
			Errors.Add(FString::Printf(TEXT("sensor_modes[%d].optics.k2=%.3f must be in [-1, 1]"), ModeId, M.Optics.K2));
		}
		if (!(M.Optics.PixelPitchUm > 0.0f) || !(M.Optics.WavelengthUm > 0.0f) || !(M.Optics.ExtraBlurPx >= 0.0f))
		{
			Errors.Add(FString::Printf(
				TEXT("sensor_modes[%d].optics: pixel_pitch_um (%.3f) and wavelength_um (%.3f) must be > 0, extra_blur_px (%.3f) >= 0"),
				ModeId, M.Optics.PixelPitchUm, M.Optics.WavelengthUm, M.Optics.ExtraBlurPx));
		}
		if (!(M.Optics.VignettingExponent >= 0.0f && M.Optics.VignettingExponent <= 8.0f))
		{
			Errors.Add(FString::Printf(TEXT("sensor_modes[%d].optics.vignetting_exponent=%.2f out of range [0, 8]"),
				ModeId, M.Optics.VignettingExponent));
		}
		// The GPU inverts the distortion with a fixed Newton recurrence: it must converge for every
		// distorted radius in the frame (rd over [0, corner]) at the configured HFOV. A zoomed-out live
		// FOV that breaks it falls back to no distortion at runtime (CamSimOptics::SetOptics).
		if (HFovDeg > 0.0f && HFovDeg < 180.0f && CaptureWidth > 0 && CaptureHeight > 0
			&& !CamSimOptics::DistortionConverges(CaptureWidth, CaptureHeight, HFovDeg, M.Optics.K1, M.Optics.K2))
		{
			Errors.Add(FString::Printf(
				TEXT("sensor_modes[%d].optics: distortion k1=%.3f k2=%.3f does not converge out to the frame corner "
				     "(Newton inverse fails for some rd <= %.3f at hfov_deg %.1f, %dx%d); reduce |k1|/|k2|"),
				ModeId, M.Optics.K1, M.Optics.K2,
				CamSimOptics::CornerRadius(CaptureWidth, CaptureHeight, CamSimOptics::FocalPx(CaptureWidth, HFovDeg)),
				HFovDeg, CaptureWidth, CaptureHeight));
		}
		if (!(M.Detector.FullWellE > 0.0f))
		{
			Errors.Add(FString::Printf(TEXT("sensor_modes[%d].detector.full_well_e=%.1f must be > 0"), ModeId, M.Detector.FullWellE));
		}
		if (M.Detector.AdcBits < 8 || M.Detector.AdcBits > 16)
		{
			Errors.Add(FString::Printf(TEXT("sensor_modes[%d].detector.adc_bits=%d out of range [8, 16]"), ModeId, M.Detector.AdcBits));
		}
		auto CheckNonNegative = [&](const TCHAR* FieldName, float Value)
		{
			if (!(Value >= 0.0f))
			{
				Errors.Add(FString::Printf(TEXT("sensor_modes[%d].detector.%s=%.3f must be >= 0"), ModeId, FieldName, Value));
			}
		};
		CheckNonNegative(TEXT("read_noise_e"),      M.Detector.ReadNoiseE);
		CheckNonNegative(TEXT("prnu"),              M.Detector.Prnu);
		CheckNonNegative(TEXT("dsnu_e"),            M.Detector.DsnuE);
		CheckNonNegative(TEXT("dark_current_e_s"),  M.Detector.DarkCurrentEs);
		CheckNonNegative(TEXT("max_analog_gain_db"),M.Detector.MaxAnalogGainDb);
		CheckNonNegative(TEXT("temporal_noise"),    M.Detector.TemporalNoise);
		CheckNonNegative(TEXT("pixel_fpn"),         M.Detector.PixelFpn);
		CheckNonNegative(TEXT("column_fpn"),        M.Detector.ColumnFpn);
		CheckNonNegative(TEXT("row_fpn"),           M.Detector.RowFpn);
		if (!(M.Detector.HotPixelFraction >= 0.0f && M.Detector.HotPixelFraction <= 0.01f))
		{
			Errors.Add(FString::Printf(TEXT("sensor_modes[%d].detector.hot_pixel_fraction=%.5f out of range [0, 0.01]"), ModeId, M.Detector.HotPixelFraction));
		}
		if (!(M.Detector.DeadPixelFraction >= 0.0f && M.Detector.DeadPixelFraction <= 0.01f))
		{
			Errors.Add(FString::Printf(TEXT("sensor_modes[%d].detector.dead_pixel_fraction=%.5f out of range [0, 0.01]"), ModeId, M.Detector.DeadPixelFraction));
		}
	}

	// Video
	RangeCheckInt(TEXT("VideoBitrate"), VideoBitrate, 100000, 100000000);
	RangeCheckFloat(TEXT("FrameRate"), FrameRate, 1.0f, 120.0f);

	// Ports (0 = disabled)
	if (CigiPort != 0) RangeCheckInt(TEXT("CigiPort"), CigiPort, 1, 65535);
	if (CigiResponsePort != 0) RangeCheckInt(TEXT("CigiResponsePort"), CigiResponsePort, 1, 65535);
	RangeCheckInt(TEXT("MulticastPort"), MulticastPort, 1, 65535);

	// FOV
	if (HFovDeg <= 0.0f || HFovDeg > 180.0f)
	{
		Errors.Add(FString::Printf(TEXT("HFovDeg=%.2f out of range (0, 180]"), HFovDeg));
	}

	// Gimbal limits
	if (GimbalPitchMin >= GimbalPitchMax)
	{
		Errors.Add(FString::Printf(TEXT("GimbalPitchMin (%.1f) >= GimbalPitchMax (%.1f)"),
			GimbalPitchMin, GimbalPitchMax));
	}
	if (GimbalYawMin >= GimbalYawMax)
	{
		Errors.Add(FString::Printf(TEXT("GimbalYawMin (%.1f) >= GimbalYawMax (%.1f)"),
			GimbalYawMin, GimbalYawMax));
	}

	// Entities
	RangeCheckInt(TEXT("MaxEntities"), MaxEntities, 1, 10000);

	// Time
	if (StartHour >= 0.0f) RangeCheckFloat(TEXT("StartHour"), StartHour, 0.0f, 24.0f);
	RangeCheckFloat(TEXT("SimTimeRate"), SimTimeRate, 0.0f, 10000.0f);
	if (!StartDatetime.IsEmpty())
	{
		FDateTime Parsed;
		if (!FDateTime::ParseIso8601(*StartDatetime, Parsed))
		{
			Errors.Add(FString::Printf(TEXT("StartDatetime '%s' is not ISO 8601 (e.g. 2025-03-01T06:30:00Z)"), *StartDatetime));
		}
	}

	// Watchdog
	RangeCheckInt(TEXT("WatchdogMaxReconnects"), WatchdogMaxReconnects, 0, 100);
	RangeCheckInt(TEXT("EncoderWatchdogIntervalTicks"), EncoderWatchdogIntervalTicks, 30, 9000);

	// Readback
	RangeCheckInt(TEXT("ReadbackReadyPolls"), ReadbackReadyPolls, 0, 10);

	// Codec enum
	{
		const FString Lower = VideoCodec.ToLower();
		if (Lower != TEXT("h264") && Lower != TEXT("h265"))
		{
			Errors.Add(FString::Printf(TEXT("VideoCodec='%s' must be h264 or h265"), *VideoCodec));
		}
	}

	// Encoder enum
	{
		const FString Lower = Encoder.ToLower();
		if (Lower != TEXT("auto") && Lower != TEXT("nvenc") &&
		    Lower != TEXT("libx264") && Lower != TEXT("libx265"))
		{
			Errors.Add(FString::Printf(TEXT("Encoder='%s' must be auto, nvenc, libx264, or libx265"), *Encoder));
		}
	}

	// Performance
	// A non-positive threshold would cut every frame; ShouldCutCamera skips it instead.
	if (Render.CameraCutDistanceM <= 0.0f)
	{
		Errors.Add(FString::Printf(TEXT("render.camera_cut_distance_m=%.2f must be > 0 (check skipped)"), Render.CameraCutDistanceM));
	}
	if (Render.CameraCutAngleDeg <= 0.0f)
	{
		Errors.Add(FString::Printf(TEXT("render.camera_cut_angle_deg=%.2f must be > 0 (check skipped)"), Render.CameraCutAngleDeg));
	}

	// Ocean (ROADMAP 2.6). Written !(x in range) so NaN is reported too.
	if (!(Ocean.MaxRadiusKm > 0.0f) || !FMath::IsFinite(Ocean.MaxRadiusKm))
	{
		Errors.Add(FString::Printf(TEXT("ocean.max_radius_km=%.2f must be finite and > 0"), Ocean.MaxRadiusKm));
	}
	if (!(Ocean.Beaufort >= 0.0f && Ocean.Beaufort <= 12.0f))
	{
		Errors.Add(FString::Printf(TEXT("ocean.beaufort=%.2f out of range [0, 12]"), Ocean.Beaufort));
	}
	if (!(Ocean.Choppiness >= 0.0f && Ocean.Choppiness <= 1.0f))
	{
		Errors.Add(FString::Printf(TEXT("ocean.choppiness=%.2f out of range [0, 1]"), Ocean.Choppiness));
	}
	if (!(Ocean.WaterTemperatureC >= -2.0f && Ocean.WaterTemperatureC <= 40.0f))
		Errors.Add(FString::Printf(TEXT("ocean.water_temperature_c=%.2f out of range [-2, 40]"), Ocean.WaterTemperatureC));

		// Thermal (ROADMAP 4A). Written !(x in range) so NaN is reported too.
	if (!(Thermal.AirTemperatureC >= -80.0f && Thermal.AirTemperatureC <= 60.0f))
		Errors.Add(FString::Printf(TEXT("thermal.air_temperature_c=%.2f out of range [-80, 60]"), Thermal.AirTemperatureC));
	if (!(Thermal.AirDiurnalSwingK >= 0.0f && Thermal.AirDiurnalSwingK <= 30.0f))
		Errors.Add(FString::Printf(TEXT("thermal.air_diurnal_swing_k=%.2f out of range [0, 30]"), Thermal.AirDiurnalSwingK));
	if (!(Thermal.ExtinctionPerKmMwir >= 0.0f && Thermal.ExtinctionPerKmMwir <= 10.0f))
		Errors.Add(FString::Printf(TEXT("thermal.extinction_per_km.mwir=%.3f out of range [0, 10]"), Thermal.ExtinctionPerKmMwir));
	if (!(Thermal.ExtinctionPerKmLwir >= 0.0f && Thermal.ExtinctionPerKmLwir <= 10.0f))
		Errors.Add(FString::Printf(TEXT("thermal.extinction_per_km.lwir=%.3f out of range [0, 10]"), Thermal.ExtinctionPerKmLwir));
	if (!(Thermal.FogIrFactor >= 0.0f && Thermal.FogIrFactor <= 2.0f))
		Errors.Add(FString::Printf(TEXT("thermal.fog_ir_factor=%.2f out of range [0, 2]"), Thermal.FogIrFactor));
	Errors.Append(FThermalMaterialTable::Validate(Thermal.Materials));
	// Entity thermal state (ROADMAP 4C). Written !(x in range) so NaN is reported too.
	{
		const FEntityThermalSettings& E = Thermal.Entity;
		auto InRange = [&Errors](const TCHAR* Name, float V, float Lo, float Hi)
		{
			if (!(V >= Lo && V <= Hi)) Errors.Add(FString::Printf(TEXT("thermal.entity.%s=%.3f out of range [%.1f, %.1f]"), Name, V, Lo, Hi));
		};
		auto Positive = [&Errors](const TCHAR* Name, float V)
		{
			if (!(V > 0.0f && V <= 1.0e6f)) Errors.Add(FString::Printf(TEXT("thermal.entity.%s=%.3f must be in (0, 1e6]"), Name, V));
		};
		const FEntityThermalKindParams& Eng  = E.Kind(EEntityThermalPartKind::Engine);
		const FEntityThermalKindParams& Exh  = E.Kind(EEntityThermalPartKind::Exhaust);
		const FEntityThermalKindParams& Gear = E.Kind(EEntityThermalPartKind::RunningGear);
		InRange(TEXT("moving_mps"), E.MovingMps, 0.0f, 50.0f);
		InRange(TEXT("idle_hold_s"), E.IdleHoldS, 0.0f, 86400.0f);
		InRange(TEXT("skin_running_k"), E.SkinRunningK, -50.0f, 100.0f);
		Positive(TEXT("convection_v0_mps"), E.ConvectionV0Mps);
		Positive(TEXT("skin_tau_s"), E.SkinTauS);
		InRange(TEXT("burn_k"), E.BurnK, 150.0f, 1000.0f);   // the radiance LUT range
		InRange(TEXT("burn_s"), E.BurnS, 0.0f, 86400.0f);
		Positive(TEXT("hull_cool_tau_s"), E.HullCoolTauS);
		InRange(TEXT("engine.delta_k"), Eng.DeltaK, -50.0f, 500.0f);
		InRange(TEXT("exhaust.temp_k"), Exh.TempK, 150.0f, 1000.0f);
		InRange(TEXT("running_gear.k_per_mps"), Gear.KPerMps, 0.0f, 50.0f);
		InRange(TEXT("running_gear.max_k"), Gear.MaxK, 0.0f, 500.0f);
		Positive(TEXT("engine.tau_up_s"), Eng.TauUpS);
		Positive(TEXT("engine.tau_down_s"), Eng.TauDownS);
		Positive(TEXT("exhaust.tau_up_s"), Exh.TauUpS);
		Positive(TEXT("exhaust.tau_down_s"), Exh.TauDownS);
		Positive(TEXT("running_gear.tau_up_s"), Gear.TauUpS);
		Positive(TEXT("running_gear.tau_down_s"), Gear.TauDownS);
	}
	// Land cover (ROADMAP 4B). Written !(x in range) so NaN is reported too.
	const FThermalConfig::FLandCoverConfig& LC = Thermal.LandCover;
	if (!(LC.WindowTexels >= 256 && LC.WindowTexels <= 8192 && LC.WindowTexels % 2 == 0))
		Errors.Add(FString::Printf(TEXT("thermal.land_cover.window_texels=%d must be even, in [256, 8192]"), LC.WindowTexels));
	if (!(LC.RecentreFraction >= 0.01f && LC.RecentreFraction <= 0.45f))
		Errors.Add(FString::Printf(TEXT("thermal.land_cover.recentre_fraction=%.3f out of range [0.01, 0.45]"), LC.RecentreFraction));
	if (!(LC.VegIndexLo >= -1.0f && LC.VegIndexHi <= 2.0f && LC.VegIndexLo < LC.VegIndexHi))
		Errors.Add(FString::Printf(TEXT("thermal.land_cover.veg_index_lo/hi=%.3f/%.3f must satisfy -1 <= lo < hi <= 2"), LC.VegIndexLo, LC.VegIndexHi));
	if (!(LC.AsphaltMaxLuma >= 0.0f && LC.AsphaltMaxLuma <= 1.0f))
		Errors.Add(FString::Printf(TEXT("thermal.land_cover.asphalt_max_luma=%.3f out of range [0, 1]"), LC.AsphaltMaxLuma));
	if (!(LC.WarpAmplitudeM >= 0.0f && LC.WarpAmplitudeM <= 20.0f))
		Errors.Add(FString::Printf(TEXT("thermal.land_cover.warp_amplitude_m=%.3f out of range [0, 20]"), LC.WarpAmplitudeM));
	if (!(LC.WarpCellM >= 5.0f && LC.WarpCellM <= 200.0f))
		Errors.Add(FString::Printf(TEXT("thermal.land_cover.warp_cell_m=%.3f out of range [5, 200]"), LC.WarpCellM));
	if (!(LC.VegBlurM >= 0.0f && LC.VegBlurM <= 32.0f))
		Errors.Add(FString::Printf(TEXT("thermal.land_cover.veg_blur_m=%.3f out of range [0, 32]"), LC.VegBlurM));
	if (LC.bEnabled && LC.Dir.TrimStartAndEnd().IsEmpty())
		Errors.Add(TEXT("thermal.land_cover.dir is empty (set it, or thermal.land_cover.enabled: false)"));
	Errors.Append(CamSimLandCover::ValidateClassSpecs(LC.Classes));

	return Errors;
}

TArray<FString> FCamSimConfig::ValidateWarnings() const
{
	TArray<FString> Warnings;
	for (const TPair<ESensorMode, FSensorModeConfig>& Pair : SensorModeConfigs)
	{
		const FSensorOpticsConfig& O = Pair.Value.Optics;
		if (!(O.PixelPitchUm > 0.0f)) continue;   // Validate() reports it
		// Past BudgetPsfRadius the sensor graph switches to a larger groupshared tile (BLUR_MAX_R 5/8)
		// that costs more than the 2 ms p95 GPU budget at 1080p (ROADMAP 3B.2 Task 10 measurements).
		const float SigmaO = CamSimOptics::PsfOpticalSigmaPx(O);
		const int32 Radius = CamSimOptics::PsfRadius(SigmaO);
		if (Radius > CamSimOptics::BudgetPsfRadius)
		{
			Warnings.Add(FString::Printf(
				TEXT("sensor_modes[%d].optics: optical PSF sigma %.3f px (PSF radius %d > %d) exceeds the 1080p GPU "
				     "budget tier of the sensor graph; expect > 2 ms GPU per frame at 1080p (lower extra_blur_px or f_number)"),
				static_cast<int32>(Pair.Key), SigmaO, Radius, CamSimOptics::BudgetPsfRadius));
		}
	}
	// Thermal (ROADMAP 4A): the radiance LUT spans scene temperatures (~200-400 K), which emit next to nothing below
	// ~1.5 um, so an IR mode with a visible-band detector (e.g. preset eo_hd_cmos, 0.4-0.7 um) underflows to a black frame.
	if (Thermal.bEnabled)
	{
		if (const FSensorModeConfig* Ir = SensorModeConfigs.Find(ESensorMode::IR))
		{
			if (Ir->Detector.BandLoUm < 1.5f)
			{
				Warnings.Add(FString::Printf(
					TEXT("sensor_modes[%d].detector: band_lo_um=%.2f is below 1.5 um with thermal.enabled; the thermal "
					     "radiance of scene temperatures underflows in that band (black IR frames) — use an IR preset "
					     "(mwir_cooled, lwir_uncooled) or a band in the 3-5 / 8-12 um windows"),
					static_cast<int32>(ESensorMode::IR), Ir->Detector.BandLoUm));
			}
		}
	}
	return Warnings;
}
