# Render benchmark (ROADMAP 3A)

Flies a deterministic CIGI scenario over San Francisco against a local CamSim
and records per-frame render stats plus lossless reference shots.

Phases: `warmup` (unmeasured, fills Cesium's disk cache), `orbit` (3 km),
`slew` (fast gimbal sweeps: pop-in and hitch stressor), `low_pass` (600 m at
100 m/s), `far_origin` (jump ~300 km east: origin shift and lighting).
Then 12 fixed shots via `GET /snapshot`: the 8 base
poses, an EO `night_slant`, and an IR variant of `nadir_3km`, `dusk_slant`,
`night_slant` (`Pose.sensor_id`: 0 EO, 1 IR, sent every frame via CIGI
Sensor Control). Each shot is also fetched via `GET /snapshot/sensor`
(ROADMAP 3B) as `<shot>_sensor.png`. Since 3B.2 both endpoints serve the
same image: the sensor graph's NV12 output (what is encoded, before
compression) converted to PNG — there is no pre-sensor frame.

## Run

```bash
uv run --with numpy --with pillow python scripts/bench/run_bench.py --label mytest
uv run --with numpy --with pillow python scripts/bench/run_bench.py --label smoke --smoke
```

Flags: `--skip-warmup` (only when the cache is
already warm), `--trace` (Unreal Insights `trace.utrace`), `--out DIR`,
`--env KEY=VALUE` (extra CamSim environment, repeatable; e.g.
`CAMSIM_TRACK_PIPELINE_LATENCY=1` records the pipeline latency quantiles
into `meta.latency`, `CAMSIM_ENCODER=libx264`).

### Against the Docker image

```bash
uv run --with numpy --with pillow python scripts/bench/run_bench.py --label docker \
  --docker camsim:latest [--env CAMSIM_ENCODER=libx264]
```

Runs the image (`docs/docker.md`) with `--gpus all`, host networking and the
output directory mounted at `/bench`; named volumes `camsim-bench-data` and
`camsim-bench-cache` keep the Cesium tile cache and driver shader cache warm
between runs. Adds `container.log` to the output and `meta.runtime`, `meta.gpu`,
`meta.encoder`, `meta.ready_s` to `results.json`. `--config FILE` mounts a
`camsim_config.yaml` over the image's (capture size has no env override, so a 1080p run
uses a copy with `capture_width: 1920`, `capture_height: 1080`). `--sensor ir` flies the
measured phases in IR (default `eo`; the shots keep their own waveband). `--no-gpu` runs it without
the GPU, which currently fails (ROADMAP 1.15: lavapipe can't run UE 5.8).
Output (default `.cache/bench/<time>-<label>/`): `results.json` (`meta.sensor_path`
is what CamSim's `/metrics` reported: `gpu` — the GPU sensor graph is the only path since 3B.2), `frames.jsonl`, `phases.json`,
`shots/*.png` + `shots/*_sensor.png`, `slew.ts`.

## Compare

```bash
uv run --with numpy --with pillow python scripts/bench/compare.py BASE_DIR CURRENT_DIR
```

Stored results: `baselines/<platform>-<gpu>-<label>.json` and
`shots/<platform>/<label>/` (git LFS).

## Reading it

- `families_mean`: scene renders per frame. 1.0 = one render; more means the
  viewport and a capture both render the world.
- `emitted_fps`: frames actually handed to the encoder per second.
- `hitches_66` / `hitches_100`: frames over 66.7 ms / 100 ms (wall clock;
  the engine's fixed frame rate makes `DeltaTime` useless for this).
- `popin_fraction`: share of frames drawn while a tileset was still loading.
- `sensor_gpu_ms_p50`/`p95`: GPU sensor model cost per frame; only present
  when the phase has measured rows (`sensor_gpu_ms >= 0`); absent when the
  sensor graph never ran (e.g. runs from before 3B, or no GPU timing).
- SSIM per shot is information only; TSR jitter and streaming order vary.

Needs a Cesium ion token (config) and network. Compare warm-cache runs only.
Run-to-run noise on the M1 Pro is recorded in ROADMAP 3A.

## Entity load scaling

```bash
CONFIG=my_1080p.yaml scripts/bench/load_scale.sh [OUT]   # ~15 min
python3 scripts/bench/load_report.py [OUT]
```

Runs the Docker image at ~0, 100, 250 and 500 CIGI entities
(`scripts/stress_entity_rendering.py`, which also flies the camera), then 500
with ML ground truth on, with and without the depth map. Each point is a fresh
container, 75 s recorded, frames 25–75 s summarised: thread times, fps, drops,
frame latency and annotated entities per frame. Results: ROADMAP 3B exit check.
