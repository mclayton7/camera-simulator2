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
  "readback_format": "auto",
  "hfov_deg":        60.0,

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
| `readback_format` | string | `"auto"` | `CAMSIM_READBACK_FORMAT` | Override readback byte order: `bgra`, `rgba`, `argb`, `abgr`, or `auto` (use render target format). |
| `hfov_deg` | float | `60.0` | Horizontal field of view in degrees. Used for KLV metadata and Cesium tile preloading. Overridden per-frame by CIGI View Definition packets. |

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
CAMSIM_READBACK_FORMAT=auto

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
