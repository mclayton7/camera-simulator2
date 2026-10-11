# R2 Vector Features Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Burn OSM roads, rail and paved areas, plus NHD perennial water, into scene-package land-cover tiles as codes
200–220, so that thermal IR draws them and CIGI HAT/HOT and LOS extended responses report the land-cover code at the hit
point.

**Architecture:**
- **Scene tooling.** Two new source adapters (`osm_geofabrik`, `nhd_hr`) form a fourth source layer, `features`.
  `layers/features.py` turns their vectors into polygons and burns them into the WorldCover tiles after the cut, inside the
  imagery margin only.
- **Runtime.** New codes get default classes (asphalt, concrete, new `rail_ballast`, water) and refinement families. A
  game-thread `FLandCoverCodeLookup` plus one pure rule, `QueryMaterialCode`, fill the CIGI material code.
- **No shader change.**

**Tech Stack:** Python 3.12–3.13 (uv project `scripts/scene`: numpy, shapely, rasterio, pyproj, Pillow; new: `osmium`
(pyosmium)), stdlib `sqlite3` for GeoPackage; UE 5.8 C++ (automation tests, NullRHI); `scripts/thermal_check.py`.

**Spec:** `docs/superpowers/specs/2026-10-10-r2-vector-features-design.md`. Read it first: the code tables, width rules,
skip rules and gates are defined there and copied below where a task needs them.

## Global Constraints

- **Codes:** 200 = paved asphalt/unknown surface, 201 = paved concrete, 210 = rail bed, 220 = inland water. 200–239 are
  reserved for vector features. WorldCover stays 10–100, and 0 means no data.
- **Format:** the land-cover format string stays `camsim-landcover-1`. The new `index.json` key `codes` is ignored by the
  runtime.
- **Features off:** with all `[features]` flags off (the default), `manifest.json` and `landcover/` are byte-identical to
  today's build.
- **Determinism:** two builds from the same manifest and cache are byte-identical. Features are sorted by
  (priority, source id) before burning.
- **Licences:** OSM is ODbL-1.0 and fails `plan` unless `allow` contains `ODbL-1.0`. NHD is
  `LicenseRef-PublicDomain-USGov`.
- **Burn area:** the burn covers the imagery margin area (`plan.area("margin")`) only. WorldCover outside it is untouched.
- **Raster rule:** the centre-sample rule (`all_touched=False`). Line features are buffered to a width of at least
  1.05 × the cell's longer side (1/12000° of latitude = 9.2767 m, so 9.74 m). Polygons are burned as they are.
- **Skipped features:** `bridge` ≠ no, `tunnel` ≠ no, `covered=yes`, `layer<0`, `location=underground`.
- **Bit-for-bit 4A:** `thermal.land_cover.enabled: false` stays 4A bit for bit, and no `.usf`/`.ush` file changes.
- **New material:** `rail_ballast` is appended as built-in index 14 (albedo 0.20, ε 0.93, inertia 900, h_c 10,
  k_fast 0.025, unless Task 6's literature check changes them). The built-in count becomes 15.
- **CIGI material rule:** invalid → 0; sea surface → 80; entity hit → 0; otherwise the land-cover code at the hit point
  (0 when unknown).
- **No GDAL:** no system GDAL or PDAL in `scripts/scene`. pytest runs without network.
- **C++ style:** the copyright header `// Copyright CamSim Contributors. All Rights Reserved.` and UE naming.
- **Commits:** end every message with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **A road crossing a tile edge.** Each tile burns its own clip, and the strip must be continuous across the seam. Test
   in Task 4: `test_road_across_tile_seam_is_continuous`.
2. **An OSM way only partly inside the margin.** It burns up to the margin edge, and only there. Test in Task 4:
   `test_burn_stops_at_margin`.
3. **A package built before R2.** Its manifest has no `layers.landcover.features`. It must rebuild with no change and
   `verify` must pass. Test in Task 5: `test_pre_r2_manifest_builds_unchanged`.
4. **Land cover enabled with a `dir` that has no `index.json`.** HAT/HOT still answers, with material 0 and no crash and
   no log spam. Test in Task 7: `CamSim.Thermal.LandCover.CodeLookup.MissingDir`.
5. **A HAT/HOT point exactly on a tile boundary or the antimeridian.** The lookup picks the same cell as the window
   resample (`GlobalCell`). Test in Task 7: `CamSim.Thermal.LandCover.CodeLookup.EdgesMatchResample`.

---

## File map

**Scene tooling** (`scripts/scene/`):

| File | Change |
|---|---|
| `camsim_scene/sources/base.py` | `Layer.FEATURES` |
| `camsim_scene/config.py` | `[features]` parsing, `features` priorities, `features_settings()`, `layer_settings` |
| `camsim_scene/pipeline.py` | `plan_scene` accepts the features layer; `_build_landcover` burns |
| `camsim_scene/sources/__init__.py` | registers `osm_geofabrik` and `nhd_hr` |
| `camsim_scene/sources/osm_geofabrik.py` | new: extract choice, dated snapshot URL, PBF → feature records |
| `camsim_scene/sources/nhd_hr.py` | new: TNM discovery, GPKG → water records |
| `camsim_scene/features.py` | new: feature records, width and surface rules, polygons, burn into one tile |
| `camsim_scene/layers/landcover.py` | `build_index(..., codes=, extra_credits=)` |
| `camsim_scene/verify.py` | legend check in `--deep` |
| `examples/pendleton.toml` | `[features]` on, `allow = ["ODbL-1.0"]` |
| `pyproject.toml`, `uv.lock` | `osmium` |
| `tools/road_registration.py` | new: gate 2 |
| tests | `tests/test_features.py`, `tests/test_osm_geofabrik.py`, `tests/test_nhd_hr.py`, `tests/test_road_registration.py`, edits to `test_config.py`, `test_pipeline.py`, `test_verify.py` |

**Runtime** (`unreal_project/CamSimTest/Source/CamSimTest/`):

| File | Change |
|---|---|
| `Thermal/LandCoverClasses.cpp` | defaults for 200, 201, 210, 220 |
| `Thermal/ThermalMaterials.{h,cpp}` | `rail_ballast` = 14 |
| `Thermal/LandCoverLookup.{h,cpp}` | new: `FLandCoverCodeLookup`, `QueryMaterialCode` |
| `Subsystem/CamSimSubsystem.{h,cpp}` | owns the lookup |
| `CIGI/CigiQueryHandler.cpp` | material in HAT/HOT and LOS extended |
| tests | `Tests/LandCoverLookupTest.cpp` (new), edits to `ThermalLandCoverMaterialsTest.cpp`, `ThermalLandCoverConfigTest.cpp`, `ThermalConfigTest.cpp` |

**Scripts and docs:**

- `scripts/thermal_check.py`: the `roads` run and gate r.
- Docs: `docs/scene-packages.md`, `docs/thermal.md`, `docs/terrain-feedback.md`, `docs/configuration.md`, `ROADMAP.md`,
  `REALISM.md`, `CLAUDE.md`.

## How to run things

- Scene tests: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests -q` (from the repo root).
- Build: `scripts/run.sh --build-only`.
- UE tests (macOS):

  ```bash
  "$UE_BIN" unreal_project/CamSimTest/CamSimTest.uproject -ExecCmds="Automation RunTests CamSim.Thermal+CamSim.CIGI+Quit" \
    -TestExit="Automation Test Queue Empty" -ReportExportPath=.cache/automation-report \
    -unattended -nullrhi -nosound -nosplash -DisablePython -log -stdout -FullStdOutLogOutput
  ```

  Then read `.cache/automation-report/index.json` with `encoding="utf-8-sig"`.

---

### Task 1: `features` source layer and `[features]` config

**Files:**
- Modify: `scripts/scene/camsim_scene/sources/base.py` (the `Layer` enum)
- Modify: `scripts/scene/camsim_scene/config.py`
- Modify: `scripts/scene/camsim_scene/pipeline.py:94-113` (the `plan_scene` layer loop)
- Test: `scripts/scene/tests/test_config.py`

**Interfaces:**
- Produces: `Layer.FEATURES == "features"`; `config.LAYERS == ("terrain", "imagery", "landcover", "features")`;
  `ScenePlan.features: dict` (keys `roads`, `paved_areas`, `rail`, `water` (bools), `road_classes` (list[str]));
  `config.features_settings(plan) -> dict | None`. Its dict holds:
  - `area`: [W, S, E, N] of `plan.area("margin")`;
  - `sources`: the feature source ids in burn order;
  - `roads`, `paved_areas`, `rail`, `water`, `road_classes`;
  - `rules`: `features.RULES_VERSION`.

  `layer_settings(plan)["landcover"]["features"]` is set only when `features_settings` is not None. Feature priorities are
  `plan.priorities["features"]`: `["nhd_hr"]` if water, then `["osm_geofabrik"]` if any OSM flag.

- [ ] **Step 1: Write failing tests** (append to `tests/test_config.py`)

```python
from camsim_scene.config import ConfigError, features_settings, layer_settings, parse_scene
import pytest

BASE = {"name": "t", "bbox": [-117.5, 33.2, -117.4, 33.3]}


def test_features_default_off_keeps_layer_settings():
    plan = parse_scene(dict(BASE))
    assert plan.priorities["features"] == []
    assert features_settings(plan) is None
    assert "features" not in layer_settings(plan)["landcover"]


def test_features_flags_set_priorities_and_settings():
    plan = parse_scene({**BASE, "allow": ["ODbL-1.0"], "features": {"roads": True, "water": True}})
    assert plan.priorities["features"] == ["nhd_hr", "osm_geofabrik"]
    s = layer_settings(plan)["landcover"]["features"]
    assert s["roads"] and s["water"] and not s["rail"] and not s["paved_areas"]
    assert s["sources"] == ["nhd_hr", "osm_geofabrik"]
    assert s["area"] == list(plan.area("margin"))
    assert s["road_classes"][0] == "motorway"


def test_water_only_needs_no_osm():
    plan = parse_scene({**BASE, "features": {"water": True}})
    assert plan.priorities["features"] == ["nhd_hr"]


@pytest.mark.parametrize("bad", [{"roads": 1}, {"bogus": True}, {"road_classes": ["motorway", "footway"]}])
def test_features_validation(bad):
    with pytest.raises(ConfigError):
        parse_scene({**BASE, "features": bad})


def test_features_not_settable_through_priorities():
    with pytest.raises(ConfigError):
        parse_scene({**BASE, "priorities": {"features": ["osm_geofabrik"]}})
```

- [ ] **Step 2: Run the tests and watch them fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_config.py -q -k features`
Expected: FAIL (`ImportError: cannot import name 'features_settings'`)

- [ ] **Step 3: Implement**

In `sources/base.py`, add `FEATURES = "features"` to `Layer`.

In `config.py`:

```python
LAYERS = ("terrain", "imagery", "landcover", "features")
# add "features" to KEYS
ROAD_CLASSES = ("motorway", "trunk", "primary", "secondary", "tertiary", "unclassified", "residential")
ROAD_CLASSES_ALLOWED = ROAD_CLASSES + ("service", "track", "living_street")
FEATURE_FLAGS = ("roads", "paved_areas", "rail", "water")
OSM_FLAGS = ("roads", "paved_areas", "rail")


def _parse_features(data) -> dict:
    f = data.get("features", {})
    if not isinstance(f, dict):
        raise ConfigError("features must be a table")
    unknown = set(f) - set(FEATURE_FLAGS) - {"road_classes"}
    if unknown:
        raise ConfigError(f"unknown key(s) in features: {sorted(unknown)}")
    out = {}
    for k in FEATURE_FLAGS:
        v = f.get(k, False)
        if not isinstance(v, bool):
            raise ConfigError(f"features.{k} must be true or false, got {v!r}")
        out[k] = v
    rc = f.get("road_classes", list(ROAD_CLASSES))
    if not isinstance(rc, list) or not rc or not all(c in ROAD_CLASSES_ALLOWED for c in rc):
        raise ConfigError(f"features.road_classes must be a non-empty list from {ROAD_CLASSES_ALLOWED}")
    out["road_classes"] = list(rc)
    return out
```

In `parse_scene`:
- Reject `"features"` in `data["priorities"]` with `ConfigError("priorities.features is derived from [features]; set the flags instead")`. Do this before the loop that copies priorities.
- Call `features = _parse_features(data)` and set
  `prio["features"] = (["nhd_hr"] if features["water"] else []) + (["osm_geofabrik"] if any(features[k] for k in OSM_FLAGS) else [])`.
- Add the field `features: dict = field(default_factory=dict)` to `ScenePlan` and pass `features=features`.
- Add `"features": []` to both profiles' `priorities` so that `prio` always has the key.

```python
def features_settings(plan: ScenePlan) -> dict | None:
    """The vector-feature burn's settings (REALISM R2), or None when every flag is off."""
    from .features import RULES_VERSION

    if not plan.priorities["features"]:
        return None
    return {
        "area": list(plan.area("margin")),
        "sources": list(plan.priorities["features"]),
        **{k: plan.features[k] for k in FEATURE_FLAGS},
        "road_classes": list(plan.features["road_classes"]),
        "rules": RULES_VERSION,
    }
```

In `layer_settings`, after the dict is built: `fs = features_settings(plan)`, and if it isn't None set
`out["landcover"]["features"] = fs`.

Create `camsim_scene/features.py` holding only a docstring and `RULES_VERSION = "1"` for now (Task 4 fills it).

In `pipeline.plan_scene`, the existing loop over `LAYERS` now includes `features`. The `src.layer != layer` check already
holds. Nothing else changes there.

- [ ] **Step 4: Run the tests**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests -q`
Expected: all pass. Existing tests that build `ScenePlan` by hand may need `priorities["features"] = []`; add it where a
test constructs priorities directly.

- [ ] **Step 5: Commit**

```bash
git add scripts/scene && git commit -m "feat(scene): features source layer and [features] config (REALISM R2)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: `osm_geofabrik` source

**Files:**
- Create: `scripts/scene/camsim_scene/sources/osm_geofabrik.py`
- Modify: `scripts/scene/camsim_scene/sources/__init__.py` (`BUILTIN["osm_geofabrik"] = "camsim_scene.sources.osm_geofabrik:OsmGeofabrik"`)
- Modify: `scripts/scene/pyproject.toml` (add `"osmium>=4.0"`), then `uv lock --project scripts/scene`
- Test: `scripts/scene/tests/test_osm_geofabrik.py`, `scripts/scene/tests/fixtures/http/geofabrik-index.json` (small: 3
  extracts)

**Interfaces:**
- Consumes: `Layer.FEATURES` (Task 1).
- Produces:
  - `OsmGeofabrik.discover(area) -> [Asset(id="<extract>-<yymmdd>", url=<dated pbf url>, role="data", metadata={"extract": id, "date": "YYYY-MM-DD"})]`;
  - `OsmGeofabrik.prepare(...) -> None`;
  - `read_osm(pbf: Path, area: Bounds, settings: dict) -> list[features.Feature]`, which uses `features.Feature` (Task 4).
    Task 2 defines `Feature` in `features.py` now so it can be imported here.

  ```python
  @dataclass(frozen=True)
  class Feature:
      kind: str            # "road" | "paved_area" | "rail" | "water"
      source_id: str       # "osm:w123", "osm:a456", "nhd:<permanent id>"
      geom: object         # shapely geometry in lon/lat (LineString / Polygon / MultiPolygon)
      tags: dict           # the tags the rules read (str -> str)
  ```

- [ ] **Step 1: Confirm Geofabrik's live layout (one-off, network)**

```bash
curl -s https://download.geofabrik.de/index-v1.json | python3 -c "import json,sys; d=json.load(sys.stdin); f=[x for x in d['features'] if x['properties']['id']=='socal'][0]; print(f['properties']['urls'])"
curl -sI https://download.geofabrik.de/north-america/us/california/socal-$(date -u +%y%m%d).osm.pbf | head -1
```

Expected: `urls.pbf` = `https://download.geofabrik.de/north-america/us/california/socal-latest.osm.pbf`, and the dated
file returns 200, or 404 before Geofabrik's daily build (try yesterday's date). Record the dated-name pattern in the
module docstring. If dated files aren't there, fall back to `-latest` and rely on the cache hash (the manifest still pins
the sha256).

- [ ] **Step 2: Write failing tests**

```python
import json
from pathlib import Path

import osmium
import pytest
from fakes import FakeHttp, fixture_json

from camsim_scene.features import Feature
from camsim_scene.sources import make_source
from camsim_scene.sources.base import Area
from camsim_scene.sources.osm_geofabrik import INDEX_URL, dated_url, read_osm

SETTINGS = {"roads": True, "paved_areas": True, "rail": True, "road_classes": ["motorway", "residential"]}


def test_smallest_extract_containing_the_area_wins():
    http = FakeHttp({INDEX_URL: fixture_json("geofabrik-index.json")})
    src = make_source("osm_geofabrik", options={"date": "2026-10-09"}, http=http)
    [a] = src.discover(Area((-117.6, 33.2, -117.3, 33.5)))
    assert a.metadata["extract"] == "socal"
    assert a.url == "https://download.geofabrik.de/north-america/us/california/socal-261009.osm.pbf"


def test_forced_extract_must_contain_the_area():
    http = FakeHttp({INDEX_URL: fixture_json("geofabrik-index.json")})
    src = make_source("osm_geofabrik", options={"extract": "norcal", "date": "2026-10-09"}, http=http)
    with pytest.raises(ValueError, match="does not contain"):
        src.discover(Area((-117.6, 33.2, -117.3, 33.5)))


def test_dated_url():
    assert dated_url("https://x/a/socal-latest.osm.pbf", "2026-01-02") == "https://x/a/socal-260102.osm.pbf"


def _write_pbf(path: Path):
    w = osmium.SimpleWriter(str(path))
    pts = {1: (-117.50, 33.30), 2: (-117.49, 33.30), 3: (-117.48, 33.30), 4: (-117.40, 34.50), 5: (-117.39, 34.50)}
    for nid, (lon, lat) in pts.items():
        w.add_node(osmium.osm.mutable.Node(id=nid, location=(lon, lat)))
    w.add_way(osmium.osm.mutable.Way(id=10, nodes=[1, 2], tags={"highway": "motorway", "lanes": "3"}))
    w.add_way(osmium.osm.mutable.Way(id=11, nodes=[2, 3], tags={"highway": "motorway", "bridge": "yes"}))
    w.add_way(osmium.osm.mutable.Way(id=12, nodes=[1, 3], tags={"highway": "footway"}))
    w.add_way(osmium.osm.mutable.Way(id=13, nodes=[4, 5], tags={"highway": "motorway"}))  # outside the area
    w.add_way(osmium.osm.mutable.Way(id=14, nodes=[1, 3], tags={"railway": "rail"}))
    w.close()


def test_read_osm_keeps_wanted_ways_in_area(tmp_path):
    pbf = tmp_path / "t.osm.pbf"
    _write_pbf(pbf)
    feats = read_osm(pbf, (-117.6, 33.2, -117.3, 33.5), SETTINGS)
    ids = sorted((f.kind, f.source_id) for f in feats)
    assert ids == [("rail", "osm:w14"), ("road", "osm:w10"), ("road", "osm:w11")]  # the bridge is dropped later, by the rules
    assert all(isinstance(f, Feature) for f in feats)
```

The fixture `geofabrik-index.json` is a GeoJSON FeatureCollection with three features:

| id | polygon | `urls.pbf` |
|---|---|---|
| `california` | (-125, 32) to (-114, 42.1) | `https://download.geofabrik.de/north-america/us/california-latest.osm.pbf` |
| `socal` | (-121, 32.5) to (-114, 35.8) | `https://download.geofabrik.de/north-america/us/california/socal-latest.osm.pbf` |
| `norcal` | (-125, 35.8) to (-117, 42.1) | `https://download.geofabrik.de/north-america/us/california/norcal-latest.osm.pbf` |

Each has `properties: {"id": ..., "urls": {"pbf": ...}}`.

- [ ] **Step 3: Run the tests and watch them fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_osm_geofabrik.py -q`
Expected: FAIL (`ModuleNotFoundError`)

- [ ] **Step 4: Implement**

```python
"""OpenStreetMap through Geofabrik extracts (ODbL 1.0; REALISM R2). Discovery picks the smallest extract whose polygon
contains the area (or the configured `extract`) and pins the dated daily file; the cache stores it by sha256, which is
the snapshot. read_osm streams the PBF with pyosmium and returns the roads, paved areas and rail the burn needs."""

from __future__ import annotations

import datetime as dt
import re
from pathlib import Path

import shapely
from shapely.geometry import box, shape

from ..features import Feature
from .base import Area, Asset, Layer, SourceBase

INDEX_URL = "https://download.geofabrik.de/index-v1.json"
ROAD_KEYS = ("highway",)
AREA_TAGS = (("amenity", "parking"), ("aeroway", "apron"))


def dated_url(latest_url: str, date: str) -> str:
    d = dt.date.fromisoformat(date)
    return re.sub(r"-latest\.osm\.pbf$", f"-{d:%y%m%d}.osm.pbf", latest_url)


class OsmGeofabrik(SourceBase):
    id = "osm_geofabrik"
    layer = Layer.FEATURES
    licence = "ODbL-1.0"
    dataset = "OpenStreetMap (Geofabrik extract)"
    attribution = "© OpenStreetMap contributors"
    area_kind = "margin"

    def discover(self, area: Area) -> list[Asset]:
        idx = self.http.get_json(INDEX_URL, {})
        want = box(*area.bounds)
        forced = self._options.get("extract")
        cands = []
        for f in idx["features"]:
            p = f["properties"]
            if forced and p["id"] != forced:
                continue
            if f.get("geometry") is None or "pbf" not in p.get("urls", {}):
                continue
            g = shape(f["geometry"])
            if g.contains(want):
                cands.append((g.area, p["id"], p["urls"]["pbf"]))
        if not cands:
            if forced:
                raise ValueError(f"Geofabrik extract {forced!r} does not contain {area.bounds}")
            raise ValueError(f"no Geofabrik extract contains {area.bounds}")
        _, ext, url = min(cands)
        date = self._options.get("date") or dt.datetime.now(dt.UTC).date().isoformat()
        self.version = date
        return [Asset(id=f"{ext}-{date}", url=dated_url(url, date), group=self.id,
                      metadata={"extract": ext, "date": date})]

    def prepare(self, path, asset, cache):
        return None
```

```python
def _kind(tags) -> str | None:
    if "highway" in tags and tags.get("area") != "yes":
        return "road"
    if "railway" in tags:
        return "rail"
    if tags.get("aeroway") in ("runway", "taxiway"):
        return "road"
    return None


def _area_kind(tags) -> str | None:
    if "area:highway" in tags or ("highway" in tags and tags.get("area") == "yes"):
        return "paved_area"
    if any(tags.get(k) == v for k, v in AREA_TAGS):
        return "paved_area"
    return None


def read_osm(pbf: Path, bounds, settings: dict) -> list[Feature]:
    """Roads / rail ways and paved-area polygons whose geometry intersects bounds. Tags are copied as plain dicts; the
    width / surface / skip rules are applied by features.py, not here."""
    import osmium

    want = box(*bounds)
    wkb = osmium.geom.WKBFactory()
    keep_roads, keep_rail, keep_areas = settings["roads"], settings["rail"], settings["paved_areas"]
    classes = set(settings["road_classes"]) | {c + "_link" for c in settings["road_classes"]}
    out: list[Feature] = []
    fp = (osmium.FileProcessor(str(pbf)).with_locations().with_areas()
          .with_filter(osmium.filter.KeyFilter("highway", "railway", "aeroway", "amenity", "area:highway")))
    for o in fp:
        tags = {t.k: t.v for t in o.tags}
        if o.is_way():
            k = _kind(tags)
            if k is None or (k == "road" and not keep_roads) or (k == "rail" and not keep_rail):
                continue
            if k == "road" and "highway" in tags and tags["highway"] not in classes:
                continue
            if k == "rail" and tags.get("railway") not in ("rail", "light_rail"):
                continue
            try:
                g = shapely.from_wkb(wkb.create_linestring(o), hex=True)
            except Exception:
                continue  # a way with missing node locations (cut at the extract edge)
            if g.intersects(want):
                out.append(Feature(k, f"osm:w{o.id}", g, tags))
        elif o.is_area() and keep_areas:
            if _area_kind(tags) is None or tags.get("parking") in ("underground", "multi-storey"):
                continue
            try:
                g = shapely.from_wkb(wkb.create_multipolygon(o), hex=True)
            except Exception:
                continue
            if g.intersects(want):
                out.append(Feature("paved_area", f"osm:a{o.id}", g, tags))
    out.sort(key=lambda f: f.source_id)
    return out
```

Note: `WKBFactory` returns hex WKB by default in pyosmium (`hex=True`). If the installed version returns bytes, drop
`hex=True`. The test catches this.

- [ ] **Step 5: Run the tests**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_osm_geofabrik.py -q`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add scripts/scene && git commit -m "feat(scene): osm_geofabrik source (Geofabrik extracts, pyosmium) (REALISM R2)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: `nhd_hr` source

**Files:**
- Create: `scripts/scene/camsim_scene/sources/nhd_hr.py`
- Modify: `scripts/scene/camsim_scene/sources/__init__.py` (`BUILTIN["nhd_hr"] = "camsim_scene.sources.nhd_hr:NhdHr"`)
- Test: `scripts/scene/tests/test_nhd_hr.py`

**Interfaces:**
- Consumes: `tnm_products(http, bounds, dataset)` (`sources/tnm.py`), `features.Feature`.
- Produces:
  - `NhdHr.discover(area) -> [Asset]`, one per HU4 GeoPackage zip;
  - `NhdHr.prepare(path, asset, cache) -> Path`, the GPKG unzipped into `cache.derived_path(<zip sha256>, ".gpkg")`;
  - `read_nhd(gpkg: Path, bounds) -> list[Feature]` (kind `"water"`, `source_id` `"nhd:<Permanent_Identifier>"`).

- [ ] **Step 1: Confirm the TNM dataset name and the GPKG layout (one-off, network)**

```bash
curl -s "https://tnmaccess.nationalmap.gov/api/v1/products?bbox=-117.6,33.2,-117.3,33.5&datasets=National%20Hydrography%20Dataset%20(NHD)%20Best%20Resolution&prodFormats=GeoPackage&max=20&outputFormat=JSON" \
  | python3 -c "import json,sys; [print(i['title'], i['downloadURL'], i['sizeInBytes']) for i in json.load(sys.stdin)['items']]"
```

Expected: an `NHD_H_1807_HU4_GPKG.zip` item (other names are possible: keep only items whose title contains `HU4`). Then
download that zip once to the scratch area and print its layout:

```bash
python3 -c "import sqlite3,sys; c=sqlite3.connect(sys.argv[1]); print(c.execute('select table_name,column_name,geometry_type_name from gpkg_geometry_columns').fetchall()); print([r[1] for r in c.execute('pragma table_info(NHDWaterbody)')]); print(c.execute(\"select * from NHDFCode where FCode in (39004,39001,46006,46003,43600) \").fetchall()[:5])" <path>.gpkg
```

Record in the module docstring the table names and the column-name case (e.g. `FType`/`ftype`) the code must accept.
The code matches columns case-insensitively, so either works.

- [ ] **Step 2: Write failing tests** (build a tiny GPKG in the test with `sqlite3`)

```python
import sqlite3
import struct

import shapely
from shapely.geometry import Polygon, box

from camsim_scene.sources.nhd_hr import gpkg_geometry, keep_fcode, read_nhd


def _blob(geom) -> bytes:
    return b"GP" + bytes([0, 0x01]) + struct.pack("<i", 4326) + shapely.to_wkb(geom)  # flags: LE, no envelope


def _gpkg(path):
    c = sqlite3.connect(path)
    c.execute("create table gpkg_geometry_columns (table_name, column_name, geometry_type_name, srs_id, z, m)")
    for t in ("NHDWaterbody", "NHDArea"):
        c.execute(f"create table {t} (Permanent_Identifier text, FType int, FCode int, Shape blob)")
        c.execute("insert into gpkg_geometry_columns values (?, 'Shape', 'MULTIPOLYGON', 4326, 0, 0)", (t,))
    c.execute("create table NHDFCode (FCode int, Description text)")
    c.executemany("insert into NHDFCode values (?, ?)", [
        (39004, "Lake/Pond: Hydrographic Category = Perennial"),
        (39001, "Lake/Pond: Hydrographic Category = Intermittent"),
        (46006, "Stream/River: Hydrographic Category = Perennial"),
        (44500, "Sea/Ocean"),
    ])
    sq = box(-117.45, 33.25, -117.44, 33.26)
    c.executemany("insert into NHDWaterbody values (?, ?, ?, ?)", [
        ("lake", 390, 39004, _blob(sq)), ("dry", 390, 39001, _blob(sq)), ("sea", 445, 44500, _blob(sq))])
    c.execute("insert into NHDArea values ('river', 460, 46006, ?)", (_blob(sq),))
    c.commit()
    c.close()


def test_read_nhd_keeps_perennial_water_only(tmp_path):
    p = tmp_path / "t.gpkg"
    _gpkg(p)
    ids = sorted(f.source_id for f in read_nhd(p, (-117.5, 33.2, -117.4, 33.3)))
    assert ids == ["nhd:lake", "nhd:river"]


def test_read_nhd_clips_to_area(tmp_path):
    p = tmp_path / "t.gpkg"
    _gpkg(p)
    assert read_nhd(p, (-118.0, 34.0, -117.9, 34.1)) == []


def test_gpkg_geometry_skips_envelope():
    g = Polygon([(0, 0), (1, 0), (1, 1)])
    env = struct.pack("<4d", 0, 1, 0, 1)
    blob = b"GP" + bytes([0, 0x03]) + struct.pack("<i", 4326) + env + shapely.to_wkb(g)  # flags: LE, xy envelope
    assert gpkg_geometry(blob).equals(g)


def test_keep_fcode():
    assert keep_fcode(390, "Lake/Pond: Hydrographic Category = Perennial")
    assert not keep_fcode(390, "Lake/Pond: Hydrographic Category = Intermittent")
    assert not keep_fcode(460, "Stream/River: Hydrographic Category = Ephemeral")
    assert keep_fcode(436, "Reservoir")
    assert not keep_fcode(445, "Sea/Ocean")
```

- [ ] **Step 3: Run the tests and watch them fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_nhd_hr.py -q`
Expected: FAIL (`ModuleNotFoundError`)

- [ ] **Step 4: Implement**

```python
"""USGS NHD High Resolution water (public domain; REALISM R2). NHD is retired (static since 2023), so the HU4
GeoPackages on TNM are reproducible as-is; 3DHP replaces this adapter when per-unit downloads exist. GeoPackages are read
with sqlite3 + shapely (no GDAL). Kept: perennial lakes/ponds (FType 390), reservoirs (436) and stream/river areas
(460); intermittent / ephemeral features, the sea, estuaries and every flowline are not."""

from __future__ import annotations

import sqlite3
import struct
import zipfile
from pathlib import Path

import shapely
from shapely.geometry import box

from ..features import Feature
from .base import Area, Asset, Layer, SourceBase
from .tnm import tnm_products

DATASET = "National Hydrography Dataset (NHD) Best Resolution"
FTYPES = {"NHDWaterbody": (390, 436), "NHDArea": (460,)}
_ENVELOPE = {0: 0, 1: 32, 2: 48, 3: 48, 4: 64}


def keep_fcode(ftype: int, description: str) -> bool:
    if ftype not in (390, 436, 460):
        return False
    d = description.lower()
    return "intermittent" not in d and "ephemeral" not in d


def gpkg_geometry(blob: bytes):
    if blob[:2] != b"GP":
        raise ValueError("not a GeoPackage geometry")
    flags = blob[3]
    env = _ENVELOPE[(flags >> 1) & 0x07]
    return shapely.from_wkb(blob[8 + env :])


def _cols(c, table) -> dict[str, str]:
    return {r[1].lower(): r[1] for r in c.execute(f'pragma table_info("{table}")')}


def read_nhd(gpkg: Path, bounds) -> list[Feature]:
    want = box(*bounds)
    c = sqlite3.connect(f"file:{gpkg}?mode=ro", uri=True)
    try:
        fc = _cols(c, "NHDFCode")
        desc = dict(c.execute(f'select "{fc["fcode"]}", "{fc["description"]}" from NHDFCode'))
        geom_col = dict(c.execute("select table_name, column_name from gpkg_geometry_columns"))
        out = []
        for table, ftypes in FTYPES.items():
            if table not in geom_col:
                continue
            cols = _cols(c, table)
            q = (f'select "{cols["permanent_identifier"]}", "{cols["ftype"]}", "{cols["fcode"]}", "{geom_col[table]}" '
                 f'from "{table}"')
            for pid, ftype, fcode, blob in c.execute(q):
                if ftype not in ftypes or blob is None or not keep_fcode(ftype, desc.get(fcode, "")):
                    continue
                g = gpkg_geometry(blob)
                if g.intersects(want):
                    out.append(Feature("water", f"nhd:{pid}", g, {"ftype": str(ftype), "fcode": str(fcode)}))
        out.sort(key=lambda f: f.source_id)
        return out
    finally:
        c.close()


class NhdHr(SourceBase):
    id = "nhd_hr"
    layer = Layer.FEATURES
    licence = "LicenseRef-PublicDomain-USGov"
    dataset = "USGS National Hydrography Dataset High Resolution"
    version = "HU4 GeoPackage (final, NHD retired 2023)"
    attribution = "USGS National Hydrography Dataset (NHD) High Resolution"
    area_kind = "margin"

    def discover(self, area: Area) -> list[Asset]:
        items = [i for i in tnm_products(self.http, area.bounds, DATASET)
                 if "HU4" in i.get("title", "") and i.get("format", "GeoPackage").lower().startswith("geopackage")]
        items.sort(key=lambda i: i["downloadURL"])
        return [Asset(id=Path(i["downloadURL"]).stem, url=i["downloadURL"], size=i.get("sizeInBytes"),
                      group=self.id, rank=n) for n, i in enumerate(items)]

    def prepare(self, path: Path, asset: Asset, cache) -> Path:
        out = cache.derived_path(asset.sha256 or Path(path).stem, ".gpkg")
        if not out.exists():
            out.parent.mkdir(parents=True, exist_ok=True)
            with zipfile.ZipFile(path) as z:
                name = next(n for n in z.namelist() if n.lower().endswith(".gpkg"))
                tmp = out.with_suffix(".tmp")
                with z.open(name) as src, open(tmp, "wb") as dst:
                    while chunk := src.read(1 << 20):
                        dst.write(chunk)
                tmp.rename(out)
        return out
```

Adjust the `format` filter to what Step 1 printed (TNM's field may be `format` or `prodFormats`), and keep the "HU4"
title filter.

- [ ] **Step 5: Run the tests**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_nhd_hr.py -q`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add scripts/scene && git commit -m "feat(scene): nhd_hr source (HU4 GeoPackage, perennial water) (REALISM R2)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Feature rules and burning (`features.py`)

**Files:**
- Modify: `scripts/scene/camsim_scene/features.py`
- Test: `scripts/scene/tests/test_features.py`

**Interfaces:**
- Consumes: `Feature`, `TILE_DEG`, `TILE_PX`, `CELLS_PER_DEG` (`sources/worldcover.py`).
- Produces:
  - `CODES = {200: "paved_asphalt", 201: "paved_concrete", 210: "rail", 220: "inland_water"}`;
  - `code_for(f: Feature) -> int | None` (None = skip);
  - `width_m(f: Feature) -> float`;
  - `burn_tile(codes: np.ndarray, i: int, j: int, feats: list[Feature], area: Bounds) -> int`, which edits `codes` in
    place and returns the number of texels changed.

- [ ] **Step 1: Write failing tests**

```python
import numpy as np
import pytest
from shapely.geometry import LineString, box

from camsim_scene.features import CELL_M, PRIORITY, Feature, burn_tile, code_for, width_m

I, J = 666, -2350  # tile +33.30_-117.50
W0, S0 = J * 0.05, I * 0.05
AREA = (W0 - 1, S0 - 1, W0 + 1, S0 + 1)


def road(tags, coords=None, sid="osm:w1"):
    coords = coords or [(W0 + 0.01, S0 + 0.025), (W0 + 0.04, S0 + 0.025)]
    return Feature("road", sid, LineString(coords), {"highway": "residential", **tags})


@pytest.mark.parametrize("tags,expected", [
    ({"highway": "motorway"}, 2 * 3.5 + 3.0),
    ({"highway": "motorway", "lanes": "4"}, 4 * 3.5 + 3.0),
    ({"highway": "residential"}, 2 * 3.5 + 1.0),
    ({"highway": "residential", "oneway": "yes"}, 1 * 3.5 + 1.0),
    ({"highway": "primary", "width": "12.5"}, 12.5),
    ({"highway": "primary", "width": "12 m"}, 12.0),
    ({"highway": "primary", "lanes": "x"}, 2 * 3.5 + 1.0),
    ({"aeroway": "runway"}, 45.0),
    ({"aeroway": "taxiway"}, 23.0),
])
def test_width(tags, expected):
    assert width_m(Feature("road", "osm:w1", LineString([(0, 0), (1, 0)]), tags)) == pytest.approx(expected)


def test_rail_width_per_track():
    f = Feature("rail", "osm:w1", LineString([(0, 0), (1, 0)]), {"railway": "rail", "tracks": "2"})
    assert width_m(f) == pytest.approx(8.0)


@pytest.mark.parametrize("tags,code", [
    ({}, 200), ({"surface": "asphalt"}, 200), ({"surface": "concrete:plates"}, 201),
    ({"surface": "gravel"}, None), ({"surface": "unpaved"}, None),
    ({"bridge": "yes"}, None), ({"bridge": "no"}, 200), ({"tunnel": "culvert"}, None),
    ({"covered": "yes"}, None), ({"layer": "-1"}, None), ({"layer": "1"}, 200), ({"location": "underground"}, None),
])
def test_code_for_roads(tags, code):
    assert code_for(road(tags)) == code


def test_runway_defaults_to_concrete():
    assert code_for(Feature("road", "osm:w1", LineString([(0, 0), (1, 0)]), {"aeroway": "runway"})) == 201
    assert code_for(Feature("road", "osm:w1", LineString([(0, 0), (1, 0)]), {"aeroway": "runway", "surface": "asphalt"})) == 200


def test_rail_and_water_codes():
    assert code_for(Feature("rail", "osm:w1", LineString([(0, 0), (1, 0)]), {"railway": "rail"})) == 210
    assert code_for(Feature("water", "nhd:x", box(0, 0, 1, 1), {})) == 220


def _strip_runs(mask: np.ndarray, axis: int) -> list[int]:
    """Texel count of the mask in each line across the road (rows when the road runs E-W)."""
    return [int(v) for v in mask.sum(axis=axis) if v]


@pytest.mark.parametrize("bearing", [0, 30, 45, 60, 90])
def test_narrow_road_is_a_continuous_one_texel_strip(bearing):
    import math

    c = (W0 + 0.025, S0 + 0.025)
    d = 0.015
    dx, dy = d * math.sin(math.radians(bearing)) / math.cos(math.radians(S0)), d * math.cos(math.radians(bearing))
    f = Feature("road", "osm:w1", LineString([(c[0] - dx, c[1] - dy), (c[0] + dx, c[1] + dy)]),
                {"highway": "residential", "width": "3"})
    codes = np.full((600, 600), 30, np.uint8)
    burn_tile(codes, I, J, [f], AREA)
    m = codes == 200
    ys, xs = np.nonzero(m)
    # 8-connected and gap free: every step along the road's major axis has at least one texel
    major = ys if abs(dy) >= abs(dx * math.cos(math.radians(S0))) else xs
    assert set(range(major.min(), major.max() + 1)) <= set(major.tolist())
    # about one texel wide: across the road no line has more than 3 texels
    across = xs if major is ys else ys
    for v in np.unique(major):
        assert (major == v).sum() <= 3


def test_wide_road_burns_about_its_width():
    f = road({"width": "30"})
    codes = np.full((600, 600), 30, np.uint8)
    burn_tile(codes, I, J, [f], AREA)
    col = (codes[:, 300] == 200).sum()
    assert 3 <= col <= 4  # 30 m / 9.28 m per row


def test_priority_road_over_water_and_bridge_skipped():
    water = Feature("water", "nhd:a", box(W0 + 0.02, S0 + 0.02, W0 + 0.03, S0 + 0.03), {})
    over = road({}, sid="osm:w2")
    bridge = road({"bridge": "yes"}, [(W0 + 0.025, S0 + 0.01), (W0 + 0.025, S0 + 0.04)], sid="osm:w3")
    codes = np.full((600, 600), 30, np.uint8)
    burn_tile(codes, I, J, [bridge, over, water], AREA)
    r, c = 300, 300  # tile centre: inside the water box, on the E-W road
    assert codes[r, c] == 200
    assert codes[250, 300] == 220  # water, north of the road, where only the (skipped) bridge crosses
    assert PRIORITY["water"] < PRIORITY["road"]


def test_road_across_tile_seam_is_continuous():
    f = road({}, [(W0 + 0.04, S0 + 0.025), (W0 + 0.06, S0 + 0.025)])
    a = np.zeros((600, 600), np.uint8)
    b = np.zeros((600, 600), np.uint8)
    burn_tile(a, I, J, [f], AREA)
    burn_tile(b, I, J + 1, [f], AREA)
    assert (a[:, -1] == 200).any() and (b[:, 0] == 200).any()
    assert np.array_equal(np.nonzero(a[:, -1] == 200)[0], np.nonzero(b[:, 0] == 200)[0])


def test_burn_stops_at_margin():
    f = road({}, [(W0 + 0.01, S0 + 0.025), (W0 + 0.04, S0 + 0.025)])
    area = (W0 - 1, S0 - 1, W0 + 0.025, S0 + 1)  # margin ends half way across the tile
    codes = np.zeros((600, 600), np.uint8)
    burn_tile(codes, I, J, [f], area)
    cols = np.nonzero((codes == 200).any(axis=0))[0]
    assert cols.max() < 300 and cols.min() < 200


def test_burn_is_deterministic_regardless_of_input_order():
    fs = [road({}, sid="osm:w2"), Feature("water", "nhd:a", box(W0 + 0.02, S0 + 0.02, W0 + 0.03, S0 + 0.03), {})]
    a = np.full((600, 600), 30, np.uint8)
    b = a.copy()
    burn_tile(a, I, J, fs, AREA)
    burn_tile(b, I, J, fs[::-1], AREA)
    assert np.array_equal(a, b)
```

- [ ] **Step 2: Run the tests and watch them fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_features.py -q`
Expected: FAIL (`ImportError`)

- [ ] **Step 3: Implement** (`features.py`; keep `RULES_VERSION` and `Feature` from Tasks 1 and 2)

```python
"""Vector features burned into the land-cover tiles (REALISM R2, spec docs/superpowers/specs/2026-10-10-r2-vector-
features-design.md, sections 1, 3, 4). Each feature becomes a polygon in a local metric frame per tile (equirectangular
about the tile centre), at least one cell (9.2767 m) wide, rasterised with the centre-sample rule in priority order
(water < paved areas < rail < roads; later wins) and sorted by source id within a priority, so output is deterministic.
Bridges, tunnels and underground features are skipped (no deck geometry before R3)."""

from __future__ import annotations

import math
import re
from dataclasses import dataclass

import numpy as np
import shapely
from shapely import affinity
from shapely.geometry import box

from .sources.worldcover import CELLS_PER_DEG, TILE_DEG, TILE_PX

RULES_VERSION = "1"
M_PER_DEG_LAT = 111_320.0
CELL_M = M_PER_DEG_LAT / CELLS_PER_DEG  # 9.2767 m: a cell's N-S side, its longer side at every latitude (E-W shrinks by cos lat)
MIN_W_M = 1.05 * CELL_M  # minimum burned width: a strip this wide covers a cell centre in every cross-section, with margin
CODES = {200: "paved_asphalt", 201: "paved_concrete", 210: "rail", 220: "inland_water"}
PRIORITY = {"water": 0, "paved_area": 1, "rail": 2, "road": 3}
LANE_M = 3.5
SHOULDER_M = {"motorway": 3.0, "trunk": 3.0}
RUNWAY_M = {"runway": 45.0, "taxiway": 23.0}
TRACK_M = 4.0
CONCRETE = {"concrete", "concrete:plates", "concrete:lanes"}
UNPAVED = {"unpaved", "gravel", "dirt", "ground", "compacted", "fine_gravel", "sand", "grass", "earth", "mud"}


@dataclass(frozen=True)
class Feature:
    kind: str
    source_id: str
    geom: object
    tags: dict


def _num(v: str | None) -> float | None:
    if v is None:
        return None
    m = re.match(r"^\s*([0-9]+(?:\.[0-9]+)?)\s*m?\s*$", v)
    return float(m.group(1)) if m else None


def _skipped(t: dict) -> bool:
    if t.get("bridge", "no") != "no" or t.get("tunnel", "no") != "no":
        return True
    if t.get("covered") == "yes" or t.get("location") == "underground":
        return True
    try:
        return float(t.get("layer", "0")) < 0
    except ValueError:
        return False


def width_m(f: Feature) -> float:
    t = f.tags
    if f.kind == "rail":
        tracks = _num(t.get("tracks")) or 1.0
        return TRACK_M * tracks
    w = _num(t.get("width"))
    if w:
        return w
    if t.get("aeroway") in RUNWAY_M:
        return RUNWAY_M[t["aeroway"]]
    hw = t.get("highway", "").removesuffix("_link")
    lanes = _num(t.get("lanes"))
    if not lanes or lanes != int(lanes):
        lanes = 2.0 if hw in ("motorway", "trunk") else (1.0 if t.get("oneway") == "yes" else 2.0)
    return lanes * LANE_M + SHOULDER_M.get(hw, 1.0)


def code_for(f: Feature) -> int | None:
    if f.kind == "water":
        return 220
    t = f.tags
    if _skipped(t):
        return None
    if f.kind == "rail":
        return 210
    s = t.get("surface")
    if s in UNPAVED:
        return None
    if s in CONCRETE or (s is None and t.get("aeroway") == "runway"):
        return 201
    return 200


def _to_local(lat0: float, lon0: float):
    kx = M_PER_DEG_LAT * math.cos(math.radians(lat0))
    return lambda g: affinity.affine_transform(g, [kx, 0, 0, M_PER_DEG_LAT, -lon0 * kx, -lat0 * M_PER_DEG_LAT])


def _polygon(f: Feature, local) -> object:
    """Lines (roads, rail, runways) are buffered to max(width, MIN_W_M); polygons (water, paved areas) are burned as they
    are: a polygon arm narrower than a cell may vanish, which is the 10 m raster's limit, not widened."""
    g = local(f.geom)
    if g.geom_type in ("LineString", "MultiLineString"):
        return g.buffer(max(width_m(f), MIN_W_M) / 2.0, cap_style="flat", join_style="round")
    if g.geom_type in ("Polygon", "MultiPolygon"):
        return g
    return None


def burn_tile(codes: np.ndarray, i: int, j: int, feats: list[Feature], area) -> int:
    from rasterio import features as rf
    from rasterio.transform import Affine

    s, w = i * TILE_DEG, j * TILE_DEG
    lat0, lon0 = s + TILE_DEG / 2.0, w + TILE_DEG / 2.0
    local = _to_local(lat0, lon0)
    tile = box(w, s, w + TILE_DEG, s + TILE_DEG)
    clip = local(tile.intersection(box(*area)))
    if clip.is_empty:
        return 0
    pad = local(tile).buffer(100.0)  # features reaching into the tile from outside it
    kx = M_PER_DEG_LAT * math.cos(math.radians(lat0))
    cell_x, cell_y = kx / CELLS_PER_DEG, M_PER_DEG_LAT / CELLS_PER_DEG
    x0, y0 = (w - lon0) * kx, (s + TILE_DEG - lat0) * M_PER_DEG_LAT  # north-west corner, local metres
    transform = Affine(cell_x, 0, x0, 0, -cell_y, y0)
    before = codes.copy()
    for f in sorted(feats, key=lambda f: (PRIORITY[f.kind], f.source_id)):
        code = code_for(f)
        if code is None:
            continue
        p = _polygon(f, local)
        if p is None or p.is_empty or not p.intersects(pad):
            continue
        p = p.intersection(clip)
        if p.is_empty:
            continue
        m = rf.rasterize([(p, 1)], out_shape=(TILE_PX, TILE_PX), transform=transform, fill=0, all_touched=False,
                         dtype="uint8").astype(bool)
        codes[m] = code
    return int((codes != before).sum())
```

`burn_tile` imports `Affine` from `rasterio.transform` (it re-exports `affine.Affine`).

- [ ] **Step 4: Run the tests**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_features.py -q`
Expected: PASS. If `test_narrow_road_is_a_continuous_one_texel_strip` fails at some bearing, check that the buffer uses
`MIN_W_M` and that the transform's `cell_x` is the E-W size (shorter than `cell_y`). Do not switch to `all_touched`: it
doubles the road width.

- [ ] **Step 5: Commit**

```bash
git add scripts/scene && git commit -m "feat(scene): vector feature rules and tile burn (REALISM R2)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Pipeline integration, index legend and verify

**Files:**
- Modify: `scripts/scene/camsim_scene/pipeline.py` (`_cut_landcover`, `_build_landcover`)
- Modify: `scripts/scene/camsim_scene/layers/landcover.py` (`build_index`, `fetch` write the legend and credits when
  given)
- Modify: `scripts/scene/camsim_scene/verify.py` (legend check in `_deep`)
- Modify: `scripts/scene/examples/pendleton.toml`
- Test: `scripts/scene/tests/test_pipeline.py`, `scripts/scene/tests/test_verify.py`

**Interfaces:**
- Consumes:
  - `features_settings` / `layers.landcover.features` (Task 1);
  - `read_osm` (Task 2);
  - `read_nhd`, `NhdHr.prepare` output paths via `ctx.asset_paths` (Task 3);
  - `burn_tile`, `CODES` (Task 4).
- Produces:
  - `index.json` gains `"codes": {"200": "paved_asphalt", ...}`, only the codes present, and only when features are on;
  - `"credits": [<attribution>, ...]` for the feature sources;
  - `ATTRIBUTION.txt` gains one block per feature source.

- [ ] **Step 1: Write failing tests**

Append to `tests/test_pipeline.py`. The setup mirrors `test_build_json_counts_files_per_layer`: it uses the synthetic
package, a patched WorldCover reader of grassland (30), and in-process isolation.

```python
from PIL import Image
from shapely.geometry import LineString

from camsim_scene.features import Feature

FEATURES = {"area": [10.0, 10.0, 10.1, 10.04], "sources": ["osm_geofabrik"], "roads": True, "paved_areas": False,
            "rail": False, "water": False, "road_classes": ["motorway"], "rules": "1"}


def _lc_setup(tmp_path, monkeypatch, features=None):
    from camsim_scene import pipeline
    from camsim_scene.context import BuildContext
    from camsim_scene.manifest import SourceRecord

    pkg, _, _ = build_synthetic(tmp_path)
    m = Manifest.load(pkg / "manifest.json")
    m.sources.append(SourceRecord("lc", "worldcover", "lc", "1", "CC-BY-4.0", "lc", {}, []))
    m.layers["landcover"] = {"priorities": ["lc"], "bounds": [10.0, 10.0, 10.1, 10.04], "tile_deg": 0.05}
    if features:
        m.sources.append(SourceRecord("osm_geofabrik", "osm_geofabrik", "osm", "1", "ODbL-1.0", "© OSM", {}, []))
        m.layers["landcover"]["features"] = dict(features)
    monkeypatch.setattr(
        pipeline.landcover, "local_reader", lambda paths: lambda url, r, c: np.full((600, 600), 30, np.uint8)
    )
    monkeypatch.setattr(pipeline, "_run_isolated", lambda fn, *a: fn(*a))
    road = Feature("road", "osm:w1", LineString([(10.01, 10.02), (10.04, 10.02)]), {"highway": "motorway"})
    monkeypatch.setattr(pipeline, "_load_features", lambda m, ctx: [road])
    build = lambda: pipeline._build_landcover(pkg, m, BuildContext(str(pkg), m.to_dict(), {}, {}))  # noqa: E731
    return pkg, m, build


def test_pre_r2_manifest_builds_unchanged(tmp_path, monkeypatch):
    pkg, _, build = _lc_setup(tmp_path, monkeypatch)
    build()
    index = json.loads((pkg / "landcover" / "index.json").read_text())
    assert "codes" not in index and "credits" not in index
    for t in index["tiles"]:
        assert (np.array(Image.open(pkg / "landcover" / t["file"])) == 30).all()


def test_features_burn_into_tiles_and_legend(tmp_path, monkeypatch):
    pkg, _, build = _lc_setup(tmp_path, monkeypatch, FEATURES)
    build()
    index = json.loads((pkg / "landcover" / "index.json").read_text())
    assert index["codes"] == {"200": "paved_asphalt"}
    assert index["credits"] == ["© OSM"]
    burned = sum(int((np.array(Image.open(pkg / "landcover" / t["file"])) == 200).sum()) for t in index["tiles"])
    assert burned > 300  # ~3.3 km of road at ~1 texel per 7.8 m column
    assert "© OSM" in (pkg / "landcover" / "ATTRIBUTION.txt").read_text()


def test_features_settings_change_rebuilds_landcover(tmp_path, monkeypatch):
    pkg, m, build = _lc_setup(tmp_path, monkeypatch, FEATURES)
    assert build().built == 2
    assert build().skipped == 2
    m.layers["landcover"]["features"]["rail"] = True
    assert build().built == 2
```

In `tests/test_verify.py`, test the legend check as a pure function on a temporary directory:

```python
from camsim_scene.verify import landcover_code_problems


def _lc_dir(tmp_path, codes_in_tile, legend):
    from PIL import Image

    d = tmp_path / "landcover"
    d.mkdir()
    a = np.full((600, 600), 30, np.uint8)
    for k, c in enumerate(codes_in_tile):
        a[k, 0] = c
    Image.fromarray(a).save(d / "+10.00_+10.00.png")
    idx = {"format": "camsim-landcover-1", "tiles": [{"file": "+10.00_+10.00.png", "lat_index": 200, "lon_index": 200}]}
    if legend is not None:
        idx["codes"] = legend
    (d / "index.json").write_text(json.dumps(idx))
    return d


def test_legend_covers_tile_codes(tmp_path):
    assert landcover_code_problems(_lc_dir(tmp_path, [200, 0, 80], {"200": "paved_asphalt"})) == []


def test_code_missing_from_legend_is_reported(tmp_path):
    [p] = landcover_code_problems(_lc_dir(tmp_path, [200, 210], {"200": "paved_asphalt"}))
    assert "210" in p and "+10.00_+10.00.png" in p


def test_pre_r2_tiles_need_no_legend(tmp_path):
    assert landcover_code_problems(_lc_dir(tmp_path, [10, 95, 100], None)) == []
```

- [ ] **Step 2: Run the tests and watch them fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_pipeline.py scripts/scene/tests/test_verify.py -q`
Expected: the new tests FAIL (`AttributeError: ... '_load_features'` and `ImportError: landcover_code_problems`).

- [ ] **Step 3: Implement**

In `pipeline.py`:

```python
def _load_features(m: Manifest, ctx: BuildContext) -> list:
    """Every feature record of the manifest's feature sources, from the cached extract (a derived file per asset +
    rules version: the 600 MB PBF is read once)."""
    from .features import RULES_VERSION
    from .sources.nhd_hr import read_nhd
    from .sources.osm_geofabrik import read_osm

    fs = m.layers["landcover"]["features"]
    area = tuple(fs["area"])
    out = []
    for sid in fs["sources"]:
        rec = m.source(sid)
        for a in rec.assets:
            path = Path(ctx.asset_paths[f"{rec.id}/{a.id}"])
            out += read_osm(path, area, fs) if sid == "osm_geofabrik" else read_nhd(path, area)
    return out
```

Cache `_load_features`' result as a pickle at `cache.derived_path(inputs_hash(a.sha256..., RULES_VERSION, json of fs),
".features.pkl")` if `ctx` carries a cache root. If it doesn't, skip the cache. A rebuild then re-reads the PBF (minutes),
which is correct, just slower. Check `make_context` for a cache handle before adding one.

`_cut_landcover(bounds, out, paths, feats=None, area=None)`:
- After `landcover.fetch(...)`, and when `feats` is set: for each tile in `index["tiles"]` whose bounds intersect `area`,
  load the PNG with Pillow, call `burn_tile`, and rewrite it with `landcover.write_tile` when anything changed.
- Pass `credits = [m.source(sid).attribution for sid in fs["sources"]]` from `_build_landcover` into
  `_cut_landcover`.
- Collect the set of codes present among `CODES`, then rewrite `index.json` with
  `codes={str(c): CODES[c] for c in sorted(present)}` and the credits.

`_build_landcover`:
- When `"features" in m.layers["landcover"]`:
  - add the feature sources' asset sha256s to `inputs` (the settings are already in `layer_settings_hash`);
  - call `feats = _load_features(m, ctx)` in the main process, so the isolated process doesn't need the cache;
  - pass `feats` and `area` to `_run_isolated`. Shapely geometries pickle.
- Without features, the call is exactly today's, so the `inputs` hash is unchanged and the R1 package is skipped as
  up to date.

In `layers/landcover.py`, give `build_index` and `fetch` the optional keyword arguments `codes: dict | None = None` and
`credits: list[str] | None = None`. Add the keys only when they are given, so existing output is byte-identical. Append
the credits to `ATTRIBUTION.txt` after the WorldCover block, one block per credit.

In `verify.py`:

```python
WORLDCOVER_CODES = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 95, 100}


def landcover_code_problems(lc: Path) -> list[str]:
    """Codes in the land-cover tiles that neither WorldCover nor index.json's `codes` legend defines (REALISM R2)."""
    from PIL import Image

    index = json.loads((lc / "index.json").read_text(encoding="utf-8"))
    allowed = WORLDCOVER_CODES | {int(k) for k in index.get("codes", {})}
    out = []
    for t in index["tiles"]:
        found = set(np.unique(np.array(Image.open(lc / t["file"]))).tolist())
        out += [f"landcover: code {c} not in index.json codes (tile {t['file']})" for c in sorted(found - allowed)]
    return out
```

In `_deep`, when `pkg / "landcover" / "index.json"` exists, add
`_guard("landcover_codes", lambda: Check("landcover_codes", not (p := landcover_code_problems(pkg / "landcover")), _summary(p)))`.
Match `Check`'s actual constructor and `_guard`'s signature (`verify.py:61`). Every tile is read: the tiles are few
(0.05° each) and small.

In `examples/pendleton.toml`, add:

```toml
allow = ["ODbL-1.0"]       # OSM roads (REALISM R2)

[features]                 # REALISM R2: OSM roads / paved areas / rail + NHD water burned into land cover
roads = true
paved_areas = true
rail = true
water = true
```

- [ ] **Step 4: Run all scene tests**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests -q`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add scripts/scene && git commit -m "feat(scene): burn vector features into land cover, legend + verify (REALISM R2)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: Runtime classes and `rail_ballast`

**Files:**
- Modify: `Thermal/LandCoverClasses.cpp:8-36`
- Modify: `Thermal/ThermalMaterials.h` (named index constants; add `RailBallast = 14`) and `Thermal/ThermalMaterials.cpp:63-77`
- Test: `Tests/ThermalLandCoverMaterialsTest.cpp` (count 14 → 15, and contrasts), `Tests/ThermalLandCoverConfigTest.cpp`
  (families), `Tests/ThermalConfigTest.cpp:184` (only if it hard-codes 14)

**Interfaces:**
- Produces: `FThermalMaterialTable::RailBallast == 14`; defaults:
  - 200 → `asphalt`, family `Bare`;
  - 201 → `concrete`, family `Bare`;
  - 210 → `rail_ballast`, family `Bare`;
  - 220 → `water`, family `None`.

- [ ] **Step 1: Check the ballast values against the literature (desk check, 15 min)**

Search for measured railway ballast surface temperatures (e.g. "railway ballast surface temperature diurnal",
"track ballast thermal properties"). Compare the closed-form noon and 02:00 temperatures that
`CamSim.Thermal.Materials.DiurnalContrastsAtSanFrancisco` prints with the proposed values against them:
- expected: ballast hotter than grass at noon and close to bare soil;
- expected: ballast cooler than asphalt at night (lower inertia).

If a value is clearly off, change it, and record the source and the numbers in the commit message and in
`docs/thermal.md` (Task 10).

- [ ] **Step 2: Write failing tests**

In `ThermalLandCoverMaterialsTest.cpp`, change `TestEqual(TEXT("14 built-ins"), B.Num(), 14);` to 15 and add:

```cpp
	TestEqual(TEXT("rail_ballast is built-in 14"), B[FThermalMaterialTable::RailBallast].Name, FString(TEXT("rail_ballast")));
```

In `DiurnalContrastsAtSanFrancisco`, add:

```cpp
	TestTrue(TEXT("noon: rail ballast warmer than grassland by >= 1 K"), K(T::RailBallast, Noon) - K(T::Grassland, Noon) >= 1.0);
	TestTrue(TEXT("night: rail ballast cooler than asphalt"), K(T::RailBallast, Night) < K(T::Asphalt, Night));
```

In `ThermalLandCoverConfigTest.cpp`, next to the family assertions at :182-186, add:

```cpp
	TestEqual(TEXT("200 is bare family"), Table.Family[200], static_cast<uint8>(ELandCoverFamily::Bare));
	TestEqual(TEXT("201 is bare family"), Table.Family[201], static_cast<uint8>(ELandCoverFamily::Bare));
	TestEqual(TEXT("210 is bare family"), Table.Family[210], static_cast<uint8>(ELandCoverFamily::Bare));
	TestEqual(TEXT("220 has no refinement"), Table.Family[220], static_cast<uint8>(ELandCoverFamily::None));
	TestEqual(TEXT("200 is asphalt"), static_cast<int32>(Table.Class[200]), static_cast<int32>(FThermalMaterialTable::Asphalt));
	TestEqual(TEXT("201 is concrete"), static_cast<int32>(Table.Class[201]), static_cast<int32>(FThermalMaterialTable::Concrete));
	TestEqual(TEXT("210 is rail_ballast"), static_cast<int32>(Table.Class[210]), static_cast<int32>(FThermalMaterialTable::RailBallast));
	TestEqual(TEXT("220 is water"), static_cast<int32>(Table.Class[220]), static_cast<int32>(FThermalMaterialTable::Water));
```

Use the constant names the header actually has. If `Water`/`Asphalt`/`Concrete` are spelled differently, follow the
header.

- [ ] **Step 3: Build and run the tests; watch them fail**

Run: `scripts/run.sh --build-only`, which fails to compile on `RailBallast`. That is the expected failure.

- [ ] **Step 4: Implement**

`ThermalMaterials.h`: `static constexpr int32 RailBallast = 14;` after the last named index.

`ThermalMaterials.cpp`, appended to `B` after `wetland`:

```cpp
		// REALISM R2: railway ballast (dark, porous crushed rock: low thermal inertia, hot by day, cool at night).
		MakeMaterial(TEXT("rail_ballast"),    0.20f, 0.93f,  900.0f, 10.0f, 0.025f),
```

Update the comment above `MaxClasses` / in `docs/thermal.md` that says "14 built-ins leave 18 of 32 class slots" to say
15 and 17.

`LandCoverClasses.cpp`, in `DefaultMaterialName`:

```cpp
		case 200: return TEXT("asphalt");       // REALISM R2: paved, asphalt or unknown surface
		case 201: return TEXT("concrete");      // paved, concrete
		case 210: return TEXT("rail_ballast");  // rail bed
		case 220: return TEXT("water");         // inland water (NHD)
```

In `FamilyForCode`, add `case 60: case 200: case 201: case 210: return ELandCoverFamily::Bare;`, merging with the
existing `case 60`.

- [ ] **Step 5: Build and run the tests**

Run: `scripts/run.sh --build-only`, then the automation command with `RunTests CamSim.Thermal`.
Expected: all `CamSim.Thermal.*` pass, including the 4A bit-for-bit test.

- [ ] **Step 6: Commit**

```bash
git add unreal_project/CamSimTest/Source && git commit -m "feat(thermal): land-cover codes 200-220 and rail_ballast (REALISM R2)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: CIGI material codes

**Files:**
- Create: `Thermal/LandCoverLookup.h`, `Thermal/LandCoverLookup.cpp`
- Modify: `Subsystem/CamSimSubsystem.h` / `.cpp` (own the lookup, next to `Ocean` at `.cpp:78` and `:745-760`)
- Modify: `CIGI/CigiQueryHandler.cpp:131-145` (HAT/HOT) and `:400-430` (LOS extended)
- Test: `Tests/LandCoverLookupTest.cpp`

**Interfaces:**
- Consumes: `FLandCoverTileCache` (`Thermal/LandCoverTiles.h`), `CamSimLandCover::GlobalCell` and `CellToTile`
  (`Thermal/LandCoverGeometry.h`), and the test helpers in `Tests/LandCoverTestTiles.h`.
- Produces:

```cpp
// Thermal/LandCoverLookup.h
class CAMSIMTEST_API FLandCoverCodeLookup
{
public:
	static constexpr int32 MaxTiles = 16;
	/** Game thread. An empty Dir, or one with no valid index.json, makes every Code() 0 (one warning in TakeWarnings). */
	void Configure(const FString& Dir);
	/** The land-cover code of the cell holding (Lat, Lon), or 0 (no data / no tile / not configured). Game thread. */
	uint8 Code(double LatDeg, double LonDeg);
	bool IsAvailable() const;
	TArray<FString> TakeWarnings();
private:
	TUniquePtr<FLandCoverTileCache> Cache;
	FString ConfiguredDir;
	TArray<FString> Warnings;
};

namespace CamSimLandCover
{
	inline constexpr uint8 SeaSurfaceCode = 80;   // WorldCover water
	/** REALISM R2 rule for CIGI HAT/HOT and LOS extended material codes. Lookup may be null (land cover off). */
	CAMSIMTEST_API uint32 QueryMaterialCode(bool bValid, bool bWater, bool bEntityHit, double LatDeg, double LonDeg,
		FLandCoverCodeLookup* Lookup);
}
```

Subsystem: `FLandCoverCodeLookup* UCamSimSubsystem::GetLandCoverLookup();` (null when `thermal.land_cover.enabled` is
false).

- [ ] **Step 1: Write failing tests** (`Tests/LandCoverLookupTest.cpp`)

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Tests/LandCoverTestTiles.h"
#include "Thermal/LandCoverGeometry.h"
#include "Thermal/LandCoverLookup.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverLookupCodeTest, "CamSim.Thermal.LandCover.CodeLookup.Codes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverLookupCodeTest::RunTest(const FString& Parameters)
{
	using namespace CamSimLandCoverTest;
	const FString Dir = TempDir(TEXT("CodeLookup"));
	// Tile (666, -2350) = +33.30_-117.50, PatternCodes: (row + 3 col) % 101
	WriteTileDir(Dir, { FTestTile{ 666, -2350, PatternCodes() } });
	FLandCoverCodeLookup L;
	L.Configure(Dir);
	TestTrue(TEXT("available"), L.IsAvailable());
	// cell (row 0, col 0) = north-west corner of the tile
	const double Lat = 33.35 - 0.5 / 12000.0, Lon = -117.50 + 0.5 / 12000.0;
	TestEqual(TEXT("NW cell"), static_cast<int32>(L.Code(Lat, Lon)), 0);
	TestEqual(TEXT("row 1 col 2"), static_cast<int32>(L.Code(Lat - 1.0 / 12000.0, Lon + 2.0 / 12000.0)), (1 + 3 * 2) % 101);
	TestEqual(TEXT("no tile -> 0"), static_cast<int32>(L.Code(10.0, 10.0)), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverLookupEdgesTest, "CamSim.Thermal.LandCover.CodeLookup.EdgesMatchResample",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverLookupEdgesTest::RunTest(const FString& Parameters)
{
	using namespace CamSimLandCoverTest;
	const FString Dir = TempDir(TEXT("CodeLookupEdges"));
	WriteTileDir(Dir, { FTestTile{ 666, -2350, PatternCodes() }, FTestTile{ 666, -2349, PatternCodes() },
	                    FTestTile{ 0, 3599, PatternCodes() }, FTestTile{ 0, -3600, PatternCodes() } });
	FLandCoverCodeLookup L;
	L.Configure(Dir);
	// Exactly on a tile boundary and the antimeridian: the same cell GlobalCell + CellToTile pick for the window resample.
	const double Pts[][2] = { { 33.35, -117.45 }, { 33.30, -117.45 }, { 0.025, 180.0 }, { 0.025, -180.0 }, { 0.025, 179.99999 } };
	for (const auto& P : Pts)
	{
		FIntPoint Tile, In;
		CamSimLandCover::CellToTile(CamSimLandCover::GlobalCell(P[0], P[1]), Tile, In);
		const bool bHasTile = (Tile == FIntPoint(666, -2350)) || (Tile == FIntPoint(666, -2349)) || (Tile == FIntPoint(0, 3599)) || (Tile == FIntPoint(0, -3600));
		const int32 Expected = bHasTile ? (In.Y + 3 * In.X) % 101 : 0;
		TestEqual(*FString::Printf(TEXT("(%.5f, %.5f)"), P[0], P[1]), static_cast<int32>(L.Code(P[0], P[1])), Expected);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverLookupMissingTest, "CamSim.Thermal.LandCover.CodeLookup.MissingDir",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverLookupMissingTest::RunTest(const FString& Parameters)
{
	FLandCoverCodeLookup L;
	L.Configure(CamSimLandCoverTest::TempDir(TEXT("CodeLookupEmpty")));   // no index.json
	TestFalse(TEXT("not available"), L.IsAvailable());
	TestEqual(TEXT("code 0"), static_cast<int32>(L.Code(33.3, -117.5)), 0);
	TestEqual(TEXT("one warning"), L.TakeWarnings().Num(), 1);
	L.Code(33.3, -117.5);
	TestEqual(TEXT("no repeat warning"), L.TakeWarnings().Num(), 0);
	FLandCoverCodeLookup Off;
	Off.Configure(FString());
	TestEqual(TEXT("empty dir: code 0, no warning"), static_cast<int32>(Off.Code(33.3, -117.5)) + Off.TakeWarnings().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverQueryMaterialTest, "CamSim.CIGI.MaterialCode.Rule",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverQueryMaterialTest::RunTest(const FString& Parameters)
{
	using namespace CamSimLandCoverTest;
	const FString Dir = TempDir(TEXT("QueryMaterial"));
	TArray<uint8> Road; Road.Init(200, TilePx * TilePx);
	WriteTileDir(Dir, { FTestTile{ 666, -2350, Road } });
	FLandCoverCodeLookup L;
	L.Configure(Dir);
	using CamSimLandCover::QueryMaterialCode;
	TestEqual(TEXT("invalid"), QueryMaterialCode(false, false, false, 33.32, -117.48, &L), 0u);
	TestEqual(TEXT("sea surface"), QueryMaterialCode(true, true, false, 33.32, -117.48, &L), 80u);
	TestEqual(TEXT("entity"), QueryMaterialCode(true, false, true, 33.32, -117.48, &L), 0u);
	TestEqual(TEXT("terrain"), QueryMaterialCode(true, false, false, 33.32, -117.48, &L), 200u);
	TestEqual(TEXT("no lookup"), QueryMaterialCode(true, false, false, 33.32, -117.48, nullptr), 0u);
	TestEqual(TEXT("sea without lookup"), QueryMaterialCode(true, true, false, 33.32, -117.48, nullptr), 80u);
	return true;
}
```

`WriteTileDir(Dir, TArray<FTestTile>)` must write `index.json` plus PNGs. Check `LandCoverTestTiles.h` below line 60 for
the existing helper (its name may differ, e.g. `WriteDir`) and use it.

- [ ] **Step 2: Build and watch it fail**

Run: `scripts/run.sh --build-only`
Expected: compile error, `Thermal/LandCoverLookup.h` not found.

- [ ] **Step 3: Implement**

```cpp
// Thermal/LandCoverLookup.cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Thermal/LandCoverLookup.h"
#include "Thermal/LandCoverGeometry.h"
#include "Thermal/LandCoverTiles.h"

void FLandCoverCodeLookup::Configure(const FString& Dir)
{
	if (Dir == ConfiguredDir && (Cache.IsValid() || Dir.IsEmpty())) return;
	ConfiguredDir = Dir;
	Cache.Reset();
	if (Dir.IsEmpty()) return;
	TUniquePtr<FLandCoverTileCache> C = MakeUnique<FLandCoverTileCache>(Dir, MaxTiles);
	if (!C->IsValid())
	{
		Warnings.Add(FString::Printf(TEXT("CIGI material codes off: land cover %s: %s"), *Dir, *C->GetError()));
		return;
	}
	Cache = MoveTemp(C);
}

bool FLandCoverCodeLookup::IsAvailable() const { return Cache.IsValid(); }

uint8 FLandCoverCodeLookup::Code(double LatDeg, double LonDeg)
{
	if (!Cache.IsValid() || !FMath::IsFinite(LatDeg) || !FMath::IsFinite(LonDeg)) return 0;
	FIntPoint Tile, In;
	CamSimLandCover::CellToTile(CamSimLandCover::GlobalCell(LatDeg, LonDeg), Tile, In);
	const TSharedPtr<const FLandCoverTile, ESPMode::ThreadSafe> T = Cache->Get(Tile.X, Tile.Y);
	for (const FString& W : Cache->TakeWarnings()) Warnings.Add(W);
	if (!T.IsValid()) return 0;
	return T->Codes[In.Y * FLandCoverTileCache::TilePx + In.X];
}

TArray<FString> FLandCoverCodeLookup::TakeWarnings() { return MoveTemp(Warnings); }

uint32 CamSimLandCover::QueryMaterialCode(bool bValid, bool bWater, bool bEntityHit, double LatDeg, double LonDeg,
	FLandCoverCodeLookup* Lookup)
{
	if (!bValid || bEntityHit) return 0;
	if (bWater) return SeaSurfaceCode;
	return Lookup ? Lookup->Code(LatDeg, LonDeg) : 0;
}
```

Check `CellToTile`'s output convention: `OutTile` is (LatIndex, LonIndex), and `OutInTile` is (column, row). This is
documented in `LandCoverGeometry.h`. Read the `.cpp` and swap `.X`/`.Y` above if it is the other way round. The Edges
test catches a mismatch.

Subsystem:
- add `TUniquePtr<FLandCoverCodeLookup> LandCoverLookup;` to `FSubsystemImpl`;
- at startup, next to the ocean block (`.cpp:745`), when `Config.Thermal.LandCover.bEnabled`: create the lookup, call
  `Configure(Config.Thermal.LandCover.Dir)`, and log its warnings with
  `UE_LOG(LogCamSim, Warning, TEXT("%s"), *W)`;
- add `GetLandCoverLookup()` returning `Impl ? Impl->LandCoverLookup.Get() : nullptr`.

`thermal.land_cover.dir` is already the scene package's `landcover/` when `scene.dir` is set (R0). Confirm in
`Config/ScenePackage.cpp` that the override happens before the subsystem init reads it.

Query handler, HAT/HOT extended (replace the `// CamSim has no terrain material data yet` comment and the `0` argument):

```cpp
			const uint32 Material = CamSimLandCover::QueryMaterialCode(bValid, R.bWater, /*bEntityHit=*/false,
				Req.Lat, Req.Lon, Subsystem ? Subsystem->GetLandCoverLookup() : nullptr);   // REALISM R2
			Sender->EnqueueHatHotExtendedResponse(Req.HatHotId, bValid, HAT, HOT, Material, NormalAz, NormalEl);
```

HAT/HOT uses the request's lat/lon. It is a vertical trace, so that is the hit's cell.

LOS extended, before `Sender->EnqueueLosExtendedResponse(Resp)`:

```cpp
	Resp.Material = CamSimLandCover::QueryMaterialCode(Hit != nullptr, /*bWater=*/false,
		Hit && Cast<ACamSimEntity>(Hit->GetActor()) != nullptr, Lat, Lon, Subsystem->GetLandCoverLookup());   // REALISM R2
```

Check the field name on `FCigiLosExtendedResponse` (`CigiPacketTypes.h:264`), and that `CigiSender.cpp:272` sends it
when `bValid` (it does for HAT/HOT at :241; mirror that). `Hit` is null for a clear segment, so the material is 0 there.

- [ ] **Step 4: Build and run**

Run: `scripts/run.sh --build-only`, then the automation run with `RunTests CamSim.Thermal.LandCover+CamSim.CIGI`.
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source && git commit -m "feat(cigi): land-cover material codes in HAT/HOT and LOS extended responses (REALISM R2)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: Pendleton build and road registration (gates 1–2)

**Files:**
- Create: `scripts/scene/tools/road_registration.py`
- Test: `scripts/scene/tests/test_road_registration.py`
- Modify: `scripts/scene/tools/README.md` (one entry)

**Interfaces:**
- Consumes: the package's `manifest.json` (OSM asset path via the fetch cache), `read_osm`, and the package's imagery
  TMS at z17.
- Produces:
  - `best_offset(profile: np.ndarray, step_m: float, road_w_m: float) -> float | None`: the signed offset (m) of the
    best dark-box match from the profile centre, or None when the match contrast is below `MIN_CONTRAST_DN`;
  - the CLI `uv run --project scripts/scene python scripts/scene/tools/road_registration.py PKG [--n 200] [--seed 0] [--json out.json]`.

- [ ] **Step 1: Write failing tests**

```python
import numpy as np

from tools.road_registration import best_offset  # tests already import tools.* (see test_coast_check.py)


def test_best_offset_finds_the_dark_band():
    x = np.arange(-30.0, 30.01, 0.5)
    prof = np.where(np.abs(x - 4.0) <= 5.0, 60.0, 160.0)
    assert abs(best_offset(prof, 0.5, 10.0) - 4.0) <= 0.5


def test_best_offset_none_without_contrast():
    assert best_offset(np.full(121, 120.0), 0.5, 10.0) is None


def test_best_offset_bright_road_is_none():
    x = np.arange(-30.0, 30.01, 0.5)
    prof = np.where(np.abs(x) <= 5.0, 200.0, 120.0)  # bright concrete: not a dark band
    assert best_offset(prof, 0.5, 10.0) is None
```

Check how `test_coast_check.py` imports tools and follow it.

- [ ] **Step 2: Run the tests and watch them fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_road_registration.py -q`
Expected: FAIL (import error)

- [ ] **Step 3: Implement `tools/road_registration.py`**

```python
"""REALISM R2 gate 2: OSM road centrelines vs NAIP roads. Samples N points (seeded) on motorway..tertiary centrelines at
least 50 m from a junction or a skipped (bridge/tunnel) way, reads a +-30 m luminance profile across the road from the
package imagery (z17 TMS, bilinear), and finds the offset of the best match to a dark box of the road's width. Gate:
median |offset| <= 10 m. Profiles without a dark band (concrete, shade, trees) are excluded and counted."""

MIN_CONTRAST_DN = 15.0
GATE_M = 10.0
HALF_M = 30.0
STEP_M = 0.5


def best_offset(profile, step_m, road_w_m):
    import numpy as np

    n = len(profile)
    half = max(1, int(round(road_w_m / 2 / step_m)))
    best, best_c = None, MIN_CONTRAST_DN
    for c in range(half, n - half):
        inside = profile[c - half : c + half + 1].mean()
        lo, hi = max(0, c - 3 * half), min(n, c + 3 * half + 1)
        ring = np.concatenate([profile[lo : c - half], profile[c + half + 1 : hi]])
        if ring.size == 0:
            continue
        contrast = ring.mean() - inside
        if contrast > best_c:
            best, best_c = c, contrast
    return None if best is None else (best - (n - 1) / 2) * step_m
```

The rest of the CLI:
1. Load the manifest and features (`pipeline._load_features` with a cache-backed context, as `build` does).
2. Keep roads in motorway…tertiary with `code_for == 200` (dark asphalt; concrete is excluded by design).
3. Build a junction set: nodes shared by two or more kept ways, taken from the ways' coordinate tuples rounded to 1e-7.
4. Sample points along the ways by cumulative length with `random.Random(seed)`. Reject points within 50 m of a junction
   or the margin edge.
5. Read imagery luminance on the perpendicular with a small TMS sampler. Reuse the z17 tile read in
   `tools/registration.py` if it has one; otherwise decode with Pillow and use bilinear sampling.
6. Print the median, p90, n used and n excluded. Write JSON. Exit 1 when the median exceeds `GATE_M`.

- [ ] **Step 4: Run the tests**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_road_registration.py -q`
Expected: PASS

- [ ] **Step 5: Build Pendleton with features and run gates 1–2**

```bash
cd /Users/mclayton/developer/simulation_apps/camsim
PKG=<the existing Pendleton sim package dir; see docs/scene-packages.md "Measured">
cp -r "$PKG/landcover" .cache/r1-landcover-ref            # reference for the features-off check
uv run --project scripts/scene camsim-scene plan scripts/scene/examples/pendleton.toml "$PKG"
uv run --project scripts/scene camsim-scene fetch "$PKG"
uv run --project scripts/scene camsim-scene build "$PKG"     # reports landcover build time
uv run --project scripts/scene camsim-scene verify --deep "$PKG"
python3 - "$PKG" <<'EOF'   # burned texels per code
import json, sys, numpy as np
from pathlib import Path
from PIL import Image
lc = Path(sys.argv[1]) / "landcover"
idx = json.loads((lc / "index.json").read_text())
tot = {}
for t in idx["tiles"]:
    a = np.array(Image.open(lc / t["file"]))
    for c in (200, 201, 210, 220):
        tot[c] = tot.get(c, 0) + int((a == c).sum())
print(tot, idx.get("codes"))
EOF
```

Then:
- **Determinism:** copy the package's `landcover/` aside, delete `.state/landcover.json`, rebuild, and confirm
  `diff -r` is empty.
- **Features off:** build with a copy of `pendleton.toml` whose `[features]` flags are all false into a scratch package
  that copies the R1 manifest. `diff -r` its `landcover/` against `.cache/r1-landcover-ref`; it must be empty.

Run gate 2:

```bash
uv run --project scripts/scene python scripts/scene/tools/road_registration.py "$PKG" --n 200 --json .cache/r2/road_registration.json
```

Expected: median ≤ 10 m. Record the median, p90, n and excluded count for Task 10.

- [ ] **Step 6: Commit** (the tool and its tests; no package data)

```bash
git add scripts/scene && git commit -m "feat(scene): road registration tool; Pendleton R2 build (REALISM R2 gates 1-2)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 9: `thermal_check.py` gate r (road vs verge)

**Files:**
- Modify: `scripts/thermal_check.py`: the docstring gate list, constants, `expected_rows`, `_poses`, `build_runs`, a new
  `road_rows()` check, report wiring, and a `--scene` argument.
- Test: pure helpers are exercised by a small pytest file `scripts/tests/test_thermal_check_roads.py` (if
  `scripts/tests/` doesn't exist, put it next to the existing thermal_check tests: `grep -rl thermal_check scripts --include=test_*.py`).

**Interfaces:**
- Produces:
  - `road_masks(shape, gsd_m, road_w_m, verge_side: int) -> (road_mask, verge_mask)`;
  - `road_row(band, tod, y_mean, masks, threshold) -> dict` (gate row r);
  - run label `"<band>_roads"` with env `CAMSIM_SCENE_DIR`.

- [ ] **Step 1: Pick the I-5 pose (one-off)**

Find a straight southbound I-5 carriageway way in Pendleton (`highway=motorway`, `ref` contains `I 5`). It should be at
least 600 m long with a bearing change under 1° and NAIP showing undeveloped verge on the outer side for 40 m. Print its
centre, bearing, width, OSM id and the outer side (+1 = right of travel, −1 = left), computed in a scratch script from
`read_osm` on the package's cached PBF. Get the ground HAE at the centre from the package terrain with the point-height
helper the R1 tools use (`grep -n "def point_heights" -r scripts/scene`). Hard-code the result:

```python
ROAD_POSE = dict(lat=..., lon=..., ground_hae=..., heading=..., width_m=..., outer_side=+1, osm_way=...)  # I-5 SB, Pendleton
ROAD_UP_M = 300.0
ROAD_FOV = 20.0
ROAD_NOON_DN = 6.0  # (r) noon: carriageway - outer verge >= +6 DN (white-hot)
ROAD_NIGHT_DN = 2.0  # (r) night: >= +2 DN (asphalt keeps its heat)
ROAD_VERGE_M = (8.0, 28.0)  # verge strip: this far beyond the carriageway edge
ROAD_EDGE_INSET_M = 2.0  # road strip: carriageway minus this at each edge
```

- [ ] **Step 2: Write failing tests**

```python
import numpy as np

import thermal_check as tc


def test_road_masks_geometry():
    road, verge = tc.road_masks((200, 300), gsd_m=0.5, road_w_m=14.0, verge_side=+1)
    cols_r = np.nonzero(road.any(axis=0))[0]
    cols_v = np.nonzero(verge.any(axis=0))[0]
    assert abs((cols_r.min() + cols_r.max()) / 2 - 149.5) <= 1  # centred
    assert len(cols_r) == round((14.0 - 2 * tc.ROAD_EDGE_INSET_M) / 0.5)
    assert cols_v.min() > cols_r.max()  # right of travel = image right when heading up
    assert not (road & verge).any()


def test_road_row_pass_fail():
    y = np.full((200, 300), 100.0)
    road, verge = tc.road_masks((200, 300), 0.5, 14.0, +1)
    y[road] = 110.0
    assert tc.road_row("mwir", "noon", y, (road, verge), 6.0)["pass"]
    assert not tc.road_row("mwir", "noon", y, (road, verge), 12.0)["pass"]
```

- [ ] **Step 3: Run the tests and watch them fail**

Run: `uv run --with numpy --with pillow --with pytest pytest scripts/tests/test_thermal_check_roads.py -q`. Run it from
the repo root, with whatever the existing thermal_check tests are run with if that's different.
Expected: FAIL (`AttributeError: road_masks`)

- [ ] **Step 4: Implement**

```python
def road_masks(shape, gsd_m, road_w_m, verge_side):
    """Nadir, heading along the road (road vertical through the image centre). Central 60 % rows only."""
    h, w = shape
    cx = (w - 1) / 2.0
    x = (np.arange(w) - cx) * gsd_m
    half = road_w_m / 2.0
    road_cols = np.abs(x) <= half - ROAD_EDGE_INSET_M
    d = x * verge_side
    verge_cols = (d >= half + ROAD_VERGE_M[0]) & (d <= half + ROAD_VERGE_M[1])
    rows = central_mask(shape).any(axis=1)
    road = rows[:, None] & road_cols[None, :]
    verge = rows[:, None] & verge_cols[None, :]
    return road, verge


def road_row(band, tod, y, masks, threshold):
    road, verge = masks
    v = region_mean(y, road) - region_mean(y, verge)
    return {"check": "r", "band": band, "time": tod, "value": round(v, 2), "threshold": threshold,
            "pass": bool(v >= threshold), "detail": f"carriageway - outer verge mean Y, {int(road.sum())}/{int(verge.sum())} px"}
```

Make sure `road_cols` has exactly `round((road_w - 2·inset)/gsd)` columns: use the half-open test
`np.abs(x) < half - inset` with an even/odd adjustment, or build it from indices. Make the test pass rather than the
other way round.

Wiring:
- `_poses()`: `"road": static(scenario.Pose(ROAD_POSE["lat"], ROAD_POSE["lon"], ROAD_POSE["ground_hae"] + ROAD_UP_M, yaw=ROAD_POSE["heading"], gimbal_pitch=-90.0, fov_h=ROAD_FOV))`.
- `build_runs`: if `"roads" in wanted`, for each band add
  `RunSpec(f"{b}_roads", {"CAMSIM_IR_PRESET": BANDS[b], "CAMSIM_SCENE_DIR": args.scene}, 1, ["night", "noon"], lambda tod: [View("road", p["road"], 10, None)], ml=False, exec_cmds=["camsim.Thermal.Log 1"], dis_args=[])`.
  - `build_runs` needs the scene path: pass it as a new parameter `scene: str | None`.
  - Error out early with `--runs roads` but no `--scene`.
  - Check that `run_once` handles `dis_args=[]` by not starting the DIS sender. If it always starts it, add that branch.
- `run_group` / `run_band`: map the `_roads` suffix like `_lcoff`.
- `expected_rows`: `if "roads" in runs: rows += [("r", b, t) for b in bands for t in ("noon", "night")]`.
- In the evaluation pass, for `*_roads` runs compute `y_mean` = the temporal mean of the view's frames, and the GSD with
  `nadir_gsd_m(ROAD_UP_M, ROAD_FOV, width)`. Add `road_row` with `ROAD_NOON_DN` / `ROAD_NIGHT_DN`.
- Add an info row `r_info` with the asphalt and grassland temperatures parsed from the run's `camsim.Thermal.Log 1`
  lines. Find how the existing code parses those lines (`grep -n "Thermal.Log\|class_temps" scripts/thermal_check.py`)
  and reuse it.
- Docstring: add `(r)` to the gate list, and `roads` to `--runs` with a one-line description.

- [ ] **Step 5: Run the tests**

Run: the Step 3 command. Expected: PASS. Also `python3 scripts/thermal_check.py --help` shows `--scene`.

- [ ] **Step 6: Commit**

```bash
git add scripts && git commit -m "feat(thermal_check): gate r, road vs verge on the Pendleton package (REALISM R2)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 10: Acceptance runs and docs

**Files:**
- Modify: `docs/scene-packages.md` (codes table, `[features]`, sources, "Measured" R2 entry, known limits)
- Modify: `docs/thermal.md` (new codes and classes, `rail_ballast`, slot count, known limits)
- Modify: `docs/terrain-feedback.md` (replace the stale "extended is downgraded" text; add the material code table and
  rule)
- Modify: `docs/configuration.md` (`thermal.land_cover.dir` now also feeds CIGI material codes)
- Modify: `ROADMAP.md` (1.9: close "material code is 0"; add an R2 entry with the measurements)
- Modify: `REALISM.md` (R2 status, the 3DHP→NHD decision, NDVI moved to a follow-up)
- Modify: `CLAUDE.md` (one gotcha line: codes 200–220, the `[features]` ODbL opt-in, and the material rule)

- [ ] **Step 1: Regression runs**

```bash
scripts/run.sh --build-only
"$UE_BIN" unreal_project/CamSimTest/CamSimTest.uproject -ExecCmds="Automation RunTests CamSim+Quit" \
  -TestExit="Automation Test Queue Empty" -ReportExportPath=.cache/automation-report \
  -unattended -nullrhi -nosound -nosplash -DisablePython -log -stdout -FullStdOutLogOutput
scripts/run_gpu_tests.sh CamSim.GPU.Thermal
uv run --project scripts/scene --with pytest pytest scripts/scene/tests -q
```

Expected: all pass. Report counts from `.cache/automation-report/index.json`.

- [ ] **Step 2: Thermal gates**

```bash
python3 scripts/thermal_check.py --runs bands,lcoff,pan,entity      # 4A-4C gates a-q unchanged
python3 scripts/thermal_check.py --runs roads --scene "$PKG"         # gate r
```

Expected: everything passes. If gate r fails, record the values and the `r_info` temperatures. Do not change the
thresholds. If the closed-form contrast is the problem, look at the model; if it is the geometry (masks, pose), look at
the pose.

- [ ] **Step 3: Bench**

```bash
python3 scripts/bench/run_bench.py --site pendleton --out .cache/bench/r2 && python3 scripts/bench/compare.py .cache/bench/<r1 run> .cache/bench/r2
```

Use the R1 run named in `docs/scene-packages.md` "Measured". Expected: ThermalCS time within noise (no shader change).

- [ ] **Step 4: CIGI material check**

Launch with the package (`CAMSIM_SCENE_DIR="$PKG" scripts/run.sh`). Send extended HAT/HOT queries with
`scripts/send_cigi_test.py`; it has HAT/HOT options, check `--help`, and add a `--hat-extended LAT,LON` option in this
task if there's none. Query:
- the I-5 pose (expect 200);
- Lake O'Neill, about 33.3250, -117.3330 (expect 220 or 80);
- 33.25, -117.50 offshore (expect 80);
- an inland grass point (expect a WorldCover code).

Then send an LOS segment from the sensor to the I-5 point (expect 200). Record the responses.

- [ ] **Step 5: Visual review frames for the human**

Capture MWIR and LWIR noon frames at three places: I-5 (the gate pose), an Oceanside residential grid (pick a pose 300 m
up), and the MCAS Camp Pendleton runway (33.3017, -117.3550, 600 m up). Put them in `.cache/r2/visual/` and list them in
the report. The review questions are spec Acceptance 6.

- [ ] **Step 6: Docs**

Write the measured numbers into `docs/scene-packages.md` "Measured", in the style of the R1 entries: date, hardware,
burned texels per code, build time, the road-registration median and p90, gate r values, and the CIGI responses.

Copy the spec's "Known limits" into `docs/scene-packages.md` and `docs/thermal.md`.

Update `REALISM.md` R2:
- strike through what's done;
- say that NDVI moved to a follow-up spec;
- say that NHD HR is used and 3DHP is deferred, and why.

In `ROADMAP.md`, record the visual review as editor-free human work.

- [ ] **Step 7: Commit**

```bash
git add docs ROADMAP.md REALISM.md CLAUDE.md scripts && git commit -m "docs(realism): R2 vector features measured on Pendleton

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```
