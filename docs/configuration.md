# Configuration Reference

CamSim reads `camsim_config.json` from the project directory (or the binary
directory as a fallback). Environment variables override any JSON value.

**Canonical file:** `deploy/camsim_config.json` is the single source of truth
for all three deployment modes. `run.sh` copies it into the UE project
directory before launch; the container `entrypoint.sh` copies it into the
binary directory. Edit only `deploy/camsim_config.json`; the project-dir copy
is generated and is listed in `.gitignore`.

## Full Example

```json
{
  "cigi_bind_addr":      "0.0.0.0",
  "cigi_port":           8888,
  "cigi_response_addr":  "127.0.0.1",
  "cigi_response_port":  8889,
  "camera_entity_id":    0,

  "multicast_addr":  "239.1.1.1",
  "multicast_port":  5004,
  "video_bitrate":   4000000,
  "h264_preset":     "ultrafast",
  "h264_tune":       "zerolatency",

  "capture_width":   1920,
  "capture_height":  1080,
  "frame_rate":      30.0,
  "swap_rb_readback": false,
  "readback_ready_polls": 2,
  "readback_format": "auto",
  "encoder_watchdog_policy": "reconnect",
  "encoder_watchdog_interval_ticks": 150,
  "hfov_deg":        60.0,
  "terrain_provider": "cesium",
  "imagery_provider": "cesium",

  "tile_preload_fov_scale":      2.0,
  "max_simultaneous_tile_loads": 40,

  "start_latitude":  38.8977,
  "start_longitude": -77.0365,
  "start_altitude":  500.0,
  "start_yaw":       0.0,
  "start_pitch":     -45.0,
  "start_roll":      0.0,
  "start_hour":      12.0,

  "gimbal_max_slew_rate": 60.0,
  "gimbal_pitch_min":    -90.0,
  "gimbal_pitch_max":     30.0,
  "gimbal_yaw_min":     -180.0,
  "gimbal_yaw_max":      180.0,
  "sensor_fov_presets":  [60.0, 30.0, 10.0, 3.0],

  "sensor_quality": {
    "preset": "medium",
    "noise_scale": 1.0,
    "vignetting_scale": 1.0,
    "scan_line_scale": 1.0,
    "atmosphere_scale": 1.0,
    "blur_radius": 0,
    "contrast": 1.0,
    "brightness_bias": 0.0
  },

  "output_views": [
    {
      "view_id": 0,
      "enabled": true,
      "multicast_addr": "239.1.1.1",
      "multicast_port": 5004,
      "video_bitrate": 4000000,
      "h264_preset": "ultrafast",
      "h264_tune": "zerolatency",
      "hfov_deg": 0.0
    },
    {
      "view_id": 1,
      "enabled": false,
      "multicast_addr": "239.1.1.2",
      "multicast_port": 5005,
      "video_bitrate": 2500000,
      "h264_preset": "ultrafast",
      "h264_tune": "zerolatency",
      "hfov_deg": 20.0
    }
  ],

  "ground_truth": {
    "enabled": false,
    "output_path": "camsim_groundtruth.jsonl",
    "interval_frames": 1
  },

  "entity_scale": {
    "max_draw_distance_m": 0.0,
    "tick_rate_hz": 0.0,
    "default_max_update_rate_hz": 0.0,
    "max_update_rate_hz_overrides": {
      "1": 30.0
    }
  },

  "scenario": {
    "enabled": false,
    "time_scale": 1.0,
    "entities": [
      {
        "entity_id": 2001,
        "entity_type": 1001,
        "start_latitude": 38.8977,
        "start_longitude": -77.0365,
        "start_altitude": 900.0,
        "start_yaw": 90.0,
        "start_pitch": 0.0,
        "start_roll": 0.0,
        "spawn_time_sec": 0.0,
        "despawn_time_sec": 0.0,
        "update_rate_hz": 10.0,
        "north_rate_mps": 50.0,
        "east_rate_mps": 0.0,
        "up_rate_mps": 0.0,
        "yaw_rate_dps": 0.0,
        "pitch_rate_dps": 0.0,
        "roll_rate_dps": 0.0
      }
    ]
  },

  "entity_types": {
    "1001": {
      "mesh":           "/Game/Models/F16/F16.F16",
      "mesh_damaged":   "/Game/Models/F16/F16_Damaged.F16_Damaged",
      "mesh_destroyed": "/Game/Models/F16/F16_Destroyed.F16_Destroyed",
      "skeletal":       true
    },
    "2001": {
      "mesh":     "/Game/Models/Truck/Truck.Truck",
      "skeletal": false
    }
  }
}
```

## Field Reference

### CIGI Input

| Field | Type | Default | Env var | Description |
|-------|------|---------|---------|-------------|
| `cigi_bind_addr` | string | `"0.0.0.0"` | `CAMSIM_CIGI_BIND_ADDR` | Local address to bind the CIGI UDP socket. Use `"0.0.0.0"` to listen on all interfaces. |
| `cigi_port` | int | `8888` | `CAMSIM_CIGI_PORT` | UDP port for incoming CIGI 3.3 packets (Host → IG). |
| `cigi_response_addr` | string | `"127.0.0.1"` | `CAMSIM_CIGI_RESPONSE_ADDR` | Destination IP address for IG → Host packets (SOF heartbeat, HAT/HOT responses, LOS responses). Set to the host simulation's IP. |
| `cigi_response_port` | int | `8889` | `CAMSIM_CIGI_RESPONSE_PORT` | Destination UDP port for IG → Host packets. |
| `camera_entity_id` | int | `0` | — | CIGI Entity ID that controls the camera. All other entity IDs are managed by the entity renderer. Must match the `--entity-id` value passed to `send_cigi_test.py`. |

### Video Output

| Field | Type | Default | Env var | Description |
|-------|------|---------|---------|-------------|
| `multicast_addr` | string | `"239.1.1.1"` | `CAMSIM_MULTICAST_ADDR` | UDP multicast group (or unicast address) for the MPEG-TS stream. |
| `multicast_port` | int | `5004` | `CAMSIM_MULTICAST_PORT` | Destination UDP port. |
| `video_bitrate` | int | `4000000` | `CAMSIM_VIDEO_BITRATE` | Target H.264 bitrate in bits per second. |
| `h264_preset` | string | `"ultrafast"` | `CAMSIM_H264_PRESET` | libx264 encoding preset. Slower presets (`fast`, `medium`) give better quality at higher CPU cost. |
| `h264_tune` | string | `"zerolatency"` | — | libx264 tune parameter. `"zerolatency"` minimises encode latency. |

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
| `encoder_watchdog_policy` | string | `"reconnect"` | `CAMSIM_ENCODER_WATCHDOG_POLICY` | Encoder watchdog action when no frames are written for `encoder_watchdog_interval_ticks`: `reconnect`, `log_only`, or `fail_fast`. |
| `encoder_watchdog_interval_ticks` | int | `150` | `CAMSIM_ENCODER_WATCHDOG_INTERVAL_TICKS` | Tick interval used by the encoder watchdog and runtime health checks. |

### Geospatial Providers (Phase F1 foundation)

| Field | Type | Default | Env var | Description |
|-------|------|---------|---------|-------------|
| `terrain_provider` | string | `"cesium"` | `CAMSIM_TERRAIN_PROVIDER` | Terrain/georeference provider selector. Currently supported: `cesium` (unsupported values fall back to `cesium` with warning). |
| `imagery_provider` | string | `"cesium"` | `CAMSIM_IMAGERY_PROVIDER` | Imagery provider selector (currently informational; `cesium` supported). |

### Cesium Tile Streaming

| Field | Type | Default | Env var | Description |
|-------|------|---------|---------|-------------|
| `tile_preload_fov_scale` | float | `2.0` | `CAMSIM_TILE_FOV_SCALE` | Multiplier applied to `hfov_deg` when registering with `ACesiumCameraManager`. Values above 1.0 pre-fetch tiles outside the visible frustum to reduce pop-in when the camera pans. |
| `max_simultaneous_tile_loads` | int | `40` | `CAMSIM_MAX_TILE_LOADS` | Maximum concurrent Cesium tile HTTP requests. Higher values speed up initial scene load at the cost of network/CPU. |

### Camera Start Position

Used as the initial camera pose before the first CIGI Entity Control packet arrives.

| Field | Type | Default | Env var | Description |
|-------|------|---------|---------|-------------|
| `start_latitude` | double | `38.8977` | `CAMSIM_START_LAT` | WGS-84 latitude in decimal degrees. |
| `start_longitude` | double | `-77.0365` | `CAMSIM_START_LON` | WGS-84 longitude in decimal degrees. |
| `start_altitude` | double | `500.0` | `CAMSIM_START_ALT` | Height above WGS-84 ellipsoid in metres. |
| `start_yaw` | float | `0.0` | `CAMSIM_START_YAW` | Initial heading in degrees [0, 360). |
| `start_pitch` | float | `-45.0` | `CAMSIM_START_PITCH` | Initial pitch in degrees. Negative = looking down. |
| `start_roll` | float | `0.0` | `CAMSIM_START_ROLL` | Initial roll in degrees. |
| `start_hour` | float | `12.0` | `CAMSIM_START_HOUR` | Initial time of day (0–24). Used to set sun position before a CIGI Celestial Control packet is received. |

### Gimbal and Sensor (Phase 9)

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `gimbal_max_slew_rate` | float | `60.0` | Maximum gimbal slew rate in degrees per second. `0` means unlimited (instant). Applies to both yaw and pitch axes. |
| `gimbal_pitch_min` | float | `-90.0` | Lower pitch limit in degrees (negative = looking down). |
| `gimbal_pitch_max` | float | `30.0` | Upper pitch limit in degrees. |
| `gimbal_yaw_min` | float | `-180.0` | Left yaw limit in degrees relative to platform heading. |
| `gimbal_yaw_max` | float | `180.0` | Right yaw limit in degrees relative to platform heading. |
| `sensor_fov_presets` | float[] | `[60.0, 30.0, 10.0, 3.0]` | Horizontal FOV values in degrees, ordered wide to narrow. The Sensor Control packet's Gain field (0.0–1.0) selects the preset by index. |

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

Per-waveband `sensor_modes` now also supports:

- `atmospheric_visibility_m`
- `atmosphere_strength`
- `color_temperature_k`
- `contrast`
- `brightness_bias`
- `blur_radius`

### Multi-stream Output Views (Phase D2)

`output_views` optionally fans out the same captured frame to multiple MPEG-TS
outputs. Each view can use an independent route and output HFOV metadata.
When `hfov_deg` is narrower than the live HFOV, CamSim applies a center crop
digital zoom before encoding that view.

When `CAMSIM_MULTICAST_ADDR` and/or `CAMSIM_MULTICAST_PORT` are set in the
environment (for example via `./scripts/run.sh --local`), CamSim applies those
route overrides to all configured `output_views` so local unicast testing
does not silently keep per-view multicast routes from JSON.

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
| `entity_scale.max_update_rate_hz_overrides` | object | `{}` | — | Per-entity overrides keyed by `EntityId` string. |

Legacy flat keys (`entity_max_draw_distance_m`, `entity_tick_rate_hz`, `entity_default_max_update_rate_hz`) are still accepted.

### Scenario Orchestration (Phase C1)

`scenario` enables deterministic entity spawn/update/despawn behavior directly in
CamSim (without an external CIGI controller). This is useful for repeatable
scenario authoring and CI smoke scenes.

| Field | Type | Default | Env var | Description |
|-------|------|---------|---------|-------------|
| `scenario.enabled` | bool | `false` | `CAMSIM_SCENARIO_ENABLED` | Enable built-in scenario entity orchestration. |
| `scenario.time_scale` | float | `1.0` | `CAMSIM_SCENARIO_TIME_SCALE` | Multiplier for scenario time progression. |
| `scenario.entities` | array | `[]` | — | Scripted entity definitions. |

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

### Entity Types

The `entity_types` object maps CIGI Entity Type IDs (uint16, as JSON string keys)
to asset paths and flags:

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `mesh` | string | Yes | UE content browser path for the primary mesh asset. Format: `"/Game/Path/To/Asset.Asset"`. |
| `skeletal` | bool | No (default `false`) | `true` for `USkeletalMesh` (enables articulated part control). `false` for `UStaticMesh`. |
| `mesh_damaged` | string | No | Alternative mesh for damage state 1 (Component Control CompId=10, state=1). Falls back to `mesh` if omitted. |
| `mesh_destroyed` | string | No | Alternative mesh for damage state 2 (Component Control CompId=10, state=2). Falls back to `mesh` if omitted. |

Entity type IDs are defined by the host simulation. CamSim does not reserve any
specific IDs — the mapping is entirely user-configured.

At startup, CamSim now performs preflight validation for each entry:

- Verifies `mesh` exists.
- Validates static vs skeletal compatibility for `/Game/...` assets.
- Validates optional `mesh_damaged` / `mesh_destroyed` paths and ignores invalid variants with warnings.
- Skips invalid entity type entries instead of failing later at spawn time.

**Example:** To add a helicopter (type 3001) and an armoured vehicle (type 4001):

```json
"entity_types": {
  "3001": { "mesh": "/Game/Models/Helo/UH60.UH60", "skeletal": true },
  "4001": { "mesh": "/Game/Models/Vehicles/M1A2.M1A2", "skeletal": false }
}
```

## Environment Variable Quick Reference

```bash
# CIGI input (Host → IG)
CAMSIM_CIGI_BIND_ADDR=0.0.0.0
CAMSIM_CIGI_PORT=8888

# CIGI output (IG → Host: SOF, HAT/HOT, LOS)
CAMSIM_CIGI_RESPONSE_ADDR=127.0.0.1
CAMSIM_CIGI_RESPONSE_PORT=8889

# Video output
CAMSIM_MULTICAST_ADDR=239.1.1.1   # or 127.0.0.1 for unicast loopback test
CAMSIM_MULTICAST_PORT=5004
CAMSIM_VIDEO_BITRATE=4000000
CAMSIM_H264_PRESET=ultrafast
CAMSIM_SWAP_RB_READBACK=0
CAMSIM_READBACK_READY_POLLS=2
CAMSIM_READBACK_FORMAT=auto
CAMSIM_ENCODER_WATCHDOG_POLICY=reconnect
CAMSIM_ENCODER_WATCHDOG_INTERVAL_TICKS=150
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
CAMSIM_START_LAT=38.8977
CAMSIM_START_LON=-77.0365
CAMSIM_START_ALT=500.0
CAMSIM_START_YAW=0.0
CAMSIM_START_PITCH=-45.0
CAMSIM_START_ROLL=0.0
CAMSIM_START_HOUR=12.0

# Cesium
CAMSIM_TILE_FOV_SCALE=2.0
CAMSIM_MAX_TILE_LOADS=40
```
