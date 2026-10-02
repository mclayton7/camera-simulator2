// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Sensor/SensorTypes.h"         // ESensorMode, sensor config structs
#include "Thermal/ThermalTypes.h"        // FThermalMaterialSpec

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
 *   CAMSIM_READBACK_READY_POLLS   - Consecutive IsReady polls before Lock         (default 2)
 *   CAMSIM_ENCODER_WATCHDOG_POLICY - reconnect|log_only|fail_fast                 (default reconnect)
 *   CAMSIM_ENCODER_WATCHDOG_INTERVAL_TICKS - watchdog check interval              (default 150)
 *   CAMSIM_START_HOUR             - Fallback time-of-day (0-24)                  (default 12.0)
 *   CAMSIM_SENSOR_QUALITY_PRESET  - low|medium|high|ultra|custom                 (default medium)
 *   CAMSIM_TERRAIN_PROVIDER       - geospatial terrain provider                    (default cesium)
 *   CAMSIM_IMAGERY_PROVIDER       - imagery provider                               (default cesium)
 *   CAMSIM_ENTITY_MAX_DRAW_DISTANCE_M - entity culling distance                    (default 0=disabled)
 *   CAMSIM_ENTITY_TICK_RATE_HZ    - entity actor tick rate                          (default 0=unlimited)
 *   CAMSIM_ENTITY_DEFAULT_MAX_UPDATE_RATE_HZ - default pose apply cap               (default 0=unlimited)
 *   CAMSIM_FPS_ENTITY_ID          - FPS entity (0=disabled)                          (default 0)
 *   CAMSIM_FPS_EYE_HEIGHT_M       - eye height above entity origin                   (default 1.7)
 *   CAMSIM_MAX_SSE                - Cesium MaximumScreenSpaceError                  (default 2.0)
 *   CAMSIM_MAX_CACHED_MB          - Cesium tile cache budget in MB                  (default 2048)
 *   CAMSIM_ENCODER                - H.264 encoder: auto|nvenc|libx264               (default auto)
 *   CAMSIM_MAX_ENTITIES           - Max simultaneous entities                        (default 500)
 */
struct FCamSimConfig
{
	enum class EEncoderPreference : uint8
	{
		Auto    = 0,
		Nvenc,
		LibX264,
		LibX265,
		VideoToolbox
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
	int32   ReadbackReadyPolls = 1; // require N consecutive IsReady() polls before Lock()

	// Horizontal field of view in degrees (used for KLV metadata)
	float   HFovDeg         = 60.0f;

	// Geospatial provider selection (Phase F foundation).
	// Currently supported: "cesium".
	FString TerrainProvider = TEXT("cesium");
	FString ImageryProvider = TEXT("cesium");

	// Cesium tile streaming tuning
	// Enable Cesium's per-frame tile selection logging (very verbose).
	// Env: CAMSIM_LOG_TILE_STATS
	bool    bLogTileSelectionStats = false;
	// TilePreloadFovScale inflates the FOV on the prefetch camera so tiles beyond
	// the visible frustum are pre-fetched (1.0 = exact FOV, 1.5 = 50% wider).
	// Too high wastes loading slots on off-screen tiles.
	float   TilePreloadFovScale = 1.5f;
	// Maximum simultaneous tile HTTP requests (Cesium default is 20).
	// Keep ≤20 for real-time 30fps; higher values load faster but stall the game thread.
	int32   MaxSimultaneousTileLoads = 20;
	// Cesium LOD quality: lower = sharper terrain (Cesium default 16).
	// For quantized-mesh terrain (CWT), internally divided by 8: so 8.0 → effective 1.0 px.
	float   MaximumScreenSpaceError = 16.0f;
	// Detail kept for tiles outside the view (Cesium CulledScreenSpaceError; higher =
	// coarser). Tiles that enter the view after a gimbal snap show at this detail until
	// they refine to MaximumScreenSpaceError. 0 = auto (= MaximumScreenSpaceError).
	// Env: CAMSIM_CULLED_SSE
	float   CulledScreenSpaceError = 0.0f;
	// Cesium tile-selection frustum culling (EnableFrustumCulling; UE's render
	// culling is unaffected). Off by default: Cesium applies CulledScreenSpaceError
	// only to tiles culled by a disabled stage, so with this on, tiles outside the
	// view stay coarse and a gimbal snap shows them until they refine (~2 s).
	// Env: CAMSIM_FRUSTUM_CULLING
	bool    bFrustumCulling = false;
	// Tile cache budget in MB per tileset (0 = Cesium default / uncapped).
	// Large cache avoids re-downloading tiles when revisiting areas.
	int32   MaximumCachedBytesMB = 2048;
	// Max descendant tiles to load simultaneously (Cesium default 20; higher = better low-alt detail)
	// Env: CAMSIM_LOADING_DESCENDANT_LIMIT
	int32   LoadingDescendantLimit = 20;
	// Cesium's dithered LOD crossfade (UseLodTransitions). Needs temporal AA
	// (TSR, the primary view's anti-aliasing) to resolve the dither.
	// Off by default: Cesium updates the fade of every tile in the render set each
	// frame, off-screen tiles included, which cost ~6 ms of game thread and doubled
	// streaming hitches once culled_screen_space_error kept full detail off screen.
	// Env: CAMSIM_USE_LOD_TRANSITIONS
	bool    bUseLodTransitions = false;
	// Duration of LOD crossfade in seconds (only used when bUseLodTransitions=true)
	// Env: CAMSIM_LOD_TRANSITION_LENGTH
	float   LodTransitionLength = 0.5f;
	// Cook collision for Cesium tiles. CIGI HAT/HOT and LOS queries and the KLV
	// frame centre (Tags 21, 23-25, 78) are line traces against it.
	// Env: CAMSIM_CREATE_PHYSICS_MESHES
	bool    bCreatePhysicsMeshes = true;

	// Default camera start position (WGS-84) -- used before first CIGI packet
	double  StartLatitude   = 38.8977;     // Washington DC
	double  StartLongitude  = -77.0365;
	double  StartAltitude   = 500.0;       // metres above WGS-84 ellipsoid
	float   StartYaw        = 0.0f;
	float   StartPitch      = -45.0f;      // look downward
	float   StartRoll       = 0.0f;

	// Sim clock start (UTC), until a CIGI Celestial Sphere Control sets it.
	// StartDatetime (ISO 8601, e.g. "2025-03-01T06:30:00Z") wins; otherwise
	// StartHour >= 0 sets that UTC hour on today's date; otherwise the clock
	// starts at the wall-clock time.
	FString StartDatetime;
	float   StartHour       = -1.0f;
	// Sim time rate: 1 = real time, 0 = frozen, >1 faster than real time.
	float   SimTimeRate     = 1.0f;

	// Encoder watchdog behavior
	EEncoderWatchdogPolicy EncoderWatchdogPolicy = EEncoderWatchdogPolicy::Reconnect;
	int32   EncoderWatchdogIntervalTicks = 150;
	int32   WatchdogMaxReconnects = 3;

	// Encoder selection: "auto" tries NVENC, then libx264/libx265. Explicit
	// values: "nvenc", "videotoolbox" (macOS, opt-in: overshoots the rate cap
	// on noisy scenes), "libx264", "libx265".
	//
	// Phase 4: the raw string is preserved (for YAML/env-var round-trip
	// compatibility) and parsed into EncoderPref at load time. Consumers
	// should use EncoderPref for runtime branching — see VideoEncoder.cpp.
	FString Encoder = TEXT("auto");
	EEncoderPreference EncoderPref = EEncoderPreference::Auto;

	// Entity scalability
	int32   MaxEntities = 500;

	// CIGI entity ID that drives the camera (all others -> entity manager)
	int32   CameraEntityId  = 0;

	// Phase 22G: First-person view
	// 0 = disabled. Non-zero attaches camera to the named entity.
	// Overridden at runtime by FCigiViewControl.EntityId != 0.
	int32   FpsEntityId     = 0;
	float   FpsEyeHeightM   = 1.7f;  // metres above entity origin

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
	//   CAMSIM_ML_INTERVAL_FRAMES     - annotation cadence             (default 1)
	//   CAMSIM_ML_DEPTH_FAR_PLANE_M   - depth quantization ceiling (m) (default 5000)
	//   CAMSIM_ML_MIN_VISIBLE_PIXELS  - drop annotations with fewer visible px (default 1, min 1)
	//   CAMSIM_ML_SEGMENTATION_ENABLED - COCO RLE segmentation per annotation (default 1)
	struct FMLTrainingConfig
	{
		bool    bEnabled                 = false;
		FString OutputDir;                       // empty → <BinaryDir>/ml_output
		int32   AnnotationIntervalFrames = 1;
		bool    bDepthMap                = true;   // 17A: 16-bit PNG depth
		bool    bBoundingBoxes           = true;   // 17D: project entity AABB to screen
		bool    bCocoExport              = true;   // 17G: streaming COCO JSONL
		float   DepthFarPlaneM           = 5000.0f;
		int32   MinVisiblePixels         = 1;      // ml_training.min_visible_pixels (>= 1)
		bool    bSegmentation            = true;   // ml_training.segmentation: COCO RLE modal mask
	};
	FMLTrainingConfig MLTraining;

	// Phase 24 — Rendering Quality
	struct FRenderingQualityConfig
	{
		// 24A: Entity shadow casting + contact shadows
		// Env: CAMSIM_ENTITY_SHADOWS / CAMSIM_CONTACT_SHADOWS
		bool  bEntityShadows      = true;
		bool  bContactShadows     = false;   // expensive; off by default
		float ContactShadowLength = 0.1f;    // fraction of screen height [0,1]

		// 24B: Ambient occlusion (Lumen GTAO intensity/radius)
		// Env: CAMSIM_AO_INTENSITY / CAMSIM_AO_RADIUS
		float AOIntensity = 0.0f;    // [0,1]; 0 = off
		float AORadius    = 200.0f;  // UE cm units

		// Ray tracing backend (BLAS/TLAS allocation). DefaultEngine.ini default is False.
		// Set True on NVIDIA builds that want RT features. Requires project restart.
		// Env: CAMSIM_RT_ENABLED
		bool bRayTracingEnabled = false;

		// 24E: Shadow distance and VSM quality
		// Env: CAMSIM_SHADOW_DISTANCE_SCALE / CAMSIM_VSM_RESOLUTION_BIAS / CAMSIM_VSM_MAX_PAGES
		float ShadowDistanceScale = 1.0f;   // multiplies engine max shadow distance
		int32 VSMResolutionBias   = 0;      // 0 = engine default
		int32 VSMMaxPhysicalPages = 2048;   // engine default

		// 24F: TSR screen percentage (100 = native; >100 = super-sample for entity edge quality)
		// Env: CAMSIM_TSR_SCREEN_PERCENTAGE
		int32 TSRScreenPercentage = 100;
	};
	FRenderingQualityConfig RenderingQuality;

	// Phase 27 — Performance & Optimization
	struct FPerformanceConfig
	{
		// 27G Texture Paging Budget
		int32 TexturePoolBudgetMB = 0;

		// 28G Per-Frame Latency Tracking
		bool  bTrackPipelineLatency = false;
	};
	FPerformanceConfig Performance;

	// Weather & Atmosphere (Phase 18)
	struct FPhase18Config
	{
		// 18C Second fog layer (low-lying mist)
		bool  bSecondFog        = false;
		float FogDensity        = 0.02f;  // [0,1]
		float FogHeightFalloff  = 0.2f;   // UE ExponentialHeightFog param
		// 18K Visibility range (metres)
		float VisibilityRangeM       = 10000.0f;

		// 18A/18B Volumetric cloud shadow strength (cloud actor exists in scene)
		bool  bVolumetricClouds   = false;
		float CloudShadowStrength = 0.6f;   // [0,1] shadow intensity on terrain
	};
	FPhase18Config Phase18;

	/** Ocean surface for boats (ROADMAP 2.6). */
	struct FOceanConfig
	{
		bool    bEnabled          = true;     // startup only
		float   Beaufort          = 3.0f;     // 0–12, fractional; used while no CIGI Wave Control wave is enabled
		float   WaveDirectionDeg  = 270.0f;   // direction waves come FROM, true north
		float   Choppiness        = 0.5f;     // 0 = sine, 1 = steepest without looping
		bool    bVesselMotion     = true;     // boats pitch/roll/heave with the waves
		float   VesselMotionScale = 1.0f;
		float   MaxRadiusKm       = 400.0f;   // horizon cap for the ocean mesh
		float   WaterTemperatureC = 15.0f;    // initial water temperature (thermal water class) until CIGI Maritime Surface sets it
		FString MaterialPath      = TEXT("/Game/Ocean/M_Ocean");
	};
	FOceanConfig Ocean;

		/** Thermal radiance for IR (ROADMAP 4A, docs/thermal.md). */
	struct FThermalConfig
	{
		// Startup decides whether thermal is available (and entities get stencils for it); false
		// runs IR as the 3B.2 luminance proxy (restart to A/B compare).
		bool  bEnabled            = true;
		float AirTemperatureC     = 15.0f;   // daily mean, until CIGI Atmosphere Control sets it
		float AirDiurnalSwingK    = 8.0f;    // peak-to-peak; T_air = mean + swing/2 cos(w (t - 15 h local solar))
		float ExtinctionPerKmMwir = 0.15f;   // band extinction, per km
		float ExtinctionPerKmLwir = 0.10f;
		float FogIrFactor         = 0.4f;    // beta_fog = 3.912 / V_km * factor (IR sees farther than visible)
		TArray<FThermalMaterialSpec> Materials;   // thermal.materials overrides / additions (yaml only)

		/** Terrain thermal classes from land cover (ROADMAP 4B, docs/thermal.md). */
		struct FLandCoverConfig
		{
			bool    bEnabled         = true;
			FString Dir              = TEXT("Content/NonUFS/LandCover");   // index.json + tiles; relative to the project directory
			int32   WindowTexels     = 2048;    // camera-centred window, 10 m texels (2048 = 20.48 km)
			float   RecentreFraction = 0.25f;   // rebuild when the camera is this fraction of the window from its centre
			float   VegIndexLo       = 0.05f;   // base-colour excess green where the vegetation weight starts
			float   VegIndexHi       = 0.20f;   // ... and reaches 1
			float   AsphaltMaxLuma   = 0.12f;   // built-up: linear base luminance below which ground is asphalt (soft ramp)
			float   WarpAmplitudeM   = 6.0f;    // geo-anchored domain warp of the lookup (breaks the 10 m grid); 0 = off
			float   WarpCellM        = 20.0f;   // warp noise lattice spacing on the ground
			float   VegBlurM         = 2.0f;    // vegetation index from base colour blurred over this ground radius; 0 = off
			TArray<FLandCoverClassSpec> Classes;   // WorldCover code -> material overrides (yaml only)

			bool operator==(const FLandCoverConfig&) const = default;
		};
		FLandCoverConfig LandCover;
	};
	FThermalConfig Thermal;

	/** Cesium backend: ion server, terrain source, imagery overlay */
	struct FCesiumBackendConfig
	{
		// UCesiumIonServer has two distinct URL fields:
		//   ServerUrl — portal/OAuth redirect URL (default: "https://ion.cesium.com")
		//   ApiUrl    — REST tile API endpoint    (default: "https://api.cesium.com")
		// For self-hosted ion these are typically different hosts.
		FString IonPortalUrl = TEXT("https://ion.cesium.com");  // → UCesiumIonServer::ServerUrl
		FString IonApiUrl    = TEXT("https://api.cesium.com");  // → UCesiumIonServer::ApiUrl
		// IonToken: never log at any verbosity level.
		// DefaultIonAccessTokenId not set — only needed for Editor sign-in UI, not headless use.
		FString IonToken = TEXT("");  // empty = use level asset default

		struct FTerrainConfig
		{
			FString Source     = TEXT("cesium_ion"); // "cesium_ion" | "url" | "flat"
			int32   IonAssetId = 1;                  // Cesium World Terrain
			FString Url        = TEXT("");
		} Terrain;

		struct FImageryConfig
		{
			FString Source        = TEXT("cesium_ion"); // "cesium_ion" | "wms" | "none"
			int32   IonAssetId    = 2;                  // Bing Maps Aerial
			FString WmsUrl        = TEXT("");
			FString WmsLayers     = TEXT("");
			int32   WmsTileWidth  = 256;
			int32   WmsTileHeight = 256;
			// Raster overlay quality: applied before Activate() in ApplyCesiumBackendConfig.
			// Overlay SSE controls imagery resolution: 2.0 = 1 source px covers 2x2 screen px,
			// 1.0 = 1:1 pixel mapping (sharpest). Plugin default 2.0; ISR needs 1.0.
			// Env: CAMSIM_CESIUM_IMAGERY_MAX_SSE
			double  MaximumScreenSpaceError    = 2.0;
			// 0 = plugin default (2048). 2048 is Cesium default; sufficient at 720p.
			// Env: CAMSIM_CESIUM_IMAGERY_MAX_TEXTURE_SIZE
			int32   MaximumTextureSize         = 2048;
			// Concurrent HTTP requests for imagery tiles (plugin default 20).
			// Env: CAMSIM_CESIUM_IMAGERY_MAX_TILE_LOADS
			int32   MaximumSimultaneousTileLoads = 20;
		} Imagery;
	} CesiumBackend;

	// Phase 21 — DIS (IEEE 1278.1) Protocol
	//   CAMSIM_DIS_ENABLED              - master toggle                  (default 0)
	//   CAMSIM_DIS_PORT                 - UDP port                       (default 3000)
	//   CAMSIM_DIS_BIND_ADDR            - bind address                   (default 0.0.0.0)
	//   CAMSIM_DIS_MULTICAST_GROUP      - multicast group                (default empty)
	//   CAMSIM_DIS_EXERCISE_ID          - exercise filter (0=any)        (default 1)
	//   CAMSIM_DIS_HEARTBEAT_TIMEOUT    - entity removal timeout (sec)   (default 12.0)
	//   CAMSIM_DIS_ID_BASE_OFFSET       - CamSim ID range start         (default 1000)
	//   CAMSIM_DIS_DEFAULT_ENTITY_TYPE  - fallback entity type ID        (default 1001)
	struct FDisConfig
	{
		bool    bEnabled              = false;
		FString BindAddr              = TEXT("0.0.0.0");
		int32   Port                  = 3000;
		FString MulticastGroup;                          // empty = no multicast join
		int32   ExerciseId            = 1;               // 0 = accept all exercises
		int32   SiteId                = 1;
		int32   ApplicationId         = 1;
		float   HeartbeatTimeoutSec   = 12.0f;
		int32   DefaultEntityTypeId   = 1001;            // fallback CamSim entity type

		// Place land entities on the terrain and surface entities on the water
		// (senders without terrain). false = use the sender's altitude/attitude.
		// Env: CAMSIM_DIS_CLAMP_TO_SURFACE
		bool    bClampToSurface       = true;

		// DIS entity type → CamSim type ID mappings from YAML
		// Key: "kind:domain:country:category:subcategory:specific:extra"
		// Value: CamSim uint16 entity type ID
		TMap<FString, uint16> EntityTypeMappings;
	};
	FDisConfig DIS;

	// Phase 26 — Standards Compliance (MISB ST 0601.9 + BCC-16)
	struct FPhase26Config
	{
		// Tag 4: Platform Tail Number (ISO 646, up to 127 chars; empty = omit)
		FString PlatformTailNumber;

		// Tag 3: Mission ID (ISO 646, up to 127 chars; empty = omit)
		FString MissionId;

		// Tag 10: Platform Designation, e.g. "MQ-1B" (ISO 646, up to 127 chars; empty = omit)
		FString PlatformDesignation;

		// Tag 59: Platform Call Sign (ISO 646, up to 127 chars; empty = omit)
		FString PlatformCallSign;

		// Tags 90/91: full-range platform pitch/roll (int32, ±90°). Off by default:
		// misb.js 0.1.30, the downstream reference decoder, decodes them as ~0°.
		bool bKlvFullRangeAttitude = false;

		// Tag 43: Target Track Gate Width (pixels, 0 = omit)
		float TargetTrackGateWidth = 0.0f;

		// Tag 44: Target Track Gate Height (pixels, 0 = omit)
		float TargetTrackGateHeight = 0.0f;
	};
	FPhase26Config Phase26;

	// Terrain readiness gate: hold frame output until Cesium tiles for the
	// view have loaded (startup and teleports). See FTerrainReadinessGate.
	struct FTerrainGateConfig
	{
		bool   bEnabled           = true;
		float  MinLoadProgressPct = 99.0f;   // 0-100
		float  TimeoutSec         = 30.0f;
		double TeleportDistanceM  = 5000.0;
	};
	FTerrainGateConfig TerrainGate;

	// Phase 28 — Operational Hardening
	struct FOperationalConfig
	{
		// 28B: Structured JSON logging sidecar
		FString StructuredLogPath;           // empty = disabled
		int32   StructuredLogMaxMB = 100;    // rotation threshold

		// 28C: HTTP health endpoints
		// Default true so the sim-environment orchestrator (Docker Compose)
		// can probe /live, /ready, /metrics out of the box. K8s deployments
		// that don't want the server can set CAMSIM_HEALTH_HTTP_ENABLED=0.
		bool  bHealthHttpEnabled = true;
		int32 HealthHttpPort     = 8080;

		// 3A: GET /snapshot returns the next grabbed frame as PNG (bench harness).
		bool    bSnapshotEndpointEnabled = false;
		// 3A: per-frame JSONL render stats for the bench harness (empty = disabled).
		FString FrameStatsPath;
	};
	FOperationalConfig Operational;

	// ROADMAP 3A — render path
	struct FRenderConfig
	{
		// Pose jumps above either threshold in one frame reset TSR history.
		// Env: CAMSIM_RENDER_CAMERA_CUT_DISTANCE_M / CAMSIM_RENDER_CAMERA_CUT_ANGLE_DEG
		float CameraCutDistanceM = 500.0f;
		float CameraCutAngleDeg  = 30.0f;

		// Rebase the Cesium georeference when the camera is this far from the
		// origin, in metres. 0 = disabled. Env: CAMSIM_RENDER_ORIGIN_SHIFT_DISTANCE_M
		double OriginShiftDistanceM = 20000.0;
	};
	FRenderConfig Render;

	// Phase 13C: set to true when config was loaded (or defaults are valid).
	// Set to false only if YAML parsing fails AND no defaults are available.
	bool bLoadedSuccessfully = true;

	/** Load from YAML file, then apply env var overrides. */
	static FCamSimConfig Load();

	/** Parse YAML text (defaults for anything absent), then apply env var overrides. */
	static FCamSimConfig LoadFromYamlString(const FString& YamlContent, const FString& SourceName = TEXT("<string>"));

	/** YAML keys no setting reads (typos, removed settings), as dotted paths. Warned at load. */
	TArray<FString> UnknownYamlKeys;

	/** Return the path to the resolved config file (for re-parsing by other modules). */
	static FString GetConfigFilePath();

	/** Pre-flight validation — returns empty array if config is valid. */
	TArray<FString> Validate() const;

	/** Settings that are valid but probably unintended (e.g. a PSF that exceeds the sensor
	 *  graph's GPU budget). Logged as warnings at startup; never blocks the config. */
	TArray<FString> ValidateWarnings() const;

private:
	static FCamSimConfig LoadFromYaml(const FString* YamlContent, const FString& YamlPath);
	static void ApplyEnvOverrides(FCamSimConfig& Cfg);
};
