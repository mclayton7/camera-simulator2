# NDVI Layer and NAIP Sun Metadata Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Package an NDVI layer (NAIP, fitted onto Sentinel-2's scale, Sentinel-2 behind it) next to a scene
package's imagery, and record each NAIP quarter-quad's acquisition date and sun-position bounds in the manifest.

**Architecture:** Scene tooling only (`scripts/scene`, the `camsim-scene` uv project); no CamSim runtime change.
Config gains `ndvi` / `ndvi_max_zoom` and a `layers.ndvi` manifest section with its own settings hash. Adapters
expose a (red, NIR) raster (`ndvi_bands`); a new `layers/ndvi.py` builds NDVI leaves by reusing the imagery leaf's
sampling, feather and water-distance helpers, and parents by a nodata-aware 2 × 2 mean. A new `ndvi_fit.py` fits
NAIP → Sentinel-2 once per build (`ndvi/fit.json`, like `imagery/balance.json`). The engine learns tiles that
produce no file (all nodata). `sun.py` computes solar-noon elevation/azimuth and the azimuth window at 30°
elevation for `naip_pc` assets at `plan`. `verify --deep` gains `ndvi_values`; `tools/ndvi_check.py` computes the
plausibility and seam gates. Task 8 is the Pendleton acceptance.

**Tech Stack:** Python 3.12–3.13, numpy, scipy, shapely 2, rasterio, Pillow, pytest. No new dependencies.

**Spec:** `docs/superpowers/specs/2026-10-09-ndvi-sun-metadata-design.md`

## Global Constraints

- All commands run from the repo root. Tests: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests -q`
  (253 pass + 1 skipped before this work; no network).
- Lint: ruff, line length 120 (`scripts/scene/pyproject.toml`): `uv run --project scripts/scene --with ruff ruff check scripts/scene`.
  Match the package's style: module docstrings, terse comments, `from __future__ import annotations`.
- `ndvi`: bool; default true in `sim`, false in `preview`. `ndvi_max_zoom`: int, default 15, range [10, 17].
- `ndvi = true` set explicitly with neither `naip_pc` nor `wc_s2` in `priorities.imagery`: `ConfigError`. Not set
  (profile default) with neither: the layer is silently off (no `layers.ndvi`).
- Encoding (copy exactly): 8-bit grayscale PNG, 256 px; code 0 = nodata; `code = 1 + floor((clip(ndvi, -1, 1) + 1) * 127 + 0.5)`;
  `ndvi = (code - 1) / 127 - 1`. NDVI = `(NIR - red) / (NIR + red)` from raw values; NaN where both are 0.
- Bands (verified from the files, 2026-10-09): NAIP COG bands R, G, B, NIR → `ndvi_bands = (1, 4)`; WorldCover S2
  composite "Band 1: B04 (Red) … Band 4: B08 (Infrared)" → `ndvi_bands = (1, 4)`.
- Merge (copy exactly): NAIP (`reference`) first, through the fit; Sentinel-2 behind it; NAIP feathered over
  200 m (`feather_m`) inward from its valid-data edge; with WorldCover and `naip_water_buffer_m` > 0, NAIP used only
  within the buffer of land; where nothing is behind NAIP, NAIP is kept. No colour-match-style fade over water.
- Fit (copy exactly): least squares `s2 ≈ gain · naip + offset` on the `balance.lattice_blocks` lattice at
  `fit_step_m` 10 m, WorldCover classes 0 and 80 excluded, even (i + j) nodes fit, odd held out, ≥ 500 fit samples
  (`min_samples`); stored as integers `gain_e6`, `offset_e6` (millionths) with `format: 1`; applied then clamped to
  [−1, 1]. Report (to `build.json` `ndvi` and the log, never into `fit.json`): samples, gain, offset, held-out MAE and
  median bias before/after, median residual bias per NAIP acquisition date.
- Tile set: the imagery plan's tiles at z ≤ min(`ndvi_max_zoom`, imagery depth) overlapping (ring ∩ union of the
  NDVI sources' footprints). Tiles with no valid pixel produce no file (marker output `""`).
- Sun metadata on every `naip_pc` asset at `plan`: `acquired` (`YYYY-MM-DD`), `sun_noon`
  (`elevation_deg`, `azimuth_deg`), `sun_window` (`min_elevation_deg` 30.0, `azimuth_deg` [morning, afternoon]; absent
  when the noon sun is below 30°), all at the STAC bbox centre, rounded to 0.01°. `wc_s2` assets: `composite: "2021"`.
  Asset metadata never feeds a tile's inputs hash.
- `ndvi = false`: no `ndvi/` directory, no `.state/ndvi*`; every other file identical to an `ndvi = true` build of
  the same scene (and so to the code before this plan: imagery, terrain and land cover paths don't change).
- Same inputs → byte-identical `fit.json` and NDVI tiles, including `-j 1` vs `-j 2`.
- Commits end with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01EuqoXt1sAk5mjhhppnKP9D
  ```

## Review Focus

- **Imagery bytes must not change** when `sample_entries` is generalised to N bands. Test: the existing legacy-leaf
  tests (`test_leaf_without_balance_matches_the_legacy_algorithm`, `test_balance_off_and_no_margin_reproduce_legacy_tiles`)
  plus Task 5 `test_ndvi_off_gives_the_same_package_without_the_layer`.
- **An existing package re-planned with the layer** must rebuild only NDVI (terrain/imagery skip: their settings
  hashes don't include `layers.ndvi` or asset metadata). Test: Task 5 `test_adding_ndvi_rebuilds_no_other_tile`.
- **Toggling `ndvi` off** must remove `ndvi/`, its markers and its fit marker, and drop them from `hashes.txt`. Test:
  Task 5 `test_turning_ndvi_off_removes_the_layer`.
- **A parent whose children are partly outside the NDVI area** (unplanned, no marker) must build from the children
  that exist instead of failing the build. Test: Task 5 `test_ndvi_layer_is_written_hashed_and_fitted` (the ring's
  coarse tiles straddle the Sentinel-2 box) and Task 3 `test_parent_means_the_valid_pixels_of_each_block`.
- **An all-nodata tile left from an earlier build** (data source removed) must be deleted, not kept. Test: Task 5
  engine test `test_a_producer_returning_none_writes_no_file_and_is_skipped_next_time` (the stale-file case).
- **A Sentinel-2-only NDVI layer** (`preview` with `ndvi = true`: no reference) must build with no fit and no crash.
  Test: Task 5 `test_sentinel2_only_ndvi_builds_without_a_fit`.

---

## File Structure

| File | Responsibility |
|---|---|
| `scripts/scene/camsim_scene/config.py` (modify) | `ndvi`, `ndvi_max_zoom` keys; `NDVI` constants; `ndvi_settings()`; `layers.ndvi` |
| `scripts/scene/camsim_scene/sun.py` (create) | Solar noon elevation/azimuth and the azimuth window at a minimum elevation (NOAA equations) |
| `scripts/scene/camsim_scene/sources/base.py` (modify) | `ndvi_bands` attribute and `ndvi_raster()` on adapters |
| `scripts/scene/camsim_scene/sources/naip_pc.py`, `wc_s2.py` (modify) | NIR band; NAIP `acquired` / `sun_noon` / `sun_window`; S2 `composite` |
| `scripts/scene/camsim_scene/layers/imagery.py` (modify) | `sample_entries` returns as many bands as the rasters have |
| `scripts/scene/camsim_scene/layers/ndvi.py` (create) | NDVI encoding, PNG I/O, leaf (merge/feather/water clip), parent |
| `scripts/scene/camsim_scene/ndvi_fit.py` (create) | `NdviFit` (JSON, apply) and `fit_ndvi` |
| `scripts/scene/camsim_scene/context.py` (modify) | `index["ndvi"]`, NDVI fit / interior / water in `WorkerState` |
| `scripts/scene/camsim_scene/engine.py` (modify) | Tiles that produce no file (`EMPTY`); `ndvi/*.png` markers |
| `scripts/scene/camsim_scene/tiling.py`, `tms.py` (modify) | `ndvi_plan()`; PNG `tilemapresource.xml` |
| `scripts/scene/camsim_scene/pipeline.py` (modify) | `_fit_ndvi`, NDVI tile jobs, `_build_ndvi`, `build.json` `ndvi` |
| `scripts/scene/camsim_scene/verify.py` (modify) | `ndvi_tilemapresource`, deep `ndvi_values` |
| `scripts/scene/tools/ndvi_check.py` (create) | Acceptance gates 3–4 and a false-colour overview |
| `scripts/scene/tests/fake_sources.py` (modify) | 4-band synthetic NAIP/S2 with known NDVI (`ndvi=True`) |
| `scripts/scene/tests/test_sun.py`, `test_ndvi.py`, `test_ndvi_fit.py`, `test_ndvi_build.py`, `test_ndvi_check.py` (create) | Tests |
| `docs/scene-packages.md`, `scripts/scene/tools/README.md`, `REALISM.md`, `ROADMAP.md` (modify) | Docs and status |

---

### Task 1: Config — `ndvi`, `ndvi_max_zoom`, `layers.ndvi`

**Files:**
- Modify: `scripts/scene/camsim_scene/config.py`
- Test: `scripts/scene/tests/test_config.py`

**Interfaces:**
- Produces: `config.NDVI` (dict: `feather_m` 200.0, `fit_step_m` 10.0, `min_samples` 500, `exclude_classes` [0, 80]),
  `config.NDVI_REFERENCE = "naip_pc"`, `config.NDVI_TARGET = "wc_s2"`, `config.NDVI_MAX_ZOOM = 15`,
  `ScenePlan.ndvi: bool`, `ScenePlan.ndvi_max_zoom: int`, `config.ndvi_settings(plan) -> dict | None` with keys
  `priorities` (list), `reference` (str | None), `target` (str | None), `max_zoom`, `tile_px`, `format`, `encoding`,
  `feather_m`, `fit_step_m`, `min_samples`, `exclude_classes`, `naip_water_buffer_m`. `layer_settings(plan)["ndvi"]`
  exists only when `ndvi_settings` is not None.

- [ ] **Step 1: Write the failing tests** (append to `scripts/scene/tests/test_config.py`)

```python
def test_ndvi_defaults_per_profile():
    sim = parse_scene({"name": "t", "bbox": PENDLETON})
    assert sim.ndvi is True and sim.ndvi_max_zoom == 15
    s = config.layer_settings(sim)["ndvi"]
    assert s["priorities"] == ["naip_pc", "wc_s2"] and s["reference"] == "naip_pc" and s["target"] == "wc_s2"
    assert s["max_zoom"] == 15 and s["feather_m"] == 200.0 and s["naip_water_buffer_m"] == 200.0
    assert s["fit_step_m"] == 10.0 and s["min_samples"] == 500 and s["exclude_classes"] == [0, 80]
    prev = parse_scene({"name": "t", "bbox": PENDLETON, "profile": "preview"})
    assert prev.ndvi is False and "ndvi" not in config.layer_settings(prev)


def test_ndvi_off_leaves_the_other_layers_unchanged():
    on, off = (config.layer_settings(parse_scene({"name": "t", "bbox": PENDLETON, "ndvi": v})) for v in (True, False))
    assert "ndvi" not in off and {k: v for k, v in on.items() if k != "ndvi"} == off


def test_preview_can_turn_ndvi_on_with_sentinel2_only():
    plan = parse_scene({"name": "t", "bbox": PENDLETON, "profile": "preview", "ndvi": True, "ndvi_max_zoom": 12})
    s = config.layer_settings(plan)["ndvi"]
    assert s["priorities"] == ["wc_s2"] and s["reference"] is None and s["target"] == "wc_s2" and s["max_zoom"] == 12


def test_default_ndvi_is_off_without_ndvi_sources():
    plan = parse_scene({"name": "t", "bbox": PENDLETON, "priorities": {"imagery": ["bmng"]}})
    assert "ndvi" not in config.layer_settings(plan)


@pytest.mark.parametrize(
    "patch, match",
    [
        ({"ndvi": "yes"}, "ndvi must be true or false"),
        ({"ndvi_max_zoom": 9}, "ndvi_max_zoom"),
        ({"ndvi_max_zoom": 18}, "ndvi_max_zoom"),
        ({"ndvi_max_zoom": 15.0}, "ndvi_max_zoom"),
        ({"ndvi": True, "priorities": {"imagery": ["bmng"]}}, "needs naip_pc or wc_s2"),
    ],
)
def test_bad_ndvi(patch, match):
    with pytest.raises(ConfigError, match=match):
        parse_scene({"name": "t", "bbox": PENDLETON, **patch})
```

- [ ] **Step 2: Run them to make sure they fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_config.py -q -k ndvi`
Expected: FAIL (`unknown key(s) in scene: ['ndvi']` / `AttributeError: ... 'ndvi'`).

- [ ] **Step 3: Implement**

In `config.py`:

1. Module docstring example: after the `naip_water_buffer_m = 200` line add
   ```
   ndvi = true                                # NDVI layer beside the imagery (sim default; preview false)
   ndvi_max_zoom = 15                         # NDVI pyramid depth, [10, 17]
   ```
2. Add `"ndvi"` and `"ndvi_max_zoom"` to `KEYS`.
3. In `PROFILES`, add `"ndvi": False` to `"preview"` and `"ndvi": True` to `"sim"` (same level as `"priorities"`).
4. After `MAX_WATER_BUFFER_M = 5000.0` add:

```python
NDVI_REFERENCE, NDVI_TARGET = "naip_pc", "wc_s2"
NDVI = {  # NDVI layer (layers/ndvi.py) and its NAIP -> Sentinel-2 fit (ndvi_fit.py)
    "feather_m": BALANCE["feather_m"],
    "fit_step_m": 10.0,
    "min_samples": 500,
    "exclude_classes": [0, 80],  # WorldCover no data, permanent water
}
NDVI_MAX_ZOOM = 15
```

5. `ScenePlan`: after `naip_water_buffer_m: float = NAIP_WATER_BUFFER_M` add
   ```python
       ndvi: bool = False
       ndvi_max_zoom: int = NDVI_MAX_ZOOM
   ```
6. `parse_scene`: after the `buffer_m` validation block add

```python
    ndvi_given = "ndvi" in data
    ndvi = data.get("ndvi", PROFILES[profile]["ndvi"])
    if not isinstance(ndvi, bool):
        raise ConfigError(f"ndvi must be true or false, got {ndvi!r}")
    ndvi_max_zoom = _int(data, "ndvi_max_zoom", NDVI_MAX_ZOOM, 10, 17)
```

   and after the `for layer, ids in data.get("priorities", {}).items():` loop add

```python
    if ndvi and ndvi_given and not any(s in prio["imagery"] for s in (NDVI_REFERENCE, NDVI_TARGET)):
        raise ConfigError("ndvi = true needs naip_pc or wc_s2 in priorities.imagery")
```

   and pass `ndvi=ndvi, ndvi_max_zoom=ndvi_max_zoom` to the `ScenePlan(...)` call (after `naip_water_buffer_m=`).
7. After `balance_settings` add:

```python
def ndvi_settings(plan: ScenePlan) -> dict | None:
    """The NDVI layer's settings, or None when it is off or the imagery has neither NAIP nor Sentinel-2."""
    prio = [s for s in plan.priorities["imagery"] if s in (NDVI_REFERENCE, NDVI_TARGET)]
    if not plan.ndvi or not prio:
        return None
    return {
        "priorities": prio,
        "reference": NDVI_REFERENCE if NDVI_REFERENCE in prio else None,
        "target": NDVI_TARGET if NDVI_TARGET in prio else None,
        "max_zoom": plan.ndvi_max_zoom,
        "tile_px": TILE_PX,
        "format": "png",
        "encoding": "L8: 0 nodata, 1 + floor((ndvi + 1) * 127 + 0.5)",
        **NDVI,
        "naip_water_buffer_m": plan.naip_water_buffer_m,
    }
```

8. `layer_settings`: assign the existing returned dict to `out`, then
   ```python
       ndvi = ndvi_settings(plan)
       if ndvi is not None:
           out["ndvi"] = ndvi
       return out
   ```

- [ ] **Step 4: Run the config tests and the whole suite**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_config.py -q` → all pass.
Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests -q` → 253 + 9 pass, 1 skipped.
(`naip_s2_scene` manifests now carry `layers.ndvi` from the `sim` default; nothing reads it until Task 3, which also
makes those scenes set `ndvi` explicitly.)

- [ ] **Step 5: Commit**

```bash
git add scripts/scene/camsim_scene/config.py scripts/scene/tests/test_config.py
git commit -m "feat(scene): ndvi and ndvi_max_zoom settings; layers.ndvi in the manifest

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01EuqoXt1sAk5mjhhppnKP9D"
```

---

### Task 2: Sun position and adapter metadata (NIR bands, acquisition date, sun bounds)

**Files:**
- Create: `scripts/scene/camsim_scene/sun.py`
- Modify: `scripts/scene/camsim_scene/sources/base.py`, `sources/naip_pc.py`, `sources/wc_s2.py`, `scripts/scene/tests/fake_sources.py` (`FakeSource`)
- Test: `scripts/scene/tests/test_sun.py` (create), `scripts/scene/tests/test_sources_imagery.py`

**Interfaces:**
- Produces: `sun.solar_noon(date: dt.date, lon: float, lat: float) -> {"elevation_deg": float, "azimuth_deg": float}`;
  `sun.flight_window(date, lon, lat, min_elevation=30.0) -> {"min_elevation_deg": float, "azimuth_deg": [float, float]} | None`;
  `SourceBase.ndvi_bands: tuple[int, int] | None`; `SourceBase.ndvi_raster(path, asset) -> SourceRaster | None`
  (the `open()` raster with `bands=ndvi_bands`); `naip_pc` asset metadata `acquired`, `sun_noon`, `sun_window`;
  `wc_s2` asset metadata `composite`; `FakeSource` option `ndvi_bands` (list of two ints).

- [ ] **Step 1: Write the failing tests**

`scripts/scene/tests/test_sun.py`:

```python
import datetime as dt

import pytest

from camsim_scene.sun import _declination_and_eot, _julian_day, flight_window, solar_noon


@pytest.mark.parametrize(
    "day, lon, lat, elevation, azimuth",
    [
        (dt.date(2022, 6, 21), -117.4, 33.3, 80.14, 180.0),  # 90 - (33.3 - 23.44)
        (dt.date(2022, 12, 21), -117.4, 33.3, 33.26, 180.0),  # 90 - (33.3 + 23.44)
        (dt.date(2022, 6, 21), 151.2, -33.9, 32.66, 0.0),  # southern winter: the sun is due north
        (dt.date(2022, 6, 21), 0.0, 10.0, 76.56, 0.0),  # tropics, sun north of the zenith
    ],
)
def test_noon_elevation_is_ninety_minus_latitude_minus_declination(day, lon, lat, elevation, azimuth):
    s = solar_noon(day, lon, lat)
    assert s["elevation_deg"] == pytest.approx(elevation, abs=0.05) and s["azimuth_deg"] == azimuth


def test_declination_and_equation_of_time_on_a_naip_flight_day():
    decl, eot = _declination_and_eot(_julian_day(dt.date(2022, 5, 30), 20 * 60))
    assert decl == pytest.approx(21.86, abs=0.1) and eot == pytest.approx(2.4, abs=0.3)  # minutes


def test_flight_window_azimuths():
    eq = flight_window(dt.date(2022, 3, 20), 0.0, 0.0)  # equinox at the equator: due east, then due west
    assert eq["min_elevation_deg"] == 30.0
    assert eq["azimuth_deg"][0] == pytest.approx(90.0, abs=0.5) and eq["azimuth_deg"][1] == pytest.approx(270.0, abs=0.5)
    summer = flight_window(dt.date(2022, 5, 30), -117.34377, 33.2187485)
    assert summer["azimuth_deg"] == pytest.approx([82.2, 277.8], abs=0.05)
    winter = flight_window(dt.date(2022, 12, 21), -117.4, 33.3)
    assert winter["azimuth_deg"] == pytest.approx([158.24, 201.76], abs=0.05)
    assert flight_window(dt.date(2022, 12, 21), 0.0, 70.0) is None  # the sun never reaches 30 degrees
```

Append to `scripts/scene/tests/test_sources_imagery.py`:

```python
def test_naip_records_the_acquisition_date_and_sun_bounds():
    src = make_source(
        "naip_pc", options={"year": "2022"}, http=FakeHttp({STAC_SEARCH: lambda body: fixture_json("stac_naip.json")})
    )
    (a,) = src.discover(AREA)
    assert a.metadata["acquired"] == "2022-05-30"  # the T16:00:00Z placeholder is dropped
    assert a.metadata["sun_noon"] == {"elevation_deg": 78.64, "azimuth_deg": 180.0}
    assert a.metadata["sun_window"] == {"min_elevation_deg": 30.0, "azimuth_deg": [82.2, 277.8]}


def test_naip_and_s2_feed_ndvi_from_red_and_nir():
    from camsim_scene.sources.base import SourceBase

    ras = make_source("naip_pc", http=FakeHttp({})).ndvi_raster("x.tif", Asset("a", "u"))
    assert ras.bands == (1, 4) and ras.nodata_rule == "all_zero" and ras.datum == "nad83_2011"
    s2 = make_source("wc_s2", http=FakeHttp({})).ndvi_raster("x.tif", Asset("a", "u"))
    assert s2.bands == (1, 4) and s2.decode == "s2_reflectance"
    assert SourceBase().ndvi_raster("x.tif", Asset("a", "u")) is None


def test_wc_s2_assets_are_marked_as_a_composite():
    routes = {("HEAD", URL.format(lat="N33", name="N33W118")): (200, 1)}
    (a,) = make_source("wc_s2", http=FakeHttp(routes)).discover(Area((-117.9, 33.1, -117.5, 33.5)))
    assert a.metadata["composite"] == "2021"
```

- [ ] **Step 2: Run them to make sure they fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_sun.py scripts/scene/tests/test_sources_imagery.py -q`
Expected: FAIL (`ModuleNotFoundError: camsim_scene.sun`, `KeyError: 'acquired'`, `AttributeError: ... ndvi_raster`).

- [ ] **Step 3: Implement**

`scripts/scene/camsim_scene/sun.py`:

```python
"""Sun position for NAIP acquisition metadata (REALISM R1 chunk 2): NOAA Global Monitoring Laboratory solar
calculator equations (after Meeus, "Astronomical Algorithms"). Geometric elevation: no refraction. The date is all
NAIP gives (its times are a placeholder), so these are bounds: the sun at local solar noon, and its azimuths when it
crosses NAIP's minimum flying elevation."""

from __future__ import annotations

import datetime as dt
import math

NAIP_MIN_ELEVATION_DEG = 30.0


def _declination_and_eot(jd: float) -> tuple[float, float]:
    """Solar declination (degrees) and equation of time (minutes) at Julian day jd."""
    t = (jd - 2451545.0) / 36525.0
    l0 = (280.46646 + t * (36000.76983 + t * 0.0003032)) % 360.0
    m = 357.52911 + t * (35999.05029 - 0.0001537 * t)
    e = 0.016708634 - t * (0.000042037 + 0.0000001267 * t)
    mr = math.radians(m)
    c = (
        math.sin(mr) * (1.914602 - t * (0.004817 + 0.000014 * t))
        + math.sin(2 * mr) * (0.019993 - 0.000101 * t)
        + math.sin(3 * mr) * 0.000289
    )
    omega = 125.04 - 1934.136 * t
    lam = l0 + c - 0.00569 - 0.00478 * math.sin(math.radians(omega))
    eps0 = 23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) / 60.0
    eps = eps0 + 0.00256 * math.cos(math.radians(omega))
    decl = math.degrees(math.asin(math.sin(math.radians(eps)) * math.sin(math.radians(lam))))
    y = math.tan(math.radians(eps / 2)) ** 2
    l0r = math.radians(l0)
    eot = 4.0 * math.degrees(
        y * math.sin(2 * l0r)
        - 2 * e * math.sin(mr)
        + 4 * e * y * math.sin(mr) * math.cos(2 * l0r)
        - 0.5 * y * y * math.sin(4 * l0r)
        - 1.25 * e * e * math.sin(2 * mr)
    )
    return decl, eot


def _julian_day(d: dt.date, minutes_utc: float) -> float:
    return d.toordinal() + 1721424.5 + minutes_utc / 1440.0


def _noon_declination(date: dt.date, lon: float) -> float:
    """The declination at local solar noon (which depends on the equation of time there: two steps converge)."""
    minutes = 720.0
    for _ in range(3):
        _, eot = _declination_and_eot(_julian_day(date, minutes))
        minutes = 720.0 - 4.0 * lon - eot
    return _declination_and_eot(_julian_day(date, minutes))[0]


def solar_noon(date: dt.date, lon: float, lat: float) -> dict:
    """Sun elevation and azimuth (degrees from north, rounded to 0.01) at local solar noon on `date` at (lon, lat)."""
    decl = _noon_declination(date, lon)
    return {"elevation_deg": round(90.0 - abs(lat - decl), 2), "azimuth_deg": 180.0 if lat >= decl else 0.0}


def flight_window(date: dt.date, lon: float, lat: float, min_elevation: float = NAIP_MIN_ELEVATION_DEG) -> dict | None:
    """The sun's azimuths (degrees from north, rounded to 0.01) when it crosses `min_elevation` in the morning and
    the afternoon of `date` (declination at solar noon); None when the noon sun is below it."""
    decl = _noon_declination(date, lon)
    if 90.0 - abs(lat - decl) < min_elevation:
        return None
    p, d, h0 = math.radians(lat), math.radians(decl), math.radians(min_elevation)
    cos_h = (math.sin(h0) - math.sin(p) * math.sin(d)) / (math.cos(p) * math.cos(d))
    h = math.acos(max(-1.0, min(1.0, cos_h)))
    az = math.degrees(math.atan2(math.sin(h), math.cos(h) * math.sin(p) - math.tan(d) * math.cos(p))) + 180.0
    return {"min_elevation_deg": min_elevation, "azimuth_deg": [round(360.0 - az, 2), round(az, 2)]}
```

`sources/base.py`:
- `from dataclasses import dataclass, field, replace`
- In `class Source(Protocol)`: add `ndvi_bands: tuple[int, int] | None` and
  `def ndvi_raster(self, path: Path, asset: Asset) -> SourceRaster | None: ...`
- In `class SourceBase`: add the class attribute
  `ndvi_bands: tuple[int, int] | None = None  # (red, near-infrared) band numbers: the source feeds the NDVI layer`
  and, after `open`:

```python
    def ndvi_raster(self, path: Path, asset: Asset) -> SourceRaster | None:
        """The raster as (red, near-infrared) for the NDVI layer, or None when the source has no NIR band."""
        if self.ndvi_bands is None:
            return None
        return replace(self.open(path, asset), bands=self.ndvi_bands)
```

`sources/naip_pc.py`:
- Docstring: append "Each asset records its acquisition date and sun bounds (sun.py) for the baked-shadow limit."
- Imports: `from ..sun import flight_window, solar_noon`
- Class attribute: `ndvi_bands = (1, 4)  # red, near-infrared`
- Module function (above `class PcSigner`):

```python
def acquisition_metadata(item: dict) -> dict:
    """`acquired` (the date; Planetary Computer's NAIP datetimes all carry a placeholder T16:00:00Z), the sun at local
    solar noon, and its azimuths at NAIP's 30 degree minimum elevation, at the centre of the item's bbox."""
    when = item["properties"].get("datetime")
    if not when:
        return {}
    day = dt.date.fromisoformat(when[:10])
    w, s, e, n = item["bbox"]
    lon, lat = (w + e) / 2, (s + n) / 2
    out = {"acquired": day.isoformat(), "sun_noon": solar_noon(day, lon, lat)}
    window = flight_window(day, lon, lat)
    if window is not None:
        out["sun_window"] = window
    return out
```

- In `discover`, the metadata dict becomes `{"bbox": …, "epsg": …, "datetime": …, **acquisition_metadata(i)}`.

`sources/wc_s2.py`: class attribute `ndvi_bands = (1, 4)  # B04 red, B08 near-infrared`; in `discover`, metadata
`{"bbox": [lon_i, lat_i, lon_i + 1, lat_i + 1], "composite": "2021"}`.

`tests/fake_sources.py`, `FakeSource.__init__`, after `self.datum = …`:
`self.ndvi_bands = tuple(o["ndvi_bands"]) if "ndvi_bands" in o else None`

- [ ] **Step 4: Run the tests**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_sun.py scripts/scene/tests/test_sources_imagery.py -q` → pass.
Run the whole suite → all pass (asset metadata changes no tile).

- [ ] **Step 5: Commit**

```bash
git add scripts/scene/camsim_scene/sun.py scripts/scene/camsim_scene/sources scripts/scene/tests/test_sun.py \
  scripts/scene/tests/test_sources_imagery.py scripts/scene/tests/fake_sources.py
git commit -m "feat(scene): NAIP acquisition date and sun bounds in asset metadata; NIR bands for NDVI

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01EuqoXt1sAk5mjhhppnKP9D"
```

---

### Task 3: The NDVI layer core — encoding, leaf, parent, the NDVI index

**Files:**
- Create: `scripts/scene/camsim_scene/layers/ndvi.py`
- Modify: `scripts/scene/camsim_scene/layers/imagery.py` (`sample_entries`), `scripts/scene/camsim_scene/context.py`
  (`_index`, `index["ndvi"]`), `scripts/scene/tests/fake_sources.py` (NDVI truth, `ndvi=` on the scenes)
- Test: `scripts/scene/tests/test_ndvi.py` (create)

**Interfaces:**
- Consumes: `SourceBase.ndvi_raster` (Task 2), `layers["ndvi"]["priorities"]` (Task 1).
- Produces (`layers/ndvi.py`): `encode(v) -> uint8 array`, `decode(code) -> float array (NaN nodata)`,
  `ndvi_of(vals (2, ...)) -> float array`, `sample_ndvi(entries, lon, lat, target_m) -> (values, valid, which)`,
  `water_allow(z, x, y, water, feather_m) -> (allow | None, dpx | None)`,
  `reference_weight(z, x, y, ref, feather_m, allow, dpx, buffer_m) -> (256, 256) float`,
  `leaf_ndvi(z, x, y, entries, fit=None, interior=None, water=None, reference="naip_pc", feather_m=200.0) -> (256, 256) float`,
  `leaf_tile(...same args...) -> bytes | None`, `parent_tile(children: dict[(dx, dy), bytes | None]) -> bytes | None`,
  `png_bytes(code) -> bytes`, `read_png(data) -> (256, 256) uint8`. `fit` is anything with `apply(ndarray) -> ndarray`.
  `WorkerState.index["ndvi"]` (a `LayerIndex` of (red, NIR) rasters) when `layers.ndvi` exists.
  `fake_sources`: `ndvi_truth`, `s2_ndvi_truth`, `naip_ndvi_truth`, `NAIP_NIR`; `naip_s2_scene(..., ndvi=False)` and
  `coast_scene(..., ndvi=False)` (the scene dict always carries `"ndvi": ndvi`).

- [ ] **Step 1: Extend the synthetic scenes** (`tests/fake_sources.py`)

Add after `raw_truth`:

```python
NAIP_NIR = 240  # synthetic NAIP near-infrared DN everywhere (ndvi scenes): red carries the NDVI


def ndvi_truth(lon, lat):
    """Smooth synthetic field in [0, 1] (period ~2 km)."""
    return 0.5 + 0.5 * np.sin(np.asarray(lon) * 300.0) * np.cos(np.asarray(lat) * 250.0)


def s2_ndvi_truth(lon, lat):
    return 0.05 + 0.55 * ndvi_truth(lon, lat)


def naip_ndvi_truth(lon, lat):
    """NAIP's (DN) NDVI: the true NAIP -> Sentinel-2 fit is gain 0.9, offset 0.05."""
    return (s2_ndvi_truth(lon, lat) - 0.05) / 0.9
```

Change `naip_s2_scene` to take `ndvi: bool = False` (last parameter) and document it in its docstring: "ndvi: 4-band
files (band 4 NIR) with NDVI from `s2_ndvi_truth` / `naip_ndvi_truth` (+ offset / 200 on NAIP), NAIP red derived from
NIR so it stays in 1..255; the scene sets `ndvi` and the sources' `ndvi_bands`". In the body:

- After `raw = np.stack(...)` for Sentinel-2:

```python
    if ndvi:
        n = s2_ndvi_truth(lon, lat)
        nir = np.rint(raw[0].astype(np.float64) * (1 + n) / (1 - n)).astype(np.uint16)
        raw = np.concatenate([raw, nir[None]])
```

- Replace the NAIP `write_geotiff(root / "naip.tif", ...)` call with:

```python
    naip = np.clip(np.rint(ref), 1, 255).astype(np.uint8)
    if ndvi:
        n = naip_ndvi_truth(lon, lat) + offset(lon, lat) / 200.0
        naip[0] = np.clip(np.rint(NAIP_NIR * (1 - n) / (1 + n)), 1, 255).astype(np.uint8)
        naip = np.concatenate([naip, np.full((1, *lon.shape), NAIP_NIR, np.uint8)])
    write_geotiff(root / "naip.tif", naip, ref_box[0], ref_box[3], res, overviews=(2, 4))
```

  (`lon, lat` at that point are NAIP's cell centres; the `classes` line after it still uses them.)
- In the returned dict add `"ndvi": ndvi,` and give both `naip_pc` and `wc_s2` sources
  `**({"ndvi_bands": [1, 4]} if ndvi else {})`.
- `coast_scene(root, shore=COAST, extra_water=None, ndvi=False)`: pass `ndvi=ndvi` to `naip_s2_scene`; docstring
  gains "ndvi: as naip_s2_scene (NAIP NDVI 0.3 lower east of COAST, from the -60 DN offset)".

- [ ] **Step 2: Write the failing tests** (`scripts/scene/tests/test_ndvi.py`)

```python
import io

import numpy as np
import pytest
from fake_sources import COAST, coast_scene, fake_context, naip_ndvi_truth, naip_s2_scene, s2_ndvi_truth
from PIL import Image

from camsim_scene import tiling
from camsim_scene.context import WorkerState, class_rasters
from camsim_scene.layers import imagery, ndvi
from camsim_scene.water import Water


class Linear:
    def __init__(self, gain, offset):
        self.gain, self.offset = gain, offset

    def apply(self, n):
        return self.gain * np.asarray(n) + self.offset


def state(tmp_path, scene=None):
    ctx = fake_context(tmp_path, scene or naip_s2_scene(tmp_path / "src", ndvi=True))
    return WorkerState(ctx), ctx


def leaf_at(st, z, lon, lat, fit=None, water=None, interior=None):
    s = tiling.tile_size_deg(z)
    x, y = int((lon + 180) // s), int((lat + 90) // s)
    entries = st.index["ndvi"].query(imagery.query_bounds(z, x, y, 200.0))
    (glon, glat), _ = imagery.pixel_grid(z, x, y)
    return ndvi.leaf_ndvi(z, x, y, entries, fit, interior, water), glon, glat


def test_encode_decode_round_trip():
    codes = np.arange(256, dtype=np.uint8)
    assert np.array_equal(ndvi.encode(ndvi.decode(codes)), codes)
    assert ndvi.encode(np.array([-1.0, 0.0, 1.0, np.nan, 2.0, -3.0])).tolist() == [1, 128, 255, 0, 255, 1]
    assert np.isnan(ndvi.decode(np.array([0], np.uint8))[0])


def test_ndvi_of_red_and_nir():
    v = ndvi.ndvi_of(np.array([[100.0, 0.0, 50.0, np.nan], [300.0, 0.0, 50.0, 10.0]]))
    assert v[0] == pytest.approx(0.5) and np.isnan(v[1]) and v[2] == 0.0 and np.isnan(v[3])


def test_ndvi_index_reads_red_and_nir(tmp_path):
    st, _ = state(tmp_path)
    e = st.index["ndvi"].query((10.05, 10.05, 10.06, 10.06))
    assert [imagery.source_of(x) for x in e] == ["naip_pc", "wc_s2"] and all(x.raster.bands == (1, 4) for x in e)


def test_inside_naip_the_leaf_is_naip_ndvi(tmp_path):
    st, _ = state(tmp_path)
    v, lon, lat = leaf_at(st, 12, 10.1, 10.1)
    assert np.isfinite(v).all() and np.abs(v - naip_ndvi_truth(lon, lat)).max() < 0.02


def test_outside_naip_the_leaf_is_sentinel2_ndvi(tmp_path):
    st, _ = state(tmp_path)
    v, lon, lat = leaf_at(st, 12, 9.9, 9.9)
    assert np.isfinite(v).all() and np.abs(v - s2_ndvi_truth(lon, lat)).max() < 0.02


def test_the_fit_applies_to_naip_only(tmp_path):
    st, _ = state(tmp_path)
    for lon0 in (10.1, 9.9):  # inside NAIP (fitted onto Sentinel-2's scale), outside (Sentinel-2 untouched)
        v, lon, lat = leaf_at(st, 12, lon0, lon0, fit=Linear(0.9, 0.05))
        assert np.abs(v - s2_ndvi_truth(lon, lat)).max() < 0.02


def test_naip_feathers_into_sentinel2_over_200_m(tmp_path):
    st, _ = state(tmp_path)
    v, lon, lat = leaf_at(st, 14, 10.001, 10.1, fit=Linear(1.0, 0.3))  # straddles NAIP's west edge (10.0 E)
    naip, s2 = naip_ndvi_truth(lon, lat) + 0.3, s2_ndvi_truth(lon, lat)
    w = (v - s2) / (naip - s2)  # NAIP's share
    d = (lon - 10.0) * 111320.0  # metres east of the edge
    assert np.abs(w[d < -30]).max() < 0.1 and np.abs(w[d > 280] - 1).max() < 0.1
    assert 0.2 < w[(d > 80) & (d < 120)].mean() < 0.8


def test_naip_is_clipped_200_m_offshore_and_kept_on_land(tmp_path):
    st, ctx = state(tmp_path, coast_scene(tmp_path / "src", ndvi=True))
    classes, _ = class_rasters(st.manifest, ctx.asset_paths)
    water = Water(tuple(classes), 200.0, 0.0)
    v, lon, lat = leaf_at(st, 13, 10.11, 10.10, water=water)  # all sea, east of COAST
    off = (lon > COAST + 0.004) & (lon < 10.139) & (lat > 10.081) & (lat < 10.119)
    assert np.abs(v[off] - s2_ndvi_truth(lon, lat)[off]).max() < 0.02
    v, lon, lat = leaf_at(st, 13, 10.085, 10.10, water=water)  # land, with a 300 m lake (kept: within the buffer)
    land = (lon > 10.065) & (lat < 10.119)
    assert np.abs(v[land] - naip_ndvi_truth(lon, lat)[land]).max() < 0.02


def test_png_bytes_are_deterministic_grayscale():
    code = ndvi.encode(np.linspace(-1, 1, 256 * 256).reshape(256, 256))
    a = ndvi.png_bytes(code)
    assert a == ndvi.png_bytes(code.copy())
    im = Image.open(io.BytesIO(a))
    assert im.mode == "L" and im.size == (256, 256) and np.array_equal(ndvi.read_png(a), code)


def test_read_png_refuses_a_wrong_tile():
    buf = io.BytesIO()
    Image.new("RGB", (256, 256)).save(buf, "PNG")
    with pytest.raises(ValueError, match="expected L"):
        ndvi.read_png(buf.getvalue())


def test_parent_means_the_valid_pixels_of_each_block():
    cols = np.arange(256)[None, :] * np.ones((256, 1))
    a = ndvi.encode(np.full((256, 256), 0.5))
    b = ndvi.encode(np.where(cols < 128, 0.2, np.nan))
    c = ndvi.encode(np.where(np.indices((256, 256)).sum(0) % 2 == 0, 0.6, np.nan))  # checkerboard
    kids = {(0, 1): ndvi.png_bytes(a), (1, 1): ndvi.png_bytes(b), (0, 0): ndvi.png_bytes(c), (1, 0): None}
    p = ndvi.decode(ndvi.read_png(ndvi.parent_tile(kids)))
    assert np.allclose(p[:128, :128], 0.5, atol=0.005)
    assert np.allclose(p[:128, 128:192], 0.2, atol=0.005) and np.isnan(p[:128, 192:]).all()
    assert np.allclose(p[128:, :128], 0.6, atol=0.005)  # two valid pixels in every block
    assert np.isnan(p[128:, 128:]).all()


def test_tiles_without_data_produce_nothing():
    assert ndvi.parent_tile({(0, 0): None, (1, 0): None, (0, 1): None, (1, 1): None}) is None
    assert ndvi.leaf_tile(12, 0, 0, []) is None
```

- [ ] **Step 3: Run them to make sure they fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_ndvi.py -q`
Expected: FAIL (`ImportError: cannot import name 'ndvi'`).

- [ ] **Step 4: Implement**

`layers/imagery.py`, `sample_entries`: replace the docstring's "(values (3, ...)" with "(values (bands, ...)" and the
first lines of the body with

```python
    nb = len(entries[0].raster.bands) if entries else 3
    vals = np.full((nb, *lon.shape), np.nan)
```

and `vals[:, take] = v[:3][:, take]` with `vals[:, take] = v[:nb][:, take]`. (Imagery rasters have three bands, so
imagery is unchanged.)

`context.py`, `WorkerState.__init__`, after `self.index = {layer: self._index(layer) for layer in ("terrain", "imagery")}`:

```python
        if self.manifest.layers.get("ndvi"):
            self.index["ndvi"] = self._index("ndvi")
```

and in `_index`, replace `raster = src.open(Path(self.ctx.asset_paths[qid]), to_asset(a))` with

```python
                opener = src.ndvi_raster if layer == "ndvi" else src.open
                raster = opener(Path(self.ctx.asset_paths[qid]), to_asset(a))
                if raster is None:
                    continue  # no near-infrared band
```

(the `entries.append` / `geoms.append` that follow stay together, so the STRtree stays aligned). Update the module
docstring's first sentence to "…a SourceRaster and DatumTransform per data asset (and a (red, NIR) raster for the
NDVI layer), ordered by layer priority…".

`layers/ndvi.py` (create):

```python
"""NDVI tile jobs (REALISM R1 chunk 2; docs/superpowers/specs/2026-10-09-ndvi-sun-metadata-design.md).

Leaves compute (NIR - red) / (NIR + red) from each source's raw values at pixel centres and merge them as the imagery
leaves do: the reference (NAIP) first, through its fit onto the target's (Sentinel-2's) scale, fading into the rest
over `feather_m` inside its valid-data edge, and with a water mask used only within `buffer_m` of land. Parents are
the mean of the valid child pixels in each 2 x 2 block. 8-bit grayscale PNG: 0 = nodata, 1..255 = NDVI -1..+1. A
tile with no valid pixel is not written (None)."""

from __future__ import annotations

import io

import numpy as np
from PIL import Image

from ..config import TILE_PX
from ..sources.base import M_PER_DEG
from . import imagery

NODATA = 0
SCALE = 127.0  # codes per unit of NDVI (step 1/127)


def encode(v) -> np.ndarray:
    v = np.asarray(v, np.float64)
    ok = np.isfinite(v)
    code = 1.0 + np.floor((np.clip(np.where(ok, v, 0.0), -1.0, 1.0) + 1.0) * SCALE + 0.5)
    return np.where(ok, code, NODATA).astype(np.uint8)


def decode(code) -> np.ndarray:
    c = np.asarray(code).astype(np.float64)
    return np.where(c > 0, (c - 1.0) / SCALE - 1.0, np.nan)


def ndvi_of(vals: np.ndarray) -> np.ndarray:
    """NDVI from raw (red, near-infrared) values (2, ...); NaN where either is NaN or both are 0."""
    red, nir = vals[0], vals[1]
    s = nir + red
    with np.errstate(divide="ignore", invalid="ignore"):
        return np.where(s > 0, (nir - red) / s, np.nan)


def sample_ndvi(entries, lon, lat, target_m: float):
    """NDVI merged first-valid-wins in `entries` order: (values, valid, index of the winning entry (-1 none))."""
    vals, ok, which = imagery.sample_entries(entries, lon, lat, target_m)
    v = ndvi_of(vals)
    return v, ok & np.isfinite(v), which


def water_allow(z: int, x: int, y: int, water, feather_m: float):
    """(where the reference may be used, on the feather lattice; metres to land at the pixels), or (None, None)
    without a water mask, with a zero buffer, or with no water near the tile."""
    if water is None or water.buffer_m <= 0:
        return None, None
    dist = imagery.land_distance(z, x, y, water, feather_m)
    if dist is None:
        return None, None
    return dist <= water.buffer_m, imagery.pixel_distance(z, x, y, dist, feather_m)


def reference_weight(z: int, x: int, y: int, ref, feather_m: float, allow, dpx, buffer_m: float) -> np.ndarray:
    """Per pixel, the reference's share where it is valid: the feather ramp, 0 beyond the water buffer."""
    if not ref:
        return np.zeros((TILE_PX, TILE_PX))
    wt = imagery.feather_weight(z, x, y, ref, feather_m, allow)
    return wt if allow is None else np.where(dpx <= buffer_m, wt, 0.0)


def leaf_ndvi(z, x, y, entries, fit=None, interior=None, water=None, reference="naip_pc", feather_m=200.0):
    """NDVI per pixel (NaN = nodata)."""
    (lon, lat), d = imagery.pixel_grid(z, x, y)
    tm = d * M_PER_DEG
    ref = [e for e in entries if imagery.source_of(e) == reference]
    rest = [e for e in entries if imagery.source_of(e) != reference]
    rn, rok, _ = sample_ndvi(ref, lon, lat, tm)
    if fit is not None:
        rn = np.where(rok, fit.apply(rn), np.nan)
    allow, dpx = water_allow(z, x, y, water, feather_m)
    if rok.all() and imagery.inside(interior, z, x, y) and (allow is None or allow.all()):
        return rn
    wt = reference_weight(z, x, y, ref, feather_m, allow, dpx, water.buffer_m if water is not None else 0.0)
    sn, sok, _ = sample_ndvi(rest, lon, lat, tm)
    wt = np.where(sok, wt, 1.0)  # nothing behind the reference here: keep it
    wt = np.where(rok, wt, 0.0)
    out = wt * np.nan_to_num(rn) + (1.0 - wt) * np.nan_to_num(sn)
    return np.where(rok | sok, out, np.nan)


def png_bytes(code: np.ndarray) -> bytes:
    buf = io.BytesIO()
    Image.fromarray(np.ascontiguousarray(code, dtype=np.uint8)).save(buf, "PNG", optimize=False, compress_level=6)
    return buf.getvalue()


def read_png(data: bytes) -> np.ndarray:
    im = Image.open(io.BytesIO(data))
    im.load()
    if im.mode != "L" or im.size != (TILE_PX, TILE_PX):
        raise ValueError(f"NDVI tile is {im.mode} {im.size}, expected L {(TILE_PX, TILE_PX)}")
    return np.asarray(im, np.uint8)


def leaf_tile(z, x, y, entries, fit=None, interior=None, water=None, reference="naip_pc", feather_m=200.0):
    code = encode(leaf_ndvi(z, x, y, entries, fit, interior, water, reference, feather_m))
    return png_bytes(code) if code.any() else None


def parent_tile(children: dict) -> bytes | None:
    """Each pixel: the mean of the valid decoded values in its 2 x 2 block of the children's mosaic (child (dx, dy),
    dy = 1 north). A missing child (None) is nodata."""
    m = np.full((2 * TILE_PX, 2 * TILE_PX), np.nan)
    for (dx, dy), data in children.items():
        if data is not None:
            r0, c0 = (1 - dy) * TILE_PX, dx * TILE_PX
            m[r0 : r0 + TILE_PX, c0 : c0 + TILE_PX] = decode(read_png(data))
    q = [m[0::2, 0::2], m[1::2, 0::2], m[0::2, 1::2], m[1::2, 1::2]]
    n = sum(np.isfinite(v).astype(np.int64) for v in q)
    total = np.nan_to_num(q[0]) + np.nan_to_num(q[1]) + np.nan_to_num(q[2]) + np.nan_to_num(q[3])
    with np.errstate(divide="ignore", invalid="ignore"):
        code = encode(np.where(n > 0, total / n, np.nan))
    return png_bytes(code) if code.any() else None
```

Update `layers/__init__.py`'s docstring to "Layer builders (terrain, imagery, NDVI, land cover). …".

- [ ] **Step 5: Run the tests**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_ndvi.py -q` → pass.
Run the whole suite → all pass (the imagery legacy tests prove `sample_entries` is unchanged for RGB).

- [ ] **Step 6: Commit**

```bash
git add scripts/scene/camsim_scene/layers scripts/scene/camsim_scene/context.py scripts/scene/tests/fake_sources.py \
  scripts/scene/tests/test_ndvi.py
git commit -m "feat(scene): NDVI leaves and parents (NAIP feathered into Sentinel-2, water clip, 8-bit PNG)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01EuqoXt1sAk5mjhhppnKP9D"
```

---

### Task 4: The NAIP → Sentinel-2 NDVI fit

**Files:**
- Create: `scripts/scene/camsim_scene/ndvi_fit.py`
- Test: `scripts/scene/tests/test_ndvi_fit.py` (create)

**Interfaces:**
- Consumes: `layers.ndvi.sample_ndvi` (Task 3), `balance.lattice_blocks`, `balance.land_mask`, `WorkerState.index["ndvi"]`.
- Produces: `ndvi_fit.FILE = "fit.json"`, `ndvi_fit.FORMAT = 1`; `NdviFit(reference, target, gain_e6, offset_e6, report)`
  with `make(reference, target, gain, offset)`, `.gain`, `.offset`, `apply(n)`, `to_json() -> bytes`,
  `from_json(bytes)`; `linear_fit(x, y) -> (gain, offset)`;
  `fit_ndvi(index, classes, bounds, s: dict, dates: dict | None = None) -> NdviFit | None` (`s` = `layers.ndvi`;
  `dates`: reference qid `"<source>/<asset id>"` → `YYYY-MM-DD`). `report` keys: `samples_fit`, `samples_heldout`,
  `gain`, `offset`, `heldout` {`mae_before`, `mae_after`, `bias_before`, `bias_after`}, `bias_after_by_date`
  {date or "unknown": {`n`, `bias`}}.

- [ ] **Step 1: Write the failing tests** (`scripts/scene/tests/test_ndvi_fit.py`)

```python
import numpy as np
import pytest
from fake_sources import coast_scene, fake_context, naip_s2_scene

from camsim_scene import config
from camsim_scene.context import WorkerState, class_rasters
from camsim_scene.ndvi_fit import NdviFit, fit_ndvi, linear_fit

S = {**config.NDVI, "reference": "naip_pc", "target": "wc_s2", "fit_step_m": 200.0, "min_samples": 20}


def test_json_round_trip_is_quantised():
    f = NdviFit.make("naip_pc", "wc_s2", 0.9000004, 0.0499996)
    assert (f.gain_e6, f.offset_e6) == (900000, 50000)
    g = NdviFit.from_json(f.to_json())
    assert g == f and g.to_json() == f.to_json() and b'"gain_e6": 900000' in f.to_json()


def test_another_format_is_refused():
    with pytest.raises(ValueError, match="format"):
        NdviFit.from_json(b'{"format": 0}')


def test_apply_clamps_and_keeps_nan():
    v = NdviFit.make("a", "b", 2.0, 0.5).apply(np.array([0.0, 0.5, -1.0, np.nan]))
    assert v[:3].tolist() == [0.5, 1.0, -1.0] and np.isnan(v[3])


def test_the_fit_is_the_same_for_shuffled_samples():
    rng = np.random.default_rng(1)
    x = rng.uniform(-0.2, 0.8, 5000)
    y = 0.9 * x + 0.05 + rng.normal(0, 0.02, 5000)
    p = rng.permutation(5000)
    a, b = (NdviFit.make("r", "t", *linear_fit(xx, yy)) for xx, yy in ((x, y), (x[p], y[p])))
    assert a.to_json() == b.to_json()


def test_fit_recovers_the_synthetic_gain_and_offset(tmp_path):
    st = WorkerState(fake_context(tmp_path, naip_s2_scene(tmp_path / "src", ndvi=True)))
    f = fit_ndvi(st.index["ndvi"], [], (10.0, 10.0, 10.2, 10.2), S, {"naip_pc/naip": "2022-05-30"})
    assert f.gain == pytest.approx(0.9, abs=0.02) and f.offset == pytest.approx(0.05, abs=0.01)
    r = f.report
    assert r["samples_fit"] >= 20 and r["samples_heldout"] > 0
    assert abs(r["heldout"]["bias_after"]) <= 0.005 and r["heldout"]["mae_after"] < r["heldout"]["mae_before"]
    assert list(r["bias_after_by_date"]) == ["2022-05-30"] and r["bias_after_by_date"]["2022-05-30"]["n"] > 0


def test_no_shared_samples_is_not_fitted(tmp_path):
    scene = naip_s2_scene(tmp_path / "src", ndvi=True, s2_box=(11.0, 11.0, 11.2, 11.2))
    st = WorkerState(fake_context(tmp_path, scene))
    assert fit_ndvi(st.index["ndvi"], [], (10.0, 10.0, 10.2, 10.2), S) is None


def test_water_is_left_out_of_the_fit(tmp_path):
    ctx = fake_context(tmp_path, coast_scene(tmp_path / "src", ndvi=True))
    st = WorkerState(ctx)
    classes, _ = class_rasters(st.manifest, ctx.asset_paths)
    f = fit_ndvi(st.index["ndvi"], classes, (10.06, 10.08, 10.14, 10.12), {**S, "fit_step_m": 50.0})
    assert f.gain == pytest.approx(0.9, abs=0.03) and f.offset == pytest.approx(0.05, abs=0.015)  # not the sea's -0.3
    assert list(f.report["bias_after_by_date"]) == ["unknown"]
```

- [ ] **Step 2: Run them to make sure they fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_ndvi_fit.py -q`
Expected: FAIL (`ModuleNotFoundError: camsim_scene.ndvi_fit`).

- [ ] **Step 3: Implement** (`scripts/scene/camsim_scene/ndvi_fit.py`)

```python
"""NAIP -> Sentinel-2 NDVI fit (REALISM R1 chunk 2; docs/superpowers/specs/2026-10-09-ndvi-sun-metadata-design.md).

NAIP NDVI comes from uncalibrated DN; one linear map (gain, offset), least squares on the land lattice the colour
match uses (balance.py), puts it on Sentinel-2's near-reflectance scale. Stored as ndvi/fit.json in integer
millionths, so float summation order can't change the file; workers read it back from the file."""

from __future__ import annotations

import json
import logging
from dataclasses import dataclass, field

import numpy as np

from .balance import land_mask, lattice_blocks
from .manifest import canonical_json
from .sources.base import M_PER_DEG
from .tiling import Bounds

log = logging.getLogger(__name__)
FILE = "fit.json"
FORMAT = 1
MICRO = 1_000_000


@dataclass
class NdviFit:
    reference: str
    target: str
    gain_e6: int
    offset_e6: int
    report: dict = field(default_factory=dict, compare=False)

    @classmethod
    def make(cls, reference: str, target: str, gain: float, offset: float) -> NdviFit:
        return cls(reference, target, int(round(gain * MICRO)), int(round(offset * MICRO)))

    @property
    def gain(self) -> float:
        return self.gain_e6 / MICRO

    @property
    def offset(self) -> float:
        return self.offset_e6 / MICRO

    def apply(self, n) -> np.ndarray:
        """Reference NDVI on the target's scale, clamped to [-1, 1]; NaN stays NaN."""
        return np.clip(self.gain * np.asarray(n, np.float64) + self.offset, -1.0, 1.0)

    def to_json(self) -> bytes:
        d = {
            "format": FORMAT,
            "reference": self.reference,
            "target": self.target,
            "gain_e6": self.gain_e6,
            "offset_e6": self.offset_e6,
        }
        return canonical_json(d).encode()

    @classmethod
    def from_json(cls, data: bytes) -> NdviFit:
        d = json.loads(data)
        if d.get("format") != FORMAT:
            raise ValueError(f"ndvi fit format {d.get('format')!r} != {FORMAT}")
        return cls(d["reference"], d["target"], int(d["gain_e6"]), int(d["offset_e6"]))


def linear_fit(x: np.ndarray, y: np.ndarray) -> tuple[float, float]:
    """Least-squares (gain, offset) for y ~ gain * x + offset."""
    gain, offset = np.polyfit(np.asarray(x, np.float64), np.asarray(y, np.float64), 1)
    return float(gain), float(offset)


def _r4(v) -> float | None:
    return None if v is None or not np.isfinite(v) else round(float(v), 4)


def fit_ndvi(index, classes, bounds: Bounds, s: dict, dates: dict | None = None) -> NdviFit | None:
    """Fit on lattice nodes over `bounds` (the reference's footprints) where the reference and the target are both
    valid and the land cover isn't excluded: even (i + j) nodes fit, odd ones are held out for the report, which gives
    each reference acquisition date's residual bias (`dates`: reference qid -> date). None with fewer than
    s["min_samples"] fit samples."""
    from .layers.imagery import source_of
    from .layers.ndvi import sample_ndvi

    dates = dates or {}
    step_m = s["fit_step_m"]
    exclude = set(s["exclude_classes"])
    parts = []
    for lon, lat, i, j in lattice_blocks(bounds, step_m / M_PER_DEG):
        entries = index.query((float(lon.min()), float(lat.min()), float(lon.max()), float(lat.max())))
        ref = [e for e in entries if source_of(e) == s["reference"]]
        tgt = [e for e in entries if source_of(e) == s["target"]]
        if not ref or not tgt:
            continue
        rn, rok, rw = sample_ndvi(ref, lon, lat, step_m)
        tn, tok, _ = sample_ndvi(tgt, lon, lat, step_m)
        keep = rok & tok & land_mask(classes, lon, lat, step_m, exclude)
        if not keep.any():
            continue
        day = np.array([dates.get(e.qid, "") for e in ref], dtype=object)[rw[keep]]
        parts.append((rn[keep], tn[keep], ((i + j) % 2 == 1)[keep], day))
    if not parts:
        log.warning("ndvi fit: no shared land samples between %s and %s", s["reference"], s["target"])
        return None
    x, y, held, day = (np.concatenate([p[k] for p in parts]) for k in range(4))
    fit = ~held
    if fit.sum() < s["min_samples"]:
        log.warning("ndvi fit: only %d shared land samples; not fitted", int(fit.sum()))
        return None
    f = NdviFit.make(s["reference"], s["target"], *linear_fit(x[fit], y[fit]))
    before, after = y[held] - x[held], y[held] - f.apply(x[held])
    by_date = {}
    for d in sorted(set(day[held].tolist())):
        m = day[held] == d
        by_date[d or "unknown"] = {"n": int(m.sum()), "bias": _r4(np.median(after[m]))}
    has = before.size > 0
    f.report = {
        "samples_fit": int(fit.sum()),
        "samples_heldout": int(held.sum()),
        "gain": f.gain,
        "offset": f.offset,
        "heldout": {
            "mae_before": _r4(np.abs(before).mean()) if has else None,
            "mae_after": _r4(np.abs(after).mean()) if has else None,
            "bias_before": _r4(np.median(before)) if has else None,
            "bias_after": _r4(np.median(after)) if has else None,
        },
        "bias_after_by_date": by_date,
    }
    log.info("ndvi fit: gain %.6f offset %.6f, held out %s", f.gain, f.offset, json.dumps(f.report["heldout"]))
    return f
```

- [ ] **Step 4: Run the tests**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_ndvi_fit.py -q` → pass; then the
whole suite.

- [ ] **Step 5: Commit**

```bash
git add scripts/scene/camsim_scene/ndvi_fit.py scripts/scene/tests/test_ndvi_fit.py
git commit -m "feat(scene): NAIP -> Sentinel-2 NDVI fit (gain and offset in millionths, per-date residuals)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01EuqoXt1sAk5mjhhppnKP9D"
```

---

### Task 5: Build wiring — empty tiles, the NDVI plan, fit stage, NDVI jobs

**Files:**
- Modify: `scripts/scene/camsim_scene/engine.py`, `tiling.py`, `tms.py`, `context.py`, `pipeline.py`
- Test: `scripts/scene/tests/test_engine.py`, `scripts/scene/tests/test_tiling.py`, `scripts/scene/tests/test_ndvi_build.py` (create)

**Interfaces:**
- Consumes: Tasks 1, 3, 4.
- Produces: `engine.EMPTY = ""`; `run_tile` accepting a producer that returns `None` (no file; marker output `EMPTY`);
  `tiling.ndvi_plan(imagery: LayerPlan, max_zoom: int, area) -> LayerPlan`;
  `tms.tilemapresource_xml(title, max_zoom, bounds, mime="image/jpeg", ext="jpg")`;
  `WorkerState.ndvi_fit`, `.ndvi_fit_sha`, `.ndvi_interior`, `.ndvi_water`, `.ndvi_water_shas`;
  `pipeline.ndvi_batch`, `build.json`/`build_scene()` return value key `"ndvi"` (fit report or None) and
  `layers["ndvi"]` stats when the layer exists.

- [ ] **Step 1: Write the failing tests**

Append to `scripts/scene/tests/test_engine.py`:

```python
def test_a_producer_returning_none_writes_no_file_and_is_skipped_next_time(tmp_path):
    m = Markers(tmp_path)
    r = engine.run_tile(tmp_path, m, "ndvi", "png", 3, 1, 2, "in", lambda: None)
    assert not r.skipped and r.size == 0 and r.sha256 == engine.EMPTY
    assert not (tmp_path / "ndvi/3/1/2.png").exists()
    r = engine.run_tile(tmp_path, m, "ndvi", "png", 3, 1, 2, "in", lambda: pytest.fail("rebuilt"))
    assert r.skipped
    (tmp_path / "ndvi/3/1").mkdir(parents=True)
    (tmp_path / "ndvi/3/1/2.png").write_bytes(b"stale")  # e.g. data that a source no longer has
    r = engine.run_tile(tmp_path, m, "ndvi", "png", 3, 1, 2, "in", lambda: None)
    assert not r.skipped and not (tmp_path / "ndvi/3/1/2.png").exists()


def test_ndvi_files_resolve_to_their_markers(tmp_path):
    m = Markers(tmp_path)
    m.write("ndvi", 4, 5, 6, "i", "abc")
    assert m.output_for_file("ndvi/4/5/6.png") == "abc"


def test_stats_count_no_file_for_an_empty_tile():
    st = LayerStats()
    st.add(engine.TileResult("ndvi", 0, 0, 0, False, 0, engine.EMPTY, 0.0, 1, 1.0))
    assert st.tiles == 1 and st.files == 0
```

Append to `scripts/scene/tests/test_tiling.py`:

```python
def test_ndvi_plan_caps_the_imagery_and_keeps_to_the_area():
    regions = [
        Region.from_bounds("globe", tiling.GLOBE, {"imagery": 2}),
        Region.from_bounds("bbox", (10.0, 10.0, 10.5, 10.5), {"imagery": 9}),
    ]
    cov = Coverage()
    cov.add(shapely.box(*tiling.GLOBE), 9)
    ip = plan_tiles(regions, "imagery", cov)
    area = shapely.box(10.0, 10.0, 10.2, 10.2)
    p = tiling.ndvi_plan(ip, 7, area)
    assert p.max_zoom == 7 and np.array_equal(p.leaves[7], p.tiles[7])
    for z, keys in p.tiles.items():
        assert shapely.intersects(tiling.tile_boxes(z, keys), area).all() and np.isin(keys, ip.tiles[z]).all()
    assert len(p.tiles[7]) < len(ip.tiles[7])
    far = tiling.ndvi_plan(ip, 7, shapely.box(50, 50, 51, 51))  # only the globe's z0-2 reach it
    assert far.max_zoom == 2 and np.array_equal(far.leaves[2], far.tiles[2])
    assert tiling.ndvi_plan(ip, 7, shapely.Polygon()).tiles == {}


def test_tilemapresource_names_png_tiles():
    xml = tms.tilemapresource_xml("t", 3, (0.0, 0.0, 1.0, 1.0), "image/png", "png")
    tf = ET.fromstring(xml).find("TileFormat")
    assert tf.get("mime-type") == "image/png" and tf.get("extension") == "png"
    assert 'mime-type="image/jpeg"' in tms.tilemapresource_xml("t", 3, (0.0, 0.0, 1.0, 1.0))
```

`scripts/scene/tests/test_ndvi_build.py` (create):

```python
import json

import numpy as np
import pytest
from fake_sources import COAST, build_synthetic, coast_scene, naip_s2_scene, s2_ndvi_truth

from camsim_scene import config, tiling
from camsim_scene.layers import imagery, ndvi
from camsim_scene.pipeline import build_scene

ZOOM = {"globe": {"terrain": 2, "imagery": 2}, "ring": {"terrain": 3, "imagery": 3}, "bbox": {"terrain": 3, "imagery": 11}}


@pytest.fixture(autouse=True)
def fast_fits(monkeypatch):
    monkeypatch.setitem(config.BALANCE, "fit_step_m", 200.0)
    monkeypatch.setitem(config.BALANCE, "min_cell_samples", 20)
    monkeypatch.setitem(config.NDVI, "fit_step_m", 200.0)
    monkeypatch.setitem(config.NDVI, "min_samples", 20)


def scene(root, **kw):
    return {**naip_s2_scene(root, ndvi=True, **kw), "zoom": ZOOM, "ndvi_max_zoom": 10}


def hashes(pkg):
    return (pkg / "hashes.txt").read_text()


def tile(pkg, z, lon, lat):
    s = tiling.tile_size_deg(z)
    x, y = int((lon + 180) // s), int((lat + 90) // s)
    (glon, glat), _ = imagery.pixel_grid(z, x, y)
    return ndvi.decode(ndvi.read_png((pkg / f"ndvi/{z}/{x}/{y}.png").read_bytes())), glon, glat


def test_ndvi_layer_is_written_hashed_and_fitted(tmp_path):
    pkg, _, info = build_synthetic(tmp_path, scene(tmp_path / "src"))
    names = hashes(pkg)
    assert "ndvi/fit.json" in names and "ndvi/tilemapresource.xml" in names and "ndvi/10/" in names
    assert info["ndvi"]["fitted"] is True and info["ndvi"]["gain"] == pytest.approx(0.9, abs=0.03)
    assert info["layers"]["ndvi"]["files"] > 0
    assert json.loads((pkg / "build.json").read_text())["ndvi"]["samples_fit"] > 0
    v, lon, lat = tile(pkg, 10, 10.1, 10.1)
    inside = (lon > 10.01) & (lon < 10.19) & (lat > 10.01) & (lat < 10.19)
    assert np.abs(v[inside] - s2_ndvi_truth(lon, lat)[inside]).max() < 0.03  # NAIP fitted onto Sentinel-2
    assert sorted(int(p.name) for p in (pkg / "ndvi").iterdir() if p.is_dir()) == list(range(11))
    assert 'mime-type="image/png"' in (pkg / "ndvi/tilemapresource.xml").read_text()
    for p in (pkg / "ndvi").rglob("*.png"):
        assert ndvi.read_png(p.read_bytes()).any()


def test_rebuild_skips_every_ndvi_tile_and_the_fit(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path, scene(tmp_path / "src"))
    before = hashes(pkg)
    info = build_scene(pkg, cache, jobs=1)
    assert info["ndvi"]["skipped"] is True and info["layers"]["ndvi"]["built"] == 0
    assert hashes(pkg) == before


def test_ndvi_build_is_independent_of_worker_count(tmp_path):
    s = scene(tmp_path / "src")
    a, cache, _ = build_synthetic(tmp_path, s, jobs=1, name="a")
    b, _, _ = build_synthetic(tmp_path, s, jobs=2, name="b", cache=cache)
    assert hashes(a) == hashes(b)


def without_ndvi(text):
    return [line for line in text.splitlines() if not line.split("  ", 1)[1].startswith("ndvi/")]


def test_ndvi_off_gives_the_same_package_without_the_layer(tmp_path):
    s = scene(tmp_path / "src")
    a, cache, _ = build_synthetic(tmp_path, s, name="on")
    b, _, info = build_synthetic(tmp_path, {**s, "ndvi": False}, name="off", cache=cache)
    assert not (b / "ndvi").exists() and info["ndvi"] is None and "ndvi" not in info["layers"]
    assert hashes(b).splitlines() == without_ndvi(hashes(a))


def test_adding_ndvi_rebuilds_no_other_tile(tmp_path):
    s = scene(tmp_path / "src")
    pkg, cache, _ = build_synthetic(tmp_path, {**s, "ndvi": False})
    on, _, _ = build_synthetic(tmp_path, s, name="on", cache=cache)
    (pkg / "manifest.json").write_bytes((on / "manifest.json").read_bytes())
    info = build_scene(pkg, cache, jobs=1)
    assert info["layers"]["terrain"]["built"] == 0 and info["layers"]["imagery"]["built"] == 0
    assert info["layers"]["ndvi"]["built"] > 0 and hashes(pkg) == hashes(on)


def test_turning_ndvi_off_removes_the_layer(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path, scene(tmp_path / "src"))
    m = json.loads((pkg / "manifest.json").read_text())
    del m["layers"]["ndvi"]
    (pkg / "manifest.json").write_text(json.dumps(m))
    build_scene(pkg, cache, jobs=1)
    assert not (pkg / "ndvi").exists() and not (pkg / ".state/ndvi").exists()
    assert not (pkg / ".state/ndvi_fit.json").exists() and "ndvi/" not in hashes(pkg)


def test_sentinel2_only_ndvi_builds_without_a_fit(tmp_path):
    s = scene(tmp_path / "src")
    s["priorities"] = {**s["priorities"], "imagery": ["wc_s2", "base_rgb"]}
    del s["sources"]["naip_pc"]
    pkg, _, info = build_synthetic(tmp_path, s)
    assert info["ndvi"] is None and not (pkg / "ndvi/fit.json").exists()
    v, lon, lat = tile(pkg, 10, 10.1, 10.1)
    ok = np.isfinite(v)
    assert ok.mean() > 0.5 and np.abs(v[ok] - s2_ndvi_truth(lon, lat)[ok]).max() < 0.03


def test_coast_ndvi_is_sentinel2_offshore(tmp_path):
    zoom = {**ZOOM, "bbox": {"terrain": 3, "imagery": 13}}
    pkg, _, info = build_synthetic(tmp_path, {**coast_scene(tmp_path / "src", ndvi=True), "zoom": zoom, "ndvi_max_zoom": 13})
    assert info["ndvi"]["fitted"] is True
    v, lon, lat = tile(pkg, 13, 10.11, 10.10)
    off = (lon > COAST + 0.004) & (lon < 10.139) & (lat > 10.081) & (lat < 10.119)
    assert np.abs(v[off] - s2_ndvi_truth(lon, lat)[off]).max() < 0.02
```

- [ ] **Step 2: Run them to make sure they fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_engine.py scripts/scene/tests/test_tiling.py scripts/scene/tests/test_ndvi_build.py -q`
Expected: FAIL (`AttributeError: module 'camsim_scene.engine' has no attribute 'EMPTY'`, no `ndvi_plan`, no `ndvi/`).

- [ ] **Step 3: Implement the engine** (`engine.py`)

- `TILE_RE = re.compile(r"^(?P<layer>terrain|imagery|ndvi)/(?P<z>\d+)/(?P<x>\d+)/(?P<y>\d+)\.(terrain|jpg|png)$")`
- After `TILE_RE`: `EMPTY = ""  # marker output of a tile whose producer returned None: no file (an NDVI tile without data)`
- `LayerStats.add`: `self.files += r.sha256 != EMPTY` (replacing `self.files += 1`).
- `run_tile` (`produce: Callable[[], bytes | None]`):

```python
    t0 = time.perf_counter()
    path = tile_path(pkg, layer, z, x, y, ext)
    rec = markers.read(layer, z, x, y)
    if rec and rec["inputs"] == inputs:
        if rec["output"] == EMPTY and not path.exists():
            return TileResult(layer, z, x, y, True, 0, EMPTY, time.perf_counter() - t0, os.getpid(), peak_rss_mb())
        if path.exists():
            sha = sha256_file(path)
            if sha == rec["output"]:
                return TileResult(
                    layer, z, x, y, True, path.stat().st_size, sha, time.perf_counter() - t0, os.getpid(), peak_rss_mb()
                )
    data = produce()
    if data is None:
        path.unlink(missing_ok=True)
        markers.write(layer, z, x, y, inputs, EMPTY)
        return TileResult(layer, z, x, y, False, 0, EMPTY, time.perf_counter() - t0, os.getpid(), peak_rss_mb())
    atomic_write(path, data)
    ...unchanged
```

- [ ] **Step 4: Implement the plan and the XML** (`tiling.py`, `tms.py`)

`tiling.py`, after `plan_tiles`:

```python
def ndvi_plan(imagery: LayerPlan, max_zoom: int, area) -> LayerPlan:
    """The NDVI pyramid: the imagery's tiles up to min(max_zoom, the imagery's depth) that overlap `area` (where the
    NIR sources have data). Leaves are the tiles at the cap and the imagery's leaves below it. Empty without overlap."""
    tiles: dict[int, np.ndarray] = {}
    leaves: dict[int, np.ndarray] = {}
    top = min(max_zoom, imagery.max_zoom)
    for z in range(top + 1):
        keys = imagery.tiles[z]
        keys = keys[_overlaps(tile_boxes(z, keys), area)] if len(keys) else keys
        if not len(keys):
            break
        tiles[z] = keys
        leaves[z] = keys if z == top else keys[np.isin(keys, imagery.leaves[z])]
    return LayerPlan(tiles, leaves)
```

`tms.py`: `def tilemapresource_xml(title: str, max_zoom: int, bounds: Bounds, mime: str = "image/jpeg", ext: str = "jpg") -> str:`
and the `TileFormat` line becomes `f'<TileFormat width="256" height="256" mime-type="{mime}" extension="{ext}"/>'`.
Module docstring: "(geodetic profile, EPSG:4326, origin -180/-90, 256 px JPEG or PNG tiles)".

- [ ] **Step 5: Implement the worker state** (`context.py`)

Imports: `from .ndvi_fit import FILE as NDVI_FIT_FILE` and `from .ndvi_fit import NdviFit`. Replace the Task 3 lines
(`if self.manifest.layers.get("ndvi"): self.index["ndvi"] = self._index("ndvi")`) with, after the imagery balance
block at the end of `__init__`:

```python
        self.ndvi_fit: NdviFit | None = None
        self.ndvi_fit_sha = ""
        self.ndvi_interior = None
        self.ndvi_water: Water | None = None
        self.ndvi_water_shas: list[str] = []
        ns = self.manifest.layers.get("ndvi")
        if ns:
            self.index["ndvi"] = self._index("ndvi")
            path = self.pkg / "ndvi" / NDVI_FIT_FILE
            if path.exists():
                data = path.read_bytes()
                self.ndvi_fit, self.ndvi_fit_sha = NdviFit.from_json(data), sha256_bytes(data)
            if ns["reference"]:
                fps = reference_footprints(self.manifest, ns["reference"])
                if fps:
                    self.ndvi_interior = ref_interior(fps, ns["feather_m"])
            if ns["naip_water_buffer_m"] > 0:
                classes, shas = class_rasters(self.manifest, ctx.asset_paths)
                if classes:
                    self.ndvi_water = Water(tuple(classes), float(ns["naip_water_buffer_m"]), 0.0)
                    self.ndvi_water_shas = sorted(shas)
```

Module docstring: add "…plus the package's NDVI fit (ndvi/fit.json) and water mask when it has an NDVI layer."

- [ ] **Step 6: Implement the pipeline** (`pipeline.py`)

Imports: `from . import ndvi_fit`; `from .layers import imagery, landcover, ndvi, terrain`;
`from .tiling import LayerPlan, available_ranges, ndvi_plan, plan_tiles, split_keys`;
`from .verify import present_tiles`. Module docstring's build line: "build: plan the pyramids, run the tile jobs
(resumable), then write layer.json, tilemapresource.xml (imagery, NDVI), land cover, …". Add
`EXT = {"terrain": "terrain", "imagery": "jpg", "ndvi": "png"}` after `CHUNK`, and in `_run_layer` use
`remove_stale(Path(ctx.pkg), layer, EXT[layer], plan)`.

Workers (after `imagery_batch`):

```python
def _ndvi_parent_bytes(pkg: Path, z: int, x: int, y: int) -> bytes | None:
    kids = {}
    for dx in (0, 1):
        for dy in (0, 1):
            p = tile_path(pkg, "ndvi", z + 1, 2 * x + dx, 2 * y + dy, "png")
            kids[(dx, dy)] = p.read_bytes() if p.exists() else None
    return ndvi.parent_tile(kids)


def _ndvi_tile(z: int, x: int, y: int, leaf: bool):
    st = _STATE
    s = st.manifest.layers["ndvi"]
    if leaf:
        entries = st.index["ndvi"].query(imagery.query_bounds(z, x, y, s["feather_m"]))
        extra = ([st.ndvi_fit_sha] if st.ndvi_fit_sha else []) + st.ndvi_water_shas
        inputs = inputs_hash(st.settings["ndvi"], __version__, "leaf", *extra, *sorted(e.sha256 for e in entries))
        produce = partial(
            ndvi.leaf_tile, z, x, y, entries, st.ndvi_fit, st.ndvi_interior, st.ndvi_water, s["reference"], s["feather_m"]
        )
    else:
        outs = []
        for dx, dy in ((0, 0), (1, 0), (0, 1), (1, 1)):
            rec = _MARKERS.read("ndvi", z + 1, 2 * x + dx, 2 * y + dy)
            outs.append(rec["output"] if rec else "absent")  # children outside the NDVI area are not planned
        inputs = inputs_hash(st.settings["ndvi"], __version__, "parent", *outs)
        produce = partial(_ndvi_parent_bytes, st.pkg, z, x, y)
    return run_tile(st.pkg, _MARKERS, "ndvi", "png", z, x, y, inputs, produce)


def ndvi_batch(items: list[tuple[int, int, int, bool]]) -> list:
    return [_ndvi_tile(*it) for it in items]
```

Fit and layer (after `_fit_balance`):

```python
def _fit_ndvi(pkg: Path, m: Manifest, ctx: BuildContext) -> dict | None:
    """Fit (or reuse) ndvi/fit.json before the NDVI tiles; the report goes to build.json. None without an NDVI layer
    that has both NAIP and Sentinel-2."""
    s = m.layers.get("ndvi")
    out = pkg / "ndvi" / ndvi_fit.FILE
    marker = pkg / STATE_DIR / "ndvi_fit.json"
    if not s or not s["reference"] or not s["target"]:
        out.unlink(missing_ok=True)
        marker.unlink(missing_ok=True)
        return None
    classes, class_shas = class_rasters(m, ctx.asset_paths)
    shas = sorted(a.sha256 for sid in (s["reference"], s["target"]) for a in m.source(sid).assets) + sorted(class_shas)
    inputs = inputs_hash(m.layer_settings_hash("ndvi"), __version__, "ndvi_fit", *shas)
    prev = json.loads(marker.read_text()) if marker.exists() else None
    if prev and prev["inputs"] == inputs:
        have = sha256_file(out) if out.exists() else None
        if have == prev["output"]:
            return {**prev["report"], "skipped": True}
    t0 = time.monotonic()
    out.unlink(missing_ok=True)  # an old or unreadable file must never block the refit
    marker.unlink(missing_ok=True)
    fps = reference_footprints(m, s["reference"])
    fit = None
    if fps:
        st = WorkerState(ctx)
        dates = {f"{s['reference']}/{a.id}": a.metadata.get("acquired", "") for a in m.source(s["reference"]).assets}
        fit = ndvi_fit.fit_ndvi(st.index["ndvi"], classes, shapely.union_all(fps).bounds, s, dates)
    if fit is None:
        log.warning("ndvi: no fit; NAIP NDVI stays on its own (DN) scale")
        output, report = None, {"fitted": False}
    else:
        atomic_write(out, fit.to_json())
        output, report = sha256_file(out), {"fitted": True, **fit.report}
    report["seconds"] = round(time.monotonic() - t0, 3)
    atomic_write(marker, json.dumps({"inputs": inputs, "output": output, "report": report}).encode())
    return {**report, "skipped": False}


def _ndvi_area(m: Manifest):
    """Where the NDVI layer is built: the ring, within the footprints of its sources' data."""
    ring = next(r for r in m.region_objs() if r.name == "ring").geometry()
    return shapely.intersection(ring, shapely.union_all(coverage(m, "ndvi").geoms))


def _build_ndvi(pkg: Path, m: Manifest, ctx: BuildContext, iplan: LayerPlan, jobs: int, json_progress: bool):
    """The NDVI tiles and their tilemapresource.xml (zooms and bounds of the tiles written); removes the layer when
    the manifest has none. Returns its stats, or None."""
    s = m.layers.get("ndvi")
    if not s:
        shutil.rmtree(pkg / "ndvi", ignore_errors=True)
        shutil.rmtree(pkg / STATE_DIR / "ndvi", ignore_errors=True)
        return None
    plan = ndvi_plan(iplan, s["max_zoom"], _ndvi_area(m))
    phases = _imagery_phases(plan) if plan.count() else []
    stats = _run_layer(ctx, "ndvi", plan, phases, ndvi_batch, jobs, json_progress)
    xml = pkg / "ndvi" / "tilemapresource.xml"
    present = {z: k for z, k in present_tiles(pkg, "ndvi", "png").items() if len(k)}
    if present:
        bounds = plan_bounds(LayerPlan(present, {}))
        atomic_write(xml, tilemapresource_xml(m.name, max(present), bounds, "image/png", "png").encode())
    else:
        xml.unlink(missing_ok=True)
    return stats
```

`build_scene`: after `bal_report = _fit_balance(pkg, m, ctx)` add
`ndvi_report = _fit_ndvi(pkg, m, ctx)  # before any worker starts: they load fit.json`; after the imagery
`tilemapresource.xml` write add

```python
        ndvi_stats = _build_ndvi(pkg, m, ctx, iplan, jobs, json_progress)
        if ndvi_stats is not None:
            stats["ndvi"] = ndvi_stats
```

and add `"ndvi": ndvi_report,` to `info` after `"balance": bal_report,`.

- [ ] **Step 7: Run the tests**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_engine.py scripts/scene/tests/test_tiling.py scripts/scene/tests/test_ndvi_build.py -q` → pass.
Run the whole suite → all pass; lint clean.

- [ ] **Step 8: Commit**

```bash
git add scripts/scene/camsim_scene scripts/scene/tests/test_engine.py scripts/scene/tests/test_tiling.py \
  scripts/scene/tests/test_ndvi_build.py
git commit -m "feat(scene): build the NDVI layer (fit stage, tile jobs, empty tiles, PNG tilemapresource)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01EuqoXt1sAk5mjhhppnKP9D"
```

---

### Task 6: verify — `ndvi_tilemapresource` and deep `ndvi_values`

**Files:**
- Modify: `scripts/scene/camsim_scene/verify.py`
- Test: `scripts/scene/tests/test_verify.py`

**Interfaces:**
- Consumes: `layers.ndvi.{read_png, encode, sample_ndvi, water_allow, reference_weight}`, `WorkerState.ndvi_*`.
- Produces: quick check `ndvi_tilemapresource` (only when NDVI tiles exist); deep check `ndvi_values`; report key
  `ndvi_error_steps` (`n`, `p50`, `p99`, `max`, `leaves`) when the package has an NDVI layer.

- [ ] **Step 1: Write the failing tests** (append to `scripts/scene/tests/test_verify.py`; add
  `import shutil`, `import numpy as np`, `from fake_sources import naip_s2_scene`, `from camsim_scene import config`
  and `from camsim_scene.layers import ndvi` to its imports)

```python
@pytest.fixture(scope="module")
def built_ndvi(tmp_path_factory):
    root = tmp_path_factory.mktemp("verify_ndvi")
    zoom = {"globe": {"terrain": 2, "imagery": 2}, "ring": {"terrain": 3, "imagery": 3}, "bbox": {"terrain": 3, "imagery": 11}}
    scene = {**naip_s2_scene(root / "src", ndvi=True), "zoom": zoom, "ndvi_max_zoom": 10}
    with pytest.MonkeyPatch.context() as mp:
        for d, k, v in ((config.BALANCE, "fit_step_m", 200.0), (config.BALANCE, "min_cell_samples", 20),
                        (config.NDVI, "fit_step_m", 200.0), (config.NDVI, "min_samples", 20)):
            mp.setitem(d, k, v)
        return build_synthetic(root, scene)


def test_ndvi_package_passes_deep_verify(built_ndvi):
    pkg, cache, _ = built_ndvi
    r = verify(pkg, cache, deep=True, all_tiles=True)
    assert r["ok"], [c for c in r["checks"] if not c["ok"]]
    c = checks(r)
    assert c["ndvi_tilemapresource"]["ok"] and c["ndvi_values"]["ok"]
    assert r["ndvi_error_steps"]["n"] > 0 and r["ndvi_error_steps"]["p99"] <= 1


def test_deep_catches_wrong_ndvi_values_that_hash_correctly(built_ndvi, tmp_path):
    pkg = copy_pkg(built_ndvi[0], tmp_path / "p")
    for p in (pkg / "ndvi/10").rglob("*.png"):
        code = ndvi.read_png(p.read_bytes()).astype(int)
        p.write_bytes(ndvi.png_bytes(np.where(code > 0, np.clip(code + 10, 1, 255), 0).astype(np.uint8)))
    m = Manifest.load(pkg / "manifest.json")
    m.hashes_sha256 = write_hashes(pkg)
    m.write(pkg / "manifest.json")
    r = verify(pkg, built_ndvi[1], deep=True, all_tiles=True)
    assert checks(r)["hashes"]["ok"] and not checks(r)["ndvi_values"]["ok"]


def test_a_missing_ndvi_level_fails_its_tilemapresource(built_ndvi, tmp_path):
    pkg = copy_pkg(built_ndvi[0], tmp_path / "p")
    shutil.rmtree(pkg / "ndvi/10")
    assert not checks(verify(pkg))["ndvi_tilemapresource"]["ok"]


def test_a_package_without_ndvi_has_no_ndvi_checks(built):
    r = verify(built[0], built[1], deep=True)
    assert "ndvi_values" not in checks(r) and "ndvi_tilemapresource" not in checks(r) and "ndvi_error_steps" not in r
```

- [ ] **Step 2: Run them to make sure they fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_verify.py -q`
Expected: the four new tests FAIL (`KeyError: 'ndvi_tilemapresource'` …); the old ones pass.

- [ ] **Step 3: Implement** (`verify.py`)

- Docstring: add "…decodes every JPEG, and checks a sample of NDVI leaf pixels against the sources (`ndvi_values`)."
- Imports: `from .sources.base import M_PER_DEG`; `from .tiling import LayerPlan, available_keys, make_keys, split_keys, tile_bounds`.
- Constants after `SAMPLE = 0.02`: `NDVI_PX = 64  # pixels checked per sampled NDVI leaf` and `NDVI_P99_STEPS = 1`.
- `_check_tms` becomes

```python
def _check_tms(pkg: Path, layer: str = "imagery", ext: str = "jpg", name: str = "tilemapresource") -> Check:
    levels, bounds = parse_tilemapresource((pkg / layer / "tilemapresource.xml").read_text())
    have = {z: k for z, k in present_tiles(pkg, layer, ext).items() if len(k)}
    problems = []
    if levels != list(range(max(have) + 1)):
        problems.append(f"levels {levels[:3]}..{levels[-1:]} but tiles to z{max(have)}")
    hb = plan_bounds(LayerPlan(have, {}))
    if any(abs(a - b) > 1e-9 for a, b in zip(bounds, hb)):
        problems.append(f"BoundingBox {bounds} != tiles {hb}")
    return Check(name, not problems, _summary(problems))
```

- After `_jpeg_bad` add:

```python
def _ndvi_leaves(present: dict[int, np.ndarray]) -> dict[int, np.ndarray]:
    """NDVI tiles with no tile below them (a parent is written only where a child holds data)."""
    out = {}
    for z, keys in present.items():
        kids = present.get(z + 1)
        if kids is None or not len(kids):
            out[z] = keys
            continue
        x, y = split_keys(kids)
        out[z] = keys[~np.isin(keys, np.unique(make_keys(x >> 1, y >> 1)))]
    return out


def _ndvi_values(pkg: Path, m: Manifest, st, rng: random.Random, all_tiles: bool) -> tuple[Check, dict]:
    """Sampled leaf pixels against NDVI recomputed from the sources at the pixel centre (the fit applied to the
    reference): pixels that are purely the reference (feather weight 1, or nothing behind it) or purely the rest;
    feather pixels are left out. Codes must agree within NDVI_P99_STEPS at p99, and nodata must be nodata."""
    from .layers import imagery, ndvi

    s = m.layers["ndvi"]
    present = {z: k for z, k in present_tiles(pkg, "ndvi", "png").items() if len(k)}
    errs, bad, leaves = [], [], 0
    for z, keys in sorted(_ndvi_leaves(present).items()):
        ks = keys.tolist()
        if not ks:
            continue
        for k in sorted(ks if all_tiles else rng.sample(ks, max(1, math.ceil(SAMPLE * len(ks))))):
            x, y = k >> 32, k & 0xFFFFFFFF
            try:
                code = ndvi.read_png((pkg / "ndvi" / str(z) / str(x) / f"{y}.png").read_bytes())
            except Exception as e:  # noqa: BLE001 - any decode failure is a finding
                bad.append(f"{z}/{x}/{y}: {e}")
                continue
            entries = st.index["ndvi"].query(imagery.query_bounds(z, x, y, s["feather_m"]))
            ref = [e for e in entries if imagery.source_of(e) == s["reference"]]
            rest = [e for e in entries if imagery.source_of(e) != s["reference"]]
            allow, dpx = ndvi.water_allow(z, x, y, st.ndvi_water, s["feather_m"])
            buffer_m = st.ndvi_water.buffer_m if st.ndvi_water is not None else 0.0
            wt = ndvi.reference_weight(z, x, y, ref, s["feather_m"], allow, dpx, buffer_m).ravel()
            (lon, lat), d = imagery.pixel_grid(z, x, y)
            idx = np.array(sorted(rng.sample(range(lon.size), NDVI_PX)))
            plon, plat, w, have = lon.ravel()[idx], lat.ravel()[idx], wt[idx], code.ravel()[idx]
            rn, rok, _ = ndvi.sample_ndvi(ref, plon, plat, d * M_PER_DEG)
            if st.ndvi_fit is not None:
                rn = st.ndvi_fit.apply(rn)
            sn, sok, _ = ndvi.sample_ndvi(rest, plon, plat, d * M_PER_DEG)
            use_ref = rok & ((w >= 1.0) | ~sok)
            use_rest = sok & (~rok | (w <= 0.0))
            want = np.where(use_ref, rn, np.where(use_rest, sn, np.nan))
            pure = use_ref | use_rest
            errs.append(np.abs(have[pure].astype(np.int64) - ndvi.encode(want[pure]).astype(np.int64)))
            if (have[~(rok | sok)] != 0).any():
                bad.append(f"{z}/{x}/{y}: data where the sources have none")
            leaves += 1
    e = np.concatenate(errs) if errs else np.zeros(0, np.int64)
    stats = {**_error_stats(e.astype(np.float64)), "leaves": leaves}
    ok = not bad and stats["p99"] <= NDVI_P99_STEPS
    detail = f"{stats['n']} pixels in {leaves} leaves: p99 {stats['p99']:.1f} steps, max {stats['max']:.0f}"
    if bad:
        detail += "; " + _summary(bad)
    return Check("ndvi_values", ok, detail), stats
```

- `_deep` returns a third value: before `checks = [...]` add

```python
    ndvi_stats = None
    ndvi_checks = []
    if m.layers.get("ndvi"):
        c, ndvi_stats = _ndvi_values(pkg, m, st, random.Random(m.seed + 1), all_tiles)
        ndvi_checks.append(c)
```

  then `checks += ndvi_checks` after the list, and `return checks, stats, ndvi_stats`.
- `verify()`: after the `attribution` guard,

```python
    if {z: k for z, k in present_tiles(pkg, "ndvi", "png").items() if len(k)}:
        checks.append(_guard("ndvi_tilemapresource", _check_tms, pkg, "ndvi", "png", "ndvi_tilemapresource"))
```

  and in the deep branch `deep_checks, report["terrain_error_m"], ndvi_stats = _deep(...)` followed by
  `if ndvi_stats is not None: report["ndvi_error_steps"] = ndvi_stats` (inside the `try`).

- [ ] **Step 4: Run the tests**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_verify.py -q` → pass; whole suite;
lint.

- [ ] **Step 5: Commit**

```bash
git add scripts/scene/camsim_scene/verify.py scripts/scene/tests/test_verify.py
git commit -m "feat(scene): verify the NDVI layer (tilemapresource; deep ndvi_values against the sources)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01EuqoXt1sAk5mjhhppnKP9D"
```

---

### Task 7: `ndvi_check.py` and documentation

**Files:**
- Create: `scripts/scene/tools/ndvi_check.py`, `scripts/scene/tests/test_ndvi_check.py`
- Modify: `docs/scene-packages.md`, `scripts/scene/tools/README.md`

**Interfaces:**
- Consumes: `layers.ndvi.{decode, read_png, sample_ndvi}`, `context.{WorkerState, class_rasters, reference_footprints}`, `water.class_codes`.
- Produces: `ndvi_check.class_order_failures(medians: dict[int, float]) -> list[str]`,
  `ndvi_check.seam_pairs(footprints, step_m) -> (inside (2, n), outside (2, n))`, `ndvi_check.colourise(v) -> (h, w, 3) uint8`,
  CLI `ndvi_check.py PKG --cache DIR [--overview OUT.png] [--step-m 100]` (exit 0 pass, 1 fail, 2 no NDVI layer).

- [ ] **Step 1: Write the failing tests** (`scripts/scene/tests/test_ndvi_check.py`)

```python
"""ndvi_check.py: the gate logic (the CLI runs in Task 8 on a real package)."""

from __future__ import annotations

import importlib.util
from pathlib import Path

import numpy as np
import shapely

_SPEC = importlib.util.spec_from_file_location(
    "ndvi_check", Path(__file__).resolve().parents[1] / "tools" / "ndvi_check.py"
)
ndvi_check = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(ndvi_check)

GOOD = {10: 0.6, 20: 0.4, 30: 0.35, 50: 0.1, 60: 0.05, 80: -0.2}


def test_class_order():
    assert ndvi_check.class_order_failures(GOOD) == []
    assert ndvi_check.class_order_failures({**GOOD, 10: 0.38})  # trees not above shrub/grass
    assert ndvi_check.class_order_failures({**GOOD, 60: 0.36})  # bare not below shrub/grass
    assert ndvi_check.class_order_failures({**GOOD, 80: 0.1})  # water not below 0
    assert ndvi_check.class_order_failures({k: v for k, v in GOOD.items() if k != 50}) == []  # a missing class is skipped


def test_too_few_classes_is_a_failure():
    assert ndvi_check.class_order_failures({80: -0.2}) == [
        "too few classes sampled (need tree cover, shrub or grass, built-up or bare)"
    ]


def test_seam_pairs_straddle_the_edge():
    sq = shapely.box(10.0, 10.0, 10.1, 10.1)
    ins, outs = ndvi_check.seam_pairs([sq], 500.0)
    assert ins.shape[0] == 2 and ins.shape[1] > 50 and ins.shape == outs.shape
    assert shapely.contains_xy(sq, *ins).all() and not shapely.contains_xy(sq, *outs).any()
    d_in = shapely.distance(sq.exterior, shapely.points(ins.T)) * 111320.0
    assert np.allclose(d_in, ndvi_check.INSIDE_M, rtol=0.05)


def test_colourise_marks_nodata():
    rgb = ndvi_check.colourise(np.array([[np.nan, -0.2, 0.8]]))
    assert rgb.shape == (1, 3, 3) and rgb[0, 0].tolist() == list(ndvi_check.NODATA_RGB)
    assert rgb[0, 2, 1] > rgb[0, 2, 0]  # dense vegetation is green
```

- [ ] **Step 2: Run them to make sure they fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_ndvi_check.py -q`
Expected: FAIL (`FileNotFoundError` for `tools/ndvi_check.py`).

- [ ] **Step 3: Implement** (`scripts/scene/tools/ndvi_check.py`)

```python
"""R1 chunk 2 NDVI gates (spec acceptance 3 and 4) and a false-colour overview for review.

    uv run --project scripts/scene python scripts/scene/tools/ndvi_check.py PKG --cache .cache/scene \
        [--overview OUT.png] [--step-m 100]

Values come from the package's NDVI tiles, the deepest tile holding each point (nearest pixel).
Gate 3 (plausibility): median NDVI per WorldCover class on a lattice over the bbox: tree cover (10) above
shrubland (20) and grassland (30), both above built-up (50) and bare (60), permanent water (80) below 0. Classes with
fewer than MIN_N samples are reported and left out of the comparisons.
Gate 4 (seam): land points every --step-m along NAIP's footprint edge, INSIDE_M inside (pure NAIP) and OUTSIDE_M
outside (pure Sentinel-2): |median(inside - outside)| <= SEAM_MAX. The same median with unfitted NAIP NDVI inside is
reported beside it.
Prints a JSON report; exits 1 when a gate fails, 2 when the package has no NDVI layer."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
import shapely
from PIL import Image

from camsim_scene.cache import Cache
from camsim_scene.context import WorkerState, class_rasters, reference_footprints
from camsim_scene.layers import ndvi
from camsim_scene.manifest import Manifest
from camsim_scene.pipeline import make_context
from camsim_scene.sources.base import M_PER_DEG
from camsim_scene.tiling import tile_size_deg
from camsim_scene.water import class_codes

MIN_N = 50
INSIDE_M, OUTSIDE_M = 250.0, 50.0
SEAM_MAX = 0.03
TREE, SHRUB, GRASS, BUILT, BARE, WATER = 10, 20, 30, 50, 60, 80
NODATA_RGB = (40, 60, 120)
OVERVIEW_PX = 2048


class NdviTiles:
    """NDVI at points from the deepest tile holding each one (nearest pixel; NaN where no tile)."""

    def __init__(self, pkg: Path):
        root = pkg / "ndvi"
        self.root = root
        self.zooms = sorted((int(p.name) for p in root.iterdir() if p.is_dir() and p.name.isdigit()), reverse=True)
        self._codes: dict = {}

    def _tile(self, z: int, x: int, y: int):
        key = (z, x, y)
        if key not in self._codes:
            p = self.root / str(z) / str(x) / f"{y}.png"
            self._codes[key] = ndvi.read_png(p.read_bytes()) if p.exists() else None
        return self._codes[key]

    def at(self, lon, lat) -> np.ndarray:
        lon, lat = np.asarray(lon, np.float64), np.asarray(lat, np.float64)
        out = np.full(lon.shape, np.nan)
        todo = np.ones(lon.shape, bool)
        for z in self.zooms:
            if not todo.any():
                break
            t = tile_size_deg(z)
            fx, fy = (lon + 180.0) / t, (lat + 90.0) / t
            x, y = np.floor(fx).astype(np.int64), np.floor(fy).astype(np.int64)
            col = np.clip(((fx - x) * 256).astype(np.int64), 0, 255)
            row = np.clip(((1.0 - (fy - y)) * 256).astype(np.int64), 0, 255)
            for i in np.flatnonzero(todo):
                code = self._tile(z, int(x.flat[i]), int(y.flat[i]))
                if code is not None:
                    out.flat[i] = ndvi.decode(code[row.flat[i], col.flat[i]])
                    todo.flat[i] = False
        return out


def class_order_failures(med: dict[int, float]) -> list[str]:
    tree = [med[c] for c in (TREE,) if c in med]
    mid = [med[c] for c in (SHRUB, GRASS) if c in med]
    low = [med[c] for c in (BUILT, BARE) if c in med]
    if not (tree and mid and low):
        return ["too few classes sampled (need tree cover, shrub or grass, built-up or bare)"]
    fails = []
    if not tree[0] > max(mid):
        fails.append(f"tree cover {tree[0]:.3f} not above shrub/grass {max(mid):.3f}")
    if not min(mid) > max(low):
        fails.append(f"shrub/grass {min(mid):.3f} not above built-up/bare {max(low):.3f}")
    if WATER in med and not med[WATER] < 0:
        fails.append(f"permanent water {med[WATER]:.3f} not below 0")
    return fails


def seam_pairs(footprints, step_m: float):
    """Points every step_m along the exterior of the footprints' union, moved INSIDE_M inward and OUTSIDE_M outward
    along the local normal (degrees of latitude on both axes, as the build); pairs whose inside point isn't inside or
    whose outside point isn't outside (corners, concave edges) are dropped. (inside (2, n), outside (2, n))."""
    union = shapely.union_all(list(footprints))
    step = step_m / M_PER_DEG
    ins, outs = [], []
    for poly in getattr(union, "geoms", [union]):
        ring = poly.exterior
        d = np.arange(int(ring.length / step)) * step
        p = shapely.get_coordinates(shapely.line_interpolate_point(ring, d))
        q = shapely.get_coordinates(shapely.line_interpolate_point(ring, d + 0.1 * step))
        t = q - p
        t /= np.linalg.norm(t, axis=1, keepdims=True)
        nrm = np.column_stack([-t[:, 1], t[:, 0]])
        probe = p + nrm * (INSIDE_M / M_PER_DEG)
        sign = np.where(shapely.contains_xy(union, probe[:, 0], probe[:, 1]), 1.0, -1.0)[:, None]
        ins.append(p + sign * nrm * (INSIDE_M / M_PER_DEG))
        outs.append(p - sign * nrm * (OUTSIDE_M / M_PER_DEG))
    a, b = np.concatenate(ins), np.concatenate(outs)
    keep = shapely.contains_xy(union, a[:, 0], a[:, 1]) & ~shapely.contains_xy(union, b[:, 0], b[:, 1])
    return a[keep].T, b[keep].T


def colourise(v: np.ndarray) -> np.ndarray:
    """NDVI -> brown (-0.2) / straw (0.2) / green (0.8); nodata NODATA_RGB."""
    stops = [-0.2, 0.2, 0.8]
    cols = np.array([[140, 100, 60], [220, 200, 120], [30, 140, 40]], np.float64)
    x = np.nan_to_num(v, nan=0.0)
    rgb = np.stack([np.interp(x, stops, cols[:, k]) for k in range(3)], axis=-1)
    rgb[np.isnan(v)] = NODATA_RGB
    return np.rint(rgb).astype(np.uint8)


def write_overview(tiles: NdviTiles, bbox, out: Path) -> None:
    w, s, e, n = bbox
    step = max(e - w, n - s) / OVERVIEW_PX
    lon, lat = np.meshgrid(np.arange(w + step / 2, e, step), np.arange(n - step / 2, s, -step))
    Image.fromarray(colourise(tiles.at(lon, lat))).save(out)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("pkg", type=Path)
    ap.add_argument("--cache", type=Path, required=True)
    ap.add_argument("--overview", type=Path)
    ap.add_argument("--step-m", type=float, default=100.0)
    a = ap.parse_args(argv)
    m = Manifest.load(a.pkg / "manifest.json")
    s = m.layers.get("ndvi")
    if not s or not (a.pkg / "ndvi").is_dir():
        print(json.dumps({"error": "the package has no NDVI layer"}))
        return 2
    ctx = make_context(a.pkg, m, Cache(a.cache))
    st = WorkerState(ctx)
    classes, _ = class_rasters(m, ctx.asset_paths)
    tiles = NdviTiles(a.pkg)
    report: dict = {"zooms": tiles.zooms[::-1], "fails": []}
    step = a.step_m / M_PER_DEG
    w, s_, e, n = m.bbox
    lon, lat = np.meshgrid(np.arange(w + step / 2, e, step), np.arange(s_ + step / 2, n, step))
    v = tiles.at(lon, lat)
    codes = class_codes(classes, lon, lat) if classes else np.zeros(lon.shape, np.uint8)
    med, counts = {}, {}
    for c in np.unique(codes).tolist():
        k = (codes == c) & np.isfinite(v)
        counts[c] = int(k.sum())
        if k.sum() >= MIN_N:
            med[c] = round(float(np.median(v[k])), 4)
    report["class_median"], report["class_n"] = med, counts
    report["fails"] += class_order_failures(med)
    if s["reference"]:
        fps = reference_footprints(m, s["reference"])
        ins, outs = seam_pairs(fps, a.step_m)
        keep = np.ones(ins.shape[1], bool)
        if classes:
            keep &= (class_codes(classes, *ins) != WATER) & (class_codes(classes, *outs) != WATER)
        vi, vo = tiles.at(*ins), tiles.at(*outs)
        keep &= np.isfinite(vi) & np.isfinite(vo)
        union = shapely.union_all(fps)
        ref = [x for x in st.index["ndvi"].query(union.bounds) if x.qid.startswith(s["reference"] + "/")]
        tm = tile_size_deg(tiles.zooms[0]) / 256 * M_PER_DEG
        raw, rok, _ = ndvi.sample_ndvi(ref, ins[0], ins[1], tm)
        kr = keep & rok
        bias = round(float(np.median(vi[keep] - vo[keep])), 4) if keep.any() else None
        report["seam"] = {
            "pairs": int(keep.sum()),
            "bias": bias,
            "bias_unfitted": round(float(np.median(raw[kr] - vo[kr])), 4) if kr.any() else None,
        }
        if bias is None:
            report["fails"].append("no seam pairs sampled")
        elif abs(bias) > SEAM_MAX:
            report["fails"].append(f"seam bias {bias:.4f} beyond {SEAM_MAX}")
    if a.overview:
        write_overview(tiles, m.bbox, a.overview)
    print(json.dumps(report, indent=1))
    return 1 if report["fails"] else 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Run the tests**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_ndvi_check.py -q` → pass; lint.

- [ ] **Step 5: Documentation**

`scripts/scene/tools/README.md`: add a row after `water_check.py`:
`| ndvi_check.py | R1 chunk 2 NDVI gates: median NDVI per WorldCover class (plausibility), the bias across NAIP's edge (seam), --overview false-colour PNG |`

`docs/scene-packages.md`:

1. Package layout block: after the `imagery/{z}/{x}/{y}.jpg` line add

```
  ndvi/tilemapresource.xml      # TMS 1.0, image/png (packages with an NDVI layer)
  ndvi/fit.json                 # NAIP -> Sentinel-2 NDVI fit
  ndvi/{z}/{x}/{y}.png          # 8-bit grayscale NDVI (0 nodata, 1..255 = -1..+1); tiles without data are absent
```

2. Standards table, TMS row: `| TMS 1.0 (OSGeo Tile Map Service) tilemapresource.xml | imagery/ and ndvi/ (SRS EPSG:4326, profile geodetic) |`
3. `scene.toml` reference block: after `naip_water_buffer_m = 200 …` add

```toml
ndvi = true                                 # NDVI layer (default true in sim, false in preview; NDVI section)
ndvi_max_zoom = 15                          # [10, 17]: NDVI pyramid depth (z15 ~ 2.4 m)
```

4. A new section after "Imagery edge (R1)" (before "## Sources"):

```markdown
## NDVI (R1)

**Why.** A vegetation signal for thermal IR and vegetation placement that doesn't come from JPEG colour: R2 compares
it with 4B's GBuffer ExG in `ThermalCS`, R4 places trees with it. Nothing in CamSim reads it yet.

**Values.** `(NIR - red) / (NIR + red)` per pixel from each source's raw values: NAIP DN bands 4 and 1, Sentinel-2
reflectance B08 and B04 (the composite's bands 4 and 1). 8-bit grayscale PNG: code 0 = nodata, codes 1-255 = NDVI
-1 to +1 (`ndvi = (code - 1) / 127 - 1`, step ~0.0079).

**Tiles.** The imagery's grid and addressing, from z0 to min(`ndvi_max_zoom`, the imagery tile's depth) (default
z15, ~2.4 m), only where the tile overlaps the ring and NAIP or Sentinel-2 data (Blue Marble has no NIR). Tiles with
no valid pixel are not written; `tilemapresource.xml` lists the zooms and bounds of the tiles present. Parents are the
mean of the valid child pixels in each 2 x 2 block.

**Merge.** As the imagery: NAIP first, Sentinel-2 behind it; NAIP feathers into Sentinel-2 over 200 m inside its
valid-data edge and is used only within `naip_water_buffer_m` of WorldCover land. Open sea beyond the composite's land
tiles is nodata.

**Fit (`ndvi/fit.json`).** NAIP NDVI is computed from uncalibrated DN; one linear map, `gain x ndvi + offset` (then
clamped to [-1, 1]), puts it on Sentinel-2's scale. Least squares on the colour match's 10 m land lattice
(WorldCover 0 and 80 excluded, even nodes fitted, odd held out, >= 500 samples), stored in integer millionths,
hashed, and joined to every NDVI leaf's inputs. `build.json` -> `ndvi`: samples, gain, offset, held-out MAE and median
bias before/after, and the residual median bias per NAIP acquisition date. No shared land: no `fit.json`, NAIP NDVI
unfitted (warning). A rebuild with unchanged inputs skips the fit.

**Off switch.** `ndvi = false` (the `preview` default): no `ndvi/`; every other file is unchanged. Setting
`ndvi = true` without `naip_pc` or `wc_s2` in the imagery priorities is an error; `preview` with `ndvi = true` gets a
Sentinel-2-only layer (no fit). A package planned before this layer has none until `build --replan`, which rebuilds
only the NDVI tiles.

**Acquisition date and sun.** Each NAIP asset in `manifest.json` records `acquired` (the date), `sun_noon`
(`elevation_deg`, `azimuth_deg` at local solar noon) and `sun_window` (`min_elevation_deg` 30, `azimuth_deg`
[morning, afternoon]: where the sun is when it crosses 30 degrees), at the centre of the quad's bbox, NOAA equations,
no refraction. Planetary Computer's NAIP times are a placeholder (`T16:00:00Z` on every quad), so the true sun is
unknown: NAIP flies with the sun at 30 degrees or more, so its elevation was between 30 degrees and
`sun_noon.elevation_deg`, and its azimuth within `sun_window.azimuth_deg`. At Pendleton on 2022-05-30 that window is
82-278 degrees, so in summer the azimuth is barely constrained. Imagery shadows are baked in for that sun (a known
limit, not corrected). Sentinel-2 assets record `composite: "2021"` (a year's median: no single sun). Asset metadata
doesn't feed tile hashes, so re-planning adds it without rebuilding tiles.

**Known limits.** NAIP NDVI is from DN: the fit matches Sentinel-2 on average, not per pixel. One global fit:
per-flight-date differences are reported, not corrected. NDVI is leaf-on and dated (NAIP flies in the growing
season; the composite is 2021). Outside NAIP it is 10 m Sentinel-2.

`scripts/scene/tools/ndvi_check.py PKG --cache DIR [--overview OUT.png]` computes the plausibility and seam gates.
```

5. verify section: quick table add
   `| ndvi_tilemapresource | (packages with NDVI tiles) ndvi/tilemapresource.xml levels and BoundingBox equal the NDVI tiles present |`;
   deep table add
   `| ndvi_values | 64 pixels of each sampled NDVI leaf (same 2 % / --all sampling) against NDVI recomputed from the sources at the pixel centre (fit applied to NAIP); pixels purely NAIP or purely Sentinel-2 (feather pixels left out): p99 <= 1 code step, and no data where the sources have none |`;
   and add to the paragraph after the deep table: "`ndvi_error_steps` records the NDVI code errors (`n`, `p50`,
   `p99`, `max`, `leaves`)."

- [ ] **Step 6: Commit**

```bash
git add scripts/scene/tools/ndvi_check.py scripts/scene/tests/test_ndvi_check.py scripts/scene/tools/README.md docs/scene-packages.md
git commit -m "docs(scene): NDVI layer and sun metadata; ndvi_check gate tool

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01EuqoXt1sAk5mjhhppnKP9D"
```

---

### Task 8: Acceptance on Camp Pendleton

Long-running; runs on the Mac with the populated cache (`.cache/scene`; NAIP and Sentinel-2 COGs already fetched, so
no new downloads are expected; re-planning re-runs discovery, which needs the network for STAC/HEAD requests). No
code changes unless a gate fails — then stop and report rather than tuning constants. `-j 6` throughout (as R1 chunk
1).

**Files:**
- Modify: `docs/scene-packages.md` ("Measured": a new "R1 chunk 2" table), `REALISM.md`, `ROADMAP.md`

- [ ] **Step 1: Start from the chunk 1 build** (keeps its terrain and imagery tiles, which must all be skipped)

```bash
cp -a .cache/scene-packages/pendleton-r1b .cache/scene-packages/pendleton-ndvi
uv run --project scripts/scene camsim-scene --cache .cache/scene build .cache/scene-packages/pendleton-ndvi \
  --config scripts/scene/examples/pendleton.toml --replan -j 6 2>&1 | tail -20
```

Expected: completes; `build.json`: `layers.terrain.built` 0, `layers.imagery.built` 0, `ndvi.fitted` true,
`layers.ndvi.files` > 0. Record wall time, NDVI tiles/files/bytes/seconds, peak RSS, fit seconds, gain, offset.

- [ ] **Step 2: Fit gate** (acceptance 2)

Run: `python3 -c "import json; print(json.dumps(json.load(open('.cache/scene-packages/pendleton-ndvi/build.json'))['ndvi'], indent=1))"`
Expected: `heldout.bias_after` within ±0.02; record MAE and bias before/after and `bias_after_by_date` for
2022-04-25, 05-12 and 05-30. Any date with |bias| > 0.03 → note it as a follow-up (not a failure).

- [ ] **Step 3: Sun metadata**

Run: `python3 -c "import json; m=json.load(open('.cache/scene-packages/pendleton-ndvi/manifest.json')); a=[a for s in m['sources'] if s['id']=='naip_pc' for a in s['assets']]; print(len(a), {x['metadata']['acquired'] for x in a}, a[0]['metadata']['sun_noon'], a[0]['metadata']['sun_window'])"`
Expected: 44 assets, the three dates, elevations ~76–79°, windows ~[82, 278].

- [ ] **Step 4: Plausibility and seam gates** (acceptance 3, 4)

```bash
mkdir -p .cache/ndvi_check
uv run --project scripts/scene python scripts/scene/tools/ndvi_check.py .cache/scene-packages/pendleton-ndvi \
  --cache .cache/scene --overview .cache/ndvi_check/pendleton.png | tee .cache/ndvi_check/report.json
```

Expected: exit 0, `fails: []`; record `class_median`, `class_n`, `seam` (bias and bias_unfitted, pairs).

- [ ] **Step 5: verify and determinism** (acceptance 1, 5, 6)

```bash
uv run --project scripts/scene camsim-scene --cache .cache/scene verify .cache/scene-packages/pendleton-ndvi --deep -j 6
mkdir -p .cache/scene-packages/pendleton-ndvi-b
cp .cache/scene-packages/pendleton-ndvi/manifest.json .cache/scene-packages/pendleton-ndvi-b/
uv run --project scripts/scene camsim-scene --cache .cache/scene build .cache/scene-packages/pendleton-ndvi-b -j 1
cmp .cache/scene-packages/pendleton-ndvi/hashes.txt .cache/scene-packages/pendleton-ndvi-b/hashes.txt && echo IDENTICAL
uv run --project scripts/scene camsim-scene --cache .cache/scene build .cache/scene-packages/pendleton-ndvi -j 6 2>&1 | tail -3
python3 - <<'EOF'
a = [l for l in open('.cache/scene-packages/pendleton-ndvi/hashes.txt') if not l.split('  ', 1)[1].startswith('ndvi/')]
b = open('.cache/scene-packages/pendleton-r1b/hashes.txt').readlines()
print('OFF-SWITCH IDENTICAL' if a == b else f'DIFFER: {len(a)} vs {len(b)} lines')
EOF
```

Expected: every verify check `ok` including `ndvi_values` (record `ndvi_error_steps`); `IDENTICAL` (the `-j 1`
build from the manifest alone; it rebuilds terrain and imagery too, so it takes longer); the second `-j 6` build
skips every tile (`built` 0 in all layers); `OFF-SWITCH IDENTICAL` (all non-NDVI files equal the chunk 1 package).

- [ ] **Step 6: Review the overview** — open `.cache/ndvi_check/pendleton.png` with the user (green canopy along the
  Santa Margarita river, brown chaparral/bare, no visible step where NAIP ends inland, nodata blue only over open
  sea). Ask the user to look before marking this done.

- [ ] **Step 7: Record and commit** — add an "R1 chunk 2 (NDVI and sun metadata)" table to `docs/scene-packages.md`
  "Measured" (gates 1–7 with the numbers above, the build cost per NDVI tile and the layer size), update the
  `REALISM.md` status paragraph ("R1 chunk 2 (NDVI + sun metadata) accepted on Pendleton <date>: …; Next: the rest of
  R1 (coastline vs sea level, Linux/Docker `file://`, R1 gates)") and the R1 bullet "NAIP also has a near-infrared
  band…" (mark done, pointing at the NDVI section), and the `ROADMAP.md` realism paragraph (one sentence with the
  results and any per-date follow-up).

```bash
git add docs/scene-packages.md REALISM.md ROADMAP.md
git commit -m "docs(scene): R1 chunk 2 acceptance on Camp Pendleton (NDVI layer, sun metadata)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01EuqoXt1sAk5mjhhppnKP9D"
```
