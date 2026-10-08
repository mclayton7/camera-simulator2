# Scene package build tooling (REALISM R0) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `camsim-scene` turns a bounding box into a self-contained, whole-Earth scene package (quantized-mesh terrain, geodetic TMS imagery, WorldCover land cover, manifest, attribution, hashes), verifies it, and packs it into one reproducible SquashFS image.

**Architecture:** A uv Python project in `scripts/scene/` (`camsim_scene`). `plan` discovers source assets through adapters and freezes them in `manifest.json`; `fetch` downloads whole assets into a content-addressed cache, hashes them and computes valid-data footprints; `build` plans the tile pyramid per layer from regions and footprints, then runs per-tile jobs in a process pool with resume markers and atomic writes; `verify` re-checks hashes, availability, licences and (deep) decoded heights against the sources; `pack` calls `mksquashfs` with fixed flags. All datum maths goes through explicit PROJ pipeline strings pinned in the manifest.

**Tech Stack:** Python 3.12–3.13, numpy, scipy, rasterio (bundled GDAL), pyproj (PROJ 9.8), shapely 2, pydelatin, Pillow, requests; pytest; Ubuntu 24.04 build container with squashfs-tools.

**Spec:** `docs/superpowers/specs/2026-10-07-scene-package-r0-design.md` (read it with this plan). Evidence: `docs/realism-r0-spike.md` (its "Pitfalls" table is a checklist), `docs/realism/data-sources.md`, `docs/realism/cesium-findings.md`, `docs/realism/offline-hosting.md`. Reference code: `scripts/scene/spike/`.

## Global Constraints

- R0 is tooling only: no CamSim runtime change (`imagery.source: tms`, offline profile, `Main.umap` and package land cover are R1). Gate 5 applies the spike's TMS patch locally and does not commit it.
- Python `>=3.12,<3.14` (pydelatin 0.3.0 wheels stop at cp313; rasterio arm64 wheels need macOS 15+). No system GDAL or PDAL needed outside the container.
- One tile grid for terrain and imagery: Cesium geographic TMS, EPSG:4326 lon/lat in ITRF2014, 2 × 1 root tiles, tile size `180° / 2^z`, `y` counted from the south.
- Regions: `globe` terrain + imagery z0–8; `ring` (bbox + `ring_km`, default 100) z9–10; `bbox` terrain to the source limit (z16 on 1 m, z14 on 1/3″, z8 on ETOPO), imagery to z17. The highest-detail region a tile overlaps wins.
- Complete siblings: a tile has either 0 or 4 children, in both layers.
- Terrain: 257 × 257 sample grid + margin (≥ 32 samples), 30 m feather, `max_error = max(0.1 m, 0.25 × 77067 m / 2^z)`, quantized-mesh-1.0, gzip level 9, mtime 0, `octvertexnormals`.
- Imagery: 256 px, JPEG q85 4:2:0 (Pillow), parents = 2 × 2 box filter of the four children.
- Datum target ITRF2014 (`EPSG:7912`), coordinate epoch 2010.0, explicit pipelines stored in the manifest, `PROJ_NETWORK=OFF` during builds. Known point: 100 m NAVD88 at UTM 11N 470000 E 3685000 N → 65.283 m.
- Licence allow-list default: public domain and CC BY 4.0, written as SPDX ids `LicenseRef-PublicDomain-USGov` and `CC-BY-4.0` (Decision 16); anything else needs `--allow <id>`.
- Reproducibility: same `manifest.json` + same build-image digest ⇒ every file except `build.json` byte-identical. Native builds must pass `verify` but needn't match bytes.
- Peak RSS per worker ≤ 2 GB; memory independent of area size.
- Package files: `manifest.json`, `build.json`, `ATTRIBUTION.txt`, `hashes.txt`, `terrain/layer.json`, `terrain/{z}/{x}/{y}.terrain`, `imagery/tilemapresource.xml`, `imagery/{z}/{x}/{y}.jpg`, `landcover/index.json`, `landcover/<lat>_<lon>.png` (today's 4B format, unchanged).
- Unit tests make no network calls (recorded or synthetic inputs). Tests that need the internet are marked `@pytest.mark.network` and run only with `--network`.
- Pre-commit rejects files > 500 KB: no large fixtures in git.
- Code style: match `scripts/landcover/fetch_worldcover.py` (module docstring, `from __future__ import annotations`, type hints, small functions). `ruff check` and `ruff format --check` must pass on `scripts/scene/` (line length 120, set in its `pyproject.toml`).
- Commit messages end with the two attribution lines from the session (`Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01YKrr13eicb7kvapaTx4o61`). They are omitted from the commit commands below for brevity; add them to every commit.

## Decisions this plan makes where the spec is silent or inconsistent

1. **`hashes.txt` excludes `manifest.json` too.** The spec lists every file except `build.json` and `hashes.txt`, but `manifest.json` carries `hashes_sha256` (the sha256 of `hashes.txt`), which would be circular. The manifest is covered by its own `hashes_sha256` instead.
2. **GEOID12A is unsupported.** `us_noaa_g2012au0.tif` isn't on cdn.proj.org (404, checked 2026-10-07; only the GEOID12B grids are). 1 m projects on GEOID12A are skipped with a warning, like unknown geoids. For Pendleton that is `San_Diego_CA_2014_LiDAR` (Oceanside), which `CA_SanDiegoCo_D24` (GEOID18, newer) covers anyway.
3. **Profiles.** `preview` = terrain `dep3_13 → etopo2022`, imagery `wc_s2 → bmng`, bbox zooms terrain 14 / imagery 13. `sim` = terrain `dep3_1m → dep3_13 → etopo2022`, imagery `naip_pc → wc_s2 → bmng`, bbox zooms 16 / 17. (REALISM.md puts 1 m terrain under `high`, but the spec's gate 5 checks 1 m heights on a `sim` build.) Land cover is `worldcover` in both.
4. **Imagery leaves follow the same depth rule as terrain**: min(region limit, best source limit), with source limits NAIP z17, WC S2 z13, Blue Marble z8. So ocean tiles inside the bbox stop where NAIP stops instead of upsampling Blue Marble to z17.
5. **Source asset footprints** are valid-data polygons computed at `fetch` from each asset's coarse overview (dilated by one overview pixel, simplified, 1e-7° precision) and stored in the manifest, so "the zoom limit follows the source actually used" (e.g. the D24 1 m DEM stops on a diagonal). Global sources (ETOPO, Blue Marble) cover the globe by declaration.
6. **Overview levels** are chosen from the zoom alone (target sample spacing in metres at the equator), never from the tile's position, so neighbouring tiles read the same pixels. Assets without internal overviews or tiling (ETOPO, Blue Marble PNGs) get a derived tiled GeoTIFF with average overviews in the cache at `fetch` (`.cache/scene/derived/`).
7. **The vertical offset lattice** (NAVD88 → ellipsoid, ETOPO geoid) is evaluated exactly on nodes spaced `tile_size(z)/32` and aligned to the global tile grid, then interpolated bilinearly. Neighbours share nodes on their common edge, so their edge heights agree exactly. Measured error ≤ 5 mm at z9 and ≤ 0.3 mm at z12+ (GEOID18, Pendleton).
8. **The 30 m feather is measured in degrees of latitude** (30 m / 111,320 m per degree) on both axes, so every tile at a zoom uses the same distance field. East–west it is ≈ 30 m × cos φ (25 m at Pendleton).
9. **Horizon occlusion point**: Cesium's algorithm; for tiles that reach past the horizon of their own centre (z ≤ 1) it stores the ellipsoid surface point under the centre.
10. **Land cover is rebuilt as a whole unless its inputs hash matches** (`.state/landcover.json`): ~2,200 tiles, a few seconds; no per-tile markers.
11. **`verify.json` and the `.sqfs` are written beside the package** (`<pkg>.verify.json`, `<pkg>.sqfs`, `<pkg>.sqfs.sha256`), so the package directory never changes after `build`.
12. **Builds remove stale tiles**: tiles and markers that aren't in the current plan (e.g. after a smaller bbox) are deleted before the jobs run, and a `.state/lock` file lock refuses a second concurrent build of the same package.
13. **Source options live in the manifest** (`sources[].options`, plus `adapter`), and each asset record carries `id`, `group`, `rank`, `role`, so `build` can recreate every adapter from the manifest alone. `licence_allow` and `tool` are also in the manifest.
14. **WC S2 tone curve**: reflectance DN × 1e-4, white at 0.30, gamma 1/2.2 → 8-bit. NAIP and WC S2 colours won't match (colour balancing is a non-goal).
15. **WC S2 licence**: the composite GeoTIFFs carry `license=CC-BY 4.0 - https://creativecommons.org/licenses/by/4.0/` in their own tags (checked 2026-10-07 on `N33W118`). The adapter refuses an asset whose tag says otherwise. This settles the spec's open licence question.

16. **SPDX licence identifiers.** The register's keys, adapters' `licence`, `--allow` and `manifest.licence_allow` use SPDX ids (`CC-BY-4.0`, `ODbL-1.0`, `CDLA-Permissive-2.0`, `CC-BY-NC-SA-4.0`); licences SPDX doesn't list use its `LicenseRef-` form (`LicenseRef-PublicDomain-USGov` for USGS/USDA/NOAA/NASA data, `LicenseRef-Proprietary`). The spec's `public-domain` / `cc-by-4.0` are these two.
17. **The tile grid is OGC TMS 2.0's `WorldCRS84Quad`** (OGC 17-083r4: CRS84, 2 × 1 tiles at level 0, 256 px, y from the top in OGC terms). The manifest's `tiling` names it (`"tile_matrix_set": "WorldCRS84Quad"` with its URI) next to the TMS (y-from-south) addressing that Cesium uses, and `docs/scene-packages.md` states the row flip (`row_ogc = 2^z - 1 - y_tms`).
18. **Derived rasters are Cloud Optimized GeoTIFFs** (OGC 21-026): `prepare_raster` writes through GDAL's COG driver (deflate, 512 px blocks, average overviews), so cached derivatives can be read with range requests by any COG reader and inspected with standard tools.

## Review Focus

1. **A rebuild into an existing package after the bbox or zooms shrank** — the user expects the package to contain exactly the new plan (no leftover tiles, so `verify` passes). Pinned in Task 18 (`test_build_removes_stale_tiles`).
2. **Two builds started on the same package directory** (two terminals, or a retried CI job) — the second must refuse with a clear message, not interleave writes. Pinned in Task 17 (`test_build_lock_refuses_second_holder`) and Task 18 (`test_build_refuses_when_locked`).
3. **An asset that is entirely nodata** (an offshore NAIP quad, a 1 m DEM tile outside its project boundary) — it must be ignored for planning (no deeper tiles), not crash the footprint or the tile job. Pinned in Task 9 (`test_footprint_of_all_nodata_is_none`) and Task 18 (`test_all_nodata_asset_adds_no_tiles`).
4. **Planetary Computer SAS token expiring mid-fetch** (16 GB of NAIP takes longer than a token lives) — the fetch must re-sign and continue; the error message must never print the token. Pinned in Task 7 (`test_forbidden_resigns_and_redacts`) and Task 11 (`test_pc_signer_refreshes_on_force`).
5. **Degenerate bboxes**: one narrower than a z17 tile, one on exact tile boundaries, one that would cross the antimeridian (W > E) — the first two must plan normally; the third must be rejected with a message that names the antimeridian. Pinned in Task 2 (`test_bbox_on_tile_boundary_does_not_deepen_neighbour`) and Task 3 (`test_tiny_bbox_plans_to_z17`, `test_bbox_crossing_antimeridian_is_rejected`).

## File Structure

```
scripts/scene/
  pyproject.toml, uv.lock, .python-version, .gitignore, README.md
  Dockerfile, build.sh                      # reference build container (Task 22)
  examples/pendleton.toml, examples/pendleton-preview-10k.toml
  camsim_scene/
    __init__.py        # __version__ only (import-light: the land-cover wrapper imports this package)
    fsutil.py          # atomic_write, sha256_file, sha256_bytes
    tiling.py          # tile grid, Region, Coverage, LayerPlan, plan_tiles, available_ranges
    tms.py             # tilemapresource.xml writer/parser, plan_bounds
    config.py          # scene.toml -> ScenePlan (profiles, regions, validation, default file)
    manifest.py        # Manifest/SourceRecord/AssetRecord, canonical JSON, settings hashes, hashes.txt
    qmesh.py           # quantized-mesh-1.0 encoder/decoder, layer.json
    licences.py        # register, allow-list, attribution text
    licences.toml
    net.py             # Http: JSON/HEAD/stream with retries
    cache.py           # content-addressed blobs, URL index, derived files
    datum.py           # pinned PROJ pipelines, DatumTransform, offset lattice
    context.py         # BuildContext, WorkerState (rasters, transforms, asset index), coverage
    engine.py          # Markers, run_tile, run_pool, BuildLock, remove_stale, stats
    pipeline.py        # plan_scene, fetch_scene, build_scene (+ worker batch functions)
    verify.py          # quick + deep checks -> <pkg>.verify.json
    pack.py            # mksquashfs
    cli.py             # camsim-scene entry point
    sources/
      __init__.py      # registry: make_source()
      base.py          # Layer, Area, Asset, Source protocol, SourceRaster (sampling), prepare, footprint
      tnm.py           # TNM Access API helper
      dep3_13.py, dep3_1m.py, naip_pc.py, wc_s2.py, etopo2022.py, bmng.py, worldcover.py
    layers/
      __init__.py
      terrain.py, imagery.py, landcover.py
  tests/
    conftest.py, fakes.py, rasters.py, fake_sources.py
    fixtures/http/*.json, fixtures/grids/us_noaa_g2018u0.tif (79 KB crop), fixtures/make_geoid_fixture.py
    test_*.py
  tools/
    render_check.py, hot_check.py, tms_overlay.patch, offline.sb   # gate 5 (moved from spike/)
scripts/landcover/fetch_worldcover.py       # thin wrapper over camsim_scene (same CLI/output/tests)
docs/scene-packages.md                      # user guide
.github/workflows/ci.yml                    # + scene-tests job
```

---

### Task 1: Project scaffold, file helpers, CI job

**Files:**
- Create: `scripts/scene/pyproject.toml`, `scripts/scene/.python-version`, `scripts/scene/.gitignore`, `scripts/scene/README.md`
- Create: `scripts/scene/camsim_scene/__init__.py`, `scripts/scene/camsim_scene/fsutil.py`
- Create: `scripts/scene/tests/conftest.py`, `scripts/scene/tests/test_fsutil.py`
- Modify: `.github/workflows/ci.yml` (new job after `python-tests`)

**Interfaces:**
- Produces: `camsim_scene.__version__: str`; `fsutil.atomic_write(path: Path, data: bytes) -> None`, `fsutil.sha256_file(path: Path) -> str`, `fsutil.sha256_bytes(data: bytes) -> str`; pytest option `--network` and marker `network`.

- [ ] **Step 1: Write the project files**

`scripts/scene/pyproject.toml`:

```toml
[project]
name = "camsim-scene"
version = "0.1.0"
description = "CamSim scene packages: terrain, imagery and land cover from public data (REALISM R0)"
requires-python = ">=3.12,<3.14"
dependencies = [
    "numpy>=2.5.3",
    "pillow>=12.3.0",
    "pydelatin>=0.3.0",
    "pyproj>=3.8.0",
    "rasterio>=1.5.2",
    "requests>=2.34.2",
    "scipy>=1.18.1",
    "shapely>=2.2.0",
]

[project.scripts]
camsim-scene = "camsim_scene.cli:main"

[dependency-groups]
dev = [
    "pytest>=8.4",
    "quantized-mesh-encoder>=0.5.0",
]

[build-system]
requires = ["hatchling"]
build-backend = "hatchling.build"

[tool.hatch.build.targets.wheel]
packages = ["camsim_scene"]

[tool.pytest.ini_options]
testpaths = ["tests"]
markers = ["network: needs the internet (run with --network)"]

[tool.ruff]
line-length = 120
target-version = "py312"
```

`scripts/scene/.python-version`: `3.12`

`scripts/scene/.gitignore`:

```
.venv/
out/
```

`scripts/scene/README.md`:

```markdown
# camsim-scene

Scene package build tooling (REALISM R0). Guide: [`docs/scene-packages.md`](../../docs/scene-packages.md).
Design: [`docs/superpowers/specs/2026-10-07-scene-package-r0-design.md`](../../docs/superpowers/specs/2026-10-07-scene-package-r0-design.md).

    uv run --project scripts/scene camsim-scene --help
    uv run --project scripts/scene --with pytest pytest scripts/scene/tests
```

`scripts/scene/camsim_scene/__init__.py`:

```python
"""camsim-scene: scene packages (terrain, imagery, land cover) from public data. Keep this module import-light."""

__version__ = "0.1.0"
```

- [ ] **Step 2: Write the failing test**

`scripts/scene/tests/conftest.py`:

```python
"""Shared pytest setup: tests marked `network` run only with --network."""

import pytest


def pytest_addoption(parser):
    parser.addoption("--network", action="store_true", help="run tests that need the internet")


def pytest_collection_modifyitems(config, items):
    if config.getoption("--network"):
        return
    skip = pytest.mark.skip(reason="needs --network")
    for item in items:
        if "network" in item.keywords:
            item.add_marker(skip)
```

`scripts/scene/tests/test_fsutil.py`:

```python
import hashlib
import os

import pytest

from camsim_scene import fsutil


def test_atomic_write_creates_parents_and_hashes(tmp_path):
    p = tmp_path / "a" / "b" / "c.bin"
    fsutil.atomic_write(p, b"hello")
    assert p.read_bytes() == b"hello"
    assert fsutil.sha256_file(p) == hashlib.sha256(b"hello").hexdigest() == fsutil.sha256_bytes(b"hello")
    assert oct(p.stat().st_mode & 0o777) == "0o644"


def test_interrupted_atomic_write_leaves_nothing(tmp_path, monkeypatch):
    p = tmp_path / "t.bin"

    def boom(src, dst):
        raise KeyboardInterrupt

    monkeypatch.setattr(os, "replace", boom)
    with pytest.raises(KeyboardInterrupt):
        fsutil.atomic_write(p, b"x" * 1000)
    assert list(tmp_path.iterdir()) == []
```

- [ ] **Step 3: Run it to see it fail**

Run: `cd scripts/scene && uv lock && uv run pytest tests/test_fsutil.py -v`
Expected: FAIL, `ImportError: cannot import name 'fsutil'`.

- [ ] **Step 4: Implement `fsutil.py`**

```python
"""Filesystem helpers shared by every module: atomic writes and sha256."""

from __future__ import annotations

import hashlib
import os
import tempfile
from pathlib import Path

CHUNK = 1 << 20


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(CHUNK), b""):
            h.update(chunk)
    return h.hexdigest()


def atomic_write(path: Path, data: bytes) -> None:
    """Write via a temp file in the same directory + os.replace: readers never see a partial file.
    Mode 0644 regardless of umask, so packed images don't depend on the build host."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=f".{path.name}.", suffix=".part")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.chmod(tmp, 0o644)
        os.replace(tmp, path)
    except BaseException:
        Path(tmp).unlink(missing_ok=True)
        raise
```

- [ ] **Step 5: Run the tests**

Run: `cd scripts/scene && uv run pytest tests/test_fsutil.py -v`
Expected: 2 passed.

- [ ] **Step 6: Add the CI job**

In `.github/workflows/ci.yml`, after the `python-tests` job, add:

```yaml
  scene-tests:
    name: Scene package tooling (pytest)
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Install uv
        uses: astral-sh/setup-uv@v4

      - name: Unit tests (scripts/scene/tests, no network)
        run: uv run --project scripts/scene --frozen pytest scripts/scene/tests -q
```

- [ ] **Step 7: Lint and commit**

Run: `uvx ruff check scripts/scene && uvx ruff format --check scripts/scene`
Expected: no findings (run `uvx ruff format scripts/scene` first if needed).

```bash
git add scripts/scene/pyproject.toml scripts/scene/uv.lock scripts/scene/.python-version scripts/scene/.gitignore \
  scripts/scene/README.md scripts/scene/camsim_scene scripts/scene/tests .github/workflows/ci.yml
git commit -m "feat(scene): camsim-scene project scaffold, atomic writes, CI job"
```

---

### Task 2: Tile grid, regions, pyramid planning, `tilemapresource.xml`

**Files:**
- Create: `scripts/scene/camsim_scene/tiling.py`, `scripts/scene/camsim_scene/tms.py`
- Test: `scripts/scene/tests/test_tiling.py`

**Interfaces:**
- Produces (`tiling`): `Bounds = tuple[float, float, float, float]` (W, S, E, N); `GLOBE`; `tile_size_deg(z) -> float`; `tile_bounds(z, x, y) -> Bounds`; `make_keys(x, y) -> np.ndarray[int64]`, `split_keys(keys) -> (x, y)`; `children(keys) -> keys`; `tile_boxes(z, keys) -> np.ndarray[shapely.Geometry]`; `Region(name, polygon, max_zoom)` with `from_bounds`, `geometry()`, `to_dict()`, `from_dict()`; `Coverage(geoms, limits)` with `add(geom, limit)`; `LayerPlan(tiles: dict[int, keys], leaves: dict[int, keys])` with `max_zoom`, `count()`, `has(z, x, y)`, `is_leaf(z, x, y)`; `plan_tiles(regions, layer, coverage) -> LayerPlan`; `available_ranges(plan) -> list[list[dict]]`; `available_keys(available) -> dict[int, np.ndarray]`.
- Produces (`tms`): `tilemapresource_xml(title, max_zoom, bounds) -> str`; `parse_tilemapresource(text) -> tuple[list[int], Bounds]`; `plan_bounds(plan) -> Bounds`.

- [ ] **Step 1: Write the failing tests**

`scripts/scene/tests/test_tiling.py`:

```python
import math
import xml.etree.ElementTree as ET

import numpy as np
import shapely

from camsim_scene import tiling, tms
from camsim_scene.tiling import Coverage, Region, plan_tiles


def globe_coverage(limit=8):
    return Coverage([shapely.box(*tiling.GLOBE)], [limit])


def test_tile_bounds_roots_and_z1():
    assert tiling.tile_bounds(0, 0, 0) == (-180.0, -90.0, 0.0, 90.0)
    assert tiling.tile_bounds(0, 1, 0) == (0.0, -90.0, 180.0, 90.0)
    assert tiling.tile_bounds(1, 3, 1) == (90.0, 0.0, 180.0, 90.0)
    assert tiling.tile_size_deg(16) == 180.0 / 65536


def test_pendleton_point_is_inside_its_z16_tile():
    lon, lat, z = -117.38, 33.22, 16
    s = tiling.tile_size_deg(z)
    x, y = math.floor((lon + 180) / s), math.floor((lat + 90) / s)
    w, so, e, n = tiling.tile_bounds(z, x, y)
    assert w <= lon < e and so <= lat < n


def test_keys_round_trip_and_children():
    k = tiling.make_keys([5, 7], [3, 9])
    x, y = tiling.split_keys(k)
    assert x.tolist() == [5, 7] and y.tolist() == [3, 9]
    cx, cy = tiling.split_keys(tiling.children(tiling.make_keys([1], [2])))
    assert sorted(zip(cx.tolist(), cy.tolist())) == [(2, 4), (2, 5), (3, 4), (3, 5)]


def test_globe_region_is_complete_to_its_zoom():
    plan = plan_tiles([Region.from_bounds("globe", tiling.GLOBE, {"terrain": 3})], "terrain", globe_coverage())
    assert [len(plan.tiles[z]) for z in range(4)] == [2, 8, 32, 128]
    assert plan.max_zoom == 3 and len(plan.leaves[3]) == 128 and all(len(plan.leaves[z]) == 0 for z in range(3))


def test_source_limit_caps_depth():
    plan = plan_tiles([Region.from_bounds("globe", tiling.GLOBE, {"terrain": 5})], "terrain", globe_coverage(2))
    assert plan.max_zoom == 2


def test_bbox_region_deepens_only_near_bbox_with_complete_siblings():
    bbox = (10.0, 10.0, 10.5, 10.5)
    regions = [Region.from_bounds("globe", tiling.GLOBE, {"imagery": 2}), Region.from_bounds("bbox", bbox, {"imagery": 7})]
    plan = plan_tiles(regions, "imagery", globe_coverage(17))
    assert plan.max_zoom == 7
    box = shapely.box(*bbox)
    for z in range(3, 8):
        x, y = tiling.split_keys(plan.tiles[z])
        parents = set(zip((x // 2).tolist(), (y // 2).tolist()))
        for px, py in parents:  # complete siblings: every parent with children has all four
            assert all(plan.has(z, 2 * px + dx, 2 * py + dy) for dx in (0, 1) for dy in (0, 1))
            assert shapely.box(*tiling.tile_bounds(z - 1, px, py)).intersects(box)


def test_bbox_on_tile_boundary_does_not_deepen_neighbour():
    b = tiling.tile_bounds(3, 9, 5)  # exactly one z3 tile
    regions = [Region.from_bounds("globe", tiling.GLOBE, {"terrain": 2}), Region.from_bounds("bbox", b, {"terrain": 4})]
    plan = plan_tiles(regions, "terrain", globe_coverage())
    x, y = tiling.split_keys(plan.tiles[4])
    assert set(zip((x // 2).tolist(), (y // 2).tolist())) == {(9, 5)}


def test_partial_source_footprint_limits_depth_locally():
    regions = [Region.from_bounds("globe", tiling.GLOBE, {"terrain": 1}), Region.from_bounds("bbox", (0, 0, 40, 40), {"terrain": 6})]
    cov = Coverage([shapely.box(*tiling.GLOBE), shapely.box(0.0, 0.0, 5.0, 5.0)], [3, 6])
    plan = plan_tiles(regions, "terrain", cov)
    x, y = tiling.split_keys(plan.tiles[6])
    s = tiling.tile_size_deg(6)
    assert len(x) > 0 and np.all(-180 + x * s < 5.0 + s) and np.all(-90 + y * s < 5.0 + s)


def test_available_ranges_merge_runs_and_rows():
    tiles = {0: tiling.make_keys([0, 1], [0, 0]), 1: tiling.make_keys([0, 1, 0, 1, 3], [0, 0, 1, 1, 1])}
    plan = tiling.LayerPlan(tiles, {0: tiles[0][:0], 1: tiles[1]})
    av = tiling.available_ranges(plan)
    assert av[0] == [{"startX": 0, "startY": 0, "endX": 1, "endY": 0}]
    assert av[1] == [{"startX": 0, "startY": 0, "endX": 1, "endY": 1}, {"startX": 3, "startY": 1, "endX": 3, "endY": 1}]
    back = tiling.available_keys(av)
    assert np.array_equal(np.sort(back[1]), np.sort(tiles[1]))


def test_tilemapresource_xml_round_trip():
    xml = tms.tilemapresource_xml("pendleton", 2, tiling.GLOBE)
    root = ET.fromstring(xml)
    assert root.find("SRS").text == "EPSG:4326"
    assert root.find("TileSets").get("profile") == "geodetic"
    sets = root.find("TileSets").findall("TileSet")
    assert [s.get("href") for s in sets] == ["0", "1", "2"]
    assert float(sets[2].get("units-per-pixel")) == 180.0 / 256 / 4
    assert tms.parse_tilemapresource(xml) == ([0, 1, 2], tiling.GLOBE)
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd scripts/scene && uv run pytest tests/test_tiling.py -v`
Expected: FAIL, `ImportError` (no `tiling`).

- [ ] **Step 3: Implement `tiling.py`**

```python
"""Cesium's geographic TMS grid (EPSG:4326, 2 x 1 root tiles, tile size 180 deg / 2^z, y from the south)
and pyramid planning: which tiles a layer has at each zoom, from regions and source footprints."""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np
import shapely

Bounds = tuple[float, float, float, float]  # W, S, E, N in degrees
GLOBE: Bounds = (-180.0, -90.0, 180.0, 90.0)
MAX_PLAN_ZOOM = 22


def tile_size_deg(z: int) -> float:
    return 180.0 / (1 << z)


def tile_bounds(z: int, x: int, y: int) -> Bounds:
    s = tile_size_deg(z)
    return (-180.0 + x * s, -90.0 + y * s, -180.0 + (x + 1) * s, -90.0 + (y + 1) * s)


def make_keys(x, y) -> np.ndarray:
    return (np.asarray(x, np.int64) << 32) | np.asarray(y, np.int64)


def split_keys(keys) -> tuple[np.ndarray, np.ndarray]:
    k = np.asarray(keys, np.int64)
    return k >> 32, k & 0xFFFFFFFF


def children(keys: np.ndarray) -> np.ndarray:
    x, y = split_keys(keys)
    cx = np.concatenate([2 * x, 2 * x + 1, 2 * x, 2 * x + 1])
    cy = np.concatenate([2 * y, 2 * y, 2 * y + 1, 2 * y + 1])
    return np.unique(make_keys(cx, cy))


def tile_boxes(z: int, keys: np.ndarray) -> np.ndarray:
    x, y = split_keys(keys)
    s = tile_size_deg(z)
    return shapely.box(-180.0 + x * s, -90.0 + y * s, -180.0 + (x + 1) * s, -90.0 + (y + 1) * s)


@dataclass
class Region:
    """A lon/lat polygon with a maximum zoom per layer. A tile takes the highest limit of the regions it overlaps."""

    name: str
    polygon: list[tuple[float, float]]
    max_zoom: dict[str, int]

    @classmethod
    def from_bounds(cls, name: str, b: Bounds, max_zoom: dict[str, int]) -> Region:
        w, s, e, n = (float(v) for v in b)
        return cls(name, [(w, s), (e, s), (e, n), (w, n), (w, s)], dict(max_zoom))

    def geometry(self) -> shapely.Polygon:
        return shapely.Polygon(self.polygon)

    def to_dict(self) -> dict:
        return {
            "name": self.name,
            "polygon": [[float(a), float(b)] for a, b in self.polygon],
            "max_zoom": {k: int(v) for k, v in sorted(self.max_zoom.items())},
        }

    @classmethod
    def from_dict(cls, d: dict) -> Region:
        return cls(d["name"], [(float(a), float(b)) for a, b in d["polygon"]], {k: int(v) for k, v in d["max_zoom"].items()})


@dataclass
class Coverage:
    """Where a layer's sources have data: footprints (lon/lat) with each source's zoom limit."""

    geoms: list = field(default_factory=list)
    limits: list[int] = field(default_factory=list)

    def add(self, geom, limit: int) -> None:
        self.geoms.append(geom)
        self.limits.append(int(limit))


@dataclass
class LayerPlan:
    tiles: dict[int, np.ndarray]  # z -> sorted keys
    leaves: dict[int, np.ndarray]  # z -> sorted keys of tiles without children

    @property
    def max_zoom(self) -> int:
        return max(self.tiles)

    def count(self) -> int:
        return sum(len(k) for k in self.tiles.values())

    @staticmethod
    def _member(keys: np.ndarray | None, x: int, y: int) -> bool:
        if keys is None or len(keys) == 0:
            return False
        k = (int(x) << 32) | int(y)
        i = int(np.searchsorted(keys, k))
        return i < len(keys) and int(keys[i]) == k

    def has(self, z: int, x: int, y: int) -> bool:
        return self._member(self.tiles.get(z), x, y)

    def is_leaf(self, z: int, x: int, y: int) -> bool:
        return self._member(self.leaves.get(z), x, y)


def _overlaps(boxes: np.ndarray, geom) -> np.ndarray:
    """Interior overlap: a tile that only touches a region along an edge or at a corner doesn't count."""
    return shapely.intersects(boxes, geom) & ~shapely.touches(boxes, geom)


def plan_tiles(regions: list[Region], layer: str, coverage: Coverage) -> LayerPlan:
    """Expand the two root tiles level by level. A tile gets its four children when
    min(highest overlapping region limit, highest intersecting source limit) > z (complete siblings)."""
    tree = shapely.STRtree(coverage.geoms) if coverage.geoms else None
    limits = np.asarray(coverage.limits, np.int64)
    region_geoms = [(r.geometry(), r.max_zoom.get(layer, -1)) for r in regions]
    tiles = {0: make_keys([0, 1], [0, 0])}
    leaves: dict[int, np.ndarray] = {}
    z = 0
    while True:
        keys = tiles[z]
        boxes = tile_boxes(z, keys)
        rlim = np.full(len(keys), -1, np.int64)
        for geom, mz in region_geoms:
            hit = _overlaps(boxes, geom)
            rlim[hit] = np.maximum(rlim[hit], mz)
        slim = np.full(len(keys), -1, np.int64)
        if tree is not None:
            ti, gi = tree.query(boxes, predicate="intersects")
            np.maximum.at(slim, ti, limits[gi])
        expand = (np.minimum(rlim, slim) > z) & (z < MAX_PLAN_ZOOM)
        leaves[z] = keys[~expand]
        if not expand.any():
            return LayerPlan(tiles, leaves)
        tiles[z + 1] = children(keys[expand])
        z += 1


def available_ranges(plan: LayerPlan) -> list[list[dict]]:
    """layer.json `available`: per zoom, rectangles of tiles (runs along x, merged across consecutive rows)."""
    out = []
    for z in range(plan.max_zoom + 1):
        x, y = split_keys(plan.tiles[z])
        order = np.lexsort((x, y))
        x, y = x[order].tolist(), y[order].tolist()
        runs, start = [], 0
        for i in range(1, len(x) + 1):
            if i == len(x) or y[i] != y[start] or x[i] != x[i - 1] + 1:
                runs.append((y[start], x[start], x[i - 1]))
                start = i
        rects, open_rects = [], {}
        for yy, x0, x1 in runs:
            r = open_rects.get((x0, x1))
            if r is not None and r["endY"] == yy - 1:
                r["endY"] = yy
            else:
                r = {"startX": x0, "startY": yy, "endX": x1, "endY": yy}
                rects.append(r)
                open_rects[(x0, x1)] = r
        rects.sort(key=lambda r: (r["startY"], r["startX"]))
        out.append(rects)
    return out


def available_keys(available: list[list[dict]]) -> dict[int, np.ndarray]:
    out = {}
    for z, rects in enumerate(available):
        parts = []
        for r in rects:
            xs, ys = np.meshgrid(np.arange(r["startX"], r["endX"] + 1), np.arange(r["startY"], r["endY"] + 1))
            parts.append(make_keys(xs.ravel(), ys.ravel()))
        out[z] = np.unique(np.concatenate(parts)) if parts else np.zeros(0, np.int64)
    return out
```

- [ ] **Step 4: Implement `tms.py`**

```python
"""TMS `tilemapresource.xml` (geodetic profile, EPSG:4326, origin -180/-90, 256 px JPEG tiles).
For file:// URLs Cesium's TMS overlay must point at this file itself (it appends nothing)."""

from __future__ import annotations

import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape

import numpy as np

from .tiling import Bounds, LayerPlan, split_keys, tile_size_deg


def tilemapresource_xml(title: str, max_zoom: int, bounds: Bounds) -> str:
    sets = "".join(
        f'<TileSet href="{z}" units-per-pixel="{180.0 / 256 / (1 << z):.16g}" order="{z}"/>' for z in range(max_zoom + 1)
    )
    w, s, e, n = bounds
    return (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<TileMap version="1.0.0" tilemapservice="http://tms.osgeo.org/1.0.0">'
        f"<Title>{escape(title)}</Title><Abstract/><SRS>EPSG:4326</SRS>"
        f'<BoundingBox minx="{w:.10f}" miny="{s:.10f}" maxx="{e:.10f}" maxy="{n:.10f}"/>'
        '<Origin x="-180" y="-90"/>'
        '<TileFormat width="256" height="256" mime-type="image/jpeg" extension="jpg"/>'
        f'<TileSets profile="geodetic">{sets}</TileSets></TileMap>\n'
    )


def parse_tilemapresource(text: str) -> tuple[list[int], Bounds]:
    root = ET.fromstring(text)
    bb = root.find("BoundingBox")
    bounds = tuple(float(bb.get(k)) for k in ("minx", "miny", "maxx", "maxy"))
    levels = sorted(int(t.get("href")) for t in root.find("TileSets").findall("TileSet"))
    return levels, bounds


def plan_bounds(plan: LayerPlan) -> Bounds:
    """Union of the bounds of every tile in the plan."""
    w = s = 180.0
    e = n = -180.0
    for z, keys in plan.tiles.items():
        if len(keys) == 0:
            continue
        x, y = split_keys(keys)
        t = tile_size_deg(z)
        w = min(w, float(-180.0 + np.min(x) * t))
        e = max(e, float(-180.0 + (np.max(x) + 1) * t))
        s = min(s, float(-90.0 + np.min(y) * t))
        n = max(n, float(-90.0 + (np.max(y) + 1) * t))
    return (w, s, e, n)
```

- [ ] **Step 5: Run the tests**

Run: `cd scripts/scene && uv run pytest tests/test_tiling.py -v`
Expected: 10 passed.

- [ ] **Step 6: Commit**

```bash
git add scripts/scene/camsim_scene/tiling.py scripts/scene/camsim_scene/tms.py scripts/scene/tests/test_tiling.py
git commit -m "feat(scene): geodetic TMS grid, regions, pyramid planning with complete siblings"
```

---

### Task 3: `scene.toml` → `ScenePlan`

**Files:**
- Create: `scripts/scene/camsim_scene/config.py`
- Test: `scripts/scene/tests/test_config.py`

**Interfaces:**
- Consumes: `tiling.Region`, `tiling.GLOBE`, `tiling.Bounds`, `tiling.plan_tiles`, `tiling.Coverage` (test only).
- Produces: `LAYERS = ("terrain", "imagery", "landcover")`; `TERRAIN_GRID = 257`, `FEATHER_M = 30.0`, `TILE_PX = 256`, `TILING: dict`; `layer_settings(plan) -> dict` (the manifest's `layers` section); `PROFILES: dict`; `ConfigError(ValueError)`; `validate_bbox(b) -> Bounds`; `ring_bounds(bbox, ring_km) -> Bounds`; `ScenePlan` (frozen dataclass: `name, bbox, profile, seed, ring_km, bmng_month, jpeg_quality, priorities: dict[str, list[str]], source_options: dict[str, dict], zoom: dict[str, dict[str, int]], allow: tuple[str, ...]`) with `ring_bounds()`, `area(kind: str) -> Bounds` (`"globe" | "ring" | "bbox"`), `regions() -> list[Region]`, `source_ids() -> list[str]`; `load_scene(path) -> ScenePlan`; `parse_scene(data: dict) -> ScenePlan`; `default_scene_toml(name, bbox, profile) -> str`.

- [ ] **Step 1: Write the failing tests**

`scripts/scene/tests/test_config.py`:

```python
import pytest
import shapely

from camsim_scene import config, tiling
from camsim_scene.config import ConfigError, parse_scene

PENDLETON = [-117.62, 33.19, -117.24, 33.52]


def test_defaults_for_sim_profile():
    p = parse_scene({"name": "pendleton", "bbox": PENDLETON})
    assert p.profile == "sim" and p.ring_km == 100 and p.bmng_month == 7 and p.jpeg_quality == 85
    assert p.priorities["terrain"] == ["dep3_1m", "dep3_13", "etopo2022"]
    assert p.priorities["imagery"] == ["naip_pc", "wc_s2", "bmng"]
    assert p.priorities["landcover"] == ["worldcover"]
    assert p.source_options["bmng"]["month"] == 7 and p.source_options["naip_pc"]["year"] == "2022"
    assert [r.name for r in p.regions()] == ["globe", "ring", "bbox"]
    assert [r.max_zoom for r in p.regions()] == [
        {"terrain": 8, "imagery": 8},
        {"terrain": 10, "imagery": 10},
        {"terrain": 16, "imagery": 17},
    ]
    assert p.source_ids() == ["dep3_1m", "dep3_13", "etopo2022", "naip_pc", "wc_s2", "bmng", "worldcover"]


def test_preview_profile_has_no_naip_or_1m():
    p = parse_scene({"name": "pv", "bbox": PENDLETON, "profile": "preview"})
    assert p.priorities["terrain"] == ["dep3_13", "etopo2022"] and p.priorities["imagery"] == ["wc_s2", "bmng"]
    assert p.regions()[2].max_zoom == {"terrain": 14, "imagery": 13}


def test_ring_is_100_km_at_pendleton():
    w, s, e, n = config.ring_bounds(tuple(PENDLETON), 100.0)
    assert s == pytest.approx(33.19 - 100 / 111.32) and n == pytest.approx(33.52 + 100 / 111.32)
    assert 1.05 < (-117.62 - w) < 1.15 and 1.05 < (e + 117.24) < 1.15


def test_zoom_and_priority_overrides():
    p = parse_scene(
        {
            "name": "t",
            "bbox": [10, 10, 10.5, 10.5],
            "priorities": {"terrain": ["a"], "imagery": ["b"], "landcover": []},
            "sources": {"a": {"adapter": "x:Y", "k": 1}},
            "zoom": {"globe": {"terrain": 2, "imagery": 2}, "bbox": {"terrain": 6}},
        }
    )
    assert p.priorities["landcover"] == [] and p.source_options["a"] == {"adapter": "x:Y", "k": 1}
    assert p.zoom["globe"] == {"terrain": 2, "imagery": 2} and p.zoom["bbox"] == {"terrain": 6, "imagery": 17}


@pytest.mark.parametrize(
    "patch, match",
    [
        ({"bbox": [10, 0, 5, 1]}, "antimeridian"),
        ({"bbox": [0, 5, 1, 4]}, "S < N"),
        ({"bbox": [0, 0, 1, 91]}, "outside"),
        ({"bbox": [0, 0, 1]}, "4 numbers"),
        ({"profile": "ultra"}, "profile"),
        ({"bmng_month": 13}, "bmng_month"),
        ({"jpeg_quality": 0}, "jpeg_quality"),
        ({"name": "Bad Name"}, "name"),
        ({"colour": "red"}, "unknown key"),
        ({"priorities": {"terrain": "dep3_13"}}, "list"),
    ],
)
def test_invalid_scenes_are_rejected(patch, match):
    with pytest.raises(ConfigError, match=match):
        parse_scene({"name": "ok", "bbox": PENDLETON, **patch})


def test_bbox_crossing_antimeridian_is_rejected():
    with pytest.raises(ConfigError, match="antimeridian"):
        parse_scene({"name": "fiji", "bbox": [177.0, -19.0, -179.0, -16.0]})


def test_tiny_bbox_plans_to_z17():
    p = parse_scene({"name": "tiny", "bbox": [-117.380001, 33.220001, -117.380000, 33.220002]})
    cov = tiling.Coverage([shapely.box(*tiling.GLOBE)], [17])
    plan = tiling.plan_tiles(p.regions(), "imagery", cov)
    assert plan.max_zoom == 17 and len(plan.tiles[17]) == 4


def test_tiling_names_the_ogc_tile_matrix_set():
    assert config.TILING["tile_matrix_set"] == "WorldCRS84Quad" and config.TILING["root_tiles"] == [2, 1]


def test_layer_settings_carry_quality_and_priorities():
    ls = config.layer_settings(parse_scene({"name": "p", "bbox": PENDLETON, "jpeg_quality": 80}))
    assert ls["imagery"]["jpeg_quality"] == 80 and ls["terrain"]["grid"] == 257 and ls["terrain"]["feather_m"] == 30.0
    assert ls["landcover"]["priorities"] == ["worldcover"] and len(ls["landcover"]["bounds"]) == 4


def test_default_scene_toml_round_trips(tmp_path):
    path = tmp_path / "pendleton.scene.toml"
    path.write_text(config.default_scene_toml("pendleton", tuple(PENDLETON), "sim"))
    p = config.load_scene(path)
    assert p.name == "pendleton" and p.bbox == tuple(PENDLETON) and p.profile == "sim"
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd scripts/scene && uv run pytest tests/test_config.py -v`
Expected: FAIL, `ImportError` (no `config`).

- [ ] **Step 3: Implement `config.py`**

```python
"""scene.toml -> ScenePlan: validation, profile defaults, regions (globe / ring / bbox).

    name = "pendleton"
    bbox = [-117.62, 33.19, -117.24, 33.52]   # W S E N, degrees; must not cross the antimeridian
    profile = "sim"                            # preview | sim
    seed = 0
    ring_km = 100
    bmng_month = 7
    jpeg_quality = 85
    allow = []                                 # extra licence ids (see licences.toml)
    [priorities]                               # optional; defaults from the profile
    terrain = ["dep3_1m", "dep3_13", "etopo2022"]
    [sources.naip_pc]                          # per-source options; `adapter` picks a non-default class
    year = "2022"
    [zoom.bbox]                                # optional zoom overrides per region
    imagery = 17
"""

from __future__ import annotations

import math
import re
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

from .tiling import GLOBE, Bounds, Region

LAYERS = ("terrain", "imagery", "landcover")
TILED_LAYERS = ("terrain", "imagery")
KM_PER_DEG = 111.32
NAME_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{0,63}$")
KEYS = {"name", "bbox", "profile", "seed", "ring_km", "bmng_month", "jpeg_quality", "allow", "priorities", "sources", "zoom"}

PROFILES = {
    "preview": {
        "priorities": {"terrain": ["dep3_13", "etopo2022"], "imagery": ["wc_s2", "bmng"], "landcover": ["worldcover"]},
        "bbox_zoom": {"terrain": 14, "imagery": 13},
    },
    "sim": {
        "priorities": {
            "terrain": ["dep3_1m", "dep3_13", "etopo2022"],
            "imagery": ["naip_pc", "wc_s2", "bmng"],
            "landcover": ["worldcover"],
        },
        "bbox_zoom": {"terrain": 16, "imagery": 17},
    },
}
GLOBE_ZOOM = {"terrain": 8, "imagery": 8}
RING_ZOOM = {"terrain": 10, "imagery": 10}
DEFAULT_SOURCE_OPTIONS = {"naip_pc": {"year": "2022"}}
TERRAIN_GRID = 257
FEATHER_M = 30.0
TILE_PX = 256
TILING = {  # OGC TMS 2.0 WorldCRS84Quad, addressed TMS-style (y from the south) as Cesium expects
    "tile_matrix_set": "WorldCRS84Quad",
    "tile_matrix_set_uri": "http://www.opengis.net/def/tilematrixset/OGC/1.0/WorldCRS84Quad",
    "scheme": "tms", "projection": "EPSG:4326", "root_tiles": [2, 1], "tile_px": 256, "y_origin": "south",
}


class ConfigError(ValueError):
    pass


def validate_bbox(b) -> Bounds:
    if not isinstance(b, (list, tuple)) or len(b) != 4:
        raise ConfigError(f"bbox must be 4 numbers W S E N, got {b!r}")
    w, s, e, n = (float(v) for v in b)
    if not all(math.isfinite(v) for v in (w, s, e, n)):
        raise ConfigError(f"bbox must be finite, got {b!r}")
    if not (-180.0 <= w <= 180.0 and -180.0 <= e <= 180.0 and -90.0 <= s <= 90.0 and -90.0 <= n <= 90.0):
        raise ConfigError(f"bbox outside [-180, 180] x [-90, 90]: {b!r}")
    if w >= e:
        raise ConfigError(f"bbox needs W < E (a bbox must not cross the antimeridian; split it): {b!r}")
    if s >= n:
        raise ConfigError(f"bbox needs S < N: {b!r}")
    return (w, s, e, n)


def ring_bounds(bbox: Bounds, ring_km: float) -> Bounds:
    """bbox grown by ring_km on every side; the longitude pad uses the pole-ward latitude (never too small)."""
    w, s, e, n = bbox
    dlat = ring_km / KM_PER_DEG
    lat = min(89.0, max(abs(s), abs(n)) + dlat)
    dlon = ring_km / (KM_PER_DEG * math.cos(math.radians(lat)))
    return (max(-180.0, w - dlon), max(-90.0, s - dlat), min(180.0, e + dlon), min(90.0, n + dlat))


@dataclass(frozen=True)
class ScenePlan:
    name: str
    bbox: Bounds
    profile: str = "sim"
    seed: int = 0
    ring_km: float = 100.0
    bmng_month: int = 7
    jpeg_quality: int = 85
    priorities: dict = field(default_factory=dict)
    source_options: dict = field(default_factory=dict)
    zoom: dict = field(default_factory=dict)
    allow: tuple[str, ...] = ()

    def ring_bounds(self) -> Bounds:
        return ring_bounds(self.bbox, self.ring_km)

    def area(self, kind: str) -> Bounds:
        return {"globe": GLOBE, "ring": self.ring_bounds(), "bbox": self.bbox}[kind]

    def regions(self) -> list[Region]:
        return [
            Region.from_bounds("globe", GLOBE, self.zoom["globe"]),
            Region.from_bounds("ring", self.ring_bounds(), self.zoom["ring"]),
            Region.from_bounds("bbox", self.bbox, self.zoom["bbox"]),
        ]

    def source_ids(self) -> list[str]:
        out: list[str] = []
        for layer in LAYERS:
            out += [s for s in self.priorities[layer] if s not in out]
        return out


def _int(data: dict, key: str, default: int, lo: int, hi: int) -> int:
    v = data.get(key, default)
    if not isinstance(v, int) or isinstance(v, bool) or not lo <= v <= hi:
        raise ConfigError(f"{key} must be an integer in [{lo}, {hi}], got {v!r}")
    return v


def parse_scene(data: dict) -> ScenePlan:
    unknown = set(data) - KEYS
    if unknown:
        raise ConfigError(f"unknown key(s) in scene: {sorted(unknown)}")
    name = data.get("name")
    if not isinstance(name, str) or not NAME_RE.match(name):
        raise ConfigError(f"name must match {NAME_RE.pattern}, got {name!r}")
    bbox = validate_bbox(data.get("bbox"))
    profile = data.get("profile", "sim")
    if profile not in PROFILES:
        raise ConfigError(f"profile must be one of {sorted(PROFILES)}, got {profile!r}")
    ring_km = float(data.get("ring_km", 100.0))
    if not 0.0 <= ring_km <= 1000.0:
        raise ConfigError(f"ring_km must be in [0, 1000], got {ring_km}")
    month = _int(data, "bmng_month", 7, 1, 12)
    quality = _int(data, "jpeg_quality", 85, 1, 95)
    seed = _int(data, "seed", 0, 0, 2**31 - 1)
    prio = {k: list(v) for k, v in PROFILES[profile]["priorities"].items()}
    for layer, ids in data.get("priorities", {}).items():
        if layer not in LAYERS:
            raise ConfigError(f"priorities.{layer}: unknown layer (one of {LAYERS})")
        if not isinstance(ids, list) or not all(isinstance(i, str) for i in ids):
            raise ConfigError(f"priorities.{layer} must be a list of source ids")
        prio[layer] = list(ids)
    options = {k: dict(v) for k, v in DEFAULT_SOURCE_OPTIONS.items()}
    for sid, opts in data.get("sources", {}).items():
        if not isinstance(opts, dict):
            raise ConfigError(f"sources.{sid} must be a table")
        options.setdefault(sid, {}).update(opts)
    options.setdefault("bmng", {})["month"] = month
    zoom = {
        "globe": dict(GLOBE_ZOOM),
        "ring": dict(RING_ZOOM),
        "bbox": dict(PROFILES[profile]["bbox_zoom"]),
    }
    for region, z in data.get("zoom", {}).items():
        if region not in zoom or not isinstance(z, dict):
            raise ConfigError(f"zoom.{region}: unknown region (one of {sorted(zoom)})")
        for layer, v in z.items():
            if layer not in TILED_LAYERS or not isinstance(v, int) or not 0 <= v <= 22:
                raise ConfigError(f"zoom.{region}.{layer} must be an integer in [0, 22]")
            zoom[region][layer] = v
    allow = data.get("allow", [])
    if not isinstance(allow, list) or not all(isinstance(a, str) for a in allow):
        raise ConfigError("allow must be a list of licence ids")
    return ScenePlan(name, bbox, profile, seed, ring_km, month, quality, prio, options, zoom, tuple(allow))


def load_scene(path: Path) -> ScenePlan:
    try:
        data = tomllib.loads(Path(path).read_text(encoding="utf-8"))
    except tomllib.TOMLDecodeError as e:
        raise ConfigError(f"{path}: {e}") from e
    return parse_scene(data)


def layer_settings(plan: ScenePlan) -> dict:
    """The manifest's `layers` section: everything per layer that tiles depend on besides regions and data."""
    return {
        "terrain": {
            "priorities": plan.priorities["terrain"], "grid": TERRAIN_GRID, "feather_m": FEATHER_M,
            "max_error": "max(0.1, 0.25 * 77067 / 2^z) m", "format": "quantized-mesh-1.0",
            "extensions": ["octvertexnormals"], "gzip_level": 9,
        },
        "imagery": {
            "priorities": plan.priorities["imagery"], "tile_px": TILE_PX, "format": "jpeg",
            "jpeg_quality": plan.jpeg_quality, "subsampling": "4:2:0",
        },
        "landcover": {"priorities": plan.priorities["landcover"], "bounds": list(plan.ring_bounds()), "tile_deg": 0.05},
    }


def default_scene_toml(name: str, bbox: Bounds, profile: str = "sim") -> str:
    w, s, e, n = validate_bbox(bbox)
    return (
        f"# camsim-scene scene file (docs/scene-packages.md)\n"
        f'name = "{name}"\n'
        f"bbox = [{w!r}, {s!r}, {e!r}, {n!r}]  # W S E N\n"
        f'profile = "{profile}"\n'
        f"seed = 0\n"
        f"ring_km = 100\n"
        f"bmng_month = 7\n"
        f"jpeg_quality = 85\n"
    )
```

- [ ] **Step 4: Run the tests**

Run: `cd scripts/scene && uv run pytest tests/test_config.py -v`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add scripts/scene/camsim_scene/config.py scripts/scene/tests/test_config.py
git commit -m "feat(scene): scene.toml parsing, profiles and regions"
```

---

### Task 4: Manifest schema, canonical JSON, `hashes.txt`

**Files:**
- Create: `scripts/scene/camsim_scene/manifest.py`
- Test: `scripts/scene/tests/test_manifest.py`

**Interfaces:**
- Consumes: `fsutil.atomic_write`, `fsutil.sha256_bytes`, `fsutil.sha256_file`; `tiling.Region`.
- Produces: `SCHEMA_VERSION = 1`; `STATE_DIR = ".state"`; `NOT_HASHED = ("manifest.json", "build.json", "hashes.txt")`; `canonical_json(obj) -> str`; `AssetRecord(id, url, sha256=None, size=None, group="", rank=0, role="data", metadata={})`; `SourceRecord(id, adapter, dataset, version, licence, attribution, options, assets)`; `Manifest(name, bbox, seed, regions, tiling, layers, datum, sources, licence_allow, tool, schema_version=1, hashes_sha256=None)` with `to_dict()`, `from_dict(d)`, `dumps()`, `load(path)`, `write(path)`, `source(id)`, `region_objs()`, `is_fetched()`, `layer_settings_hash(layer)`; `package_files(pkg) -> Iterator[str]` (sorted, excludes `NOT_HASHED` at the top level and `.state/`); `write_hashes(pkg, known=None) -> str` (returns sha256 of `hashes.txt`); `read_hashes(pkg) -> dict[str, str]`.

- [ ] **Step 1: Write the failing tests**

`scripts/scene/tests/test_manifest.py`:

```python
import json

from camsim_scene import fsutil
from camsim_scene.manifest import AssetRecord, Manifest, SourceRecord, canonical_json, package_files, read_hashes, write_hashes


def sample_manifest() -> Manifest:
    src = SourceRecord(
        id="dep3_13",
        adapter="dep3_13",
        dataset="USGS 3DEP 1/3 arc-second DEM",
        version="20260915",
        licence="LicenseRef-PublicDomain-USGov",
        attribution="U.S. Geological Survey",
        options={},
        assets=[AssetRecord(id="USGS_13_n34w118_20260915", url="https://x/y.tif", sha256="ab" * 32, size=10, group="dep3_13")],
    )
    return Manifest(
        name="t",
        bbox=[0.0, 0.0, 1.0, 1.0],
        seed=0,
        regions=[],
        tiling={"scheme": "tms"},
        layers={"terrain": {"grid": 257}, "imagery": {"jpeg_quality": 85}, "landcover": {}},
        datum={"grids": {}, "datums": {}},
        sources=[src],
        licence_allow=["CC-BY-4.0", "LicenseRef-PublicDomain-USGov"],
        tool={"name": "camsim-scene", "version": "0.1.0"},
    )


def test_canonical_json_is_key_order_independent():
    assert canonical_json({"b": 1, "a": [1.5, {"d": 0.1, "c": None}]}) == canonical_json({"a": [1.5, {"c": None, "d": 0.1}], "b": 1})


def test_manifest_round_trip_and_fetched(tmp_path):
    m = sample_manifest()
    m.write(tmp_path / "manifest.json")
    back = Manifest.load(tmp_path / "manifest.json")
    assert back.to_dict() == m.to_dict() and back.dumps() == m.dumps()
    assert back.source("dep3_13").assets[0].group == "dep3_13"
    assert back.is_fetched()
    back.sources[0].assets[0].sha256 = None
    assert not back.is_fetched()
    assert json.loads(m.dumps())["schema_version"] == 1


def test_settings_hash_changes_only_for_the_changed_layer():
    a, b = sample_manifest(), sample_manifest()
    b.layers["imagery"]["jpeg_quality"] = 90
    assert a.layer_settings_hash("terrain") == b.layer_settings_hash("terrain")
    assert a.layer_settings_hash("imagery") != b.layer_settings_hash("imagery")
    b.hashes_sha256 = "ff" * 32  # output, not an input
    assert a.layer_settings_hash("terrain") == b.layer_settings_hash("terrain")


def test_hashes_txt_is_sorted_and_excludes_state_and_meta(tmp_path):
    for rel in ["terrain/1/0/0.terrain", "terrain/10/0/0.terrain", "terrain/1.json", "ATTRIBUTION.txt",
                "manifest.json", "build.json", ".state/terrain/1/0/0", "imagery/0/0/0.jpg"]:
        fsutil.atomic_write(tmp_path / rel, rel.encode())
    files = list(package_files(tmp_path))
    assert files == sorted(files)
    assert files == ["ATTRIBUTION.txt", "imagery/0/0/0.jpg", "terrain/1.json", "terrain/1/0/0.terrain", "terrain/10/0/0.terrain"]
    digest = write_hashes(tmp_path, known=lambda rel: "00" * 32 if rel.endswith(".terrain") else None)
    assert digest == fsutil.sha256_file(tmp_path / "hashes.txt")
    h = read_hashes(tmp_path)
    assert h["terrain/1/0/0.terrain"] == "00" * 32
    assert h["ATTRIBUTION.txt"] == fsutil.sha256_bytes(b"ATTRIBUTION.txt")
    assert "manifest.json" not in h and "hashes.txt" not in h
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd scripts/scene && uv run pytest tests/test_manifest.py -v`
Expected: FAIL, `ImportError`.

- [ ] **Step 3: Implement `manifest.py`**

```python
"""manifest.json: the deterministic description of a package (inputs, settings, datum, sources, hashes).
Nothing in it depends on time or host; build-time facts go to build.json."""

from __future__ import annotations

import json
import os
from collections.abc import Callable, Iterator
from dataclasses import asdict, dataclass, field
from pathlib import Path

from .fsutil import atomic_write, sha256_bytes, sha256_file
from .tiling import Region

SCHEMA_VERSION = 1
STATE_DIR = ".state"
NOT_HASHED = ("manifest.json", "build.json", "hashes.txt")


def canonical_json(obj) -> str:
    """Sorted keys, 2-space indent, Python's shortest round-trip float repr, no NaN."""
    return json.dumps(obj, sort_keys=True, indent=2, ensure_ascii=False, allow_nan=False) + "\n"


@dataclass
class AssetRecord:
    id: str
    url: str  # never signed
    sha256: str | None = None
    size: int | None = None
    group: str = ""  # assets of one group are merged first-valid-wins; groups are feathered against each other
    rank: int = 0  # order inside the source (0 = preferred)
    role: str = "data"  # "data" or an auxiliary role ("geoid")
    metadata: dict = field(default_factory=dict)


@dataclass
class SourceRecord:
    id: str
    adapter: str
    dataset: str
    version: str
    licence: str
    attribution: str
    options: dict
    assets: list[AssetRecord]


@dataclass
class Manifest:
    name: str
    bbox: list[float]
    seed: int
    regions: list[dict]
    tiling: dict
    layers: dict
    datum: dict
    sources: list[SourceRecord]
    licence_allow: list[str]
    tool: dict
    schema_version: int = SCHEMA_VERSION
    hashes_sha256: str | None = None

    def to_dict(self) -> dict:
        return asdict(self)

    @classmethod
    def from_dict(cls, d: dict) -> Manifest:
        if d.get("schema_version") != SCHEMA_VERSION:
            raise ValueError(f"manifest schema_version {d.get('schema_version')} != {SCHEMA_VERSION}")
        d = dict(d)
        d["sources"] = [
            SourceRecord(**{**s, "assets": [AssetRecord(**a) for a in s["assets"]]}) for s in d["sources"]
        ]
        return cls(**d)

    def dumps(self) -> str:
        return canonical_json(self.to_dict())

    @classmethod
    def load(cls, path: Path) -> Manifest:
        return cls.from_dict(json.loads(Path(path).read_text(encoding="utf-8")))

    def write(self, path: Path) -> None:
        atomic_write(Path(path), self.dumps().encode("utf-8"))

    def source(self, source_id: str) -> SourceRecord:
        for s in self.sources:
            if s.id == source_id:
                return s
        raise KeyError(f"no source {source_id!r} in the manifest")

    def region_objs(self) -> list[Region]:
        return [Region.from_dict(r) for r in self.regions]

    def is_fetched(self) -> bool:
        assets_ok = all(a.sha256 for s in self.sources for a in s.assets)
        grids_ok = all(g.get("sha256") for g in self.datum.get("grids", {}).values())
        return assets_ok and grids_ok

    def layer_settings_hash(self, layer: str) -> str:
        """Everything (except asset bytes) that a tile of `layer` depends on."""
        return sha256_bytes(
            canonical_json(
                {"layer": self.layers[layer], "regions": self.regions, "tiling": self.tiling, "datum": self.datum, "tool": self.tool}
            ).encode()
        )


def _walk(root: Path, rel: str) -> Iterator[str]:
    # Sorting entries by name + "/" for directories yields global lexicographic order of full paths.
    entries = sorted(os.scandir(root / rel if rel else root), key=lambda e: e.name + ("/" if e.is_dir() else ""))
    for e in entries:
        path = f"{rel}/{e.name}" if rel else e.name
        if e.is_dir():
            if not rel and e.name == STATE_DIR:
                continue
            yield from _walk(root, path)
        elif not (not rel and e.name in NOT_HASHED):
            yield path


def package_files(pkg: Path) -> Iterator[str]:
    """Package-relative POSIX paths of every hashed file, in sorted order (streaming; no full list in memory)."""
    yield from _walk(Path(pkg), "")


def write_hashes(pkg: Path, known: Callable[[str], str | None] | None = None) -> str:
    """Write hashes.txt ("sha256  path" per line); `known` may supply already-verified hashes (tile markers)."""
    pkg = Path(pkg)
    lines = []
    for rel in package_files(pkg):
        h = known(rel) if known else None
        lines.append(f"{h or sha256_file(pkg / rel)}  {rel}\n")
    data = "".join(lines).encode("utf-8")
    atomic_write(pkg / "hashes.txt", data)
    return sha256_bytes(data)


def read_hashes(pkg: Path) -> dict[str, str]:
    out = {}
    for line in (Path(pkg) / "hashes.txt").read_text(encoding="utf-8").splitlines():
        h, rel = line.split("  ", 1)
        out[rel] = h
    return out
```

- [ ] **Step 4: Run the tests**

Run: `cd scripts/scene && uv run pytest tests/test_manifest.py -v`
Expected: 4 passed.

- [ ] **Step 5: Commit**

```bash
git add scripts/scene/camsim_scene/manifest.py scripts/scene/tests/test_manifest.py
git commit -m "feat(scene): manifest schema, canonical JSON, hashes.txt"
```

---
### Task 5: Quantized-mesh writer and decoder

**Files:**
- Create: `scripts/scene/camsim_scene/qmesh.py`
- Test: `scripts/scene/tests/test_qmesh.py`

**Interfaces:**
- Consumes: `fsutil.atomic_write`.
- Produces: `QMAX = 32767`; `ecef(lon, lat, h) -> np.ndarray (n, 3)`; `geodetic_up(lon, lat) -> (n, 3)`; `oct_encode(n) -> uint8 (k, 2)`, `oct_decode(e) -> (k, 3)`; `renumber(tris) -> (order, tris)`; `hwm_encode(flat) -> codes`, `hwm_decode(codes) -> flat`; `encode(lon, lat, height, triangles, bounds) -> bytes` (uncompressed tile); `gzip_tile(raw) -> bytes`; `write_tile(path, raw)`; `QMesh` (fields `center, min_height, max_height, sphere_center, sphere_radius, horizon_occlusion, u, v, h, triangles, west, south, east, north, extensions`; methods `heights()`, `lonlat(bounds)`, `normals()`); `decode(data) -> QMesh` (accepts gzip or raw); `layer_json(name, available, attribution) -> dict`.

Format reference: https://github.com/CesiumGS/quantized-mesh. Pitfalls from the spike that these tests pin: float64 quantisation with rounding (not float32 truncation), edge vertices exactly 0/32767, high-water-mark vertex order, finite normals at the poles, CCW triangles.

- [ ] **Step 1: Write the failing tests**

`scripts/scene/tests/test_qmesh.py`:

```python
import gzip
import io

import numpy as np
import pytest

from camsim_scene import qmesh
from camsim_scene.qmesh import QMAX


def grid_mesh(bounds, n=9, hfun=lambda lon, lat: 100.0 + 1000.0 * (lon - lon.min()) - 300.0 * (lat - lat.min())):
    """Regular n x n vertex grid in two CCW triangles per cell (x east, y north); triangle order scrambled."""
    w, s, e, nn = bounds
    c, r = np.meshgrid(np.arange(n), np.arange(n))
    lon = w + c.ravel() * (e - w) / (n - 1)
    lat = s + r.ravel() * (nn - s) / (n - 1)
    tris = []
    for rr in range(n - 1):
        for cc in range(n - 1):
            a, b, d, f = rr * n + cc, rr * n + cc + 1, (rr + 1) * n + cc + 1, (rr + 1) * n + cc
            tris += [(a, b, d), (a, d, f)]
    tris = np.array(tris)[np.random.default_rng(1).permutation(len(tris))]
    return lon, lat, hfun(lon, lat), tris


B16 = (-117.388916015625, 33.2171630859375, -117.38616943359375, 33.21990966796875)  # a z16 tile


def test_round_trip_positions_heights_and_triangles():
    lon, lat, h, tris = grid_mesh(B16)
    q = qmesh.decode(qmesh.encode(lon, lat, h, tris, B16))
    order, t2 = qmesh.renumber(tris)
    assert np.array_equal(q.triangles, t2)
    dlon, dlat = q.lonlat(B16)
    assert np.abs(dlon - lon[order]).max() <= 0.5 * (B16[2] - B16[0]) / QMAX + 1e-12
    assert np.abs(dlat - lat[order]).max() <= 0.5 * (B16[3] - B16[1]) / QMAX + 1e-12
    assert np.abs(q.heights() - h[order]).max() <= 0.5 * (h.max() - h.min()) / QMAX + 1e-4
    assert q.min_height <= h.min() and q.max_height >= h.max()


def test_high_water_mark_codes_round_trip_and_are_valid():
    _, t2 = qmesh.renumber(grid_mesh(B16)[3])
    flat = t2.ravel()
    codes = qmesh.hwm_encode(flat)
    assert np.array_equal(qmesh.hwm_decode(codes), flat)
    running = np.maximum.accumulate(np.concatenate([[-1], flat[:-1]]))
    assert np.all(flat <= running + 1)  # every index is old or exactly the next new one


def test_edges_are_complete_exact_and_sorted():
    lon, lat, h, tris = grid_mesh(B16, n=9)
    q = qmesh.decode(qmesh.encode(lon, lat, h, tris, B16))
    for idx, coord, other, value in ((q.west, q.u, q.v, 0), (q.east, q.u, q.v, QMAX), (q.south, q.v, q.u, 0), (q.north, q.v, q.u, QMAX)):
        assert len(idx) == 9
        assert np.all(coord[idx] == value)
        assert np.all(np.diff(other[idx].astype(int)) > 0)
    assert (q.u == 0).sum() == 9 and (q.u == QMAX).sum() == 9


def test_triangles_are_counter_clockwise():
    lon, lat, h, tris = grid_mesh(B16)
    q = qmesh.decode(qmesh.encode(lon, lat, h, tris, B16))
    u, v = q.u.astype(float), q.v.astype(float)
    a, b, c = q.triangles.T
    area2 = (u[b] - u[a]) * (v[c] - v[a]) - (u[c] - u[a]) * (v[b] - v[a])
    assert np.all(area2 > 0)


def test_normals_finite_unit_and_outward_even_at_the_poles():
    b = (-180.0, -90.0, 0.0, 90.0)  # z0 west tile: rows at lat +-90 make degenerate triangles
    lon, lat, h, tris = grid_mesh(b, n=5, hfun=lambda lon, lat: np.zeros_like(lon))
    q = qmesh.decode(qmesh.encode(lon, lat, h, tris, b))
    n = q.normals()
    assert np.all(np.isfinite(n)) and np.allclose(np.linalg.norm(n, axis=1), 1.0, atol=1e-6)
    dlon, dlat = q.lonlat(b)
    assert np.all((n * qmesh.geodetic_up(dlon, dlat)).sum(1) > 0.9)
    assert np.all(np.isfinite(q.horizon_occlusion)) and np.isfinite(q.sphere_radius)


def test_bytes_are_identical_across_runs_and_gzip_has_no_timestamp():
    lon, lat, h, tris = grid_mesh(B16)
    a = qmesh.gzip_tile(qmesh.encode(lon, lat, h, tris, B16))
    b = qmesh.gzip_tile(qmesh.encode(lon, lat, h, tris, B16))
    assert a == b and a[4:8] == b"\0\0\0\0"
    assert qmesh.decode(a).triangles.shape == qmesh.decode(gzip.decompress(a)).triangles.shape


def test_more_than_65536_vertices_uses_32_bit_indices():
    lon, lat, h, tris = grid_mesh(B16, n=258)
    raw = qmesh.encode(lon, lat, h, tris, B16)
    q = qmesh.decode(raw)
    assert len(q.u) == 258 * 258 and q.triangles.max() == 258 * 258 - 1
    assert len(q.west) == 258


def test_decoder_reads_an_independent_encoder():
    qme = pytest.importorskip("quantized_mesh_encoder")
    b = (0.0, 0.0, 1.0, 1.0)
    lon, lat, h, tris = grid_mesh(b, n=5)
    order, t2 = qmesh.renumber(tris)  # quantized-mesh-encoder doesn't reorder
    pos = np.stack([lon[order], lat[order], h[order]], 1).astype(np.float32)
    buf = io.BytesIO()
    qme.encode(buf, pos, t2.astype(np.uint32), bounds=b)
    q = qmesh.decode(buf.getvalue())
    assert np.array_equal(q.triangles, t2)
    assert np.abs(q.u.astype(float) - lon[order] * QMAX).max() <= 1.0  # it truncates; we round
    assert np.abs(q.v.astype(float) - lat[order] * QMAX).max() <= 1.0
    assert np.abs(q.heights() - h[order]).max() <= (h.max() - h.min()) / QMAX * 1.01 + 1e-3
    assert len(q.west) == len(q.east) == len(q.south) == len(q.north) == 5


def test_layer_json_shape():
    lj = qmesh.layer_json("pendleton", [[{"startX": 0, "startY": 0, "endX": 1, "endY": 0}]], "USGS")
    assert lj["format"] == "quantized-mesh-1.0" and lj["scheme"] == "tms" and lj["projection"] == "EPSG:4326"
    assert lj["extensions"] == ["octvertexnormals"] and lj["maxzoom"] == 0 and lj["tiles"] == ["{z}/{x}/{y}.terrain"]
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd scripts/scene && uv run pytest tests/test_qmesh.py -v`
Expected: FAIL, `ImportError`.

- [ ] **Step 3: Implement `qmesh.py`**

```python
"""Quantized-mesh-1.0 terrain tiles (https://github.com/CesiumGS/quantized-mesh): writer and decoder.

Our own encoder, not quantized-mesh-encoder: that one casts positions to float32 (~0.7 m at lon -117) and
truncates to int16, so edge vertices miss 0/32767 (no skirts), and it assumes high-water-mark vertex order
without reordering (docs/realism-r0-spike.md, "Pitfalls")."""

from __future__ import annotations

import gzip
import struct
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from .fsutil import atomic_write

QMAX = 32767
A = 6378137.0
B = 6356752.314245179
E2 = 1.0 - (B * B) / (A * A)
HEADER = struct.Struct("<3d2f4d3d")  # centre, min/max height, bounding sphere, horizon occlusion point (88 bytes)
EXT_OCT_NORMALS = 1


def ecef(lon, lat, h) -> np.ndarray:
    lo, la = np.radians(np.asarray(lon, np.float64)), np.radians(np.asarray(lat, np.float64))
    h = np.asarray(h, np.float64)
    n = A / np.sqrt(1.0 - E2 * np.sin(la) ** 2)
    return np.stack(
        [(n + h) * np.cos(la) * np.cos(lo), (n + h) * np.cos(la) * np.sin(lo), (n * (1.0 - E2) + h) * np.sin(la)], -1
    )


def geodetic_up(lon, lat) -> np.ndarray:
    lo, la = np.radians(np.asarray(lon, np.float64)), np.radians(np.asarray(lat, np.float64))
    return np.stack([np.cos(la) * np.cos(lo), np.cos(la) * np.sin(lo), np.sin(la)], -1)


def _sign_not_zero(v):
    return np.where(v < 0.0, -1.0, 1.0)


def oct_encode(n: np.ndarray) -> np.ndarray:
    """Cesium's 2 x 8-bit octahedral encoding."""
    p = n / np.abs(n).sum(1, keepdims=True)
    x, y, z = p[:, 0], p[:, 1], p[:, 2]
    neg = z < 0.0
    x2 = np.where(neg, (1.0 - np.abs(y)) * _sign_not_zero(x), x)
    y2 = np.where(neg, (1.0 - np.abs(x)) * _sign_not_zero(y), y)
    q = np.rint((np.clip(np.stack([x2, y2], 1), -1.0, 1.0) * 0.5 + 0.5) * 255.0)
    return q.astype(np.uint8)


def oct_decode(e: np.ndarray) -> np.ndarray:
    xy = e.astype(np.float64) / 255.0 * 2.0 - 1.0
    x, y = xy[:, 0], xy[:, 1]
    z = 1.0 - np.abs(x) - np.abs(y)
    neg = z < 0.0
    x2 = np.where(neg, (1.0 - np.abs(y)) * _sign_not_zero(x), x)
    y2 = np.where(neg, (1.0 - np.abs(x)) * _sign_not_zero(y), y)
    v = np.stack([x2, y2, z], 1)
    return v / np.linalg.norm(v, axis=1, keepdims=True)


def renumber(tris: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Vertex order by first use in the index list (high-water-mark order); unused vertices are dropped."""
    tris = np.asarray(tris, np.int64).reshape(-1, 3)
    flat = tris.ravel()
    _, first = np.unique(flat, return_index=True)
    order = flat[np.sort(first)]
    remap = np.full(int(flat.max()) + 1, -1, np.int64)
    remap[order] = np.arange(len(order))
    return order, remap[tris]


def hwm_encode(flat: np.ndarray) -> np.ndarray:
    flat = np.asarray(flat, np.int64)
    highest = np.concatenate([[0], np.maximum.accumulate(flat)[:-1] + 1])
    codes = highest - flat
    if np.any(codes < 0):
        raise ValueError("indices are not in high-water-mark order (renumber first)")
    return codes


def hwm_decode(codes: np.ndarray) -> np.ndarray:
    codes = np.asarray(codes, np.int64)
    zeros_before = np.concatenate([[0], np.cumsum(codes == 0)[:-1]])
    return zeros_before - codes


def _zigzag(d: np.ndarray) -> np.ndarray:
    d = d.astype(np.int32)
    return ((d << 1) ^ (d >> 31)).astype(np.uint16)


def _unzigzag(a: np.ndarray) -> np.ndarray:
    a = a.astype(np.int32)
    return (a >> 1) ^ -(a & 1)


def _quantize(v: np.ndarray, lo: float, hi: float) -> np.ndarray:
    q = np.rint(np.clip((v - lo) / (hi - lo), 0.0, 1.0) * QMAX)
    tol = abs(hi - lo) * 1e-12
    q[np.abs(v - lo) <= tol] = 0
    q[np.abs(v - hi) <= tol] = QMAX
    return q.astype(np.int32)


def _f32_down(x: float) -> float:
    f = np.float32(x)
    return float(f if f <= x else np.nextafter(f, np.float32(-np.inf)))


def _f32_up(x: float) -> float:
    f = np.float32(x)
    return float(f if f >= x else np.nextafter(f, np.float32(np.inf)))


def horizon_occlusion_point(points: np.ndarray, center: np.ndarray) -> np.ndarray:
    """Cesium's EllipsoidalOccluder.computeHorizonCullingPointFromPoints, in ellipsoid-scaled space. A tile that
    reaches past the horizon of its own centre (z <= 1) gets the surface point under the centre."""
    scale = np.array([1.0 / A, 1.0 / A, 1.0 / B])
    d = center * scale
    d = d / np.linalg.norm(d)
    p = points * scale
    m = np.linalg.norm(p, axis=1)
    direction = p / m[:, None]
    m2 = np.maximum(1.0, m * m)
    m = np.maximum(1.0, m)
    cos_a = direction @ d
    sin_a = np.linalg.norm(np.cross(direction, d), axis=1)
    cos_b = 1.0 / m
    sin_b = np.sqrt(m2 - 1.0) * cos_b
    denom = cos_a * cos_b - sin_a * sin_b
    if np.any(denom <= 0.0):
        return d
    return d * float((1.0 / denom).max())


def vertex_normals(pts: np.ndarray, tris: np.ndarray, lon: np.ndarray, lat: np.ndarray) -> np.ndarray:
    """Area-weighted face normals in ECEF; degenerate or non-finite -> the ellipsoid normal."""
    a, b, c = pts[tris[:, 0]], pts[tris[:, 1]], pts[tris[:, 2]]
    fn = np.cross(b - a, c - a)  # |fn| = 2 x area
    acc = np.zeros_like(pts)
    for k in range(3):
        np.add.at(acc, tris[:, k], fn)
    norm = np.linalg.norm(acc, axis=1)
    good = np.isfinite(norm) & (norm > 0.0)
    out = geodetic_up(lon, lat)
    out[good] = acc[good] / norm[good, None]
    return out


def encode(lon, lat, height, triangles, bounds) -> bytes:
    """One uncompressed quantized-mesh tile. Triangles must be CCW in (lon, lat); vertices are renumbered."""
    lon, lat, height = (np.asarray(a, np.float64) for a in (lon, lat, height))
    order, tris = renumber(triangles)
    lon, lat, height = lon[order], lat[order], height[order]
    w, s, e, n = bounds
    u, v = _quantize(lon, w, e), _quantize(lat, s, n)
    hmin, hmax = _f32_down(float(height.min())), _f32_up(float(height.max()))
    span = hmax - hmin
    hq = np.rint(np.clip((height - hmin) / span, 0.0, 1.0) * QMAX).astype(np.int32) if span > 0 else np.zeros_like(u)
    pts = ecef(lon, lat, height)
    center = (pts.min(0) + pts.max(0)) / 2.0
    radius = float(np.linalg.norm(pts - center, axis=1).max())
    hop = horizon_occlusion_point(pts, center)
    out = bytearray(HEADER.pack(*center, hmin, hmax, *center, radius, *hop))
    nv = len(u)
    out += struct.pack("<I", nv)
    for q in (u, v, hq):
        out += _zigzag(np.diff(q, prepend=0)).astype("<u2").tobytes()
    itype, align = ("<u4", 4) if nv > 65536 else ("<u2", 2)
    out += b"\0" * (-len(out) % align)
    out += struct.pack("<I", len(tris)) + hwm_encode(tris.ravel()).astype(itype).tobytes()
    for mask, along in ((u == 0, v), (v == 0, u), (u == QMAX, v), (v == QMAX, u)):
        idx = np.nonzero(mask)[0]
        idx = idx[np.argsort(along[idx], kind="stable")]
        out += struct.pack("<I", len(idx)) + idx.astype(itype).tobytes()
    enc = oct_encode(vertex_normals(pts, tris, lon, lat)).tobytes()
    out += struct.pack("<BI", EXT_OCT_NORMALS, len(enc)) + enc
    return bytes(out)


def gzip_tile(raw: bytes) -> bytes:
    return gzip.compress(raw, compresslevel=9, mtime=0)


def write_tile(path: Path, raw: bytes) -> None:
    atomic_write(path, gzip_tile(raw))


@dataclass
class QMesh:
    center: tuple
    min_height: float
    max_height: float
    sphere_center: tuple
    sphere_radius: float
    horizon_occlusion: tuple
    u: np.ndarray
    v: np.ndarray
    h: np.ndarray
    triangles: np.ndarray
    west: np.ndarray
    south: np.ndarray
    east: np.ndarray
    north: np.ndarray
    extensions: dict

    def heights(self) -> np.ndarray:
        return self.min_height + self.h.astype(np.float64) / QMAX * (self.max_height - self.min_height)

    def lonlat(self, bounds) -> tuple[np.ndarray, np.ndarray]:
        w, s, e, n = bounds
        return w + self.u / QMAX * (e - w), s + self.v / QMAX * (n - s)

    def normals(self) -> np.ndarray | None:
        data = self.extensions.get(EXT_OCT_NORMALS)
        return None if data is None else oct_decode(np.frombuffer(data, np.uint8).reshape(-1, 2))


def decode(data: bytes) -> QMesh:
    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    hd = HEADER.unpack_from(data, 0)
    off = HEADER.size
    nv = struct.unpack_from("<I", data, off)[0]
    off += 4
    arrs = []
    for _ in range(3):
        a = np.frombuffer(data, "<u2", nv, off)
        off += 2 * nv
        arrs.append(np.cumsum(_unzigzag(a)).astype(np.uint16))
    itype, size = ("<u4", 4) if nv > 65536 else ("<u2", 2)
    off += -off % size
    nt = struct.unpack_from("<I", data, off)[0]
    off += 4
    tris = hwm_decode(np.frombuffer(data, itype, nt * 3, off)).reshape(-1, 3)
    off += size * nt * 3
    edges = []
    for _ in range(4):
        k = struct.unpack_from("<I", data, off)[0]
        off += 4
        edges.append(np.frombuffer(data, itype, k, off).astype(np.int64))
        off += size * k
    ext = {}
    while off < len(data):
        eid, ln = struct.unpack_from("<BI", data, off)
        off += 5
        ext[eid] = bytes(data[off : off + ln])
        off += ln
    return QMesh(hd[0:3], hd[3], hd[4], hd[5:8], hd[8], hd[9:12], *arrs, tris, *edges, ext)


def layer_json(name: str, available: list[list[dict]], attribution: str) -> dict:
    return {
        "tilejson": "2.1.0",
        "name": name,
        "version": "1.0.0",
        "format": "quantized-mesh-1.0",
        "scheme": "tms",
        "tiles": ["{z}/{x}/{y}.terrain"],
        "projection": "EPSG:4326",
        "bounds": [-180.0, -90.0, 180.0, 90.0],
        "minzoom": 0,
        "maxzoom": len(available) - 1,
        "extensions": ["octvertexnormals"],
        "available": available,
        "attribution": attribution,
    }
```

- [ ] **Step 4: Run the tests**

Run: `cd scripts/scene && uv run pytest tests/test_qmesh.py -v`
Expected: 9 passed. If `test_decoder_reads_an_independent_encoder` fails on the call signature, check `help(quantized_mesh_encoder.encode)` (0.5.0 takes `(f, positions, indices, bounds=..., sphere_method=..., ellipsoid=..., extensions=...)`) and adjust the test call only.

- [ ] **Step 5: Commit**

```bash
git add scripts/scene/camsim_scene/qmesh.py scripts/scene/tests/test_qmesh.py
git commit -m "feat(scene): quantized-mesh-1.0 encoder/decoder (float64 rounding, HWM order, oct normals)"
```

---

### Task 6: Licence register and attribution

**Files:**
- Create: `scripts/scene/camsim_scene/licences.py`, `scripts/scene/camsim_scene/licences.toml`
- Test: `scripts/scene/tests/test_licences.py`

**Interfaces:**
- Consumes: `manifest.Manifest` (duck-typed: `.name`, `.sources[].{id, dataset, version, attribution, licence}`).
- Produces: `DEFAULT_ALLOW = ("LicenseRef-PublicDomain-USGov", "CC-BY-4.0")`; `Licence(id, name, url, redistributable)`; `LicenceError(Exception)`; `load_register() -> dict[str, Licence]`; `check_allowed(licences: Mapping[str, str], allow: Iterable[str]) -> None`; `attribution_text(manifest) -> str`.

- [ ] **Step 1: Write the failing tests**

`scripts/scene/tests/test_licences.py`:

```python
from types import SimpleNamespace

import pytest

from camsim_scene import licences
from camsim_scene.licences import DEFAULT_ALLOW, LicenceError, check_allowed


def test_default_allow_list_accepts_public_domain_and_cc_by():
    check_allowed({"dep3_13": "LicenseRef-PublicDomain-USGov", "worldcover": "CC-BY-4.0"}, DEFAULT_ALLOW)


def test_source_outside_the_allow_list_is_refused_with_the_flag_to_use():
    with pytest.raises(LicenceError, match=r"osm: licence 'ODbL-1.0'.*--allow ODbL-1.0"):
        check_allowed({"osm": "ODbL-1.0"}, DEFAULT_ALLOW)
    check_allowed({"osm": "ODbL-1.0"}, (*DEFAULT_ALLOW, "ODbL-1.0"))


def test_unknown_licence_id_is_refused():
    with pytest.raises(LicenceError, match="unknown licence"):
        check_allowed({"x": "beerware"}, ("beerware",))


def test_attribution_text_is_sorted_and_complete():
    src = lambda i, lic: SimpleNamespace(id=i, dataset=f"DS {i}", version="1", attribution=f"credit {i}", licence=lic)  # noqa: E731
    m = SimpleNamespace(name="pendleton", sources=[src("wc", "CC-BY-4.0"), src("ab", "LicenseRef-PublicDomain-USGov")])
    text = licences.attribution_text(m)
    assert text.index("DS ab") < text.index("DS wc")
    assert "credit wc" in text and "https://creativecommons.org/licenses/by/4.0/" in text
    assert text == licences.attribution_text(m)
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd scripts/scene && uv run pytest tests/test_licences.py -v`
Expected: FAIL, `ImportError`.

- [ ] **Step 3: Implement**

`scripts/scene/camsim_scene/licences.toml`:

```toml
# Licence register for scene package sources (REALISM R0). Keys are SPDX license identifiers
# (https://spdx.org/licenses/); licences SPDX doesn't list use its LicenseRef- prefix (US Government works have no
# SPDX id). A build accepts only the licences on its allow-list (default: LicenseRef-PublicDomain-USGov, CC-BY-4.0;
# add more with --allow <id> after checking the terms). `redistributable` is informational.

[LicenseRef-PublicDomain-USGov]
name = "Public domain (US Government work)"
url = ""
redistributable = true

["CC-BY-4.0"]
name = "Creative Commons Attribution 4.0 International"
url = "https://creativecommons.org/licenses/by/4.0/"
redistributable = true

["ODbL-1.0"]
name = "Open Database License 1.0 (share-alike)"
url = "https://opendatacommons.org/licenses/odbl/1-0/"
redistributable = true

["CDLA-Permissive-2.0"]
name = "Community Data License Agreement Permissive 2.0"
url = "https://cdla.dev/permissive-2-0/"
redistributable = true

["CC-BY-NC-SA-4.0"]
name = "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 (non-commercial)"
url = "https://creativecommons.org/licenses/by-nc-sa/4.0/"
redistributable = false

[LicenseRef-Proprietary]
name = "Proprietary (not redistributable)"
url = ""
redistributable = false
```

`scripts/scene/camsim_scene/licences.py`:

```python
"""Licence register (licences.toml), allow-list check, and the ATTRIBUTION.txt text."""

from __future__ import annotations

import tomllib
from collections.abc import Iterable, Mapping
from dataclasses import dataclass
from importlib import resources

DEFAULT_ALLOW = ("LicenseRef-PublicDomain-USGov", "CC-BY-4.0")


@dataclass(frozen=True)
class Licence:
    id: str
    name: str
    url: str
    redistributable: bool


class LicenceError(Exception):
    pass


def load_register() -> dict[str, Licence]:
    text = resources.files("camsim_scene").joinpath("licences.toml").read_text(encoding="utf-8")
    return {k: Licence(k, v["name"], v.get("url", ""), bool(v["redistributable"])) for k, v in tomllib.loads(text).items()}


def check_allowed(licences: Mapping[str, str], allow: Iterable[str]) -> None:
    """`licences`: source id -> licence id. Raises on an unknown licence id or one outside `allow`."""
    reg = load_register()
    allowed = set(allow)
    bad = []
    for sid, lic in sorted(licences.items()):
        if lic not in reg:
            bad.append(f"{sid}: unknown licence {lic!r} (add it to licences.toml)")
        elif lic not in allowed:
            bad.append(
                f"{sid}: licence {lic!r} ({reg[lic].name}) is not on the allow-list; "
                f"pass --allow {lic} only if the package may carry it"
            )
    if bad:
        raise LicenceError("; ".join(bad))


def attribution_text(manifest) -> str:
    reg = load_register()
    lines = [f"Scene package {manifest.name}: data sources and attribution", "(generated by camsim-scene from manifest.json)", ""]
    for s in sorted(manifest.sources, key=lambda s: s.id):
        lic = reg.get(s.licence)
        lic_line = f"  Licence: {lic.name if lic else s.licence}" + (f" ({lic.url})" if lic and lic.url else "")
        lines += [f"{s.dataset} ({s.version})", f"  {s.attribution}", lic_line, f"  Source id: {s.id}", ""]
    return "\n".join(lines)
```

- [ ] **Step 4: Run the tests**

Run: `cd scripts/scene && uv run pytest tests/test_licences.py -v`
Expected: 4 passed.

- [ ] **Step 5: Commit**

```bash
git add scripts/scene/camsim_scene/licences.py scripts/scene/camsim_scene/licences.toml scripts/scene/tests/test_licences.py
git commit -m "feat(scene): licence register, allow-list and attribution text"
```

---

### Task 7: HTTP with retries and the content-addressed fetch cache

**Files:**
- Create: `scripts/scene/camsim_scene/net.py`, `scripts/scene/camsim_scene/cache.py`
- Create: `scripts/scene/tests/fakes.py`
- Test: `scripts/scene/tests/test_cache.py`

**Interfaces:**
- Consumes: `fsutil.atomic_write`, `fsutil.CHUNK`.
- Produces (`net`): `HttpError(Exception)`; `Http(session=None, retries=5, backoff_s=1.0, sleep=time.sleep)` with `get_json(url, params=None)`, `post_json(url, body)`, `head(url) -> tuple[int, int | None]`, `stream(url) -> response` (has `.status_code`, `.headers`, `.iter_content(n)`, `.close()`); `redact(url) -> str` (drops the query string).
- Produces (`cache`): `CacheError(Exception)`; `Signer = Callable[[str, bool], str]` (url, force) -> href; `Cache(root: Path, http: Http | None = None, attempts=4, sleep=time.sleep)` with `blob_path(sha256) -> Path`, `get(url, sha256=None, sign=None) -> tuple[Path, str, int]`, `derived_path(key, suffix) -> Path`, attribute `http`; `default_cache_root() -> Path`.
- Produces (`tests/fakes.py`): `FakeResponse`, `FakeSession`, `FakeHttp` (used by Tasks 10–13).

- [ ] **Step 1: Write the fakes and the failing tests**

`scripts/scene/tests/fakes.py`:

```python
"""Test doubles for HTTP: no test talks to the network."""

from __future__ import annotations

import json
from pathlib import Path

import requests

FIXTURES = Path(__file__).parent / "fixtures"


def fixture_json(name: str):
    return json.loads((FIXTURES / "http" / name).read_text(encoding="utf-8"))


class FakeResponse:
    def __init__(self, status=200, body=b"", headers=None, json_data=None, fail_after=None):
        self.status_code = status
        self.body = body
        self.headers = dict(headers or {})
        if body and "Content-Length" not in self.headers:
            self.headers["Content-Length"] = str(len(body))
        self._json = json_data
        self.fail_after = fail_after

    def iter_content(self, n):
        for i in range(0, len(self.body), max(1, n)):
            if self.fail_after is not None and i >= self.fail_after:
                raise requests.ConnectionError("connection reset")
            yield self.body[i : i + n]

    def json(self):
        return self._json

    def close(self):
        pass


class FakeSession:
    """Returns queued responses (or calls a function) and records (method, url, kwargs)."""

    def __init__(self, responses):
        self.responses = responses
        self.calls = []

    def request(self, method, url, **kw):
        self.calls.append((method, url, kw))
        if callable(self.responses):
            return self.responses(method, url, kw)
        return self.responses.pop(0)


class FakeHttp:
    """Duck-types net.Http for adapters: routes keyed by URL (GET/POST JSON) or ("HEAD", url)."""

    def __init__(self, routes: dict):
        self.routes = routes
        self.calls = []

    def _route(self, key, *args):
        self.calls.append((key, *args))
        r = self.routes[key]
        return r(*args) if callable(r) else r

    def get_json(self, url, params=None):
        return self._route(url, params)

    def post_json(self, url, body):
        return self._route(url, body)

    def head(self, url):
        return self.routes.get(("HEAD", url), (404, None))
```

`scripts/scene/tests/test_cache.py`:

```python
import hashlib

import pytest

from camsim_scene.cache import Cache, CacheError
from camsim_scene.net import Http, HttpError
from fakes import FakeResponse, FakeSession

BODY = b"x" * 3_000_000
SHA = hashlib.sha256(BODY).hexdigest()
URL = "https://example.org/data.tif"


def make_cache(tmp_path, responses):
    session = FakeSession(responses)
    http = Http(session=session, retries=3, sleep=lambda s: None)
    return Cache(tmp_path / "cache", http=http, sleep=lambda s: None), session


def test_download_stores_blob_by_hash_and_indexes_url(tmp_path):
    cache, session = make_cache(tmp_path, [FakeResponse(body=BODY)])
    path, sha, size = cache.get(URL)
    assert sha == SHA and size == len(BODY) and path == cache.blob_path(SHA) and path.read_bytes() == BODY
    assert cache.get(URL) == (path, SHA, len(BODY))  # served from the index: no second request
    assert len(session.calls) == 1


def test_pinned_hash_already_cached_needs_no_request(tmp_path):
    cache, session = make_cache(tmp_path, [FakeResponse(body=BODY)])
    cache.get(URL)
    assert cache.get("https://mirror.example.org/other-name.tif", sha256=SHA)[1] == SHA
    assert len(session.calls) == 1


def test_hash_mismatch_fails(tmp_path):
    cache, _ = make_cache(tmp_path, [FakeResponse(body=BODY)])
    with pytest.raises(CacheError, match="manifest pins"):
        cache.get(URL, sha256="0" * 64)


def test_interrupted_download_retries_and_leaves_no_partial(tmp_path):
    cache, session = make_cache(tmp_path, [FakeResponse(body=BODY, fail_after=1 << 20), FakeResponse(body=BODY)])
    path, sha, _ = cache.get(URL)
    assert sha == SHA and len(session.calls) == 2
    assert list((tmp_path / "cache" / "tmp").iterdir()) == []


def test_truncated_body_is_a_failure(tmp_path):
    short = FakeResponse(body=BODY[:1000], headers={"Content-Length": str(len(BODY))})
    cache, _ = make_cache(tmp_path, [short, FakeResponse(body=BODY)])
    assert cache.get(URL)[1] == SHA


def test_forbidden_resigns_and_redacts(tmp_path):
    signed = []

    def sign(url, force):
        signed.append(force)
        return url + ("?sig=NEW" if force else "?sig=OLD")

    cache, session = make_cache(tmp_path, [FakeResponse(status=403), FakeResponse(body=BODY)])
    assert cache.get(URL, sign=sign)[1] == SHA
    assert signed == [False, True] and session.calls[1][1].endswith("?sig=NEW")

    cache2, _ = make_cache(tmp_path / "b", lambda m, u, kw: FakeResponse(status=403))
    with pytest.raises(CacheError) as err:
        cache2.get(URL, sign=sign)
    assert URL in str(err.value) and "sig=" not in str(err.value)


def test_file_urls_are_copied(tmp_path):
    src = tmp_path / "src.bin"
    src.write_bytes(BODY)
    cache, session = make_cache(tmp_path, [])
    path, sha, _ = cache.get(src.as_uri())
    assert sha == SHA and path.read_bytes() == BODY and session.calls == []


def test_http_retries_server_errors_then_reports(tmp_path):
    session = FakeSession([FakeResponse(status=503), FakeResponse(status=200, json_data={"ok": 1})])
    assert Http(session=session, retries=3, sleep=lambda s: None).get_json("https://api") == {"ok": 1}
    session = FakeSession(lambda m, u, kw: FakeResponse(status=503))
    with pytest.raises(HttpError, match="503"):
        Http(session=session, retries=2, sleep=lambda s: None).get_json("https://api")
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd scripts/scene && uv run pytest tests/test_cache.py -v`
Expected: FAIL, `ImportError`.

- [ ] **Step 3: Implement `net.py`**

```python
"""HTTP with retries and backoff: JSON APIs, HEAD, and streamed downloads (used by cache.py)."""

from __future__ import annotations

import time

import requests

from . import __version__

RETRY_STATUS = (429, 500, 502, 503, 504)


class HttpError(Exception):
    pass


def redact(url: str) -> str:
    """Drop the query string (Planetary Computer SAS tokens live there)."""
    return url.split("?", 1)[0]


class Http:
    def __init__(self, session=None, retries: int = 5, backoff_s: float = 1.0, sleep=time.sleep):
        self.session = session or requests.Session()
        if session is None:
            self.session.headers["User-Agent"] = f"camsim-scene/{__version__}"
        self.retries, self.backoff_s, self.sleep = retries, backoff_s, sleep

    def _request(self, method: str, url: str, **kw):
        last: object = None
        for attempt in range(self.retries):
            try:
                r = self.session.request(method, url, timeout=(15, 300), **kw)
                if r.status_code not in RETRY_STATUS:
                    return r
                last = f"HTTP {r.status_code}"
                r.close()
            except requests.RequestException as e:
                last = e
            if attempt + 1 < self.retries:
                self.sleep(self.backoff_s * 2**attempt)
        raise HttpError(f"{method} {redact(url)}: {last}")

    def get_json(self, url: str, params: dict | None = None):
        r = self._request("GET", url, params=params)
        if r.status_code != 200:
            raise HttpError(f"GET {redact(url)}: HTTP {r.status_code}")
        return r.json()

    def post_json(self, url: str, body: dict):
        r = self._request("POST", url, json=body)
        if r.status_code != 200:
            raise HttpError(f"POST {redact(url)}: HTTP {r.status_code}")
        return r.json()

    def head(self, url: str) -> tuple[int, int | None]:
        r = self._request("HEAD", url, allow_redirects=True)
        size = r.headers.get("Content-Length")
        return r.status_code, int(size) if size is not None else None

    def stream(self, url: str):
        return self._request("GET", url, stream=True)
```

- [ ] **Step 4: Implement `cache.py`**

```python
"""Content-addressed fetch cache (.cache/scene by default, or $CAMSIM_SCENE_CACHE):

    blobs/sha256/ab/<sha256>       whole assets, named by their hash
    urls/<h[:2]>/<sha256(url)>.json {url, sha256, size}: what a URL last resolved to
    derived/<k[:2]>/<key><suffix>  files derived from blobs (tiled GeoTIFFs with overviews)
    tmp/                           downloads in progress (never referenced)

A download streams into tmp/, is hashed, and is renamed into blobs/ only when complete, so an interrupted
fetch leaves no cache entry. Builds use cached bytes; a pinned hash that doesn't match fails the build."""

from __future__ import annotations

import hashlib
import json
import os
import time
import urllib.parse
import uuid
from collections.abc import Callable
from pathlib import Path

import requests

from .fsutil import CHUNK, atomic_write
from .net import Http, HttpError, redact

Signer = Callable[[str, bool], str]
REPO = Path(__file__).resolve().parents[3]


class CacheError(Exception):
    pass


class _Forbidden(Exception):
    pass


class _Truncated(Exception):
    pass


def default_cache_root() -> Path:
    env = os.environ.get("CAMSIM_SCENE_CACHE")
    return Path(env) if env else REPO / ".cache" / "scene"


class Cache:
    def __init__(self, root: Path, http: Http | None = None, attempts: int = 4, sleep=time.sleep):
        self.root = Path(root)
        self.http = http or Http()
        self.attempts, self.sleep = attempts, sleep

    def blob_path(self, sha256: str) -> Path:
        return self.root / "blobs" / "sha256" / sha256[:2] / sha256

    def derived_path(self, key: str, suffix: str) -> Path:
        return self.root / "derived" / key[:2] / f"{key}{suffix}"

    def _index_path(self, url: str) -> Path:
        h = hashlib.sha256(url.encode()).hexdigest()
        return self.root / "urls" / h[:2] / f"{h}.json"

    def get(self, url: str, sha256: str | None = None, sign: Signer | None = None) -> tuple[Path, str, int]:
        """Local path, sha256 and size of `url`'s bytes, downloading only when needed."""
        if sha256 and self.blob_path(sha256).exists():
            p = self.blob_path(sha256)
            return p, sha256, p.stat().st_size
        idx = self._index_path(url)
        if idx.exists():
            known = json.loads(idx.read_text())
            p = self.blob_path(known["sha256"])
            if p.exists() and sha256 in (None, known["sha256"]):
                return p, known["sha256"], known["size"]
        path, got, size = self._download(url, sign)
        if sha256 and got != sha256:
            raise CacheError(f"{redact(url)}: downloaded sha256 {got} but the manifest pins {sha256}")
        return path, got, size

    def _download(self, url: str, sign: Signer | None) -> tuple[Path, str, int]:
        tmpdir = self.root / "tmp"
        tmpdir.mkdir(parents=True, exist_ok=True)
        last: object = None
        for attempt in range(self.attempts):
            href = sign(url, attempt > 0) if sign else url
            tmp = tmpdir / f"{uuid.uuid4().hex}.part"
            try:
                h = hashlib.sha256()
                size = 0
                with open(tmp, "wb") as f:
                    for chunk in self._chunks(href):
                        f.write(chunk)
                        h.update(chunk)
                        size += len(chunk)
                sha = h.hexdigest()
                dst = self.blob_path(sha)
                dst.parent.mkdir(parents=True, exist_ok=True)
                os.replace(tmp, dst)
                atomic_write(self._index_path(url), json.dumps({"url": url, "sha256": sha, "size": size}).encode())
                return dst, sha, size
            except (HttpError, OSError, requests.RequestException, _Forbidden, _Truncated) as e:
                last = e
                if attempt + 1 < self.attempts:
                    self.sleep(2**attempt)
            finally:
                tmp.unlink(missing_ok=True)
        raise CacheError(f"{redact(url)}: download failed after {self.attempts} attempts: {last}")

    def _chunks(self, href: str):
        if href.startswith("file://"):
            with open(urllib.parse.unquote(urllib.parse.urlparse(href).path), "rb") as f:
                yield from iter(lambda: f.read(CHUNK), b"")
            return
        r = self.http.stream(href)
        try:
            if r.status_code in (401, 403):
                raise _Forbidden(f"HTTP {r.status_code}")
            if r.status_code != 200:
                raise HttpError(f"HTTP {r.status_code}")
            expected = r.headers.get("Content-Length")
            n = 0
            for chunk in r.iter_content(CHUNK):
                n += len(chunk)
                yield chunk
            if expected is not None and n != int(expected):
                raise _Truncated(f"got {n} of {expected} bytes")
        finally:
            r.close()
```

`requests.ConnectionError` from `iter_content` is a `RequestException`, not an `OSError`, hence its place in the `except` tuple.

- [ ] **Step 5: Run the tests**

Run: `cd scripts/scene && uv run pytest tests/test_cache.py -v`
Expected: 8 passed.

- [ ] **Step 6: Commit**

```bash
git add scripts/scene/camsim_scene/net.py scripts/scene/camsim_scene/cache.py scripts/scene/tests/fakes.py scripts/scene/tests/test_cache.py
git commit -m "feat(scene): HTTP retries and content-addressed fetch cache"
```

---

### Task 8: Datum pipelines (NAD83(2011)/NAVD88/GEOID → ITRF2014 epoch 2010.0)

**Files:**
- Create: `scripts/scene/camsim_scene/datum.py`
- Create: `scripts/scene/tests/fixtures/make_geoid_fixture.py`, `scripts/scene/tests/fixtures/grids/us_noaa_g2018u0.tif` (generated, ~79 KB)
- Test: `scripts/scene/tests/test_datum.py`

**Interfaces:**
- Consumes: `tiling.tile_size_deg`.
- Produces: `EPOCH = 2010.0`; `TARGET_CRS = "EPSG:7912"`; `GRID_URL = "https://cdn.proj.org/{name}"`; `GEOID_DATUMS: dict[str, str]` (`"GEOID18" -> "nad83_2011_navd88_geoid18"`, `"GEOID12B" -> "nad83_2011_navd88_geoid12b"`); `geoid_datum(name: str) -> str | None`; `Datum(id, from_itrf_h, to_itrf_3d, grids, vertical, geographic_crs)`; `DATUMS: dict[str, Datum]`; `DatumError(Exception)`; `manifest_section(datum_ids) -> dict` (`{"target", "epoch", "grids": {name: {"url", "sha256": None}}, "datums": {id: {...}}}`); `resolve_grids(pipeline, grid_paths) -> str`; `DatumTransform(entry: dict, grid_paths: dict[str, Path], geoid_sampler=None)` with `vertical` (`"none" | "proj" | "raster_geoid"`), `to_source_geographic(lon, lat) -> (lon, lat)`, `vertical_offset(slon, slat) -> np.ndarray`; `offset_lattice(dt, z, lon, lat) -> np.ndarray` (offsets at ITRF lon/lat, bilinear from exact nodes spaced `tile_size(z)/32` on the global grid); `SUBGRID = 32`.
- `geoid_sampler`: `Callable[[np.ndarray, np.ndarray], np.ndarray]` returning geoid undulation N (m) at WGS84 lon/lat (Task 14 supplies one from the ETOPO geoid raster).

- [ ] **Step 1: Generate the GEOID18 test fixture (one-off, needs the network)**

`scripts/scene/tests/fixtures/make_geoid_fixture.py`:

```python
"""One-off: crop PROJ's GEOID18 grid (public domain, NOAA) to Pendleton + 100 km for offline datum tests.
Keeps PROJ's grid tags (TYPE=VERTICAL_OFFSET_GEOGRAPHIC_TO_VERTICAL), so vgridshift accepts the crop.

    uv run --project scripts/scene python scripts/scene/tests/fixtures/make_geoid_fixture.py
"""

import urllib.request
from pathlib import Path

import rasterio
from rasterio.windows import from_bounds

URL = "https://cdn.proj.org/us_noaa_g2018u0.tif"
OUT = Path(__file__).parent / "grids" / "us_noaa_g2018u0.tif"


def main() -> None:
    src = OUT.with_suffix(".full.tif")
    urllib.request.urlretrieve(URL, src)
    with rasterio.open(src) as ds:  # longitudes are positive east (230..300)
        w = from_bounds(-118.8 + 360, 32.2, -116.0 + 360, 34.6, ds.transform).round_offsets().round_lengths()
        data = ds.read(window=w)
        prof = ds.profile.copy()
        prof.update(width=data.shape[2], height=data.shape[1], transform=ds.window_transform(w), compress="deflate", tiled=False)
        prof.pop("blockxsize", None)
        prof.pop("blockysize", None)
        with rasterio.open(OUT, "w", **prof) as out:
            out.write(data)
            out.update_tags(**ds.tags())
            out.update_tags(1, **ds.tags(1))
    src.unlink()
    print(OUT, OUT.stat().st_size, "bytes")


if __name__ == "__main__":
    main()
```

Run: `uv run --project scripts/scene python scripts/scene/tests/fixtures/make_geoid_fixture.py`
Expected: `.../grids/us_noaa_g2018u0.tif 78840 bytes` (± a few hundred).

- [ ] **Step 2: Write the failing tests**

`scripts/scene/tests/test_datum.py`:

```python
from pathlib import Path

import numpy as np
import pyproj
import pytest

from camsim_scene import datum, tiling
from camsim_scene.datum import DatumError, DatumTransform

GRIDS = {"us_noaa_g2018u0.tif": Path(__file__).parent / "fixtures" / "grids" / "us_noaa_g2018u0.tif"}


def g18() -> DatumTransform:
    section = datum.manifest_section(["nad83_2011_navd88_geoid18"])
    return DatumTransform(section["datums"]["nad83_2011_navd88_geoid18"], GRIDS)


def test_known_point_100m_navd88_is_65_283m_ellipsoidal():
    # UTM 11N 470000 E 3685000 N (NAD83(2011)) -> geographic, then the pinned vertical step
    lon, lat = pyproj.Transformer.from_crs("EPSG:6340", "EPSG:6318", always_xy=True).transform(470000.0, 3685000.0)
    off = g18().vertical_offset(np.array([lon]), np.array([lat]))
    assert 100.0 + off[0] == pytest.approx(65.283, abs=0.001)


def test_pendleton_horizontal_shift_is_about_1_35_m():
    lon, lat = np.array([-117.43]), np.array([33.355])
    slon, slat = g18().to_source_geographic(lon, lat)
    _, _, dist = pyproj.Geod(ellps="GRS80").inv(lon, lat, slon, slat)
    assert 1.25 < dist[0] < 1.45


def test_wgs84_is_identity():
    dt = DatumTransform(datum.manifest_section(["wgs84"])["datums"]["wgs84"], {})
    lon, lat = np.array([10.5, -117.0]), np.array([1.0, 33.0])
    a, b = dt.to_source_geographic(lon, lat)
    assert np.array_equal(a, lon) and np.array_equal(b, lat)
    assert np.all(dt.vertical_offset(lon, lat) == 0.0)


def test_missing_grid_fails():
    entry = datum.manifest_section(["nad83_2011_navd88_geoid18"])["datums"]["nad83_2011_navd88_geoid18"]
    with pytest.raises(DatumError, match="us_noaa_g2018u0.tif"):
        DatumTransform(entry, {})


def test_manifest_section_lists_pipelines_and_grids():
    s = datum.manifest_section(["nad83_2011", "nad83_2011_navd88_geoid18"])
    assert s["target"] == "EPSG:7912" and s["epoch"] == 2010.0
    assert set(s["grids"]) == {"us_noaa_g2018u0.tif"} and s["grids"]["us_noaa_g2018u0.tif"]["sha256"] is None
    assert "vgridshift +grids=us_noaa_g2018u0.tif" in s["datums"]["nad83_2011_navd88_geoid18"]["to_itrf_3d"]
    assert "t_epoch=2010" in s["datums"]["nad83_2011"]["from_itrf_h"]
    assert datum.geoid_datum("geoid 18") == "nad83_2011_navd88_geoid18" and datum.geoid_datum("GEOID12A") is None


@pytest.mark.parametrize("z, bound_mm", [(9, 10.0), (12, 1.0), (16, 0.1)])
def test_offset_lattice_error_bound(z, bound_mm):
    dt = g18()
    s = tiling.tile_size_deg(z)
    x, y = int((-117.43 + 180) // s), int((33.355 + 90) // s)
    w, so, e, n = tiling.tile_bounds(z, x, y)
    lon, lat = np.meshgrid(np.linspace(w, e, 257), np.linspace(n, so, 257))
    exact = dt.vertical_offset(*dt.to_source_geographic(lon, lat))
    approx = datum.offset_lattice(dt, z, lon, lat)
    assert np.abs(approx - exact).max() * 1000 <= bound_mm


def test_offset_lattice_agrees_exactly_on_a_shared_edge():
    dt, z = g18(), 12
    s = tiling.tile_size_deg(z)
    x, y = int((-117.43 + 180) // s), int((33.355 + 90) // s)
    edge_lon = np.full(257, tiling.tile_bounds(z, x, y)[2])
    edge_lat = np.linspace(*tiling.tile_bounds(z, x, y)[1::2], 257)
    left = datum.offset_lattice(dt, z, np.concatenate([edge_lon - s / 2, edge_lon]), np.concatenate([edge_lat, edge_lat]))[257:]
    right = datum.offset_lattice(dt, z, np.concatenate([edge_lon, edge_lon + s / 2]), np.concatenate([edge_lat, edge_lat]))[:257]
    assert np.array_equal(left, right)
```

- [ ] **Step 3: Run them to see them fail**

Run: `cd scripts/scene && uv run pytest tests/test_datum.py -v`
Expected: FAIL, `ImportError`.

- [ ] **Step 4: Implement `datum.py`**

```python
"""Datum declarations -> pinned PROJ pipelines (target ITRF2014 geographic + ellipsoid height, EPSG:7912,
coordinate epoch 2010.0). Adapters only declare a datum id; all datum maths is here.

The pipelines are explicit strings stored in the manifest (PROJ's defaults are wrong here: EPSG:4979 picks a
GEOID03 chain 0.78 m off, EPSG:9755 silently drops the geoid; docs/realism/data-sources.md 1.4). Grids are
named by their cdn.proj.org file name, fetched into the cache, and substituted by absolute path at build
time with PROJ's network access off. Sources labelled NAD83 (EPSG:4269, 269xx) are taken as the
NAD83(2011) realisation."""

from __future__ import annotations

import re
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import pyproj
from scipy.ndimage import map_coordinates

from .tiling import tile_size_deg

EPOCH = 2010.0
TARGET_CRS = "EPSG:7912"
GRID_URL = "https://cdn.proj.org/{name}"
SUBGRID = 32

# "ITRF2014 to NAD83(2011) (1)" (time-dependent Helmert, coordinate frame), as PROJ 9.8 emits it.
HELMERT_ITRF2014_TO_NAD83_2011 = (
    "+proj=helmert +x=1.0053 +y=-1.90921 +z=-0.54157 +rx=0.02678138 +ry=-0.00042027 +rz=0.01093206 "
    "+s=0.00036891 +dx=0.00079 +dy=-0.0006 +dz=-0.00144 +drx=6.667e-05 +dry=-0.00075744 +drz=-5.133e-05 "
    "+ds=-7.201e-05 +t_epoch=2010 +convention=coordinate_frame"
)
_ITRF_TO_NAD83_H = (
    "+proj=pipeline +step +proj=unitconvert +xy_in=deg +xy_out=rad +step +proj=cart +ellps=GRS80 "
    f"+step {HELMERT_ITRF2014_TO_NAD83_2011} +step +inv +proj=cart +ellps=GRS80 +step +proj=unitconvert +xy_in=rad +xy_out=deg"
)


def _nad83_navd88_to_itrf(grid: str) -> str:
    return (
        "+proj=pipeline +step +proj=unitconvert +xy_in=deg +xy_out=rad "
        f"+step +proj=vgridshift +grids={grid} +multiplier=1 +step +proj=cart +ellps=GRS80 "
        f"+step +inv {HELMERT_ITRF2014_TO_NAD83_2011} +step +inv +proj=cart +ellps=GRS80 "
        "+step +proj=unitconvert +xy_in=rad +xy_out=deg"
    )


@dataclass(frozen=True)
class Datum:
    id: str
    from_itrf_h: str  # ITRF2014 lon/lat (deg) -> source geographic lon/lat (deg)
    to_itrf_3d: str | None  # source geographic lon/lat + height -> ITRF2014 lon/lat/h ("proj" vertical only)
    grids: tuple[str, ...]
    vertical: str  # "none" | "proj" | "raster_geoid"
    geographic_crs: str


DATUMS = {
    d.id: d
    for d in (
        Datum("nad83_2011", _ITRF_TO_NAD83_H, None, (), "none", "EPSG:6318"),
        Datum("nad83_2011_navd88_geoid18", _ITRF_TO_NAD83_H, _nad83_navd88_to_itrf("us_noaa_g2018u0.tif"),
              ("us_noaa_g2018u0.tif",), "proj", "EPSG:6318"),
        Datum("nad83_2011_navd88_geoid12b", _ITRF_TO_NAD83_H, _nad83_navd88_to_itrf("us_noaa_g2012bu0.tif"),
              ("us_noaa_g2012bu0.tif",), "proj", "EPSG:6318"),
        Datum("wgs84", "+proj=noop", None, (), "none", "EPSG:4326"),  # WGS 84 (G2139) == ITRF2014 at cm level
        Datum("wgs84_egm2008_raster", "+proj=noop", None, (), "raster_geoid", "EPSG:4326"),
    )
}
GEOID_DATUMS = {"GEOID18": "nad83_2011_navd88_geoid18", "GEOID12B": "nad83_2011_navd88_geoid12b"}


class DatumError(Exception):
    pass


def geoid_datum(name: str) -> str | None:
    """Datum id for a 3DEP geoid name ("GEOID18", "Geoid 12B", ...); None when unsupported (e.g. GEOID12A)."""
    return GEOID_DATUMS.get(re.sub(r"\s+", "", name or "").upper())


def manifest_section(datum_ids) -> dict:
    datums, grids = {}, {}
    for did in sorted(set(datum_ids)):
        d = DATUMS[did]
        datums[did] = {
            "from_itrf_h": d.from_itrf_h,
            "to_itrf_3d": d.to_itrf_3d,
            "grids": list(d.grids),
            "vertical": d.vertical,
            "geographic_crs": d.geographic_crs,
        }
        for g in d.grids:
            grids[g] = {"url": GRID_URL.format(name=g), "sha256": None}
    return {"target": TARGET_CRS, "epoch": EPOCH, "grids": grids, "datums": datums}


def resolve_grids(pipeline: str, grid_paths: dict[str, Path]) -> str:
    def sub(m: re.Match) -> str:
        name = m.group(1)
        if name not in grid_paths or not Path(grid_paths[name]).exists():
            raise DatumError(f"PROJ grid {name} is not in the cache (run `camsim-scene fetch`)")
        return f"+grids={Path(grid_paths[name]).resolve()}"

    return re.sub(r"\+grids=([^\s]+)", sub, pipeline)


class DatumTransform:
    def __init__(self, entry: dict, grid_paths: dict[str, Path], geoid_sampler: Callable | None = None):
        pyproj.network.set_network_enabled(False)
        self.vertical = entry["vertical"]
        self.geographic_crs = entry["geographic_crs"]
        self._noop = entry["from_itrf_h"] == "+proj=noop"
        self._h = None if self._noop else pyproj.Transformer.from_pipeline(resolve_grids(entry["from_itrf_h"], grid_paths))
        self._v = (
            pyproj.Transformer.from_pipeline(resolve_grids(entry["to_itrf_3d"], grid_paths)) if entry["to_itrf_3d"] else None
        )
        if self.vertical == "raster_geoid" and geoid_sampler is None:
            raise DatumError("datum needs a geoid raster")
        self._geoid = geoid_sampler

    def to_source_geographic(self, lon: np.ndarray, lat: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        if self._noop:
            return np.array(lon, np.float64, copy=True), np.array(lat, np.float64, copy=True)
        lon, lat = np.asarray(lon, np.float64), np.asarray(lat, np.float64)
        x, y, _, _ = self._h.transform(lon, lat, np.zeros_like(lon), np.full_like(lon, EPOCH))
        return np.asarray(x), np.asarray(y)

    def vertical_offset(self, slon: np.ndarray, slat: np.ndarray) -> np.ndarray:
        """Ellipsoid height of the source's height zero at source-geographic lon/lat (h = H + offset)."""
        slon, slat = np.asarray(slon, np.float64), np.asarray(slat, np.float64)
        if self.vertical == "none":
            return np.zeros_like(slon)
        if self.vertical == "raster_geoid":
            return np.asarray(self._geoid(slon, slat), np.float64)
        _, _, h, _ = self._v.transform(slon, slat, np.zeros_like(slon), np.full_like(slon, EPOCH))
        return np.asarray(h)


def offset_lattice(dt: DatumTransform, z: int, lon: np.ndarray, lat: np.ndarray) -> np.ndarray:
    """Vertical offset at ITRF lon/lat (any shape): exact on lattice nodes spaced tile_size(z)/32 and aligned to
    the global tile grid, bilinear in between. Neighbouring tiles share the nodes on their common edge."""
    lon, lat = np.asarray(lon, np.float64), np.asarray(lat, np.float64)
    if dt.vertical == "none":
        return np.zeros_like(lon)
    step = tile_size_deg(z) / SUBGRID
    i0 = int(np.floor((lon.min() + 180.0) / step))
    i1 = int(np.ceil((lon.max() + 180.0) / step))
    j0 = int(np.floor((lat.min() + 90.0) / step))
    j1 = int(np.ceil((lat.max() + 90.0) / step))
    node_lon = -180.0 + np.arange(i0, i1 + 1) * step
    node_lat = np.clip(-90.0 + np.arange(j0, j1 + 1) * step, -90.0, 90.0)
    LO, LA = np.meshgrid(node_lon, node_lat)
    nodes = dt.vertical_offset(*dt.to_source_geographic(LO, LA))
    c = (lon + 180.0) / step - i0
    r = (lat + 90.0) / step - j0
    return map_coordinates(nodes, [r, c], order=1, mode="nearest")
```

- [ ] **Step 5: Run the tests**

Run: `cd scripts/scene && uv run pytest tests/test_datum.py -v`
Expected: 9 passed.

- [ ] **Step 6: Commit**

```bash
git add scripts/scene/camsim_scene/datum.py scripts/scene/tests/test_datum.py scripts/scene/tests/fixtures/make_geoid_fixture.py \
  scripts/scene/tests/fixtures/grids/us_noaa_g2018u0.tif
git commit -m "feat(scene): pinned NAD83(2011)/NAVD88 -> ITRF2014 pipelines and offset lattice"
```

---
### Task 9: Source interface, raster sampling, prepare, footprints, registry

**Files:**
- Create: `scripts/scene/camsim_scene/sources/__init__.py`, `scripts/scene/camsim_scene/sources/base.py`
- Create: `scripts/scene/tests/rasters.py`
- Test: `scripts/scene/tests/test_sources_base.py`

**Interfaces:**
- Consumes: `fsutil.sha256_bytes`; `cache.Cache.derived_path` (duck-typed).
- Produces (`sources.base`; must import only numpy at module level, because the land-cover wrapper imports `sources.worldcover` without rasterio/shapely/pyproj): `M_PER_DEG = 111320.0`; `Layer(StrEnum)` (`TERRAIN`, `IMAGERY`, `LANDCOVER`); `Area(bounds)`; `Asset(id, url, size=None, group="", rank=0, role="data", metadata={}, sha256=None)`; `Source` (Protocol); `SourceBase(options=None, http=None)` with class attributes `id, layer, licence, dataset, version, attribution, max_zoom, global_coverage=False, area_kind="bbox", datum=None` and methods `options()`, `sign(url, force=False)`, `prepare(path, asset, cache)`; `SourceRaster(path, datum, bands=(1,), nodata=None, nodata_rule="value", crs=None, clamp_edges=False, decode=None, vertical_asset=None)` with `file_crs()`, `valid_mask(data)`, `sample(x, y, target_m) -> (values (nb, *shape) float64, valid bool shape)`; `project(raster, slon, slat) -> (x, y)`; `choose_overview(res_m, factors, target_m) -> int | None`; `DECODERS` (`None` → clip to uint8, `"s2_reflectance"`); `to_uint8(raster, values)`; `prepare_raster(path, asset, cache, crs=None, transform=None, resampling="average") -> Path | None` (writes a COG); `footprint_lonlat(raster) -> shapely geometry | None`.
- Produces (`sources`): `BUILTIN: dict[str, str]`; `UnknownSource(KeyError)`; `make_source(source_id, adapter=None, options=None, http=None)` (adapter = built-in id or `"module:Class"`; sets `src.id = source_id`).
- Produces (`tests/rasters.py`): `write_geotiff(path, data, west, north, res, crs="EPSG:4326", nodata=None, overviews=(2, 4), tiled=True, tags=None) -> Path`.

Sampling rules (Decisions 6): pixel centres, bilinear, valid only when all four neighbours are valid; overview level = the coarsest whose pixel size (metres; degrees × 111,320 for geographic rasters) is ≤ `target_m`; `clamp_edges` extends edge pixels by up to one pixel (lon ±180, poles, Blue Marble tile seams) and never further.

- [ ] **Step 1: Write the raster helper and the failing tests**

`scripts/scene/tests/rasters.py`:

```python
"""Write small synthetic GeoTIFFs for tests."""

from pathlib import Path

import numpy as np
import rasterio
from rasterio.enums import Resampling
from rasterio.transform import from_origin


def write_geotiff(path: Path, data, west, north, res, crs="EPSG:4326", nodata=None, overviews=(2, 4), tiled=True, tags=None) -> Path:
    data = np.asarray(data)
    if data.ndim == 2:
        data = data[None]
    profile = dict(
        driver="GTiff", width=data.shape[2], height=data.shape[1], count=data.shape[0], dtype=data.dtype,
        crs=crs, transform=from_origin(west, north, res, res), nodata=nodata,
    )
    if tiled:
        profile.update(tiled=True, blockxsize=256, blockysize=256)
    with rasterio.open(path, "w", **profile) as ds:
        ds.write(data)
        if tags:
            ds.update_tags(**tags)
    if overviews:
        with rasterio.open(path, "r+") as ds:
            ds.build_overviews(list(overviews), Resampling.average)
    return Path(path)
```

`scripts/scene/tests/test_sources_base.py`:

```python
import numpy as np
import pytest
import rasterio
from PIL import Image

from camsim_scene import sources
from camsim_scene.sources import base
from camsim_scene.sources.base import Asset, SourceRaster
from rasters import write_geotiff


class StubCache:
    def __init__(self, root):
        self.root = root

    def derived_path(self, key, suffix):
        return self.root / "derived" / key[:2] / f"{key}{suffix}"


def plane_raster(tmp_path, west=10.0, north=11.0, res=0.01, n=100, **kw):
    r, c = np.mgrid[0:n, 0:n]
    return write_geotiff(tmp_path / "plane.tif", (3.0 * c + 7.0 * r).astype(np.float32), west, north, res, **kw)


def plane_at(lon, lat, west=10.0, north=11.0, res=0.01):
    return 3.0 * ((lon - west) / res - 0.5) + 7.0 * ((north - lat) / res - 0.5)


def test_bilinear_is_exact_on_a_plane(tmp_path):
    ras = SourceRaster(plane_raster(tmp_path), datum="wgs84")
    rng = np.random.default_rng(0)
    lon, lat = rng.uniform(10.01, 10.98, 500), rng.uniform(10.02, 10.99, 500)
    vals, ok = ras.sample(lon, lat, target_m=1.0)
    assert ok.all() and np.abs(vals[0] - plane_at(lon, lat)).max() < 1e-3


def test_points_outside_are_invalid_and_shapes_are_kept(tmp_path):
    ras = SourceRaster(plane_raster(tmp_path), datum="wgs84")
    lon, lat = np.meshgrid(np.linspace(9.0, 10.5, 7), np.linspace(10.5, 12.0, 5))
    vals, ok = ras.sample(lon, lat, target_m=1.0)
    assert vals.shape == (1, 5, 7) and ok.shape == (5, 7)
    assert not ok[lon < 10.0].any() and not ok[lat > 11.0].any() and ok[(lon > 10.1) & (lat < 10.9)].all()


def test_choose_overview_picks_the_coarsest_not_coarser_than_target():
    assert base.choose_overview(1.0, [2, 4, 8], 0.5) is None
    assert base.choose_overview(1.0, [2, 4, 8], 3.0) == 0
    assert base.choose_overview(1.0, [2, 4, 8], 100.0) == 2


def test_nodata_invalidates_its_neighbourhood_only(tmp_path):
    data = np.full((50, 50), 5.0, np.float32)
    data[20, 20] = -999999.0
    path = write_geotiff(tmp_path / "nd.tif", data, 0.0, 50.0, 1.0, nodata=-999999.0, overviews=())
    ras = SourceRaster(path, datum="wgs84", nodata=-999999.0)
    vals, ok = ras.sample(np.array([20.6, 20.2, 40.5]), np.array([29.4, 29.6, 10.5]), target_m=1e9)
    assert ok.tolist() == [False, False, True] and vals[0, 2] == 5.0


def test_clamp_edges_extends_one_pixel_only(tmp_path):
    path = write_geotiff(tmp_path / "g.tif", np.ones((180, 360), np.float32), -180.0, 90.0, 1.0, overviews=())
    lon, lat = np.array([-179.9, -180.0, 10.0]), np.array([0.0, 89.99, 0.0])
    assert SourceRaster(path, datum="wgs84").sample(lon, lat, 1e9)[1].tolist() == [False, False, True]
    assert SourceRaster(path, datum="wgs84", clamp_edges=True).sample(lon, lat, 1e9)[1].tolist() == [True, True, True]
    small = write_geotiff(tmp_path / "s.tif", np.ones((10, 10), np.float32), 0.0, 10.0, 1.0, overviews=())
    far = SourceRaster(small, datum="wgs84", clamp_edges=True).sample(np.array([12.0]), np.array([5.0]), 1e9)[1]
    assert far.tolist() == [False]


def test_all_zero_rule_for_imagery(tmp_path):
    rgb = np.full((3, 20, 20), 100, np.uint8)
    rgb[:, :, :10] = 0
    rgb[1, :, 15:] = 0  # one zero band is still data
    path = write_geotiff(tmp_path / "rgb.tif", rgb, 0.0, 20.0, 1.0, overviews=())
    ras = SourceRaster(path, datum="nad83_2011", bands=(1, 2, 3), nodata_rule="all_zero")
    _, ok = ras.sample(np.array([3.0, 12.5, 17.5]), np.array([10.0, 10.0, 10.0]), 1e9)
    assert ok.tolist() == [False, True, True]


def test_projected_raster_is_sampled_through_its_projection(tmp_path):
    import pyproj

    r, c = np.mgrid[0:200, 0:200]
    path = write_geotiff(tmp_path / "utm.tif", (c + 1000.0 * r).astype(np.float64), 470000.0, 3685000.0, 1.0, crs="EPSG:26911", overviews=())
    ras = SourceRaster(path, datum="nad83_2011_navd88_geoid18")
    lon, lat = pyproj.Transformer.from_crs("EPSG:26911", "EPSG:4269", always_xy=True).transform(470100.3, 3684900.7)
    x, y = base.project(ras, np.array([lon]), np.array([lat]))
    assert x[0] == pytest.approx(470100.3, abs=1e-6) and y[0] == pytest.approx(3684900.7, abs=1e-6)
    vals, ok = ras.sample(x, y, 1.0)
    assert ok[0] and vals[0, 0] == pytest.approx((x[0] - 470000.0 - 0.5) + 1000.0 * (3685000.0 - y[0] - 0.5), abs=1e-6)


def test_prepare_tiles_strips_and_georeferences_pngs(tmp_path):
    cache = StubCache(tmp_path)
    strip = write_geotiff(tmp_path / "strip.tif", np.ones((1000, 2000), np.float32), 0, 10, 0.01, tiled=False, overviews=())
    out = base.prepare_raster(strip, Asset("s", "file://x", sha256="ab" * 32), cache)
    with rasterio.open(out) as ds:
        assert ds.profile["tiled"] and ds.overviews(1) == [2, 4]
        assert ds.tags(ns="IMAGE_STRUCTURE").get("LAYOUT") == "COG"
    assert base.prepare_raster(strip, Asset("s", "file://x", sha256="ab" * 32), cache) == out
    cog = write_geotiff(tmp_path / "cog.tif", np.ones((300, 300), np.float32), 0, 10, 0.01)
    assert base.prepare_raster(cog, Asset("c", "file://y", sha256="cd" * 32), cache) is None
    png = tmp_path / "t.png"
    Image.fromarray(np.full((16, 16, 3), 7, np.uint8)).save(png)
    from rasterio.transform import Affine

    t = Affine(90.0 / 21600, 0, -90.0, 0, -90.0 / 21600, 0.0)
    out = base.prepare_raster(png, Asset("p", "file://z", sha256="ef" * 32), cache, crs="EPSG:4326", transform=t)
    with rasterio.open(out) as ds:
        assert ds.crs.to_epsg() == 4326 and ds.transform == t and ds.read(1)[0, 0] == 7


def test_footprint_of_half_valid_raster(tmp_path):
    data = np.full((400, 400), 5.0, np.float32)
    data[:, 200:] = -999999.0
    path = write_geotiff(tmp_path / "half.tif", data, 10.0, 14.0, 0.01, nodata=-999999.0, overviews=(2, 4))
    fp = base.footprint_lonlat(SourceRaster(path, datum="wgs84", nodata=-999999.0))
    w, s, e, n = fp.bounds
    assert w == pytest.approx(10.0, abs=0.05) and e == pytest.approx(12.0, abs=0.1) and s == pytest.approx(10.0, abs=0.05)
    assert fp.area == pytest.approx(8.0, rel=0.1)


def test_footprint_of_all_nodata_is_none(tmp_path):
    path = write_geotiff(tmp_path / "empty.tif", np.zeros((3, 100, 100), np.uint8), 0, 1, 0.01, overviews=())
    assert base.footprint_lonlat(SourceRaster(path, datum="wgs84", bands=(1, 2, 3), nodata_rule="all_zero")) is None


def test_s2_decoder():
    out = base.DECODERS["s2_reflectance"](np.array([[0.0, 3000.0, 6000.0, np.nan]]))
    assert out.dtype == np.uint8 and out.tolist() == [[0, 255, 255, 0]]


def test_registry_builds_by_module_path_and_rejects_unknown():
    src = sources.make_source("my_id", "camsim_scene.sources.base:SourceBase", {"k": 1})
    assert src.id == "my_id" and src.options() == {"k": 1}
    with pytest.raises(sources.UnknownSource):
        sources.make_source("nope")
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd scripts/scene && uv run pytest tests/test_sources_base.py -v`
Expected: FAIL, `ImportError`.

- [ ] **Step 3: Implement `sources/base.py`**

```python
"""Source adapter interface, and the raster sampling every layer uses.

Adapters declare a datum id (datum.py does the maths), open cached files as SourceRasters, and may sign URLs
(Planetary Computer) or prepare derived files (overviews, georeferencing). Keep module-level imports to
numpy: the land-cover wrapper imports sources.worldcover in a minimal environment."""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from enum import StrEnum
from functools import lru_cache
from pathlib import Path
from typing import Protocol

import numpy as np

from ..fsutil import sha256_bytes

M_PER_DEG = 111320.0
PREPARE_VERSION = "1"
Bounds = tuple[float, float, float, float]


class Layer(StrEnum):
    TERRAIN = "terrain"
    IMAGERY = "imagery"
    LANDCOVER = "landcover"


@dataclass(frozen=True)
class Area:
    bounds: Bounds


@dataclass
class Asset:
    id: str
    url: str
    size: int | None = None
    group: str = ""
    rank: int = 0
    role: str = "data"
    metadata: dict = field(default_factory=dict)
    sha256: str | None = None


class Source(Protocol):
    id: str
    layer: Layer
    licence: str
    dataset: str
    version: str
    attribution: str
    max_zoom: int
    global_coverage: bool
    area_kind: str  # "globe" | "ring" | "bbox": where discovery looks
    datum: str | None

    def options(self) -> dict: ...
    def discover(self, area: Area) -> list[Asset]: ...
    def open(self, path: Path, asset: Asset) -> SourceRaster: ...
    def sign(self, url: str, force: bool = False) -> str: ...
    def prepare(self, path: Path, asset: Asset, cache) -> Path | None: ...


class SourceBase:
    id = ""
    layer: Layer = Layer.TERRAIN
    licence = "LicenseRef-PublicDomain-USGov"
    dataset = ""
    version = ""
    attribution = ""
    max_zoom = 0
    global_coverage = False
    area_kind = "bbox"
    datum: str | None = None

    def __init__(self, options: dict | None = None, http=None):
        self._options = dict(options or {})
        self.http = http

    def options(self) -> dict:
        return dict(self._options)

    def discover(self, area: Area) -> list[Asset]:
        return []

    def open(self, path: Path, asset: Asset) -> SourceRaster:
        raise NotImplementedError

    def sign(self, url: str, force: bool = False) -> str:
        return url

    def prepare(self, path: Path, asset: Asset, cache) -> Path | None:
        return prepare_raster(path, asset, cache)


S2_WHITE_DN = 3000.0  # reflectance 0.30 (DN x 1e-4) -> white
S2_GAMMA = 2.2


def _uint8(v: np.ndarray) -> np.ndarray:
    return np.clip(np.rint(np.nan_to_num(v)), 0, 255).astype(np.uint8)


def _s2_reflectance(v: np.ndarray) -> np.ndarray:
    r = np.clip(np.nan_to_num(v) / S2_WHITE_DN, 0.0, 1.0) ** (1.0 / S2_GAMMA)
    return np.rint(r * 255.0).astype(np.uint8)


DECODERS = {None: _uint8, "s2_reflectance": _s2_reflectance}


@lru_cache(maxsize=64)
def _dataset(path: str, level: int | None):
    import rasterio

    return rasterio.open(path) if level is None else rasterio.open(path, overview_level=level)


@lru_cache(maxsize=64)
def _projector(crs_text: str):
    import pyproj

    crs = pyproj.CRS.from_user_input(crs_text)
    if crs.is_geographic:
        return None
    return pyproj.Transformer.from_crs(crs.geodetic_crs, crs, always_xy=True)  # projection only, no datum change


def _span(v: np.ndarray, n: int) -> tuple[int, int] | None:
    """Pixel index range [lo, hi] (at least two pixels) covering v's bilinear neighbourhoods, or None if outside."""
    lo = max(0, int(np.floor(v.min())))
    hi = min(n - 1, int(np.floor(v.max())) + 1)
    if lo > hi or n < 2:
        return None
    if hi == lo:
        lo = max(0, hi - 1)
        hi = lo + 1
    return lo, hi


def choose_overview(res_m: float, factors: list[int], target_m: float) -> int | None:
    level = None
    for i, f in enumerate(factors):
        if res_m * f <= target_m:
            level = i
    return level


@dataclass
class SourceRaster:
    path: Path
    datum: str
    bands: tuple[int, ...] = (1,)
    nodata: float | None = None
    nodata_rule: str = "value"  # "value": band 1 is nodata or non-finite; "all_zero": every band is 0
    crs: str | None = None  # None: the file's own CRS
    clamp_edges: bool = False  # extend edge pixels by up to one pixel (global rasters, tile seams)
    decode: str | None = None  # DECODERS key (imagery)
    vertical_asset: str | None = None  # "<source>/<asset>" of a geoid raster (raster_geoid datum)

    def file_crs(self) -> str:
        return self.crs or _dataset(str(self.path), None).crs.to_wkt()

    def valid_mask(self, data: np.ndarray) -> np.ndarray:
        if self.nodata_rule == "all_zero":
            return np.any(data != 0, axis=0)
        v = data[0]
        ok = np.isfinite(v)
        if self.nodata is not None:
            ok &= v != self.nodata
        return ok

    def sample(self, x, y, target_m: float) -> tuple[np.ndarray, np.ndarray]:
        """Bilinear values at source-CRS coordinates (any shape). A point is valid when its four neighbours are."""
        from rasterio.windows import Window

        x, y = np.asarray(x, np.float64), np.asarray(y, np.float64)
        shape, nb = x.shape, len(self.bands)
        out = np.full((nb, *shape), np.nan)
        valid = np.zeros(shape, bool)
        ds0 = _dataset(str(self.path), None)
        res = abs(ds0.transform.a)
        res_m = res * M_PER_DEG if ds0.crs is None or ds0.crs.is_geographic else res
        level = choose_overview(res_m, ds0.overviews(self.bands[0]), target_m)
        ds = ds0 if level is None else _dataset(str(self.path), level)
        col, row = ~ds.transform * (x, y)
        col, row = np.asarray(col) - 0.5, np.asarray(row) - 0.5
        if self.clamp_edges:
            col = np.where((col >= -1.0) & (col <= ds.width), np.clip(col, 0.0, ds.width - 1.0), col)
            row = np.where((row >= -1.0) & (row <= ds.height), np.clip(row, 0.0, ds.height - 1.0), row)
        fin = np.isfinite(col) & np.isfinite(row)
        if not fin.any():
            return out, valid
        cs, rs = _span(col[fin], ds.width), _span(row[fin], ds.height)
        if cs is None or rs is None:
            return out, valid
        (c0, c1), (r0, r1) = cs, rs
        data = ds.read(list(self.bands), window=Window(c0, r0, c1 - c0 + 1, r1 - r0 + 1)).astype(np.float64)
        ok_px = self.valid_mask(data)
        h, w = ok_px.shape
        lc, lr = np.where(fin, col - c0, -10.0), np.where(fin, row - r0, -10.0)
        j0 = np.minimum(np.floor(lc).astype(np.int64), w - 2)
        i0 = np.minimum(np.floor(lr).astype(np.int64), h - 2)
        fc, fr = lc - j0, lr - i0
        inside = fin & (j0 >= 0) & (i0 >= 0) & (fc <= 1.0) & (fr <= 1.0)
        j0c, i0c = np.clip(j0, 0, w - 2), np.clip(i0, 0, h - 2)
        corners = [(i0c, j0c), (i0c, j0c + 1), (i0c + 1, j0c), (i0c + 1, j0c + 1)]
        ok = inside.copy()
        for ii, jj in corners:
            ok &= ok_px[ii, jj]
        wts = [(1 - fr) * (1 - fc), (1 - fr) * fc, fr * (1 - fc), fr * fc]
        for b in range(nb):
            v = sum(wt * data[b][ii, jj] for wt, (ii, jj) in zip(wts, corners))
            out[b] = np.where(ok, v, np.nan)
        return out, ok


def project(raster: SourceRaster, slon, slat) -> tuple[np.ndarray, np.ndarray]:
    """Source-geographic lon/lat -> the raster's CRS (identity for geographic rasters)."""
    t = _projector(raster.file_crs())
    if t is None:
        return np.asarray(slon, np.float64), np.asarray(slat, np.float64)
    x, y = t.transform(slon, slat)
    return np.asarray(x), np.asarray(y)


def to_uint8(raster: SourceRaster, values: np.ndarray) -> np.ndarray:
    return DECODERS[raster.decode](values)


def prepare_raster(path: Path, asset: Asset, cache, crs=None, transform=None, resampling: str = "average") -> Path | None:
    """A Cloud Optimized GeoTIFF (OGC 21-026; deflate, 512 px blocks, average overviews) derived from `path`, or None
    when the file is already tiled with overviews. `crs`/`transform` georeference files that carry none (Blue Marble)."""
    import rasterio
    import rasterio.shutil
    from rasterio.windows import Window

    with rasterio.open(path) as ds:
        ready = ds.profile.get("tiled", False) and (ds.overviews(1) or max(ds.width, ds.height) <= 1024)
        if crs is None and transform is None and ready:
            return None
        key = sha256_bytes(f"{asset.sha256}|{PREPARE_VERSION}|{crs}|{tuple(transform) if transform else None}|{resampling}".encode())
        dst = cache.derived_path(key, ".tif")
        if dst.exists():
            return dst
        dst.parent.mkdir(parents=True, exist_ok=True)
        integer = np.issubdtype(np.dtype(ds.dtypes[0]), np.integer)
        # GDAL's COG driver only copies, so stream the source (row strips: PNGs decode sequentially) into a tiled
        # intermediate first, then let the driver add overviews and lay the file out.
        profile = {
            "driver": "GTiff", "width": ds.width, "height": ds.height, "count": ds.count, "dtype": ds.dtypes[0],
            "crs": crs or ds.crs, "transform": transform or ds.transform, "nodata": ds.nodata, "tiled": True,
            "blockxsize": 512, "blockysize": 512, "compress": "deflate", "BIGTIFF": "IF_SAFER",
        }
        tmp = dst.with_name(dst.name + ".stage.tif")
        with rasterio.open(tmp, "w", **profile) as out:
            for row in range(0, ds.height, 512):
                win = Window(0, row, ds.width, min(512, ds.height - row))
                out.write(ds.read(window=win), window=win)
    part = dst.with_name(dst.name + ".part")
    try:
        rasterio.shutil.copy(
            tmp, part, driver="COG", COMPRESS="DEFLATE", PREDICTOR="2" if integer else "3", BLOCKSIZE="512",
            OVERVIEW_RESAMPLING=resampling.upper(), BIGTIFF="IF_SAFER", NUM_THREADS="1",
        )
        os.replace(part, dst)
    finally:
        tmp.unlink(missing_ok=True)
        part.unlink(missing_ok=True)
    return dst


def footprint_lonlat(raster: SourceRaster):
    """Valid-data polygon in lon/lat from the coarsest overview with >= 64 px, dilated by one pixel,
    simplified to half a pixel and snapped to 1e-7 degrees. None when the raster holds no data."""
    import pyproj
    import rasterio.features
    import shapely
    import shapely.geometry
    from scipy.ndimage import binary_dilation

    ds0 = _dataset(str(raster.path), None)
    level = None
    for i, f in enumerate(ds0.overviews(raster.bands[0])):
        if min(ds0.width, ds0.height) / f >= 64:
            level = i
    ds = ds0 if level is None else _dataset(str(raster.path), level)
    valid = raster.valid_mask(ds.read(list(raster.bands)).astype(np.float64))
    if not valid.any():
        return None
    valid = binary_dilation(valid, iterations=1)
    shapes = rasterio.features.shapes(valid.astype(np.uint8), mask=valid, transform=ds.transform)
    geom = shapely.union_all([shapely.geometry.shape(g) for g, v in shapes if v == 1])
    crs = pyproj.CRS.from_user_input(raster.file_crs())
    res = abs(ds.transform.a)
    if crs.is_geographic:
        tol = res / 2
    else:
        geom = shapely.segmentize(geom, res * 8)
        t = pyproj.Transformer.from_crs(crs, "EPSG:4326", always_xy=True)
        geom = shapely.transform(geom, lambda xy: np.column_stack(t.transform(xy[:, 0], xy[:, 1])))
        tol = res / M_PER_DEG / 2
    geom = shapely.set_precision(geom.simplify(tol), 1e-7)
    return None if geom.is_empty else geom
```

- [ ] **Step 4: Implement `sources/__init__.py`**

```python
"""Source adapter registry. A built-in id maps to its class; "module:Class" names any importable adapter
(the test suite's synthetic sources). Imports are lazy so this package stays light."""

from __future__ import annotations

import importlib

BUILTIN = {
    "dep3_13": "camsim_scene.sources.dep3_13:Dep313",
    "dep3_1m": "camsim_scene.sources.dep3_1m:Dep31m",
    "naip_pc": "camsim_scene.sources.naip_pc:NaipPc",
    "wc_s2": "camsim_scene.sources.wc_s2:WcS2",
    "etopo2022": "camsim_scene.sources.etopo2022:Etopo2022",
    "bmng": "camsim_scene.sources.bmng:Bmng",
    "worldcover": "camsim_scene.sources.worldcover:WorldCover",
}


class UnknownSource(KeyError):
    pass


def make_source(source_id: str, adapter: str | None = None, options: dict | None = None, http=None):
    name = adapter or source_id
    spec = BUILTIN.get(name, name)
    if ":" not in spec:
        raise UnknownSource(f"unknown source adapter {name!r} (built-ins: {sorted(BUILTIN)})")
    module, cls = spec.split(":", 1)
    src = getattr(importlib.import_module(module), cls)(options or {}, http)
    src.id = source_id
    return src
```

- [ ] **Step 5: Run the tests**

Run: `cd scripts/scene && uv run pytest tests/test_sources_base.py -v`
Expected: 13 passed. (The registry's built-in modules don't exist yet; only the module-path form is exercised.)

- [ ] **Step 6: Commit**

```bash
git add scripts/scene/camsim_scene/sources scripts/scene/tests/rasters.py scripts/scene/tests/test_sources_base.py
git commit -m "feat(scene): source interface, bilinear raster sampling, prepare, footprints"
```

---

### Task 10: 3DEP adapters (1/3″ seamless, 1 m project DEMs)

**Files:**
- Create: `scripts/scene/camsim_scene/sources/tnm.py`, `scripts/scene/camsim_scene/sources/dep3_13.py`, `scripts/scene/camsim_scene/sources/dep3_1m.py`
- Create: `scripts/scene/tests/fixtures/http/tnm_13.json`, `tnm_1m.json`, `wesm.json`
- Test: `scripts/scene/tests/test_sources_dep3.py`

**Interfaces:**
- Consumes: `sources.base.{SourceBase, Asset, Area, Layer, SourceRaster}`; `datum.geoid_datum`; `net.HttpError`.
- Produces: `tnm.TNM_PRODUCTS`, `tnm.tnm_products(http, bounds, dataset, page=1000) -> list[dict]`; `Dep313` (id `dep3_13`, terrain, `max_zoom` 14, `area_kind` "ring", datum `nad83_2011_navd88_geoid18`, option `as_of` "YYYYMMDD"); `Dep31m` (id `dep3_1m`, terrain, `max_zoom` 16, `area_kind` "bbox", per-asset `metadata["datum"]`, option `geoid_overrides: {project: "GEOID18"}`); `dep3_1m.WESM_QUERY`.

API facts (checked 2026-10-07): TNM `GET /api/v1/products?bbox=W,S,E,N&datasets=<name>&max=&offset=&outputFormat=JSON` returns `{"total", "items": [{"title", "publicationDate", "downloadURL", "boundingBox": {"minX","maxX","minY","maxY"}, "sizeInBytes"}]}`. 1/3″ items are dated `historical/` copies (`.../13/TIFF/historical/n34w118/USGS_13_n34w118_20260915.tif`). 1 m files are `.../1m/Projects/<project>/TIFF/USGS_1M_<zone>_x..y.._<project>.tif`, EPSG:269<zone>, nodata −999999, no geoid in the file. The project's geoid comes from the WESM index (`3DEPElevationIndex/MapServer/24`, fields `workunit, project, geoid, collect_end`). That query endpoint returned HTTP 400 for every query on 2026-10-07 (layer metadata answered); `geoid_overrides` lets a build proceed while it is down. Match a TNM project against WESM `project` or `workunit`.

- [ ] **Step 1: Write the fixtures**

`scripts/scene/tests/fixtures/http/tnm_13.json` (recorded 2026-10-07, trimmed to the fields used):

```json
{"total": 5, "items": [
 {"title": "USGS 1/3 Arc Second n34w118 20120201", "publicationDate": "2012-02-01", "sizeInBytes": 331541887, "downloadURL": "https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/13/TIFF/historical/n34w118/USGS_13_n34w118_20120201.tif", "boundingBox": {"minX": -118.00055555589358, "maxX": -116.99944444370556, "minY": 32.99944444360705, "maxY": 34.00055555579513}},
 {"title": "USGS 1/3 Arc Second n34w118 20250813", "publicationDate": "2025-08-13", "sizeInBytes": 352889091, "downloadURL": "https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/13/TIFF/historical/n34w118/USGS_13_n34w118_20250813.tif", "boundingBox": {"minX": -118.00055555589358, "maxX": -116.99944444370556, "minY": 32.99944444360705, "maxY": 34.00055555579513}},
 {"title": "USGS 1/3 Arc Second n34w118 20250826", "publicationDate": "2025-08-27", "sizeInBytes": 352493233, "downloadURL": "https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/13/TIFF/historical/n34w118/USGS_13_n34w118_20250826.tif", "boundingBox": {"minX": -118.00055555589358, "maxX": -116.99944444370556, "minY": 32.99944444360705, "maxY": 34.00055555579513}},
 {"title": "USGS 1/3 Arc Second n34w118 20260915", "publicationDate": "2026-09-15", "sizeInBytes": 351919765, "downloadURL": "https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/13/TIFF/historical/n34w118/USGS_13_n34w118_20260915.tif", "boundingBox": {"minX": -118.00055555589358, "maxX": -116.99944444370556, "minY": 32.99944444360705, "maxY": 34.00055555579513}},
 {"title": "USGS 13 arc-second n34w118 1 x 1 degree", "publicationDate": "2019-09-17", "sizeInBytes": 345547, "downloadURL": "https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/13/TIFF/historical/n34w118/USGS_13_n34w118_20190917.tif", "boundingBox": {"minX": -118.000555556, "maxX": -116.999444444, "minY": 32.9994444436, "maxY": 34.0005555558}}
]}
```

`scripts/scene/tests/fixtures/http/tnm_1m.json` (recorded 2026-10-07, trimmed):

```json
{"total": 2, "items": [
 {"title": "USGS 1 Meter 11 x46y368 CA_SanDiegoCo_D24", "publicationDate": "2026-09-15", "sizeInBytes": 231550972, "downloadURL": "https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/1m/Projects/CA_SanDiegoCo_D24/TIFF/USGS_1M_11_x46y368_CA_SanDiegoCo_D24.tif", "boundingBox": {"minX": -117.42944681599994, "maxX": -117.32175658099999, "minY": 33.16805754400008, "maxY": 33.258580119000044}},
 {"title": "USGS 1 Meter 11 x46y368 San_Diego_CA_2014_LiDAR", "publicationDate": "2024-04-26", "sizeInBytes": 233216779, "downloadURL": "https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/1m/Projects/San_Diego_CA_2014_LiDAR/TIFF/USGS_1M_11_x46y368_San_Diego_CA_2014_LiDAR.tif", "boundingBox": {"minX": -117.42944681599994, "maxX": -117.32175658099999, "minY": 33.16805754400008, "maxY": 33.258580119000044}}
]}
```

`scripts/scene/tests/fixtures/http/wesm.json` (hand-written in the ArcGIS JSON shape; values from `docs/realism/data-sources.md` 1.2):

```json
{"features": [
 {"attributes": {"project": "CA_SanDiegoCo_D24", "workunit": "CA_SanDiegoCo_1_D24", "geoid": "GEOID18", "collect_end": 1733011200000}},
 {"attributes": {"project": "CA_SanDiegoQL2_2014", "workunit": "San_Diego_CA_2014_LiDAR", "geoid": "GEOID12A", "collect_end": 1422748800000}}
]}
```

- [ ] **Step 2: Write the failing tests**

`scripts/scene/tests/test_sources_dep3.py`:

```python
import logging

import pytest

from camsim_scene.net import Http
from camsim_scene.sources import make_source
from camsim_scene.sources.base import Area, Layer
from camsim_scene.sources.dep3_1m import WESM_QUERY
from camsim_scene.sources.tnm import TNM_PRODUCTS
from fakes import FakeHttp, fixture_json

AREA = Area((-117.41, 33.20, -117.35, 33.25))


def tnm(name):
    return lambda params: fixture_json(name) if params["offset"] == 0 else {"total": fixture_json(name)["total"], "items": []}


def test_dep3_13_picks_newest_dated_copy():
    src = make_source("dep3_13", http=FakeHttp({TNM_PRODUCTS: tnm("tnm_13.json")}))
    assert src.layer == Layer.TERRAIN and src.max_zoom == 14
    (a,) = src.discover(AREA)
    assert a.id == "USGS_13_n34w118_20260915" and "/historical/n34w118/" in a.url and a.size == 351919765
    assert a.metadata["bbox"][0] == pytest.approx(-118.00055555589358) and src.version == "20260915"
    assert src.open(a.url, a).datum == "nad83_2011_navd88_geoid18"


def test_dep3_13_as_of_pins_an_older_copy():
    src = make_source("dep3_13", options={"as_of": "20251231"}, http=FakeHttp({TNM_PRODUCTS: tnm("tnm_13.json")}))
    assert src.discover(AREA)[0].id == "USGS_13_n34w118_20250826"


def test_dep3_1m_maps_geoids_orders_newest_first_and_skips_unknown(caplog):
    http = FakeHttp({TNM_PRODUCTS: tnm("tnm_1m.json"), WESM_QUERY: lambda params: fixture_json("wesm.json")})
    src = make_source("dep3_1m", http=http)
    with caplog.at_level(logging.WARNING):
        assets = src.discover(AREA)
    assert [a.id for a in assets] == ["USGS_1M_11_x46y368_CA_SanDiegoCo_D24"]
    a = assets[0]
    assert a.group == "CA_SanDiegoCo_D24" and a.metadata["datum"] == "nad83_2011_navd88_geoid18" and a.metadata["utm_zone"] == 11
    assert "San_Diego_CA_2014_LiDAR" in caplog.text and "GEOID12A" in caplog.text


def test_dep3_1m_geoid_override_includes_project_after_newer_one():
    http = FakeHttp({TNM_PRODUCTS: tnm("tnm_1m.json"), WESM_QUERY: lambda params: fixture_json("wesm.json")})
    src = make_source("dep3_1m", options={"geoid_overrides": {"San_Diego_CA_2014_LiDAR": "GEOID12B"}}, http=http)
    assets = src.discover(AREA)
    assert [a.group for a in assets] == ["CA_SanDiegoCo_D24", "San_Diego_CA_2014_LiDAR"]
    assert assets[1].metadata["datum"] == "nad83_2011_navd88_geoid12b" and assets[0].rank < assets[1].rank


@pytest.mark.network
def test_live_tnm_and_wesm_for_pendleton():
    http = Http()
    assert make_source("dep3_13", http=http).discover(AREA)
    one_m = make_source("dep3_1m", http=http).discover(AREA)
    assert any(a.metadata["datum"] == "nad83_2011_navd88_geoid18" for a in one_m)
```

- [ ] **Step 3: Run them to see them fail**

Run: `cd scripts/scene && uv run pytest tests/test_sources_dep3.py -v`
Expected: FAIL, `ModuleNotFoundError: camsim_scene.sources.dep3_1m`.

- [ ] **Step 4: Implement**

`scripts/scene/camsim_scene/sources/tnm.py`:

```python
"""The National Map Access API (no auth; pages of up to 1000 products)."""

from __future__ import annotations

TNM_PRODUCTS = "https://tnmaccess.nationalmap.gov/api/v1/products"


def tnm_products(http, bounds, dataset: str, page: int = 1000) -> list[dict]:
    items: list[dict] = []
    offset = 0
    while True:
        params = {
            "bbox": ",".join(f"{v:.6f}" for v in bounds),
            "datasets": dataset,
            "max": page,
            "offset": offset,
            "outputFormat": "JSON",
        }
        d = http.get_json(TNM_PRODUCTS, params)
        batch = d.get("items", [])
        items += batch
        offset += len(batch)
        if not batch or offset >= int(d.get("total", 0)):
            return items
```

`scripts/scene/camsim_scene/sources/dep3_13.py`:

```python
"""USGS 3DEP 1/3 arc-second seamless DEM (1 x 1 degree GeoTIFFs, EPSG:4269 + NAVD88, GEOID18). Uses the dated
`historical/` copies, so a manifest URL never changes content: the newest per cell, or the newest on or before
the `as_of` option (YYYYMMDD)."""

from __future__ import annotations

import re
from pathlib import Path

from .base import Area, Asset, Layer, SourceBase, SourceRaster
from .tnm import tnm_products

DATASET = "National Elevation Dataset (NED) 1/3 arc-second"
URL_RE = re.compile(r"/historical/([ns]\d{2}[ew]\d{3})/USGS_13_\1_(\d{8})\.tif$")


class Dep313(SourceBase):
    id = "dep3_13"
    layer = Layer.TERRAIN
    licence = "LicenseRef-PublicDomain-USGov"
    dataset = "USGS 3DEP 1/3 arc-second DEM"
    attribution = "U.S. Geological Survey, 3D Elevation Program (3DEP)"
    max_zoom = 14
    area_kind = "ring"
    datum = "nad83_2011_navd88_geoid18"

    def discover(self, area: Area) -> list[Asset]:
        as_of = self._options.get("as_of")
        best: dict[str, tuple[str, dict]] = {}
        for it in tnm_products(self.http, area.bounds, DATASET):
            m = URL_RE.search(it.get("downloadURL", ""))
            if not m:
                continue
            cell, date = m.groups()
            if as_of and date > as_of:
                continue
            if cell not in best or date > best[cell][0]:
                best[cell] = (date, it)
        assets = []
        for rank, cell in enumerate(sorted(best)):
            date, it = best[cell]
            bb = it["boundingBox"]
            assets.append(
                Asset(
                    id=f"USGS_13_{cell}_{date}", url=it["downloadURL"], size=it.get("sizeInBytes"), group=self.id, rank=rank,
                    metadata={"cell": cell, "date": date, "bbox": [bb["minX"], bb["minY"], bb["maxX"], bb["maxY"]]},
                )
            )
        self.version = max((a.metadata["date"] for a in assets), default="")
        return assets

    def open(self, path: Path, asset: Asset) -> SourceRaster:
        return SourceRaster(path=Path(path), datum=self.datum, nodata=-999999.0)
```

`scripts/scene/camsim_scene/sources/dep3_1m.py`:

```python
"""USGS 3DEP 1 m project DEMs (10 x 10 km UTM tiles, NAD83 + NAVD88 with the project's geoid). Projects are
groups, newest collection first; the geoid comes from the WESM index (or the `geoid_overrides` option,
{project: "GEOID18"}). A project whose geoid is unknown or has no vendored PROJ grid is skipped."""

from __future__ import annotations

import datetime as dt
import logging
import re
from collections import defaultdict
from pathlib import Path

from ..datum import geoid_datum
from ..net import HttpError
from .base import Area, Asset, Layer, SourceBase, SourceRaster
from .tnm import tnm_products

log = logging.getLogger(__name__)
DATASET = "Digital Elevation Model (DEM) 1 meter"
WESM_QUERY = "https://index.nationalmap.gov/arcgis/rest/services/3DEPElevationIndex/MapServer/24/query"
URL_RE = re.compile(r"/1m/Projects/([^/]+)/TIFF/(USGS_1M_(\d+)_x\d+y\d+_[^/]+)\.tif$")


class Dep31m(SourceBase):
    id = "dep3_1m"
    layer = Layer.TERRAIN
    licence = "LicenseRef-PublicDomain-USGov"
    dataset = "USGS 3DEP 1 m project DEMs"
    attribution = "U.S. Geological Survey, 3D Elevation Program (3DEP)"
    max_zoom = 16
    area_kind = "bbox"

    def _wesm(self, bounds) -> dict[str, dict]:
        d = self.http.get_json(
            WESM_QUERY,
            {
                "where": "1=1",
                "geometry": ",".join(f"{v:.6f}" for v in bounds),
                "geometryType": "esriGeometryEnvelope",
                "inSR": "4326",
                "spatialRel": "esriSpatialRelIntersects",
                "outFields": "project,workunit,geoid,collect_end",
                "returnGeometry": "false",
                "f": "json",
            },
        )
        if "error" in d:
            raise HttpError(f"WESM query failed: {d['error']} (set sources.dep3_1m.geoid_overrides to proceed)")
        out: dict[str, dict] = {}
        for f in d.get("features", []):
            a = f["attributes"]
            for key in {a.get("project"), a.get("workunit")} - {None}:
                p = out.setdefault(key, {"geoids": set(), "collect_end": 0})
                if a.get("geoid"):
                    p["geoids"].add(a["geoid"])
                p["collect_end"] = max(p["collect_end"], int(a.get("collect_end") or 0))
        return out

    def discover(self, area: Area) -> list[Asset]:
        overrides = self._options.get("geoid_overrides", {})
        wesm = self._wesm(area.bounds)
        by_project: dict[str, list] = defaultdict(list)
        for it in tnm_products(self.http, area.bounds, DATASET):
            m = URL_RE.search(it.get("downloadURL", ""))
            if m:
                by_project[m.group(1)].append((m, it))

        def collected(p: str) -> str:
            ms = wesm.get(p, {}).get("collect_end", 0)
            if ms:
                return dt.datetime.fromtimestamp(ms / 1000, dt.UTC).date().isoformat()
            return max(it.get("publicationDate", "") for _, it in by_project[p])

        assets: list[Asset] = []
        for p in sorted(by_project, key=lambda p: (collected(p), p), reverse=True):
            geoids = wesm.get(p, {}).get("geoids", set())
            geoid = overrides.get(p) or (next(iter(geoids)) if len(geoids) == 1 else None)
            did = geoid_datum(geoid) if geoid else None
            if did is None:
                log.warning("dep3_1m: skipping project %s: geoid %s is unknown or has no PROJ grid (%s)", p,
                            geoid or sorted(geoids), "set sources.dep3_1m.geoid_overrides if you know it")
                continue
            for m, it in sorted(by_project[p], key=lambda t: t[0].group(2)):
                bb = it["boundingBox"]
                assets.append(
                    Asset(
                        id=m.group(2), url=it["downloadURL"], size=it.get("sizeInBytes"), group=p, rank=len(assets),
                        metadata={
                            "project": p, "geoid": geoid, "datum": did, "utm_zone": int(m.group(3)), "collected": collected(p),
                            "bbox": [bb["minX"], bb["minY"], bb["maxX"], bb["maxY"]],
                        },
                    )
                )
        self.version = max((it.get("publicationDate", "") for v in by_project.values() for _, it in v), default="")
        return assets

    def open(self, path: Path, asset: Asset) -> SourceRaster:
        return SourceRaster(path=Path(path), datum=asset.metadata["datum"], nodata=-999999.0)
```

- [ ] **Step 5: Run the tests**

Run: `cd scripts/scene && uv run pytest tests/test_sources_dep3.py -v`
Expected: 4 passed, 1 skipped (network).

- [ ] **Step 6: Check the live services once**

Run: `cd scripts/scene && uv run pytest tests/test_sources_dep3.py -v --network -k live`
Expected: PASS. If WESM still answers HTTP 400, record that in the commit message, inspect `https://index.nationalmap.gov/arcgis/rest/services/3DEPElevationIndex/MapServer/24?f=json` for changed field names, and re-run later. Don't change the unit fixtures to match a broken service.

- [ ] **Step 7: Commit**

```bash
git add scripts/scene/camsim_scene/sources/tnm.py scripts/scene/camsim_scene/sources/dep3_13.py scripts/scene/camsim_scene/sources/dep3_1m.py \
  scripts/scene/tests/fixtures/http scripts/scene/tests/test_sources_dep3.py
git commit -m "feat(scene): 3DEP 1/3 arc-second and 1 m DEM adapters (dated URLs, WESM geoids)"
```

---

### Task 11: NAIP (Planetary Computer) and WorldCover S2 composite adapters

**Files:**
- Create: `scripts/scene/camsim_scene/sources/naip_pc.py`, `scripts/scene/camsim_scene/sources/wc_s2.py`
- Create: `scripts/scene/tests/fixtures/http/stac_naip.json`
- Test: `scripts/scene/tests/test_sources_imagery.py`

**Interfaces:**
- Consumes: `sources.base.*`, `licences.LicenceError`, `net.HttpError`.
- Produces: `naip_pc.STAC_SEARCH`, `naip_pc.SAS_TOKEN`, `naip_pc.PcSigner(http, now=time.time)` (callable `(url, force=False) -> signed url`), `NaipPc` (id `naip_pc`, imagery, `max_zoom` 17, `area_kind` "bbox", datum `nad83_2011`, option `year`); `wc_s2.URL`, `wc_s2.cell_name(lat_i, lon_i) -> str`, `WcS2` (id `wc_s2`, imagery, `max_zoom` 13, `area_kind` "ring", datum `wgs84`, licence `CC-BY-4.0`, decoder `s2_reflectance`).

API facts (checked 2026-10-07): STAC search `POST https://planetarycomputer.microsoft.com/api/stac/v1/search` with `{"collections": ["naip"], "bbox": [...], "limit": N}`; items carry `properties["naip:year"]` (string), `proj:epsg` (26911), `bbox`, and an unsigned `assets.image.href` on `naipeuwest.blob.core.windows.net/naip/...` (4-band RGBN COG, nodata 0). Tokens: `GET https://planetarycomputer.microsoft.com/api/sas/v1/token/{account}/{container}` → `{"token": "...", "msft:expiry": "<ISO 8601>"}`, appended as the query string. WC S2: `https://esa-worldcover-s2.s3.eu-central-1.amazonaws.com/rgbnir/2021/N33/ESA_WorldCover_10m_2021_v200_N33W118_S2RGBNIR.tif` (1° cells named by their SW corner; ~486 MB each; 404 over open ocean; 4 × uint16 B04/B03/B02/B08, DN × 1e-4 reflectance, nodata 0, tag `license=CC-BY 4.0 - https://creativecommons.org/licenses/by/4.0/`).

- [ ] **Step 1: Write the fixture**

`scripts/scene/tests/fixtures/http/stac_naip.json`:

```json
{"type": "FeatureCollection", "links": [], "features": [
 {"id": "ca_m_3311754_nw_11_060_20220530", "bbox": [-117.377413, 33.185508, -117.310127, 33.251989],
  "properties": {"naip:year": "2022", "proj:epsg": 26911, "datetime": "2022-05-30T16:00:00Z"},
  "assets": {"image": {"href": "https://naipeuwest.blob.core.windows.net/naip/v002/ca/2022/ca_060cm_2022/33117/m_3311754_nw_11_060_20220530.tif"}}},
 {"id": "ca_m_3311754_nw_11_060_20200521", "bbox": [-117.377413, 33.185508, -117.310127, 33.251989],
  "properties": {"naip:year": "2020", "proj:epsg": 26911, "datetime": "2020-05-21T16:00:00Z"},
  "assets": {"image": {"href": "https://naipeuwest.blob.core.windows.net/naip/v002/ca/2020/ca_060cm_2020/33117/m_3311754_nw_11_060_20200521.tif"}}}
]}
```

- [ ] **Step 2: Write the failing tests**

`scripts/scene/tests/test_sources_imagery.py`:

```python
import numpy as np
import pytest

from camsim_scene.licences import LicenceError
from camsim_scene.sources import make_source
from camsim_scene.sources.base import Area, Asset
from camsim_scene.sources.naip_pc import SAS_TOKEN, STAC_SEARCH, PcSigner
from camsim_scene.sources.wc_s2 import URL, cell_name
from fakes import FakeHttp, fixture_json
from rasters import write_geotiff

AREA = Area((-117.41, 33.20, -117.35, 33.25))
TOKEN_URL = SAS_TOKEN.format(account="naipeuwest", container="naip")


def test_naip_discovery_filters_year_and_keeps_unsigned_urls():
    src = make_source("naip_pc", options={"year": "2022"}, http=FakeHttp({STAC_SEARCH: lambda body: fixture_json("stac_naip.json")}))
    (a,) = src.discover(AREA)
    assert a.id == "ca_m_3311754_nw_11_060_20220530" and "?" not in a.url and a.metadata["epsg"] == 26911
    ras = src.open("x.tif", a)
    assert ras.bands == (1, 2, 3) and ras.nodata_rule == "all_zero" and ras.datum == "nad83_2011"


def test_naip_follows_post_next_links():
    pages = [
        {"features": fixture_json("stac_naip.json")["features"][:1],
         "links": [{"rel": "next", "href": STAC_SEARCH, "method": "POST", "body": {"token": "p2"}}]},
        {"features": [], "links": []},
    ]
    http = FakeHttp({STAC_SEARCH: lambda body: pages[1] if body.get("token") == "p2" else pages[0]})
    assert len(make_source("naip_pc", http=http).discover(AREA)) == 1


def test_pc_signer_refreshes_on_force():
    tokens = iter([{"token": "sig=A", "msft:expiry": "2026-10-08T00:00:00Z"}, {"token": "sig=B", "msft:expiry": "2026-10-08T01:00:00Z"}])
    http = FakeHttp({TOKEN_URL: lambda params: next(tokens)})
    signer = PcSigner(http, now=lambda: 1_791_000_000.0)  # 2026-10-03
    url = "https://naipeuwest.blob.core.windows.net/naip/v002/x.tif"
    assert signer(url) == url + "?sig=A" and signer(url) == url + "?sig=A"
    assert signer(url, force=True) == url + "?sig=B"
    assert len(http.calls) == 2


def test_pc_signer_refreshes_near_expiry():
    tokens = iter([{"token": "sig=A", "msft:expiry": "2026-10-08T00:00:00Z"}, {"token": "sig=B", "msft:expiry": "2026-10-09T00:00:00Z"}])
    clock = [1_791_000_000.0]
    signer = PcSigner(FakeHttp({TOKEN_URL: lambda params: next(tokens)}), now=lambda: clock[0])
    url = "https://naipeuwest.blob.core.windows.net/naip/a.tif"
    assert signer(url).endswith("sig=A")
    clock[0] = 1_791_417_600.0 - 60  # one minute before the first expiry
    assert signer(url).endswith("sig=B")


def test_wc_s2_cells_and_urls():
    assert cell_name(33, -118) == "N33W118" and cell_name(-5, 7) == "S05E007"
    assert URL.format(lat="N33", name="N33W118") == (
        "https://esa-worldcover-s2.s3.eu-central-1.amazonaws.com/rgbnir/2021/N33/ESA_WorldCover_10m_2021_v200_N33W118_S2RGBNIR.tif"
    )
    routes = {("HEAD", URL.format(lat=n[:3], name=n)): (200, 486_000_000) for n in ("N33W118", "N33W117", "N32W117")}
    assets = make_source("wc_s2", http=FakeHttp(routes)).discover(Area((-117.9, 32.5, -116.5, 33.5)))
    assert [a.id for a in assets] == ["N32W117", "N33W118", "N33W117"]  # N32W118 is a 404 (ocean)
    assert assets[1].metadata["bbox"] == [-118, 33, -117, 34]


def test_wc_s2_refuses_a_file_without_cc_by(tmp_path):
    src = make_source("wc_s2", http=FakeHttp({}))
    ok = write_geotiff(tmp_path / "ok.tif", np.ones((4, 64, 64), np.uint16), -118, 34, 1 / 12000,
                       tags={"license": "CC-BY 4.0 - https://creativecommons.org/licenses/by/4.0/"})
    assert src.prepare(ok, Asset("N33W118", "u", sha256="00" * 32), cache=None) is None
    bad = write_geotiff(tmp_path / "bad.tif", np.ones((4, 64, 64), np.uint16), -118, 34, 1 / 12000, tags={"license": "CC-BY-NC"})
    with pytest.raises(LicenceError):
        src.prepare(bad, Asset("N33W118", "u", sha256="00" * 32), cache=None)
```

- [ ] **Step 3: Run them to see them fail**

Run: `cd scripts/scene && uv run pytest tests/test_sources_imagery.py -v`
Expected: FAIL, `ModuleNotFoundError`.

- [ ] **Step 4: Implement `naip_pc.py`**

```python
"""USDA NAIP from Microsoft Planetary Computer (STAC `naip`; CA newest there is 2022, 0.6 m RGBN COGs in
NAD83 UTM, nodata 0 in all bands). Manifest URLs are unsigned; fetch signs them with a SAS token and re-signs
when a token is refused or about to expire."""

from __future__ import annotations

import datetime as dt
import logging
import time
import urllib.parse
from pathlib import Path

from ..net import Http
from .base import Area, Asset, Layer, SourceBase, SourceRaster

log = logging.getLogger(__name__)
STAC_SEARCH = "https://planetarycomputer.microsoft.com/api/stac/v1/search"
SAS_TOKEN = "https://planetarycomputer.microsoft.com/api/sas/v1/token/{account}/{container}"
REFRESH_BEFORE_S = 300


class PcSigner:
    def __init__(self, http, now=time.time):
        self.http, self.now = http, now
        self._tokens: dict[tuple[str, str], tuple[str, float]] = {}

    def __call__(self, url: str, force: bool = False) -> str:
        p = urllib.parse.urlparse(url)
        key = (p.netloc.split(".")[0], p.path.lstrip("/").split("/")[0])
        tok = self._tokens.get(key)
        if force or tok is None or tok[1] - REFRESH_BEFORE_S <= self.now():
            d = self.http.get_json(SAS_TOKEN.format(account=key[0], container=key[1]))
            expiry = dt.datetime.fromisoformat(d["msft:expiry"].replace("Z", "+00:00")).timestamp()
            tok = (d["token"], expiry)
            self._tokens[key] = tok
        return f"{url}?{tok[0]}"


class NaipPc(SourceBase):
    id = "naip_pc"
    layer = Layer.IMAGERY
    licence = "LicenseRef-PublicDomain-USGov"
    dataset = "USDA NAIP (Microsoft Planetary Computer)"
    attribution = "USDA Farm Production and Conservation - Business Center, Geospatial Enterprise Operations (NAIP)"
    max_zoom = 17
    area_kind = "bbox"
    datum = "nad83_2011"

    def __init__(self, options=None, http=None):
        super().__init__(options, http)
        self._signer: PcSigner | None = None

    def discover(self, area: Area) -> list[Asset]:
        year = str(self._options.get("year", "2022"))
        url, body = STAC_SEARCH, {"collections": ["naip"], "bbox": list(area.bounds), "limit": 250}
        items: list[dict] = []
        while True:
            d = self.http.post_json(url, body)
            items += d.get("features", [])
            nxt = next((link for link in d.get("links", []) if link.get("rel") == "next"), None)
            if nxt is None:
                break
            url = nxt["href"]
            if nxt.get("method", "GET").upper() == "POST":
                body = {**body, **nxt.get("body", {})}
            else:
                d = self.http.get_json(url)
                items += d.get("features", [])
                break
        items = sorted((i for i in items if str(i["properties"].get("naip:year")) == year), key=lambda i: i["id"])
        if not items:
            log.warning("naip_pc: no NAIP %s items in %s", year, area.bounds)
        self.version = year
        return [
            Asset(
                id=i["id"], url=i["assets"]["image"]["href"], group=self.id, rank=r,
                metadata={"bbox": i["bbox"], "epsg": i["properties"].get("proj:epsg"), "datetime": i["properties"].get("datetime")},
            )
            for r, i in enumerate(items)
        ]

    def sign(self, url: str, force: bool = False) -> str:
        if self._signer is None:
            self._signer = PcSigner(self.http or Http())
        return self._signer(url, force)

    def open(self, path: Path, asset: Asset) -> SourceRaster:
        return SourceRaster(path=Path(path), datum=self.datum, bands=(1, 2, 3), nodata_rule="all_zero")
```

- [ ] **Step 5: Implement `wc_s2.py`**

```python
"""ESA WorldCover 2021 Sentinel-2 RGBNIR median composite (10 m, 1 x 1 degree COGs, CC BY 4.0): the ring
imagery (z9-10) and the fill where NAIP stops. No tiles over open ocean (404). Each file states its licence
in a GeoTIFF tag; prepare() refuses one that doesn't say CC BY 4.0."""

from __future__ import annotations

import math
from pathlib import Path

from ..licences import LicenceError
from ..net import HttpError
from .base import Area, Asset, Layer, SourceBase, SourceRaster

URL = "https://esa-worldcover-s2.s3.eu-central-1.amazonaws.com/rgbnir/2021/{lat}/ESA_WorldCover_10m_2021_v200_{name}_S2RGBNIR.tif"
ATTRIBUTION = (
    "© ESA WorldCover project 2021 / Contains modified Copernicus Sentinel data (2021) processed by ESA WorldCover consortium"
)


def cell_name(lat_i: int, lon_i: int) -> str:
    return f"{'N' if lat_i >= 0 else 'S'}{abs(lat_i):02d}{'E' if lon_i >= 0 else 'W'}{abs(lon_i):03d}"


class WcS2(SourceBase):
    id = "wc_s2"
    layer = Layer.IMAGERY
    licence = "CC-BY-4.0"
    dataset = "ESA WorldCover 2021 Sentinel-2 RGBNIR composite"
    version = "2021 v200"
    attribution = ATTRIBUTION
    max_zoom = 13
    area_kind = "ring"
    datum = "wgs84"

    def discover(self, area: Area) -> list[Asset]:
        w, s, e, n = area.bounds
        assets = []
        for lat_i in range(math.floor(s), math.ceil(n)):
            for lon_i in range(math.floor(w), math.ceil(e)):
                name = cell_name(lat_i, lon_i)
                url = URL.format(lat=name[:3], name=name)
                status, size = self.http.head(url)
                if status == 404:
                    continue
                if status != 200:
                    raise HttpError(f"HEAD {url}: HTTP {status}")
                assets.append(
                    Asset(id=name, url=url, size=size, group=self.id, rank=len(assets), metadata={"bbox": [lon_i, lat_i, lon_i + 1, lat_i + 1]})
                )
        return assets

    def prepare(self, path: Path, asset: Asset, cache) -> Path | None:
        import rasterio

        with rasterio.open(path) as ds:
            lic = ds.tags().get("license", "")
        if not lic.startswith("CC-BY 4.0"):
            raise LicenceError(f"wc_s2 {asset.id}: licence tag {lic!r}, expected CC-BY 4.0")
        return super().prepare(path, asset, cache)

    def open(self, path: Path, asset: Asset) -> SourceRaster:
        return SourceRaster(path=Path(path), datum=self.datum, bands=(1, 2, 3), nodata=0, nodata_rule="all_zero", decode="s2_reflectance")
```

`wc_s2.prepare` calls `prepare_raster` for an already-tiled COG with overviews, which returns `None` before touching the cache, so the test's `cache=None` is safe.

- [ ] **Step 6: Run the tests**

Run: `cd scripts/scene && uv run pytest tests/test_sources_imagery.py -v`
Expected: 6 passed.

- [ ] **Step 7: Commit**

```bash
git add scripts/scene/camsim_scene/sources/naip_pc.py scripts/scene/camsim_scene/sources/wc_s2.py \
  scripts/scene/tests/fixtures/http/stac_naip.json scripts/scene/tests/test_sources_imagery.py
git commit -m "feat(scene): NAIP (Planetary Computer, SAS re-signing) and WorldCover S2 adapters"
```

---

### Task 12: Global base adapters (ETOPO 2022, Blue Marble NG)

**Files:**
- Create: `scripts/scene/camsim_scene/sources/etopo2022.py`, `scripts/scene/camsim_scene/sources/bmng.py`
- Test: `scripts/scene/tests/test_sources_global.py`

**Interfaces:**
- Consumes: `sources.base.*`, `net.HttpError`.
- Produces: `etopo2022.SURFACE`, `etopo2022.GEOID`, `Etopo2022` (id `etopo2022`, terrain, `max_zoom` 8, `global_coverage`, `area_kind` "globe", datum `wgs84_egm2008_raster`; assets `ETOPO_2022_v1_30s_N90W180_surface` (role data) and `ETOPO_2022_v1_30s_N90W180_geoid` (role geoid); `open()` sets `vertical_asset="<id>/ETOPO_2022_v1_30s_N90W180_geoid"`); `bmng.MONTH_RECORDS`, `bmng.url(month, tile)`, `bmng.tile_origin(tile) -> (west, north)`, `bmng.RES`, `Bmng` (id `bmng`, imagery, `max_zoom` 8, `global_coverage`, datum `wgs84`, option `month`).

URLs (HTTP 200 checked 2026-10-07): ETOPO `https://www.ngdc.noaa.gov/mgg/global/relief/ETOPO2022/data/30s/30s_surface_elev_gtif/ETOPO_2022_v1_30s_N90W180_surface.tif` (1.59 GB, float32, nodata −99999, tiled 256, **no overviews**) and `.../30s_geoid_gtif/ETOPO_2022_v1_30s_N90W180_geoid.tif` (1.45 GB). Blue Marble: `https://eoimages.gsfc.nasa.gov/images/imagerecords/73000/<record>/world.topo.bathy.2004MM.3x21600x21600.<tile>.png` with records Jan–Dec 73580, 73605, 73630, 73655, 73701, 73726, 73751, 73776, 73801, 73826, 73884, 73909, tiles A1…D2 (columns A–D from 180° W in 90° steps, row 1 north of the equator, row 2 south), ~285 MB each, not georeferenced.

- [ ] **Step 1: Write the failing tests**

`scripts/scene/tests/test_sources_global.py`:

```python
import numpy as np
import rasterio
from PIL import Image

from camsim_scene.sources import bmng, etopo2022, make_source
from camsim_scene.sources.base import Area, Asset
from fakes import FakeHttp
from test_sources_base import StubCache

GLOBE = Area((-180.0, -90.0, 180.0, 90.0))


def test_etopo_has_surface_and_geoid_assets():
    http = FakeHttp({("HEAD", etopo2022.SURFACE): (200, 1585813987), ("HEAD", etopo2022.GEOID): (200, 1446300529)})
    src = make_source("etopo2022", http=http)
    assets = src.discover(GLOBE)
    assert [(a.id, a.role) for a in assets] == [
        ("ETOPO_2022_v1_30s_N90W180_surface", "data"), ("ETOPO_2022_v1_30s_N90W180_geoid", "geoid")]
    ras = src.open("x.tif", assets[0])
    assert ras.datum == "wgs84_egm2008_raster" and ras.clamp_edges and ras.nodata == -99999.0
    assert ras.vertical_asset == "etopo2022/ETOPO_2022_v1_30s_N90W180_geoid"
    assert src.global_coverage and src.max_zoom == 8


def test_bmng_urls_and_tile_origins():
    assert bmng.url(7, "A1") == "https://eoimages.gsfc.nasa.gov/images/imagerecords/73000/73751/world.topo.bathy.200407.3x21600x21600.A1.png"
    assert bmng.url(11, "D2").endswith("/73884/world.topo.bathy.200411.3x21600x21600.D2.png")
    assert bmng.tile_origin("A1") == (-180.0, 90.0) and bmng.tile_origin("D2") == (90.0, 0.0)
    routes = {("HEAD", bmng.url(1, t)): (200, 285_000_000) for t in bmng.TILES}
    src = make_source("bmng", options={"month": 1}, http=FakeHttp(routes))
    assets = src.discover(GLOBE)
    assert len(assets) == 8 and assets[0].metadata["bbox"] == [-180.0, 0.0, -90.0, 90.0] and src.version == "2004-01"


def test_bmng_prepare_georeferences_the_png(tmp_path):
    png = tmp_path / "b2.png"
    Image.fromarray(np.full((16, 16, 3), 9, np.uint8)).save(png)
    src = make_source("bmng", options={"month": 7}, http=FakeHttp({}))
    asset = Asset("world.topo.bathy.200407.B2", "u", sha256="12" * 32, metadata={"tile": "B2"})
    out = src.prepare(png, asset, StubCache(tmp_path))
    with rasterio.open(out) as ds:
        assert ds.crs.to_epsg() == 4326 and ds.transform.c == -90.0 and ds.transform.f == 0.0 and ds.transform.a == bmng.RES
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd scripts/scene && uv run pytest tests/test_sources_global.py -v`
Expected: FAIL, `ImportError`.

- [ ] **Step 3: Implement `etopo2022.py`**

```python
"""NOAA ETOPO 2022 30 arc-second surface elevation (land + bathymetry, EGM2008 heights) with ETOPO's own
geoid-height grid for the conversion to ellipsoid heights. Global base terrain to z8 and the fallback
everywhere. Public domain (US Government work); NCEI asks for the DOI citation."""

from __future__ import annotations

from pathlib import Path

from ..net import HttpError
from .base import Area, Asset, Layer, SourceBase, SourceRaster

BASE = "https://www.ngdc.noaa.gov/mgg/global/relief/ETOPO2022/data/30s"
SURFACE = f"{BASE}/30s_surface_elev_gtif/ETOPO_2022_v1_30s_N90W180_surface.tif"
GEOID = f"{BASE}/30s_geoid_gtif/ETOPO_2022_v1_30s_N90W180_geoid.tif"
GEOID_ID = "ETOPO_2022_v1_30s_N90W180_geoid"


class Etopo2022(SourceBase):
    id = "etopo2022"
    layer = Layer.TERRAIN
    licence = "LicenseRef-PublicDomain-USGov"
    dataset = "NOAA ETOPO 2022 30 arc-second surface elevation"
    version = "v1"
    attribution = "NOAA National Centers for Environmental Information (2022): ETOPO 2022 Global Relief Model, doi:10.25921/fd45-gt74"
    max_zoom = 8
    global_coverage = True
    area_kind = "globe"
    datum = "wgs84_egm2008_raster"

    def discover(self, area: Area) -> list[Asset]:
        out = []
        for rank, (aid, url, role) in enumerate(
            [("ETOPO_2022_v1_30s_N90W180_surface", SURFACE, "data"), (GEOID_ID, GEOID, "geoid")]
        ):
            status, size = self.http.head(url)
            if status != 200:
                raise HttpError(f"HEAD {url}: HTTP {status}")
            out.append(Asset(id=aid, url=url, size=size, group=self.id, rank=rank, role=role, metadata={"bbox": [-180.0, -90.0, 180.0, 90.0]}))
        return out

    def open(self, path: Path, asset: Asset) -> SourceRaster:
        return SourceRaster(
            path=Path(path), datum=self.datum, nodata=-99999.0, clamp_edges=True, vertical_asset=f"{self.id}/{GEOID_ID}"
        )
```

- [ ] **Step 4: Implement `bmng.py`**

```python
"""NASA Blue Marble Next Generation, topography + bathymetry, 500 m (2004 monthly composites, eight 90 x 90
degree PNG tiles of 21600 x 21600). Global base imagery to z8. The PNGs carry no georeferencing; prepare()
writes tiled GeoTIFFs with overviews (EPSG:4326, 1/240 degree pixels)."""

from __future__ import annotations

from pathlib import Path

from ..net import HttpError
from .base import Area, Asset, Layer, SourceBase, SourceRaster, prepare_raster

MONTH_RECORDS = {1: 73580, 2: 73605, 3: 73630, 4: 73655, 5: 73701, 6: 73726, 7: 73751, 8: 73776, 9: 73801, 10: 73826, 11: 73884, 12: 73909}
TILES = ("A1", "A2", "B1", "B2", "C1", "C2", "D1", "D2")
RES = 90.0 / 21600


def url(month: int, tile: str) -> str:
    rec = MONTH_RECORDS[month]
    return (
        f"https://eoimages.gsfc.nasa.gov/images/imagerecords/{rec // 1000 * 1000}/{rec}/"
        f"world.topo.bathy.2004{month:02d}.3x21600x21600.{tile}.png"
    )


def tile_origin(tile: str) -> tuple[float, float]:
    """(west, north) of a tile: columns A-D from 180 W in 90 degree steps; row 1 north of the equator."""
    return -180.0 + 90.0 * "ABCD".index(tile[0]), 90.0 - 90.0 * (int(tile[1]) - 1)


class Bmng(SourceBase):
    id = "bmng"
    layer = Layer.IMAGERY
    licence = "LicenseRef-PublicDomain-USGov"
    dataset = "NASA Blue Marble Next Generation (topography and bathymetry), 500 m"
    attribution = "NASA Earth Observatory / Reto Stöckli, Blue Marble Next Generation"
    max_zoom = 8
    global_coverage = True
    area_kind = "globe"
    datum = "wgs84"

    def discover(self, area: Area) -> list[Asset]:
        month = int(self._options.get("month", 7))
        self.version = f"2004-{month:02d}"
        out = []
        for rank, tile in enumerate(TILES):
            u = url(month, tile)
            status, size = self.http.head(u)
            if status != 200:
                raise HttpError(f"HEAD {u}: HTTP {status}")
            w, n = tile_origin(tile)
            out.append(
                Asset(id=f"world.topo.bathy.2004{month:02d}.{tile}", url=u, size=size, group=self.id, rank=rank,
                      metadata={"tile": tile, "month": month, "bbox": [w, n - 90.0, w + 90.0, n]})
            )
        return out

    def prepare(self, path: Path, asset: Asset, cache) -> Path | None:
        from rasterio.transform import Affine

        w, n = tile_origin(asset.metadata["tile"])
        return prepare_raster(path, asset, cache, crs="EPSG:4326", transform=Affine(RES, 0.0, w, 0.0, -RES, n))

    def open(self, path: Path, asset: Asset) -> SourceRaster:
        return SourceRaster(path=Path(path), datum=self.datum, bands=(1, 2, 3), clamp_edges=True)
```

- [ ] **Step 5: Run the tests**

Run: `cd scripts/scene && uv run pytest tests/test_sources_global.py -v`
Expected: 3 passed.

- [ ] **Step 6: Commit**

```bash
git add scripts/scene/camsim_scene/sources/etopo2022.py scripts/scene/camsim_scene/sources/bmng.py scripts/scene/tests/test_sources_global.py
git commit -m "feat(scene): ETOPO 2022 and Blue Marble NG global base adapters"
```

---

### Task 13: WorldCover adapter, land-cover layer, `fetch_worldcover.py` wrapper

**Files:**
- Create: `scripts/scene/camsim_scene/sources/worldcover.py`, `scripts/scene/camsim_scene/layers/__init__.py`, `scripts/scene/camsim_scene/layers/landcover.py`
- Modify: `scripts/landcover/fetch_worldcover.py` (becomes a wrapper)
- Test: `scripts/scene/tests/test_landcover.py`; existing `scripts/tests/test_fetch_worldcover.py` must pass unchanged.

**Interfaces:**
- Consumes: `sources.base.{SourceBase, Asset, Area, Layer, SourceRaster}`, `net.HttpError`.
- Produces (`sources.worldcover`): `COG_URL`, `SOURCE`, `LICENCE`, `ATTRIBUTION`, `CELLS_PER_DEG = 12000`, `TILE_DEG = 0.05`, `TILE_PX = 600`, `TILES_PER_COG = 60`, `cog_name(i, j)`, `cog_offset(i, j)`, `WorldCover` (id `worldcover`, landcover, licence `CC-BY-4.0`, `area_kind` "ring").
- Produces (`layers.landcover`): `FORMAT = "camsim-landcover-1"`; `Reader = Callable[[str, int, int], np.ndarray | None]`; `tile_range`, `tiles_for_bbox`, `tile_filename`, `read_cog_window`, `write_tile`, `build_index`, `fetch(bbox, out, reader=read_cog_window, cut_by="scripts/landcover/fetch_worldcover.py") -> dict`, `local_reader(paths: dict[str, Path]) -> Reader`. Same behaviour and output as today's `fetch_worldcover.py`.
- `layers/__init__.py` and `sources/worldcover.py` import only numpy/Pillow at module level (the wrapper runs under `uv run --with numpy --with pillow`).

- [ ] **Step 1: Write the failing test**

`scripts/scene/tests/test_landcover.py`:

```python
import json

import numpy as np
import rasterio
from rasterio.transform import from_origin
from rasterio.windows import Window

from camsim_scene.layers import landcover
from camsim_scene.sources import make_source
from camsim_scene.sources.base import Area
from camsim_scene.sources.worldcover import COG_URL, cog_name, cog_offset
from fakes import FakeHttp


def test_worldcover_discovers_the_cogs_of_the_ring():
    names = ["N30W120", "N33W120", "N30W117", "N33W117"]
    routes = {("HEAD", COG_URL.format(name=n)): (200, 100_000_000) for n in names if n != "N30W120"}
    assets = make_source("worldcover", http=FakeHttp(routes)).discover(Area((-118.7, 32.29, -116.16, 34.42)))
    assert sorted(a.id for a in assets) == ["N30W117", "N33W117", "N33W120"]


def test_local_reader_cuts_the_same_window_as_the_remote_reader(tmp_path):
    i, j = 755, -2450  # +37.75_-122.50, inside N36W123
    cog = tmp_path / "N36W123.tif"
    profile = dict(driver="GTiff", width=36000, height=36000, count=1, dtype="uint8", crs="EPSG:4326",
                   transform=from_origin(-123.0, 39.0, 1 / 12000, 1 / 12000), tiled=True, blockxsize=512, blockysize=512,
                   compress="deflate", SPARSE_OK="TRUE")
    pattern = ((np.arange(600)[:, None] + 3 * np.arange(600)[None, :]) % 101).astype(np.uint8)
    row, col = cog_offset(i, j)
    with rasterio.open(cog, "w", **profile) as ds:
        ds.write(pattern, 1, window=Window(col, row, 600, 600))
    reader = landcover.local_reader({COG_URL.format(name=cog_name(i, j)): cog})
    index = landcover.fetch((-122.50, 37.75, -122.45, 37.80), tmp_path / "lc", reader=reader, cut_by="camsim-scene")
    assert index["tiles"] == [{"file": "+37.75_-122.50.png", "lat_index": 755, "lon_index": -2450}]
    assert json.loads((tmp_path / "lc" / "index.json").read_text())["format"] == "camsim-landcover-1"
    from PIL import Image

    assert np.array_equal(np.asarray(Image.open(tmp_path / "lc" / "+37.75_-122.50.png")), pattern)
    assert reader("https://missing", 0, 0) is None
```

- [ ] **Step 2: Run it to see it fail**

Run: `cd scripts/scene && uv run pytest tests/test_landcover.py -v`
Expected: FAIL, `ModuleNotFoundError`.

- [ ] **Step 3: Implement `sources/worldcover.py`**

Move `COG_URL`, `SOURCE`, `LICENCE`, `ATTRIBUTION`, `CELLS_PER_DEG`, `TILE_DEG`, `TILE_PX`, `TILES_PER_COG`, `cog_name` and `cog_offset` verbatim from `scripts/landcover/fetch_worldcover.py` into this module, then add the adapter:

```python
"""ESA WorldCover 10 m 2021 v200 land cover (3 x 3 degree COGs, CC BY 4.0) and its COG grid. The land-cover
layer cuts these COGs into CamSim's 0.05 degree tiles (layers/landcover.py). Import-light (numpy only)."""

from __future__ import annotations

from pathlib import Path

from ..net import HttpError
from .base import Area, Asset, Layer, SourceBase, SourceRaster

TILE_DEG = 0.05
CELLS_PER_DEG = 12000
TILE_PX = 600  # TILE_DEG * CELLS_PER_DEG
TILES_PER_COG = 60  # 3 degrees / TILE_DEG
COG_URL = (
    "https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/2021/map/"
    "ESA_WorldCover_10m_2021_v200_{name}_Map.tif"
)
SOURCE = "ESA WorldCover 10m 2021 v200"
LICENCE = "CC BY 4.0"
ATTRIBUTION = (
    "© ESA WorldCover project 2021 / Contains modified Copernicus Sentinel data (2021) "
    "processed by ESA WorldCover consortium"
)


def cog_name(i: int, j: int) -> str:
    """The WorldCover COG holding tile (i, j), named by its 3-degree SW corner (N36W123)."""
    lat = (i // TILES_PER_COG) * 3
    lon = (j // TILES_PER_COG) * 3
    return f"{'N' if lat >= 0 else 'S'}{abs(lat):02d}{'E' if lon >= 0 else 'W'}{abs(lon):03d}"


def cog_offset(i: int, j: int) -> tuple[int, int]:
    """(row_off, col_off) in cells of tile (i, j) inside its COG (row 0 = the COG's north edge)."""
    row = (i // TILES_PER_COG) * TILES_PER_COG + TILES_PER_COG - 1 - i
    col = j - (j // TILES_PER_COG) * TILES_PER_COG
    return row * TILE_PX, col * TILE_PX


class WorldCover(SourceBase):
    id = "worldcover"
    layer = Layer.LANDCOVER
    licence = "CC-BY-4.0"
    dataset = SOURCE
    version = "2021 v200"
    attribution = ATTRIBUTION
    area_kind = "ring"
    datum = "wgs84"

    def discover(self, area: Area) -> list[Asset]:
        from ..layers.landcover import tiles_for_bbox

        names = sorted({cog_name(i, j) for i, j in tiles_for_bbox(*area.bounds)})
        assets = []
        for name in names:
            url = COG_URL.format(name=name)
            status, size = self.http.head(url)
            if status == 404:
                continue  # open ocean: the land-cover index lists its tiles as missing
            if status != 200:
                raise HttpError(f"HEAD {url}: HTTP {status}")
            assets.append(Asset(id=name, url=url, size=size, group=self.id, rank=len(assets)))
        return assets

    def prepare(self, path: Path, asset: Asset, cache) -> Path | None:
        return None  # cut exactly by window; no overviews needed

    def open(self, path: Path, asset: Asset) -> SourceRaster:
        return SourceRaster(path=Path(path), datum=self.datum)
```

- [ ] **Step 4: Implement `layers/__init__.py` and `layers/landcover.py`**

`layers/__init__.py`:

```python
"""Layer builders (terrain, imagery, land cover). Submodules import their heavy dependencies themselves."""
```

`layers/landcover.py`: move `Reader`, `FORMAT`, `tile_range`, `tiles_for_bbox`, `tile_filename`, `read_cog_window`, `write_tile`, `build_index`, `fetch` verbatim from `fetch_worldcover.py` (with the constants imported from `..sources.worldcover`), give `fetch` a `cut_by` parameter used in the ATTRIBUTION.txt "Source:" line, and add `local_reader`:

```python
"""CamSim land-cover tiles (ROADMAP 4B format, camsim-landcover-1): 0.05 x 0.05 degree, 600 x 600 8-bit PNGs of
WorldCover class codes, cut losslessly from the 3-degree COGs, plus index.json and ATTRIBUTION.txt.

Tile (lat_index i, lon_index j) covers latitudes [0.05 i, 0.05 (i + 1)) and longitudes [0.05 j, 0.05 (j + 1));
its file is named by its south-west corner (+37.75_-122.45.png); PNG row 0 is the north edge. A COG that
doesn't exist (open ocean) is listed under "missing" and CamSim treats it as no data (code 0)."""

from __future__ import annotations

import json
import math
from collections.abc import Callable
from pathlib import Path

import numpy as np
from PIL import Image

from ..sources.worldcover import ATTRIBUTION, CELLS_PER_DEG, COG_URL, LICENCE, SOURCE, TILE_DEG, TILE_PX, cog_name, cog_offset

FORMAT = "camsim-landcover-1"
Reader = Callable[[str, int, int], "np.ndarray | None"]

# tile_range, tiles_for_bbox, tile_filename, read_cog_window, write_tile, build_index: verbatim from
# scripts/landcover/fetch_worldcover.py (git show HEAD:scripts/landcover/fetch_worldcover.py).


def fetch(bbox, out: Path, reader: Reader = read_cog_window, cut_by: str = "scripts/landcover/fetch_worldcover.py") -> dict:
    out.mkdir(parents=True, exist_ok=True)
    written: list[tuple[int, int]] = []
    missing: list[tuple[int, int]] = []
    for i, j in tiles_for_bbox(*bbox):
        row, col = cog_offset(i, j)
        codes = reader(COG_URL.format(name=cog_name(i, j)), row, col)
        if codes is None:
            missing.append((i, j))
            continue
        write_tile(np.ascontiguousarray(codes, dtype=np.uint8), out / tile_filename(i, j))
        written.append((i, j))
    index = build_index(bbox, written, missing)
    (out / "index.json").write_text(json.dumps(index, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    (out / "ATTRIBUTION.txt").write_text(
        f"{SOURCE}\n{ATTRIBUTION}\nLicence: {LICENCE} (https://creativecommons.org/licenses/by/4.0/)\n"
        f"Source: https://esa-worldcover.org (tiles cut by {cut_by})\n",
        encoding="utf-8",
    )
    return index


def local_reader(paths: dict[str, Path]) -> Reader:
    """Reader over cached COGs keyed by their COG_URL; a URL without a cached file reads as missing (None)."""

    def read(url: str, row_off: int, col_off: int):
        import rasterio
        from rasterio.windows import Window

        p = paths.get(url)
        if p is None:
            return None
        with rasterio.open(p) as ds:
            return ds.read(1, window=Window(col_off, row_off, TILE_PX, TILE_PX))

    return read
```

(The `math` and `CELLS_PER_DEG` imports are used by the moved `tile_range` and `read_cog_window`.)

- [ ] **Step 5: Turn `scripts/landcover/fetch_worldcover.py` into the wrapper**

Replace the file with (keep the existing docstring's first two paragraphs, usage and data section, and add the line about where the code lives):

```python
# /// script
# requires-python = ">=3.12"
# dependencies = ["numpy", "pillow", "rasterio"]
# ///
"""fetch_worldcover.py — cut ESA WorldCover 2021 v200 into CamSim land-cover tiles (ROADMAP 4B).

<keep the existing paragraphs: range reads, tile layout, data/attribution, usage>

The code lives in scripts/scene (camsim_scene.layers.landcover and camsim_scene.sources.worldcover), which
also builds the land-cover layer of scene packages; this script keeps the standalone CLI and output.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts" / "scene"))

from camsim_scene.layers.landcover import (  # noqa: E402,F401
    FORMAT,
    Reader,
    build_index,
    fetch,
    read_cog_window,
    tile_filename,
    tile_range,
    tiles_for_bbox,
    write_tile,
)
from camsim_scene.sources.worldcover import (  # noqa: E402,F401
    ATTRIBUTION,
    CELLS_PER_DEG,
    COG_URL,
    LICENCE,
    SOURCE,
    TILE_DEG,
    TILE_PX,
    TILES_PER_COG,
    cog_name,
    cog_offset,
)

DEFAULT_OUT = REPO / "unreal_project/CamSimTest/Content/NonUFS/LandCover"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bbox", nargs=4, type=float, metavar=("W", "S", "E", "N"), required=True)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    a = ap.parse_args(argv)
    index = fetch(tuple(a.bbox), a.out)
    print(f"{len(index['tiles'])} tiles written, {len(index['missing'])} missing -> {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 6: Run both test suites**

Run: `cd scripts/scene && uv run pytest tests/test_landcover.py -v`
Expected: 2 passed.
Run (repo root, the CI environment for `scripts/tests`): `uv run --with pytest --with numpy --with pillow pytest scripts/tests/test_fetch_worldcover.py -v`
Expected: 8 passed, unchanged. A `ModuleNotFoundError` for shapely/pyproj/rasterio here means a module on the wrapper's import chain imports too much at module level: move that import into the function that needs it.

- [ ] **Step 7: Check the committed sample is reproduced (network, optional)**

Run: `uv run scripts/landcover/fetch_worldcover.py --bbox -122.56 37.69 -122.35 37.84 --out /tmp/lc && diff -r /tmp/lc unreal_project/CamSimTest/Content/NonUFS/LandCover`
Expected: only the ATTRIBUTION.txt "Source:" line may differ if the committed one predates the wrapper; PNGs and index.json identical.

- [ ] **Step 8: Commit**

```bash
git add scripts/scene/camsim_scene/sources/worldcover.py scripts/scene/camsim_scene/layers scripts/scene/tests/test_landcover.py \
  scripts/landcover/fetch_worldcover.py
git commit -m "refactor(landcover): move WorldCover cutting into camsim_scene; fetch_worldcover.py wraps it"
```

---
### Task 14: Build context (rasters, datum transforms, asset index per layer) and synthetic sources

**Files:**
- Create: `scripts/scene/camsim_scene/context.py`
- Create: `scripts/scene/tests/fake_sources.py`
- Test: `scripts/scene/tests/test_context.py`

**Interfaces:**
- Consumes: `manifest.{Manifest, AssetRecord, SourceRecord}`, `sources.make_source`, `sources.base.{Asset, SourceRaster}`, `datum.{DatumTransform, manifest_section}`, `tiling.{GLOBE, Coverage}`, `config.{parse_scene, layer_settings, TILING}`.
- Produces (`context`): `BuildContext(pkg: str, manifest: dict, asset_paths: dict[str, str], grid_paths: dict[str, str])` (frozen, picklable; asset keys are `"<source>/<asset>"`); `Entry(qid, sha256, order: tuple, limit: int, raster: SourceRaster, transform: DatumTransform)` where `order = (source priority, group min rank, group, asset rank, asset id)` and `order[:3]` identifies a merge group; `to_asset(AssetRecord) -> Asset`; `coverage(m, layer, use_bbox=False) -> Coverage`; `LayerIndex.query(bounds) -> list[Entry]` (sorted by `order`, highest priority first); `WorkerState(ctx)` with `pkg`, `manifest`, `settings: dict[layer, str]` (settings hashes), `index: dict["terrain"|"imagery", LayerIndex]`.
- Produces (`tests/fake_sources.py`): `ADAPTER = "fake_sources:FakeSource"`; `FakeSource` (options `layer, max_zoom, global, datum, bands, nodata, nodata_rule, licence, files: [{id, path, bbox, group?}], geoid: path?`); `synthetic_scene(root, bbox=(10.0, 10.0, 10.5, 10.5)) -> dict` (scene.toml data with four rasters and small zooms); `fake_context(root, scene=None) -> BuildContext` (a fetched-equivalent manifest built without cache/HTTP: footprints = discovery bboxes).

- [ ] **Step 1: Write the synthetic sources**

`scripts/scene/tests/fake_sources.py`:

```python
"""Synthetic sources for context/layer/pipeline tests: small local GeoTIFFs served as file:// URLs.
Referenced from scene data as adapter "fake_sources:FakeSource" (importable in spawned workers too)."""

from __future__ import annotations

from pathlib import Path

import numpy as np
import shapely

from camsim_scene import datum, fsutil
from camsim_scene.config import TILING, layer_settings, parse_scene
from camsim_scene.context import BuildContext
from camsim_scene.manifest import AssetRecord, Manifest, SourceRecord
from camsim_scene.sources import make_source
from camsim_scene.sources.base import Asset, Layer, SourceBase, SourceRaster
from rasters import write_geotiff

ADAPTER = "fake_sources:FakeSource"


class FakeSource(SourceBase):
    def __init__(self, options=None, http=None):
        super().__init__(options, http)
        o = self._options
        self.layer = Layer(o["layer"])
        self.max_zoom = int(o["max_zoom"])
        self.global_coverage = bool(o.get("global", False))
        self.datum = o.get("datum", "wgs84")
        self.licence = o.get("licence", "LicenseRef-PublicDomain-USGov")
        self.dataset, self.version, self.attribution, self.area_kind = f"fake {o['layer']}", "1", "synthetic test data", "globe"

    def discover(self, area) -> list[Asset]:
        out = [
            Asset(id=f["id"], url=Path(f["path"]).as_uri(), group=f.get("group", self.id), rank=i, metadata={"bbox": list(f["bbox"])})
            for i, f in enumerate(self._options["files"])
        ]
        if "geoid" in self._options:
            out.append(Asset(id="geoid", url=Path(self._options["geoid"]).as_uri(), group=self.id, rank=len(out), role="geoid",
                             metadata={"bbox": [-180, -90, 180, 90]}))
        return out

    def prepare(self, path, asset, cache):
        return None

    def open(self, path, asset) -> SourceRaster:
        o = self._options
        return SourceRaster(
            path=Path(path), datum=self.datum, bands=tuple(o.get("bands", [1])), nodata=o.get("nodata"),
            nodata_rule=o.get("nodata_rule", "value"), clamp_edges=self.global_coverage,
            vertical_asset=f"{self.id}/geoid" if "geoid" in o else None,
        )


def source(layer, path, bbox, max_zoom=8, **kw) -> dict:
    return {"adapter": ADAPTER, "layer": layer, "max_zoom": max_zoom, "files": [{"id": Path(path).stem, "path": str(path), "bbox": list(bbox)}], **kw}


def synthetic_scene(root: Path, bbox=(10.0, 10.0, 10.5, 10.5)) -> dict:
    """A global 1-degree DEM + RGB, and a 0.001-degree DEM + RGB around bbox (north half of the RGB brighter)."""
    root.mkdir(parents=True, exist_ok=True)
    lon, lat = np.arange(360) - 179.5, 89.5 - np.arange(180)
    dem = (100.0 + 50.0 * np.sin(np.radians(lat))[:, None] * np.cos(np.radians(lon))[None, :]).astype(np.float32)
    write_geotiff(root / "base_dem.tif", dem, -180.0, 90.0, 1.0)
    rgb = np.zeros((3, 180, 360), np.uint8)
    rgb[0], rgb[1], rgb[2] = 40, 80, 160
    write_geotiff(root / "base_rgb.tif", rgb, -180.0, 90.0, 1.0)
    w, s, e, n = bbox
    pad, res = 0.1, 0.001
    nx, ny = int(round((e - w + 2 * pad) / res)), int(round((n - s + 2 * pad) / res))
    yy, xx = np.mgrid[0:ny, 0:nx]
    write_geotiff(root / "hi_dem.tif", (205.0 + 20.0 * np.sin(xx / 50.0) * np.cos(yy / 70.0)).astype(np.float32),
                  w - pad, n + pad, res, nodata=-9999.0, overviews=(2, 4, 8))
    hrgb = np.zeros((3, ny, nx), np.uint8)
    hrgb[0], hrgb[1], hrgb[2] = 200, (xx % 256).astype(np.uint8), 50
    hrgb[0, : ny // 2] = 250
    write_geotiff(root / "hi_rgb.tif", hrgb, w - pad, n + pad, res, overviews=(2, 4, 8))
    hi_bbox = [w - pad, n + pad - ny * res, w - pad + nx * res, n + pad]
    globe = [-180, -90, 180, 90]
    return {
        "name": "synthetic",
        "bbox": list(bbox),
        "ring_km": 30,
        "priorities": {"terrain": ["hi_dem", "base_dem"], "imagery": ["hi_rgb", "base_rgb"], "landcover": []},
        "zoom": {"globe": {"terrain": 2, "imagery": 2}, "ring": {"terrain": 3, "imagery": 3}, "bbox": {"terrain": 6, "imagery": 6}},
        "sources": {
            "base_dem": source("terrain", root / "base_dem.tif", globe, **{"global": True}),
            "hi_dem": source("terrain", root / "hi_dem.tif", hi_bbox, nodata=-9999.0),
            "base_rgb": source("imagery", root / "base_rgb.tif", globe, bands=[1, 2, 3], **{"global": True}),
            "hi_rgb": source("imagery", root / "hi_rgb.tif", hi_bbox, bands=[1, 2, 3], nodata_rule="all_zero"),
        },
    }


def fake_context(root: Path, scene: dict | None = None) -> BuildContext:
    """The context a fetched manifest would give, built directly from local files (no cache, no HTTP)."""
    scene = scene or synthetic_scene(root / "src")
    plan = parse_scene(scene)
    records, paths, datums = [], {}, set()
    for sid in plan.source_ids():
        opts = dict(plan.source_options[sid])
        adapter = opts.pop("adapter")
        src = make_source(sid, adapter, opts)
        assets = []
        for a in src.discover(None):
            path = Path(a.url.removeprefix("file://"))
            paths[f"{sid}/{a.id}"] = str(path)
            fp = None if src.global_coverage else shapely.to_wkt(shapely.box(*a.metadata["bbox"]), rounding_precision=7)
            assets.append(AssetRecord(id=a.id, url=a.url, sha256=fsutil.sha256_file(path), size=path.stat().st_size, group=a.group,
                                      rank=a.rank, role=a.role, metadata={**a.metadata, "footprint": fp}))
        datums.add(src.datum)
        records.append(SourceRecord(sid, adapter, src.dataset, src.version, src.licence, src.attribution, src.options(), assets))
    m = Manifest(
        name=plan.name, bbox=list(plan.bbox), seed=plan.seed, regions=[r.to_dict() for r in plan.regions()], tiling=TILING,
        layers=layer_settings(plan), datum=datum.manifest_section(datums), sources=records,
        licence_allow=["CC-BY-4.0", "LicenseRef-PublicDomain-USGov"], tool={"name": "camsim-scene", "version": "test"},
    )
    return BuildContext(str(root / "pkg"), m.to_dict(), paths, {})
```

- [ ] **Step 2: Write the failing tests**

`scripts/scene/tests/test_context.py`:

```python
import pickle

import numpy as np
import shapely

from camsim_scene import tiling
from camsim_scene.context import WorkerState, coverage
from camsim_scene.manifest import Manifest
from fake_sources import fake_context, source, synthetic_scene
from rasters import write_geotiff


def test_query_orders_by_priority_and_footprint(tmp_path):
    st = WorkerState(fake_context(tmp_path))
    near = st.index["terrain"].query((10.1, 10.1, 10.2, 10.2))
    assert [e.qid for e in near] == ["hi_dem/hi_dem", "base_dem/base_dem"]
    assert [e.qid for e in st.index["terrain"].query((-50.0, -50.0, -49.0, -49.0))] == ["base_dem/base_dem"]
    assert near[0].order[:3] != near[1].order[:3]  # different merge groups


def test_coverage_uses_footprints_and_source_limits(tmp_path):
    m = Manifest.from_dict(fake_context(tmp_path).manifest)
    cov = coverage(m, "imagery")
    assert cov.limits == [8, 8] and cov.geoms[1].equals(shapely.box(*tiling.GLOBE))
    assert coverage(m, "terrain", use_bbox=True).geoms[0].equals(cov.geoms[0])


def test_context_pickles(tmp_path):
    ctx = fake_context(tmp_path)
    assert pickle.loads(pickle.dumps(ctx)) == ctx


def test_raster_geoid_datum_adds_the_geoid(tmp_path):
    scene = synthetic_scene(tmp_path / "src")
    geoid = write_geotiff(tmp_path / "geoid.tif", np.full((180, 360), 10.0, np.float32), -180.0, 90.0, 1.0)
    scene["sources"]["base_dem"] = source("terrain", tmp_path / "src" / "base_dem.tif", [-180, -90, 180, 90],
                                          datum="wgs84_egm2008_raster", geoid=str(geoid), **{"global": True})
    st = WorkerState(fake_context(tmp_path, scene))
    (e,) = st.index["terrain"].query((-50.0, -50.0, -49.0, -49.0))
    assert e.transform.vertical == "raster_geoid"
    assert np.allclose(e.transform.vertical_offset(np.array([-49.5]), np.array([-49.5])), 10.0)
```

- [ ] **Step 3: Run them to see them fail**

Run: `cd scripts/scene && uv run pytest tests/test_context.py -v`
Expected: FAIL, `ModuleNotFoundError: camsim_scene.context`.

- [ ] **Step 4: Implement `context.py`**

```python
"""Per-process view of a fetched manifest: a SourceRaster and DatumTransform per data asset, ordered by layer
priority, behind an STRtree of footprints. BuildContext (plain data) is what crosses the process boundary;
each worker builds its WorkerState once."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np
import shapely

from .datum import DatumTransform
from .manifest import AssetRecord, Manifest
from .sources import make_source
from .sources.base import Asset, SourceRaster
from .tiling import GLOBE, Bounds, Coverage

GEOID_TARGET_M = 900.0  # ETOPO geoid grid resolution: read at full resolution


@dataclass(frozen=True)
class BuildContext:
    pkg: str
    manifest: dict
    asset_paths: dict  # "<source>/<asset>" -> local file (prepared file when there is one)
    grid_paths: dict  # PROJ grid name -> local file


@dataclass
class Entry:
    qid: str
    sha256: str
    order: tuple  # (source priority, group min rank, group, asset rank, asset id); order[:3] = merge group
    limit: int
    raster: SourceRaster
    transform: DatumTransform


def to_asset(a: AssetRecord) -> Asset:
    return Asset(id=a.id, url=a.url, size=a.size, group=a.group, rank=a.rank, role=a.role, metadata=dict(a.metadata), sha256=a.sha256)


def _footprint(a: AssetRecord, use_bbox: bool):
    if use_bbox:
        bb = a.metadata.get("bbox")
        return None if bb is None else shapely.box(*bb)
    if "footprint" not in a.metadata:
        raise ValueError(f"asset {a.id} has no footprint yet: run `camsim-scene fetch`")
    wkt = a.metadata["footprint"]
    return None if wkt is None else shapely.from_wkt(wkt)


def coverage(m: Manifest, layer: str, use_bbox: bool = False) -> Coverage:
    """Footprints of a layer's data assets, each with its source's zoom limit (discovery bboxes before fetch)."""
    cov = Coverage()
    for sid in m.layers[layer]["priorities"]:
        rec = m.source(sid)
        src = make_source(rec.id, rec.adapter, rec.options)
        if src.global_coverage:
            cov.add(shapely.box(*GLOBE), src.max_zoom)
            continue
        for a in rec.assets:
            geom = _footprint(a, use_bbox) if a.role == "data" else None
            if geom is not None:
                cov.add(geom, src.max_zoom)
    return cov


class LayerIndex:
    def __init__(self, entries: list[Entry], geoms: list):
        self.entries = entries
        self.tree = shapely.STRtree(geoms) if geoms else None

    def query(self, bounds: Bounds) -> list[Entry]:
        if self.tree is None:
            return []
        idx = self.tree.query(shapely.box(*bounds), predicate="intersects")
        return sorted((self.entries[i] for i in idx), key=lambda e: e.order)


class WorkerState:
    def __init__(self, ctx: BuildContext):
        self.ctx = ctx
        self.pkg = Path(ctx.pkg)
        self.manifest = Manifest.from_dict(ctx.manifest)
        self.grid_paths = {k: Path(v) for k, v in ctx.grid_paths.items()}
        self.settings = {layer: self.manifest.layer_settings_hash(layer) for layer in self.manifest.layers}
        self._transforms: dict[tuple, DatumTransform] = {}
        self.index = {layer: self._index(layer) for layer in ("terrain", "imagery")}

    def _transform(self, datum_id: str, vertical_asset: str | None) -> DatumTransform:
        key = (datum_id, vertical_asset)
        if key not in self._transforms:
            sampler = None
            if vertical_asset:
                geoid = SourceRaster(path=Path(self.ctx.asset_paths[vertical_asset]), datum="wgs84", clamp_edges=True)

                def sampler(lon, lat, g=geoid):
                    return np.nan_to_num(g.sample(lon, lat, GEOID_TARGET_M)[0][0])

            self._transforms[key] = DatumTransform(self.manifest.datum["datums"][datum_id], self.grid_paths, sampler)
        return self._transforms[key]

    def _index(self, layer: str) -> LayerIndex:
        entries, geoms = [], []
        for p, sid in enumerate(self.manifest.layers[layer]["priorities"]):
            rec = self.manifest.source(sid)
            src = make_source(rec.id, rec.adapter, rec.options)
            data = [a for a in rec.assets if a.role == "data"]
            group_rank: dict[str, int] = {}
            for a in data:
                group_rank[a.group] = min(group_rank.get(a.group, a.rank), a.rank)
            for a in data:
                geom = shapely.box(*GLOBE) if src.global_coverage else _footprint(a, use_bbox=False)
                if geom is None:
                    continue  # the asset holds no data
                qid = f"{sid}/{a.id}"
                raster = src.open(Path(self.ctx.asset_paths[qid]), to_asset(a))
                order = (p, group_rank[a.group], a.group, a.rank, a.id)
                entries.append(Entry(qid, a.sha256, order, src.max_zoom, raster, self._transform(raster.datum, raster.vertical_asset)))
                geoms.append(geom)
        return LayerIndex(entries, geoms)
```

- [ ] **Step 5: Run the tests**

Run: `cd scripts/scene && uv run pytest tests/test_context.py -v`
Expected: 4 passed.

- [ ] **Step 6: Commit**

```bash
git add scripts/scene/camsim_scene/context.py scripts/scene/tests/fake_sources.py scripts/scene/tests/test_context.py
git commit -m "feat(scene): build context (rasters, datum transforms, footprint index) and synthetic test sources"
```

---

### Task 15: Terrain tile job

**Files:**
- Create: `scripts/scene/camsim_scene/layers/terrain.py`
- Test: `scripts/scene/tests/test_terrain.py`

**Interfaces:**
- Consumes: `context.Entry`; `datum.offset_lattice`; `sources.base.{project, M_PER_DEG}`; `qmesh.{encode, gzip_tile}`; `config.{TERRAIN_GRID, FEATHER_M}`; `tiling.{tile_bounds, tile_size_deg}`; `pydelatin.Delatin`.
- Produces: `TerrainError(Exception)`; `max_error(z) -> float`; `spacing_deg(z)`; `margin(z) -> int`; `query_bounds(z, x, y) -> Bounds` (tile + margin, for `LayerIndex.query`); `sample_grid(z, x, y) -> (lon, lat)`; `TileGrid(lon, lat, h, seam)` (inner 257 × 257, row 0 north); `tile_grid(z, x, y, entries) -> TileGrid`; `build_tile(z, x, y, entries) -> bytes` (gzipped quantized-mesh); `point_heights(z, lon, lat, entries) -> np.ndarray` (independent check for `verify --deep`: highest-priority valid sample, no feather, exact vertical offsets).

Algorithm (spec "Terrain tile job"; Decisions 6–8): sample a 257 × 257 grid over the tile plus `margin(z)` samples on every side; for each merge group (highest priority first) sample its assets first-valid-wins and add the vertical offset (lattice); merge groups from lowest to highest priority with weight `w = clip(EDT(valid) × spacing / FEATHER_DEG, 0, 1)`; the lowest-priority group must be complete; crop to the tile; pydelatin with `max_error(z)`; vertex `(x, y)` → `lon = W + x·d`, `lat = S + y·d` (pydelatin's `y` counts up from the last row = south), heights taken from the float64 grid; triangles kept as pydelatin's CCW order.

- [ ] **Step 1: Write the failing tests**

`scripts/scene/tests/test_terrain.py`:

```python
import math

import numpy as np
import pytest
from camsim_scene import qmesh, tiling
from camsim_scene.context import WorkerState
from camsim_scene.layers import terrain
from fake_sources import fake_context, source
from rasters import write_geotiff

GLOBE = [-180, -90, 180, 90]


def scene_with(tmp_path, hi_data, west, north, res, nodata=None, base_value=0.0):
    src = tmp_path / "src"
    src.mkdir(parents=True, exist_ok=True)
    write_geotiff(src / "base.tif", np.full((180, 360), base_value, np.float32), -180.0, 90.0, 1.0)
    write_geotiff(src / "hi.tif", hi_data.astype(np.float32), west, north, res, nodata=nodata, overviews=(2, 4, 8))
    h, w = hi_data.shape
    return {
        "name": "t", "bbox": [west, north - h * res, west + w * res, north],
        "priorities": {"terrain": ["hi", "base"], "imagery": [], "landcover": []},
        "sources": {
            "base": source("terrain", src / "base.tif", GLOBE, **{"global": True}),
            "hi": source("terrain", src / "hi.tif", [west, north - h * res, west + w * res, north], max_zoom=16, nodata=nodata),
        },
    }


def tin_on_grid(q, grid=257):
    """The decoded TIN (its own triangles) evaluated at the sample-grid nodes (row 0 = north)."""
    out = np.full((grid, grid), np.nan)
    gx, gy, h = q.u / qmesh.QMAX * (grid - 1), q.v / qmesh.QMAX * (grid - 1), q.heights()
    for a, b, c in q.triangles:
        xs, ys = gx[[a, b, c]], gy[[a, b, c]]
        X, Y = np.meshgrid(np.arange(int(np.floor(xs.min())), int(np.ceil(xs.max())) + 1),
                           np.arange(int(np.floor(ys.min())), int(np.ceil(ys.max())) + 1))
        det = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2])
        l1 = ((ys[1] - ys[2]) * (X - xs[2]) + (xs[2] - xs[1]) * (Y - ys[2])) / det
        l2 = ((ys[2] - ys[0]) * (X - xs[2]) + (xs[0] - xs[2]) * (Y - ys[2])) / det
        inside = (l1 >= -1e-3) & (l2 >= -1e-3) & (1 - l1 - l2 >= -1e-3) & (X >= 0) & (X < grid) & (Y >= 0) & (Y < grid)
        vals = l1 * h[a] + l2 * h[b] + (1 - l1 - l2) * h[c]
        out[grid - 1 - Y[inside], X[inside]] = vals[inside]
    return out


def tile_at(z, lon, lat):
    s = tiling.tile_size_deg(z)
    return math.floor((lon + 180) / s), math.floor((lat + 90) / s)


def f(lon, lat):
    return 100.0 + 30.0 * np.sin(lon * 600.0) * np.cos(lat * 450.0)


def smooth_scene(tmp_path):
    res, west, north, n = 0.00005, 9.99, 10.04, 1000  # covers the test tile and its margin
    lon = west + (np.arange(n) + 0.5) * res
    lat = north - (np.arange(n) + 0.5) * res
    return scene_with(tmp_path, f(lon[None, :], lat[:, None]), west, north, res)


def test_decoded_tin_is_within_max_error_of_the_grid_and_close_to_the_dem(tmp_path):
    st = WorkerState(fake_context(tmp_path, smooth_scene(tmp_path)))
    z = 13
    x, y = tile_at(z, 10.012, 10.012)
    entries = st.index["terrain"].query(terrain.query_bounds(z, x, y))
    b = tiling.tile_bounds(z, x, y)
    q = qmesh.decode(terrain.build_tile(z, x, y, entries))
    lon, lat = q.lonlat(b)
    h = q.heights()
    assert np.abs(h - f(lon, lat)).max() < 0.2  # vertices sit on samples of the DEM
    g = terrain.tile_grid(z, x, y, entries)
    assert np.nanmax(np.abs(tin_on_grid(q) - g.h)) <= terrain.max_error(z) + 0.05


def test_north_is_up(tmp_path):
    res, west, north, n = 0.00005, 9.99, 10.04, 1000
    lat = north - (np.arange(n) + 0.5) * res
    data = np.repeat((1000.0 * (lat - 9.99))[:, None], n, axis=1)  # rises northward
    st = WorkerState(fake_context(tmp_path, scene_with(tmp_path, data, west, north, res)))
    z = 13
    x, y = tile_at(z, 10.012, 10.012)
    q = qmesh.decode(terrain.build_tile(z, x, y, st.index["terrain"].query(terrain.query_bounds(z, x, y))))
    h = q.heights()
    assert h[q.north].min() > h[q.south].max()


def test_feather_is_continuous_across_a_shared_tile_edge(tmp_path):
    z = 15
    x, y = tile_at(z, 10.008, 10.008)
    e = tiling.tile_bounds(z, x, y)[2]
    res, west, north, n = 0.00002, 9.995, 10.02, 1250
    lon = west + (np.arange(n) + 0.5) * res
    data = np.where(lon[None, :] < e + 10.0 / 111320.0, 5.0, -9999.0) * np.ones((n, 1))
    st = WorkerState(fake_context(tmp_path, scene_with(tmp_path, data, west, north, res, nodata=-9999.0)))
    left = terrain.tile_grid(z, x, y, st.index["terrain"].query(terrain.query_bounds(z, x, y)))
    right = terrain.tile_grid(z, x + 1, y, st.index["terrain"].query(terrain.query_bounds(z, x + 1, y)))
    assert np.array_equal(left.h[:, -1], right.h[:, 0])
    row = np.concatenate([left.h[128], right.h[128, 1:]])
    assert row[0] == pytest.approx(5.0) and row[-1] == pytest.approx(0.0)
    assert np.all(np.diff(row) <= 1e-9)  # monotone ramp, no step
    assert left.seam.any() and right.seam.any()
    ql = qmesh.decode(terrain.build_tile(z, x, y, st.index["terrain"].query(terrain.query_bounds(z, x, y))))
    qr = qmesh.decode(terrain.build_tile(z, x + 1, y, st.index["terrain"].query(terrain.query_bounds(z, x + 1, y))))
    hl = dict(zip(ql.v[ql.east].tolist(), ql.heights()[ql.east]))
    hr = dict(zip(qr.v[qr.west].tolist(), qr.heights()[qr.west]))
    common = set(hl) & set(hr)
    assert len(common) >= 2
    step = max((ql.max_height - ql.min_height), (qr.max_height - qr.min_height)) / qmesh.QMAX
    assert all(abs(hl[v] - hr[v]) <= step + 1e-9 for v in common)


def test_lowest_priority_source_must_be_complete(tmp_path):
    scene = smooth_scene(tmp_path)
    scene["sources"]["base"]["global"] = False
    scene["sources"]["base"]["files"][0]["bbox"] = [0, 0, 1, 1]
    st = WorkerState(fake_context(tmp_path, scene))
    x, y = tile_at(13, 9.99, 10.012)  # straddles the DEM's west edge; the base no longer covers it
    with pytest.raises(terrain.TerrainError, match="gaps"):
        terrain.build_tile(13, x, y, st.index["terrain"].query(terrain.query_bounds(13, x, y)))


def test_point_heights_match_vertices_away_from_seams(tmp_path):
    st = WorkerState(fake_context(tmp_path, smooth_scene(tmp_path)))
    z = 13
    x, y = tile_at(z, 10.012, 10.012)
    entries = st.index["terrain"].query(terrain.query_bounds(z, x, y))
    q = qmesh.decode(terrain.build_tile(z, x, y, entries))
    lon, lat = q.lonlat(tiling.tile_bounds(z, x, y))
    assert np.abs(terrain.point_heights(z, lon, lat, entries) - q.heights()).max() < 0.2
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd scripts/scene && uv run pytest tests/test_terrain.py -v`
Expected: FAIL, `ImportError` (no `layers.terrain`).

- [ ] **Step 3: Implement `layers/terrain.py`**

```python
"""Terrain tile job: sample grid -> per-source windows -> ellipsoid heights -> priority + feather -> TIN -> writer.

Neighbouring tiles agree on their shared edge exactly: they sample the same lon/lat there (tile edges and the
sample spacing are binary fractions of a degree), choose overview levels from the zoom alone, share vertical-offset
lattice nodes, and the feather's distance field sees at least 30 m beyond every edge (the margin)."""

from __future__ import annotations

import math
from dataclasses import dataclass
from itertools import groupby

import numpy as np
from scipy.ndimage import distance_transform_edt

from .. import qmesh
from ..config import FEATHER_M, TERRAIN_GRID as GRID
from ..datum import offset_lattice
from ..sources.base import M_PER_DEG, project
from ..tiling import Bounds, tile_bounds, tile_size_deg

MIN_MARGIN = 32
FEATHER_DEG = FEATHER_M / M_PER_DEG  # measured in degrees of latitude on both axes (Decision 8)


class TerrainError(Exception):
    pass


def max_error(z: int) -> float:
    """A quarter of Cesium's assumed geometric error at level z, never below 0.1 m."""
    return max(0.1, 0.25 * 77067.0 / (1 << z))


def spacing_deg(z: int) -> float:
    return tile_size_deg(z) / (GRID - 1)


def margin(z: int) -> int:
    return max(MIN_MARGIN, math.ceil(FEATHER_DEG / spacing_deg(z)) + 2)


def query_bounds(z: int, x: int, y: int) -> Bounds:
    w, s, e, n = tile_bounds(z, x, y)
    m = margin(z) * spacing_deg(z)
    return (w - m, s - m, e + m, n + m)


def sample_grid(z: int, x: int, y: int) -> tuple[np.ndarray, np.ndarray]:
    w, s, e, n = tile_bounds(z, x, y)
    d, m = spacing_deg(z), margin(z)
    k = np.arange(-m, GRID + m)
    lon = w + k * d
    lon = np.where(lon > 180.0, lon - 360.0, np.where(lon < -180.0, lon + 360.0, lon))
    lat = np.clip(n - k * d, -90.0, 90.0)
    return np.meshgrid(lon, lat)


def _groups(entries) -> list[list]:
    return [list(g) for _, g in groupby(entries, key=lambda e: e.order[:3])]


def sample_group(group, z: int, lon: np.ndarray, lat: np.ndarray, exact_offsets: bool = False):
    """Ellipsoid heights from one merge group (its assets first-valid-wins) and where they are valid."""
    h = np.full(lon.shape, np.nan)
    valid = np.zeros(lon.shape, bool)
    target_m = spacing_deg(z) * M_PER_DEG
    for e in group:
        need = ~valid
        if not need.any():
            break
        slon, slat = e.transform.to_source_geographic(lon, lat)
        px, py = project(e.raster, slon, slat)
        vals, ok = e.raster.sample(px, py, target_m)
        take = ok & need
        if not take.any():
            continue
        off = e.transform.vertical_offset(slon, slat) if exact_offsets else offset_lattice(e.transform, z, lon, lat)
        h[take] = vals[0][take] + off[take]
        valid |= take
    return h, valid


@dataclass
class TileGrid:
    lon: np.ndarray
    lat: np.ndarray
    h: np.ndarray
    seam: np.ndarray  # True inside a feather band (excluded from verify's height comparison)


def tile_grid(z: int, x: int, y: int, entries) -> TileGrid:
    lon, lat = sample_grid(z, x, y)
    groups = _groups(entries)
    if not groups:
        raise TerrainError(f"terrain {z}/{x}/{y}: no source covers the tile")
    results = [sample_group(g, z, lon, lat) for g in groups]
    h, base_ok = results[-1]
    if not base_ok.all():
        raise TerrainError(f"terrain {z}/{x}/{y}: the lowest-priority source ({groups[-1][0].qid}) has gaps")
    h = h.copy()
    seam = np.zeros(lon.shape, bool)
    d = spacing_deg(z)
    for gh, gv in reversed(results[:-1]):
        if not gv.any():
            continue
        w = np.ones(lon.shape) if gv.all() else np.clip(distance_transform_edt(gv) * d / FEATHER_DEG, 0.0, 1.0)
        h = np.where(gv, w * np.nan_to_num(gh) + (1.0 - w) * h, h)
        seam |= gv & (w < 1.0)
    m = margin(z)
    inner = slice(m, m + GRID)
    return TileGrid(lon[inner, inner], lat[inner, inner], h[inner, inner], seam[inner, inner])


def build_tile(z: int, x: int, y: int, entries) -> bytes:
    from pydelatin import Delatin

    g = tile_grid(z, x, y, entries)
    tin = Delatin(np.ascontiguousarray(g.h, dtype=np.float32), width=GRID, height=GRID, max_error=max_error(z))
    v = np.rint(np.asarray(tin.vertices)[:, :2]).astype(np.int64)
    col, yup = v[:, 0], v[:, 1]  # pydelatin's y counts up from the last row (south)
    w, s, e, n = tile_bounds(z, x, y)
    d = spacing_deg(z)
    lon, lat = w + col * d, s + yup * d
    heights = g.h[GRID - 1 - yup, col]  # float64 grid values, not pydelatin's float32 copy
    return qmesh.gzip_tile(qmesh.encode(lon, lat, heights, np.asarray(tin.triangles, np.int64), (w, s, e, n)))


def point_heights(z: int, lon: np.ndarray, lat: np.ndarray, entries) -> np.ndarray:
    """Highest-priority valid height at arbitrary points, vertical offsets evaluated exactly (verify --deep)."""
    out = np.full(np.shape(lon), np.nan)
    for g in _groups(entries):
        need = np.isnan(out)
        if not need.any():
            break
        h, ok = sample_group(g, z, lon, lat, exact_offsets=True)
        out = np.where(need & ok, h, out)
    return out
```

- [ ] **Step 4: Run the tests**

Run: `cd scripts/scene && uv run pytest tests/test_terrain.py -v`
Expected: 5 passed. If `test_north_is_up` fails, the vertex mapping is mirrored: that is the spike's first pitfall (shiny blue back faces). Fix the mapping, never the test.

- [ ] **Step 5: Commit**

```bash
git add scripts/scene/camsim_scene/layers/terrain.py scripts/scene/tests/test_terrain.py
git commit -m "feat(scene): terrain tile job (priority merge, 30 m feather, pydelatin TIN)"
```

---

### Task 16: Imagery tile jobs (leaves and parents)

**Files:**
- Create: `scripts/scene/camsim_scene/layers/imagery.py`
- Test: `scripts/scene/tests/test_imagery.py`

**Interfaces:**
- Consumes: `context.Entry`; `sources.base.{project, to_uint8, M_PER_DEG}`; `config.TILE_PX`; `tiling.tile_bounds`.
- Produces: `pixel_grid(z, x, y) -> ((lon, lat), step_deg)`; `query_bounds(z, x, y) -> Bounds`; `leaf_rgb(z, x, y, entries) -> np.ndarray (3, 256, 256) uint8`; `leaf_tile(z, x, y, entries, quality) -> bytes`; `encode_jpeg(rgb, quality) -> bytes`; `parent_tile(children: dict[(dx, dy), bytes], quality) -> bytes` (child `(2x + dx, 2y + dy)`; `dy = 1` is the northern half, i.e. the top of the image).

- [ ] **Step 1: Write the failing tests**

`scripts/scene/tests/test_imagery.py`:

```python
import io

import numpy as np
from PIL import Image

from camsim_scene import tiling
from camsim_scene.context import WorkerState
from camsim_scene.layers import imagery
from fake_sources import fake_context

Z = 9


def decode(data):
    return np.asarray(Image.open(io.BytesIO(data)).convert("RGB")).astype(int)


def leaf(st, z, lon, lat):
    s = tiling.tile_size_deg(z)
    x, y = int((lon + 180) // s), int((lat + 90) // s)
    return decode(imagery.leaf_tile(z, x, y, st.index["imagery"].query(imagery.query_bounds(z, x, y)), 85))


def test_leaf_is_north_up_and_from_the_high_priority_source(tmp_path):
    st = WorkerState(fake_context(tmp_path))  # hi_rgb: red 250 in its north half, 200 in the south half
    img = leaf(st, 11, 10.25, 10.25)  # z11 tile inside hi_rgb, straddling its middle (10.25 N)
    assert abs(img[5, :, 0].mean() - 250) < 8 and abs(img[-5, :, 0].mean() - 200) < 8
    assert abs(img[:, :, 2].mean() - 50) < 8


def test_nodata_falls_through_to_the_next_source(tmp_path):
    st = WorkerState(fake_context(tmp_path))
    img = leaf(st, Z, 5.0, 5.0)  # outside hi_rgb: base colour
    assert np.abs(img.reshape(-1, 3).mean(0) - [40, 80, 160]).max() < 8


def test_parent_places_children_by_quadrant():
    colours = {(0, 1): (255, 0, 0), (1, 1): (0, 255, 0), (0, 0): (0, 0, 255), (1, 0): (255, 255, 0)}
    kids = {k: imagery.encode_jpeg(np.broadcast_to(np.array(c, np.uint8)[:, None, None], (3, 256, 256)).copy(), 95) for k, c in colours.items()}
    p = decode(imagery.parent_tile(kids, 95))
    assert p.shape == (256, 256, 3)
    for (dx, dy), c in colours.items():
        r0, c0 = (1 - dy) * 128, dx * 128
        assert np.abs(p[r0 + 32 : r0 + 96, c0 + 32 : c0 + 96].reshape(-1, 3).mean(0) - c).max() < 10


def test_jpeg_bytes_are_deterministic(tmp_path):
    st = WorkerState(fake_context(tmp_path))
    s = tiling.tile_size_deg(Z)
    x, y = int((10.2 + 180) // s), int((10.2 + 90) // s)
    entries = st.index["imagery"].query(imagery.query_bounds(Z, x, y))
    assert imagery.leaf_tile(Z, x, y, entries, 85) == imagery.leaf_tile(Z, x, y, entries, 85)
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd scripts/scene && uv run pytest tests/test_imagery.py -v`
Expected: FAIL, `ImportError`.

- [ ] **Step 3: Implement `layers/imagery.py`**

```python
"""Imagery tile jobs. Leaves (tiles without children) sample their sources at pixel centres in priority order,
nodata falling through; parents are a 2 x 2 box filter of their four children (always four: complete siblings).
JPEG q85 4:2:0 via Pillow."""

from __future__ import annotations

import io

import numpy as np
from PIL import Image

from ..config import TILE_PX
from ..sources.base import M_PER_DEG, project, to_uint8
from ..tiling import Bounds, tile_bounds


def pixel_grid(z: int, x: int, y: int):
    w, s, e, n = tile_bounds(z, x, y)
    d = (e - w) / TILE_PX
    k = np.arange(TILE_PX) + 0.5
    return np.meshgrid(w + k * d, n - k * d), d


def query_bounds(z: int, x: int, y: int) -> Bounds:
    w, s, e, n = tile_bounds(z, x, y)
    d = (e - w) / TILE_PX
    return (w - d, s - d, e + d, n + d)


def leaf_rgb(z: int, x: int, y: int, entries) -> np.ndarray:
    (lon, lat), d = pixel_grid(z, x, y)
    rgb = np.zeros((3, TILE_PX, TILE_PX), np.uint8)
    filled = np.zeros((TILE_PX, TILE_PX), bool)
    for e in entries:
        if filled.all():
            break
        slon, slat = e.transform.to_source_geographic(lon, lat)
        px, py = project(e.raster, slon, slat)
        vals, ok = e.raster.sample(px, py, d * M_PER_DEG)
        take = ok & ~filled
        if take.any():
            rgb[:, take] = to_uint8(e.raster, vals)[:, take]
            filled |= take
    return rgb


def encode_jpeg(rgb: np.ndarray, quality: int) -> bytes:
    buf = io.BytesIO()
    Image.fromarray(np.ascontiguousarray(np.moveaxis(rgb, 0, -1))).save(
        buf, "JPEG", quality=quality, subsampling=2, optimize=False, progressive=False
    )
    return buf.getvalue()


def leaf_tile(z: int, x: int, y: int, entries, quality: int) -> bytes:
    return encode_jpeg(leaf_rgb(z, x, y, entries), quality)


def parent_tile(children: dict[tuple[int, int], bytes], quality: int) -> bytes:
    if len(children) != 4:
        raise ValueError(f"a parent needs its four children, got {sorted(children)}")
    mosaic = np.zeros((2 * TILE_PX, 2 * TILE_PX, 3), np.uint16)
    for (dx, dy), data in children.items():
        r0, c0 = (1 - dy) * TILE_PX, dx * TILE_PX
        mosaic[r0 : r0 + TILE_PX, c0 : c0 + TILE_PX] = np.asarray(Image.open(io.BytesIO(data)).convert("RGB"))
    small = (mosaic[0::2, 0::2] + mosaic[1::2, 0::2] + mosaic[0::2, 1::2] + mosaic[1::2, 1::2] + 2) // 4
    return encode_jpeg(np.moveaxis(small.astype(np.uint8), -1, 0), quality)
```

- [ ] **Step 4: Run the tests**

Run: `cd scripts/scene && uv run pytest tests/test_imagery.py -v`
Expected: 4 passed.

- [ ] **Step 5: Commit**

```bash
git add scripts/scene/camsim_scene/layers/imagery.py scripts/scene/tests/test_imagery.py
git commit -m "feat(scene): imagery leaf and parent tile jobs"
```

---

### Task 17: Engine (resume markers, process pool, lock, stale tiles, stats)

**Files:**
- Create: `scripts/scene/camsim_scene/engine.py`
- Test: `scripts/scene/tests/test_engine.py`

**Interfaces:**
- Consumes: `fsutil.{atomic_write, sha256_bytes, sha256_file}`; `manifest.STATE_DIR`; `tiling.{LayerPlan, make_keys}`.
- Produces: `TileResult(layer, z, x, y, skipped, size, sha256, seconds, pid, rss_mb)`; `LayerStats(tiles=0, built=0, skipped=0, bytes=0, seconds=0.0, peak_rss_mb=0.0)` with `add(result)`; `Markers(pkg)` with `path`, `read`, `write`, `output_for_file(relpath) -> str | None`; `inputs_hash(*parts: str) -> str`; `tile_path(pkg, layer, z, x, y, ext) -> Path`; `run_tile(pkg, markers, layer, ext, z, x, y, inputs, produce: Callable[[], bytes]) -> TileResult`; `peak_rss_mb() -> float`; `BuildLocked(Exception)`, `BuildLock(pkg)` (context manager); `run_pool(fn, phases, jobs, initializer, initargs, on_result)` (phases: iterable of iterables of chunks; each phase drains before the next starts; `fn(chunk) -> list[TileResult]`); `remove_stale(pkg, layer, ext, plan) -> int`; `Progress(layer, total, json_lines=False, stream=sys.stderr)` with `update(result)` and `done()`.

Resume rule (spec "Engine"): marker `<pkg>/.state/<layer>/<z>/<x>/<y>` = `{"inputs", "output"}`; skip when `inputs` match and the tile file's sha256 equals `output`. Markers are written after the tile, so a crash between the two only causes a rebuild.

- [ ] **Step 1: Write the failing tests**

`scripts/scene/tests/test_engine.py`:

```python
import json
import os

import numpy as np
import pytest

from camsim_scene import engine, tiling
from camsim_scene.engine import BuildLock, BuildLocked, LayerStats, Markers


def produce_counter(calls, data=b"tile"):
    def produce():
        calls.append(1)
        return data

    return produce


def test_run_tile_builds_then_skips(tmp_path):
    m, calls = Markers(tmp_path), []
    r1 = engine.run_tile(tmp_path, m, "terrain", "terrain", 3, 1, 2, "in-1", produce_counter(calls))
    r2 = engine.run_tile(tmp_path, m, "terrain", "terrain", 3, 1, 2, "in-1", produce_counter(calls))
    assert not r1.skipped and r2.skipped and len(calls) == 1
    assert (tmp_path / "terrain/3/1/2.terrain").read_bytes() == b"tile"
    assert m.output_for_file("terrain/3/1/2.terrain") == r1.sha256


def test_changed_inputs_or_tampered_file_rebuild(tmp_path):
    m, calls = Markers(tmp_path), []
    engine.run_tile(tmp_path, m, "imagery", "jpg", 0, 0, 0, "a", produce_counter(calls))
    engine.run_tile(tmp_path, m, "imagery", "jpg", 0, 0, 0, "b", produce_counter(calls))
    (tmp_path / "imagery/0/0/0.jpg").write_bytes(b"tampered")
    engine.run_tile(tmp_path, m, "imagery", "jpg", 0, 0, 0, "b", produce_counter(calls))
    assert len(calls) == 3 and (tmp_path / "imagery/0/0/0.jpg").read_bytes() == b"tile"


def test_failed_job_leaves_no_file_and_no_marker(tmp_path):
    def boom():
        raise KeyboardInterrupt

    with pytest.raises(KeyboardInterrupt):
        engine.run_tile(tmp_path, Markers(tmp_path), "terrain", "terrain", 1, 0, 0, "x", boom)
    assert not (tmp_path / "terrain/1/0/0.terrain").exists() and Markers(tmp_path).read("terrain", 1, 0, 0) is None


def test_build_lock_refuses_second_holder(tmp_path):
    with BuildLock(tmp_path):
        with pytest.raises(BuildLocked, match="another build"):
            with BuildLock(tmp_path):
                pass
    with BuildLock(tmp_path):
        pass


def test_remove_stale_keeps_exactly_the_plan(tmp_path):
    plan = tiling.LayerPlan({0: tiling.make_keys([0, 1], [0, 0]), 1: tiling.make_keys([0], [0])}, {})
    m = Markers(tmp_path)
    for rel in ["terrain/0/0/0.terrain", "terrain/0/1/0.terrain", "terrain/1/0/0.terrain", "terrain/1/3/1.terrain",
                "terrain/2/0/0.terrain", "terrain/0/0/.0.terrain.abc.part", "terrain/layer.json"]:
        (tmp_path / rel).parent.mkdir(parents=True, exist_ok=True)
        (tmp_path / rel).write_bytes(b"x")
    m.write("terrain", 1, 3, 1, "i", "o")
    m.write("terrain", 2, 0, 0, "i", "o")
    removed = engine.remove_stale(tmp_path, "terrain", "terrain", plan)
    left = sorted(str(p.relative_to(tmp_path)) for p in (tmp_path / "terrain").rglob("*") if p.is_file())
    assert left == ["terrain/0/0/0.terrain", "terrain/0/1/0.terrain", "terrain/1/0/0.terrain", "terrain/layer.json"]
    assert removed == 3 and m.read("terrain", 1, 3, 1) is None and m.read("terrain", 2, 0, 0) is None


def square(chunk):
    return [engine.TileResult("t", 0, x, 0, False, x * x, "", 0.0, os.getpid(), 0.0) for x in chunk]


def noop_init():
    pass


@pytest.mark.parametrize("jobs", [1, 2])
def test_run_pool_runs_phases_in_order(jobs):
    seen = []
    phases = [[[1, 2], [3]], [[4]]]
    engine.run_pool(square, phases, jobs, noop_init, (), lambda r: seen.append(r.x))
    assert sorted(seen[:3]) == [1, 2, 3] and seen[3] == 4


def test_layer_stats_and_progress_json(capsys):
    st = LayerStats()
    r = engine.TileResult("terrain", 1, 0, 0, False, 100, "ab", 0.5, 1, 512.0)
    st.add(r)
    st.add(engine.TileResult("terrain", 1, 1, 0, True, 50, "cd", 0.0, 2, 700.0))
    assert (st.tiles, st.built, st.skipped, st.bytes, st.peak_rss_mb) == (2, 1, 1, 150, 700.0)
    import io

    out = io.StringIO()
    p = engine.Progress("terrain", 2, json_lines=True, stream=out)
    p.update(r)
    p.done()
    lines = [json.loads(line) for line in out.getvalue().splitlines()]
    assert lines[-1]["layer"] == "terrain" and lines[-1]["done"] == 1 and lines[-1]["total"] == 2
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd scripts/scene && uv run pytest tests/test_engine.py -v`
Expected: FAIL, `ImportError`.

- [ ] **Step 3: Implement `engine.py`**

```python
"""Tile scheduler: resume markers, atomic writes, a spawn-based process pool, a per-package build lock, removal
of tiles that left the plan, and per-layer stats (tiles, bytes, wall time, peak RSS per worker)."""

from __future__ import annotations

import fcntl
import json
import multiprocessing
import os
import re
import resource
import shutil
import sys
import time
from collections.abc import Callable, Iterable
from concurrent.futures import FIRST_COMPLETED, ProcessPoolExecutor, wait
from dataclasses import asdict, dataclass
from pathlib import Path

import numpy as np

from .fsutil import atomic_write, sha256_bytes, sha256_file
from .manifest import STATE_DIR
from .tiling import LayerPlan, make_keys

TILE_RE = re.compile(r"^(?P<layer>terrain|imagery)/(?P<z>\d+)/(?P<x>\d+)/(?P<y>\d+)\.(terrain|jpg)$")


@dataclass
class TileResult:
    layer: str
    z: int
    x: int
    y: int
    skipped: bool
    size: int
    sha256: str
    seconds: float
    pid: int
    rss_mb: float


@dataclass
class LayerStats:
    tiles: int = 0
    built: int = 0
    skipped: int = 0
    bytes: int = 0
    seconds: float = 0.0
    peak_rss_mb: float = 0.0

    def add(self, r: TileResult) -> None:
        self.tiles += 1
        self.built += not r.skipped
        self.skipped += r.skipped
        self.bytes += r.size
        self.peak_rss_mb = max(self.peak_rss_mb, r.rss_mb)

    def to_dict(self) -> dict:
        return asdict(self)


def peak_rss_mb() -> float:
    r = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    return r / 1024.0 if sys.platform.startswith("linux") else r / (1024.0 * 1024.0)


def inputs_hash(*parts: str) -> str:
    return sha256_bytes("\n".join(parts).encode())


def tile_path(pkg: Path, layer: str, z: int, x: int, y: int, ext: str) -> Path:
    return Path(pkg) / layer / str(z) / str(x) / f"{y}.{ext}"


class Markers:
    def __init__(self, pkg: Path):
        self.root = Path(pkg) / STATE_DIR

    def path(self, layer: str, z: int, x: int, y: int) -> Path:
        return self.root / layer / str(z) / str(x) / str(y)

    def read(self, layer: str, z: int, x: int, y: int) -> dict | None:
        try:
            return json.loads(self.path(layer, z, x, y).read_text())
        except (FileNotFoundError, json.JSONDecodeError):
            return None

    def write(self, layer: str, z: int, x: int, y: int, inputs: str, output: str) -> None:
        atomic_write(self.path(layer, z, x, y), json.dumps({"inputs": inputs, "output": output}, sort_keys=True).encode())

    def output_for_file(self, relpath: str) -> str | None:
        m = TILE_RE.match(relpath)
        if not m:
            return None
        rec = self.read(m["layer"], int(m["z"]), int(m["x"]), int(m["y"]))
        return rec["output"] if rec else None


def run_tile(pkg: Path, markers: Markers, layer: str, ext: str, z: int, x: int, y: int, inputs: str,
             produce: Callable[[], bytes]) -> TileResult:
    t0 = time.perf_counter()
    path = tile_path(pkg, layer, z, x, y, ext)
    rec = markers.read(layer, z, x, y)
    if rec and rec["inputs"] == inputs and path.exists():
        sha = sha256_file(path)
        if sha == rec["output"]:
            return TileResult(layer, z, x, y, True, path.stat().st_size, sha, time.perf_counter() - t0, os.getpid(), peak_rss_mb())
    data = produce()
    atomic_write(path, data)
    sha = sha256_bytes(data)
    markers.write(layer, z, x, y, inputs, sha)
    return TileResult(layer, z, x, y, False, len(data), sha, time.perf_counter() - t0, os.getpid(), peak_rss_mb())


class BuildLocked(Exception):
    pass


class BuildLock:
    """Exclusive flock on <pkg>/.state/lock for the duration of a build."""

    def __init__(self, pkg: Path):
        self.path = Path(pkg) / STATE_DIR / "lock"
        self.fd: int | None = None

    def __enter__(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.fd = os.open(self.path, os.O_RDWR | os.O_CREAT, 0o644)
        try:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            os.close(self.fd)
            self.fd = None
            raise BuildLocked(f"another build is running on {self.path.parents[1]}") from None
        return self

    def __exit__(self, *exc):
        fcntl.flock(self.fd, fcntl.LOCK_UN)
        os.close(self.fd)
        self.fd = None


def run_pool(fn, phases: Iterable[Iterable], jobs: int, initializer, initargs, on_result) -> None:
    """Run fn(chunk) -> list[TileResult] over every chunk; each phase finishes before the next one starts."""
    if jobs <= 1:
        initializer(*initargs)
        for phase in phases:
            for chunk in phase:
                for r in fn(chunk):
                    on_result(r)
        return
    ctx = multiprocessing.get_context("spawn")
    with ProcessPoolExecutor(max_workers=jobs, mp_context=ctx, initializer=initializer, initargs=initargs) as ex:
        try:
            for phase in phases:
                pending = set()
                for chunk in phase:
                    pending.add(ex.submit(fn, chunk))
                    if len(pending) >= 4 * jobs:
                        done, pending = wait(pending, return_when=FIRST_COMPLETED)
                        for f in done:
                            for r in f.result():
                                on_result(r)
                for f in wait(pending).done:
                    for r in f.result():
                        on_result(r)
        except BaseException:
            ex.shutdown(wait=False, cancel_futures=True)
            raise


def _stale_in(root: Path, plan: LayerPlan, ext: str | None) -> int:
    """Remove z/x/y files (ext) or markers (ext None) under root that aren't in plan. Returns the count."""
    removed = 0
    if not root.exists():
        return 0
    suffix = f".{ext}" if ext else ""
    for zdir in list(root.iterdir()):
        if not zdir.is_dir():
            continue
        if not zdir.name.isdigit() or int(zdir.name) not in plan.tiles:
            removed += sum(1 for p in zdir.rglob("*") if p.is_file())
            shutil.rmtree(zdir)
            continue
        planned = plan.tiles[int(zdir.name)]
        for xdir in list(zdir.iterdir()):
            if not xdir.is_dir():
                xdir.unlink()
                removed += 1
                continue
            files = [p for p in xdir.iterdir() if p.is_file()]
            ys, keep = [], []
            for p in files:
                stem = p.name[: -len(suffix)] if suffix and p.name.endswith(suffix) else (p.name if not suffix else None)
                ok = xdir.name.isdigit() and stem is not None and stem.isdigit()
                ys.append(int(stem) if ok else -1)
                keep.append(ok)
            if files:
                k = make_keys(np.full(len(ys), int(xdir.name) if xdir.name.isdigit() else -1), np.asarray(ys))
                keep = np.asarray(keep) & np.isin(k, planned)
                for p, kp in zip(files, keep):
                    if not kp:
                        p.unlink()
                        removed += 1
            if not any(xdir.iterdir()):
                xdir.rmdir()
    return removed


def remove_stale(pkg: Path, layer: str, ext: str, plan: LayerPlan) -> int:
    """Delete tiles (and their markers) that the current plan doesn't contain, and leftover temp files."""
    n = _stale_in(Path(pkg) / layer, plan, ext)
    _stale_in(Path(pkg) / STATE_DIR / layer, plan, None)
    return n


class Progress:
    def __init__(self, layer: str, total: int, json_lines: bool = False, stream=sys.stderr, every_s: float = 2.0):
        self.layer, self.total, self.json_lines, self.stream, self.every_s = layer, total, json_lines, stream, every_s
        self.done_n = self.skipped = 0
        self.t0 = self.last = time.monotonic()

    def update(self, r: TileResult) -> None:
        self.done_n += 1
        self.skipped += r.skipped
        now = time.monotonic()
        if now - self.last >= self.every_s:
            self.last = now
            self._emit(now)

    def done(self) -> None:
        self._emit(time.monotonic())

    def _emit(self, now: float) -> None:
        rate = self.done_n / max(now - self.t0, 1e-9)
        if self.json_lines:
            rec = {"layer": self.layer, "done": self.done_n, "total": self.total, "skipped": self.skipped, "tiles_per_s": round(rate, 1)}
            print(json.dumps(rec), file=self.stream, flush=True)
        else:
            print(f"{self.layer}: {self.done_n}/{self.total} ({self.skipped} skipped) {rate:.0f} tiles/s", file=self.stream, flush=True)
```

- [ ] **Step 4: Run the tests**

Run: `cd scripts/scene && uv run pytest tests/test_engine.py -v`
Expected: 8 passed (the `jobs=2` case spawns two workers; it imports `test_engine` in them).

- [ ] **Step 5: Commit**

```bash
git add scripts/scene/camsim_scene/engine.py scripts/scene/tests/test_engine.py
git commit -m "feat(scene): tile engine (resume markers, spawn pool, build lock, stale-tile removal, stats)"
```

---
### Task 18: Pipeline — plan, fetch, build (end-to-end on synthetic sources)

**Files:**
- Create: `scripts/scene/camsim_scene/pipeline.py`
- Modify: `scripts/scene/tests/fake_sources.py` (add `build_synthetic`)
- Test: `scripts/scene/tests/test_pipeline.py`

**Interfaces:**
- Consumes: everything above: `config.{LAYERS, TILING, ScenePlan, layer_settings}`, `context.{BuildContext, WorkerState, coverage, to_asset}`, `engine.*`, `layers.{terrain, imagery, landcover}`, `licences.{DEFAULT_ALLOW, check_allowed, attribution_text}`, `manifest.*`, `cache.Cache`, `net.Http`, `qmesh.layer_json`, `sources.make_source`, `sources.base.{Area, Layer, footprint_lonlat}`, `tiling.{plan_tiles, available_ranges, split_keys}`, `tms.{tilemapresource_xml, plan_bounds}`, `datum.manifest_section`.
- Produces: `PlanError(Exception)`, `BuildError(Exception)`; `TOOL`; `plan_scene(plan: ScenePlan, pkg: Path, http=None, out=sys.stderr) -> Manifest` (writes `manifest.json` with unhashed assets, prints the estimate); `estimate(m) -> dict` (`cache_bytes`, `unknown_sizes`, `tiles: {terrain, imagery, landcover}`, `package_bytes`); `fetch_scene(pkg, cache) -> Manifest` (hashes, sizes, prepared files, footprints, grid hashes; writes the manifest after each source); `make_context(pkg, m, cache) -> BuildContext` (refetches cache misses, fails on hash mismatch); `init_worker(ctx)`, `terrain_batch(keys)`, `imagery_batch(items)` (pool functions); `build_scene(pkg, cache, jobs=None, json_progress=False, max_worker_rss_mb=2048.0) -> dict` (the `build.json` content).
- Produces (`tests/fake_sources.py`): `build_synthetic(tmp_path, scene=None, jobs=1, name="pkg", cache=None) -> (pkg, cache, info)`.

Build order inside the lock: plan terrain + imagery tiles from regions and footprints → remove stale tiles → terrain jobs (one phase) → `terrain/layer.json` → imagery jobs (one phase per zoom, deepest first) → `imagery/tilemapresource.xml` → land cover (whole layer, skipped when `.state/landcover.json` matches) → `ATTRIBUTION.txt` → `hashes.txt` (tile hashes from markers) → `manifest.json` with `hashes_sha256` → `build.json`. The RSS bound is checked after everything is written, so a violation never wastes the build.

- [ ] **Step 1: Add the build helper to `tests/fake_sources.py`**

```python
def build_synthetic(tmp_path: Path, scene: dict | None = None, jobs: int = 1, name: str = "pkg", cache=None):
    """plan + fetch + build a synthetic package; returns (pkg, cache, build info)."""
    import io

    from camsim_scene.cache import Cache
    from camsim_scene.pipeline import build_scene, plan_scene
    from fakes import FakeHttp

    scene = scene or synthetic_scene(tmp_path / "src")
    cache = cache or Cache(tmp_path / "cache")
    pkg = tmp_path / name
    plan_scene(parse_scene(scene), pkg, http=FakeHttp({}), out=io.StringIO())
    return pkg, cache, build_scene(pkg, cache, jobs=jobs)
```

- [ ] **Step 2: Write the failing tests**

`scripts/scene/tests/test_pipeline.py`:

```python
import io
import json

import numpy as np
import pytest
import shapely

from camsim_scene import fsutil, qmesh, tiling
from camsim_scene.config import parse_scene
from camsim_scene.engine import BuildLock, BuildLocked
from camsim_scene.layers import terrain
from camsim_scene.licences import LicenceError
from camsim_scene.manifest import Manifest
from camsim_scene.pipeline import BuildError, PlanError, build_scene, estimate, plan_scene
from fake_sources import build_synthetic, source, synthetic_scene
from fakes import FakeHttp
from rasters import write_geotiff


def test_end_to_end_layout_and_hashes(tmp_path):
    pkg, _, info = build_synthetic(tmp_path)
    lj = json.loads((pkg / "terrain/layer.json").read_text())
    assert lj["available"][0] == [{"startX": 0, "startY": 0, "endX": 1, "endY": 0}]
    assert lj["available"][2] == [{"startX": 0, "startY": 0, "endX": 7, "endY": 3}] and lj["maxzoom"] == 6
    assert "<TileSet href=\"6\"" in (pkg / "imagery/tilemapresource.xml").read_text()
    m = Manifest.load(pkg / "manifest.json")
    assert m.hashes_sha256 == fsutil.sha256_file(pkg / "hashes.txt")
    t = info["layers"]["terrain"]
    assert t["tiles"] == t["built"] == len(list((pkg / "terrain").rglob("*.terrain"))) > 0
    assert info["layers"]["imagery"]["built"] == len(list((pkg / "imagery").rglob("*.jpg")))
    for p in (pkg / "terrain").rglob("*.terrain"):
        assert np.isfinite(qmesh.decode(p.read_bytes()).heights()).all()
    assert "synthetic test data" in (pkg / "ATTRIBUTION.txt").read_text()
    assert all(a.metadata["footprint"] for a in m.source("hi_dem").assets)


def test_rebuild_skips_everything(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path)
    before = (pkg / "hashes.txt").read_bytes()
    info = build_scene(pkg, cache, jobs=1)
    assert info["layers"]["terrain"]["built"] == 0 and info["layers"]["imagery"]["built"] == 0
    assert (pkg / "hashes.txt").read_bytes() == before


def test_worker_count_does_not_change_bytes(tmp_path):
    scene = synthetic_scene(tmp_path / "src")
    a, cache, _ = build_synthetic(tmp_path, scene, jobs=1, name="a")
    b, _, _ = build_synthetic(tmp_path, scene, jobs=2, name="b", cache=cache)
    assert (a / "hashes.txt").read_bytes() == (b / "hashes.txt").read_bytes()
    assert (a / "manifest.json").read_bytes() == (b / "manifest.json").read_bytes()


def test_rebuild_from_the_manifest_alone_is_identical(tmp_path):
    a, cache, _ = build_synthetic(tmp_path, name="a")
    b = tmp_path / "b"
    b.mkdir()
    (b / "manifest.json").write_bytes((a / "manifest.json").read_bytes())
    build_scene(b, cache, jobs=1)
    assert (a / "hashes.txt").read_bytes() == (b / "hashes.txt").read_bytes()


def test_imagery_setting_dirties_only_imagery(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path)
    m = Manifest.load(pkg / "manifest.json")
    m.layers["imagery"]["jpeg_quality"] = 70
    m.write(pkg / "manifest.json")
    info = build_scene(pkg, cache, jobs=1)
    assert info["layers"]["terrain"]["built"] == 0
    assert info["layers"]["imagery"]["built"] == info["layers"]["imagery"]["tiles"]


def test_new_asset_dirties_only_its_tiles(tmp_path):
    scene = synthetic_scene(tmp_path / "src")
    pkg, cache, _ = build_synthetic(tmp_path, scene)
    patch_bbox = [10.40, 10.40, 10.45, 10.45]
    write_geotiff(tmp_path / "src/patch.tif", np.full((50, 50), 300.0, np.float32), 10.40, 10.45, 0.001, nodata=-9999.0, overviews=())
    scene["sources"]["hi_dem"]["files"].append({"id": "patch", "path": str(tmp_path / "src/patch.tif"), "bbox": patch_bbox})
    plan_scene(parse_scene(scene), pkg, http=FakeHttp({}), out=io.StringIO())
    info = build_scene(pkg, cache, jobs=1)
    keys = tiling.available_keys(json.loads((pkg / "terrain/layer.json").read_text())["available"])
    patch = shapely.box(*patch_bbox)
    expected = sum(
        shapely.box(*terrain.query_bounds(z, int(k) >> 32, int(k) & 0xFFFFFFFF)).intersects(patch) for z, ks in keys.items() for k in ks
    )
    assert 0 < info["layers"]["terrain"]["built"] == expected < info["layers"]["terrain"]["tiles"]
    assert info["layers"]["imagery"]["built"] == 0


def test_build_removes_stale_tiles(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path)
    (pkg / "terrain/9/0").mkdir(parents=True)
    (pkg / "terrain/9/0/0.terrain").write_bytes(b"old")
    (pkg / "imagery/6/0").mkdir(parents=True, exist_ok=True)
    (pkg / "imagery/6/0/0.jpg").write_bytes(b"old")
    build_scene(pkg, cache, jobs=1)
    assert not (pkg / "terrain/9").exists() and not (pkg / "imagery/6/0/0.jpg").exists()
    assert "terrain/9/0/0.terrain" not in (pkg / "hashes.txt").read_text()


def test_build_refuses_when_locked(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path)
    with BuildLock(pkg), pytest.raises(BuildLocked):
        build_scene(pkg, cache, jobs=1)


def test_all_nodata_asset_adds_no_tiles(tmp_path):
    scene = synthetic_scene(tmp_path / "src")
    base, _, info0 = build_synthetic(tmp_path, scene, name="base")
    write_geotiff(tmp_path / "src/empty.tif", np.zeros((3, 100, 100), np.uint8), 10.0, 10.5, 0.005, overviews=())
    scene["sources"]["empty_rgb"] = source("imagery", tmp_path / "src/empty.tif", [10.0, 10.0, 10.5, 10.5], max_zoom=12,
                                           bands=[1, 2, 3], nodata_rule="all_zero")
    scene["priorities"]["imagery"] = ["empty_rgb", *scene["priorities"]["imagery"]]
    pkg, _, info = build_synthetic(tmp_path, scene, name="with_empty")
    assert info["layers"]["imagery"]["tiles"] == info0["layers"]["imagery"]["tiles"]
    assert Manifest.load(pkg / "manifest.json").source("empty_rgb").assets[0].metadata["footprint"] is None


def test_licence_outside_the_allow_list_fails_plan(tmp_path):
    scene = synthetic_scene(tmp_path / "src")
    scene["sources"]["hi_rgb"]["licence"] = "ODbL-1.0"
    with pytest.raises(LicenceError, match="--allow ODbL-1.0"):
        plan_scene(parse_scene(scene), tmp_path / "pkg", http=FakeHttp({}), out=io.StringIO())
    scene["allow"] = ["ODbL-1.0"]
    m = plan_scene(parse_scene(scene), tmp_path / "pkg", http=FakeHttp({}), out=io.StringIO())
    assert "ODbL-1.0" in m.licence_allow


def test_source_under_the_wrong_layer_fails_plan(tmp_path):
    scene = synthetic_scene(tmp_path / "src")
    scene["priorities"]["terrain"] = ["hi_rgb", "base_dem"]
    with pytest.raises(PlanError, match="hi_rgb"):
        plan_scene(parse_scene(scene), tmp_path / "pkg", http=FakeHttp({}), out=io.StringIO())


def test_estimate_before_fetch(tmp_path):
    out = io.StringIO()
    m = plan_scene(parse_scene(synthetic_scene(tmp_path / "src")), tmp_path / "pkg", http=FakeHttp({}), out=out)
    est = estimate(m)
    assert est["tiles"]["terrain"] > 0 and est["package_bytes"] > 0 and "tiles" in out.getvalue()


def test_rss_bound_is_reported_after_a_complete_build(tmp_path):
    scene = synthetic_scene(tmp_path / "src")
    from camsim_scene.cache import Cache

    pkg = tmp_path / "pkg"
    plan_scene(parse_scene(scene), pkg, http=FakeHttp({}), out=io.StringIO())
    with pytest.raises(BuildError, match="RSS"):
        build_scene(pkg, Cache(tmp_path / "cache"), jobs=1, max_worker_rss_mb=1.0)
    assert (pkg / "hashes.txt").exists() and (pkg / "build.json").exists()
```

- [ ] **Step 3: Run them to see them fail**

Run: `cd scripts/scene && uv run pytest tests/test_pipeline.py -v`
Expected: FAIL, `ModuleNotFoundError: camsim_scene.pipeline`.

- [ ] **Step 4: Implement `pipeline.py`**

```python
"""plan -> fetch -> build.

plan:  discover each source's assets for its area (globe / ring / bbox) and write manifest.json (unhashed).
fetch: download whole assets into the cache, hash them, prepare derived files, compute footprints, hash grids.
build: plan the pyramids, run the tile jobs (resumable), then write layer.json, tilemapresource.xml, land cover,
       ATTRIBUTION.txt, hashes.txt, the final manifest.json and build.json."""

from __future__ import annotations

import datetime as dt
import json
import logging
import os
import platform
import shutil
import subprocess
import sys
import time
from functools import partial
from pathlib import Path

import numpy as np
import shapely

from . import __version__, datum
from .cache import Cache
from .config import LAYERS, TILING, ScenePlan, layer_settings
from .context import BuildContext, WorkerState, coverage, to_asset
from .engine import (
    BuildLock, LayerStats, Markers, Progress, inputs_hash, peak_rss_mb, remove_stale, run_pool, run_tile, tile_path,
)
from .fsutil import atomic_write, sha256_file
from .layers import imagery, landcover, terrain
from .licences import DEFAULT_ALLOW, attribution_text, check_allowed
from .manifest import STATE_DIR, AssetRecord, Manifest, SourceRecord, canonical_json, write_hashes
from .net import Http
from .qmesh import layer_json
from .sources import make_source
from .sources.base import Area, Layer, footprint_lonlat
from .tiling import LayerPlan, available_ranges, plan_tiles, split_keys
from .tms import plan_bounds, tilemapresource_xml

log = logging.getLogger("camsim_scene")
TOOL = {"name": "camsim-scene", "version": __version__}
CHUNK = 64
EST_KB = {"terrain": 7.5, "imagery": 16.0, "landcover": 15.0}  # per tile, from the R0 spike


class PlanError(Exception):
    pass


class BuildError(Exception):
    pass


def _utc_now() -> str:
    return dt.datetime.now(dt.UTC).strftime("%Y-%m-%dT%H:%M:%SZ")


# ---------------------------------------------------------------- plan


def plan_scene(plan: ScenePlan, pkg: Path, http=None, out=sys.stderr) -> Manifest:
    http = http or Http()
    built: dict[str, tuple[str, object]] = {}
    for layer in LAYERS:
        for sid in plan.priorities[layer]:
            if sid in built:
                raise PlanError(f"source {sid} is listed under more than one layer")
            opts = dict(plan.source_options.get(sid, {}))
            adapter = opts.pop("adapter", sid)
            src = make_source(sid, adapter, opts, http)
            if src.layer != layer:
                raise PlanError(f"source {sid} is a {src.layer} source but is listed under priorities.{layer}")
            built[sid] = (adapter, src)
    allow = sorted(set(DEFAULT_ALLOW) | set(plan.allow))
    check_allowed({sid: s.licence for sid, (_, s) in built.items()}, allow)
    records, datums = [], set()
    for sid, (adapter, src) in built.items():
        assets = src.discover(Area(plan.area(src.area_kind)))
        if not assets:
            log.warning("%s: no assets in %s", sid, plan.area(src.area_kind))
        for a in assets:
            if a.role == "data" and src.layer != Layer.LANDCOVER and (a.metadata.get("datum") or src.datum):
                datums.add(a.metadata.get("datum") or src.datum)
        records.append(
            SourceRecord(sid, adapter, src.dataset, src.version, src.licence, src.attribution, src.options(),
                         [AssetRecord(a.id, a.url, None, a.size, a.group, a.rank, a.role, dict(a.metadata)) for a in assets])
        )
    m = Manifest(
        name=plan.name, bbox=list(plan.bbox), seed=plan.seed, regions=[r.to_dict() for r in plan.regions()], tiling=TILING,
        layers=layer_settings(plan), datum=datum.manifest_section(datums), sources=records, licence_allow=allow, tool=TOOL,
    )
    pkg.mkdir(parents=True, exist_ok=True)
    m.write(pkg / "manifest.json")
    est = estimate(m)
    unknown = f" (+{est['unknown_sizes']} assets of unknown size)" if est["unknown_sizes"] else ""
    print(
        f"plan {m.name}: {sum(len(s.assets) for s in m.sources)} assets, cache {est['cache_bytes'] / 1e9:.1f} GB{unknown}; "
        f"tiles terrain {est['tiles']['terrain']}, imagery {est['tiles']['imagery']}, land cover {est['tiles']['landcover']}; "
        f"package ~{est['package_bytes'] / 1e9:.1f} GB",
        file=out,
    )
    return m


def estimate(m: Manifest) -> dict:
    sizes = [a.size for s in m.sources for a in s.assets]
    use_bbox = not m.is_fetched()
    tiles = {layer: plan_tiles(m.region_objs(), layer, coverage(m, layer, use_bbox=use_bbox)).count() for layer in ("terrain", "imagery")}
    lc = m.layers["landcover"]
    tiles["landcover"] = len(landcover.tiles_for_bbox(*lc["bounds"])) if lc["priorities"] else 0
    package = sum(tiles[k] * EST_KB[k] * 1024 for k in tiles)
    return {
        "cache_bytes": sum(v for v in sizes if v),
        "unknown_sizes": sum(1 for v in sizes if v is None),
        "tiles": tiles,
        "package_bytes": int(package),
    }


# ---------------------------------------------------------------- fetch


def fetch_scene(pkg: Path, cache: Cache) -> Manifest:
    m = Manifest.load(pkg / "manifest.json")
    for rec in m.sources:
        src = make_source(rec.id, rec.adapter, rec.options, cache.http)
        for a in rec.assets:
            path, a.sha256, a.size = cache.get(a.url, a.sha256, sign=src.sign)
            obj = to_asset(a)
            prepared = src.prepare(path, obj, cache) or path
            if a.role == "data" and src.layer != Layer.LANDCOVER and not src.global_coverage and "footprint" not in a.metadata:
                fp = footprint_lonlat(src.open(prepared, obj))
                a.metadata["footprint"] = None if fp is None else shapely.to_wkt(fp, rounding_precision=7)
                if fp is None:
                    log.warning("%s/%s holds no data; it is ignored", rec.id, a.id)
            log.info("fetched %s/%s (%d bytes)", rec.id, a.id, a.size)
        m.write(pkg / "manifest.json")  # an interrupted fetch keeps what it already hashed
    for g in m.datum["grids"].values():
        _, g["sha256"], _ = cache.get(g["url"], g.get("sha256"))
    m.write(pkg / "manifest.json")
    return m


def make_context(pkg: Path, m: Manifest, cache: Cache) -> BuildContext:
    paths = {}
    for rec in m.sources:
        src = make_source(rec.id, rec.adapter, rec.options, cache.http)
        for a in rec.assets:
            blob, _, _ = cache.get(a.url, a.sha256, sign=src.sign)  # refetches a miss; fails on a hash mismatch
            paths[f"{rec.id}/{a.id}"] = str(src.prepare(blob, to_asset(a), cache) or blob)
    grids = {name: str(cache.get(g["url"], g["sha256"])[0]) for name, g in m.datum["grids"].items()}
    return BuildContext(str(pkg), m.to_dict(), paths, grids)


# ---------------------------------------------------------------- workers

_STATE: WorkerState | None = None
_MARKERS: Markers | None = None


def init_worker(ctx: BuildContext) -> None:
    global _STATE, _MARKERS
    os.environ.setdefault("GDAL_CACHEMAX", "256")
    _STATE = WorkerState(ctx)
    _MARKERS = Markers(_STATE.pkg)


def _terrain_tile(z: int, x: int, y: int):
    st = _STATE
    entries = st.index["terrain"].query(terrain.query_bounds(z, x, y))
    inputs = inputs_hash(st.settings["terrain"], __version__, *sorted(e.sha256 for e in entries))
    return run_tile(st.pkg, _MARKERS, "terrain", "terrain", z, x, y, inputs, partial(terrain.build_tile, z, x, y, entries))


def terrain_batch(keys: list[tuple[int, int, int]]) -> list:
    return [_terrain_tile(*k) for k in keys]


def _parent_bytes(pkg: Path, z: int, x: int, y: int, quality: int) -> bytes:
    kids = {(dx, dy): tile_path(pkg, "imagery", z + 1, 2 * x + dx, 2 * y + dy, "jpg").read_bytes() for dx in (0, 1) for dy in (0, 1)}
    return imagery.parent_tile(kids, quality)


def _imagery_tile(z: int, x: int, y: int, leaf: bool):
    st = _STATE
    quality = st.manifest.layers["imagery"]["jpeg_quality"]
    if leaf:
        entries = st.index["imagery"].query(imagery.query_bounds(z, x, y))
        inputs = inputs_hash(st.settings["imagery"], __version__, "leaf", *sorted(e.sha256 for e in entries))
        produce = partial(imagery.leaf_tile, z, x, y, entries, quality)
    else:
        outs = []
        for dx, dy in ((0, 0), (1, 0), (0, 1), (1, 1)):
            rec = _MARKERS.read("imagery", z + 1, 2 * x + dx, 2 * y + dy)
            if rec is None:
                raise BuildError(f"imagery {z}/{x}/{y}: child {z + 1}/{2 * x + dx}/{2 * y + dy} was not built")
            outs.append(rec["output"])
        inputs = inputs_hash(st.settings["imagery"], __version__, "parent", *outs)
        produce = partial(_parent_bytes, st.pkg, z, x, y, quality)
    return run_tile(st.pkg, _MARKERS, "imagery", "jpg", z, x, y, inputs, produce)


def imagery_batch(items: list[tuple[int, int, int, bool]]) -> list:
    return [_imagery_tile(*it) for it in items]


# ---------------------------------------------------------------- build


def _chunks(items, n: int = CHUNK):
    buf = []
    for it in items:
        buf.append(it)
        if len(buf) == n:
            yield buf
            buf = []
    if buf:
        yield buf


def _terrain_items(plan: LayerPlan):
    for z in sorted(plan.tiles):
        x, y = split_keys(plan.tiles[z])
        for a, b in zip(x.tolist(), y.tolist()):
            yield (z, a, b)


def _imagery_phases(plan: LayerPlan):
    for z in range(plan.max_zoom, -1, -1):
        keys = plan.tiles[z]
        leaf = np.isin(keys, plan.leaves[z])
        x, y = split_keys(keys)
        yield _chunks((z, a, b, bool(lf)) for a, b, lf in zip(x.tolist(), y.tolist(), leaf.tolist()))


def _run_layer(ctx: BuildContext, layer: str, plan: LayerPlan, phases, fn, jobs: int, json_progress: bool) -> LayerStats:
    removed = remove_stale(Path(ctx.pkg), layer, "terrain" if layer == "terrain" else "jpg", plan)
    if removed:
        log.info("%s: removed %d tiles that left the plan", layer, removed)
    stats, prog = LayerStats(), Progress(layer, plan.count(), json_lines=json_progress)
    t0 = time.monotonic()

    def on_result(r):
        stats.add(r)
        prog.update(r)

    run_pool(fn, phases, jobs, init_worker, (ctx,), on_result)
    prog.done()
    stats.seconds = round(time.monotonic() - t0, 3)
    return stats


def _build_landcover(pkg: Path, m: Manifest, ctx: BuildContext) -> LayerStats:
    st, out = LayerStats(), pkg / "landcover"
    prio = m.layers["landcover"]["priorities"]
    if not prio:
        shutil.rmtree(out, ignore_errors=True)
        return st
    t0 = time.monotonic()
    rec = m.source(prio[0])
    paths = {a.url: Path(ctx.asset_paths[f"{rec.id}/{a.id}"]) for a in rec.assets}
    inputs = inputs_hash(m.layer_settings_hash("landcover"), __version__, *sorted(a.sha256 for a in rec.assets))
    marker, index_path = pkg / STATE_DIR / "landcover.json", out / "index.json"
    prev = json.loads(marker.read_text()) if marker.exists() else None
    if prev and prev["inputs"] == inputs and index_path.exists() and sha256_file(index_path) == prev["output"]:
        index = json.loads(index_path.read_text())
        st.skipped = len(index["tiles"])
    else:
        shutil.rmtree(out, ignore_errors=True)
        index = landcover.fetch(tuple(m.layers["landcover"]["bounds"]), out, reader=landcover.local_reader(paths), cut_by="camsim-scene")
        atomic_write(marker, json.dumps({"inputs": inputs, "output": sha256_file(index_path)}).encode())
        st.built = len(index["tiles"])
    st.tiles = len(index["tiles"])
    st.bytes = sum(p.stat().st_size for p in out.iterdir())
    st.seconds = round(time.monotonic() - t0, 3)
    st.peak_rss_mb = peak_rss_mb()
    return st


def _git_commit() -> str | None:
    if os.environ.get("CAMSIM_SCENE_GIT_COMMIT"):
        return os.environ["CAMSIM_SCENE_GIT_COMMIT"]
    try:
        r = subprocess.run(["git", "rev-parse", "HEAD"], cwd=Path(__file__).parent, capture_output=True, text=True, check=True)
        return r.stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def _credits(m: Manifest, layer: str) -> str:
    return "; ".join(m.source(s).attribution for s in m.layers[layer]["priorities"])


def build_scene(pkg: Path, cache: Cache, jobs: int | None = None, json_progress: bool = False,
                max_worker_rss_mb: float = 2048.0) -> dict:
    pkg = Path(pkg)
    m = Manifest.load(pkg / "manifest.json")
    if not m.is_fetched():
        m = fetch_scene(pkg, cache)
    jobs = jobs or os.cpu_count() or 1
    started = _utc_now()
    with BuildLock(pkg):
        ctx = make_context(pkg, m, cache)
        tplan = plan_tiles(m.region_objs(), "terrain", coverage(m, "terrain"))
        iplan = plan_tiles(m.region_objs(), "imagery", coverage(m, "imagery"))
        stats = {
            "terrain": _run_layer(ctx, "terrain", tplan, [_chunks(_terrain_items(tplan))], terrain_batch, jobs, json_progress)
        }
        lj = layer_json(m.name, available_ranges(tplan), _credits(m, "terrain"))
        atomic_write(pkg / "terrain" / "layer.json", canonical_json(lj).encode())
        stats["imagery"] = _run_layer(ctx, "imagery", iplan, _imagery_phases(iplan), imagery_batch, jobs, json_progress)
        atomic_write(pkg / "imagery" / "tilemapresource.xml", tilemapresource_xml(m.name, iplan.max_zoom, plan_bounds(iplan)).encode())
        stats["landcover"] = _build_landcover(pkg, m, ctx)
        atomic_write(pkg / "ATTRIBUTION.txt", attribution_text(m).encode())
        m.hashes_sha256 = write_hashes(pkg, Markers(pkg).output_for_file)
        m.write(pkg / "manifest.json")
        info = {
            "started_utc": started, "finished_utc": _utc_now(), "host": platform.node(), "platform": platform.platform(),
            "python": platform.python_version(), "tool_version": __version__, "git_commit": _git_commit(),
            "image_digest": os.environ.get("CAMSIM_SCENE_IMAGE_DIGEST"), "jobs": jobs, "cpu_count": os.cpu_count(),
            "layers": {k: v.to_dict() for k, v in stats.items()},
        }
        atomic_write(pkg / "build.json", (json.dumps(info, indent=2, sort_keys=True) + "\n").encode())
    peak = max(s.peak_rss_mb for s in stats.values())
    if peak > max_worker_rss_mb:
        raise BuildError(f"peak worker RSS {peak:.0f} MB exceeds {max_worker_rss_mb:.0f} MB (the package is complete; see build.json)")
    return info
```

- [ ] **Step 5: Run the tests**

Run: `cd scripts/scene && uv run pytest tests/test_pipeline.py -v`
Expected: 13 passed (about a minute: each test builds a ~60 + ~60 tile package).

- [ ] **Step 6: Run the whole suite and lint**

Run: `cd scripts/scene && uv run pytest -q && uvx ruff check . && uvx ruff format --check .`
Expected: all pass, no lint findings.

- [ ] **Step 7: Commit**

```bash
git add scripts/scene/camsim_scene/pipeline.py scripts/scene/tests/fake_sources.py scripts/scene/tests/test_pipeline.py
git commit -m "feat(scene): plan/fetch/build pipeline with resumable, reproducible tile builds"
```

---

### Task 19: `verify` (quick and deep)

**Files:**
- Create: `scripts/scene/camsim_scene/verify.py`
- Test: `scripts/scene/tests/test_verify.py`

**Interfaces:**
- Consumes: `manifest.{Manifest, package_files}`, `fsutil.{sha256_file, atomic_write}`, `tiling.{available_keys, tile_bounds, make_keys, LayerPlan}`, `tms.{parse_tilemapresource, plan_bounds}`, `licences.{check_allowed, attribution_text, LicenceError}`, `qmesh.{decode, QMAX}`, `layers.terrain.{tile_grid, point_heights, query_bounds}`, `config.TERRAIN_GRID`, `context.WorkerState`, `pipeline.make_context`.
- Produces: `P50_M = 0.05`, `P99_M = 0.5`, `SAMPLE = 0.02`; `Check(name, ok, detail)`; `verify(pkg, cache=None, deep=False, all_tiles=False, jobs=1, report_path=None) -> dict` (writes `<pkg>.verify.json` beside the package; keys `package, deep, ok, checks[], terrain_error_m?`); `present_tiles(pkg, layer, ext) -> dict[int, np.ndarray]`.

Checks (spec "Verify"): quick = `hashes` (every file listed, unchanged, none missing; `hashes_sha256` matches), `terrain_available` (`layer.json` `available` == tiles present), `tilemapresource` (levels 0..max and BoundingBox == imagery present), `licences`, `attribution`. Deep = `terrain_heights` (p50 ≤ 5 cm, p99 ≤ 0.5 m, outside feather bands), `terrain_finite`, `terrain_normals`, `terrain_edges` (same-zoom east and north neighbours agree at common edge vertices within one height quantum of either tile), `imagery_decode` (every JPEG decodes to 256 × 256 RGB).

- [ ] **Step 1: Write the failing tests**

`scripts/scene/tests/test_verify.py`:

```python
import json

import numpy as np
import pytest

from camsim_scene import qmesh, tiling
from camsim_scene.manifest import Manifest, write_hashes
from camsim_scene.verify import verify
from fake_sources import build_synthetic


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    return build_synthetic(tmp_path_factory.mktemp("verify"))


def checks(report):
    return {c["name"]: c for c in report["checks"]}


def copy_pkg(src, dst):
    import shutil

    shutil.copytree(src, dst)
    return dst


def test_clean_package_passes_quick_and_deep(built, tmp_path):
    pkg, cache, _ = built
    r = verify(pkg, cache, deep=True, all_tiles=True)
    assert r["ok"], [c for c in r["checks"] if not c["ok"]]
    assert r["terrain_error_m"]["p99"] <= 0.5 and r["terrain_error_m"]["n"] > 0
    assert json.loads((pkg.parent / f"{pkg.name}.verify.json").read_text())["ok"]


def test_tampered_tile_and_extra_file_fail_hashes(built, tmp_path):
    pkg = copy_pkg(built[0], tmp_path / "p")
    tile = next((pkg / "terrain/2").rglob("*.terrain"))
    tile.write_bytes(tile.read_bytes() + b"x")
    (pkg / "imagery/stray.txt").write_text("x")
    c = checks(verify(pkg))["hashes"]
    assert not c["ok"] and "modified terrain/2/" in c["detail"] and "unlisted imagery/stray.txt" in c["detail"]


def test_missing_attribution_line_fails(built, tmp_path):
    pkg = copy_pkg(built[0], tmp_path / "p")
    text = (pkg / "ATTRIBUTION.txt").read_text().splitlines()
    (pkg / "ATTRIBUTION.txt").write_text("\n".join(text[:-3]) + "\n")
    r = verify(pkg)
    assert not checks(r)["attribution"]["ok"] and not r["ok"]


def test_missing_tile_fails_availability(built, tmp_path):
    pkg = copy_pkg(built[0], tmp_path / "p")
    next((pkg / "terrain/1").rglob("*.terrain")).unlink()
    assert not checks(verify(pkg))["terrain_available"]["ok"]


def test_deep_catches_wrong_heights_that_hash_correctly(built, tmp_path):
    pkg = copy_pkg(built[0], tmp_path / "p")
    for p in (pkg / "terrain/6").rglob("*.terrain"):
        z, x, y = 6, int(p.parent.name), int(p.stem)
        q = qmesh.decode(p.read_bytes())
        lon, lat = q.lonlat(tiling.tile_bounds(z, x, y))
        p.write_bytes(qmesh.gzip_tile(qmesh.encode(lon, lat, q.heights() + 10.0, q.triangles, tiling.tile_bounds(z, x, y))))
    m = Manifest.load(pkg / "manifest.json")
    m.hashes_sha256 = write_hashes(pkg)
    m.write(pkg / "manifest.json")
    r = verify(pkg, built[1], deep=True, all_tiles=True)
    assert checks(r)["hashes"]["ok"] and not checks(r)["terrain_heights"]["ok"]
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd scripts/scene && uv run pytest tests/test_verify.py -v`
Expected: FAIL, `ModuleNotFoundError`.

- [ ] **Step 3: Implement `verify.py`**

```python
"""verify: quick checks always (hashes, availability, licences, attribution). --deep decodes a 2 % sample of the
terrain tiles per zoom (--all: every tile), compares vertex heights with the sources resampled independently
(highest-priority source, exact datum offsets, feather bands excluded), checks shared edges, NaNs and normals,
and decodes every JPEG. Writes <pkg>.verify.json beside the package; the package itself is never modified."""

from __future__ import annotations

import io
import json
import math
import random
from concurrent.futures import ProcessPoolExecutor
from dataclasses import asdict, dataclass
from pathlib import Path

import numpy as np
from PIL import Image

from .config import TERRAIN_GRID as GRID
from .fsutil import atomic_write, sha256_file
from .licences import LicenceError, attribution_text, check_allowed
from .manifest import Manifest, package_files
from .qmesh import QMAX, decode
from .tiling import LayerPlan, available_keys, make_keys, tile_bounds
from .tms import parse_tilemapresource, plan_bounds

P50_M, P99_M = 0.05, 0.5
SAMPLE = 0.02


@dataclass
class Check:
    name: str
    ok: bool
    detail: str = ""


def _summary(problems: list[str]) -> str:
    more = f" (+{len(problems) - 10} more)" if len(problems) > 10 else ""
    return "; ".join(problems[:10]) + more


def present_tiles(pkg: Path, layer: str, ext: str) -> dict[int, np.ndarray]:
    out: dict[int, list[int]] = {}
    root = Path(pkg) / layer
    for zdir in root.iterdir() if root.exists() else []:
        if not (zdir.is_dir() and zdir.name.isdigit()):
            continue
        keys = out.setdefault(int(zdir.name), [])
        for xdir in zdir.iterdir():
            for f in xdir.iterdir() if xdir.is_dir() else []:
                if f.name.endswith(f".{ext}") and f.name[: -len(ext) - 1].isdigit():
                    keys.append((int(xdir.name) << 32) | int(f.name[: -len(ext) - 1]))
    return {z: np.unique(np.asarray(k, np.int64)) for z, k in sorted(out.items())}


def _check_hashes(pkg: Path, m: Manifest) -> Check:
    path = pkg / "hashes.txt"
    if not path.exists():
        return Check("hashes", False, "no hashes.txt")
    problems = []
    with open(path, encoding="utf-8") as f:
        listed = (line.rstrip("\n").split("  ", 1)[::-1] for line in f)
        files = iter(package_files(pkg))
        a, b = next(files, None), next(listed, None)
        while a is not None or b is not None:
            if b is None or (a is not None and a < b[0]):
                problems.append(f"unlisted {a}")
                a = next(files, None)
            elif a is None or b[0] < a:
                problems.append(f"missing {b[0]}")
                b = next(listed, None)
            else:
                if sha256_file(pkg / a) != b[1]:
                    problems.append(f"modified {a}")
                a, b = next(files, None), next(listed, None)
    if m.hashes_sha256 != sha256_file(path):
        problems.append("hashes.txt does not match manifest.hashes_sha256")
    return Check("hashes", not problems, _summary(problems))


def _check_available(pkg: Path) -> Check:
    lj = json.loads((pkg / "terrain" / "layer.json").read_text())
    want, have = available_keys(lj["available"]), present_tiles(pkg, "terrain", "terrain")
    bad = [f"z{z}: layer.json {len(want.get(z, []))} tiles, files {len(have.get(z, []))}"
           for z in sorted(set(want) | set(have))
           if not np.array_equal(want.get(z, np.zeros(0, np.int64)), have.get(z, np.zeros(0, np.int64)))]
    return Check("terrain_available", not bad, _summary(bad))


def _check_tms(pkg: Path) -> Check:
    levels, bounds = parse_tilemapresource((pkg / "imagery" / "tilemapresource.xml").read_text())
    have = present_tiles(pkg, "imagery", "jpg")
    problems = []
    if levels != list(range(max(have) + 1)):
        problems.append(f"levels {levels[:3]}..{levels[-1:]} but tiles to z{max(have)}")
    hb = plan_bounds(LayerPlan(have, {}))
    if any(abs(a - b) > 1e-9 for a, b in zip(bounds, hb)):
        problems.append(f"BoundingBox {bounds} != tiles {hb}")
    return Check("tilemapresource", not problems, _summary(problems))


def _check_licences(m: Manifest) -> Check:
    try:
        check_allowed({s.id: s.licence for s in m.sources}, m.licence_allow)
        return Check("licences", True)
    except LicenceError as e:
        return Check("licences", False, str(e))


def _check_attribution(pkg: Path, m: Manifest) -> Check:
    p = pkg / "ATTRIBUTION.txt"
    ok = p.exists() and p.read_text(encoding="utf-8") == attribution_text(m)
    return Check("attribution", ok, "" if ok else "ATTRIBUTION.txt differs from the text generated from manifest.json")


def _edge_problems(qa, qb, a_edge, b_edge, along_a, along_b) -> int:
    ha = dict(zip(along_a[a_edge].tolist(), qa.heights()[a_edge]))
    hb = dict(zip(along_b[b_edge].tolist(), qb.heights()[b_edge]))
    step = max(qa.max_height - qa.min_height, qb.max_height - qb.min_height) / QMAX
    return sum(1 for k in set(ha) & set(hb) if abs(ha[k] - hb[k]) > step + 1e-6)


def _jpeg_bad(paths: list[str]) -> list[str]:
    bad = []
    for p in paths:
        try:
            im = Image.open(io.BytesIO(Path(p).read_bytes()))
            im.load()
            if im.size != (256, 256) or im.mode != "RGB":
                bad.append(f"{p}: {im.size} {im.mode}")
        except Exception as e:  # noqa: BLE001 - any decode failure is a finding
            bad.append(f"{p}: {e}")
    return bad


def _deep(pkg: Path, m: Manifest, cache, all_tiles: bool, jobs: int) -> tuple[list[Check], dict]:
    from .context import WorkerState
    from .layers import terrain
    from .pipeline import make_context

    st = WorkerState(make_context(pkg, m, cache))
    rng = random.Random(m.seed)
    present = present_tiles(pkg, "terrain", "terrain")
    errs, nonfinite, normals_bad, edges_bad = [], [], [], 0
    for z, keys in present.items():
        ks = keys.tolist()
        pick = ks if all_tiles else rng.sample(ks, max(1, math.ceil(SAMPLE * len(ks))))
        for k in sorted(pick):
            x, y = k >> 32, k & 0xFFFFFFFF
            q = decode((pkg / "terrain" / str(z) / str(x) / f"{y}.terrain").read_bytes())
            h = q.heights()
            lon, lat = q.lonlat(tile_bounds(z, x, y))
            if not np.isfinite(h).all():
                nonfinite.append(f"{z}/{x}/{y}")
                continue
            n = q.normals()
            if n is None or not (np.isfinite(n).all() and np.allclose(np.linalg.norm(n, axis=1), 1.0, atol=1e-3)):
                normals_bad.append(f"{z}/{x}/{y}")
            entries = st.index["terrain"].query(terrain.query_bounds(z, x, y))
            seam = terrain.tile_grid(z, x, y, entries).seam
            col = np.rint(q.u / QMAX * (GRID - 1)).astype(int)
            row = GRID - 1 - np.rint(q.v / QMAX * (GRID - 1)).astype(int)
            keep = ~seam[row, col]
            errs.append(np.abs(h - terrain.point_heights(z, lon, lat, entries))[keep])
            for nx, ny, east in ((x + 1, y, True), (x, y + 1, False)):
                if not np.isin(make_keys(nx, ny), keys):
                    continue
                qb = decode((pkg / "terrain" / str(z) / str(nx) / f"{ny}.terrain").read_bytes())
                if east:
                    edges_bad += _edge_problems(q, qb, q.east, qb.west, q.v, qb.v)
                else:
                    edges_bad += _edge_problems(q, qb, q.north, qb.south, q.u, qb.u)
    e = np.concatenate(errs) if errs else np.zeros(0)
    stats = {"n": int(e.size), "p50": float(np.percentile(e, 50)) if e.size else 0.0,
             "p99": float(np.percentile(e, 99)) if e.size else 0.0, "max": float(e.max()) if e.size else 0.0}
    jpgs = [str(p) for p in sorted((pkg / "imagery").rglob("*.jpg"))]
    chunks = [jpgs[i : i + 512] for i in range(0, len(jpgs), 512)]
    if jobs > 1 and len(chunks) > 1:
        with ProcessPoolExecutor(jobs) as ex:
            bad_jpg = [b for part in ex.map(_jpeg_bad, chunks) for b in part]
    else:
        bad_jpg = [b for c in chunks for b in _jpeg_bad(c)]
    checks = [
        Check("terrain_heights", stats["p50"] <= P50_M and stats["p99"] <= P99_M,
              f"p50 {stats['p50']:.3f} m, p99 {stats['p99']:.3f} m, max {stats['max']:.2f} m over {stats['n']} vertices"),
        Check("terrain_finite", not nonfinite, _summary(nonfinite)),
        Check("terrain_normals", not normals_bad, _summary(normals_bad)),
        Check("terrain_edges", edges_bad == 0, f"{edges_bad} shared-edge vertices disagree" if edges_bad else ""),
        Check("imagery_decode", not bad_jpg, _summary(bad_jpg)),
    ]
    return checks, stats


def verify(pkg: Path, cache=None, deep: bool = False, all_tiles: bool = False, jobs: int = 1, report_path: Path | None = None) -> dict:
    pkg = Path(pkg)
    m = Manifest.load(pkg / "manifest.json")
    checks = [_check_hashes(pkg, m), _check_available(pkg), _check_tms(pkg), _check_licences(m), _check_attribution(pkg, m)]
    report: dict = {"package": str(pkg), "deep": deep}
    if deep:
        if cache is None:
            raise ValueError("verify --deep needs the fetch cache (the sources)")
        deep_checks, report["terrain_error_m"] = _deep(pkg, m, cache, all_tiles, jobs)
        checks += deep_checks
    report["checks"] = [asdict(c) for c in checks]
    report["ok"] = all(c.ok for c in checks)
    atomic_write(report_path or pkg.parent / f"{pkg.name}.verify.json", (json.dumps(report, indent=2) + "\n").encode())
    return report
```

- [ ] **Step 4: Run the tests**

Run: `cd scripts/scene && uv run pytest tests/test_verify.py -v`
Expected: 5 passed.

- [ ] **Step 5: Commit**

```bash
git add scripts/scene/camsim_scene/verify.py scripts/scene/tests/test_verify.py
git commit -m "feat(scene): verify (hashes, availability, licences, attribution; deep height/edge/JPEG checks)"
```

---

### Task 20: `pack` (reproducible SquashFS)

**Files:**
- Create: `scripts/scene/camsim_scene/pack.py`
- Test: `scripts/scene/tests/test_pack.py`

**Interfaces:**
- Consumes: `manifest.{package_files, STATE_DIR}`, `fsutil.sha256_file`.
- Produces: `PackError(Exception)`; `FLAGS: list[str]`; `sort_file_text(pkg) -> str`; `pack(pkg, out=None, exe="mksquashfs") -> Path` (writes `<pkg>.sqfs` and `<pkg>.sqfs.sha256`).

- [ ] **Step 1: Write the failing tests**

`scripts/scene/tests/test_pack.py`:

```python
import shutil
import subprocess

import pytest

from camsim_scene import pack
from fake_sources import build_synthetic


def test_sort_file_puts_coarse_tiles_first(tmp_path):
    for rel in ["manifest.json", "terrain/0/0/0.terrain", "terrain/9/1/2.terrain", "imagery/12/0/0.jpg", ".state/x"]:
        (tmp_path / rel).parent.mkdir(parents=True, exist_ok=True)
        (tmp_path / rel).write_bytes(b"x")
    lines = dict(line.rsplit(" ", 1) for line in pack.sort_file_text(tmp_path).splitlines())
    assert int(lines["terrain/0/0/0.terrain"]) > int(lines["terrain/9/1/2.terrain"]) > int(lines["imagery/12/0/0.jpg"])
    assert ".state/x" not in lines and "manifest.json" in lines


def test_pack_refuses_an_unbuilt_package(tmp_path):
    with pytest.raises(pack.PackError, match="build"):
        pack.pack(tmp_path)


@pytest.mark.skipif(shutil.which("mksquashfs") is None, reason="mksquashfs not installed")
def test_pack_is_reproducible_and_excludes_state(tmp_path):
    pkg, _, _ = build_synthetic(tmp_path)
    a = pack.pack(pkg, tmp_path / "a.sqfs")
    b = pack.pack(pkg, tmp_path / "b.sqfs")
    assert a.read_bytes() == b.read_bytes()
    assert (tmp_path / "a.sqfs.sha256").read_text().endswith("  a.sqfs\n")
    listing = subprocess.run(["unsquashfs", "-l", str(a)], capture_output=True, text=True, check=True).stdout
    assert "terrain/layer.json" in listing and ".state" not in listing
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd scripts/scene && uv run pytest tests/test_pack.py -v`
Expected: FAIL, `ImportError`.

- [ ] **Step 3: Implement `pack.py`**

```python
"""pack: the package directory -> one SquashFS image, uncompressed (JPEG and gzip tiles don't compress further),
reproducible (owner root, all timestamps 0, coarse tiles first), plus <image>.sha256. Pinned by the build
container's squashfs-tools (>= 4.6, reproducible by default). Mounting: docs/realism/offline-hosting.md."""

from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path

from .fsutil import sha256_file
from .manifest import NOT_HASHED, STATE_DIR, package_files

FLAGS = ["-noappend", "-noI", "-noD", "-noF", "-noX", "-all-root", "-all-time", "0", "-mkfs-time", "0", "-no-progress", "-quiet"]
TILE_RE = re.compile(r"^(terrain|imagery)/(\d+)/")


class PackError(Exception):
    pass


def sort_file_text(pkg: Path) -> str:
    """mksquashfs -sort list: higher priority is stored first. Metadata first, then tiles by zoom (coarse first)."""
    lines = [f"{name} 1000" for name in NOT_HASHED if (Path(pkg) / name).exists()]
    for rel in package_files(pkg):
        m = TILE_RE.match(rel)
        lines.append(f"{rel} {500 - int(m.group(2)) if m else 1000}")
    return "\n".join(lines) + "\n"


def pack(pkg: Path, out: Path | None = None, exe: str = "mksquashfs") -> Path:
    pkg = Path(pkg)
    if not (pkg / "hashes.txt").exists():
        raise PackError(f"{pkg}: no hashes.txt; build the package first")
    exe_path = shutil.which(exe)
    if exe_path is None:
        raise PackError("mksquashfs not found (apt install squashfs-tools, or brew install squashfs)")
    out = Path(out) if out else pkg.parent / f"{pkg.name}.sqfs"
    sort = pkg.parent / f".{pkg.name}.sort"
    sort.write_text(sort_file_text(pkg), encoding="utf-8")
    try:
        subprocess.run([exe_path, str(pkg), str(out), *FLAGS, "-sort", str(sort), "-e", STATE_DIR], check=True)
    finally:
        sort.unlink(missing_ok=True)
    (out.parent / f"{out.name}.sha256").write_text(f"{sha256_file(out)}  {out.name}\n", encoding="utf-8")
    return out
```

- [ ] **Step 4: Run the tests**

Run: `cd scripts/scene && uv run pytest tests/test_pack.py -v`
Expected: 2 passed, 1 skipped on a machine without squashfs-tools (`brew install squashfs` to run it on macOS; it runs in the build container in Task 22). If mksquashfs warns about sort-file paths, check `man mksquashfs` for the container's version: entries are paths as they appear inside the image.

- [ ] **Step 5: Commit**

```bash
git add scripts/scene/camsim_scene/pack.py scripts/scene/tests/test_pack.py
git commit -m "feat(scene): reproducible SquashFS packing"
```

---

### Task 21: CLI and example scenes

**Files:**
- Create: `scripts/scene/camsim_scene/cli.py`, `scripts/scene/examples/pendleton.toml`, `scripts/scene/examples/pendleton-preview-10k.toml`
- Test: `scripts/scene/tests/test_cli.py`

**Interfaces:**
- Consumes: `config.{load_scene, default_scene_toml, ConfigError, ScenePlan}`, `pipeline.{plan_scene, fetch_scene, build_scene, PlanError, BuildError}`, `verify.verify`, `pack.{pack, PackError}`, `licences.{attribution_text, LicenceError}`, `cache.{Cache, CacheError, default_cache_root}`, `net.{Http, HttpError}`, `datum.DatumError`, `engine.BuildLocked`, `layers.terrain.TerrainError`.
- Produces: `main(argv=None) -> int` (exit 0 ok, 1 verify failed, 2 usage/build error); console script `camsim-scene`.

- [ ] **Step 1: Write the example scenes**

`scripts/scene/examples/pendleton.toml`:

```toml
# R0 exit gates 1-5: Camp Pendleton, sim profile (3DEP 1 m + 1/3", NAIP 2022, WorldCover S2 ring, global base)
name = "pendleton"
bbox = [-117.62, 33.19, -117.24, 33.52]  # W S E N
profile = "sim"
seed = 0
ring_km = 100
bmng_month = 7
jpeg_quality = 85

[sources.naip_pc]
year = "2022"
```

`scripts/scene/examples/pendleton-preview-10k.toml`:

```toml
# R0 exit gate 6: ~10,000 km2 (~100 x 100 km) around Pendleton, preview profile (no NAIP, no 1 m DEM)
name = "pendleton-preview-10k"
bbox = [-117.97, 32.90, -116.90, 33.80]
profile = "preview"
ring_km = 100
```

- [ ] **Step 2: Write the failing tests**

`scripts/scene/tests/test_cli.py`:

```python
import io

from camsim_scene import cli
from camsim_scene.config import parse_scene
from camsim_scene.pipeline import plan_scene
from fake_sources import synthetic_scene
from fakes import FakeHttp


def test_build_verify_attribution_round_trip(tmp_path, capsys):
    pkg = tmp_path / "pkg"
    plan_scene(parse_scene(synthetic_scene(tmp_path / "src")), pkg, http=FakeHttp({}), out=io.StringIO())
    cache = ["--cache", str(tmp_path / "cache")]
    assert cli.main([*cache, "build", str(pkg), "-j", "1"]) == 0
    assert cli.main([*cache, "verify", str(pkg)]) == 0
    assert cli.main([*cache, "attribution", str(pkg)]) == 0
    assert "synthetic test data" in capsys.readouterr().out


def test_build_without_manifest_or_config_is_a_usage_error(tmp_path, capsys):
    assert cli.main(["--cache", str(tmp_path / "c"), "build", str(tmp_path / "nothing")]) == 2
    assert "manifest.json" in capsys.readouterr().err


def test_bbox_across_the_antimeridian_is_rejected_before_any_network(tmp_path, capsys, monkeypatch):
    monkeypatch.chdir(tmp_path)
    assert cli.main(["plan", str(tmp_path / "p"), "--bbox", "177", "-19", "-179", "-16", "--name", "fiji"]) == 2
    assert "antimeridian" in capsys.readouterr().err


def test_verify_failure_exit_code(tmp_path):
    pkg = tmp_path / "pkg"
    plan_scene(parse_scene(synthetic_scene(tmp_path / "src")), pkg, http=FakeHttp({}), out=io.StringIO())
    cache = ["--cache", str(tmp_path / "cache")]
    cli.main([*cache, "build", str(pkg), "-j", "1"])
    (pkg / "ATTRIBUTION.txt").write_text("tampered\n")
    assert cli.main([*cache, "verify", str(pkg)]) == 1
```

- [ ] **Step 3: Run them to see them fail**

Run: `cd scripts/scene && uv run pytest tests/test_cli.py -v`
Expected: FAIL, `ImportError`.

- [ ] **Step 4: Implement `cli.py`**

```python
"""camsim-scene: build CamSim scene packages (docs/scene-packages.md).

    camsim-scene plan PKG --config scene.toml          # or --bbox W S E N [--name N] [--profile sim]
    camsim-scene fetch PKG                              # download + hash every asset into the cache
    camsim-scene build PKG [--config scene.toml] [-j N] # runs plan/fetch first when needed; resumable
    camsim-scene verify PKG [--deep [--all]]            # writes PKG.verify.json; exit 1 when a check fails
    camsim-scene pack PKG [--out FILE]                  # PKG.sqfs + .sha256
    camsim-scene attribution PKG                        # print the attribution text

Cache: --cache DIR, else $CAMSIM_SCENE_CACHE, else <repo>/.cache/scene.
"""

from __future__ import annotations

import argparse
import dataclasses
import logging
import os
import sys
from pathlib import Path

from .cache import Cache, CacheError, default_cache_root
from .config import ConfigError, ScenePlan, default_scene_toml, load_scene, validate_bbox
from .datum import DatumError
from .engine import BuildLocked
from .layers.terrain import TerrainError
from .licences import LicenceError, attribution_text
from .manifest import Manifest
from .net import HttpError
from .pack import PackError, pack
from .pipeline import BuildError, PlanError, build_scene, fetch_scene, plan_scene
from .verify import verify

ERRORS = (ConfigError, LicenceError, CacheError, HttpError, DatumError, BuildLocked, BuildError, PlanError, PackError,
          TerrainError, FileNotFoundError)


def _scene_args(p: argparse.ArgumentParser) -> None:
    p.add_argument("--config", type=Path, help="scene.toml")
    p.add_argument("--bbox", type=float, nargs=4, metavar=("W", "S", "E", "N"), help="write a default scene file for this bbox")
    p.add_argument("--name", help="package name for --bbox (default: the package directory name)")
    p.add_argument("--profile", default="sim", choices=["preview", "sim"])
    p.add_argument("--allow", action="append", default=[], metavar="LICENCE", help="also accept this licence id")


def _scene(a) -> ScenePlan | None:
    if a.bbox:
        validate_bbox(a.bbox)
        name = a.name or a.pkg.name
        path = a.config or Path(f"{name}.scene.toml")
        if not path.exists():
            path.write_text(default_scene_toml(name, tuple(a.bbox), a.profile), encoding="utf-8")
            print(f"wrote {path}", file=sys.stderr)
        a.config = path
    if not a.config:
        return None
    plan = load_scene(a.config)
    return dataclasses.replace(plan, allow=tuple(sorted(set(plan.allow) | set(a.allow)))) if a.allow else plan


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="camsim-scene", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cache", type=Path, help="fetch cache directory")
    ap.add_argument("-v", "--verbose", action="store_true")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("plan", help="discover assets and write manifest.json")
    p.add_argument("pkg", type=Path)
    _scene_args(p)
    f = sub.add_parser("fetch", help="download and hash every asset")
    f.add_argument("pkg", type=Path)
    b = sub.add_parser("build", help="build (or resume) the package")
    b.add_argument("pkg", type=Path)
    _scene_args(b)
    b.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 1)
    b.add_argument("--json-progress", action="store_true", help="progress as JSON lines on stderr")
    b.add_argument("--max-worker-rss-mb", type=float, default=2048.0)
    v = sub.add_parser("verify", help="check a built package")
    v.add_argument("pkg", type=Path)
    v.add_argument("--deep", action="store_true")
    v.add_argument("--all", dest="all_tiles", action="store_true", help="with --deep: every terrain tile, not a 2 %% sample")
    v.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 1)
    k = sub.add_parser("pack", help="pack into one SquashFS image")
    k.add_argument("pkg", type=Path)
    k.add_argument("--out", type=Path)
    t = sub.add_parser("attribution", help="print the attribution text")
    t.add_argument("pkg", type=Path)
    a = ap.parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO, format="%(levelname)s %(message)s", stream=sys.stderr)
    cache = Cache(a.cache or default_cache_root())
    try:
        if a.cmd in ("plan", "build"):
            plan = _scene(a)
            if a.cmd == "plan":
                if plan is None:
                    raise ConfigError("plan needs --config or --bbox")
                plan_scene(plan, a.pkg, cache.http)
                return 0
            if plan is not None:
                plan_scene(plan, a.pkg, cache.http)
            elif not (a.pkg / "manifest.json").exists():
                raise ConfigError(f"{a.pkg}: no manifest.json; pass --config or --bbox")
            info = build_scene(a.pkg, cache, a.jobs, a.json_progress, a.max_worker_rss_mb)
            for layer, s in info["layers"].items():
                print(f"{layer}: {s['tiles']} tiles ({s['built']} built, {s['skipped']} skipped), "
                      f"{s['bytes'] / 1e6:.1f} MB, {s['seconds']:.0f} s, peak RSS {s['peak_rss_mb']:.0f} MB")
            return 0
        if a.cmd == "fetch":
            fetch_scene(a.pkg, cache)
            return 0
        if a.cmd == "verify":
            r = verify(a.pkg, cache, a.deep, a.all_tiles, a.jobs)
            for c in r["checks"]:
                print(f"{'ok  ' if c['ok'] else 'FAIL'} {c['name']}" + (f": {c['detail']}" if c["detail"] else ""))
            return 0 if r["ok"] else 1
        if a.cmd == "pack":
            out = pack(a.pkg, a.out)
            print(out)
            return 0
        if a.cmd == "attribution":
            print(attribution_text(Manifest.load(a.pkg / "manifest.json")))
            return 0
    except ERRORS as e:
        print(f"camsim-scene: error: {e}", file=sys.stderr)
        return 2
    return 2


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 5: Run the tests**

Run: `cd scripts/scene && uv run pytest tests/test_cli.py -v && uv run camsim-scene --help`
Expected: 4 passed; the help text lists the six commands.

- [ ] **Step 6: Commit**

```bash
git add scripts/scene/camsim_scene/cli.py scripts/scene/examples scripts/scene/tests/test_cli.py
git commit -m "feat(scene): camsim-scene CLI and Pendleton example scenes"
```

---
### Task 22: Reference build container

**Files:**
- Create: `scripts/scene/Dockerfile`, `scripts/scene/.dockerignore`, `scripts/scene/build.sh`

**Interfaces:**
- Consumes: the `camsim-scene` console script; env `CAMSIM_SCENE_IMAGE_DIGEST`, `CAMSIM_SCENE_GIT_COMMIT` (read by `pipeline.build_scene` into `build.json`), `CAMSIM_SCENE_CACHE`.
- Produces: image `camsim-scene:r0`; `scripts/scene/build.sh [--build-image] <camsim-scene args>` with the repo at `/work` (read-only, working directory), the cache at `/cache`, outputs at `/out`.

- [ ] **Step 1: Pin the base image**

Run: `docker pull ubuntu:24.04 && docker image inspect --format '{{index .RepoDigests 0}}' ubuntu:24.04`
Expected: `ubuntu@sha256:<64 hex>`. Use that digest in the `FROM` line below.

- [ ] **Step 2: Write the Dockerfile**

`scripts/scene/Dockerfile`:

```dockerfile
# camsim-scene reference build container (REALISM R0). Byte-identical packages need the same image digest:
# build once, keep the image, and reference it by digest (build.json records it). apt packages aren't
# snapshot-pinned, so a rebuilt image may differ.
FROM ubuntu:24.04@sha256:<digest from step 1>

ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8 TZ=UTC PYTHONDONTWRITEBYTECODE=1 PROJ_NETWORK=OFF \
    UV_PYTHON_DOWNLOADS=never UV_LINK_MODE=copy CAMSIM_SCENE_CACHE=/cache
RUN apt-get update \
 && apt-get install -y --no-install-recommends python3.12 python3.12-venv gdal-bin proj-bin squashfs-tools ca-certificates \
 && rm -rf /var/lib/apt/lists/*
COPY --from=ghcr.io/astral-sh/uv:0.12.23 /uv /usr/local/bin/uv

WORKDIR /opt/camsim-scene
COPY pyproject.toml uv.lock .python-version README.md ./
RUN uv sync --frozen --no-dev --no-install-project --python /usr/bin/python3.12
COPY camsim_scene ./camsim_scene
RUN uv sync --frozen --no-dev --python /usr/bin/python3.12
ENV PATH=/opt/camsim-scene/.venv/bin:$PATH

WORKDIR /work
ENTRYPOINT ["camsim-scene"]
```

`scripts/scene/.dockerignore`:

```
.venv
out
tests
tools
spike
**/__pycache__
```

- [ ] **Step 3: Write `build.sh`**

```bash
#!/usr/bin/env bash
# Run camsim-scene in the reference build container (docs/scene-packages.md).
#   scripts/scene/build.sh [--build-image] <camsim-scene args...>
# Inside: the repo is /work (read-only, cwd), the fetch cache /cache, outputs /out. Defaults:
#   CAMSIM_SCENE_IMAGE=camsim-scene:r0  CAMSIM_SCENE_CACHE=<repo>/.cache/scene  CAMSIM_SCENE_OUT=<repo>/.cache/scene-packages
# Example: scripts/scene/build.sh build /out/pendleton --config scripts/scene/examples/pendleton.toml -j 16
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
IMAGE=${CAMSIM_SCENE_IMAGE:-camsim-scene:r0}
CACHE=${CAMSIM_SCENE_CACHE:-$REPO/.cache/scene}
OUT=${CAMSIM_SCENE_OUT:-$REPO/.cache/scene-packages}
if [[ "${1:-}" == "--build-image" ]]; then
  shift
  docker build -t "$IMAGE" "$HERE"
fi
mkdir -p "$CACHE" "$OUT"
DIGEST=$(docker image inspect --format '{{.Id}}' "$IMAGE")
TTY=()
[[ -t 1 ]] && TTY=(-t)
exec docker run --rm "${TTY[@]}" -u "$(id -u):$(id -g)" \
  -v "$REPO:/work:ro" -v "$CACHE:/cache" -v "$OUT:/out" -w /work \
  -e CAMSIM_SCENE_IMAGE_DIGEST="$DIGEST" -e CAMSIM_SCENE_GIT_COMMIT="$(git -C "$REPO" rev-parse HEAD)" \
  "$IMAGE" "$@"
```

Run: `chmod +x scripts/scene/build.sh`

- [ ] **Step 4: Build the image and run the suite inside it**

Run: `scripts/scene/build.sh --build-image --help`
Expected: the `camsim-scene` help text.
Run: `docker run --rm --entrypoint bash -v "$PWD/scripts/scene:/src:ro" -e UV_PROJECT_ENVIRONMENT=/tmp/venv camsim-scene:r0 -c 'cp -r /src /tmp/s && cd /tmp/s && uv sync --frozen --python /usr/bin/python3.12 -q && uv run --frozen pytest -q'`
Expected: all tests pass, including `test_pack_is_reproducible_and_excludes_state` (squashfs-tools is in the image). Record `mksquashfs -version` (`docker run --rm --entrypoint mksquashfs camsim-scene:r0 -version`) for the docs.

- [ ] **Step 5: Commit**

```bash
git add scripts/scene/Dockerfile scripts/scene/.dockerignore scripts/scene/build.sh
git commit -m "feat(scene): pinned Ubuntu 24.04 build container and build.sh"
```

---

### Task 23: Gate tooling (moved from the spike) and documentation

**Files:**
- Create (moved and adapted): `scripts/scene/tools/render_check.py` (from `spike/spike_run.py`), `scripts/scene/tools/hot_check.py` (from `spike/hot_check.py`), `scripts/scene/tools/tms_overlay.patch` (from `spike/tms_overlay_spike.patch`, unchanged), `scripts/scene/tools/offline.sb` (unchanged), `scripts/scene/tools/README.md`
- Create: `scripts/scene/tools/registration.py`, `docs/scene-packages.md`
- Modify: `CLAUDE.md` (commands table, architecture tree, testing), `docs/realism/offline-hosting.md` (pointer to `camsim-scene pack`)

**Interfaces:**
- Consumes: `scripts/run.sh`, `scripts/stop.sh`, `scripts/bench/run_bench.py` (`Host`, `wait_ready`, `wait_terrain`, `fetch_snapshot`, `SHOT_SETTLE_S`, `http_text`), `scripts/bench/scenario.py` (`Pose`), `scripts/check_cigi_responses.py` (`parse_responses`).
- Produces: `render_check.py OUT {package|cwt} [PKG] [--offline]`; `hot_check.py {package|cwt} PKG --third SRC --onem SRC` (prints a PASS/FAIL table against 0.25 m on 1 m data and 1 m on 1/3″ data); `registration.py A.png B.png` (phase-correlation shift in pixels).

- [ ] **Step 1: Move the files**

```bash
mkdir -p scripts/scene/tools
git mv scripts/scene/spike/spike_run.py scripts/scene/tools/render_check.py
git mv scripts/scene/spike/hot_check.py scripts/scene/tools/hot_check.py
git mv scripts/scene/spike/tms_overlay_spike.patch scripts/scene/tools/tms_overlay.patch
git mv scripts/scene/spike/offline.sb scripts/scene/tools/offline.sb
```

- [ ] **Step 2: Adapt `render_check.py`**

Replace its contents with:

```python
"""R0 gate 5: launch CamSim on a scene package (file://; apply tools/tms_overlay.patch locally first) or on
Cesium ion ("cwt"), fly fixed shots over Camp Pendleton and save /snapshot PNGs + result.json.

    uv run --project scripts/scene python scripts/scene/tools/render_check.py OUT package PKG [--offline]
    uv run --project scripts/scene python scripts/scene/tools/render_check.py OUT cwt

--offline (macOS) runs CamSim under sandbox-exec with ports 80/443 blocked (tools/offline.sb).
"""

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "scripts"))
sys.path.insert(0, str(REPO / "scripts" / "bench"))
import run_bench as rb  # noqa: E402
from bench import scenario  # noqa: E402

P = scenario.Pose
SHOTS = {
    "nadir_2km": P(lat=33.225, lon=-117.380, alt=2000, gimbal_pitch=-90, fov_h=30),
    "slant_ne": P(lat=33.205, lon=-117.405, alt=800, yaw=45, gimbal_pitch=-20, fov_h=40),
    "rivermouth": P(lat=33.222, lon=-117.400, alt=250, yaw=90, gimbal_pitch=-25, fov_h=50),
    "interior_nadir": P(lat=33.380, lon=-117.420, alt=2000, gimbal_pitch=-90, fov_h=30),  # 1/3" terrain only
    "high_oblique": P(lat=33.300, lon=-117.450, alt=6000, yaw=30, gimbal_pitch=-10, fov_h=60),
    "ring_edge": P(lat=33.350, lon=-117.430, alt=9000, yaw=270, gimbal_pitch=-12, fov_h=60),  # over the ring to the global base
    "slant_ne_ir": P(lat=33.205, lon=-117.405, alt=800, yaw=45, gimbal_pitch=-20, fov_h=40, sensor_id=1),
}


def env_for(mode: str, pkg: Path | None) -> dict:
    env = {"CAMSIM_SNAPSHOT_ENDPOINT_ENABLED": "1"}
    if mode == "package":
        env.update(
            CAMSIM_CESIUM_TERRAIN_SOURCE="url", CAMSIM_CESIUM_TERRAIN_URL=f"file://{pkg}/terrain/layer.json",
            CAMSIM_CESIUM_IMAGERY_SOURCE="tms", CAMSIM_CESIUM_IMAGERY_WMS_URL=f"file://{pkg}/imagery/tilemapresource.xml",
            CAMSIM_CESIUM_ION_TOKEN="", CAMSIM_THERMAL_LAND_COVER_DIR=str(pkg / "landcover"),
        )
    return env


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", type=Path)
    ap.add_argument("mode", choices=["package", "cwt"])
    ap.add_argument("pkg", type=Path, nargs="?")
    ap.add_argument("--offline", action="store_true")
    a = ap.parse_args()
    if a.mode == "package" and not a.pkg:
        ap.error("package mode needs PKG")
    a.out.mkdir(parents=True, exist_ok=True)
    cmd = [str(REPO / "scripts" / "run.sh"), "--headless", "--local", "--detach"]
    if a.offline:
        cmd = ["sandbox-exec", "-f", str(Path(__file__).with_name("offline.sb")), *cmd]
    host = rb.Host()
    host.pose = SHOTS["nadir_2km"]
    host.thread.start()
    subprocess.run(cmd, env=dict(os.environ, **env_for(a.mode, a.pkg and a.pkg.resolve())), check=True, stdout=subprocess.DEVNULL)
    t0, res = time.time(), {"mode": a.mode, "offline": a.offline}
    try:
        rb.wait_ready(REPO / ".cache" / "camsim.pid")
        res["ready_s"] = round(time.time() - t0)
        for name, pose in SHOTS.items():
            host.pose = pose
            time.sleep(1.0)
            ok = rb.wait_terrain(90)
            time.sleep(rb.SHOT_SETTLE_S + 2)
            rb.fetch_snapshot(a.out / f"{name}.png")
            res[name] = {"terrain_ready": ok}
            print(name, ok, flush=True)
        res["metrics"] = [m for m in (rb.http_text("/metrics") or "").splitlines() if "tile" in m.lower() or "terrain" in m.lower()][:20]
    finally:
        subprocess.run([str(REPO / "scripts" / "stop.sh")], check=False, stdout=subprocess.DEVNULL)
        host.stop.set()
    (a.out / "result.json").write_text(json.dumps(res, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 3: Adapt `hot_check.py`**

Replace its contents with:

```python
"""R0 gate 5: CIGI frame-centre heights (opcode 107) at nadir points vs 3DEP truth (NAVD88 -> ITRF2014 ellipsoid via
GEOID18). Pass: |error| <= 0.25 m where the 1 m DEM exists, <= 1.0 m on 1/3 arc-second data.

    uv run --project scripts/scene python scripts/scene/tools/hot_check.py package PKG --third SRC --onem SRC
    uv run --project scripts/scene python scripts/scene/tools/hot_check.py cwt --third SRC --onem SRC

SRC is a URL or a local path (e.g. the cached blob of the asset named in PKG/manifest.json).
"""

import argparse
import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

import pyproj
import rasterio

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "scripts"))
sys.path.insert(0, str(REPO / "scripts" / "bench"))
sys.path.insert(0, str(Path(__file__).parent))
import check_cigi_responses as ccr  # noqa: E402
import run_bench as rb  # noqa: E402
from bench import scenario  # noqa: E402
from render_check import env_for  # noqa: E402

POINTS = [  # five near Oceanside (1 m D24), two in the base interior (1/3" only)
    (33.2100, -117.3700), (33.2300, -117.3600), (33.2400, -117.3900), (33.2150, -117.3950), (33.2450, -117.3700),
    (33.3800, -117.4200), (33.4200, -117.3500),
]
LIMIT_M = {"1m": 0.25, '1/3"': 1.0}


def open_src(src: str):
    return rasterio.open("/vsicurl/" + src) if src.startswith("http") else rasterio.open(src)


def truth(lat: float, lon: float, onem: str, third: str) -> tuple[float, str]:
    pyproj.network.set_network_enabled(True)  # a tool, not a build: PROJ may fetch GEOID18
    to_itrf = pyproj.Transformer.from_crs("EPSG:6318+5703", "EPSG:7912", always_xy=True)
    lo, la = pyproj.Transformer.from_crs("EPSG:7912", "EPSG:6318", always_xy=True).transform(lon, lat)
    for src, label in ((onem, "1m"), (third, '1/3"')):
        with rasterio.Env(GDAL_DISABLE_READDIR_ON_OPEN="EMPTY_DIR"), open_src(src) as ds:
            x, y = pyproj.Transformer.from_crs(ds.crs.geodetic_crs, ds.crs, always_xy=True).transform(lo, la)
            inside = ds.bounds.left <= x <= ds.bounds.right and ds.bounds.bottom <= y <= ds.bounds.top
            v = float(next(ds.sample([(x, y)]))[0]) if inside else None
            if v is not None and v != ds.nodata and v > -1000:
                return float(to_itrf.transform(lo, la, v, 2010.0)[2]), label
    raise SystemExit(f"no 3DEP truth at {lat}, {lon}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["package", "cwt"])
    ap.add_argument("pkg", type=Path, nargs="?")
    ap.add_argument("--third", required=True)
    ap.add_argument("--onem", required=True)
    a = ap.parse_args()
    env = env_for(a.mode, a.pkg and a.pkg.resolve())
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", 8889))
    sock.settimeout(0.5)
    host = rb.Host()
    host.pose = scenario.Pose(lat=POINTS[0][0], lon=POINTS[0][1], alt=1500, gimbal_pitch=-90, fov_h=20)
    host.thread.start()
    subprocess.run([str(REPO / "scripts" / "run.sh"), "--headless", "--local", "--detach"], env=dict(os.environ, **env),
                   check=True, stdout=subprocess.DEVNULL)
    out = []
    try:
        rb.wait_ready(REPO / ".cache" / "camsim.pid")
        for lat, lon in POINTS:
            host.pose = scenario.Pose(lat=lat, lon=lon, alt=1500, gimbal_pitch=-90, fov_h=20)
            time.sleep(1)
            rb.wait_terrain(90)
            time.sleep(3)
            got, t_end = [], time.time() + 3
            while time.time() < t_end:
                try:
                    d = sock.recv(65536)
                except socket.timeout:
                    continue
                got += [s for s in ccr.parse_responses(d)["sensor_ext"] if s["status"] != 0]
            if not got:
                out.append({"pt": [lat, lon], "error": "no frame-centre response", "pass": False})
                continue
            fc = got[-1]
            t, src = truth(fc["lat"], fc["lon"], a.onem, a.third)
            err = fc["alt"] - t
            rec = {"pt": [lat, lon], "fc": [fc["lat"], fc["lon"], round(fc["alt"], 2)], "truth": round(t, 2), "src": src,
                   "err_m": round(err, 2), "pass": abs(err) <= LIMIT_M[src]}
            print(f"{lat:.4f} {lon:.4f} {src:5s} err {err:+.2f} m {'PASS' if rec['pass'] else 'FAIL'}", flush=True)
            out.append(rec)
    finally:
        subprocess.run([str(REPO / "scripts" / "stop.sh")], check=False, stdout=subprocess.DEVNULL)
        host.stop.set()
    ok = all(r["pass"] for r in out)
    Path(f"hot_{a.mode}.json").write_text(json.dumps({"points": out, "pass": ok}, indent=1))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Write `registration.py`**

```python
"""Sub-pixel registration between two shots of the same pose (package vs Cesium World Terrain + Bing): phase
correlation on the luma of the central 512 x 512 window. Gate 5 passes below 1 px.

    uv run --project scripts/scene python scripts/scene/tools/registration.py out/package/nadir_2km.png out/cwt/nadir_2km.png
"""

import sys

import numpy as np
from PIL import Image


def luma_window(path: str, n: int = 512) -> np.ndarray:
    a = np.asarray(Image.open(path).convert("L"), np.float64)
    r0, c0 = (a.shape[0] - n) // 2, (a.shape[1] - n) // 2
    w = a[r0 : r0 + n, c0 : c0 + n]
    return (w - w.mean()) * np.outer(np.hanning(n), np.hanning(n))


def shift(a: np.ndarray, b: np.ndarray) -> tuple[float, float]:
    r = np.fft.fft2(a) * np.conj(np.fft.fft2(b))
    c = np.fft.ifft2(r / np.maximum(np.abs(r), 1e-12)).real
    i, j = np.unravel_index(np.argmax(c), c.shape)
    n = c.shape[0]

    def sub(m1, m0, p1):  # parabolic peak refinement
        d = m1 - 2 * m0 + p1
        return 0.0 if d == 0 else 0.5 * (m1 - p1) / d

    di = i + sub(c[i - 1, j], c[i, j], c[(i + 1) % n, j])
    dj = j + sub(c[i, j - 1], c[i, j], c[i, (j + 1) % n])
    return (di + n / 2) % n - n / 2, (dj + n / 2) % n - n / 2


if __name__ == "__main__":
    dy, dx = shift(luma_window(sys.argv[1]), luma_window(sys.argv[2]))
    mag = float(np.hypot(dx, dy))
    print(f"shift dx {dx:+.2f} px, dy {dy:+.2f} px, |d| {mag:.2f} px -> {'PASS' if mag < 1.0 else 'FAIL'}")
    sys.exit(0 if mag < 1.0 else 1)
```

Sanity check: `python - <<'EOF'` with a random image and a copy rolled by (3, −2) pixels → `shift` returns ≈ (3, −2); run it once and confirm.

- [ ] **Step 5: Write `scripts/scene/tools/README.md`**

```markdown
# Gate tools (REALISM R0 gate 5)

| File | What it does |
|---|---|
| `render_check.py` | Launches CamSim on a package (`package PKG`) or on ion (`cwt`), flies seven shots, saves `/snapshot` PNGs |
| `hot_check.py` | CIGI frame-centre heights vs 3DEP truth; PASS/FAIL at 0.25 m (1 m data) / 1.0 m (1/3″) |
| `registration.py` | Phase-correlation shift between a package shot and the CWT shot of the same pose (< 1 px passes) |
| `tms_overlay.patch` | The throwaway `imagery.source: tms` branch (R1 lands the real one). Apply locally, never commit |
| `offline.sb` | macOS `sandbox-exec` profile blocking ports 80/443 (`render_check.py --offline`) |
```

- [ ] **Step 6: Write `docs/scene-packages.md`**

The user guide. Sections and required content:

1. **What a package is** — the layout table from the spec ("Package format"); the one tile grid, named as OGC TMS 2.0 `WorldCRS84Quad` with the TMS row flip (`row_ogc = 2^z - 1 - y`, Decision 17); regions and zoom limits; complete siblings; and that it covers the whole globe. List the standards a package uses: WorldCRS84Quad, quantized-mesh-1.0 (Cesium spec), TMS 1.0 `tilemapresource.xml`, COG for derived rasters, SPDX licence ids.
2. **Quick start** — container (reference) and native:
   ```bash
   scripts/scene/build.sh --build-image build /out/pendleton --config scripts/scene/examples/pendleton.toml -j 16
   scripts/scene/build.sh verify /out/pendleton --deep
   scripts/scene/build.sh pack /out/pendleton
   # native (macOS/Linux; passes verify, bytes may differ from the container):
   uv run --project scripts/scene camsim-scene build .cache/scene-packages/pendleton --config scripts/scene/examples/pendleton.toml
   ```
3. **scene.toml reference** — every key from `config.py`'s docstring, the two profiles and their source lists (Decision 3), `[sources.<id>]` options (`naip_pc.year`, `dep3_13.as_of`, `dep3_1m.geoid_overrides`, `bmng.month` via `bmng_month`), `[zoom.<region>]`, `allow`.
4. **Sources** — table: id, dataset, licence, area, zoom limit, datum (from Tasks 10–13), expected cache volumes (spec "Sources and cache", plus ~0.5 GB per 1° WC S2 cell, ~9 cells for Pendleton + 100 km).
5. **Datums** — the pinned chain, epoch 2010.0, the known point, GEOID12A unsupported (Decision 2), WGS 84 ≈ ITRF2014.
6. **Resume, rebuild, reproducibility** — markers, what dirties what (settings hash per layer, asset hashes per tile, children for imagery parents), stale removal, the lock, `build.json` vs `manifest.json`, the container-digest contract.
7. **verify** — the checks and thresholds (Task 19), where the report goes.
8. **Packing and mounting** — `pack` flags, `mount -o loop,ro pendleton.sqfs /mnt/pendleton` or `squashfuse`, bind-mount `:ro`; Docker Desktop on macOS can't loop-mount (use the directory).
9. **Using a package in CamSim (until R1)** — `terrain.source: url` with `file://…/terrain/layer.json` works today; imagery needs `tools/tms_overlay.patch`; land cover via `thermal.land_cover.dir` → `PKG/landcover`.
10. **Measured** — a table filled in by Tasks 24–25 (gate numbers).

- [ ] **Step 7: Update `CLAUDE.md` and `offline-hosting.md`**

In the CLAUDE.md commands table add:

```markdown
| `uv run --project scripts/scene camsim-scene plan\|fetch\|build\|verify\|pack\|attribution PKG` | Scene packages (REALISM R0): bbox → whole-globe terrain + imagery + land cover (`docs/scene-packages.md`) |
| `scripts/scene/build.sh [--build-image] <args>` | Same, in the pinned build container (reference for byte-identical builds) |
| `uv run --project scripts/scene --with pytest pytest scripts/scene/tests` | Scene tooling tests (no network; `--network` adds live API checks) |
```

Change the `scripts/landcover/fetch_worldcover.py` row's description to add "(wraps `camsim_scene`)". In the Architecture tree add `scripts/scene/  # camsim-scene: scene package build tooling (REALISM R0)` next to `hitl/`. In `docs/realism/offline-hosting.md`, under the SquashFS paragraph (line ~163), add: "`camsim-scene pack PKG` writes `PKG.sqfs` with exactly these settings (`docs/scene-packages.md`)."

- [ ] **Step 8: Lint and commit**

Run: `uvx ruff check scripts/scene && uvx ruff format --check scripts/scene`
Expected: clean.

```bash
git add scripts/scene/tools docs/scene-packages.md CLAUDE.md docs/realism/offline-hosting.md
git commit -m "docs(scene): scene package guide; gate-5 tools moved out of the spike"
```

---

### Task 24: Exit gates 1, 2, 3, 4, 6 (Linux, reference container)

Run on the Linux box (RTX 5080 host, Docker). No GPU is needed. Needs ~25 GB of cache and ~5 GB of output free; check `plan`'s estimate first. Record every number; nothing in this task is a pass unless the command output says so.

**Files:**
- Modify: `docs/scene-packages.md` ("Measured"), `REALISM.md` (R0 status), `ROADMAP.md` (realism section)

- [ ] **Step 1: Plan and fetch Pendleton**

```bash
scripts/scene/build.sh --build-image plan /out/pendleton --config scripts/scene/examples/pendleton.toml
scripts/scene/build.sh fetch /out/pendleton
```

Expected: the plan line (assets, cache GB, tile counts, package GB); fetch ends without error and `/out/pendleton/manifest.json` has a sha256 on every asset and grid. Warnings about skipped 1 m projects are expected for GEOID12A projects; write them down.

- [ ] **Step 2: Gate 1 — full build, measured**

```bash
/usr/bin/time -v scripts/scene/build.sh build /out/pendleton -j "$(nproc)" 2>&1 | tee .cache/gate1.log
```

Expected: exit 0; per-layer summary lines. Record from `.cache/scene-packages/pendleton/build.json`: per layer `tiles`, `bytes`, `seconds`, `peak_rss_mb`; total wall time and `Maximum resident set size` from `time -v` (that's the docker client, so take memory from build.json); `du -sh` and `find … -type f | wc -l` per layer directory. Gate: per-worker `peak_rss_mb` ≤ 2048 (the build fails otherwise).

- [ ] **Step 3: Gate 4 — deep verify**

```bash
scripts/scene/build.sh verify /out/pendleton --deep
```

Expected: every line `ok`, exit 0. Record p50/p99/max from `pendleton.verify.json`.

- [ ] **Step 4: Gates 3 and 2 — a second clean build from the manifest, killed at ~50 % and resumed**

```bash
mkdir -p .cache/scene-packages/pendleton-b && cp .cache/scene-packages/pendleton/manifest.json .cache/scene-packages/pendleton-b/
scripts/scene/build.sh build /out/pendleton-b -j "$(nproc)" --json-progress 2> .cache/gate2-progress.jsonl &
# watch the imagery layer; kill the container at about half of it
until grep -q '"layer": "imagery"' .cache/gate2-progress.jsonl && \
      python3 -c 'import json,sys; r=[json.loads(l) for l in open(".cache/gate2-progress.jsonl") if "\"imagery\"" in l][-1]; sys.exit(r["done"] < r["total"] // 2)'; do sleep 5; done
docker ps -q --filter ancestor=camsim-scene:r0 | xargs -r docker kill
wait || true
scripts/scene/build.sh build /out/pendleton-b -j "$(nproc)"
```

Gate 2: in the resumed run's `build.json`, `terrain.skipped == terrain.tiles` and `imagery.skipped` ≈ the `done` count at the kill (± one in-flight chunk per worker); no `*.part` files remain (`find .cache/scene-packages/pendleton-b -name '*.part' | wc -l` → 0).
Gate 3:

```bash
cmp .cache/scene-packages/pendleton/hashes.txt .cache/scene-packages/pendleton-b/hashes.txt && echo HASHES-IDENTICAL
scripts/scene/build.sh pack /out/pendleton && scripts/scene/build.sh pack /out/pendleton-b
cmp .cache/scene-packages/pendleton.sqfs .cache/scene-packages/pendleton-b.sqfs && echo SQFS-IDENTICAL
```

Expected: both lines printed. If `hashes.txt` differs, `diff` it and look at the first differing file type: JPEG → Pillow/libjpeg not pinned; `.terrain` → nondeterminism in sampling or pydelatin; fix before going on.

- [ ] **Step 5: Gate 6 — 10,000 km² preview**

```bash
scripts/scene/build.sh plan /out/preview10k --config scripts/scene/examples/pendleton-preview-10k.toml
/usr/bin/time -v scripts/scene/build.sh build /out/preview10k -j "$(nproc)"
scripts/scene/build.sh verify /out/preview10k
```

Record the same numbers as gate 1. Memory-independence check: the per-worker `peak_rss_mb` of gate 6 is within ~20 % of gate 1's per layer (both are bounded by tile size, not area).

- [ ] **Step 6: Record and commit**

Fill the "Measured" table in `docs/scene-packages.md` (Pendleton sim, 10k preview: wall time, per-layer tiles/files/bytes/seconds, peak worker RSS, cache size, `.sqfs` size, image digest, verify p50/p99). Update `REALISM.md` R0 status (replace "Next: the R0 spec" with the gate results, and replace section 3's volume estimates with the measured numbers, as R0's gate list asks) and the ROADMAP realism paragraph (one sentence with the headline numbers).

```bash
git add docs/scene-packages.md REALISM.md ROADMAP.md
git commit -m "docs(realism): R0 gates 1-4 and 6 measured (Pendleton sim, 10k km2 preview)"
```

---

### Task 25: Exit gate 5 (CamSim renders the package) and spike removal

Run on the macOS machine with CamSim built (M1 Pro). The TMS patch is applied locally and reverted afterwards; it is never committed (R1 lands the real `imagery.source: tms`).

**Files:**
- Delete: `scripts/scene/spike/` (remaining files)
- Modify: `docs/scene-packages.md`, `REALISM.md`, `ROADMAP.md`, `docs/realism-r0-spike.md` (pointer to the R0 tooling)

- [ ] **Step 1: Get the package onto the Mac**

Copy `.cache/scene-packages/pendleton/` from the Linux box (rsync), or build it natively: `uv run --project scripts/scene camsim-scene build .cache/scene-packages/pendleton --config scripts/scene/examples/pendleton.toml` (native builds need only pass verify). Then `uv run --project scripts/scene camsim-scene verify .cache/scene-packages/pendleton` → all `ok`.

- [ ] **Step 2: Apply the TMS patch and build CamSim**

```bash
git apply scripts/scene/tools/tms_overlay.patch && scripts/run.sh --build-only
```

Expected: build succeeds.

- [ ] **Step 3: Shots on the package (offline) and on ion**

```bash
PKG="$PWD/.cache/scene-packages/pendleton"
uv run --project scripts/scene python scripts/scene/tools/render_check.py .cache/gate5/package package "$PKG" --offline
uv run --project scripts/scene python scripts/scene/tools/render_check.py .cache/gate5/cwt cwt
```

Expected: `/ready` reached in the offline run and every shot `True`. In the CamSim log (`~/Library/Logs/CamSimTest/CamSimTest.log`) the only network errors allowed are the three frame-0 ion requests from `Main.umap` (human follow-up in ROADMAP); any TMS or terrain load failure is a gate failure.

- [ ] **Step 4: Registration and heights**

```bash
uv run --project scripts/scene python scripts/scene/tools/registration.py .cache/gate5/package/nadir_2km.png .cache/gate5/cwt/nadir_2km.png
uv run --project scripts/scene python scripts/scene/tools/registration.py .cache/gate5/package/interior_nadir.png .cache/gate5/cwt/interior_nadir.png
uv run --project scripts/scene python scripts/scene/tools/hot_check.py package "$PKG" --third <cached 1/3" blob> --onem <cached D24 blob>
```

(Blob paths: `jq -r '.sources[] | select(.id=="dep3_13") | .assets[0].sha256' "$PKG/manifest.json"` → `.cache/scene/blobs/sha256/<ab>/<sha>`; same for the `dep3_1m` asset covering 33.21 N −117.37 E.)
Expected: both shifts < 1 px (PASS); hot_check PASS for every point (≤ 0.25 m on 1 m, ≤ 1.0 m on 1/3″).

- [ ] **Step 5: Visual check of the high oblique views**

Open `.cache/gate5/package/high_oblique.png` and `ring_edge.png`. Gate: no holes (sky or black through the ground), no white/untextured tiles, no visible seam where NAIP meets WC S2 or WC S2 meets Blue Marble beyond a colour change. If the ring shows untextured tiles where Cesium asked for imagery deeper than z10, note it as an R1 item (imagery availability per region) rather than changing R0's zoom table here, and mark the gate failed in the record.

- [ ] **Step 6: Revert the patch**

Run: `git checkout -- unreal_project/CamSimTest/Source/CamSimTest/Geospatial/CamSimGeospatialProvider.cpp && git status --short`
Expected: no UE source changes listed.

- [ ] **Step 7: Delete the spike and record gate 5**

```bash
git rm -r scripts/scene/spike
```

Add gate 5's numbers and two shot comparisons (package vs CWT, `slant_ne` and `high_oblique`, JPEG under `docs/images/realism-r0/`, each < 200 KB) to `docs/scene-packages.md` "Measured". In `REALISM.md` mark R0 done (date, headline numbers, link to the guide); in `ROADMAP.md`'s realism paragraph replace "R0 spike done" with "R0 done" and the numbers; in `docs/realism-r0-spike.md` add one line at the top: "The R0 tooling replaced the spike code: `scripts/scene/` (`docs/scene-packages.md`)."

- [ ] **Step 8: Final verification and commit**

Run: `uv run --project scripts/scene --frozen pytest scripts/scene/tests -q && uv run --with pytest --with numpy --with pillow --with pycocotools pytest scripts/tests -q && uvx ruff check scripts/scene && uvx ruff format --check scripts/scene`
Expected: all pass.

```bash
git add -A docs/scene-packages.md docs/images/realism-r0 REALISM.md ROADMAP.md docs/realism-r0-spike.md scripts/scene
git commit -m "docs(realism): R0 gate 5 (CamSim renders the package offline); remove the spike"
```

---

## Spec coverage

| Spec item | Task |
|---|---|
| `plan`/`fetch`/`build`/`verify`/`pack`/`attribution` CLI, `build` runs missing steps | 21 (18) |
| `scene.toml` → `ScenePlan`, `--bbox` default file | 3, 21 |
| Manifest schema, canonical JSON, `hashes.txt`, `build.json` | 4, 18 |
| One tile grid, regions, terrain depth rule, complete siblings, `available` | 2 |
| Datum declarations, pinned pipelines, grids in the cache, `PROJ_NETWORK=OFF`, known point, 1.35 m shift, missing grid, sub-grid error | 8, 18 (grid fetch) |
| Content-addressed cache, URL index, atomic downloads, retries, re-signing, hash mismatch fails | 7 |
| Licence register, allow-list, `--allow`, attribution text, WC S2 licence confirmed | 6, 11, 21 |
| Source protocol, `SourceRaster`, adapters `dep3_13`, `dep3_1m`, `naip_pc`, `worldcover`, `etopo2022`, `bmng`, `wc_s2` | 9–13 |
| Priorities (1 m newest first → 1/3″ → ETOPO; NAIP → WC S2 → BMNG) | 3, 10, 14 |
| Terrain tile job (grid + margin, priority + 30 m feather, TIN, max_error) | 15 |
| Quantized-mesh writer + decoder, `layer.json` | 5, 18 |
| Imagery leaves/parents, JPEG q85 4:2:0, `tilemapresource.xml` | 16, 2, 18 |
| Land cover from WorldCover, `fetch_worldcover.py` wrapper, same output | 13, 18 |
| Global base (ETOPO 30″ + geoid, Blue Marble month) | 12 |
| Engine: process pool, worker LRU, resume markers, atomic writes, stats, `--json-progress` | 17, 9 (LRU), 18 |
| Verify quick + deep, `verify.json` | 19 |
| Pack (mksquashfs flags, sort file, `.sha256`) | 20 |
| Build container, `build.sh`, image digest in `build.json` | 22 |
| Edge cases: poles/antimeridian, coast fallthrough, overlapping 1 m projects, missing coverage, PC token expiry, interrupted fetch, disk estimate | 3, 5, 9, 10, 11, 7, 18 |
| RSS ≤ 2 GB per worker; memory independent of area | 18, 24 |
| Unit tests (all listed groups) + CI | 1–21 |
| Exit gates 1–6 | 24, 25 |
| Spike tools moved, spike deleted | 23, 25 |
