# ROADMAP 2.4: Control API with lockstep capture (design)

Date: 2026-10-15. Status: the user chose 2.4 as the next item and delegated the design choices ("Use your
recommendations and see it through to the finish line"). The choices below are the recommended ones.

## 1. Goal

A program can set up a scene, pose the camera, step the clock and capture a frame with its ground truth, one call at a
time, with no CIGI host. The first user is ML dataset generation: a seeded Python script that writes a COCO dataset
(EO and IR images, instance masks, boxes, depth) from a scene package. This replaces what the 2026-10-01 trim removed
(scenario engine, randomizer, `batch_run.py`).

Success:

- A seeded generator produces a COCO dataset of N captures at Camp Pendleton (or SF without a package) with the
  ground truth matched to each image by construction, not by guessing which frame came next (as
  `gt_occlusion_check.py` does today).
- With the clock in lockstep, sim time moves only when the client steps it. Two runs with the same seed give the same
  annotations and images that match to R6's measured run-to-run level.
- No CIGI host is needed: `/ready` goes true once the API has posed the camera.

Non-goals (v1): gRPC, streaming sequences of frames per call, a CIGI query (HOT/LOS) over the API, multiple
clients/sessions, authentication, ROS 2 (5.3, builds on this later).

## 2. Decisions

| # | Decision | Why |
|---|---|---|
| D1 | **HTTP + JSON** under `/api/v1/` on the existing health server (`FHttpServerModule`, port 8080), not gRPC. | gRPC means building grpc + protobuf as ThirdParty static libs for Linux, macOS and Docker, with protobuf clashes against UE's own copy. The HTTP server is already in-process, its handlers run on the game thread (so no new threading), and `Json` is already a module dependency. A Python client needs only the standard library. A gRPC gateway can sit in front later if wanted. |
| D2 | Off by default: `control_api.enabled` (`CAMSIM_CONTROL_API_ENABLED`). Needs the health server. | It moves the camera for anyone who can reach port 8080 (bound on 0.0.0.0). |
| D3 | **Lockstep = sim time gated, engine not paused.** The engine keeps ticking at its fixed 30 fps: Cesium streams, TSR and Lumen converge, frames are rendered and encoded. What runs on sim time freezes until stepped. | Pausing the UE world stops tile streaming and temporal history too, so nothing could settle. All sim-state consumers that matter already read `FSimClock` (ocean, thermal, entity thermal, sun, KLV). |
| D4 | In lockstep, dead reckoning and gimbal slew use the **sim delta** (`FSimDelta`, §4.2); in other modes they keep using the engine delta. | 2.1 lists them as still on `DeltaTime`. A frozen clock in real-time mode (CIGI Ephemeris off = "fixed sun") must keep entities moving, so the switch applies to lockstep only. |
| D5 | In lockstep the sensor AE/AGC adapts on **frame time** (1/`frame_rate` per rendered frame), not sim time. | Today they hold their first snap on a frozen clock (R6 open item). A capture moves the camera to a new view and must expose for it. The settle frame count is fixed, so this stays deterministic. Outside lockstep nothing changes. |
| D6 | **Capture is one asynchronous call** that waits for the scene to settle, forces the next captured frame to be annotated, and returns everything **inline**: PNG image, 16-bit depth PNG (base64), and that frame's annotation record. CamSim writes nothing extra to disk. | Ties image and labels together by construction. Works the same native, in Docker or remote, with no shared filesystem. A 1080p PNG is ~3 MB; fine on a LAN. The client lays out the dataset. |
| D7 | Canonical commands for everything the API drives: camera pose → new rig entry, `FViewCommand`, `FSensorCommand`, `FTimeCommand`, `FAtmosphereCommand`, `FWeatherCommand`, `FOceanWaveCommand` gain consumers (today only produced). | 2.3's rule: the renderer never sees protocol types. The CIGI path keeps its current code for now; moving it onto the same consumers is a follow-up. |
| D8 | **Camera ownership:** once the API sets the camera, CIGI camera-entity frames (pose, view, sensor) are drained and dropped until the API releases it (`DELETE /api/v1/camera`). CIGI and DIS entities still render. | A forgotten bench host at 30 Hz would otherwise fight the API every frame. |
| D9 | API entities live in their own namespace (`EHostSource::ControlApi`, "api:N"). The adapter **re-submits each entity's last state every tick**, like a host at frame rate. | A ground-clamped entity is clamped at each pose commit against loaded tiles; a single commit before the tiles load would leave it floating. |
| D10 | The randomizer and COCO assembly live in the **Python client**, not in C++. | Easier to change per dataset; C++ stays a deterministic executor. |

## 3. API

All bodies are JSON. Errors return `{"error": "<message>"}` with 400 (bad request), 404 (unknown entity), 409 (wrong
state, e.g. step outside lockstep) or 503 (capture timed out, sensor graph unavailable). Angles are degrees, heights
WGS-84 ellipsoid metres (as everywhere in CamSim), heading/pitch/roll are CIGI's (north-referenced, `CigiToNeu`).
Every field in a `PUT` body is optional except where marked; omitted fields keep their value.

### 3.1 Status

`GET /api/v1/status` →
`{"ready": bool, "camera_owned": bool, "lockstep": bool, "utc": "2026-10-15T18:00:00.000Z", "frame": N,
"load_progress_pct": 99.5, "vegetation_idle": bool, "sensor_mode": "eo"|"ir", "ground_truth": {"masks": bool,
"depth": bool}, "entities": K}`

### 3.2 Clock

- `PUT /api/v1/clock` `{"lockstep": true, "utc": "2026-07-01T19:30:00Z", "rate": 1.0}`: `lockstep` →
  `FSimClock::SetLockstep`; `utc` → `SetUtc`; `rate` → `SetRate` (real-time mode only; 409 with lockstep).
- `POST /api/v1/clock/step` `{"seconds": 0.5}` (required, 0 < s ≤ 3600): `FSimClock::Step`. 409 unless lockstep.
  Returns the new UTC. The step is applied in the handler, i.e. on the core ticker before the world ticks, so every
  consumer sees it in the same frame.

### 3.3 Camera

`PUT /api/v1/camera`:

```json
{"lat": 33.3, "lon": -117.4, "alt": 900.0, "heading": 30, "pitch": 0, "roll": 0,
 "gimbal": {"yaw": 0, "pitch": -45, "roll": 0},
 "hfov_deg": 20.0,
 "sensor": {"mode": "ir", "polarity": "white_hot"}}
```

- Pose (all of `lat`, `lon`, `alt` together, or none) → `FCamSimPlatformRig::SetPlatformPose(FGeoPose)` (the
  existing private `ApplyPose`, made a public entry; detaches the platform from any CIGI attach parent).
- `gimbal` → `FViewCommand{Gimbal = Snap}` → `UCamSimGimbalComponent::ApplyView` (the body of today's
  `ApplyViewControl`: unwind, clamp to the envelope, cancel any slew).
- `hfov_deg` → `FViewCommand::HFovDeg` → the View Definition clamp, shared.
- `sensor` → `FSensorCommand` → `UCamSimSensorComponent::ApplySensor` (the waveband/polarity/on part of
  `ApplySensorControl`). Zoom presets are not exposed (`hfov_deg` covers it).
- First call takes camera ownership (D8). `DELETE /api/v1/camera` releases it.

`GET /api/v1/camera` returns the current pose, gimbal, FOV and sensor in the same shape.

### 3.4 Entities

- `PUT /api/v1/entities/{id}` (`id` 1..65535):
  `{"type_id": 2001, "lat": .., "lon": .., "alt": .., "heading": .., "pitch": 0, "roll": 0,
  "surface": "ground"|"water"|"none", "hidden": false, "components": [{"class": 0, "id": 11, "state": 1}]}`.
  `type_id` and the position are required when the entity is new. `surface` defaults to `none`. Components go through
  `FComponentCommand` (4C thermal: damage 10, power plant 11, flaming 12).
- `DELETE /api/v1/entities/{id}`; `DELETE /api/v1/entities` removes all API entities.
- `GET /api/v1/entities` lists API entities with their committed pose (after clamping).

### 3.5 Environment

`PUT /api/v1/environment`:

```json
{"atmosphere": {"visibility_m": 20000, "humidity_pct": 40, "air_temp_c": 22, "wind_mps": 4, "wind_dir_deg": 270},
 "clouds": {"coverage_pct": 40, "base_m": 2000, "thickness_m": 500, "type": 1},
 "sea": {"beaufort": 3, "direction_deg": 225}}
```

`atmosphere` → `FAtmosphereCommand` → new `ACamSimEnvironment::Apply(const FAtmosphereCommand&)`; `clouds` → one
global `FWeatherCommand` layer → `Apply(const FWeatherCommand&)`; `sea` → the ocean config path
(`UCamSimSubsystem::ApplyOceanConfig` with Beaufort and direction). Time of day goes through `/clock`.

### 3.6 Capture

`POST /api/v1/capture` `{"settle_frames": 30, "timeout_s": 120, "depth": true}` (all optional) → after settling:

```json
{"frame_id": 1234, "utc": "...", "timestamp_us": ..., "width": 1280, "height": 720,
 "sensor_mode": "eo", "image_png_b64": "...", "depth_png_b64": "..." ,
 "depth_far_plane_m": 5000,
 "annotation": { ...the frame's camsim_coco.jsonl record: frame_id, timestamp_us, platform, annotations[]... },
 "settle": {"frames_waited": 52, "wall_ms": 1733}}
```

Settling (`FControlCaptureGate`, a pure state machine, testable without UE): the request is admitted, then waits
until all of these hold on one tick — terrain gate open, minimum tileset load progress ≥
`control_api.settle_load_pct` (99.9), vegetation `IsIdle()`, no camera or entity command applied this tick — and then
counts `settle_frames` further rendered frames (any command resets the count). The next `Capture()` after that is
the **capture frame**: annotated regardless of `annotation_interval_frames`, its depth read back if depth is
available and requested, and tagged with the request. On delivery the NV12 is copied into the job and converted to PNG in its own task, depth
encoded to 16-bit PNG, and the collector's record for that frame is returned. One capture in flight at a time (409
otherwise). Timeout → 503 with what was still not settled.

Without masks (`ml_training` off or no instance-ID pass) `annotation.annotations` holds projected boxes only, as in
the COCO JSONL today, and `status.ground_truth` says so. Without depth, `depth_png_b64` is absent.

## 4. CamSim internals

### 4.1 New and changed units

| Unit | Kind | Purpose |
|---|---|---|
| `Control/ControlApiJson.h/.cpp` | pure | Parse request JSON into canonical commands + validation errors; build response JSON. All unit tested. |
| `Control/ControlApiAdapter.h/.cpp` | game thread | Host adapter: holds API entities' last commands, camera ownership, pending camera/env commands; `Poll(ISimCommandSink&)` re-submits entities each tick (D9). |
| `Control/ControlCaptureGate.h/.cpp` | pure | Capture settle state machine (§3.6). |
| `Control/ControlApiService.h/.cpp` | game thread | Binds the `/api/v1` routes on the health server, owns adapter + gate, completes capture callbacks from its tick. |
| `Time/SimDelta.h` | pure | `FSimDelta::Next(EngineDeltaSec)`: lockstep → clock delta since the last call (never negative); otherwise the engine delta. One instance per consumer, so call order within the frame doesn't matter. |
| `FCamSimHealthServer` | changed | Generic `BindRoute(Path, Verbs, Handler)` (the snapshot binding becomes a use of it); `/ready`'s CIGI condition becomes "CIGI packets received **or** the API owns the camera". |
| `FCamSimPlatformRig` | changed | Public `SetPlatformPose(const FGeoPose&)`. |
| `UCamSimGimbalComponent` / `UCamSimSensorComponent` | changed | `ApplyView(const FViewCommand&)`, `ApplySensor(const FSensorCommand&)`; the CIGI paths call the same internals. |
| `ACamSimCamera` | changed | Drops CIGI camera frames while the API owns the camera (D8); exposes FOV clamp for both sources. |
| `ACamSimEnvironment` | changed | `Apply(const FAtmosphereCommand&)`, `Apply(const FWeatherCommand&)` (converted into the existing CIGI-state fold for now). |
| `UCamSimCaptureComponent` | changed | `RequestCaptureFrame(TSharedRef<FControlCaptureJob>)`: forces annotation + depth on the next captured frame and carries the job through `SubmitFrameToEncoder`, which fills it (PNG, depth PNG, record) on the task thread. AE/AGC step on frame time in lockstep (D5). |
| `FCocoAnnotationWriter` / `FDepthMapWriter` | changed | Expose `BuildFrameRecord(...) → FString` and `EncodeDepthPng(...) → TArray<uint8>` so the capture job reuses exactly what the files contain. |
| `ACamSimEntity` / gimbal slew | changed | Dead reckoning and slew take `FSimDelta` (D4). |

`FCamSimEntityManager::Tick` polls the API adapter after DIS and before `ApplyHostPlatformState`, so API commands sit
in the same ordered pass as the other hosts.

### 4.2 Config

```yaml
control_api:
  enabled: false            # CAMSIM_CONTROL_API_ENABLED
  settle_load_pct: 99.9     # CAMSIM_CONTROL_API_SETTLE_LOAD_PCT
  default_settle_frames: 30 # CAMSIM_CONTROL_API_SETTLE_FRAMES
  capture_timeout_s: 120    # CAMSIM_CONTROL_API_CAPTURE_TIMEOUT_S
```

Documented in `docs/configuration.md`; `Config.CanonicalConfigHasNoUnknownKeys` covers the new section.

## 5. Python client and generator (`scripts/control/`, uv project `camsim_control`)

- `camsim_control/client.py`: `Client(base="http://127.0.0.1:8080")` with `status()`, `set_clock()`, `step()`,
  `set_camera()`, `release_camera()`, `put_entity()`, `delete_entity()`, `clear_entities()`, `set_environment()`,
  `capture() -> Capture` (decoded PNG bytes + record). Standard library only (`urllib`, `json`, `base64`).
- `camsim_control/coco.py`: `DatasetWriter(out_dir)`: `images/NNNNNN_{eo,ir}.png`, `depth/NNNNNN.png`,
  `annotations.json` in standard COCO (`images`, `annotations` with RLE segmentation, bbox, area, `iscrowd 0`,
  CamSim extras under `camsim`: obb, visibility, box3d, geo; `categories` from the entity types' `class_name`).
- `camsim_control/randomize.py`: seeded sampler (`random.Random(seed)`) for camera (range, depression, heading,
  FOV), time of day, visibility, clouds, sea state, and entity placement around an aim point (type, heading, offset,
  surface). Every draw comes from the sample index's own `Random(f"{seed}:{i}")`, so sample *i* doesn't depend on
  the ones before it.
- `make_dataset.py --out DIR --count N --seed S --site lat,lon [--modes eo,ir] [--types 2001,3001]`: lockstep on,
  per sample: clear entities, place, set environment and clock, pose camera, capture per mode, write.
- Tests: `uv run --project scripts/control --with pytest pytest scripts/control/tests` (no CamSim: a fake HTTP
  server for the client, COCO conversion against a recorded record, randomizer determinism).

## 6. Acceptance: `scripts/control_api_check.py --out DIR [--scene PKG]`

Launches CamSim (`camsim_session` with `CAMSIM_CONTROL_API_ENABLED=1`, ML on, **no CIGI host**), then:

| Gate | Check |
|---|---|
| C1 ready | `/ready` goes true with no CIGI traffic, after the first `PUT /camera`. |
| C2 lockstep | With lockstep on, `utc` is unchanged over 3 s of wall time; `step 10` moves the next capture's `timestamp_us` by exactly 10 000 000. |
| C3 pairing | A truck (2001, ground) and a boat (3001, water) placed in view: each capture's record contains them with masks (`mask_source` instance), the boxes lie inside the image, and the depth PNG decodes to the image size. |
| C4 placement | The truck's committed pose (GET entities) is on the surface: its height equals a CIGI HOT at its position within 0.1 m. |
| C5 ownership | A CIGI host sending a different camera pose at 30 Hz doesn't move the camera while the API owns it (KLV / `GET /camera` unchanged); after release it does. |
| C6 dataset | `make_dataset.py --count 12 --seed 7 --modes eo,ir` writes a COCO file that `pycocotools` loads; every annotation's RLE decodes; image count = 24. |
| C7 determinism | Two launches, same seed, 6 samples: annotation boxes within 1 px and masks IoU ≥ 0.98 per object; image mean abs diff ≤ 1 DN (R6 measured 0.11–0.38 DN with clouds on). |
| C8 throughput | Report captures/minute and settle wall time (informational; no gate). |

C++ tests (NullRHI, `CamSim.ControlApi.*`): JSON parsing/validation per route, the adapter (namespace, re-submit,
ownership), the capture gate state machine, `FSimDelta`, `/ready` with API ownership, routes over HTTP (the two-ticker
pattern), dead reckoning on sim time in lockstep, the COCO record builder unchanged against a golden line.

## 7. Risks

- **Settling detection misses something** (e.g. Lumen still converging). `settle_frames` is the knob; C7 measures
  what's left.
- **Large step + entity thermal**: `EntityThermal` relaxes each temperature by `1 - exp(-dt/tau)` toward a target
  that is constant over the step, so one 600 s step equals many small ones; a step over its `MaxStepS` (3600 s)
  snaps to the targets, which is the steady state anyway. The 3600 s step limit in §3.2 matches it.
- **PNG encode cost** (~30–60 ms at 1080p): the capture job's NV12 → PNG and depth PNG run in their own task after
  the ground-truth write, as the depth PNGs already do, so they never hold `bSensorBusy`.
