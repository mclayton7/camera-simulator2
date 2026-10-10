# R1 completion (seabed, local sea level, R1 gates) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace 3DEP's hydro-flattened plate over tidal water with NOAA topobathy, make a scene package's tide-0 sea equal local MSL, and run the R1 gates on the rebuilt Camp Pendleton package on macOS.

**Architecture:** Two new terrain source adapters (`noaa_sd13`, `noaa_crm_socal`) slot into the existing priority + feather merge; a tidal-water mask (WorldCover water ∧ 3DEP ≤ +1.5 m NAVD88 ∧ topobathy valid) invalidates 3DEP samples so they fall through. `plan` snapshots a NOAA CO-OPS station's datums into the manifest; `build` writes `sea_level.json` (local MSL − EGM96); CamSim reads it in `ResolvePackage` and adds it in `FOceanSurface::SeaLevelM`. Gate tools in `scripts/scene/tools/` and the bench measure the result.

**Tech Stack:** Python 3.12 (`uv`, numpy, rasterio 1.5.2 / GDAL 3.12 with netCDF, pyproj, pytest), UE 5.8 C++ (automation tests), CIGI 3.3 over UDP.

**Spec:** `docs/superpowers/specs/2026-10-10-r1-coast-and-gates-design.md`

## Global Constraints

- Tidal mask: WorldCover class **80**; 3DEP source value **≤ +1.5 m NAVD88**; masked sources `dep3_1m`, `dep3_13`; topobathy sources `noaa_sd13`, `noaa_crm_socal`.
- Terrain priorities: `sim` = `dep3_1m, dep3_13, noaa_sd13, noaa_crm_socal, etopo2022`; `preview` = `dep3_13, noaa_sd13, noaa_crm_socal, etopo2022`.
- `noaa_sd13`: `https://www.ngdc.noaa.gov/thredds/fileServer/regional/san_diego_13_navd88_2012.nc`, extent lon −117.83..−117.00, lat 32.45..33.60, datum `nad83_2011_navd88_geoid18`, max_zoom 14, area_kind `ring`.
- `noaa_crm_socal`: `https://www.ngdc.noaa.gov/thredds/fileServer/crm/crm_socal_3as_vers2.nc`, extent lon −128..−115, lat 30..37, MSL heights + `msl_above_navd88_m`, datum `nad83_2011_navd88_geoid18`, max_zoom 12, area_kind `ring`.
- Licence `LicenseRef-PublicDomain-USGov` for both.
- `sea_level.offset_m` must be finite and within **[−3, 3] m** at runtime; otherwise a resolve error.
- A manifest without `sea_level` / `layers.terrain.tidal_mask` (R0, chunk 1, chunk 2) builds **byte-identical** to before, and `manifest.json` written from it is unchanged.
- KLV Tags 15/25 stay EGM96 (do not touch `Metadata/KlvBuilder.cpp`).
- macOS only. Linux/Docker items are recorded as open, not run.
- Python style: match `scripts/scene` (module docstrings, `from __future__ import annotations`, ruff line length as the repo has). C++: UE naming, the copyright header `// Copyright CamSim Contributors. All Rights Reserved.`
- Headless UE tests on macOS need `-DisablePython`.
- Commit after every task; messages end with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01EuqoXt1sAk5mjhhppnKP9D
  ```

## Review Focus

1. **Inland water above sea level** (lakes, reservoirs, WorldCover 80 at +100 m): must keep 3DEP's surface — test in Task 4 (`test_lake_above_threshold_keeps_3dep`).
2. **Tidal water with no topobathy** (a coast outside both NOAA extents): 3DEP plate must stay (never fall to ETOPO's coarse cells) — test in Task 4 (`test_no_topobathy_keeps_3dep`).
3. **Old manifests**: a package planned before this change rebuilds byte-identical and its `manifest.json` round-trips unchanged — tests in Task 3 (`test_manifest_without_sea_level_round_trips_unchanged`) and Task 4 (`test_no_tidal_mask_in_manifest_builds_identical_tiles`).
4. **CRM without a station** (an area in the CRM extent, no `[sea_level]`): plan fails with a message naming the fix, never builds MSL heights as NAVD88 — test in Task 3 (`test_crm_assets_without_station_is_a_plan_error`).
5. **Malformed or extreme `sea_level.json`** at runtime: resolve error, not a silently shifted sea — test in Task 7 (`CamSim.Scene.Package.SeaLevel`).

---

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `scripts/scene/camsim_scene/sources/base.py` | `SourceRaster.add_m` (constant added to valid samples) | 1 |
| `scripts/scene/camsim_scene/water.py` | `class_codes(..., target_m=None)` reads a WorldCover overview at coarse zooms | 1 |
| `scripts/scene/camsim_scene/sources/noaa_dem.py` (new) | `NoaaSd13`, `NoaaCrmSocal` adapters | 2 |
| `scripts/scene/camsim_scene/sources/__init__.py` | registry entries | 2 |
| `scripts/scene/camsim_scene/sea_level.py` (new) | CO-OPS station section (plan), offset computation (build/verify) | 3, 5 |
| `scripts/scene/camsim_scene/config.py` | `[sea_level]`, new priorities, `tidal_mask` layer setting | 3 |
| `scripts/scene/camsim_scene/manifest.py` | optional `sea_level` field, omitted when `None` | 3 |
| `scripts/scene/camsim_scene/pipeline.py` | plan: station + CRM metadata; fetch: EGM96 grid; terrain inputs hash; build writes `sea_level.json` | 3, 4, 5 |
| `scripts/scene/camsim_scene/layers/terrain.py` | `TidalMask`, mask in `tile_grid` / `point_heights` | 4 |
| `scripts/scene/camsim_scene/context.py` | `WorkerState.tidal`, `terrain_extra` | 4 |
| `scripts/scene/camsim_scene/verify.py` | mask in deep check; `sea_level` check | 4, 5 |
| `scripts/scene/examples/pendleton.toml`, `docs/scene-packages.md` | config + docs | 6 |
| `unreal_project/.../Config/CamSimConfig.h`, `Config/ScenePackage.cpp`, `Ocean/OceanSurface.{h,cpp}`, `Subsystem/CamSimSubsystem.cpp`, `Tests/ScenePackageTest.cpp`, `Tests/OceanSurfaceTest.cpp` | runtime sea level | 7 |
| `docs/configuration.md`, `CLAUDE.md` | runtime docs | 7 |
| `scripts/scene/tools/coast_check.py` (new), `scripts/scene/tests/test_coast_check.py` (new) | gate 3 | 8 |
| `scripts/scene/tools/hot_check.py`, `scripts/scene/tools/render_check.py` | gates 4, 6, 7 | 9 |
| `scripts/bench/scenario.py`, `scripts/bench/run_bench.py` | `--site pendleton`, `coast_pass` | 10 |
| `docs/scene-packages.md`, `REALISM.md`, `scripts/scene/tools/README.md` | results | 11-13 |

All paths below are relative to the repo root. Python commands run from the repo root as
`uv run --project scripts/scene ...`; the scene test suite is
`uv run --project scripts/scene --with pytest pytest scripts/scene/tests -q`.

---

### Task 1: `SourceRaster.add_m` and overview class lookup

**Files:**
- Modify: `scripts/scene/camsim_scene/sources/base.py` (`SourceRaster` dataclass and `sample`)
- Modify: `scripts/scene/camsim_scene/water.py` (`_pixels`, `class_codes`)
- Test: `scripts/scene/tests/test_sources_base.py`, `scripts/scene/tests/test_water.py`

**Interfaces:**
- Produces: `SourceRaster(..., add_m: float = 0.0)` — `sample()` returns values + `add_m` where valid. `class_codes(classes, lon, lat, target_m: float | None = None) -> np.ndarray[uint8]` — `None` keeps today's full-resolution behaviour; a value reads the coarsest overview whose pixel ≤ `target_m` (same rule as `choose_overview`).

- [ ] **Step 1: Write the failing tests**

In `test_sources_base.py` (reuse the file's existing GeoTIFF helpers; `write_geotiff` is in `tests/rasters.py`):

```python
def test_add_m_shifts_valid_samples_only(tmp_path):
    data = np.array([[1.0, 2.0], [-9999.0, 4.0]], np.float32)
    write_geotiff(tmp_path / "a.tif", data, 0.0, 2.0, 1.0, nodata=-9999.0)
    plain = SourceRaster(path=tmp_path / "a.tif", datum="wgs84", nodata=-9999.0)
    shifted = SourceRaster(path=tmp_path / "a.tif", datum="wgs84", nodata=-9999.0, add_m=0.774)
    x, y = np.array([0.5, 1.5, 0.5]), np.array([1.5, 1.5, 0.5])
    v0, ok0 = plain.sample(x, y, 1.0)
    v1, ok1 = shifted.sample(x, y, 1.0)
    assert (ok0 == ok1).all()
    np.testing.assert_allclose(v1[0][ok1], v0[0][ok0] + 0.774)
    assert not ok1[2]
```

In `test_water.py`:

```python
def test_class_codes_with_target_m_reads_an_overview(tmp_path):
    # 400 x 400 px at 0.0001 deg (~11 m): west half water (80), east half land (10); overviews 2, 4, 8
    codes = np.full((400, 400), 10, np.uint8)
    codes[:, :200] = 80
    write_geotiff(tmp_path / "wc.tif", codes, 10.0, 10.04, 0.0001, overviews=(2, 4, 8), resampling="nearest")
    r = SourceRaster(path=tmp_path / "wc.tif", datum="wgs84")
    lon, lat = np.array([10.005, 10.035]), np.array([10.02, 10.02])
    np.testing.assert_array_equal(class_codes((r,), lon, lat), [80, 10])
    np.testing.assert_array_equal(class_codes((r,), lon, lat, target_m=90.0), [80, 10])
    assert class_codes((r,), lon, lat, target_m=5.0).tolist() == [80, 10]  # finer than the data: full resolution
```

If `write_geotiff` has no `resampling` argument, add one (default `"average"`, keeping today's behaviour) in
`tests/rasters.py` and pass it to `ds.build_overviews`.

- [ ] **Step 2: Run them to see them fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_sources_base.py::test_add_m_shifts_valid_samples_only scripts/scene/tests/test_water.py::test_class_codes_with_target_m_reads_an_overview -q`
Expected: FAIL (`unexpected keyword argument 'add_m'` / `'target_m'`).

- [ ] **Step 3: Implement**

`base.py`, in `SourceRaster` after `vertical_asset`:

```python
    add_m: float = 0.0  # added to every valid sample (a constant vertical datum shift, e.g. MSL -> NAVD88)
```

At the end of `SourceRaster.sample`, just before it returns `out, valid`:

```python
        if self.add_m:
            out[:, valid] += self.add_m
```

`water.py`:

```python
def _pixels(r, lon, lat, level: int | None = None):
    ds = _dataset(str(r.path), level)
    ...  # unchanged


def _level(r, target_m: float | None) -> int | None:
    """Overview level for a read at target_m (metres per sample), or None for full resolution."""
    if target_m is None:
        return None
    from .sources.base import M_PER_DEG, choose_overview

    ds = _dataset(str(r.path), None)
    return choose_overview(abs(ds.transform.a) * M_PER_DEG, list(ds.overviews(r.bands[0])), target_m)
```

and in `class_codes(classes, lon, lat, target_m: float | None = None)`: `ds, col, row = _pixels(r, lon, lat, _level(r, target_m))`. Update the docstring: "`target_m`: read the coarsest overview whose pixel is at most this size (coarse zooms); None: full resolution".

- [ ] **Step 4: Run the tests and the whole suite**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests -q`
Expected: all pass (network tests skipped).

- [ ] **Step 5: Commit**

```bash
git add scripts/scene/camsim_scene/sources/base.py scripts/scene/camsim_scene/water.py scripts/scene/tests
git commit -m "feat(scene): SourceRaster.add_m and overview reads in class_codes"
```

---

### Task 2: NOAA topobathy adapters

**Files:**
- Create: `scripts/scene/camsim_scene/sources/noaa_dem.py`
- Modify: `scripts/scene/camsim_scene/sources/__init__.py`
- Test: `scripts/scene/tests/test_sources_noaa.py` (new)

**Interfaces:**
- Consumes: `SourceRaster.add_m` (Task 1).
- Produces: adapters `noaa_sd13` (`NoaaSd13`) and `noaa_crm_socal` (`NoaaCrmSocal`). `NoaaCrmSocal.vertical_from_msl = True` (class attribute, read by Task 3's `plan_scene`); its `open` requires `asset.metadata["msl_above_navd88_m"]` and raises `ValueError` without it. `NoaaSd13.vertical_from_msl = False`.

- [ ] **Step 1: Write the failing tests**

```python
"""NOAA topobathy adapters (R1 seabed): discovery by extent, nodata from the file, CRM's MSL shift."""

import numpy as np
import pytest
from fakes import FakeHttp
from rasters import write_geotiff

from camsim_scene.sources import make_source
from camsim_scene.sources.base import Area, Asset
from camsim_scene.sources.noaa_dem import CRM_URL, SD13_URL

PENDLETON_RING = (-118.709, 32.292, -116.151, 34.418)


def http():
    return FakeHttp({("HEAD", SD13_URL): (200, 445596180), ("HEAD", CRM_URL): (200, 524449536)})


def test_discover_inside_extent():
    for sid, url, size in (("noaa_sd13", SD13_URL, 445596180), ("noaa_crm_socal", CRM_URL, 524449536)):
        a = make_source(sid, http=http()).discover(Area(PENDLETON_RING))
        assert [(x.url, x.size, x.role) for x in a] == [(url, size, "data")]
        assert len(a[0].metadata["bbox"]) == 4


def test_discover_outside_extent_finds_nothing():
    sf = (-122.6, 37.6, -122.3, 37.9)
    assert make_source("noaa_sd13", http=http()).discover(Area(sf)) == []
    assert make_source("noaa_crm_socal", http=FakeHttp({})).discover(Area((-80.0, 25.0, -79.0, 26.0))) == []


def test_open_takes_nodata_from_the_file_and_crm_adds_its_shift(tmp_path):
    write_geotiff(tmp_path / "d.tif", np.array([[-5.0, -9999.0]], np.float32), -117.5, 33.3, 0.01, nodata=-9999.0)
    sd = make_source("noaa_sd13").open(tmp_path / "d.tif", Asset("sd", SD13_URL))
    assert sd.nodata == -9999.0 and sd.add_m == 0.0 and sd.datum == "nad83_2011_navd88_geoid18"
    crm = make_source("noaa_crm_socal").open(
        tmp_path / "d.tif", Asset("crm", CRM_URL, metadata={"msl_above_navd88_m": 0.774})
    )
    assert crm.add_m == 0.774 and crm.nodata == -9999.0


def test_crm_open_without_the_shift_is_an_error(tmp_path):
    write_geotiff(tmp_path / "d.tif", np.zeros((1, 1), np.float32), -117.5, 33.3, 0.01)
    with pytest.raises(ValueError, match="msl_above_navd88_m"):
        make_source("noaa_crm_socal").open(tmp_path / "d.tif", Asset("crm", CRM_URL))


@pytest.mark.network
def test_live_heads_answer():
    from camsim_scene.net import Http

    for sid in ("noaa_sd13", "noaa_crm_socal"):
        a = make_source(sid, http=Http()).discover(Area(PENDLETON_RING))
        assert a and a[0].size > 100_000_000
```

- [ ] **Step 2: Run them to see them fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_sources_noaa.py -q`
Expected: FAIL (`No module named 'camsim_scene.sources.noaa_dem'`).

- [ ] **Step 3: Implement `noaa_dem.py`**

```python
"""NOAA NGDC coastal topobathy DEMs for the seabed under tidal water (REALISM R1; spec
docs/superpowers/specs/2026-10-10-r1-coast-and-gates-design.md). CUDEM has no Southern California tiles
(checked 2026-10-10), so the Pendleton package uses the San Diego 1/3" NAVD88 tsunami DEM near the coast and
the Coastal Relief Model (MSL) across the ring. Both are static netCDF files (2016 / 2018) on NCEI THREDDS;
`prepare` turns each into a COG. Public domain (US Government work); not for navigation."""

from __future__ import annotations

from pathlib import Path

from ..net import HttpError
from .base import Area, Asset, Layer, SourceBase, SourceRaster, _dataset

SD13_URL = "https://www.ngdc.noaa.gov/thredds/fileServer/regional/san_diego_13_navd88_2012.nc"
CRM_URL = "https://www.ngdc.noaa.gov/thredds/fileServer/crm/crm_socal_3as_vers2.nc"


def _intersects(a, b) -> bool:
    return a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]


class _NoaaDem(SourceBase):
    layer = Layer.TERRAIN
    licence = "LicenseRef-PublicDomain-USGov"
    area_kind = "ring"
    datum = "nad83_2011_navd88_geoid18"  # NAD83 taken as NAD83(2011) (datum.py); GEOID18 for NAVD88
    url = ""
    extent: tuple[float, float, float, float] = (0.0, 0.0, 0.0, 0.0)
    asset_id = ""
    vertical_from_msl = False  # True: heights are MSL; plan_scene stores the station's MSL - NAVD88 per asset

    def discover(self, area: Area) -> list[Asset]:
        if not _intersects(area.bounds, self.extent):
            return []
        status, size = self.http.head(self.url)
        if status != 200:
            raise HttpError(f"HEAD {self.url}: HTTP {status}")
        return [Asset(id=self.asset_id, url=self.url, size=size, group=self.id, metadata={"bbox": list(self.extent)})]

    def _raster(self, path: Path, add_m: float) -> SourceRaster:
        return SourceRaster(path=Path(path), datum=self.datum, nodata=_dataset(str(path), None).nodata, add_m=add_m)

    def open(self, path: Path, asset: Asset) -> SourceRaster:
        return self._raster(path, 0.0)


class NoaaSd13(_NoaaDem):
    id = "noaa_sd13"
    dataset = "NOAA NGDC San Diego, CA 1/3 arc-second NAVD 88 Coastal Digital Elevation Model"
    version = "2012"
    attribution = (
        "NOAA National Geophysical Data Center (2012): San Diego, CA 1/3 arc-second NAVD 88 Coastal Digital "
        "Elevation Model"
    )
    max_zoom = 14
    url = SD13_URL
    extent = (-117.83005, 32.44995, -116.99995, 33.60005)
    asset_id = "san_diego_13_navd88_2012"


class NoaaCrmSocal(_NoaaDem):
    id = "noaa_crm_socal"
    dataset = "NOAA NGDC U.S. Coastal Relief Model - Southern California vers. 2 (3 arc-second)"
    version = "2"
    attribution = (
        "National Geophysical Data Center, 2012. U.S. Coastal Relief Model - Southern California vers. 2. "
        "NOAA. doi:10.7289/V5V985ZM"
    )
    max_zoom = 12
    url = CRM_URL
    extent = (-128.0, 30.0, -115.0, 37.0)
    asset_id = "crm_socal_3as_vers2"
    vertical_from_msl = True

    def open(self, path: Path, asset: Asset) -> SourceRaster:
        if "msl_above_navd88_m" not in asset.metadata:
            raise ValueError(
                f"{self.id}/{asset.id}: no msl_above_navd88_m in the asset metadata (the CRM is MSL; the package "
                "needs a [sea_level] station, re-plan)"
            )
        return self._raster(path, float(asset.metadata["msl_above_navd88_m"]))
```

Note: `SourceBase.__init__` sets `self.http`; tests that call `open` pass no http, which is fine.
`make_source` sets `src.id`. `prepare` is inherited (`prepare_raster`: netCDF has no tiling or overviews, so it converts).

Registry, in `sources/__init__.py` `BUILTIN`:

```python
    "noaa_sd13": "camsim_scene.sources.noaa_dem:NoaaSd13",
    "noaa_crm_socal": "camsim_scene.sources.noaa_dem:NoaaCrmSocal",
```

- [ ] **Step 4: Run the tests**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_sources_noaa.py -q` → PASS.
Then once, with the network: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_sources_noaa.py -q --network` → PASS (real HEADs).

- [ ] **Step 5: Commit**

```bash
git add scripts/scene/camsim_scene/sources scripts/scene/tests/test_sources_noaa.py
git commit -m "feat(scene): NOAA San Diego 1/3\" and Coastal Relief Model topobathy sources"
```

---

### Task 3: `[sea_level]` station, priorities, tidal-mask setting, manifest field

**Files:**
- Create: `scripts/scene/camsim_scene/sea_level.py`
- Modify: `scripts/scene/camsim_scene/config.py`, `scripts/scene/camsim_scene/manifest.py`, `scripts/scene/camsim_scene/pipeline.py` (`plan_scene`, `plan_differences`, `_fetch`, `make_context`)
- Test: `scripts/scene/tests/test_sea_level.py` (new), `scripts/scene/tests/test_config.py`, `scripts/scene/tests/test_manifest.py`, `scripts/scene/tests/test_pipeline.py`

**Interfaces:**
- Consumes: `vertical_from_msl` (Task 2).
- Produces:
  - `config.ScenePlan.sea_level_station: str | None` (default `None`).
  - `config.TIDAL_MASK` dict and `layer_settings(plan)["terrain"]["tidal_mask"]` (present only when the plan has a masked source, a topobathy source and `worldcover` in the land-cover priorities).
  - `Manifest.sea_level: dict | None = None` (omitted from `to_dict()` when `None`).
  - `sea_level.EGM96_GRID = "us_nga_egm96_15.tif"`, `sea_level.station_section(http, station: str) -> dict` returning
    `{"station", "name", "lat", "lon", "epoch", "msl_m", "navd88_m", "msl_above_navd88_m", "geoid": "nad83_2011_navd88_geoid18", "egm96_grid": {"name", "url", "sha256": None}}`.
  - `make_context(...).grid_paths` also holds `"us_nga_egm96_15.tif"` when the manifest has `sea_level`.

- [ ] **Step 1: Write the failing tests**

`test_sea_level.py`:

```python
import pytest
from fakes import FakeHttp

from camsim_scene import sea_level

DATUMS = sea_level.COOPS_DATUMS.format(station="9410230")
STATION = sea_level.COOPS_STATION.format(station="9410230")
LA_JOLLA = {
    "datums": [
        {"name": "MLLW", "value": 1.331},
        {"name": "MSL", "value": 2.163},
        {"name": "MHW", "value": 2.733},
        {"name": "NAVD88", "value": 1.389},
    ],
    "epoch": "1983-2001",
    "units": "meters",
}


def http(datums=LA_JOLLA):
    return FakeHttp(
        {DATUMS: datums, STATION: {"stations": [{"id": "9410230", "name": "La Jolla", "lat": 32.8669, "lng": -117.2571}]}}
    )


def test_station_section_from_coops():
    s = sea_level.station_section(http(), "9410230")
    assert s["station"] == "9410230" and s["name"] == "La Jolla"
    assert s["msl_above_navd88_m"] == pytest.approx(0.774, abs=1e-9)
    assert (s["lat"], s["lon"], s["epoch"]) == (32.8669, -117.2571, "1983-2001")
    assert s["egm96_grid"] == {
        "name": "us_nga_egm96_15.tif",
        "url": "https://cdn.proj.org/us_nga_egm96_15.tif",
        "sha256": None,
    }


def test_station_without_navd88_is_an_error():
    no_navd = {**LA_JOLLA, "datums": [d for d in LA_JOLLA["datums"] if d["name"] != "NAVD88"]}
    with pytest.raises(sea_level.SeaLevelError, match="NAVD88"):
        sea_level.station_section(http(no_navd), "9410230")
```

`test_config.py` additions:

```python
def test_sea_level_station_parses():
    p = parse_scene({"name": "x", "bbox": [-117.6, 33.2, -117.2, 33.5], "sea_level": {"station": "9410230"}})
    assert p.sea_level_station == "9410230"


@pytest.mark.parametrize("bad", [{"station": 9410230}, {"station": ""}, {"station": "x1"}, {"stn": "9410230"}])
def test_sea_level_station_rejects_bad_values(bad):
    with pytest.raises(ConfigError, match="sea_level"):
        parse_scene({"name": "x", "bbox": [-117.6, 33.2, -117.2, 33.5], "sea_level": bad})


def test_sim_terrain_priorities_and_tidal_mask():
    p = parse_scene({"name": "x", "bbox": [-117.6, 33.2, -117.2, 33.5]})
    assert p.priorities["terrain"] == ["dep3_1m", "dep3_13", "noaa_sd13", "noaa_crm_socal", "etopo2022"]
    assert layer_settings(p)["terrain"]["tidal_mask"] == {
        "classes_source": "worldcover",
        "class": 80,
        "max_navd88_m": 1.5,
        "sources": ["dep3_1m", "dep3_13"],
        "topobathy": ["noaa_sd13", "noaa_crm_socal"],
    }


def test_no_tidal_mask_without_topobathy():
    p = parse_scene(
        {"name": "x", "bbox": [-117.6, 33.2, -117.2, 33.5], "priorities": {"terrain": ["dep3_13", "etopo2022"]}}
    )
    assert "tidal_mask" not in layer_settings(p)["terrain"]
```

`test_manifest.py` addition (use the file's existing helper that builds a small `Manifest`; if none, build one with `Manifest(name="t", bbox=[0,0,1,1], seed=0, regions=[], tiling={}, layers={}, datum={}, sources=[], licence_allow=[], tool={})`):

```python
def test_manifest_without_sea_level_round_trips_unchanged(tmp_path):
    m = small_manifest()
    text = m.dumps()
    assert '"sea_level"' not in text
    assert Manifest.from_dict(json.loads(text)).dumps() == text


def test_manifest_with_sea_level_round_trips(tmp_path):
    m = small_manifest()
    m.sea_level = {"station": "9410230", "msl_above_navd88_m": 0.774}
    again = Manifest.from_dict(json.loads(m.dumps()))
    assert again.sea_level == m.sea_level
```

`test_pipeline.py` additions (use the file's existing `plan_scene` helpers and `FakeHttp`; the synthetic scene uses fake sources, so add a fake CRM by subclassing the fake source with `vertical_from_msl = True`):

```python
def test_crm_assets_without_station_is_a_plan_error(tmp_path):
    scene = synthetic_scene(tmp_path / "src")
    scene["priorities"]["terrain"].insert(1, "crm")
    scene["sources"]["crm"] = source("terrain", tmp_path / "src" / "base_dem.tif", GLOBE, adapter="fake_sources:FakeMslSource")
    with pytest.raises(PlanError, match=r"\[sea_level\] station"):
        plan_scene(parse_scene(scene), tmp_path / "pkg", http=FakeHttp({}))


def test_station_metadata_reaches_crm_assets(tmp_path):
    scene = synthetic_scene(tmp_path / "src")
    scene["priorities"]["terrain"].insert(1, "crm")
    scene["sources"]["crm"] = source("terrain", tmp_path / "src" / "base_dem.tif", GLOBE, adapter="fake_sources:FakeMslSource")
    scene["sea_level"] = {"station": "9410230"}
    m = plan_scene(parse_scene(scene), tmp_path / "pkg", http=coops_http())  # coops_http: the La Jolla routes above
    assert m.sea_level["msl_above_navd88_m"] == pytest.approx(0.774)
    assert all(a.metadata["msl_above_navd88_m"] == pytest.approx(0.774) for a in m.source("crm").assets)
```

Add `FakeMslSource(FakeSource)` with `vertical_from_msl = True` to `tests/fake_sources.py`, and put the La Jolla
routes in a shared helper `coops_http()` in `tests/fakes.py` (move `LA_JOLLA` there and import it in
`test_sea_level.py`).

- [ ] **Step 2: Run them to see them fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_sea_level.py scripts/scene/tests/test_config.py scripts/scene/tests/test_manifest.py scripts/scene/tests/test_pipeline.py -q`
Expected: FAIL (missing module / attributes).

- [ ] **Step 3: Implement**

`sea_level.py` (plan half; Task 5 adds the computation):

```python
"""Local mean sea level for a scene package (REALISM R1): CamSim's tide-0 sea is EGM96, local MSL is not. `plan`
snapshots a NOAA CO-OPS station's tidal datums into the manifest (`sea_level`); `build` turns them into
sea_level.json (offset_m = local MSL - EGM96 at the station), which CamSim adds under the CIGI tide."""

from __future__ import annotations

COOPS = "https://api.tidesandcurrents.noaa.gov/mdapi/prod/webapi/stations/{station}"
COOPS_DATUMS = COOPS + "/datums.json?units=metric"
COOPS_STATION = COOPS + ".json"
GEOID_DATUM = "nad83_2011_navd88_geoid18"
EGM96_GRID = "us_nga_egm96_15.tif"
FILE = "sea_level.json"


class SeaLevelError(Exception):
    pass


def station_section(http, station: str) -> dict:
    """The manifest's `sea_level` section from CO-OPS (values as published, metres, station datum)."""
    d = http.get_json(COOPS_DATUMS.format(station=station))
    values = {x["name"]: x["value"] for x in d.get("datums") or [] if x.get("value") is not None}
    missing = [k for k in ("MSL", "NAVD88") if k not in values]
    if missing:
        raise SeaLevelError(
            f"CO-OPS station {station} publishes no {' or '.join(missing)} datum: pick a station with a NAVD88 tie"
        )
    st = (http.get_json(COOPS_STATION.format(station=station)).get("stations") or [{}])[0]
    from .datum import GRID_URL

    return {
        "station": station,
        "name": st.get("name", ""),
        "lat": float(st["lat"]),
        "lon": float(st["lng"]),
        "epoch": d.get("epoch", ""),
        "msl_m": float(values["MSL"]),
        "navd88_m": float(values["NAVD88"]),
        "msl_above_navd88_m": round(float(values["MSL"]) - float(values["NAVD88"]), 6),
        "geoid": GEOID_DATUM,
        "egm96_grid": {"name": EGM96_GRID, "url": GRID_URL.format(name=EGM96_GRID), "sha256": None},
    }
```

`net.Http.get_json(url, params=None)` already exists (FakeHttp mirrors it); check its signature in `net.py` and pass
the query string inside the URL as above.

`config.py`:
- Add `"sea_level"` to `KEYS`; update the module docstring's example with a `[sea_level]` table
  (`station = "9410230"   # NOAA CO-OPS station with a NAVD88 tie (omit inland)`).
- Profiles: `sim` terrain `["dep3_1m", "dep3_13", "noaa_sd13", "noaa_crm_socal", "etopo2022"]`, `preview` terrain
  `["dep3_13", "noaa_sd13", "noaa_crm_socal", "etopo2022"]`.
- Constants:

```python
TIDAL_MASK = {  # 3DEP's hydro-flattened plate over tidal water falls through to topobathy (layers/terrain.py)
    "classes_source": "worldcover",
    "class": 80,  # WorldCover 2021: permanent water bodies
    "max_navd88_m": 1.5,  # just above MHW (+1.34 m NAVD88 at La Jolla): lakes above sea level keep 3DEP
    "sources": ["dep3_1m", "dep3_13"],
    "topobathy": ["noaa_sd13", "noaa_crm_socal"],
}
STATION_RE = re.compile(r"^\d{7}$")
```

- `ScenePlan` gains `sea_level_station: str | None = None`. In `parse_scene`:

```python
    sl = data.get("sea_level")
    station = None
    if sl is not None:
        if not isinstance(sl, dict) or set(sl) != {"station"} or not isinstance(sl["station"], str) \
                or not STATION_RE.match(sl["station"]):
            raise ConfigError('sea_level must be a table with one key, station = "<7-digit NOAA CO-OPS id>"')
        station = sl["station"]
```

  and pass `sea_level_station=station` to `ScenePlan(...)`.
- `layer_settings`: after building `out["terrain"]`:

```python
    t = plan.priorities["terrain"]
    masked = [s for s in TIDAL_MASK["sources"] if s in t]
    topo = [s for s in TIDAL_MASK["topobathy"] if s in t]
    if masked and topo and TIDAL_MASK["classes_source"] in plan.priorities["landcover"]:
        out["terrain"]["tidal_mask"] = {**TIDAL_MASK, "sources": masked, "topobathy": topo}
```

`manifest.py`: add the field last (after `hashes_sha256`) so positional construction elsewhere is unaffected:

```python
    sea_level: dict | None = None  # NOAA CO-OPS station datums (sea_level.py); absent in packages without one

    def to_dict(self) -> dict:
        d = asdict(self)
        if d["sea_level"] is None:
            del d["sea_level"]  # packages planned without a station keep their exact manifest.json
        return d
```

`pipeline.py`:
- `plan_scene`: before the sources loop,
  `sea = sea_level.station_section(http, plan.sea_level_station) if plan.sea_level_station else None`
  (wrap `SeaLevelError` in `PlanError`). Inside the loop after `assets = src.discover(...)`:

```python
        if assets and getattr(src, "vertical_from_msl", False):
            if sea is None:
                raise PlanError(
                    f"{sid} heights are MSL: the scene needs a [sea_level] station (a NOAA CO-OPS id with a NAVD88 "
                    "tie, e.g. station = \"9410230\") to convert them"
                )
            for a in assets:
                a.metadata["msl_above_navd88_m"] = sea["msl_above_navd88_m"]
```

  and pass `sea_level=sea` to `Manifest(...)`.
- `plan_differences`: add `"sea_level_station": plan.sea_level_station` to `want` and
  `"sea_level_station": (m.sea_level or {}).get("station")` to `have`.
- `_fetch`: after the grids loop,

```python
    if m.sea_level:
        g = m.sea_level["egm96_grid"]
        _, g["sha256"], _ = cache.get(g["url"], g.get("sha256"))
```

- `Manifest.is_fetched`: also require `m.sea_level["egm96_grid"]["sha256"]` when `sea_level` is set.
- `make_context`: after `grids = {...}`,

```python
    if m.sea_level:
        g = m.sea_level["egm96_grid"]
        grids[g["name"]] = str(cache.get(g["url"], g["sha256"])[0])
```

- [ ] **Step 4: Run the suite**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests -q` → all pass. Existing tests that
assert the default `sim`/`preview` priorities must be updated to the new lists (that is the intended change); no
other existing test may change.

- [ ] **Step 5: Commit**

```bash
git add scripts/scene
git commit -m "feat(scene): [sea_level] station from NOAA CO-OPS, topobathy priorities, tidal-mask setting"
```

---

### Task 4: Tidal-water mask in the terrain merge and verify

**Files:**
- Modify: `scripts/scene/camsim_scene/layers/terrain.py`, `scripts/scene/camsim_scene/context.py`, `scripts/scene/camsim_scene/pipeline.py` (`_terrain_tile`), `scripts/scene/camsim_scene/verify.py` (`_deep`)
- Test: `scripts/scene/tests/test_terrain.py`

**Interfaces:**
- Consumes: `class_codes(..., target_m)` (Task 1), `layers.terrain.tidal_mask` (Task 3), `context.class_rasters(m, asset_paths)` (exists).
- Produces:
  - `terrain.TidalMask(classes: tuple, water_class: int, max_raw_m: float, sources: frozenset[str], topobathy: frozenset[str])`, `TidalMask.from_settings(settings: dict, classes: tuple) -> TidalMask`.
  - `terrain.tile_grid(z, x, y, entries, tidal: TidalMask | None = None)`, `terrain.build_tile(z, x, y, entries, tidal=None)`, `terrain.point_heights(z, lon, lat, entries, tidal=None)`.
  - `WorkerState.tidal: TidalMask | None`, `WorkerState.terrain_extra: list[str]` (hashes added to every terrain tile's inputs: the WorldCover sha256s and `sha256(canonical_json(manifest.sea_level))` when present).

- [ ] **Step 1: Write the failing tests** (in `test_terrain.py`)

Build a small scene: base (global, 0 m), `hi` 3DEP-like source (id `dep3_13`, a 0.0 m plate over the west half,
+50 m land over the east half), a topobathy source (id `noaa_sd13`, −10 m everywhere in its box), and a WorldCover-like
class raster (west half 80, east half 10) wired as the land-cover `worldcover` source. Use the fake source adapter
with ids chosen to match `TIDAL_MASK`:

```python
def coast_scene(tmp_path, plate=0.0, topo_box=None, class_west=80):
    src = tmp_path / "src"
    src.mkdir(parents=True, exist_ok=True)
    res, west, north, n = 0.00005, 9.99, 10.04, 1000
    dem = np.full((n, n), 50.0, np.float32)
    dem[:, : n // 2] = plate
    write_geotiff(src / "base.tif", np.zeros((180, 360), np.float32), -180.0, 90.0, 1.0)
    write_geotiff(src / "dep.tif", dem, west, north, res, overviews=(2, 4, 8))
    write_geotiff(src / "topo.tif", np.full((n, n), -10.0, np.float32), west, north, res, overviews=(2, 4, 8))
    codes = np.full((n, n), 10, np.uint8)
    codes[:, : n // 2] = class_west
    write_geotiff(src / "wc.tif", codes, west, north, res, overviews=(2, 4, 8), resampling="nearest")
    box = [west, north - n * res, west + n * res, north]
    terrain_prio = ["dep3_13", "noaa_sd13", "base"] if topo_box is not False else ["dep3_13", "base"]
    scene = {
        "name": "t",
        "bbox": box,
        "priorities": {"terrain": terrain_prio, "imagery": [], "landcover": ["worldcover"]},
        "sources": {
            "base": source("terrain", src / "base.tif", GLOBE, **{"global": True}),
            "dep3_13": source("terrain", src / "dep.tif", box, max_zoom=16),
            "noaa_sd13": source("terrain", src / "topo.tif", topo_box or box, max_zoom=16),
            "worldcover": source("landcover", src / "wc.tif", box),
        },
    }
    return scene


def mid_heights(tmp_path, scene, z=14):
    st = WorkerState(fake_context(tmp_path, scene))
    x, y = tile_at(z, 10.015, 10.015)
    g = terrain.tile_grid(z, x, y, st.index["terrain"].query(terrain.query_bounds(z, x, y)), st.tidal)
    west = g.h[:, :40][np.isfinite(g.h[:, :40])]
    east = g.h[:, -40:]
    return st, west, east


def test_plate_over_tidal_water_falls_through_to_topobathy(tmp_path):
    st, west, east = mid_heights(tmp_path, coast_scene(tmp_path))
    assert st.tidal is not None
    np.testing.assert_allclose(west, -10.0, atol=1e-6)
    np.testing.assert_allclose(east, 50.0, atol=1e-6)


def test_lake_above_threshold_keeps_3dep(tmp_path):
    _, west, _ = mid_heights(tmp_path, coast_scene(tmp_path, plate=2.0))
    np.testing.assert_allclose(west, 2.0, atol=1e-6)


def test_no_topobathy_keeps_3dep(tmp_path):
    far = [20.0, 20.0, 20.1, 20.1]  # the topobathy source covers nowhere near the tile
    _, west, _ = mid_heights(tmp_path, coast_scene(tmp_path, topo_box=far))
    np.testing.assert_allclose(west, 0.0, atol=1e-6)


def test_land_class_keeps_3dep(tmp_path):
    _, west, _ = mid_heights(tmp_path, coast_scene(tmp_path, class_west=10))
    np.testing.assert_allclose(west, 0.0, atol=1e-6)


def test_no_tidal_mask_in_manifest_builds_identical_tiles(tmp_path):
    scene = coast_scene(tmp_path)
    st = WorkerState(fake_context(tmp_path / "a", scene))
    assert st.tidal is not None
    z = 14
    x, y = tile_at(z, 10.015, 10.015)
    entries = st.index["terrain"].query(terrain.query_bounds(z, x, y))
    assert terrain.build_tile(z, x, y, entries) == terrain.build_tile(z, x, y, entries, None)
    assert terrain.build_tile(z, x, y, entries, st.tidal) != terrain.build_tile(z, x, y, entries)


def test_point_heights_apply_the_mask(tmp_path):
    st = WorkerState(fake_context(tmp_path, coast_scene(tmp_path)))
    z = 14
    lon, lat = np.array([10.0, 10.03]), np.array([10.015, 10.015])
    entries = st.index["terrain"].query((9.99, 10.0, 10.04, 10.04))
    np.testing.assert_allclose(terrain.point_heights(z, lon, lat, entries, st.tidal), [-10.0, 50.0], atol=1e-6)
    np.testing.assert_allclose(terrain.point_heights(z, lon, lat, entries), [0.0, 50.0], atol=1e-6)
```

`fake_context` writes the manifest from the scene via `layer_settings`, so the `tidal_mask` key appears whenever
the priorities qualify (Task 3). If `fake_context` doesn't hand land-cover assets to `class_rasters` (it reads
`m.layers["landcover"]["priorities"]` and `m.source("worldcover")`), check that the fake land-cover source's assets
are in `ctx.asset_paths` (they are for every source in `synthetic_scene`).

- [ ] **Step 2: Run them to see them fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_terrain.py -q`
Expected: the new tests FAIL (`WorkerState` has no `tidal`; `tile_grid` takes no `tidal`).

- [ ] **Step 3: Implement**

`layers/terrain.py`:

```python
@dataclass(frozen=True)
class TidalMask:
    """3DEP's hydro-flattened plate over tidal water falls through to topobathy (spec Part A): a sample of a
    `sources` group is invalid where WorldCover is water, its source value is <= max_raw_m and a `topobathy` group
    is valid there."""

    classes: tuple  # WorldCover SourceRasters (water.class_codes)
    water_class: int
    max_raw_m: float
    sources: frozenset
    topobathy: frozenset

    @classmethod
    def from_settings(cls, s: dict, classes: tuple) -> TidalMask:
        return cls(tuple(classes), int(s["class"]), float(s["max_navd88_m"]), frozenset(s["sources"]),
                   frozenset(s["topobathy"]))


def _source_id(group) -> str:
    return group[0].qid.split("/", 1)[0]
```

Change `sample_group` to also report low samples when asked:

```python
def sample_group(group, z, lon, lat, exact_offsets=False, lattice_lon=None, max_raw: float | None = None):
    """... Returns (h, valid), or (h, valid, low) with max_raw: low = valid and the source value <= max_raw."""
    ...
    low = np.zeros(lon.shape, bool) if max_raw is not None else None
    for e in group:
        ...
        h[take] = vals[0][take] + off[take]
        if low is not None:
            low |= take & (vals[0] <= max_raw)
        valid |= take
    return (h, valid) if low is None else (h, valid, low)
```

A helper that applies the mask to a list of per-group results:

```python
def _apply_tidal(groups, results, tidal: TidalMask | None, z: int, lon, lat) -> list:
    """results[i] = (h, valid[, low]); returns [(h, valid)] with tidal samples of masked groups made invalid."""
    if tidal is None:
        return [r[:2] for r in results]
    topo = np.zeros(lon.shape, bool)
    for g, r in zip(groups, results):
        if _source_id(g) in tidal.topobathy:
            topo |= r[1]
    out = []
    for g, r in zip(groups, results):
        h, valid = r[0], r[1]
        if len(r) == 3:
            cand = r[2] & topo
            if cand.any():
                target_m = spacing_deg(z) * M_PER_DEG
                water = class_codes(tidal.classes, lon[cand], lat[cand], target_m) == tidal.water_class
                idx = np.nonzero(cand)  # works for the 2-D tile grid and 1-D verify points
                valid = valid.copy()
                valid[tuple(i[water] for i in idx)] = False
        out.append((h, valid))
    return out
```

In `tile_grid(z, x, y, entries, tidal: TidalMask | None = None)`:

```python
    results = [
        sample_group(g, z, lon, lat, lattice_lon=ulon,
                     max_raw=tidal.max_raw_m if tidal and _source_id(g) in tidal.sources else None)
        for g in groups
    ]
    results = _apply_tidal(groups, results, tidal, z, lon, lat)
```

(the rest of `tile_grid` is unchanged). `build_tile(z, x, y, entries, tidal=None)` passes `tidal` to `tile_grid`.
`point_heights(z, lon, lat, entries, tidal=None)`: compute every group's result first (with `exact_offsets=True`
and the same `max_raw` rule), apply `_apply_tidal`, then first-valid-wins in priority order as today. Import
`class_codes` from `..water` and `M_PER_DEG` (already imported).

`context.py` (`WorkerState.__init__`, after `self.index = ...`):

```python
        self.tidal = None
        self.terrain_extra: list[str] = []
        ts = self.manifest.layers["terrain"].get("tidal_mask")
        if ts:
            classes, shas = class_rasters(self.manifest, ctx.asset_paths)
            if not classes:
                raise ContextError("terrain tidal_mask needs the package's WorldCover (land cover) assets")
            self.tidal = TidalMask.from_settings(ts, tuple(classes))
            self.terrain_extra = sorted(shas)
        if self.manifest.sea_level:
            self.terrain_extra.append(sha256_bytes(canonical_json(self.manifest.sea_level).encode()))
```

(import `TidalMask` from `.layers.terrain`, `canonical_json` from `.manifest`).

`pipeline._terrain_tile`:

```python
    inputs = inputs_hash(st.settings["terrain"], __version__, *st.terrain_extra, *sorted(e.sha256 for e in entries))
    return run_tile(..., partial(terrain.build_tile, z, x, y, entries, st.tidal))
```

With no mask and no `sea_level`, `terrain_extra` is empty and the inputs hash is exactly today's.

`verify._deep`: pass `st.tidal` to both `terrain.tile_grid(z, x, y, entries, st.tidal)` and
`terrain.point_heights(z, lon, lat, entries, st.tidal)`.

- [ ] **Step 4: Run the suite**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests -q` → all pass.

- [ ] **Step 5: Commit**

```bash
git add scripts/scene
git commit -m "feat(scene): tidal-water mask drops 3DEP's flattened plate where topobathy covers it"
```

---

### Task 5: `sea_level.json` (build) and its verify check

**Files:**
- Modify: `scripts/scene/camsim_scene/sea_level.py`, `scripts/scene/camsim_scene/pipeline.py` (`build_scene`), `scripts/scene/camsim_scene/verify.py`
- Test: `scripts/scene/tests/test_sea_level.py`, `scripts/scene/tests/test_verify.py`

**Interfaces:**
- Consumes: `Manifest.sea_level`, `BuildContext.grid_paths` with GEOID18 and EGM96 (Task 3).
- Produces: `sea_level.compute(section: dict, grid_paths: dict[str, Path]) -> dict` →
  `{"offset_m", "station", "name", "msl_above_navd88_m", "navd88_ellipsoid_m", "egm96_n_m"}` (floats rounded to 4 decimals);
  `sea_level.write(pkg: Path, m: Manifest, grid_paths) -> dict | None`; `<pkg>/sea_level.json` (canonical JSON, hashed);
  verify check `"sea_level"`.

- [ ] **Step 1: Write the failing tests**

```python
from pathlib import Path

import numpy as np
from rasters import write_geotiff

GEOID18 = Path(__file__).parent / "fixtures" / "grids" / "us_noaa_g2018u0.tif"


def egm96_grid(tmp_path, value=-35.2):
    # a constant fake EGM96 grid (15' nodes) around La Jolla
    write_geotiff(tmp_path / "egm96.tif", np.full((40, 40), value, np.float32), -122.0, 37.0, 0.25)
    return tmp_path / "egm96.tif"


def test_compute_offset_at_la_jolla(tmp_path):
    s = sea_level.station_section(http(), "9410230")
    grids = {"us_noaa_g2018u0.tif": GEOID18, sea_level.EGM96_GRID: egm96_grid(tmp_path)}
    r = sea_level.compute(s, grids)
    # NAVD88 zero at La Jolla is ~ -35.6 m ellipsoid height (GEOID18 + ITRF2014 Helmert)
    assert -36.5 < r["navd88_ellipsoid_m"] < -34.5
    assert r["egm96_n_m"] == pytest.approx(-35.2, abs=1e-6)
    assert r["offset_m"] == pytest.approx(r["navd88_ellipsoid_m"] + 0.774 + 35.2, abs=1e-4)
```

In `test_verify.py`, extend the existing deep-verify fixture package (or a new minimal one from
`synthetic_scene` with `sea_level` set and a `coops_http`) so that: verify passes; editing
`sea_level.json`'s `offset_m` by +0.01 makes the `sea_level` check fail; deleting the file makes it fail.

```python
def test_verify_checks_sea_level(tmp_path):
    pkg, cache = built_package_with_sea_level(tmp_path)  # helper: synthetic scene + [sea_level], plan/fetch/build
    assert check(verify(pkg, cache, deep=True), "sea_level").ok
    p = pkg / "sea_level.json"
    d = json.loads(p.read_text())
    d["offset_m"] += 0.01
    p.write_text(json.dumps(d))
    assert not check(verify(pkg, cache, deep=True), "sea_level").ok
```

(The hashes check also fails after the edit; that is fine — the test asserts the `sea_level` check itself.) The
helper needs the EGM96 and GEOID18 grids served to the fake cache: copy the fixture GEOID18 and the fake EGM96
into the stub cache the way `test_pipeline.py`'s existing datum tests provide `us_noaa_g2018u0.tif`.

- [ ] **Step 2: Run them to see them fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_sea_level.py scripts/scene/tests/test_verify.py -q` → FAIL.

- [ ] **Step 3: Implement**

`sea_level.py`:

```python
def compute(section: dict, grid_paths: dict) -> dict:
    """offset_m = h_ell(NAVD88 0) + (MSL - NAVD88) - N_EGM96 at the station (PROJ network off, pinned grids).
    N_EGM96 is bilinear on NGA's 15' grid, as CamSim's Geospatial/Geoid.cpp reads WW15MGH.DAC."""
    import numpy as np

    from .datum import DatumTransform, manifest_section
    from .sources.base import SourceRaster

    lon, lat = np.array([section["lon"]]), np.array([section["lat"]])
    entry = manifest_section([section["geoid"]])["datums"][section["geoid"]]
    dt = DatumTransform(entry, {k: Path(v) for k, v in grid_paths.items()})
    h0 = float(dt.vertical_offset(*dt.to_source_geographic(lon, lat))[0])
    egm = SourceRaster(path=Path(grid_paths[EGM96_GRID]), datum="wgs84", clamp_edges=True)
    vals, ok = egm.sample(lon, lat, 0.0)
    if not ok[0]:
        raise SeaLevelError(f"EGM96 grid has no value at {section['lat']}, {section['lon']}")
    n = float(vals[0][0])
    return {
        "station": section["station"],
        "name": section.get("name", ""),
        "msl_above_navd88_m": section["msl_above_navd88_m"],
        "navd88_ellipsoid_m": round(h0, 4),
        "egm96_n_m": round(n, 4),
        "offset_m": round(h0 + section["msl_above_navd88_m"] - n, 4),
    }


def write(pkg: Path, m, grid_paths: dict) -> dict | None:
    """Write <pkg>/sea_level.json when the manifest has a station; remove a stale one otherwise."""
    from .fsutil import atomic_write
    from .manifest import canonical_json

    p = Path(pkg) / FILE
    if not m.sea_level:
        p.unlink(missing_ok=True)
        return None
    r = compute(m.sea_level, grid_paths)
    atomic_write(p, canonical_json(r).encode())
    return r
```

Add `from pathlib import Path` at the top. Check `SourceRaster.sample`'s `target_m=0.0` path selects full
resolution (it does via `choose_overview` returning `None`).

`pipeline.build_scene`: immediately before `m.hashes_sha256 = write_hashes(...)` (line ~681):

```python
        sea = sea_level.write(pkg, m, {k: Path(v) for k, v in ctx.grid_paths.items()})
        if sea:
            log.info("sea level: EGM96 %+.3f m (NOAA %s %s)", sea["offset_m"], sea["station"], sea["name"])
```

`verify.py`: a non-deep check that `sea_level.json` exists and parses whenever `m.sea_level` is set (and does not
exist otherwise), and in `_deep` (which has the context) a recompute:

```python
def _check_sea_level(pkg: Path, m: Manifest, grid_paths: dict | None) -> Check:
    p = pkg / sea_level.FILE
    if not m.sea_level:
        return Check("sea_level", not p.exists(), "no station" if not p.exists() else "sea_level.json without a station")
    if not p.exists():
        return Check("sea_level", False, "sea_level.json missing")
    have = json.loads(p.read_text(encoding="utf-8"))
    if grid_paths is None:
        return Check("sea_level", isinstance(have.get("offset_m"), (int, float)), f"offset {have.get('offset_m')} m")
    want = sea_level.compute(m.sea_level, grid_paths)
    ok = abs(have["offset_m"] - want["offset_m"]) <= 0.001
    return Check("sea_level", ok, f"offset {have['offset_m']} m (recomputed {want['offset_m']} m)")
```

Match the existing `Check(name, ok, detail)` constructor and `_guard` pattern exactly (read `verify.py`'s
`Check` dataclass first). Non-deep: `_guard("sea_level", _check_sea_level, pkg, m, None)`; deep: replace it with
the recompute using `{k: Path(v) for k, v in st.ctx.grid_paths.items()}`.

- [ ] **Step 4: Run the suite**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests -q` → all pass.

- [ ] **Step 5: Commit**

```bash
git add scripts/scene
git commit -m "feat(scene): sea_level.json (local MSL - EGM96) written by build and checked by verify"
```

---

### Task 6: Pendleton scene file and scene-package docs

**Files:**
- Modify: `scripts/scene/examples/pendleton.toml`, `docs/scene-packages.md`, `scripts/scene/README.md` (if it lists sources)

**Interfaces:**
- Consumes: everything in Tasks 1-5.

- [ ] **Step 1: Scene file**

Append to `scripts/scene/examples/pendleton.toml`:

```toml

[sea_level]
station = "9410230"  # NOAA CO-OPS La Jolla: MSL +0.774 m NAVD88 (1983-2001); Oceanside Harbor has no NAVD88 tie
```

and update its first comment line to say it also carries the R1 seabed and sea level.

- [ ] **Step 2: Live plan smoke test**

Run: `uv run --project scripts/scene camsim-scene plan /private/tmp/r1c-plan-smoke --config scripts/scene/examples/pendleton.toml`
(scratch directory; delete it afterwards).
Expected: a line `plan pendleton: … tiles terrain 182602 …`. **Stop and report** if the terrain tile count differs
from 182,602 (the spec says the tile set is unchanged). Check `manifest.json` has `sea_level.msl_above_navd88_m`
0.774, `layers.terrain.tidal_mask`, and `noaa_crm_socal`'s asset carries `msl_above_navd88_m`.

- [ ] **Step 3: Docs** (`docs/scene-packages.md`)

- "Sources": add both NOAA products (URLs, sizes, extents, datums, max zoom, the CRM's 1 m accuracy and MSL → NAVD88
  shift, "not for navigation", the CUDEM gap).
- New section "Seabed (R1)" after "NDVI (R1)": the 0 NAVD88 plate finding (numbers from the spec's "Why"), the
  mask rule (three conditions, threshold +1.5 m and why), priorities, the feather, and that older manifests build as
  before.
- New section "Sea level (R1)": `[sea_level] station`, the CO-OPS snapshot in the manifest, the offset formula,
  `sea_level.json`, what CamSim does with it (link `docs/configuration.md`), KLV unchanged, and the limit (one
  constant per package; sea-surface topography varies over 100 km by centimetres to a decimetre).
- `scene.toml` reference: the `[sea_level]` table; default priorities.
- "Imagery edge" known limits: mark the turquoise-shelf bullet as explained (the plate) and fixed by the seabed
  (results in Task 11).

- [ ] **Step 4: Commit**

```bash
git add scripts/scene/examples/pendleton.toml docs/scene-packages.md scripts/scene/README.md
git commit -m "docs(scene): seabed and sea level; Pendleton uses NOAA La Jolla"
```

---

### Task 7: CamSim reads `sea_level.json` and offsets the sea

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.h` (`FSceneConfig`)
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Config/ScenePackage.cpp` (`ResolvePackage`), `Config/ScenePackage.h` (doc comment)
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Ocean/OceanSurface.h`, `Ocean/OceanSurface.cpp`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.cpp` (ocean creation, ~line 753)
- Test: `Tests/ScenePackageTest.cpp`, `Tests/OceanSurfaceTest.cpp`
- Docs: `docs/configuration.md` ("Scene Package"), `CLAUDE.md` (Ocean gotcha)

**Interfaces:**
- Produces: `FCamSimConfig::FSceneConfig::SeaLevelOffsetM` (double, 0 by default), `SeaLevelStation` (FString);
  `FOceanSurface::SetDatumOffsetM(double)`, `GetDatumOffsetM() const`; `SeaLevelM = Geoid + DatumOffsetM + TideOffsetM`.

- [ ] **Step 1: Write the failing tests**

`ScenePackageTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenePackageSeaLevelTest,
	"CamSim.Scene.Package.SeaLevel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FScenePackageSeaLevelTest::RunTest(const FString& Parameters)
{
	{
		const FString D = MakePackage(TEXT("SeaAbsent"), true, true, false);
		const FCamSimConfig Cfg = ResolvedConfigFor(D);
		TestEqual(TEXT("absent: no offset"), Cfg.Scene.SeaLevelOffsetM, 0.0);
		TestEqual(TEXT("absent: no errors"), Cfg.Scene.ResolveErrors.Num(), 0);
	}
	{
		const FString D = MakePackage(TEXT("SeaPresent"), true, true, false);
		WriteFile(D / TEXT("sea_level.json"), TEXT("{\"offset_m\": 0.3912, \"station\": \"9410230\"}"));
		const FCamSimConfig Cfg = ResolvedConfigFor(D);
		TestEqual(TEXT("present: offset"), Cfg.Scene.SeaLevelOffsetM, 0.3912, 1e-12);
		TestEqual(TEXT("present: station"), Cfg.Scene.SeaLevelStation, FString(TEXT("9410230")));
		TestEqual(TEXT("present: no errors"), Cfg.Scene.ResolveErrors.Num(), 0);
	}
	const TCHAR* Bad[] = {
		TEXT("{\"offset_m\": 3.5}"), TEXT("{\"offset_m\": -4}"), TEXT("{\"offset_m\": \"0.4\"}"),
		TEXT("{\"station\": \"9410230\"}"), TEXT("not json")};
	int32 I = 0;
	for (const TCHAR* Text : Bad)
	{
		const FString D = MakePackage(*FString::Printf(TEXT("SeaBad%d"), I++), true, true, false);
		WriteFile(D / TEXT("sea_level.json"), Text);
		const FCamSimConfig Cfg = ResolvedConfigFor(D);
		TestTrue(FString::Printf(TEXT("bad '%s' is a resolve error"), Text), Cfg.Scene.ResolveErrors.Num() > 0);
		TestEqual(FString::Printf(TEXT("bad '%s' leaves the offset at 0"), Text), Cfg.Scene.SeaLevelOffsetM, 0.0);
	}
	return true;
}
```

`OceanSurfaceTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanSurfaceDatumOffsetTest, "CamSim.Ocean.Surface.DatumOffsetAddsUnderTide",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanSurfaceDatumOffsetTest::RunTest(const FString& Parameters)
{
	FOceanSurface S = MakeSurface(-35.0);
	S.SetAnchor(33.2, -117.4);
	S.SetDatumOffsetM(0.39);
	TestEqual(TEXT("geoid + datum"), S.SeaLevelM(33.2, -117.4).Get(0.0), -34.61, 1e-12);
	S.SetTideOffsetM(-0.5);
	TestEqual(TEXT("geoid + datum + tide"), S.SeaLevelM(33.2, -117.4).Get(0.0), -35.11, 1e-12);
	TestEqual(TEXT("calm surface follows"), S.SurfaceHeightM(33.2, -117.4).Get(0.0), -35.11, 1e-12);
	S.SetDatumOffsetM(std::numeric_limits<double>::quiet_NaN());
	TestEqual(TEXT("non-finite datum offset -> 0"), S.GetDatumOffsetM(), 0.0);
	return true;
}
```

- [ ] **Step 2: Build and run them to see them fail**

Build: `scripts/run.sh --build-only` → compile error is the expected failure (no `SeaLevelOffsetM`, no `SetDatumOffsetM`).

- [ ] **Step 3: Implement**

`CamSimConfig.h`, in `FSceneConfig` after `PackageName`:

```cpp
		// From <dir>/sea_level.json (REALISM R1): local MSL - EGM96 at the package's NOAA tide station, added to
		// the ocean's sea level under the CIGI tide. 0 without the file.
		double SeaLevelOffsetM = 0.0;
		FString SeaLevelStation;
```

`ScenePackage.cpp`, at the end of `ResolvePackage` (after land cover):

```cpp
	const FString SeaFile = Dir / TEXT("sea_level.json");
	if (FPaths::FileExists(SeaFile))
	{
		FString SeaText;
		TSharedPtr<FJsonObject> Sea;
		double Offset = 0.0;
		if (!FFileHelper::LoadFileToString(SeaText, *SeaFile)
			|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(SeaText), Sea) || !Sea.IsValid())
		{
			Scene.ResolveErrors.Add(FString::Printf(TEXT("scene.dir '%s': sea_level.json is not valid JSON"), *Dir));
		}
		else if (!Sea->HasTypedField<EJson::Number>(TEXT("offset_m"))
			|| !Sea->TryGetNumberField(TEXT("offset_m"), Offset) || !FMath::IsFinite(Offset)
			|| FMath::Abs(Offset) > MaxSeaLevelOffsetM)
		{
			Scene.ResolveErrors.Add(FString::Printf(
				TEXT("scene.dir '%s': sea_level.json offset_m must be a number within +-%.0f m"), *Dir, MaxSeaLevelOffsetM));
		}
		else
		{
			Scene.SeaLevelOffsetM = Offset;
			Sea->TryGetStringField(TEXT("station"), Scene.SeaLevelStation);
		}
	}
```

with `constexpr double MaxSeaLevelOffsetM = 3.0;` declared in `ScenePackage.h` next to `SupportedSchemaVersion`
(doc comment: "Largest |sea_level.json offset_m| accepted (local MSL - EGM96 is within ~2 m on every US coast)").
Mention `sea_level.json` in `ResolvePackage`'s doc comment.

`OceanSurface.h`: next to the tide accessors,

```cpp
	/** Local MSL - EGM96 from the scene package (REALISM R1); added under the tide. Non-finite -> 0. */
	void   SetDatumOffsetM(double M) { DatumOffsetM = FMath::IsFinite(M) ? M : 0.0; }
	double GetDatumOffsetM() const { return DatumOffsetM; }
```

member `double DatumOffsetM = 0.0;` next to `TideOffsetM`, and update the class comment to "EGM96 sea level
(+ scene-package datum offset + CIGI tide offset)". `OceanSurface.cpp` `SeaLevelM`: `return *G + DatumOffsetM + TideOffsetM;`.

`CamSimSubsystem.cpp`, after `Impl->Ocean = MakeUnique<FOceanSurface>();`:

```cpp
			Impl->Ocean->SetDatumOffsetM(Config.Scene.SeaLevelOffsetM);
			if (Config.Scene.SeaLevelOffsetM != 0.0)
			{
				UE_LOG(LogCamSim, Log, TEXT("Scene package sea level: EGM96 %+.3f m (NOAA %s)"),
					Config.Scene.SeaLevelOffsetM, *Config.Scene.SeaLevelStation);
			}
```

Audit (no code change expected): `Entity/SurfaceProbe.cpp:63` passes the raw geoid only as `ClampWater`'s
fallback `SeaLevelM`, which `SurfaceClamp.cpp:121` ignores whenever `Water.Ocean` is set (the ocean's `SeaLevelM`
wins). Confirm by reading both; if `Water.Ocean` can be null while the ocean exists, route the fallback through
`Water.Ocean->SeaLevelM` instead. `KlvBuilder.cpp` stays EGM96.

- [ ] **Step 4: Build and run the tests**

```bash
scripts/run.sh --build-only
UE_BIN="/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"
"$UE_BIN" "$PWD/unreal_project/CamSimTest/CamSimTest.uproject" \
  -ExecCmds="Automation RunTests CamSim+Quit" -TestExit="Automation Test Queue Empty" \
  -ReportExportPath="$PWD/.cache/automation-report" \
  -unattended -nullrhi -nosound -nosplash -DisablePython -log -stdout -FullStdOutLogOutput > .cache/automation.log 2>&1
python3 -c "import json;d=json.load(open('.cache/automation-report/index.json',encoding='utf-8-sig'));print(d['succeeded'],d['failed'],d['notRun'])"
```

Expected: `failed` 0; `CamSim.Scene.Package.SeaLevel` and `CamSim.Ocean.Surface.DatumOffsetAddsUnderTide` succeeded
(grep `index.json` for both names). Report the totals.

- [ ] **Step 5: Docs**

- `docs/configuration.md`, "Scene Package": a paragraph on `sea_level.json` (what it holds, that the sea is
  EGM96 + `offset_m` + CIGI tide, the ±3 m bound, absent = 0, KLV unchanged).
- `CLAUDE.md`, Ocean gotcha: "sea level = EGM96 geoid + CIGI tide" → "sea level = EGM96 geoid + scene package
  `sea_level.json` offset (local MSL, `FOceanSurface::SetDatumOffsetM`) + CIGI tide". Update the test count in
  `CLAUDE.md` ("487 tests across 82 files") to the new totals from Step 4.

- [ ] **Step 6: Commit**

```bash
git add unreal_project/CamSimTest/Source docs/configuration.md CLAUDE.md
git commit -m "feat(ocean): scene package sea_level.json offsets the sea to local MSL"
```

---

### Task 8: `coast_check.py` (gate 3)

**Files:**
- Create: `scripts/scene/tools/coast_check.py`, `scripts/scene/tests/test_coast_check.py`
- Modify: `scripts/scene/tools/README.md`

**Interfaces:**
- Consumes: package terrain (`qmesh.decode`, `tiling`), `sea_level.json`, WorldCover rasters via `context.class_rasters` / `water.class_codes`, the EGM96 grid (`unreal_project/CamSimTest/Content/NonUFS/Geoid/WW15MGH.DAC`, as CamSim reads it).
- Produces: CLI `coast_check.py PKG [--before PKG] [--out DIR]`, exit 0 = all gates pass; JSON report `coast_check.json`; pure functions `terrain_height(pkg, lon, lat) -> (z, h)`, `egm96(lat, lon) -> float`, `transect(lat0, lon0, bearing_deg, step_m, length_m) -> (lon[], lat[])`, `gate_offshore(rel, dist_land_m) -> dict`, `gate_steps(h, dist_edge_m) -> dict`, `waterline_offsets(...)`.

- [ ] **Step 1: Write the failing tests** (pure functions on synthetic arrays)

```python
import numpy as np
import pytest
import coast_check as cc  # tools/ is put on sys.path by the test (see test_water_check.py for the pattern)


def test_transect_steps_along_bearing():
    lon, lat = cc.transect(33.0, -117.0, 270.0, 10.0, 100.0)
    assert len(lon) == 11 and np.allclose(lat, 33.0) and lon[-1] < lon[0]


def test_offshore_gate():
    rel = np.array([-2.0] * 99 + [0.1])  # one sample of 100 within 0.5 m of the sea
    assert cc.gate_offshore(rel, np.full(100, 500.0))["pass"]  # 99 % rule
    assert not cc.gate_offshore(np.array([-0.2] * 10), np.full(10, 500.0))["pass"]
    assert cc.gate_offshore(np.array([0.0] * 10), np.full(10, 50.0))["n"] == 0  # < 100 m from land: not counted


def test_step_gate():
    h = np.array([-5.0, -5.5, -12.0, -13.0])  # a 6.5 m jump between adjacent 30 m samples
    assert not cc.gate_steps(h, np.array([100.0, 100.0, 100.0, 100.0]))["pass"]
    assert cc.gate_steps(h, np.array([5000.0] * 4))["pass"]  # far from the old 3DEP edge: not gated


def test_egm96_matches_camsim_grid_at_la_jolla():
    assert cc.egm96(32.8669, -117.2571) == pytest.approx(-35.4, abs=0.6)
```

- [ ] **Step 2: Run them to see them fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_coast_check.py -q` → FAIL.

- [ ] **Step 3: Implement `coast_check.py`**

Start from the 2026-10-10 probe (its decode/height/EGM96 logic is below) and add the gates:

```python
"""R1 coast gate (spec docs/superpowers/specs/2026-10-10-r1-coast-and-gates-design.md, Part C gate 3): package
terrain along transects across the Camp Pendleton coast vs the sea CamSim draws (EGM96 + sea_level.json).

    uv run --project scripts/scene python scripts/scene/tools/coast_check.py PKG [--before OLDPKG] [--out DIR]

Gates: (a) over WorldCover water >= 100 m from land, terrain <= sea - 0.5 m for >= 99 % of samples; (b) no step
> 3 m between adjacent 30 m samples within 1 km of the old 3DEP coverage edge; (c) median distance between the
terrain / local-MSL crossing and the WorldCover water edge <= 30 m; (d) sea_level.json offset_m equals an
independent pyproj computation within 1 cm. --before prints the same numbers for an older package."""
```

Implementation outline (write it all; ~250 lines):
- `REPO = Path(__file__).resolve().parents[3]`; EGM96 from `WW15MGH.DAC` (721×1440 big-endian int16 cm, rows 90N→90S,
  columns 0E→359.75E), bilinear exactly as the probe.
- `terrain_height(pkg, lon, lat)`: deepest existing tile `terrain/{z}/{x}/{y}.terrain` from z17 down, decode with
  `camsim_scene.qmesh.decode`, barycentric interpolation in the containing triangle (LRU cache of decoded tiles,
  `functools.lru_cache(maxsize=256)` on `(pkg, z, x, y)`).
- `TRANSECTS`: 12 bbox starts spread along the coast from San Onofre to Oceanside, each ~300 m inland at
  bearing 235° (approximately normal to the coast), plus 4 ring starts (Dana Point 33.47 −117.70, Laguna 33.54
  −117.79, Carlsbad 33.12 −117.33, Encinitas 33.04 −117.30), 10 m steps, 6 km long. Generate the 12 bbox starts by
  walking the WorldCover water edge: for lat in `np.linspace(33.20, 33.40, 12)`, march west from −117.30 at 10 m
  until WorldCover reads 80, step back 300 m.
- WorldCover lookup: open the cached WorldCover COGs named in `PKG/manifest.json` through the fetch cache
  (`camsim_scene.cache.Cache` + `context.class_rasters(m, paths)` where `paths` come from
  `pipeline.make_context(pkg, m, cache).asset_paths`), `water.class_codes(classes, lon, lat)`.
- `dist_land_m`: along each transect, distance (m) from each water sample back to the last non-water sample.
- Old-3DEP-edge distance: the `dep3_13` asset footprints in the manifest (`shapely.from_wkt`), distance from each
  sample to their union's boundary in metres (`* 111320`, latitude-scaled for longitude).
- `sea = egm96(lat, lon) + offset_m` (from `PKG/sea_level.json`, 0 if absent); `rel = h - sea`.
- `gate_offshore(rel, dist_land_m)`: `sel = dist_land_m >= 100`; `n = sel.sum()`; `frac_bad = mean(rel[sel] > -0.5)`;
  pass if `n > 0 and frac_bad <= 0.01`.
- `gate_steps(h, dist_edge_m)`: resample to 30 m (every 3rd 10 m sample), `d = abs(diff(h))` where both samples
  have `dist_edge_m <= 1000`; pass if `max(d) <= 3.0` (or nothing to gate).
- Waterline (c): per transect, first index where `rel` goes from ≥ 0 to < 0 (terrain crosses local MSL) vs first
  index where WorldCover becomes 80; distance = |Δindex| × 10 m; pass if the median over transects ≤ 30 m.
- (d): independent offset: `pyproj.Transformer.from_crs("EPSG:6318+5703", "EPSG:7912", always_xy=True)` with
  network enabled (a tool, as `hot_check.py` does) on (lon, lat, 0, 2010.0) at the station from
  `manifest.json` `sea_level`, plus `msl_above_navd88_m`, minus `egm96(station)`; pass if within 0.01 m of
  `sea_level.json`.
- Print a table per transect (crossing, offshore min/median/max rel, max step) and the four gate lines; write
  `coast_check.json` to `--out` (default `.cache/coast_check/`); with `--before`, also compute (a)-(c) for the old
  package and print them side by side. Exit 1 if any gate fails.

- [ ] **Step 4: Run tests, then the tool on the current package (expected to FAIL gate a)**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_coast_check.py -q` → PASS.
Run: `uv run --project scripts/scene python scripts/scene/tools/coast_check.py .cache/scene-packages/pendleton-ndvi`
Expected: gate (a) FAIL (the plate: rel ≈ −0.2…−1.1 m), (b) FAIL (the cliff), (d) "no sea_level.json". This is the
"before" baseline; keep its output for Task 12.

- [ ] **Step 5: README and commit**

Add a `coast_check.py` row to `scripts/scene/tools/README.md`.

```bash
git add scripts/scene/tools/coast_check.py scripts/scene/tools/README.md scripts/scene/tests/test_coast_check.py
git commit -m "feat(scene): coast_check gate tool (terrain vs local sea level across the coast)"
```

---

### Task 9: `hot_check.py` HAT/HOT + sea points; `render_check.py` coast shots and readiness timing

**Files:**
- Modify: `scripts/scene/tools/hot_check.py`, `scripts/scene/tools/render_check.py`, `scripts/scene/tools/camsim_session.py` (only if an env pass-through is missing), `scripts/scene/tools/README.md`

**Interfaces:**
- Consumes: `ocean_check.pack_hat_hot_request(request_id, lat, lon, alt=0.0, req_type=2)` and
  `ocean_check.send_packets(host, payload)` (import `ocean_check` from `scripts/`, already on `sys.path` via
  `camsim_session`); `check_cigi_responses.parse_responses(datagram)["hat_hot"]` → `[{"op": 103, "id", "valid", "hat", "hot"}]`.
- Produces: `hot_check.py` JSON gains `"hot": [...]` per land point and `"sea": [...]` records; `render_check.py`
  `result.json` gains per-shot `"ready_s"` and top-level `"gate_timeouts"`.

- [ ] **Step 1: hot_check HAT/HOT**

In `hot_check.py`:
- `SEA_POINTS = [(33.2100, -117.4300), (33.3000, -117.5000), (33.3700, -117.5900)]` (≥ 2 km offshore, inside the bbox).
- Env: add `CAMSIM_OCEAN_BEAUFORT="0"` to the session env (both modes) so HOT over sea is the flat sea.
- For each land point, after the frame-centre read: send `pack_hat_hot_request(rid, lat, lon, req_type=2)` via
  `send_packets(host, ...)` on the CIGI host socket, collect opcode 103 responses for `rid` from the response socket for
  up to 3 s (`parse_responses(d)["hat_hot"]`), and compare `hot` with `truth(lat, lon, ...)` (the request point, not
  the frame centre). Same limits (`LIMIT_M`). Record `{"pt", "hot", "truth", "src", "err_m", "pass"}`.
- For each sea point: same request; expected `egm96(lat, lon) + offset` where `offset` is read from
  `PKG/sea_level.json` (0 in `cwt` mode or when absent) and `egm96` is imported from `coast_check`; pass if
  `|hot - expected| <= 0.05`.
- Overall pass requires frame-centre, HOT and sea records all to pass. Print one line per record.

- [ ] **Step 2: render_check shots, readiness timing**

In `render_check.py`:
- Add shots:
  `"coast_low": P(lat=33.300, lon=-117.470, alt=300, yaw=225, gimbal_pitch=-15, fov_h=50)` (along the beach, out to
  sea) and `"coast_waves": P(lat=33.300, lon=-117.470, alt=300, yaw=225, gimbal_pitch=-15, fov_h=50)` with the
  ocean at Beaufort 5 for that shot (send CIGI Weather/Maritime the way `ocean_check.py` sets Beaufort over CIGI —
  reuse its packer; if only env control exists, run `coast_waves` in a second session with `CAMSIM_OCEAN_BEAUFORT=5`).
- Per shot: `t = time.time(); ok = rb.wait_terrain(90); res[name] = {"terrain_ready": ok, "ready_s": round(time.time() - t, 1)}`.
- After the session: `res["gate_timeouts"]` = log lines from `LOG` containing the terrain gate's timeout message
  (grep `Camera/` or `TerrainReadinessGate` source for its exact timeout log text first and match that string).
  Exit 1 if any timeout line or any `terrain_ready` false.

- [ ] **Step 3: Syntax check and commit**

Run: `uv run --project scripts/scene python -m py_compile scripts/scene/tools/hot_check.py scripts/scene/tools/render_check.py`
(no CamSim run here; the runs are Task 12). Update the tools README rows.

```bash
git add scripts/scene/tools
git commit -m "feat(scene): HAT/HOT and sea points in hot_check; coast shots and readiness timing in render_check"
```

---

### Task 10: Bench `--site pendleton` with a coastal pass

**Files:**
- Modify: `scripts/bench/scenario.py`, `scripts/bench/run_bench.py`, `scripts/bench/README.md`
- Test: `scripts/bench/snap_test.py` only if it builds phases (check); otherwise a quick import check

**Interfaces:**
- Produces: `scenario.Site(name, lat, lon, coast: bool)`; `scenario.SITES = {"sf": Site("sf", 37.7749, -122.4194, False), "pendleton": Site("pendleton", 33.30, -117.45, True)}`;
  `build_phases(smoke=False, site=SITES["sf"])`, `build_shots(smoke=False, site=SITES["sf"])`; `run_bench.py --site {sf,pendleton}` (default `sf`), recorded in the output JSON as `"site"`.

- [ ] **Step 1: Refactor without changing SF**

Replace the module constants' uses with the site: `_slew`, `_low_pass` and `_orbit` callers take `lat/lon` from
`site`; `FAR_LON = site.lon + 3.41`. Keep `BASE_LAT`/`BASE_LON` as the SF defaults for any external importer
(grep `scripts/` for `scenario.BASE_` and keep them working). For `site.coast`, append a measured phase:

```python
def _coast_pass(t: float) -> Pose:
    # 100 m/s at 300 m along the Pendleton coast (bearing 138 deg, SE), gimbal right (toward the sea), 20 deg down
    lat0, lon0, brg = 33.385, -117.585, math.radians(138.0)
    d = 100.0 * t
    lat = lat0 + d * math.cos(brg) / 111_320.0
    lon = lon0 + d * math.sin(brg) / (111_320.0 * math.cos(math.radians(lat0)))
    return Pose(lat, lon, 300.0, yaw=138.0, gimbal_yaw=90.0, gimbal_pitch=-20.0)
```

  `Phase("coast_pass", 90.0, _coast_pass)`, and include it in the warmup (one pass at 3× speed).
- Verify SF is unchanged: `uv run --with numpy --with pillow python -c "import sys; sys.path.insert(0,'scripts'); from bench import scenario as s; print([(p.name,p.duration_s) for p in s.build_phases()]); print(s.build_phases()[1].pose_at(10.0))"`
  before and after the refactor — identical output.

- [ ] **Step 2: `run_bench.py --site`**

`ap.add_argument("--site", choices=sorted(scenario.SITES), default="sf")`; pass `scenario.SITES[args.site]` to
`build_phases`/`build_shots`; store `"site": args.site` in the result JSON. `compare.py` should refuse to compare two
runs with different sites (one check with a clear message).

- [ ] **Step 3: Commit**

```bash
git add scripts/bench
git commit -m "feat(bench): --site pendleton with a coastal low pass"
```

---

### Task 11: Build `pendleton-r1c` (gates 1-2)  — controller-run, long

**Files:** none in git except docs at the end of Task 12. Packages live under `.cache/scene-packages/`.

- [ ] **Step 1: Clone and re-plan**

```bash
cp -cR .cache/scene-packages/pendleton-ndvi .cache/scene-packages/pendleton-r1c
uv run --project scripts/scene camsim-scene build .cache/scene-packages/pendleton-r1c \
  --config scripts/scene/examples/pendleton.toml --replan -j 6 2>&1 | tee .cache/scene-packages/pendleton-r1c-build.log
```

Run it with `set -o pipefail`. Expected: plan line terrain **182,602**; fetch downloads the two NOAA files (~1 GB),
converts them (`prepare`), and the EGM96 grid; terrain rebuilds all 182,602 tiles (inputs changed), imagery
277,286, land cover 2,288 and NDVI 7,043 all **skipped**; log line `sea level: EGM96 +0.3x m (NOAA 9410230 La Jolla)`.
Record wall time, terrain time, terrain size (`du -sh terrain`) vs `pendleton-ndvi`.
If anything other than terrain rebuilds, stop and investigate (the settings hashes for imagery/NDVI must not change).

- [ ] **Step 2: Verify**

```bash
uv run --project scripts/scene camsim-scene verify .cache/scene-packages/pendleton-r1c --deep -j 6
```

Expected: every check `ok`, including `sea_level`; record scene p50/p99/max and the per-zoom base numbers.

- [ ] **Step 3: Before/after compare**

Every non-`terrain/` line of `hashes.txt` equals `pendleton-ndvi`'s, except the new `sea_level.json` line:

```bash
diff <(grep -v ' terrain/' .cache/scene-packages/pendleton-ndvi/hashes.txt) \
     <(grep -v ' terrain/' .cache/scene-packages/pendleton-r1c/hashes.txt)
```

Expected: only `> … sea_level.json`.

---

### Task 12: Gates 3-7 on macOS — controller-run

- [ ] **Step 1: Coast (gate 3)**

`uv run --project scripts/scene python scripts/scene/tools/coast_check.py .cache/scene-packages/pendleton-r1c --before .cache/scene-packages/pendleton-ndvi`
Expected: (a)-(d) PASS. A failure here is a finding: diagnose (superpowers:systematic-debugging), do not loosen
the gate without the user.

- [ ] **Step 2: Heights (gate 4)**

```bash
cd scripts/scene/tools
uv run --project .. python hot_check.py package ../../../.cache/scene-packages/pendleton-r1c \
  --third <dep3_13 blob path from the manifest> --onem <dep3_1m D24 blob path>
```

(the R0 gate-5 run's `--third/--onem` arguments are in `docs/scene-packages.md`'s gate-5 notes or the shell
history; resolve the cached blobs via the manifest's asset sha256 under `.cache/scene/blobs/`). Expected: 7/7
frame-centre, 7/7 HOT, 3/3 sea PASS.

- [ ] **Step 3: Render, registration, offline, readiness (gates 5-7)**

```bash
uv run --project scripts/scene python scripts/scene/tools/render_check.py .cache/render/r1c package .cache/scene-packages/pendleton-r1c
uv run --project scripts/scene python scripts/scene/tools/render_check.py .cache/render/r1c-off package .cache/scene-packages/pendleton-r1c --offline
uv run --project scripts/scene python scripts/scene/tools/render_check.py .cache/render/cwt cwt
uv run --project scripts/scene python scripts/scene/tools/registration.py .cache/render/r1c .cache/render/cwt
```

Expected: every shot `terrain_ready` with no gate timeout (record `ready_s` per shot and the cold start); offline
run logs no outbound request; registration < 1 px on the land EO shots (`nadir_2km`, `slant_ne`, `rivermouth`,
`interior_nadir`, `high_oblique`, `bbox_edge`); check `registration.py`'s CLI first and match it.

- [ ] **Step 4: Visual review (user)**

Publish the before/after pairs (`pendleton-ndvi` render from chunk 2 under `.cache/render/` if present, else render
it now with `render_check.py`) for `sea_offshore`, `bbox_edge`, `rivermouth`, `ring_edge`, `coast_low`,
`coast_waves` side by side for the user. **Wait for the user's verdict** before marking gate 6.

---

### Task 13: Bench (gate 8) — controller-run

- [ ] **Step 1: Runs**

Two runs each, 720p, same session, nothing else running:

```bash
for label in cwt-a cwt-b; do uv run --with numpy --with pillow python scripts/bench/run_bench.py --site pendleton --label r1-$label; done
for pkg in pendleton-ndvi pendleton-r1c; do for r in a b; do
  uv run --with numpy --with pillow python scripts/bench/run_bench.py --site pendleton --label r1-$pkg-$r \
    --env CAMSIM_SCENE_DIR=$PWD/.cache/scene-packages/$pkg; done; done
```

(check `run_bench.py --help` for the 720p flag/config; the macOS baselines are 720p).

- [ ] **Step 2: Compare**

`python scripts/bench/compare.py` pairwise; compute per phase game-thread p50/p95: spread = |a − b| within each
config; pass if `pendleton-r1c` ≤ reference × 1.05 or within the reference's spread, against both CWT and
`pendleton-ndvi`, in every phase including `coast_pass`; 30 fps and 0 drops everywhere. Record the table.

---

### Task 14: Results and roadmap

**Files:** `docs/scene-packages.md` (Measured: new "R1 completion" table), `REALISM.md` (status paragraph, R1
bullets: seabed and sea level done; gates done on macOS; open: Linux `file://`, `--network none`, SquashFS timing),
`docs/scene-packages.md` imagery-edge known limit (shelf fixed, with the coast_check numbers).

- [ ] **Step 1: Write the tables** with the numbers from Tasks 11-13 (every gate row: what, tool, result).
- [ ] **Step 2: Commit**

```bash
git add docs/scene-packages.md REALISM.md
git commit -m "docs(realism): R1 completed on macOS (seabed, local sea level, gates); Linux items open"
```
