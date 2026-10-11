# Realism vs cost bench for buildings and trees (REALISM R3/R4 slices) — design

2026-10-10. Bench tooling only (`scripts/bench/`). Shared by the two thin-slice specs:
`2026-10-10-r3-buildings-slice-design.md` and `2026-10-10-r4-trees-slice-design.md`.

## Goal

Buildings and trees buy realism with frame time, VRAM and game-thread time. The user's concern (2026-10-10) is the trade
between the two, so the slices are designed to *measure* it rather than to hit one fixed setting: every realism knob is a
config key, the bench sweeps the knobs over a worst-case path, and the defaults are picked from the resulting table and
shots. If no "on" setting fits the budget, that is the finding, and it decides what the deepening pass works on.

## Findings this rests on

- **Today's Pendleton phases would not see the cost.** The site centre (33.30, −117.45) is the empty interior of the
  base; `orbit`, `slew` and `far_origin` fly there at 3,000 m, `low_pass` at 600 m, and `coast_pass` is mostly beach.
  A "within noise" result there says nothing about buildings or trees.
- **`results.json` has no game-thread p95** (R1 used wall p95 as the tail check). Tree cells built on the game thread
  would show as spikes, not in p50.
- Baseline (R2, M1 Pro / Metal, `run_bench.py --site pendleton`): EO game-thread p50 2.2–3.5 ms by phase, GPU frame p50
  17.7–21.5 ms, wall p95 33.8–41.0 ms, 30 fps and 0 dropped frames in every phase (`docs/scene-packages.md`, "Measured").

## Design

### 1. `dense_pass` phase (Pendleton site)

A new measured phase, appended after `coast_pass` when `site.coast` (Pendleton) and also visited once in the warm-up:
100 m/s at ~300 m AGL (altitude = HAE of the start point's terrain + 300 m, constant along the path), gimbal 20° down,
over Mainside / Area 22 (buildings) and then up the Santa Margarita River riparian corridor (WorldCover tree cover).
90 s, like the other phases.

The exact path is chosen from data by a committed selector (`scripts/bench/select_dense_path.py`, the gate (r) pattern):
candidate straight segments of 9 km are scored by the number of package buildings plus WorldCover code-10 texels inside
the camera footprint along the segment (footprint from the pose and FOV over the package DEM); the best segment's start,
bearing and altitude are written into `scenario.py` as constants with a comment naming the selector run. The selector
reads only the package (`features/buildings/` once R3 exists; before that, land cover and Overture's snapshot).

### 2. Game-thread p95

`run_bench.py` already records per-frame game-thread time in `frames.jsonl`; `analyze.py` adds `game_p95_ms` per phase to
`results.json` and `compare.py` prints it. No runtime change.

### 3. `scripts/bench/sweep.py`

```
uv run scripts/bench/sweep.py --site pendleton --scene <package> --out .cache/bench-r3r4/<name> \
    --axis buildings=off,on --axis buildings_sse=8,16,32,64 ...
```

- Runs `run_bench.py` with only the `dense_pass` phase (new `--phases` filter; warm-up still runs) once per row. A row is
  one point of the cartesian product of the given axes, but axes of a disabled feature collapse (no SSE rows with
  buildings off).
- Each axis maps to a `CAMSIM_*` env var (table below); the mapping lives in `sweep.py` so new knobs are one line.
- Output: `sweep.csv` / `sweep.md` with one row per setting: game p50, game p95, GPU p50, wall p95, dropped frames, fps,
  peak GPU memory (new: on Linux `nvidia-smi memory.used` polled as `scale_bench.py` does; on macOS the process's Metal
  allocation from a console memory dump issued by the bench at the end of the phase and parsed from the log (`rhi.DumpMemory`
  or `memreport`; the plan's first bench task confirms which one reports Metal allocations);
  `run_bench.py` records neither today), plus a reference shot per
  row from a fixed `dense_pass` pose (`shots/<row>.png`) and a contact sheet (`sweep_sheet.png`) for the visual side.
- `--repeat N` (default 2) runs each row N times and reports the range, as the R1/R2 tables do.

| Axis | Env var | Values (default sweep) |
|---|---|---|
| `buildings` | `CAMSIM_SCENE_BUILDINGS_ENABLED` | off, on |
| `buildings_sse` | `CAMSIM_SCENE_BUILDINGS_MAXIMUM_SCREEN_SPACE_ERROR` | 8, 16, 32, 64 |
| `buildings_physics` | `CAMSIM_SCENE_BUILDINGS_CREATE_PHYSICS_MESHES` | on, off |
| `trees` | `CAMSIM_VEGETATION_ENABLED` | off, on |
| `trees_radius` | `CAMSIM_VEGETATION_RADIUS_M` | 500, 1000, 2000 |
| `trees_density` | `CAMSIM_VEGETATION_DENSITY_SCALE` | 0.25, 0.5, 1.0 |
| `trees_shadows` | `CAMSIM_VEGETATION_CAST_SHADOWS` | on, off |

The full product is large; the documented runs are one-factor sweeps around a candidate default plus the combined
both-on row (a few hours on the laptop at 2 repeats).

### 4. Budget

M1 Pro, 1080p, EO, `dense_pass`, buildings and trees both on at their defaults, against both off in the same session:

| Metric | Budget |
|---|---|
| fps / dropped frames | 30 / 0 (as today) |
| Game-thread p50 | ≤ +1.5 ms combined |
| Game-thread p95 | ≤ +3.0 ms combined |
| GPU frame p50 | ≤ +5 ms |
| VRAM | ≤ +400 MB |
| Existing phases | within noise of R2 (game p50, wall p95) |

Defaults are the most realistic row (largest radius / density, smallest SSE, shadows on) that fits the budget, chosen
with the user from `sweep.md` and the contact sheet. Linux / RTX 5080 at 1, 2 and 4 instances joins the `FOR_LINUX.md`
list, as for R1 and R2.

## Testing

- `scripts/bench` unit tests (existing pytest pattern, no UE): axis collapse and env mapping in `sweep.py`; `game_p95_ms`
  in `analyze.py` from a fixture `frames.jsonl`; the selector's footprint scoring on a synthetic package.
- `run_bench.py --site pendleton` without `--phases` runs every phase as before, plus `dense_pass`.

## Out of scope

Automated realism scoring (the visual side is the contact sheet and the user's review; R6 owns metrics). IR sweeps
(thermal is deferred in both slices; `--sensor ir` still works per row if wanted).
