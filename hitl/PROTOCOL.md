# HITL wire formats

Two formats are shared between separately built pieces of the Hooter HITL rig
(design: `HITL.md` at the repo root). Change both ends together.

## 1. CamSimTruth datagram (X-Plane plugin → IG host)

One UDP datagram per X-Plane flight-loop callback
(`xplm_FlightLoop_Phase_AfterFlightModel`). Default destination `127.0.0.1:49300`,
set in `CamSimTruth.ini` next to the plugin (`dest = host:port`).

- **Little-endian**, packed, fixed size **224 bytes**. Every field is naturally aligned.
- Receivers reject a datagram whose magic, version or size differs.
- Fields named after X-Plane datarefs carry the raw dataref value (no unit conversion),
  except where noted.
- X-Plane 11 publishes the cloud layers as separate scalar datarefs whose names include the
  index (`sim/weather/cloud_base_msl_m[0]`); X-Plane 12 publishes arrays. The plugin tries the
  XP11 names first.

| Offset | Type | Field | Source / units |
| --- | --- | --- | --- |
| 0 | char[4] | magic | `"CSTR"` |
| 4 | u16 | version | 1 |
| 6 | u16 | flags | bit0 paused (`sim/time/paused`), bit1 in replay (`sim/time/is_in_replay`), bit2 weather block valid, bit3 terrain probe valid |
| 8 | u32 | cycle | `XPLMGetCycleNumber()` |
| 12 | u32 | seq | Plugin's own counter, +1 per datagram |
| 16 | u64 | mono_ns | Sender's monotonic clock, ns (ordering and jitter only; different machine from the host) |
| 24 | f64 | latitude | `sim/flightmodel/position/latitude`, deg |
| 32 | f64 | longitude | `.../longitude`, deg |
| 40 | f64 | elevation | `.../elevation`, m MSL |
| 48 | f64 | terrain_msl | `XPLMProbeTerrainXYZ` under the aircraft, converted with `XPLMLocalToWorld`, m MSL (flag bit3) |
| 56 | f32 | true_psi | deg, true north |
| 60 | f32 | true_theta | deg |
| 64 | f32 | true_phi | deg |
| 68 | f32 | p | `Prad`, rad/s body |
| 72 | f32 | q | `Qrad`, rad/s body |
| 76 | f32 | r | `Rrad`, rad/s body |
| 80 | f32 | local_vx | m/s, OpenGL frame (east) |
| 84 | f32 | local_vy | m/s, OpenGL frame (up) |
| 88 | f32 | local_vz | m/s, OpenGL frame (south) |
| 92 | f32 | true_airspeed | m/s |
| 96 | f32 | groundspeed | m/s |
| 100 | f32 | indicated_airspeed | knots |
| 104 | f32 | mag_psi | deg |
| 108 | f32 | y_agl | m |
| 112 | f32 | sim_speed | `sim/time/sim_speed` (int dataref, sent as float) |
| 116 | f32 | sim_speed_actual_ogl | `sim/time/sim_speed_actual_ogl` |
| 120 | f32 | zulu_time_sec | `sim/time/zulu_time_sec` |
| 124 | f32 | local_time_sec | `sim/time/local_time_sec` |
| 128 | u8 | local_month | `sim/cockpit2/clock_timer/current_month` |
| 129 | u8 | local_day | `sim/cockpit2/clock_timer/current_day` |
| 130 | u8 | use_system_time | `sim/time/use_system_time` |
| 131 | u8 | reserved | 0 |
| 132 | f32 | earth_radius_m | `sim/physics/earth_radius_m` |
| 136 | f32 | visibility_m | `sim/weather/visibility_reported_m` (weather block: flag bit2, refreshed about once a second; zeros otherwise) |
| 140 | f32 | temperature_c | `sim/weather/temperature_ambient_c` |
| 144 | f32 | humidity_pct | `sim/weather/relative_humidity_sealevel_percent` |
| 148 | f32 | wind_speed | `sim/weather/wind_speed_kt` (raw; the dataref is actually m/s) |
| 152 | f32 | wind_dir_degt | `sim/weather/wind_direction_degt` (from, true) |
| 156 | f32 | baro_inhg | `sim/weather/barometer_sealevel_inhg` |
| 160 | f32[3] | cloud_base_msl_m | `sim/weather/cloud_base_msl_m[0]`..`[2]` |
| 172 | f32[3] | cloud_tops_msl_m | `sim/weather/cloud_tops_msl_m[0]`..`[2]` |
| 184 | f32[3] | cloud_coverage | `sim/weather/cloud_coverage[0]`..`[2]` |
| 196 | f32[3] | cloud_type | `sim/weather/cloud_type[0]`..`[2]` (int, sent as float) |
| 208 | f32 | wave_amplitude | `sim/weather/wave_amplitude` |
| 212 | f32 | wave_length | `sim/weather/wave_length` |
| 216 | f32 | wave_speed | `sim/weather/wave_speed` |
| 220 | f32 | wave_dir | `sim/weather/wave_dir` |
| 224 | | end | |

Python: `struct.Struct("<4sHHIIQdddd" + "f"*18 + "BBBB" + "f"*23)` (224 bytes).

## 2. CIGI user-defined packet 201: Platform Kinematics (IG host → CamSim)

CIGI 3.3 has no fields for airspeed, magnetic heading, velocity or a sample time, so the
host sends them in a user-defined packet (opcodes 201–255 are reserved for that). CamSim
parses it raw, like Celestial, in the byte order declared by the datagram's IG Control
(Byte Swap Magic), so it is **big-endian** from a standard host.

- Opcode 201, packet size 48. Send it in every datagram that carries the camera's Entity
  Control, after it.
- Only applies when Entity ID = `camera_entity_id`; other IDs are ignored.
- A field whose valid bit is clear is ignored (CamSim keeps its own estimate or omits the
  KLV tag).

| Offset | Type | Field | Notes |
| --- | --- | --- | --- |
| 0 | u8 | opcode | 201 |
| 1 | u8 | size | 48 |
| 2 | u16 | entity_id | `camera_entity_id` |
| 4 | u8 | flags | bit0 airspeeds valid, bit1 magnetic heading valid, bit2 NED velocity valid, bit3 sample time valid |
| 5 | u8[3] | reserved | 0 |
| 8 | f32 | true_airspeed | m/s → KLV Tag 8 |
| 12 | f32 | indicated_airspeed | m/s → KLV Tag 9 (the host converts from knots) |
| 16 | f32 | magnetic_heading | deg, 0–360 → KLV Tag 64 |
| 20 | f32 | vel_north | m/s → KLV Tag 79 |
| 24 | f32 | vel_east | m/s → KLV Tag 80 |
| 28 | f32 | vel_down | m/s (not in KLV today) |
| 32 | f64 | sample_utc | Seconds since the Unix epoch (UTC) of the pose in this datagram, i.e. the predicted render time t_r. Used for KLV Tag 2 (gap 10) |
| 40 | u8[8] | reserved | 0 |
| 48 | | end | |
