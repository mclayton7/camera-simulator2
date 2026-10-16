# Control API (ROADMAP 2.4)

An HTTP/JSON API on CamSim's health port for **setting up a scene and capturing frames with their ground truth, one
call at a time, with no CIGI host**. The main use is generating ML/ATR datasets: a seeded script places vehicles,
picks a camera pose, time of day and weather, and gets back the image, a depth map and the frame's annotations
(boxes, oriented boxes, RLE masks, visibility, 3D boxes) — matched to the image by construction.

Design: `docs/superpowers/specs/2026-10-15-control-api-design.md`. Python client: `scripts/control/`.

## Enabling it

```bash
CAMSIM_CONTROL_API_ENABLED=1 \
CAMSIM_ML_ENABLED=1 CAMSIM_ML_INTERVAL_FRAMES=100000 \
scripts/run.sh --headless --local
```

- `control_api.enabled` (`CAMSIM_CONTROL_API_ENABLED`) is **off by default**: the API moves the camera for anyone who
  can reach the health port (bound on 0.0.0.0). It needs `operational.health_http_enabled` (on by default).
- Ground truth (`ml_training.enabled`) must be on for annotations and depth; without it a capture still returns the
  image, with `"annotation": null`. A capture frame is always annotated whatever `annotation_interval_frames` says, so
  a large interval (`CAMSIM_ML_INTERVAL_FRAMES=100000`) keeps `camsim_coco.jsonl` and `depth/` to the capture frames.
- Settings: `control_api.settle_load_pct`, `default_settle_frames`, `capture_timeout_s` (`docs/configuration.md`).
- Docker: the same env vars; the API is on the published health port.

`/ready` normally needs CIGI packets; with the control API it also goes true once the API has posed the camera
(and the encoder, sensor graph, first frame and terrain are ready, as before).

## Routes

All bodies are JSON. Partial updates: top-level fields and the camera's `gimbal` axes and `sensor` fields you omit
keep their value; but each `environment` sub-object (`atmosphere`, `clouds`, `sea`) replaces its whole group, with
omitted fields at their defaults (e.g. `{"clouds": {"base_m": 3000}}` means coverage 0, clouds off). Errors: `{"error": "..."}` with 400 (bad request: unknown field, wrong type, out of range),
404 (unknown entity), 409 (wrong state) or 503 (no camera yet, capture timed out). Heights are **WGS-84 ellipsoid**
metres; heading/pitch/roll are CIGI's (degrees, heading from true north). Omitted fields keep their value.

| Route | What |
|---|---|
| `GET /api/v1/status` | `ready`, `terrain_ready`, `frame`, `load_progress_pct`, `vegetation_idle`, `sensor_mode`, `ground_truth {masks, depth}`, `camera_owned`, `lockstep`, `utc`, `entities`, `capture_pending` |
| `GET` / `PUT /api/v1/clock` | `{"lockstep": true, "utc": "2026-06-15T19:00:00Z", "rate": 1}` (`rate` only outside lockstep: 409) |
| `POST /api/v1/clock/step` | `{"seconds": 0.5}` (0 < s ≤ 3600; lockstep only, else 409) |
| `GET` / `PUT` / `DELETE /api/v1/camera` | `{"lat", "lon", "alt"` (together), `"heading", "pitch", "roll", "gimbal": {"yaw", "pitch", "roll"}, "hfov_deg", "sensor": {"mode": "eo"\|"ir", "polarity": "white_hot"\|"black_hot", "on": true}}`; DELETE releases the camera to CIGI |
| `PUT` / `DELETE /api/v1/entities/{id}` | id 1..65535: `{"type_id", "lat", "lon", "alt", "heading", "pitch", "roll", "surface": "none"\|"ground"\|"water", "hidden", "components": [{"class", "id", "state"}]}`; a new entity needs `type_id` and the position |
| `GET` / `DELETE /api/v1/entities` | list (with the committed, clamped pose) / remove all API entities |
| `PUT /api/v1/environment` | `{"atmosphere": {"visibility_m", "humidity_pct", "air_temp_c", "wind_mps", "wind_dir_deg", "pressure_mb", "enabled"}, "clouds": {"coverage_pct", "base_m", "thickness_m", "type"}, "sea": {"beaufort", "direction_deg"}}` |
| `POST /api/v1/capture` | `{"settle_frames": 30, "timeout_s": 120, "depth": true}` (all optional) → the capture (below) |

Entity types are the `entity_types` of `camsim_config.yaml` (2001 truck, 3001 boat, …). Components are CIGI
Component Control's: 10 damage, 11 power plant, 12 flaming (thermal state, `docs/thermal.md`).

### Camera ownership

The first `PUT /camera` takes the camera: from then on CIGI camera-entity packets (pose, view, sensor, gimbal) are
drained and dropped, so a forgotten host doesn't fight the API. CIGI and DIS **entities** still render, and CIGI
environment packets (time, weather) still apply. `DELETE /camera` gives it back.

### Lockstep

`PUT /clock {"lockstep": true}` freezes sim time; only `POST /clock/step` moves it. The engine keeps rendering at
its fixed 30 fps (tiles stream, TSR and Lumen converge, video is still encoded): what freezes is everything on sim
time — sun, ocean waves and ripples, thermal model and entity thermal state, KLV time, and in lockstep also dead
reckoning and gimbal slews (outside lockstep they run on engine time, as before, so CIGI's "static time of day"
keeps entities moving). A step is applied before the world ticks, so every consumer sees it in the same frame; a
600 s step relaxes entity temperatures exactly as 600 one-second steps would. The sensor's AE/AGC adapts on frame
time in lockstep, so a capture is exposed for its own view.

### Capture

A capture waits until, on one tick: the terrain gate is open, every tileset is loaded to `settle_load_pct`, vegetation
streaming is idle, and no API command was applied; then `settle_frames` more rendered frames (any command restarts
the count). The next frame is the **capture frame**: annotated, its depth read back, and returned:

```json
{"frame_id": 220, "timestamp_us": 1781550000000000, "utc": "2026-06-15T19:00:00.000Z", "width": 1280, "height": 720,
 "sensor_mode": "eo", "image_png_b64": "...", "depth_png_b64": "...", "depth_far_plane_m": 5000,
 "annotation": {"frame_id": 220, "platform": {...}, "annotations": [{"source": "api", "source_id": "1",
   "category": {"id": 2001, "name": "truck"}, "bbox": [616, 654, 51, 40], "mask_source": "render",
   "segmentation": {"size": [720, 1280], "counts": "..."}, "obb": [...], "visibility": 1.0, ...}]},
 "settle": {"frames_waited": 41, "wall_ms": 3378}}
```

- The image is the sensor output (what is encoded, before compression), 8-bit PNG. The depth PNG is 16-bit:
  metres × 65535 / `depth_far_plane_m`. `annotation` is exactly the frame's `camsim_coco.jsonl` record
  (`docs/ground-truth.md` for the fields).
- One capture at a time (409). A capture that doesn't settle within `timeout_s` answers 503 with what it was
  waiting for ("tiles 97.2%", "terrain", …).

## Python client and datasets

```python
from camsim_control import Client

c = Client("http://127.0.0.1:8080")
c.wait_ready()
c.set_clock(lockstep=True, utc="2026-06-15T19:00:00Z")
c.put_entity(1, type_id=2001, lat=33.3073, lon=-117.3446, alt=0, heading=45, surface="ground")
c.set_camera(lat=33.3010, lon=-117.3446, alt=740, gimbal={"pitch": -45}, hfov_deg=10, sensor={"mode": "eo"})
cap = c.capture()
```

`scripts/control/make_dataset.py` writes a COCO dataset (`images/`, `depth/`, `annotations.json`: standard
`images`/`annotations`/`categories`, CamSim's extra fields under `camsim`) from a seeded sampler: per sample, 1–4
vehicles around an aim point, a camera at 300–2000 m slant range and 20–90° depression, 5–40° FOV, local solar time
07–17 h, visibility, clouds and sea state; each mode (EO, IR) captured on the same scene. Sample *i* depends only on
`(seed, i)`.

```bash
uv run --project scripts/control python scripts/control/make_dataset.py --out OUT --count 500 --seed 7 \
    --site 33.3072917,-117.344625 --ground-alt -11 --modes eo,ir --types 2001:ground
```

`--ground-alt` is the ellipsoid height of the ground at the site (`GET /api/v1/entities` shows a ground-clamped
entity's height; CIGI HOT gives it too).

## Acceptance

`scripts/control_api_check.py --scene PKG --out DIR` (gates C1–C8, spec §6): ready without CIGI, lockstep, record
pairing with rendered masks and depth, ground placement vs CIGI HOT, camera ownership, a 24-image dataset loaded by
pycocotools, determinism across two launches, throughput. Results: `ROADMAP.md` 2.4.

## Limits (v1)

- One client at a time; no authentication (keep the port private).
- No HAT/HOT or LOS over the API (use CIGI); no per-entity motion (pose each step yourself, or use DIS/CIGI).
- Images travel base64 in JSON (~4 MB per 1080p capture): fine on a LAN.
- **Repeatability:** the labels (boxes, masks, OBBs) of a seeded dataset are identical across launches. Images agree
  to ~0.6 DN on average (EO and IR; most images within 0.2 DN), but a single image can differ by up to ~3 DN, a global
  offset: the renderer carries state from earlier frames (TSR, Lumen, exposure history), so the same view rendered
  after a different history differs a little, even within one launch (REALISM R6 open item, `ROADMAP.md` 2.4). Pin the
  sensor noise and TSR jitter for pixel comparisons (`-ExecCmds="camsim.Sensor.FixedNoiseFrame 0,r.Test.FreezeTemporalSequences 1"`),
  as `control_api_check.py` does (it also turns the level's engine-time volumetric clouds off, `r.VolumetricCloud 0`).
- An entity far from the camera is clamped against the coarse tiles loaded there (it is re-submitted every tick, as a
  host's would be); its height settles to the fine tiles once the camera looks at it, which a capture waits for.
