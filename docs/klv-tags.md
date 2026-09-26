# MISB ST 0601 KLV Tag Reference

CamSim outputs a MISB ST 0601.9 Local Set embedded in every MPEG-TS packet as a KLVA
data stream (PID assigned by FFmpeg alongside the H.264 video PID).

## Packet Structure

```
[16 bytes] Universal Label  — 0x060E2B34 020B0101 0E010301 01000000
[1–3 bytes] BER length      — short form (<128 bytes) or long form (0x81/0x82 prefix)
[N bytes]   TLV payload     — tags in ascending order (ST 0601 requirement)
[4 bytes]   Tag 1 checksum  — always last
```

Each TLV triplet: `[tag: uint8] [length: uint8] [value: N bytes]`.

## Reference decoder

The KLV is parsed downstream by [misb.js](https://github.com/vidterra/misb.js)
(`@vidterra/misb.js` 0.1.30), so misb.js defines "correct" for this project. Every tag
below is checked against it by `scripts/klv_conformance/check.js` (see Validation).

## Checksum

Tag 1 is a running 16-bit sum: even-indexed bytes are added to the high byte and
odd-indexed bytes to the low byte, modulo 2^16. It covers every byte from the first byte
of the Universal Label through the checksum item's own tag and length bytes (`01 02`),
i.e. everything except the two checksum value bytes. This matches misb.js
`klv.calculateChecksum`. misb.js marks packets with a wrong checksum `valid: false`.

---

## Tags Implemented

Tag 2 is first and Tag 1 last, as ST 0601 requires; the rest are in ascending order.

### Tag 2 — UNIX Time Stamp

| Field | Value |
|-------|-------|
| Format | `uint64`, 8 bytes, big-endian |
| Units | Microseconds since Unix epoch (UTC) |
| Range | 0 .. 2^64-1 |

**Source:** `FUtcClock::NowMicros()`, sampled in `ACamSimCamera::CaptureAndEncode()`
immediately before the scene capture. The clock anchors the monotonic platform timer to
UTC once at startup, so timestamps are real UTC but never jump if the system clock is
adjusted mid-run.

---

### Tag 4 — Platform Tail Number (Phase 26A)

| Field | Value |
|-------|-------|
| Format | ISO 646 (ASCII) string, variable length |
| Max length | 127 bytes |
| Omitted | Tag is not written if `platform_tail_number` is empty |

**Source:** Config value `phase26.platform_tail_number` / `CAMSIM_PLATFORM_TAIL_NUMBER`.
Set once at startup via `FKlvBuilder::Configure()`. Examples: `"N12345"`, `"CAMSIM-01"`.

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
value comes from `hfov_deg` in `camsim_config.yaml`.

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

### Tags 43, 44 — Target Track Gate Width / Height (Phase 26D)

| Field | Value |
|-------|-------|
| Format | `uint8`, 1 byte each |
| Units | Pixels / 2 (misb.js decodes `2 × value`) |
| Range | 0..510 px |
| Omitted | Not written when `target_track_gate_width` / `_height` is 0 |

**Source:** Config values `phase26.target_track_gate_width` / `_height` (pixels) or
`CAMSIM_TARGET_TRACK_GATE_WIDTH` / `_HEIGHT`. Static — set once at startup.

Tags 40/41 are Target Location Latitude/Longitude (4 bytes each). Writing a 1-byte gate
value there makes misb.js throw and drop the whole packet.

---

### Tag 47 — Generic Flag Data

| Field | Value |
|-------|-------|
| Format | `uint8`, 1 byte, bitmask |

Bits are numbered from 1 at the least-significant end:

| Bit | Mask | Name | CamSim |
|-----|------|------|--------|
| 1 | `0x01` | Laser Range | 0 |
| 2 | `0x02` | Auto-Track | 0 |
| 3 | `0x04` | IR Polarity (1 = black-hot) | Set when `SensorPolarity == 1` |
| 4 | `0x08` | Icing Detected | 0 |
| 5 | `0x10` | Slant Range (1 = measured, 0 = calculated) | 0 — the range is ray-cast |
| 6 | `0x20` | Image Invalid | 0 |

**Source:** `UCamSimSensorComponent::GetPolarity()` — set by CIGI Sensor Control;
toggled via `--polarity` in `send_cigi_test.py`.

---

### Tag 48 — Security Local Set (ST 0102)

Nested ST 0102 TLVs, built once by `FKlvBuilder::SetSecurityMetadata()`:

| ST 0102 tag | Name | Encoding |
|-------------|------|----------|
| 1 | Security Classification | `uint8` enum (1 = UNCLASSIFIED … 5 = TOP SECRET) |
| 2 | CC/RI Coding Method | `1` (ISO-3166 two letter) |
| 3 | Classifying Country | ISO 646 string, e.g. `//US` |
| 5 | Caveats | ISO 646 string (optional) |
| 6 | Releasing Instructions | ISO 646 string (optional) |
| 12 | Object Country Coding Method | `1` (ISO-3166 two letter) |
| 13 | Object Country Codes | UTF-16BE string, e.g. `00 55 00 53` = "US" |
| 22 | Version | `uint16` = 12 |

---

### Tag 56 — Platform Ground Speed (Phase 26C)

| Field | Value |
|-------|-------|
| Format | `uint8`, 1 byte |
| Units | Metres/second, 0..255 |
| Encoding | `clamp(round(speed_mps), 0, 255)` |
| Omitted | Not written if `GroundSpeedMps <= 0` |

**Source:** Computed from successive WGS-84 position deltas in `ACamSimCamera::ApplyCigiState()`,
divided by frame delta time. Tag 8 (True Airspeed) is not emitted; CamSim doesn't model airspeed.

---

### Tag 65 — UAS Datalink Local Set Version Number

| Field | Value |
|-------|-------|
| Format | `uint8`, 1 byte |
| Value | `9` (ST 0601.9) |

**Source:** Hardcoded constant. Required in every packet per ST 0601. Allows
decoders to select the correct tag dictionary.

---

### Tag 1 — Checksum

| Field | Value |
|-------|-------|
| Format | `uint16`, 2 bytes, big-endian |
| Algorithm | Running 16-bit sum (see Checksum above) |
| Coverage | Universal Label through the checksum's own `01 02` bytes |

Always the final tag in the packet. See checksum note at the top of this document.

---

## Data Flow Summary

```
CIGI Entity Control (opcode 2)
  └─> FCigiEntityState.{Lat,Lon,Alt,Yaw,Pitch,Roll}
        ├─> Tags 5, 6, 7, 13, 14, 15
        └─> Position delta → Tag 56 (ground speed)

Config (camsim_config.yaml / env vars)
  ├─> phase26.platform_tail_number → Tag 4
  └─> phase26.target_track_gate_width/height → Tags 43, 44

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
              (Tags 21, 23-25 omitted when the boresight is above the horizon)

CIGI Sensor Control (opcode 17)
  └─> UCamSimSensorComponent.{Mode,Polarity}
        ├─> Tag 11  (sensor name string)
        └─> Tag 47  (flag bitmask)

FUtcClock (UTC, monotonic)
  └─> Tag 2

Static / derived
  └─> Tag 12  ("Geodetic WGS84")
  └─> Tag 65  (version = 9)
  └─> Tag 1   (checksum: running 16-bit sum)
```

---

## Validation

`scripts/klv_conformance/check.js` decodes KLV with pinned misb.js and fails on parse
errors, bad checksums, unknown tags, or a non-UTC timestamp:

```sh
cd scripts/klv_conformance && npm ci

# Packets exported by the CamSim.KlvConformance.ExportPackets automation test —
# every tag is compared against the telemetry that produced it
node check.js packets ../../unreal_project/CamSimTest/Saved/KlvConformance/packets.jsonl

# A recording or a live stream (captured for --duration-sec)
node check.js stream /tmp/capture.ts
node check.js stream udp://239.1.1.1:5004 --duration-sec 5

# Add multicast route if needed (macOS)
sudo route add -net 239.0.0.0/8 -interface lo0

# Exercise platform attitude tags
uv run scripts/send_cigi_test.py --sweep

# Exercise sensor mode / polarity tags
uv run scripts/send_cigi_test.py --sensor-id 1 --polarity 1
```

CI runs the `packets` check after the automation tests and the `stream` check in
`scripts/ci_validate.sh`.
