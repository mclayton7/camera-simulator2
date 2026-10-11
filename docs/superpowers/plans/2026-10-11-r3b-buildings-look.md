# Buildings That Look and Behave Right (REALISM R3b) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The building footprints a scene package already keeps become, at build time, a fine 1-bit mask (trees stay out of buildings with no runtime sampling), a land-cover burn of code 230 (infrared and CIGI see "building"), and a packed roof texture atlas per buildings tile cut from the imagery sources; CamSim reads the mask for the tree scatter and gives code 230 a new `building_roof` thermal class.

**Architecture:** Offline, one cached step rasterises the buildings layer's own kept buildings (same filter, same `source_id` order) on the land-cover tile grid at 8 × its resolution (`camsim_scene/footprints.py`, cached under `.state/`). The buildings layer writes that raster as `features/buildings/mask/` and the land-cover layer burns it as 230 between paved areas and rail. Roof pixels are sampled from the imagery layer's sources through `imagery.sample_entries` + `decode_entries`, packed by a pure deterministic shelf packer (`roofatlas.py`) into one power-of-two JPEG per GLB, with `TEXCOORD_0` on the shared vertex arrays. At runtime `ResolvePackage` exposes the mask directory, `FVegetationBuildingMask` decodes mask tiles on the scatter worker (8-tile LRU), `GenerateCell` takes an optional clearance predicate, and the method order is mask → runtime tileset check → none. Thermal gains a 16th built-in class; no shader changes.

**Tech Stack:** Python 3.12–3.13 (`uv`, numpy, shapely ≥ 2.2, rasterio, Pillow), pytest; Node (`3d-tiles-validator` 0.6.1, test-only); UE 5.8 C++ (Cesium for Unreal 2.29, ImageWrapper), UE Automation tests (NullRHI); bench scripts (`scripts/bench/run_bench.py`, `sweep.py`).

**Spec:** `docs/superpowers/specs/2026-10-11-r3b-buildings-look-design.md` (binding; read it first). Builds on `docs/superpowers/specs/2026-10-10-r3-buildings-slice-design.md` and `docs/superpowers/specs/2026-10-10-r4-trees-slice-design.md`; bench budget from `docs/superpowers/specs/2026-10-10-r3r4-realism-cost-bench-design.md`.

## Global Constraints

- Altitudes are WGS-84 ellipsoid heights everywhere (`CLAUDE.md`, "Altitudes"); building base/roof heights, pose altitudes and `ground_hae` values below are ellipsoid.
- Packages without `[buildings]` stay byte-identical to R3: same `manifest.json`, `hashes.txt` and every file. The landcover layer reads buildings only when the buildings layer is configured.
- Mask: `features/buildings/mask/`; tiles share the land-cover grid and naming (0.05° tiles named by SW corner, row 0 = north); each tile **4800 × 4800** (8 × land cover), 1-bit PNG (mode `1`), a cell set where its centre is inside a footprint, holes excluded; a tile with no set cell is not written; `index.json` lists the written tiles, the resolution and the rules version.
- Land-cover burn: a cell becomes **230** when **≥ 32 of its 8 × 8** mask cells are set. Priority, later wins: WorldCover, water, paved areas, **buildings**, rail, roads. `index.json` `codes` gains 230 when present.
- `BUILDINGS_RULES_VERSION` is bumped; the landcover inputs hash gains the buildings snapshot sha, the buildings layer settings hash and the buildings rules version. Mask and burn are byte-identical across rebuilds (deterministic rasterisation, PNG `optimize=True`).
- Roof atlas: every buildings tile at every tier gets one; walls keep the flat wall colour `(0.42, 0.41, 0.40)`; flat-roof fallback keeps `(0.30, 0.30, 0.31)`. Default texel `buildings.roof_texel_m` = **0.5 m**; a roof whose best source is coarser than **2 m** keeps the flat roof. One rectangle per roof AABB in tile-local ENU metres, **2 px gutter** filled by edge extension; shelf packer ordered by rectangle height in pixels descending, then `source_id` ascending; atlas square, power of two, ≤ `buildings.roof_texture_max_px` (default **2048**); texel steps **0.5, 0.75, 1.0, 1.5, 2.0 m**; a roof that does not fit at 2.0 m keeps the flat roof; chosen texel recorded per tile (`roof_texel_m`).
- GLB: roof vertices gain `TEXCOORD_0`, wall vertices (0, 0), arrays shared and non-indexed; textured roof material = `baseColorTexture`, one embedded JPEG (`image/jpeg`, quality 85, 4:2:0), sampler mag LINEAR / min LINEAR_MIPMAP_LINEAR, wrap CLAMP_TO_EDGE, `baseColorFactor` 1; up to three primitives (textured roof, flat roof, wall). Tiles pass `3d-tiles-validator` 0.6.1 with 0 errors.
- New `[buildings]` keys (in the buildings settings hash): `roof_textures` (true), `roof_texel_m` (0.5), `roof_texture_max_px` (2048).
- New runtime key: `vegetation.building_clearance_m`, env `CAMSIM_VEGETATION_BUILDING_CLEARANCE_M`, default **2.0**, range **[0, 10]**. `vegetation.drop_in_buildings` (default true) gates both methods. Without a mask every cell hash is bit-identical to the R3/R4 build.
- `FVegetationBuildingMask`: decoded tiles are bitsets (~2.9 MB), **8-tile LRU** (~23 MB), separate from the land-cover sampler; a missing tile means no buildings.
- Thermal: new built-in `building_roof`, the **16th** (16 of 32 slots free); `DefaultMaterialName(230) = "building_roof"`; code 50 keeps `built_up`; class-table data only, no shader change; `CamSim.GPU.Thermal.LandCoverMatchesCpu` and `MatchesCpu` still hold; `thermal.land_cover.enabled: false` leaves IR unchanged.
- CIGI: no code change.
- Bench gates (both features on at defaults, vs all-off): game p95 **≤ +3.0 ms**, memory **≤ +1,024 MiB** over the whole run, **30 fps** and **0 dropped**; if memory is over, rebuild once with `roof_texel_m = 1.0` and report both. No default changes beyond the spec's.
- Code style per `CLAUDE.md`: UE naming, `// Copyright CamSim Contributors. All Rights Reserved.` on every new C++ file (and the Python scripts that carry it), `#include "CoreMinimal.h"` first, `.generated.h` last; Python ruff line length 120.
- Never stage into `Saved/StagedBuilds/`; never put Cesium in the project's `Plugins/`.

## Review Focus

1. **A building that straddles a land-cover tile edge.** Expect its mask cells split between both tiles with no cell lost or doubled, and each tile's burn counted from its own cells. Test: `test_mask_building_across_tile_edge_sets_both_tiles` (Task 1).
2. **Imagery with a nodata hole under part of a roof** (NAIP's edge crossing a roof). Expect the roof still textured from its valid texels, with the invalid ones filled by the roof's mean colour, never black. Test: `test_roof_partial_nodata_fills_with_the_roof_mean` (Task 4).
3. **Only a roof setting changes** (the memory contingency, `roof_texel_m` 0.5 → 1.0). Expect the GLBs rebuilt and the mask and land-cover burn byte-identical. Test: `test_roof_texel_change_rebuilds_tiles_not_the_mask` (Task 5).
4. **A bad mask tile at runtime** (git LFS pointer, wrong size, truncated). Expect "no buildings" there with one warning, no crash, and the other tiles still answering. Test: `CamSim.Vegetation.BuildingMask.BadTilesAreClear` (Task 6).
5. **A package whose imagery has no sub-2 m source** (`preview` profile, or NAIP missing). Expect every roof flat, no texture in the GLB, `roof_texel_m` null, and the tile still valid. Test: `test_all_coarse_tile_has_no_texture_and_null_texel` (Task 4).

---

## File Structure

**Scene tooling (`scripts/scene/`):**

| File | Responsibility |
|---|---|
| `camsim_scene/footprints.py` (new) | Footprint raster on the land-cover grid (8 ×): rasterise, pack, burn rule, mask PNG + index, raster cache file |
| `camsim_scene/roofatlas.py` (new) | Pure packer: texel ladder, rectangles with gutter, deterministic shelf pack, power-of-two size, fallback |
| `camsim_scene/roofs.py` (new) | Roof atlas for one tile: coarse-source test, sampling, gutter fill, texcoords, JPEG |
| `camsim_scene/buildings.py` (modify) | `BUILDINGS_RULES_VERSION` "2"; `TileGeometry` / `tile_geometry`; `tile_mesh(..., roof, sampler, stats)` |
| `camsim_scene/gltf.py` (modify) | `TEXCOORD_0`, textured material, image/texture/sampler, root `extras`; decoder reads them |
| `camsim_scene/layers/buildings.py` (modify) | `footprint_inputs`, `plan_cached`, `footprint_raster`, mask writer, `ImageryRoofSampler`, roof inputs |
| `camsim_scene/features.py` (modify) | `PRIORITY` with `building`, `CODES[230]`, `BUILDING_CODE`, `burn_tile(..., buildings)` |
| `camsim_scene/pipeline.py` (modify) | `_cut_landcover(..., footprints)`, `_build_landcover` burns 230 and hashes the buildings inputs |
| `camsim_scene/config.py` (modify) | roof keys, `buildings_settings` `mask`/`roof` entries, `landcover.buildings` settings |
| `camsim_scene/sources/base.py` (modify) | `native_res_m(raster)` |
| `camsim_scene/verify.py` (modify) | mask files in the quick check; deep `buildings_mask` (mask ↔ burn) and `buildings_roofs` |
| `tests/test_footprints.py`, `tests/test_landcover_buildings.py`, `tests/test_roofatlas.py`, `tests/test_roofs.py` (new); `tests/test_features.py`, `tests/test_gltf.py`, `tests/test_buildings_layer.py`, `tests/test_buildings_verify.py`, `tests/test_config.py`, `tests/fake_sources.py` (modify) | tests and fixtures |
| `tools/burn_probe_points.py`, `tests/test_burn_probe_points.py` (new) | acceptance: CIGI probe points in/out of a burned cell, roof-view statistics |
| `examples/pendleton.toml` (modify) | the three roof keys, explicit |

**Runtime (`unreal_project/CamSimTest/Source/CamSimTest/`):**

| File | Responsibility |
|---|---|
| `Vegetation/VegetationBuildingMask.h/.cpp` (new) | mask `index.json`, 1-bit PNG → bitset, thread-safe 8-tile LRU, `IsMasked`, 5-point `IsClear` |
| `Vegetation/VegetationScatter.h/.cpp` (modify) | `GenerateCell` overload with an `IsClear` predicate |
| `Vegetation/VegetationManager.h/.cpp` (modify) | `EBuildingExclusion`, `ChooseBuildingExclusion`, `DescribeBuildingExclusion`, wiring in `CreateLiveManager` |
| `Config/CamSimConfig.h/.cpp` (modify) | `Scene.BuildingsMaskDir`, `Vegetation.BuildingClearanceM` (yaml, env, validation) |
| `Config/ScenePackage.cpp/.h` (modify) | resolve `features/buildings/mask/index.json` |
| `Thermal/ThermalMaterials.h/.cpp`, `Thermal/LandCoverClasses.cpp` (modify) | `building_roof` (index 15), code 230 |
| `Tests/VegetationBuildingMaskTest.cpp` (new); `Tests/VegetationScatterTest.cpp`, `Tests/VegetationConfigTest.cpp`, `Tests/VegetationManagerTest.cpp`, `Tests/ScenePackageTest.cpp`, `Tests/ThermalLandCoverMaterialsTest.cpp`, `Tests/ThermalLandCoverConfigTest.cpp` (modify) | tests |

**Acceptance scripts (`scripts/`):** `bench/dense_path.py` (`BuildingMask`), `vegetation_check.py` (`footprints` stage), `thermal_check.py` (`roofs` run, gates s / s_air), `tests/test_bench_dense_path.py`, `tests/test_vegetation_footprints.py`, `tests/test_thermal_check_roofs.py`.

**Docs:** `docs/scene-packages.md`, `docs/vegetation.md`, `docs/thermal.md`, `docs/configuration.md`, `deploy/camsim_config.yaml`, `docs/terrain-feedback.md`, `CLAUDE.md`, `ROADMAP.md`, `REALISM.md`.

## Commands

- Scene tooling tests: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests -q` (`-k <name>` to narrow).
- Validator (first use): `(cd scripts/scene/tests/validator && npm ci && npm rebuild sharp --ignore-scripts=false)`.
- Script tests: `uv run --with pytest --with numpy --with pillow --with pycocotools pytest scripts/tests -q`.
- Build editor: `scripts/run.sh --build-only` (never pipe through `tee` without `set -o pipefail`).
- UE automation, headless (macOS; on Linux use `UE_BIN=/opt/UE/Engine/Binaries/Linux/UnrealEditor` and drop `-DisablePython`):
  ```bash
  UE_BIN="/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"
  "$UE_BIN" "$PWD/unreal_project/CamSimTest/CamSimTest.uproject" \
    -ExecCmds="Automation RunTests CamSim.Vegetation+Quit" \
    -TestExit="Automation Test Queue Empty" \
    -ReportExportPath="$PWD/.cache/automation-report" \
    -unattended -nullrhi -nosound -nosplash -DisablePython -log -stdout -FullStdOutLogOutput > .cache/automation.log 2>&1
  python3 scripts/parse_automation_report.py .cache/automation-report/index.json
  ```
  Replace `CamSim.Vegetation` with the filter each task names. `parse_automation_report.py` exits 0 when every test passed, 1 on a failure, 2 if the report is missing.
- GPU tests: `scripts/run_gpu_tests.sh CamSim.GPU.Thermal` (Metal on macOS).
- Packages used live: `PKG3="$PWD/.cache/scene-packages/pendleton-r3"` (R3, read only) and `PKG="$PWD/.cache/scene-packages/pendleton-r4"` (built in Task 11). Absolute paths.

---
### Task 1: Footprint raster, mask tiles and the shared cached step

**Files:**
- Create: `scripts/scene/camsim_scene/footprints.py`, `scripts/scene/tests/test_footprints.py`
- Modify: `scripts/scene/camsim_scene/buildings.py:17` (`BUILDINGS_RULES_VERSION`), `scripts/scene/camsim_scene/layers/buildings.py` (whole file), `scripts/scene/camsim_scene/config.py:269-285` (`buildings_settings`)

**Interfaces:**
- Consumes: `buildings.Building` (`.parts`, `.source_id`), `buildings.plan_buildings`, `layers.buildings.plan_from_manifest(m, ctx, st)`, `layers.landcover.tile_filename(i, j)`, `layers.landcover.tile_range(lo, hi)`, `sources.worldcover.TILE_DEG` / `TILE_PX`, `fsutil.atomic_write`, `engine.inputs_hash`, `manifest.STATE_DIR` (`.state`).
- Produces (later tasks rely on these exact names):
  - `footprints.MASK_FACTOR = 8`, `MASK_PX = 4800`, `MASK_FORMAT = "camsim-buildings-mask-1"`, `MASK_RULES_VERSION = "1"`, `BURN_MIN_CELLS = 32`.
  - `footprints.rasterise_tile(parts: list, i: int, j: int) -> np.ndarray` (bool, `(MASK_PX, MASK_PX)`, row 0 = north).
  - `footprints.raster_tiles(buildings: list) -> dict[tuple[int, int], np.ndarray]` (packed rows, `uint8 (MASK_PX, MASK_PX // 8)`, only tiles with a set cell, keys sorted).
  - `footprints.unpack(packed: np.ndarray) -> np.ndarray` (bool), `footprints.burn_cells(mask: np.ndarray) -> np.ndarray` (bool `(TILE_PX, TILE_PX)`, ≥ 32 of 64).
  - `footprints.mask_png(packed) -> bytes`, `footprints.write_mask_tile(packed, path: Path) -> None`, `footprints.mask_index(written: list[tuple[int, int]]) -> dict`.
  - `footprints.save_raster(raster: dict, path: Path) -> None`, `footprints.load_raster(path: Path) -> dict[tuple[int, int], np.ndarray]`.
  - `layers.buildings.MASK = Path("features/buildings/mask")`, `footprint_inputs(m: Manifest, wst: WorkerState) -> str`, `plan_cached(pkg: Path, m: Manifest, ctx: BuildContext, wst: WorkerState | None = None) -> tuple[list[Building], dict]`, `footprint_raster(pkg, m, ctx, wst=None) -> tuple[Path, str]` (the cached raster file and its inputs key), `write_mask(pkg: Path, raster: dict) -> str` (sha256 of the written `index.json`, "" when nothing was written).
  - Buildings layer settings gain `"mask": {"tile_px": 4800, "cell_factor": 8, "rules": "1"}`; the layer writes the mask only when that key is present (R3 manifests build as before).

- [ ] **Step 1: Write the failing tests**

`scripts/scene/tests/test_footprints.py`:

```python
import json

import numpy as np
import shapely
from fake_sources import build_synthetic
from PIL import Image
from test_buildings_layer import ROWS, _scene

from camsim_scene import buildings as B
from camsim_scene import footprints as F

D = F.TILE_DEG / F.MASK_PX  # one mask cell, degrees
I0, J0 = 200, 200  # tile +10.00_+10.00: lat 10.00-10.05, lon 10.00-10.05


def _ring(x0, y0, x1, y1, i=I0, j=J0):
    """A rectangle in cell units of tile (i, j): x east from its west edge, y south from its north edge."""
    w, n = j * F.TILE_DEG, (i + 1) * F.TILE_DEG
    return [(w + x0 * D, n - y0 * D), (w + x1 * D, n - y0 * D), (w + x1 * D, n - y1 * D), (w + x0 * D, n - y1 * D)]


def _bld(sid, poly):
    return B.Building(sid, [poly], "", 4.0, 2, 0.0, 4.5, 100.0, (15, 0, 0))


def _centres(rows: slice, cols: slice, i=I0, j=J0):
    w, n = j * F.TILE_DEG, (i + 1) * F.TILE_DEG
    lon = w + (np.arange(F.MASK_PX)[cols] + 0.5) * D
    lat = n - (np.arange(F.MASK_PX)[rows] + 0.5) * D
    return np.meshgrid(lon, lat)


def test_polygon_with_hole_sets_exactly_the_centre_cells():
    # edges a quarter cell off the grid, so no cell centre lies on an edge
    poly = shapely.Polygon(_ring(100.25, 200.25, 160.25, 240.25), [_ring(120.25, 210.25, 130.25, 220.25)])
    m = F.rasterise_tile([poly], I0, J0)
    assert m.shape == (F.MASK_PX, F.MASK_PX) and m.dtype == bool
    assert m.sum() == 60 * 40 - 10 * 10  # cols 100..159, rows 200..239, minus the hole's cols 120..129, rows 210..219
    rows, cols = slice(190, 250), slice(90, 170)
    lon, lat = _centres(rows, cols)
    assert np.array_equal(m[rows, cols], shapely.contains_xy(poly, lon, lat))
    assert not m[215, 125] and m[205, 125]


def test_raster_skips_tiles_without_buildings_and_packs_rows():
    poly = shapely.Polygon(_ring(10.25, 10.25, 20.25, 20.25))
    r = F.raster_tiles([_bld("a", poly)])
    assert list(r) == [(I0, J0)]
    assert r[(I0, J0)].dtype == np.uint8 and r[(I0, J0)].shape == (F.MASK_PX, F.MASK_PX // 8)
    assert F.unpack(r[(I0, J0)]).sum() == 100
    assert F.raster_tiles([]) == {}


def test_mask_building_across_tile_edge_sets_both_tiles():
    # x from 10.25 cells west of the tile's east edge to 9.75 cells east of it (in the next tile)
    w, n = J0 * F.TILE_DEG, (I0 + 1) * F.TILE_DEG
    e = w + F.TILE_DEG
    poly = shapely.box(e - 10.25 * D, n - 110.25 * D, e + 9.75 * D, n - 100.25 * D)
    r = F.raster_tiles([_bld("edge", poly)])
    assert sorted(r) == [(I0, J0), (I0, J0 + 1)]
    west, east = F.unpack(r[(I0, J0)]), F.unpack(r[(I0, J0 + 1)])
    assert west.sum() == 100 and west[100:110, F.MASK_PX - 10 :].all()
    assert east.sum() == 100 and east[100:110, :10].all()


def test_burn_cells_half_cover_rule():
    m = np.zeros((F.MASK_PX, F.MASK_PX), bool)
    m[0:8, 0:8].flat[:31] = True  # land-cover cell (0, 0): 31 of 64
    m[0:8, 8:16].flat[:32] = True  # cell (0, 1): 32 of 64
    b = F.burn_cells(m)
    assert b.shape == (F.TILE_PX, F.TILE_PX)
    assert not b[0, 0] and b[0, 1] and b.sum() == 1


def test_mask_png_is_one_bit_and_round_trips(tmp_path):
    m = np.zeros((F.MASK_PX, F.MASK_PX), bool)
    m[5:9, 4790:4800] = True
    p = tmp_path / "t.png"
    F.write_mask_tile(np.packbits(m, axis=1), p)
    im = Image.open(p)
    assert im.mode == "1" and im.size == (F.MASK_PX, F.MASK_PX)
    assert np.array_equal(np.array(im), m)
    assert F.mask_png(np.packbits(m, axis=1)) == p.read_bytes()  # deterministic bytes


def test_raster_file_round_trip(tmp_path):
    r = F.raster_tiles([_bld("a", shapely.Polygon(_ring(1.25, 1.25, 3.25, 3.25)))])
    F.save_raster(r, tmp_path / "r.npz")
    back = F.load_raster(tmp_path / "r.npz")
    assert list(back) == list(r) and np.array_equal(back[(I0, J0)], r[(I0, J0)])
    F.save_raster({}, tmp_path / "e.npz")
    assert F.load_raster(tmp_path / "e.npz") == {}


def test_layer_writes_mask_tiles_and_index(tmp_path):
    pkg, _, _ = build_synthetic(tmp_path, _scene(tmp_path, ROWS))
    d = pkg / "features/buildings/mask"
    index = json.loads((d / "index.json").read_text())
    assert index["format"] == F.MASK_FORMAT and index["tile_px"] == 4800 and index["tile_deg"] == 0.05
    assert index["cell_factor"] == 8 and index["rules"] == F.MASK_RULES_VERSION
    # b1 (60 x 50 m centred on 10.20, 10.20) straddles both tile edges; b2 straddles the 10.20 N edge
    files = ["+10.15_+10.15.png", "+10.15_+10.20.png", "+10.20_+10.15.png", "+10.20_+10.20.png"]
    assert [t["file"] for t in index["tiles"]] == files
    total = sum(int(np.array(Image.open(d / f)).sum()) for f in files)
    assert 2000 < total < 3000  # 3,184 m2 of footprint at ~1.14 m x 1.16 m cells (lat 10): ~2,400 cells
    hashes = (pkg / "hashes.txt").read_text()
    assert "features/buildings/mask/index.json" in hashes and "features/buildings/mask/+10.20_+10.20.png" in hashes


def test_plan_runs_once_per_build(tmp_path, monkeypatch):
    calls = []
    real = B.plan_buildings
    monkeypatch.setattr(B, "plan_buildings", lambda *a, **k: (calls.append(1), real(*a, **k))[1])
    build_synthetic(tmp_path, _scene(tmp_path, ROWS))
    assert len(calls) == 1  # the GLBs and the footprint raster share one cached plan
```

- [ ] **Step 2: Run them to verify they fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_footprints.py -q`
Expected: FAIL at collection with `ModuleNotFoundError: No module named 'camsim_scene.footprints'`.

- [ ] **Step 3: Write `footprints.py`**

`scripts/scene/camsim_scene/footprints.py`:

```python
"""Building footprint raster (REALISM R3b, spec docs/superpowers/specs/2026-10-11-r3b-buildings-look-design.md
section 1): the buildings layer's kept buildings rasterised once on the land-cover tile grid at MASK_FACTOR x its
resolution. It is the single source of the fine mask (features/buildings/mask/, read by the tree scatter) and of the
land-cover burn (code 230), so the two cannot disagree. A cell is set when its centre is inside a footprint (holes
excluded). Rasterised in lon/lat, where the grid is regular, so a cell belongs to exactly one tile."""

from __future__ import annotations

import io
import json
from pathlib import Path

import numpy as np
from PIL import Image

from .fsutil import atomic_write
from .layers.landcover import tile_filename, tile_range
from .sources.worldcover import TILE_DEG, TILE_PX

MASK_FACTOR = 8
MASK_PX = TILE_PX * MASK_FACTOR  # 4800: ~1.16 m x 0.97 m per cell at 33 N
MASK_FORMAT = "camsim-buildings-mask-1"
MASK_RULES_VERSION = "1"  # bump on any change to the rasterisation or the burn rule (it is in both layers' settings)
BURN_MIN_CELLS = 32  # of the MASK_FACTOR^2 mask cells under a land-cover cell: at least half of it covered


def rasterise_tile(parts: list, i: int, j: int) -> np.ndarray:
    """(MASK_PX, MASK_PX) bool for tile (lat index i, lon index j), row 0 = north: cell centres inside `parts`."""
    from rasterio import features as rf
    from rasterio.transform import Affine

    if not parts:
        return np.zeros((MASK_PX, MASK_PX), bool)
    d = TILE_DEG / MASK_PX
    transform = Affine(d, 0.0, j * TILE_DEG, 0.0, -d, (i + 1) * TILE_DEG)
    out = rf.rasterize(
        [(p, 1) for p in parts], out_shape=(MASK_PX, MASK_PX), transform=transform, fill=0, all_touched=False,
        dtype="uint8",
    )
    return out > 0


def raster_tiles(buildings: list) -> dict[tuple[int, int], np.ndarray]:
    """Every tile with at least one set cell -> its rows packed MSB first (np.packbits(axis=1)), in sorted key order.
    Binary union, so the result does not depend on the buildings' order."""
    by_tile: dict[tuple[int, int], list] = {}
    for b in buildings:
        for p in b.parts:
            w, s, e, n = p.bounds
            for i in tile_range(s, n):
                for j in tile_range(w, e):
                    by_tile.setdefault((i, j), []).append(p)
    out = {}
    for key in sorted(by_tile):
        m = rasterise_tile(by_tile[key], *key)
        if m.any():
            out[key] = np.packbits(m, axis=1)
    return out


def unpack(packed: np.ndarray) -> np.ndarray:
    return np.unpackbits(packed, axis=1, count=MASK_PX).astype(bool)


def burn_cells(mask: np.ndarray) -> np.ndarray:
    """(TILE_PX, TILE_PX) bool: the land-cover cells with at least BURN_MIN_CELLS of their mask cells set."""
    cover = mask.reshape(TILE_PX, MASK_FACTOR, TILE_PX, MASK_FACTOR).sum(axis=(1, 3))
    return cover >= BURN_MIN_CELLS


def mask_png(packed: np.ndarray) -> bytes:
    buf = io.BytesIO()
    Image.frombytes("1", (MASK_PX, MASK_PX), np.ascontiguousarray(packed).tobytes()).save(buf, "PNG", optimize=True)
    return buf.getvalue()


def write_mask_tile(packed: np.ndarray, path: Path) -> None:
    atomic_write(Path(path), mask_png(packed))


def mask_index(written: list[tuple[int, int]]) -> dict:
    return {
        "format": MASK_FORMAT,
        "tile_deg": TILE_DEG,
        "tile_px": MASK_PX,
        "cell_factor": MASK_FACTOR,
        "rule": "cell centre inside a building footprint, holes excluded",
        "rules": MASK_RULES_VERSION,
        "tiles": [{"file": tile_filename(i, j), "lat_index": i, "lon_index": j} for i, j in written],
    }


def mask_index_bytes(written: list[tuple[int, int]]) -> bytes:
    return (json.dumps(mask_index(written), indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def save_raster(raster: dict[tuple[int, int], np.ndarray], path: Path) -> None:
    buf = io.BytesIO()
    np.savez(buf, **{f"{i}_{j}": a for (i, j), a in sorted(raster.items())})
    atomic_write(Path(path), buf.getvalue())


def load_raster(path: Path) -> dict[tuple[int, int], np.ndarray]:
    with np.load(Path(path)) as z:
        return {tuple(int(v) for v in k.split("_")): z[k] for k in sorted(z.files, key=lambda k: tuple(map(int, k.split("_"))))}
```

- [ ] **Step 4: Bump the buildings rules and add the mask settings**

In `scripts/scene/camsim_scene/buildings.py` line 17:

```python
BUILDINGS_RULES_VERSION = "2"  # bump when geometry/height/tier rules change (it is in the layer settings hash). 2: R3b footprint mask + roof atlas
```

In `scripts/scene/camsim_scene/config.py`, `buildings_settings`, import and add the entry:

```python
def buildings_settings(plan: ScenePlan) -> dict | None:
    """The buildings layer's settings (REALISM R3), or None when the layer is off."""
    from .buildings import BUILDINGS_RULES_VERSION, GEOMETRIC_ERROR, LEAF_ZOOM, TIER_MIN_AREA_M2
    from .footprints import MASK_FACTOR, MASK_PX, MASK_RULES_VERSION

    if not plan.priorities["buildings"]:
        return None
    return {
        "priorities": list(plan.priorities["buildings"]),
        "release": plan.buildings["release"],
        "min_area_m2": plan.buildings["min_area_m2"],
        "tiers": {str(z): a for z, a in TIER_MIN_AREA_M2.items()},
        "leaf_zoom": LEAF_ZOOM,
        "geometric_error": {str(k): v for k, v in GEOMETRIC_ERROR.items()},
        "rules": BUILDINGS_RULES_VERSION,
        "format": "3d-tiles-1.1",
        "mask": {"tile_px": MASK_PX, "cell_factor": MASK_FACTOR, "rules": MASK_RULES_VERSION},  # REALISM R3b
    }
```

- [ ] **Step 5: Rewrite `layers/buildings.py` with the cached plan, the raster and the mask**

Replace `scripts/scene/camsim_scene/layers/buildings.py` with:

```python
"""Buildings layer build (REALISM R3, R3b): snapshot -> plan_buildings (heights from the package's own bare-earth
terrain sources, the same height path verify --deep uses) -> one GLB per tile + tileset.json under features/buildings/,
plus the footprint mask (features/buildings/mask/) from the shared footprint raster. The plan and the raster are cached
under .state/ by their inputs, so the land-cover burn and this layer read the same buildings and plan them once.
Rebuilt as a whole (Pendleton ~82k buildings) and skipped when its inputs hash is unchanged."""

from __future__ import annotations

import json
import logging
import pickle
import shutil
import time
from pathlib import Path

import numpy as np

from .. import __version__
from .. import buildings as B
from .. import footprints as F
from ..context import BuildContext, WorkerState
from ..engine import LayerStats, inputs_hash, peak_rss_mb
from ..fsutil import atomic_write, sha256_file
from ..gltf import write_glb
from ..licences import layer_credit
from ..manifest import STATE_DIR, Manifest, canonical_json
from . import terrain
from .landcover import tile_filename

log = logging.getLogger(__name__)

OUT = Path("features") / "buildings"
MASK = OUT / "mask"
MARKER = "buildings.json"
PLAN_MARKER, PLAN_FILE = "buildings-plan.json", "buildings-plan.pkl"
RASTER_MARKER, RASTER_FILE = "buildings-footprints.json", "buildings-footprints.npz"


def terrain_zoom(m: Manifest) -> int:
    return max(int(r["max_zoom"]["terrain"]) for r in m.regions)


def heights_fn(st: WorkerState, m: Manifest):
    """Bare-earth WGS-84 ellipsoid heights at points: `terrain.point_heights` is the exact-offset sampler verify --deep
    compares terrain tiles against; each source height gets its datum's vertical offset (geoid -> ellipsoid) added in
    `sample_group`, so the result is ellipsoid height like the terrain tiles themselves."""
    z = terrain_zoom(m)

    def fn(lon: np.ndarray, lat: np.ndarray) -> np.ndarray:
        bounds = (float(lon.min()), float(lat.min()), float(lon.max()), float(lat.max()))
        entries = st.index["terrain"].query(bounds)
        if not entries:
            return np.full(lon.shape, np.nan)
        return terrain.point_heights(z, lon, lat, entries, st.tidal)

    return fn


def plan_from_manifest(m: Manifest, ctx: BuildContext, st: WorkerState | None = None):
    s = m.layers["buildings"]
    rec = m.source(s["priorities"][0])
    snapshot = Path(ctx.asset_paths[f"{rec.id}/{rec.assets[0].id}"])
    st = st or WorkerState(ctx)
    return B.plan_buildings(B.read_snapshot(snapshot), heights_fn(st, m), float(s["min_area_m2"]))


def footprint_inputs(m: Manifest, wst: WorkerState) -> str:
    """Everything the kept buildings depend on: the snapshot, min_area_m2, the rules, and the terrain (a building
    without a ground height is dropped)."""
    s = m.layers["buildings"]
    rec = m.source(s["priorities"][0])
    terrain_shas = [a.sha256 for sid in m.layers["terrain"]["priorities"] for a in m.source(sid).assets]  # in order
    return inputs_hash(
        __version__, B.BUILDINGS_RULES_VERSION, rec.assets[0].sha256, repr(float(s["min_area_m2"])),
        m.layer_settings_hash("terrain"), *wst.terrain_extra, *terrain_shas,
    )


def _cache_hit(pkg: Path, marker: str, data: str, inputs: str) -> Path | None:
    mk, path = Path(pkg) / STATE_DIR / marker, Path(pkg) / STATE_DIR / data
    if mk.exists() and path.exists() and json.loads(mk.read_text()).get("inputs") == inputs:
        return path
    return None


def plan_cached(pkg: Path, m: Manifest, ctx: BuildContext, wst: WorkerState | None = None) -> tuple[list, dict]:
    """plan_from_manifest, pickled under .state/ by footprint_inputs (the land-cover burn and the GLBs share it)."""
    wst = wst or WorkerState(ctx)
    key = footprint_inputs(m, wst)
    hit = _cache_hit(pkg, PLAN_MARKER, PLAN_FILE, key)
    if hit is not None:
        try:
            return pickle.loads(hit.read_bytes())
        except Exception as e:  # a truncated or stale pickle: plan again
            log.warning("ignoring %s: %s", hit, e)
    out = plan_from_manifest(m, ctx, wst)
    atomic_write(Path(pkg) / STATE_DIR / PLAN_FILE, pickle.dumps(out, protocol=pickle.HIGHEST_PROTOCOL))
    atomic_write(Path(pkg) / STATE_DIR / PLAN_MARKER, json.dumps({"inputs": key}).encode())
    return out


def footprint_raster(pkg: Path, m: Manifest, ctx: BuildContext, wst: WorkerState | None = None) -> tuple[Path, str]:
    """The cached footprint raster file of the kept buildings (footprints.save_raster) and its inputs key."""
    wst = wst or WorkerState(ctx)
    key = inputs_hash(footprint_inputs(m, wst), F.MASK_RULES_VERSION)
    path = Path(pkg) / STATE_DIR / RASTER_FILE
    if _cache_hit(pkg, RASTER_MARKER, RASTER_FILE, key) is None:
        blds, _ = plan_cached(pkg, m, ctx, wst)
        F.save_raster(F.raster_tiles(blds), path)
        atomic_write(Path(pkg) / STATE_DIR / RASTER_MARKER, json.dumps({"inputs": key}).encode())
    return path, key


def write_mask(pkg: Path, raster: dict) -> str:
    """features/buildings/mask/: one 1-bit PNG per raster tile + index.json; returns the index's sha256 ("" = none)."""
    written = sorted(raster)
    if not written:
        return ""
    d = Path(pkg) / MASK
    for i, j in written:
        F.write_mask_tile(raster[(i, j)], d / tile_filename(i, j))
    atomic_write(d / "index.json", F.mask_index_bytes(written))
    return sha256_file(d / "index.json")


def _mask_intact(pkg: Path, sha: str) -> bool:
    index = Path(pkg) / MASK / "index.json"
    if not sha:
        return not index.exists()
    if not index.exists() or sha256_file(index) != sha:
        return False
    return all((index.parent / t["file"]).is_file() for t in json.loads(index.read_text())["tiles"])


def plan_stats(pkg: Path) -> dict | None:
    marker = Path(pkg) / STATE_DIR / MARKER
    return json.loads(marker.read_text())["stats"] if marker.exists() else None


def build(pkg: Path, m: Manifest, ctx: BuildContext) -> LayerStats:
    st, out = LayerStats(), Path(pkg) / OUT
    s = m.layers.get("buildings")
    if not s:
        shutil.rmtree(Path(pkg) / "features", ignore_errors=True)
        for name in (MARKER, PLAN_MARKER, PLAN_FILE, RASTER_MARKER, RASTER_FILE):
            (Path(pkg) / STATE_DIR / name).unlink(missing_ok=True)
        return st
    t0 = time.monotonic()
    rec = m.source(s["priorities"][0])
    wst = WorkerState(ctx)
    credit = layer_credit(rec.attribution, rec.licence, layer="buildings")
    inputs = inputs_hash(m.layer_settings_hash("buildings"), __version__, rec.assets[0].sha256, credit,
                         footprint_inputs(m, wst))
    marker, ts_path = Path(pkg) / STATE_DIR / MARKER, out / "tileset.json"
    prev = json.loads(marker.read_text()) if marker.exists() else None
    if prev and prev["inputs"] == inputs and not prev["output"] and not out.exists():
        st.seconds = round(time.monotonic() - t0, 3)  # no building survived planning: nothing on disk
        return st
    if (
        prev and prev["inputs"] == inputs and prev["output"] and ts_path.exists()
        and sha256_file(ts_path) == prev["output"] and _mask_intact(pkg, prev.get("mask", ""))
    ):
        uris = [u for u, _ in B.walk_tileset(json.loads(ts_path.read_text()))]
        if all((out / u).is_file() for u in uris):
            files = [p for p in out.rglob("*") if p.is_file()]
            st.tiles = st.skipped = len(uris)
            st.files, st.bytes = len(files), sum(p.stat().st_size for p in files)
            st.seconds = round(time.monotonic() - t0, 3)
            return st
    shutil.rmtree(out, ignore_errors=True)
    blds, stats = plan_cached(pkg, m, ctx, wst)
    by: dict[tuple[int, int, int], list] = {}
    for b in blds:
        by.setdefault(b.tile, []).append(b)
    # tiles only exist for keys with at least one building: write_glb rejects empty meshes
    tiles = {k: (v, B.tile_origin(*k, v)) for k, v in sorted(by.items())}
    for (z, x, y), (bl, origin) in tiles.items():
        mesh, table = B.tile_mesh(bl, origin)
        p = out / str(z) / str(x) / f"{y}.glb"
        p.parent.mkdir(parents=True, exist_ok=True)
        atomic_write(p, write_glb(mesh, table, credit.replace("\n", " ")))
    output = mask_sha = ""  # no tileset and no mask when nothing survived planning
    if tiles:
        out.mkdir(parents=True, exist_ok=True)
        atomic_write(ts_path, canonical_json(B.tileset_json(tiles)).encode())
        output = sha256_file(ts_path)
        if s.get("mask"):  # R3b; an R3 manifest (no "mask" settings) builds exactly as before
            raster_path, _ = footprint_raster(pkg, m, ctx, wst)
            mask_sha = write_mask(pkg, F.load_raster(raster_path))
    atomic_write(marker, json.dumps({"inputs": inputs, "output": output, "mask": mask_sha, "stats": stats}).encode())
    files = [p for p in out.rglob("*") if p.is_file()]
    st.tiles = st.built = len(tiles)
    st.files, st.bytes = len(files), sum(p.stat().st_size for p in files)
    st.peak_rss_mb = peak_rss_mb()
    st.seconds = round(time.monotonic() - t0, 3)
    return st
```

(`F.mask_index_bytes` is defined in Step 3 beside `mask_index`; `write_mask` uses it so `verify` and the layer write the same bytes.)

- [ ] **Step 6: Run the tests to verify they pass**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_footprints.py scripts/scene/tests/test_buildings_layer.py scripts/scene/tests/test_buildings_verify.py scripts/scene/tests/test_config.py -q`
Expected: all PASS (the validator test is skipped without Node). `test_rebuild_is_identical_and_skips` now also covers the mask (its files are in `hashes.txt`).

- [ ] **Step 7: Commit**

```bash
git add scripts/scene/camsim_scene/footprints.py scripts/scene/camsim_scene/buildings.py \
  scripts/scene/camsim_scene/layers/buildings.py scripts/scene/camsim_scene/config.py scripts/scene/tests/test_footprints.py
git commit -m "feat(scene): building footprint raster and mask tiles (REALISM R3b)"
```

---
### Task 2: Land-cover burn of code 230, hashing, packages without buildings unchanged

**Files:**
- Create: `scripts/scene/tests/test_landcover_buildings.py`
- Modify: `scripts/scene/camsim_scene/features.py` (constants, `burn_tile`), `scripts/scene/camsim_scene/pipeline.py:490-607` (`_cut_landcover`, `_build_landcover`), `scripts/scene/camsim_scene/config.py` (`layer_settings`), `scripts/scene/tests/test_features.py` (new tests), `scripts/scene/tests/test_config.py` (new tests)

**Interfaces:**
- Consumes: Task 1's `layers.buildings.footprint_raster(pkg, m, ctx, wst=None) -> (Path, str)`, `footprints.load_raster`, `footprints.burn_cells`, `footprints.unpack`, `footprints.MASK_RULES_VERSION`, `buildings.BUILDINGS_RULES_VERSION`.
- Produces:
  - `features.BUILDING_CODE = 230`; `features.CODES[230] = "building"`; `features.PRIORITY = {"water": 0, "paved_area": 1, "building": 2, "rail": 3, "road": 4}` (same relative order as R2 for the feature kinds, so features-only output is unchanged).
  - `features.burn_tile(codes, i, j, feats, area, buildings: np.ndarray | None = None) -> int` (changed cells). `buildings` is a `(600, 600)` bool of cells to burn as 230 after water and paved areas and before rail and roads; `None` is exactly R2. `area` may be `None` when `feats` is empty.
  - `pipeline._cut_landcover(bounds, out, paths, feats=None, area=None, credits=None, footprints: str | None = None)`.
  - Land-cover settings gain `"buildings": {"code": 230, "cell_factor": 8, "min_cells": 32, "priority": [...], "rules": "1"}` only when the buildings layer is on; `_build_landcover` burns only when that key is present (R3 manifests and packages without `[buildings]` are untouched).

- [ ] **Step 1: Write the failing unit tests (burn order and rule)**

Append to `scripts/scene/tests/test_features.py`:

```python
def _rail_at_col(col, sid="osm:w9"):
    x = W0 + (col + 0.5) / 12000.0
    return Feature("rail", sid, LineString([(x, S0 + 0.005), (x, S0 + 0.045)]), {"railway": "rail"})


def test_buildings_burn_between_paved_areas_and_rail():
    from camsim_scene.features import BUILDING_CODE

    paved = Feature("paved_area", "osm:a1", box(W0 + 0.005, S0 + 0.01, W0 + 0.02, S0 + 0.04), {"amenity": "parking"})
    over = road({}, sid="osm:w2")  # E-W along row 300
    rail = _rail_at_col(400)
    bmask = np.zeros((600, 600), bool)
    bmask[100:500, 100:500] = True
    codes = np.full((600, 600), 30, np.uint8)
    burn_tile(codes, TI, TJ, [rail, over, paved], AREA, bmask)
    assert BUILDING_CODE == 230
    assert codes[200, 150] == 230  # a paved area under a building becomes building (parking structures)
    assert codes[200, 70] == 200  # the paved area outside the building stays paved
    assert codes[300, 150] == 200  # the road over the building wins
    assert codes[200, 400] == 210  # the rail over the building wins
    assert codes[200, 450] == 230 and codes[50, 50] == 30 and codes[200, 550] == 30
    assert PRIORITY["paved_area"] < PRIORITY["building"] < PRIORITY["rail"] < PRIORITY["road"]


def test_buildings_none_or_empty_burns_features_exactly_as_before():
    feats = [road({}, sid="osm:w2"), _rail_at_col(400), Feature("water", "nhd:a", box(W0 + 0.02, S0 + 0.02, W0 + 0.03, S0 + 0.03), {})]
    a = np.full((600, 600), 30, np.uint8)
    b = a.copy()
    na = burn_tile(a, TI, TJ, feats, AREA)
    nb = burn_tile(b, TI, TJ, feats, AREA, np.zeros((600, 600), bool))
    assert na == nb and np.array_equal(a, b)


def test_buildings_alone_need_no_features_or_area():
    bmask = np.zeros((600, 600), bool)
    bmask[10:12, 20:23] = True
    codes = np.full((600, 600), 10, np.uint8)
    assert burn_tile(codes, TI, TJ, [], None, bmask) == 6
    assert (codes == 230).sum() == 6
```

Append to `scripts/scene/tests/test_config.py`:

```python
def test_landcover_settings_without_buildings_are_r3s():
    plan = parse_scene({**BUILDINGS_TOML, "allow": ["ODbL-1.0"], "features": {"roads": True}})
    lc = layer_settings(plan)["landcover"]
    assert set(lc) == {"priorities", "bounds", "tile_deg", "features"}
    assert lc["features"]["rules"] == "1"  # features.RULES_VERSION is not bumped: features-only packages stay R3's


def test_landcover_settings_with_buildings_carry_the_burn_rule():
    plan = parse_scene({**BUILDINGS_TOML, "allow": ["ODbL-1.0"], "buildings": {}})
    lb = layer_settings(plan)["landcover"]["buildings"]
    assert lb["code"] == 230 and lb["cell_factor"] == 8 and lb["min_cells"] == 32 and lb["rules"] == "1"
    assert lb["priority"] == ["worldcover", "water", "paved_area", "building", "rail", "road"]
```

- [ ] **Step 2: Run them to verify they fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_features.py scripts/scene/tests/test_config.py -q`
Expected: FAIL: `ImportError: cannot import name 'BUILDING_CODE'`, `TypeError: burn_tile() takes 5 positional arguments but 6 were given`, `KeyError: 'buildings'`.

- [ ] **Step 3: Implement the burn in `features.py`**

In `scripts/scene/camsim_scene/features.py`, update the constants:

```python
CODES = {200: "paved_asphalt", 201: "paved_concrete", 210: "rail", 220: "inland_water", 230: "building"}
BUILDING_CODE = 230  # REALISM R3b: = the CIGI material code of a building hit (Thermal/LandCoverLookup.h)
# Burn order, later wins. "building" is the R3b footprint burn (not a Feature kind); the feature kinds keep R2's order.
PRIORITY = {"water": 0, "paved_area": 1, "building": 2, "rail": 3, "road": 4}
```

Replace `burn_tile` with the three functions below (the shape loop is R2's, moved into `_feature_shapes`, with each shape's priority kept):

```python
def _tile_frame(i: int, j: int):
    """The tile's local metric frame (equirectangular about its centre), metres per degree of longitude, and the
    raster transform of its TILE_PX x TILE_PX cells (north-west corner, local metres)."""
    from rasterio.transform import Affine

    s, w = i * TILE_DEG, j * TILE_DEG
    lat0, lon0 = s + TILE_DEG / 2.0, w + TILE_DEG / 2.0
    kx = M_PER_DEG_LAT * math.cos(math.radians(lat0))
    cell_x, cell_y = kx / CELLS_PER_DEG, M_PER_DEG_LAT / CELLS_PER_DEG
    x0, y0 = (w - lon0) * kx, (s + TILE_DEG - lat0) * M_PER_DEG_LAT
    return _to_local(lat0, lon0), kx, Affine(cell_x, 0, x0, 0, -cell_y, y0)


def _feature_shapes(i: int, j: int, feats: list[Feature], area, local, kx: float) -> list[tuple]:
    """(local polygon, code, priority) of every feature burned in tile (i, j) within `area`, in burn order."""
    s, w = i * TILE_DEG, j * TILE_DEG
    tile = box(w, s, w + TILE_DEG, s + TILE_DEG)
    inside = tile.intersection(box(*area))
    clip = local(inside)
    if clip.is_empty:
        return []
    cw, cs, ce, cn = inside.bounds
    shapes = []
    for f in sorted(feats, key=lambda f: (PRIORITY[f.kind], f.source_id)):
        code = code_for(f)
        if code is None:
            continue
        # cheap early-out on raw lon/lat bounds, before any transform or buffer
        reach = (max(width_m(f), MIN_W_M) / 2.0 if f.kind in ("road", "rail") else 0.0) + 1.0
        fw, fs, fe, fn = f.geom.bounds
        dlat, dlon = reach / M_PER_DEG_LAT, reach / kx
        if fe + dlon < cw or fw - dlon > ce or fn + dlat < cs or fs - dlat > cn:
            continue
        p = _polygon(f, local)
        if p is None or p.is_empty:
            continue
        p = p.intersection(clip)
        if p.is_empty:
            continue
        shapes.append((p, code, PRIORITY[f.kind]))
    return shapes


def _rasterise_into(codes: np.ndarray, shapes: list[tuple], transform) -> None:
    """One rasterize pass; later shapes overwrite earlier ones. Codes are >= 200, so 0 means untouched."""
    from rasterio import features as rf

    if not shapes:
        return
    out = rf.rasterize(
        [(p, c) for p, c, _ in shapes], out_shape=(TILE_PX, TILE_PX), transform=transform, fill=0,
        all_touched=False, dtype="uint8",
    )
    m = out > 0
    codes[m] = out[m]


def burn_tile(codes: np.ndarray, i: int, j: int, feats: list[Feature], area, buildings: np.ndarray | None = None) -> int:
    """Burn `feats` (within `area`) into tile (i, j)'s codes in priority order; with `buildings` (REALISM R3b, a
    (TILE_PX, TILE_PX) bool from footprints.burn_cells) those cells become BUILDING_CODE after water and paved areas and
    before rail and roads. buildings=None is R2's single pass, byte for byte. Returns the number of changed cells."""
    local, kx, transform = _tile_frame(i, j)
    before = codes.copy()
    shapes = _feature_shapes(i, j, feats, area, local, kx) if feats and area is not None else []
    if buildings is None:
        _rasterise_into(codes, shapes, transform)
    else:
        b = PRIORITY["building"]
        _rasterise_into(codes, [x for x in shapes if x[2] < b], transform)
        codes[buildings] = BUILDING_CODE
        _rasterise_into(codes, [x for x in shapes if x[2] > b], transform)
    return int((codes != before).sum())
```

Update the module docstring's priority sentence to: "rasterised with the centre-sample rule in priority order (water < paved areas < buildings (R3b, from the footprint raster) < rail < roads; later wins)".

- [ ] **Step 4: Add the land-cover burn settings in `config.py`**

In `layer_settings`, replace the tail `bs = buildings_settings(plan) ...` with:

```python
    bs = buildings_settings(plan)
    if bs is not None:
        from .footprints import BURN_MIN_CELLS, MASK_FACTOR, MASK_RULES_VERSION

        out["buildings"] = bs
        out["landcover"]["buildings"] = {  # REALISM R3b: footprints burned as 230 (only with [buildings])
            "code": 230,
            "cell_factor": MASK_FACTOR,
            "min_cells": BURN_MIN_CELLS,
            "priority": ["worldcover", "water", "paved_area", "building", "rail", "road"],
            "rules": MASK_RULES_VERSION,
        }
    return out
```

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_features.py scripts/scene/tests/test_config.py -q`
Expected: PASS.

- [ ] **Step 5: Write the failing pipeline tests**

`scripts/scene/tests/test_landcover_buildings.py`:

```python
import json

import numpy as np
from fake_sources import build_synthetic, source
from PIL import Image
from rasters import write_geotiff
from test_buildings_layer import ROWS, _scene
from test_pipeline import FEATURES, _lc_setup

from camsim_scene import footprints as F
from camsim_scene import pipeline
from camsim_scene.manifest import AssetRecord, SourceRecord


def _with_buildings(tmp_path, monkeypatch, features=None, key="k1"):
    pkg, m, build = _lc_setup(tmp_path, monkeypatch, features)
    m.sources.append(
        SourceRecord("overture_buildings", "overture_buildings", "overture", "2026-09-23.1", "ODbL-1.0",
                     "© Overture Maps Foundation", {}, [AssetRecord("snap", "overture://snap", "cd" * 32, 1)])
    )
    m.layers["buildings"] = {"priorities": ["overture_buildings"], "min_area_m2": 10.0, "rules": "2"}
    m.layers["landcover"]["buildings"] = {"code": 230, "cell_factor": 8, "min_cells": 32, "rules": F.MASK_RULES_VERSION}
    mask = np.zeros((F.MASK_PX, F.MASK_PX), bool)
    mask[800:1600, 800:1600] = True  # land-cover cells 100..199 of tile (200, 200)
    npz = tmp_path / "fp.npz"
    F.save_raster({(200, 200): np.packbits(mask, axis=1)}, npz)
    state = {"key": key, "calls": 0}

    def fake_raster(pkg_, m_, ctx_, wst=None):
        state["calls"] += 1
        return npz, state["key"]

    monkeypatch.setattr(pipeline.buildings_layer, "footprint_raster", fake_raster)
    return pkg, m, build, state


def test_buildings_burn_230_with_legend_and_credit(tmp_path, monkeypatch):
    pkg, _, build, _ = _with_buildings(tmp_path, monkeypatch)
    build()
    index = json.loads((pkg / "landcover" / "index.json").read_text())
    assert index["codes"] == {"230": "building"}
    assert len(index["credits"]) == 1 and index["credits"][0].startswith("© Overture Maps Foundation")
    codes = np.array(Image.open(pkg / "landcover" / "+10.00_+10.00.png"))
    assert (codes[100:200, 100:200] == 230).all() and (codes == 230).sum() == 100 * 100
    assert "© Overture Maps Foundation" in (pkg / "landcover" / "ATTRIBUTION.txt").read_text()


def test_buildings_and_features_share_the_legend(tmp_path, monkeypatch):
    pkg, _, build, _ = _with_buildings(tmp_path, monkeypatch, FEATURES)
    build()
    index = json.loads((pkg / "landcover" / "index.json").read_text())
    assert index["codes"] == {"200": "paved_asphalt", "230": "building"}
    assert len(index["credits"]) == 2  # OSM, then Overture


def test_buildings_inputs_rebuild_and_skip(tmp_path, monkeypatch):
    pkg, m, build, state = _with_buildings(tmp_path, monkeypatch)
    assert build().built == 2
    assert build().skipped == 2
    state["key"] = "k2"  # the kept buildings changed (snapshot, terrain, rules)
    assert build().built == 2
    m.layers["buildings"]["min_area_m2"] = 20.0  # the buildings settings hash
    assert build().built == 2


def test_no_buildings_never_reads_footprints(tmp_path, monkeypatch):
    pkg, m, build = _lc_setup(tmp_path, monkeypatch, FEATURES)

    def boom(*a, **k):
        raise AssertionError("footprint_raster called without [buildings]")

    monkeypatch.setattr(pipeline.buildings_layer, "footprint_raster", boom)
    build()
    assert json.loads((pkg / "landcover" / "index.json").read_text())["codes"] == {"200": "paved_asphalt"}


def test_r3_manifest_with_buildings_but_no_burn_settings_is_untouched(tmp_path, monkeypatch):
    pkg, m, build, state = _with_buildings(tmp_path, monkeypatch)
    del m.layers["landcover"]["buildings"]  # an R3 manifest
    build()
    assert state["calls"] == 0
    assert "codes" not in json.loads((pkg / "landcover" / "index.json").read_text())


def test_end_to_end_burn_and_mask_agree(tmp_path, monkeypatch):
    # the synthetic buildings scene plus a land-cover source; the COG reader returns tree cover (10) everywhere and the
    # cut runs in-process so the patched reader applies (synthetic sources are not WorldCover COGs)
    scene = _scene(tmp_path, ROWS)
    write_geotiff(tmp_path / "src" / "wc.tif", np.full((10, 10), 10, np.uint8), 10.0, 10.5, 0.05, overviews=(), tiled=False)
    scene["priorities"]["landcover"] = ["worldcover"]
    scene["sources"]["worldcover"] = source("landcover", tmp_path / "src" / "wc.tif", [10.0, 10.0, 10.5, 10.5])
    monkeypatch.setattr(pipeline.landcover, "local_reader", lambda paths: lambda url, r, c: np.full((600, 600), 10, np.uint8))
    monkeypatch.setattr(pipeline, "_run_isolated", lambda fn, *a: fn(*a))
    pkg, _, _ = build_synthetic(tmp_path, scene)
    lc = json.loads((pkg / "landcover" / "index.json").read_text())
    assert lc["codes"] == {"230": "building"}
    mask_index = json.loads((pkg / "features/buildings/mask/index.json").read_text())
    masks = {(t["lat_index"], t["lon_index"]): t["file"] for t in mask_index["tiles"]}
    burned = 0
    for t in lc["tiles"]:
        codes = np.array(Image.open(pkg / "landcover" / t["file"]))
        key = (t["lat_index"], t["lon_index"])
        want = (F.burn_cells(np.array(Image.open(pkg / "features/buildings/mask" / masks[key])))
                if key in masks else np.zeros(codes.shape, bool))
        assert np.array_equal(codes == 230, want)
        burned += int(want.sum())
    assert 20 <= burned <= 50  # ~3,200 m2 at ~9.1 m x 9.3 m cells (lat 10): ~37 cells, fewer at the half-cover edges
```

(The synthetic imagery is 111 m, so these roofs fall back to flat once Task 4 lands; the test does not depend on it.)

- [ ] **Step 6: Run them to verify they fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_landcover_buildings.py -q`
Expected: FAIL: no 230 in the tiles (`assert index["codes"] == {"230": "building"}` sees no `codes` key), and the end-to-end test finds no `"230"`.

- [ ] **Step 7: Burn in the pipeline**

In `scripts/scene/camsim_scene/pipeline.py`, add the import beside the other `from .` imports:

```python
from .buildings import BUILDINGS_RULES_VERSION
```

Replace `_cut_landcover` with:

```python
def _cut_landcover(
    bounds: tuple,
    out: Path,
    paths: dict[str, Path],
    feats: list | None = None,
    area: tuple | None = None,
    credits: list[str] | None = None,
    footprints: str | None = None,
) -> tuple[dict, float]:
    """Runs in its own process, so its peak RSS isn't the main process's lifetime peak (fetch, planning). With `feats`
    (REALISM R2), burns them into the tiles that meet `area`; with `footprints` (REALISM R3b, a footprints.save_raster
    file), burns its >= 32-of-64 cells as 230 into every tile it covers. Either writes the code legend and credits into
    index.json."""
    if feats is None and footprints is None:
        index = landcover.fetch(bounds, out, reader=landcover.local_reader(paths), cut_by="camsim-scene")
        return index, peak_rss_mb()
    from PIL import Image

    from . import footprints as F
    from .features import CODES, burn_tile

    index = landcover.fetch(
        bounds, out, reader=landcover.local_reader(paths), cut_by="camsim-scene", codes={}, credits=credits
    )
    raster = F.load_raster(Path(footprints)) if footprints else {}
    present: set[int] = set()
    for t in index["tiles"]:
        i, j = t["lat_index"], t["lon_index"]
        tw, ts = j * landcover.TILE_DEG, i * landcover.TILE_DEG
        in_area = area is not None and not (
            tw >= area[2] or tw + landcover.TILE_DEG <= area[0] or ts >= area[3] or ts + landcover.TILE_DEG <= area[1]
        )
        burn = F.burn_cells(F.unpack(raster[(i, j)])) if (i, j) in raster else None
        if not in_area and burn is None:
            continue
        path = out / t["file"]
        codes = np.array(Image.open(path), dtype=np.uint8)
        if burn_tile(codes, i, j, (feats or []) if in_area else [], area if in_area else None, burn):
            landcover.write_tile(codes, path)
        present |= set(np.unique(codes).tolist()) & CODES.keys()
    index["codes"] = {str(c): CODES[c] for c in sorted(present)}
    landcover.write_index(index, out)
    return index, peak_rss_mb()
```

In `_build_landcover`, replace the block from `fs = lc.get("features")` down to the end of the `else:` branch with:

```python
    fs = lc.get("features")
    credits = None
    if fs:  # REALISM R2: the feature extracts are inputs too (their settings are in the layer settings hash)
        shas += sorted(a.sha256 for sid in fs["sources"] for a in m.source(sid).assets)
        credits = [layer_credit(m.source(sid).attribution, m.source(sid).licence) for sid in fs["sources"]]
        shas += credits  # the layer's own notice is an output too
    fp_path = None
    if lc.get("buildings") and m.layers.get("buildings"):  # REALISM R3b: building footprints burned as 230
        fp_path, fp_key = buildings_layer.footprint_raster(pkg, m, ctx)
        brec = m.source(m.layers["buildings"]["priorities"][0])
        bcredit = layer_credit(brec.attribution, brec.licence)
        credits = (credits or []) + [bcredit]
        shas += [m.layer_settings_hash("buildings"), brec.assets[0].sha256, BUILDINGS_RULES_VERSION, fp_key, bcredit]
    inputs = inputs_hash(m.layer_settings_hash("landcover"), __version__, *shas)
    marker, index_path = pkg / STATE_DIR / "landcover.json", out / "index.json"
    prev = json.loads(marker.read_text()) if marker.exists() else None
    index = None
    if prev and prev["inputs"] == inputs and index_path.exists() and sha256_file(index_path) == prev["output"]:
        index = json.loads(index_path.read_text())
        if not _landcover_complete(index, out):
            index = None
    if index is not None:
        st.skipped = len(index["tiles"])
    else:
        shutil.rmtree(out, ignore_errors=True)
        bounds = tuple(lc["bounds"])
        if fs or fp_path:
            feats = _load_features(m, ctx) if fs else None  # in the main process: the cut's process needs no cache
            area = tuple(fs["area"]) if fs else None
            fp = str(fp_path) if fp_path else None
            index, st.peak_rss_mb = _run_isolated(_cut_landcover, bounds, out, paths, feats, area, credits, fp)
        else:
            index, st.peak_rss_mb = _run_isolated(_cut_landcover, bounds, out, paths)
        atomic_write(marker, json.dumps({"inputs": inputs, "output": sha256_file(index_path)}).encode())
        st.built = len(index["tiles"])
```

(`fp_key` is the raster's inputs key: snapshot, `min_area_m2`, rules, terrain and the mask rules, so a change in the kept buildings rebuilds the land cover; the three items the spec names are hashed explicitly beside it.)

- [ ] **Step 8: Run the tests to verify they pass**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_landcover_buildings.py scripts/scene/tests/test_pipeline.py scripts/scene/tests/test_features.py scripts/scene/tests/test_footprints.py -q`
Expected: all PASS, including R2's `test_pre_r2_manifest_builds_unchanged` and `test_features_burn_into_tiles_and_legend`.

- [ ] **Step 9: Full tooling suite (packages without `[buildings]` unchanged)**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests -q`
Expected: all PASS. The byte-identity of a real package without `[buildings]` is checked on Pendleton in Task 11 Step 2.

- [ ] **Step 10: Commit**

```bash
git add scripts/scene/camsim_scene/features.py scripts/scene/camsim_scene/pipeline.py scripts/scene/camsim_scene/config.py \
  scripts/scene/tests/test_landcover_buildings.py scripts/scene/tests/test_features.py scripts/scene/tests/test_config.py
git commit -m "feat(scene): burn building footprints into land cover as code 230 (REALISM R3b)"
```

---
### Task 3: Roof atlas packer (pure)

**Files:**
- Create: `scripts/scene/camsim_scene/roofatlas.py`, `scripts/scene/tests/test_roofatlas.py`

**Interfaces:**
- Consumes: nothing (pure Python + math).
- Produces:
  - `roofatlas.GUTTER_PX = 2`, `roofatlas.TEXEL_STEPS_M = (0.5, 0.75, 1.0, 1.5, 2.0)`.
  - `roofatlas.texel_ladder(start_m: float) -> tuple[float, ...]` = `(start_m,)` + every step above it.
  - `roofatlas.content_px(extent_m: float, texel_m: float) -> int` (≥ 1).
  - `@dataclass(frozen=True) Placement(x: int, y: int, w: int, h: int)` — the roof's content rectangle in atlas pixels (its gutter is outside it).
  - `@dataclass Packing(size: int, texel_m: float, placed: dict[str, Placement], flat: list[str])` — `size` 0 when nothing is placed; `flat` sorted.
  - `roofatlas.shelf_pack(rects: dict[str, tuple[int, int]], size: int, skip_unfit: bool) -> dict[str, tuple[int, int]] | None` (top-left of each gutter-inclusive rectangle; `None` when one does not fit and `skip_unfit` is false).
  - `roofatlas.pack_roofs(extents: dict[str, tuple[float, float]], start_texel_m: float, max_px: int) -> Packing` (keys are `source_id`s; extents are AABB width/height in metres).

- [ ] **Step 1: Write the failing tests**

`scripts/scene/tests/test_roofatlas.py`:

```python
import pytest

from camsim_scene.roofatlas import GUTTER_PX, Placement, content_px, pack_roofs, shelf_pack, texel_ladder


def test_texel_ladder():
    assert texel_ladder(0.5) == (0.5, 0.75, 1.0, 1.5, 2.0)
    assert texel_ladder(1.0) == (1.0, 1.5, 2.0)
    assert texel_ladder(0.6) == (0.6, 0.75, 1.0, 1.5, 2.0)
    assert texel_ladder(2.0) == (2.0,)


def test_content_px_rounds_up_and_is_at_least_one():
    assert content_px(10.0, 0.5) == 20 and content_px(10.1, 0.5) == 21 and content_px(0.0, 0.5) == 1


def test_shelf_order_is_height_then_source_id():
    rects = {"b": (10, 8), "a": (10, 8), "c": (10, 20), "d": (30, 5)}
    pos = shelf_pack(rects, 64, skip_unfit=False)
    assert pos == {"c": (0, 0), "a": (10, 0), "b": (20, 0), "d": (30, 0)}


def test_shelf_wraps_to_a_new_shelf_and_reports_overflow():
    rects = {f"r{k}": (30, 30) for k in range(4)}
    assert shelf_pack(rects, 64, skip_unfit=False) == {"r0": (0, 0), "r1": (30, 0), "r2": (0, 30), "r3": (30, 30)}
    rects["r4"] = (30, 30)
    assert shelf_pack(rects, 64, skip_unfit=False) is None
    assert set(shelf_pack(rects, 64, skip_unfit=True)) == {"r0", "r1", "r2", "r3"}


def test_pack_is_deterministic_and_square_power_of_two():
    ext = {"z": (12.0, 9.0), "a": (40.0, 30.0), "m": (12.0, 9.0), "q": (5.0, 30.0)}
    p1 = pack_roofs(ext, 0.5, 2048)
    p2 = pack_roofs(dict(reversed(list(ext.items()))), 0.5, 2048)
    assert p1 == p2 and p1.texel_m == 0.5 and p1.flat == []
    assert p1.size & (p1.size - 1) == 0 and p1.size <= 2048
    a = p1.placed["a"]
    assert (a.w, a.h) == (80, 60) and a.x >= GUTTER_PX and a.y >= GUTTER_PX
    rects = sorted((pl.x - GUTTER_PX, pl.y - GUTTER_PX, pl.x + pl.w + GUTTER_PX, pl.y + pl.h + GUTTER_PX)
                   for pl in p1.placed.values())
    for k, r in enumerate(rects):  # gutter-inclusive rectangles never overlap and stay inside the atlas
        assert r[2] <= p1.size and r[3] <= p1.size
        for s in rects[k + 1:]:
            assert r[2] <= s[0] or s[2] <= r[0] or r[3] <= s[1] or s[3] <= r[1]


def test_smallest_power_of_two_that_fits():
    assert pack_roofs({"a": (10.0, 10.0)}, 0.5, 2048).size == 32  # 20 + 4 px
    assert pack_roofs({}, 0.5, 2048) == pack_roofs({}, 0.5, 2048)
    assert pack_roofs({}, 0.5, 2048).size == 0


def test_overflow_steps_the_texel_and_records_it():
    ext = {f"r{k:02d}": (300.0, 300.0) for k in range(16)}  # 604 px each at 0.5 m: 9 fit in 2048, 16 do not
    p = pack_roofs(ext, 0.5, 2048)
    assert p.texel_m == 0.75 and p.flat == [] and len(p.placed) == 16
    assert {(pl.w, pl.h) for pl in p.placed.values()} == {(400, 400)}


def test_a_roof_too_big_at_two_metres_falls_back_alone():
    ext = {"giant": (5000.0, 40.0), "small": (20.0, 20.0)}  # 2,504 px wide even at 2.0 m
    p = pack_roofs(ext, 0.5, 2048)
    assert p.flat == ["giant"] and set(p.placed) == {"small"} and p.texel_m == 0.5


def test_roofs_that_still_do_not_fit_at_two_metres_go_flat_in_order():
    ext = {f"r{k:03d}": (1000.0, 1000.0) for k in range(5)}  # 504 px at 2.0 m: 16 fit in 2048, 4 fit in 1024
    p = pack_roofs(ext, 0.5, 1024)
    assert p.texel_m == 2.0 and p.size == 1024
    assert sorted(p.placed) == ["r000", "r001", "r002", "r003"] and p.flat == ["r004"]
    assert p.placed["r000"] == Placement(GUTTER_PX, GUTTER_PX, 500, 500)


@pytest.mark.parametrize("max_px", [256, 2048])
def test_never_larger_than_max(max_px):
    ext = {f"r{k:03d}": (float(5 + k % 7), float(4 + k % 5)) for k in range(400)}
    p = pack_roofs(ext, 0.5, max_px)
    assert p.size <= max_px and len(p.placed) + len(p.flat) == 400
```

- [ ] **Step 2: Run them to verify they fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_roofatlas.py -q`
Expected: FAIL at collection: `ModuleNotFoundError: No module named 'camsim_scene.roofatlas'`.

- [ ] **Step 3: Implement**

`scripts/scene/camsim_scene/roofatlas.py`:

```python
"""Roof atlas packing (REALISM R3b, spec section 2 "Packing"). Each roof's axis-aligned box in the tile's local ENU
metres becomes one rectangle of content pixels plus GUTTER_PX on each side; a deterministic shelf packer places them,
ordered by rectangle height in pixels descending, then source_id ascending (unique, so no ties). The atlas is the
smallest square power of two that holds them, at most max_px; if none does, the texel steps up the ladder. A roof that
cannot fit even alone at the last step is flat from the start (so one giant roof does not coarsen the whole tile); the
ones that still do not fit at the last step are flat too, skipped in packing order. Pure; no I/O."""

from __future__ import annotations

import math
from dataclasses import dataclass, field

GUTTER_PX = 2
TEXEL_STEPS_M = (0.5, 0.75, 1.0, 1.5, 2.0)


@dataclass(frozen=True)
class Placement:
    x: int  # content rectangle, atlas pixels (row 0 = top = north); its gutter is GUTTER_PX outside it
    y: int
    w: int
    h: int


@dataclass
class Packing:
    size: int  # atlas side in pixels; 0 = nothing placed
    texel_m: float
    placed: dict[str, Placement] = field(default_factory=dict)
    flat: list[str] = field(default_factory=list)  # sorted source ids that keep the flat roof material


def texel_ladder(start_m: float) -> tuple[float, ...]:
    return (float(start_m),) + tuple(s for s in TEXEL_STEPS_M if s > start_m + 1e-9)


def content_px(extent_m: float, texel_m: float) -> int:
    return max(1, math.ceil(extent_m / texel_m - 1e-9))


def _rects(extents: dict[str, tuple[float, float]], texel_m: float) -> dict[str, tuple[int, int]]:
    g = 2 * GUTTER_PX
    return {k: (content_px(w, texel_m) + g, content_px(h, texel_m) + g) for k, (w, h) in extents.items()}


def shelf_pack(rects: dict[str, tuple[int, int]], size: int, skip_unfit: bool) -> dict[str, tuple[int, int]] | None:
    x = y = shelf_h = 0
    out: dict[str, tuple[int, int]] = {}
    for k in sorted(rects, key=lambda k: (-rects[k][1], k)):
        w, h = rects[k]
        if w > size or h > size:
            if skip_unfit:
                continue
            return None
        if x + w > size:
            y, x, shelf_h = y + shelf_h, 0, 0
        if y + h > size:
            if skip_unfit:
                continue
            return None
        out[k] = (x, y)
        x += w
        shelf_h = max(shelf_h, h)
    return out


def _pow2_at_least(n: int) -> int:
    return 1 << max(0, math.ceil(math.log2(max(1, n))))


def _placements(rects: dict[str, tuple[int, int]], pos: dict[str, tuple[int, int]]) -> dict[str, Placement]:
    g = GUTTER_PX
    return {k: Placement(x + g, y + g, rects[k][0] - 2 * g, rects[k][1] - 2 * g) for k, (x, y) in sorted(pos.items())}


def pack_roofs(extents: dict[str, tuple[float, float]], start_texel_m: float, max_px: int) -> Packing:
    ladder = texel_ladder(start_texel_m)
    last = _rects(extents, ladder[-1])
    flat = sorted(k for k, (w, h) in last.items() if w > max_px or h > max_px)
    todo = {k: v for k, v in extents.items() if k not in flat}
    if not todo:
        return Packing(0, ladder[0], {}, flat)
    for t in ladder:
        rects = _rects(todo, t)
        area = sum(w * h for w, h in rects.values())
        size = _pow2_at_least(max(max(max(r) for r in rects.values()), math.ceil(math.sqrt(area))))
        while size <= max_px:
            pos = shelf_pack(rects, size, skip_unfit=False)
            if pos is not None:
                return Packing(size, t, _placements(rects, pos), flat)
            size *= 2
    t = ladder[-1]
    rects = _rects(todo, t)
    pos = shelf_pack(rects, max_px, skip_unfit=True)
    flat = sorted(set(flat) | (set(todo) - set(pos)))
    if not pos:
        return Packing(0, t, {}, flat)
    return Packing(max_px, t, _placements(rects, pos), flat)
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_roofatlas.py -q`
Expected: PASS (11 tests).

- [ ] **Step 5: Commit**

```bash
git add scripts/scene/camsim_scene/roofatlas.py scripts/scene/tests/test_roofatlas.py
git commit -m "feat(scene): deterministic roof atlas packer (REALISM R3b)"
```

---
### Task 4: Roof textures in the tiles (sampling, `TEXCOORD_0`, embedded JPEG, config keys)

**Files:**
- Create: `scripts/scene/camsim_scene/roofs.py`, `scripts/scene/tests/test_roofs.py`
- Modify: `scripts/scene/camsim_scene/gltf.py` (`TileMesh`, `DecodedTile`, `_check`, `write_glb`, `_accessor`, `decode_tile`), `scripts/scene/camsim_scene/buildings.py` (`TileGeometry`, `tile_geometry`, `tile_mesh`), `scripts/scene/camsim_scene/sources/base.py` (`native_res_m`), `scripts/scene/camsim_scene/layers/buildings.py` (`ImageryRoofSampler`, roof inputs and tile loop), `scripts/scene/camsim_scene/config.py` (`BUILDINGS_KEYS`, `_parse_buildings`, `buildings_settings`), `scripts/scene/tests/fake_sources.py` (`fine_roof_scene`, `fine_truth`), `scripts/scene/tests/test_gltf.py`, `scripts/scene/tests/test_buildings_layer.py`, `scripts/scene/tests/test_buildings_verify.py`, `scripts/scene/tests/test_config.py`

The config keys are part of this task (not a separate one) because the layer reads them; Task 5 adds only `verify`.

**Interfaces:**
- Consumes: Task 3's `roofatlas.pack_roofs`, `GUTTER_PX`, `TEXEL_STEPS_M`; `imagery.sample_entries(entries, lon, lat, target_m)`, `imagery.decode_entries(entries, vals, which, lon, lat, balance=None, land=None)`; `WorkerState.index["imagery"]`, `.balance`, `.balance_sha`; `geodesy.ecef_to_geodetic`.
- Produces:
  - `gltf.TileMesh(..., textured_indices=np.zeros(0, np.uint32), texcoords: np.ndarray | None = None, texture_jpeg: bytes | None = None, extras: dict | None = None)`; material 0 = flat roof, 1 = wall, 2 = textured roof (present only with a texture). `gltf.DecodedTile` gains `textured_indices`, `texcoords`, `texture` (bytes or None), `extras` (dict or None). `gltf.LINEAR = 9729`, `LINEAR_MIPMAP_LINEAR = 9987`, `CLAMP_TO_EDGE = 33071`.
  - `buildings.TileGeometry(positions, normals, feature_ids, roof: dict[int, np.ndarray], wall_indices, table, matrix)`, `buildings.tile_geometry(buildings, origin) -> TileGeometry`, `buildings.tile_mesh(buildings, origin, roof: RoofOptions | None = None, sampler: RoofSampler | None = None, stats: dict | None = None) -> tuple[TileMesh, PropertyTable]` (no `roof` = R3 bytes).
  - `roofs.MAX_SOURCE_M = 2.0`, `roofs.ROOF_JPEG_QUALITY = 85`, `roofs.RoofSampler` (protocol: `sample(lon, lat, target_m) -> (rgb uint8 (3, ...), valid bool, res_m float)`), `roofs.RoofOptions(texel_m=0.5, max_px=2048, max_source_m=2.0, jpeg_quality=85)` with `RoofOptions.from_settings(s: dict)`, `roofs.RoofTexture(texcoords, textured: frozenset[int], texel_m, rgb, jpeg, n_coarse, n_overflow)`, `roofs.enu_to_lonlat(matrix, east, north, up) -> (lon, lat)`, `roofs.roof_texture(positions, roof, source_ids, matrix, sampler, opts) -> RoofTexture`.
  - `sources.base.native_res_m(raster: SourceRaster) -> float`; `layers.buildings.ImageryRoofSampler(st: WorkerState)`.
  - `[buildings]` keys `roof_textures` (bool, true), `roof_texel_m` (float in [0.25, 2.0], 0.5), `roof_texture_max_px` (256…8192 power of two, 2048); buildings settings gain them plus `"roof": {"texel_steps_m", "gutter_px", "max_source_m", "jpeg_quality", "sampler"}`. Each GLB of a package with `roof_textures` carries root `extras.roof_texel_m` (the tile's texel, `null` when every roof is flat); `build.json` `buildings.roofs` = `{"textured", "flat_coarse", "flat_overflow", "tiles_by_texel_m"}`.

- [ ] **Step 1: Write the failing glTF tests**

Append to `scripts/scene/tests/test_gltf.py` (add `import io`, `import pytest` and `from PIL import Image` at the top):

```python
def _jpeg() -> bytes:
    buf = io.BytesIO()
    Image.fromarray(np.full((4, 4, 3), 90, np.uint8)).save(buf, "JPEG", quality=85)
    return buf.getvalue()


def _textured():
    m = _mesh()
    uv = np.array([[0, 0], [1, 0], [1, 1], [0, 1]], np.float32)
    return gltf.TileMesh(m.positions, m.normals, m.feature_ids, np.zeros(0, np.uint32), m.wall_indices,
                         textured_indices=m.roof_indices, texcoords=uv, texture_jpeg=_jpeg(),
                         extras={"roof_texel_m": 0.5})


def test_textured_tile_layout_and_round_trip():
    m = _textured()
    data = gltf.write_glb(m, _table(), "© test")
    doc, _ = gltf.read_glb(data)
    assert doc["samplers"] == [{"magFilter": 9729, "minFilter": 9987, "wrapS": 33071, "wrapT": 33071}]
    assert doc["images"][0]["mimeType"] == "image/jpeg" and doc["textures"] == [{"sampler": 0, "source": 0}]
    pbr = doc["materials"][2]["pbrMetallicRoughness"]
    assert pbr["baseColorTexture"] == {"index": 0} and pbr["baseColorFactor"] == [1.0, 1.0, 1.0, 1.0]
    prims = doc["meshes"][0]["primitives"]
    assert sorted(p["material"] for p in prims) == [1, 2]
    assert all("TEXCOORD_0" in p["attributes"] for p in prims)  # shared arrays: walls carry (0, 0) texcoords too
    assert doc["extras"] == {"roof_texel_m": 0.5}
    for bv in doc["bufferViews"]:
        assert bv["byteOffset"] % 8 == 0
    t = gltf.decode_tile(data)
    assert t.texture == m.texture_jpeg and t.extras == {"roof_texel_m": 0.5} and len(t.roof_indices) == 0
    assert np.array_equal(t.textured_indices, m.textured_indices) and np.array_equal(t.texcoords, m.texcoords)


def test_untextured_tile_keeps_the_r3_layout():
    doc, _ = gltf.read_glb(gltf.write_glb(_mesh(), _table(), "© test"))
    assert not {"images", "textures", "samplers", "extras"} & set(doc) and len(doc["materials"]) == 2
    t = gltf.decode_tile(gltf.write_glb(_mesh(), _table(), "© test"))
    assert t.texture is None and t.texcoords is None and t.extras is None and len(t.textured_indices) == 0


def test_textured_tile_needs_texcoords_and_texture():
    m = _textured()
    m.texcoords = None
    with pytest.raises(ValueError, match="texcoords"):
        gltf.write_glb(m, _table(), "c")
    m = _textured()
    m.texture_jpeg = None
    with pytest.raises(ValueError, match="texture"):
        gltf.write_glb(m, _table(), "c")
```

- [ ] **Step 2: Run them to verify they fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_gltf.py -q`
Expected: FAIL: `TypeError: TileMesh.__init__() got an unexpected keyword argument 'textured_indices'`.

- [ ] **Step 3: Implement the glTF changes**

In `scripts/scene/camsim_scene/gltf.py`: change the dataclass import to `from dataclasses import dataclass, field`, add the sampler constants after `ARRAY_BUFFER, ELEMENT_ARRAY_BUFFER = 34962, 34963`:

```python
LINEAR, LINEAR_MIPMAP_LINEAR, CLAMP_TO_EDGE = 9729, 9987, 33071  # roof atlas sampler (REALISM R3b)
```

and update the module docstring's first sentence to: "Minimal glTF 2.0 binary (GLB) for building tiles (REALISM R3, R3b): one mesh with up to three primitives (flat roofs, walls, atlas roofs) sharing POSITION / NORMAL / _FEATURE_ID_0 (and TEXCOORD_0 when the tile has a roof atlas), one EXT_structural_metadata property table `building`."

Replace `TileMesh` and `DecodedTile`:

```python
@dataclass
class TileMesh:
    positions: np.ndarray  # (n, 3) float32: east, up, -north (m) from the tile's ENU origin
    normals: np.ndarray  # (n, 3) float32, same axes
    feature_ids: np.ndarray  # (n,) float32: index into the property table
    roof_indices: np.ndarray  # (k,) uint32: flat-colour roofs (material 0)
    wall_indices: np.ndarray  # (j,) uint32: walls (material 1)
    textured_indices: np.ndarray = field(default_factory=lambda: np.zeros(0, np.uint32))  # atlas roofs (material 2)
    texcoords: np.ndarray | None = None  # (n, 2) float32 TEXCOORD_0 (atlas UV, origin top-left); with textured_indices
    texture_jpeg: bytes | None = None  # the tile's roof atlas; with textured_indices
    extras: dict | None = None  # glTF root extras; R3b: {"roof_texel_m": float | None} when roof textures are on


@dataclass
class DecodedTile:
    positions: np.ndarray
    normals: np.ndarray
    feature_ids: np.ndarray
    roof_indices: np.ndarray
    wall_indices: np.ndarray
    table: PropertyTable
    copyright: str
    textured_indices: np.ndarray = field(default_factory=lambda: np.zeros(0, np.uint32))
    texcoords: np.ndarray | None = None
    texture: bytes | None = None
    extras: dict | None = None
```

In `_check`, replace the "no roof or wall indices" check and add the texture checks:

```python
    if len(mesh.roof_indices) == 0 and len(mesh.wall_indices) == 0 and len(mesh.textured_indices) == 0:
        raise ValueError("tile mesh has no roof or wall indices")
    if len(mesh.textured_indices):
        if mesh.texcoords is None or np.shape(mesh.texcoords) != (len(mesh.positions), 2):
            raise ValueError("textured roofs need (n, 2) texcoords")
        if not mesh.texture_jpeg:
            raise ValueError("textured roofs need a texture")
```

In `write_glb`, replace from `fid = accessor(...)` through the `prims` loop with:

```python
    fid = accessor(mesh.feature_ids.astype("<f4"), FLOAT, "SCALAR", ARRAY_BUFFER)
    has_tex = len(mesh.textured_indices) > 0
    attrs = {"POSITION": pos, "NORMAL": nrm, "_FEATURE_ID_0": fid}
    if has_tex:  # after the R3 accessors, so an untextured tile's JSON is unchanged
        attrs["TEXCOORD_0"] = accessor(np.asarray(mesh.texcoords, "<f4"), FLOAT, "VEC2", ARRAY_BUFFER)
    features = {
        "EXT_mesh_features": {
            "featureIds": [{"featureCount": len(table.source_ids), "attribute": 0, "propertyTable": 0}]
        }
    }
    prims = []
    for mat, idx in ((0, mesh.roof_indices), (1, mesh.wall_indices), (2, mesh.textured_indices)):
        if len(idx) == 0:
            continue
        ia = accessor(idx.astype("<u4"), UINT, "SCALAR", ELEMENT_ARRAY_BUFFER)
        prims.append({"attributes": dict(attrs), "indices": ia, "material": mat, "mode": 4, "extensions": features})
```

Replace the `doc = {...}` statement (keep the property-table code above it unchanged) with:

```python
    materials = [
        {"pbrMetallicRoughness": {"baseColorFactor": list(rgba), "metallicFactor": 0.0, "roughnessFactor": 0.9}}
        for rgba in (ROOF_RGBA, WALL_RGBA)
    ]
    if has_tex:
        img = b.add(bytes(mesh.texture_jpeg))
        materials.append({"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                                                   "metallicFactor": 0.0, "roughnessFactor": 0.9}})
    doc = {
        "asset": {"version": "2.0", "generator": "camsim-scene", "copyright": copyright},
        "extensionsUsed": ["EXT_mesh_features", "EXT_structural_metadata"],
        "extensions": {
            "EXT_structural_metadata": {
                "schema": SCHEMA,
                "propertyTables": [{"class": "building", "count": len(table.source_ids), "properties": props}],
            }
        },
        "materials": materials,
        "meshes": [{"primitives": prims}],
        "nodes": [{"mesh": 0}],
        "scenes": [{"nodes": [0]}],
        "scene": 0,
        "accessors": acc,
        "bufferViews": b.views,
        "buffers": [{"byteLength": len(b.data) + (-len(b.data) % 4)}],
    }
    if has_tex:
        doc["images"] = [{"bufferView": img, "mimeType": "image/jpeg"}]
        doc["textures"] = [{"sampler": 0, "source": 0}]
        doc["samplers"] = [{"magFilter": LINEAR, "minFilter": LINEAR_MIPMAP_LINEAR, "wrapS": CLAMP_TO_EDGE,
                            "wrapT": CLAMP_TO_EDGE}]
    if mesh.extras is not None:
        doc["extras"] = mesh.extras
```

(The JSON is dumped with `sort_keys=True`, so key order in the dict does not matter; the bytes of a tile without a texture and without extras are R3's.)

Replace `_accessor` and `decode_tile`:

```python
def _accessor(doc: dict, bin_: bytes, i: int) -> np.ndarray:
    a = doc["accessors"][i]
    dt = {FLOAT: "<f4", UINT: "<u4", UBYTE: "<u1"}[a["componentType"]]
    n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3}[a["type"]]
    arr = np.frombuffer(_view(doc, bin_, a["bufferView"]), dt, count=a["count"] * n)
    return arr.reshape(-1, n) if n > 1 else arr


def decode_tile(data: bytes) -> DecodedTile:
    doc, bin_ = read_glb(data)
    prims = doc["meshes"][0]["primitives"]
    attrs = prims[0]["attributes"]
    idx = {p["material"]: _accessor(doc, bin_, p["indices"]) for p in prims}
    empty = np.zeros(0, np.uint32)
    t = doc["extensions"]["EXT_structural_metadata"]["propertyTables"][0]["properties"]
    t_ids = _decode_strings(doc, bin_, t["source_id"])
    table = PropertyTable(
        t_ids,
        _decode_strings(doc, bin_, t["class"]) if "class" in t else [""] * len(t_ids),
        np.frombuffer(_view(doc, bin_, t["height_m"]["values"]), np.float32).copy(),
        np.frombuffer(_view(doc, bin_, t["height_source"]["values"]), np.uint8).copy(),
    )
    tc = attrs.get("TEXCOORD_0")
    images = doc.get("images") or []
    return DecodedTile(
        _accessor(doc, bin_, attrs["POSITION"]).copy(),
        _accessor(doc, bin_, attrs["NORMAL"]).copy(),
        _accessor(doc, bin_, attrs["_FEATURE_ID_0"]).copy(),
        idx.get(0, empty).copy(),
        idx.get(1, empty).copy(),
        table,
        doc["asset"].get("copyright", ""),
        idx.get(2, empty).copy(),
        _accessor(doc, bin_, tc).copy() if tc is not None else None,
        bytes(_view(doc, bin_, images[0]["bufferView"])) if images else None,
        doc.get("extras"),
    )
```

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_gltf.py scripts/scene/tests/test_buildings.py -q`
Expected: PASS.

- [ ] **Step 4: Write the failing roof tests**

`scripts/scene/tests/test_roofs.py`:

```python
import math

import numpy as np
from overture_fixture import box_lonlat

from camsim_scene import buildings as B
from camsim_scene import gltf, roofs
from camsim_scene.roofs import RoofOptions

M = 111320.0
LON0, LAT0 = 9.999, 9.999  # south-west of every test building, so encoded positions are positive
ORIGIN = (10.0, 10.0, 55.0)


class PosSampler:
    """RGB = the point's position, 1/16 m east / north of (LON0, LAT0), 12 bits each (no wrap within 256 m); RGB 0 and
    res inf where `valid` is false; native resolution `res_m` (a number, or a function of lon)."""

    def __init__(self, res_m=0.6, valid=None):
        self.res_m, self.valid = res_m, valid

    @staticmethod
    def en(lon, lat):
        return (np.asarray(lon) - LON0) * M * math.cos(math.radians(LAT0)), (np.asarray(lat) - LAT0) * M

    def sample(self, lon, lat, target_m):
        lon, lat = np.asarray(lon, float), np.asarray(lat, float)
        e, n = self.en(lon, lat)
        ex = np.round(e * 16).astype(np.int64) & 4095
        ny = np.round(n * 16).astype(np.int64) & 4095
        rgb = np.stack([ex >> 4, ((ex & 15) << 4) | (ny >> 8), ny & 255]).astype(np.uint8)
        ok = np.ones(lon.shape, bool) if self.valid is None else self.valid(lon, lat)
        rgb[:, ~ok] = 0
        res = self.res_m(lon) if callable(self.res_m) else np.full(lon.shape, float(self.res_m))
        return rgb, ok, np.where(ok, res, np.inf)

    @staticmethod
    def decode(rgb):
        rgb = rgb.astype(np.int64)
        ex = (rgb[..., 0] << 4) | (rgb[..., 1] >> 4)
        ny = ((rgb[..., 1] & 15) << 8) | rgb[..., 2]
        return ex / 16.0, ny / 16.0


def flat(h):
    return lambda lon, lat: np.full(np.shape(lon), float(h))


def _plan(fps):
    blds, _ = B.plan_buildings(fps, flat(50.0), 10.0)
    return blds


def _tex(g, sampler, opts=RoofOptions()):
    return roofs.roof_texture(g.positions, g.roof, g.table.source_ids, g.matrix, sampler, opts)


def _two():
    ring = box_lonlat(10.0, 10.0, 30, 24).difference(box_lonlat(10.0, 10.0, 8, 6))
    return [B.Footprint("a", ring, 9.0, None, ""), B.Footprint("b", box_lonlat(10.0004, 10.0002, 12, 9), 6.0, None, "")]


def test_texcoords_sample_the_expected_source_pixels():
    g = B.tile_geometry(_plan(_two()), ORIGIN)
    s = PosSampler()
    tex = _tex(g, s)
    assert tex.textured == frozenset({0, 1}) and tex.texel_m == 0.5 and tex.n_coarse == tex.n_overflow == 0
    size = tex.rgb.shape[0]
    assert tex.rgb.shape == (size, size, 3) and size & (size - 1) == 0
    tol = tex.texel_m * (0.5 + 1e-3) + 1 / 16  # the pixel centre is within half a texel of the point; 1/16 m encoding
    for f, idx in g.roof.items():
        tri = idx.reshape(-1, 3)
        uv = tex.texcoords[tri].astype(np.float64).mean(axis=1)  # UV is affine in (east, north): centroid -> centroid
        p = g.positions[tri].astype(np.float64).mean(axis=1)
        lon, lat = roofs.enu_to_lonlat(g.matrix, p[:, 0], -p[:, 2], p[:, 1])
        want_e, want_n = s.en(lon, lat)
        got_e, got_n = s.decode(tex.rgb[np.floor(uv[:, 1] * size).astype(int), np.floor(uv[:, 0] * size).astype(int)])
        assert np.abs(got_e - want_e).max() <= tol and np.abs(got_n - want_n).max() <= tol
    roof_v = np.concatenate(list(g.roof.values()))
    assert (tex.texcoords[np.setdiff1d(np.arange(len(g.positions)), roof_v)] == 0).all()  # walls (0, 0)
    assert tex.texcoords[roof_v].min() >= 0 and tex.texcoords[roof_v].max() <= 1


def test_roof_texture_is_deterministic():
    g = B.tile_geometry(_plan(_two()), ORIGIN)
    a, b = _tex(g, PosSampler()), _tex(g, PosSampler())
    assert a.jpeg == b.jpeg and np.array_equal(a.texcoords, b.texcoords)


def test_coarse_source_falls_back_to_flat():
    fps = [B.Footprint("w", box_lonlat(10.0, 10.0, 20, 20), 6.0, None, ""),
           B.Footprint("e", box_lonlat(10.0006, 10.0, 20, 20), 6.0, None, "")]
    g = B.tile_geometry(_plan(fps), ORIGIN)
    tex = _tex(g, PosSampler(res_m=lambda lon: np.where(lon < 10.0003, 0.6, 10.0)))
    assert [g.table.source_ids[f] for f in sorted(tex.textured)] == ["w"] and tex.n_coarse == 1


def test_all_coarse_tile_has_no_texture_and_null_texel():
    blds = _plan(_two())
    g = B.tile_geometry(blds, ORIGIN)
    tex = _tex(g, PosSampler(res_m=10.0))
    assert tex.textured == frozenset() and tex.jpeg is None and tex.texel_m is None and tex.n_coarse == 2
    mesh, table = B.tile_mesh(blds, ORIGIN, RoofOptions(), PosSampler(res_m=10.0))
    assert mesh.texture_jpeg is None and mesh.extras == {"roof_texel_m": None} and len(mesh.textured_indices) == 0
    doc, _ = gltf.read_glb(gltf.write_glb(mesh, table, "c"))
    assert "images" not in doc and doc["extras"] == {"roof_texel_m": None} and len(doc["materials"]) == 2


def test_roof_partial_nodata_fills_with_the_roof_mean():
    g = B.tile_geometry(_plan([B.Footprint("a", box_lonlat(10.0, 10.0, 30, 20), 6.0, None, "")]), ORIGIN)
    edge = 10.0 - 5.5 / (M * math.cos(math.radians(10.0)))  # the west ~9.5 m of the 30 m roof has no imagery
    tex = _tex(g, PosSampler(valid=lambda lon, lat: lon > edge))
    assert tex.textured == frozenset({0})
    size = tex.rgb.shape[0]
    uv = tex.texcoords[g.roof[0]] * size
    x0, x1 = int(np.floor(uv[:, 0].min())), int(np.ceil(uv[:, 0].max()))
    y0, y1 = int(np.floor(uv[:, 1].min())), int(np.ceil(uv[:, 1].max()))
    block = tex.rgb[y0:y1, x0:x1].reshape(y1 - y0, x1 - x0, 3)
    assert not (block == 0).all(axis=2).any()  # never black
    west = block[:, :8].reshape(-1, 3)
    assert (west == west[0]).all()  # the invalid texels share one fill colour


def test_tile_mesh_without_roof_options_is_the_r3_tile():
    blds = _plan(_two())
    mesh, table = B.tile_mesh(blds, ORIGIN)
    assert mesh.extras is None and mesh.texcoords is None and len(mesh.textured_indices) == 0
    g = B.tile_geometry(blds, ORIGIN)
    assert np.array_equal(mesh.roof_indices, np.concatenate([g.roof[f] for f in sorted(g.roof)]))


def test_tile_mesh_splits_textured_and_flat_and_counts():
    fps = [B.Footprint("w", box_lonlat(10.0, 10.0, 20, 20), 6.0, None, ""),
           B.Footprint("e", box_lonlat(10.0006, 10.0, 20, 20), 6.0, None, "")]
    blds = _plan(fps)
    stats = {}
    mesh, table = B.tile_mesh(blds, ORIGIN, RoofOptions(), PosSampler(res_m=lambda lon: np.where(lon < 10.0003, 0.6, 10.0)),
                              stats)
    fid = mesh.feature_ids.astype(int)
    assert set(fid[mesh.textured_indices]) == {1} and set(fid[mesh.roof_indices]) == {0}  # ids sort: e = 0, w = 1
    assert stats == {"textured": 1, "flat_coarse": 1, "flat_overflow": 0, "tiles_by_texel_m": {"0.5": 1}}
    t = gltf.decode_tile(gltf.write_glb(mesh, table, "c"))
    assert t.extras == {"roof_texel_m": 0.5} and t.texture == mesh.texture_jpeg
```

- [ ] **Step 5: Run them to verify they fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_roofs.py -q`
Expected: FAIL at collection: `ImportError: cannot import name 'roofs' from 'camsim_scene'`.

- [ ] **Step 6: Implement `roofs.py` and the tile geometry split**

`scripts/scene/camsim_scene/roofs.py`:

```python
"""Roof texture atlas of one buildings tile (REALISM R3b, spec section 2). Each roof is sampled from the imagery
layer's own sources (a RoofSampler) on a grid of texel_m in the tile's local ENU frame, over its axis-aligned box, and
packed into one square power-of-two atlas (roofatlas.pack_roofs). A roof whose best source (the one supplying most of
its triangle centroids) is coarser than max_source_m keeps the flat roof material, as does one that does not fit at the
last texel step. Texels with no valid source inside a textured roof take the roof's mean colour. Gutters repeat the edge
texels, so mipmaps do not bleed. UV origin is the atlas's top-left (north-west); u east, v south."""

from __future__ import annotations

import io
from dataclasses import dataclass
from typing import Protocol

import numpy as np
from PIL import Image

from . import geodesy
from .roofatlas import GUTTER_PX, pack_roofs

MAX_SOURCE_M = 2.0
ROOF_JPEG_QUALITY = 85
FILL_RGB = (128, 128, 128)  # a textured roof with no valid texel at all (never expected: its centroids were valid)


class RoofSampler(Protocol):
    def sample(self, lon: np.ndarray, lat: np.ndarray, target_m: float) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
        """(rgb uint8 (3, *shape), valid bool (*shape), native resolution in m of the source of each value, inf where
        invalid)."""


@dataclass(frozen=True)
class RoofOptions:
    texel_m: float = 0.5
    max_px: int = 2048
    max_source_m: float = MAX_SOURCE_M
    jpeg_quality: int = ROOF_JPEG_QUALITY

    @staticmethod
    def from_settings(s: dict) -> RoofOptions:
        return RoofOptions(float(s["roof_texel_m"]), int(s["roof_texture_max_px"]))


@dataclass
class RoofTexture:
    texcoords: np.ndarray  # (n, 2) float32 for every vertex of the tile; (0, 0) off the atlas
    textured: frozenset  # feature indices whose roof is drawn from the atlas
    texel_m: float | None  # None: no textured roof in this tile
    rgb: np.ndarray | None  # (size, size, 3) uint8 atlas before JPEG (tests, verify)
    jpeg: bytes | None
    n_coarse: int  # roofs flat because their best source is coarser than max_source_m (or has no data)
    n_overflow: int  # roofs flat because they did not fit at the last texel step


def enu_to_lonlat(matrix: np.ndarray, east, north, up) -> tuple[np.ndarray, np.ndarray]:
    enu = np.stack([np.asarray(east, np.float64), np.asarray(north, np.float64), np.asarray(up, np.float64)], axis=-1)
    lon, lat, _ = geodesy.ecef_to_geodetic(enu @ matrix[:3, :3].T + matrix[:3, 3])
    return lon, lat


def encode_jpeg(rgb: np.ndarray, quality: int) -> bytes:
    buf = io.BytesIO()
    Image.fromarray(np.ascontiguousarray(rgb)).save(buf, "JPEG", quality=quality, subsampling=2, optimize=False,
                                                   progressive=False)
    return buf.getvalue()


def _majority_res(res: np.ndarray, ok: np.ndarray) -> float:
    if not ok.any():
        return float("inf")
    vals, counts = np.unique(res[ok], return_counts=True)
    return float(vals[int(np.argmax(counts))])  # ties: the finer source (np.unique sorts ascending)


def roof_texture(positions: np.ndarray, roof: dict, source_ids: list[str], matrix: np.ndarray, sampler: RoofSampler,
                 opts: RoofOptions) -> RoofTexture:
    p = np.asarray(positions, np.float64)
    east, up, north = p[:, 0], p[:, 1], -p[:, 2]
    uv = np.zeros((len(p), 2), np.float32)
    extents: dict[str, tuple[float, float]] = {}
    boxes: dict[int, tuple[float, float, float]] = {}
    n_coarse = 0
    for f in sorted(roof):
        idx = roof[f]
        c = p[idx].reshape(-1, 3, 3).mean(axis=1)  # triangle centroids: inside the roof
        lon, lat = enu_to_lonlat(matrix, c[:, 0], -c[:, 2], c[:, 1])
        _, ok, res = sampler.sample(lon, lat, opts.texel_m)
        if _majority_res(res, ok) > opts.max_source_m:
            n_coarse += 1
            continue
        e0, e1, n0, n1 = east[idx].min(), east[idx].max(), north[idx].min(), north[idx].max()
        boxes[f] = (float(e0), float(n1), float(up[idx][0]))
        extents[source_ids[f]] = (float(e1 - e0), float(n1 - n0))
    pk = pack_roofs(extents, opts.texel_m, opts.max_px)
    if not pk.placed:
        return RoofTexture(uv, frozenset(), None, None, None, n_coarse, len(pk.flat))
    by_id = {source_ids[f]: f for f in boxes}
    t, size, g = pk.texel_m, pk.size, GUTTER_PX
    atlas = np.zeros((size, size, 3), np.uint8)
    for sid, pl in sorted(pk.placed.items()):
        f = by_id[sid]
        e0, n1, u0 = boxes[f]
        ge, gn = np.meshgrid(e0 + (np.arange(pl.w) + 0.5) * t, n1 - (np.arange(pl.h) + 0.5) * t)
        lon, lat = enu_to_lonlat(matrix, ge, gn, np.full(ge.shape, u0))
        rgb, ok, _ = sampler.sample(lon, lat, t)
        block = np.ascontiguousarray(np.moveaxis(rgb, 0, -1), np.uint8).copy()
        if not ok.all():
            fill = np.rint(block[ok].mean(axis=0)).astype(np.uint8) if ok.any() else np.array(FILL_RGB, np.uint8)
            block[~ok] = fill
        atlas[pl.y - g : pl.y + pl.h + g, pl.x - g : pl.x + pl.w + g] = np.pad(block, ((g, g), (g, g), (0, 0)), mode="edge")
        idx = roof[f]
        uv[idx, 0] = (pl.x + (east[idx] - e0) / t) / size
        uv[idx, 1] = (pl.y + (n1 - north[idx]) / t) / size
    textured = frozenset(by_id[sid] for sid in pk.placed)
    return RoofTexture(uv, textured, t, atlas, encode_jpeg(atlas, opts.jpeg_quality), n_coarse, len(pk.flat))
```

In `scripts/scene/camsim_scene/buildings.py`, add `TileGeometry` after `Building` and replace `tile_mesh` with `tile_geometry` + `tile_mesh`:

```python
@dataclass
class TileGeometry:
    positions: np.ndarray  # (n, 3) float32: east, up, -north (m) from the tile's ENU origin
    normals: np.ndarray  # (n, 3) float32
    feature_ids: np.ndarray  # (n,) float32
    roof: dict  # feature index -> its roof vertex indices (uint32, ascending; consecutive triples are triangles)
    wall_indices: np.ndarray  # uint32
    table: PropertyTable
    matrix: np.ndarray  # 4 x 4 ENU -> ECEF of the tile origin


def tile_geometry(buildings: list[Building], origin: tuple[float, float, float]) -> TileGeometry:
    m = geodesy.enu_to_ecef_matrix(*origin)
    rot, o = m[:3, :3].T, m[:3, 3]  # ECEF -> ENU rotation, origin

    def to_gl(lonlat: np.ndarray, h: float) -> np.ndarray:
        e = geodesy.geodetic_to_ecef(lonlat[:, 0], lonlat[:, 1], np.full(len(lonlat), h))
        enu = (rot @ (e - o).T).T
        return np.stack([enu[:, 0], enu[:, 2], -enu[:, 1]], axis=1)

    pos, fid, wall = [], [], []
    roof: dict[int, list[int]] = {}
    n = 0

    def add(tri_gl: np.ndarray, out: list, f: int):
        nonlocal n
        pos.append(tri_gl)
        fid.extend([f] * len(tri_gl))
        out.extend(range(n, n + len(tri_gl)))
        n += len(tri_gl)

    bs = sorted(buildings, key=lambda b: b.source_id)
    for f, b in enumerate(bs):
        for part in b.parts:
            for tri in _cdt_triangles(part):
                add(to_gl(tri, b.roof_m), roof.setdefault(f, []), f)
            for ring in [part.exterior, *part.interiors]:
                c = np.asarray(ring.coords)
                bot, top = to_gl(c, b.base_m), to_gl(c, b.roof_m)
                for k in range(len(c) - 1):
                    if np.linalg.norm(bot[k + 1] - bot[k]) < MIN_EDGE_M:
                        continue
                    a0, b0, b1, a1 = bot[k], bot[k + 1], top[k + 1], top[k]
                    add(np.stack([a0, b0, b1, a0, b1, a1]), wall, f)
    p = np.concatenate(pos) if pos else np.zeros((0, 3))
    tri = p.reshape(-1, 3, 3)
    nrm = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    nrm /= np.linalg.norm(nrm, axis=1, keepdims=True)
    table = PropertyTable(
        [b.source_id for b in bs],
        [b.cls for b in bs],
        np.asarray([b.height_m for b in bs], np.float32),
        np.asarray([b.height_source for b in bs], np.uint8),
    )
    return TileGeometry(
        p.astype(np.float32),
        np.repeat(nrm, 3, axis=0).astype(np.float32),
        np.asarray(fid, np.float32),
        {f: np.asarray(v, np.uint32) for f, v in roof.items()},
        np.asarray(wall, np.uint32),
        table,
        m,
    )


def tile_mesh(buildings: list[Building], origin: tuple[float, float, float], roof=None, sampler=None,
              stats: dict | None = None) -> tuple[TileMesh, PropertyTable]:
    """The tile's mesh. Without `roof` (roofs.RoofOptions) and `sampler` it is R3's (flat roofs, byte for byte); with
    them each roof is drawn from the tile's atlas or, when roofs.roof_texture leaves it out, kept flat. `stats`
    accumulates textured / flat_coarse / flat_overflow roof counts and tiles_by_texel_m."""
    from . import roofs as R

    g = tile_geometry(buildings, origin)

    def cat(fs) -> np.ndarray:
        return np.concatenate([g.roof[f] for f in fs]).astype(np.uint32) if fs else np.zeros(0, np.uint32)

    if roof is None or sampler is None:
        return TileMesh(g.positions, g.normals, g.feature_ids, cat(sorted(g.roof)), g.wall_indices), g.table
    tex = R.roof_texture(g.positions, g.roof, g.table.source_ids, g.matrix, sampler, roof)
    textured, flat = sorted(tex.textured), sorted(set(g.roof) - tex.textured)
    if stats is not None:
        for k, v in (("textured", len(textured)), ("flat_coarse", tex.n_coarse), ("flat_overflow", tex.n_overflow)):
            stats[k] = stats.get(k, 0) + v
        by = stats.setdefault("tiles_by_texel_m", {})
        if tex.texel_m is not None:
            by[f"{tex.texel_m:g}"] = by.get(f"{tex.texel_m:g}", 0) + 1
    mesh = TileMesh(
        g.positions, g.normals, g.feature_ids, cat(flat), g.wall_indices,
        textured_indices=cat(textured), texcoords=tex.texcoords if textured else None, texture_jpeg=tex.jpeg,
        extras={"roof_texel_m": tex.texel_m},
    )
    return mesh, g.table
```

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_roofs.py scripts/scene/tests/test_buildings.py scripts/scene/tests/test_gltf.py -q`
Expected: PASS.

- [ ] **Step 7: Write the failing config and layer tests**

In `scripts/scene/tests/test_config.py`, change `test_buildings_section_defaults_and_settings`'s first assertion and add checks:

```python
def test_buildings_section_defaults_and_settings():
    plan = parse_scene({**BUILDINGS_TOML, "allow": ["ODbL-1.0"], "buildings": {}})
    assert plan.buildings == {"enabled": True, "release": "2026-09-23.1", "min_area_m2": 10.0,
                              "roof_textures": True, "roof_texel_m": 0.5, "roof_texture_max_px": 2048}
    assert plan.priorities["buildings"] == ["overture_buildings"]
    assert plan.source_options["overture_buildings"] == {"release": "2026-09-23.1"}
    s = layer_settings(plan)["buildings"]
    assert s["tiers"] == {"13": 2000.0, "14": 300.0} and s["leaf_zoom"] == 15
    assert s["geometric_error"] == {"root": 200.0, "13": 60.0, "14": 20.0, "15": 6.0}
    assert s["roof_textures"] is True and s["roof_texel_m"] == 0.5 and s["roof_texture_max_px"] == 2048
    assert s["roof"]["texel_steps_m"] == [0.5, 0.75, 1.0, 1.5, 2.0] and s["roof"]["gutter_px"] == 2
    assert s["roof"]["max_source_m"] == 2.0 and s["roof"]["jpeg_quality"] == 85


def test_roof_keys_change_the_buildings_settings_hash():
    a = layer_settings(parse_scene({**BUILDINGS_TOML, "allow": ["ODbL-1.0"], "buildings": {}}))["buildings"]
    b = layer_settings(parse_scene({**BUILDINGS_TOML, "allow": ["ODbL-1.0"], "buildings": {"roof_texel_m": 1.0}}))["buildings"]
    assert a != b and b["roof_texel_m"] == 1.0
```

and extend the `bad` parametrize list of `test_buildings_section_rejects_bad_values` with:

```python
     {"roof_textures": "yes"}, {"roof_texel_m": 0.1}, {"roof_texel_m": 3.0}, {"roof_texel_m": True},
     {"roof_texture_max_px": 3000}, {"roof_texture_max_px": 128}, {"roof_texture_max_px": 2048.0}],
```

In `scripts/scene/tests/fake_sources.py`, after `synthetic_scene`, add:

```python
FINE_BOX = (10.195, 10.195, 10.225, 10.215)  # around test_buildings_layer.ROWS
FINE_RES = 0.00001  # ~1.1 m: a roof source finer than roofs.MAX_SOURCE_M


def fine_truth(lon, lat) -> np.ndarray:
    """uint8 (3, ...): smooth ramps of period 0.004 deg (~440 m), never all zero."""
    lon, lat = np.asarray(lon, np.float64), np.asarray(lat, np.float64)
    r = 128.0 + 100.0 * np.sin((lon - FINE_BOX[0]) * 2.0 * np.pi / 0.004)
    g = 128.0 + 100.0 * np.sin((lat - FINE_BOX[1]) * 2.0 * np.pi / 0.004)
    return np.rint(np.stack([r, g, np.full(lon.shape, 60.0)])).astype(np.uint8)


def fine_roof_scene(root: Path) -> dict:
    """synthetic_scene plus `fine_rgb` (first in imagery priority): FINE_RES pixels of fine_truth over FINE_BOX."""
    scene = synthetic_scene(root)
    lon, lat = _cell_centres(FINE_BOX, FINE_RES)
    write_geotiff(root / "fine_rgb.tif", fine_truth(lon, lat), FINE_BOX[0], FINE_BOX[3], FINE_RES, overviews=(2, 4))
    scene["priorities"]["imagery"] = ["fine_rgb", *scene["priorities"]["imagery"]]
    scene["sources"]["fine_rgb"] = source("imagery", root / "fine_rgb.tif", FINE_BOX, max_zoom=17, bands=[1, 2, 3],
                                          nodata_rule="all_zero")
    return scene
```

In `scripts/scene/tests/test_buildings_layer.py`, add the imports `import io`, `import numpy as np`, `from PIL import Image`, `from fake_sources import fine_roof_scene, fine_truth`, `from camsim_scene import geodesy`, and:

```python
def _roof_scene(tmp_path, rows=ROWS, **buildings):
    scene = fine_roof_scene(tmp_path / "src")
    pq = write_overture_parquet(tmp_path / "src" / "overture.parquet", rows)
    scene["allow"] = ["ODbL-1.0"]
    scene["buildings"] = dict(buildings)
    scene.setdefault("sources", {})["overture_buildings"] = {"parquet": str(pq)}
    return scene


def test_roof_atlas_matches_the_imagery(tmp_path):
    pkg, _, info = build_synthetic(tmp_path, _roof_scene(tmp_path))
    root = pkg / "features/buildings"
    checked = 0
    for uri, mat in B.walk_tileset(json.loads((root / "tileset.json").read_text())):
        t = gltf.decode_tile((root / uri).read_bytes())
        assert t.extras["roof_texel_m"] == 0.5 and t.texture is not None
        atlas = np.asarray(Image.open(io.BytesIO(t.texture)).convert("RGB"))
        size = atlas.shape[0]
        assert atlas.shape[1] == size and size & (size - 1) == 0 and size <= 2048
        tri = t.textured_indices.reshape(-1, 3)
        uv = t.texcoords[tri].astype(np.float64).mean(axis=1)
        p = t.positions[tri].astype(np.float64).mean(axis=1)
        enu = np.stack([p[:, 0], -p[:, 2], p[:, 1]], axis=1)
        lon, lat, _ = geodesy.ecef_to_geodetic(enu @ mat[:3, :3].T + mat[:3, 3])
        got = atlas[np.floor(uv[:, 1] * size).astype(int), np.floor(uv[:, 0] * size).astype(int)].astype(int)
        assert np.abs(got - fine_truth(lon, lat).T.astype(int)).max() <= 8  # JPEG q85 + half a texel of ramp
        checked += len(tri)
    assert checked > 0
    assert info["buildings"]["roofs"]["textured"] == 3 and info["buildings"]["roofs"]["flat_coarse"] == 0


def test_roof_textures_off_writes_flat_r3_tiles(tmp_path):
    pkg, _, info = build_synthetic(tmp_path, _roof_scene(tmp_path, roof_textures=False))
    for glb in (pkg / "features/buildings").rglob("*.glb"):
        doc, _ = gltf.read_glb(glb.read_bytes())
        assert "images" not in doc and "extras" not in doc and len(doc["materials"]) == 2
    assert "roofs" not in info["buildings"]


def test_coarse_imagery_keeps_every_roof_flat(tmp_path):
    pkg, _, info = build_synthetic(tmp_path, _scene(tmp_path, ROWS))  # 111 m imagery only
    for glb in (pkg / "features/buildings").rglob("*.glb"):
        t = gltf.decode_tile(glb.read_bytes())
        assert t.texture is None and t.extras == {"roof_texel_m": None}
    assert info["buildings"]["roofs"]["flat_coarse"] == 3 and info["buildings"]["roofs"]["textured"] == 0
```

In `scripts/scene/tests/test_buildings_verify.py`, run the validator on the textured package too: replace the test's signature and the build line with a parametrised scene:

```python
@pytest.mark.skipif(shutil.which("npx") is None, reason="needs Node")
@pytest.mark.parametrize("textured", [False, True])
def test_3d_tiles_validator_is_clean(tmp_path, textured):
    if not (VALIDATOR / "node_modules").exists():
        subprocess.run(["npm", "ci"], cwd=VALIDATOR, check=True, capture_output=True)
        # .npmrc skips install scripts (better-sqlite3 does not compile on current Node); sharp needs its binary
        subprocess.run(["npm", "rebuild", "sharp", "--ignore-scripts=false"], cwd=VALIDATOR, check=True,
                       capture_output=True)
    from test_buildings_layer import _roof_scene

    pkg, _, _ = build_synthetic(tmp_path, _roof_scene(tmp_path) if textured else _scene(tmp_path, ROWS))
    out = tmp_path / "report.json"
    subprocess.run(["npx", "3d-tiles-validator", "--tilesetFile", str(pkg / "features/buildings/tileset.json"),
                    "--reportFile", str(out)], cwd=VALIDATOR, check=False, capture_output=True)
    report = json.loads(out.read_text())
    assert report["numErrors"] == 0, json.dumps(report["issues"], indent=1)[:4000]
```

- [ ] **Step 8: Run them to verify they fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_config.py scripts/scene/tests/test_buildings_layer.py -q`
Expected: FAIL: `ConfigError: unknown key(s) in buildings: ['roof_textures']`, the defaults assertion, and `KeyError: 'roofs'` / `t.extras is None` in the layer tests.

- [ ] **Step 9: Implement the config keys, the sampler and the layer wiring**

In `scripts/scene/camsim_scene/config.py`:

```python
BUILDINGS_KEYS = ("enabled", "release", "min_area_m2", "roof_textures", "roof_texel_m", "roof_texture_max_px")
ROOF_MAX_PX = (256, 512, 1024, 2048, 4096, 8192)
```

At the end of `_parse_buildings`, replace the `return` with:

```python
    tex = b.get("roof_textures", True)
    if not isinstance(tex, bool):
        raise ConfigError(f"buildings.roof_textures must be true or false, got {tex!r}")
    texel = b.get("roof_texel_m", 0.5)
    if isinstance(texel, bool) or not isinstance(texel, (int, float)) or not 0.25 <= texel <= 2.0:
        raise ConfigError(f"buildings.roof_texel_m must be a number in [0.25, 2.0], got {texel!r}")
    max_px = b.get("roof_texture_max_px", 2048)
    if isinstance(max_px, bool) or not isinstance(max_px, int) or max_px not in ROOF_MAX_PX:
        raise ConfigError(f"buildings.roof_texture_max_px must be one of {list(ROOF_MAX_PX)}, got {max_px!r}")
    return {"enabled": enabled, "release": release, "min_area_m2": float(area), "roof_textures": tex,
            "roof_texel_m": float(texel), "roof_texture_max_px": max_px}
```

In `buildings_settings`, import `from .roofatlas import GUTTER_PX, TEXEL_STEPS_M` and `from .roofs import MAX_SOURCE_M, ROOF_JPEG_QUALITY`, and add after the `"mask"` entry:

```python
        "roof_textures": plan.buildings["roof_textures"],
        "roof_texel_m": plan.buildings["roof_texel_m"],
        "roof_texture_max_px": plan.buildings["roof_texture_max_px"],
        "roof": {
            "texel_steps_m": list(TEXEL_STEPS_M),
            "gutter_px": GUTTER_PX,
            "max_source_m": MAX_SOURCE_M,
            "jpeg_quality": ROOF_JPEG_QUALITY,
            "sampler": "imagery sources in priority order + colour match; no NAIP feather or water fade",
        },
```

In `scripts/scene/camsim_scene/sources/base.py`, after `SourceRaster`:

```python
def native_res_m(raster: SourceRaster) -> float:
    """The raster's full-resolution pixel size in metres (degrees x M_PER_DEG for geographic CRSs), as sample()
    computes it to pick an overview."""
    ds = _dataset(str(raster.path), None)
    res = abs(ds.transform.a)
    return res * M_PER_DEG if ds.crs is None or ds.crs.is_geographic else res
```

In `scripts/scene/camsim_scene/layers/buildings.py`: change the layer imports to `from . import imagery, terrain`, add `from ..roofs import RoofOptions` and `from ..sources.base import native_res_m`, and add the sampler after `heights_fn`:

```python
class ImageryRoofSampler:
    """Roof pixels from the imagery layer's own sources (REALISM R3b): first valid in priority order, the colour match
    (imagery/balance.json) applied to its target source, as leaf tiles do. NAIP's edge feather and the water fade are
    leaf-tile blends and are not applied: a roof is textured only where a <= 2 m source (NAIP) supplies it."""

    def __init__(self, st: WorkerState):
        self.st = st
        self._res: dict[str, float] = {}

    def _native(self, entry) -> float:
        if entry.qid not in self._res:
            self._res[entry.qid] = native_res_m(entry.raster)
        return self._res[entry.qid]

    def sample(self, lon, lat, target_m):
        lon, lat = np.asarray(lon, np.float64), np.asarray(lat, np.float64)
        bounds = (float(lon.min()), float(lat.min()), float(lon.max()), float(lat.max()))
        entries = self.st.index["imagery"].query(bounds)
        if not entries:
            return np.zeros((3, *lon.shape), np.uint8), np.zeros(lon.shape, bool), np.full(lon.shape, np.inf)
        vals, ok, which = imagery.sample_entries(entries, lon, lat, target_m)
        rgb = imagery.decode_entries(entries, vals, which, lon, lat, self.st.balance)
        res = np.full(lon.shape, np.inf)
        for k, e in enumerate(entries):
            m = which == k
            if m.any():
                res[m] = self._native(e)
        return rgb, ok, res
```

In `build()`, right after the `inputs = inputs_hash(...)` line, add the roof inputs:

```python
    roof = RoofOptions.from_settings(s) if s.get("roof_textures") else None
    if roof is not None:  # roof pixels come from the imagery sources and the colour match: both are inputs
        img_shas = [a.sha256 for sid in m.layers["imagery"]["priorities"] for a in m.source(sid).assets]
        inputs = inputs_hash(inputs, m.layer_settings_hash("imagery"), wst.balance_sha, *img_shas)
```

and replace the tile loop with:

```python
    sampler = ImageryRoofSampler(wst) if roof is not None else None
    roof_stats: dict = {}
    for (z, x, y), (bl, origin) in tiles.items():
        mesh, table = B.tile_mesh(bl, origin, roof, sampler, roof_stats)
        p = out / str(z) / str(x) / f"{y}.glb"
        p.parent.mkdir(parents=True, exist_ok=True)
        atomic_write(p, write_glb(mesh, table, credit.replace("\n", " ")))
    if roof is not None:
        stats = {**stats, "roofs": {"textured": 0, "flat_coarse": 0, "flat_overflow": 0, "tiles_by_texel_m": {},
                                    **roof_stats}}
```

(`stats` is the cached plan's dict: copy it, never mutate it, or the pickled plan changes under the next build.)

- [ ] **Step 10: Run the tests to verify they pass**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests -q`
Expected: all PASS; the two validator cases PASS with Node installed (run `(cd scripts/scene/tests/validator && npm ci && npm rebuild sharp --ignore-scripts=false)` first) and are skipped without it.

- [ ] **Step 11: Commit**

```bash
git add scripts/scene/camsim_scene/roofs.py scripts/scene/camsim_scene/gltf.py scripts/scene/camsim_scene/buildings.py \
  scripts/scene/camsim_scene/sources/base.py scripts/scene/camsim_scene/layers/buildings.py scripts/scene/camsim_scene/config.py \
  scripts/scene/tests/test_roofs.py scripts/scene/tests/test_gltf.py scripts/scene/tests/test_buildings_layer.py \
  scripts/scene/tests/test_buildings_verify.py scripts/scene/tests/test_config.py scripts/scene/tests/fake_sources.py
git commit -m "feat(scene): roof texture atlas per buildings tile from the imagery sources (REALISM R3b)"
```

---
### Task 5: `verify` (mask ↔ burn, `roof_texel_m`) and rebuild determinism

**Files:**
- Modify: `scripts/scene/camsim_scene/verify.py` (`_check_buildings_tileset`, new `_check_buildings_mask`, `_check_buildings_roofs`, `_deep`), `scripts/scene/tests/test_buildings_verify.py`

**Interfaces:**
- Consumes: Task 1's mask layout and `footprints.burn_cells`; Task 2's land-cover burn (`lc["buildings"]` settings); Task 4's `DecodedTile.extras` / `.texture` / `.texcoords` / `.textured_indices`, `roofatlas.texel_ladder`.
- Produces: verify checks `buildings_tileset` (quick; now also lists missing mask tiles), `buildings_mask` (deep, when the package has a mask, land cover and the burn settings) and `buildings_roofs` (deep, whenever the buildings tileset exists). Rules:
  - `buildings_mask`: a land-cover cell is 230 only if ≥ 32 of its 64 mask cells are set; a cell with ≥ 32 set is 230, or 200/201/210 (roads and rail win over buildings).
  - `buildings_roofs`: with `roof_textures` every GLB has `extras.roof_texel_m`; it is `null` exactly when the GLB has no texture; otherwise it is on the ladder of `roof_texel_m`, the texture is a square power-of-two JPEG ≤ `roof_texture_max_px`, and the atlas texcoords are within [0, 1]. Without `roof_textures` no GLB has a texture or the key.

- [ ] **Step 1: Write the failing tests**

Append to `scripts/scene/tests/test_buildings_verify.py` (add `import numpy as np`, `from PIL import Image`, `from fake_sources import source`, `from rasters import write_geotiff`, `from camsim_scene import pipeline`, `from camsim_scene.manifest import Manifest`, `from camsim_scene.pipeline import build_scene` at the top):

```python
def _lc_roof_package(tmp_path, monkeypatch, name="pkg"):
    """The textured-roof scene with land cover (tree cover everywhere through a patched COG reader, cut in-process)."""
    from test_buildings_layer import _roof_scene

    scene = _roof_scene(tmp_path)
    write_geotiff(tmp_path / "src" / "wc.tif", np.full((10, 10), 10, np.uint8), 10.0, 10.5, 0.05, overviews=(), tiled=False)
    scene["priorities"]["landcover"] = ["worldcover"]
    scene["sources"]["worldcover"] = source("landcover", tmp_path / "src" / "wc.tif", [10.0, 10.0, 10.5, 10.5])
    monkeypatch.setattr(pipeline.landcover, "local_reader", lambda paths: lambda url, r, c: np.full((600, 600), 10, np.uint8))
    monkeypatch.setattr(pipeline, "_run_isolated", lambda fn, *a: fn(*a))
    return build_synthetic(tmp_path, scene, name=name)


def _burned_tile(pkg):
    lc = pkg / "landcover"
    for t in json.loads((lc / "index.json").read_text())["tiles"]:
        codes = np.array(Image.open(lc / t["file"]))
        if (codes == 230).any():
            return lc / t["file"], codes
    raise AssertionError("no burned land-cover tile")


def test_deep_checks_mask_burn_and_roofs(tmp_path, monkeypatch):
    pkg, cache, _ = _lc_roof_package(tmp_path, monkeypatch)
    c = _checks(verify(pkg, cache, deep=True))
    assert c["buildings_mask"]["ok"] and c["buildings_roofs"]["ok"] and c["buildings_tileset"]["ok"], c


def test_deep_catches_a_cell_the_burn_lost_or_invented(tmp_path, monkeypatch):
    pkg, cache, _ = _lc_roof_package(tmp_path, monkeypatch)
    path, codes = _burned_tile(pkg)
    r, col = np.argwhere(codes == 230)[0]
    codes[r, col] = 10  # a cell >= 32/64 covered that is no longer a building
    codes[0, 0] = 230  # a building cell with no mask under it
    Image.fromarray(codes).save(path, format="PNG", optimize=True)
    c = _checks(verify(pkg, cache, deep=True))
    assert not c["buildings_mask"]["ok"]
    assert "not 230" in c["buildings_mask"]["detail"] and "without" in c["buildings_mask"]["detail"]


def test_deep_catches_a_missing_roof_texel(tmp_path, monkeypatch):
    pkg, cache, _ = _lc_roof_package(tmp_path, monkeypatch)
    glb = next((pkg / "features/buildings").rglob("*.glb"))
    t = gltf.decode_tile(glb.read_bytes())
    glb.write_bytes(gltf.write_glb(gltf.TileMesh(t.positions, t.normals, t.feature_ids, t.roof_indices, t.wall_indices,
                                                 t.textured_indices, t.texcoords, t.texture, None), t.table, t.copyright))
    c = _checks(verify(pkg, cache, deep=True))
    assert not c["buildings_roofs"]["ok"] and "roof_texel_m" in c["buildings_roofs"]["detail"]


def test_quick_catches_a_missing_mask_tile(tmp_path):
    pkg, _, _ = build_synthetic(tmp_path, _scene(tmp_path, ROWS))
    next((pkg / "features/buildings/mask").glob("*.png")).unlink()
    c = _checks(verify(pkg))
    assert not c["buildings_tileset"]["ok"] and "missing mask/" in c["buildings_tileset"]["detail"]


def test_rebuild_with_textures_and_burn_is_identical(tmp_path, monkeypatch):
    a, cache, _ = _lc_roof_package(tmp_path, monkeypatch, name="a")
    b = tmp_path / "b"
    b.mkdir()
    (b / "manifest.json").write_bytes((a / "manifest.json").read_bytes())
    build_scene(b, cache, jobs=1)
    assert (a / "hashes.txt").read_bytes() == (b / "hashes.txt").read_bytes()


def test_roof_texel_change_rebuilds_tiles_not_the_mask(tmp_path, monkeypatch):
    pkg, cache, _ = _lc_roof_package(tmp_path, monkeypatch)
    before = dict(line.split("  ", 1)[::-1] for line in (pkg / "hashes.txt").read_text().splitlines())
    m = Manifest.load(pkg / "manifest.json")
    m.layers["buildings"]["roof_texel_m"] = 1.0
    m.write(pkg / "manifest.json")
    info = build_scene(pkg, cache, jobs=1)
    after = dict(line.split("  ", 1)[::-1] for line in (pkg / "hashes.txt").read_text().splitlines())
    glbs = [k for k in before if k.endswith(".glb")]
    burn = [k for k in before if k.startswith("features/buildings/mask/") or k.startswith("landcover/")]
    assert glbs and burn and set(before) == set(after)
    assert all(before[k] != after[k] for k in glbs)  # every tile here has a textured roof
    assert all(before[k] == after[k] for k in burn)  # mask and land cover byte-identical
    assert info["buildings"]["roofs"]["tiles_by_texel_m"] == {"1": len(glbs)}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_buildings_verify.py -q`
Expected: the three `deep` tests FAIL with `KeyError: 'buildings_mask'` / `'buildings_roofs'`; `test_quick_catches_a_missing_mask_tile` FAILs (the quick check passes); the two rebuild tests PASS already (Tasks 1–4 are deterministic) and stay as guards.

- [ ] **Step 3: Implement**

In `scripts/scene/camsim_scene/verify.py`, replace `_check_buildings_tileset` and add the two deep checks after `_check_buildings_heights`:

```python
def _check_buildings_tileset(pkg: Path) -> Check:
    from .buildings import walk_tileset

    root = pkg / "features" / "buildings"
    doc = json.loads((root / "tileset.json").read_text())
    missing = [u for u, _ in walk_tileset(doc) if not (root / u).is_file()]
    mask = root / "mask" / "index.json"
    if mask.exists():  # REALISM R3b footprint mask
        index = json.loads(mask.read_text())
        missing += [f"mask/{t['file']}" for t in index["tiles"] if not (mask.parent / t["file"]).is_file()]
    return Check("buildings_tileset", not missing, _summary([f"missing {u}" for u in missing]))


def _check_buildings_mask(pkg: Path) -> Check:
    """Mask <-> burn (REALISM R3b): a land-cover cell is 230 only where >= 32 of its 64 mask cells are set, and such a
    cell is 230 unless a road or rail (200, 201, 210) was burned over it."""
    from .features import BUILDING_CODE
    from .footprints import burn_cells

    root = pkg / "features" / "buildings" / "mask"
    masks = {(t["lat_index"], t["lon_index"]): root / t["file"]
             for t in json.loads((root / "index.json").read_text())["tiles"]}
    lc = pkg / "landcover"
    problems = []
    for t in json.loads((lc / "index.json").read_text())["tiles"]:
        codes = np.array(Image.open(lc / t["file"]))
        key = (t["lat_index"], t["lon_index"])
        burned = burn_cells(np.array(Image.open(masks[key])).astype(bool)) if key in masks else np.zeros(codes.shape, bool)
        stray = int(((codes == BUILDING_CODE) & ~burned).sum())
        lost = int((burned & ~np.isin(codes, (BUILDING_CODE, 200, 201, 210))).sum())
        if stray:
            problems.append(f"{t['file']}: {stray} cells are 230 without >= 32 of 64 mask cells")
        if lost:
            problems.append(f"{t['file']}: {lost} cells with >= 32 of 64 mask cells are not 230 (nor road or rail)")
    return Check("buildings_mask", not problems, _summary(problems))


def _check_buildings_roofs(pkg: Path, m: Manifest) -> Check:
    """Roof atlases (REALISM R3b): roof_texel_m present per tile and consistent with the texture."""
    from .buildings import walk_tileset
    from .gltf import decode_tile
    from .roofatlas import texel_ladder

    s = m.layers["buildings"]
    want = bool(s.get("roof_textures", False))
    ladder = texel_ladder(float(s["roof_texel_m"])) if want else ()
    root = pkg / "features" / "buildings"
    problems = []
    for uri, _ in walk_tileset(json.loads((root / "tileset.json").read_text())):
        t = decode_tile((root / uri).read_bytes())
        has = t.extras is not None and "roof_texel_m" in t.extras
        if not want:
            if has or t.texture is not None:
                problems.append(f"{uri}: roof texture or roof_texel_m with roof_textures off")
            continue
        if not has:
            problems.append(f"{uri}: no roof_texel_m")
            continue
        texel = t.extras["roof_texel_m"]
        if (texel is None) != (t.texture is None):
            problems.append(f"{uri}: roof_texel_m {texel} but texture {'absent' if t.texture is None else 'present'}")
            continue
        if texel is None:
            continue
        if not any(abs(texel - x) < 1e-9 for x in ladder):
            problems.append(f"{uri}: roof_texel_m {texel} not in {ladder}")
        img = Image.open(io.BytesIO(t.texture))
        w, h = img.size
        if img.format != "JPEG" or w != h or w & (w - 1) or w > int(s["roof_texture_max_px"]):
            problems.append(f"{uri}: roof atlas {img.format} {w}x{h}")
        uv = t.texcoords[t.textured_indices] if t.texcoords is not None else np.zeros((0, 2))
        if len(uv) == 0 or uv.min() < 0.0 or uv.max() > 1.0:
            problems.append(f"{uri}: roof texcoords outside [0, 1]")
    return Check("buildings_roofs", not problems, _summary(problems))
```

In `_deep`, replace the buildings block at its end with:

```python
    buildings_ts = pkg / "features" / "buildings" / "tileset.json"
    if m.layers.get("buildings") and buildings_ts.exists():
        checks.append(_guard("buildings_heights", _check_buildings_heights, pkg, m, st))
        checks.append(_guard("buildings_roofs", _check_buildings_roofs, pkg, m))
    mask_index = pkg / "features" / "buildings" / "mask" / "index.json"
    if (m.layers.get("buildings") and m.layers["landcover"].get("buildings") and mask_index.exists()
            and (pkg / "landcover" / "index.json").exists()):
        checks.append(_guard("buildings_mask", _check_buildings_mask, pkg))
    return checks, stats, ndvi_stats
```

Update the module docstring's last sentence before "Writes": "…checks a sample of NDVI leaf pixels against the sources (`ndvi_values`), and for buildings: roof heights, the mask against the land-cover burn and the roof atlases (REALISM R3b)."

- [ ] **Step 4: Run the tests to verify they pass**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests -q`
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add scripts/scene/camsim_scene/verify.py scripts/scene/tests/test_buildings_verify.py
git commit -m "feat(scene): verify the footprint mask against the land-cover burn and the roof atlases (REALISM R3b)"
```

---
### Task 6: Runtime mask reader (`FVegetationBuildingMask`) and the package's mask directory

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Vegetation/VegetationBuildingMask.h`, `.../Vegetation/VegetationBuildingMask.cpp`, `.../Tests/VegetationBuildingMaskTest.cpp`
- Modify: `.../Config/CamSimConfig.h:474` (`FSceneConfig::BuildingsMaskDir`), `.../Config/ScenePackage.cpp:95,173-177` (reset and resolve), `.../Config/ScenePackage.h` (`ResolvePackage` comment), `.../Tests/ScenePackageTest.cpp` (new test)

**Interfaces:**
- Consumes: `FLandCoverTileCache::ResolveDir`, `CamSimLandCover::MeridionalRadiusM` / `PrimeVerticalRadiusM` (`Thermal/LandCoverGeometry.h`), the mask format of Task 1 (`camsim-buildings-mask-1`, `tile_px` 4800, `tile_deg` 0.05, names `%+.2f_%+.2f.png`, row 0 = north, 1-bit greyscale).
- Produces:
  - `FCamSimConfig::FSceneConfig::BuildingsMaskDir` (`FString`, absolute directory; empty unless the package has `features/buildings/mask/index.json`).
  - `class FVegetationBuildingMask` with `TilePx = 4800`, `TileDeg = 0.05`, `CellsPerDeg = 96000.0`, `MaxTiles = 8`, `WordsPerTile = 360000`; `static bool ParseIndex(const FString& Json, TMap<FIntPoint, FString>& OutFiles, FString& OutError)`; `static bool DecodeMaskPng(IImageWrapperModule&, TConstArrayView<uint8> Png, TArray<uint64>& OutBits, FString& OutError)` (1-bit or 8-bit greyscale, nonzero = building); `static void CellOf(double LatDeg, double LonDeg, FIntPoint& OutTile, FIntPoint& OutInTile)` (tile = (LatIndex, LonIndex), in-tile = (column, row)); `static bool IsClearOf(double LatDeg, double LonDeg, double ClearanceM, TFunctionRef<bool(double, double)> IsMaskedAt)` (the 5-point rule); `explicit FVegetationBuildingMask(const FString& Dir)` (game thread); `bool IsValid() const`; `const FString& GetError() const`; `bool IsMasked(double LatDeg, double LonDeg)` and `bool IsClear(double LatDeg, double LonDeg, double ClearanceM)` (any thread); `int32 NumCached() const`; `int32 NumLoads() const`.

- [ ] **Step 1: Write the failing tests**

`unreal_project/CamSimTest/Source/CamSimTest/Tests/VegetationBuildingMaskTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Async/ParallelFor.h"
#include "IImageWrapperModule.h"
#include "Misc/AutomationTest.h"
#include "Misc/Compression.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"
#include "Tests/LandCoverTestTiles.h"
#include "Thermal/LandCoverGeometry.h"
#include "Vegetation/VegetationBuildingMask.h"

namespace
{
	constexpr int32 Px = FVegetationBuildingMask::TilePx;
	constexpr double Half = 0.5 / FVegetationBuildingMask::CellsPerDeg;   // half a mask cell, degrees

	uint32 PngCrc(const uint8* D, int64 N)
	{
		uint32 C = 0xFFFFFFFFu;
		for (int64 I = 0; I < N; ++I)
		{
			C ^= D[I];
			for (int32 K = 0; K < 8; ++K) C = (C >> 1) ^ (0xEDB88320u & (0u - (C & 1u)));
		}
		return ~C;
	}

	void PutBE32(TArray<uint8>& O, uint32 V) { O.Add(uint8(V >> 24)); O.Add(uint8(V >> 16)); O.Add(uint8(V >> 8)); O.Add(uint8(V)); }

	void PutChunk(TArray<uint8>& O, const char* Type, const TArray<uint8>& Data)
	{
		PutBE32(O, static_cast<uint32>(Data.Num()));
		const int32 Start = O.Num();
		for (int32 K = 0; K < 4; ++K) O.Add(uint8(Type[K]));
		O.Append(Data);
		PutBE32(O, PngCrc(O.GetData() + Start, O.Num() - Start));
	}

	/** A Px x Px 1-bit greyscale PNG, as camsim-scene writes the mask (Pillow mode "1"): Set(column, row), row 0 = north. */
	TArray<uint8> MakeOneBitPng(TFunctionRef<bool(int32, int32)> Set)
	{
		const int32 RowBytes = Px / 8;
		TArray<uint8> Raw;
		Raw.SetNumZeroed((RowBytes + 1) * Px);   // filter byte 0 (none) + packed row, MSB first
		for (int32 Y = 0; Y < Px; ++Y)
		{
			uint8* R = Raw.GetData() + Y * (RowBytes + 1) + 1;
			for (int32 X = 0; X < Px; ++X) if (Set(X, Y)) R[X >> 3] |= uint8(0x80u >> (X & 7));
		}
		int32 ZLen = FCompression::CompressMemoryBound(NAME_Zlib, Raw.Num());
		TArray<uint8> Z;
		Z.SetNumUninitialized(ZLen);
		verify(FCompression::CompressMemory(NAME_Zlib, Z.GetData(), ZLen, Raw.GetData(), Raw.Num()));
		Z.SetNum(ZLen);
		TArray<uint8> Ihdr;
		PutBE32(Ihdr, Px); PutBE32(Ihdr, Px);
		Ihdr.Append({ 1, 0, 0, 0, 0 });   // bit depth 1, greyscale, deflate, filter 0, no interlace
		TArray<uint8> Png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
		PutChunk(Png, "IHDR", Ihdr);
		PutChunk(Png, "IDAT", Z);
		PutChunk(Png, "IEND", TArray<uint8>());
		return Png;
	}

	FString TileFile(FIntPoint Tile) { return FString::Printf(TEXT("%+.2f_%+.2f.png"), Tile.X * 0.05, Tile.Y * 0.05); }

	/** A mask directory with these tiles (file bytes as given) in its index. */
	FString WriteMaskDir(const TCHAR* Name, const TArray<TPair<FIntPoint, TArray<uint8>>>& Tiles, const TArray<FIntPoint>& IndexedOnly = {})
	{
		const FString Dir = CamSimLandCoverTest::TempDir(Name);
		FString Entries;
		auto Entry = [&Entries](FIntPoint K)
		{
			Entries += FString::Printf(TEXT("%s{\"file\": \"%s\", \"lat_index\": %d, \"lon_index\": %d}"),
				Entries.IsEmpty() ? TEXT("") : TEXT(", "), *TileFile(K), K.X, K.Y);
		};
		for (const auto& T : Tiles) { FFileHelper::SaveArrayToFile(T.Value, *(Dir / TileFile(T.Key))); Entry(T.Key); }
		for (const FIntPoint K : IndexedOnly) Entry(K);
		FFileHelper::SaveStringToFile(FString::Printf(
			TEXT("{\"format\": \"camsim-buildings-mask-1\", \"tile_deg\": 0.05, \"tile_px\": 4800, \"cell_factor\": 8, ")
			TEXT("\"rules\": \"1\", \"tiles\": [%s]}"), *Entries), *(Dir / TEXT("index.json")));
		return Dir;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegMaskCellOfTest, "CamSim.Vegetation.BuildingMask.CellOfMatchesTheBuildGrid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegMaskCellOfTest::RunTest(const FString& Parameters)
{
	FIntPoint Tile, In;
	FVegetationBuildingMask::CellOf(33.3001, -117.4999, Tile, In);   // camsim-scene: tile (666, -2350), column 9, row 4790
	TestEqual(TEXT("tile"), Tile, FIntPoint(666, -2350));
	TestEqual(TEXT("cell"), In, FIntPoint(9, 4790));
	FVegetationBuildingMask::CellOf(33.30 + Half, -117.30 + Half, Tile, In);   // just NE of a tile corner
	TestEqual(TEXT("NE tile"), Tile, FIntPoint(666, -2346));
	TestEqual(TEXT("its SW cell"), In, FIntPoint(0, Px - 1));
	FVegetationBuildingMask::CellOf(33.30 - Half, -117.30 - Half, Tile, In);
	TestEqual(TEXT("SW tile"), Tile, FIntPoint(665, -2347));
	TestEqual(TEXT("its NE cell"), In, FIntPoint(Px - 1, 0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegMaskDecodeTest, "CamSim.Vegetation.BuildingMask.DecodesOneAndEightBitPngs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegMaskDecodeTest::RunTest(const FString& Parameters)
{
	IImageWrapperModule& M = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	auto Bit = [](const TArray<uint64>& B, int32 X, int32 Y) { const int64 K = int64(Y) * Px + X; return ((B[K >> 6] >> (K & 63)) & 1u) != 0; };
	TArray<uint64> Bits;
	FString Error;
	const TArray<uint8> One = MakeOneBitPng([](int32 X, int32 Y) { return (X == 0 && Y == 0) || (X == Px - 1 && Y == 0) || (X == 7 && Y == 9); });
	if (TestTrue(*FString::Printf(TEXT("1-bit decodes (%s)"), *Error), FVegetationBuildingMask::DecodeMaskPng(M, One, Bits, Error)))
	{
		TestEqual(TEXT("words"), Bits.Num(), FVegetationBuildingMask::WordsPerTile);
		TestTrue(TEXT("(0, 0)"), Bit(Bits, 0, 0));
		TestTrue(TEXT("(4799, 0)"), Bit(Bits, Px - 1, 0));
		TestTrue(TEXT("(7, 9)"), Bit(Bits, 7, 9));
		TestFalse(TEXT("(1, 0)"), Bit(Bits, 1, 0));
		int64 Set = 0;
		for (const uint64 W : Bits) Set += FMath::CountBits(W);
		TestEqual(TEXT("exactly three set"), Set, int64(3));
	}
	TArray<uint8> Grey;
	Grey.SetNumZeroed(Px * Px);
	Grey[12 * Px + 34] = 255;
	const TArray64<uint8> Eight = CamSimLandCoverTest::EncodeGreyPng(Grey, Px, Px);
	if (TestTrue(TEXT("8-bit decodes"), FVegetationBuildingMask::DecodeMaskPng(M, TConstArrayView<uint8>(Eight.GetData(), Eight.Num()), Bits, Error)))
	{
		TestTrue(TEXT("(34, 12)"), Bit(Bits, 34, 12));
	}
	TArray<uint8> Small;
	Small.SetNumZeroed(600 * 600);
	const TArray64<uint8> Wrong = CamSimLandCoverTest::EncodeGreyPng(Small, 600, 600);
	TestFalse(TEXT("a land-cover-sized PNG is refused"), FVegetationBuildingMask::DecodeMaskPng(M, TConstArrayView<uint8>(Wrong.GetData(), Wrong.Num()), Bits, Error));
	TestFalse(TEXT("an LFS pointer is refused"), FVegetationBuildingMask::DecodeMaskPng(M, CamSimLandCoverTest::LfsPointerBytes(), Bits, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegMaskCornersTest, "CamSim.Vegetation.BuildingMask.TileEdgesAndCorners",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegMaskCornersTest::RunTest(const FString& Parameters)
{
	// Four tiles meet at (33.30, -117.30); each sets only its cell at that corner.
	const FIntPoint NE(666, -2346), NW(666, -2347), SE(665, -2346), SW(665, -2347);
	const FString Dir = WriteMaskDir(TEXT("MaskCorners"), {
		{ NE, MakeOneBitPng([](int32 X, int32 Y) { return X == 0 && Y == Px - 1; }) },
		{ NW, MakeOneBitPng([](int32 X, int32 Y) { return X == Px - 1 && Y == Px - 1; }) },
		{ SE, MakeOneBitPng([](int32 X, int32 Y) { return X == 0 && Y == 0; }) },
		{ SW, MakeOneBitPng([](int32 X, int32 Y) { return X == Px - 1 && Y == 0; }) } });
	FVegetationBuildingMask Mask(Dir);
	if (!TestTrue(*FString::Printf(TEXT("valid (%s)"), *Mask.GetError()), Mask.IsValid())) return false;
	TestTrue(TEXT("NE corner cell"), Mask.IsMasked(33.30 + Half, -117.30 + Half));
	TestTrue(TEXT("NW corner cell"), Mask.IsMasked(33.30 + Half, -117.30 - Half));
	TestTrue(TEXT("SE corner cell"), Mask.IsMasked(33.30 - Half, -117.30 + Half));
	TestTrue(TEXT("SW corner cell"), Mask.IsMasked(33.30 - Half, -117.30 - Half));
	TestFalse(TEXT("one cell north of NE"), Mask.IsMasked(33.30 + 3.0 * Half, -117.30 + Half));
	TestFalse(TEXT("one cell east of SE"), Mask.IsMasked(33.30 - Half, -117.30 + 3.0 * Half));
	TestEqual(TEXT("four tiles loaded"), Mask.NumLoads(), 4);
	// A tree 1.5 cells east of the corner, in the NE tile's bottom row: its west probe (2 m, ~2.1 cells) lands in NW's corner cell
	const double DLon = 1.5 * 2.0 * Half;
	TestFalse(TEXT("west probe hits the NW corner cell"), Mask.IsClear(33.30 + Half, -117.30 + DLon, 2.0));
	TestTrue(TEXT("clearance 0 tests the trunk only"), Mask.IsClear(33.30 + Half, -117.30 + DLon, 0.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegMaskBadTilesTest, "CamSim.Vegetation.BuildingMask.BadTilesAreClear",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegMaskBadTilesTest::RunTest(const FString& Parameters)
{
	const FIntPoint Good(666, -2346), Lfs(666, -2347), Gone(665, -2346);
	const FString Dir = WriteMaskDir(TEXT("MaskBad"), {
		{ Good, MakeOneBitPng([](int32, int32) { return true; }) },
		{ Lfs, CamSimLandCoverTest::LfsPointerBytes() } }, { Gone });   // Gone: in the index, no file
	FVegetationBuildingMask Mask(Dir);
	if (!TestTrue(TEXT("valid"), Mask.IsValid())) return false;
	AddExpectedMessage(TEXT("building mask tile"), EAutomationExpectedErrorFlags::Contains, 0);   // the one warning
	TestTrue(TEXT("good tile answers"), Mask.IsMasked(33.31, -117.29));
	TestFalse(TEXT("LFS pointer tile = no buildings"), Mask.IsMasked(33.31, -117.31));
	TestFalse(TEXT("missing file = no buildings"), Mask.IsMasked(33.29, -117.29));
	TestFalse(TEXT("tile not in the index = no buildings"), Mask.IsMasked(10.0, 10.0));
	TestFalse(TEXT("asked again: still clear, no reload"), Mask.IsMasked(33.31, -117.31));
	TestEqual(TEXT("only the good tile decoded"), Mask.NumLoads(), 1);
	FVegetationBuildingMask NoIndex(CamSimLandCoverTest::TempDir(TEXT("MaskNone")));
	TestFalse(TEXT("no index.json: invalid"), NoIndex.IsValid());
	TestFalse(TEXT("invalid: clear everywhere"), NoIndex.IsMasked(33.31, -117.29));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegMaskFivePointTest, "CamSim.Vegetation.BuildingMask.FivePointRule",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegMaskFivePointTest::RunTest(const FString& Parameters)
{
	// A building east of Lon0 and north of Lat0 (its south-west corner at (Lat0, Lon0)); metres -> degrees at Lat0.
	const double Lat0 = 33.3, Lon0 = -117.3;
	const double MLat = FMath::DegreesToRadians(1.0) * CamSimLandCover::MeridionalRadiusM(Lat0);
	const double MLon = FMath::DegreesToRadians(1.0) * CamSimLandCover::PrimeVerticalRadiusM(Lat0) * FMath::Cos(FMath::DegreesToRadians(Lat0));
	auto In = [&](double La, double Lo) { return La >= Lat0 && Lo >= Lon0; };
	auto At = [&](double NorthM, double EastM, double Clearance)
	{
		return FVegetationBuildingMask::IsClearOf(Lat0 + NorthM / MLat, Lon0 + EastM / MLon, Clearance, In);
	};
	TestFalse(TEXT("trunk inside"), At(5.0, 5.0, 2.0));
	TestFalse(TEXT("1.5 m west of the wall: east probe inside"), At(5.0, -1.5, 2.0));
	TestTrue (TEXT("2.5 m west of the wall: clear"), At(5.0, -2.5, 2.0));
	TestFalse(TEXT("1.5 m south of the wall: north probe inside"), At(-1.5, 5.0, 2.0));
	TestTrue (TEXT("clearance 0: only the trunk"), At(5.0, -1.5, 0.0));
	TestTrue (TEXT("diagonal from the corner: axis probes miss it (documented limit)"), At(-1.0, -1.0, 2.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegMaskThreadsTest, "CamSim.Vegetation.BuildingMask.AnyThreadAndLru",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegMaskThreadsTest::RunTest(const FString& Parameters)
{
	// Ten tiles in a row along 33.30 N; tile k is all set when k is odd, empty when even.
	TArray<TPair<FIntPoint, TArray<uint8>>> Tiles;
	for (int32 K = 0; K < 10; ++K)
		Tiles.Add({ FIntPoint(666, -2350 + K), MakeOneBitPng([K](int32, int32) { return (K & 1) != 0; }) });
	FVegetationBuildingMask Mask(WriteMaskDir(TEXT("MaskThreads"), Tiles));
	if (!TestTrue(TEXT("valid"), Mask.IsValid())) return false;
	TArray<int32> Wrong;
	Wrong.SetNumZeroed(400);
	ParallelFor(400, [&Mask, &Wrong](int32 I)
	{
		const int32 K = (I * 7) % 10;
		const double Lon = (-2350 + K) * 0.05 + 0.025;
		Wrong[I] = Mask.IsMasked(33.325, Lon) != ((K & 1) != 0) ? 1 : 0;
	});
	int32 Bad = 0;
	for (const int32 W : Wrong) Bad += W;
	TestEqual(TEXT("every answer right"), Bad, 0);
	TestTrue(*FString::Printf(TEXT("at most %d tiles cached (%d)"), FVegetationBuildingMask::MaxTiles, Mask.NumCached()),
		Mask.NumCached() <= FVegetationBuildingMask::MaxTiles);
	return true;
}
```

Append to `.../Tests/ScenePackageTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneResolveBuildingsMaskTest,
	"CamSim.Scene.Resolve.BuildingsMask",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneResolveBuildingsMaskTest::RunTest(const FString& Parameters)
{
	const FString D = MakePackage(TEXT("buildings_mask"), true, true, true);
	WriteFile(D / TEXT("features/buildings/tileset.json"), TEXT("{}"));
	TestTrue(TEXT("no mask index: empty"), ResolvedConfigFor(D).Scene.BuildingsMaskDir.IsEmpty());
	WriteFile(D / TEXT("features/buildings/mask/index.json"), TEXT("{}"));
	TestEqual(TEXT("mask dir"), ResolvedConfigFor(D).Scene.BuildingsMaskDir, D / TEXT("features/buildings/mask"));
	FCamSimConfig Off = ResolvedConfigFor(D);
	Off.Scene.Dir.Reset();
	CamSimScene::ResolvePackage(Off);
	TestTrue(TEXT("cleared when scene.dir is unset"), Off.Scene.BuildingsMaskDir.IsEmpty());
	return true;
}
```

- [ ] **Step 2: Build to verify they fail**

Run: `scripts/run.sh --build-only`
Expected: compile errors: `'Vegetation/VegetationBuildingMask.h' file not found` and `no member named 'BuildingsMaskDir'`.

- [ ] **Step 3: Add the config field and resolve the mask directory**

In `Config/CamSimConfig.h`, after `FString BuildingsUrl;` (line 474):

```cpp
		// Absolute features/buildings/mask/ of the package when it has index.json (REALISM R3b; the tree scatter reads it).
		FString BuildingsMaskDir;
```

In `Config/ScenePackage.cpp`, `ResolvePackage`: after `Scene.BuildingsUrl.Reset();` add `Scene.BuildingsMaskDir.Reset();`, and after the `BuildingsFile` block:

```cpp
	if (FPaths::FileExists(Dir / TEXT("features/buildings/mask/index.json")))
	{
		Scene.BuildingsMaskDir = Dir / TEXT("features/buildings/mask");
	}
```

In `Config/ScenePackage.h`, extend the `ResolvePackage` comment: "…features/buildings/tileset.json sets Cfg.Scene.BuildingsUrl and features/buildings/mask/index.json Cfg.Scene.BuildingsMaskDir (REALISM R3b)."

- [ ] **Step 4: Write `VegetationBuildingMask.h`**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include <atomic>

class IImageWrapperModule;

/**
 * Building footprint mask for the tree scatter (REALISM R3b, docs/vegetation.md): the scene package's
 * features/buildings/mask/ (camsim-buildings-mask-1: the land-cover tile grid at 8 x its resolution, 4800^2 1-bit PNGs,
 * row 0 = north, a cell set where its centre is inside a footprint). Tiles decode on the caller (the scatter worker) into
 * bitsets (~2.9 MB) held in a MaxTiles LRU (~23 MB), separate from the land-cover sampler. A tile missing from the index,
 * missing on disk or unreadable (a git LFS pointer) means "no buildings" there; the first failure logs one warning.
 * IsMasked / IsClear are thread-safe. No network.
 */
class CAMSIMTEST_API FVegetationBuildingMask
{
public:
	static constexpr int32  TilePx = 4800;
	static constexpr double TileDeg = 0.05;
	static constexpr double CellsPerDeg = 96000.0;   // 12000 WorldCover cells per degree x 8
	static constexpr int32  MaxTiles = 8;
	static constexpr int32  WordsPerTile = TilePx * TilePx / 64;   // 360000 uint64

	static bool ParseIndex(const FString& Json, TMap<FIntPoint, FString>& OutFiles, FString& OutError);
	/** 1-bit (as written) or 8-bit greyscale TilePx^2 PNG -> bitset, bit (row * TilePx + column); nonzero = building. */
	static bool DecodeMaskPng(IImageWrapperModule& Module, TConstArrayView<uint8> Png, TArray<uint64>& OutBits, FString& OutError);
	/** The mask cell holding (Lat, Lon): OutTile = (LatIndex, LonIndex), OutInTile = (column, row), as camsim-scene grids it. */
	static void CellOf(double LatDeg, double LonDeg, FIntPoint& OutTile, FIntPoint& OutInTile);
	/** The 5-point rule: clear when neither the point nor the 4 points ClearanceM east, west, north and south of it are
	 *  masked. Axis probes only: a footprint corner diagonally within ClearanceM is not seen. */
	static bool IsClearOf(double LatDeg, double LonDeg, double ClearanceM, TFunctionRef<bool(double, double)> IsMaskedAt);

	/** Game thread (loads the ImageWrapper module). Dir = Cfg.Scene.BuildingsMaskDir (relative = project dir). */
	explicit FVegetationBuildingMask(const FString& Dir);

	bool IsValid() const { return bValid; }
	const FString& GetError() const { return Error; }
	/** Any thread. False with an invalid mask, a non-finite point or no tile there. */
	bool IsMasked(double LatDeg, double LonDeg);
	/** Any thread. IsClearOf with this mask. */
	bool IsClear(double LatDeg, double LonDeg, double ClearanceM);
	int32 NumCached() const;
	/** Tiles decoded and kept so far (hits and a concurrent duplicate decode don't count). */
	int32 NumLoads() const;

private:
	struct FTile
	{
		TArray<uint64> Bits;   // WordsPerTile
	};
	struct FEntry
	{
		TSharedPtr<const FTile, ESPMode::ThreadSafe> Tile;
		uint64 LastUse = 0;
	};

	TSharedPtr<const FTile, ESPMode::ThreadSafe> Get(FIntPoint Key);

	FString Dir;
	FString Error;
	bool    bValid = false;
	IImageWrapperModule* ImageWrapper = nullptr;
	TMap<FIntPoint, FString> Files;   // immutable after construction

	mutable FCriticalSection Lock;    // guards everything below; never held across file I/O or decode
	TMap<FIntPoint, FEntry> Cache;
	TSet<FIntPoint> Failed;
	uint64 UseCounter = 0;
	int32  Loads = 0;
	std::atomic<bool> bWarned{ false };
};
```

- [ ] **Step 5: Write `VegetationBuildingMask.cpp`**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Vegetation/VegetationBuildingMask.h"

#include "CamSimTest.h"
#include "Dom/JsonObject.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Thermal/LandCoverGeometry.h"
#include "Thermal/LandCoverTiles.h"

namespace
{
	const TCHAR* MaskFormat = TEXT("camsim-buildings-mask-1");
	constexpr int32  Px = FVegetationBuildingMask::TilePx;
	constexpr int32  GlobalRows = 17280000;   // 180 deg x 96000
	constexpr int32  GlobalCols = 34560000;   // 360 deg x 96000
	constexpr int32  TileRowsToEquator = GlobalRows / Px / 2;   // 1800
	constexpr int32  TileColsToMeridian = GlobalCols / Px / 2;  // 3600
	constexpr double CellEps = 1e-6;   // cells: absorbs round-off at exact grid lines (as CamSimLandCover::GlobalCell)

	bool GetIndexField(const FJsonObject& O, const TCHAR* Key, int32 Lo, int32 Hi, int32& Out)
	{
		double V = 0.0;
		if (!O.TryGetNumberField(Key, V) || !FMath::IsFinite(V) || V != FMath::FloorToDouble(V) || V < Lo || V > Hi) return false;
		Out = static_cast<int32>(V);
		return true;
	}
}

bool FVegetationBuildingMask::ParseIndex(const FString& Json, TMap<FIntPoint, FString>& OutFiles, FString& OutError)
{
	OutFiles.Reset();
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		OutError = TEXT("index.json is not valid JSON");
		return false;
	}
	FString Format;
	if (!Root->TryGetStringField(TEXT("format"), Format) || Format != MaskFormat)
	{
		OutError = FString::Printf(TEXT("index.json format '%s' is not %s"), *Format, MaskFormat);
		return false;
	}
	double Deg = 0.0, TilePxField = 0.0;
	if (!Root->TryGetNumberField(TEXT("tile_deg"), Deg) || FMath::Abs(Deg - TileDeg) > 1e-9
		|| !Root->TryGetNumberField(TEXT("tile_px"), TilePxField) || TilePxField != static_cast<double>(TilePx))
	{
		OutError = FString::Printf(TEXT("index.json tile_deg / tile_px must be %.2f / %d"), TileDeg, TilePx);
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Tiles = nullptr;
	if (!Root->TryGetArrayField(TEXT("tiles"), Tiles))
	{
		OutError = TEXT("index.json has no tiles array");
		return false;
	}
	for (int32 K = 0; K < Tiles->Num(); ++K)
	{
		const TSharedPtr<FJsonObject>* T = nullptr;
		FString File;
		int32 Lat = 0, Lon = 0;
		const bool bOk = (*Tiles)[K].IsValid() && (*Tiles)[K]->TryGetObject(T) && T && (*T).IsValid()
			&& (*T)->TryGetStringField(TEXT("file"), File) && File.EndsWith(TEXT(".png"))
			&& !File.Contains(TEXT("/")) && !File.Contains(TEXT("\\")) && !File.Contains(TEXT(".."))
			&& GetIndexField(**T, TEXT("lat_index"), -1800, 1799, Lat) && GetIndexField(**T, TEXT("lon_index"), -3600, 3599, Lon);
		if (!bOk)
		{
			OutError = FString::Printf(TEXT("index.json tiles[%d] is malformed"), K);
			return false;
		}
		OutFiles.Add(FIntPoint(Lat, Lon), File);
	}
	return true;
}

bool FVegetationBuildingMask::DecodeMaskPng(IImageWrapperModule& Module, TConstArrayView<uint8> Png, TArray<uint64>& OutBits, FString& OutError)
{
	const TSharedPtr<IImageWrapper> W = Module.CreateImageWrapper(EImageFormat::PNG);
	if (!W.IsValid() || Png.Num() == 0 || !W->SetCompressed(Png.GetData(), Png.Num()))
	{
		OutError = TEXT("not a PNG (a git LFS pointer? run git lfs pull)");
		return false;
	}
	const int32 Depth = W->GetBitDepth();
	if (W->GetWidth() != Px || W->GetHeight() != Px || W->GetFormat() != ERGBFormat::Gray || (Depth != 1 && Depth != 8))
	{
		OutError = FString::Printf(TEXT("%lldx%lld, %d-bit: expected a %dx%d 1-bit greyscale PNG"),
			static_cast<long long>(W->GetWidth()), static_cast<long long>(W->GetHeight()), Depth, Px, Px);
		return false;
	}
	TArray64<uint8> Raw;   // libpng expands 1-bit grey to 0 / 255
	if (!W->GetRaw(ERGBFormat::Gray, 8, Raw) || Raw.Num() != static_cast<int64>(Px) * Px)
	{
		OutError = TEXT("PNG decode failed");
		return false;
	}
	OutBits.SetNumZeroed(WordsPerTile);
	for (int64 K = 0; K < Raw.Num(); ++K)
	{
		if (Raw[K] != 0) OutBits[K >> 6] |= uint64(1) << (K & 63);
	}
	return true;
}

void FVegetationBuildingMask::CellOf(double LatDeg, double LonDeg, FIntPoint& OutTile, FIntPoint& OutInTile)
{
	const int32 Row = FMath::Clamp(FMath::FloorToInt32((90.0 - LatDeg) * CellsPerDeg + CellEps), 0, GlobalRows - 1);
	int64 Col = FMath::FloorToInt64((LonDeg + 180.0) * CellsPerDeg + CellEps) % GlobalCols;
	if (Col < 0) Col += GlobalCols;
	const int32 C = static_cast<int32>(Col);
	OutTile = FIntPoint(TileRowsToEquator - 1 - Row / Px, C / Px - TileColsToMeridian);
	OutInTile = FIntPoint(C % Px, Row % Px);
}

bool FVegetationBuildingMask::IsClearOf(double LatDeg, double LonDeg, double ClearanceM, TFunctionRef<bool(double, double)> IsMaskedAt)
{
	if (IsMaskedAt(LatDeg, LonDeg)) return false;
	if (!(ClearanceM > 0.0)) return true;
	const double DLat = FMath::RadiansToDegrees(ClearanceM / CamSimLandCover::MeridionalRadiusM(LatDeg));
	const double DLon = FMath::RadiansToDegrees(ClearanceM
		/ (CamSimLandCover::PrimeVerticalRadiusM(LatDeg) * FMath::Max(FMath::Cos(FMath::DegreesToRadians(LatDeg)), 1e-6)));
	return !IsMaskedAt(LatDeg, LonDeg + DLon) && !IsMaskedAt(LatDeg, LonDeg - DLon)
		&& !IsMaskedAt(LatDeg + DLat, LonDeg) && !IsMaskedAt(LatDeg - DLat, LonDeg);
}

FVegetationBuildingMask::FVegetationBuildingMask(const FString& InDir)
	: Dir(FLandCoverTileCache::ResolveDir(InDir))
{
	check(IsInGameThread());
	ImageWrapper = &FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	const FString IndexPath = FPaths::Combine(Dir, TEXT("index.json"));
	FString Json;
	if (Dir.IsEmpty() || !FFileHelper::LoadFileToString(Json, *IndexPath))
	{
		Error = FString::Printf(TEXT("no building mask index at %s"), *IndexPath);
		return;
	}
	FString ParseError;
	if (!ParseIndex(Json, Files, ParseError))
	{
		Error = IndexPath + TEXT(": ") + ParseError;
		return;
	}
	bValid = true;
}

bool FVegetationBuildingMask::IsMasked(double LatDeg, double LonDeg)
{
	if (!bValid || !FMath::IsFinite(LatDeg) || !FMath::IsFinite(LonDeg)) return false;
	FIntPoint Tile, In;
	CellOf(LatDeg, LonDeg, Tile, In);
	const TSharedPtr<const FTile, ESPMode::ThreadSafe> T = Get(Tile);
	if (!T.IsValid()) return false;
	const int64 K = static_cast<int64>(In.Y) * Px + In.X;
	return ((T->Bits[K >> 6] >> (K & 63)) & 1u) != 0;
}

bool FVegetationBuildingMask::IsClear(double LatDeg, double LonDeg, double ClearanceM)
{
	return IsClearOf(LatDeg, LonDeg, ClearanceM, [this](double La, double Lo) { return IsMasked(La, Lo); });
}

TSharedPtr<const FVegetationBuildingMask::FTile, ESPMode::ThreadSafe> FVegetationBuildingMask::Get(FIntPoint Key)
{
	const FString* File = Files.Find(Key);
	if (!File) return nullptr;
	{
		FScopeLock L(&Lock);
		if (FEntry* E = Cache.Find(Key))
		{
			E->LastUse = ++UseCounter;
			return E->Tile;
		}
		if (Failed.Contains(Key)) return nullptr;
	}
	// Unlocked I/O and decode; two threads loading the same tile both decode and the first insert wins.
	const FString Path = FPaths::Combine(Dir, *File);
	TArray<uint8> Png;
	TArray<uint64> Bits;
	FString Why;
	if (!FFileHelper::LoadFileToArray(Png, *Path, FILEREAD_Silent)) Why = TEXT("missing (git lfs pull?)");
	else DecodeMaskPng(*ImageWrapper, Png, Bits, Why);

	FScopeLock L(&Lock);
	if (FEntry* E = Cache.Find(Key))
	{
		E->LastUse = ++UseCounter;
		return E->Tile;
	}
	if (Bits.Num() != WordsPerTile)
	{
		bool bAlready = false;
		Failed.Add(Key, &bAlready);
		if (!bAlready)
		{
			UE_LOG(LogCamSim, Verbose, TEXT("Vegetation: building mask tile %s: %s; treated as no buildings"), *Path, *Why);
			if (!bWarned.exchange(true))
			{
				UE_LOG(LogCamSim, Warning, TEXT("Vegetation: building mask tile %s unreadable (%s); treated as no buildings "
					"(further tiles: log LogCamSim Verbose)"), *Path, *Why);
			}
		}
		return nullptr;
	}
	++Loads;
	TSharedRef<FTile, ESPMode::ThreadSafe> Tile = MakeShared<FTile, ESPMode::ThreadSafe>();
	Tile->Bits = MoveTemp(Bits);
	if (Cache.Num() >= MaxTiles)
	{
		FIntPoint Oldest = FIntPoint::ZeroValue;
		uint64 OldestUse = MAX_uint64;
		for (const TPair<FIntPoint, FEntry>& P : Cache)
		{
			if (P.Value.LastUse < OldestUse) { OldestUse = P.Value.LastUse; Oldest = P.Key; }
		}
		Cache.Remove(Oldest);
	}
	Cache.Add(Key, FEntry{ Tile, ++UseCounter });
	return Tile;
}

int32 FVegetationBuildingMask::NumCached() const
{
	FScopeLock L(&Lock);
	return Cache.Num();
}

int32 FVegetationBuildingMask::NumLoads() const
{
	FScopeLock L(&Lock);
	return Loads;
}
```

- [ ] **Step 6: Build and run the tests**

Run: `scripts/run.sh --build-only`, then the headless command with filter `CamSim.Vegetation.BuildingMask` and again with `CamSim.Scene`.
Expected: build succeeds; `parse_automation_report.py` exits 0 for both (6 BuildingMask tests; every `CamSim.Scene.*` including `Resolve.BuildingsMask`). `BadTilesAreClear` logs one `Vegetation: building mask tile … unreadable` warning (expected by the test).

If `DecodesOneAndEightBitPngs` fails on the 1-bit case, read `.cache/automation.log` for the error text: the decoder relies on UE's `PngImageWrapper` expanding 1-bit grey (`png_set_expand_gray_1_2_4_to_8`, `PngImageWrapper.cpp` in UE 5.8); do not switch the build tooling to 8-bit PNGs without measuring the package size first.

- [ ] **Step 7: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Vegetation/VegetationBuildingMask.h \
  unreal_project/CamSimTest/Source/CamSimTest/Vegetation/VegetationBuildingMask.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/VegetationBuildingMaskTest.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.h unreal_project/CamSimTest/Source/CamSimTest/Config/ScenePackage.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Config/ScenePackage.h unreal_project/CamSimTest/Source/CamSimTest/Tests/ScenePackageTest.cpp
git commit -m "feat(vegetation): read the scene package's building footprint mask (REALISM R3b)"
```

---
### Task 7: Clearance predicate in the scatter, `vegetation.building_clearance_m`, method order

**Files:**
- Modify: `.../Vegetation/VegetationScatter.h/.cpp` (overload), `.../Vegetation/VegetationManager.h/.cpp` (method enum, selection, wiring, log line), `.../Config/CamSimConfig.h:392` (`BuildingClearanceM`), `.../Config/CamSimConfig.cpp:692,1120,1553` (yaml, env, validation), `.../Tests/VegetationScatterTest.cpp`, `.../Tests/VegetationManagerTest.cpp`, `.../Tests/VegetationConfigTest.cpp`

**Interfaces:**
- Consumes: Task 6's `FVegetationBuildingMask` (`IsValid`, `GetError`, `IsClear`) and `Cfg.Scene.BuildingsMaskDir`; `CamSim::Geospatial::FindBuildingsTileset(UWorld*)`; the existing `GenerateCell(Seed, Cell, P, Sampler)`, `HashInstances`.
- Produces:
  - `CamSimVegetation::FClearPredicate = TFunctionRef<bool(double LatDeg, double LonDeg)>`; `GenerateCell(uint32 Seed, FVegCellId Cell, const FScatterParams& P, FCodeSampler Sampler, FClearPredicate IsClear)` — a candidate accepted by the land cover is dropped when `IsClear` is false; the hash draws per candidate are unchanged, so the kept trees are bit-identical to the 4-argument call's.
  - `enum class CamSimVegetation::EBuildingExclusion : uint8 { Off, None, Mask, RuntimeCheck }`; `EBuildingExclusion ChooseBuildingExclusion(bool bDropInBuildings, bool bHasMask, bool bHasBuildingsTileset)`; `FString DescribeBuildingExclusion(EBuildingExclusion E, float ClearanceM)` (`mask (clearance 2.0 m)`, `runtime check (buildings tileset, no mask)`, `none`, `off (drop_in_buildings false)`).
  - `FVegetationConfig::BuildingClearanceM` (float, 2.0, yaml `vegetation.building_clearance_m`, env `CAMSIM_VEGETATION_BUILDING_CLEARANCE_M`, valid [0, 10]).
  - Log line: `Vegetation: on (type '…', radius … m, density …, shadows …, seed …, land cover …, buildings: <DescribeBuildingExclusion>)`.

- [ ] **Step 1: Write the failing scatter tests**

Append to `.../Tests/VegetationScatterTest.cpp` (add `#include "SensorHash.h"` at the top):

```cpp
namespace
{
	/** VegetationScatter.cpp's GenerateCell as of the R4 slice, verbatim: the oracle for "without a mask every cell hash
	 *  is bit-identical to the R3/R4 build". Do not edit it to follow later scatter changes. */
	TArray<FTreeInstance> R4GenerateCell(uint32 Seed, FVegCellId Cell, const FScatterParams& P, FCodeSampler Sampler)
	{
		constexpr uint32 SubJitterE = 2u * CamSimHash::VegetationStream + 0u;
		constexpr uint32 SubJitterN = 2u * CamSimHash::VegetationStream + 1u;
		constexpr uint32 SubAccept  = 2u * CamSimHash::VegetationStream + 2u;
		constexpr uint32 SubYaw     = 2u * CamSimHash::VegetationStream + 3u;
		constexpr uint32 SubScale   = 2u * CamSimHash::VegetationStream + 4u;
		TArray<FTreeInstance> Out;
		const float Accept = FMath::Clamp(P.DensityScale * P.PTree, 0.0f, 1.0f);
		if (Accept <= 0.0f) return Out;
		const FVegCellBounds B = CellBounds(Cell, P.CellSizeM);
		const int32 N = CandidatesPerAxis(P);
		const uint32 X = static_cast<uint32>(Cell.Col), Y = static_cast<uint32>(Cell.Row);
		for (int32 J = 0; J < N; ++J)
			for (int32 I = 0; I < N; ++I)
			{
				const uint32 K = static_cast<uint32>(J * N + I);
				if (CamSimHash::Uniform(CamSimHash::Hash(X, Y, K, Seed, SubAccept)) >= Accept) continue;
				const double Fx = (I + CamSimHash::Uniform(CamSimHash::Hash(X, Y, K, Seed, SubJitterE))) / N;
				const double Fy = (J + CamSimHash::Uniform(CamSimHash::Hash(X, Y, K, Seed, SubJitterN))) / N;
				FTreeInstance T;
				T.LatDeg = B.SouthDeg + Fy * (B.NorthDeg - B.SouthDeg);
				T.LonDeg = B.WestDeg + Fx * (B.EastDeg - B.WestDeg);
				if (Sampler(T.LatDeg, T.LonDeg) != P.TreeCode) continue;
				T.YawDeg = 360.0f * CamSimHash::Uniform(CamSimHash::Hash(X, Y, K, Seed, SubYaw));
				if (T.YawDeg >= 360.0f) T.YawDeg = 0.0f;
				T.Scale = P.ScaleMin + (P.ScaleMax - P.ScaleMin) * CamSimHash::Uniform(CamSimHash::Hash(X, Y, K, Seed, SubScale));
				Out.Add(T);
			}
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegScatterNoMaskIsR4Test, "CamSim.Vegetation.Scatter.NoMaskIsBitIdenticalToR4",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegScatterNoMaskIsR4Test::RunTest(const FString& Parameters)
{
	const FScatterParams P;
	const uint32 Seed = VegetationSeed(0u);   // Pendleton's manifest seed 0
	const FVegCellId Origin = CellOf(33.302, -117.355, P.CellSizeM);
	auto Mixed = [](double Lat, double Lon) -> uint8 { return FMath::Frac(Lat * 3000.0 + Lon * 2000.0) < 0.6 ? 10 : 30; };
	auto Always = [](double, double) { return true; };
	for (int32 K = 0; K < 25; ++K)
	{
		const FVegCellId C{ Origin.Row + K / 5, Origin.Col + K % 5 };
		const TArray<FTreeInstance> Ref = R4GenerateCell(Seed, C, P, Mixed);
		TestTrue(TEXT("4-argument call == R4"), GenerateCell(Seed, C, P, Mixed) == Ref);
		TestTrue(TEXT("always-clear predicate == R4"), GenerateCell(Seed, C, P, Mixed, Always) == Ref);
		TestEqual(TEXT("hash"), HashInstances(GenerateCell(Seed, C, P, Mixed, Always)), HashInstances(Ref));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegScatterPredicateTest, "CamSim.Vegetation.Scatter.ClearPredicateDropsOnlyRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegScatterPredicateTest::RunTest(const FString& Parameters)
{
	const FScatterParams P;
	const uint32 Seed = VegetationSeed(42u);
	const FVegCellId C = CellOf(33.302, -117.355, P.CellSizeM);
	const FVegCellBounds B = CellBounds(C, P.CellSizeM);
	const double Mid = 0.5 * (B.WestDeg + B.EastDeg);
	const TArray<FTreeInstance> All = GenerateCell(Seed, C, P, AllTrees);
	const TArray<FTreeInstance> East = GenerateCell(Seed, C, P, AllTrees, [Mid](double, double Lon) { return Lon >= Mid; });
	TArray<FTreeInstance> Want;
	for (const FTreeInstance& T : All) if (T.LonDeg >= Mid) Want.Add(T);
	TestTrue(TEXT("some trees on each side"), East.Num() > 0 && East.Num() < All.Num());
	TestTrue(TEXT("exactly the unfiltered east trees, same bits and order"), East == Want);
	TestEqual(TEXT("reject all"), GenerateCell(Seed, C, P, AllTrees, [](double, double) { return false; }).Num(), 0);
	return true;
}
```

Append to `.../Tests/VegetationManagerTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegBuildingsMethodTest, "CamSim.Vegetation.Buildings.MethodSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegBuildingsMethodTest::RunTest(const FString& Parameters)
{
	using E = CamSimVegetation::EBuildingExclusion;
	using CamSimVegetation::ChooseBuildingExclusion;
	TestTrue(TEXT("mask wins over the tileset"), ChooseBuildingExclusion(true, true, true) == E::Mask);
	TestTrue(TEXT("mask without a tileset (buildings disabled)"), ChooseBuildingExclusion(true, true, false) == E::Mask);
	TestTrue(TEXT("R3 package: runtime check"), ChooseBuildingExclusion(true, false, true) == E::RuntimeCheck);
	TestTrue(TEXT("neither: none"), ChooseBuildingExclusion(true, false, false) == E::None);
	TestTrue(TEXT("drop_in_buildings false gates the mask"), ChooseBuildingExclusion(false, true, true) == E::Off);
	TestTrue(TEXT("drop_in_buildings false gates the runtime check"), ChooseBuildingExclusion(false, false, true) == E::Off);
	TestEqual(TEXT("mask text"), CamSimVegetation::DescribeBuildingExclusion(E::Mask, 2.0f), FString(TEXT("mask (clearance 2.0 m)")));
	TestEqual(TEXT("runtime text"), CamSimVegetation::DescribeBuildingExclusion(E::RuntimeCheck, 2.0f),
		FString(TEXT("runtime check (buildings tileset, no mask)")));
	TestEqual(TEXT("none text"), CamSimVegetation::DescribeBuildingExclusion(E::None, 2.0f), FString(TEXT("none")));
	TestEqual(TEXT("off text"), CamSimVegetation::DescribeBuildingExclusion(E::Off, 2.0f), FString(TEXT("off (drop_in_buildings false)")));
	return true;
}
```

In `.../Tests/VegetationConfigTest.cpp`: in `Defaults` add `TestEqual(TEXT("building_clearance_m"), V.BuildingClearanceM, 2.0f);`; in `YamlAndEnv` add the line `"  building_clearance_m: 3.5\n"` to the yaml, `TestEqual(TEXT("clearance"), Cfg.Vegetation.BuildingClearanceM, 3.5f);` after the yaml asserts, `{ TEXT("CAMSIM_VEGETATION_BUILDING_CLEARANCE_M"), TEXT("4") }` to `Vars`, and `TestEqual(TEXT("env clearance"), Cfg.Vegetation.BuildingClearanceM, 4.0f);` after the env asserts; in `Validate` add:

```cpp
	TestTrue (TEXT("clearance -0.5"), Has(With([](auto& V) { V.BuildingClearanceM = -0.5f; }), TEXT("vegetation.building_clearance_m")));
	TestTrue (TEXT("clearance 10.5"), Has(With([](auto& V) { V.BuildingClearanceM = 10.5f; }), TEXT("vegetation.building_clearance_m")));
	TestTrue (TEXT("clearance NaN"),  Has(With([NaN](auto& V) { V.BuildingClearanceM = NaN; }), TEXT("vegetation.building_clearance_m")));
	TestFalse(TEXT("clearance 0"),    Has(With([](auto& V) { V.BuildingClearanceM = 0.0f; }), TEXT("vegetation.building_clearance_m")));
	TestFalse(TEXT("clearance 10"),   Has(With([](auto& V) { V.BuildingClearanceM = 10.0f; }), TEXT("vegetation.building_clearance_m")));
```

- [ ] **Step 2: Build to verify they fail**

Run: `scripts/run.sh --build-only`
Expected: compile errors: no matching `GenerateCell` with 5 arguments, `EBuildingExclusion` and `BuildingClearanceM` undeclared.

- [ ] **Step 3: Implement the scatter overload**

In `.../Vegetation/VegetationScatter.h`, after `using FCodeSampler = ...;`:

```cpp
	/** REALISM R3b: false = the candidate is too close to a building (FVegetationBuildingMask::IsClear). */
	using FClearPredicate = TFunctionRef<bool(double LatDeg, double LonDeg)>;
```

and after the existing `GenerateCell` declaration:

```cpp
	/** GenerateCell with a building clearance test, applied after the land cover; the per-candidate hash draws are
	 *  unchanged, so the trees it keeps are bit-identical to the 4-argument call's. */
	CAMSIMTEST_API TArray<FTreeInstance> GenerateCell(uint32 Seed, FVegCellId Cell, const FScatterParams& P, FCodeSampler Sampler,
		FClearPredicate IsClear);
```

In `.../Vegetation/VegetationScatter.cpp`, move the body of `GenerateCell` into an anonymous-namespace helper with an optional predicate and define both overloads on it:

```cpp
	namespace
	{
		TArray<FTreeInstance> GenerateCellImpl(uint32 Seed, FVegCellId Cell, const FScatterParams& P, FCodeSampler Sampler,
			const FClearPredicate* IsClear)
		{
			TArray<FTreeInstance> Out;
			const float Accept = FMath::Clamp(P.DensityScale * P.PTree, 0.0f, 1.0f);
			if (Accept <= 0.0f) return Out;
			const FVegCellBounds B = CellBounds(Cell, P.CellSizeM);
			const int32 N = CandidatesPerAxis(P);
			const uint32 X = static_cast<uint32>(Cell.Col), Y = static_cast<uint32>(Cell.Row);
			for (int32 J = 0; J < N; ++J)
				for (int32 I = 0; I < N; ++I)
				{
					const uint32 K = static_cast<uint32>(J * N + I);
					if (CamSimHash::Uniform(CamSimHash::Hash(X, Y, K, Seed, SubAccept)) >= Accept) continue;
					const double Fx = (I + CamSimHash::Uniform(CamSimHash::Hash(X, Y, K, Seed, SubJitterE))) / N;
					const double Fy = (J + CamSimHash::Uniform(CamSimHash::Hash(X, Y, K, Seed, SubJitterN))) / N;
					FTreeInstance T;
					T.LatDeg = B.SouthDeg + Fy * (B.NorthDeg - B.SouthDeg);
					T.LonDeg = B.WestDeg + Fx * (B.EastDeg - B.WestDeg);
					if (Sampler(T.LatDeg, T.LonDeg) != P.TreeCode) continue;
					if (IsClear && !(*IsClear)(T.LatDeg, T.LonDeg)) continue;   // REALISM R3b: inside a footprint + clearance
					T.YawDeg = 360.0f * CamSimHash::Uniform(CamSimHash::Hash(X, Y, K, Seed, SubYaw));
					if (T.YawDeg >= 360.0f) T.YawDeg = 0.0f;
					T.Scale = P.ScaleMin + (P.ScaleMax - P.ScaleMin) * CamSimHash::Uniform(CamSimHash::Hash(X, Y, K, Seed, SubScale));
					Out.Add(T);
				}
			return Out;
		}
	}

	TArray<FTreeInstance> GenerateCell(uint32 Seed, FVegCellId Cell, const FScatterParams& P, FCodeSampler Sampler)
	{
		return GenerateCellImpl(Seed, Cell, P, Sampler, nullptr);
	}

	TArray<FTreeInstance> GenerateCell(uint32 Seed, FVegCellId Cell, const FScatterParams& P, FCodeSampler Sampler, FClearPredicate IsClear)
	{
		return GenerateCellImpl(Seed, Cell, P, Sampler, &IsClear);
	}
```

(The helper must come after the existing anonymous namespace that defines `SubJitterE` … `SeedMix`; keep that namespace as it is.)

- [ ] **Step 4: Add the config key**

`.../Config/CamSimConfig.h`, in `FVegetationConfig` after `bDropInBuildings`:

```cpp
		float   BuildingClearanceM = 2.0f; // CAMSIM_VEGETATION_BUILDING_CLEARANCE_M; with the package's footprint mask, no tree within this of a footprint along E/W/N/S (REALISM R3b)
```

`.../Config/CamSimConfig.cpp`: in the `vegetation` yaml block after `drop_in_buildings`:

```cpp
			YamlFloat (V, "building_clearance_m", Cfg.Vegetation.BuildingClearanceM);
```

after the `CAMSIM_VEGETATION_DROP_IN_BUILDINGS` env line:

```cpp
	Cfg.Vegetation.BuildingClearanceM = GetEnvFloat(TEXT("CAMSIM_VEGETATION_BUILDING_CLEARANCE_M"), Cfg.Vegetation.BuildingClearanceM);
```

and in `Validate()` after the `vegetation.seed` check:

```cpp
	if (!(Vegetation.BuildingClearanceM >= 0.0f && Vegetation.BuildingClearanceM <= 10.0f))
		Errors.Add(FString::Printf(TEXT("vegetation.building_clearance_m=%.2f out of range [0, 10]"), Vegetation.BuildingClearanceM));
```

- [ ] **Step 5: Method selection and wiring in the manager**

`.../Vegetation/VegetationManager.h`, inside `namespace CamSimVegetation` after `KeepClearOfBuildings`:

```cpp
	/** How trees are kept out of buildings (REALISM R3b): the package's footprint mask, else (an R3 package) the runtime
	 *  height check on the buildings tileset, else nothing. vegetation.drop_in_buildings false turns both off. */
	enum class EBuildingExclusion : uint8 { Off, None, Mask, RuntimeCheck };

	CAMSIMTEST_API EBuildingExclusion ChooseBuildingExclusion(bool bDropInBuildings, bool bHasMask, bool bHasBuildingsTileset);
	/** The `buildings:` part of the "Vegetation: on (...)" log line. */
	CAMSIMTEST_API FString DescribeBuildingExclusion(EBuildingExclusion E, float ClearanceM);
```

Update the `CreateLiveManager` comment: "…SampleHeightMostDetailed on Terrain; trees are kept out of buildings by the package's footprint mask (FVegetationBuildingMask, on the scatter worker) or, without one, by a second batch on the buildings tileset (tag CamSimBuildings), per ChooseBuildingExclusion…".

`.../Vegetation/VegetationManager.cpp`: add `#include "Vegetation/VegetationBuildingMask.h"`; define the two functions after `KeepClearOfBuildings`:

```cpp
	EBuildingExclusion ChooseBuildingExclusion(bool bDropInBuildings, bool bHasMask, bool bHasBuildingsTileset)
	{
		if (!bDropInBuildings) return EBuildingExclusion::Off;
		if (bHasMask) return EBuildingExclusion::Mask;
		return bHasBuildingsTileset ? EBuildingExclusion::RuntimeCheck : EBuildingExclusion::None;
	}

	FString DescribeBuildingExclusion(EBuildingExclusion E, float ClearanceM)
	{
		switch (E)
		{
		case EBuildingExclusion::Mask:         return FString::Printf(TEXT("mask (clearance %.1f m)"), ClearanceM);
		case EBuildingExclusion::RuntimeCheck: return TEXT("runtime check (buildings tileset, no mask)");
		case EBuildingExclusion::None:         return TEXT("none");
		default:                               return TEXT("off (drop_in_buildings false)");
		}
	}
```

In `CreateLiveManager`, before `FVegetationHooks H;` add:

```cpp
		// Trees inside building footprints (REALISM R3b): the package's mask on the scatter worker when it has one (no
		// runtime sampling); else, for an R3 package, the runtime check (a second height batch on the buildings tileset).
		TSharedPtr<FVegetationBuildingMask, ESPMode::ThreadSafe> Mask;
		if (V.bDropInBuildings && !Cfg.Scene.BuildingsMaskDir.IsEmpty())
		{
			Mask = MakeShared<FVegetationBuildingMask, ESPMode::ThreadSafe>(Cfg.Scene.BuildingsMaskDir);
			if (!Mask->IsValid())
			{
				UE_LOG(LogCamSim, Warning, TEXT("Vegetation: building mask unusable (%s); falling back"), *Mask->GetError());
				Mask.Reset();
			}
		}
		ACesium3DTileset* Buildings = CamSim::Geospatial::FindBuildingsTileset(World);
		const EBuildingExclusion Exclusion = ChooseBuildingExclusion(V.bDropInBuildings, Mask.IsValid(), Buildings != nullptr);
		const double ClearanceM = V.BuildingClearanceM;
```

Replace `H.Generate = ...;` with:

```cpp
		// Always answers (an empty land cover lookup yields no trees); the manager's weak pointer drops it after teardown.
		H.Generate = [LandCover, Mask, P, Seed, ClearanceM](FVegCellId Id, TFunction<void(TArray<FTreeInstance>)> Done)
		{
			UE::Tasks::Launch(UE_SOURCE_LOCATION, [LandCover, Mask, P, Seed, ClearanceM, Id, Done = MoveTemp(Done)]() mutable
			{
				auto Code = [&LandCover](double La, double Lo) { return LandCover->Code(La, Lo); };
				TArray<FTreeInstance> T = Mask.IsValid()
					? GenerateCell(Seed, Id, P, Code, [&Mask, ClearanceM](double La, double Lo) { return Mask->IsClear(La, Lo, ClearanceM); })
					: GenerateCell(Seed, Id, P, Code);
				AsyncTask(ENamedThreads::GameThread, [Done = MoveTemp(Done), T = MoveTemp(T)]() mutable { Done(MoveTemp(T)); });
			});
		};
```

Replace the two lines that pick `Buildings` and bind `SampleBuildingHeights` with:

```cpp
		if (Exclusion == EBuildingExclusion::RuntimeCheck)
			H.SampleBuildingHeights = MakeSampler(TWeakObjectPtr<ACesium3DTileset>(Buildings), TEXT("buildings"), /*bWarnOnFail=*/false);
```

and the log line with:

```cpp
		UE_LOG(LogCamSim, Log, TEXT("Vegetation: on (type '%s', radius %.0f m, density %.2f, shadows %d, seed %u, land cover %s, buildings: %s)"),
			*Type.Name, V.RadiusM, V.DensityScale, bShadows ? 1 : 0, Seed, *Cfg.Thermal.LandCover.Dir,
			*DescribeBuildingExclusion(Exclusion, V.BuildingClearanceM));
```

(With `Exclusion != Mask` the mask pointer is null in the hook even if it loaded; a valid mask always selects `Mask`, so this only happens with `drop_in_buildings` false, where it was never created.)

- [ ] **Step 6: Build and run the tests**

Run: `scripts/run.sh --build-only`, then the headless command with filter `CamSim.Vegetation`.
Expected: build succeeds; `parse_automation_report.py` exits 0 (all `CamSim.Vegetation.*`, including the new `Scatter.NoMaskIsBitIdenticalToR4`, `Scatter.ClearPredicateDropsOnlyRejected`, `Buildings.MethodSelection`, the extended `Config.*` and the existing `Buildings.*` runtime-check tests).

- [ ] **Step 7: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Vegetation unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.h \
  unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.cpp unreal_project/CamSimTest/Source/CamSimTest/Tests/VegetationScatterTest.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/VegetationManagerTest.cpp unreal_project/CamSimTest/Source/CamSimTest/Tests/VegetationConfigTest.cpp
git commit -m "feat(vegetation): keep trees out of buildings with the footprint mask, no runtime sampling (REALISM R3b)"
```

---

### Task 8: `building_roof` thermal class for code 230

**Files:**
- Modify: `.../Thermal/ThermalMaterials.h` (index constant), `.../Thermal/ThermalMaterials.cpp` (built-in), `.../Thermal/LandCoverClasses.cpp` (code 230), `.../Thermal/LandCoverClasses.h` (family comment), `.../Tests/ThermalLandCoverMaterialsTest.cpp`, `.../Tests/ThermalLandCoverConfigTest.cpp`

**Interfaces:**
- Consumes: `FThermalModel` (`Update`, `TemperatureK`, `AirTemperatureK`), `FLandCoverClassTable::Build`, `CamSimThermalRef::RefinedClassData` / `ClassData`, `FThermalFrameParams`.
- Produces: `FThermalMaterialTable::BuildingRoof = 15` (the 16th built-in, 16 of 32 class slots left); `DefaultMaterialName(230) == "building_roof"`; `FamilyForCode(230) == ELandCoverFamily::Bare` (see the parameter note below for why not `BuiltUp`).

**Parameters** (albedo 0.30, emissivity 0.90, thermal inertia 250 J m⁻² K⁻¹ s⁻½, h_c 10 W m⁻² K⁻¹, k_fast 0.03 K per W m⁻²), a light, low-mass roof:
- *Albedo 0.30.* Existing commercial and residential roofs are mostly 0.05–0.25 (dark membranes, asphalt shingle, aged metal) with a minority of aged cool roofs at 0.5–0.6 (Akbari, Menon and Rosenfeld 2009, "Global cooling: increasing world-wide urban albedos to offset CO2", *Climatic Change* 94; LBNL Heat Island Group, "Cool roofs"); 0.30 is a stock average weighted toward the dark majority.
- *Emissivity 0.90.* Non-metallic roofing (membranes, shingle, tile, coated metal) is 0.85–0.92 thermal emittance (Cool Roof Rating Council product directory; ASTM C1371 measurements). Bare galvanised metal (0.05–0.25) is a minority and would read colder still; it is not modelled.
- *Thermal inertia 250.* The surface layer of a roof sits on insulation or an air gap, so its effective inertia is that of the thin top layer: wood or plywood ~350–400 (k 0.13 W m⁻¹ K⁻¹, ρ 550 kg m⁻³, c 1,600–2,000 J kg⁻¹ K⁻¹: √(kρc)), asphalt shingle ~700–900, polyisocyanurate board ~35 (k 0.025, ρ 30, c 1,500) (material properties as tabulated in Oke 1987, *Boundary Layer Climates*, Table 1.1, and Incropera et al., *Fundamentals of Heat and Mass Transfer*, Table A.3). 250 is between the insulated-deck and timber values, well below the built-in `built_up` (1,650) and `asphalt` (1,500) ground classes, which is what makes a roof hot at noon and cold at night.
- *h_c 10.* The convective coefficient of a roof in a 2–3 m s⁻¹ wind is ~10–15 W m⁻² K⁻¹ (McAdams' flat-plate fit h ≈ 5.7 + 3.8 v, as used in ASHRAE practice); 10 keeps it equal to the other non-vegetation classes, so the contrast comes from albedo and inertia alone.
- *k_fast 0.03.* The per-pixel fast term (K per W m⁻² absorbed above the class reference) scales with how quickly a surface follows the sun: between asphalt (0.02) and vehicle paint (0.04), as a thin roof skin is.
- *Model check* (the class model of `FThermalModel`, reproduced in Python while writing this plan; Step 1's test runs the real one): at 33.3° N on 21 December (air 15 °C mean, 8 K swing, clear sky) `building_roof` is 310.8 K at noon vs `grassland` 300.6 K (+10.2 K), and 282.2 K at 02:00 vs air 284.3 K (−2.1 K); on 21 June 332.8 K vs 312.2 K at noon, 283.3 K vs air 284.3 K at 02:00.
- *Family.* `FamilyForCode(230)` is `Bare` (blend toward `vegetation` by the base-colour vegetation index, else the code's own class), not `BuiltUp`: `RefinedClassData` replaces a `BuiltUp` code's class entirely by the asphalt / concrete split (`ThermalReference.cpp`), so with `BuiltUp` the `building_roof` class would never be used and roofs would be warmer than air at night. This is the one deviation from the spec's wording; the spec's intent (roofs refine on their real colour, hot by day, below air at night) holds with `Bare`.

- [ ] **Step 1: Write the failing tests**

In `.../Tests/ThermalLandCoverMaterialsTest.cpp`, `LandCoverBuiltIns`: change `TestEqual(TEXT("15 built-ins"), B.Num(), 15);` to `TestEqual(TEXT("16 built-ins"), B.Num(), 16);` and add `{ FThermalMaterialTable::BuildingRoof, TEXT("building_roof") },` at the end of `Expected`. Then append (add `#include "Thermal/ThermalReference.h"` and `#include "ThermalFrameParams.h"` at the top):

```cpp
// REALISM R3b: a light, low-mass roof is hotter than grass at noon and below air temperature at night, at Camp Pendleton
// (33.3 N) on both solstices, clear sky, air 15 C mean / 8 K swing (thermal_check's defaults).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalBuildingRoofTest, "CamSim.Thermal.Materials.BuildingRoofDiurnal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalBuildingRoofTest::RunTest(const FString& Parameters)
{
	using T = FThermalMaterialTable;
	const T Table;
	for (const int32 Day : { 355, 172 })
	{
		FThermalSite S;
		S.Year = 2026; S.DayOfYear = Day; S.LatDeg = 33.3; S.LonDeg = -117.3;
		S.TairMeanK = 288.15; S.AirSwingK = 8.0; S.Cloud = 0.0;
		FThermalModel M;
		M.Update(S, Table);
		const double RoofNoon = M.TemperatureK(T::BuildingRoof, 12.0 * 3600.0, 288.15);
		const double GrassNoon = M.TemperatureK(T::Grassland, 12.0 * 3600.0, 288.15);
		const double RoofNight = M.TemperatureK(T::BuildingRoof, 2.0 * 3600.0, 288.15);
		const double AirNight = FThermalModel::AirTemperatureK(S, 2.0);
		AddInfo(FString::Printf(TEXT("day %d: roof %.1f / grass %.1f K at noon, roof %.1f / air %.1f K at 02:00"),
			Day, RoofNoon, GrassNoon, RoofNight, AirNight));
		TestTrue(*FString::Printf(TEXT("day %d: roof >= grass + 3 K at noon"), Day), RoofNoon - GrassNoon >= 3.0);
		TestTrue(*FString::Printf(TEXT("day %d: roof <= air - 0.5 K at 02:00"), Day), AirNight - RoofNight >= 0.5);
		TestTrue(*FString::Printf(TEXT("day %d: roof colder than built-up ground at night"), Day),
			RoofNight < M.TemperatureK(T::BuiltUp, 2.0 * 3600.0, 288.15));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalBuildingRoofRefineTest, "CamSim.Thermal.Materials.BuildingRoofKeepsItsClass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalBuildingRoofRefineTest::RunTest(const FString& Parameters)
{
	using T = FThermalMaterialTable;
	const T Materials;
	FLandCoverClassTable Classes;
	Classes.Build({}, Materials);
	FThermalFrameParams P;
	P.NumClasses = static_cast<uint32>(Materials.Num());
	for (uint32 C = 0; C < P.NumClasses; ++C) { P.ClassTempK[C] = 280.0f + C; P.ClassEmissivity[C] = 0.9f; }
	FMemory::Memcpy(P.LandCoverClass, Classes.Class, sizeof(P.LandCoverClass));
	FMemory::Memcpy(P.LandCoverFamily, Classes.Family, sizeof(P.LandCoverFamily));
	const FVector4f Grey = CamSimThermalRef::RefinedClassData(P, 230, FVector2f(0.0f, 0.7f), true);   // no vegetation
	TestEqual(TEXT("a grey roof keeps building_roof"), Grey.X, 280.0f + T::BuildingRoof);
	const FVector4f Green = CamSimThermalRef::RefinedClassData(P, 230, FVector2f(1.0f, 0.7f), true);  // canopy over it
	TestEqual(TEXT("a green pixel blends to vegetation"), Green.X, 280.0f + T::Vegetation);
	return true;
}
```

In `.../Tests/ThermalLandCoverConfigTest.cpp`, `LandCoverClassTable`: change `{ 230, T::TerrainDefault }` to `{ 230, T::BuildingRoof }` and add after the 210 family line:

```cpp
	TestEqual(TEXT("230 is bare family (keeps building_roof, blends to vegetation)"), Table.Family[230], static_cast<uint8>(ELandCoverFamily::Bare));
```

- [ ] **Step 2: Build to verify they fail**

Run: `scripts/run.sh --build-only`
Expected: compile error: `no member named 'BuildingRoof' in 'FThermalMaterialTable'`.

- [ ] **Step 3: Implement**

`.../Thermal/ThermalMaterials.h`: change the index comment to "…14 is rail_ballast (REALISM R2); 15 is building_roof (REALISM R3b)." and add:

```cpp
	static constexpr int32 BuildingRoof   = 15;
```

`.../Thermal/ThermalMaterials.cpp`, at the end of the `BuiltIns()` list:

```cpp
		// REALISM R3b: a light, low-mass roof (land-cover code 230, the burned footprints): the thin top layer over
		// insulation gives a low effective inertia, so it is hot at midday and below air temperature on a clear night.
		// Values and sources: docs/thermal.md "Classes and keys".
		MakeMaterial(TEXT("building_roof"),   0.30f, 0.90f,  250.0f, 10.0f, 0.030f),
```

`.../Thermal/LandCoverClasses.cpp`: in `DefaultMaterialName` add `case 230: return TEXT("building_roof");  // REALISM R3b: burned building footprints`; in `FamilyForCode` change the Bare case to `case 60: case 200: case 201: case 210: case 230: return ELandCoverFamily::Bare;`.

`.../Thermal/LandCoverClasses.h`: the `Bare` comment becomes `// 60, 200/201/210 (R2), 230 (R3b): blend toward vegetation by v, else the code's own class`.

- [ ] **Step 4: Build and run the tests (NullRHI, then GPU)**

Run: `scripts/run.sh --build-only`; headless with filter `CamSim.Thermal`; then `scripts/run_gpu_tests.sh CamSim.GPU.Thermal`.
Expected: build succeeds; `parse_automation_report.py` exits 0 for `CamSim.Thermal` (the `AddInfo` line shows roof/grass/air values close to the plan's model check); the GPU run passes `CamSim.GPU.Thermal.MatchesCpu`, `LandCoverMatchesCpu` and `EntityPartsMatchesCpu` (class-table data only; no shader change).

- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalMaterials.h unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalMaterials.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Thermal/LandCoverClasses.cpp unreal_project/CamSimTest/Source/CamSimTest/Thermal/LandCoverClasses.h \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalLandCoverMaterialsTest.cpp unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalLandCoverConfigTest.cpp
git commit -m "feat(thermal): building_roof class for land-cover code 230 (REALISM R3b)"
```

---
### Task 9: Docs and config reference

**Files:**
- Modify: `docs/scene-packages.md` ("What a package is" tree, `scene.toml` reference, "Buildings (R3)" → new subsection "Footprint mask, land-cover burn and roof atlas (R3b)", "verify"), `docs/vegetation.md` ("How it works", "Configuration", "Known limits"), `docs/thermal.md` ("Classes and keys", "Limits (R2)" → note), `docs/configuration.md` (vegetation table, `thermal.materials` row, env quick reference), `deploy/camsim_config.yaml` (`vegetation:`), `docs/terrain-feedback.md` (code 230 on terrain), `scripts/scene/examples/pendleton.toml`, `CLAUDE.md` (gotchas, architecture comments; the test count is updated in Task 11)

**Interfaces:**
- Consumes: the names and values of Tasks 1–8 (no code).
- Produces: docs only.

- [ ] **Step 1: `scripts/scene/examples/pendleton.toml`** — make the roof keys explicit under `[buildings]`:

```toml
[buildings]                # REALISM R3: Overture footprints -> features/buildings/ (3D Tiles 1.1); ODbL, in allow above
release = "2026-09-23.1"
roof_textures = true       # REALISM R3b: per-tile roof atlas from the imagery sources (flat grey without NAIP)
roof_texel_m = 0.5         # atlas texel; steps up 0.75 / 1.0 / 1.5 / 2.0 m when a tile's roofs do not fit
roof_texture_max_px = 2048 # largest atlas side (power of two)
```

- [ ] **Step 2: `docs/scene-packages.md`**
  - Package tree (line ~32): add below the tileset line
    `  features/buildings/mask/          # footprint mask: 0.05 deg 4800^2 1-bit PNGs + index.json (R3b; with [buildings])`.
  - `scene.toml` reference, under `[buildings]`: add the three keys with their defaults and one-line meanings (as in Step 1).
  - In "Buildings (R3)", the `Config` table: add rows `roof_textures` (`true`, "per-tile roof atlas from the imagery sources; `false` = R3's flat grey roofs"), `roof_texel_m` (`0.5`, "atlas texel, m, [0.25, 2.0]"), `roof_texture_max_px` (`2048`, "largest atlas side; 256 … 8192, a power of two").
  - Replace the runtime bullet "Thermal IR is unchanged: building pixels take the land-cover class under them." with "Thermal IR: burned cells (code 230) take the `building_roof` class (R3b, below); a building over a cell less than half covered still takes that cell's class."
  - Replace the known limits "Flat roofs, one neutral colour…" and "Thermal IR: buildings take the land-cover class…" with the R3b limits (below), and keep the Overture/NAIP/ODbL ones.
  - Add, before "## Sea level (R1)":

```markdown
### Footprint mask, land-cover burn and roof atlas (R3b)

Design: [`superpowers/specs/2026-10-11-r3b-buildings-look-design.md`](superpowers/specs/2026-10-11-r3b-buildings-look-design.md).
All three come from the buildings the layer keeps (same filter, same `source_id` order), planned once per build and
cached under `.state/` (`buildings-plan.pkl`, `buildings-footprints.npz`, keyed by the snapshot, `min_area_m2`, the
rules and the terrain).

- **Footprint mask** (`features/buildings/mask/`, format `camsim-buildings-mask-1`): the land-cover tile grid at 8 x its
  resolution, 0.05° tiles of 4800 x 4800 cells (~1.16 m x 0.97 m at 33° N) named by their SW corner, row 0 = north,
  1-bit PNG. A cell is set when its centre is inside a footprint (holes excluded). Tiles without a set cell are not
  written; `index.json` lists the others with `tile_px`, `cell_factor` and `rules`. CamSim's tree scatter reads it
  (`docs/vegetation.md`).
- **Land-cover burn**: a land-cover cell becomes **230** (`building`) when at least 32 of its 64 mask cells are set.
  Order, later wins: WorldCover, water, paved areas, buildings, rail, roads (buildings overwrite parking lots and
  structures; a road or rail burned over a misregistered footprint stays road or rail). `index.json` `codes` gains
  `"230": "building"` and `credits` the Overture notice. CIGI terrain hits in those cells report 230, and thermal IR
  gives them the `building_roof` class.
- **Roof atlas**: each GLB carries one square power-of-two JPEG (quality 85, 4:2:0, ≤ `roof_texture_max_px`) of its roofs,
  sampled from the imagery layer's sources and colour match (not from the built JPEGs) at `roof_texel_m` over each
  roof's box in the tile's ENU frame, with a 2 px edge-extended gutter, packed by height then `source_id`. When the
  rectangles do not fit, the tile's texel steps 0.5 → 0.75 → 1.0 → 1.5 → 2.0 m; the tile's root `extras.roof_texel_m`
  records it (`null` when every roof is flat). A roof keeps the flat grey material when its best source (the one
  under most of its triangles) is coarser than 2 m (no NAIP), or when it does not fit even at 2.0 m. Walls stay flat.
  Roof vertices carry `TEXCOORD_0`; wall and flat-roof vertices (0, 0). Cesium's default material samples the texture;
  nothing changes at runtime. `build.json` `buildings.roofs` counts textured and flat roofs and tiles per texel.
- **Hashing**: the landcover layer's inputs gain the buildings snapshot, settings hash, rules version and the footprint
  raster's key, and its settings a `buildings` entry (only with `[buildings]`), so packages without `[buildings]` are
  byte-identical to R3. Roof textures add the imagery sources, settings and colour match to the buildings layer's inputs.
- **verify**: quick checks the mask tiles listed in its index exist; `--deep` checks every land-cover cell against the
  mask (230 exactly where ≥ 32 of 64 are set, unless road or rail) and every tile's `roof_texel_m` against its texture.

Known limits (R3b): NAIP is not a true orthophoto, so tall buildings' roofs carry their lean (fine for low-rise, not
corrected); roof texture is only as good as NAIP (0.6 m, one date, shadows baked in); a 9 m land-cover cell is
"building" only when at least half covered, so small buildings keep their WorldCover class in IR; walls stay flat
coloured and roofs flat (no pitch); no wall textures, no KTX2 / Basis compression.
```

- [ ] **Step 3: `docs/vegetation.md`**
  - In "How it works", replace the "Trees inside building footprints are dropped…" bullet with:

```markdown
- **Trees stay out of buildings** (REALISM R3 / R3b; `vegetation.drop_in_buildings`, default true, gates all of it).
  Land cover alone would put ~10 % of footprint area under trees (WorldCover tags canopy over roofs as code 10). In
  order of preference:
  1. **Footprint mask** (a package built with R3b's `features/buildings/mask/`): `FVegetationBuildingMask` decodes the
     1-bit mask tiles on the scatter worker into bitsets (8-tile LRU, ~23 MB, separate from the land-cover sampler; a
     missing or unreadable tile means no buildings there, one warning). `GenerateCell` drops a candidate whose trunk,
     or any of 4 points `vegetation.building_clearance_m` (2 m) east, west, north and south of it, is masked. No
     runtime sampling; the hash draws are unchanged, so without a mask every cell is bit-identical to R4's. The probes
     are axis-aligned: a footprint corner diagonally within the clearance is not seen. It applies whether or not the
     buildings tileset is drawn (`scene.buildings.enabled`), since the roofs are in the imagery either way.
  2. **Runtime check** (an R3 package: buildings tileset, no mask): a second `SampleHeightMostDetailed` batch per cell on
     the `CamSimBuildings` tileset; a hit more than 0.5 m above the terrain drops the tree (`KeepClearOfBuildings`).
     It costs about +2.5 ms game p95 and +34 MiB on Pendleton.
  3. Neither: no exclusion.
  The `Vegetation: on (...)` line ends with `buildings: mask (clearance 2.0 m)`, `buildings: runtime check (buildings
  tileset, no mask)`, `buildings: none` or `buildings: off (drop_in_buildings false)`. The dump's `in_buildings` counts
  only the runtime check's drops.
```

  - In "Configuration", add the row `vegetation.building_clearance_m` | `CAMSIM_VEGETATION_BUILDING_CLEARANCE_M` | `2.0` | `[0, 10]`; with the package's footprint mask, no tree within this distance of a footprint along east, west, north and south.
  - In "Known limits", add: "Building clearance is tested at 4 axis points; a crown can still overhang a footprint corner or, with a clearance below the crown radius, a wall."
  - In "Trees vs building footprints (`pendleton-r3`)", change "the log says `buildings none`" to "the log said `buildings none` (R4's wording; now `buildings: none`)".

- [ ] **Step 4: `docs/thermal.md`**
  - "Classes and keys": change the intro to "(index order fixed; 6-13 added by 4B land cover, 14 by R2, 15 by R3b)" and add the row `| \`building_roof\` (R3b) | 0.30 | 0.90 | 250 | 10 | 0.03 | model |`, then a paragraph:

```markdown
`building_roof` (REALISM R3b) is the class of land-cover code 230, the burned building footprints. A light, low-mass
roof: albedo 0.30 (existing roofs are mostly 0.05–0.25 with a minority of aged cool roofs at 0.5–0.6; Akbari, Menon and
Rosenfeld 2009, *Climatic Change* 94; LBNL Heat Island Group), emissivity 0.90 (non-metallic roofing 0.85–0.92; Cool Roof
Rating Council directory, ASTM C1371), thermal inertia 250 (the thin top layer over insulation: between polyisocyanurate
~35 and timber ~350–400, from k, ρ, c in Oke 1987 and Incropera), h_c 10 (McAdams' flat-plate fit at 2–3 m/s) and
k_fast 0.03 (a thin skin follows the sun: between asphalt and vehicle paint). The model gives 310.8 K vs grassland
300.6 K at noon and 282.2 K vs air 284.3 K at 02:00 at 33.3° N on 21 December (`CamSim.Thermal.Materials.BuildingRoofDiurnal`).
Code 230 refines in the `Bare` family (toward `vegetation` by the base-colour vegetation index, else `building_roof`):
the `BuiltUp` family would replace the class by the asphalt/concrete split. Code 50 (WorldCover built-up) keeps
`built_up` for the ground between buildings. With 16 built-ins, 16 of the 32 class slots are left for `thermal.materials`.
```

  - "Limits (R2)": add a bullet "Buildings (R3b): a building over cells less than half covered by its footprint takes those cells' land-cover class; building geometry still carries no stencil (pixels are classified by the 10 m cell under the hit)."

- [ ] **Step 5: `docs/configuration.md` and `deploy/camsim_config.yaml`**
  - Vegetation table: add after `drop_in_buildings`:
    `| \`vegetation.building_clearance_m\` | \`CAMSIM_VEGETATION_BUILDING_CLEARANCE_M\` | \`2.0\` | With the scene package's footprint mask (REALISM R3b): no tree whose trunk or the 4 points this far east, west, north and south of it fall in a footprint. \`[0, 10]\`. |`
  - Rewrite the `drop_in_buildings` description: "Keep trees out of building footprints. With the package's footprint mask (R3b) the scatter tests the mask on its worker, no runtime cost; with an R3 package (buildings tileset, no mask) a second height batch per cell on the buildings tileset (about +2.5 ms game p95, +34 MiB); `false` turns both off ([`vegetation.md`](vegetation.md), "How it works")."
  - `thermal.materials.<name>` row: add `building_roof` to the built-in list and change "17 slots … after the 15 built-ins" to "16 slots … after the 16 built-ins".
  - Env quick reference: add `CAMSIM_VEGETATION_BUILDING_CLEARANCE_M`.
  - `deploy/camsim_config.yaml`, under `vegetation:` after `drop_in_buildings` (and change that line's comment):

```yaml
  drop_in_buildings: true                # CAMSIM_VEGETATION_DROP_IN_BUILDINGS — keep trees out of buildings (package mask, else runtime check)
  building_clearance_m: 2.0              # CAMSIM_VEGETATION_BUILDING_CLEARANCE_M — [0, 10]; mask method: E/W/N/S clearance from footprints
```

- [ ] **Step 6: `docs/terrain-feedback.md`** — after the paragraph "Code 230 exists only with a package built with `[buildings]` …", add:

```markdown
Since REALISM R3b a package built with `[buildings]` also burns 230 into its land cover where a 10 m cell is at least half
covered by footprints (`docs/scene-packages.md`, "Footprint mask, land-cover burn and roof atlas (R3b)"). A terrain hit
in such a cell reports 230 by rule 5 (the stored land-cover byte), with or without the buildings tileset: with
`scene.buildings.enabled: false`, a HAT/HOT inside a footprint reports 230 from the land cover. No CIGI code changed.
```

  and replace the limit "A building in thermal IR still takes the land-cover class under it; only CIGI reports 230." with "Thermal IR gives burned cells (230) the `building_roof` class; a building over cells less than half covered takes those cells' class."

- [ ] **Step 7: `CLAUDE.md`**
  - Architecture comment for `Vegetation/`: `# Trees (REALISM R4): vegetation cell actor (globe-anchored HISM), probe console command; R3b building footprint mask (FVegetationBuildingMask)`.
  - In the **Vegetation** gotcha, replace the sentence from "Trees inside building footprints are dropped when the package has buildings…" to its end with: "Trees stay out of buildings by the package's footprint mask when it has one (REALISM R3b, `FVegetationBuildingMask`: 1-bit 4800² tiles decoded on the scatter worker, 8-tile LRU; `GenerateCell`'s `IsClear` drops a candidate whose trunk or 4 points `vegetation.building_clearance_m` E/W/N/S are masked; no runtime sampling, cell hashes unchanged without a mask), else (R3 packages) by the runtime check, a second `SampleHeightMostDetailed` batch on the `CamSimBuildings` tileset (`KeepClearOfBuildings`); `vegetation.drop_in_buildings` gates both and the `Vegetation: on` line names the method."
  - In the **Buildings** gotcha, replace "Flat roofs, one neutral colour, no bridges" with: "R3b: the layer also writes `features/buildings/mask/` (8 × the land-cover grid, 1-bit) and burns 230 into land cover (≥ 32 of 64 cells; after paved areas, before rail and roads), both from one cached footprint raster of the kept buildings (`camsim_scene/footprints.py`, `.state/`); each GLB has a roof atlas JPEG from the imagery sources (`roofs.py`, `roofatlas.py`; `extras.roof_texel_m`; flat grey without a ≤ 2 m source). Thermal IR maps 230 to `building_roof`. Walls flat, roofs flat, no bridges".
  - In the **Land cover for thermal IR** gotcha, change "15 built-ins leave 17 of 32 class slots" to "16 built-ins (R3b added `building_roof`) leave 16 of 32 class slots".

- [ ] **Step 8: Check links and commit**

Run: `grep -n "building_clearance_m\|building_roof\|buildings/mask" docs/*.md deploy/camsim_config.yaml CLAUDE.md | wc -l`
Expected: at least 12 lines (each doc names its key).

```bash
git add docs/scene-packages.md docs/vegetation.md docs/thermal.md docs/configuration.md docs/terrain-feedback.md \
  deploy/camsim_config.yaml scripts/scene/examples/pendleton.toml CLAUDE.md
git commit -m "docs(realism): footprint mask, land-cover burn, roof atlas, building_roof (REALISM R3b)"
```

---
### Task 10: Acceptance tooling (trees vs footprints, roofs in MWIR, CIGI probe points)

**Files:**
- Create: `scripts/scene/tools/burn_probe_points.py`, `scripts/scene/tests/test_burn_probe_points.py`, `scripts/tests/test_vegetation_footprints.py`, `scripts/tests/test_thermal_check_roofs.py`
- Modify: `scripts/bench/dense_path.py` (`BuildingMask`), `scripts/tests/test_bench_dense_path.py`, `scripts/vegetation_check.py` (`footprint_gate`, `load_footprints`, `footprints` stage), `scripts/thermal_check.py` (roofs run, gates s / s_air), `CLAUDE.md` (Commands rows)

**Interfaces:**
- Consumes: the mask layout (Task 1), the burn (Task 2), the `Vegetation: on (... buildings: mask ...)` log line (Task 7), the `building_roof` class at index 15 (Task 8), the `camsim.Thermal.Log` line format (`classes[K] … Tair=… K … sunLux=…`), `bench.dense_path.LandCover`, `bench.dense_path.lc_tile_filename`.
- Produces:
  - `bench.dense_path.MASK_PX = 4800`, `bench.dense_path.BuildingMask(root: Path)` with `masked(lon, lat) -> np.ndarray` (bool; a missing tile reads False).
  - `vegetation_check.footprint_gate(trees: list[tuple[float, float]], mask, clearance_m: float, polygons: list | None = None) -> dict` (`trees` = (lat, lon); `mask` has `masked(lon, lat)`) with keys `trees`, `probe_hits`, and with polygons `trunks_inside_footprints`, `within_clearance`, then `pass`; `vegetation_check.load_footprints(scene: Path) -> list | None`; `--stages footprints` writes `report["footprints"]`.
  - `thermal_check` constants `ROOF_POSE`, `ROOF_UP_M = 460.0`, `ROOF_FOV = 20.0`, `ROOF_NOON_DN = 3.0`, `ROOF_AIR_K = 0.5`; functions `roof_masks(shape, gsd_m, lat0, lon0, masked, codes) -> (roof, grass)`, `roof_row(band, tod, y, masks, threshold) -> dict`, `roof_air_temps(lines) -> dict[str, tuple[float, float, float]]`; run `mwir_roofs` (group `roofs`, needs `--scene`); rows `("s", "mwir", "noon")` and `("s_air", "mwir", "night")`; `check_all(out, results, bands, scene=None)`.
  - `burn_probe_points.py PKG --near LAT,LON [--gap-m 10]` → JSON `{"inside": [lat, lon], "outside": [lat, lon], "outside_code": int, "direction": "W|E|N|S"}`; functions `code_at(lc_dir, lat, lon) -> int`, `nearest_burned(lc_dir, lat, lon, search_cells=60) -> tuple[float, float] | None`, `outside_point(lc_dir, lat, lon, gap_m=10.0) -> tuple[float, float, int, str] | None`.

- [ ] **Step 1: Write the failing script tests**

Append to `scripts/tests/test_bench_dense_path.py`:

```python
def test_building_mask_reads_cells(tmp_path):
    from PIL import Image

    m = np.zeros((dp.MASK_PX, dp.MASK_PX), bool)
    m[10, 20] = True
    Image.frombytes("1", (dp.MASK_PX, dp.MASK_PX), np.packbits(m, axis=1).tobytes()).save(tmp_path / "+33.30_-117.50.png")
    bm = dp.BuildingMask(tmp_path)
    d = dp.TILE_DEG / dp.MASK_PX
    lat = (666 + 1) * dp.TILE_DEG - 10.5 * d  # row 10 centre
    lon = -2350 * dp.TILE_DEG + 20.5 * d  # column 20 centre
    got = bm.masked(np.array([lon, lon + d, lon]), np.array([lat, lat, 10.0]))
    assert got.tolist() == [True, False, False]  # the cell, its east neighbour, a tile that is not there
```

`scripts/tests/test_vegetation_footprints.py`:

```python
"""Unit tests for the footprints gate (REALISM R3b) in scripts/vegetation_check.py."""

import math

import numpy as np
import pytest
import vegetation_check as vc

LAT0, LON0 = 33.3, -117.3
KX = 111_320.0 * math.cos(math.radians(LAT0))


def _pt(north_m, east_m):
    return (LAT0 + north_m / 111_320.0, LON0 + east_m / KX)


class _Mask:
    """A building occupying 0..20 m east and 0..20 m north of (LAT0, LON0)."""

    def masked(self, lon, lat):
        e = (np.asarray(lon) - LON0) * KX
        n = (np.asarray(lat) - LAT0) * 111_320.0
        return (e >= 0) & (e < 20) & (n >= 0) & (n < 20)


def test_probe_hits_count_trunks_and_axis_probes():
    trees = [_pt(10, 10), _pt(10, -1.5), _pt(10, -5.0), _pt(-1.0, -1.0)]  # inside, 1.5 m west, 5 m west, diagonal
    r = vc.footprint_gate(trees, _Mask(), 2.0)
    assert r["trees"] == 4 and r["probe_hits"] == 2 and not r["pass"]
    assert vc.footprint_gate([_pt(10, -5.0), _pt(-1.0, -1.0)], _Mask(), 2.0)["pass"]


def test_polygon_counts_deep_trunks_and_reports_the_buffer():
    shapely = pytest.importorskip("shapely")
    sq = shapely.box(LON0, LAT0, LON0 + 20 / KX, LAT0 + 20 / 111_320.0)
    trees = [_pt(10, 10), _pt(10, 0.3), _pt(-1.0, -1.0), _pt(10, -5.0)]  # deep inside, at the wall, corner 1.4 m, 5 m
    r = vc.footprint_gate(trees, _Mask(), 2.0, [sq])
    assert r["trunks_inside_footprints"] == 1  # the wall tree is within the mask's half-cell diagonal of the edge
    assert r["within_clearance"] == 3  # info: inside, at the wall, the corner tree (the axis probes miss corners)
```

`scripts/tests/test_thermal_check_roofs.py`:

```python
"""Unit tests for gates s / s_air (roofs, REALISM R3b) in scripts/thermal_check.py."""

import math

import numpy as np
import thermal_check as tc


def _sources(lat0, lon0):
    kx = 111_320.0 * math.cos(math.radians(lat0))

    def masked(lon, lat):  # a building over the east half
        return (np.asarray(lon) - lon0) * kx > 0.0

    def codes(lon, lat):  # grassland everywhere
        return np.full(np.shape(lon), 30, np.uint8)

    return masked, codes


def test_roof_masks_geometry():
    masked, codes = _sources(33.3, -117.3)
    roof, grass = tc.roof_masks((200, 300), 0.5, 33.3, -117.3, masked, codes)
    cols_r, cols_g = np.nonzero(roof.any(axis=0))[0], np.nonzero(grass.any(axis=0))[0]
    centre = (300 - 1) / 2.0
    assert cols_r.min() * 1.0 > centre + tc.ROOF_ERODE_M / 0.5 - 1  # eroded inside the footprint
    assert cols_g.max() * 1.0 < centre - tc.ROOF_GRASS_CLEAR_M / 0.5 + 1  # grass at least 10 m from it
    assert not (roof & grass).any() and roof.sum() > 0 and grass.sum() > 0
    assert not (roof | grass)[:, :30].any()  # central 60 % only


def test_roof_row_pass_fail_and_minimum_area():
    masked, codes = _sources(33.3, -117.3)
    masks = tc.roof_masks((200, 300), 0.5, 33.3, -117.3, masked, codes)
    y = np.full((200, 300), 100.0)
    y[masks[0]] = 108.0
    assert tc.roof_row("mwir", "noon", y, masks, 3.0)["pass"]
    assert not tc.roof_row("mwir", "noon", y, masks, 10.0)["pass"]
    tiny = (np.zeros_like(masks[0]), masks[1])
    tiny[0][100, 200] = True
    assert not tc.roof_row("mwir", "noon", y, tiny, 3.0)["pass"]  # one roof pixel is not a measurement


def test_roof_air_temps_parse_the_thermal_log():
    temps = " ".join(f"{280.0 + k:.1f}" for k in range(16))
    night = f"LogCamSim: Thermal: classes[K] {temps} terrain=0 water=1 Tair=284.3 K cloud=0.00 epsZ=0.800 sunLux=0 x"
    noon = night.replace("Tair=284.3", "Tair=291.0").replace("sunLux=0", "sunLux=90000")
    out = tc.roof_air_temps([night, noon])
    assert out["night"] == (295.0, 288.0, 284.3) and out["noon"][2] == 291.0
    assert tc.roof_air_temps(["no thermal line"]) == {}


def test_expected_rows_include_the_roof_gates():
    rows = tc.expected_rows(["mwir"], {"roofs"})
    assert rows == [("s", "mwir", "noon"), ("s_air", "mwir", "night")]
    assert tc.expected_rows(["lwir"], {"roofs"}) == []
    assert tc.run_group("mwir_roofs") == "roofs" and tc.run_band("mwir_roofs") == "mwir"
```

`scripts/scene/tests/test_burn_probe_points.py`:

```python
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import burn_probe_points as bp  # noqa: E402

from camsim_scene.layers.landcover import tile_filename


def _lc(tmp_path):
    codes = np.full((600, 600), 30, np.uint8)
    codes[100:103, 200:204] = 230
    Image.fromarray(codes).save(tmp_path / tile_filename(666, -2350))
    return tmp_path


def _centre(row, col):
    return (667 * 0.05 - (row + 0.5) / 12000.0, -2350 * 0.05 + (col + 0.5) / 12000.0)


def test_nearest_burned_cell_and_a_point_outside(tmp_path):
    lc = _lc(tmp_path)
    lat, lon = _centre(110, 210)  # 8 cells south-east of the block
    inside = bp.nearest_burned(lc, lat, lon)
    assert inside is not None and bp.code_at(lc, *inside) == 230
    assert abs(inside[0] - _centre(102, 203)[0]) < 1e-9 and abs(inside[1] - _centre(102, 203)[1]) < 1e-9
    out = bp.outside_point(lc, *inside, gap_m=10.0)
    assert out is not None and out[2] == 30 and bp.code_at(lc, out[0], out[1]) == 30


def test_no_burned_cell_nearby(tmp_path):
    lc = _lc(tmp_path)
    assert bp.nearest_burned(lc, *_centre(500, 500), search_cells=20) is None
```

- [ ] **Step 2: Run them to verify they fail**

Run: `uv run --with pytest --with numpy --with pillow --with pycocotools --with shapely pytest scripts/tests/test_bench_dense_path.py scripts/tests/test_vegetation_footprints.py scripts/tests/test_thermal_check_roofs.py -q`
Expected: FAIL: `AttributeError: module 'bench.dense_path' has no attribute 'MASK_PX'`, `... 'vegetation_check' has no attribute 'footprint_gate'`, `... 'thermal_check' has no attribute 'roof_masks'`.

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_burn_probe_points.py -q`
Expected: FAIL: `ModuleNotFoundError: No module named 'burn_probe_points'`.

- [ ] **Step 3: `BuildingMask` in `scripts/bench/dense_path.py`** (after `class LandCover`):

```python
MASK_PX = 4800  # camsim-buildings-mask-1 (REALISM R3b): the land-cover grid at 8 x


class BuildingMask:
    """The scene package's building footprint mask (features/buildings/mask/, REALISM R3b) at points; a tile that is not
    there reads as no building. Same cell rule as LandCover (and the runtime FVegetationBuildingMask)."""

    def __init__(self, root: Path) -> None:
        self.root = Path(root)
        self._tiles: dict[tuple[int, int], np.ndarray | None] = {}

    def _tile(self, i: int, j: int) -> np.ndarray | None:
        if (i, j) not in self._tiles:
            p = self.root / lc_tile_filename(i, j)
            self._tiles[(i, j)] = np.asarray(Image.open(p)).astype(bool) if p.exists() else None
        return self._tiles[(i, j)]

    def masked(self, lon: np.ndarray, lat: np.ndarray) -> np.ndarray:
        lon, lat = np.asarray(lon, float), np.asarray(lat, float)
        ti = np.floor(np.round(lat / TILE_DEG, 9)).astype(np.int64)
        tj = np.floor(np.round(lon / TILE_DEG, 9)).astype(np.int64)
        out = np.zeros(lon.shape, bool)
        for i, j in set(zip(ti.ravel().tolist(), tj.ravel().tolist())):
            a = self._tile(i, j)
            if a is None:
                continue
            m = (ti == i) & (tj == j)
            col = np.clip(np.floor((lon[m] - j * TILE_DEG) / TILE_DEG * MASK_PX).astype(np.int64), 0, MASK_PX - 1)
            row = np.clip(np.floor(((i + 1) * TILE_DEG - lat[m]) / TILE_DEG * MASK_PX).astype(np.int64), 0, MASK_PX - 1)
            out[m] = a[row, col]
        return out
```

- [ ] **Step 4: The `footprints` stage in `scripts/vegetation_check.py`**

Add to the docstring's gate list: "footprints (`--stages footprints`, REALISM R3b; needs a package with `features/buildings/mask/`) every dumped tree's trunk and its 4 probes at `building_clearance_m` (E/W/N/S) are off the package's mask, no trunk lies inside a footprint polygon shrunk by half a mask cell's diagonal, and the log names the mask method; trees within the clearance of a footprint polygon are counted (info: the axis probes do not see corners)". Then add after `dist_m`:

```python
MASK_HALF_DIAG_M = 0.76  # half the diagonal of a mask cell (1.16 m x 0.97 m at 33 N): the raster's own position error
FOOTPRINT_POSES = [  # docs/vegetation.md "Trees vs building footprints": the dense_pass midpoint, then Mainside
    (33.2200, -117.3103, 357.7, 105.0, -30.0),
    (33.3050, -117.3140, 350.0, 0.0, -30.0),
]


def footprint_gate(trees: list[tuple[float, float]], mask, clearance_m: float, polygons: list | None = None) -> dict:
    """REALISM R3b. trees: (lat, lon). mask: .masked(lon, lat) (bench.dense_path.BuildingMask). The runtime rule exactly:
    a tree whose trunk or any of the 4 points clearance_m east, west, north or south of it is masked is a probe hit. With
    the footprint polygons (lon/lat): trunks inside a polygon shrunk by MASK_HALF_DIAG_M (gated) and trees within
    clearance_m of a polygon (info: axis probes do not see a corner diagonally within the clearance)."""
    lat = np.array([t[0] for t in trees], float)
    lon = np.array([t[1] for t in trees], float)
    out: dict = {"trees": len(trees)}
    if not trees:
        out.update(probe_hits=0, **({"trunks_inside_footprints": 0, "within_clearance": 0} if polygons is not None else {}))
        out["pass"] = False  # no trees dumped: the gate has nothing to measure
        return out
    dlat = clearance_m / M_PER_DEG
    dlon = clearance_m / (M_PER_DEG * np.cos(np.radians(lat)))
    hit = mask.masked(lon, lat)
    for dx, dy in ((dlon, 0.0), (-dlon, 0.0), (0.0, dlat), (0.0, -dlat)):
        hit |= mask.masked(lon + dx, lat + dy)
    out["probe_hits"] = int(hit.sum())
    ok = out["probe_hits"] == 0
    if polygons is not None:
        import shapely
        from shapely import affinity

        k = M_PER_DEG * math.cos(math.radians(float(lat.mean())))
        polys = [affinity.affine_transform(p, [k, 0.0, 0.0, M_PER_DEG, 0.0, 0.0]) for p in polygons]
        pts = shapely.points(lon * k, lat * M_PER_DEG)
        deep = [g for g in (p.buffer(-MASK_HALF_DIAG_M) for p in polys) if not g.is_empty]
        inside = shapely.STRtree(deep).query(pts, predicate="within")[0] if deep else np.zeros(0, int)
        near = shapely.STRtree(polys).query(pts, predicate="dwithin", distance=clearance_m)[0] if polys else np.zeros(0, int)
        out["trunks_inside_footprints"] = int(len(np.unique(inside)))
        out["within_clearance"] = int(len(np.unique(near)))
        ok = ok and out["trunks_inside_footprints"] == 0
    out["pass"] = ok
    return out


def load_footprints(scene: Path) -> list | None:
    """The package's kept footprint polygons (lon/lat) from its cached Overture snapshot (same filter as the buildings
    layer, minus the terrain-height drop, which keeps all at Pendleton); None when the snapshot is not in the cache."""
    from camsim_scene import buildings as B
    from camsim_scene.cache import Cache, default_cache_root
    from camsim_scene.manifest import Manifest

    m = Manifest.load(scene / "manifest.json")
    if not m.layers.get("buildings"):
        return None
    a = m.source(m.layers["buildings"]["priorities"][0]).assets[0]
    hit = Cache(default_cache_root()).lookup(a.url, a.sha256)
    if hit is None:
        return None
    area = float(m.layers["buildings"]["min_area_m2"])
    return [p for fp in B.read_snapshot(hit[0]) for p in B.clean(fp.geometry, area)]


def footprints_gate(a: argparse.Namespace, report: dict) -> None:
    run = a.out / "footprints"
    dump = run / "cells.jsonl"
    run.mkdir(parents=True, exist_ok=True)
    dump.unlink(missing_ok=True)
    host = rb.Host(scenario.SITES["pendleton"])
    host.pose = pose(*FOOTPRINT_POSES[0])
    try:
        launch({"CAMSIM_SCENE_DIR": str(a.scene)}, [f"camsim.Vegetation.DumpPath {dump}"], host)
        for k, p in enumerate(FOOTPRINT_POSES):
            host.pose = pose(*p)
            time.sleep(5.0)
            wait_settled(dump)
            snapshot(run / f"pose{k}.png")
    finally:
        log = stop(host, run / "CamSimTest.log")
    cells = read_dump(dump)
    trees = [(i[0], i[1]) for d in cells.values() for i in d["instances"]]
    clearance = float(os.environ.get("CAMSIM_VEGETATION_BUILDING_CLEARANCE_M", "2.0"))
    res = footprint_gate(trees, dp.BuildingMask(a.scene / "features" / "buildings" / "mask"), clearance,
                         load_footprints(a.scene))
    line = next((x for x in log.splitlines() if "Vegetation: on (" in x), "")
    res["log_line"] = line.split("LogCamSim: ", 1)[-1]
    res["cells"] = len(cells)
    res["pass"] = res["pass"] and "buildings: mask" in line
    report["footprints"] = res
```

In `main()`, add `"footprints": footprints_gate` to the `run` dict, extend the `--stages` help to name `footprints`, and replace the `gates = [...]` line with:

```python
    gates = ["placement", "grounding", "determinism", "origin_shift", "off_switch", "gt"]
    gates += ["footprints"] if "footprints" in report else []
```

- [ ] **Step 5: The roofs run in `scripts/thermal_check.py`**

Docstring: add to `--runs` "roofs  (REALISM R3b, not in the default; needs --scene, the Pendleton package built with R3b) MWIR, night then noon, no DIS vehicles: roof, nadir 460 m over Camp Pendleton Mainside (ROOF_POSE), 20 deg FOV (10 frames)", and to the checks "(s) roofs (REALISM R3b): building roofs - grassland mean Y on the temporal-mean frame >= +3 DN at noon (night value info, s_night). Roof = the package's footprint mask eroded 1.5 m; grass = land-cover 30 at least 10 m from any footprint; central 60 %, each region >= 2 % of it. (s_air) night: the building_roof class temperature (camsim.Thermal.Log) <= T_air - 0.5 K".

Constants, after `ROAD_CLASS_SLOTS`:

```python
# (s) REALISM R3b: roofs vs grass at noon (image) and the building_roof class vs air at 02:00 (class model). Pose picked
# while writing the R3b plan on pendleton-r3: within a 160 m square view, 34 % of the ground under Overture footprints and
# 26 % WorldCover grassland (30), the best min(building, grass) of a 0.0025 deg scan over 33.28-33.33 N, 117.28-117.34 W.
# Ground 74.88 m HAE (package terrain, layers.buildings.heights_fn at the centre).
ROOF_POSE = dict(lat=33.3000, lon=-117.3175, ground_hae=74.88)
ROOF_UP_M = 460.0  # 20 deg FOV: a 162 m wide view
ROOF_FOV = 20.0
ROOF_NOON_DN = 3.0  # (s) noon: roof - grass mean Y >= +3 DN (white-hot)
ROOF_AIR_K = 0.5  # (s_air) 02:00: T_air - T(building_roof) >= 0.5 K
ROOF_ERODE_M = 1.5  # roof pixels this far inside a footprint (mask registration, PSF)
ROOF_GRASS_CLEAR_M = 10.0  # grass pixels this far from any footprint
ROOF_MIN_FRAC = 0.02  # each region >= 2 % of the central 60 %
ROOF_CLASS_SLOTS = {"building_roof": 15, "grassland": 8}  # FThermalMaterialTable indices
ROOF_M_PER_DEG = 111_320.0
```

Functions, after `road_class_temps`:

```python
def _dilate(m: np.ndarray, r: int) -> np.ndarray:
    """Square binary dilation by r pixels."""
    if r <= 0:
        return m.copy()
    p = np.pad(m, r)
    rows = np.lib.stride_tricks.sliding_window_view(p, 2 * r + 1, axis=0).any(axis=-1)
    return np.lib.stride_tricks.sliding_window_view(rows, 2 * r + 1, axis=1).any(axis=-1)


def roof_masks(shape: tuple[int, int], gsd_m: float, lat0: float, lon0: float, masked, codes) -> tuple[np.ndarray, np.ndarray]:
    """(s) Nadir, heading 0 (image up = north, right = east): roof = the footprint mask eroded by ROOF_ERODE_M; grass =
    land-cover 30 at least ROOF_GRASS_CLEAR_M from any footprint; both within the central 60 %. masked(lon, lat) and
    codes(lon, lat) read the package (bench.dense_path.BuildingMask / LandCover)."""
    h, w = shape
    e, n = np.meshgrid((np.arange(w) - (w - 1) / 2.0) * gsd_m, ((h - 1) / 2.0 - np.arange(h)) * gsd_m)
    lat = lat0 + n / ROOF_M_PER_DEG
    lon = lon0 + e / (ROOF_M_PER_DEG * math.cos(math.radians(lat0)))
    b = np.asarray(masked(lon, lat), bool)
    centre = central_mask(shape)
    roof = ~_dilate(~b, max(1, round(ROOF_ERODE_M / gsd_m))) & centre
    grass = (np.asarray(codes(lon, lat)) == 30) & ~_dilate(b, max(1, round(ROOF_GRASS_CLEAR_M / gsd_m))) & centre
    return roof, grass


def roof_row(band: str, tod: str, y: np.ndarray, masks: tuple[np.ndarray, np.ndarray], threshold: float) -> dict:
    """(s) roof - grass mean Y >= threshold, with each region >= ROOF_MIN_FRAC of the central 60 % (else it fails)."""
    roof, grass = masks
    n_min = ROOF_MIN_FRAC * central_mask(y.shape).sum()
    v = region_mean(y, roof) - region_mean(y, grass)
    enough = roof.sum() >= n_min and grass.sum() >= n_min
    return {
        "check": "s",
        "band": band,
        "time": tod,
        "value": round(v, 2),
        "threshold": threshold,
        "pass": bool(enough and v >= threshold),
        "detail": f"roof - grass mean Y, {int(roof.sum())}/{int(grass.sum())} px"
        + ("" if enough else f" (a region under {ROOF_MIN_FRAC:.0%} of the view: pick another ROOF_POSE)"),
    }


def roof_air_temps(lines: list[str]) -> dict[str, tuple[float, float, float]]:
    """(s_air) (building_roof K, grassland K, T_air K) from the last camsim.Thermal.Log line of each time of day."""
    out: dict[str, tuple[float, float, float]] = {}
    for ln in lines:
        if "classes[K]" not in ln or " sunLux=" not in ln or " Tair=" not in ln:
            continue
        try:
            vals = [float(t) for t in ln.split("classes[K]", 1)[1].split(" terrain=", 1)[0].split()]
            lux = float(ln.split(" sunLux=", 1)[1].split()[0])
            tair = float(ln.split(" Tair=", 1)[1].split()[0])
        except ValueError:
            continue
        r, g = (vals[i] if i < len(vals) else float("nan") for i in (ROOF_CLASS_SLOTS["building_roof"], ROOF_CLASS_SLOTS["grassland"]))
        out["noon" if lux > 0 else "night"] = (r, g, tair)
    return out
```

In `_poses()`, add beside the road views:

```python
        "roof": static(
            scenario.Pose(ROOF_POSE["lat"], ROOF_POSE["lon"], ROOF_POSE["ground_hae"] + ROOF_UP_M, yaw=0.0,
                          gimbal_pitch=-90.0, fov_h=ROOF_FOV)
        ),
```

In `build_runs`, after the roads block:

```python
    if "roofs" in wanted and "mwir" in bands:  # REALISM R3b gates (s, s_air) on the scene package (main() requires --scene)
        runs.append(
            RunSpec(
                "mwir_roofs",
                {"CAMSIM_IR_PRESET": BANDS["mwir"], "CAMSIM_SCENE_DIR": scene or ""},
                1,
                ["night", "noon"],
                lambda tod: [View("roof", p["roof"], 10, None)],
                ml=False,
                exec_cmds=["camsim.Thermal.Log 1"],
                dis_args=[],
            )
        )
```

In `run_group`, before `raise ValueError(label)`: `if label.endswith("_roofs"): return "roofs"  # REALISM R3b gates (s, s_air)`. In `run_band`, before `return None`: `if label.endswith("_roofs"): return label[: -len("_roofs")]`. In `expected_rows`, before `return rows`:

```python
    if "roofs" in runs and "mwir" in bands:
        rows += [("s", "mwir", "noon"), ("s_air", "mwir", "night")]
```

Change `check_all`'s signature to `def check_all(out: Path, results: dict[str, dict], bands: list[str], scene: str | None = None) -> tuple[list, list, list]:` and insert after the (r) block (before the `# (l)` comment):

```python
    # (s) REALISM R3b: roofs vs grass at noon (image; night as info) and the building_roof class vs air at 02:00 (log).
    run = results.get("mwir_roofs")
    if run:
        if not scene:
            raise SystemExit("gates (s, s_air) need --scene: the package's footprint mask and land cover")
        from bench import dense_path as dp

        bmask = dp.BuildingMask(Path(scene) / "features" / "buildings" / "mask")
        lcov = dp.LandCover(Path(scene) / "landcover")
        for rec in run["views"]:
            vd = load_view(out, run, rec)
            if vd is None or rec["view"] != "roof":
                continue
            shots.append(rec["shot"])
            y_mean = vd.y.astype(np.float32).mean(axis=0)
            gsd = nadir_gsd_m(ROOF_UP_M, ROOF_FOV, y_mean.shape[1])
            masks = roof_masks(y_mean.shape, gsd, ROOF_POSE["lat"], ROOF_POSE["lon"], bmask.masked, lcov.codes)
            row = roof_row("mwir", rec["time"], y_mean, masks, ROOF_NOON_DN)
            row["detail"] += f", GSD {gsd:.3f} m, {len(vd.y)} frames"
            if rec["time"] == "noon":
                checks.append(row)
            else:
                row["check"] = "s_night"
                info.append({k: row[k] for k in ("check", "band", "time", "value", "detail")})
            shots.append(overlay(out, vd, masks[0], masks[1], None, len(vd.y) // 2))
        temps = roof_air_temps(thermal_log_lines(out / run["label"] / "CamSimTest.log"))
        nan3 = (float("nan"),) * 3
        r, g, tair = temps.get("night", nan3)
        checks.append(
            {
                "check": "s_air",
                "band": "mwir",
                "time": "night",
                "value": round(tair - r, 2),
                "threshold": ROOF_AIR_K,
                "pass": bool(tair - r >= ROOF_AIR_K),
                "detail": f"class temperatures (camsim.Thermal.Log): building_roof {r:.1f} K, air {tair:.1f} K, grassland {g:.1f} K",
            }
        )
        r, g, tair = temps.get("noon", nan3)
        info.append({"check": "s_air_noon", "band": "mwir", "time": "noon", "value": round(r - g, 2),
                     "detail": f"building_roof {r:.1f} K, grassland {g:.1f} K, air {tair:.1f} K"})
```

In `main()`: add `roofs` to the `--runs` help ("roofs needs --scene; not in the default"); change the scene requirement to `if ({"roads", "roofs"} & wanted) and not a.check_only:` with the error text "--runs roads/roofs needs --scene <Pendleton scene package>"; add `"roofs"` to the set passed to `build_runs(...)`; and call `check_all(out, results, bands, scene)`.

- [ ] **Step 6: `scripts/scene/tools/burn_probe_points.py`**

```python
#!/usr/bin/env python3
# Copyright CamSim Contributors. All Rights Reserved.
"""Acceptance helper (REALISM R3b): CIGI probe points for land-cover code 230 on terrain. Prints the centre of the burned
(230) cell nearest --near, and a point --gap-m beyond that cell's edge (west, east, north or south: the first whose cell
is not 230) with that cell's code. Query them with CAMSIM_SCENE_BUILDINGS_ENABLED=0, so HAT/HOT hits the terrain:

  uv run --project scripts/scene python scripts/scene/tools/burn_probe_points.py "$PKG" --near 33.3000,-117.3175
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image

from camsim_scene.layers.landcover import tile_filename

TILE_DEG, TILE_PX = 0.05, 600
CELL_DEG = TILE_DEG / TILE_PX
M_PER_DEG = 111_320.0
_TILES: dict[tuple[str, int, int], np.ndarray | None] = {}


def _tile(lc_dir: Path, i: int, j: int) -> np.ndarray | None:
    key = (str(lc_dir), i, j)
    if key not in _TILES:
        p = Path(lc_dir) / tile_filename(i, j)
        _TILES[key] = np.asarray(Image.open(p)) if p.exists() else None
    return _TILES[key]


def _cell(lat: float, lon: float) -> tuple[int, int, int, int]:
    i, j = math.floor(round(lat / TILE_DEG, 9)), math.floor(round(lon / TILE_DEG, 9))
    row = min(TILE_PX - 1, max(0, math.floor(((i + 1) * TILE_DEG - lat) / CELL_DEG)))
    col = min(TILE_PX - 1, max(0, math.floor((lon - j * TILE_DEG) / CELL_DEG)))
    return i, j, row, col


def code_at(lc_dir: Path, lat: float, lon: float) -> int:
    i, j, row, col = _cell(lat, lon)
    t = _tile(lc_dir, i, j)
    return int(t[row, col]) if t is not None else 0


def nearest_burned(lc_dir: Path, lat: float, lon: float, search_cells: int = 60) -> tuple[float, float] | None:
    """Centre of the 230 cell nearest (lat, lon) within search_cells (Chebyshev) in the same tile."""
    i, j, row, col = _cell(lat, lon)
    t = _tile(lc_dir, i, j)
    if t is None:
        return None
    r0, r1 = max(0, row - search_cells), min(TILE_PX, row + search_cells + 1)
    c0, c1 = max(0, col - search_cells), min(TILE_PX, col + search_cells + 1)
    rr, cc = np.nonzero(t[r0:r1, c0:c1] == 230)
    if len(rr) == 0:
        return None
    k = int(np.argmin((rr + r0 - row) ** 2 + (cc + c0 - col) ** 2))
    r, c = int(rr[k] + r0), int(cc[k] + c0)
    return (i + 1) * TILE_DEG - (r + 0.5) * CELL_DEG, j * TILE_DEG + (c + 0.5) * CELL_DEG


def outside_point(lc_dir: Path, lat: float, lon: float, gap_m: float = 10.0) -> tuple[float, float, int, str] | None:
    """A point gap_m beyond the edge of the cell centred at (lat, lon), in the first direction whose cell is not 230."""
    kx = M_PER_DEG * math.cos(math.radians(lat))
    half_n, half_e = CELL_DEG / 2.0 * M_PER_DEG, CELL_DEG / 2.0 * kx
    for name, de, dn in (("W", -(half_e + gap_m), 0.0), ("E", half_e + gap_m, 0.0), ("N", 0.0, half_n + gap_m),
                         ("S", 0.0, -(half_n + gap_m))):
        plat, plon = lat + dn / M_PER_DEG, lon + de / kx
        code = code_at(lc_dir, plat, plon)
        if code != 230:
            return plat, plon, code, name
    return None


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pkg", type=Path)
    ap.add_argument("--near", required=True, help="LAT,LON")
    ap.add_argument("--gap-m", type=float, default=10.0)
    a = ap.parse_args()
    lat, lon = (float(v) for v in a.near.split(","))
    lc = a.pkg / "landcover"
    inside = nearest_burned(lc, lat, lon)
    if inside is None:
        raise SystemExit(f"no 230 cell within 60 cells of {a.near}")
    out = outside_point(lc, *inside, gap_m=a.gap_m)
    if out is None:
        raise SystemExit("every neighbour direction is 230 too: pick another --near")
    print(json.dumps({"inside": [round(v, 7) for v in inside], "outside": [round(out[0], 7), round(out[1], 7)],
                      "outside_code": out[2], "direction": out[3]}))


if __name__ == "__main__":
    main()
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `uv run --with pytest --with numpy --with pillow --with pycocotools --with shapely pytest scripts/tests -q`
Expected: all PASS (the shapely-dependent case skips without `--with shapely`).
Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_burn_probe_points.py -q`
Expected: PASS.

- [ ] **Step 8: `CLAUDE.md` Commands rows and commit**

Add to the Commands table: `| \`scripts/scene/tools/burn_probe_points.py PKG --near LAT,LON\` | CIGI probe points inside / 10 m outside a burned (230) land-cover cell (REALISM R3b acceptance) |`; extend the `scripts/vegetation_check.py` row with "; `--stages footprints` (R3b): no tree within the building clearance of the package's footprint mask"; extend the `scripts/thermal_check.py` row with "; R3b (`--runs roofs --scene PKG`): MWIR roofs vs grass at noon, `building_roof` vs air at 02:00".

```bash
git add scripts/bench/dense_path.py scripts/vegetation_check.py scripts/thermal_check.py scripts/scene/tools/burn_probe_points.py \
  scripts/tests/test_bench_dense_path.py scripts/tests/test_vegetation_footprints.py scripts/tests/test_thermal_check_roofs.py \
  scripts/scene/tests/test_burn_probe_points.py CLAUDE.md
git commit -m "test(realism): R3b acceptance tooling: trees vs footprints, MWIR roofs, CIGI 230 probe points"
```

---
### Task 11: Live acceptance on `pendleton-r4`, bench, status docs

**Files:**
- Modify: `docs/scene-packages.md` ("Measured": new "R3b buildings look (2026-…, macOS)"), `docs/vegetation.md` ("Measured": "Trees vs building footprints (R3b mask)"), `docs/thermal.md` ("Acceptance": gates s / s_air results), `ROADMAP.md` (R3b status; close "Build-time building-footprint exclusion for trees"), `REALISM.md` (R3 status), `CLAUDE.md` (test counts), `FOR_LINUX.md` (R3b on Linux/Vulkan items)

**Interfaces:**
- Consumes: everything above. Packages: `PKG3="$PWD/.cache/scene-packages/pendleton-r3"`, `PKG2="$PWD/.cache/scene-packages/pendleton-r2"`, new `PKG="$PWD/.cache/scene-packages/pendleton-r4"`. Outputs under `.cache/r3b/` and `.cache/bench-r3b/` (untracked).
- Produces: measured results in the docs; no code. **No default changes beyond the spec's** (the roof keys, `building_clearance_m` 2.0, `building_roof`); the only sanctioned contingency is the memory rebuild in Step 9.

- [ ] **Step 1: Build `pendleton-r4` from the `pendleton-r3` package and the example toml**

```bash
mkdir -p .cache/r3b
cp -c -R "$PKG3" "$PKG"     # APFS clone (Linux: cp -a --reflink=auto); terrain, imagery, NDVI are skipped
/usr/bin/time -l uv run --project scripts/scene camsim-scene build "$PKG" --config scripts/scene/examples/pendleton.toml \
  --replan -j 6 2>&1 | tee .cache/r3b/build.log; test "${PIPESTATUS[0]}" -eq 0
uv run --project scripts/scene camsim-scene verify "$PKG" --deep -j 6 2>&1 | tee .cache/r3b/verify.log; test "${PIPESTATUS[0]}" -eq 0
OUT="$PWD/.cache/r3b"
(cd scripts/scene/tests/validator && npx 3d-tiles-validator --tilesetFile "$PKG/features/buildings/tileset.json" \
  --reportFile "$OUT/validator.json")
python3 -c "import json;r=json.load(open('.cache/r3b/validator.json'));print(r['numErrors'],r['numWarnings'],r['numInfos'])"
python3 -c "import json;b=json.load(open('$PKG/build.json'));print(json.dumps(b['buildings'],indent=1));print(b['layers']['buildings'],b['layers']['landcover'])"
```

Expected: build exit 0; `verify.json` `ok: true` with `buildings_tileset`, `buildings_heights`, `buildings_roofs`, `buildings_mask` and `landcover_codes` all ok; validator `numErrors` 0. Record: kept buildings (82,260 expected, unchanged from R3), `roofs.textured` / `flat_coarse` / `flat_overflow` / `tiles_by_texel_m`, mask tile count and bytes (`du -sh "$PKG/features/buildings/mask"`), buildings layer bytes vs R3's 247.9 MB, land-cover cells burned (`python3 -c` over the tiles: count of 230), layer and total build time and peak RSS (`/usr/bin/time -l`). If peak worker RSS is over 2,048 MB the build fails by design; report it and stop (do not raise the limit without the user).

- [ ] **Step 2: Determinism and packages without `[buildings]`**

```bash
cp -c -R "$PKG3" "$PKG-b"
uv run --project scripts/scene camsim-scene build "$PKG-b" --config scripts/scene/examples/pendleton.toml --replan -j 6
cmp "$PKG/hashes.txt" "$PKG-b/hashes.txt" && echo REBUILD-IDENTICAL
sed '/^\[buildings\]/,/^$/d' scripts/scene/examples/pendleton.toml > .cache/r3b/pendleton-nobuildings.toml
cp -c -R "$PKG2" .cache/r3b/r2-again
uv run --project scripts/scene camsim-scene build .cache/r3b/r2-again --config .cache/r3b/pendleton-nobuildings.toml --replan -j 6
cmp "$PKG2/hashes.txt" .cache/r3b/r2-again/hashes.txt && cmp "$PKG2/manifest.json" .cache/r3b/r2-again/manifest.json \
  && test ! -e .cache/r3b/r2-again/features && echo NO-BUILDINGS-IDENTICAL
diff <(cut -c67- "$PKG3/hashes.txt") <(cut -c67- "$PKG/hashes.txt") | grep -c '^[<>]'
```

Expected: `REBUILD-IDENTICAL`, `NO-BUILDINGS-IDENTICAL`. The last command lists the files that differ between R3 and R4 by path (the `cut` drops the hash): only `features/buildings/` (GLBs, the new `mask/`), `landcover/` tiles with burned cells, `landcover/index.json`, `landcover/ATTRIBUTION.txt` and `ATTRIBUTION.txt`; record the counts. Delete `$PKG-b` and `.cache/r3b/r2-again` afterwards.

- [ ] **Step 3: Full test suites**

```bash
uv run --project scripts/scene --with pytest pytest scripts/scene/tests -q
uv run --with pytest --with numpy --with pillow --with pycocotools --with shapely pytest scripts/tests -q
scripts/run.sh --build-only
# headless UE automation, filter CamSim (Commands section), then:
python3 scripts/parse_automation_report.py .cache/automation-report/index.json
scripts/run_gpu_tests.sh
grep -rE 'IMPLEMENT_[A-Z_]*AUTOMATION_TEST\(' unreal_project/CamSimTest/Source/CamSimTest/Tests | wc -l
grep -rlE 'IMPLEMENT_[A-Z_]*AUTOMATION_TEST\(' unreal_project/CamSimTest/Source/CamSimTest/Tests | wc -l
grep -rE 'IMPLEMENT_[A-Z_]*AUTOMATION_TEST\(' unreal_project/CamSimTest/Source/CamSimTest/Tests | grep -c '"CamSim.GPU.'
```

Expected: all pass (GPU 26/26). Update `CLAUDE.md`'s two test-count mentions ("N tests across M files, G of them `CamSim.GPU.*`") with the three numbers.

- [ ] **Step 4: Trees gate and the vegetation checks**

```bash
caffeinate -ims uv run --project scripts/scene --with pycocotools python scripts/vegetation_check.py \
  --scene "$PKG" --baseline .cache/veg/baseline --out .cache/r3b/veg --stages footprints,approach,off
python3 -c "import json;r=json.load(open('.cache/r3b/veg/report.json'));print(json.dumps({k:r[k] for k in ('footprints','placement','determinism','off_switch')},indent=1))"
```

Expected: `footprints.pass` true with `probe_hits` 0, `trunks_inside_footprints` 0 and a `log_line` ending `buildings: mask (clearance 2.0 m))`; `within_clearance` recorded (info); `placement`, `determinism` and `off_switch` pass. (`grounding`, `origin_shift` and `gt` are reported as usual; they are R4's gates and unaffected.)

Without a mask nothing changed: rerun the approach stage on `pendleton-r2` and compare every cell hash with the R4 run already on disk (`.cache/veg/check-i1-r2/`):

```bash
caffeinate -ims uv run --project scripts/scene --with pycocotools python scripts/vegetation_check.py \
  --scene "$PKG2" --baseline .cache/veg/baseline --out .cache/r3b/veg-r2 --stages approach
python3 - <<'EOF'
import json
def cells(p):
    return {(d["row"], d["col"]): d["hash"] for d in map(json.loads, open(p).read().splitlines()) if d}
for side in ("north", "south"):
    a, b = cells(f".cache/veg/check-i1-r2/{side}/cells.jsonl"), cells(f".cache/r3b/veg-r2/{side}/cells.jsonl")
    common = set(a) & set(b)
    print(side, len(common), "common cells,", sum(a[k] != b[k] for k in common), "hash mismatches")
EOF
```

Expected: hundreds of common cells per side and **0 mismatches**; the run's log line ends `buildings: none`.

- [ ] **Step 5: Roof shots, textures on and off**

```bash
cp -c -R "$PKG" "$PKG-flat"
sed 's/^roof_textures = true/roof_textures = false/' scripts/scene/examples/pendleton.toml > .cache/r3b/pendleton-flat.toml
uv run --project scripts/scene camsim-scene build "$PKG-flat" --config .cache/r3b/pendleton-flat.toml --replan -j 6
caffeinate -ims uv run --project scripts/scene --with pycocotools python scripts/vegetation_check.py \
  --scene "$PKG-flat" --baseline .cache/veg/baseline --out .cache/r3b/veg-flat --stages footprints
```

The `footprints` stage saves `pose0.png` (the `dense_pass` midpoint on the Oceanside dense path, 357.7 m HAE, heading 105°, −30°) and `pose1.png` (Mainside, 350 m, north, −30°) for each package: `.cache/r3b/veg/footprints/pose{0,1}.png` (textured) and `.cache/r3b/veg-flat/footprints/pose{0,1}.png` (flat). Also take the bench's `dense_oblique` shot on both (`run_bench.py --phases dense_pass --shots dense_oblique`, below). Judge from the shots that low-rise roof texture sits on its roof within 1–2 m (roof edges against the drawn walls; tall buildings carry NAIP's lean, a known limit). Record the judgement and attach the paths for the user's visual review (Step 10).

- [ ] **Step 6: Infrared**

```bash
caffeinate -ims uv run scripts/thermal_check.py --band mwir --runs roofs,roads --scene "$PKG" --out .cache/r3b/thermal-pendleton
caffeinate -ims uv run scripts/thermal_check.py --band both --out .cache/r3b/thermal-default
```

Expected: `[s] PASS mwir noon` (roof − grass ≥ +3 DN), `[s_air] PASS mwir night` (building_roof ≤ air − 0.5 K; the log values near the model's 282 / 284 K for 21 December), `[r]` rows pass as before (R2's road gate on the burned package); the default run passes every gate a–q as before R3b. If (s) reports "a region under 2 % of the view", the burn or the mask is not where the plan's pose survey (pendleton-r3 footprints) found it: inspect `.cache/r3b/thermal-pendleton/overlays/` before choosing another `ROOF_POSE`, and record the change.

- [ ] **Step 7: CIGI on terrain inside a burned cell**

```bash
uv run --project scripts/scene python scripts/scene/tools/burn_probe_points.py "$PKG" --near 33.3000,-117.3175 | tee .cache/r3b/probe.json
CAMSIM_SCENE_DIR="$PKG" CAMSIM_SCENE_BUILDINGS_ENABLED=0 CAMSIM_MULTICAST_ADDR=127.0.0.1 \
  scripts/run.sh --headless --local --detach
# inside / outside from probe.json; the sensor 300 m above the inside point, looking down
uv run scripts/send_cigi_test.py --lat <inside lat> --lon <inside lon> --alt 400 --pitch -90 --duration 20 \
  --hat-extended <inside lat>,<inside lon> --hat-extended <outside lat>,<outside lon> | tee .cache/r3b/cigi.txt
scripts/stop.sh
```

Expected: the inside response reports material code **230** (terrain, buildings off), the outside one reports `outside_code` from `probe.json`. Then repeat with buildings on (no `CAMSIM_SCENE_BUILDINGS_ENABLED=0`) at a roof point: 230 from the building hit as in R3. Record both.

- [ ] **Step 8: Bench — combined sweep and full runs**

```bash
uv run --with numpy --with pillow python scripts/bench/sweep.py --scene "$PKG" --out .cache/bench-r3b/combined \
  --axis buildings=off,on --axis trees=off,on --repeat 2
for label in full-off full-on; do
  env_off=$([ "$label" = full-off ] && echo "CAMSIM_SCENE_BUILDINGS_ENABLED=0 CAMSIM_VEGETATION_ENABLED=0")
  env CAMSIM_SCENE_DIR="$PKG" $env_off caffeinate -ims uv run --with numpy --with pillow python scripts/bench/run_bench.py \
    --label "r3b-$label" --site pendleton --gpu-mem --out ".cache/bench-r3b/$label"
done
uv run --with numpy --with pillow python scripts/bench/compare.py .cache/bench-r3b/full-off .cache/bench-r3b/full-on
for label in flat-dense textured-dense; do
  pkg=$([ "$label" = flat-dense ] && echo "$PKG-flat" || echo "$PKG")
  env CAMSIM_SCENE_DIR="$pkg" uv run --with numpy --with pillow python scripts/bench/run_bench.py --label "r3b-$label" \
    --site pendleton --gpu-mem --phases dense_pass --shots dense_oblique --out ".cache/bench-r3b/$label"
done
```

Gates (both on at the defaults vs all off; the sweep's `sweep.md` computes them against its all-off row, the full runs per phase with `compare.py`): game p95 ≤ +3.0 ms, memory ≤ +1,024 MiB over the whole run (the full runs' per-phase `gpu_mem_mib_max`, max over phases), every run 30 fps (`emitted_fps` ≥ 29.5) and 0 dropped frames. Record the sweep table (rows buildings × trees), the full-run phase table (as R3's), and the textured vs flat `dense_pass` delta (the atlas's own cost).

- [ ] **Step 9: Memory contingency (only if Step 8's memory gate fails)**

```bash
cp -c -R "$PKG" "$PKG-t1"
sed 's/^roof_texel_m = 0.5 /roof_texel_m = 1.0 /' scripts/scene/examples/pendleton.toml > .cache/r3b/pendleton-t1.toml
uv run --project scripts/scene camsim-scene build "$PKG-t1" --config .cache/r3b/pendleton-t1.toml --replan -j 6
env CAMSIM_SCENE_DIR="$PKG-t1" caffeinate -ims uv run --with numpy --with pillow python scripts/bench/run_bench.py \
  --label r3b-full-on-t1 --site pendleton --gpu-mem --out .cache/bench-r3b/full-on-t1
```

Report both runs (0.5 m and 1.0 m) side by side. Do not change the `roof_texel_m` default yourself: the choice is the user's (spec: "report both"). The mask and land cover of `$PKG-t1` are byte-identical to `$PKG`'s (Task 5's guard); check with `diff <(grep -E ' (features/buildings/mask|landcover)/' "$PKG/hashes.txt") <(grep -E ' (features/buildings/mask|landcover)/' "$PKG-t1/hashes.txt")` (no output).

- [ ] **Step 10: Visual review (the user's)**

Send the user: `.cache/r3b/veg/footprints/pose{0,1}.png` vs `.cache/r3b/veg-flat/footprints/pose{0,1}.png` (roof close-ups, textured vs flat), `.cache/bench-r3b/{textured,flat}-dense/shots/dense_oblique.png`, trees near buildings before and after (`.cache/veg/i1-footprints/close_oblique.png`, the R3/R4 runtime check, vs `.cache/r3b/veg/footprints/pose1.png`), and MWIR day and night (`.cache/r3b/thermal-pendleton/shots/` roof views and overlays). Record "pending" (or the user's sign-off / requested changes) in `docs/scene-packages.md`.

- [ ] **Step 11: Status docs**

- `docs/scene-packages.md` "Measured": a "R3b buildings look (date, macOS)" subsection with a gate table: 1 Package (Step 1 numbers, verify, validator), 2 Determinism / no-`[buildings]` (Step 2), 3 Trees (Step 4), 4 Roofs (Step 5 judgement, shot paths), 5 Infrared (Step 6 values), 6 CIGI (Step 7 responses), 7 Bench (Step 8 tables; Step 9 if run), 8 Visual review (pending / signed off).
- `docs/vegetation.md` "Measured": "Trees vs building footprints (R3b mask)": trees dumped, probe hits 0, trunks inside 0, within clearance (info), the r2 hash comparison, and the game p95 / memory change of the trees-on rows against R4's runtime check (+2.5 ms p95, +34 MiB).
- `docs/thermal.md` "Acceptance": gates s / s_air with values.
- `ROADMAP.md`: tick "Build-time building-footprint exclusion for trees" with "done in R3b (date): `features/buildings/mask/` + `GenerateCell` clearance predicate; the runtime check stays only for R3 packages"; add a dated R3b status line under the Realism track (what shipped, the measured gates, open items: visual review, Linux/Vulkan). No editor work (Cesium's default material samples the embedded texture); say so.
- `REALISM.md` R3: add "**R3b (date):** footprint mask (trees), land-cover burn 230 (`building_roof` in IR, CIGI terrain), roof atlas from NAIP; measured …; deferred: lean correction, wall textures, pitched roofs, KTX2".
- `FOR_LINUX.md` §5: add R3b items: build `pendleton-r4` (or copy it), `thermal_check.py --runs roofs`, the combined sweep on Vulkan, and the atlas texture memory on the RTX 5080 at 1, 2 and 4 instances.

- [ ] **Step 12: Commit**

```bash
git add docs/scene-packages.md docs/vegetation.md docs/thermal.md ROADMAP.md REALISM.md CLAUDE.md FOR_LINUX.md
git commit -m "docs(realism): R3b buildings look measured on Pendleton"
```

---
## Self-Review

**Spec coverage.**
- §1 one shared raster of exactly the kept buildings → Task 1 (`plan_cached` + `footprint_raster`, both from `plan_from_manifest`; `test_plan_runs_once_per_build`).
- §1 fine mask: grid, naming, 4800², mode `1`, centre rule, holes, unwritten empty tiles, `index.json` → Task 1 (layout tests, hole test, edge test); runtime reader → Task 6.
- §1 burn: ≥ 32 of 64, code 230, priority, `codes` legend → Task 2 (`test_burn_cells_half_cover_rule`, `test_buildings_burn_between_paved_areas_and_rail`, legend/credit tests, end-to-end burn = mask).
- §1 determinism and hashing: `BUILDINGS_RULES_VERSION` bump (Task 1), landcover inputs (Task 2 Step 7), byte-identical rebuilds (Task 5 rebuild tests, Task 11 Step 2), packages without `[buildings]` unchanged (Task 2 `test_no_buildings_never_reads_footprints` + settings test; Task 11 Step 2 on Pendleton).
- §2 atlas: scope every tier, walls flat (Task 4); pixel source `sample_entries` + colour match, 0.5 m, > 2 m fallback (Task 4 `ImageryRoofSampler`, `test_coarse_source_falls_back_to_flat`); packing AABB, 2 px edge gutter, order, power of two ≤ max, texel ladder, 2.0 m fallback, recorded texel (Task 3, Task 4); tile output `TEXCOORD_0`, (0, 0) walls, shared arrays, JPEG q85 4:2:0, sampler, factor 1, three primitives, `roof_texel_m`, validator (Task 4 glTF tests, validator case); config keys in the settings hash (Task 4).
- §3 `BuildingsMaskDir`, `FVegetationBuildingMask` (worker decode, bitsets, 8-tile LRU, missing = none) → Task 6; `IsClear` 5-point rule, hash streams unchanged, method order, `drop_in_buildings` gating, log line, `building_clearance_m` → Task 7; `building_roof` 16th class, 230 mapping, cited parameters, no shader change, GPU parity → Task 8; code 50 unchanged and `thermal.land_cover.enabled: false` unchanged (no code path touched; Task 8 Step 4 runs the existing tests); CIGI no change + doc → Task 9 Step 6; textures need no runtime change (Task 4, documented in Task 9).
- §4 docs → Task 9 (all nine files) and Task 11 Step 11 (measured status, ROADMAP item closed).
- Testing → pytest in Tasks 1–5 and 10, UE automation in Tasks 6–8, live acceptance 1–6 in Task 11 Steps 4–10 (bench gates and the `roof_texel_m = 1.0` contingency in Steps 8–9).
- Known limits → Task 9 (scene-packages, vegetation, thermal).

**Placeholder scan.** Every code step has the code; every run step has the command and the expected result. Values filled at execution time are measured ones only (build sizes and times, bench numbers, CIGI responses, the visual judgement) and each step says where they come from. `ROOF_POSE` (33.3000, −117.3175, ground 74.88 m HAE) was surveyed on `pendleton-r3` while writing this plan (its comment records how); Task 11 Step 6 says what to do if the burned package disagrees.

**Type and name consistency.** `MASK_PX`/`MASK_FACTOR`/`BURN_MIN_CELLS`/`MASK_RULES_VERSION`, `raster_tiles`, `unpack`, `burn_cells`, `save_raster`/`load_raster`, `mask_index`/`mask_index_bytes`, `write_mask_tile`; `footprint_inputs`, `plan_cached`, `footprint_raster` → `(Path, str)`, `write_mask`; `BUILDING_CODE`, `PRIORITY["building"]`, `burn_tile(..., buildings=None)`, `_cut_landcover(..., footprints=None)`; `GUTTER_PX`, `TEXEL_STEPS_M`, `texel_ladder`, `Placement`, `Packing`, `pack_roofs`; `RoofOptions.from_settings`, `RoofTexture`, `roof_texture`, `enu_to_lonlat`, `MAX_SOURCE_M`, `ROOF_JPEG_QUALITY`; `TileGeometry`, `tile_geometry`, `tile_mesh(..., roof, sampler, stats)`; `TileMesh(..., textured_indices, texcoords, texture_jpeg, extras)`, `DecodedTile(..., textured_indices, texcoords, texture, extras)`; `native_res_m`, `ImageryRoofSampler`; `FVegetationBuildingMask::{TilePx, CellsPerDeg, MaxTiles, WordsPerTile, ParseIndex, DecodeMaskPng, CellOf, IsClearOf, IsMasked, IsClear, NumCached, NumLoads}`; `FClearPredicate`, `GenerateCell(..., IsClear)`; `EBuildingExclusion`, `ChooseBuildingExclusion`, `DescribeBuildingExclusion`; `FVegetationConfig::BuildingClearanceM`, `FSceneConfig::BuildingsMaskDir`; `FThermalMaterialTable::BuildingRoof`; `dense_path.BuildingMask.masked`, `footprint_gate`, `load_footprints`, `roof_masks`, `roof_row`, `roof_air_temps`, `burn_probe_points.{code_at, nearest_burned, outside_point}` — used with the same names and signatures in every task. The Python code of Tasks 1–5 and the pure acceptance helpers of Task 10 were run against a scratch copy of `camsim_scene` while writing the plan (the full scene suite, 515 tests, passed with Tasks 1–5 applied; the textured validator case passed).

**Review Focus.** Five uncovered failure modes, each with its test in the owning task: tile-edge buildings (Task 1), nodata under a roof (Task 4), roof-only rebuild leaves the mask and burn untouched (Task 5), bad mask tiles at runtime (Task 6), no sub-2 m imagery at all (Task 4).

**Decomposition changes from the suggested outline (and why).**
1. The `[buildings]` roof keys are in Task 4, not Task 5: the layer reads them, so Task 4 cannot be tested without them; Task 5 keeps `verify` and the rebuild guards.
2. The acceptance tooling is its own Task 10 (unit-tested script changes: `BuildingMask`, the `footprints` stage, the `roofs` run, `burn_probe_points.py`), separate from the live Task 11, so a reviewer can accept or reject the tools before anyone spends a bench session on them. That makes 11 tasks.

**Spec ambiguities resolved here (for the reviewer).**
1. *"The landcover `RULES_VERSION` is bumped" vs "packages without `[buildings]` stay byte-identical".* `features.RULES_VERSION` is in every `[features]` package's manifest (and the derived-features cache key), so bumping it would change R2/R3 packages without buildings. The burn is versioned instead by `footprints.MASK_RULES_VERSION` inside a `landcover.buildings` settings entry that exists only with `[buildings]`, plus `BUILDINGS_RULES_VERSION` (bumped to "2") in the landcover inputs. Test: `test_landcover_settings_without_buildings_are_r3s`.
2. *`FamilyForCode(230) = BuiltUp`.* The `BuiltUp` refinement replaces the code's class by the asphalt/concrete split (`ThermalReference.cpp` `RefinedClassData`), so `building_roof` would never be used and roofs would not be below air at night. The plan uses `Bare` (blend toward vegetation by the base-colour index, else `building_roof`), which keeps the spec's intent (roofs refine on their real colour). Test: `CamSim.Thermal.Materials.BuildingRoofKeepsItsClass`.
3. *Trees gate "0 trees inside a footprint buffered by the clearance".* The binding 5-point rule (axis probes) cannot see a footprint corner diagonally within the clearance, so a buffered-polygon count is not zero by construction. The gate is: 0 trees whose trunk or 4 probes hit the package's mask (the runtime rule exactly) and 0 trunks inside a footprint polygon shrunk by half a mask cell's diagonal (0.76 m, the raster's own error); trees within the buffered polygon are reported as information.
4. *Mask vs `scene.buildings.enabled`.* The spec's method order says "mask present: the mask is used"; the plan uses the mask whenever the package has one and `drop_in_buildings` is true, even with the buildings tileset disabled (the roofs are in the imagery either way).
5. *"Best source coarser than 2 m".* Taken as the source supplying most of the roof's triangle centroids (ties to the finer one); a roof with no valid centroid is flat.
6. *Packing overflow.* A roof that cannot fit even alone at 2.0 m is flat from the start, so one giant roof does not push its whole tile to 2.0 m; the ladder then runs on the rest, and at the last step the rectangles that still do not fit (in packing order) are flat.
7. *Colour balance.* Roofs use the imagery sources in priority order with the colour match on its target source; NAIP's edge feather and the water fade (leaf-tile blends) are not applied, because a roof is textured only from a ≤ 2 m source (NAIP).
8. *Landcover inputs.* Beside the three items the spec names, the inputs also hash the footprint raster's key (snapshot, `min_area_m2`, rules, terrain, mask rules), since the kept buildings depend on the terrain (no-height drops).
9. *Older packages.* R3 manifests (no `mask`, `roof_*` or `landcover.buildings` settings) build exactly as before: the mask, burn and roofs are each gated on their settings keys.
10. *Cache location.* The shared plan and raster are cached under the package's `.state/` (not hashed, not packed), not the fetch cache, so a package-only rebuild still reuses them.
11. *Ranges the spec leaves open.* `roof_texel_m` ∈ [0.25, 2.0]; `roof_texture_max_px` ∈ {256, …, 8192} (powers of two). The mask decoder also accepts 8-bit greyscale tiles (nonzero = building), as a tolerance; the tooling writes 1-bit.
12. *"Roof cooler than air at 02:00".* The display is AGC-relative, so the gate reads the `building_roof` class temperature against `T_air` from the `camsim.Thermal.Log` line (as R2's `r_info` reads class temperatures); "hotter than grass at noon" is measured in the image (gate s).
