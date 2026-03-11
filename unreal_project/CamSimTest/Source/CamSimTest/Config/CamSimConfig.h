// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Sensor/SensorTypes.h"         // ESensorMode, sensor config structs
#include "Overlay/FHudOverlay.h"        // FHudOverlayConfig, ECrosshairStyle

/**
 * Runtime configuration for CamSim.
 *
 * Values are loaded from camsim_config.yaml in the binary directory.
 * Individual fields may be overridden via environment variables at startup.
 *
 * Env vars:
 *   CAMSIM_CIGI_PORT              - UDP port to listen for CIGI packets        (default 8888)
 *   CAMSIM_CIGI_BIND_ADDR         - Local address to bind the CIGI socket       (default 0.0.0.0)
 *   CAMSIM_CIGI_RESPONSE_ADDR     - Host IP for IG->host response packets        (default 127.0.0.1)
 *   CAMSIM_CIGI_RESPONSE_PORT     - Host's incoming CIGI port for responses     (default 8889)
 *   CAMSIM_MULTICAST_ADDR         - Multicast group for output stream; also overrides output_views routes when set (default 239.1.1.1)
 *   CAMSIM_MULTICAST_PORT         - UDP port for output stream; also overrides output_views routes when set (default 5004)
 *   CAMSIM_VIDEO_BITRATE          - Target H.264 bitrate in bps                  (default 4000000)
 *   CAMSIM_H264_PRESET            - libx264 preset string                        (default ultrafast)
 *   CAMSIM_SWAP_RB_READBACK       - Force red/blue swap on GPU readback          (default 0)
 *   CAMSIM_READBACK_READY_POLLS   - Consecutive IsReady polls before Lock         (default 2)
 *   CAMSIM_ENCODER_WATCHDOG_POLICY - reconnect|log_only|fail_fast                 (default reconnect)
 *   CAMSIM_ENCODER_WATCHDOG_INTERVAL_TICKS - watchdog check interval              (default 150)
 *   CAMSIM_START_HOUR             - Fallback time-of-day (0-24)                  (default 12.0)
 *   CAMSIM_SENSOR_QUALITY_PRESET  - low|medium|high|ultra|custom                 (default medium)
 *   CAMSIM_TERRAIN_PROVIDER       - geospatial terrain provider                    (default cesium)
 *   CAMSIM_IMAGERY_PROVIDER       - imagery provider                               (default cesium)
 *   CAMSIM_GROUND_TRUTH_ENABLED   - write JSONL sidecar telemetry                  (default 0)
 *   CAMSIM_GROUND_TRUTH_PATH      - sidecar output path                            (default empty)
 *   CAMSIM_GROUND_TRUTH_INTERVAL_FRAMES - sidecar cadence                          (default 1)
 *   CAMSIM_ENTITY_MAX_DRAW_DISTANCE_M - entity culling distance                    (default 0=disabled)
 *   CAMSIM_ENTITY_TICK_RATE_HZ    - entity actor tick rate                          (default 0=unlimited)
 *   CAMSIM_ENTITY_DEFAULT_MAX_UPDATE_RATE_HZ - default pose apply cap               (default 0=unlimited)
 *   CAMSIM_SCENARIO_ENABLED       - enable scenario_entities                        (default 0)
 *   CAMSIM_SCENARIO_TIME_SCALE    - scenario time multiplier                        (default 1.0)
 *   CAMSIM_MAX_SSE                - Cesium MaximumScreenSpaceError                  (default 2.0)
 *   CAMSIM_MAX_CACHED_MB          - Cesium tile cache budget in MB                  (default 2048)
 *   CAMSIM_ENCODER                - H.264 encoder: auto|nvenc|libx264               (default auto)
 *   CAMSIM_MAX_ENTITIES           - Max simultaneous entities                        (default 500)
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

	// CIGI response output (IG -> host: SOF heartbeat + HAT/HOT + LOS responses)
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

	// Geospatial provider selection (Phase F foundation).
	// Currently supported: "cesium".
	FString TerrainProvider = TEXT("cesium");
	FString ImageryProvider = TEXT("cesium");

	// Cesium tile streaming tuning
	// TilePreloadFovScale inflates the FOV reported to Cesium so tiles beyond
	// the visible frustum are pre-fetched (1.0 = exact FOV, 2.0 = double).
	float   TilePreloadFovScale = 2.0f;
	// Maximum simultaneous tile HTTP requests (Cesium default is 20)
	int32   MaxSimultaneousTileLoads = 40;
	// Cesium LOD quality: lower = sharper terrain (Cesium default 16; 2 = high quality ISR)
	float   MaximumScreenSpaceError = 2.0f;
	// Tile cache budget in MB (0 = Cesium default / uncapped)
	int32   MaximumCachedBytesMB = 2048;

	// Default camera start position (WGS-84) -- used before first CIGI packet
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
	int32   WatchdogMaxReconnects = 3;

	// Encoder selection: "auto" tries NVENC first, falls back to libx264.
	// Explicit values: "nvenc", "libx264".
	FString Encoder = TEXT("auto");

	// Entity scalability
	int32   MaxEntities = 500;
	bool    bUseInstancedRendering = true;

	// GPU sensor post-processing (Phase 5). When true, sensor effects run as
	// post-process materials on the GPU before readback -- CPU pipeline is skipped.
	// Set false for Mesa llvmpipe compatibility (CPU fallback).
	bool    bGpuSensorEffects = false;

	// CIGI entity ID that drives the camera (all others -> entity manager)
	int32   CameraEntityId  = 0;

	// Gimbal slew rate limit in degrees/second (0 = unlimited / instantaneous snap)
	float   GimbalMaxSlewRateDegPerSec = 0.0f;

	// Gimbal axis limits (degrees). Applied after every slew update.
	float   GimbalPitchMin = -90.0f;
	float   GimbalPitchMax =  30.0f;
	float   GimbalYawMin   = -180.0f;
	float   GimbalYawMax   =  180.0f;

	// FOV presets driven by Sensor Control Gain field (0.0=wide -> 1.0=narrow).
	// Index is selected by linear mapping: idx = floor(gain * N), clamped to [0, N-1].
	// Empty = ignore Gain; use ViewDef FOV only.
	TArray<float> SensorFovPresets;

	// Per-waveband sensor simulation parameters (Phase 11).
	// Populated from "sensor_modes" YAML block; defaults applied if block is absent.
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

	// Telemetry JSONL sidecar (pre-Phase 17 ground truth stream).
	// Distinct from FMLTrainingConfig: writes per-frame KLV telemetry to a JSONL
	// file alongside the video stream (used by MultiViewFrameSink).
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

	// Security metadata for MISB ST 0102 (Phase 12A)
	struct FSecurityMetadataConfig
	{
		FString Classification     = TEXT("UNCLASSIFIED");
		FString ClassifyingCountry = TEXT("//US");
		FString ObjectCountryCodes = TEXT("US");
		FString Caveats;
		FString ReleasingInstructions;
	};
	FSecurityMetadataConfig SecurityMetadata;

	// Video codec: "h264" or "h265" (Phase 12B)
	FString VideoCodec = TEXT("h264");

	// Prometheus metrics file path (empty = disabled). Phase 12D.
	// A Prometheus node_exporter textfile-collector compatible .prom file.
	FString PrometheusMetricsPath;

	// Recording & Playback (Phase 12E)
	struct FRecordingConfig
	{
		FString CigiRecordPath;      // empty = no CIGI recording
		FString VideoRecordPath;     // empty = no local .ts recording
		FString CigiPlaybackPath;    // empty = live UDP input
	};
	FRecordingConfig Recording;

	// ML Training Data Generation (Phase 17)
	//   CAMSIM_ML_ENABLED             - master toggle                  (default 0)
	//   CAMSIM_ML_OUTPUT_DIR          - base output directory          (default <BinaryDir>/ml_output)
	//   CAMSIM_ML_DEPTH_ENABLED       - write 16-bit PNG depth maps    (default 1)
	//   CAMSIM_ML_BBOX_ENABLED        - project entity AABB to screen  (default 1)
	//   CAMSIM_ML_COCO_ENABLED        - write COCO JSONL sidecar       (default 1)
	//   CAMSIM_ML_VOC_ENABLED         - write Pascal VOC XML per frame (default 0)
	//   CAMSIM_ML_INTERVAL_FRAMES     - annotation cadence             (default 1)
	//   CAMSIM_ML_DEPTH_FAR_PLANE_M   - depth quantization ceiling (m) (default 5000)
	struct FMLTrainingConfig
	{
		bool    bEnabled                 = false;
		FString OutputDir;                       // empty → <BinaryDir>/ml_output
		int32   AnnotationIntervalFrames = 1;
		bool    bDepthMap                = true;   // 17A: 16-bit PNG depth
		bool    bBoundingBoxes           = true;   // 17D: project entity AABB to screen
		bool    bCocoExport              = true;   // 17G: streaming COCO JSONL
		bool    bVocExport               = false;  // 17H: Pascal VOC XML per frame
		float   DepthFarPlaneM           = 5000.0f;
	};
	FMLTrainingConfig MLTraining;

	// Optical realism effects (Phase 15)
	struct FOpticalRealismConfig
	{
		bool bEnabled = false;              // master toggle (false = clean ML frames)
		// 15A Motion Blur
		bool  bMotionBlur = true;
		float MotionBlurAmount = 0.5f;      // [0,1]
		int32 MotionBlurMax = 5;            // max blur pixels
		// 15B Lens Distortion (CPU-side Brown-Conrady)
		bool  bLensDistortion = false;
		float DistortionK1 = 0.0f;         // + barrel, - pincushion
		float DistortionK2 = 0.0f;
		// 15C Bloom
		bool  bBloom = true;
		float BloomIntensity = 0.675f;
		float BloomThreshold = -1.0f;       // -1 = auto
		// 15D Chromatic Aberration
		bool  bChromaticAberration = false;
		float ChromaticAberrationIntensity = 0.0f; // 0-5
		// 15E Depth of Field
		bool  bDepthOfField = false;
		float FocalDistance = 0.0f;         // cm, 0 = auto-focus from LOS
		float ApertureFStop = 4.0f;
		float SensorWidth = 24.576f;        // mm
		// 15F Lens Flare
		bool  bLensFlare = false;
		float LensFlareIntensity = 1.0f;
		float LensFlareBokehSize = 3.0f;
		float LensFlareThreshold = 8.0f;
	};
	FOpticalRealismConfig OpticalRealism;

	// Weather, Atmosphere & Particle Effects (Phase 18)
	struct FPhase18Config
	{
		/** YAML-configured position for a regional weather zone (18L). */
		struct FWeatherZoneConfig
		{
			int32  ZoneID  = 0;
			double LatDeg  = 0.0;
			double LonDeg  = 0.0;
			float  RadiusM = 10000.0f;
		};
		// 18C Second fog layer (low-lying mist)
		bool  bSecondFog        = false;
		float FogDensity        = 0.02f;  // [0,1]
		float FogHeightFalloff  = 0.2f;   // UE ExponentialHeightFog param
		// 18D Precipitation overlay (CPU pixel pass)
		bool  bPrecipitation    = false;
		float RainIntensity     = 0.0f;   // [0,1] — drop density
		float SnowIntensity     = 0.0f;   // [0,1] — flake density
		// 18E God rays / light shafts
		bool  bGodRays          = false;
		float GodRayIntensity   = 1.0f;
		// 18J Sky atmospheric scattering overrides
		bool  bAtmosphericScattering = false;
		float RayleighScattering     = 1.0f; // multiplier on Rayleigh coefficient
		float MieScattering          = 1.0f; // multiplier on Mie coefficient
		// 18K Dynamic IR extinction driven by atmospheric visibility
		bool  bDynamicIRExtinction   = false;
		float VisibilityRangeM       = 10000.0f; // metres — overrides sensor_modes IR coeff

		// 18A/18B Volumetric cloud shadow strength (cloud actor exists in scene)
		bool  bVolumetricClouds   = false;
		float CloudShadowStrength = 0.6f;   // [0,1] shadow intensity on terrain

		// 18L Regional weather zones — positions from YAML, parameters from CIGI RegionId
		bool                       bWeatherZones = false;
		TArray<FWeatherZoneConfig> WeatherZoneConfigs;  // up to 16, populated from YAML array

		// 18F/18G/18H Niagara particle FX — soft asset paths, authored in UE editor
		FString NiagaraRotorWash  = TEXT("/Game/Effects/NS_RotorWash");
		FString NiagaraSmoke      = TEXT("/Game/Effects/NS_Smoke");
		FString NiagaraFire       = TEXT("/Game/Effects/NS_Fire");
		FString NiagaraContrail   = TEXT("/Game/Effects/NS_Contrail");
		float   ContrailAltM      = 8000.0f;
		// NOTE: speed threshold not enforced — FCigiEntityState has no velocity field.
		float   ContrailSpeedMs   = 100.0f;
		int32   SmokeComponentID  = 1;
		int32   FireComponentID   = 2;

		// 18I Decal cratering
		// NOTE: FCigiComponentControl has no CompData field — radius uses config default only.
		FString CraterDecalMaterial     = TEXT("/Game/Effects/M_Crater");
		int32   CraterImpactComponentID = 10;
		int32   MaxCraters              = 32;
		float   CraterDefaultRadiusM    = 5.0f;
	};
	FPhase18Config Phase18;

	// HUD/OSD overlay burn-in (Phase 20)
	FHudOverlayConfig OverlayConfig;

	// Phase 13C: set to true when config was loaded (or defaults are valid).
	// Set to false only if YAML parsing fails AND no defaults are available.
	bool bLoadedSuccessfully = true;

	/** Load from YAML file, then apply env var overrides. */
	static FCamSimConfig Load();

	/** Return the path to the resolved config file (for re-parsing by other modules). */
	static FString GetConfigFilePath();

private:
	static void ApplyEnvOverrides(FCamSimConfig& Cfg);
};
