# Configuration Reference

CamSim reads `camsim_config.yaml` from the project directory (or the binary
directory as a fallback). Environment variables override any YAML value. Boolean
variables accept `1`/`0`, `true`/`false`, `yes`/`no` and `on`/`off`.

**YAML format:** The config file uses YAML with native `#` comments for clean,
readable configuration. Keys are unquoted; string values only need quotes when
they contain special YAML characters.

**Canonical file:** `deploy/camsim_config.yaml` is the single source of truth
for all three deployment modes. `run.sh` copies it into the UE project
directory before launch; the Docker image bakes it into the project directory
(`/opt/camsim/CamSimTest/camsim_config.yaml`, mount over it to override). Edit only `deploy/camsim_config.yaml`; the project-dir copy
is generated and is listed in `.gitignore`.

**Unknown keys:** a key that no setting reads (a typo such as `cigi_prot`, or a
removed setting) is logged at load as
`Config: unknown key 'section.key' in <file> is ignored`, instead of silently
falling back to the default. The `CamSim.Config.CanonicalConfigHasNoUnknownKeys`
test keeps `deploy/camsim_config.yaml` free of them.

## Full Example

```yaml
# CamSim Configuration

cigi_bind_addr: "0.0.0.0"
cigi_port: 8888
cigi_response_addr: "127.0.0.1"
cigi_response_port: 8889
camera_entity_id: 0

multicast_addr: "239.1.1.1"
multicast_port: 5004
video_bitrate: 4000000
h264_preset: ultrafast
h264_tune: zerolatency
video_codec: h264
encoder: auto

capture_width: 1920
capture_height: 1080
frame_rate: 30.0
readback_ready_polls: 2
encoder_watchdog_policy: reconnect
encoder_watchdog_interval_ticks: 150
watchdog_max_reconnects: 3
hfov_deg: 60.0
terrain_provider: cesium
imagery_provider: cesium

tile_preload_fov_scale: 2.0
max_simultaneous_tile_loads: 40
maximum_screen_space_error: 2.0
maximum_cached_bytes_mb: 2048

start_latitude: 32.9768
start_longitude: -114.2665
start_altitude: 1500.0
start_yaw: 200.0
start_pitch: 0.0
start_roll: 0.0
# start_datetime: "2025-03-01T06:30:00Z"   # sim clock start (default: wall-clock now)
# start_hour: 12.0                         # or: this UTC hour, today
sim_time_rate: 1.0

gimbal_max_slew_rate: 0.0
gimbal_pitch_min: -90.0
gimbal_pitch_max: 30.0
gimbal_yaw_min: -180.0
gimbal_yaw_max: 180.0
sensor_fov_presets:
  - 60.0
  - 20.0
  - 5.0

max_entities: 500

output_views:
  - view_id: 0
    enabled: true
    multicast_addr: "239.1.1.1"
    multicast_port: 5004
    video_bitrate: 4000000
    h264_preset: ultrafast
    h264_tune: zerolatency
    hfov_deg: 0.0

entity_scale:
  max_draw_distance_m: 0.0
  tick_rate_hz: 0.0
  default_max_update_rate_hz: 0.0
  max_update_rate_hz_overrides:
    "1": 30.0

sensor_modes:
  eo:
    preset: eo_hd_cmos
    seed: 1
    exposure:
      min_gain_ev: -20
      max_photon_gain_ev: -12.5
      target_grey: 0.18
      highlight_percentile: 0.99
      lag_frames: 2
      manual_gain_ev: -12
  ir:
    preset: mwir_cooled
    seed: 1
    agc_enabled: true
    agc_low_percentile: 0.01
    agc_high_percentile: 0.99
    agc_lag_frames: 2
    exposure:
      min_gain_ev: -20
      max_photon_gain_ev: -6
      target_grey: 0.18
      highlight_percentile: 0.99
      lag_frames: 2
      manual_gain_ev: -12

security_metadata:
  classification: "UNCLASSIFIED"
  classifying_country: "//US"
  object_country_codes: "US"
  caveats: ""
  releasing_instructions: ""

recording:
  cigi_record_path: ""
  video_record_path: ""
  cigi_playback_path: ""

entity_types:
  "1001":
    mesh: f16/f16-c_falcon.glb
    skeletal: false
    scale: 1.0
    rotation:
      pitch: 0.0
      yaw: 0.0
      roll: 0.0
  "2001":
    mesh: truck/ural_4320.glb
    skeletal: false
    class_name: truck
    scale: 1.0
    rotation:
      pitch: 0.0
      yaw: 180.0
      roll: 0.0
    half_length_m: 3.78
    half_beam_m: 1.53
  "3001":
    mesh: boat/mako_655.glb
    skeletal: false
    class_name: boat
    scale: 1.19
    rotation:
      pitch: 0.0
      yaw: 180.0
      roll: 0.0
    z_offset_m: -0.49
    half_length_m: 3.27
    half_beam_m: 1.31
```

## Field Reference

### CIGI Input

| Field | Type | Default | Env var | Description |
|-------|------|---------|---------|-------------|
| `cigi_bind_addr` | string | `"0.0.0.0"` | `CAMSIM_CIGI_BIND_ADDR` | Local address to bind the CIGI UDP socket. Use `"0.0.0.0"` to listen on all interfaces. |
| `cigi_port` | int | `8888` | `CAMSIM_CIGI_PORT` | UDP port for incoming CIGI 3.3 packets (Host -> IG). |
| `cigi_response_addr` | string | `"127.0.0.1"` | `CAMSIM_CIGI_RESPONSE_ADDR` | Destination IP address for IG -> Host packets (SOF heartbeat, HAT/HOT responses, LOS responses). Set to the host simulation's IP. |
| `cigi_response_port` | int | `8889` | `CAMSIM_CIGI_RESPONSE_PORT` | Destination UDP port for IG -> Host packets. |
| `camera_entity_id` | int | `0` | -- | CIGI Entity ID that controls the camera. All other entity IDs are managed by the entity renderer. Must match the `--entity-id` value passed to `send_cigi_test.py`. |

### DIS Input (IEEE 1278.1)

Guide: [`dis.md`](dis.md) (test sender, type mapping, surface placement, ground truth).

| Field | Type | Default | Env var | Description |
|-------|------|---------|---------|-------------|
| `dis.enabled` | bool | `false` | `CAMSIM_DIS_ENABLED` | Listen for DIS PDUs (Entity State, Designator). The shipped `deploy/camsim_config.yaml` sets `true`. |
| `dis.bind_addr` | string | `"0.0.0.0"` | `CAMSIM_DIS_BIND_ADDR` | UDP bind address. |
| `dis.port` | int | `3000` | `CAMSIM_DIS_PORT` | UDP port (the IEEE 1278.1 default). |
| `dis.multicast_group` | string | `""` | `CAMSIM_DIS_MULTICAST_GROUP` | Multicast group to join; empty = unicast only. The shipped config uses `"239.1.2.3"`. |
| `dis.exercise_id` | int | `1` | `CAMSIM_DIS_EXERCISE_ID` | Only PDUs with this exercise ID are accepted; `0` = all exercises. |
| `dis.site_id` | int | `1` | `CAMSIM_DIS_SITE_ID` | This IG's DIS site ID. |
| `dis.application_id` | int | `1` | `CAMSIM_DIS_APP_ID` | This IG's DIS application ID. |
| `dis.heartbeat_timeout_sec` | float | `12.0` | `CAMSIM_DIS_HEARTBEAT_TIMEOUT` | An entity with no Entity State PDU for this long is removed (DIS has no explicit remove). |
| `dis.default_entity_type_id` | int | `1001` | `CAMSIM_DIS_DEFAULT_ENTITY_TYPE` | CamSim entity type for DIS types that no `dis.entity_type_map` level matches. |
| `dis.clamp_to_surface` | bool | `true` | `CAMSIM_DIS_CLAMP_TO_SURFACE` | Place land platforms (kind 1, domain 1) on the terrain (height, pitch, roll) and surface platforms (kind 1, domain 3) on the water. Other kinds (munitions, whose domain is the target's; life forms; …) and other domains use the sender's pose. `false` = the sender's altitude and attitude for every entity. |
| `dis.entity_type_map` | map | `{}` | -- | DIS entity type → CamSim entity type ID. Keys are `"kind:domain:country:category:subcategory:specific:extra"` strings (see `deploy/camsim_config.yaml`); values are CamSim entity type IDs from `entity_types`. Lookup order for an incoming DIS entity type is: exact match (all seven fields) → `kind:domain:category` fuzzy match → `kind:domain` fallback → `dis.default_entity_type_id`. The `kind:domain` fallback lets any unmapped land (domain 1) or surface (domain 3) platform pick up a generic truck/boat mapping instead of falling all the way through to the default. When more than one entry maps to the same fallback key at a given level, the entry with a generic (`0`) subcategory/country wins; among equally generic entries the lower CamSim entity type ID wins, so the result is deterministic regardless of map iteration order. |

### Video Output

| Field | Type | Default | Env var | Description |
|-------|------|---------|---------|-------------|
| `multicast_addr` | string | `"239.1.1.1"` | `CAMSIM_MULTICAST_ADDR` | UDP multicast group (or unicast address) for the MPEG-TS stream. |
| `multicast_port` | int | `5004` | `CAMSIM_MULTICAST_PORT` | Destination UDP port. |
| `video_bitrate` | int | `4000000` | `CAMSIM_VIDEO_BITRATE` | Target H.264 bitrate in bits per second. |
| `h264_preset` | string | `"ultrafast"` | `CAMSIM_H264_PRESET` | libx264 encoding preset. Slower presets (`fast`, `medium`) give better quality at higher CPU cost. |
| `h264_tune` | string | `"zerolatency"` | -- | libx264 tune parameter. `"zerolatency"` minimises encode latency. |

### Capture

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `capture_width` | int | `1920` | Render target width in pixels. Env `CAMSIM_CAPTURE_WIDTH` (restart only). |
| `capture_height` | int | `1080` | Render target height in pixels. Env `CAMSIM_CAPTURE_HEIGHT` (restart only). |
| `frame_rate` | float | `30.0` | Render, sensor-integration and output rate (fps, 1–120). One encoded frame per rendered frame; a value other than 30 overrides `DefaultEngine.ini`'s `FixedFrameRate` at startup. |
| `readback_ready_polls` | int | `2` | `CAMSIM_READBACK_READY_POLLS` | Number of consecutive `FRHIGPUTextureReadback::IsReady()` polls required before `Lock()`. Increase on Linux/Vulkan if occasional partial-row tearing appears. |
| `hfov_deg` | float | `60.0` | Horizontal field of view in degrees. Used for KLV metadata and Cesium tile preloading. Overridden per-frame by CIGI View Definition packets. |

### Runtime Hardening

| Field | Type | Default | Env var | Description |
|-------|------|---------|---------|-------------|
| `video_codec` | string | `"h264"` | `CAMSIM_VIDEO_CODEC` | Video codec: `h264` or `h265`/`hevc` (STANAG 4609 Ed4). |
| `encoder` | string | `"auto"` | `CAMSIM_ENCODER` | Encoder implementation: `auto` (tries NVENC, then libx264/libx265; an encoder that fails to open falls through to the next), `nvenc`, `videotoolbox`, `libx264`, or `libx265`. `videotoolbox` is the macOS hardware encoder: much lower CPU cost, but it only approximates the bitrate cap and can run ~3.6× over it on very noisy scenes (IR grain), so `auto` never picks it. |
| `encoder_watchdog_policy` | string | `"reconnect"` | `CAMSIM_ENCODER_WATCHDOG_POLICY` | Encoder watchdog action when no frames are written for `encoder_watchdog_interval_ticks`: `reconnect`, `log_only`, or `fail_fast`. |
| `encoder_watchdog_interval_ticks` | int | `150` | `CAMSIM_ENCODER_WATCHDOG_INTERVAL_TICKS` | Tick interval used by the encoder watchdog and runtime health checks. |
| `watchdog_max_reconnects` | int | `3` | -- | Maximum encoder reconnect attempts before `RequestExit`. `0` = unlimited retries. |
| `max_entities` | int | `500` | `CAMSIM_MAX_ENTITIES` | Maximum simultaneous entities managed by the entity renderer. |

### Geospatial Providers (Phase F1 foundation)

| Field | Type | Default | Env var | Description |
|-------|------|---------|---------|-------------|
| `terrain_provider` | string | `"cesium"` | `CAMSIM_TERRAIN_PROVIDER` | Terrain/georeference provider selector. Currently supported: `cesium` (unsupported values fall back to `cesium` with warning). |
| `imagery_provider` | string | `"cesium"` | `CAMSIM_IMAGERY_PROVIDER` | Imagery provider selector (currently informational; `cesium` supported). |

### Cesium Backend

Controls which Cesium ion server, terrain source, and imagery overlay CamSim uses at runtime. All settings default to the standard Cesium ion public cloud; override for air-gapped or multi-profile deployments.

| Environment Variable | Default | Description |
|---|---|---|
| `CAMSIM_CESIUM_ION_PORTAL_URL` | `https://ion.cesium.com` | Ion portal URL (`UCesiumIonServer::ServerUrl`). Set for self-hosted ion. |
| `CAMSIM_CESIUM_ION_API_URL` | `https://api.cesium.com` | Ion REST API URL (`UCesiumIonServer::ApiUrl`). Set for self-hosted ion. |
| `CAMSIM_CESIUM_ION_TOKEN` | *(empty)* | Ion access token. **Never logged.** Leave empty to use level asset default. |
| `CAMSIM_CESIUM_TERRAIN_SOURCE` | `cesium_ion` | Terrain source: `cesium_ion`, `url`, or `flat`. |
| `CAMSIM_CESIUM_TERRAIN_ION_ASSET_ID` | `1` | Cesium ion asset ID for terrain (Cesium World Terrain = 1). |
| `CAMSIM_CESIUM_TERRAIN_URL` | *(empty)* | Quantized-mesh terrain URL (used when `TERRAIN_SOURCE=url`). |
| `CAMSIM_CESIUM_IMAGERY_SOURCE` | `cesium_ion` | Imagery overlay source: `cesium_ion`, `wms`, or `none`. |
| `CAMSIM_CESIUM_IMAGERY_ION_ASSET_ID` | `2` | Cesium ion asset ID for imagery (Bing Maps Aerial = 2). |
| `CAMSIM_CESIUM_IMAGERY_WMS_URL` | *(empty)* | WMS base URL (used when `IMAGERY_SOURCE=wms`). |
| `CAMSIM_CESIUM_IMAGERY_WMS_LAYERS` | *(empty)* | WMS layer name(s), comma-separated. |
| `CAMSIM_CESIUM_IMAGERY_WMS_TILE_WIDTH` | `256` | WMS tile width in pixels. |
| `CAMSIM_CESIUM_IMAGERY_WMS_TILE_HEIGHT` | `256` | WMS tile height in pixels. |

### Cesium Tile Streaming

| Field | Type | Default | Env var | Description |
|-------|------|---------|---------|-------------|
| `tile_preload_fov_scale` | float | `1.5` | `CAMSIM_TILE_FOV_SCALE` | Multiplier applied to `hfov_deg` when registering with `ACesiumCameraManager`. Values above 1.0 pre-fetch tiles outside the visible frustum to reduce pop-in when the camera pans. |
| `max_simultaneous_tile_loads` | int | `20` | `CAMSIM_MAX_TILE_LOADS` | Maximum concurrent Cesium tile HTTP requests. Higher values speed up initial scene load at the cost of network/CPU. |
| `maximum_screen_space_error` | float | `16.0` | `CAMSIM_MAX_SSE` | Cesium LOD quality: lower = sharper terrain (Cesium default 16). On an M1 Pro, 8 raises GPU time and pop-in, and 4 causes hundreds of hitches during gimbal slews (ROADMAP 3A sweep); adaptive SSE raises it further when over budget. |
| `create_physics_meshes` | bool | `true` | `CAMSIM_CREATE_PHYSICS_MESHES` | Cook collision for Cesium tiles. CIGI HAT/HOT and LOS queries and the KLV frame centre (Tags 21, 23–25, 78) are line traces against it; with it off they never hit the terrain. |
| `culled_screen_space_error` | float | `0` | `CAMSIM_CULLED_SSE` | Detail kept for tiles outside the view (Cesium `CulledScreenSpaceError`; higher = coarser). After a gimbal snap the new view shows these tiles until they refine. `0` = same as `maximum_screen_space_error`, so snaps land on full-detail tiles. Measured over Yosemite (M1 Pro, 90° snaps): 200 (the old value) left 30–80% of the view coarse for ~2 s; 32 about 1–16% for ~1.5 s; 16 none, for ~60% more tiles (~930 MB). Only takes effect with `frustum_culling` off. Scaled by the zoom at run time: below a 60° horizontal FOV the value used is `× tan(30°) / tan(HFOV / 2)`, so off-screen tiles keep 60°-view detail instead of zoomed-in detail all the way round (at 10° that was 226k UObjects and 0.2–0.8 s GC stalls). |
| `frustum_culling` | bool | `false` | `CAMSIM_FRUSTUM_CULLING` | Cesium's tile-selection frustum culling (`EnableFrustumCulling`; UE's render culling is unaffected). Off by default: Cesium applies `culled_screen_space_error` only to tiles culled by a disabled stage, so with this on, tiles outside the view stay coarse. Measured over Yosemite (M1 Pro, 90° snaps): on, snaps settle in 1.8–2.0 s with up to 59% of the view coarse after 1 s; off, 0.6–0.8 s and none. Cost (SF bench, M1 Pro): game thread +1.4–3.2 ms p50 (roughly double), frames over 66 ms 4 → 20 across the four phases, GPU unchanged; still 0 dropped frames. Turn it on for smooth steady flight when the gimbal doesn't snap. |
| `maximum_cached_bytes_mb` | int | `2048` | `CAMSIM_MAX_CACHED_MB` | Cesium tile cache budget in MB. `0` = Cesium default (uncapped). Sized for the off-screen tiles `culled_screen_space_error` keeps loaded. |
| `use_lod_transitions` | bool | `false` | `CAMSIM_USE_LOD_TRANSITIONS` | Cesium's dithered LOD crossfade, which hides tile LOD pops. Off by default: Cesium updates every tile in the render set each frame while it's on, off-screen tiles included, which cost ~6 ms of game thread per frame and doubled hitches in moving-camera phases (SF bench, M1 Pro, culled SSE 16). Needs temporal AA (TSR, the primary view's anti-aliasing) to resolve the dither. |
| `lod_transition_length` | float | `0.5` | `CAMSIM_LOD_TRANSITION_LENGTH` | Crossfade duration in seconds. |

**Choosing the terrain settings for a deployment** (Linux, RTX 5080, Docker, 1080p, one terrain
tileset, P-core pinned; 2026-10-01; `scripts/bench/snap_test.py`, 90° gimbal snaps at 3 km over SF;
sharpness = edge variance of the frame, as % of the defaults' settled view; two snaps each):

| Setting | Game thread p50 (settled) | Frame +0.3 s after a snap | +1 s | +3 s |
| --- | --- | --- | --- | --- |
| Defaults (`frustum_culling: false`, SSE 16) | 2.9 ms | sharp (86%) | 96–99% | 100% |
| `frustum_culling: true` | 1.3 ms | holes (missing shore) and coarse city (30–34%) | 77–82% | ~100% |
| `maximum_screen_space_error: 24` | 1.9 ms | no holes, but soft (56–67%) | 61–75% | **63–79%: permanently softer** |

Keep the defaults for training/ISR imagery: they are the only setting without visible artifacts,
and the budget allows them (1080p game thread p99 ~10 ms; 500 entities ~14.5 ms, under 50%). Turn
`frustum_culling` on only where the gimbal never snaps (steady flight, fixed sensor) and the game
thread is short of budget (many entities, several output streams). Raising
`maximum_screen_space_error` trades sharpness everywhere, always (coarser terrain tiles also carry
coarser imagery); not recommended at 1080p.

### Terrain Readiness Gate

Holds frame output until Cesium has loaded tiles for the view, at startup and after a
teleport, so coarse placeholder terrain is never streamed as real imagery. Once open, the
gate stays open during normal flight. `/ready` reports
`terrain_ready`.

```yaml
terrain_gate:
  enabled: true
  min_load_progress: 99.0
  timeout_sec: 30.0
  teleport_distance_m: 5000.0
```

| Field | Type | Default | Env var | Description |
|-------|------|---------|---------|-------------|
| `terrain_gate.enabled` | bool | `true` | `CAMSIM_TERRAIN_GATE_ENABLED` | Enable the gate. With no Cesium tilesets in the level it never blocks. |
| `terrain_gate.min_load_progress` | float | `99.0` | `CAMSIM_TERRAIN_GATE_MIN_LOAD_PROGRESS` | Lowest tileset load progress (0-100) at which frames start. Cesium reports 99-99.99% for a few frames while occlusion results settle. |
| `terrain_gate.timeout_sec` | float | `30.0` | `CAMSIM_TERRAIN_GATE_TIMEOUT_SEC` | Start streaming anyway after this long (logged as a warning). |
| `terrain_gate.teleport_distance_m` | float | `5000.0` | `CAMSIM_TERRAIN_GATE_TELEPORT_M` | A platform jump larger than this in one tick re-arms the gate. |

### Camera Start Position

Used as the initial camera pose before the first CIGI Entity Control packet arrives.

| Field | Type | Default | Env var | Description |
|-------|------|---------|---------|-------------|
| `start_latitude` | double | `32.9768` | `CAMSIM_START_LAT` | WGS-84 latitude in decimal degrees. |
| `start_longitude` | double | `-114.2665` | `CAMSIM_START_LON` | WGS-84 longitude in decimal degrees. |
| `start_altitude` | double | `1500.0` | `CAMSIM_START_ALT` | Height above WGS-84 ellipsoid in metres. |
| `start_yaw` | float | `200.0` | `CAMSIM_START_YAW` | Initial heading in degrees [0, 360). |
| `start_pitch` | float | `0.0` | `CAMSIM_START_PITCH` | Initial pitch in degrees. Negative = looking down. |
| `start_roll` | float | `0.0` | `CAMSIM_START_ROLL` | Initial roll in degrees. |
| `start_datetime` | string | *(unset)* | `CAMSIM_START_DATETIME` | Sim clock start, ISO 8601 UTC (e.g. `2025-03-01T06:30:00Z`). Unset: the clock starts at the wall-clock time. One clock drives the sun, KLV Tag 2 and ground truth; a CIGI Celestial Sphere Control with Date/Time Valid sets it at runtime. |
| `start_hour` | float | *(unset)* | `CAMSIM_START_HOUR` | Start at this UTC hour (0-24) on today's date. Ignored when `start_datetime` is set. |
| `sim_time_rate` | float | `1.0` | `CAMSIM_SIM_TIME_RATE` | Sim clock rate: 1 = real time, 0 = frozen, >1 faster. A Celestial Sphere Control with Ephemeris Model Enable = 0 freezes it (static time of day). |

### Gimbal and Sensor (Phase 9)

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `gimbal_max_slew_rate` | float | `0.0` | Maximum gimbal slew rate in degrees per second. `0` means unlimited (instant). Applies to both yaw and pitch axes. |
| `gimbal_pitch_min` | float | `-90.0` | Lower pitch limit in degrees (negative = looking down). |
| `gimbal_pitch_max` | float | `30.0` | Upper pitch limit in degrees. |
| `gimbal_yaw_min` | float | `-180.0` | Left yaw limit in degrees relative to platform heading. |
| `gimbal_yaw_max` | float | `180.0` | Right yaw limit in degrees relative to platform heading. |
| `sensor_fov_presets` | float[] | `[60.0, 20.0, 5.0]` | Horizontal FOV values in degrees, ordered wide to narrow. The Sensor Control packet's Gain field (0.0-1.0) selects the preset by index. A preset is applied only when the gain selects a different one, so a host that resends the same gain keeps a View Definition FOV; a View Definition in the same frame as a preset change wins. |

### Sensor Modes (per-waveband)

Per-waveband GPU sensor path parameters. Configured under `sensor_modes.eo`
and `sensor_modes.ir`. (NVG, formerly `sensor_modes.nvg`/SensorId 2, was removed —
ROADMAP 3B.2. The legacy CPU sensor effect fields — noise, vignetting, scan lines,
IR extinction/atmosphere, quantization, defect pixels, MTF blur, AC banding, IR
pointer overlay, thermal drift, rolling shutter, vibration, gain/offset jitter,
sun glint, contrast/brightness bias — and `sensor_quality`/`sensor_quality_profiles`
were also removed in 3B.2; see "Removed in 3B.2" below.)

| Field | Type | EO default | IR default | Description |
|-------|------|------------|------------|-------------|
| `signal_weight_r` | float | `0.2126` | `0.2126` | Detector spectral response: signal = dot(scene RGB, signal_weight_*). Default is BT.709 luminance. No env override. |
| `signal_weight_g` | float | `0.7152` | `0.7152` | See `signal_weight_r`. No env override. |
| `signal_weight_b` | float | `0.0722` | `0.0722` | See `signal_weight_r`. No env override. |
| `agc_enabled` | bool | `false` | `true` | Enable histogram-stretch AGC (percentile band mapped to output range); typically IR only. |
| `agc_low_percentile` | float | `0.01` | `0.01` | Black-point percentile `[0, 1]` when AGC is enabled. |
| `agc_high_percentile` | float | `0.99` | `0.99` | White-point percentile `[0, 1]` when AGC is enabled. |
| `agc_lag_frames` | int | `0` | `2` | Frames for AGC convergence (`0` = instant). IR env override: `CAMSIM_IR_AGC_LAG_FRAMES`. |
| `agc_max_display_gain` | float | `40` | `40` | ROADMAP 4A. Highest display gain of the **thermal** IR AGC (normalised DN); when it binds, the band is centred on mid-grey. The 3B.2 luminance proxy AGC is not capped. Must be >= 1. |

`CAMSIM_IR_PRESET` re-applies the named preset to the IR mode after the yaml is read (dropping yaml `optics:`/`detector:` overrides); an unknown name is a validation error.


**`sensor_modes.<mode>.exposure`** (ROADMAP 3B, GPU sensor path). No env overrides — per-mode only. The sensor AE exposes the scene histogram's median to `target_grey`, but never lets the `highlight_percentile` pixel pass the clip point (full well, i.e. ADC full scale, which displays as white), then clamps the total gain to `[min_gain_ev, max_photon_gain_ev + detector.max_analog_gain_db/20·log2(10)]` (ROADMAP 3B.2). The total is split photon-first: the photon stage (integration time) takes gain up to `max_photon_gain_ev`, and only the remainder is analog gain, applied after the detector noise (so it amplifies noise too). A microbolometer has no analog stage. Defaults were calibrated on the bench shots on 2026-09-27 (M1 Pro, San Francisco; ROADMAP 3B.1): daylight scene medians are 2^10.9–2^12.4 and expose inside the limits (EO mean luma 105–114, < 0.1% clipped); `night_slant` (median 2^6.6) clamps at EO's `max_photon_gain_ev` (mean luma 26 in 3B.1, photon gain only). Re-measured with the full 3B.2 model and unchanged defaults (ROADMAP 3B.2, 720p): daylight EO mean luma 106–118 with ≤ 0.1% of pixels luma-clipped, dawn/dusk 104/91, `night_slant` 34 — analog gain lifts it past the photon clamp, so its temporal noise is 1.83 DN against 1.13 DN in daylight. The same per-mode values are FCamSimConfig's built-in defaults (used when the yaml has no `exposure:` block; `CamSim.Sensor.Config.PerModeExposureDefaults`/`…CanonicalConfig` keep code and yaml in step); a bare `FSensorExposureConfig` holds neutral values (−20/−6, 0.18, 0.99).

| Field | Type | EO default | IR default | Description |
|-------|------|------------|------------|-------------|
| `exposure.auto` | bool | `true` | `true` | Auto-exposure on; `false` uses `manual_gain_ev`. |
| `exposure.min_gain_ev` | float | `-20.0` | `-20.0` | Lowest gain the simulated camera can select, log2 of the multiplier applied to absolute scene-linear values (higher = brighter). Must be `>= -40`. |
| `exposure.max_photon_gain_ev` | float | `-12.5` | `-6.0` | Highest photon-stage gain (longest integration) the simulated camera can select (renamed from `max_gain_ev` in 3B.2). Past it, the AE adds analog gain up to `detector.max_analog_gain_db`. This is the mode's sensitivity calibration: it is not yet derived from `optics.f_number`, `pixel_pitch_um`, QE or integration time (radiometric photon gain is ROADMAP 3B.3). |
| `exposure.target_grey` | float | `0.18` | `0.18` | Linear value the histogram median is exposed to. |
| `exposure.highlight_percentile` | float | `0.99` | `0.99` | This percentile of the histogram is kept below clipping. |
| `exposure.lag_frames` | int | `2` | `2` | Convergence time constant in frames at 30 Hz (sim time); `0` = instant. |
| `exposure.manual_gain_ev` | float | `-12.0` | `-12.0` | Gain used when `exposure.auto` is `false`. |

**`sensor_modes.<mode>.thermal_exposure`** (ROADMAP 4A). The AE for the thermal radiance input (signal = L / B(300 K), about 1 for a 300 K scene): the photon gain exposes a 300 K scene to mid-range. `exposure` keeps serving `thermal.enabled: false` (the luminance proxy). Same fields as `exposure`; no env overrides.

| Field | Type | EO default | IR default | Description |
|-------|------|------------|------------|-------------|
| `thermal_exposure.auto` | bool | `true` | `true` | Auto-exposure on; `false` uses `manual_gain_ev`. |
| `thermal_exposure.min_gain_ev` | float | `-8.0` | `-8.0` | Lowest gain. Must be `>= -40`. |
| `thermal_exposure.max_photon_gain_ev` | float | `0.0` | `0.0` | Highest photon-stage gain; must be `>= min_gain_ev`. |
| `thermal_exposure.target_grey` | float | `0.5` | `0.5` | Value the histogram median is exposed to (300 K mid-range). |
| `thermal_exposure.highlight_percentile` | float | `0.99` | `0.99` | Kept below clipping; in `(0, 1]`. |
| `thermal_exposure.lag_frames` | int | `2` | `2` | Convergence time constant in frames (sim time). |
| `thermal_exposure.manual_gain_ev` | float | `-1.0` | `-1.0` | Gain used when `auto` is `false`. |

With `agc_enabled` the IR `exposure` block still sets the photon gain (integration time, hence the noise level); the IR AGC then maps its `agc_low_percentile`…`agc_high_percentile` band of the normalised signal to the output range.

#### Sensor-class presets, optics and detector (ROADMAP 3B.2 Task 5)

`sensor_modes.<mode>.preset` selects a sensor-class preset supplying default
`optics:`/`detector:` values (a physical model: optics → electrons → detector
noise → ADC → display). `optics:`/`detector:` blocks then override individual
fields on top of the preset. `seed` sets the PCG noise stream seed for that
mode's detector noise and defect patterns. No env overrides — yaml only.

| Field | Type | EO default | IR default | Description |
|-------|------|------|------|-------------|
| `preset` | string | `eo_hd_cmos` | `mwir_cooled` | Sensor-class preset name (see table below). Unknown names are reported by `Validate()` and keep the built-in preset defaults. |
| `seed` | uint32 | `1` | `1` | PCG noise stream seed for this mode's detector noise/defect patterns. The mode is folded in (`FSensorController::ModeSeed`), so EO and IR with the same seed still get independent fixed patterns (PRNU, DSNU, defects); the same seed always gives a mode the same pattern. |

Presets (`Sensor/SensorPresets.h`, `CamSimSensorPresets::Apply`):

| Parameter | `eo_hd_cmos` (1080p industrial CMOS) | `mwir_cooled` (640×512 InSb) | `lwir_uncooled` (640×512 VOx) |
|---|---|---|---|
| `detector.type` | `photon` | `photon` | `microbolometer` |
| `detector.full_well_e` / `read_noise_e` | 10,000 / 2 | 7,000,000 / 400 | — |
| `detector.prnu` / `dsnu_e` / `dark_current_e_s` | 0.01 / 1 / 5 | 0.001 / 2,000 / 0 (residual after NUC; cooled) | — |
| `detector.temporal_noise` / `pixel_fpn` / `column_fpn` / `row_fpn` | — | — | 0.004 / 0.003 / 0.0015 / 0.001 |
| `detector.adc_bits` | 12 | 14 | 14 |
| `detector.max_analog_gain_db` | 30 | 0 (AGC) | 0 (no analog stage) |
| `optics.f_number` / `pixel_pitch_um` / `wavelength_um` | 4 / 2.9 / 0.55 | 4 / 15 / 4.0 | 1.2 / 12 / 10 |
| `detector.hot_pixel_fraction` / `dead_pixel_fraction` | 1e-5 / 1e-5 | 1e-4 / 1e-4 | 1e-4 / 1e-4 |
| `detector.band_lo_um` / `band_hi_um` | 0.4 / 0.7 | 3 / 5 | 8 / 12 |
| `optics.vignetting_exponent` / `extra_blur_px` / `k1` / `k2` | 4 / 0 / 0 / 0 | 4 / 0 / 0 / 0 | 4 / 0 / 0 / 0 |

`optics:`/`detector:` override keys:

| Field | Type | Description |
|-------|------|-------------|
| `optics.f_number` | float | Lens f-number. Must be `> 0`. |
| `optics.pixel_pitch_um` | float | Detector pixel pitch in micrometres. |
| `optics.wavelength_um` | float | Design wavelength in micrometres (diffraction blur). |
| `optics.extra_blur_px` | float | Additional Gaussian blur sigma in pixels, on top of the diffraction/pixel-pitch PSF. |
| `optics.vignetting_exponent` | float | Falloff exponent `n` in `cos^n θ`. Must be in `[0, 8]`. |
| `optics.k1`, `optics.k2` | float | Brown-Conrady radial distortion coefficients (`rd = ru (1 + k1 ru² + k2 ru⁴)`, radii normalised by the focal length in pixels). Each must be in `[-1, 1]`, and the lens must be invertible across the frame at `hfov_deg` (see below). Ground truth (bounding boxes, depth, KLV frame corners) is pinhole and does not include this distortion: with `k1`/`k2` ≠ 0 the labels misalign with the image toward the edges (ROADMAP 3B.3). |
| `detector.type` | string | `photon` or `microbolometer` (case-insensitive); anything else is a validation error. |
| `detector.full_well_e` | float | Full-well capacity in electrons (photon detectors). Must be `> 0`. |
| `detector.read_noise_e` | float | Read noise in electrons RMS (photon detectors). Must be `>= 0`. |
| `detector.prnu` | float | Photo-response non-uniformity, fractional (photon detectors). Must be `>= 0`. |
| `detector.dsnu_e` | float | Dark-signal non-uniformity in electrons (photon detectors). Must be `>= 0`. |
| `detector.dark_current_e_s` | float | Dark current in electrons/second (photon detectors). Must be `>= 0`. |
| `detector.max_analog_gain_db` | float | Maximum analog gain in dB (photon detectors; ignored for a microbolometer). The AE uses it only once the photon gain is at `exposure.max_photon_gain_ev`. Must be `>= 0`. |
| `detector.temporal_noise` | float | Temporal noise, fraction of full scale (microbolometer). Must be `>= 0`. |
| `detector.pixel_fpn`, `detector.column_fpn`, `detector.row_fpn` | float | Fixed-pattern noise components, fraction of full scale (microbolometer). Must be `>= 0`. |
| `detector.adc_bits` | int | ADC resolution in bits. Must be in `[8, 16]`. |
| `detector.hot_pixel_fraction`, `detector.dead_pixel_fraction` | float | Defect pixel fractions. Must be in `[0, 0.01]`. |
| `detector.band_lo_um`, `detector.band_hi_um` | float | Thermal radiance band in µm (ROADMAP 4A); `0 < lo < hi ≤ 30`. |

**Validation errors:** unknown `preset` or `detector.type`; `full_well_e <= 0`; `adc_bits` outside `[8, 16]`; negative
noise parameters or `f_number <= 0`; `pixel_pitch_um` / `wavelength_um <= 0` or `extra_blur_px < 0`;
`vignetting_exponent` outside `[0, 8]`; `exposure.min_gain_ev < -40`;
defect fractions outside `[0, 0.01]`; `band_lo_um`/`band_hi_um` not satisfying `0 < lo < hi <= 30`; `agc_max_display_gain < 1`; `thermal_exposure` like `exposure`; `|k1|` or `|k2| > 1` (a NaN fails every range check); a distortion that "does not converge out
to the frame corner": the sensor inverts `k1`/`k2` with a fixed 3-step Newton recurrence (CPU and
GPU alike), checked at 64 distorted radii from the centre to the corner of the
`capture_width × capture_height` frame at `hfov_deg` (e.g. `k1: -1.0` fails at 60°, `-0.3` passes).
At runtime the focal length follows the live FOV (CIGI zoom, gain presets); a wider FOV where the
lens stops converging logs one warning and renders without distortion (vignetting and PSF kept)
until the FOV narrows again.

The defaults (`eo_hd_cmos`, `mwir_cooled`) are the final 3B.2 values; none changed during the
3B.2 bench calibration. Two looks to know about: `cos⁴` vignetting (`vignetting_exponent: 4`)
darkens the corners of a 60° HFOV frame to ~0.48× the centre (set a lower exponent for flatter
optics), and IR renders thermal radiance (ROADMAP 4A, [`thermal.md`](thermal.md)); with
`thermal.enabled: false` it sees the visible-light scene and night IR is dark.

**Validation warnings** (logged at startup, never fatal): an optical PSF sigma
`σ_o = sqrt((0.42 λ N / pitch)² + extra_blur_px²)` above 2/3 px (PSF radius > 3) exceeds the 1080p
GPU budget tier of the sensor graph (≤ 2 ms p95); every preset is below it (EO 0.32, MWIR 0.45,
LWIR 0.42 px).

### Multi-stream Output Views (Phase D2)

`output_views` optionally fans out the same captured frame to multiple MPEG-TS
outputs. Each view can use an independent route and output HFOV metadata.
When `hfov_deg` is narrower than the live HFOV, CamSim applies a center crop
digital zoom before encoding that view.

When `CAMSIM_MULTICAST_ADDR` and/or `CAMSIM_MULTICAST_PORT` are set in the
environment (for example via `./scripts/run.sh --local`), CamSim applies those
route overrides to all configured `output_views` so local unicast testing
does not silently keep per-view multicast routes from the config.

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `output_views` | array | `[]` | If empty/omitted, CamSim uses the root `multicast_*` and encoder fields as a single output. |
| `output_views[].view_id` | int | array index | Identifier used in logs and ground-truth sidecar. |
| `output_views[].enabled` | bool | `true` | Enables/disables this view at startup. |
| `output_views[].multicast_addr` | string | root `multicast_addr` | Destination multicast/unicast IP for this view. |
| `output_views[].multicast_port` | int | root `multicast_port` | Destination UDP port for this view. |
| `output_views[].video_bitrate` | int | root `video_bitrate` | H.264 bitrate for this view. |
| `output_views[].h264_preset` | string | root `h264_preset` | x264 preset for this view. |
| `output_views[].h264_tune` | string | root `h264_tune` | x264 tune for this view. |
| `output_views[].hfov_deg` | float | `0.0` | `0` = use live HFOV; otherwise narrow HFOV (digital zoom) for this stream. |

### ML Training Data (`ml_training:`)

Per-frame ground truth for ML/ATR training (COCO JSONL, 16-bit depth).

| Field | Type | Default | Env var | Description |
|-------|------|---------|---------|-------------|
| `ml_training.enabled` | bool | `false` | `CAMSIM_ML_ENABLED` | Master toggle. |
| `ml_training.output_dir` | string | `ml_output` | `CAMSIM_ML_OUTPUT_DIR` | Output directory (relative paths resolve from the binary directory). |
| `ml_training.depth_map` | bool | `true` | `CAMSIM_ML_DEPTH_ENABLED` | 16-bit grayscale depth PNG per annotated frame: linear view depth from the primary view's scene depth, aligned with the image and masks (no second render; `docs/ground-truth.md`). |
| `ml_training.bounding_boxes` | bool | `true` | `CAMSIM_ML_BBOX_ENABLED` | Per-frame entity annotations. |
| `ml_training.coco_export` | bool | `true` | `CAMSIM_ML_COCO_ENABLED` | COCO JSONL (one object per frame). |
| `ml_training.min_visible_pixels` | int | `1` | `CAMSIM_ML_MIN_VISIBLE_PIXELS` | Drop annotations with fewer visible (unoccluded, in-frame) pixels than this. Fully hidden vehicles are never labelled. Clamped to >= 1. |
| `ml_training.segmentation` | bool | `true` | `CAMSIM_ML_SEGMENTATION_ENABLED` | COCO RLE `segmentation` (modal mask) per annotation; the bulk of each line. |
| `ml_training.annotation_interval_frames` | int | `1` | `CAMSIM_ML_INTERVAL_FRAMES` | Write every N frames. |
| `ml_training.depth_far_plane_m` | float | `5000` | `CAMSIM_ML_DEPTH_FAR_PLANE_M` | Depth quantization ceiling (0 m -> 0, far -> 65535). |

### Entity Runtime Scale Controls (Phase C3)

`entity_scale` applies runtime throttles for dense scenes:

| Field | Type | Default | Env var | Description |
|-------|------|---------|---------|-------------|
| `entity_scale.max_draw_distance_m` | float | `0.0` | `CAMSIM_ENTITY_MAX_DRAW_DISTANCE_M` | Draw/cull distance in metres for entity meshes/lights. `0` disables culling by distance. |
| `entity_scale.tick_rate_hz` | float | `0.0` | `CAMSIM_ENTITY_TICK_RATE_HZ` | Tick rate applied to `ACamSimEntity`. `0` means every frame. |
| `entity_scale.default_max_update_rate_hz` | float | `0.0` | `CAMSIM_ENTITY_DEFAULT_MAX_UPDATE_RATE_HZ` | Global cap for pose-apply rate (reduces transform churn). `0` means uncapped. |
| `entity_scale.max_update_rate_hz_overrides` | object | `{}` | -- | Per-entity overrides keyed by `EntityId` string. |

Legacy flat keys (`entity_max_draw_distance_m`, `entity_tick_rate_hz`, `entity_default_max_update_rate_hz`) are still accepted.

### Encoder (Phase 12B)

| Field | Type | Default | Env var | Description |
|-------|------|---------|---------|-------------|
| `video_codec` | string | `"h264"` | `CAMSIM_VIDEO_CODEC` | Video codec: `h264` or `h265`/`hevc` (STANAG 4609 Ed4). |
| `encoder` | string | `"auto"` | `CAMSIM_ENCODER` | Encoder implementation: `auto` (tries NVENC, then libx264/libx265), `nvenc`, `videotoolbox` (macOS, opt-in), `libx264`, or `libx265`. |

### Security Metadata (Phase 12A, MISB ST 0102)

Embedded in every KLV packet as ST 0601 Tag 48. Required for STANAG 4609 compliance.

| Field | Type | Default | Env var | Description |
|-------|------|---------|---------|-------------|
| `security_metadata.classification` | string | `"UNCLASSIFIED"` | `CAMSIM_SECURITY_CLASSIFICATION` | Classification level: `UNCLASSIFIED`, `RESTRICTED`, `CONFIDENTIAL`, `SECRET`, `TOP SECRET`. |
| `security_metadata.classifying_country` | string | `"//US"` | `CAMSIM_SECURITY_CLASSIFYING_COUNTRY` | Classifying country code in ISO-3166 format with `//` prefix. |
| `security_metadata.object_country_codes` | string | `"US"` | `CAMSIM_SECURITY_OBJECT_COUNTRY` | Object country codes (ISO-3166). |
| `security_metadata.caveats` | string | `""` | -- | Security caveats (optional). |
| `security_metadata.releasing_instructions` | string | `""` | -- | Releasing instructions (optional). |

### Recording & Playback (Phase 12E)

| Field | Type | Default | Env var | Description |
|-------|------|---------|---------|-------------|
| `recording.cigi_record_path` | string | `""` | `CAMSIM_CIGI_RECORD_PATH` | Binary file: raw CIGI UDP datagrams with timestamps for deterministic replay. Empty = disabled. |
| `recording.video_record_path` | string | `""` | `CAMSIM_VIDEO_RECORD_PATH` | Local MPEG-TS file: H.264/H.265 video + KLV metadata mirror of the UDP stream. Empty = disabled. |
| `recording.cigi_playback_path` | string | `""` | `CAMSIM_CIGI_PLAYBACK_PATH` | When set, CigiReceiver reads from this file instead of UDP socket (playback mode). Empty = live UDP input. |

### Entity Types

The `entity_types` map uses CIGI Entity Type IDs (uint16, as YAML string keys)
to asset paths and flags:

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `mesh` | string | Yes | UE content browser path for the primary mesh asset (`"/Game/Path/To/Asset.Asset"`), or a glTF/GLB file relative to `entities/` (loaded with glTFRuntime at startup, e.g. `truck/ural_4320.glb`). |
| `skeletal` | bool | No (default `false`) | `true` for `USkeletalMesh` (enables articulated part control). `false` for `UStaticMesh`. |
| `mesh_damaged` | string | No | Alternative mesh for damage state 1 (Component Control CompId=10, state=1). Falls back to `mesh` if omitted. |
| `mesh_destroyed` | string | No | Alternative mesh for damage state 2 (Component Control CompId=10, state=2). Falls back to `mesh` if omitted. |
| `class_name` | string | No (default `type_NNNN`) | ML ground-truth label (COCO `category.name`). |
| `scale` | float | No (default `1.0`) | Uniform model scale, e.g. to bring a glTF model to its real size. |
| `rotation` | map | No | `pitch` / `yaw` / `roll` offset in degrees so the model's nose points along UE +X (the entity's heading). |
| `z_offset_m` | float | No (default `0.0`) | Vertical offset of the model from the entity origin, in metres (+ = up). Surface-clamped entities have their origin on the terrain / water, so a boat uses a negative value to sit at its draft (the shipped `3001` uses `-0.49`). |
| `half_length_m` / `half_beam_m` | float | No | Half length / half width of the footprint after `scale`, in metres. Used by the DIS surface clamp's four ground traces (see [`dis.md`](dis.md#surface-placement)) and vessel wave motion; the loaded mesh's bounds are used when absent. |
| `thermal_material` | string | No (default `vehicle_paint`) | ROADMAP 4A. `FThermalMaterialTable` class name for this type's pixels; an unknown name falls back with one warning. |
| `thermal_offset_k` | float | No | ROADMAP 4A. Temperature offset in K. Default +8 for land/sea vehicles (entities placed on the surface), else 0. Outside `[-50, 500]` is ignored with a warning. |

Entity type IDs are defined by the host simulation. CamSim does not reserve any
specific IDs -- the mapping is entirely user-configured.

The shipped config defines three glTF types (models under `entities/`; the
truck and boat directories each carry a `LICENSE.md` with the source and the
CC BY 4.0 attribution):

| ID | Model | `class_name` | Size (L x W, after `scale`) | DIS mapping |
|----|-------|--------------|-----------------------------|-------------|
| `1001` | F-16C (`f16/f16-c_falcon.glb`) | -- | -- | `1:2:225:2:0:0:0` |
| `2001` | Ural-4320 6x6 cargo truck (`truck/ural_4320.glb`) | `truck` | 7.57 x 3.06 m | `1:1:225:7:0:0:0` (and any land platform via the `kind:domain` fallback) |
| `3001` | Mako 655 rigid-hull inflatable (`boat/mako_655.glb`) | `boat` | 6.54 x 2.63 m | `1:3:225:7:0:0:0` (and any surface platform via the `kind:domain` fallback) |

`CamSim.GPU.Entity.ModelFacing` checks that `2001` and `3001`, after their
`rotation` and `scale`, are longest along UE +X and match `half_length_m` /
`half_beam_m` within 10%.

At startup, CamSim now performs preflight validation for each entry:

- Verifies `mesh` exists.
- Validates static vs skeletal compatibility for `/Game/...` assets.
- Validates optional `mesh_damaged` / `mesh_destroyed` paths and ignores invalid variants with warnings.
- Skips invalid entity type entries instead of failing later at spawn time.

**Example:** To add a helicopter (type 3001) and an armoured vehicle (type 4001):

```yaml
entity_types:
  "3001":
    mesh: "/Game/Models/Helo/UH60.UH60"
    skeletal: true
  "4001":
    mesh: "/Game/Models/Vehicles/M1A2.M1A2"
    skeletal: false
```

## Environment Variable Quick Reference

```bash
# CIGI input (Host -> IG)
CAMSIM_CIGI_BIND_ADDR=0.0.0.0
CAMSIM_CIGI_PORT=8888

# CIGI output (IG -> Host: SOF, HAT/HOT, LOS)
CAMSIM_CIGI_RESPONSE_ADDR=127.0.0.1
CAMSIM_CIGI_RESPONSE_PORT=8889

# Video output
CAMSIM_MULTICAST_ADDR=239.1.1.1   # or 127.0.0.1 for unicast loopback test
CAMSIM_MULTICAST_PORT=5004
CAMSIM_VIDEO_BITRATE=4000000
CAMSIM_H264_PRESET=ultrafast
CAMSIM_ENCODER=auto
CAMSIM_SWAP_RB_READBACK=0
CAMSIM_READBACK_READY_POLLS=2
CAMSIM_READBACK_FORMAT=auto
CAMSIM_ENCODER_WATCHDOG_POLICY=reconnect
CAMSIM_ENCODER_WATCHDOG_INTERVAL_TICKS=150
CAMSIM_MAX_ENTITIES=500
CAMSIM_TERRAIN_PROVIDER=cesium
CAMSIM_IMAGERY_PROVIDER=cesium
CAMSIM_ENTITY_MAX_DRAW_DISTANCE_M=0
CAMSIM_ENTITY_TICK_RATE_HZ=0
CAMSIM_ENTITY_DEFAULT_MAX_UPDATE_RATE_HZ=0

# Start position
CAMSIM_START_LAT=32.9768
CAMSIM_START_LON=-114.2665
CAMSIM_START_ALT=1500.0
CAMSIM_START_YAW=200.0
CAMSIM_START_PITCH=0.0
CAMSIM_START_ROLL=0.0
CAMSIM_START_DATETIME=2025-03-01T06:30:00Z   # or CAMSIM_START_HOUR=12.0
CAMSIM_SIM_TIME_RATE=1.0

# Cesium
CAMSIM_TILE_FOV_SCALE=2.0
CAMSIM_MAX_TILE_LOADS=40
CAMSIM_MAX_SSE=2.0
CAMSIM_MAX_CACHED_MB=2048

# Encoder (Phase 12B)
CAMSIM_VIDEO_CODEC=h264

# Security metadata (Phase 12A)
CAMSIM_SECURITY_CLASSIFICATION=UNCLASSIFIED
CAMSIM_SECURITY_CLASSIFYING_COUNTRY=//US
CAMSIM_SECURITY_OBJECT_COUNTRY=US

# Recording & playback (Phase 12E)
CAMSIM_CIGI_RECORD_PATH=
CAMSIM_VIDEO_RECORD_PATH=
CAMSIM_CIGI_PLAYBACK_PATH=

# Ocean (ROADMAP 2.6)
CAMSIM_OCEAN_ENABLED=1
CAMSIM_OCEAN_BEAUFORT=3.0
CAMSIM_OCEAN_WAVE_DIR=270.0
CAMSIM_OCEAN_CHOPPINESS=0.5
CAMSIM_OCEAN_MOTION_ENABLED=1
CAMSIM_OCEAN_MOTION_SCALE=1.0
CAMSIM_OCEAN_MAX_RADIUS_KM=400.0

# Operational hardening (Phase 28)
CAMSIM_STRUCTURED_LOG_PATH=
CAMSIM_STRUCTURED_LOG_MAX_MB=100
CAMSIM_HEALTH_HTTP_ENABLED=1
CAMSIM_HEALTH_HTTP_PORT=8080
CAMSIM_TRACK_PIPELINE_LATENCY=0

# Docker entrypoint only (deploy/entrypoint.sh, docs/docker.md)
CAMSIM_BINARY=                       # game binary (default: Development, then Shipping)
CAMSIM_ALLOW_SOFTWARE_RENDERING=0    # 1 = try Mesa lavapipe without a GPU (crashes on Mesa <= 26.2)
CAMSIM_PIN_PCORES=1                  # hybrid Intel CPUs: pin to the P-cores (also scripts/run.sh on Linux); 0 = off
```

## Phase 28 — Operational Hardening

### Structured JSON Logging (28B)

Writes one JSON line per event to a rolling log file. Used for ELK/Datadog ingestion.

```yaml
operational:
  structured_log_path: "/var/log/camsim.jsonl"   # empty = disabled
  structured_log_max_mb: 100                     # rotate at this size
```

| Key | Env | Default | Description |
|---|---|---|---|
| `operational.structured_log_path` | `CAMSIM_STRUCTURED_LOG_PATH` | `""` | Path to the JSONL log file. Empty string disables structured logging. |
| `operational.structured_log_max_mb` | `CAMSIM_STRUCTURED_LOG_MAX_MB` | `100` | Rotation threshold. On overflow, the file is closed, renamed to `<path>.1`, and reopened. |

### HTTP Health & Metrics Server (28C)

An embedded HTTP server on a dedicated port exposing liveness, readiness, and Prometheus-format metrics. Used by Kubernetes probes, by the `sim-environment` REST orchestrator for Docker Compose health monitoring, and by Grafana for direct metric scraping.

```yaml
operational:
  health_http_enabled: true   # default true — set to false to disable entirely
  health_http_port: 8080
```

| Key | Env | Default | Description |
|---|---|---|---|
| `operational.health_http_enabled` | `CAMSIM_HEALTH_HTTP_ENABLED` | `true` | Master toggle. Default on for `sim-environment` Docker Compose compatibility. Set to `0` (env) or `false` (YAML) to disable. |
| `operational.health_http_port` | `CAMSIM_HEALTH_HTTP_PORT` | `8080` | Listen port. Binds on all interfaces (0.0.0.0) — `FHttpServerModule::GetHttpRouter` does not take a bind address. |
| `operational.snapshot_endpoint_enabled` | `CAMSIM_SNAPSHOT_ENDPOINT_ENABLED` | `false` | ROADMAP 3A/3B. Binds `GET /snapshot`: returns the next grabbed frame as PNG (the sensor image: the GPU sensor graph replaces the tonemapper), 503 if no frame arrives within 5 s. Also binds `GET /snapshot/sensor` (ROADMAP 3B): the same sensor image (kept for the bench). Both are for the bench harness (`scripts/bench/`). Unbound → 404. |
| `operational.frame_stats_path` | `CAMSIM_FRAME_STATS_PATH` | `""` | ROADMAP 3A/3B. Per-frame JSONL render stats (wall-clock frame time, `stat unit` thread/GPU times, frames emitted/dropped, tileset load %, SSE, camera cut, scene renders per frame, and — ROADMAP 3B — `sensor_gpu_ms`/`sensor_gain_ev`/`scene_median_log2` from the GPU sensor graph; without the sensor graph `sensor_gpu_ms` is −1 and the other two are `null`). Empty disables. A relative path is taken from the directory CamSim was launched from. |

**Routes:**

| Route | Semantics | 200 body | 503 body |
|---|---|---|---|
| `GET /live` | Kubernetes liveness convention. Watchdog: returns 200 when the game-thread `Tick()` has fired within the last 5 seconds, 503 otherwise. | `{"status":"ok"}` | `{"status":"stalled","last_tick_ago_s":12.3}` |
| `GET /health` | `sim-environment` REST orchestrator convention. **Alias for `/live`** — same handler, same semantics. Added so the orchestrator's generic `/health` probe naming works without breaking existing K8s manifests. | `{"status":"ok"}` | Same as `/live` |
| `GET /ready` | Readiness: encoder open AND GPU sensor graph available (`sensor_graph`, decided once at startup) AND at least one CIGI packet received AND first frame successfully encoded AND terrain ready. 200 only when every gate passes. | `{"status":"ready","encoder":true,"sensor_graph":true,"cigi":true,"first_frame":true,"terrain_ready":true}` | `{"status":"not_ready","encoder":true,"sensor_graph":false,"cigi":true,"first_frame":false,"terrain_ready":true}` |
| `GET /metrics` | Prometheus exposition format (`text/plain; charset=utf-8`, version 0.0.4). | See metric list below. | N/A — always 200. |
| `GET /snapshot` | ROADMAP 3A. Only bound when `operational.snapshot_endpoint_enabled`. Next grabbed frame as PNG: the sensor image (same as `/snapshot/sensor`). | `image/png` | `{"status":"no_frame"}` if no frame arrives within 5 s |
| `GET /snapshot/sensor` | ROADMAP 3B. Only bound when `operational.snapshot_endpoint_enabled`. The encoded sensor image as PNG (same as `/snapshot`). | `image/png` | Same as `/snapshot` |

**`/metrics` contract** (matches the `sim-environment` orchestrator spec §10.4):

- **Gauges:** `camsim_render_fps`, `camsim_output_fps`, `camsim_entity_count`, `camsim_uptime_seconds`
- **Counters:** `camsim_frame_drops_total`, `camsim_frame_drops_by_reason_total{reason="encoder_busy"|"readback_timeout"|"socket_error"}`, `camsim_sensor_stats_stale_total` (AE held on a stale histogram; not a drop), `camsim_cigi_packets_total`, `camsim_dis_packets_total`, `camsim_frames_encoded_total`
- **Histograms (optional):** `camsim_frame_latency_ms` — P50/P95/P99 from `FPipelineLatencyTracker`. Only emitted when `performance.track_pipeline_latency = true`.

The render/output FPS gauges are 1Hz rolling measurements updated from the subsystem's `Tick()` (not target values from config). FPS is `(frame_count_delta) / (wall_clock_delta)` over approximately one second.

### Render Path (ROADMAP 3A)

```yaml
render:
  camera_cut_distance_m: 500.0
  camera_cut_angle_deg: 30.0
  origin_shift_distance_m: 20000.0
```

| Key | Env | Default | Description |
|---|---|---|---|
| `render.camera_cut_distance_m` | `CAMSIM_RENDER_CAMERA_CUT_DISTANCE_M` | `500.0` | A camera move larger than this in one frame (teleport, origin rebase) resets TSR history. Must be > 0: a non-positive value is a validation error and that check is skipped. |
| `render.camera_cut_angle_deg` | `CAMSIM_RENDER_CAMERA_CUT_ANGLE_DEG` | `30.0` | A view rotation larger than this in one frame resets TSR history. Must be > 0: a non-positive value is a validation error and that check is skipped. |
| `render.origin_shift_distance_m` | `CAMSIM_RENDER_ORIGIN_SHIFT_DISTANCE_M` | `20000.0` | Rebase the Cesium georeference (`CesiumOriginShiftComponent`, `ChangeCesiumGeoreference` mode) when the camera is this far from the origin. Keeps local "up" = +Z and coordinates small. `0` disables. |

**Sensor graph (ROADMAP 3B).** The GPU sensor graph replaces UE's tonemapper in the primary view (the sensor is the game viewport's view — TSR, one scene render per frame; this is the only render path since 3B.2) and is the only sensor path: UE's tonemapper-stage effects (vignette, film grain, colour grading, bloom dirt mask) don't apply. The stream's transfer characteristic is tagged BT.709 (the graph applies the BT.709 OETF). It needs NV12-compatible dimensions (`capture_width` a multiple of 4 — also a config validation error — and an even `capture_height`) and a real RHI with SM5 compute and the sensor shaders. Checked once at startup: without it CamSim logs `sensor graph unavailable: <reason>` as an error, produces no frames, and `/ready` stays false. When it runs, `/metrics` reports `camsim_sensor_path{path="gpu"} 1`.

**Removed in 3B.2** (the legacy CPU sensor path and scene-capture render path; the YAML keys now produce the standard unknown-key warning and the env vars are ignored):

- `render.view_source` / `CAMSIM_RENDER_VIEW_SOURCE` — the sensor is always the primary view; the `scene_capture` path (and its BGRA readback/encode) is gone.
- `swap_rb_readback` / `CAMSIM_SWAP_RB_READBACK`, `readback_format` / `CAMSIM_READBACK_FORMAT` — BGRA readback-format handling; the sensor graph writes NV12 directly.
- `render.sensor_path` / `CAMSIM_RENDER_SENSOR_PATH` — the GPU sensor graph is the only path.
- `render.exposure_compensation_ev` / `CAMSIM_RENDER_EXPOSURE_COMPENSATION_EV` — UE's auto-exposure bias, legacy path only; the sensor AE uses `sensor_modes.*.exposure`.
- `performance.gpu_sensor_effects`, `performance.gpu_sensor_material_path`, `performance.gpu_sensor_mpc_path` / `CAMSIM_PERF_GPU_SENSOR` — the 27A material path.
- `overlay.*` / `CAMSIM_OVERLAY_*` — the HUD burn-in (no burned-in overlays).
- `laser_designator.*` / `CAMSIM_LASER_*` — the drawn laser spot. DIS Designator PDUs are still received and tracked.
- `phase18.precipitation`, `phase18.rain_intensity`, `phase18.snow_intensity` / `CAMSIM_PRECIPITATION`, `CAMSIM_RAIN_INTENSITY`, `CAMSIM_SNOW_INTENSITY` — the CPU precipitation overlay. CIGI weather is unaffected.
- `randomization.randomize_weather`, `randomization.weather_probability` — they only toggled the precipitation overlay.
- `sensor_quality.*` / `CAMSIM_SENSOR_QUALITY_PRESET`, `CAMSIM_SENSOR_QUALITY_NOISE_SCALE`, `CAMSIM_SENSOR_QUALITY_VIGNETTING_SCALE`, `CAMSIM_SENSOR_QUALITY_SCANLINE_SCALE`, `CAMSIM_SENSOR_QUALITY_ATMOSPHERE_SCALE`, `CAMSIM_SENSOR_QUALITY_BLUR_RADIUS`, `CAMSIM_SENSOR_QUALITY_CONTRAST`, `CAMSIM_SENSOR_QUALITY_BRIGHTNESS_BIAS`, `CAMSIM_SENSOR_QUALITY_GAUSSIAN_SIGMA_SCALE` — the global quality-preset system (low/medium/high/ultra/custom) applied on top of the legacy CPU sensor effects, which are also gone.
- `sensor_quality_profiles.*` — user-defined quality profiles for the same removed system.
- `sensor_modes.<mode>.{noise_netd, fixed_pattern_noise, vignetting, scan_lines, scan_line_strength, ir_extinction_coeff, atmospheric_visibility_m, atmosphere_strength, color_temperature_k, contrast, brightness_bias, blur_radius}` — the legacy CPU sensor noise/vignette/scan-line/atmosphere/tone model. The GPU sensor path models noise, detector response and AE directly.
- `sensor_modes.<mode>.{agc_manual_level, agc_manual_gain}` — manual AGC override; the GPU AGC is either on (percentile stretch) or off (uses `exposure`).
- `sensor_modes.<mode>.{quantization_bits, quantization_dither}` / no env override — CPU quantization/dither simulation.
- `sensor_modes.<mode>.{defect_pixel_count, defect_hot_ratio, defect_seed}` — CPU hot/dead pixel defect seeding.
- `sensor_modes.<mode>.{gaussian_sigma, ac_banding_amplitude, ac_banding_frequency}` — CPU MTF blur and AC banding artifacts.
- `sensor_modes.<mode>.{ir_pointer_enabled, ir_pointer_x, ir_pointer_y, ir_pointer_radius, ir_pointer_intensity}` — the IR laser-pointer dot overlay (already unreachable after NVG removal).
- `sensor_modes.ir.{thermal_drift_enabled, thermal_drift_rate, nuc_interval_sec}` / `CAMSIM_IR_THERMAL_DRIFT_ENABLED`, `CAMSIM_IR_THERMAL_DRIFT_RATE`, `CAMSIM_IR_NUC_INTERVAL_SEC` — CPU thermal drift/NUC simulation.
- `sensor_modes.ir.{gain_jitter, offset_jitter}` / `CAMSIM_IR_GAIN_JITTER`, `CAMSIM_IR_OFFSET_JITTER` — CPU per-frame gain/offset noise.
- `sensor_modes.eo.rolling_shutter_strength` / `CAMSIM_EO_ROLLING_SHUTTER_STRENGTH` — CPU rolling-shutter blend.
- `sensor_modes.eo.{sun_glint_intensity, sun_glint_threshold, sun_glint_spread}` / `CAMSIM_EO_SUN_GLINT_INTENSITY`, `CAMSIM_EO_SUN_GLINT_THRESHOLD`, `CAMSIM_EO_SUN_GLINT_SPREAD` — CPU sun-glint highlight boost.
- `sensor_modes.<mode>.vibration_amplitude` / `CAMSIM_IR_VIBRATION_AMPLITUDE`, `CAMSIM_EO_VIBRATION_AMPLITUDE` — CPU subpixel platform-vibration jitter.
- `optical_realism.{lens_distortion, distortion_k1, distortion_k2}` / `CAMSIM_DISTORTION_K1`, `CAMSIM_DISTORTION_K2` — CPU-side Brown-Conrady lens distortion; never applied to the rendered frame.
- `optical_realism.{chromatic_aberration, chromatic_aberration_intensity}` — the GPU scene-fringe post-process. (The rest of `optical_realism` went in the 2026-10 trim, below.)
- `phase18.dynamic_ir_extinction` / `CAMSIM_DYNAMIC_IR_EXTINCTION` — toggled the (now-removed) CPU IR extinction coefficient from `visibility_range_m`; `phase18.visibility_range_m` / `CAMSIM_VISIBILITY_RANGE_M` itself is unaffected.

**Removed in the 2026-10 feature trim** (unused, dead or duplicated features; the YAML keys produce the standard unknown-key warning and the env vars are ignored):

- `streaming.*` / `CAMSIM_COT_*`, `CAMSIM_ATAK_*`, `CAMSIM_ROVER_*` — the Cursor-on-Target position sender, the second ATAK FMV encode and the ROVER PID/Baseline-profile remap. Receivers find the video and KLV through the PMT.
- `scenario.*` / `CAMSIM_SCENARIO_ENABLED`, `CAMSIM_SCENARIO_TIME_SCALE`, `CAMSIM_SCENARIO_START_HOUR`, the flat `scenario_enabled` / `scenario_time_scale` keys, and `randomization.*` / `CAMSIM_RAND_ENABLED`, `CAMSIM_RAND_SEED` — the built-in scenario engine (scripted entities, waypoints, triggers, pattern-of-life, formation flying) and its randomizer, and `scripts/batch_run.py`. Drive entities from a CIGI or DIS host (`scripts/send_dis_test.py`); set the clock with `start_datetime` / `start_hour` / `sim_time_rate`.
- `optical_realism.*` / `CAMSIM_OPTICAL_REALISM_ENABLED`, `CAMSIM_MOTION_BLUR_AMOUNT`, `CAMSIM_FOCAL_DISTANCE`, `CAMSIM_APERTURE_FSTOP` — UE motion blur, bloom, depth of field and lens flare on top of the physical sensor model. Motion blur is now always off.
- `phase18.{god_rays, god_ray_intensity, atmospheric_scattering, rayleigh_scattering, mie_scattering}` / `CAMSIM_GOD_RAYS` — light shafts and sky-scattering multipliers.
- `phase18.{weather_zones, zone_positions}` / `CAMSIM_WEATHER_ZONES` — regional weather blending. CIGI Weather Control with Region ID > 0 is now ignored; global weather (Region ID 0) is unchanged.
- `phase18.{niagara_rotor_wash, niagara_smoke, niagara_fire, niagara_contrail, contrail_alt_m, contrail_speed_ms, smoke_component_id, fire_component_id, crater_decal_material, crater_impact_component_id, max_craters, crater_default_radius_m}` and their `CAMSIM_*` env vars — particle effects and crater decals whose assets were never in the repository.
- `damage_transition.*` / `CAMSIM_DAMAGE_TRANSITION_FX`, `CAMSIM_DAMAGE_GRADUAL`, `CAMSIM_DAMAGE_INTERPOLATION_SEC` — damage smoke/fire and the delayed ("gradual") mesh swap. CIGI Component Control 10 still swaps to the damaged/destroyed mesh, immediately.
- `ground_truth.*` (and the flat `ground_truth_*` keys) / `CAMSIM_GROUND_TRUTH_*` — the per-frame telemetry JSONL sidecar. The KLV stream carries the same telemetry; `ml_training` is the ground truth.
- `ml_training.voc_export` / `CAMSIM_ML_VOC_ENABLED` — Pascal VOC XML. COCO carries everything VOC did, plus masks and oriented boxes.
- `prometheus_metrics_path` / `CAMSIM_PROMETHEUS_METRICS_PATH` and the `camsim_health.json` file — both duplicated `/metrics` and `/ready`.
- `performance.{track_frame_drops_by_category}` / `CAMSIM_PERF_TRACK_DROPS` — drops are always counted, by reason, on `/metrics`.
- `performance.{hot_reload_config, hot_reload_poll_interval_sec}` / `CAMSIM_PERF_HOT_RELOAD`, `CAMSIM_PERF_POLL_INTERVAL` — config hot reload. Restart to apply config changes.
- `performance.{tile_prefetch_slew_threshold_deg_per_sec, tile_prefetch_fov_boost, tile_prefetch_boost_frames, adaptive_sse, adaptive_sse_min, adaptive_sse_max}` / `CAMSIM_PERF_TILE_*`, `CAMSIM_PERF_ADAPTIVE_SSE*` — runtime Cesium SSE changes. Tune with `maximum_screen_space_error` / `culled_screen_space_error` and the bench.
- `performance.{render_frame_rate_hz, output_frame_rate_hz}` / `CAMSIM_PERF_RENDER_FPS`, `CAMSIM_PERF_OUTPUT_FPS` — use `frame_rate` (render = sensor = output rate).
- `rendering_quality.rt_reflections` / `CAMSIM_RT_REFLECTIONS` — never read.
- `use_instanced_rendering` — never read.

**Render resolution (TSR).** `rendering_quality.tsr_screen_percentage`
(`CAMSIM_TSR_SCREEN_PERCENTAGE`, default `100`) renders below the output size and lets TSR
upscale. Measured on an Apple M1 Pro (ROADMAP 3A bench, San Francisco): at 1080p30, 100% holds
30 fps with 17–19 ms GPU per frame; 75% cuts that to 12–15 ms with no visible difference in the
reference shots. Use 75 when GPU headroom matters (e.g. for GPU sensor effects).

### Per-Frame Latency Tracking (28G)

Captures pipeline-stage timestamps for P50/P95/P99 latency analysis across the 4-thread pipeline.

```yaml
performance:
  track_pipeline_latency: false   # enable to populate camsim_frame_latency_ms on /metrics
```

| Key | Env | Default | Description |
|---|---|---|---|
| `performance.track_pipeline_latency` | `CAMSIM_TRACK_PIPELINE_LATENCY` | `false` | Enable ring-buffer tracking of per-frame latency. Adds a small overhead per frame; recommended for dev/staging, off for production unless needed. |

## Ocean (`ocean:`)

The sea for boats (ROADMAP 2.6): sea level is the EGM96 geoid undulation
(`CamSim::Geospatial::GetGeoidUndulation`) plus a CIGI tide offset, and waves
come from `beaufort`/`wave_direction_deg` below until a CIGI Wave Control
packet enables a host wave (IDs 0-3), which then wins. Owned by
`UCamSimSubsystem` as an `FOceanSurface`; nullptr when `enabled` is off or the
EGM96 grid (`Content/NonUFS/Geoid/WW15MGH.DAC`, git LFS) is missing.

The waves (and so boat heave/pitch/roll and HAT/HOT over water) run on the sim
clock. A frozen clock — CIGI Celestial Sphere Control with Ephemeris Model
Enable off, or `sim_time_rate: 0` — freezes the sea, while dead-reckoned boats
keep moving and the material's small ripple normal keeps animating on engine
time (ROADMAP 2.6 findings, 2.1 open items).

```yaml
ocean:
  enabled: true
  beaufort: 3.0
  wave_direction_deg: 270.0
  choppiness: 0.5
  vessel_motion: true
  vessel_motion_scale: 1.0
  max_radius_km: 400.0
  water_temperature_c: 15.0
  material: "/Game/Ocean/M_Ocean"
```

| Key | Env | Default | Description |
|---|---|---|---|
| `ocean.enabled` | `CAMSIM_OCEAN_ENABLED` | `true` | Master switch. **Startup only** — the `FOceanSurface` is created once in `Initialize`. On by default: the ROADMAP 2.6 acceptance (`scripts/ocean_check.py`) measured ~3–4 ms of GPU time for the drawn sea with the 30 fps budget held. Off, boat placement and HAT/HOT are exactly the pre-2.6 behaviour (the Cesium surface, i.e. the seabed over bathymetry). |
| `ocean.beaufort` | `CAMSIM_OCEAN_BEAUFORT` | `3.0` | Sea state, 0–12, fractional (outside that, or NaN, fails validation). Used while no CIGI Wave Control wave is enabled. |
| `ocean.wave_direction_deg` | `CAMSIM_OCEAN_WAVE_DIR` | `270.0` | Direction the waves come FROM, true north. |
| `ocean.choppiness` | `CAMSIM_OCEAN_CHOPPINESS` | `0.5` | 0 = sine waves, 1 = steepest waveform without looping; must be in [0, 1]. |
| `ocean.vessel_motion` | `CAMSIM_OCEAN_MOTION_ENABLED` | `true` | Boats pitch/roll/heave with the waves. |
| `ocean.vessel_motion_scale` | `CAMSIM_OCEAN_MOTION_SCALE` | `1.0` | Amplitude multiplier on vessel motion. |
| `ocean.max_radius_km` | `CAMSIM_OCEAN_MAX_RADIUS_KM` | `400.0` | Horizon cap for the ocean mesh; must be finite and > 0. |
| `ocean.water_temperature_c` | `CAMSIM_OCEAN_WATER_TEMPERATURE_C` | `15.0` | Initial water temperature for the thermal water class (ROADMAP 4A); CIGI Maritime Surface Conditions overrides it. Startup only. `[-2, 40]`. |
| `ocean.material` | *(none — set via YAML only)* | `/Game/Ocean/M_Ocean` | Ocean material asset path. **Startup only.** |

Removed: the old `phase19:` block (`ocean_enabled`, `beaufort_state`,
`wave_amplitude_scale`, `wave_frequency_scale`, `wave_choppiness`,
`ocean_material_path`, `vessel_wakes_enabled`, `niagara_vessel_wake`,
`wake_fade_time`, `vessel_motion_enabled`, `vessel_motion_scale`,
`ocean_reflections_enabled`, `ssr_intensity`, `reflection_capture_radius`) and
its flat-plane `FGerstnerOceanSurface` renderer are gone, replaced by the
`ocean:` block above and the analytic `FOceanSurface`/`FOceanWaves`. The
Niagara vessel-wake-trail and SSR-reflection sub-features (19B, 19D) have no
successor yet.

## Thermal (`thermal:`)

IR radiance (ROADMAP 4A, guide: [`docs/thermal.md`](thermal.md)). In IR mode each pixel's
in-band radiance comes from a surface temperature (a closed-form diurnal model per material
class, plus a per-pixel solar term from the EO render) and emissivity, with sky and path terms,
and goes through the unchanged sensor model. The band comes from the IR preset
(`detector.band_lo_um`/`band_hi_um`).

```yaml
thermal:
  enabled: true
  air_temperature_c: 15.0
  air_diurnal_swing_k: 8.0
  extinction_per_km: {mwir: 0.15, lwir: 0.10}
  fog_ir_factor: 0.4
  materials:            # optional
    asphalt: {albedo: 0.1, k_fast: 0.02}
  land_cover:           # ROADMAP 4B
    enabled: true
    window_texels: 2048
    classes: {10: tree_canopy}
```

The `thermal.land_cover.*` keys below are described, with the data pipeline, in [`thermal.md`](thermal.md#land-cover-roadmap-4b).

| Key | Env | Default | Description |
|---|---|---|---|
| `thermal.enabled` | `CAMSIM_THERMAL_ENABLED` | `true` | **Startup** decides whether the thermal pass is available (and entities get custom-depth stencils for it). `false` runs IR as the 3B.2 luminance proxy (restart to A/B compare). |
| `thermal.air_temperature_c` | `CAMSIM_THERMAL_AIR_TEMPERATURE_C` | `15.0` | Daily mean air temperature until a CIGI Atmosphere Control packet sets one; `[-80, 60]`. |
| `thermal.air_diurnal_swing_k` | `CAMSIM_THERMAL_AIR_DIURNAL_SWING_K` | `8.0` | Peak-to-peak diurnal air swing, peak at 15:00 local solar; `[0, 30]`. |
| `thermal.extinction_per_km.mwir` / `.lwir` | `CAMSIM_THERMAL_EXTINCTION_MWIR` / `_LWIR` | `0.15` / `0.10` | Band extinction β per km; the band is MWIR when the IR preset's band centre is below 6.5 µm. `[0, 10]`. |
| `thermal.fog_ir_factor` | `CAMSIM_THERMAL_FOG_IR_FACTOR` | `0.4` | While CIGI Atmosphere Control has fog enabled, β += 3.912 / V_km × factor; `[0, 2]`. |
| `thermal.materials.<name>` | *(yaml only)* | — | Overrides a built-in class (`terrain_default`, `water`, `vehicle_paint`, `asphalt`, `vegetation`, `concrete`, `tree_canopy`, `shrubland`, `grassland`, `cropland`, `built_up`, `bare_soil`, `snow_ice`, `wetland`) or adds one (unset fields copy `terrain_default`; ≤ 32 classes, so 18 slots are left for user materials after the 14 built-ins). Fields: `albedo` `[0,1)`, `emissivity` `(0,1]`, `thermal_inertia` J m⁻² K⁻¹ s⁻½ `[0,20000]`, `convection_w_m2k` `(0,200]`, `k_fast` K/(W m⁻²) `[0,0.2]`, `temperature` `model`\|`water`\|`snow`. Names: lower-case letters, digits, `_`. |
| `thermal.land_cover.enabled` | `CAMSIM_THERMAL_LAND_COVER_ENABLED` | `true` | ROADMAP 4B. Terrain pixels take their thermal class from land cover (ESA WorldCover). `false`: 4A output bit for bit with 4A's default classes (every terrain pixel `terrain_default`; the 4A `vegetation` class was retuned, so an entity type set to `vegetation` differs from 4A). |
| `thermal.land_cover.dir` | `CAMSIM_THERMAL_LAND_COVER_DIR` | `Content/NonUFS/LandCover` | Directory with `index.json` and the tiles (`scripts/landcover/fetch_worldcover.py`); relative to the project directory (staged as loose files). Missing: one warning, land cover off. Must be non-empty when enabled. |
| `thermal.land_cover.window_texels` | `CAMSIM_THERMAL_LAND_COVER_WINDOW_TEXELS` | `2048` | Side of the camera-centred window in 10 m texels (2048 = 20.48 km; 4 MB). Terrain outside it uses `terrain_default`. Even, `[256, 8192]`. 2048 is the recommended size; larger windows cost build time (task thread) and memory. |
| `thermal.land_cover.recentre_fraction` | `CAMSIM_THERMAL_LAND_COVER_RECENTRE_FRACTION` | `0.25` | A new window is built (task thread) when the camera is this fraction of the window size from its centre, East or North. `[0.01, 0.45]`. |
| `thermal.land_cover.veg_index_lo` / `veg_index_hi` | `CAMSIM_THERMAL_LAND_COVER_VEG_INDEX_LO` / `_HI` | `0.05` / `0.20` | Vegetation weight v = saturate((ExG − lo) / (hi − lo)), ExG = (2G − R − B) / (R + G + B) of the linear GBuffer base colour. `-1 <= lo < hi <= 2`. |
| `thermal.land_cover.asphalt_max_luma` | `CAMSIM_THERMAL_LAND_COVER_ASPHALT_MAX_LUMA` | `0.12` | Built-up ground below this linear base luminance is asphalt, above it concrete (0.04-wide soft ramp). `[0, 1]`. |
| `thermal.land_cover.warp_amplitude_m` | `CAMSIM_THERMAL_LAND_COVER_WARP_AMPLITUDE_M` | `6` | Breaks the visible 10 m texel grid: the class lookup position is moved by up to this many metres East and North by two smooth noise fields fixed to the ground (they do not move when the camera pans or the window re-centres). `0` turns the warp off (class blending stays smooth). Keep it below `warp_cell_m / 3` (the default 6 m / 20 m does) or the warped lookup folds back on itself. `[0, 20]`. |
| `thermal.land_cover.warp_cell_m` | `CAMSIM_THERMAL_LAND_COVER_WARP_CELL_M` | `20` | Cell size of the warp noise on the ground, in metres: larger cells give broader, gentler wiggles of the class boundaries. `[5, 200]`. |
| `thermal.land_cover.veg_blur_m` | `CAMSIM_THERMAL_LAND_COVER_VEG_BLUR_M` | `2` | The vegetation weight is computed from the base colour averaged over 5 samples: the pixel and 4 diagonal neighbours offset by R pixels along each image axis, R = `veg_blur_m` / (slant range × pixel angle), rounded and clamped to 1-32 px. Each neighbour is √2·R pixels away (about 2.8 m on the ground at the 2 m default when facing the ground; longer along the look direction at grazing angles, where the ground is foreshortened). This hides the 16-pixel colour blocks of JPEG-compressed imagery, which the green index would otherwise turn into square patches in IR. The asphalt/concrete split still uses the pixel itself. Larger values smooth more, but the 5 sparse samples then show as faint shifted copies of roads and edges (visible from about 4 m at 0.45 m pixels). `0` = single sample. `[0, 32]`. |
| `thermal.land_cover.classes.<code>` | *(yaml only)* | see `docs/thermal.md` | WorldCover code (0–255) → thermal material name; an unknown material keeps the default with one warning. A code mapped to a material other than its default renders it unrefined (no base-colour refinement: e.g. `50: concrete` is concrete everywhere, not the asphalt/concrete split). |

Built-in classes:

| Class | albedo | ε | inertia | h_c | k_fast | temperature |
|---|---|---|---|---|---|---|
| `terrain_default` | 0.20 | 0.95 | 1200 | 10 | 0.015 | model |
| `water` | 0.06 | 0.98 | — | — | 0 | water ± 0.5 K |
| `vehicle_paint` | 0.30 | 0.90 | 600 | 12 | 0.04 | model |
| `asphalt` | 0.10 | 0.95 | 1500 | 10 | 0.02 | model |
| `vegetation` | 0.40\* | 0.98 | 500 | 18 | 0.008 | model |
| `concrete` | 0.35 | 0.92 | 1800 | 10 | 0.015 | model |
| `tree_canopy` | 0.40\* | 0.98 | 800 | 25 | 0.004 | model |
| `shrubland` | 0.40\* | 0.97 | 600 | 18 | 0.008 | model |
| `grassland` | 0.50\* | 0.97 | 300 | 15 | 0.010 | model |
| `cropland` | 0.40\* | 0.97 | 700 | 15 | 0.010 | model |
| `built_up` | 0.20 | 0.93 | 1650 | 10 | 0.018 | model |
| `bare_soil` | 0.25 | 0.93 | 1300 | 8 | 0.025 | model |
| `snow_ice` | 0.75 | 0.99 | 600 | 10 | 0 | snow (model, capped at 273.15 K; `k_fast` 0 keeps sunlit snow at or below 0 °C) |
| `wetland` | 0.40\* | 0.98 | 2500 | 15 | 0.004 | model |

\* Effective albedo: folds evapotranspiration into the absorbed solar (the model has no latent heat term).
