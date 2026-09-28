# Render benchmark (ROADMAP 3A)

Flies a deterministic CIGI scenario over San Francisco against a local CamSim
and records per-frame render stats plus lossless reference shots.

Phases: `warmup` (unmeasured, fills Cesium's disk cache), `orbit` (3 km),
`slew` (fast gimbal sweeps: pop-in and hitch stressor), `low_pass` (600 m at
100 m/s), `far_origin` (jump ~300 km east: origin shift and lighting).
Then 12 fixed shots via `GET /snapshot` (pre-sensor, lossless): the 8 base
poses, an EO `night_slant`, and an IR variant of `nadir_3km`, `dusk_slant`,
`night_slant` (`Pose.sensor_id`: 0 EO, 1 IR, sent every frame via CIGI
Sensor Control). Each shot is also fetched via `GET /snapshot/sensor`
(ROADMAP 3B) as `<shot>_sensor.png` — the post-sensor-model image; on the
legacy path it's the same as `/snapshot`.

## Run

```bash
uv run --with numpy --with pillow python scripts/bench/run_bench.py --label mytest
uv run --with numpy --with pillow python scripts/bench/run_bench.py --label smoke --smoke
```

Flags: `--skip-warmup` (only when the cache is
already warm), `--trace` (Unreal Insights `trace.utrace`), `--out DIR`.
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
