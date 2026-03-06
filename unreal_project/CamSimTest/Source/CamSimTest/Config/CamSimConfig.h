// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Sensor/SensorTypes.h"         // ESensorMode, sensor config structs

/**
 * Runtime configuration for CamSim.
 *
 * Values are loaded from camsim_config.json in the binary directory.
 * Individual fields may be overridden via environment variables at startup.
 *
 * Env vars:
 *   CAMSIM_CIGI_PORT              – UDP port to listen for CIGI packets        (default 8888)
 *   CAMSIM_CIGI_BIND_ADDR         – Local address to bind the CIGI socket       (default 0.0.0.0)
 *   CAMSIM_CIGI_RESPONSE_ADDR     – Host IP for IG→host response packets        (default 127.0.0.1)
 *   CAMSIM_CIGI_RESPONSE_PORT     – Host's incoming CIGI port for responses     (default 8889)
 *   CAMSIM_MULTICAST_ADDR         – Multicast group for output stream            (default 239.1.1.1)
 *   CAMSIM_MULTICAST_PORT         – UDP port for output stream                   (default 5004)
 *   CAMSIM_VIDEO_BITRATE          – Target H.264 bitrate in bps                  (default 4000000)
 *   CAMSIM_H264_PRESET            – libx264 preset string                        (default ultrafast)
 *   CAMSIM_SWAP_RB_READBACK       – Force red/blue swap on GPU readback          (default 0)
 *   CAMSIM_READBACK_READY_POLLS   – Consecutive IsReady polls before Lock         (default 2)
 *   CAMSIM_ENCODER_WATCHDOG_POLICY – reconnect|log_only|fail_fast                 (default reconnect)
 *   CAMSIM_ENCODER_WATCHDOG_INTERVAL_TICKS – watchdog check interval              (default 150)
 *   CAMSIM_START_HOUR             – Fallback time-of-day (0-24)                  (default 12.0)
 *   CAMSIM_SENSOR_QUALITY_PRESET  – low|medium|high|ultra|custom                 (default medium)
 *   CAMSIM_GROUND_TRUTH_ENABLED   – write JSONL sidecar telemetry                  (default 0)
 *   CAMSIM_GROUND_TRUTH_PATH      – sidecar output path                            (default empty)
 *   CAMSIM_GROUND_TRUTH_INTERVAL_FRAMES – sidecar cadence                          (default 1)
 *   CAMSIM_ENTITY_MAX_DRAW_DISTANCE_M – entity culling distance                    (default 0=disabled)
 *   CAMSIM_ENTITY_TICK_RATE_HZ    – entity actor tick rate                          (default 0=unlimited)
 *   CAMSIM_ENTITY_DEFAULT_MAX_UPDATE_RATE_HZ – default pose apply cap               (default 0=unlimited)
 *   CAMSIM_SCENARIO_ENABLED       – enable scenario_entities                        (default 0)
 *   CAMSIM_SCENARIO_TIME_SCALE    – scenario time multiplier                        (default 1.0)
 */
struct FCamSimConfig
{
	enum class EReadbackFormat : uint8
	{
		Auto = 0,
		BGRA,
		RGBA,
		ARGB,
		ABGR
	};

	enum class EEncoderWatchdogPolicy : uint8
	{
		Reconnect = 0,
		LogOnly,
		FailFast
	};
	// CIGI input
	FString CigiBindAddr    = TEXT("0.0.0.0");
	int32   CigiPort        = 8888;

	// CIGI response output (IG → host: SOF heartbeat + HAT/HOT + LOS responses)
	FString CigiResponseAddr = TEXT("127.0.0.1");
	int32   CigiResponsePort = 8889;

	// Video output
	FString MulticastAddr   = TEXT("239.1.1.1");
	int32   MulticastPort   = 5004;
	int32   VideoBitrate    = 4'000'000;   // bps
	FString H264Preset      = TEXT("ultrafast");
	FString H264Tune        = TEXT("zerolatency");

	// Capture resolution
	int32   CaptureWidth    = 1920;
	int32   CaptureHeight   = 1080;
	float   FrameRate       = 30.0f;
	bool    bSwapRBReadback = false;
	EReadbackFormat ReadbackFormat = EReadbackFormat::Auto;
	int32   ReadbackReadyPolls = 2; // require N consecutive IsReady() polls before Lock()

	// Horizontal field of view in degrees (used for KLV metadata)
	float   HFovDeg         = 60.0f;

	// Cesium tile streaming tuning
	// TilePreloadFovScale inflates the FOV reported to Cesium so tiles beyond
	// the visible frustum are pre-fetched (1.0 = exact FOV, 2.0 = double).
	float   TilePreloadFovScale = 2.0f;
	// Maximum simultaneous tile HTTP requests (Cesium default is 20)
	int32   MaxSimultaneousTileLoads = 40;

	// Default camera start position (WGS-84) — used before first CIGI packet
	double  StartLatitude   = 38.8977;     // Washington DC
	double  StartLongitude  = -77.0365;
	double  StartAltitude   = 500.0;       // metres above WGS-84 ellipsoid
	float   StartYaw        = 0.0f;
	float   StartPitch      = -45.0f;      // look downward
	float   StartRoll       = 0.0f;

	// Default time-of-day (hours 0-24) used before first CIGI celestial packet
	float   StartHour       = 12.0f;

	// Encoder watchdog behavior
	EEncoderWatchdogPolicy EncoderWatchdogPolicy = EEncoderWatchdogPolicy::Reconnect;
	int32   EncoderWatchdogIntervalTicks = 150;

	// CIGI entity ID that drives the camera (all others → entity manager)
	int32   CameraEntityId  = 0;

	// Gimbal slew rate limit in degrees/second (0 = unlimited / instantaneous snap)
	float   GimbalMaxSlewRateDegPerSec = 0.0f;

	// Gimbal axis limits (degrees). Applied after every slew update.
	float   GimbalPitchMin = -90.0f;
	float   GimbalPitchMax =  30.0f;
	float   GimbalYawMin   = -180.0f;
	float   GimbalYawMax   =  180.0f;

	// FOV presets driven by Sensor Control Gain field (0.0=wide → 1.0=narrow).
	// Index is selected by linear mapping: idx = floor(gain * N), clamped to [0, N-1].
	// Empty = ignore Gain; use ViewDef FOV only.
	TArray<float> SensorFovPresets;

	// Per-waveband sensor simulation parameters (Phase 11).
	// Populated from "sensor_modes" JSON block; defaults applied if block is absent.
	TMap<ESensorMode, FSensorModeConfig> SensorModeConfigs;

	// Global quality profile applied to the sensor post-process pipeline.
	FString SensorQualityPreset = TEXT("medium");
	TMap<FString, FSensorQualityConfig> SensorQualityProfiles;
	FSensorQualityConfig ActiveSensorQuality;

	struct FOutputViewConfig
	{
		int32   ViewId = 0;
		bool    bEnabled = true;
		FString MulticastAddr;
		int32   MulticastPort = 5004;
		int32   VideoBitrate = 4'000'000;
		FString H264Preset = TEXT("ultrafast");
		FString H264Tune = TEXT("zerolatency");
		float   HFovDeg = 0.0f; // 0 = use live capture HFOV
	};

	// Optional multi-stream output views. If empty, CamSim emits one stream
	// using the root multicast/video settings above.
	TArray<FOutputViewConfig> OutputViews;

	struct FGroundTruthConfig
	{
		bool    bEnabled = false;
		FString OutputPath;
		int32   IntervalFrames = 1;
	};
	FGroundTruthConfig GroundTruth;

	struct FEntityScaleConfig
	{
		// 0 disables distance culling.
		float MaxDrawDistanceM = 0.0f;
		// 0 means tick every frame.
		float TickRateHz = 0.0f;
		// 0 means apply every incoming pose update.
		float DefaultMaxUpdateRateHz = 0.0f;
		// Optional per-entity max update-rate overrides by EntityId.
		TMap<int32, float> MaxUpdateRateHzOverrides;
	};
	FEntityScaleConfig EntityScale;

	struct FScenarioEntityConfig
	{
		int32 EntityId = 1;
		int32 EntityType = 1001;
		double StartLatitude = 38.8977;
		double StartLongitude = -77.0365;
		double StartAltitude = 500.0;
		float StartYaw = 0.0f;
		float StartPitch = 0.0f;
		float StartRoll = 0.0f;

		float SpawnTimeSec = 0.0f;
		float DespawnTimeSec = 0.0f; // <= SpawnTimeSec means persistent
		float UpdateRateHz = 10.0f;  // 0 = every manager tick

		// Scripted trajectory rates.
		float NorthRateMps = 0.0f;
		float EastRateMps = 0.0f;
		float UpRateMps = 0.0f;
		float YawRateDegPerSec = 0.0f;
		float PitchRateDegPerSec = 0.0f;
		float RollRateDegPerSec = 0.0f;
	};

	bool bScenarioEnabled = false;
	float ScenarioTimeScale = 1.0f;
	TArray<FScenarioEntityConfig> ScenarioEntities;

	/**
	 * Load from JSON file, then apply env var overrides.
	 * If OutJsonRoot is non-null the parsed JSON root is written to it so the
	 * caller can load auxiliary tables (e.g. FEntityTypeTable) from the same
	 * document without re-reading the file.
	 */
	static FCamSimConfig Load(TSharedPtr<FJsonObject>* OutJsonRoot = nullptr);

private:
	static void ApplyEnvOverrides(FCamSimConfig& Cfg);
};
