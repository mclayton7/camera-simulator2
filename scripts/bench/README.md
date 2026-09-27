# Render benchmark (ROADMAP 3A)

Flies a deterministic CIGI scenario over San Francisco against a local CamSim
and records per-frame render stats plus lossless reference shots.

Phases: `warmup` (unmeasured, fills Cesium's disk cache), `orbit` (3 km),
`slew` (fast gimbal sweeps: pop-in and hitch stressor), `low_pass` (600 m at
100 m/s), `far_origin` (jump ~300 km east: origin shift and lighting).
Then about eight fixed shots via `GET /snapshot` (pre-sensor, lossless).

## Run

```bash
uv run --with numpy --with pillow python scripts/bench/run_bench.py --label mytest
uv run --with numpy --with pillow python scripts/bench/run_bench.py --label smoke --smoke
```

Flags: `--view-source primary|scene_capture`, `--skip-warmup` (only when the
cache is already warm), `--trace` (Unreal Insights `trace.utrace`), `--out DIR`.
Output (default `.cache/bench/<time>-<label>/`): `results.json`, `frames.jsonl`,
`phases.json`, `shots/*.png`, `slew.ts`.

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
- SSIM per shot is information only; TSR jitter and streaming order vary.

Needs a Cesium ion token (config) and network. Compare warm-cache runs only.
Run-to-run noise on the M1 Pro is recorded in ROADMAP 3A.
