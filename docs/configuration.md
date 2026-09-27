# Configuration Reference

CamSim reads `camsim_config.yaml` from the project directory (or the binary
directory as a fallback). Environment variables override any YAML value. Boolean
variables accept `1`/`0`, `true`/`false`, `yes`/`no` and `on`/`off`.

**YAML format:** The config file uses YAML with native `#` comments for clean,
readable configuration. Keys are unquoted; string values only need quotes when
they contain special YAML characters.

**Canonical file:** `deploy/camsim_config.yaml` is the single source of truth
for all three deployment modes. `run.sh` copies it into the UE project
directory before launch; the container `entrypoint.sh` copies it into the
binary directory. Edit only `deploy/camsim_config.yaml`; the project-dir copy
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
swap_rb_readback: false
readback_ready_polls: 2
readback_format: auto
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
use_instanced_rendering: true
gpu_sensor_effects: false

sensor_quality:
  preset: medium
  noise_scale: 1.0
  vignetting_scale: 1.0
  scan_line_scale: 1.0
  atmosphere_scale: 1.0
  blur_radius: 0
  contrast: 1.0
  brightness_bias: 0.0

output_views:
  - view_id: 0
    enabled: true
    multicast_addr: "239.1.1.1"
    multicast_port: 5004
    video_bitrate: 4000000
    h264_preset: ultrafast
    h264_tune: zerolatency
    hfov_deg: 0.0

ground_truth:
  enabled: false
  output_path: camsim_groundtruth.jsonl
  interval_frames: 1

entity_scale:
  max_draw_distance_m: 0.0
  tick_rate_hz: 0.0
  default_max_update_rate_hz: 0.0
  max_update_rate_hz_overrides:
    "1": 30.0

scenario:
  enabled: false
  time_scale: 1.0
  entities:
    - entity_id: 2001
      entity_type: 1001
      start_latitude: 38.8977
      start_longitude: -77.0365
      start_altitude: 900.0
      start_yaw: 90.0
      start_pitch: 0.0
      start_roll: 0.0
      spawn_time_sec: 0.0
      despawn_time_sec: 0.0
      update_rate_hz: 10.0
      north_rate_mps: 50.0
      east_rate_mps: 0.0
      up_rate_mps: 0.0
      yaw_rate_dps: 0.0
      pitch_rate_dps: 0.0
      roll_rate_dps: 0.0

sensor_modes:
  eo:
    noise_netd: 0.0
    fixed_pattern_noise: 0.0
    vignetting: 0.10
    scan_lines: false
    scan_line_strength: 0.0
    ir_extinction_coeff: 0.0
    atmospheric_visibility_m: 0.0
    atmosphere_strength: 1.0
    color_temperature_k: 6500.0
    contrast: 1.0
    brightness_bias: 0.0
    blur_radius: 0
  ir:
    noise_netd: 0.01
    fixed_pattern_noise: 0.005
    vignetting: 0.20
    scan_lines: false
    scan_line_strength: 0.0
    ir_extinction_coeff: 0.00001
    atmospheric_visibility_m: 12000.0
    atmosphere_strength: 0.75
    color_temperature_k: 0.0
    contrast: 1.1
    brightness_bias: -0.03
    blur_radius: 0
  nvg:
    noise_netd: 0.03
    fixed_pattern_noise: 0.0
    vignetting: 0.35
    scan_lines: false
    scan_line_strength: 0.05
    ir_extinction_coeff: 0.0
    atmospheric_visibility_m: 8000.0
    atmosphere_strength: 0.9
    color_temperature_k: 5200.0
    contrast: 1.2
    brightness_bias: 0.02
    blur_radius: 0

security_metadata:
  classification: "UNCLASSIFIED"
  classifying_country: "//US"
  object_country_codes: "US"
  caveats: ""
  releasing_instructions: ""

prometheus_metrics_path: ""

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
| `capture_width` | int | `1920` | Render target width in pixels. |
| `capture_height` | int | `1080` | Render target height in pixels. |
| `frame_rate` | float | `30.0` | Fixed tick rate (must match `DefaultEngine.ini` `FixedFrameRate`). |
| `swap_rb_readback` | bool | `false` | `CAMSIM_SWAP_RB_READBACK` | Force a red/blue swap on GPU readback if the platform reports BGRA but delivers RGBA. |
| `readback_ready_polls` | int | `2` | `CAMSIM_READBACK_READY_POLLS` | Number of consecutive `FRHIGPUTextureReadback::IsReady()` polls required before `Lock()`. Increase on Linux/Vulkan if occasional partial-row tearing appears. |
| `readback_format` | string | `"auto"` | `CAMSIM_READBACK_FORMAT` | Override readback byte order: `bgra`, `rgba`, `argb`, `abgr`, or `auto` (use render target format). |
| `hfov_deg` | float | `60.0` | Horizontal field of view in degrees. Used for KLV metadata and Cesium tile preloading. Overridden per-frame by CIGI View Definition packets. |

### Runtime Hardening

| Field | Type | Default | Env var | Description |
|-------|------|---------|---------|-------------|
| `video_codec` | string | `"h264"` | `CAMSIM_VIDEO_CODEC` | Video codec: `h264` or `h265`/`hevc` (STANAG 4609 Ed4). |
| `encoder` | string | `"auto"` | `CAMSIM_ENCODER` | Encoder implementation: `auto` (tries NVENC, then libx264/libx265; an encoder that fails to open falls through to the next), `nvenc`, `videotoolbox`, `libx264`, or `libx265`. `videotoolbox` is the macOS hardware encoder: much lower CPU cost, but it only approximates the bitrate cap and can run ~3.6× over it on very noisy scenes (IR/NVG grain), so `auto` never picks it. |
| `encoder_watchdog_policy` | string | `"reconnect"` | `CAMSIM_ENCODER_WATCHDOG_POLICY` | Encoder watchdog action when no frames are written for `encoder_watchdog_interval_ticks`: `reconnect`, `log_only`, or `fail_fast`. |
| `encoder_watchdog_interval_ticks` | int | `150` | `CAMSIM_ENCODER_WATCHDOG_INTERVAL_TICKS` | Tick interval used by the encoder watchdog and runtime health checks. |
| `watchdog_max_reconnects` | int | `3` | -- | Maximum encoder reconnect attempts before `RequestExit`. `0` = unlimited retries. |
| `max_entities` | int | `500` | `CAMSIM_MAX_ENTITIES` | Maximum simultaneous entities managed by the entity renderer. |
| `use_instanced_rendering` | bool | `true` | -- | Use instanced rendering for entities with the same mesh type. |
| `gpu_sensor_effects` | bool | `false` | -- | Use GPU post-process materials for sensor effects instead of CPU pipeline. Set `false` for Mesa llvmpipe compatibility. |

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
| `culled_screen_space_error` | float | `0` | `CAMSIM_CULLED_SSE` | Detail kept for tiles outside the view (Cesium `CulledScreenSpaceError`; higher = coarser). After a gimbal snap the new view shows these tiles until they refine. `0` = same as `maximum_screen_space_error`, so snaps land on full-detail tiles. Measured over Yosemite (M1 Pro, 90° snaps): 200 (the old value) left 30–80% of the view coarse for ~2 s; 32 about 1–16% for ~1.5 s; 16 none, for ~60% more tiles (~930 MB) and no frame-time cost. |
| `maximum_cached_bytes_mb` | int | `2048` | `CAMSIM_MAX_CACHED_MB` | Cesium tile cache budget in MB. `0` = Cesium default (uncapped). Sized for the off-screen tiles `culled_screen_space_error` keeps loaded. |
| `use_lod_transitions` | bool | `true` | `CAMSIM_USE_LOD_TRANSITIONS` | Cesium's dithered LOD crossfade, which hides tile LOD pops. Needs temporal AA to resolve the dither, so it is applied only with `render.view_source: primary` (TSR); with `scene_capture` (FXAA) it would blur moving views, so it stays off there. |
| `lod_transition_length` | float | `0.5` | `CAMSIM_LOD_TRANSITION_LENGTH` | Crossfade duration in seconds. |

### Terrain Readiness Gate

Holds frame output until Cesium has loaded tiles for the view, at startup and after a
teleport, so coarse placeholder terrain is never streamed as real imagery. Once open, the
gate stays open during normal flight. `/ready` and `camsim_health.json` report
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
| `start_datetime` | string | *(unset)* | `CAMSIM_START_DATETIME` | Sim clock start, ISO 8601 UTC (e.g. `2025-03-01T06:30:00Z`). Unset: the clock starts at the wall-clock time. One clock drives the sun, KLV Tag 2, CoT, the HUD time and ground truth; a CIGI Celestial Sphere Control with Date/Time Valid sets it at runtime. |
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

### Sensor Quality (Phase D1)

`sensor_quality` is a global profile applied on top of per-waveband `sensor_modes`
parameters. Use it to quickly shift output quality/fidelity without editing each
mode.

| Field | Type | Default | Env var | Description |
|-------|------|---------|---------|-------------|
| `sensor_quality.preset` | string | `"medium"` | `CAMSIM_SENSOR_QUALITY_PRESET` | One of `low`, `medium`, `high`, `ultra`, `custom`. |
| `sensor_quality.noise_scale` | float | `1.0` | `CAMSIM_SENSOR_QUALITY_NOISE_SCALE` | Multiplier for NETD + fixed-pattern noise amplitudes. |
| `sensor_quality.vignetting_scale` | float | `1.0` | `CAMSIM_SENSOR_QUALITY_VIGNETTING_SCALE` | Multiplier for vignette strength. |
| `sensor_quality.scan_line_scale` | float | `1.0` | `CAMSIM_SENSOR_QUALITY_SCANLINE_SCALE` | Multiplier for scan-line effect strength. |
| `sensor_quality.atmosphere_scale` | float | `1.0` | `CAMSIM_SENSOR_QUALITY_ATMOSPHERE_SCALE` | Multiplier for atmospheric attenuation/extinction terms. |
| `sensor_quality.blur_radius` | int | `0` | `CAMSIM_SENSOR_QUALITY_BLUR_RADIUS` | Additional post-effect box-blur radius in pixels. |
| `sensor_quality.contrast` | float | `1.0` | `CAMSIM_SENSOR_QUALITY_CONTRAST` | Global contrast multiplier. |
| `sensor_quality.brightness_bias` | float | `0.0` | `CAMSIM_SENSOR_QUALITY_BRIGHTNESS_BIAS` | Global brightness offset in normalized range `[-1, 1]`. |

### Sensor Modes (per-waveband)

Per-waveband CPU-side post-processing parameters. Configured under `sensor_modes.eo`,
`sensor_modes.ir`, and `sensor_modes.nvg`.

| Field | Type | EO default | IR default | NVG default | Description |
|-------|------|------------|------------|-------------|-------------|
| `noise_netd` | float | `0.0` | `0.01` | `0.03` | NETD noise amplitude. |
| `fixed_pattern_noise` | float | `0.0` | `0.005` | `0.0` | Fixed pattern noise amplitude. |
| `vignetting` | float | `0.10` | `0.20` | `0.35` | Vignette edge darkening strength. |
| `scan_lines` | bool | `false` | `false` | `false` | Enable scan line overlay. |
| `scan_line_strength` | float | `0.0` | `0.0` | `0.05` | Scan line intensity. |
| `ir_extinction_coeff` | float | `0.0` | `0.00001` | `0.0` | IR atmospheric extinction coefficient. |
| `atmospheric_visibility_m` | float | `0.0` | `12000` | `8000` | Visibility range in metres. |
| `atmosphere_strength` | float | `1.0` | `0.75` | `0.9` | Atmosphere effect multiplier. |
| `color_temperature_k` | float | `6500` | `0.0` | `5200` | White balance in Kelvin. |
| `contrast` | float | `1.0` | `1.1` | `1.2` | Contrast multiplier. |
| `brightness_bias` | float | `0.0` | `-0.03` | `0.02` | Brightness offset `[-1, 1]`. |
| `blur_radius` | int | `0` | `0` | `0` | Post-effect blur in pixels. |
| `signal_weight_r` | float | `0.2126` | `0.2126` | `0.6` | Detector spectral response: signal = dot(scene RGB, signal_weight_*). Default is BT.709 luminance; NVG weights red/NIR heavily (the photocathode is red/NIR-sensitive; the green is only the phosphor display). No env override. |
| `signal_weight_g` | float | `0.7152` | `0.7152` | `0.3` | See `signal_weight_r`. No env override. |
| `signal_weight_b` | float | `0.0722` | `0.0722` | `0.1` | See `signal_weight_r`. No env override. |

**`sensor_modes.<mode>.exposure`** (ROADMAP 3B, GPU sensor path). No env overrides — per-mode only. The sensor AE exposes the scene histogram's median to `target_grey`, but never lets the `highlight_percentile` pixel pass the clip point, then clamps the gain to `[min_gain_ev, max_gain_ev]`. `render.exposure_compensation_ev` does not apply (it is UE's auto-exposure bias on the legacy path). Defaults were calibrated on the bench shots on 2026-09-27 (M1 Pro, San Francisco; ROADMAP 3B.1): daylight scene medians are 2^10.9–2^12.4 and expose inside the limits (EO mean luma 105–114, < 0.1% clipped); `night_slant` (median 2^6.6) clamps at EO's `max_gain_ev` (mean luma 26); NVG at night is not clamped (gain −8.8, mean luma 67). The same per-mode values are FCamSimConfig's built-in defaults (used when the yaml has no `exposure:` block; `CamSim.Sensor.Config.PerModeExposureDefaults`/`…CanonicalConfig` keep code and yaml in step); a bare `FSensorExposureConfig` holds neutral values (−20/−6, 0.18, 0.99).

| Field | Type | EO default | IR default | NVG default | Description |
|-------|------|------------|------------|-------------|-------------|
| `exposure.auto` | bool | `true` | `true` | `true` | Auto-exposure on; `false` uses `manual_gain_ev`. |
| `exposure.min_gain_ev` | float | `-20.0` | `-20.0` | `-20.0` | Lowest gain the simulated camera can select, log2 of the multiplier applied to absolute scene-linear values (higher = brighter). |
| `exposure.max_gain_ev` | float | `-12.5` | `-6.0` | `6.0` | Highest gain the simulated camera can select. EO's value makes night scenes stay dark (dawn/dusk clamp slightly too). |
| `exposure.target_grey` | float | `0.18` | `0.18` | `0.3` | Linear value the histogram median is exposed to. |
| `exposure.highlight_percentile` | float | `0.99` | `0.99` | `0.97` | This percentile of the histogram is kept below clipping. NVG lets its brightest 3% bloom. |
| `exposure.lag_frames` | int | `2` | `2` | `2` | Convergence time constant in frames at 30 Hz (sim time); `0` = instant. |
| `exposure.manual_gain_ev` | float | `-12.0` | `-12.0` | `-12.0` | Gain used when `exposure.auto` is `false`. |

IR's `exposure` block is used only when `agc_enabled` is `false`; with AGC on, the IR AGC maps its `agc_low_percentile`…`agc_high_percentile` band to the output range instead.

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

### Ground-truth Sidecar (Phase D3)

Ground-truth sidecar writes per-frame JSONL records (pose, gimbal, LOS, sensor
state, and active view routes) for analytics and dataset generation workflows.

| Field | Type | Default | Env var | Description |
|-------|------|---------|---------|-------------|
| `ground_truth.enabled` | bool | `false` | `CAMSIM_GROUND_TRUTH_ENABLED` | Enable JSONL sidecar writes. |
| `ground_truth.output_path` | string | `camsim_groundtruth.jsonl` | `CAMSIM_GROUND_TRUTH_PATH` | Output path (relative paths resolve from binary directory). |
| `ground_truth.interval_frames` | int | `1` | `CAMSIM_GROUND_TRUTH_INTERVAL_FRAMES` | Emit every N frames. |

### Entity Runtime Scale Controls (Phase C3)

`entity_scale` applies runtime throttles for dense scenes:

| Field | Type | Default | Env var | Description |
|-------|------|---------|---------|-------------|
| `entity_scale.max_draw_distance_m` | float | `0.0` | `CAMSIM_ENTITY_MAX_DRAW_DISTANCE_M` | Draw/cull distance in metres for entity meshes/lights. `0` disables culling by distance. |
| `entity_scale.tick_rate_hz` | float | `0.0` | `CAMSIM_ENTITY_TICK_RATE_HZ` | Tick rate applied to `ACamSimEntity`. `0` means every frame. |
| `entity_scale.default_max_update_rate_hz` | float | `0.0` | `CAMSIM_ENTITY_DEFAULT_MAX_UPDATE_RATE_HZ` | Global cap for pose-apply rate (reduces transform churn). `0` means uncapped. |
| `entity_scale.max_update_rate_hz_overrides` | object | `{}` | -- | Per-entity overrides keyed by `EntityId` string. |

Legacy flat keys (`entity_max_draw_distance_m`, `entity_tick_rate_hz`, `entity_default_max_update_rate_hz`) are still accepted.

### Scenario Orchestration (Phase C1)

`scenario` enables deterministic entity spawn/update/despawn behavior directly in
CamSim (without an external CIGI controller). This is useful for repeatable
scenario authoring and CI smoke scenes.

| Field | Type | Default | Env var | Description |
|-------|------|---------|---------|-------------|
| `scenario.enabled` | bool | `false` | `CAMSIM_SCENARIO_ENABLED` | Enable built-in scenario entity orchestration. |
| `scenario.time_scale` | float | `1.0` | `CAMSIM_SCENARIO_TIME_SCALE` | Scenario time is the sim clock; this multiplies the clock's rate when the scenario starts (prefer `sim_time_rate`). |
| `scenario.start_hour` | float | *(unset)* | `CAMSIM_SCENARIO_START_HOUR` | Local solar hour (0–24, at the start position) the sim clock is set to when the scenario starts; the sun and pattern-of-life schedules follow it. Unset: keep the sim clock's time. |
| `scenario.entities` | array | `[]` | -- | Scripted entity definitions. |

Per-entry fields in `scenario.entities[]`:

- `entity_id`, `entity_type`
- `start_latitude`, `start_longitude`, `start_altitude`
- `start_yaw`, `start_pitch`, `start_roll`
- `spawn_time_sec`, `despawn_time_sec`
- `update_rate_hz`
- `north_rate_mps`, `east_rate_mps`, `up_rate_mps`
- `yaw_rate_dps`, `pitch_rate_dps`, `roll_rate_dps`

`despawn_time_sec <= spawn_time_sec` means the entity persists for the full run.
`update_rate_hz = 0` applies updates every manager tick.

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

### Prometheus Metrics (Phase 12D)

| Field | Type | Default | Env var | Description |
|-------|------|---------|---------|-------------|
| `prometheus_metrics_path` | string | `""` | `CAMSIM_PROMETHEUS_METRICS_PATH` | Path for Prometheus node_exporter textfile-collector compatible `.prom` file. Empty = disabled. |

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
| `mesh` | string | Yes | UE content browser path for the primary mesh asset. Format: `"/Game/Path/To/Asset.Asset"`. |
| `skeletal` | bool | No (default `false`) | `true` for `USkeletalMesh` (enables articulated part control). `false` for `UStaticMesh`. |
| `mesh_damaged` | string | No | Alternative mesh for damage state 1 (Component Control CompId=10, state=1). Falls back to `mesh` if omitted. |
| `mesh_destroyed` | string | No | Alternative mesh for damage state 2 (Component Control CompId=10, state=2). Falls back to `mesh` if omitted. |

Entity type IDs are defined by the host simulation. CamSim does not reserve any
specific IDs -- the mapping is entirely user-configured.

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
CAMSIM_SENSOR_QUALITY_PRESET=medium
CAMSIM_SENSOR_QUALITY_NOISE_SCALE=1.0
CAMSIM_SENSOR_QUALITY_VIGNETTING_SCALE=1.0
CAMSIM_SENSOR_QUALITY_SCANLINE_SCALE=1.0
CAMSIM_SENSOR_QUALITY_ATMOSPHERE_SCALE=1.0
CAMSIM_SENSOR_QUALITY_BLUR_RADIUS=0
CAMSIM_SENSOR_QUALITY_CONTRAST=1.0
CAMSIM_SENSOR_QUALITY_BRIGHTNESS_BIAS=0.0
CAMSIM_GROUND_TRUTH_ENABLED=0
CAMSIM_GROUND_TRUTH_PATH=camsim_groundtruth.jsonl
CAMSIM_GROUND_TRUTH_INTERVAL_FRAMES=1
CAMSIM_TERRAIN_PROVIDER=cesium
CAMSIM_IMAGERY_PROVIDER=cesium
CAMSIM_ENTITY_MAX_DRAW_DISTANCE_M=0
CAMSIM_ENTITY_TICK_RATE_HZ=0
CAMSIM_ENTITY_DEFAULT_MAX_UPDATE_RATE_HZ=0
CAMSIM_SCENARIO_ENABLED=0
CAMSIM_SCENARIO_TIME_SCALE=1.0

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

# Prometheus metrics (Phase 12D)
CAMSIM_PROMETHEUS_METRICS_PATH=

# Recording & playback (Phase 12E)
CAMSIM_CIGI_RECORD_PATH=
CAMSIM_VIDEO_RECORD_PATH=
CAMSIM_CIGI_PLAYBACK_PATH=

# Operational hardening (Phase 28)
CAMSIM_STRUCTURED_LOG_PATH=
CAMSIM_STRUCTURED_LOG_MAX_MB=100
CAMSIM_HEALTH_HTTP_ENABLED=1
CAMSIM_HEALTH_HTTP_PORT=8080
CAMSIM_TRACK_PIPELINE_LATENCY=0
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
| `operational.snapshot_endpoint_enabled` | `CAMSIM_SNAPSHOT_ENDPOINT_ENABLED` | `false` | ROADMAP 3A/3B. Binds `GET /snapshot`: returns the next grabbed frame as PNG (legacy path: before sensor effects and encoding; GPU sensor path: the sensor image, since the graph replaces the tonemapper), 503 if no frame arrives within 5 s. Also binds `GET /snapshot/sensor` (ROADMAP 3B): the encoded sensor image as PNG; legacy path: same as `/snapshot`. Both are for the bench harness (`scripts/bench/`). Unbound → 404. |
| `operational.frame_stats_path` | `CAMSIM_FRAME_STATS_PATH` | `""` | ROADMAP 3A/3B. Per-frame JSONL render stats (wall-clock frame time, `stat unit` thread/GPU times, frames emitted/dropped, tileset load %, SSE, camera cut, scene renders per frame, and — ROADMAP 3B — `sensor_gpu_ms`/`sensor_gain_ev`/`scene_median_log2` from the GPU sensor graph; on the legacy path `sensor_gpu_ms` is −1 and the other two are `null`). Empty disables. A relative path is taken from the directory CamSim was launched from. |

**Routes:**

| Route | Semantics | 200 body | 503 body |
|---|---|---|---|
| `GET /live` | Kubernetes liveness convention. Watchdog: returns 200 when the game-thread `Tick()` has fired within the last 5 seconds, 503 otherwise. | `{"status":"ok"}` | `{"status":"stalled","last_tick_ago_s":12.3}` |
| `GET /health` | `sim-environment` REST orchestrator convention. **Alias for `/live`** — same handler, same semantics. Added so the orchestrator's generic `/health` probe naming works without breaking existing K8s manifests. | `{"status":"ok"}` | Same as `/live` |
| `GET /ready` | Readiness: encoder open AND at least one CIGI packet received AND first frame successfully encoded. 200 only when all three gates pass. | `{"status":"ready","encoder":true,"cigi":true,"first_frame":true}` | `{"status":"not_ready","encoder":false,"cigi":true,"first_frame":false}` |
| `GET /metrics` | Prometheus exposition format (`text/plain; charset=utf-8`, version 0.0.4). | See metric list below. | N/A — always 200. |
| `GET /snapshot` | ROADMAP 3A. Only bound when `operational.snapshot_endpoint_enabled`. Next grabbed frame as PNG: before sensor effects and encoding on the legacy path; the sensor image on the GPU sensor path (same as `/snapshot/sensor`). | `image/png` | `{"status":"no_frame"}` if no frame arrives within 5 s |
| `GET /snapshot/sensor` | ROADMAP 3B. Only bound when `operational.snapshot_endpoint_enabled`. The encoded sensor image as PNG; legacy path: same as `/snapshot`. | `image/png` | Same as `/snapshot` |

**`/metrics` contract** (matches the `sim-environment` orchestrator spec §10.4):

- **Gauges:** `camsim_render_fps`, `camsim_output_fps`, `camsim_entity_count`, `camsim_uptime_seconds`
- **Counters:** `camsim_frame_drops_total`, `camsim_cigi_packets_total`, `camsim_dis_packets_total`, `camsim_frames_encoded_total`
- **Histograms (optional):** `camsim_frame_latency_ms` — P50/P95/P99 from `FPipelineLatencyTracker`. Only emitted when `performance.track_pipeline_latency = true`.

The render/output FPS gauges are 1Hz rolling measurements updated from the subsystem's `Tick()` (not target values from config). FPS is `(frame_count_delta) / (wall_clock_delta)` over approximately one second.

### Render Path (ROADMAP 3A)

```yaml
render:
  view_source: primary
  camera_cut_distance_m: 500.0
  camera_cut_angle_deg: 30.0
  origin_shift_distance_m: 20000.0
  exposure_compensation_ev: -1.0
  sensor_path: auto
```

| Key | Env | Default | Description |
|---|---|---|---|
| `render.view_source` | `CAMSIM_RENDER_VIEW_SOURCE` | `primary` | `primary`: the sensor is the game viewport's view (TSR, one scene render per frame). `scene_capture`: legacy `SceneCapture2D` path, kept for A/B comparison until ROADMAP 3B. Unknown values warn and use `primary`. |
| `render.camera_cut_distance_m` | `CAMSIM_RENDER_CAMERA_CUT_DISTANCE_M` | `500.0` | A camera move larger than this in one frame (teleport, origin rebase) resets TSR history. Must be > 0: a non-positive value is a validation error and that check is skipped. |
| `render.camera_cut_angle_deg` | `CAMSIM_RENDER_CAMERA_CUT_ANGLE_DEG` | `30.0` | A view rotation larger than this in one frame resets TSR history. Must be > 0: a non-positive value is a validation error and that check is skipped. |
| `render.exposure_compensation_ev` | `CAMSIM_RENDER_EXPOSURE_COMPENSATION_EV` | `-1.0` | Auto-exposure compensation (EV), applied as the sensor view's `AutoExposureBias`. Auto-exposure stays on; this shifts where it settles. UE's default metering (0) over-brightens sunlit Cesium terrain by about 1 EV. Applied at startup. Legacy path only; the GPU path uses `sensor_modes.*.exposure.target_grey` (UE's exposure is manual there, driven by the sensor AE). |
| `render.origin_shift_distance_m` | `CAMSIM_RENDER_ORIGIN_SHIFT_DISTANCE_M` | `20000.0` | Rebase the Cesium georeference (`CesiumOriginShiftComponent`, `ChangeCesiumGeoreference` mode) when the camera is this far from the origin. Keeps local "up" = +Z and coordinates small. `0` disables. |
| `render.sensor_path` | `CAMSIM_RENDER_SENSOR_PATH` | `auto` | ROADMAP 3B. `auto`: GPU sensor model once every enabled effect is ported, else legacy. `gpu`: force the GPU sensor model (unported effects are logged and ignored). `legacy`: force the UE tonemapper + CPU sensor model (3B.1 default behaviour). Unknown values warn and use `auto`. The stream's transfer characteristic is tagged BT.709 on the GPU path (it applies the BT.709 OETF) and sRGB on the legacy path. The GPU path replaces UE's tonemapper, so UE's tonemapper-stage effects (vignette, film grain, colour grading, bloom dirt mask) don't apply on it; chromatic aberration (`optical_realism.chromatic_aberration`) counts as unported until 3B.3. It needs NV12-compatible dimensions (`capture_width` a multiple of 4, even `capture_height`), a real RHI with SM5 compute and the sensor shaders: otherwise CamSim logs an error and runs legacy (a bad width is also a config validation error whenever the GPU path is wanted). Chosen once at startup: a hot reload doesn't change it, or `capture_width`/`capture_height`. |

**Render resolution (TSR).** With `view_source: primary`, `rendering_quality.tsr_screen_percentage`
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
