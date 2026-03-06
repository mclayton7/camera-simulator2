// Copyright CamSim Contributors. All Rights Reserved.

#include "Config/CamSimConfig.h"
#include "CamSimTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "JsonObjectConverter.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

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
// FCamSimConfig
// ---------------------------------------------------------------------------

FCamSimConfig FCamSimConfig::Load(TSharedPtr<FJsonObject>* OutJsonRoot)
{
	FCamSimConfig Cfg; // default values from member initialisers

	// Apply default sensor mode configs (overwritten by JSON if present)
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

	// Attempt to read JSON config from binary dir
	FString JsonPath = FPaths::Combine(FPaths::ProjectDir(), TEXT("camsim_config.json"));
	if (!FPaths::FileExists(JsonPath))
	{
		// Fall back to the directory the binary lives in
		JsonPath = FPaths::Combine(FPlatformProcess::BaseDir(), TEXT("camsim_config.json"));
	}

	FString JsonContent;
	if (FFileHelper::LoadFileToString(JsonContent, *JsonPath))
	{
		TSharedPtr<FJsonObject> Root;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonContent);
		if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
		{
			if (OutJsonRoot) *OutJsonRoot = Root;
			Root->TryGetStringField(TEXT("cigi_bind_addr"),      Cfg.CigiBindAddr);
			Root->TryGetNumberField(TEXT("cigi_port"),           Cfg.CigiPort);
			Root->TryGetStringField(TEXT("cigi_response_addr"),  Cfg.CigiResponseAddr);
			Root->TryGetNumberField(TEXT("cigi_response_port"),  Cfg.CigiResponsePort);
			Root->TryGetStringField(TEXT("multicast_addr"),   Cfg.MulticastAddr);
			Root->TryGetNumberField(TEXT("multicast_port"),   Cfg.MulticastPort);
			Root->TryGetNumberField(TEXT("video_bitrate"),    Cfg.VideoBitrate);
			Root->TryGetStringField(TEXT("h264_preset"),      Cfg.H264Preset);
			Root->TryGetStringField(TEXT("h264_tune"),        Cfg.H264Tune);
			Root->TryGetNumberField(TEXT("capture_width"),    Cfg.CaptureWidth);
			Root->TryGetNumberField(TEXT("capture_height"),   Cfg.CaptureHeight);
			Root->TryGetNumberField(TEXT("frame_rate"),       Cfg.FrameRate);
			Root->TryGetBoolField  (TEXT("swap_rb_readback"), Cfg.bSwapRBReadback);
			Root->TryGetNumberField(TEXT("readback_ready_polls"), Cfg.ReadbackReadyPolls);
			{
				FString ReadbackFmt;
				if (Root->TryGetStringField(TEXT("readback_format"), ReadbackFmt))
				{
					Cfg.ReadbackFormat = ParseReadbackFormat(ReadbackFmt);
				}
			}
			{
				FString WatchdogPolicy;
				if (Root->TryGetStringField(TEXT("encoder_watchdog_policy"), WatchdogPolicy))
				{
					Cfg.EncoderWatchdogPolicy = ParseWatchdogPolicy(WatchdogPolicy);
				}
			}
			Root->TryGetNumberField(TEXT("encoder_watchdog_interval_ticks"), Cfg.EncoderWatchdogIntervalTicks);
			Root->TryGetNumberField(TEXT("hfov_deg"),         Cfg.HFovDeg);
			Root->TryGetNumberField(TEXT("tile_preload_fov_scale"),     Cfg.TilePreloadFovScale);
			Root->TryGetNumberField(TEXT("max_simultaneous_tile_loads"), Cfg.MaxSimultaneousTileLoads);
			Root->TryGetNumberField(TEXT("start_latitude"),   Cfg.StartLatitude);
			Root->TryGetNumberField(TEXT("start_longitude"),  Cfg.StartLongitude);
			Root->TryGetNumberField(TEXT("start_altitude"),   Cfg.StartAltitude);
			Root->TryGetNumberField(TEXT("start_yaw"),        Cfg.StartYaw);
			Root->TryGetNumberField(TEXT("start_pitch"),      Cfg.StartPitch);
			Root->TryGetNumberField(TEXT("start_roll"),       Cfg.StartRoll);
			Root->TryGetNumberField(TEXT("start_hour"),       Cfg.StartHour);
			Root->TryGetNumberField(TEXT("camera_entity_id"),           Cfg.CameraEntityId);
			Root->TryGetNumberField(TEXT("gimbal_max_slew_rate"),       Cfg.GimbalMaxSlewRateDegPerSec);
			Root->TryGetNumberField(TEXT("gimbal_pitch_min"),           Cfg.GimbalPitchMin);
			Root->TryGetNumberField(TEXT("gimbal_pitch_max"),           Cfg.GimbalPitchMax);
			Root->TryGetNumberField(TEXT("gimbal_yaw_min"),             Cfg.GimbalYawMin);
			Root->TryGetNumberField(TEXT("gimbal_yaw_max"),             Cfg.GimbalYawMax);

			// FOV presets: optional JSON array of floats (wide → narrow)
			const TArray<TSharedPtr<FJsonValue>>* PresetsArr = nullptr;
			if (Root->TryGetArrayField(TEXT("sensor_fov_presets"), PresetsArr) && PresetsArr)
			{
				for (const TSharedPtr<FJsonValue>& Val : *PresetsArr)
				{
					double V = 60.0;
					if (Val.IsValid() && Val->TryGetNumber(V))
					{
						Cfg.SensorFovPresets.Add(static_cast<float>(V));
					}
				}
			}

			// -----------------------------------------------------------------------
			// sensor_modes: per-waveband simulation parameters (Phase 11)
			// Overwrites the defaults set above with JSON values where present.
			// -----------------------------------------------------------------------
			{
				const TSharedPtr<FJsonObject>* ModesObj = nullptr;
				if (Root->TryGetObjectField(TEXT("sensor_modes"), ModesObj) && ModesObj)
				{
					auto ParseMode = [&](const FString& Key, ESensorMode M)
					{
						const TSharedPtr<FJsonObject>* ModeObj = nullptr;
						if (!(*ModesObj)->TryGetObjectField(Key, ModeObj) || !ModeObj) return;

						FSensorModeConfig& MC = Cfg.SensorModeConfigs.FindOrAdd(M);
						double TmpD = 0.0;
						bool   TmpB = false;

						if ((*ModeObj)->TryGetNumberField(TEXT("noise_netd"),          TmpD)) MC.NETD              = static_cast<float>(TmpD);
						if ((*ModeObj)->TryGetNumberField(TEXT("fixed_pattern_noise"),  TmpD)) MC.FixedPatternNoise = static_cast<float>(TmpD);
						if ((*ModeObj)->TryGetNumberField(TEXT("vignetting"),           TmpD)) MC.Vignetting        = static_cast<float>(TmpD);
						if ((*ModeObj)->TryGetBoolField  (TEXT("scan_lines"),           TmpB)) MC.bScanLines        = TmpB;
						if ((*ModeObj)->TryGetNumberField(TEXT("scan_line_strength"),   TmpD)) MC.ScanLineStrength  = static_cast<float>(TmpD);
						if ((*ModeObj)->TryGetNumberField(TEXT("ir_extinction_coeff"),  TmpD)) MC.IRExtinctionCoeff = static_cast<float>(TmpD);
						if ((*ModeObj)->TryGetNumberField(TEXT("atmospheric_visibility_m"), TmpD)) MC.AtmosphericVisibilityM = static_cast<float>(TmpD);
						if ((*ModeObj)->TryGetNumberField(TEXT("atmosphere_strength"),   TmpD)) MC.AtmosphereStrength = static_cast<float>(TmpD);
						if ((*ModeObj)->TryGetNumberField(TEXT("color_temperature_k"),   TmpD)) MC.ColorTemperatureK = static_cast<float>(TmpD);
						if ((*ModeObj)->TryGetNumberField(TEXT("contrast"),              TmpD)) MC.Contrast = static_cast<float>(TmpD);
						if ((*ModeObj)->TryGetNumberField(TEXT("brightness_bias"),       TmpD)) MC.BrightnessBias = static_cast<float>(TmpD);
						if ((*ModeObj)->TryGetNumberField(TEXT("blur_radius"),           TmpD)) MC.BlurRadius = FMath::Max(0, static_cast<int32>(TmpD));
					};

					ParseMode(TEXT("eo"),  ESensorMode::EO);
					ParseMode(TEXT("ir"),  ESensorMode::IR);
					ParseMode(TEXT("nvg"), ESensorMode::NVG);
				}
			}

			// Optional user-defined sensor quality profiles.
			{
				const TSharedPtr<FJsonObject>* ProfilesObj = nullptr;
				if (Root->TryGetObjectField(TEXT("sensor_quality_profiles"), ProfilesObj) && ProfilesObj)
				{
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*ProfilesObj)->Values)
					{
						const FString PresetKey = NormalizeQualityPreset(Pair.Key);
						const TSharedPtr<FJsonObject>* ProfileObj = nullptr;
						if (!(*ProfilesObj)->TryGetObjectField(Pair.Key, ProfileObj) || !ProfileObj) continue;

						FSensorQualityConfig Profile = Cfg.SensorQualityProfiles.FindRef(TEXT("medium"));
						double TmpD = 0.0;
						if ((*ProfileObj)->TryGetNumberField(TEXT("noise_scale"),      TmpD)) Profile.NoiseScale = static_cast<float>(TmpD);
						if ((*ProfileObj)->TryGetNumberField(TEXT("vignetting_scale"), TmpD)) Profile.VignettingScale = static_cast<float>(TmpD);
						if ((*ProfileObj)->TryGetNumberField(TEXT("scan_line_scale"),  TmpD)) Profile.ScanLineScale = static_cast<float>(TmpD);
						if ((*ProfileObj)->TryGetNumberField(TEXT("atmosphere_scale"), TmpD)) Profile.AtmosphereScale = static_cast<float>(TmpD);
						if ((*ProfileObj)->TryGetNumberField(TEXT("blur_radius"),      TmpD)) Profile.BlurRadius = FMath::Max(0, static_cast<int32>(TmpD));
						if ((*ProfileObj)->TryGetNumberField(TEXT("contrast"),         TmpD)) Profile.Contrast = static_cast<float>(TmpD);
						if ((*ProfileObj)->TryGetNumberField(TEXT("brightness_bias"),  TmpD)) Profile.BrightnessBias = static_cast<float>(TmpD);
						Cfg.SensorQualityProfiles.Add(PresetKey, Profile);
					}
				}
			}

			// Active sensor quality preset and optional inline overrides.
			{
				const TSharedPtr<FJsonObject>* QualityObj = nullptr;
				if (Root->TryGetObjectField(TEXT("sensor_quality"), QualityObj) && QualityObj)
				{
					FString Preset;
					if ((*QualityObj)->TryGetStringField(TEXT("preset"), Preset))
					{
						Cfg.SensorQualityPreset = NormalizeQualityPreset(Preset);
					}
					ResolveActiveSensorQuality(Cfg);

					double TmpD = 0.0;
					if ((*QualityObj)->TryGetNumberField(TEXT("noise_scale"), TmpD))
						Cfg.ActiveSensorQuality.NoiseScale = static_cast<float>(TmpD);
					if ((*QualityObj)->TryGetNumberField(TEXT("vignetting_scale"), TmpD))
						Cfg.ActiveSensorQuality.VignettingScale = static_cast<float>(TmpD);
					if ((*QualityObj)->TryGetNumberField(TEXT("scan_line_scale"), TmpD))
						Cfg.ActiveSensorQuality.ScanLineScale = static_cast<float>(TmpD);
					if ((*QualityObj)->TryGetNumberField(TEXT("atmosphere_scale"), TmpD))
						Cfg.ActiveSensorQuality.AtmosphereScale = static_cast<float>(TmpD);
					if ((*QualityObj)->TryGetNumberField(TEXT("blur_radius"), TmpD))
						Cfg.ActiveSensorQuality.BlurRadius = FMath::Max(0, static_cast<int32>(TmpD));
					if ((*QualityObj)->TryGetNumberField(TEXT("contrast"), TmpD))
						Cfg.ActiveSensorQuality.Contrast = static_cast<float>(TmpD);
					if ((*QualityObj)->TryGetNumberField(TEXT("brightness_bias"), TmpD))
						Cfg.ActiveSensorQuality.BrightnessBias = static_cast<float>(TmpD);
				}
				else
				{
					ResolveActiveSensorQuality(Cfg);
				}
			}

			// Optional multi-stream output views.
			{
				const TArray<TSharedPtr<FJsonValue>>* ViewsArr = nullptr;
				if (Root->TryGetArrayField(TEXT("output_views"), ViewsArr) && ViewsArr)
				{
					Cfg.OutputViews.Reset();
					int32 DefaultViewId = 0;
					for (const TSharedPtr<FJsonValue>& ViewVal : *ViewsArr)
					{
						if (!ViewVal.IsValid()) continue;
						const TSharedPtr<FJsonObject> ViewObj = ViewVal->AsObject();
						if (!ViewObj.IsValid()) continue;

						FCamSimConfig::FOutputViewConfig ViewCfg;
						ViewCfg.ViewId = DefaultViewId++;
						ViewCfg.MulticastAddr = Cfg.MulticastAddr;
						ViewCfg.MulticastPort = Cfg.MulticastPort;
						ViewCfg.VideoBitrate = Cfg.VideoBitrate;
						ViewCfg.H264Preset = Cfg.H264Preset;
						ViewCfg.H264Tune = Cfg.H264Tune;
						ViewCfg.HFovDeg = 0.0f;

						double TmpNum = 0.0;
						bool TmpBool = true;
						if (ViewObj->TryGetNumberField(TEXT("view_id"), TmpNum)) ViewCfg.ViewId = static_cast<int32>(TmpNum);
						if (ViewObj->TryGetBoolField(TEXT("enabled"), TmpBool)) ViewCfg.bEnabled = TmpBool;
						ViewObj->TryGetStringField(TEXT("multicast_addr"), ViewCfg.MulticastAddr);
						if (ViewObj->TryGetNumberField(TEXT("multicast_port"), TmpNum)) ViewCfg.MulticastPort = static_cast<int32>(TmpNum);
						if (ViewObj->TryGetNumberField(TEXT("video_bitrate"), TmpNum)) ViewCfg.VideoBitrate = static_cast<int32>(TmpNum);
						ViewObj->TryGetStringField(TEXT("h264_preset"), ViewCfg.H264Preset);
						ViewObj->TryGetStringField(TEXT("h264_tune"), ViewCfg.H264Tune);
						if (ViewObj->TryGetNumberField(TEXT("hfov_deg"), TmpNum)) ViewCfg.HFovDeg = static_cast<float>(TmpNum);

						Cfg.OutputViews.Add(ViewCfg);
					}
				}
			}

			// Optional ground-truth sidecar output.
			{
				const TSharedPtr<FJsonObject>* GTObj = nullptr;
				if (Root->TryGetObjectField(TEXT("ground_truth"), GTObj) && GTObj)
				{
					bool TmpB = false;
					double TmpD = 0.0;
					if ((*GTObj)->TryGetBoolField(TEXT("enabled"), TmpB)) Cfg.GroundTruth.bEnabled = TmpB;
					(*GTObj)->TryGetStringField(TEXT("output_path"), Cfg.GroundTruth.OutputPath);
					if ((*GTObj)->TryGetNumberField(TEXT("interval_frames"), TmpD))
						Cfg.GroundTruth.IntervalFrames = FMath::Max(1, static_cast<int32>(TmpD));
				}
				Root->TryGetBoolField(TEXT("ground_truth_enabled"), Cfg.GroundTruth.bEnabled);
				Root->TryGetStringField(TEXT("ground_truth_path"), Cfg.GroundTruth.OutputPath);
				{
					double GTInterval = Cfg.GroundTruth.IntervalFrames;
					if (Root->TryGetNumberField(TEXT("ground_truth_interval_frames"), GTInterval))
					{
						Cfg.GroundTruth.IntervalFrames = FMath::Max(1, static_cast<int32>(GTInterval));
					}
				}
			}

			// Entity runtime scale controls (LOD/culling/update throttling).
			{
				const TSharedPtr<FJsonObject>* ScaleObj = nullptr;
				if (Root->TryGetObjectField(TEXT("entity_scale"), ScaleObj) && ScaleObj)
				{
					double TmpD = 0.0;
					if ((*ScaleObj)->TryGetNumberField(TEXT("max_draw_distance_m"), TmpD))
						Cfg.EntityScale.MaxDrawDistanceM = static_cast<float>(TmpD);
					if ((*ScaleObj)->TryGetNumberField(TEXT("tick_rate_hz"), TmpD))
						Cfg.EntityScale.TickRateHz = static_cast<float>(TmpD);
					if ((*ScaleObj)->TryGetNumberField(TEXT("default_max_update_rate_hz"), TmpD))
						Cfg.EntityScale.DefaultMaxUpdateRateHz = static_cast<float>(TmpD);

					const TSharedPtr<FJsonObject>* OverridesObj = nullptr;
					if ((*ScaleObj)->TryGetObjectField(TEXT("max_update_rate_hz_overrides"), OverridesObj) && OverridesObj)
					{
						for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*OverridesObj)->Values)
						{
							int32 EntityId = FCString::Atoi(*Pair.Key);
							double RateHz = 0.0;
							if (Pair.Value.IsValid() && Pair.Value->TryGetNumber(RateHz))
							{
								Cfg.EntityScale.MaxUpdateRateHzOverrides.Add(EntityId, static_cast<float>(RateHz));
							}
						}
					}
				}

				// Legacy flat keys (kept for backwards compatibility).
				{
					double TmpD = Cfg.EntityScale.MaxDrawDistanceM;
					if (Root->TryGetNumberField(TEXT("entity_max_draw_distance_m"), TmpD))
					{
						Cfg.EntityScale.MaxDrawDistanceM = static_cast<float>(TmpD);
					}
				}
				{
					double TmpD = Cfg.EntityScale.TickRateHz;
					if (Root->TryGetNumberField(TEXT("entity_tick_rate_hz"), TmpD))
					{
						Cfg.EntityScale.TickRateHz = static_cast<float>(TmpD);
					}
				}
				{
					double TmpD = Cfg.EntityScale.DefaultMaxUpdateRateHz;
					if (Root->TryGetNumberField(TEXT("entity_default_max_update_rate_hz"), TmpD))
					{
						Cfg.EntityScale.DefaultMaxUpdateRateHz = static_cast<float>(TmpD);
					}
				}
			}

			// Optional scenario entity orchestration block.
			{
				const TSharedPtr<FJsonObject>* ScenarioObj = nullptr;
				if (Root->TryGetObjectField(TEXT("scenario"), ScenarioObj) && ScenarioObj)
				{
					bool TmpB = false;
					double TmpD = 0.0;
					if ((*ScenarioObj)->TryGetBoolField(TEXT("enabled"), TmpB))
						Cfg.bScenarioEnabled = TmpB;
					if ((*ScenarioObj)->TryGetNumberField(TEXT("time_scale"), TmpD))
						Cfg.ScenarioTimeScale = static_cast<float>(TmpD);

					const TArray<TSharedPtr<FJsonValue>>* EntitiesArr = nullptr;
					if ((*ScenarioObj)->TryGetArrayField(TEXT("entities"), EntitiesArr) && EntitiesArr)
					{
						Cfg.ScenarioEntities.Reset();
						for (const TSharedPtr<FJsonValue>& EntityVal : *EntitiesArr)
						{
							if (!EntityVal.IsValid()) continue;
							const TSharedPtr<FJsonObject> EntityObj = EntityVal->AsObject();
							if (!EntityObj.IsValid()) continue;

							FCamSimConfig::FScenarioEntityConfig Spec;
							if (EntityObj->TryGetNumberField(TEXT("entity_id"), TmpD))
								Spec.EntityId = static_cast<int32>(TmpD);
							if (EntityObj->TryGetNumberField(TEXT("entity_type"), TmpD))
								Spec.EntityType = static_cast<int32>(TmpD);
							if (EntityObj->TryGetNumberField(TEXT("start_latitude"), TmpD))
								Spec.StartLatitude = TmpD;
							if (EntityObj->TryGetNumberField(TEXT("start_longitude"), TmpD))
								Spec.StartLongitude = TmpD;
							if (EntityObj->TryGetNumberField(TEXT("start_altitude"), TmpD))
								Spec.StartAltitude = TmpD;
							if (EntityObj->TryGetNumberField(TEXT("start_yaw"), TmpD))
								Spec.StartYaw = static_cast<float>(TmpD);
							if (EntityObj->TryGetNumberField(TEXT("start_pitch"), TmpD))
								Spec.StartPitch = static_cast<float>(TmpD);
							if (EntityObj->TryGetNumberField(TEXT("start_roll"), TmpD))
								Spec.StartRoll = static_cast<float>(TmpD);

							if (EntityObj->TryGetNumberField(TEXT("spawn_time_sec"), TmpD))
								Spec.SpawnTimeSec = static_cast<float>(TmpD);
							if (EntityObj->TryGetNumberField(TEXT("despawn_time_sec"), TmpD))
								Spec.DespawnTimeSec = static_cast<float>(TmpD);
							if (EntityObj->TryGetNumberField(TEXT("update_rate_hz"), TmpD))
								Spec.UpdateRateHz = static_cast<float>(TmpD);

							if (EntityObj->TryGetNumberField(TEXT("north_rate_mps"), TmpD))
								Spec.NorthRateMps = static_cast<float>(TmpD);
							if (EntityObj->TryGetNumberField(TEXT("east_rate_mps"), TmpD))
								Spec.EastRateMps = static_cast<float>(TmpD);
							if (EntityObj->TryGetNumberField(TEXT("up_rate_mps"), TmpD))
								Spec.UpRateMps = static_cast<float>(TmpD);
							if (EntityObj->TryGetNumberField(TEXT("yaw_rate_dps"), TmpD))
								Spec.YawRateDegPerSec = static_cast<float>(TmpD);
							if (EntityObj->TryGetNumberField(TEXT("pitch_rate_dps"), TmpD))
								Spec.PitchRateDegPerSec = static_cast<float>(TmpD);
							if (EntityObj->TryGetNumberField(TEXT("roll_rate_dps"), TmpD))
								Spec.RollRateDegPerSec = static_cast<float>(TmpD);

							Cfg.ScenarioEntities.Add(Spec);
						}
					}
				}

				// Legacy flat keys.
				Root->TryGetBoolField(TEXT("scenario_enabled"), Cfg.bScenarioEnabled);
				{
					double TmpD = Cfg.ScenarioTimeScale;
					if (Root->TryGetNumberField(TEXT("scenario_time_scale"), TmpD))
					{
						Cfg.ScenarioTimeScale = static_cast<float>(TmpD);
					}
				}
			}

			UE_LOG(LogCamSim, Log, TEXT("Loaded config from %s"), *JsonPath);
		}
		else
		{
			UE_LOG(LogCamSim, Warning, TEXT("Failed to parse %s - using defaults"), *JsonPath);
		}
	}
	else
	{
		UE_LOG(LogCamSim, Log, TEXT("No config file found at %s - using defaults"), *JsonPath);
	}

	ApplyEnvOverrides(Cfg);
	return Cfg;
}

void FCamSimConfig::ApplyEnvOverrides(FCamSimConfig& Cfg)
{
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

	UE_LOG(LogCamSim, Log,
		TEXT("Config: CIGI=%s:%d Out=udp://%s:%d Bitrate=%d Preset=%s ReadbackReadyPolls=%d WatchdogInterval=%d ")
		TEXT("SensorQuality=%s GroundTruth=%d EntityScale(draw=%.1fm tick=%.1fHz pose_cap=%.1fHz) Scenario=%d entities=%d time_scale=%.2f"),
		*Cfg.CigiBindAddr, Cfg.CigiPort,
		*Cfg.MulticastAddr, Cfg.MulticastPort,
		Cfg.VideoBitrate, *Cfg.H264Preset,
		Cfg.ReadbackReadyPolls, Cfg.EncoderWatchdogIntervalTicks,
		*Cfg.SensorQualityPreset, Cfg.GroundTruth.bEnabled ? 1 : 0,
		Cfg.EntityScale.MaxDrawDistanceM, Cfg.EntityScale.TickRateHz, Cfg.EntityScale.DefaultMaxUpdateRateHz,
		Cfg.bScenarioEnabled ? 1 : 0, Cfg.ScenarioEntities.Num(), Cfg.ScenarioTimeScale);
}
