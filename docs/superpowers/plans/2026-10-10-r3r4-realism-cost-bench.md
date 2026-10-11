# Realism vs Cost Bench (REALISM R3/R4 slices) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the buildings and trees slices a worst-case Pendleton bench phase (`dense_pass`), game-thread p95, GPU memory, and a `sweep.py` that runs one bench per realism setting and writes a cost table plus a contact sheet of shots.

**Architecture:** All changes are in `scripts/bench/` (Python, no UE change). A pure scoring library (`dense_path.py`) and a CLI (`select_dense_path.py`) pick the `dense_pass` path from the scene package's land cover and terrain, and write `dense_path.json`, which `scenario.py` reads. `run_bench.py` gains opt-in `--phases`, `--shots` and `--gpu-mem` flags. `sweep.py` drives `run_bench.py` once per row, then aggregates `results.json` files into `sweep.csv`, `sweep.md` and `sweep_sheet.png`.

**Tech Stack:** Python 3.12, numpy, Pillow (bench); `camsim_scene.qmesh` (only in the selector CLI, run under `uv run --project scripts/scene`); pytest; `nvidia-smi` (Linux), `/usr/bin/footprint` (macOS).

**Spec:** `docs/superpowers/specs/2026-10-10-r3r4-realism-cost-bench-design.md` (siblings: `2026-10-10-r3-buildings-slice-design.md`, `2026-10-10-r4-trees-slice-design.md`).

## Global Constraints

- `run_bench.py` with none of the new flags behaves exactly as today, except that `--site pendleton` gains the `dense_pass` phase, a longer warm-up that flies it once, and one extra shot `dense_oblique`.
- The bench needs no new third-party dependencies: numpy + pillow only (`uv run --with numpy --with pillow python scripts/bench/...`). The selector CLI is the one exception: it runs under `uv run --project scripts/scene`.
- Bench unit tests live in `scripts/tests/test_bench_*.py` and run with `uv run --with pytest --with numpy --with pillow --with pycocotools pytest scripts/tests/ -v` (the CI `unit-tests` job). Tests that need `camsim_scene` use `pytest.importorskip("camsim_scene")`.
- Env var names, verbatim from the spec: `CAMSIM_SCENE_BUILDINGS_ENABLED`, `CAMSIM_SCENE_BUILDINGS_MAXIMUM_SCREEN_SPACE_ERROR`, `CAMSIM_SCENE_BUILDINGS_CREATE_PHYSICS_MESHES`, `CAMSIM_VEGETATION_ENABLED`, `CAMSIM_VEGETATION_RADIUS_M`, `CAMSIM_VEGETATION_DENSITY_SCALE`, `CAMSIM_VEGETATION_CAST_SHADOWS`. CamSim reads env bools as ints, so `on` is sent as `1` and `off` as `0`.
- Budget (spec §4), as deltas of both-on over both-off in the same session: fps 30 and 0 dropped frames; game p50 ≤ +1.5 ms; game p95 ≤ +3.0 ms; GPU p50 ≤ +5 ms; GPU memory ≤ +400 MB (= 381 MiB).
- `dense_pass`: 100 m/s, 90 s, altitude = the start point's terrain HAE + 300 m (constant), gimbal 20° down, Pendleton site only.
- Python style follows the existing bench modules: `from __future__ import annotations`, type hints, module docstring with a usage line, `# noqa: BLE001` with a reason on broad excepts.
- Scene package for live steps: `.cache/scene-packages/pendleton-r2` (absolute path, passed as `CAMSIM_SCENE_DIR`).

## Review Focus

1. **`--phases` / `--shots` with a misspelt name**: expect an immediate error listing the valid names, never a silent run that measures nothing. Tested in Task 2.
2. **A sweep row whose CamSim run crashes or produces no `dense_pass` rows** (no `results.json`, or `frames == 0`, which `_pct` reports as 0.0 ms, i.e. as "fast"): expect the row marked `failed`, kept out of the budget verdict, and the sweep continuing. Tested in Task 7.
3. **GPU memory unavailable** (no `nvidia-smi`; `footprint` missing, denied or printing no GPU category; macOS Docker): expect `gpu_mem_mib_max: null` and `n/a` in tables, never an exception or a 0. Tested in Task 6.
4. **Selector candidates near the package edge or over missing tiles** (land-cover tile absent, terrain tile absent): expect them to score 0 or be skipped, never a crash or a path that leaves the bbox. Tested in Tasks 3 and 4.
5. **Axes of a disabled feature** (`buildings=off` with `buildings_sse=8,16`): expect exactly one `buildings=off` row, not one per SSE value; and a sweep with no axes at all gives one `baseline` row. Tested in Task 7.

---

## File Structure

| File | Responsibility |
|---|---|
| `scripts/bench/analyze.py` (modify) | `game_ms_p95`; optional per-phase `gpu_mem_mib_max` from memory samples |
| `scripts/bench/compare.py` (modify) | print the two new metrics |
| `scripts/bench/run_bench.py` (modify) | `--phases`, `--shots`, `--gpu-mem` flags; `filter_phases`, `filter_shots`; `MemSampler` + `nvidia_smi_mib` / `footprint_mib` parsers |
| `scripts/bench/dense_path.py` (create) | pure: land-cover reading, class grids, candidate segments, strip scoring, ranking |
| `scripts/bench/select_dense_path.py` (create) | CLI: package → terrain heights (quantized-mesh) → chosen path → `dense_path.json` |
| `scripts/bench/dense_path.json` (create, committed) | the chosen path: start lat/lon, bearing, altitude, provenance |
| `scripts/bench/scenario.py` (modify) | `dense_pass` phase, warm-up visit, `dense_oblique` shot (read from `dense_path.json`) |
| `scripts/bench/sweep.py` (create) | axes → rows → `run_bench.py` runs → `sweep.csv` / `sweep.md` / `sweep_sheet.png` |
| `scripts/tests/test_bench_*.py` (modify / create) | unit tests |
| `scripts/tests/fixtures/footprint_camsim.txt` (create) | real `footprint` output captured in Task 6 |
| `scripts/bench/README.md`, `CLAUDE.md` (modify) | docs |

---

### Task 1: Game-thread p95 in analyze and compare

**Files:**
- Modify: `scripts/bench/analyze.py` (`summarize`)
- Modify: `scripts/bench/compare.py` (`METRICS`)
- Test: `scripts/tests/test_bench_analyze.py`, `scripts/tests/test_bench_compare.py`

**Interfaces:**
- Produces: `results.json` phases carry `game_ms_p95: float`; `compare.METRICS` contains `"game_ms_p95"` right after `"game_ms_p50"`.

- [ ] **Step 1: Write the failing tests**

Append to `scripts/tests/test_bench_analyze.py`:

```python
def test_game_thread_p95_is_reported_per_phase():
    rows = [dict(_row(t, 33.0), game_ms=float(t)) for t in range(100)]
    phases = [{"name": "orbit", "start": 0, "end": 100, "measured": True}]
    out = analyze.summarize(rows, phases)["orbit"]
    assert out["game_ms_p50"] == 50.0
    assert out["game_ms_p95"] == 94.0
```

Append to `scripts/tests/test_bench_compare.py`:

```python
def test_game_p95_is_compared():
    assert compare.METRICS.index("game_ms_p95") == compare.METRICS.index("game_ms_p50") + 1
```

(`compare` is already imported in that file; if not, add `from bench import compare`.)

- [ ] **Step 2: Run them to verify they fail**

Run: `uv run --with pytest --with numpy --with pillow --with pycocotools pytest scripts/tests/test_bench_analyze.py scripts/tests/test_bench_compare.py -v`
Expected: FAIL with `KeyError: 'game_ms_p95'` and `ValueError: 'game_ms_p95' is not in list`.

- [ ] **Step 3: Implement**

In `analyze.summarize`, after the `"game_ms_p50"` entry:

```python
            "game_ms_p95": _pct([r["game_ms"] for r in sel], 95),
```

In `compare.METRICS`, after `"game_ms_p50",`:

```python
    "game_ms_p95",
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `uv run --with pytest --with numpy --with pillow --with pycocotools pytest scripts/tests/test_bench_analyze.py scripts/tests/test_bench_compare.py -v`
Expected: PASS (all, including the existing tests).

- [ ] **Step 5: Commit**

```bash
git add scripts/bench/analyze.py scripts/bench/compare.py scripts/tests/test_bench_analyze.py scripts/tests/test_bench_compare.py
git commit -m "feat(bench): game-thread p95 per phase (REALISM R3/R4 bench)"
```

---

### Task 2: `--phases` and `--shots` filters

**Files:**
- Modify: `scripts/bench/run_bench.py` (`build_parser`, new `filter_phases` / `filter_shots`, `main`)
- Test: `scripts/tests/test_bench_run.py`

**Interfaces:**
- Produces:
  - `filter_phases(phases: list[scenario.Phase], names: list[str] | None, skip_warmup: bool) -> list[scenario.Phase]`
  - `filter_shots(shots: list[scenario.Shot], names: list[str] | None) -> list[scenario.Shot]`
  - CLI: `--phases a,b` and `--shots a,b` (comma lists; default: all). Task 7's `sweep.py` passes `--phases dense_pass --shots dense_oblique`.

- [ ] **Step 1: Write the failing tests**

Append to `scripts/tests/test_bench_run.py`:

```python
import pytest

from bench import run_bench, scenario


def test_filter_phases_keeps_warmup_and_named_measured_phases():
    phases = scenario.build_phases(site=scenario.SITES["pendleton"])
    out = run_bench.filter_phases(phases, ["coast_pass"], skip_warmup=False)
    assert [p.name for p in out] == ["warmup", "coast_pass"]


def test_filter_phases_none_means_all_and_skip_warmup_drops_it():
    phases = scenario.build_phases()
    assert run_bench.filter_phases(phases, None, False) == phases
    assert [p.name for p in run_bench.filter_phases(phases, None, True)] == [
        "orbit", "slew", "low_pass", "far_origin"
    ]


def test_filter_phases_rejects_unknown_names_with_the_valid_list():
    with pytest.raises(SystemExit, match="unknown phase 'dense_pas'.*orbit"):
        run_bench.filter_phases(scenario.build_phases(), ["dense_pas"], False)


def test_filter_shots_selects_and_rejects():
    shots = scenario.build_shots()
    assert [s.name for s in run_bench.filter_shots(shots, ["horizon"])] == ["horizon"]
    assert run_bench.filter_shots(shots, None) == shots
    with pytest.raises(SystemExit, match="unknown shot 'nope'"):
        run_bench.filter_shots(shots, ["nope"])


def test_phases_and_shots_flags_parse_as_lists():
    a = run_bench.build_parser().parse_args(
        ["--label", "x", "--phases", "orbit,slew", "--shots", "horizon"]
    )
    assert a.phases == ["orbit", "slew"] and a.shots == ["horizon"]
    b = run_bench.build_parser().parse_args(["--label", "x"])
    assert b.phases is None and b.shots is None
```

(If `test_bench_run.py` already imports `run_bench` under another form, keep the existing import and drop the duplicate.)

- [ ] **Step 2: Run them to verify they fail**

Run: `uv run --with pytest --with numpy --with pillow --with pycocotools pytest scripts/tests/test_bench_run.py -v`
Expected: FAIL with `AttributeError: module 'bench.run_bench' has no attribute 'filter_phases'`.

- [ ] **Step 3: Implement**

In `run_bench.py`, above `build_parser`:

```python
def _csv_list(text: str) -> list[str]:
    return [s for s in (x.strip() for x in text.split(",")) if s]


def filter_phases(
    phases: list[scenario.Phase], names: list[str] | None, skip_warmup: bool
) -> list[scenario.Phase]:
    """The phases to fly: every phase, or the warm-up plus the named measured
    phases, in scenario order. An unknown name ends the run before CamSim starts."""
    valid = [p.name for p in phases if p.measured]
    for n in names or []:
        if n not in valid:
            sys.exit(f"unknown phase '{n}' (valid: {', '.join(valid)})")
    out = []
    for p in phases:
        if not p.measured:
            if not skip_warmup:
                out.append(p)
        elif names is None or p.name in names:
            out.append(p)
    return out


def filter_shots(
    shots: list[scenario.Shot], names: list[str] | None
) -> list[scenario.Shot]:
    if names is None:
        return shots
    valid = [s.name for s in shots]
    for n in names:
        if n not in valid:
            sys.exit(f"unknown shot '{n}' (valid: {', '.join(valid)})")
    return [s for s in shots if s.name in names]
```

In `build_parser`, before `return ap`:

```python
    ap.add_argument(
        "--phases",
        type=_csv_list,
        default=None,
        metavar="A,B",
        help="measured phases to fly (comma list; the warm-up still runs). Default: all",
    )
    ap.add_argument(
        "--shots",
        type=_csv_list,
        default=None,
        metavar="A,B",
        help="shots to take (comma list). Default: all",
    )
```

In `main`, validate before launching CamSim (right after `site = ...`):

```python
    phases = filter_phases(
        scenario.build_phases(smoke=args.smoke, site=site), args.phases, args.skip_warmup
    )
    shots = filter_shots(scenario.build_shots(smoke=args.smoke, site=site), args.shots)
```

Replace the phase loop header and its warm-up skip:

```python
        for ph in phases:
            print(f"[bench] phase {ph.name} ({ph.duration_s:.0f}s)", flush=True)
```

(delete the two lines `if ph.name == "warmup" and args.skip_warmup: continue`), and the shot loop header with `for shot in shots:`. Add to `results["meta"]`: `"phases_filter": args.phases, "shots_filter": args.shots,`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `uv run --with pytest --with numpy --with pillow --with pycocotools pytest scripts/tests/ -v`
Expected: PASS (whole `scripts/tests` suite).

- [ ] **Step 5: Commit**

```bash
git add scripts/bench/run_bench.py scripts/tests/test_bench_run.py
git commit -m "feat(bench): --phases and --shots filters (REALISM R3/R4 bench)"
```

---

### Task 3: Dense-path scoring library (pure)

**Decision recorded here (spec deviation, listed in the plan report):** the spec scores candidates by "package buildings plus WorldCover code-10 texels". This plan uses WorldCover **built-up (code 50)** texels as the building signal. Built-up is in every package today, so the selector runs before R3's buildings layer exists, and the `dense_pass` constants never depend on R3's GLB format. The two signals are normalised and summed: `score = T/T_max + B/B_max` over all candidates, and both must be > 0. A raw sum would be dominated by whichever class has more texels.

**Files:**
- Create: `scripts/bench/dense_path.py`
- Test: `scripts/tests/test_bench_dense_path.py`

**Interfaces:**
- Produces (all in `bench.dense_path`):
  - `TREE = 10`, `BUILT = 50`, `M_PER_DEG = 111_320.0`
  - `lc_tile_filename(i: int, j: int) -> str` (same names as `camsim_scene.layers.landcover.tile_filename`, e.g. `"+33.25_-117.45.png"`)
  - `class LandCover(root: Path)` with `codes(lon: np.ndarray, lat: np.ndarray) -> np.ndarray` (uint8; 0 where the tile is missing)
  - `@dataclass Grid(origin_lon: float, origin_lat: float, cell_m: float, tree: np.ndarray, built: np.ndarray)` with arrays shaped `[rows (north), cols (east)]`, holding the fraction of samples per cell
  - `class_grid(lc: LandCover, bbox: tuple[float, float, float, float], cell_m: float = 100.0, step_m: float = 20.0) -> Grid`
  - `@dataclass Candidate(x0: float, y0: float, bearing_deg: float, tree: float, built: float, score: float = 0.0)` (x east / y north metres from the grid origin)
  - `candidates(grid: Grid, length_m: float = 9000.0, start_step_m: float = 1000.0, bearing_step_deg: float = 15.0, margin_m: float = 500.0) -> list[Candidate]` (scored and ranked best first)
  - `strip_points(x0, y0, bearing_deg, length_m, near_m=400.0, far_m=2000.0, half_width_m=800.0, step_m=100.0) -> tuple[np.ndarray, np.ndarray]`
  - `xy_to_lonlat(grid: Grid, x: float, y: float) -> tuple[float, float]`

- [ ] **Step 1: Write the failing tests**

Create `scripts/tests/test_bench_dense_path.py`:

```python
"""dense_pass path selection: land-cover reading, strip scoring, ranking (REALISM R3/R4 bench)."""

import numpy as np
from PIL import Image

from bench import dense_path as dp


def _tile(root, i, j, fill):
    a = np.full((600, 600), fill, np.uint8)
    Image.fromarray(a).save(root / dp.lc_tile_filename(i, j))
    return a


def test_tile_filename_matches_the_landcover_layer():
    assert dp.lc_tile_filename(665, -2349) == "+33.25_-117.45.png"


def test_landcover_reads_codes_and_missing_tiles_are_zero(tmp_path):
    a = _tile(tmp_path, 665, -2349, 30)
    a[0, 0] = 10  # north-west corner texel
    Image.fromarray(a).save(tmp_path / dp.lc_tile_filename(665, -2349))
    lc = dp.LandCover(tmp_path)
    got = lc.codes(np.array([-117.45 + 1e-6, -117.40 + 0.01]), np.array([33.30 - 1e-6, 33.27]))
    assert got.tolist() == [10, 0]


def test_class_grid_fractions(tmp_path):
    a = np.full((600, 600), 30, np.uint8)
    a[:, :300] = 10  # west half trees
    Image.fromarray(a).save(tmp_path / dp.lc_tile_filename(665, -2349))
    g = dp.class_grid(dp.LandCover(tmp_path), (-117.45, 33.25, -117.40, 33.30), cell_m=500.0)
    assert g.tree.shape == g.built.shape
    assert g.tree[:, 0].min() == 1.0 and g.tree[:, -1].max() == 0.0
    assert g.built.max() == 0.0


def test_strip_points_run_along_the_bearing():
    x, y = dp.strip_points(0.0, 0.0, 90.0, 1000.0, near_m=0.0, far_m=0.0, half_width_m=0.0, step_m=500.0)
    assert np.allclose(sorted(set(np.round(x))), [0.0, 500.0, 1000.0])
    assert np.allclose(y, 0.0, atol=1e-9)


def _synthetic_grid(n=120, cell=100.0):
    tree = np.zeros((n, n))
    built = np.zeros((n, n))
    tree[60:62, :] = 1.0  # an east-west tree band (row index runs north to south)
    built[:, 30:32] = 1.0  # a north-south built band
    return dp.Grid(-117.5, 33.2, cell, tree, built)


def test_best_candidate_needs_both_classes_and_stays_inside():
    g = _synthetic_grid()
    ranked = dp.candidates(g, length_m=4000.0, start_step_m=1000.0, bearing_step_deg=45.0)
    best = ranked[0]
    assert best.tree > 0 and best.built > 0
    assert all(c.tree > 0 and c.built > 0 for c in ranked)
    ext = g.tree.shape[1] * g.cell_m
    for c in ranked:
        x1 = c.x0 + 4000.0 * np.sin(np.radians(c.bearing_deg))
        y1 = c.y0 + 4000.0 * np.cos(np.radians(c.bearing_deg))
        assert 500.0 <= min(c.x0, x1) and max(c.x0, x1) <= ext - 500.0
        assert 500.0 <= min(c.y0, y1) and max(c.y0, y1) <= ext - 500.0
    assert ranked == sorted(ranked, key=lambda c: -c.score)


def test_ranking_is_deterministic():
    g = _synthetic_grid()
    a = dp.candidates(g, length_m=4000.0, bearing_step_deg=45.0)
    b = dp.candidates(g, length_m=4000.0, bearing_step_deg=45.0)
    assert a == b


def test_xy_to_lonlat_round_trip_origin():
    g = _synthetic_grid()
    lon, lat = dp.xy_to_lonlat(g, 0.0, 0.0)
    assert (lon, lat) == (-117.5, 33.2)
    lon, lat = dp.xy_to_lonlat(g, 0.0, dp.M_PER_DEG)
    assert abs(lat - 34.2) < 1e-9
```

Note on `Grid` orientation: `tree[r, c]` is the cell `r` rows **north** of the origin (row 0 = south edge), so `y = (r + 0.5) * cell_m`. The tree band at rows 60–61 is therefore at y ≈ 6 km.

- [ ] **Step 2: Run them to verify they fail**

Run: `uv run --with pytest --with numpy --with pillow --with pycocotools pytest scripts/tests/test_bench_dense_path.py -v`
Expected: FAIL with `ImportError: cannot import name 'dense_path'`.

- [ ] **Step 3: Implement `scripts/bench/dense_path.py`**

```python
"""dense_pass path selection (REALISM R3/R4 bench): score straight segments by the tree cover (WorldCover 10) and
built-up land (50) the forward-looking camera sweeps over, from a scene package's land-cover tiles.

The building signal is built-up land, not R3's buildings layer, so the path does not depend on R3. The camera
footprint is approximated by the strip it sweeps: from near_m to length + far_m along the track, +-half_width_m across.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from PIL import Image

TREE = 10
BUILT = 50
M_PER_DEG = 111_320.0
TILE_DEG = 0.05  # camsim-landcover-1
TILE_PX = 600


def lc_tile_filename(i: int, j: int) -> str:
    return f"{i * TILE_DEG:+.2f}_{j * TILE_DEG:+.2f}.png"


class LandCover:
    """Land-cover codes at points; a missing tile reads as 0."""

    def __init__(self, root: Path) -> None:
        self.root = Path(root)
        self._tiles: dict[tuple[int, int], np.ndarray | None] = {}

    def _tile(self, i: int, j: int) -> np.ndarray | None:
        if (i, j) not in self._tiles:
            p = self.root / lc_tile_filename(i, j)
            self._tiles[(i, j)] = np.asarray(Image.open(p)) if p.exists() else None
        return self._tiles[(i, j)]

    def codes(self, lon: np.ndarray, lat: np.ndarray) -> np.ndarray:
        lon, lat = np.asarray(lon, float), np.asarray(lat, float)
        ti = np.floor(lat / TILE_DEG).astype(np.int64)
        tj = np.floor(lon / TILE_DEG).astype(np.int64)
        out = np.zeros(lon.shape, np.uint8)
        for i, j in set(zip(ti.ravel().tolist(), tj.ravel().tolist())):
            a = self._tile(i, j)
            if a is None:
                continue
            m = (ti == i) & (tj == j)
            col = np.minimum(((lon[m] - j * TILE_DEG) / TILE_DEG * TILE_PX).astype(np.int64), TILE_PX - 1)
            row = np.minimum((((i + 1) * TILE_DEG - lat[m]) / TILE_DEG * TILE_PX).astype(np.int64), TILE_PX - 1)
            out[m] = a[row, col]
        return out


@dataclass
class Grid:
    origin_lon: float  # south-west corner
    origin_lat: float
    cell_m: float
    tree: np.ndarray  # [row north of origin, col east of origin], fraction 0..1
    built: np.ndarray

    def m_per_deg_lon(self) -> float:
        return M_PER_DEG * math.cos(math.radians(self.origin_lat))


def xy_to_lonlat(grid: Grid, x: float, y: float) -> tuple[float, float]:
    return grid.origin_lon + x / grid.m_per_deg_lon(), grid.origin_lat + y / M_PER_DEG


def class_grid(
    lc: LandCover, bbox: tuple[float, float, float, float], cell_m: float = 100.0, step_m: float = 20.0
) -> Grid:
    w, s, e, n = bbox
    kx = M_PER_DEG * math.cos(math.radians(s))
    cols = int(round((e - w) * kx / cell_m))
    rows = int(round((n - s) * M_PER_DEG / cell_m))
    k = max(1, int(round(cell_m / step_m)))
    sub = (np.arange(k) + 0.5) * cell_m / k
    tree = np.zeros((rows, cols))
    built = np.zeros((rows, cols))
    for r in range(rows):  # one row of cells at a time keeps memory flat
        ys = r * cell_m + sub
        xs = (np.arange(cols)[:, None] * cell_m + sub[None, :]).ravel()
        X, Y = np.meshgrid(xs, ys)
        c = lc.codes(w + X / kx, s + Y / M_PER_DEG).reshape(k, cols, k)
        tree[r] = (c == TREE).mean(axis=(0, 2))
        built[r] = (c == BUILT).mean(axis=(0, 2))
    return Grid(w, s, cell_m, tree, built)


def strip_points(
    x0: float,
    y0: float,
    bearing_deg: float,
    length_m: float,
    near_m: float = 400.0,
    far_m: float = 2000.0,
    half_width_m: float = 800.0,
    step_m: float = 100.0,
) -> tuple[np.ndarray, np.ndarray]:
    b = math.radians(bearing_deg)
    fx, fy = math.sin(b), math.cos(b)  # forward (bearing from north, clockwise)
    rx, ry = fy, -fx  # right
    along = np.arange(near_m, length_m + far_m + step_m / 2, step_m)
    across = np.arange(-half_width_m, half_width_m + step_m / 2, step_m) if half_width_m > 0 else np.array([0.0])
    A, C = np.meshgrid(along, across)
    return (x0 + A * fx + C * rx).ravel(), (y0 + A * fy + C * ry).ravel()


def _sample(grid: Grid, a: np.ndarray, x: np.ndarray, y: np.ndarray) -> float:
    c = np.floor(x / grid.cell_m).astype(np.int64)
    r = np.floor(y / grid.cell_m).astype(np.int64)
    ok = (r >= 0) & (r < a.shape[0]) & (c >= 0) & (c < a.shape[1])
    return float(a[r[ok], c[ok]].sum())


@dataclass
class Candidate:
    x0: float
    y0: float
    bearing_deg: float
    tree: float
    built: float
    score: float = 0.0


def candidates(
    grid: Grid,
    length_m: float = 9000.0,
    start_step_m: float = 1000.0,
    bearing_step_deg: float = 15.0,
    margin_m: float = 500.0,
) -> list[Candidate]:
    """Every straight segment whose start and end stay margin_m inside the grid, scored by the normalised sum of the
    tree and built-up fractions in its swept strip; segments missing either class are dropped. Best first; ties keep
    generation order (start south-west first, bearing ascending)."""
    ext_x = grid.tree.shape[1] * grid.cell_m
    ext_y = grid.tree.shape[0] * grid.cell_m
    out: list[Candidate] = []
    for y0 in np.arange(margin_m, ext_y - margin_m + 1e-6, start_step_m):
        for x0 in np.arange(margin_m, ext_x - margin_m + 1e-6, start_step_m):
            for bd in np.arange(0.0, 360.0, bearing_step_deg):
                b = math.radians(bd)
                x1, y1 = x0 + length_m * math.sin(b), y0 + length_m * math.cos(b)
                if not (margin_m <= x1 <= ext_x - margin_m and margin_m <= y1 <= ext_y - margin_m):
                    continue
                xs, ys = strip_points(float(x0), float(y0), float(bd), length_m)
                t, bu = _sample(grid, grid.tree, xs, ys), _sample(grid, grid.built, xs, ys)
                if t > 0 and bu > 0:
                    out.append(Candidate(float(x0), float(y0), float(bd), t, bu))
    if not out:
        return out
    tmax, bmax = max(c.tree for c in out), max(c.built for c in out)
    for c in out:
        c.score = c.tree / tmax + c.built / bmax
    return sorted(out, key=lambda c: -c.score)  # sorted() is stable
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `uv run --with pytest --with numpy --with pillow --with pycocotools pytest scripts/tests/test_bench_dense_path.py -v`
Expected: PASS. If `test_class_grid_fractions` fails on the edge columns, the bbox-to-cell rounding is off by one: fix `class_grid`, not the test.

- [ ] **Step 5: Commit**

```bash
git add scripts/bench/dense_path.py scripts/tests/test_bench_dense_path.py
git commit -m "feat(bench): dense_pass path scoring from land cover (REALISM R3/R4 bench)"
```

---

### Task 4: Selector CLI, run on Pendleton, commit `dense_path.json`

**Decision recorded here (spec addition, listed in the plan report):** altitude is the start point's terrain + 300 m, as the spec says. Because that is constant along a 9 km track, the selector also **rejects segments whose terrain rises more than 150 m above the start** (AGL would fall below 150 m), and takes the best-ranked segment that passes. Terrain heights come from the package's own quantized-mesh tiles (z12, the maximum vertex height within 250 m of each sample), which is what CamSim renders.

**Files:**
- Create: `scripts/bench/select_dense_path.py`
- Create (generated, committed): `scripts/bench/dense_path.json`
- Test: `scripts/tests/test_bench_dense_path.py` (append; `importorskip("camsim_scene")`)

**Interfaces:**
- Consumes: `bench.dense_path` (Task 3); `camsim_scene.qmesh.encode/decode`, `camsim_scene.manifest.Manifest`.
- Produces:
  - `terrain_max_height(pkg: Path, lon: float, lat: float, radius_m: float = 250.0, z: int = 12) -> float | None`
  - `choose(pkg: Path, grid: dp.Grid, ranked: list[dp.Candidate], length_m: float = 9000.0, agl_m: float = 300.0, max_rise_m: float = 150.0, top_n: int = 50) -> dict | None`
  - `dense_path.json`: `{"start_lat": float, "start_lon": float, "bearing_deg": float, "alt_hae_m": float, "length_m": 9000.0, "speed_mps": 100.0, "gimbal_pitch_deg": -20.0, "selector": {"package": str, "tree": float, "built": float, "score": float, "rank": int, "terrain_start_m": float, "terrain_max_m": float}}`. Task 5 reads the first seven keys.

- [ ] **Step 1: Write the failing tests**

Append to `scripts/tests/test_bench_dense_path.py`:

```python
import pytest


def _qmesh_package(tmp_path, height):
    qmesh = pytest.importorskip("camsim_scene.qmesh")
    z, d = 12, 180.0 / 2**12
    lon, lat = -117.45, 33.30
    x, y = int((lon + 180.0) // d), int((lat + 90.0) // d)
    w, s = -180.0 + x * d, -90.0 + y * d
    u = np.array([w, w + d, w, w + d])
    v = np.array([s, s, s + d, s + d])
    h = np.full(4, height)
    raw = qmesh.encode(u, v, h, np.array([[0, 1, 2], [1, 3, 2]]), (w, s, w + d, s + d))
    p = tmp_path / "terrain" / str(z) / str(x)
    p.mkdir(parents=True)
    qmesh.write_tile(p / f"{y}.terrain", qmesh.gzip_tile(raw))
    return tmp_path, lon, lat


def test_terrain_max_height_reads_the_package_tile(tmp_path):
    from bench import select_dense_path as sdp

    pkg, lon, lat = _qmesh_package(tmp_path, 123.0)
    # The tile is ~4.9 km wide, so a 250 m radius may contain no vertex: the reader falls back to the
    # nearest vertex rather than answering None.
    assert abs(sdp.terrain_max_height(pkg, lon, lat) - 123.0) < 0.5


def test_terrain_max_height_missing_tile_is_none(tmp_path):
    pytest.importorskip("camsim_scene.qmesh")
    from bench import select_dense_path as sdp

    assert sdp.terrain_max_height(tmp_path, -117.45, 33.30) is None
```

(`qmesh.encode` takes `(lon, lat, height, triangles, bounds)`; check the signature in `scripts/scene/camsim_scene/qmesh.py:161` and adapt the arguments if it has changed. `write_tile` and `gzip_tile` are at `:197-205`.)

- [ ] **Step 2: Run them to verify they fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/tests/test_bench_dense_path.py -v`
Expected: FAIL with `ImportError: cannot import name 'select_dense_path'`. (Under the plain `scripts/tests` command these two tests skip.)

- [ ] **Step 3: Implement `scripts/bench/select_dense_path.py`**

```python
"""Choose the dense_pass path on a scene package (REALISM R3/R4 bench) and write scripts/bench/dense_path.json.

    uv run --project scripts/scene python scripts/bench/select_dense_path.py PKG [--out scripts/bench/dense_path.json]

Rule (fixed before any bench run): candidates from bench.dense_path.candidates (9 km straight segments starting on a
1 km grid, bearings every 15 deg, 500 m inside the package bbox, ranked by tree cover + built-up land in the swept
strip); altitude = terrain at the start + 300 m (package quantized-mesh, z12, max vertex within 250 m); the first of the
top 50 whose terrain (sampled every 500 m) never rises more than 150 m above the start wins.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from bench import dense_path as dp  # noqa: E402


def terrain_max_height(
    pkg: Path, lon: float, lat: float, radius_m: float = 250.0, z: int = 12
) -> float | None:
    from camsim_scene import qmesh

    d = 180.0 / 2**z
    x, y = int((lon + 180.0) // d), int((lat + 90.0) // d)
    p = Path(pkg) / "terrain" / str(z) / str(x) / f"{y}.terrain"
    if not p.exists():
        return None
    q = qmesh.decode(p.read_bytes())
    w, s = -180.0 + x * d, -90.0 + y * d
    vl, vt = q.lonlat((w, s, w + d, s + d))
    h = q.heights()
    kx = dp.M_PER_DEG * math.cos(math.radians(lat))
    dist = np.hypot((vl - lon) * kx, (vt - lat) * dp.M_PER_DEG)
    near = dist <= radius_m
    return float(h[near].max() if near.any() else h[np.argmin(dist)])


def choose(
    pkg: Path,
    grid: dp.Grid,
    ranked: list[dp.Candidate],
    length_m: float = 9000.0,
    agl_m: float = 300.0,
    max_rise_m: float = 150.0,
    top_n: int = 50,
) -> dict | None:
    for rank, c in enumerate(ranked[:top_n]):
        b = math.radians(c.bearing_deg)
        hs = []
        for a in np.arange(0.0, length_m + 1.0, 500.0):
            lon, lat = dp.xy_to_lonlat(grid, c.x0 + a * math.sin(b), c.y0 + a * math.cos(b))
            hs.append(terrain_max_height(pkg, lon, lat))
        if any(h is None for h in hs) or max(hs) - hs[0] > max_rise_m:
            continue
        lon0, lat0 = dp.xy_to_lonlat(grid, c.x0, c.y0)
        return {
            "start_lat": round(lat0, 6),
            "start_lon": round(lon0, 6),
            "bearing_deg": c.bearing_deg,
            "alt_hae_m": round(hs[0] + agl_m, 1),
            "length_m": length_m,
            "speed_mps": 100.0,
            "gimbal_pitch_deg": -20.0,
            "selector": {
                "package": Path(pkg).name,
                "tree": round(c.tree, 2),
                "built": round(c.built, 2),
                "score": round(c.score, 4),
                "rank": rank,
                "terrain_start_m": round(hs[0], 1),
                "terrain_max_m": round(max(hs), 1),
            },
        }
    return None


def main(argv: list[str] | None = None) -> int:
    from camsim_scene.manifest import Manifest

    ap = argparse.ArgumentParser()
    ap.add_argument("pkg", type=Path)
    ap.add_argument("--out", type=Path, default=Path(__file__).with_name("dense_path.json"))
    a = ap.parse_args(argv)
    m = Manifest.load(a.pkg / "manifest.json")
    grid = dp.class_grid(dp.LandCover(a.pkg / "landcover"), tuple(m.bbox))
    ranked = dp.candidates(grid)
    print(f"[select] {len(ranked)} candidates with both tree cover and built-up land")
    best = choose(a.pkg, grid, ranked)
    if best is None:
        print("[select] no candidate passed the terrain-rise rule", file=sys.stderr)
        return 1
    a.out.write_text(json.dumps(best, indent=2) + "\n")
    print(json.dumps(best, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `uv run --project scripts/scene --with pytest pytest scripts/tests/test_bench_dense_path.py -v`
Expected: PASS (all, including Task 3's tests).

- [ ] **Step 5: Run the selector on Pendleton**

Run: `uv run --project scripts/scene python scripts/bench/select_dense_path.py "$PWD/.cache/scene-packages/pendleton-r2"`
Expected:
- exit 0 and `scripts/bench/dense_path.json` written;
- the printed `start_lat`/`start_lon` lie inside W −117.62 S 33.19 E −117.24 N 33.52;
- `selector.tree > 0`, `selector.built > 0`, `terrain_max_m − terrain_start_m ≤ 150`.

Sanity check: put the start and end points into a map viewer and confirm the track crosses built-up land (Mainside / Oceanside fringe) and tree cover (Santa Margarita riparian). The rule is fixed: do not hand-edit the result. If the path is implausible, report it in the task report rather than change the rule.

- [ ] **Step 6: Commit**

```bash
git add scripts/bench/select_dense_path.py scripts/bench/dense_path.json scripts/tests/test_bench_dense_path.py
git commit -m "feat(bench): dense_pass selector and the Pendleton path (REALISM R3/R4 bench)"
```

---

### Task 5: `dense_pass` phase, warm-up visit, `dense_oblique` shot

**Files:**
- Modify: `scripts/bench/scenario.py`
- Test: `scripts/tests/test_bench_scenario.py`

**Interfaces:**
- Consumes: `scripts/bench/dense_path.json` (Task 4).
- Produces:
  - `scenario.DENSE: dict` (the loaded JSON)
  - `scenario._dense_pass(t: float) -> Pose`
  - phase `"dense_pass"` (measured, 90 s) after `coast_pass` when `site.coast`; Pendleton warm-up 180 s
  - shot `"dense_oblique"` (Pendleton only) at the path's midpoint (t = 45 s). Task 7 requests it with `--shots dense_oblique`.

- [ ] **Step 1: Write the failing tests**

In `scripts/tests/test_bench_scenario.py`, change the last line of `test_pendleton_site_adds_a_measured_coast_pass` to:

```python
    assert len(scenario.build_shots(site=site)) == len(scenario.build_shots()) + 1
```

and append:

```python
import math


def test_pendleton_adds_dense_pass_after_coast_and_warms_it():
    site = scenario.SITES["pendleton"]
    phases = scenario.build_phases(site=site)
    names = [p.name for p in phases]
    assert names[-2:] == ["coast_pass", "dense_pass"]
    dense = phases[-1]
    assert dense.measured and dense.duration_s == 90.0
    assert phases[0].duration_s == 180.0  # warm-up flies every measured area once
    assert "dense_pass" not in [p.name for p in scenario.build_phases()]


def test_dense_pass_flies_the_selected_path():
    d = scenario.DENSE
    p0, p90 = scenario._dense_pass(0.0), scenario._dense_pass(90.0)
    assert (p0.lat, p0.lon) == (d["start_lat"], d["start_lon"])
    assert p0.alt == p90.alt == d["alt_hae_m"]
    assert p0.yaw == d["bearing_deg"] and p0.gimbal_pitch == -20.0 and p0.gimbal_yaw == 0.0
    dn = (p90.lat - p0.lat) * 111_320.0
    de = (p90.lon - p0.lon) * 111_320.0 * math.cos(math.radians(p0.lat))
    assert abs(math.hypot(dn, de) - 9000.0) < 5.0


def test_warmup_visits_the_dense_path():
    site = scenario.SITES["pendleton"]
    warm = scenario.build_phases(site=site)[0]
    p = warm.pose_at(135.0)  # middle of the 120-150 s dense visit
    assert p.alt == scenario.DENSE["alt_hae_m"]


def test_dense_oblique_shot_is_pendleton_only():
    site = scenario.SITES["pendleton"]
    shots = {s.name: s for s in scenario.build_shots(site=site)}
    assert shots["dense_oblique"].pose == scenario._dense_pass(45.0)
    assert "dense_oblique" not in [s.name for s in scenario.build_shots()]
```

- [ ] **Step 2: Run them to verify they fail**

Run: `uv run --with pytest --with numpy --with pillow --with pycocotools pytest scripts/tests/test_bench_scenario.py -v`
Expected: FAIL with `AttributeError: module 'bench.scenario' has no attribute 'DENSE'`.

- [ ] **Step 3: Implement**

In `scenario.py`, add `import json` and `from pathlib import Path` to the imports. Below `_coast_pass`:

```python
# Chosen by select_dense_path.py on the Pendleton package (REALISM R3/R4 bench): over built-up land and tree cover.
DENSE = json.loads(Path(__file__).with_name("dense_path.json").read_text())


def _dense_pass(t: float) -> Pose:
    # 100 m/s along the selected path at a constant HAE (start terrain + 300 m), looking ahead 20 deg down
    lat0, lon0 = DENSE["start_lat"], DENSE["start_lon"]
    brg = math.radians(DENSE["bearing_deg"])
    d = DENSE["speed_mps"] * t
    lat = lat0 + d * math.cos(brg) / 111_320.0
    lon = lon0 + d * math.sin(brg) / (111_320.0 * math.cos(math.radians(lat0)))
    return Pose(
        lat,
        lon,
        DENSE["alt_hae_m"],
        yaw=DENSE["bearing_deg"],
        gimbal_yaw=0.0,
        gimbal_pitch=DENSE["gimbal_pitch_deg"],
    )
```

In `build_phases`, replace the warm-up's coast branch:

```python
        if site.coast:
            if t < 120.0:
                return _coast_pass((t - 90.0) * 3.0)
            if t < 150.0:
                return _dense_pass((t - 120.0) * 3.0)
            return far(t - 150.0)
        return far(t - 90.0)
```

the warm-up duration with `Phase("warmup", 180.0 if site.coast else 120.0, warmup, measured=False)`, and the coast append with:

```python
    if site.coast:
        phases.append(Phase("coast_pass", 90.0, _coast_pass))
        phases.append(Phase("dense_pass", 90.0, _dense_pass))
```

In `build_shots`, before the `by_name = ...` line:

```python
    if site.coast:
        shots.append(Shot("dense_oblique", _dense_pass(45.0)))
```

Then update the README phase list (Task 8 does the docs, but keep the module docstring right): add to the module docstring's last sentence "and a measured `dense_pass` over buildings and trees (`dense_path.json`)".

- [ ] **Step 4: Run the tests to verify they pass**

Run: `uv run --with pytest --with numpy --with pillow --with pycocotools pytest scripts/tests/ -v`
Expected: PASS (whole suite: `test_shots_cover_ir_and_night` still sees 12 SF shots).

- [ ] **Step 5: Commit**

```bash
git add scripts/bench/scenario.py scripts/tests/test_bench_scenario.py
git commit -m "feat(bench): dense_pass phase and dense_oblique shot at Pendleton (REALISM R3/R4 bench)"
```

---

### Task 6: GPU memory capture (`--gpu-mem`)

**Decision recorded here (spec deviation, listed in the plan report):** the spec proposes a console memory dump at the end of the phase (`rhi.DumpMemory` or `memreport`). CamSim has no runtime console channel: `-ExecCmds` runs only at startup, and the health server has no exec route. On macOS this plan therefore samples the process externally with `/usr/bin/footprint -p PID` (Apple Silicon unified memory: the GPU allocations appear under an `IOAccelerator` category). Step 1 confirms the category against a live CamSim; if none exists, the value is `null` / `n/a`. On Linux it samples `nvidia-smi memory.used` (whole-GPU, adequate with one instance; `scale_bench.py` does the same).

**Files:**
- Modify: `scripts/bench/run_bench.py` (`nvidia_smi_mib`, `footprint_mib`, `MemSampler`, `--gpu-mem`, `main`)
- Modify: `scripts/bench/analyze.py` (`summarize(..., mem_samples=None)`)
- Modify: `scripts/bench/compare.py` (`METRICS`)
- Create: `scripts/tests/fixtures/footprint_camsim.txt`
- Test: `scripts/tests/test_bench_run.py`, `scripts/tests/test_bench_analyze.py`

**Interfaces:**
- Produces:
  - `nvidia_smi_mib(run=subprocess.run) -> float | None`
  - `footprint_mib(pid: str, run=subprocess.run) -> float | None`
  - `FOOTPRINT_CATEGORY: str` (confirmed in Step 1)
  - `class MemSampler(read: Callable[[], float | None], interval_s: float = 5.0)` with `.start()`, `.stop()`, `.samples: list[tuple[float, float | None]]`
  - `analyze.summarize(rows, phases, mem_samples: list[tuple[float, float | None]] | None = None)`: when `mem_samples` is not None, every measured phase gets `gpu_mem_mib_max: float | None`
  - CLI `--gpu-mem`; `results.meta.gpu_mem_source` ∈ {`"nvidia-smi"`, `"footprint"`, `null`}

- [ ] **Step 1: Capture real `footprint` output (macOS)**

Start CamSim on the package and capture one sample:

```bash
CAMSIM_SCENE_DIR="$PWD/.cache/scene-packages/pendleton-r2" scripts/run.sh --headless --local --detach
# wait for /ready
until curl -sf localhost:8080/ready | grep -q '"ready"'; do sleep 5; done
footprint -p "$(head -1 .cache/camsim.pid)" > .cache/footprint_camsim_full.txt; echo "exit $?"
scripts/stop.sh
grep -n -i "accel\|graphics\|iosurface\|dirty" .cache/footprint_camsim_full.txt | head -20
```

Copy the header lines and the category rows (no paths or user names) into `scripts/tests/fixtures/footprint_camsim.txt`. Expected: a row whose category contains `IOAccelerator` with a size of hundreds of MB to GB. Set `FOOTPRINT_CATEGORY` (Step 3) to that exact category text. If `footprint` needs `sudo`, or no accelerator row exists, write the observed output into the fixture anyway, set `FOOTPRINT_CATEGORY = "IOAccelerator"`, and the parser test asserts `None` for that fixture. That is the `n/a` fallback.

- [ ] **Step 2: Write the failing tests**

Append to `scripts/tests/test_bench_run.py`:

```python
from pathlib import Path
from types import SimpleNamespace

FIXTURES = Path(__file__).parent / "fixtures"


def _ran(stdout, rc=0):
    return lambda *a, **k: SimpleNamespace(returncode=rc, stdout=stdout, stderr="")


def test_nvidia_smi_sums_gpus_and_handles_absence():
    assert run_bench.nvidia_smi_mib(_ran("1024\n2048\n")) == 3072.0
    assert run_bench.nvidia_smi_mib(_ran("", rc=127)) is None

    def boom(*a, **k):
        raise FileNotFoundError("nvidia-smi")

    assert run_bench.nvidia_smi_mib(boom) is None


def test_footprint_parses_the_captured_camsim_output():
    text = (FIXTURES / "footprint_camsim.txt").read_text()
    got = run_bench.footprint_mib("123", _ran(text))
    if run_bench.FOOTPRINT_CATEGORY.lower() in text.lower():
        assert got is not None and got > 100.0  # CamSim holds well over 100 MiB of GPU memory
    else:
        assert got is None


def test_footprint_units_and_failures():
    table = (
        "      Dirty      Clean  Reclaimable    Regions    Category\n"
        "        ---        ---          ---        ---    ---\n"
        f"    1.5 GB        0 B          0 B         12    {run_bench.FOOTPRINT_CATEGORY}\n"
    )
    assert run_bench.footprint_mib("1", _ran(table)) == 1536.0
    assert run_bench.footprint_mib("1", _ran("no such process", rc=1)) is None

    def boom(*a, **k):
        raise PermissionError("footprint")

    assert run_bench.footprint_mib("1", boom) is None


def test_gpu_mem_flag_parses():
    assert run_bench.build_parser().parse_args(["--label", "x", "--gpu-mem"]).gpu_mem
```

Append to `scripts/tests/test_bench_analyze.py`:

```python
def test_gpu_memory_max_per_phase_and_unknown_is_none():
    rows = [_row(t, 33.0) for t in range(20)]
    phases = [
        {"name": "orbit", "start": 0, "end": 10, "measured": True},
        {"name": "slew", "start": 10, "end": 20, "measured": True},
    ]
    samples = [(1.0, 900.0), (5.0, 1200.0), (12.0, None)]
    out = analyze.summarize(rows, phases, mem_samples=samples)
    assert out["orbit"]["gpu_mem_mib_max"] == 1200.0
    assert out["slew"]["gpu_mem_mib_max"] is None
    assert "gpu_mem_mib_max" not in analyze.summarize(rows, phases)["orbit"]
```

If Step 1 found the format differs from the synthetic `table` (e.g. different column order), change `test_footprint_units_and_failures`'s table to the real layout. The fixture is the authority.

- [ ] **Step 3: Run them to verify they fail**

Run: `uv run --with pytest --with numpy --with pillow --with pycocotools pytest scripts/tests/test_bench_run.py scripts/tests/test_bench_analyze.py -v`
Expected: FAIL with `AttributeError: ... 'nvidia_smi_mib'` and `TypeError: summarize() got an unexpected keyword argument 'mem_samples'`.

- [ ] **Step 4: Implement**

In `run_bench.py` (add `import shutil` and `from collections.abc import Callable`):

```python
FOOTPRINT_CATEGORY = "IOAccelerator"  # set from the Step 1 capture (scripts/tests/fixtures/footprint_camsim.txt)
_UNITS = {"B": 1.0 / 2**20, "KB": 1.0 / 1024, "MB": 1.0, "GB": 1024.0}


def nvidia_smi_mib(run=subprocess.run) -> float | None:
    """memory.used summed over GPUs (whole-GPU, as scale_bench.py reads it); None without nvidia-smi."""
    try:
        r = run(
            ["nvidia-smi", "--query-gpu=memory.used", "--format=csv,noheader,nounits"],
            capture_output=True,
            text=True,
            check=False,
        )
    except Exception:  # noqa: BLE001 -- absent or broken tool reads as "unknown"
        return None
    vals = [float(x) for x in r.stdout.split() if x.replace(".", "", 1).isdigit()]
    return sum(vals) if r.returncode == 0 and vals else None


def footprint_mib(pid: str, run=subprocess.run) -> float | None:
    """Dirty size of the FOOTPRINT_CATEGORY row of `footprint -p PID` (macOS; Apple Silicon GPU allocations)."""
    try:
        r = run(["footprint", "-p", pid], capture_output=True, text=True, check=False)
    except Exception:  # noqa: BLE001 -- absent or denied tool reads as "unknown"
        return None
    if r.returncode != 0:
        return None
    pat = re.compile(r"^\s*([\d.]+)\s*(B|KB|MB|GB)\s+.*" + re.escape(FOOTPRINT_CATEGORY), re.IGNORECASE)
    total = None
    for line in r.stdout.splitlines():
        m = pat.match(line)
        if m:
            total = (total or 0.0) + float(m.group(1)) * _UNITS[m.group(2).upper()]
    return total


class MemSampler:
    """Polls a GPU-memory reader every interval_s on a thread; samples are (unix time, MiB or None)."""

    def __init__(self, read: Callable[[], float | None], interval_s: float = 5.0) -> None:
        self.read, self.interval_s = read, interval_s
        self.samples: list[tuple[float, float | None]] = []
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def _run(self) -> None:
        while not self._stop.is_set():
            self.samples.append((time.time(), self.read()))
            self._stop.wait(self.interval_s)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=30)


def gpu_mem_reader(docker: bool, pid_file: Path) -> tuple[str | None, Callable[[], float | None] | None]:
    if shutil.which("nvidia-smi"):
        return "nvidia-smi", nvidia_smi_mib
    if sys.platform == "darwin" and not docker and shutil.which("footprint"):
        return "footprint", lambda: footprint_mib(pid_file.read_text().split()[0]) if pid_file.exists() else None
    return None, None
```

In `build_parser`:

```python
    ap.add_argument(
        "--gpu-mem",
        action="store_true",
        help="sample GPU memory during the run (nvidia-smi on Linux, footprint on macOS); off by default",
    )
```

In `main`: after `ready_s = ...`, start the sampler:

```python
        mem_source, mem_read = gpu_mem_reader(bool(args.docker), pid_file) if args.gpu_mem else (None, None)
        sampler = MemSampler(mem_read) if mem_read else None
        if sampler:
            sampler.start()
```

Declare `sampler = None` and `mem_source = None` next to `phases_log` (so the `finally` block and the results see them), stop it first thing in `finally` (`if sampler: sampler.stop()`), and change the summary call:

```python
        "phases": analyze.summarize(
            rows, phases_log, mem_samples=(sampler.samples if sampler else ([] if args.gpu_mem else None))
        ),
```

Add `"gpu_mem_source": mem_source,` to `results["meta"]`. (With `--gpu-mem` but no reader, the empty sample list gives `gpu_mem_mib_max: null` in every phase, which is the `n/a` case.)

In `analyze.summarize`, change the signature to `def summarize(rows: list[dict], phases: list[dict], mem_samples: list[tuple[float, float | None]] | None = None) -> dict:` and add at the end of the per-phase body:

```python
        if mem_samples is not None:
            mem = [v for t, v in mem_samples if ph["start"] <= t < ph["end"] and v is not None]
            out[ph["name"]]["gpu_mem_mib_max"] = max(mem) if mem else None
```

In `compare.METRICS`, append `"gpu_mem_mib_max",` after `"dropped",`. (`_fmt` already prints `—` for None.)

- [ ] **Step 5: Run the tests to verify they pass**

Run: `uv run --with pytest --with numpy --with pillow --with pycocotools pytest scripts/tests/ -v`
Expected: PASS.

- [ ] **Step 6: Live check**

Run: `uv run --with numpy --with pillow python scripts/bench/run_bench.py --label memcheck --site pendleton --phases dense_pass --shots dense_oblique --gpu-mem --env CAMSIM_SCENE_DIR="$PWD/.cache/scene-packages/pendleton-r2" --out .cache/bench-r3r4/memcheck`
Expected:
- `results.json` → `meta.gpu_mem_source == "footprint"` and `phases.dense_pass.gpu_mem_mib_max` a number (or `null` if Step 1 found no category);
- `game_ms_p95` present;
- `shots/dense_oblique.png` exists;
- 30 fps, 0 dropped.

This run is also the first both-off `dense_pass` reference. Write its numbers into the task report.

- [ ] **Step 7: Commit**

```bash
git add scripts/bench/run_bench.py scripts/bench/analyze.py scripts/bench/compare.py scripts/tests/test_bench_run.py scripts/tests/test_bench_analyze.py scripts/tests/fixtures/footprint_camsim.txt
git commit -m "feat(bench): --gpu-mem samples GPU memory (nvidia-smi, footprint) per phase (REALISM R3/R4 bench)"
```

---

### Task 7: `sweep.py`

**Files:**
- Create: `scripts/bench/sweep.py`
- Test: `scripts/tests/test_bench_sweep.py`

**Interfaces:**
- Consumes: `run_bench.py` flags `--label --site --out --phases --shots --gpu-mem --env --sensor` (Tasks 2, 6); `results.json` phase keys `game_ms_p50, game_ms_p95, gpu_ms_p50, wall_ms_p95, dropped, emitted_fps, gpu_mem_mib_max, frames`.
- Produces:
  - `AXES: dict[str, tuple[str, str | None]]` (axis → (env var, master axis or None))
  - `parse_axis(text: str) -> tuple[str, list[str]]`
  - `build_rows(axes: list[tuple[str, list[str]]]) -> list[dict[str, str]]`
  - `row_label(row: dict[str, str]) -> str`
  - `row_env(row: dict[str, str]) -> dict[str, str]`
  - `run_metrics(results_path: Path) -> dict` (with `status`: `"ok"` / `"failed"`)
  - `aggregate(rows: list[dict], runs: dict[str, list[dict]]) -> list[dict]`
  - `budget_verdict(row_agg: dict, base_agg: dict) -> dict[str, bool]`
  - `render_md(agg: list[dict]) -> str`, `write_csv(path: Path, rows: list[dict], runs: dict[str, list[dict]]) -> None`, `contact_sheet(images: list[tuple[str, Path]], out: Path, thumb_w: int = 640, cols: int = 3) -> None`

- [ ] **Step 1: Write the failing tests**

Create `scripts/tests/test_bench_sweep.py`:

```python
"""Realism-vs-cost sweep: axes, row collapse, env mapping, aggregation, budget (REALISM R3/R4 bench)."""

import json

import pytest
from PIL import Image

from bench import sweep


def test_parse_axis_and_unknown_axis():
    assert sweep.parse_axis("buildings_sse=8,16") == ("buildings_sse", ["8", "16"])
    with pytest.raises(SystemExit, match="unknown axis 'roofs'"):
        sweep.parse_axis("roofs=flat")
    with pytest.raises(SystemExit, match="no values"):
        sweep.parse_axis("trees=")


def test_disabled_feature_collapses_its_axes():
    rows = sweep.build_rows(
        [("buildings", ["off", "on"]), ("buildings_sse", ["8", "16"]), ("trees", ["off"])]
    )
    assert rows == [
        {"buildings": "off", "trees": "off"},
        {"buildings": "on", "buildings_sse": "8", "trees": "off"},
        {"buildings": "on", "buildings_sse": "16", "trees": "off"},
    ]


def test_no_axes_is_one_baseline_row():
    rows = sweep.build_rows([])
    assert rows == [{}]
    assert sweep.row_label({}) == "baseline"


def test_env_mapping_uses_ints_for_switches():
    env = sweep.row_env({"buildings": "on", "trees_shadows": "off", "trees_radius": "1000"})
    assert env == {
        "CAMSIM_SCENE_BUILDINGS_ENABLED": "1",
        "CAMSIM_VEGETATION_CAST_SHADOWS": "0",
        "CAMSIM_VEGETATION_RADIUS_M": "1000",
    }


def test_row_label_is_filesystem_safe_and_ordered():
    assert sweep.row_label({"buildings": "on", "buildings_sse": "16"}) == "buildings-on_buildings_sse-16"
    assert sweep.row_label({"trees_density": "0.25"}) == "trees_density-0.25"


def _results(tmp_path, name, **phase):
    p = tmp_path / name / "results.json"
    p.parent.mkdir(parents=True)
    base = {"frames": 2700, "game_ms_p50": 3.0, "game_ms_p95": 4.0, "gpu_ms_p50": 20.0,
            "wall_ms_p95": 34.0, "dropped": 0, "emitted_fps": 29.9, "gpu_mem_mib_max": 3000.0}
    base.update(phase)
    p.write_text(json.dumps({"phases": {"dense_pass": base}}))
    return p


def test_run_metrics_marks_crashes_and_empty_phases_failed(tmp_path):
    assert sweep.run_metrics(tmp_path / "missing" / "results.json")["status"] == "failed"
    assert sweep.run_metrics(_results(tmp_path, "empty", frames=0))["status"] == "failed"
    ok = sweep.run_metrics(_results(tmp_path, "ok"))
    assert ok["status"] == "ok" and ok["game_ms_p95"] == 4.0


def test_aggregate_ranges_and_budget_against_the_all_off_row(tmp_path):
    rows = [{"buildings": "off", "trees": "off"}, {"buildings": "on", "trees": "on"}]
    off = [sweep.run_metrics(_results(tmp_path, f"off{i}", game_ms_p50=3.0 + i * 0.1)) for i in range(2)]
    on = [sweep.run_metrics(_results(tmp_path, f"on{i}", game_ms_p50=4.0, game_ms_p95=8.0,
                                     gpu_mem_mib_max=3300.0)) for i in range(2)]
    agg = sweep.aggregate(rows, {sweep.row_label(rows[0]): off, sweep.row_label(rows[1]): on})
    assert agg[0]["baseline"] and not agg[1]["baseline"]
    assert agg[0]["game_ms_p50"] == (3.0, 3.1)
    v = agg[1]["budget"]
    assert v["game_ms_p50"] is True  # +0.95 ms (median 4.0 vs 3.05) <= 1.5
    assert v["game_ms_p95"] is False  # +4.0 ms > 3.0
    assert v["gpu_mem_mib_max"] is True  # +300 MiB <= 381
    assert agg[1]["within_budget"] is False


def test_failed_rows_get_no_verdict_and_unknown_memory_is_not_a_pass(tmp_path):
    rows = [{"trees": "off"}, {"trees": "on"}]
    off = [sweep.run_metrics(_results(tmp_path, "off", gpu_mem_mib_max=None))]
    on = [sweep.run_metrics(tmp_path / "nope" / "results.json")]
    agg = sweep.aggregate(rows, {"trees-off": off, "trees-on": on})
    assert agg[1]["status"] == "failed" and agg[1]["budget"] == {}
    md = sweep.render_md(agg)
    assert "failed" in md and "n/a" in md


def test_contact_sheet_tiles_labelled_thumbnails(tmp_path):
    imgs = []
    for i in range(4):
        p = tmp_path / f"{i}.png"
        Image.new("RGB", (1920, 1080), (i * 60, 0, 0)).save(p)
        imgs.append((f"row{i}", p))
    imgs.append(("missing", tmp_path / "none.png"))
    out = tmp_path / "sheet.png"
    sweep.contact_sheet(imgs, out, thumb_w=320, cols=3)
    sheet = Image.open(out)
    assert sheet.width == 3 * 320
    assert sheet.height >= 2 * 180  # two rows of 16:9 thumbnails plus labels
```

- [ ] **Step 2: Run them to verify they fail**

Run: `uv run --with pytest --with numpy --with pillow --with pycocotools pytest scripts/tests/test_bench_sweep.py -v`
Expected: FAIL with `ImportError: cannot import name 'sweep'`.

- [ ] **Step 3: Implement `scripts/bench/sweep.py`**

```python
"""Realism vs cost sweep for buildings and trees (REALISM R3/R4 bench,
docs/superpowers/specs/2026-10-10-r3r4-realism-cost-bench-design.md).

Runs run_bench.py once per row (only the dense_pass phase plus the warm-up, the dense_oblique shot, GPU memory on)
and writes sweep.csv (one line per run), sweep.md (ranges per row, deltas and budget against the all-off row) and
sweep_sheet.png (the dense_oblique shot of every row).

Usage:
  uv run --with numpy --with pillow python scripts/bench/sweep.py --scene PKG --out DIR \
      --axis buildings=off,on --axis buildings_sse=8,16,32,64 [--repeat 2] [--sensor eo] [--dry-run]
"""

from __future__ import annotations

import argparse
import csv
import itertools
import json
import statistics
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# axis -> (CamSim env var, the master axis whose "off" disables it)
AXES: dict[str, tuple[str, str | None]] = {
    "buildings": ("CAMSIM_SCENE_BUILDINGS_ENABLED", None),
    "buildings_sse": ("CAMSIM_SCENE_BUILDINGS_MAXIMUM_SCREEN_SPACE_ERROR", "buildings"),
    "buildings_physics": ("CAMSIM_SCENE_BUILDINGS_CREATE_PHYSICS_MESHES", "buildings"),
    "trees": ("CAMSIM_VEGETATION_ENABLED", None),
    "trees_radius": ("CAMSIM_VEGETATION_RADIUS_M", "trees"),
    "trees_density": ("CAMSIM_VEGETATION_DENSITY_SCALE", "trees"),
    "trees_shadows": ("CAMSIM_VEGETATION_CAST_SHADOWS", "trees"),
}
SWITCH = {"on": "1", "off": "0"}  # CamSim reads env bools as ints
PHASE = "dense_pass"
SHOT = "dense_oblique"
METRICS = ["game_ms_p50", "game_ms_p95", "gpu_ms_p50", "wall_ms_p95", "dropped", "emitted_fps", "gpu_mem_mib_max"]
# Spec section 4: increase of a row's median over the all-off row's median. 400 MB = 381 MiB.
BUDGET = {"game_ms_p50": 1.5, "game_ms_p95": 3.0, "gpu_ms_p50": 5.0, "gpu_mem_mib_max": 381.0}
MIN_FPS = 29.5  # "30 fps": emitted_fps over a 90 s phase reads ~29.9


def parse_axis(text: str) -> tuple[str, list[str]]:
    name, _, vals = text.partition("=")
    if name not in AXES:
        sys.exit(f"unknown axis '{name}' (valid: {', '.join(AXES)})")
    values = [v for v in (x.strip() for x in vals.split(",")) if v]
    if not values:
        sys.exit(f"axis '{name}' has no values")
    return name, values


def build_rows(axes: list[tuple[str, list[str]]]) -> list[dict[str, str]]:
    """Cartesian product in axis order; a dependent axis is dropped from rows where its master is off, and the
    duplicates that leaves are removed (first occurrence kept)."""
    rows: list[dict[str, str]] = []
    for combo in itertools.product(*[v for _, v in axes]) if axes else [()]:
        row = dict(zip([n for n, _ in axes], combo))
        row = {k: v for k, v in row.items() if not (AXES[k][1] and row.get(AXES[k][1]) == "off")}
        if row not in rows:
            rows.append(row)
    return rows


def row_label(row: dict[str, str]) -> str:
    return "_".join(f"{k}-{v}" for k, v in row.items()) or "baseline"


def row_env(row: dict[str, str]) -> dict[str, str]:
    return {AXES[k][0]: SWITCH.get(v, v) for k, v in row.items()}


def run_metrics(results_path: Path) -> dict:
    try:
        ph = json.loads(results_path.read_text())["phases"][PHASE]
    except Exception:  # noqa: BLE001 -- a crashed run leaves no (or a partial) results.json
        return {"status": "failed"}
    if not ph.get("frames"):
        return {"status": "failed"}  # _pct reports 0.0 ms for an empty phase: never read that as fast
    return {"status": "ok", **{m: ph.get(m) for m in METRICS}}


def _median(runs: list[dict], m: str) -> float | None:
    vals = [r[m] for r in runs if r.get(m) is not None]
    return statistics.median(vals) if vals else None


def budget_verdict(row: dict, base: dict) -> dict[str, bool]:
    v = {}
    for m, limit in BUDGET.items():
        a, b = base.get(f"{m}_median"), row.get(f"{m}_median")
        v[m] = a is not None and b is not None and b - a <= limit  # unknown is not a pass
    v["dropped"] = row.get("dropped_median") == 0
    v["fps"] = (row.get("emitted_fps_median") or 0.0) >= MIN_FPS
    return v


def _is_baseline(row: dict[str, str]) -> bool:
    """The all-off row: it names at least one feature switch and every switch it names is off."""
    ms = [k for k in row if AXES[k][1] is None]
    return bool(ms) and all(row[k] == "off" for k in ms)


def aggregate(rows: list[dict], runs: dict[str, list[dict]]) -> list[dict]:
    out = []
    for row in rows:
        label = row_label(row)
        ok = [r for r in runs.get(label, []) if r["status"] == "ok"]
        a = {"label": label, "row": row, "runs": len(runs.get(label, [])), "ok_runs": len(ok),
             "status": "ok" if ok else "failed", "baseline": _is_baseline(row)}
        for m in METRICS:
            vals = [r[m] for r in ok if r.get(m) is not None]
            a[m] = (min(vals), max(vals)) if vals else None
            a[f"{m}_median"] = _median(ok, m)
        out.append(a)
    base = next((a for a in out if a["baseline"] and a["status"] == "ok"), None)
    for a in out:
        a["budget"] = budget_verdict(a, base) if base and a["status"] == "ok" and not a["baseline"] else {}
        a["within_budget"] = all(a["budget"].values()) if a["budget"] else None
    return out


def _rng(v) -> str:
    if v is None:
        return "n/a"
    lo, hi = v
    return f"{lo:.2f}" if lo == hi else f"{lo:.2f}-{hi:.2f}"


def render_md(agg: list[dict]) -> str:
    head = ["row", "status", *METRICS, "budget"]
    lines = ["| " + " | ".join(head) + " |", "|" + " --- |" * len(head)]
    for a in agg:
        verdict = ("baseline" if a["baseline"] else
                   "n/a" if a["within_budget"] is None else
                   "pass" if a["within_budget"] else
                   "FAIL: " + ", ".join(k for k, ok in a["budget"].items() if not ok))
        cells = [a["label"], f"{a['status']} ({a['ok_runs']}/{a['runs']})", *[_rng(a[m]) for m in METRICS], verdict]
        lines.append("| " + " | ".join(cells) + " |")
    lines.append("")
    lines.append(f"Budget (spec section 4): increase of the row median over the all-off row: "
                 f"{', '.join(f'{k} <= {v}' for k, v in BUDGET.items())}; dropped 0; fps >= {MIN_FPS}. "
                 f"Memory in MiB (n/a: not measurable on this host). Ranges are over repeats.")
    return "\n".join(lines) + "\n"


def write_csv(path: Path, rows: list[dict], runs: dict[str, list[dict]]) -> None:
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["row", "repeat", "status", *METRICS])
        for row in rows:
            for i, r in enumerate(runs.get(row_label(row), [])):
                w.writerow([row_label(row), i, r["status"], *[r.get(m, "") for m in METRICS]])


def contact_sheet(images: list[tuple[str, Path]], out: Path, thumb_w: int = 640, cols: int = 3) -> None:
    from PIL import Image, ImageDraw

    thumb_h = thumb_w * 9 // 16
    label_h = 24
    n = len(images)
    rows = (n + cols - 1) // cols
    sheet = Image.new("RGB", (cols * thumb_w, rows * (thumb_h + label_h)), (32, 32, 32))
    draw = ImageDraw.Draw(sheet)
    for i, (label, path) in enumerate(images):
        x, y = (i % cols) * thumb_w, (i // cols) * (thumb_h + label_h)
        if path.exists():
            im = Image.open(path).convert("RGB")
            im.thumbnail((thumb_w, thumb_h))
            sheet.paste(im, (x, y + label_h))
        else:
            draw.text((x + 8, y + label_h + thumb_h // 2), "no shot (run failed)", fill=(255, 96, 96))
        draw.text((x + 8, y + 6), label, fill=(255, 255, 255))
    sheet.save(out)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--scene", type=Path, required=True, help="scene package (absolute path)")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--axis", action="append", default=[], metavar="NAME=V1,V2")
    ap.add_argument("--repeat", type=int, default=2)
    ap.add_argument("--site", default="pendleton")
    ap.add_argument("--sensor", choices=["eo", "ir"], default="eo")
    ap.add_argument("--dry-run", action="store_true", help="print the rows and commands, run nothing")
    a = ap.parse_args(argv)
    axes = [parse_axis(t) for t in a.axis]
    rows = build_rows(axes)
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    runs: dict[str, list[dict]] = {}
    for row in rows:
        label = row_label(row)
        for k in range(a.repeat):
            run_dir = out / "rows" / label / f"r{k}"
            cmd = [sys.executable, str(REPO / "scripts" / "bench" / "run_bench.py"),
                   "--label", f"{label}-r{k}", "--site", a.site, "--out", str(run_dir),
                   "--phases", PHASE, "--shots", SHOT, "--gpu-mem", "--sensor", a.sensor,
                   "--env", f"CAMSIM_SCENE_DIR={a.scene.resolve()}"]
            for key, val in row_env(row).items():
                cmd += ["--env", f"{key}={val}"]
            print(f"[sweep] {label} r{k}: {' '.join(cmd)}", flush=True)
            if a.dry_run:
                continue
            subprocess.run(cmd, check=False)  # a failed run is recorded as failed, the sweep goes on
            runs.setdefault(label, []).append(run_metrics(run_dir / "results.json"))
    if a.dry_run:
        return 0
    agg = aggregate(rows, runs)
    write_csv(out / "sweep.csv", rows, runs)
    (out / "sweep.md").write_text(render_md(agg))
    (out / "shots").mkdir(exist_ok=True)
    sheet = []
    for row in rows:
        label = row_label(row)
        src = out / "rows" / label / "r0" / "shots" / f"{SHOT}.png"
        dst = out / "shots" / f"{label}.png"
        if src.exists():
            dst.write_bytes(src.read_bytes())
        sheet.append((label, dst))
    contact_sheet(sheet, out / "sweep_sheet.png")
    print((out / "sweep.md").read_text())
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `uv run --with pytest --with numpy --with pillow --with pycocotools pytest scripts/tests/ -v`
Expected: PASS.

- [ ] **Step 5: Dry run**

Run: `uv run --with numpy --with pillow python scripts/bench/sweep.py --scene "$PWD/.cache/scene-packages/pendleton-r2" --out .cache/bench-r3r4/dry --axis buildings=off,on --axis buildings_sse=16,32 --axis trees=off,on --repeat 1 --dry-run`
Expected: 6 commands (2 × 3 buildings rows: off, on/16, on/32 × trees off/on), each with `--phases dense_pass --shots dense_oblique --gpu-mem` and the right `--env` lines; nothing launched.

- [ ] **Step 6: Commit**

```bash
git add scripts/bench/sweep.py scripts/tests/test_bench_sweep.py
git commit -m "feat(bench): sweep.py realism-vs-cost table and contact sheet (REALISM R3/R4 bench)"
```

---

### Task 8: Docs and the live end-to-end check

**Files:**
- Modify: `scripts/bench/README.md`
- Modify: `CLAUDE.md` (Commands table)

**Interfaces:**
- Consumes: everything above.

- [ ] **Step 1: README**

In `scripts/bench/README.md`, extend the `--site pendleton` paragraph:

```markdown
`--site pendleton` also appends `dense_pass` (REALISM R3/R4 bench): 100 m/s for 90 s at the start point's terrain
+ 300 m HAE, looking ahead 20 deg down, along the path in `dense_path.json` over built-up land and tree cover. The
warm-up grows to 180 s to fly it once, and the shots gain `dense_oblique` (the path's midpoint). The path comes from
`select_dense_path.py PKG` (rule in its docstring; built-up land stands in for buildings, so it does not depend on the
buildings layer): `uv run --project scripts/scene python scripts/bench/select_dense_path.py PKG`.
```

Add to the flags paragraph:

```markdown
`--phases A,B` (measured phases to fly; the warm-up still runs), `--shots A,B`, `--gpu-mem` (samples GPU memory every
5 s into `gpu_mem_mib_max` per phase: `nvidia-smi memory.used` on Linux, `footprint -p` on macOS; `null` where neither
works, e.g. macOS Docker; `meta.gpu_mem_source` says which).
```

Add a `## Realism vs cost sweep` section:

```markdown
## Realism vs cost sweep (REALISM R3/R4)

    uv run --with numpy --with pillow python scripts/bench/sweep.py --scene "$PWD/.cache/scene-packages/<pkg>" \
        --out .cache/bench-r3r4/<name> --axis buildings=off,on --axis buildings_sse=8,16,32,64 --repeat 2

One `run_bench.py` per row and repeat (`dense_pass` only, plus the warm-up; ~5 min each). Axes: `buildings`,
`buildings_sse`, `buildings_physics`, `trees`, `trees_radius`, `trees_density`, `trees_shadows` (switches `on`/`off`;
axes of a feature that is `off` collapse). Output: `sweep.csv` (one line per run), `sweep.md` (ranges per row; rows
are judged against the row with every feature `off`, using the budget in
`docs/superpowers/specs/2026-10-10-r3r4-realism-cost-bench-design.md` §4), `sweep_sheet.png` + `shots/<row>.png`
(the `dense_oblique` shot per row). A crashed run or an empty phase marks the row `failed`. Use one-factor sweeps
around a candidate default plus the both-on row; the full product is hours.
```

Update the "Reading it" list with `game_ms_p95` and `gpu_mem_mib_max`.

- [ ] **Step 2: CLAUDE.md**

In the Commands table, after the `scripts/bench/run_bench.py` row, add:

```markdown
| `scripts/bench/sweep.py --scene PKG --out DIR --axis NAME=V1,V2` | Realism-vs-cost sweep (REALISM R3/R4): one `dense_pass` bench per buildings/trees setting → `sweep.md` cost table vs the all-off row + `sweep_sheet.png` |
```

and append ` (`--site pendleton` adds `dense_pass`; `--phases`, `--shots`, `--gpu-mem`)` to the `run_bench.py` row's description.

- [ ] **Step 3: Live end-to-end (one-row sweep, the both-off reference)**

Run: `uv run --with numpy --with pillow python scripts/bench/sweep.py --scene "$PWD/.cache/scene-packages/pendleton-r2" --out .cache/bench-r3r4/reference --repeat 2`
Expected:
- one `baseline` row, status `ok (2/2)`;
- `sweep.md` lists game p50/p95, GPU p50, wall p95, dropped 0, fps ≈ 29.9 and GPU memory (or `n/a`);
- `sweep_sheet.png` shows the `dense_oblique` shot: built-up land and tree cover visible ahead.

Record the table in the task report: it is the R2 reference for `dense_pass` that the R3/R4 slices compare against.

- [ ] **Step 4: Full Pendleton bench still works**

Run: `uv run --with numpy --with pillow python scripts/bench/run_bench.py --label r3r4-bench-check --site pendleton --env CAMSIM_SCENE_DIR="$PWD/.cache/scene-packages/pendleton-r2" --out .cache/bench-r3r4/full-check`
Expected: phases `orbit, slew, low_pass, far_origin, coast_pass, dense_pass` in `results.json`, all 30 fps / 0 dropped; game p50 of the five old phases within noise of the R2 table in `docs/scene-packages.md` ("R2 vector features", bench table); 13 shots (+ `_sensor` copies).

- [ ] **Step 5: Run the full unit suite**

Run: `uv run --with pytest --with numpy --with pillow --with pycocotools pytest scripts/tests/ -v && uv run --project scripts/scene --with pytest pytest scripts/tests/test_bench_dense_path.py -v`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add scripts/bench/README.md CLAUDE.md
git commit -m "docs(bench): dense_pass, --gpu-mem and the realism-vs-cost sweep (REALISM R3/R4 bench)"
```
