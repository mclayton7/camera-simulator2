# Detail Polygons (REALISM R5) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A scene package can cover a ~200 km box at `preview` and build named GeoJSON polygons inside it at `sim`, fetching NAIP and 3DEP 1 m only for the polygons.

**Architecture:** Polygons are validated and normalised at plan time (`camsim_scene/detail.py`), stored in the manifest (`detail` section) and as regions (`detail:<name>` at the sim zooms, `detail_margin:<name>` = polygon + `imagery_margin_km`). Sources that only the detail profile uses (`dep3_1m`, `naip_pc`) are *scoped*: discovered per polygon, their footprints clipped to the scope at fetch, and their samples masked to it in every tile job, so the existing tile expansion, NAIP feather and colour match handle the polygon edges unchanged.

**Tech Stack:** Python 3.12, shapely 2, numpy, pytest + pytest-xdist (`uv run --project scripts/scene`), the reference build container (`scripts/scene/build.sh`), CamSim (`scripts/run.sh`, Docker image) for the render gates.

**Spec:** `docs/superpowers/specs/2026-10-14-r5-detail-polygons-design.md`

## Global Constraints

- Without `[detail]`, a scene's regions, layer settings and `manifest.json` are byte-identical to before (gate 1). Never add a manifest key or a region field that is emitted when the feature is unused.
- `[detail]` keys: `file` (required, relative to the toml's directory), `profile` (only `"sim"`), `scoped` (optional table `terrain = [...]`, `imagery = [...]`).
- The box `profile` must differ from the detail profile (in practice `preview`).
- Coordinates are snapped to 1e-7 degrees (`shapely.set_precision`) and normalised (`shapely.normalize`) before they are stored; WKT is written with `rounding_precision=7`.
- Scope by layer: a scoped terrain source → union of `detail:` regions; a scoped imagery source → union of `detail_margin:` regions.
- Default scoped sources: in the detail profile's priorities for the layer and not in the box profile's (today `dep3_1m`, `naip_pc`).
- Builds and `verify` never read the GeoJSON file; only `plan` / `--replan` do.
- No CamSim (C++) change.
- Run tests with `TMPDIR=/opt/mac/tmp` on the Linux box (pytest keeps ~4 GB of temp files): `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests -n auto -q`.
- Commits end with the session's `Co-Authored-By` / `Claude-Session` trailer lines.
- Only one CamSim at a time; don't run CPU/GPU-heavy work while a bench measures.

## Review Focus

1. **A polygon touching the bbox edge or another polygon** — expected: accepted (`covers`, not `contains`), and overlapping polygons simply union in the scope. Test in Task 2.
2. **A MultiPolygon or a polygon with a hole** — expected: one region per part, holes kept (no sim tiles inside a hole unless the margin covers them). Tests in Task 1 (Region holes) and Task 2 (parts and names).
3. **A scoped asset that overhangs its scope** (a NAIP quarter-quad half inside the margin) — expected: no pixel of it outside the scope in any tile, including the coarse leaves just outside. Test in Task 5 (end-to-end).
4. **Feature order, ring orientation or float noise in the GeoJSON** — expected: same manifest and hash. Test in Task 2.
5. **Re-planning after the GeoJSON changes** — expected: `build` without `--replan` refuses (regions differ), `--replan` picks up the new polygons. Test in Task 3.

---

## File map

- Create `scripts/scene/camsim_scene/detail.py`: load/validate/normalise polygons, hash, regions, km buffer, scope geometries, per-polygon discovery, clipping, the sample mask.
- Modify `scripts/scene/camsim_scene/tiling.py`: `Region.holes`, `Region.from_polygon`.
- Modify `scripts/scene/camsim_scene/config.py`: `[detail]` parsing, defaults from the detail profile, `zoom.detail`, regions, `scoped` layer settings, `load_scene` base dir.
- Modify `scripts/scene/camsim_scene/manifest.py`: optional `detail` field.
- Modify `scripts/scene/camsim_scene/pipeline.py`: per-polygon discovery, `detail` section, footprint clip at fetch.
- Modify `scripts/scene/camsim_scene/context.py`: clip discovery bboxes in `coverage`, `Entry.clip`.
- Modify `scripts/scene/camsim_scene/layers/imagery.py`, `layers/terrain.py`: mask samples by `Entry.clip`.
- Modify `scripts/scene/camsim_scene/verify.py`: `detail` check.
- Create `scripts/scene/tools/detail_check.py` (gates 2 and 3); modify `tools/edge_shots.py` (`--crop`), `tools/render_check.py` (`--shots detail`).
- Create `scripts/scene/examples/pendleton-r5veg.toml`, `scripts/scene/examples/socal-r5.toml`, `scripts/scene/examples/socal-r5-detail.geojson`.
- Tests: `scripts/scene/tests/test_detail.py`, `test_detail_config.py`, `test_detail_build.py`, `test_detail_check.py`, `fixtures/plan_golden/*.json`.
- Docs: `docs/scene-packages.md` ("Detail polygons (R5)", "Measured"), `REALISM.md`, `ROADMAP.md`, `CLAUDE.md`.

---

### Task 0: Golden plans of the existing examples (gate 1, before any change)

**Files:**
- Create: `scripts/scene/examples/pendleton-r5veg.toml` (copy of `.cache/linux-tomls/pendleton-r5veg.toml`)
- Create: `scripts/scene/tests/fixtures/plan_golden/{pendleton,pendleton-preview-10k,pendleton-r5veg}.json`
- Create: `scripts/scene/tests/test_detail_identity.py`

**Interfaces:**
- Produces: golden files `{"regions": [...], "layers": {...}}` in `canonical_json` form, used by gate 1 for the rest of the plan.

- [ ] **Step 1: Add the R4 example toml**

```bash
cp .cache/linux-tomls/pendleton-r5veg.toml scripts/scene/examples/pendleton-r5veg.toml
```
Check the copy's first comment line says what it is; if not, replace line 1 with:
`# REALISM R4 deeper: Camp Pendleton sim package with R2 features, R3/R3b buildings, CHMv2 canopy and the site's vegetation mix`

- [ ] **Step 2: Generate the goldens from the unchanged code**

```bash
cd /opt/mac/camera-simulator2
mkdir -p scripts/scene/tests/fixtures/plan_golden
uv run --project scripts/scene python - <<'EOF'
from pathlib import Path
from camsim_scene.config import layer_settings, load_scene
from camsim_scene.manifest import canonical_json
ex = Path("scripts/scene/examples")
out = Path("scripts/scene/tests/fixtures/plan_golden")
for name in ("pendleton", "pendleton-preview-10k", "pendleton-r5veg"):
    plan = load_scene(ex / f"{name}.toml")
    doc = {"regions": [r.to_dict() for r in plan.regions()], "layers": layer_settings(plan)}
    (out / f"{name}.json").write_text(canonical_json(doc))
    print(name, "ok")
EOF
```
Expected: three `ok` lines.

- [ ] **Step 3: Write the identity test**

`scripts/scene/tests/test_detail_identity.py`:
```python
"""REALISM R5 gate 1: without [detail], planning is byte-identical to before R5 (goldens made before the change)."""

from __future__ import annotations

from pathlib import Path

import pytest

from camsim_scene.config import layer_settings, load_scene
from camsim_scene.manifest import canonical_json

EXAMPLES = Path(__file__).resolve().parents[1] / "examples"
GOLDEN = Path(__file__).parent / "fixtures" / "plan_golden"


@pytest.mark.parametrize("name", ["pendleton", "pendleton-preview-10k", "pendleton-r5veg"])
def test_plan_without_detail_is_unchanged(name):
    plan = load_scene(EXAMPLES / f"{name}.toml")
    got = canonical_json({"regions": [r.to_dict() for r in plan.regions()], "layers": layer_settings(plan)})
    assert got == (GOLDEN / f"{name}.json").read_text(encoding="utf-8")
```

- [ ] **Step 4: Run it**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests/test_detail_identity.py -q`
Expected: 3 passed.

- [ ] **Step 5: Commit**

```bash
git add scripts/scene/examples/pendleton-r5veg.toml scripts/scene/tests/fixtures/plan_golden scripts/scene/tests/test_detail_identity.py
git commit -m "test(scene): golden plans of the example scenes before R5 (gate 1)"
```

---

### Task 1: Regions with holes

**Files:**
- Modify: `scripts/scene/camsim_scene/tiling.py` (class `Region`, ~line 47)
- Test: `scripts/scene/tests/test_tiling.py`

**Interfaces:**
- Produces: `Region(name, polygon, max_zoom, holes=[])`; `Region.from_polygon(name: str, geom: shapely.Polygon, max_zoom: dict[str, int]) -> Region`; `to_dict()` emits `"holes"` only when non-empty; `from_dict` reads it when present.

- [ ] **Step 1: Write the failing tests** (append to `test_tiling.py`)

```python
def test_region_from_polygon_keeps_holes_and_round_trips():
    import shapely

    from camsim_scene.tiling import Region

    g = shapely.Polygon([(0, 0), (4, 0), (4, 4), (0, 4)], [[(1, 1), (2, 1), (2, 2), (1, 2)]])
    r = Region.from_polygon("detail:a", g, {"terrain": 5})
    assert r.geometry().equals(g)
    d = r.to_dict()
    assert d["holes"] == [[[1.0, 1.0], [2.0, 1.0], [2.0, 2.0], [1.0, 2.0], [1.0, 1.0]]]
    assert Region.from_dict(d).geometry().equals(g)


def test_region_without_holes_serialises_as_before():
    from camsim_scene.tiling import Region

    r = Region.from_bounds("bbox", (0.0, 0.0, 1.0, 1.0), {"imagery": 3})
    assert set(r.to_dict()) == {"name", "polygon", "max_zoom"}
    assert Region.from_dict(r.to_dict()).holes == []
```

- [ ] **Step 2: Run them to see them fail**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests/test_tiling.py -q -k region`
Expected: FAIL (`AttributeError: type object 'Region' has no attribute 'from_polygon'`).

- [ ] **Step 3: Implement** (replace the `Region` class in `tiling.py`)

```python
@dataclass
class Region:
    """A lon/lat polygon (optionally with holes) with a maximum zoom per layer. A tile takes the highest limit of the
    regions it overlaps."""

    name: str
    polygon: list[tuple[float, float]]
    max_zoom: dict[str, int]
    holes: list[list[tuple[float, float]]] = field(default_factory=list)

    @classmethod
    def from_bounds(cls, name: str, b: Bounds, max_zoom: dict[str, int]) -> Region:
        w, s, e, n = (float(v) for v in b)
        return cls(name, [(w, s), (e, s), (e, n), (w, n), (w, s)], dict(max_zoom))

    @classmethod
    def from_polygon(cls, name: str, geom: shapely.Polygon, max_zoom: dict[str, int]) -> Region:
        def ring(r):
            return [(float(a), float(b)) for a, b in r.coords]

        return cls(name, ring(geom.exterior), dict(max_zoom), [ring(h) for h in geom.interiors])

    def geometry(self) -> shapely.Polygon:
        return shapely.Polygon(self.polygon, self.holes or None)

    def to_dict(self) -> dict:
        d = {
            "name": self.name,
            "polygon": [[float(a), float(b)] for a, b in self.polygon],
            "max_zoom": {k: int(v) for k, v in sorted(self.max_zoom.items())},
        }
        if self.holes:  # regions without holes keep their exact manifest form
            d["holes"] = [[[float(a), float(b)] for a, b in h] for h in self.holes]
        return d

    @classmethod
    def from_dict(cls, d: dict) -> Region:
        return cls(
            d["name"],
            [(float(a), float(b)) for a, b in d["polygon"]],
            {k: int(v) for k, v in d["max_zoom"].items()},
            [[(float(a), float(b)) for a, b in h] for h in d.get("holes", [])],
        )
```

- [ ] **Step 4: Run the tests**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests/test_tiling.py scripts/scene/tests/test_detail_identity.py -q`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add scripts/scene/camsim_scene/tiling.py scripts/scene/tests/test_tiling.py
git commit -m "feat(scene): regions with holes (REALISM R5)"
```

---

### Task 2: `detail.py` — polygons, hash, regions, scopes, clip, mask

**Files:**
- Create: `scripts/scene/camsim_scene/detail.py`
- Test: `scripts/scene/tests/test_detail.py`

**Interfaces:**
- Consumes: `Region.from_polygon` (Task 1); `config.ConfigError`, `config.NAME_RE`, `config.KM_PER_DEG`; `manifest.canonical_json`; `fsutil.sha256_bytes`.
- Produces (used by Tasks 3–7):
  - `DETAIL = "detail"`, `MARGIN = "detail_margin"`
  - `load_polygons(path: Path, bbox: Bounds) -> list[dict]` — sorted `[{"name": str, "wkt": str}]`; raises `ConfigError`
  - `polygons_sha256(polygons: list[dict]) -> str`
  - `section(polygons: list[dict], profile: str) -> dict` — `{"profile", "polygons", "sha256"}`
  - `buffer_km(geom, km: float)` — snapped, normalised geometry
  - `regions(polygons, detail_zoom: dict, margin_zoom: dict, margin_km: float) -> list[Region]`
  - `scope_geometry(regions: list[Region], kind: str)` — union or `None`
  - `scoped_map(layers: dict) -> dict[str, str]` — source id → kind, from `layers.terrain.scoped` / `layers.imagery.scoped`
  - `scopes(m) -> dict[str, geometry]` — source id → scope geometry, from a `Manifest`
  - `clip(geom, scope)` — polygonal intersection snapped to 1e-7, or `None` when empty
  - `discover_in(src, scope) -> list[Asset]`
  - `inside(scope, lon, lat) -> np.ndarray` (bool)

- [ ] **Step 1: Write the failing tests**

`scripts/scene/tests/test_detail.py`:
```python
"""REALISM R5 detail polygons: loading, normalisation, regions, scopes, clipping, the sample mask."""

from __future__ import annotations

import json

import numpy as np
import pytest
import shapely

from camsim_scene import detail as D
from camsim_scene.config import ConfigError
from camsim_scene.sources.base import Asset
from camsim_scene.tiling import Region

BBOX = (10.0, 10.0, 10.5, 10.5)
SQUARE = [[10.1, 10.1], [10.2, 10.1], [10.2, 10.2], [10.1, 10.2], [10.1, 10.1]]


def fc(*features, crs=None) -> dict:
    d = {"type": "FeatureCollection", "features": list(features)}
    if crs:
        d["crs"] = {"type": "name", "properties": {"name": crs}}
    return d


def feat(name, coords, kind="Polygon") -> dict:
    return {"type": "Feature", "properties": {"name": name}, "geometry": {"type": kind, "coordinates": coords}}


def write(tmp_path, doc) -> object:
    p = tmp_path / "detail.geojson"
    p.write_text(json.dumps(doc))
    return p


def test_loads_sorted_and_normalised(tmp_path):
    p = write(tmp_path, fc(feat("b", [SQUARE]), feat("a", [SQUARE[::-1]])))
    polys = D.load_polygons(p, BBOX)
    assert [x["name"] for x in polys] == ["a", "b"]
    assert polys[0]["wkt"] == polys[1]["wkt"]  # orientation normalised


def test_hash_ignores_order_orientation_and_noise(tmp_path):
    a = D.load_polygons(write(tmp_path, fc(feat("x", [SQUARE]), feat("y", [SQUARE]))), BBOX)
    noisy = [[x + 2e-9, y - 2e-9] for x, y in SQUARE[::-1]]
    noisy[-1] = noisy[0]
    b = D.load_polygons(write(tmp_path, fc(feat("y", [noisy]), feat("x", [SQUARE]))), BBOX)
    assert D.polygons_sha256(a) == D.polygons_sha256(b)


@pytest.mark.parametrize(
    "doc, msg",
    [
        ({"type": "Feature"}, "FeatureCollection"),
        (fc(), "FeatureCollection"),
        (fc(feat("Bad Name", [SQUARE])), "properties.name"),
        (fc(feat("a", [SQUARE]), feat("a", [SQUARE])), "duplicate"),
        (fc(feat("a", [[10.1, 10.1], [10.2, 10.1]], "LineString")), "Polygon or MultiPolygon"),
        (fc(feat("a", [[[10.1, 10.1], [10.2, 10.2], [10.2, 10.1], [10.1, 10.2], [10.1, 10.1]]])), "invalid"),
        (fc(feat("a", [[[9.9, 10.1], [10.2, 10.1], [10.2, 10.2], [9.9, 10.2], [9.9, 10.1]]])), "inside the bbox"),
        (fc(feat("a", [SQUARE]), crs="urn:ogc:def:crs:EPSG::3857"), "EPSG:4326"),
    ],
)
def test_rejects(tmp_path, doc, msg):
    with pytest.raises(ConfigError, match=msg):
        D.load_polygons(write(tmp_path, doc), BBOX)


def test_missing_file_is_a_config_error(tmp_path):
    with pytest.raises(ConfigError, match="detail.file"):
        D.load_polygons(tmp_path / "nope.geojson", BBOX)


def test_polygon_on_the_bbox_edge_is_accepted(tmp_path):
    edge = [[10.0, 10.0], [10.5, 10.0], [10.5, 10.1], [10.0, 10.1], [10.0, 10.0]]
    assert D.load_polygons(write(tmp_path, fc(feat("a", [edge]), crs="urn:ogc:def:crs:OGC:1.3:CRS84")), BBOX)


def test_regions_per_part_with_margin(tmp_path):
    other = [[10.3, 10.3], [10.4, 10.3], [10.4, 10.4], [10.3, 10.4], [10.3, 10.3]]
    polys = D.load_polygons(write(tmp_path, fc(feat("m", [[SQUARE], [other]], "MultiPolygon"))), BBOX)
    rs = D.regions(polys, {"terrain": 16, "imagery": 17}, {"terrain": 10, "imagery": 17}, 3.0)
    assert [r.name for r in rs] == ["detail:m/0", "detail:m/1", "detail_margin:m/0", "detail_margin:m/1"]
    assert rs[0].max_zoom == {"terrain": 16, "imagery": 17} and rs[2].max_zoom == {"terrain": 10, "imagery": 17}
    inner, outer = rs[0].geometry(), rs[2].geometry()
    assert outer.contains(inner)
    # 3 km of latitude ~ 0.02695 degrees
    assert outer.bounds[3] - inner.bounds[3] == pytest.approx(3.0 / 111.32, abs=1e-6)


def test_single_part_names_have_no_index(tmp_path):
    polys = D.load_polygons(write(tmp_path, fc(feat("p", [SQUARE]))), BBOX)
    assert [r.name for r in D.regions(polys, {"terrain": 1}, {"terrain": 1}, 0.0)] == ["detail:p", "detail_margin:p"]


def test_scope_geometry_and_scoped_map():
    rs = [
        Region.from_bounds("bbox", BBOX, {}),
        Region.from_bounds("detail:a", (10.1, 10.1, 10.2, 10.2), {}),
        Region.from_bounds("detail:b", (10.3, 10.3, 10.4, 10.4), {}),
        Region.from_bounds("detail_margin:a", (10.0, 10.0, 10.3, 10.3), {}),
    ]
    assert D.scope_geometry(rs, D.DETAIL).area == pytest.approx(0.02)
    assert D.scope_geometry(rs, D.MARGIN).area == pytest.approx(0.09)
    assert D.scope_geometry(rs[:1], D.DETAIL) is None
    layers = {"terrain": {"scoped": {"dep": "detail"}}, "imagery": {"scoped": {"naip": "detail_margin"}}}
    assert D.scoped_map(layers) == {"dep": "detail", "naip": "detail_margin"}
    assert D.scoped_map({"terrain": {}, "imagery": {}}) == {}


def test_clip_is_polygonal_or_none():
    scope = shapely.box(0, 0, 1, 1)
    assert D.clip(shapely.box(0.5, 0.5, 2, 2), scope).equals(shapely.box(0.5, 0.5, 1, 1))
    assert D.clip(shapely.box(1, 0, 2, 1), scope) is None  # touches along an edge only
    assert D.clip(shapely.box(3, 3, 4, 4), scope) is None


def test_discover_in_queries_each_part_and_merges():
    seen = []

    class Src:
        def discover(self, area):
            seen.append(tuple(area.bounds))
            return [
                Asset("near", "u1", metadata={"bbox": [0.5, 0.5, 1.5, 1.5]}),
                Asset("far", "u2", metadata={"bbox": [5.0, 5.0, 6.0, 6.0]}),
                Asset("nobbox", "u3"),
            ]

    scope = shapely.union_all([shapely.box(0, 0, 1, 1), shapely.box(2, 2, 3, 3)])
    got = D.discover_in(Src(), scope)
    assert [a.id for a in got] == ["near", "nobbox"]
    assert sorted(seen) == [(0.0, 0.0, 1.0, 1.0), (2.0, 2.0, 3.0, 3.0)]


def test_inside_mask():
    scope = shapely.box(0, 0, 1, 1)
    lon = np.array([[0.5, 1.5], [1.0, -0.1]])
    lat = np.array([[0.5, 0.5], [0.5, 0.5]])
    assert D.inside(scope, lon, lat).tolist() == [[True, False], [True, False]]
```

- [ ] **Step 2: Run them to see them fail**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests/test_detail.py -q`
Expected: FAIL (`ModuleNotFoundError: No module named 'camsim_scene.detail'`).

- [ ] **Step 3: Implement `detail.py`**

```python
"""Detail polygons (REALISM R5, docs/superpowers/specs/2026-10-14-r5-detail-polygons-design.md): named areas built
at the detail profile (sim) inside a package built at its own profile (preview).

Polygons come from a GeoJSON FeatureCollection, are validated and normalised once by `plan` and live on in the
manifest (`detail`) and as regions (`detail:<name>` at the detail zooms, `detail_margin:<name>` = the polygon grown by
imagery_margin_km); builds never read the file again. Sources only the detail profile uses are *scoped*: discovered
per polygon, their footprints clipped to the scope, their samples masked to it."""

from __future__ import annotations

import json
import math
from pathlib import Path

import numpy as np
import shapely
import shapely.affinity
import shapely.geometry

from .config import KM_PER_DEG, NAME_RE, ConfigError
from .fsutil import sha256_bytes
from .manifest import canonical_json
from .tiling import Bounds, Region

DETAIL = "detail"
MARGIN = "detail_margin"
KINDS = (DETAIL, MARGIN)
PRECISION = 1e-7
WGS84_NAMES = ("EPSG::4326", "EPSG:4326", "CRS84")


def normalise(geom):
    return shapely.normalize(shapely.set_precision(geom, PRECISION))


def load_polygons(path: Path, bbox: Bounds) -> list[dict]:
    """The file's features as [{"name", "wkt"}] sorted by name; ConfigError naming the feature on any problem."""
    try:
        doc = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        raise ConfigError(f"detail.file {path}: {e}") from e
    feats = doc.get("features") if isinstance(doc, dict) else None
    if not isinstance(doc, dict) or doc.get("type") != "FeatureCollection" or not isinstance(feats, list) or not feats:
        raise ConfigError(f"detail.file {path}: needs a GeoJSON FeatureCollection with at least one feature")
    crs = doc.get("crs")
    if crs is not None:
        name = str(((crs or {}).get("properties") or {}).get("name", "")) if isinstance(crs, dict) else ""
        if not name.endswith(WGS84_NAMES):
            raise ConfigError(f"detail.file {path}: crs must be EPSG:4326 / CRS84 lon/lat, got {name!r}")
    box = shapely.box(*bbox)
    out, seen = [], set()
    for i, f in enumerate(feats):
        name = ((f or {}).get("properties") or {}).get("name") if isinstance(f, dict) else None
        where = f"detail.file feature {i} ({name!r})"
        if not isinstance(name, str) or not NAME_RE.match(name):
            raise ConfigError(f"{where}: properties.name must match {NAME_RE.pattern}")
        if name in seen:
            raise ConfigError(f"{where}: duplicate name")
        seen.add(name)
        try:
            geom = shapely.geometry.shape(f.get("geometry"))
        except Exception as e:  # noqa: BLE001 - any malformed geometry is a config error
            raise ConfigError(f"{where}: bad geometry: {e}") from e
        if geom.geom_type not in ("Polygon", "MultiPolygon"):
            raise ConfigError(f"{where}: geometry must be a Polygon or MultiPolygon, got {geom.geom_type}")
        if geom.is_empty or not geom.is_valid:
            raise ConfigError(f"{where}: geometry is empty or invalid ({shapely.is_valid_reason(geom)})")
        geom = normalise(geom)
        if geom.is_empty or not geom.is_valid:
            raise ConfigError(f"{where}: geometry is invalid after snapping to 1e-7 degrees")
        if not box.covers(geom):  # inside the bbox, so it cannot cross the antimeridian either
            raise ConfigError(f"{where}: must lie inside the bbox {list(bbox)}")
        out.append({"name": name, "wkt": shapely.to_wkt(geom, rounding_precision=7)})
    return sorted(out, key=lambda p: p["name"])


def polygons_sha256(polygons: list[dict]) -> str:
    return sha256_bytes(canonical_json(polygons).encode())


def section(polygons: list[dict], profile: str) -> dict:
    """The manifest's `detail` section."""
    return {"profile": profile, "polygons": polygons, "sha256": polygons_sha256(polygons)}


def buffer_km(geom, km: float):
    """geom grown by km: degrees of latitude, longitude scaled by the pole-ward latitude (as config.ring_bounds, so
    the pad is never too small)."""
    if km <= 0:
        return geom
    _, s, _, n = geom.bounds
    dlat = km / KM_PER_DEG
    k = math.cos(math.radians(min(89.0, max(abs(s), abs(n)) + dlat)))
    g = shapely.affinity.scale(geom, xfact=k, yfact=1.0, origin=(0.0, 0.0))
    g = g.buffer(dlat, quad_segs=8)
    return normalise(shapely.affinity.scale(g, xfact=1.0 / k, yfact=1.0, origin=(0.0, 0.0)))


def regions(polygons: list[dict], detail_zoom: dict, margin_zoom: dict, margin_km: float) -> list[Region]:
    """`detail:<name>[/<part>]` per polygon part at detail_zoom, then `detail_margin:<name>[/<part>]` (the part grown by
    margin_km) at margin_zoom."""
    out: list[Region] = []
    for kind, zoom, grow in ((DETAIL, detail_zoom, 0.0), (MARGIN, margin_zoom, margin_km)):
        for p in polygons:
            parts = shapely.get_parts(shapely.from_wkt(p["wkt"])).tolist()
            for i, part in enumerate(parts):
                suffix = f"/{i}" if len(parts) > 1 else ""
                out.append(Region.from_polygon(f"{kind}:{p['name']}{suffix}", buffer_km(part, grow), zoom))
    return out


def scope_geometry(regions_: list[Region], kind: str):
    """Union of the regions of one kind ("detail" or "detail_margin"), or None when there are none."""
    gs = [r.geometry() for r in regions_ if r.name.split(":", 1)[0] == kind]
    return shapely.union_all(gs) if gs else None


def scoped_map(layers: dict) -> dict[str, str]:
    """Source id -> scope kind, from the manifest's (or layer_settings') terrain and imagery `scoped` tables."""
    out: dict[str, str] = {}
    for layer in ("terrain", "imagery"):
        out.update((layers.get(layer) or {}).get("scoped", {}))
    return out


def scopes(m) -> dict:
    """Source id -> scope geometry for a Manifest (empty without [detail])."""
    kinds = scoped_map(m.layers)
    if not kinds:
        return {}
    rs = m.region_objs()
    geoms = {k: scope_geometry(rs, k) for k in set(kinds.values())}
    return {sid: geoms[k] for sid, k in kinds.items() if geoms[k] is not None}


def clip(geom, scope):
    """The polygonal part of geom inside scope, snapped to 1e-7 degrees; None when nothing with area is left."""
    g = shapely.intersection(geom, scope)
    parts = [p for p in shapely.get_parts(g) if p.geom_type in ("Polygon", "MultiPolygon") and p.area > 0]
    if not parts:
        return None
    g = shapely.set_precision(shapely.union_all(parts), PRECISION)
    return None if g.is_empty else g


def discover_in(src, scope) -> list:
    """src.discover() once per part of scope (on its bounds), merged by asset id in first-seen order; assets whose
    metadata bbox misses the scope are dropped."""
    from .sources.base import Area

    out, seen = [], set()
    for part in sorted(shapely.get_parts(scope).tolist(), key=lambda g: g.bounds):
        for a in src.discover(Area(tuple(float(v) for v in part.bounds))):
            if a.id in seen:
                continue
            bb = a.metadata.get("bbox")
            if bb is not None and not shapely.intersects(shapely.box(*bb), scope):
                continue
            seen.add(a.id)
            out.append(a)
    return out


def inside(scope, lon, lat) -> np.ndarray:
    """True where (lon, lat) is in the scope or on its boundary."""
    return shapely.intersects_xy(scope, np.asarray(lon, np.float64), np.asarray(lat, np.float64))
```

- [ ] **Step 4: Run the tests**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests/test_detail.py -q`
Expected: all pass. If `test_rejects[...invalid]` fails because the bow-tie becomes valid after snapping, keep the first `is_valid` check (it runs before snapping) — it must raise there.

- [ ] **Step 5: Commit**

```bash
git add scripts/scene/camsim_scene/detail.py scripts/scene/tests/test_detail.py
git commit -m "feat(scene): detail polygons — load, normalise, regions, scopes, clip (REALISM R5)"
```

---

### Task 3: `[detail]` in scene.toml, the plan and the manifest

**Files:**
- Modify: `scripts/scene/camsim_scene/config.py` (`KEYS`, `ScenePlan`, `regions`, `parse_scene`, `load_scene`, `layer_settings`)
- Modify: `scripts/scene/camsim_scene/manifest.py` (`Manifest.detail`, `to_dict`)
- Modify: `scripts/scene/camsim_scene/pipeline.py` (`plan_scene`: the `detail` section)
- Test: `scripts/scene/tests/test_detail_config.py`

**Interfaces:**
- Consumes: `detail.load_polygons`, `detail.regions`, `detail.section`, `detail.DETAIL`, `detail.MARGIN` (Task 2).
- Produces:
  - `parse_scene(data: dict, base: Path | None = None) -> ScenePlan`
  - `ScenePlan.detail: dict` — `{}` or `{"profile": "sim", "polygons": [...], "scoped": {"terrain": [...], "imagery": [...]}}`
  - `ScenePlan.zoom["detail"]` (only with `[detail]`)
  - `layer_settings(plan)["terrain"]["scoped"]` / `["imagery"]["scoped"]` (only with `[detail]`)
  - `Manifest.detail: dict | None` (omitted from JSON when `None`)

- [ ] **Step 1: Write the failing tests**

`scripts/scene/tests/test_detail_config.py`:
```python
"""REALISM R5: [detail] in scene.toml -> ScenePlan, regions, layer settings and the manifest."""

from __future__ import annotations

import io
import json

import pytest
from fake_sources import synthetic_scene
from fakes import FakeHttp

from camsim_scene.config import ConfigError, layer_settings, load_scene, parse_scene
from camsim_scene.manifest import Manifest
from camsim_scene.pipeline import plan_differences, plan_scene

SQUARE = [[10.1, 10.1], [10.2, 10.1], [10.2, 10.2], [10.1, 10.2], [10.1, 10.1]]


def geojson(tmp_path, coords=SQUARE, name="a", fname="detail.geojson"):
    p = tmp_path / fname
    feat = {"type": "Feature", "properties": {"name": name}, "geometry": {"type": "Polygon", "coordinates": [coords]}}
    p.write_text(json.dumps({"type": "FeatureCollection", "features": [feat]}))
    return p


def detail_scene(tmp_path, **detail) -> dict:
    s = synthetic_scene(tmp_path / "src")
    s["profile"] = "preview"
    s["detail"] = {"file": str(geojson(tmp_path)), "scoped": {"terrain": ["hi_dem"], "imagery": ["hi_rgb"]}, **detail}
    return s


def test_detail_regions_and_settings(tmp_path):
    plan = parse_scene(detail_scene(tmp_path))
    names = [r.name for r in plan.regions()]
    assert names == ["globe", "ring", "margin", "bbox", "detail:a", "detail_margin:a"]
    assert plan.zoom["detail"] == {"terrain": 16, "imagery": 17}  # the sim profile's
    ls = layer_settings(plan)
    assert ls["terrain"]["scoped"] == {"hi_dem": "detail"} and ls["imagery"]["scoped"] == {"hi_rgb": "detail_margin"}


def test_without_detail_no_scoped_key_and_no_detail_zoom(tmp_path):
    plan = parse_scene(synthetic_scene(tmp_path / "src"))
    assert "detail" not in plan.zoom and plan.detail == {}
    ls = layer_settings(plan)
    assert "scoped" not in ls["terrain"] and "scoped" not in ls["imagery"]


def test_detail_profile_drives_default_priorities_and_ndvi(tmp_path):
    s = {"name": "x", "bbox": [10.0, 10.0, 10.5, 10.5], "profile": "preview", "detail": {"file": str(geojson(tmp_path))}}
    plan = parse_scene(s)
    assert plan.priorities["imagery"][0] == "naip_pc" and plan.ndvi is True
    assert plan.detail["scoped"] == {"terrain": ["dep3_1m"], "imagery": ["naip_pc"]}


def test_zoom_detail_override(tmp_path):
    s = detail_scene(tmp_path)
    s["zoom"]["detail"] = {"imagery": 15}
    assert parse_scene(s).zoom["detail"] == {"terrain": 16, "imagery": 15}


@pytest.mark.parametrize(
    "change, msg",
    [
        ({"profile": "preview2"}, "detail.profile"),
        ({"file": ""}, "detail.file"),
        ({"scoped": {"terrain": ["nope"]}}, "detail.scoped.terrain"),
        ({"scoped": {"roads": []}}, "detail.scoped must be a table"),
        ({"scoped": {"terrain": [], "imagery": []}}, "no source"),
        ({"extra": 1}, "detail must be a table"),
    ],
)
def test_detail_errors(tmp_path, change, msg):
    s = detail_scene(tmp_path)
    s["detail"].update(change)
    with pytest.raises(ConfigError, match=msg):
        parse_scene(s)


def test_sim_box_with_detail_is_an_error(tmp_path):
    s = detail_scene(tmp_path)
    s["profile"] = "sim"
    with pytest.raises(ConfigError, match="nothing to tier"):
        parse_scene(s)


def test_zoom_detail_without_detail_is_an_error(tmp_path):
    s = synthetic_scene(tmp_path / "src")
    s["zoom"]["detail"] = {"imagery": 15}
    with pytest.raises(ConfigError, match="zoom.detail"):
        parse_scene(s)


def test_file_is_relative_to_the_toml(tmp_path):
    geojson(tmp_path, fname="areas.geojson")
    (tmp_path / "s.toml").write_text(
        'name = "x"\nbbox = [10.0, 10.0, 10.5, 10.5]\nprofile = "preview"\n[detail]\nfile = "areas.geojson"\n'
    )
    assert load_scene(tmp_path / "s.toml").detail["polygons"][0]["name"] == "a"


def test_manifest_detail_section_and_absence(tmp_path):
    m = plan_scene(parse_scene(detail_scene(tmp_path)), tmp_path / "pkg", http=FakeHttp({}), out=io.StringIO())
    assert m.detail["profile"] == "sim" and m.detail["polygons"][0]["name"] == "a" and len(m.detail["sha256"]) == 64
    assert Manifest.load(tmp_path / "pkg/manifest.json").detail == m.detail
    m0 = plan_scene(parse_scene(synthetic_scene(tmp_path / "src")), tmp_path / "p0", http=FakeHttp({}), out=io.StringIO())
    assert m0.detail is None and "detail" not in json.loads((tmp_path / "p0/manifest.json").read_text())


def test_changed_polygons_need_replan(tmp_path):
    s = detail_scene(tmp_path)
    plan_scene(parse_scene(s), tmp_path / "pkg", http=FakeHttp({}), out=io.StringIO())
    geojson(tmp_path, coords=[[x + 0.05, y] for x, y in SQUARE])
    m = Manifest.load(tmp_path / "pkg/manifest.json")
    assert "regions" in plan_differences(parse_scene(s), m)
```

- [ ] **Step 2: Run them to see them fail**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests/test_detail_config.py -q`
Expected: FAIL (`unknown key(s) in scene: ['detail']`).

- [ ] **Step 3: Implement in `config.py`**

1. Add `"detail"` to `KEYS`, and below `ROOF_MAX_PX` add:
```python
DETAIL_KEYS = ("file", "profile", "scoped")
DETAIL_PROFILES = ("sim",)
```
2. Add the field to `ScenePlan` (after `vegetation`):
```python
    detail: dict = field(default_factory=dict)  # REALISM R5: {} or {"profile", "polygons", "scoped"}
```
3. Replace `ScenePlan.regions`:
```python
    def regions(self) -> list[Region]:
        margin_zoom = {"terrain": self.zoom["ring"]["terrain"], "imagery": self.zoom["bbox"]["imagery"]}
        out = [
            Region.from_bounds("globe", GLOBE, self.zoom["globe"]),
            Region.from_bounds("ring", self.ring_bounds(), self.zoom["ring"]),
            Region.from_bounds("margin", self.area("margin"), margin_zoom),
            Region.from_bounds("bbox", self.bbox, self.zoom["bbox"]),
        ]
        if self.detail:
            from . import detail as D

            dz = self.zoom["detail"]
            mz = {"terrain": self.zoom["ring"]["terrain"], "imagery": dz["imagery"]}
            out += D.regions(self.detail["polygons"], dz, mz, self.imagery_margin_km)
        return out
```
4. Add the two helpers above `parse_scene`:
```python
def _parse_detail(data: dict, base: Path | None, bbox: Bounds, profile: str) -> dict:
    """[detail] (REALISM R5) -> {"profile", "polygons", "scoped_given"?}; {} when absent."""
    from . import detail as D

    if "detail" not in data:
        return {}
    d = data["detail"]
    if not isinstance(d, dict) or set(d) - set(DETAIL_KEYS):
        raise ConfigError(f"detail must be a table with keys from {DETAIL_KEYS}")
    dp = d.get("profile", "sim")
    if dp not in DETAIL_PROFILES:
        raise ConfigError(f"detail.profile must be one of {DETAIL_PROFILES}, got {dp!r}")
    if profile == dp:
        raise ConfigError(f'[detail] needs a coarser box profile ("preview"): profile is already "{dp}", nothing to tier')
    f = d.get("file")
    if not isinstance(f, str) or not f:
        raise ConfigError("detail.file must name a GeoJSON file")
    path = Path(f) if base is None or Path(f).is_absolute() else Path(base) / f
    out = {"profile": dp, "polygons": D.load_polygons(path, bbox)}
    if "scoped" in d:
        out["scoped_given"] = d["scoped"]
    return out


def _detail_scoped(given, prio: dict, box_profile: str, detail_profile: str) -> dict:
    """Per tiled layer, the sources limited to the polygons: the given lists, else the sources in the detail
    profile's priorities and not in the box profile's."""
    if given is None:
        out = {
            layer: [
                s
                for s in prio[layer]
                if s in PROFILES[detail_profile]["priorities"][layer] and s not in PROFILES[box_profile]["priorities"][layer]
            ]
            for layer in TILED_LAYERS
        }
    else:
        if not isinstance(given, dict) or set(given) - set(TILED_LAYERS):
            raise ConfigError(f"detail.scoped must be a table with keys from {TILED_LAYERS}")
        out = {}
        for layer in TILED_LAYERS:
            ids = given.get(layer, [])
            if not isinstance(ids, list) or not all(isinstance(i, str) and i in prio[layer] for i in ids):
                raise ConfigError(f"detail.scoped.{layer} must list source ids from priorities.{layer}")
            out[layer] = list(ids)
    if not any(out.values()):
        raise ConfigError("detail.scoped: no source is limited to the polygons (nothing to tier)")
    return out
```
5. In `parse_scene`: change the signature to `def parse_scene(data: dict, base: Path | None = None) -> ScenePlan:`. Right after the `profile` check insert:
```python
    detail = _parse_detail(data, base, bbox, profile)
    prio_profile = detail.get("profile", profile)  # with [detail], defaults come from the detail profile
```
Change `ndvi = data.get("ndvi", PROFILES[profile]["ndvi"])` to `ndvi = data.get("ndvi", PROFILES[prio_profile]["ndvi"])` and `prio = {k: list(v) for k, v in PROFILES[profile]["priorities"].items()}` to `prio = {k: list(v) for k, v in PROFILES[prio_profile]["priorities"].items()}`. After the `zoom = {...}` literal and before the override loop insert:
```python
    if detail:
        zoom["detail"] = dict(PROFILES[detail["profile"]]["bbox_zoom"])
```
After `prio["buildings"] = ...` insert:
```python
    if detail:
        detail["scoped"] = _detail_scoped(detail.pop("scoped_given", None), prio, profile, detail["profile"])
```
Pass `detail=detail,` as the last keyword argument of `ScenePlan(...)`.
6. `load_scene`: `return parse_scene(data, base=Path(path).parent)`.
7. In `layer_settings`, before `t = plan.priorities["terrain"]`:
```python
    if plan.detail:  # REALISM R5: present only with [detail], so other manifests keep their exact form
        from .detail import DETAIL, MARGIN

        out["terrain"]["scoped"] = {s: DETAIL for s in plan.detail["scoped"]["terrain"]}
        out["imagery"]["scoped"] = {s: MARGIN for s in plan.detail["scoped"]["imagery"]}
```
The zoom-override error for `[zoom.detail]` without `[detail]` already comes from `zoom.{region}: unknown region`; the test matches `zoom.detail`.

- [ ] **Step 4: Implement in `manifest.py`**

After `sea_level: dict | None = None` add:
```python
    detail: dict | None = None  # REALISM R5 detail polygons (detail.section); absent in packages without [detail]
```
and in `to_dict`, after the `sea_level` deletion:
```python
        if d["detail"] is None:
            del d["detail"]
```

- [ ] **Step 5: Implement in `pipeline.py` `plan_scene`**

In the `Manifest(...)` call add `detail=_detail_section(plan),` and add above `plan_scene`:
```python
def _detail_section(plan: ScenePlan) -> dict | None:
    if not plan.detail:
        return None
    from . import detail as D

    return D.section(plan.detail["polygons"], plan.detail["profile"])
```

- [ ] **Step 6: Run the tests**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests/test_detail_config.py scripts/scene/tests/test_detail_identity.py scripts/scene/tests/test_config.py scripts/scene/tests/test_manifest.py -q`
Expected: all pass (gate 1 included).

- [ ] **Step 7: Commit**

```bash
git add scripts/scene/camsim_scene/config.py scripts/scene/camsim_scene/manifest.py scripts/scene/camsim_scene/pipeline.py scripts/scene/tests/test_detail_config.py
git commit -m "feat(scene): [detail] polygons in scene.toml, regions and manifest (REALISM R5)"
```

---

### Task 4: Scoped discovery, clipped footprints and coverage

**Files:**
- Modify: `scripts/scene/camsim_scene/pipeline.py` (`plan_scene` discovery loop, `_fetch`)
- Modify: `scripts/scene/camsim_scene/context.py` (`coverage`)
- Test: `scripts/scene/tests/test_detail_build.py` (first part)

**Interfaces:**
- Consumes: `detail.scoped_map`, `detail.scope_geometry`, `detail.discover_in`, `detail.scopes`, `detail.clip` (Task 2); `layer_settings`, `ScenePlan.regions` (Task 3).
- Produces: after `fetch`, every data asset of a scoped source has `metadata["footprint"]` inside its scope (or `None` when the clip is empty); `coverage(m, layer, use_bbox=True)` clips discovery bboxes of scoped sources.

- [ ] **Step 1: Write the failing tests**

`scripts/scene/tests/test_detail_build.py`:
```python
"""REALISM R5 end to end on the synthetic scene: a preview box (zoom 8) with one detail polygon (zoom 11)."""

from __future__ import annotations

import io
import json

import numpy as np
import pytest
import shapely
from fake_sources import synthetic_scene
from fakes import FakeHttp
from PIL import Image

from camsim_scene import detail as D
from camsim_scene import qmesh
from camsim_scene.cache import Cache
from camsim_scene.config import parse_scene
from camsim_scene.context import coverage
from camsim_scene.manifest import Manifest
from camsim_scene.pipeline import build_scene, fetch_scene, plan_scene
from camsim_scene.tiling import plan_tiles, tile_bounds

# z9 tiles are 0.3515625 deg: x540 = [9.84375, 10.1953125], x541 = [10.1953125, 10.546875]; y likewise (from 10.0
# lat: y284 = [9.84375, 10.1953125]). The polygon + 3 km (0.027 deg) stays west/south of 10.1953125.
POLY = [[10.0, 10.0], [10.15, 10.0], [10.15, 10.15], [10.0, 10.15], [10.0, 10.0]]
OUTSIDE_Z9 = [(541, 284), (540, 285), (541, 285)]


def detail_scene(tmp_path) -> dict:
    s = synthetic_scene(tmp_path / "src")
    p = tmp_path / "detail.geojson"
    feat = {"type": "Feature", "properties": {"name": "p"}, "geometry": {"type": "Polygon", "coordinates": [POLY]}}
    p.write_text(json.dumps({"type": "FeatureCollection", "features": [feat]}))
    s["profile"] = "preview"
    s["detail"] = {"file": str(p), "scoped": {"terrain": ["hi_dem"], "imagery": ["hi_rgb"]}}
    s["zoom"]["bbox"] = {"terrain": 8, "imagery": 8}
    s["zoom"]["detail"] = {"terrain": 11, "imagery": 11}
    s["sources"]["hi_dem"]["max_zoom"] = 12
    s["sources"]["hi_rgb"]["max_zoom"] = 12
    return s


def planned(tmp_path):
    pkg = tmp_path / "pkg"
    plan_scene(parse_scene(detail_scene(tmp_path)), pkg, http=FakeHttp({}), out=io.StringIO())
    return pkg


def test_fetch_clips_scoped_footprints(tmp_path):
    pkg = planned(tmp_path)
    m = fetch_scene(pkg, Cache(tmp_path / "cache"))
    sc = D.scopes(m)
    for sid in ("hi_dem", "hi_rgb"):
        fp = shapely.from_wkt(m.source(sid).assets[0].metadata["footprint"])
        assert sc[sid].buffer(1e-6).covers(fp)
    assert shapely.from_wkt(m.source("hi_dem").assets[0].metadata["footprint"]).bounds[2] == pytest.approx(10.15)


def test_coverage_before_fetch_is_clipped(tmp_path):
    m = Manifest.load(planned(tmp_path) / "manifest.json")
    cov = coverage(m, "terrain", use_bbox=True)
    hi = [g for g, lim in zip(cov.geoms, cov.limits) if lim == 12]
    assert len(hi) == 1 and hi[0].bounds == pytest.approx((10.0, 10.0, 10.15, 10.15))


def test_deep_tiles_only_inside_the_scopes(tmp_path):
    m = fetch_scene(planned(tmp_path), Cache(tmp_path / "cache"))
    detail_g = D.scope_geometry(m.region_objs(), D.DETAIL)
    margin_g = D.scope_geometry(m.region_objs(), D.MARGIN)
    for layer, scope in (("terrain", detail_g), ("imagery", margin_g)):
        p = plan_tiles(m.region_objs(), layer, coverage(m, layer))
        assert p.max_zoom == 11
        for z in range(9, 12):
            for k in p.tiles.get(z, []):
                x, y = int(k) >> 32, int(k) & 0xFFFFFFFF
                parent_box = shapely.box(*tile_bounds(z - 1, x // 2, y // 2))
                assert parent_box.intersects(scope), (layer, z, x, y)
```

- [ ] **Step 2: Run them to see them fail**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests/test_detail_build.py -q`
Expected: FAIL (footprints not clipped: bounds end at 10.6).

- [ ] **Step 3: Implement discovery in `pipeline.plan_scene`**

Add `from . import detail as D` to the imports. Before `for sid, (adapter, src) in built.items():` insert:
```python
    regions = plan.regions()
    scoped = D.scoped_map(layer_settings(plan))
    scope_geoms = {k: D.scope_geometry(regions, k) for k in set(scoped.values())}
```
Replace `assets = src.discover(Area(plan.area(src.area_kind)))` and its warning with:
```python
        if sid in scoped:  # REALISM R5: a scoped source is discovered per detail polygon only
            assets = D.discover_in(src, scope_geoms[scoped[sid]])
            if not assets:
                log.warning("%s: no assets in the %s polygons", sid, scoped[sid])
        else:
            assets = src.discover(Area(plan.area(src.area_kind)))
            if not assets:
                log.warning("%s: no assets in %s", sid, plan.area(src.area_kind))
```
and use `regions=[r.to_dict() for r in regions],` in the `Manifest(...)` call.

- [ ] **Step 4: Implement the clip in `pipeline._fetch`**

After `m = Manifest.load(pkg / "manifest.json")` add `scope_of = D.scopes(m)`. Replace the two footprint lines with:
```python
                fp = footprint_lonlat(src.open(path, to_asset(a)))
                if fp is not None and rec.id in scope_of:  # REALISM R5: a scoped asset ends at its scope
                    fp = D.clip(fp, scope_of[rec.id])
                    if fp is None:
                        log.warning("%s/%s lies outside its detail scope; it is ignored", rec.id, a.id)
                a.metadata["footprint"] = None if fp is None else shapely.to_wkt(fp, rounding_precision=7)
                if fp is None:
                    log.warning("%s/%s holds no data; it is ignored", rec.id, a.id)
```

- [ ] **Step 5: Implement the clip in `context.coverage`**

```python
def coverage(m: Manifest, layer: str, use_bbox: bool = False) -> Coverage:
    """Footprints of a layer's data assets, each with its source's zoom limit (discovery bboxes before fetch, clipped
    to their detail scope for scoped sources; fetch stores footprints already clipped)."""
    from . import detail as D

    scope_of = D.scopes(m) if use_bbox else {}
    cov = Coverage()
    for sid in m.layers[layer]["priorities"]:
        rec = m.source(sid)
        src = make_source(rec.id, rec.adapter, rec.options)
        if src.global_coverage:
            cov.add(shapely.box(*GLOBE), src.max_zoom)
            continue
        for a in rec.assets:
            geom = _footprint(a, use_bbox) if a.role == "data" else None
            if geom is not None and sid in scope_of:
                geom = D.clip(geom, scope_of[sid])
            if geom is not None:
                cov.add(geom, src.max_zoom)
    return cov
```

- [ ] **Step 6: Run the tests**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests/test_detail_build.py scripts/scene/tests/test_pipeline.py scripts/scene/tests/test_context.py -q`
Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add scripts/scene/camsim_scene/pipeline.py scripts/scene/camsim_scene/context.py scripts/scene/tests/test_detail_build.py
git commit -m "feat(scene): scoped sources discovered per polygon, footprints clipped (REALISM R5)"
```

---

### Task 5: Mask scoped samples in every tile job

**Files:**
- Modify: `scripts/scene/camsim_scene/context.py` (`Entry`, `WorkerState.__init__`, `WorkerState._index`)
- Modify: `scripts/scene/camsim_scene/layers/imagery.py` (`sample_entries`)
- Modify: `scripts/scene/camsim_scene/layers/terrain.py` (the group loop at ~line 141)
- Test: `scripts/scene/tests/test_detail_build.py` (append)

**Interfaces:**
- Consumes: `detail.scopes`, `detail.inside` (Task 2).
- Produces: `Entry.clip` (prepared scope geometry or `None`); every sample of a scoped entry outside its scope is invalid. Every consumer that samples entries (leaves, feather, NDVI, colour-balance fit, roof atlases) goes through `imagery.sample_entries` or the terrain loop, so this covers them.

- [ ] **Step 1: Write the failing tests** (append to `test_detail_build.py`)

```python
def built(tmp_path):
    pkg = planned(tmp_path)
    build_scene(pkg, Cache(tmp_path / "cache"), jobs=1)
    return pkg


def test_no_scoped_pixel_outside_the_scope(tmp_path):
    pkg = built(tmp_path)
    for x, y in OUTSIDE_Z9:
        img = np.asarray(Image.open(pkg / f"imagery/9/{x}/{y}.jpg").convert("RGB"))
        assert img[..., 0].max() < 120, (x, y)  # hi_rgb red is 200/250; the global base is 40
        h = qmesh.decode((pkg / f"terrain/9/{x}/{y}.terrain").read_bytes()).heights()
        assert h.max() < 150, (x, y)  # hi_dem is ~185-225 m; the base is ~108 m here
    assert not (pkg / "imagery/10/1082/568.jpg").exists()  # inside x541/y284: never expanded


def test_inside_the_polygon_uses_the_scoped_sources(tmp_path):
    pkg = built(tmp_path)
    # z11 tile x=2162, y=1138: lon [10.0195, 10.1074] x lat [10.0195, 10.1074] -> inside the polygon
    img = np.asarray(Image.open(pkg / "imagery/11/2162/1138.jpg").convert("RGB"))
    assert np.median(img[..., 0]) > 180


def test_rebuild_is_identical(tmp_path):
    a = built(tmp_path)
    before = (a / "hashes.txt").read_bytes()
    build_scene(a, Cache(tmp_path / "cache"), jobs=2)
    assert (a / "hashes.txt").read_bytes() == before
```
Before running, confirm the z11 tile: `uv run --project scripts/scene python -c "from camsim_scene.tiling import tile_bounds; print(tile_bounds(11, 2162, 1138))"` prints about `(10.0195, 10.0195, 10.1074, 10.1074)`.

- [ ] **Step 2: Run them to see them fail**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests/test_detail_build.py -q -k "outside or inside_the_polygon or rebuild"`
Expected: `test_no_scoped_pixel_outside_the_scope` FAILS (red ~200 in x541 tiles: the NAIP-like source still samples past its scope inside the coarse leaves).

- [ ] **Step 3: Implement `Entry.clip`**

In `context.py`, add the field last in `Entry`:
```python
    clip: object = None  # REALISM R5: the source's detail scope (prepared); samples outside it are no data
```
In `WorkerState.__init__`, before `self.index = {...}`:
```python
        from . import detail as D

        self.scopes = D.scopes(self.manifest)
        for g in self.scopes.values():
            shapely.prepare(g)
```
In `_index`, change the `Entry(...)` construction to pass the clip:
```python
                entries.append(
                    Entry(
                        qid,
                        a.sha256,
                        order,
                        src.max_zoom,
                        raster,
                        self._transform(raster.datum, raster.vertical_asset),
                        self.scopes.get(sid),
                    )
                )
```

- [ ] **Step 4: Mask in `imagery.sample_entries`**

Add `from .. import detail as D` to `layers/imagery.py` imports. In `sample_entries`, after `v, good = e.raster.sample(px, py, target_m)`:
```python
        clip = getattr(e, "clip", None)
        if clip is not None:  # REALISM R5: a scoped source never supplies a sample outside its scope
            good = good & D.inside(clip, lon, lat)
```

- [ ] **Step 5: Mask in the terrain loop**

Add `from .. import detail as D` to `layers/terrain.py` imports. After `vals, ok = e.raster.sample(px, py, target_m)`:
```python
        clip = getattr(e, "clip", None)
        if clip is not None:  # REALISM R5: a scoped source never supplies a height outside its scope
            ok = ok & D.inside(clip, lon, lat)
```
If `layers/terrain.py` or `layers/imagery.py` cannot import `detail` because of an import cycle (`detail` imports `config` → … → `layers`), move the import inside the function body.

- [ ] **Step 6: Run the tests**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests/test_detail_build.py scripts/scene/tests/test_imagery.py scripts/scene/tests/test_terrain.py scripts/scene/tests/test_context.py -q`
Expected: all pass.

- [ ] **Step 7: Run the whole scene suite** (the mask must not change any existing package)

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests -n auto -q`
Expected: everything passes (594 + the new tests), 3 skipped, 1 xfailed.

- [ ] **Step 8: Commit**

```bash
git add scripts/scene/camsim_scene/context.py scripts/scene/camsim_scene/layers/imagery.py scripts/scene/camsim_scene/layers/terrain.py scripts/scene/tests/test_detail_build.py
git commit -m "feat(scene): scoped samples masked to their detail scope in every tile job (REALISM R5)"
```

---

### Task 6: `verify` checks the detail section

**Files:**
- Modify: `scripts/scene/camsim_scene/verify.py` (new `_check_detail`, registered in `verify`)
- Test: `scripts/scene/tests/test_detail_build.py` (append)

**Interfaces:**
- Consumes: `detail.polygons_sha256`, `detail.scopes` (Task 2); `Manifest.detail` (Task 3).
- Produces: a `detail` check in the verify report (only for packages with `[detail]`).

- [ ] **Step 1: Write the failing tests** (append)

```python
def test_verify_detail_ok_and_tampered(tmp_path):
    from camsim_scene.verify import verify

    pkg = built(tmp_path)
    rep = verify(pkg)
    assert {c["name"]: c["ok"] for c in rep["checks"]}["detail"] is True
    m = Manifest.load(pkg / "manifest.json")
    m.detail["polygons"][0]["wkt"] = "POLYGON ((10 10, 10.2 10, 10.2 10.2, 10 10.2, 10 10))"
    m.source("hi_rgb").assets[0].metadata["footprint"] = "POLYGON ((10 10, 10.6 10, 10.6 10.6, 10 10.6, 10 10))"
    m.write(pkg / "manifest.json")
    det = next(c for c in verify(pkg)["checks"] if c["name"] == "detail")
    assert not det["ok"] and "sha256" in det["detail"] and "hi_rgb" in det["detail"]


def test_verify_has_no_detail_check_without_detail(tmp_path):
    from fake_sources import build_synthetic

    from camsim_scene.verify import verify

    pkg, _, _ = build_synthetic(tmp_path)
    assert "detail" not in {c["name"] for c in verify(pkg)["checks"]}
```

- [ ] **Step 2: Run them to see them fail**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests/test_detail_build.py -q -k verify`
Expected: FAIL (`StopIteration` / KeyError `detail`).

- [ ] **Step 3: Implement**

In `verify.py` add (near the other `_check_*`):
```python
def _check_detail(m: Manifest) -> Check:
    """REALISM R5: the detail section's hash matches its polygons, and every scoped source's footprints lie inside
    their scope."""
    from . import detail as D

    problems = []
    if D.polygons_sha256(m.detail["polygons"]) != m.detail["sha256"]:
        problems.append("detail.sha256 does not match its polygons")
    for sid, scope in D.scopes(m).items():
        tol = scope.buffer(1e-6)
        for a in m.source(sid).assets:
            wkt = a.metadata.get("footprint")
            if a.role == "data" and wkt and not tol.covers(shapely.from_wkt(wkt)):
                problems.append(f"{sid}/{a.id}: footprint outside its detail scope")
    return Check("detail", not problems, _summary(problems))
```
Add `import shapely` at the top of `verify.py` if it is not already imported. In `verify`, after the `attribution` guard:
```python
    if m.detail:
        checks.append(_guard("detail", _check_detail, m))
```

- [ ] **Step 4: Run the tests**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests/test_detail_build.py scripts/scene/tests/test_verify.py -q`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add scripts/scene/camsim_scene/verify.py scripts/scene/tests/test_detail_build.py
git commit -m "feat(scene): verify checks the detail polygons and scoped footprints (REALISM R5)"
```

---

### Task 7: Gate tools — `detail_check.py`, `edge_shots.py --crop`, `render_check.py --shots detail`

**Files:**
- Create: `scripts/scene/tools/detail_check.py`
- Modify: `scripts/scene/tools/edge_shots.py`, `scripts/scene/tools/render_check.py`
- Test: `scripts/scene/tests/test_detail_check.py`

**Interfaces:**
- Consumes: `detail.scope_geometry`, `detail.DETAIL`, `detail.MARGIN`; `manifest.read_hashes`; `tiling.tile_bounds`; `layers.landcover.tile_range`, `tile_filename`.
- Produces: `detail_check.py PKG --reference REF [--shrink-km 1] [--coast-km 1]` → JSON report, exit 1 on a failed gate. Pure functions: `parse_tile(path) -> (layer, z, x, y) | None`, `scope_violations(hashes, regions) -> list[str]`, `identity(hashes, ref, layer, z, inner) -> (compared, mismatched)`, `water_near(lc_dir, bounds) -> bool`.

- [ ] **Step 1: Write the failing tests**

`scripts/scene/tests/test_detail_check.py`:
```python
"""tools/detail_check.py (REALISM R5 gates 2 and 3)."""

from __future__ import annotations

import importlib.util
from pathlib import Path

import numpy as np
import shapely
from PIL import Image

from camsim_scene.layers.landcover import tile_filename
from camsim_scene.tiling import Region

_SPEC = importlib.util.spec_from_file_location(
    "detail_check", Path(__file__).resolve().parents[1] / "tools" / "detail_check.py"
)
dc = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(dc)

REGIONS = [
    Region.from_bounds("bbox", (10.0, 10.0, 10.5, 10.5), {"terrain": 8, "imagery": 8}),
    Region.from_bounds("detail:p", (10.0, 10.0, 10.15, 10.15), {"terrain": 11, "imagery": 11}),
    Region.from_bounds("detail_margin:p", (9.97, 9.97, 10.18, 10.18), {"terrain": 3, "imagery": 11}),
]


def test_parse_tile():
    assert dc.parse_tile("imagery/9/541/284.jpg") == ("imagery", 9, 541, 284)
    assert dc.parse_tile("terrain/11/2161/1137.terrain") == ("terrain", 11, 2161, 1137)
    assert dc.parse_tile("landcover/+10.00_+10.00.png") is None


def test_scope_violations():
    ok = {"imagery/9/540/284.jpg": "a", "terrain/9/540/284.terrain": "b", "imagery/8/270/142.jpg": "c"}
    assert dc.scope_violations(ok, REGIONS) == []
    sibling = {"imagery/9/541/285.jpg": "a"}  # outside the margin, but its z8 parent was expanded: a legal sibling
    assert dc.scope_violations(sibling, REGIONS) == []
    bad = {"imagery/10/1083/571.jpg": "a"}  # its z9 parent (541, 285) misses the margin: must not exist
    assert dc.scope_violations(bad, REGIONS) == ["imagery/10/1083/571.jpg"]


def test_identity_counts_only_tiles_inside():
    inner = shapely.box(10.0, 10.0, 10.2, 10.2)  # holds z11 x 2162-2163, y 1138 (0.0879-degree tiles)
    a = {"imagery/11/2162/1138.jpg": "x", "imagery/11/2163/1138.jpg": "y", "imagery/11/2170/1138.jpg": "z"}
    ref = {"imagery/11/2162/1138.jpg": "x", "imagery/11/2163/1138.jpg": "DIFF", "imagery/11/2170/1138.jpg": "q"}
    compared, mism = dc.identity(a, ref, "imagery", 11, inner)
    assert compared == 2 and mism == ["imagery/11/2163/1138.jpg"]


def test_water_near(tmp_path):
    lc = tmp_path / "landcover"
    lc.mkdir()
    codes = np.full((600, 600), 10, np.uint8)
    codes[:10, :10] = 80  # north-west corner of tile (200, 200) = lat [10.0, 10.05), lon [10.0, 10.05)
    Image.fromarray(codes).save(lc / tile_filename(200, 200))
    assert dc.water_near(lc, (10.0, 10.04, 10.002, 10.0499))
    assert not dc.water_near(lc, (10.03, 10.0, 10.04, 10.01))
    assert not dc.water_near(lc, (20.0, 20.0, 20.01, 20.01))  # no tile: no data, not water
```

- [ ] **Step 2: Run them to see them fail**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests/test_detail_check.py -q`
Expected: FAIL (`FileNotFoundError` for `tools/detail_check.py`).

- [ ] **Step 3: Implement `tools/detail_check.py`**

```python
"""REALISM R5 gates 2 and 3 (docs/superpowers/specs/2026-10-14-r5-detail-polygons-design.md, section 4).

    uv run --project scripts/scene python scripts/scene/tools/detail_check.py PKG --reference REF \
        [--shrink-km 1] [--coast-km 1]

Gate 3 (scope): no terrain tile deeper than the bbox's terrain zoom outside the detail polygons, no imagery tile
deeper than the bbox's imagery zoom outside the detail margin (a tile counts when its parent overlaps the scope,
because siblings are always built), and no scoped footprint outside its scope (verify's `detail` check).
Gate 2 (identity): leaves at the detail zooms whose tile lies inside the detail polygons shrunk by --shrink-km have
the same sha256 as REF's (a full-sim package of the same area, e.g. pendleton-r5veg). Imagery leaves with WorldCover
water (80 or 220 in PKG's land cover) within --coast-km are listed but exempt (colour-matched Sentinel-2 at the
shore depends on the balance fit's samples). Prints a JSON report; exits 1 when a gate fails."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

import numpy as np
import shapely
from PIL import Image

from camsim_scene import detail as D
from camsim_scene.config import KM_PER_DEG
from camsim_scene.layers.landcover import tile_filename, tile_range
from camsim_scene.manifest import Manifest, read_hashes
from camsim_scene.tiling import tile_bounds

TILE_RE = re.compile(r"^(terrain|imagery)/(\d+)/(\d+)/(\d+)\.(terrain|jpg)$")
WATER_CODES = (80, 220)
LC_TILE_DEG, LC_PX = 0.05, 600


def parse_tile(path: str):
    mo = TILE_RE.match(path)
    return None if mo is None else (mo.group(1), int(mo.group(2)), int(mo.group(3)), int(mo.group(4)))


def _box_zoom(regions, layer: str) -> int:
    return next(r for r in regions if r.name == "bbox").max_zoom[layer]


def scope_violations(hashes: dict, regions) -> list[str]:
    scope = {"terrain": D.scope_geometry(regions, D.DETAIL), "imagery": D.scope_geometry(regions, D.MARGIN)}
    out = []
    for path in sorted(hashes):
        t = parse_tile(path)
        if t is None:
            continue
        layer, z, x, y = t
        if z <= _box_zoom(regions, layer):
            continue
        parent = shapely.box(*tile_bounds(z - 1, x // 2, y // 2))
        if scope[layer] is None or not parent.intersects(scope[layer]):
            out.append(path)
    return out


def identity(hashes: dict, ref: dict, layer: str, z: int, inner) -> tuple[int, list[str]]:
    compared, mism = 0, []
    for path, h in sorted(hashes.items()):
        t = parse_tile(path)
        if t is None or t[0] != layer or t[1] != z:
            continue
        if not inner.contains(shapely.box(*tile_bounds(z, t[2], t[3]))):
            continue
        compared += 1
        if ref.get(path) != h:
            mism.append(path)
    return compared, mism


def water_near(lc_dir: Path, bounds) -> bool:
    """True when any land-cover cell in bounds (W S E N) is water (80 or 220); missing tiles are no data."""
    w, s, e, n = bounds
    for i in tile_range(s, n):
        for j in tile_range(w, e):
            p = Path(lc_dir) / tile_filename(i, j)
            if not p.exists():
                continue
            codes = np.asarray(Image.open(p))
            s0, w0 = i * LC_TILE_DEG, j * LC_TILE_DEG
            c0 = max(0, int(np.floor((w - w0) / LC_TILE_DEG * LC_PX)))
            c1 = min(LC_PX, int(np.ceil((e - w0) / LC_TILE_DEG * LC_PX)))
            r0 = max(0, int(np.floor((s0 + LC_TILE_DEG - n) / LC_TILE_DEG * LC_PX)))  # row 0 is the north edge
            r1 = min(LC_PX, int(np.ceil((s0 + LC_TILE_DEG - s) / LC_TILE_DEG * LC_PX)))
            if c1 > c0 and r1 > r0 and np.isin(codes[r0:r1, c0:c1], WATER_CODES).any():
                return True
    return False


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pkg", type=Path)
    ap.add_argument("--reference", type=Path, required=True)
    ap.add_argument("--shrink-km", type=float, default=1.0)
    ap.add_argument("--coast-km", type=float, default=1.0)
    a = ap.parse_args()
    m = Manifest.load(a.pkg / "manifest.json")
    if not m.detail:
        print(f"{a.pkg}: no [detail] in the manifest", file=sys.stderr)
        return 2
    regions = m.region_objs()
    hashes, ref = read_hashes(a.pkg), read_hashes(a.reference)
    rep: dict = {"package": str(a.pkg), "reference": str(a.reference)}
    viol = scope_violations(hashes, regions)
    fp_bad = [
        f"{sid}/{x.id}"
        for sid, scope in D.scopes(m).items()
        for x in m.source(sid).assets
        if x.role == "data" and x.metadata.get("footprint")
        and not scope.buffer(1e-6).covers(shapely.from_wkt(x.metadata["footprint"]))
    ]
    rep["scope"] = {"tiles_outside": len(viol), "examples": viol[:20], "footprints_outside": fp_bad}
    inner = D.scope_geometry(regions, D.DETAIL).buffer(-a.shrink_km / KM_PER_DEG)
    pad = a.coast_km / KM_PER_DEG
    rep["identity"] = {}
    ok = not viol and not fp_bad
    for layer in ("terrain", "imagery"):
        z = max(r.max_zoom[layer] for r in regions if r.name.startswith(D.DETAIL + ":"))
        compared, mism = identity(hashes, ref, layer, z, inner)
        coastal = []
        if layer == "imagery":
            for p in list(mism):
                _, zz, x, y = parse_tile(p)
                w, s, e, n = tile_bounds(zz, x, y)
                if water_near(a.pkg / "landcover", (w - pad, s - pad, e + pad, n + pad)):
                    coastal.append(p)
            mism = [p for p in mism if p not in coastal]
        rep["identity"][layer] = {
            "zoom": z,
            "compared": compared,
            "mismatched": len(mism),
            "mismatched_examples": mism[:20],
            "coastal_exempt": len(coastal),
            "coastal_examples": coastal[:20],
        }
        ok = ok and compared > 0 and not mism
    rep["ok"] = ok
    print(json.dumps(rep, indent=2))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 4: Run the tests**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests/test_detail_check.py -q`
Expected: all pass.

- [ ] **Step 5: `edge_shots.py --crop`**

Add after `ap.add_argument("--before", type=Path)`:
```python
    ap.add_argument(
        "--crop",
        action="append",
        default=[],
        metavar="NAME:Z:W:S:E:N",
        help="crop instead of the built-in Pendleton ones (repeatable), e.g. pendleton_ne:14:-117.30:33.46:-117.18:33.56",
    )
```
and replace `for name, (z, *box) in CROPS.items():` with:
```python
    crops = CROPS
    if a.crop:
        crops = {}
        for c in a.crop:
            name, z, *box = c.split(":")
            crops[name] = (int(z), *(float(v) for v in box))
    for name, (z, *box) in crops.items():
```
Check: `uv run --project scripts/scene python scripts/scene/tools/edge_shots.py --help` lists `--crop`.

- [ ] **Step 6: `render_check.py --shots detail`**

Below `SHOTS = {...}` add:
```python
DETAIL_SHOTS = {  # REALISM R5 on socal-r5 (examples/socal-r5.toml): the pendleton polygon's edges and the box beyond
    "edge_inland": P(lat=33.450, lon=-117.275, alt=800, yaw=90, gimbal_pitch=-15, fov_h=50),  # east edge, looking out
    "edge_coast": P(lat=33.205, lon=-117.360, alt=800, yaw=150, gimbal_pitch=-15, fov_h=50),  # south edge, shore
    "outside_high": P(lat=33.650, lon=-117.750, alt=6000, yaw=0, gimbal_pitch=-12, fov_h=60),  # Irvine, preview only
    "outside_low": P(lat=33.650, lon=-117.750, alt=500, yaw=0, gimbal_pitch=-20, fov_h=50),
}
```
Add `ap.add_argument("--shots", choices=["r0", "detail"], default="r0")` to `main()`, and where `main()` passes `SHOTS` to `fly(...)`, pass `SHOTS if a.shots == "r0" else DETAIL_SHOTS` instead (read `main()` and change only the shot dictionary; the Beaufort second session runs only for `r0`).
Check: `uv run --project scripts/scene python scripts/scene/tools/render_check.py --help` lists `--shots`.

- [ ] **Step 7: Commit**

```bash
git add scripts/scene/tools/detail_check.py scripts/scene/tools/edge_shots.py scripts/scene/tools/render_check.py scripts/scene/tests/test_detail_check.py
git commit -m "feat(scene): R5 gate tools — detail_check, edge_shots --crop, render_check --shots detail"
```

---

### Task 8: Example package files and docs

**Files:**
- Create: `scripts/scene/examples/socal-r5.toml`, `scripts/scene/examples/socal-r5-detail.geojson`
- Modify: `docs/scene-packages.md` (new section "Detail polygons (R5)" after "Vegetation mix (R4)"; `scene.toml reference`), `CLAUDE.md` (the `camsim-scene` row mentions `[detail]`)
- Test: `scripts/scene/tests/test_detail_config.py` (append)

- [ ] **Step 1: Write the failing test** (append)

```python
def test_socal_example_parses():
    from pathlib import Path

    ex = Path(__file__).resolve().parents[1] / "examples" / "socal-r5.toml"
    plan = load_scene(ex)
    assert plan.profile == "preview" and plan.detail["scoped"] == {"terrain": ["dep3_1m"], "imagery": ["naip_pc"]}
    assert [r.name for r in plan.regions()][-2:] == ["detail:pendleton", "detail_margin:pendleton"]
```

- [ ] **Step 2: Run it to see it fail**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests/test_detail_config.py -q -k socal`
Expected: FAIL (file not found).

- [ ] **Step 3: Write the example files**

`scripts/scene/examples/socal-r5-detail.geojson`:
```json
{
  "type": "FeatureCollection",
  "features": [
    {
      "type": "Feature",
      "properties": {"name": "pendleton"},
      "geometry": {
        "type": "Polygon",
        "coordinates": [[[-117.62, 33.19], [-117.24, 33.19], [-117.24, 33.52], [-117.62, 33.52], [-117.62, 33.19]]]
      }
    }
  ]
}
```
`scripts/scene/examples/socal-r5.toml`: copy `pendleton-r5veg.toml` and change the header comment, `name`, `bbox`, `profile`, and add `[detail]`:
```toml
# REALISM R5 gate package: 200 km x 200 km around Camp Pendleton at preview, the Pendleton bbox at sim (detail polygon);
# every other setting as pendleton-r5veg.toml
name = "socal-r5"
bbox = [-118.51, 32.46, -116.35, 34.25]  # W S E N
profile = "preview"
```
(keep `seed`, `ring_km`, `bmng_month`, `jpeg_quality`, `allow`, `[features]`, `[buildings]`, `[sources.naip_pc]`, `[sea_level]`, `[canopy]`, `[vegetation]` exactly as in `pendleton-r5veg.toml`) and append:
```toml
[detail]                   # REALISM R5: NAIP and 3DEP 1 m only inside these polygons
file = "socal-r5-detail.geojson"
profile = "sim"
```

- [ ] **Step 4: Run the test**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests/test_detail_config.py -q`
Expected: all pass.

- [ ] **Step 5: Docs**

In `docs/scene-packages.md`:
- In "scene.toml reference", add the `[detail]` table (keys `file`, `profile`, `scoped`) with one line each, as written in the spec section 1.
- Add a section `## Detail polygons (R5)` after "Vegetation mix (R4)" containing: what it is for (a `preview` box with `sim` polygons; NAIP and 3DEP 1 m fetched only there); the GeoJSON rules (FeatureCollection, `properties.name`, Polygon/MultiPolygon, inside the bbox, EPSG:4326); the regions table rows `detail:<name>` (sim zooms) and `detail_margin:<name>` (polygon + `imagery_margin_km`, sim imagery zoom, ring terrain zoom); the scope rule (terrain → polygon, imagery → polygon + margin; default sources `dep3_1m`, `naip_pc`; `[detail.scoped]` override); what stays whole-box (land cover, burns, buildings and mask, canopy, vegetation mix) and that roofs outside the NAIP scope are flat grey; that the GeoJSON is copied into the manifest (`detail`) and re-read only by `--replan`; `verify`'s `detail` check; and the gate tools (`tools/detail_check.py`, `edge_shots.py --crop`, `render_check.py --shots detail`).
- In `CLAUDE.md`'s command table, extend the `camsim-scene` row's description with "; `[detail]` polygons at `sim` inside a `preview` box (REALISM R5)".

- [ ] **Step 6: Commit**

```bash
git add scripts/scene/examples/socal-r5.toml scripts/scene/examples/socal-r5-detail.geojson scripts/scene/tests/test_detail_config.py docs/scene-packages.md CLAUDE.md
git commit -m "docs(scene): detail polygons (REALISM R5); socal-r5 example"
```

---

### Task 9: Build `socal-r5` and run gates 2, 3 and 5 (Linux box)

Operational; no code unless a gate finds a bug (then: a failing test reproducing it, the fix, the full suite, a commit, and a rebuild).

**Files:**
- Modify (results only): `docs/scene-packages.md` ("Measured" → new subsection "R5 detail polygons (2026-10-xx, Linux)")

- [ ] **Step 1: Plan and estimate**

```bash
cd /opt/mac/camera-simulator2
export TMPDIR=/opt/mac/tmp SP=$PWD/.cache/scene-packages
mkdir -p .cache/linux-r5
scripts/scene/build.sh plan $SP/socal-r5 --config scripts/scene/examples/socal-r5.toml 2>&1 | tee .cache/linux-r5/plan.log
```
Expected: one `plan socal-r5: …` line with asset count, cache GB and tile estimates. Record them. If `build.sh` cannot see `scripts/scene/examples/` (container mounts), copy the toml and GeoJSON into `.cache/linux-tomls/` and pass that path. If the estimate shows NAIP assets outside the polygon (`jq '.sources[] | select(.id=="naip_pc") | .assets | length' $SP/socal-r5/manifest.json` far above Pendleton's count), stop and investigate before fetching.

- [ ] **Step 2: Build** (long: background it, `set -o pipefail`)

```bash
set -o pipefail
/usr/bin/time -v scripts/scene/build.sh build $SP/socal-r5 -j 16 2>&1 | tee .cache/linux-r5/build.log
```
Expected: exit 0 (or exit 2 only for the known false RSS report; read the log). Record wall time, peak RSS (main process and workers), per-layer tile counts and sizes from `build.json`, the building count and textured / flat roof counts.

- [ ] **Step 3: Gate 3 and gate 2**

```bash
uv run --project scripts/scene python scripts/scene/tools/detail_check.py $SP/socal-r5 --reference $SP/pendleton-r5veg | tee .cache/linux-r5/detail_check.json
```
Expected: exit 0: `scope.tiles_outside` 0, `footprints_outside` [], `identity.terrain.mismatched` 0 and `identity.imagery.mismatched` 0 with `compared` > 0 for both. Record the coastal-exempt count.

- [ ] **Step 4: Gate 5**

```bash
scripts/scene/build.sh verify $SP/socal-r5 --deep 2>&1 | tee .cache/linux-r5/verify.log
cp $SP/socal-r5/hashes.txt .cache/linux-r5/hashes-1.txt
scripts/scene/build.sh build $SP/socal-r5 -j 16 2>&1 | tail -5     # rebuild: 0 built
rm -rf $SP/socal-r5-b && cp -a $SP/socal-r5 $SP/socal-r5-b && rm -rf "$SP/socal-r5-b/imagery/17"
scripts/scene/build.sh build $SP/socal-r5-b -j 16 2>&1 | tail -5   # partial rebuild resumes
cmp $SP/socal-r5/hashes.txt $SP/socal-r5-b/hashes.txt && echo identical
```
Expected: verify all ok including `detail`; the rebuild builds nothing; the resumed build rebuilds only z17 imagery and its parents and ends identical. Then the offline run in step 2 of Task 10 covers "no request".

- [ ] **Step 5: Record and commit**

Write the "R5 detail polygons" subsection under "Measured" in `docs/scene-packages.md` (date, hardware, head commit, the numbers from steps 1–4, and the full-`sim` estimate for the box = Pendleton's per-km² size × 40,000 / 1,295 for comparison).
```bash
git add docs/scene-packages.md
git commit -m "docs(scene): socal-r5 built — detail polygon gates 2, 3, 5 (REALISM R5)"
```

---

### Task 10: Gates 4, 6, 7 and the roadmap

Operational. Requires the packaged Docker image of the current CamSim (`camsim:linux-r4` has the current C++; the runtime is unchanged by R5).

- [ ] **Step 1: Gate 4 (edge seam)**

```bash
jq '.balance.heldout.after.cell_bias_median_dn' $SP/socal-r5/build.json
uv run --project scripts/scene python scripts/scene/tools/ndvi_check.py $SP/socal-r5 --cache .cache/scene | tee .cache/linux-r5/ndvi_check.json
uv run --project scripts/scene python scripts/scene/tools/water_check.py $SP/socal-r5 --cache .cache/scene | tee .cache/linux-r5/water_check.json
uv run --project scripts/scene python scripts/scene/tools/edge_shots.py .cache/linux-r5/edges $SP/socal-r5 \
  --crop ne:14:-117.30:33.46:-117.18:33.56 --crop south:14:-117.42:33.13:-117.30:33.23 --crop overview:11:-117.80:33.05:-117.05:33.65
```
Expected: cell bias median ≤ 5 DN per band; `ndvi_check` and `water_check` exit 0. Look at the three crops: the NAIP → Sentinel-2 seam at the polygon edge should look like R1's bbox edge.

- [ ] **Step 2: Gates 5 (offline) and 6 (render)**

```bash
uv run --project scripts/scene python scripts/scene/tools/render_check.py .cache/linux-r5/render package $SP/socal-r5 \
  --docker camsim:linux-r4 --offline --shots detail
uv run --project scripts/scene python scripts/scene/tools/render_check.py .cache/linux-r5/render-r0 package $SP/socal-r5 \
  --docker camsim:linux-r4 --offline
```
Expected: both exit 0 with no outbound request; every shot `terrain_ready`. Put the PNGs in front of the user for the visual review (they are the reviewer, gate 6).

- [ ] **Step 3: Gate 7 (bench)**

```bash
uv run -q --with numpy --with pillow python scripts/bench/run_bench.py --site pendleton --label socal-r5 \
  --out .cache/linux-r5/bench --gpu-mem --env CAMSIM_SCENE_DIR=$SP/socal-r5
uv run -q python scripts/bench/compare.py .cache/linux-6/<the most recent pendleton-r5veg full bench> .cache/linux-r5/bench
```
If no full `pendleton-r5veg` bench exists on the box, run the same command with `CAMSIM_SCENE_DIR=$SP/pendleton-r5veg` first (one CamSim at a time). Expected: every phase's game and GPU p50 within the run-to-run noise recorded in `docs/vegetation.md` ("Bench on Linux": ~±0.1 ms game p50).

- [ ] **Step 4: Record and close**

- `docs/scene-packages.md` "R5 detail polygons": gates 4, 6, 7.
- `REALISM.md` R5: replace the body with what was built (detail polygons; corridors, routes, cache warming and `high` deferred, user 2026-10-14), and its gate results.
- `ROADMAP.md` Realism track: an "R5 detail polygons" paragraph (date, hardware, numbers, the user's visual review as an open item until signed off).
```bash
git add docs/scene-packages.md REALISM.md ROADMAP.md
git commit -m "docs(realism): R5 detail polygons measured on Linux — gates 4, 6, 7"
```
