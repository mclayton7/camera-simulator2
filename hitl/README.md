# IG host for the Hooter HITL rig

`camsim_hitl` is the CIGI host for CamSim on the Hooter hardware-in-the-loop rig
(design: [`HITL.md`](../HITL.md), wire formats: [`PROTOCOL.md`](PROTOCOL.md)). It:

- receives X-Plane 11 truth from the CamSimTruth plugin (UDP 49300, PROTOCOL.md §1);
- answers PX4 on the payload port as the **gimbal device** (Gimbal Protocol v2, compid 154)
  and the **camera** (Camera Protocol, the real camera's compid), stabilising the gimbal
  with X-Plane truth;
- drives CamSim with one CIGI 3.3 datagram per CamSim frame, paced by CamSim's Start of
  Frame and sent half a frame after it, with pose and gimbal predicted to the render time.

```
X-Plane + CamSimTruth ──UDP 49300──► truth ring (interpolate / extrapolate to t_r)
                                          │
PX4 payload port ◄──MAVLink──► link ──► gimbal device (plant thread, 250 Hz) ──q_BC(t_r)──┐
                                   └──► camera component (zoom, EO/IR, capture) ─optics──┤
                                                                                         ▼
CamSim ◄──CIGI UDP 8888── CIGI host (SOF-paced: IG Ctrl, Entity, 201, View Ctrl, ...) ◄──┘
CamSim ──UDP 8889──► SOF, HAT/HOT (102/103), Sensor Extended Response (107)
```

## Setup

Python ≥ 3.12 and [uv](https://docs.astral.sh/uv/). Dependencies: pymavlink and pyserial.

```bash
cp hitl/hitl.example.toml hitl/hitl.toml        # edit; every key is documented there
uv run --project hitl python -m camsim_hitl --config hitl/hitl.toml
uv run --project hitl python -m camsim_hitl --config hitl/hitl.toml --check-config   # print the resolved config
```

Options: `--log-level DEBUG`, `--camsim HOST:PORT` (overrides `cigi.camsim_host/port`).
Ctrl-C (or SIGTERM) shuts everything down cleanly.

The EGM96 grid is CamSim's `unreal_project/CamSimTest/Content/NonUFS/Geoid/WW15MGH.DAC`
(git LFS: `git lfs pull` if it is a 130-byte pointer). The host refuses to start without it
unless `geodesy.allow_missing_geoid = true`.

### CamSim configuration for the rig

From HITL.md "CIGI mapping to CamSim" (`deploy/camsim_config.yaml`, top level):

```yaml
camera_entity_id: 1            # = cigi.camera_entity_id
gimbal_max_slew_rate: 0.0      # snap: the host's plant owns the gimbal dynamics
gimbal_pitch_max: 90.0         # the host enforces the real limits
gimbal_pitch_min: -90.0        # default; past-nadir attitudes arrive as yaw+180/roll+180
gimbal_yaw_min: -180.0         # default; keep the full yaw range
gimbal_yaw_max: 180.0
sensor_fov_presets: []         # FOV comes from View Definition
cigi_response_addr: "<IG host IP>"   # SOF / HAT/HOT / 107 come back to cigi.response_listen
operational:
  snapshot_endpoint_enabled: true    # camera image capture uses GET /snapshot
```

The host sends View Control yaw within ±180° and Entity ID 0, so it works with or without
CamSim's yaw-wrap fix (HITL.md gap 1).

## Running

### Bench: no X-Plane, no PX4

```bash
# CamSim running locally (scripts/run.sh), then:
uv run --project hitl hitl/tools/fake_xplane.py orbit --lat 37.62 --lon -122.38 --alt 300 &
uv run --project hitl python -m camsim_hitl --config hitl/hitl.toml
```

`fake_xplane.py` sends PROTOCOL.md §1 datagrams at 60 Hz: `orbit` (coordinated turn round
`--lat/--lon` at `--radius`, `--speed`), `straight` (`--heading`, `--speed`, `--pitch`),
`parked` (`--alt` MSL, `--agl`). Weather is attached once a second; `--pause-at T
--pause-for S` exercises the pause handling. Without CamSim the host free-runs at
`cigi.free_run_rate_hz` and warns; without truth it sends no Entity Control (or the
`[truth.fallback]` pose if enabled).

### PX4 SITL (SIH)

PX4's SITL starts a Gimbal-mode MAVLink instance (binds 13030, sends to 127.0.0.1:13280)
and the GCS instance (sends to 14550). Point the shared payload link at the gimbal port and
put the camera on the same link (the SITL "onboard camera" instance is limited to 4000 B/s):

```toml
[mavlink.links.payload]
url = "udpin:0.0.0.0:13280"    # or udpout:127.0.0.1:13030
[gimbal]
enabled = true
link = "payload"
[camera]
enabled = true
link = "payload"
```

SITL parameters: `MNT_MODE_IN=4`, `MNT_MODE_OUT=2`. On the rig machine,
`/opt/mac/px4/run_sitl_gimbal.sh start|stop|status|cmd <px4 command>` starts PX4 v1.17 SIH
configured this way (it moves PX4's uXRCE-DDS port off 8888, CamSim's CIGI port).
Then drive it like QGC with `fake_gcs.py` on the GCS link:

```bash
uv run --project hitl hitl/tools/fake_gcs.py watch                     # gimbal/camera traffic
uv run --project hitl hitl/tools/fake_gcs.py pitchyaw -45 30           # roll+pitch locked, yaw vs heading
uv run --project hitl hitl/tools/fake_gcs.py pitchyaw -60 -170 --earth-yaw
uv run --project hitl hitl/tools/fake_gcs.py roi 47.397 8.545 0        # DO_SET_ROI_LOCATION
uv run --project hitl hitl/tools/fake_gcs.py zoom range 50             # or step 1 / continuous 1|0|-1
uv run --project hitl hitl/tools/fake_gcs.py source ir
uv run --project hitl hitl/tools/fake_gcs.py capture                   # IMAGE_START_CAPTURE
uv run --project hitl hitl/tools/fake_gcs.py video start | video stop
uv run --project hitl hitl/tools/fake_gcs.py info | params | setparam CAM_SOURCE 1
/opt/mac/px4/run_sitl_gimbal.sh cmd gimbal status
/opt/mac/px4/run_sitl_gimbal.sh cmd listener gimbal_device_attitude_status
```

`fake_gcs.py` takes primary control with `DO_GIMBAL_MANAGER_CONFIGURE` first (PX4 denies
`DO_GIMBAL_MANAGER_PITCHYAW` otherwise), and sends ROLL_LOCK|PITCH_LOCK like QGC.

In SITL the vehicle is PX4's SIH model while the gimbal is stabilised against
`fake_xplane`'s truth, so angles relative to the world only make sense if they match.
For protocol tests against PX4's own attitude set `gimbal.vehicle_attitude = "autopilot"`
(uses `AUTOPILOT_STATE_FOR_GIMBAL_DEVICE`; never on the rig: under HIL PX4 v1.16+ sends
identity there).

MAVSDK's tester plays the autopilot (stop PX4 first, it uses the same port):

```bash
# [mavlink] sysid = 33 (the tester's), gimbal.vehicle_attitude = "autopilot",
# gimbal.heartbeat_rate_hz = 20 (the tester samples the heartbeat type in a short window)
/opt/mac/px4/mavsdk/build-tester/gimbal_device_tester udpout://127.0.0.1:13280
```

It passes every test up to the rate-mode block with `yaw_limits_deg = [-170, 170]`. With
±180° limits its "Pan 180 right" fails on the ±180° sign ambiguity of its own setpoint, and
"Look forward first" fails because it reads the status quaternion as earth-framed while the
device (per the MAVLink spec and HITL.md) reports it heading-relative with
YAW_IN_VEHICLE_FRAME in follow mode.

### The rig (X-Plane 11 + XPlaneHILInterface + PX4 HITL)

1. Install the CamSimTruth plugin (`hitl/xplane_plugin/`), `dest =` this host's IP:49300.
2. Unplug the real gimbal and camera (PX4 and QGC would see two devices).
3. Wire the host's USB-serial adapter to the payload port (e.g. TELEM2) and set
   `[mavlink.links.payload] url = "serial:/dev/ttyUSB0:921600"`.
4. Copy the real devices' identities into `[gimbal]` / `[camera]` (vendor, model,
   firmware, camera `flags`, `cam_definition_version`, the real definition XML) and the
   real gimbal's joint order, limits, rates and lens curve (`[camera.zoom] hfov_table`).
5. PX4 parameters (HITL.md "PX4 configuration"):

| Parameter | Value | Why |
| --- | --- | --- |
| `MNT_MODE_IN` | 4 (MAVLink gimbal v2), or 0 (auto) | PX4 is the gimbal manager |
| `MNT_MODE_OUT` | 2 (MAVLink gimbal v2) | Setpoints go to this host's gimbal device |
| `MAV_1_CONFIG` | The real gimbal's port, e.g. TELEM2 | The host takes the payload's place |
| `MAV_1_MODE` | 10 (Gimbal), or the real port's mode | Gimbal mode streams setpoints at 20 Hz (Onboard: 5 Hz) |
| `MAV_1_FORWARD` | 1 | Forwards GCS commands and gimbal/camera replies |
| `SER_TEL2_BAUD` | e.g. 921600 | Match the adapter; leave room for HIL_ACTUATOR_CONTROLS (~19 kB/s) |
| `MAV_HB_FORW_EN` | 1 | Else the camera's heartbeat never reaches QGC |
| `TRIG_MODE`, `TRIG_INTERFACE` | Hooter's real values | Capture de-duplication handles TRIG_INTERFACE=3 |

6. CamSim: the configuration above, `cigi.camsim_host` = CamSim's address.

## Behaviour

**Timing.** On each SOF the host sets t_r = SOF + one (measured) frame period +
`cigi.render_offset_s` and sends at SOF + `send_phase` × period (default 0.5): measured on
localhost, exactly one datagram per SOF, 17.7 ms after it at 30 Hz (p5–p95 17.2–18.0 ms).
The IG Control timestamp is t_r in 10 µs ticks from a monotonic clock (strictly
increasing); Last Received IG Frame echoes the SOF's frame number.

**Truth.** Samples are ordered by the plugin's sequence number (a large backwards jump is a
plugin restart) and timed with the plugin's monotonic clock mapped onto the host's by a
running minimum of the offsets (`truth.use_sender_clock`), which removes network jitter.
Position is interpolated linearly and attitude slerped; past the newest sample the host
extrapolates with the OpenGL velocities rotated to NED (east, up, south → N = −vz, E = vx,
D = −vy) and the body rates, for at most `truth.max_extrapolation_s` (0.1 s), then holds.
Paused: the last pose, zero rates. Replay: `freeze` (the last live pose, default) or
`follow`. `sim_speed_actual_ogl` < 0.98 logs a time-dilation warning.

**Altitude.** Entity Control altitude = `elevation` + N_EGM96 (ellipsoid). Below
`terrain_blend.band_low_m` AGL it becomes CamSim's HOT + AGL, smoothly back above
`band_high_m`; HOT is polled with HAT/HOT extended requests (opcode 24 → 103) at
`hot_poll_hz` only inside 1.5 × the band, and the correction is low-passed so HOT updates
never step the camera. The lever arm `geodesy.lever_arm_m` (body FRD from X-Plane's
reference point) is rotated by the attitude and applied with WGS-84 radii M and N. Cloud
heights get N added too.

**Gimbal.** Setpoints follow HITL.md's yaw-frame rule (v1.18 YAW_IN_* flags, else the legacy
YAW_LOCK rule). Roll+pitch locked → horizon-referenced; no locks → body-referenced; a
single roll or pitch lock is handled per axis (locked axes horizon-referenced, the others
follow the airframe). q all NaN with finite rates is rate mode (integrated in the locked
frame); q and rates all NaN (PX4 v1.17's idle setpoint) holds the current attitude (roll
levelled when ROLL_LOCK); silence for `setpoint_timeout_s` holds and zeroes rates. The
plant decomposes the target into the real joint order, picks the Euler solution inside the
limits nearest the current joints, applies limits, rate, acceleration and a first-order
lag τ, and integrates at `plant_rate_hz` in its own thread. The CIGI host samples it at
t_r by running a copy of the plant forward with truth predicted to each sub-step. Status:
10 Hz (SET_MESSAGE_INTERVAL honoured), broadcast 0/0, locks in force + exactly one YAW_IN_*,
q earth- or heading-relative, AT_*_LIMIT failure flags, delta_yaw = heading. Heartbeats go
out immediately at start (PX4 forwards to a link only after seeing the component there).
View Control angles are the yaw-pitch-roll decomposition of q_BC, continuous through nadir
(yaw held, roll takes the rest), yaw within ±180°.

**Camera.** Answers REQUEST_MESSAGE and the superseded 521/522/525/527/2504/2505, ACKs
every command before doing the work, ignores broadcast requests for messages it does not
serve (so it never answers GIMBAL_DEVICE_INFORMATION). Zoom (STEP, CONTINUOUS, RANGE;
FOCAL_LENGTH / HORIZONTAL_FOV if enabled) → HFOV → View Definition, VFOV =
2·atan(tan(HFOV/2)·H/W); SET_CAMERA_SOURCE or a `role = "source"` definition parameter →
Sensor Control ID (0 EO, 1 IR); `role = "polarity"` → Polarity. Image capture fetches
`GET /snapshot`, stores `captures/IMG_nnnnn.png`, serves it at
`http://advertise_host:http_port/captures/…` and sends CAMERA_IMAGE_CAPTURED with the
camera's MSL position and NED attitude; one photo per IMAGE_START_CAPTURE sequence number,
per DO_DIGICAM_CONTROL command identity, and per `dedup_window_s` across trigger sources.
Video capture records CamSim's raw MPEG-TS UDP to `videos/VID_*.ts` (no ffmpeg).
CAMERA_FOV_STATUS image centre = CamSim's Sensor Extended Response track point (one frame
old) converted to MSL, INT32_MIN when the host's own boresight is at or above the horizon,
INT32_MAX before any pose/centre.

**Weather and time.** Atmosphere + Weather (+ Wave Control) together every
`weather.period_s` once the plugin's weather block arrives: the dominant layer (lowest at
≥ 50 %, else the highest coverage), base + N, Atmosphere and Weather visibility equal.
Celestial: on system time Date/Time Valid 0 and Ephemeris 1 (0 while paused or replaying);
after a pause, the next minute rollover re-sets CamSim's clock with Date/Time Valid 1
(`time.resync_after_pause`). Scenario time (`time.source = "xplane"`) uses HITL.md's
`xp11_utc` with the local year and starts sending Date/Time Valid 1 at X-Plane's first
minute rollover. Packet 201 carries TAS, IAS (knots → m/s), magnetic heading, NED velocity
and the UTC of t_r.

## Tests

```bash
uv run --project hitl --with pytest pytest hitl/tests
```

Quaternion/frame maths (incl. the HITL.md sign case), packer byte layouts against CCL's
`Pack()` and PROTOCOL.md, a decode of the host's datagram by CCL itself
(`tests/ccl_harness.cpp`, built from `.build_tmp/ccl` with g++ when present, else
skipped), truth parsing/interpolation/extrapolation, geoid values, lever arm,
`xp11_utc` (incl. New Year), the gimbal over a localhost UDP MAVLink link, the camera
flows, and an end-to-end localhost run (fake_xplane → host → fake CamSim emitting SOF at
30 Hz).

## Known gaps

- PX4 v1.17 drops `roll_min/roll_max` when copying GIMBAL_DEVICE_INFORMATION into
  GIMBAL_MANAGER_INFORMATION (reports 0/0): a PX4 bug, nothing to do here.
- Not yet run against live CamSim, X-Plane or the flight controller's serial port.
- Unverified X-Plane semantics are configuration switches: `y_agl` reference
  (`terrain_blend.y_agl_offset_m` / `agl_source`), `cloud_coverage` scale,
  `wave_amplitude` (half or full height), `wave_dir` (from or toward).
- CAMERA_TRACK_* are answered UNSUPPORTED; no defocus model (SET_CAMERA_FOCUS is stored
  and reported only).
