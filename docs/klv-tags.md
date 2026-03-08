# MISB ST 0601 KLV Tag Reference

CamSim outputs a MISB ST 0601.8 Local Set embedded in every MPEG-TS packet as a KLVA
data stream (PID assigned by FFmpeg alongside the H.264 video PID).

## Packet Structure

```
[16 bytes] Universal Label  — 0x060E2B34 020B0101 0E010301 01000000
[1–3 bytes] BER length      — short form (<128 bytes) or long form (0x81/0x82 prefix)
[N bytes]   TLV payload     — tags in ascending order (ST 0601 requirement)
[4 bytes]   Tag 1 checksum  — always last
```

Each TLV triplet: `[tag: uint8] [length: uint8] [value: N bytes]`.

## Checksum

The standard (ST 0601.8 §12) specifies **BCC-16** (running 16-bit modular sum of byte pairs).
This implementation uses **CRC-16/CCITT** (polynomial 0x1021, init 0xFFFF) instead, which
covers the full packet from the Universal Label through the last TLV before Tag 1.
`scripts/validate_klv.py` also uses CRC-16/CCITT. A standards-compliant decoder expecting
BCC-16 will flag the checksum as invalid.

---

## Tags Implemented

Tags are written in ascending numerical order as required by ST 0601.

### Tag 2 — UNIX Time Stamp

| Field | Value |
|-------|-------|
| Format | `uint64`, 8 bytes, big-endian |
| Units | Microseconds since Unix epoch (UTC) |
| Range | 0 .. 2^64-1 |

**Source:** `ACamSimCamera::BuildTelemetry()` — `FDateTime::UtcNow()` converted to
microseconds at the start of each `Tick()` before frame capture.

---

### Tag 5 — Platform Heading Angle

| Field | Value |
|-------|-------|
| Format | `uint16`, 2 bytes, big-endian |
| Units | Degrees, 0..360 |
| Encoding | `round(yaw / 360.0 * 65535)` |

**Source:** `FCigiEntityState.Yaw` — set by the host via CIGI Entity Control (opcode 2)
for the camera entity (entity id == `camera_entity_id` in config). Represents true heading
of the platform airframe, not the gimbal.

---

### Tag 6 — Platform Pitch Angle

| Field | Value |
|-------|-------|
| Format | `int16`, 2 bytes, big-endian, two's complement |
| Units | Degrees, ±20 |
| Encoding | `round(pitch / 20.0 * 32767)`, clamped to ±20° |

**Source:** `FCigiEntityState.Pitch` — from CIGI Entity Control. Positive = nose up.
Values beyond ±20° are clamped at the encoder; the host should not send extreme pitch
for a fixed-wing platform.

---

### Tag 7 — Platform Roll Angle

| Field | Value |
|-------|-------|
| Format | `int16`, 2 bytes, big-endian, two's complement |
| Units | Degrees, ±50 |
| Encoding | `round(roll / 50.0 * 32767)`, clamped to ±50° |

**Source:** `FCigiEntityState.Roll` — from CIGI Entity Control. Positive = right wing down.

---

### Tag 11 — Image Source Sensor

| Field | Value |
|-------|-------|
| Format | ISO 646 (ASCII) string, variable length |
| Max length | 127 bytes |

**Source:** `UCamSimSensorComponent::GetMode()` — set by CIGI Sensor Control (opcode 17)
`SensorId` field, mapped via `--sensor-id` in `send_cigi_test.py`.

| `SensorMode` | String written |
|-------------|----------------|
| 0 (EO)      | `EO Nose`      |
| 1 (IR)      | `LWIR`         |
| 2 (NVG)     | `NVG`          |

---

### Tag 12 — Image Coordinate System

| Field | Value |
|-------|-------|
| Format | ISO 646 string, fixed |
| Value | `Geodetic WGS84` |

**Source:** Static — always `Geodetic WGS84`. CamSim reports all geographic coordinates
in WGS-84 decimal degrees as received from CIGI and confirmed by Cesium's globe model.

---

### Tag 13 — Sensor Latitude

| Field | Value |
|-------|-------|
| Format | `int32`, 4 bytes, big-endian |
| Units | Degrees, ±90 |
| Encoding | `round(lat / 90.0 * 2147483647)` |

**Source:** `FCigiEntityState.Latitude` — WGS-84 geodetic latitude of the camera platform
as commanded by the host. Updated every frame a camera Entity Control packet is received.

---

### Tag 14 — Sensor Longitude

| Field | Value |
|-------|-------|
| Format | `int32`, 4 bytes, big-endian |
| Units | Degrees, ±180 |
| Encoding | `round(lon / 180.0 * 2147483647)` |

**Source:** `FCigiEntityState.Longitude` — WGS-84 geodetic longitude of the camera platform.

---

### Tag 15 — Sensor True Altitude

| Field | Value |
|-------|-------|
| Format | `uint16`, 2 bytes, big-endian |
| Units | Metres, −900..19000 m |
| Encoding | `round((alt - (-900)) / 19900.0 * 65535)` |

**Source:** `FCigiEntityState.Altitude` — metres above the WGS-84 ellipsoid as commanded
by the host. Not terrain-relative; the host is responsible for providing ellipsoidal height.

---

### Tag 16 — Sensor Horizontal Field of View

| Field | Value |
|-------|-------|
| Format | `uint16`, 2 bytes, big-endian |
| Units | Degrees, 0..180 |
| Encoding | `round(hfov / 180.0 * 65535)` |

**Source:** `SceneCapture->FOVAngle` — set from CIGI View Definition (opcode 21) `FovLeft`
+ `FovRight` fields. Updated immediately when a View Definition packet is received. Default
value comes from `hfov_deg` in `camsim_config.json`.

---

### Tag 17 — Sensor Vertical Field of View

| Field | Value |
|-------|-------|
| Format | `uint16`, 2 bytes, big-endian |
| Units | Degrees, 0..180 |
| Encoding | `round(vfov / 180.0 * 65535)` |

**Source:** Derived from `SceneCapture->FOVAngle` × `(capture_height / capture_width)`.
CIGI View Definition only sets horizontal FOV; vertical is computed from the configured
capture aspect ratio and kept in sync automatically. Default 16:9 → VFOV = HFOV × 9/16.

---

### Tag 18 — Sensor Relative Azimuth Angle

| Field | Value |
|-------|-------|
| Format | `uint32`, 4 bytes, big-endian, **unsigned** |
| Units | Degrees, 0..360 |
| Encoding | `(uint32)(fmod(yaw + 360, 360) / 360.0 * 4294967295)` |

**Source:** `UCamSimGimbalComponent::GetGimbalYaw()` — gimbal azimuth offset relative to
the platform body frame. Driven by either CIGI Articulated Part Control (opcode 6) on the
camera entity's art-part, or CIGI View Control (opcode 16). Slew rate is limited by
`gimbal_max_slew_rate` (°/s) in config; axis limits applied via `gimbal_yaw_min/max`.

Note: The encoding normalises any negative yaw (e.g. −30°) to its positive equivalent
(330°) via `fmod` before mapping to unsigned range.

---

### Tag 19 — Sensor Relative Elevation Angle

| Field | Value |
|-------|-------|
| Format | `int32`, 4 bytes, big-endian, **signed** |
| Units | Degrees, ±180 |
| Encoding | `round(pitch / 180.0 * 2147483647)`, clamped to ±180° |

**Source:** `UCamSimGimbalComponent::GetGimbalPitch()` — gimbal elevation offset relative
to the platform body frame. Negative = looking down. Slew rate limited; axis limits
applied via `gimbal_pitch_min/max` (default −90°..+30°).

---

### Tag 20 — Sensor Relative Roll Angle

| Field | Value |
|-------|-------|
| Format | `uint32`, 4 bytes, big-endian, **unsigned** |
| Units | Degrees, 0..360 |
| Encoding | `(uint32)(fmod(roll + 360, 360) / 360.0 * 4294967295)` |

**Source:** `UCamSimGimbalComponent::GetGimbalRoll()` — gimbal roll offset relative to the
platform body frame. Same normalisation as Tag 18.

---

### Tag 21 — Slant Range

| Field | Value |
|-------|-------|
| Format | `uint32`, 4 bytes, big-endian |
| Units | Metres, 0..5 000 000 |
| Encoding | `(uint32)(range / 5000000.0 * 4294967295)` |
| Omitted | Tag is not written if `SlantRangeM == 0` (sensor above horizon or no trace hit) |

**Source:** `ACamSimCamera::ComputeGeometricLOS()`, called every frame after gimbal angles
are updated. Two computation paths:

1. **UE line trace (primary):** `World->LineTraceSingleByChannel(ECC_Visibility)` fired
   from the scene capture origin along the sensor boresight (forward vector). Hit distance
   converted from UE centimetres to metres (`distance / 100.0`). Requires Cesium terrain
   tiles to be loaded; will miss anything not in the collision mesh.

2. **Flat-earth fallback:** Used when no trace hit occurs. Computes geometric slant range
   from platform altitude and gimbal depression angle: `range = altitude / sin(depression)`.
   Returns 0 and omits the tag if the sensor is pointing at or above the horizon.

---

### Tag 23 — Frame Center Latitude

| Field | Value |
|-------|-------|
| Format | `int32`, 4 bytes, big-endian |
| Units | Degrees, ±90 |
| Encoding | Same as Tag 13 |

**Source:** `ACamSimCamera::ComputeGeometricLOS()`.

- Line trace path: `GeoProvider->WorldToGeo(Hit.Location)` — Cesium converts the UE world
  coordinate of the terrain hit point to WGS-84 geodetic coordinates.
- Flat-earth fallback: `platform_lat + (ground_range * cos(azimuth)) / 111320.0`

---

### Tag 24 — Frame Center Longitude

| Field | Value |
|-------|-------|
| Format | `int32`, 4 bytes, big-endian |
| Units | Degrees, ±180 |
| Encoding | Same as Tag 14 |

**Source:** `ACamSimCamera::ComputeGeometricLOS()`.

- Line trace path: `GeoProvider->WorldToGeo(Hit.Location)` — as above.
- Flat-earth fallback: `platform_lon + (ground_range * sin(azimuth)) / (111320.0 * cos(lat))`

---

### Tag 25 — Frame Center Elevation

| Field | Value |
|-------|-------|
| Format | `uint16`, 2 bytes, big-endian |
| Units | Metres, −900..19000 m |
| Encoding | Same as Tag 15 |

**Source:** `ACamSimCamera::ComputeGeometricLOS()`, line trace path only.
`GeoProvider->WorldToGeo(Hit.Location)` returns altitude (`HitAlt`) alongside lat/lon.

**Limitation:** This tag is only meaningful when the UE line trace hits terrain geometry
(Cesium tiles loaded, sensor looking at terrain). In the flat-earth fallback the value
stays `0.0`, which encodes as 0 m MSL — not physically meaningful but not corrupt.

---

### Tag 47 — Generic Flag Data 01

| Field | Value |
|-------|-------|
| Format | `uint8`, 1 byte, bitmask |

Bit numbering is MSB-first (bit 7 = most significant):

| Bit | Mask | Name | Set when |
|-----|------|------|----------|
| 5 | `0x20` | IR Polarity | `SensorPolarity == 1` (black-hot) |
| 3 | `0x08` | Slant Range valid | `SlantRangeM > 0` (range is computed) |
| others | — | Reserved | Always 0 |

**Source:**
- Polarity: `UCamSimSensorComponent::GetPolarity()` — set by CIGI Sensor Control `SensorOn`
  field; toggled via `--polarity` in `send_cigi_test.py`.
- Slant Range valid: `FCamSimTelemetry.SlantRangeM > 0` — same value that drives Tag 21.

---

### Tag 65 — UAS Datalink Local Set Version Number

| Field | Value |
|-------|-------|
| Format | `uint8`, 1 byte |
| Value | `8` (ST 0601.8) |

**Source:** Hardcoded constant. Required in every packet per ST 0601.8 §12. Allows
decoders to select the correct tag dictionary.

---

### Tag 1 — Checksum

| Field | Value |
|-------|-------|
| Format | `uint16`, 2 bytes, big-endian |
| Algorithm | CRC-16/CCITT, polynomial 0x1021, initial value 0xFFFF |
| Coverage | Universal Label + BER length + all TLV payload |

Always the final tag in the packet. See checksum note at the top of this document.

---

## Data Flow Summary

```
CIGI Entity Control (opcode 2)
  └─> FCigiEntityState.{Lat,Lon,Alt,Yaw,Pitch,Roll}
        └─> Tags 5, 6, 7, 13, 14, 15

CIGI View Definition (opcode 21)
  └─> SceneCapture->FOVAngle
        └─> Tags 16, 17

CIGI View Control (opcode 16) or
CIGI Art-Part Control on camera entity (opcode 6)
  └─> UCamSimGimbalComponent.{GimbalYaw,GimbalPitch,GimbalRoll}
        ├─> Tags 18, 19, 20
        └─> ComputeGeometricLOS()
              ├─> UE LineTrace -> Cesium WorldToGeo
              │     └─> Tags 21, 23, 24, 25
              └─> Flat-earth fallback
                    └─> Tags 21, 23, 24  (Tag 25 = 0)

CIGI Sensor Control (opcode 17)
  └─> UCamSimSensorComponent.{Mode,Polarity}
        ├─> Tag 11  (sensor name string)
        └─> Tag 47  (flag bitmask)

System clock (FDateTime::UtcNow)
  └─> Tag 2

Static / derived
  └─> Tag 12  ("Geodetic WGS84")
  └─> Tag 65  (version = 8)
  └─> Tag 1   (CRC-16/CCITT checksum)
```

---

## Validation

Use `scripts/validate_klv.py` against a live stream:

```sh
# Add multicast route if needed (macOS)
sudo route add -net 239.0.0.0/8 -interface lo0

# Validate KLV in the MPEG-TS stream
python3 scripts/validate_klv.py udp://239.1.1.1:5004

# Exercise platform attitude tags
python3 scripts/send_cigi_test.py --sweep

# Exercise sensor mode / polarity tags
python3 scripts/send_cigi_test.py --sensor-id 1 --polarity 1
```

The validator will print each decoded tag with its numeric value. Tag 65 should appear in
every packet with value `8`. Tags 5/6/7 should change during `--sweep`. Tag 11 should
change string value when `--sensor-id` is varied.
