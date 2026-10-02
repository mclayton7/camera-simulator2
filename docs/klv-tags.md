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

**Source:** stamped in `FCamSimTelemetryAssembler::Snapshot()` immediately before the scene
capture, from one of two clocks:

1. **The host's sample time** (HITL): when the datagram that carried this frame's camera pose
   also carried the user-defined Platform Kinematics packet (opcode 201, `hitl/PROTOCOL.md` §2)
   with its sample-time flag set, Tag 2 is that `sample_utc` — the UTC time the host
   predicted the pose for. On frames without a new pose it is carried forward by the engine's
   frame time. A pose without a valid sample time switches back to the sim clock.
2. **The sim clock** otherwise (`FSimClock::Get().NowMicros()`, `Time/SimClock.h`), the same time
   the sun and ground-truth annotations use. It starts at the wall-clock UTC time (or
   `start_datetime` / `start_hour`) and a CIGI Celestial Sphere Control with Date/Time Valid sets
   it, so after a host sets a scenario date the timestamps carry that date, not today's. It
   advances from a monotonic timer, so it never jumps if the system clock is adjusted mid-run.

Tag 2 never steps back by less than a second (host jitter, switching clocks): such a value is
replaced by the previous one + 1 µs. A larger step back (a host restart or replay) passes
through. A frozen clock (CIGI ephemeris off) still repeats its value.

---

### Tag 3 — Mission ID

| Field | Value |
|-------|-------|
| Format | ISO 646 (ASCII) string, variable length |
| Max length | 127 bytes |
| Omitted | Tag is not written if `mission_id` is empty |

**Source:** Config value `phase26.mission_id` / `CAMSIM_MISSION_ID`, set once at startup via
`FKlvBuilder::Configure()`.

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
Values beyond ±20° are clamped at the encoder; Tag 90 (opt-in) carries the full range.

---

### Tag 7 — Platform Roll Angle

| Field | Value |
|-------|-------|
| Format | `int16`, 2 bytes, big-endian, two's complement |
| Units | Degrees, ±50 |
| Encoding | `round(roll / 50.0 * 32767)`, clamped to ±50° |

**Source:** `FCigiEntityState.Roll` — from CIGI Entity Control. Positive = right wing down.
Values beyond ±50° are clamped at the encoder; Tag 91 (opt-in) carries ±90°.

---

### Tags 8, 9 — Platform True / Indicated Airspeed

| Field | Value |
|-------|-------|
| Format | `uint8`, 1 byte each |
| Units | Metres/second, 0..255 |
| Encoding | `clamp(round(speed_mps), 0, 255)` |
| Omitted | Until the host sends Platform Kinematics (opcode 201) with the airspeeds flag, and again 1 s after it stops |

**Source:** the host's `true_airspeed` and `indicated_airspeed` (m/s) in CIGI user-defined
packet 201 (`hitl/PROTOCOL.md` §2). CIGI 3.3 has no airspeed fields. Tag 8 is *true airspeed*;
the ground speed is Tag 56.

---

### Tag 10 — Platform Designation

| Field | Value |
|-------|-------|
| Format | ISO 646 (ASCII) string, variable length |
| Max length | 127 bytes |
| Omitted | Tag is not written if `platform_designation` is empty |

**Source:** Config value `phase26.platform_designation` / `CAMSIM_PLATFORM_DESIGNATION`
(e.g. `MQ-1B`), set once at startup.

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
| 1 (IR)      | `IR`           |
| other (EO)  | `EO`           |

NVG (formerly `SensorMode` 2) was removed — ROADMAP 3B.2.

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

**Source:** `FCigiEntityState.Altitude` minus the EGM96 geoid undulation at the sensor
(`Geospatial/Geoid.h`), i.e. metres above mean sea level. CIGI altitudes (and all CamSim
telemetry) are WGS-84 ellipsoid heights: CIGI 3.3 defines its "MSL" as the reference ellipsoid
surface. The raw ellipsoid height goes in Tag 75.

The geoid grid is `Content/NonUFS/Geoid/WW15MGH.DAC` (NGA's 15′ EGM96 grid, generated by
`scripts/make_egm96_dac.py`). If it is missing, Tags 15 and 25 are omitted rather than sent
as ellipsoid heights mislabelled MSL; a warning is logged once.

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

**Source:** Derived from `SceneCapture->FOVAngle` and the capture aspect ratio for a
rectilinear image: `VFOV = 2·atan(tan(HFOV/2) · capture_height / capture_width)`
(`CamSimTelemetry::VerticalFovDeg`). CIGI View Definition only sets horizontal FOV; vertical is
kept in sync automatically. At 16:9 a 90° HFOV gives 58.7° (the linear `HFOV × 9/16` used
before gave 50.6°); narrow FOVs barely differ. Output views with a digital zoom use the same
formula.

---

### Tag 18 — Sensor Relative Azimuth Angle

| Field | Value |
|-------|-------|
| Format | `uint32`, 4 bytes, big-endian, **unsigned** |
| Units | Degrees, 0..360 |
| Encoding | `(uint32)(fmod(yaw + 360, 360) / 360.0 * 4294967295)` |

**Source:** `UCamSimGimbalComponent::GetGimbalYaw()` — gimbal azimuth offset relative to
the platform body frame. Driven by either CIGI Articulated Part Control (opcode 6) on the
camera entity's art-part, or CIGI View Control (opcode 16). The CIGI yaw (0–360°) is unwound to
−180..180 before the limits `gimbal_yaw_min/max`; Articulated Part slews at
`gimbal_max_slew_rate` (°/s).

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

**Source:** `FCamSimTelemetryAssembler::UpdateFootprint()`, called every frame after the pose,
gimbal and FOV are applied. Two computation paths:

1. **UE line trace (primary):** `World->LineTraceSingleByChannel(ECC_Visibility)` fired
   from the scene capture origin along the sensor boresight (forward vector). Hit distance
   converted from UE centimetres to metres (`distance / 100.0`). Requires Cesium terrain
   tiles to be loaded; will miss anything not in the collision mesh.

2. **Ellipsoid fallback:** Used when no trace hit occurs. The boresight — the platform
   attitude composed with the gimbal angles, roll included (`SensorToNeu`) — is intersected
   with the WGS-84 ellipsoid raised to the height of the last terrain hit (0 m before any).
   Returns 0 and omits the tag if the boresight passes above that surface's horizon.

---

### Tag 23 — Frame Center Latitude

| Field | Value |
|-------|-------|
| Format | `int32`, 4 bytes, big-endian |
| Units | Degrees, ±90 |
| Encoding | Same as Tag 13 |

**Source:** `FCamSimTelemetryAssembler::UpdateFootprint()`.

- Line trace path: `GeoProvider->WorldToGeo(Hit.Location)` — Cesium converts the UE world
  coordinate of the terrain hit point to WGS-84 geodetic coordinates.
- Ellipsoid fallback: the boresight's intersection with the raised ellipsoid (Tag 21).

---

### Tag 24 — Frame Center Longitude

| Field | Value |
|-------|-------|
| Format | `int32`, 4 bytes, big-endian |
| Units | Degrees, ±180 |
| Encoding | Same as Tag 14 |

**Source:** `FCamSimTelemetryAssembler::UpdateFootprint()`.

- Line trace path: `GeoProvider->WorldToGeo(Hit.Location)` — as above.
- Ellipsoid fallback: as Tag 23.

---

### Tag 25 — Frame Center Elevation

| Field | Value |
|-------|-------|
| Format | `uint16`, 2 bytes, big-endian |
| Units | Metres, −900..19000 m |
| Encoding | Same as Tag 15 |

**Source:** `FCamSimTelemetryAssembler::UpdateFootprint()`.
`GeoProvider->WorldToGeo(Hit.Location)` returns the ellipsoid height (`HitAlt`) alongside
lat/lon; Tag 25 subtracts the EGM96 undulation at the frame centre (MSL, like Tag 15), and
Tag 78 carries the ellipsoid height. In the ellipsoid fallback the elevation is the fallback
surface's height (the last terrain hit, or 0 m before any).

---

### Tags 35, 36, 37, 39 — Wind Direction, Wind Speed, Static Pressure, Outside Air Temperature

| Tag | Name | Format | Units, range |
|-----|------|--------|--------------|
| 35 | Wind Direction | `uint16`, `round(dir / 360 · 65535)` | Degrees from true north the wind blows *from*, 0..360 |
| 36 | Wind Speed | `uint8`, `round(speed / 100 · 255)` | m/s, 0..100 |
| 37 | Static Pressure | `uint16`, `round(p / 5000 · 65535)` | mbar, 0..5000 |
| 39 | Outside Air Temperature | `int8`, `round(t)` | °C, −128..127 |

Omitted until the host has sent CIGI Atmosphere Control (opcode 10), so CamSim's defaults
never pass for measurements. **Source:** the latest Atmosphere Control
(`ACamSimEnvironment::FoldAtmosphere`): Global Wind Direction (CIGI's is also "from"),
Global Horizontal Wind Speed, Global Air Temperature. CIGI's Barometric Pressure is taken as the
sea-level value (the HITL host sends X-Plane's `barometer_sealevel`), so Tag 37 reduces it to
the platform's MSL altitude with the ISA atmosphere: `p = p0 · (1 − 2.25577e-5 · h)^5.25588`,
isothermal above 11 km (`FKlvBuilder::StaticPressureMb`).

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

### Tag 55 — Relative Humidity

| Field | Value |
|-------|-------|
| Format | `uint8`, `round(rh / 100 · 255)` |
| Units | Percent, 0..100 |
| Omitted | Until the host has sent Atmosphere Control |

**Source:** Atmosphere Control's Global Humidity.

---

### Tag 56 — Platform Ground Speed (Phase 26C)

| Field | Value |
|-------|-------|
| Format | `uint8`, 1 byte |
| Units | Metres/second, 0..255 |
| Encoding | `clamp(round(speed_mps), 0, 255)` |
| Omitted | Not written if `GroundSpeedMps <= 0` |

**Source:** the horizontal speed of the host's NED velocity, `hypot(vel_north, vel_east)`, when
the host sends it in Platform Kinematics (opcode 201). Otherwise `FGroundSpeedEstimator` —
distance between successive platform fixes divided by the *host* time between them (IG
Control timestamp when valid, otherwise message arrival time). The last value is held between
host updates. Tag 56 is ground speed; true airspeed is Tag 8.

---

### Tag 59 — Platform Call Sign

| Field | Value |
|-------|-------|
| Format | ISO 646 (ASCII) string, variable length |
| Max length | 127 bytes |
| Omitted | Tag is not written if `platform_call_sign` is empty |

**Source:** Config value `phase26.platform_call_sign` / `CAMSIM_PLATFORM_CALL_SIGN`.

---

### Tag 64 — Platform Magnetic Heading

| Field | Value |
|-------|-------|
| Format | `uint16`, 2 bytes, big-endian |
| Units | Degrees, 0..360 (same encoding as Tag 5) |
| Omitted | Until the host sends Platform Kinematics with the magnetic-heading flag, and 1 s after it stops |

**Source:** `magnetic_heading` in CIGI packet 201. CamSim has no magnetic model.

---

### Tag 65 — UAS Datalink Local Set Version Number

| Field | Value |
|-------|-------|
| Format | `uint8`, 1 byte |
| Value | `9` (ST 0601.9) |

**Source:** Hardcoded constant. Required in every packet per ST 0601. Allows
decoders to select the correct tag dictionary.

---

### Tag 75 — Sensor Ellipsoid Height

| Field | Value |
|-------|-------|
| Format | `uint16`, 2 bytes, big-endian |
| Units | Metres above the WGS-84 ellipsoid, −900..19000 m |
| Encoding | Same as Tag 15 |

**Source:** `FCigiEntityState.Altitude`, unchanged. Always emitted.

---

### Tag 78 — Frame Center Height Above Ellipsoid

| Field | Value |
|-------|-------|
| Format | `uint16`, 2 bytes, big-endian |
| Units | Metres above the WGS-84 ellipsoid, −900..19000 m |
| Encoding | Same as Tag 15 |

**Source:** Frame-centre ellipsoid height from `UpdateFootprint()` (see Tag 25). Omitted with
Tags 21 and 23–25 when the boresight has no ground intersection.

---

### Tags 79, 80 — Sensor North / East Velocity

| Field | Value |
|-------|-------|
| Format | `int16`, 2 bytes, big-endian, `round(v / 327 · 32767)` |
| Units | Metres/second, ±327 |
| Omitted | Until the host sends Platform Kinematics with the NED-velocity flag, and 1 s after it stops |

**Source:** `vel_north` / `vel_east` in CIGI packet 201. The sensor rides on the platform (no
lever arm in CamSim), so its velocity is the platform's. `vel_down` is parsed but has no tag.

---

### Tags 82–89 — Corner Latitude / Longitude Points 1–4 (Full)

| Field | Value |
|-------|-------|
| Format | `int32`, 4 bytes each, big-endian, same encoding as Tags 13 / 14 |
| Order | 82/83 point 1 upper left, 84/85 point 2 upper right, 86/87 point 3 lower right, 88/89 point 4 lower left |
| Omitted | Per corner, both tags, when that corner's ray sees no ground (above the horizon) |

**Source:** `FCamSimTelemetryAssembler::UpdateFootprint()`. Each image corner's ray (camera
frame `(1, ±tan(HFOV/2), ±tan(VFOV/2))`) is traced against the terrain like the boresight; when
it hits nothing, it is intersected with the ellipsoid raised to this frame's frame-centre
height. Cost: five line traces per frame on the game thread, measured at 0.03–0.09 ms average
(0.11 ms max) for nadir to near-horizon views on Linux/RTX 5080.

The offset corner Tags 26–33 are not sent: they encode the same points relative to the frame
centre within ±0.075° (≈8 km) at lower precision, while 82–89 carry them at full precision at
any distance; ST 0601 doesn't require both, and misb.js decodes 82–89 correctly.

---

### Tags 90, 91 — Platform Pitch / Roll Angle (Full)

| Field | Value |
|-------|-------|
| Format | `int32`, 4 bytes, big-endian, `round(angle / 90 · 2147483647)` |
| Units | Degrees, ±90 (clamped) |
| Omitted | Unless `phase26.klv_full_range_attitude` / `CAMSIM_KLV_FULL_RANGE_ATTITUDE` is on (default off) |

**Source:** the platform pitch and roll (Tags 6/7), sent alongside them when enabled, so
attitudes beyond Tags 6/7's ±20° / ±50° are not lost.

**misb.js 0.1.30 decodes these wrongly:** it reads only the first two bytes of the 4-byte
value (`readInt16BE`) and scales them as if they were the whole int32, so every angle decodes
as ≈0°. The packet still parses and the other tags are unaffected. That is why the tags are off
by default; enable them only for decoders that follow ST 0601. `check.js` decodes 90/91 itself
per ST 0601 when they are present.

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
        ├─> Tags 5, 6, 7, 13, 14, 75 (+ 90, 91 with klv_full_range_attitude)
        ├─> minus EGM96 undulation → Tag 15 (MSL)
        └─> Position delta / host time → Tag 56 (ground speed)

Config (camsim_config.yaml / env vars)
  ├─> phase26.mission_id / platform_tail_number / platform_designation / platform_call_sign
  │     → Tags 3, 4, 10, 59 (omitted when empty)
  └─> phase26.target_track_gate_width/height → Tags 43, 44

CIGI Platform Kinematics (user-defined opcode 201, camera entity, hitl/PROTOCOL.md §2)
  ├─> airspeeds → Tags 8, 9
  ├─> magnetic heading → Tag 64
  ├─> NED velocity → Tags 79, 80, and Tag 56 (horizontal speed)
  └─> sample_utc → Tag 2

CIGI Atmosphere Control (opcode 10)
  └─> wind, pressure (reduced to the platform altitude), air temperature, humidity
        → Tags 35, 36, 37, 39, 55 (omitted until received)

CIGI View Definition (opcode 21)
  └─> SceneCapture->FOVAngle
        └─> Tags 16, 17

CIGI View Control (opcode 16) or
CIGI Art-Part Control on camera entity (opcode 6)
  └─> UCamSimGimbalComponent.{GimbalYaw,GimbalPitch,GimbalRoll}
        ├─> Tags 18, 19, 20
        └─> UpdateFootprint() (boresight and the four image-corner rays)
              ├─> UE LineTrace -> Cesium WorldToGeo
              │     └─> Tags 21, 23, 24, 78 (HAE), 25 (MSL via EGM96), 82-89
              └─> Raised-ellipsoid fallback
                    └─> Tags 21, 23-25, 78, 82-89
              (Tags 21, 23-25, 78 omitted when the boresight is above the horizon;
               each corner's 82-89 pair when that corner is)

CIGI Sensor Control (opcode 17)
  └─> UCamSimSensorComponent.{Mode,Polarity}
        ├─> Tag 11  (sensor name string)
        └─> Tag 47  (flag bitmask)

FSimClock (sim time, UTC; set by config / CIGI Celestial Sphere Control)
  └─> Tag 2 (unless packet 201 carries the host's sample time)

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
