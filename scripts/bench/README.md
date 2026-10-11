# Render benchmark (ROADMAP 3A)

Flies a deterministic CIGI scenario over San Francisco against a local CamSim
and records per-frame render stats plus lossless reference shots.

Phases: `warmup` (unmeasured, fills Cesium's disk cache), `orbit` (3 km),
`slew` (fast gimbal sweeps: pop-in and hitch stressor), `low_pass` (600 m at
100 m/s), `far_origin` (jump ~300 km east: origin shift and lighting).
`--site pendleton` moves the base to Camp Pendleton and appends a measured
`coast_pass` (300 m, 100 m/s along the coast, gimbal toward the shore; the warmup
grows to 150 s to fly it once). Results record `meta.site`; `compare.py` refuses
to compare runs from different sites.
`--site pendleton` also appends `dense_pass` (REALISM R3/R4 bench): 100 m/s for 90 s at the start point's terrain
+ 300 m HAE, looking ahead 20 deg down, along the path in `dense_path.json` over built-up land and tree cover. The
warm-up grows to 180 s to fly it once, and the shots gain `dense_oblique` (the path's midpoint). The path comes from
`select_dense_path.py PKG` (rule in its docstring; built-up land stands in for buildings, so it does not depend on the
buildings layer): `uv run --project scripts/scene python scripts/bench/select_dense_path.py PKG`.
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
`--site {sf,pendleton}` (default `sf`), `--env KEY=VALUE` (extra CamSim environment, repeatable; e.g.
`CAMSIM_TRACK_PIPELINE_LATENCY=1` records the pipeline latency quantiles
into `meta.latency`, `CAMSIM_ENCODER=libx264`).
`--phases A,B` (measured phases to fly; the warm-up still runs), `--shots A,B`, `--gpu-mem` (samples GPU memory every
5 s into `gpu_mem_mib_max` per phase: `nvidia-smi memory.used` on Linux, `footprint -p` on macOS; `null` where neither
works, e.g. macOS Docker; `meta.gpu_mem_source` says which).

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

## Realism vs cost sweep (REALISM R3/R4)

    uv run --with numpy --with pillow python scripts/bench/sweep.py --scene "$PWD/.cache/scene-packages/<pkg>" \
        --out .cache/bench-r3r4/<name> --axis buildings=off,on --axis buildings_sse=8,16,32,64 --repeat 2

One `run_bench.py` per row and repeat (`dense_pass` only, plus the warm-up, with `--gpu-mem`; ~5 min each). Axes:
`buildings`, `buildings_sse`, `buildings_physics`, `trees`, `trees_radius`, `trees_density`, `trees_shadows` (switches
take `on`/`off`; an axis of a feature that is not `on` in the row is dropped from it, and duplicates collapse). Both
master switches (`CAMSIM_SCENE_BUILDINGS_ENABLED`, `CAMSIM_VEGETATION_ENABLED`) are sent on every row, `0` unless the
row names the feature `on`, because both default on at runtime; the baseline row is the one with everything off.
Output: `sweep.csv` (one line per run), `sweep.md` (ranges per row; each row's median is judged against the baseline
row's, using the budget in `docs/superpowers/specs/2026-10-10-r3r4-realism-cost-bench-design.md` section 4),
`sweep_sheet.png` + `shots/<row>.png` (the `dense_oblique` shot per row). A crashed run or an empty phase marks the row
`failed`. Use one-factor sweeps around a candidate default plus the both-on row; the full product is hours.

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
- `game_ms_p95`: game-thread p95 per phase (the budget's tail metric).
- `gpu_mem_mib_max`: peak GPU memory in a phase, only with `--gpu-mem`. On Linux it is `nvidia-smi` whole-GPU
  `memory.used`. On macOS (unified memory) it is the whole-process footprint, not VRAM: the `IOAccelerator` rows plus
  every `(graphics)` row of `footprint`, so CPU-side tile and physics data count too.
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

## Gimbal-snap test

```bash
uv run --with pillow python scripts/bench/snap_test.py FRAMES_JSONL OUT
```

Against a running CamSim with frame stats and `/snapshot` on: settles over SF, snaps the gimbal
90° (both ways) and saves snapshots at +0.3/1/3 s with their sharpness. Used to choose
`frustum_culling` / `maximum_screen_space_error` (`docs/configuration.md`, Cesium Tile Streaming).

## GPU memory (`--gpu-mem`)

Opt-in. Linux samples `nvidia-smi memory.used` (whole GPU, sum over GPUs). macOS samples
`footprint -p PID` every 5 s and sums the Dirty size of every `IOAccelerator*` row plus every
row whose category ends in `(graphics)`, each row once (footprint's `(graphics)` annotation is
disjoint from the other rows). Each phase reports `gpu_mem_mib_max`; `null` when unavailable.
