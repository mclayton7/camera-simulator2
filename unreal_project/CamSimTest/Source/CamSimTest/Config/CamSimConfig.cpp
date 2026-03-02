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
		Cfg.SensorModeConfigs.Add(ESensorMode::EO, EoCfg);

		FSensorModeConfig IrCfg;
		IrCfg.NETD              = 0.01f;
		IrCfg.FixedPatternNoise = 0.005f;
		IrCfg.Vignetting        = 0.20f;
		IrCfg.bScanLines        = false;
		IrCfg.IRExtinctionCoeff = 1e-5f;
		Cfg.SensorModeConfigs.Add(ESensorMode::IR, IrCfg);

		FSensorModeConfig NvgCfg;
		NvgCfg.NETD              = 0.03f;
		NvgCfg.FixedPatternNoise = 0.0f;
		NvgCfg.Vignetting        = 0.35f;
		NvgCfg.bScanLines        = false;
		NvgCfg.ScanLineStrength  = 0.05f;
		Cfg.SensorModeConfigs.Add(ESensorMode::NVG, NvgCfg);
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
					};

					ParseMode(TEXT("eo"),  ESensorMode::EO);
					ParseMode(TEXT("ir"),  ESensorMode::IR);
					ParseMode(TEXT("nvg"), ESensorMode::NVG);
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
	Cfg.TilePreloadFovScale     = GetEnvFloat(TEXT("CAMSIM_TILE_FOV_SCALE"),       Cfg.TilePreloadFovScale);
	Cfg.MaxSimultaneousTileLoads = GetEnvInt(TEXT("CAMSIM_MAX_TILE_LOADS"),        Cfg.MaxSimultaneousTileLoads);
	Cfg.StartLatitude  = GetEnvDouble(TEXT("CAMSIM_START_LAT"),   Cfg.StartLatitude);
	Cfg.StartLongitude = GetEnvDouble(TEXT("CAMSIM_START_LON"),   Cfg.StartLongitude);
	Cfg.StartAltitude  = GetEnvDouble(TEXT("CAMSIM_START_ALT"),   Cfg.StartAltitude);
	Cfg.StartYaw       = GetEnvFloat(TEXT("CAMSIM_START_YAW"),    Cfg.StartYaw);
	Cfg.StartPitch     = GetEnvFloat(TEXT("CAMSIM_START_PITCH"),  Cfg.StartPitch);
	Cfg.StartRoll      = GetEnvFloat(TEXT("CAMSIM_START_ROLL"),   Cfg.StartRoll);
	Cfg.StartHour      = GetEnvFloat(TEXT("CAMSIM_START_HOUR"),   Cfg.StartHour);

	UE_LOG(LogCamSim, Log,
		TEXT("Config: CIGI=%s:%d  Out=udp://%s:%d  Bitrate=%d  Preset=%s"),
		*Cfg.CigiBindAddr, Cfg.CigiPort,
		*Cfg.MulticastAddr, Cfg.MulticastPort,
		Cfg.VideoBitrate, *Cfg.H264Preset);
}
