# Buildings Thin Slice (REALISM R3-lite) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Overture building footprints become a `features/buildings/` 3D Tiles layer in scene packages, and CamSim loads it as one extra Cesium tileset that CIGI LOS/HAT/HOT hit (material code 230) and the DIS clamp ignores.

**Architecture:** Offline, a new `overture_buildings` source snapshots Overture's buildings theme (DuckDB → GeoParquet in the fetch cache), and a new `buildings` layer extrudes footprints onto the package's bare-earth DEM, writes one GLB per geographic tile (size tiers, additive refinement, `EXT_mesh_features` + `EXT_structural_metadata`) and an explicit `tileset.json`. At runtime `ResolvePackage` finds the tileset, `SetUpCesiumWorld` spawns a tagged `ACesium3DTileset` after the destroy loop, and two consumers recognise it by its actor tag: the CIGI material code and the surface probe's ignore list.

**Tech Stack:** Python 3.12–3.13 (`uv`, numpy, shapely ≥ 2.2, DuckDB — new), pytest; UE 5.8 C++ (Cesium for Unreal 2.29), UE Automation tests; Node (CesiumGS `3d-tiles-validator`, test-only).

**Spec:** `docs/superpowers/specs/2026-10-10-r3-buildings-slice-design.md` (read it first). Bench: `docs/superpowers/specs/2026-10-10-r3r4-realism-cost-bench-design.md` (owned by a separate plan; this plan only runs it in the acceptance task).

## Global Constraints

- Altitudes are WGS-84 ellipsoid heights everywhere (`CLAUDE.md`, "Altitudes"). Building base/roof heights are ellipsoid.
- Overture release pinned: `release = "2026-09-23.1"` (config default); the snapshot (GeoParquet in the fetch cache, sha256 in the manifest) is what builds read, never S3.
- Overture licence `ODbL-1.0`; a package with `[buildings]` must fail to plan without `allow = ["ODbL-1.0"]`.
- A scene toml without `[buildings]` produces a package (and `manifest.json`) byte for byte identical to today's.
- Height: Overture `height` → `num_floors × 3.2 m` → 4.0 m; clamped to [2.5, 150] m. `height_source` ∈ {`overture_height`, `overture_floors`, `default`}.
- Base = min over footprint vertices + centroid of the bare-earth DEM − 0.5 m; roof = base + 0.5 m + height.
- Colours (linear `baseColorFactor`): roof (0.30, 0.30, 0.31, 1), walls (0.42, 0.41, 0.40, 1); metallic 0, roughness 0.9.
- Tiers: footprint area ≥ 2,000 m² → z13; ≥ 300 m² → z14; else z15 (geographic TMS, EPSG:4326, 2×1 roots, `tiling.tile_bounds`). Tile = the one containing the footprint centroid.
- Geometric error: root 200 m, z13 60 m, z14 20 m, z15 6 m (spec values; z15 nodes have no children).
- `min_area_m2` default 10.
- Material code **230** = building. Codes 200–220 unchanged.
- Runtime config keys (`scene.buildings.*`): `enabled` (`CAMSIM_SCENE_BUILDINGS_ENABLED`, default true), `maximum_screen_space_error` (`CAMSIM_SCENE_BUILDINGS_MAXIMUM_SCREEN_SPACE_ERROR`, default 16), `create_physics_meshes` (`CAMSIM_SCENE_BUILDINGS_CREATE_PHYSICS_MESHES`, default true).
- `scene.buildings.enabled: false` must be R2 bit for bit (no tileset spawned, no code path changed).
- Code style: UE naming, `// Copyright CamSim Contributors. All Rights Reserved.`, `CoreMinimal.h` first; Python ruff line length 120.
- Never stage into `Saved/StagedBuilds/` (CLAUDE.md). Never put Cesium in the project's Plugins.

## Review Focus

1. **Footprints that straddle tile edges or the bbox edge.** A building whose centroid is in tile A but whose footprint extends into tile B must appear exactly once (in A), and A's bounding region must contain all of it. Test: `test_tile_region_contains_straddling_building` (Task 5).
2. **Degenerate Overture geometry** (self-intersecting rings, duplicate closing vertices, zero-length edges, holes touching the shell, a GeometryCollection out of `make_valid`). Expect: the building is repaired or dropped with a count, never a crash or a NaN vertex. Test: `test_clean_repairs_bowtie_and_drops_lines` and `test_mesh_skips_zero_length_edges` (Task 4).
3. **Rebuild after a snapshot already exists but the release is gone.** Expect: the build uses the cached snapshot with no network. A missing snapshot for a deleted release is a clear error naming the release. Test: `test_fetch_uses_cached_snapshot_without_query` and `test_missing_release_names_it` (Task 1).
4. **Two `SetUpCesiumWorld` calls / a level that already has a tagged buildings tileset.** Expect: exactly one buildings tileset. Test: `CamSim.Scene.Buildings.SpawnIsIdempotent` (Task 8).
5. **A roof below the sea surface on HAT/HOT** (piers, coastal sheds below local MSL + waves). Expect: the sea wins (80) as today, not 230. Test: a `QueryMaterialCode` case with `bWater=true, bBuildingHit=true` → 80 (Task 9).

---

## File Structure

**Scene tooling (`scripts/scene/`):**

| File | Responsibility |
|---|---|
| `camsim_scene/geodesy.py` (new) | WGS-84 geodetic ↔ ECEF, ENU frames, 4×4 ENU→ECEF matrices |
| `camsim_scene/gltf.py` (new) | GLB writer/reader: one mesh, roof + wall primitives, feature ids, one property table |
| `camsim_scene/buildings.py` (new) | Snapshot reading, footprint cleaning, heights, per-tile meshes, tier/tile assignment, `tileset.json` |
| `camsim_scene/sources/overture_buildings.py` (new) | Source adapter: discovery (no network), DuckDB snapshot via `fetch_blob` |
| `camsim_scene/layers/buildings.py` (new) | Layer build: read snapshot, sample heights, write GLBs + `tileset.json`, skip marker |
| `camsim_scene/cache.py` (modify) | `Cache.put_file(url, path)` for snapshots that are not downloads |
| `camsim_scene/sources/base.py` (modify) | `Layer.BUILDINGS` |
| `camsim_scene/sources/__init__.py` (modify) | register `overture_buildings` |
| `camsim_scene/config.py` (modify) | `[buildings]` table, `buildings` in `LAYERS`, layer settings |
| `camsim_scene/pipeline.py` (modify) | `fetch_blob` hook, footprint skip, build the layer |
| `camsim_scene/verify.py` (modify) | quick tileset check; `--deep` decode-and-compare |
| `pyproject.toml` (modify) | `duckdb>=1.5.6` |
| `tests/overture_fixture.py` (new) | writes a small Overture-shaped GeoParquet with DuckDB |
| `tests/test_geodesy.py`, `tests/test_gltf.py`, `tests/test_buildings.py`, `tests/test_overture_buildings.py`, `tests/test_buildings_layer.py` (new) | unit/integration tests |
| `tests/validator/package.json` + lock (new) | pinned `3d-tiles-validator` |
| `tools/footprint_iou.py` + `tests/test_footprint_iou.py` (new) | acceptance gate 2 |
| `examples/pendleton.toml` (modify) | `[buildings]` |

**Runtime (`unreal_project/CamSimTest/Source/CamSimTest/`):**

| File | Responsibility |
|---|---|
| `Config/CamSimConfig.h/.cpp` (modify) | `scene.buildings.*` keys, env, validation |
| `Config/ScenePackage.cpp` (modify) | `BuildingsUrl` from the package; offline check |
| `Geospatial/SceneBuildings.h/.cpp` (new) | tag, `IsBuildingsActor`, `FindBuildingsTileset`, `SpawnBuildingsTileset` |
| `Geospatial/CesiumTuning.h/.cpp` (modify) | extract `TuneTileset` (one tileset) from the loop |
| `Geospatial/CesiumWorldSetup.cpp` (modify) | spawn after `ApplyCesiumBackendConfig` |
| `Camera/CamSimCamera.cpp` (modify) | frame-stats `Sse` from the terrain, not buildings |
| `Thermal/LandCoverLookup.h/.cpp` (modify) | `QueryMaterialCode(..., bBuildingHit, ...)`, `BuildingCode = 230` |
| `CIGI/CigiQueryHandler.cpp` (modify) | pass `bBuildingHit` |
| `Entity/SurfaceProbe.h/.cpp` (modify) | `MakeProbeQueryParams`, ignore the buildings tileset |
| `Tests/SceneBuildingsTest.cpp` (new), `Tests/ScenePackageTest.cpp`, `Tests/LandCoverLookupTest.cpp`, `Tests/SurfaceClampTest.cpp` (modify) | tests |

**Docs:** `docs/configuration.md`, `docs/terrain-feedback.md`, `docs/scene-packages.md`, `deploy/camsim_config.yaml`, `CLAUDE.md`, `ROADMAP.md`, `REALISM.md`.

## Commands

- Tooling tests: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests -q` (add `-k <name>` to narrow; `--network` for live tests).
- Build editor: `scripts/run.sh --build-only` (macOS and Linux).
- UE automation, headless (macOS; on Linux drop `-DisablePython` and use `UE_BIN=/opt/UE/Engine/Binaries/Linux/UnrealEditor`):
  ```bash
  UE_BIN="/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"
  "$UE_BIN" "$PWD/unreal_project/CamSimTest/CamSimTest.uproject" \
    -ExecCmds="Automation RunTests CamSim.Scene+Quit" \
    -TestExit="Automation Test Queue Empty" \
    -ReportExportPath="$PWD/.cache/automation-report" \
    -unattended -nullrhi -nosound -nosplash -DisablePython -log -stdout -FullStdOutLogOutput
  python3 -c "import json;r=json.load(open('.cache/automation-report/index.json',encoding='utf-8-sig'));print(r['succeeded'],r['failed'])"
  ```
  Replace `CamSim.Scene` with the filter each task names.

---

### Task 1: `overture_buildings` source and snapshot fetch

**Files:**
- Create: `scripts/scene/camsim_scene/sources/overture_buildings.py`
- Create: `scripts/scene/tests/overture_fixture.py`
- Create: `scripts/scene/tests/test_overture_buildings.py`
- Modify: `scripts/scene/pyproject.toml` (dependency), `scripts/scene/camsim_scene/cache.py` (`put_file`), `scripts/scene/camsim_scene/sources/base.py` (`Layer.BUILDINGS`), `scripts/scene/camsim_scene/sources/__init__.py` (registry), `scripts/scene/camsim_scene/pipeline.py:195-202` (`_cached_assets` hook)

**Interfaces:**
- Produces: `Layer.BUILDINGS = "buildings"`; `OvertureBuildings` (id `overture_buildings`, `layer = Layer.BUILDINGS`, `licence = "ODbL-1.0"`, `area_kind = "bbox"`, options `release: str`, optional `parquet: str` local override for tests); `OvertureBuildings.fetch_blob(asset: Asset, cache: Cache) -> tuple[Path, str, int]`; `Cache.put_file(url: str, path: Path) -> tuple[Path, str, int]`; `snapshot_url(release: str, bounds) -> str`. Snapshot columns: `id VARCHAR, geometry BLOB (WKB), height DOUBLE, num_floors INTEGER, class VARCHAR, subtype VARCHAR, sources VARCHAR (JSON)`, ordered by `id`.

- [ ] **Step 1: Add DuckDB and lock**

In `scripts/scene/pyproject.toml` add `"duckdb>=1.5.6",` to `dependencies` (alphabetical, after `"numpy..."` is fine). Run:

```bash
uv lock --project scripts/scene && uv run --project scripts/scene python -c "import duckdb; print(duckdb.__version__)"
```
Expected: prints a version ≥ 1.5.6.

- [ ] **Step 2: Write the fixture helper**

`scripts/scene/tests/overture_fixture.py`:

```python
"""A small Overture-shaped buildings GeoParquet (id, geometry WKB, bbox struct, height, num_floors, class, subtype,
sources), written with DuckDB so the real snapshot SQL runs against it offline."""

from __future__ import annotations

from pathlib import Path

import duckdb
import shapely


def write_overture_parquet(path: Path, rows: list[dict]) -> Path:
    """rows: {id, geom (shapely, lon/lat), height?, num_floors?, class?, subtype?}."""
    con = duckdb.connect()
    con.execute(
        "CREATE TABLE b (id VARCHAR, geometry BLOB, "
        "bbox STRUCT(xmin DOUBLE, xmax DOUBLE, ymin DOUBLE, ymax DOUBLE), "
        "height DOUBLE, num_floors INTEGER, class VARCHAR, subtype VARCHAR, sources STRUCT(dataset VARCHAR)[])"
    )
    for r in rows:
        g = r["geom"]
        w, s, e, n = g.bounds
        con.execute(
            "INSERT INTO b VALUES (?, ?, {'xmin': ?, 'xmax': ?, 'ymin': ?, 'ymax': ?}, ?, ?, ?, ?, [{'dataset': 'OpenStreetMap'}])",
            [r["id"], shapely.to_wkb(g), w, e, s, n, r.get("height"), r.get("num_floors"), r.get("class"), r.get("subtype")],
        )
    path.parent.mkdir(parents=True, exist_ok=True)
    con.execute(f"COPY b TO '{str(path).replace(chr(39), chr(39) * 2)}' (FORMAT parquet)")
    return path


def box_lonlat(lon: float, lat: float, w_m: float, h_m: float):
    """An axis-aligned w_m x h_m rectangle centred on (lon, lat)."""
    import math

    dlat = h_m / 2 / 111320.0
    dlon = w_m / 2 / (111320.0 * math.cos(math.radians(lat)))
    return shapely.box(lon - dlon, lat - dlat, lon + dlon, lat + dlat)
```

- [ ] **Step 3: Write the failing tests**

`scripts/scene/tests/test_overture_buildings.py`:

```python
import duckdb
import pytest
from overture_fixture import box_lonlat, write_overture_parquet

from camsim_scene.cache import Cache, CacheError
from camsim_scene.sources import make_source
from camsim_scene.sources.base import Area, Layer
from camsim_scene.sources.overture_buildings import snapshot_url

BBOX = (10.0, 10.0, 10.1, 10.1)


def _rows():
    return [
        {"id": "b2", "geom": box_lonlat(10.05, 10.05, 20, 10), "height": 7.5},
        {"id": "b1", "geom": box_lonlat(10.02, 10.02, 12, 12), "num_floors": 2},
        {"id": "far", "geom": box_lonlat(11.0, 11.0, 10, 10)},  # outside the bbox
    ]


def _src(tmp_path, parquet=None, release="2026-09-23.1"):
    opts = {"release": release}
    if parquet is not None:
        opts["parquet"] = str(parquet)
    return make_source("overture_buildings", None, opts)


def test_source_metadata():
    s = make_source("overture_buildings", None, {"release": "2026-09-23.1"})
    assert s.layer == Layer.BUILDINGS and s.licence == "ODbL-1.0" and s.area_kind == "bbox"
    assert s.version == "2026-09-23.1"


def test_discover_needs_no_network_and_names_release_and_bbox():
    s = make_source("overture_buildings", None, {"release": "2026-09-23.1"})
    [a] = s.discover(Area(BBOX))
    assert a.url == snapshot_url("2026-09-23.1", BBOX)
    assert "2026-09-23.1" in a.url and "10.1" in a.url
    assert a.metadata == {"release": "2026-09-23.1", "bbox": list(BBOX)}


def test_snapshot_keeps_bbox_rows_sorted_by_id(tmp_path):
    pq = write_overture_parquet(tmp_path / "src.parquet", _rows())
    s = _src(tmp_path, pq)
    [a] = s.discover(Area(BBOX))
    path, sha, size = s.fetch_blob(a, Cache(tmp_path / "cache"))
    ids = [r[0] for r in duckdb.connect().execute(f"SELECT id FROM read_parquet('{path}')").fetchall()]
    assert ids == ["b1", "b2"] and size == path.stat().st_size and len(sha) == 64


def test_fetch_uses_cached_snapshot_without_query(tmp_path):
    pq = write_overture_parquet(tmp_path / "src.parquet", _rows())
    cache = Cache(tmp_path / "cache")
    s = _src(tmp_path, pq)
    [a] = s.discover(Area(BBOX))
    p1, sha1, _ = s.fetch_blob(a, cache)
    pq.unlink()  # the "release" is gone
    a.sha256 = sha1
    p2, sha2, _ = s.fetch_blob(a, cache)
    assert (p2, sha2) == (p1, sha1)


def test_missing_release_names_it(tmp_path):
    s = _src(tmp_path, tmp_path / "missing.parquet", release="2025-01-01.0")
    [a] = s.discover(Area(BBOX))
    with pytest.raises(CacheError, match="2025-01-01.0"):
        s.fetch_blob(a, Cache(tmp_path / "cache"))


def test_put_file_moves_into_blobs_and_indexes_url(tmp_path):
    cache = Cache(tmp_path / "cache")
    f = tmp_path / "x.bin"
    f.write_bytes(b"abc")
    path, sha, size = cache.put_file("overture://x", f)
    assert path == cache.blob_path(sha) and size == 3 and not f.exists()
    assert cache.get("overture://x") == (path, sha, 3)


@pytest.mark.network
def test_live_query_tiny_bbox(tmp_path):
    """Oceanside pier area: the real S3 path, schema and geometry type."""
    s = make_source("overture_buildings", None, {"release": "2026-09-23.1"})
    [a] = s.discover(Area((-117.386, 33.192, -117.382, 33.196)))
    path, _, _ = s.fetch_blob(a, Cache(tmp_path / "cache"))
    row = duckdb.connect().execute(f"SELECT id, geometry, typeof(geometry) FROM read_parquet('{path}') LIMIT 1").fetchone()
    assert row is not None and row[2] == "BLOB"
```

- [ ] **Step 4: Run them to verify they fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_overture_buildings.py -q`
Expected: FAIL (`ModuleNotFoundError: camsim_scene.sources.overture_buildings` / unknown adapter).

- [ ] **Step 5: Implement `Layer.BUILDINGS`, `Cache.put_file`, the source, the registry entry**

In `camsim_scene/sources/base.py`, add to `Layer`:

```python
    BUILDINGS = "buildings"
```

In `camsim_scene/cache.py`, add to `Cache` (after `get`):

```python
    def put_file(self, url: str, path: Path) -> tuple[Path, str, int]:
        """Adopt a locally produced file (a query snapshot, not a download) as `url`'s bytes: hash it, move it into
        blobs/ and index the URL, exactly as a finished download is."""
        h = hashlib.sha256()
        size = 0
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(CHUNK), b""):
                h.update(chunk)
                size += len(chunk)
        sha = h.hexdigest()
        dst = self.blob_path(sha)
        dst.parent.mkdir(parents=True, exist_ok=True)
        os.replace(path, dst)
        atomic_write(self._index_path(url), json.dumps({"url": url, "sha256": sha, "size": size}).encode())
        return dst, sha, size
```

`camsim_scene/sources/overture_buildings.py`:

```python
"""Overture Maps buildings theme (ODbL 1.0; REALISM R3). Overture deletes releases after 60 days, so the bbox query's
result is the snapshot: a GeoParquet in the fetch cache, keyed by the release and bbox (the asset URL), whose sha256 the
manifest pins. Builds read the snapshot only. Discovery makes no request.

The query reads s3://overturemaps-us-west-2/release/<release>/theme=buildings/type=building/* anonymously through
DuckDB's httpfs extension (DuckDB installs it on first use) and filters on Overture's `bbox` struct column, so the
spatial extension is not needed; geometry stays WKB (BLOB). Option `parquet` replaces the S3 glob with a local file
(tests)."""

from __future__ import annotations

import uuid
from pathlib import Path

from ..cache import CacheError
from .base import Area, Asset, Layer, SourceBase

S3_GLOB = "s3://overturemaps-us-west-2/release/{release}/theme=buildings/type=building/*"
COLUMNS = "id, geometry, height, num_floors, class, subtype, to_json(sources) AS sources"


def snapshot_url(release: str, bounds) -> str:
    w, s, e, n = (float(v) for v in bounds)
    return f"overture://{release}/theme=buildings/type=building?bbox={w!r},{s!r},{e!r},{n!r}"


def _quote(text: str) -> str:
    return "'" + text.replace("'", "''") + "'"


def snapshot_sql(src: str, bounds, out: Path) -> str:
    w, s, e, n = (float(v) for v in bounds)
    return (
        f"COPY (SELECT {COLUMNS} FROM read_parquet({_quote(src)}, hive_partitioning = true) "
        f"WHERE bbox.xmin <= {e!r} AND bbox.xmax >= {w!r} AND bbox.ymin <= {n!r} AND bbox.ymax >= {s!r} "
        f"ORDER BY id) TO {_quote(str(out))} (FORMAT parquet, COMPRESSION zstd)"
    )


def connect():
    """A DuckDB connection that keeps GeoParquet geometry as WKB bytes."""
    import duckdb

    con = duckdb.connect()
    try:
        con.execute("SET enable_geoparquet_conversion = false")
    except duckdb.Error:  # a DuckDB without the setting never converts without the spatial extension
        pass
    return con


class OvertureBuildings(SourceBase):
    id = "overture_buildings"
    layer = Layer.BUILDINGS
    licence = "ODbL-1.0"
    dataset = "Overture Maps buildings"
    attribution = (
        "© OpenStreetMap contributors, Overture Maps Foundation (also: Esri Community Maps, Microsoft Global ML "
        "Building Footprints, Google Open Buildings, USGS 3DEP; docs.overturemaps.org/attribution)"
    )
    area_kind = "bbox"

    def __init__(self, options: dict | None = None, http=None):
        super().__init__(options, http)
        self.version = str(self._options.get("release", ""))
        if not self.version:
            raise ValueError("overture_buildings needs a release option")

    def discover(self, area: Area) -> list[Asset]:
        return [
            Asset(
                id=f"buildings-{self.version}",
                url=snapshot_url(self.version, area.bounds),
                group=self.id,
                metadata={"release": self.version, "bbox": [float(v) for v in area.bounds]},
            )
        ]

    def prepare(self, path, asset, cache):
        return None

    def fetch_blob(self, asset: Asset, cache) -> tuple[Path, str, int]:
        """The snapshot's (path, sha256, size): the cached blob when there is one, else run the query once."""
        if asset.sha256 and cache.blob_path(asset.sha256).exists():
            p = cache.blob_path(asset.sha256)
            return p, asset.sha256, p.stat().st_size
        hit = cache.lookup(asset.url, asset.sha256)  # indexed by URL from an earlier fetch; never downloads
        if hit is not None:
            return hit
        src = self._options.get("parquet") or S3_GLOB.format(release=self.version)
        tmp = cache.root / "tmp" / f"{uuid.uuid4().hex}.parquet"
        tmp.parent.mkdir(parents=True, exist_ok=True)
        con = connect()
        try:
            if "parquet" not in self._options:
                con.execute("INSTALL httpfs; LOAD httpfs; SET s3_region = 'us-west-2'")
            con.execute(snapshot_sql(src, asset.metadata["bbox"], tmp))
        except Exception as e:  # noqa: BLE001 - duckdb.IOException/HTTPException: release deleted or offline
            tmp.unlink(missing_ok=True)
            raise CacheError(
                f"Overture release {self.version} could not be read ({e}); no snapshot of it is cached. Overture keeps "
                "releases 60 days: pick a current release in [buildings] release, or restore the cache"
            ) from e
        finally:
            con.close()
        path, sha, size = cache.put_file(asset.url, tmp)
        if asset.sha256 and sha != asset.sha256:
            raise CacheError(f"Overture {self.version} snapshot sha256 {sha} but the manifest pins {asset.sha256}")
        return path, sha, size
```

Add `Cache.lookup` (used above; `cache.get` would try to download `overture://…` and sleep through its retry loop on a miss):

```python
    def lookup(self, url: str, sha256: str | None = None) -> tuple[Path, str, int] | None:
        """The cached bytes `url` last resolved to (matching `sha256` when given), or None. Never downloads."""
        idx = self._index_path(url)
        if not idx.exists():
            return None
        known = json.loads(idx.read_text())
        p = self.blob_path(known["sha256"])
        if p.exists() and sha256 in (None, known["sha256"]):
            return p, known["sha256"], known["size"]
        return None
```

Register in `camsim_scene/sources/__init__.py` `BUILTIN`:

```python
    "overture_buildings": "camsim_scene.sources.overture_buildings:OvertureBuildings",
```

In `camsim_scene/pipeline.py` `_cached_assets`, use the hook:

```python
    src = make_source(rec.id, rec.adapter, rec.options, cache.http)
    fetch_blob = getattr(src, "fetch_blob", None)
    for a in rec.assets:
        if fetch_blob is not None:
            blob, a.sha256, a.size = fetch_blob(to_asset(a), cache)
        else:
            blob, a.sha256, a.size = cache.get(a.url, a.sha256, sign=src.sign)
        yield src, a, src.prepare(blob, to_asset(a), cache) or blob
```

(`to_asset`, `context.py:75`, copies `sha256` and `metadata` from the record.)

- [ ] **Step 6: Run the tests**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_overture_buildings.py -q`
Expected: 6 passed, 1 skipped (network). Then `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_overture_buildings.py --network -q -k live` → 1 passed (confirms the live path and that geometry is `BLOB`; if it is not, adjust `connect()` and record the finding in the source docstring).

- [ ] **Step 7: Run the whole tooling suite** (the `_cached_assets` change touches every fetch)

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests -q`
Expected: all previously passing tests still pass.

- [ ] **Step 8: Commit**

```bash
git add scripts/scene/pyproject.toml scripts/scene/uv.lock scripts/scene/camsim_scene/cache.py \
  scripts/scene/camsim_scene/sources/base.py scripts/scene/camsim_scene/sources/__init__.py \
  scripts/scene/camsim_scene/sources/overture_buildings.py scripts/scene/camsim_scene/pipeline.py \
  scripts/scene/tests/overture_fixture.py scripts/scene/tests/test_overture_buildings.py
git commit -m "feat(scene): overture_buildings source, DuckDB snapshot in the fetch cache (REALISM R3)"
```

---

### Task 2: `[buildings]` config and plan wiring

**Files:**
- Modify: `scripts/scene/camsim_scene/config.py`, `scripts/scene/camsim_scene/pipeline.py` (`_needs_footprints`, `_fetch`)
- Test: `scripts/scene/tests/test_config.py`, `scripts/scene/tests/test_pipeline.py`

**Interfaces:**
- Consumes: `Layer.BUILDINGS`, `overture_buildings` (Task 1).
- Produces: `config.LAYERS` includes `"buildings"`; `ScenePlan.buildings: dict` (`{"enabled": bool, "release": str, "min_area_m2": float}`, empty dict when no section); `config.buildings_settings(plan) -> dict | None`; manifest `layers["buildings"]` = `{"priorities": ["overture_buildings"], "release", "min_area_m2", "tiers": {"13": 2000.0, "14": 300.0}, "leaf_zoom": 15, "geometric_error": {"root": 200.0, "13": 60.0, "14": 20.0, "15": 6.0}, "rules": BUILDINGS_RULES_VERSION, "format": "3d-tiles-1.1"}`, absent without the section. `BUILDINGS_RULES_VERSION = "1"` lives in `camsim_scene/buildings.py` (Task 4 creates the module; this task creates it with just the constants below).

- [ ] **Step 1: Write the failing tests**

Append to `scripts/scene/tests/test_config.py`:

```python
BUILDINGS_TOML = {"name": "t", "bbox": [10.0, 10.0, 10.1, 10.1]}


def test_no_buildings_section_leaves_layers_unchanged():
    plan = parse_scene(dict(BUILDINGS_TOML))
    assert plan.buildings == {} and plan.priorities["buildings"] == []
    assert "buildings" not in layer_settings(plan)


def test_buildings_section_defaults_and_settings():
    plan = parse_scene({**BUILDINGS_TOML, "allow": ["ODbL-1.0"], "buildings": {}})
    assert plan.buildings == {"enabled": True, "release": "2026-09-23.1", "min_area_m2": 10.0}
    assert plan.priorities["buildings"] == ["overture_buildings"]
    assert plan.source_options["overture_buildings"] == {"release": "2026-09-23.1"}
    s = layer_settings(plan)["buildings"]
    assert s["tiers"] == {"13": 2000.0, "14": 300.0} and s["leaf_zoom"] == 15
    assert s["geometric_error"] == {"root": 200.0, "13": 60.0, "14": 20.0, "15": 6.0}


def test_buildings_disabled_is_no_layer():
    plan = parse_scene({**BUILDINGS_TOML, "buildings": {"enabled": False}})
    assert plan.priorities["buildings"] == [] and "buildings" not in layer_settings(plan)


@pytest.mark.parametrize(
    "bad",
    [{"enabled": "yes"}, {"release": ""}, {"release": 5}, {"min_area_m2": -1}, {"min_area_m2": True}, {"colour": 1}],
)
def test_buildings_section_rejects_bad_values(bad):
    with pytest.raises(ConfigError, match="buildings"):
        parse_scene({**BUILDINGS_TOML, "buildings": bad})


def test_priorities_buildings_cannot_be_set_directly():
    with pytest.raises(ConfigError, match="buildings"):
        parse_scene({**BUILDINGS_TOML, "priorities": {"buildings": ["overture_buildings"]}})
```

(`test_config.py` already defines `BASE` (the Pendleton bbox) for the R2 `features` tests: do not touch it, which is why these tests use the new name `BUILDINGS_TOML`. Put any missing import (`ConfigError`, `layer_settings`, `parse_scene`, `pytest`) into the file's existing top import block, never mid-file: ruff E402.)

Append to `scripts/scene/tests/test_pipeline.py`:

```python
def test_buildings_need_odbl_on_the_allow_list(tmp_path):
    scene = synthetic_scene(tmp_path / "src")
    scene["buildings"] = {}
    with pytest.raises(LicenceError, match="ODbL-1.0"):
        plan_scene(parse_scene(scene), tmp_path / "pkg", http=FakeHttp({}), out=io.StringIO())


def test_buildings_source_needs_no_footprint():
    from types import SimpleNamespace

    from camsim_scene.manifest import AssetRecord, SourceRecord
    from camsim_scene.pipeline import _needs_footprints

    asset = AssetRecord(id="buildings-x", url="overture://x", group="overture_buildings")
    rec = SourceRecord("overture_buildings", "overture_buildings", "O", "x", "ODbL-1.0", "", {"release": "x"}, [asset])
    assert not _needs_footprints(SimpleNamespace(sources=[rec]))
```

- [ ] **Step 2: Run them to verify they fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_config.py scripts/scene/tests/test_pipeline.py -q -k buildings`
Expected: FAIL (`unknown key(s) in scene: ['buildings']` / `AttributeError: buildings`).

- [ ] **Step 3: Implement**

Create `scripts/scene/camsim_scene/buildings.py` with only the constants for now (Task 4 fills it):

```python
"""Buildings layer (REALISM R3): Overture footprints -> extruded flat-roof meshes -> 3D Tiles 1.1."""

from __future__ import annotations

BUILDINGS_RULES_VERSION = "1"  # bump when geometry/height/tier rules change (it is in the layer settings hash)
DEFAULT_RELEASE = "2026-09-23.1"
TIER_MIN_AREA_M2 = {13: 2000.0, 14: 300.0}  # else LEAF_ZOOM
LEAF_ZOOM = 15
GEOMETRIC_ERROR = {"root": 200.0, 13: 60.0, 14: 20.0, 15: 6.0}
```

In `camsim_scene/config.py`:

- `LAYERS = ("terrain", "imagery", "landcover", "features", "buildings")`
- add `"buildings"` to `KEYS`
- add `buildings: dict = field(default_factory=dict)` to `ScenePlan` (last field)
- every `PROFILES[...]["priorities"]` gets `"buildings": []`
- add the parser and settings:

```python
BUILDINGS_KEYS = ("enabled", "release", "min_area_m2")


def _parse_buildings(data) -> dict:
    from .buildings import DEFAULT_RELEASE

    if "buildings" not in data:
        return {}
    b = data["buildings"]
    if not isinstance(b, dict):
        raise ConfigError("buildings must be a table")
    unknown = set(b) - set(BUILDINGS_KEYS)
    if unknown:
        raise ConfigError(f"unknown key(s) in buildings: {sorted(unknown)}")
    enabled = b.get("enabled", True)
    if not isinstance(enabled, bool):
        raise ConfigError(f"buildings.enabled must be true or false, got {enabled!r}")
    release = b.get("release", DEFAULT_RELEASE)
    if not isinstance(release, str) or not release:
        raise ConfigError(f"buildings.release must be an Overture release id, got {release!r}")
    area = b.get("min_area_m2", 10.0)
    if isinstance(area, bool) or not isinstance(area, (int, float)) or not 0.0 <= area <= 10000.0:
        raise ConfigError(f"buildings.min_area_m2 must be a number in [0, 10000], got {area!r}")
    return {"enabled": enabled, "release": release, "min_area_m2": float(area)}


def buildings_settings(plan: ScenePlan) -> dict | None:
    """The buildings layer's settings (REALISM R3), or None when the layer is off."""
    from .buildings import BUILDINGS_RULES_VERSION, GEOMETRIC_ERROR, LEAF_ZOOM, TIER_MIN_AREA_M2

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
    }
```

In `parse_scene`, next to the `features` derivation:

```python
    if "buildings" in data.get("priorities", {}):
        raise ConfigError("priorities.buildings is derived from [buildings]; set the table instead")
    buildings = _parse_buildings(data)
    prio["buildings"] = ["overture_buildings"] if buildings.get("enabled") else []
```

(the `priorities.buildings` check must come before the loop that copies `priorities`, like the `features` check). After `options` is built:

```python
    if buildings.get("enabled"):
        options.setdefault("overture_buildings", {})["release"] = buildings["release"]
```

Pass `buildings=buildings` to the `ScenePlan(...)` call. In `layer_settings`, at the end:

```python
    bs = buildings_settings(plan)
    if bs is not None:
        out["buildings"] = bs
```

In `camsim_scene/pipeline.py`, `_needs_footprints` and `_fetch`: replace `(Layer.LANDCOVER, Layer.FEATURES)` with `(Layer.LANDCOVER, Layer.FEATURES, Layer.BUILDINGS)` in both places.

- [ ] **Step 4: Run the tests**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests -q`
Expected: all pass (including the existing byte-identity tests: no `buildings` key appears without the section).

- [ ] **Step 5: Commit**

```bash
git add scripts/scene/camsim_scene/config.py scripts/scene/camsim_scene/buildings.py scripts/scene/camsim_scene/pipeline.py \
  scripts/scene/tests/test_config.py scripts/scene/tests/test_pipeline.py
git commit -m "feat(scene): [buildings] table, buildings layer settings and ODbL gate (REALISM R3)"
```

---

### Task 3: Geodesy and the GLB writer/reader

**Files:**
- Create: `scripts/scene/camsim_scene/geodesy.py`, `scripts/scene/camsim_scene/gltf.py`
- Test: `scripts/scene/tests/test_geodesy.py`, `scripts/scene/tests/test_gltf.py`

**Interfaces:**
- Produces (`geodesy`): `geodetic_to_ecef(lon, lat, h) -> np.ndarray (..., 3)`; `ecef_to_geodetic(xyz) -> (lon, lat, h)`; `enu_rotation(lon0, lat0) -> np.ndarray (3, 3)` (rows E, N, U in ECEF); `enu_to_ecef_matrix(lon0, lat0, h0) -> np.ndarray (4, 4)`; `matrix_to_column_major(m) -> list[float]`; `column_major_to_matrix(v) -> np.ndarray`.
- Produces (`gltf`): `TileMesh(positions, normals, feature_ids, roof_indices, wall_indices)`; `PropertyTable(source_ids: list[str], classes: list[str], height_m: np.ndarray float32, height_source: np.ndarray uint8)`; `HEIGHT_SOURCES = ("overture_height", "overture_floors", "default")`; `ROOF_RGBA`, `WALL_RGBA`; `write_glb(mesh, table, copyright: str) -> bytes`; `read_glb(data) -> tuple[dict, bytes]`; `decode_tile(data) -> DecodedTile(positions, normals, feature_ids, roof_indices, wall_indices, table: PropertyTable, copyright: str)`.
- glTF axes: positions are (east, up, −north) metres from the tile's ENU origin (3D Tiles' y-up convention: content (x, y, z) → tile (x, −z, y)).

- [ ] **Step 1: Write the failing tests**

`scripts/scene/tests/test_geodesy.py`:

```python
import numpy as np

from camsim_scene import geodesy


def test_ecef_round_trip_sub_mm():
    lon = np.array([-117.45, 0.0, 179.9, -45.0])
    lat = np.array([33.30, 0.0, -60.0, 89.0])
    h = np.array([120.5, -30.0, 4000.0, 0.0])
    lo, la, hh = geodesy.ecef_to_geodetic(geodesy.geodetic_to_ecef(lon, lat, h))
    assert np.allclose(lo, lon, atol=1e-10) and np.allclose(la, lat, atol=1e-10) and np.allclose(hh, h, atol=1e-4)


def test_equator_prime_meridian_is_semi_major_axis():
    assert np.allclose(geodesy.geodetic_to_ecef(0.0, 0.0, 0.0), [6378137.0, 0.0, 0.0])


def test_enu_matrix_maps_local_up_to_height():
    m = geodesy.enu_to_ecef_matrix(-117.45, 33.30, 100.0)
    p = m @ np.array([0.0, 0.0, 10.0, 1.0])
    _, _, h = geodesy.ecef_to_geodetic(p[:3])
    assert abs(h - 110.0) < 1e-6
    v = geodesy.matrix_to_column_major(m)
    assert len(v) == 16 and np.allclose(geodesy.column_major_to_matrix(v), m)
```

`scripts/scene/tests/test_gltf.py`:

```python
import json
import struct

import numpy as np

from camsim_scene import gltf


def _mesh():
    pos = np.array([[0, 0, 0], [10, 0, 0], [10, 0, -10], [0, 0, -10]], np.float32)
    nrm = np.tile(np.array([0, 1, 0], np.float32), (4, 1))
    fid = np.array([0, 0, 1, 1], np.float32)
    return gltf.TileMesh(pos, nrm, fid, np.array([0, 2, 1], np.uint32), np.array([0, 3, 2], np.uint32))


def _table():
    return gltf.PropertyTable(
        ["osm:a", "osm:bé"], ["house", ""], np.array([7.5, 4.0], np.float32), np.array([0, 2], np.uint8)
    )


def test_glb_header_alignment_and_extensions():
    data = gltf.write_glb(_mesh(), _table(), "© test")
    magic, version, length = struct.unpack_from("<III", data, 0)
    assert magic == gltf.GLB_MAGIC and version == 2 and length == len(data) and len(data) % 4 == 0
    doc, _ = gltf.read_glb(data)
    assert set(doc["extensionsUsed"]) == {"EXT_mesh_features", "EXT_structural_metadata"}
    assert doc["asset"]["copyright"] == "© test"
    assert [m["pbrMetallicRoughness"]["baseColorFactor"] for m in doc["materials"]] == [
        list(gltf.ROOF_RGBA), list(gltf.WALL_RGBA)]
    for bv in doc["bufferViews"]:
        assert bv["byteOffset"] % 8 == 0
    pos = doc["accessors"][doc["meshes"][0]["primitives"][0]["attributes"]["POSITION"]]
    assert pos["min"] == [0.0, 0.0, -10.0] and pos["max"] == [10.0, 0.0, 0.0]


def test_glb_round_trip():
    t = gltf.decode_tile(gltf.write_glb(_mesh(), _table(), "© test"))
    m = _mesh()
    assert np.array_equal(t.positions, m.positions) and np.array_equal(t.feature_ids, m.feature_ids)
    assert np.array_equal(t.roof_indices, m.roof_indices) and np.array_equal(t.wall_indices, m.wall_indices)
    assert t.table.source_ids == ["osm:a", "osm:bé"] and t.table.classes == ["house", ""]
    assert np.array_equal(t.table.height_m, _table().height_m) and list(t.table.height_source) == [0, 2]
    assert t.copyright == "© test"


def test_glb_is_deterministic():
    assert gltf.write_glb(_mesh(), _table(), "x") == gltf.write_glb(_mesh(), _table(), "x")
```

- [ ] **Step 2: Run them to verify they fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_geodesy.py scripts/scene/tests/test_gltf.py -q`
Expected: FAIL (`ImportError`).

- [ ] **Step 3: Implement `geodesy.py`**

```python
"""WGS-84 geodetic <-> ECEF and local East-North-Up frames (REALISM R3 building tiles). Degrees and metres; heights
are ellipsoid heights."""

from __future__ import annotations

import numpy as np

A = 6378137.0
F = 1.0 / 298.257223563
E2 = F * (2.0 - F)
B = A * (1.0 - F)
EP2 = (A * A - B * B) / (B * B)


def geodetic_to_ecef(lon, lat, h) -> np.ndarray:
    lon, lat, h = (np.asarray(v, np.float64) for v in (lon, lat, h))
    lo, la = np.radians(lon), np.radians(lat)
    n = A / np.sqrt(1.0 - E2 * np.sin(la) ** 2)
    return np.stack(
        [(n + h) * np.cos(la) * np.cos(lo), (n + h) * np.cos(la) * np.sin(lo), (n * (1.0 - E2) + h) * np.sin(la)],
        axis=-1,
    )


def ecef_to_geodetic(xyz) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Bowring's method plus two Newton refinements (sub-mm anywhere near the surface)."""
    xyz = np.asarray(xyz, np.float64)
    x, y, z = xyz[..., 0], xyz[..., 1], xyz[..., 2]
    p = np.hypot(x, y)
    th = np.arctan2(z * A, p * B)
    la = np.arctan2(z + EP2 * B * np.sin(th) ** 3, p - E2 * A * np.cos(th) ** 3)
    for _ in range(2):
        n = A / np.sqrt(1.0 - E2 * np.sin(la) ** 2)
        h = np.where(np.abs(np.cos(la)) > 1e-9, p / np.cos(la) - n, np.abs(z) - B)
        la = np.arctan2(z, p * (1.0 - E2 * n / (n + h)))
    n = A / np.sqrt(1.0 - E2 * np.sin(la) ** 2)
    h = np.where(np.abs(np.cos(la)) > 1e-9, p / np.cos(la) - n, np.abs(z) - B)
    return np.degrees(np.arctan2(y, x)), np.degrees(la), h


def enu_rotation(lon0: float, lat0: float) -> np.ndarray:
    lo, la = np.radians(lon0), np.radians(lat0)
    return np.array(
        [
            [-np.sin(lo), np.cos(lo), 0.0],
            [-np.sin(la) * np.cos(lo), -np.sin(la) * np.sin(lo), np.cos(la)],
            [np.cos(la) * np.cos(lo), np.cos(la) * np.sin(lo), np.sin(la)],
        ]
    )


def enu_to_ecef_matrix(lon0: float, lat0: float, h0: float) -> np.ndarray:
    m = np.eye(4)
    m[:3, :3] = enu_rotation(lon0, lat0).T  # columns E, N, U
    m[:3, 3] = geodetic_to_ecef(lon0, lat0, h0)
    return m


def matrix_to_column_major(m: np.ndarray) -> list[float]:
    return [float(v) for v in np.asarray(m, np.float64).T.reshape(16)]


def column_major_to_matrix(v) -> np.ndarray:
    return np.asarray(v, np.float64).reshape(4, 4).T
```

- [ ] **Step 4: Implement `gltf.py`**

```python
"""Minimal glTF 2.0 binary (GLB) for building tiles (REALISM R3): one mesh with two primitives (roofs, walls) sharing
POSITION / NORMAL / _FEATURE_ID_0, one EXT_structural_metadata property table `building`. Deterministic bytes: JSON with
sorted keys and no whitespace, every bufferView 8-byte aligned."""

from __future__ import annotations

import json
import struct
from dataclasses import dataclass

import numpy as np

GLB_MAGIC = 0x46546C67
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942
FLOAT, UBYTE, UINT = 5126, 5121, 5125
ARRAY_BUFFER, ELEMENT_ARRAY_BUFFER = 34962, 34963
ROOF_RGBA = (0.30, 0.30, 0.31, 1.0)
WALL_RGBA = (0.42, 0.41, 0.40, 1.0)
HEIGHT_SOURCES = ("overture_height", "overture_floors", "default")
SCHEMA = {
    "id": "camsim_buildings",
    "enums": {"heightSource": {"valueType": "UINT8", "values": [{"name": n, "value": i} for i, n in enumerate(HEIGHT_SOURCES)]}},
    "classes": {
        "building": {
            "properties": {
                "class": {"type": "STRING"},
                "height_m": {"type": "SCALAR", "componentType": "FLOAT32"},
                "height_source": {"type": "ENUM", "enumType": "heightSource"},
                "source_id": {"type": "STRING"},
            }
        }
    },
}


@dataclass
class TileMesh:
    positions: np.ndarray  # (n, 3) float32: east, up, -north (m) from the tile's ENU origin
    normals: np.ndarray  # (n, 3) float32, same axes
    feature_ids: np.ndarray  # (n,) float32: index into the property table
    roof_indices: np.ndarray  # (k,) uint32
    wall_indices: np.ndarray  # (j,) uint32


@dataclass
class PropertyTable:
    source_ids: list[str]
    classes: list[str]
    height_m: np.ndarray  # float32
    height_source: np.ndarray  # uint8, index into HEIGHT_SOURCES


@dataclass
class DecodedTile:
    positions: np.ndarray
    normals: np.ndarray
    feature_ids: np.ndarray
    roof_indices: np.ndarray
    wall_indices: np.ndarray
    table: PropertyTable
    copyright: str


class _Bin:
    def __init__(self):
        self.data = bytearray()
        self.views: list[dict] = []

    def add(self, raw: bytes, target: int | None = None) -> int:
        self.data += b"\0" * (-len(self.data) % 8)
        view = {"buffer": 0, "byteOffset": len(self.data), "byteLength": len(raw)}
        if target is not None:
            view["target"] = target
        self.data += raw
        self.views.append(view)
        return len(self.views) - 1


def _strings(b: _Bin, values: list[str]) -> tuple[int, int]:
    enc = [v.encode("utf-8") for v in values]
    offsets = np.cumsum([0] + [len(e) for e in enc], dtype=np.uint64).astype(np.uint32)
    return b.add(b"".join(enc)), b.add(offsets.tobytes())


def write_glb(mesh: TileMesh, table: PropertyTable, copyright: str) -> bytes:
    b = _Bin()
    acc: list[dict] = []

    def accessor(arr: np.ndarray, ctype: int, atype: str, target: int, minmax: bool = False) -> int:
        a = {"bufferView": b.add(np.ascontiguousarray(arr).tobytes(), target), "componentType": ctype,
             "count": int(arr.shape[0]), "type": atype}
        if minmax:
            a["min"] = [float(v) for v in arr.min(axis=0)]
            a["max"] = [float(v) for v in arr.max(axis=0)]
        acc.append(a)
        return len(acc) - 1

    pos = accessor(mesh.positions.astype(np.float32), FLOAT, "VEC3", ARRAY_BUFFER, minmax=True)
    nrm = accessor(mesh.normals.astype(np.float32), FLOAT, "VEC3", ARRAY_BUFFER)
    fid = accessor(mesh.feature_ids.astype(np.float32), FLOAT, "SCALAR", ARRAY_BUFFER)
    features = {"EXT_mesh_features": {"featureIds": [{"featureCount": len(table.source_ids), "attribute": 0, "propertyTable": 0}]}}
    prims = []
    for mat, idx in ((0, mesh.roof_indices), (1, mesh.wall_indices)):
        if len(idx) == 0:
            continue
        ia = accessor(idx.astype(np.uint32), UINT, "SCALAR", ELEMENT_ARRAY_BUFFER)
        prims.append({"attributes": {"POSITION": pos, "NORMAL": nrm, "_FEATURE_ID_0": fid}, "indices": ia,
                      "material": mat, "mode": 4, "extensions": features})
    cls_v, cls_o = _strings(b, table.classes)
    sid_v, sid_o = _strings(b, table.source_ids)
    h_v = b.add(table.height_m.astype(np.float32).tobytes())
    hs_v = b.add(table.height_source.astype(np.uint8).tobytes())
    doc = {
        "asset": {"version": "2.0", "generator": "camsim-scene", "copyright": copyright},
        "extensionsUsed": ["EXT_mesh_features", "EXT_structural_metadata"],
        "extensions": {"EXT_structural_metadata": {"schema": SCHEMA, "propertyTables": [{
            "class": "building", "count": len(table.source_ids), "properties": {
                "class": {"values": cls_v, "stringOffsets": cls_o, "stringOffsetType": "UINT32"},
                "height_m": {"values": h_v},
                "height_source": {"values": hs_v},
                "source_id": {"values": sid_v, "stringOffsets": sid_o, "stringOffsetType": "UINT32"},
            }}]}},
        "materials": [
            {"pbrMetallicRoughness": {"baseColorFactor": list(rgba), "metallicFactor": 0.0, "roughnessFactor": 0.9}}
            for rgba in (ROOF_RGBA, WALL_RGBA)
        ],
        "meshes": [{"primitives": prims}],
        "nodes": [{"mesh": 0}],
        "scenes": [{"nodes": [0]}],
        "scene": 0,
        "accessors": acc,
        "bufferViews": b.views,
        "buffers": [{"byteLength": len(b.data) + (-len(b.data) % 4)}],
    }
    js = json.dumps(doc, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    js += b" " * (-len(js) % 4)
    bin_ = bytes(b.data) + b"\0" * (-len(b.data) % 4)
    total = 12 + 8 + len(js) + 8 + len(bin_)
    return (struct.pack("<III", GLB_MAGIC, 2, total) + struct.pack("<II", len(js), CHUNK_JSON) + js
            + struct.pack("<II", len(bin_), CHUNK_BIN) + bin_)


def read_glb(data: bytes) -> tuple[dict, bytes]:
    magic, version, length = struct.unpack_from("<III", data, 0)
    if magic != GLB_MAGIC or version != 2 or length != len(data):
        raise ValueError("not a GLB 2.0 file")
    jlen, jtype = struct.unpack_from("<II", data, 12)
    if jtype != CHUNK_JSON:
        raise ValueError("first GLB chunk is not JSON")
    doc = json.loads(data[20 : 20 + jlen])
    off = 20 + jlen
    blen, btype = struct.unpack_from("<II", data, off)
    if btype != CHUNK_BIN:
        raise ValueError("second GLB chunk is not BIN")
    return doc, data[off + 8 : off + 8 + blen]


def _view(doc: dict, bin_: bytes, i: int) -> bytes:
    v = doc["bufferViews"][i]
    return bin_[v["byteOffset"] : v["byteOffset"] + v["byteLength"]]


def _accessor(doc: dict, bin_: bytes, i: int) -> np.ndarray:
    a = doc["accessors"][i]
    dt = {FLOAT: np.float32, UINT: np.uint32, UBYTE: np.uint8}[a["componentType"]]
    n = {"SCALAR": 1, "VEC3": 3}[a["type"]]
    arr = np.frombuffer(_view(doc, bin_, a["bufferView"]), dt, count=a["count"] * n)
    return arr.reshape(-1, 3) if n == 3 else arr


def _decode_strings(doc: dict, bin_: bytes, prop: dict) -> list[str]:
    raw = _view(doc, bin_, prop["values"])
    off = np.frombuffer(_view(doc, bin_, prop["stringOffsets"]), np.uint32)
    return [raw[off[i] : off[i + 1]].decode("utf-8") for i in range(len(off) - 1)]


def decode_tile(data: bytes) -> DecodedTile:
    doc, bin_ = read_glb(data)
    prims = doc["meshes"][0]["primitives"]
    attrs = prims[0]["attributes"]
    idx = {p["material"]: _accessor(doc, bin_, p["indices"]) for p in prims}
    empty = np.zeros(0, np.uint32)
    t = doc["extensions"]["EXT_structural_metadata"]["propertyTables"][0]["properties"]
    table = PropertyTable(
        _decode_strings(doc, bin_, t["source_id"]),
        _decode_strings(doc, bin_, t["class"]),
        np.frombuffer(_view(doc, bin_, t["height_m"]["values"]), np.float32).copy(),
        np.frombuffer(_view(doc, bin_, t["height_source"]["values"]), np.uint8).copy(),
    )
    return DecodedTile(
        _accessor(doc, bin_, attrs["POSITION"]).copy(),
        _accessor(doc, bin_, attrs["NORMAL"]).copy(),
        _accessor(doc, bin_, attrs["_FEATURE_ID_0"]).copy(),
        idx.get(0, empty).copy(),
        idx.get(1, empty).copy(),
        table,
        doc["asset"].get("copyright", ""),
    )
```

- [ ] **Step 5: Run the tests**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_geodesy.py scripts/scene/tests/test_gltf.py -q`
Expected: 6 passed.

- [ ] **Step 6: Commit**

```bash
git add scripts/scene/camsim_scene/geodesy.py scripts/scene/camsim_scene/gltf.py \
  scripts/scene/tests/test_geodesy.py scripts/scene/tests/test_gltf.py
git commit -m "feat(scene): WGS-84 ENU frames and a deterministic GLB writer with feature metadata (REALISM R3)"
```

---

### Task 4: Footprints to buildings (clean, heights, base, mesh)

**Files:**
- Modify: `scripts/scene/camsim_scene/buildings.py`
- Test: `scripts/scene/tests/test_buildings.py`

**Interfaces:**
- Consumes: `geodesy` (Task 3), `gltf.TileMesh`, `gltf.PropertyTable`, `gltf.HEIGHT_SOURCES` (Task 3), snapshot columns (Task 1).
- Produces:
  - `Footprint(source_id: str, geometry, height: float | None, num_floors: int | None, cls: str)` (frozen dataclass; geometry shapely, lon/lat)
  - `read_snapshot(path: Path) -> list[Footprint]` (id order, duplicates dropped)
  - `local_scale(lat0: float) -> tuple[float, float]` (metres per degree lon, lat)
  - `clean(geom, min_area_m2: float) -> list[shapely.Polygon]` (oriented: exterior CCW, holes CW)
  - `building_height(height, num_floors) -> tuple[float, int]` (metres, index into `HEIGHT_SOURCES`)
  - `tier_zoom(area_m2: float) -> int`
  - `tile_of(lon: float, lat: float, z: int) -> tuple[int, int]`
  - `Building(source_id, parts: list[Polygon], cls, height_m, height_source, base_m, roof_m, area_m2, tile: tuple[int, int, int])`
  - `plan_buildings(footprints, heights_fn, min_area_m2) -> tuple[list[Building], dict]` where `heights_fn(lon: np.ndarray, lat: np.ndarray) -> np.ndarray` (ellipsoid m, NaN = unknown) is called once per leaf (z15) tile group; stats keys `input`, `kept`, `dropped_small`, `dropped_invalid`, `dropped_no_height`
  - `tile_mesh(buildings: list[Building], origin: tuple[float, float, float]) -> tuple[TileMesh, PropertyTable]` (origin = lon, lat, h of the ENU frame)

- [ ] **Step 1: Write the failing tests**

`scripts/scene/tests/test_buildings.py`:

```python
import numpy as np
import pytest
import shapely
from overture_fixture import box_lonlat, write_overture_parquet

from camsim_scene import buildings as B
from camsim_scene import geodesy, gltf


def flat(h):
    return lambda lon, lat: np.full(np.shape(lon), float(h))


def slope(lon, lat):
    """Rises 1 m per metre eastward from 100 m at lon 10.0 (lat ~10)."""
    return 100.0 + (np.asarray(lon) - 10.0) * 111320.0 * np.cos(np.radians(10.0))


def test_height_fallbacks_and_clamps():
    assert B.building_height(7.5, 3) == (7.5, 0)
    assert B.building_height(None, 2) == (6.4, 1)
    assert B.building_height(0.0, None) == (4.0, 2)
    assert B.building_height(-3.0, 0) == (4.0, 2)
    assert B.building_height(1.0, None) == (2.5, 0)
    assert B.building_height(400.0, None) == (150.0, 0)
    assert B.building_height(None, 100) == (150.0, 1)


def test_tiers():
    assert B.tier_zoom(2000.0) == 13 and B.tier_zoom(1999.9) == 14 and B.tier_zoom(300.0) == 14
    assert B.tier_zoom(299.9) == 15


def test_tile_of_matches_tile_bounds():
    from camsim_scene.tiling import tile_bounds

    x, y = B.tile_of(-117.45, 33.30, 15)
    w, s, e, n = tile_bounds(15, x, y)
    assert w <= -117.45 < e and s <= 33.30 < n


def test_clean_repairs_bowtie_and_drops_lines():
    bowtie = shapely.Polygon([(10.0, 10.0), (10.001, 10.001), (10.001, 10.0), (10.0, 10.001)])
    parts = B.clean(bowtie, 10.0)
    assert len(parts) == 2 and all(p.is_valid and p.exterior.is_ccw for p in parts)
    assert B.clean(shapely.LineString([(10, 10), (10.001, 10)]), 10.0) == []


def test_clean_drops_slivers_and_orients_holes():
    tiny = box_lonlat(10.0, 10.0, 2, 2)  # 4 m2
    assert B.clean(tiny, 10.0) == []
    ring = box_lonlat(10.0, 10.0, 30, 30).difference(box_lonlat(10.0, 10.0, 10, 10))
    [p] = B.clean(ring, 10.0)
    assert p.exterior.is_ccw and not p.interiors[0].is_ccw


def test_plan_base_is_min_ground_minus_half_metre_on_a_slope():
    fp = B.Footprint("osm:w1", box_lonlat(10.0005, 10.0, 20, 10), 6.0, None, "house")
    [b], stats = B.plan_buildings([fp], slope, 10.0)
    west_edge = 10.0005 - 10.0 / (111320.0 * np.cos(np.radians(10.0)))
    assert b.base_m == pytest.approx(slope(west_edge, 10.0) - 0.5, abs=1e-6)
    assert b.roof_m == pytest.approx(b.base_m + 0.5 + 6.0)
    assert stats == {"input": 1, "kept": 1, "dropped_small": 0, "dropped_invalid": 0, "dropped_no_height": 0}


def test_plan_drops_buildings_without_ground_height():
    fp = B.Footprint("osm:w1", box_lonlat(10.0, 10.0, 20, 20), 6.0, None, "")
    kept, stats = B.plan_buildings([fp], lambda lon, lat: np.full(np.shape(lon), np.nan), 10.0)
    assert kept == [] and stats["dropped_no_height"] == 1


def test_plan_tier_and_tile_follow_area_and_centroid():
    big = B.Footprint("osm:big", box_lonlat(10.0, 10.0, 50, 50), None, None, "")
    small = B.Footprint("osm:small", box_lonlat(10.01, 10.01, 10, 10), None, None, "")
    got = {b.source_id: b for b in B.plan_buildings([big, small], flat(0.0), 10.0)[0]}
    assert got["osm:big"].tile[0] == 13 and got["osm:small"].tile[0] == 15
    assert got["osm:small"].tile[1:] == B.tile_of(10.01, 10.01, 15)


def _decode_up(mesh, origin):
    """glTF (e, u, -n) -> ellipsoid heights of every vertex."""
    enu = np.stack([mesh.positions[:, 0], -mesh.positions[:, 2], mesh.positions[:, 1]], axis=1).astype(np.float64)
    m = geodesy.enu_to_ecef_matrix(*origin)
    ecef = (m[:3, :3] @ enu.T).T + m[:3, 3]
    return geodesy.ecef_to_geodetic(ecef)


def test_mesh_round_trips_heights_and_roof_area_with_a_hole():
    ring = box_lonlat(10.0, 10.0, 30, 30).difference(box_lonlat(10.0, 10.0, 10, 10))
    fp = B.Footprint("osm:r1", ring, 9.0, None, "industrial")
    [b], _ = B.plan_buildings([fp], flat(50.0), 10.0)
    origin = (10.0, 10.0, 55.0)
    mesh, table = B.tile_mesh([b], origin)
    lon, lat, h = _decode_up(mesh, origin)
    assert h.min() == pytest.approx(49.5, abs=1e-3) and h.max() == pytest.approx(59.0, abs=1e-3)
    tri = mesh.positions[mesh.roof_indices].reshape(-1, 3, 3).astype(np.float64)
    cross = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    assert (cross[:, 1] > 0).all()  # every roof triangle faces up (glTF +Y)
    assert 0.5 * np.linalg.norm(cross, axis=1).sum() == pytest.approx(800.0, rel=1e-3)  # 900 - 100 m2
    assert table.source_ids == ["osm:r1"] and table.classes == ["industrial"]
    assert list(table.height_source) == [0] and table.height_m[0] == pytest.approx(9.0)


def test_mesh_walls_face_outward():
    fp = B.Footprint("osm:w1", box_lonlat(10.0, 10.0, 20, 20), 5.0, None, "")
    [b], _ = B.plan_buildings([fp], flat(0.0), 10.0)
    mesh, _ = B.tile_mesh([b], (10.0, 10.0, 0.0))
    tri = mesh.positions[mesh.wall_indices].reshape(-1, 3, 3).astype(np.float64)
    centre = tri.mean(axis=1)
    normal = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    horiz = centre[:, [0, 2]]  # (e, -n) from the building centre at the origin
    assert (np.einsum("ij,ij->i", normal[:, [0, 2]], horiz) > 0).all()
    assert np.allclose(np.linalg.norm(mesh.normals, axis=1), 1.0, atol=1e-5)


def test_mesh_skips_zero_length_edges():
    p = shapely.Polygon([(10.0, 10.0), (10.0002, 10.0), (10.0002, 10.0), (10.0002, 10.0002), (10.0, 10.0002)])
    fp = B.Footprint("osm:d", p, 5.0, None, "")
    [b], _ = B.plan_buildings([fp], flat(0.0), 10.0)
    mesh, _ = B.tile_mesh([b], (10.0, 10.0, 0.0))
    assert len(mesh.wall_indices) == 4 * 6 and np.isfinite(mesh.normals).all()


def test_feature_ids_follow_source_id_order():
    a = B.Footprint("osm:b", box_lonlat(10.0, 10.0, 10, 10), 5.0, None, "")
    c = B.Footprint("osm:a", box_lonlat(10.0003, 10.0, 10, 10), 5.0, None, "")
    blds, _ = B.plan_buildings([a, c], flat(0.0), 10.0)
    mesh, table = B.tile_mesh(blds, (10.0, 10.0, 0.0))
    assert table.source_ids == ["osm:a", "osm:b"] and set(np.unique(mesh.feature_ids)) == {0.0, 1.0}


def test_read_snapshot_orders_and_dedups(tmp_path):
    rows = [
        {"id": "b", "geom": box_lonlat(10.0, 10.0, 10, 10), "height": 5.0, "class": "house"},
        {"id": "a", "geom": box_lonlat(10.001, 10.0, 10, 10), "num_floors": 2},
        {"id": "a", "geom": box_lonlat(10.002, 10.0, 10, 10)},
    ]
    src = write_overture_parquet(tmp_path / "src.parquet", rows)
    import duckdb

    out = tmp_path / "snap.parquet"
    duckdb.connect().execute(
        f"COPY (SELECT id, geometry, height, num_floors, class, subtype, to_json(sources) AS sources "
        f"FROM read_parquet('{src}') ORDER BY id) TO '{out}' (FORMAT parquet)")
    fps = B.read_snapshot(out)
    assert [f.source_id for f in fps] == ["a", "b"]
    assert fps[0].num_floors == 2 and fps[1].height == 5.0 and fps[1].cls == "house" and fps[0].cls == ""
```

- [ ] **Step 2: Run them to verify they fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_buildings.py -q`
Expected: FAIL (`AttributeError: module 'camsim_scene.buildings' has no attribute 'building_height'`).

- [ ] **Step 3: Implement** — in `scripts/scene/camsim_scene/buildings.py`, first put this import block directly under `from __future__ import annotations` (above the Task 2 constants; every import lives in the top block, ruff E402):

```python
import math
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import shapely
from shapely.geometry.polygon import orient

from . import geodesy
from .gltf import PropertyTable, TileMesh
from .tiling import tile_bounds, tile_size_deg
```

then append the following below the Task 2 constants:

```python
M_PER_DEG = 111320.0
FLOOR_M = 3.2
DEFAULT_HEIGHT_M = 4.0
MIN_HEIGHT_M, MAX_HEIGHT_M = 2.5, 150.0
SINK_M = 0.5
MIN_EDGE_M = 0.01


@dataclass(frozen=True)
class Footprint:
    source_id: str
    geometry: object  # shapely, lon/lat
    height: float | None
    num_floors: int | None
    cls: str


@dataclass
class Building:
    source_id: str
    parts: list  # shapely Polygons, lon/lat, exterior CCW / holes CW
    cls: str
    height_m: float
    height_source: int  # index into gltf.HEIGHT_SOURCES
    base_m: float
    roof_m: float
    area_m2: float
    tile: tuple[int, int, int]  # (z, x, y)


def read_snapshot(path: Path) -> list[Footprint]:
    from .sources.overture_buildings import connect

    con = connect()
    try:
        rows = con.execute(
            "SELECT id, geometry, height, num_floors, class FROM read_parquet(?) ORDER BY id", [str(path)]
        ).fetchall()
    finally:
        con.close()
    out, seen = [], set()
    for sid, wkb, height, floors, cls in rows:
        if sid in seen or wkb is None:
            continue
        seen.add(sid)
        out.append(Footprint(sid, shapely.from_wkb(bytes(wkb)), height, floors, cls or ""))
    return out


def local_scale(lat0: float) -> tuple[float, float]:
    return M_PER_DEG * math.cos(math.radians(lat0)), M_PER_DEG


def _area_m2(poly, lat0: float) -> float:
    sx, sy = local_scale(lat0)
    return abs(poly.area) * sx * sy


def clean(geom, min_area_m2: float) -> list:
    if geom is None or geom.is_empty:
        return []
    g = shapely.make_valid(geom)
    polys = [p for p in shapely.get_parts(g) if isinstance(p, shapely.Polygon)]
    multi = [q for p in shapely.get_parts(g) if isinstance(p, shapely.MultiPolygon) for q in p.geoms]
    lat0 = geom.centroid.y if not geom.centroid.is_empty else 0.0
    out = []
    for p in sorted(polys + multi, key=lambda p: (p.bounds, p.area)):
        if not p.is_empty and _area_m2(p, lat0) >= min_area_m2:
            out.append(orient(p, sign=1.0))
    return out


def building_height(height, num_floors) -> tuple[float, int]:
    if height is not None and math.isfinite(height) and height > 0:
        h, src = float(height), 0
    elif num_floors is not None and num_floors > 0:
        h, src = num_floors * FLOOR_M, 1
    else:
        h, src = DEFAULT_HEIGHT_M, 2
    return min(MAX_HEIGHT_M, max(MIN_HEIGHT_M, h)), src


def tier_zoom(area_m2: float) -> int:
    for z in sorted(TIER_MIN_AREA_M2):
        if area_m2 >= TIER_MIN_AREA_M2[z]:
            return z
    return LEAF_ZOOM


def tile_of(lon: float, lat: float, z: int) -> tuple[int, int]:
    s = tile_size_deg(z)
    return int(math.floor((lon + 180.0) / s)), int(math.floor((lat + 90.0) / s))


def _ground_points(parts) -> np.ndarray:
    pts = [np.asarray(p.exterior.coords) for p in parts]
    pts.append(np.asarray([shapely.MultiPolygon(parts).centroid.coords[0]]))
    return np.concatenate(pts)


def plan_buildings(footprints, heights_fn, min_area_m2: float) -> tuple[list[Building], dict]:
    stats = {"input": len(footprints), "kept": 0, "dropped_small": 0, "dropped_invalid": 0, "dropped_no_height": 0}
    staged = []
    for fp in footprints:
        if fp.geometry is None or fp.geometry.is_empty or fp.geometry.geom_type not in (
            "Polygon", "MultiPolygon", "GeometryCollection"):
            stats["dropped_invalid"] += 1
            continue
        parts = clean(fp.geometry, min_area_m2)
        if not parts:
            stats["dropped_small"] += 1
            continue
        c = shapely.MultiPolygon(parts).centroid
        area = sum(_area_m2(p, c.y) for p in parts)
        staged.append((fp, parts, c.x, c.y, area))
    groups: dict[tuple[int, int], list] = {}
    for item in staged:
        groups.setdefault(tile_of(item[2], item[3], LEAF_ZOOM), []).append(item)
    out = []
    for key in sorted(groups):
        items = groups[key]
        pts = [_ground_points(it[1]) for it in items]
        allp = np.concatenate(pts)
        h = np.asarray(heights_fn(allp[:, 0], allp[:, 1]), np.float64)
        i = 0
        for (fp, parts, cx, cy, area), p in zip(items, pts):
            g = h[i : i + len(p)]
            i += len(p)
            if not np.isfinite(g).all():
                stats["dropped_no_height"] += 1
                continue
            height, src = building_height(fp.height, fp.num_floors)
            base = float(g.min()) - SINK_M
            z = tier_zoom(area)
            out.append(Building(fp.source_id, parts, fp.cls, height, src, base, base + SINK_M + height, area,
                                (z, *tile_of(cx, cy, z))))
    out.sort(key=lambda b: b.source_id)
    stats["kept"] = len(out)
    return out, stats


def _cdt_triangles(poly) -> list[np.ndarray]:
    """Roof triangles (3 x 2 lon/lat each, CCW), triangulated in a locally isotropic plane."""
    sx, sy = local_scale(poly.centroid.y)
    scaled = shapely.affinity.scale(poly, xfact=sx / sy, yfact=1.0, origin=(0, 0))
    tris = []
    for t in shapely.get_parts(shapely.constrained_delaunay_triangles(scaled)):
        c = np.asarray(t.exterior.coords)[:3]
        c[:, 0] *= sy / sx
        a = (c[1, 0] - c[0, 0]) * (c[2, 1] - c[0, 1]) - (c[2, 0] - c[0, 0]) * (c[1, 1] - c[0, 1])
        if a == 0:
            continue
        tris.append(c if a > 0 else c[::-1])
    return tris


def tile_mesh(buildings: list[Building], origin: tuple[float, float, float]) -> tuple[TileMesh, PropertyTable]:
    m = geodesy.enu_to_ecef_matrix(*origin)
    rot, o = m[:3, :3].T, m[:3, 3]  # ECEF -> ENU rotation, origin

    def to_gl(lonlat: np.ndarray, h: float) -> np.ndarray:
        e = geodesy.geodetic_to_ecef(lonlat[:, 0], lonlat[:, 1], np.full(len(lonlat), h))
        enu = (rot @ (e - o).T).T
        return np.stack([enu[:, 0], enu[:, 2], -enu[:, 1]], axis=1)

    pos, fid, roof, wall = [], [], [], []
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
                add(to_gl(tri, b.roof_m), roof, f)
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
    normals = np.repeat(nrm, 3, axis=0)
    mesh = TileMesh(p.astype(np.float32), normals.astype(np.float32), np.asarray(fid, np.float32),
                    np.asarray(roof, np.uint32), np.asarray(wall, np.uint32))
    table = PropertyTable([b.source_id for b in bs], [b.cls for b in bs],
                          np.asarray([b.height_m for b in bs], np.float32),
                          np.asarray([b.height_source for b in bs], np.uint8))
    return mesh, table
```

Add `import shapely.affinity` next to `import shapely` (submodule import).

Note on the wall winding: the existing triangle (a0, b0, b1) in glTF axes (e, u, −n) has normal `cross(b0−a0, b1−a0)`; for a CCW exterior in lon/lat that is outward — `test_mesh_walls_face_outward` pins it. If the test shows inward normals, swap to `(a0, b1, b0, a0, a1, b1)`; do not change the test.

- [ ] **Step 4: Run the tests**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_buildings.py -q`
Expected: 13 passed.

- [ ] **Step 5: Commit**

```bash
git add scripts/scene/camsim_scene/buildings.py scripts/scene/tests/test_buildings.py
git commit -m "feat(scene): footprint cleaning, height fallbacks, ground base and flat-roof meshes (REALISM R3)"
```

---

### Task 5: Tileset and the `buildings` layer build

**Files:**
- Modify: `scripts/scene/camsim_scene/buildings.py` (tileset), `scripts/scene/camsim_scene/pipeline.py` (`build_scene`)
- Create: `scripts/scene/camsim_scene/layers/buildings.py`
- Test: `scripts/scene/tests/test_buildings.py` (tileset), `scripts/scene/tests/test_buildings_layer.py` (layer + pipeline)

**Interfaces:**
- Consumes: Tasks 1–4.
- Produces:
  - `buildings.tile_origin(z, x, y, blds) -> tuple[float, float, float]` (tile-bounds centre lon/lat; h = (min base + max roof) / 2)
  - `buildings.tileset_json(tiles: dict[tuple[int,int,int], tuple[list[Building], tuple[float,float,float]]]) -> dict` (explicit tree; content nodes carry `transform` = inv(parent abs) × own ENU→ECEF)
  - `buildings.walk_tileset(doc: dict) -> list[tuple[str, np.ndarray]]` (content uri, absolute 4×4 transform)
  - `layers.buildings.build(pkg: Path, m: Manifest, ctx: BuildContext) -> LayerStats` (the same return convention as `pipeline._build_landcover`; the plan stats travel in the marker, read back with `plan_stats`), writing `features/buildings/tileset.json`, `features/buildings/<z>/<x>/<y>.glb`; marker `.state/buildings.json`
  - `layers.buildings.heights_fn(st: WorkerState, m: Manifest)` returning the `heights_fn` for `plan_buildings`
  - `layers.buildings.plan_stats(pkg: Path) -> dict | None` (the plan stats dict the build stored in its marker; None without a marker)
  - `build.json` `layers.buildings` = LayerStats, plus `buildings` = plan stats dict.

- [ ] **Step 1: Write the failing tests**

Append to `scripts/scene/tests/test_buildings.py`:

```python
def _tiles(blds):
    by = {}
    for b in blds:
        by.setdefault(b.tile, []).append(b)
    return {k: (v, B.tile_origin(*k, v)) for k, v in by.items()}


def test_tileset_tree_tiers_errors_and_transforms():
    big = B.Footprint("osm:big", box_lonlat(10.0, 10.0, 50, 50), 20.0, None, "")
    small = B.Footprint("osm:small", box_lonlat(10.001, 10.001, 10, 10), 5.0, None, "")
    blds, _ = B.plan_buildings([big, small], flat(0.0), 10.0)
    doc = B.tileset_json(_tiles(blds))
    assert doc["asset"]["version"] == "1.1" and doc["geometricError"] == 200.0
    root = doc["root"]
    assert root["refine"] == "ADD" and "content" not in root and root["geometricError"] == 200.0
    [z13] = root["children"]
    assert z13["geometricError"] == 60.0 and z13["content"]["uri"].startswith("13/")
    [z14] = z13["children"]
    assert z14["geometricError"] == 20.0 and "content" not in z14 and "transform" not in z14
    [z15] = z14["children"]
    assert z15["geometricError"] == 6.0 and z15["content"]["uri"].startswith("15/")
    walked = dict(B.walk_tileset(doc))
    for (z, x, y), (bl, origin) in _tiles(blds).items():
        assert np.allclose(walked[f"{z}/{x}/{y}.glb"], geodesy.enu_to_ecef_matrix(*origin))


def test_tile_region_contains_straddling_building():
    from camsim_scene.tiling import tile_bounds

    x, y = B.tile_of(10.0, 10.0, 15)
    w, s, e, n = tile_bounds(15, x, y)
    # centroid just inside the east edge, footprint 20 m across the edge
    fp = B.Footprint("osm:edge", box_lonlat(e - 2e-5, (s + n) / 2, 20, 10), 5.0, None, "")
    [b], _ = B.plan_buildings([fp], flat(0.0), 10.0)
    assert b.tile == (15, x, y)
    doc = B.tileset_json(_tiles([b]))
    node = doc["root"]["children"][0]["children"][0]["children"][0]
    rw, rs, re, rn, lo, hi = node["boundingVolume"]["region"]
    bw, bs, be, bn = b.parts[0].bounds
    assert rw <= np.radians(bw) and re >= np.radians(be) and lo <= b.base_m and hi >= b.roof_m
    for parent in (doc["root"], doc["root"]["children"][0]):
        pr = parent["boundingVolume"]["region"]
        assert pr[0] <= rw and pr[2] >= re and pr[4] <= lo and pr[5] >= hi
```

`scripts/scene/tests/test_buildings_layer.py`:

```python
import io
import json

import numpy as np
from fake_sources import build_synthetic, synthetic_scene
from overture_fixture import box_lonlat, write_overture_parquet

from camsim_scene import buildings as B
from camsim_scene import gltf
from camsim_scene.cache import Cache
from camsim_scene.manifest import Manifest
from camsim_scene.pipeline import build_scene


def _scene(tmp_path, rows):
    scene = synthetic_scene(tmp_path / "src")
    pq = write_overture_parquet(tmp_path / "src" / "overture.parquet", rows)
    scene["allow"] = ["ODbL-1.0"]
    scene["buildings"] = {}
    scene.setdefault("sources", {})["overture_buildings"] = {"parquet": str(pq)}
    return scene


ROWS = [
    {"id": "b1", "geom": box_lonlat(10.2, 10.2, 60, 50), "height": 12.0, "class": "industrial"},
    {"id": "b2", "geom": box_lonlat(10.21, 10.2, 12, 10), "num_floors": 2},
    {"id": "b3", "geom": box_lonlat(10.22, 10.21, 8, 8)},
]


def test_layer_writes_tileset_and_tiles(tmp_path):
    pkg, _, info = build_synthetic(tmp_path, _scene(tmp_path, ROWS))
    doc = json.loads((pkg / "features/buildings/tileset.json").read_text())
    uris = [u for u, _ in B.walk_tileset(doc)]
    assert uris and all((pkg / "features/buildings" / u).is_file() for u in uris)
    ids = sorted(s for u in uris for s in gltf.decode_tile((pkg / "features/buildings" / u).read_bytes()).table.source_ids)
    assert ids == ["b1", "b2", "b3"]
    assert info["layers"]["buildings"]["tiles"] == len(uris) and info["buildings"]["kept"] == 3
    hashes = (pkg / "hashes.txt").read_text()
    assert "features/buildings/tileset.json" in hashes
    m = Manifest.load(pkg / "manifest.json")
    assert m.source("overture_buildings").assets[0].sha256


def test_rebuild_is_identical_and_skips(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path, _scene(tmp_path, ROWS))
    before = (pkg / "hashes.txt").read_bytes()
    info = build_scene(pkg, cache, jobs=1)
    assert info["layers"]["buildings"]["skipped"] == info["layers"]["buildings"]["tiles"]
    assert (pkg / "hashes.txt").read_bytes() == before


def test_rebuild_from_manifest_after_release_is_gone(tmp_path):
    scene = _scene(tmp_path, ROWS)
    a, cache, _ = build_synthetic(tmp_path, scene, name="a")
    (tmp_path / "src" / "overture.parquet").unlink()
    b = tmp_path / "b"
    b.mkdir()
    (b / "manifest.json").write_bytes((a / "manifest.json").read_bytes())
    build_scene(b, cache, jobs=1)
    assert (a / "hashes.txt").read_bytes() == (b / "hashes.txt").read_bytes()


def test_no_buildings_section_has_no_features_dir(tmp_path):
    pkg, _, info = build_synthetic(tmp_path)
    assert not (pkg / "features").exists() and "buildings" not in info["layers"]
```

- [ ] **Step 2: Run them to verify they fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_buildings.py scripts/scene/tests/test_buildings_layer.py -q`
Expected: FAIL (`tile_origin` / `tileset_json` missing; no `features/` written).

- [ ] **Step 3: Implement the tileset** — `tile_bounds` is already in the Task 4 top import block (`from .tiling import tile_bounds, tile_size_deg`); append to `camsim_scene/buildings.py`:

```python
def tile_origin(z: int, x: int, y: int, blds: list[Building]) -> tuple[float, float, float]:
    w, s, e, n = tile_bounds(z, x, y)
    lo = min(b.base_m for b in blds)
    hi = max(b.roof_m for b in blds)
    return (w + e) / 2.0, (s + n) / 2.0, round((lo + hi) / 2.0, 3)


def _region(blds: list[Building]) -> list[float]:
    w = min(p.bounds[0] for b in blds for p in b.parts)
    s = min(p.bounds[1] for b in blds for p in b.parts)
    e = max(p.bounds[2] for b in blds for p in b.parts)
    n = max(p.bounds[3] for b in blds for p in b.parts)
    lo = min(b.base_m for b in blds)
    hi = max(b.roof_m for b in blds)
    return [math.radians(w), math.radians(s), math.radians(e), math.radians(n), lo, hi]


def _union(regions: list[list[float]]) -> list[float]:
    r = np.asarray(regions)
    return [float(r[:, 0].min()), float(r[:, 1].min()), float(r[:, 2].max()), float(r[:, 3].max()),
            float(r[:, 4].min()), float(r[:, 5].max())]


def tileset_json(tiles: dict) -> dict:
    """tiles: (z, x, y) -> (buildings, ENU origin). Every ancestor of a content tile down from z13 exists in the tree
    (content-less when it holds no building of its own tier)."""
    from . import __version__

    keys = set(tiles)
    nodes = set()
    for z, x, y in keys:
        while z >= min(TIER_MIN_AREA_M2):
            nodes.add((z, x, y))
            z, x, y = z - 1, x // 2, y // 2

    def children(z, x, y):
        return sorted(k for k in nodes if k[0] == z + 1 and (k[1] // 2, k[2] // 2) == (x, y))

    def node(key, parent_abs: np.ndarray) -> dict:
        z, x, y = key
        kids_abs = parent_abs
        out: dict = {"geometricError": GEOMETRIC_ERROR[z]}
        regions = []
        if key in tiles:
            blds, origin = tiles[key]
            own = geodesy.enu_to_ecef_matrix(*origin)
            out["transform"] = geodesy.matrix_to_column_major(np.linalg.inv(parent_abs) @ own)
            out["content"] = {"uri": f"{z}/{x}/{y}.glb"}
            kids_abs = own
            regions.append(_region(blds))
        kids = [node(k, kids_abs) for k in children(z, x, y)]
        regions += [k["boundingVolume"]["region"] for k in kids]
        out["boundingVolume"] = {"region": _union(regions)}
        if kids:
            out["children"] = kids
        return out

    top = [node(k, np.eye(4)) for k in sorted(k for k in nodes if k[0] == min(TIER_MIN_AREA_M2))]
    root = {"boundingVolume": {"region": _union([t["boundingVolume"]["region"] for t in top])},
            "geometricError": GEOMETRIC_ERROR["root"], "refine": "ADD", "children": top}
    return {"asset": {"version": "1.1", "generator": f"camsim-scene {__version__}"},
            "geometricError": GEOMETRIC_ERROR["root"], "root": root}


def walk_tileset(doc: dict) -> list[tuple[str, np.ndarray]]:
    out = []

    def visit(n: dict, parent: np.ndarray):
        m = parent @ geodesy.column_major_to_matrix(n["transform"]) if "transform" in n else parent
        if "content" in n:
            out.append((n["content"]["uri"], m))
        for c in n.get("children", []):
            visit(c, m)

    visit(doc["root"], np.eye(4))
    return out
```

- [ ] **Step 4: Implement the layer** — `camsim_scene/layers/buildings.py`:

```python
"""Buildings layer build (REALISM R3): snapshot -> plan_buildings (heights from the package's own bare-earth terrain
sources, the same height path verify --deep uses) -> one GLB per tile + tileset.json under features/buildings/.
Rebuilt as a whole (it is small: Pendleton ~82k buildings) and skipped when its inputs hash is unchanged."""

from __future__ import annotations

import json
import shutil
import time
from pathlib import Path

import numpy as np

from .. import __version__
from .. import buildings as B
from ..context import BuildContext, WorkerState
from ..engine import LayerStats, inputs_hash, peak_rss_mb
from ..fsutil import atomic_write, sha256_file
from ..licences import layer_credit
from ..manifest import STATE_DIR, Manifest, canonical_json
from . import terrain

OUT = Path("features") / "buildings"


def terrain_zoom(m: Manifest) -> int:
    return max(int(r["max_zoom"]["terrain"]) for r in m.regions)


def heights_fn(st: WorkerState, m: Manifest):
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


def plan_stats(pkg: Path) -> dict | None:
    marker = Path(pkg) / STATE_DIR / "buildings.json"
    return json.loads(marker.read_text())["stats"] if marker.exists() else None


def build(pkg: Path, m: Manifest, ctx: BuildContext) -> LayerStats:
    st, out = LayerStats(), Path(pkg) / OUT
    s = m.layers.get("buildings")
    if not s:
        shutil.rmtree(Path(pkg) / "features", ignore_errors=True)
        (Path(pkg) / STATE_DIR / "buildings.json").unlink(missing_ok=True)
        return st
    t0 = time.monotonic()
    rec = m.source(s["priorities"][0])
    terrain_shas = sorted(a.sha256 for sid in m.layers["terrain"]["priorities"] for a in m.source(sid).assets)
    credit = layer_credit(rec.attribution, rec.licence)
    inputs = inputs_hash(m.layer_settings_hash("buildings"), __version__, rec.assets[0].sha256, credit, *terrain_shas)
    marker, ts_path = Path(pkg) / STATE_DIR / "buildings.json", out / "tileset.json"
    prev = json.loads(marker.read_text()) if marker.exists() else None
    if prev and prev["inputs"] == inputs and ts_path.exists() and sha256_file(ts_path) == prev["output"]:
        uris = [u for u, _ in B.walk_tileset(json.loads(ts_path.read_text()))]
        if all((out / u).is_file() for u in uris):
            st.tiles = st.skipped = len(uris)
            st.files = len(uris) + 1
            st.bytes = sum((out / u).stat().st_size for u in uris) + ts_path.stat().st_size
            st.seconds = round(time.monotonic() - t0, 3)
            return st
    shutil.rmtree(out, ignore_errors=True)
    blds, stats = plan_from_manifest(m, ctx)
    by: dict[tuple[int, int, int], list] = {}
    for b in blds:
        by.setdefault(b.tile, []).append(b)
    tiles = {k: (v, B.tile_origin(*k, v)) for k, v in sorted(by.items())}
    from ..gltf import write_glb

    for (z, x, y), (bl, origin) in tiles.items():
        mesh, table = B.tile_mesh(bl, origin)
        p = out / str(z) / str(x) / f"{y}.glb"
        p.parent.mkdir(parents=True, exist_ok=True)
        atomic_write(p, write_glb(mesh, table, credit.replace("\n", " ")))
    atomic_write(ts_path, canonical_json(B.tileset_json(tiles)).encode())
    atomic_write(marker, json.dumps({"inputs": inputs, "output": sha256_file(ts_path), "stats": stats}).encode())
    files = [p for p in out.rglob("*") if p.is_file()]
    st.tiles = st.built = len(tiles)
    st.files, st.bytes = len(files), sum(p.stat().st_size for p in files)
    st.peak_rss_mb = peak_rss_mb()
    st.seconds = round(time.monotonic() - t0, 3)
    return st
```

`layer_credit` appends an ODbL notice that says "land-cover layer"; make it generic: in `camsim_scene/licences.py` change `layer_credit(attribution, licence_id)` to `layer_credit(attribution, licence_id, layer: str = "land-cover")` and the notice to `f"\nThis {layer} layer contains OSM-derived data and is made available under the ODbL."`; call it with `layer="buildings"` here. Run `test_features_burn_into_tiles_and_legend` afterwards — its assertion (`"OSM-derived" in attribution`) still holds.

In `camsim_scene/pipeline.py` `build_scene`, after `stats["landcover"] = _build_landcover(pkg, m, ctx)`:

```python
        bstats = buildings_layer.build(pkg, m, ctx)
        if m.layers.get("buildings"):
            stats["buildings"] = bstats
```

and add the plan stats to the `info` dict only when the layer is on:

```python
        if m.layers.get("buildings"):
            info["buildings"] = buildings_layer.plan_stats(pkg)
```

(import: `from .layers import buildings as buildings_layer`). `normalise_modes` and `write_hashes` already run after this and cover `features/`.

- [ ] **Step 5: Run the tests**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests -q`
Expected: all pass (new: 2 in `test_buildings.py`, 4 in `test_buildings_layer.py`).

- [ ] **Step 6: Commit**

```bash
git add scripts/scene/camsim_scene/buildings.py scripts/scene/camsim_scene/layers/buildings.py \
  scripts/scene/camsim_scene/pipeline.py scripts/scene/camsim_scene/licences.py \
  scripts/scene/tests/test_buildings.py scripts/scene/tests/test_buildings_layer.py
git commit -m "feat(scene): buildings layer: size-tiered 3D Tiles 1.1 tileset with per-tile ENU transforms (REALISM R3)"
```

---

### Task 6: Verify (quick + deep) and the 3D Tiles validator

**Files:**
- Modify: `scripts/scene/camsim_scene/verify.py`
- Create: `scripts/scene/tests/validator/package.json`, `scripts/scene/tests/validator/package-lock.json`, `scripts/scene/tests/test_buildings_verify.py`

**Interfaces:**
- Consumes: `walk_tileset`, `decode_tile`, `plan_from_manifest`, `heights_fn` (Tasks 3–5).
- Produces: verify checks `buildings_tileset` (quick: every content uri exists) and `buildings_heights` (deep: ids, tile assignment and roof heights vs recomputation, roof within 0.01 m — float32 positions; see "Spec gaps").

- [ ] **Step 1: Pin the validator**

```bash
cd scripts/scene/tests/validator
npm view 3d-tiles-validator version   # note the version, e.g. 0.6.x
npm init -y >/dev/null && npm install --save-exact 3d-tiles-validator@<that version>
```
Edit `package.json` to `{"name": "camsim-scene-validator", "private": true, "dependencies": {"3d-tiles-validator": "<that version>"}}` and keep the lock file. Add `scripts/scene/tests/validator/node_modules/` to the repo `.gitignore` if `node_modules/` isn't already ignored.

- [ ] **Step 2: Write the failing tests**

`scripts/scene/tests/test_buildings_verify.py`:

```python
import json
import shutil
import subprocess
from pathlib import Path

import pytest
from test_buildings_layer import ROWS, _scene

from camsim_scene import gltf
from camsim_scene.verify import verify
from fake_sources import build_synthetic

VALIDATOR = Path(__file__).parent / "validator"


def _checks(report):
    return {c["name"]: c for c in report["checks"]}


def test_quick_and_deep_pass(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path, _scene(tmp_path, ROWS))
    c = _checks(verify(pkg, cache, deep=True))
    assert c["buildings_tileset"]["ok"] and c["buildings_heights"]["ok"], c


def test_deep_catches_an_edited_height(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path, _scene(tmp_path, ROWS))
    glb = next((pkg / "features/buildings").rglob("*.glb"))
    t = gltf.decode_tile(glb.read_bytes())
    t.positions[:, 1] += 1.0  # raise every vertex 1 m
    glb.write_bytes(gltf.write_glb(gltf.TileMesh(t.positions, t.normals, t.feature_ids, t.roof_indices,
                                                 t.wall_indices), t.table, t.copyright))
    c = _checks(verify(pkg, cache, deep=True))
    assert not c["buildings_heights"]["ok"] and not c["hashes"]["ok"]


def test_quick_catches_a_missing_tile(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path, _scene(tmp_path, ROWS))
    next((pkg / "features/buildings").rglob("*.glb")).unlink()
    c = _checks(verify(pkg))
    assert not c["buildings_tileset"]["ok"] and "missing" in c["buildings_tileset"]["detail"]


@pytest.mark.skipif(shutil.which("npx") is None, reason="needs Node")
def test_3d_tiles_validator_is_clean(tmp_path):
    if not (VALIDATOR / "node_modules").exists():
        subprocess.run(["npm", "ci"], cwd=VALIDATOR, check=True, capture_output=True)
    pkg, _, _ = build_synthetic(tmp_path, _scene(tmp_path, ROWS))
    out = tmp_path / "report.json"
    subprocess.run(["npx", "3d-tiles-validator", "--tilesetFile", str(pkg / "features/buildings/tileset.json"),
                    "--reportFile", str(out)], cwd=VALIDATOR, check=False, capture_output=True)
    report = json.loads(out.read_text())
    assert report["numErrors"] == 0, json.dumps(report["issues"], indent=1)[:4000]
```

- [ ] **Step 3: Run them to verify they fail**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_buildings_verify.py -q`
Expected: FAIL (`KeyError: 'buildings_tileset'`); the validator test may already pass or show real issues — fix any issue it reports in `gltf.py`/`buildings.py` (not in the test).

- [ ] **Step 4: Implement** — in `camsim_scene/verify.py`:

```python
def _check_buildings_tileset(pkg: Path) -> Check:
    from .buildings import walk_tileset

    root = pkg / "features" / "buildings"
    doc = json.loads((root / "tileset.json").read_text())
    missing = [u for u, _ in walk_tileset(doc) if not (root / u).is_file()]
    return Check("buildings_tileset", not missing, _summary([f"missing {u}" for u in missing]))


def _check_buildings_heights(pkg: Path, m: Manifest, st) -> Check:
    from . import geodesy
    from .buildings import walk_tileset
    from .gltf import decode_tile
    from .layers.buildings import OUT, plan_from_manifest

    want, _ = plan_from_manifest(m, st.ctx, st)
    by_id = {b.source_id: b for b in want}
    root = pkg / OUT
    seen, problems = set(), []
    for uri, mat in walk_tileset(json.loads((root / "tileset.json").read_text())):
        z, x, y = (int(v) for v in uri[: -len(".glb")].split("/"))
        t = decode_tile((root / uri).read_bytes())
        enu = np.stack([t.positions[:, 0], -t.positions[:, 2], t.positions[:, 1]], axis=1).astype(np.float64)
        _, _, h = geodesy.ecef_to_geodetic((mat[:3, :3] @ enu.T).T + mat[:3, 3])
        for f, sid in enumerate(t.table.source_ids):
            b = by_id.get(sid)
            if b is None:
                problems.append(f"{uri}: unexpected {sid}")
                continue
            seen.add(sid)
            if b.tile != (z, x, y):
                problems.append(f"{sid}: in {uri}, expected {b.tile}")
            roof = float(h[t.feature_ids == f].max())
            if abs(roof - b.roof_m) > ROOF_TOL_M:
                problems.append(f"{sid}: roof {roof:.3f} m, expected {b.roof_m:.3f} m")
    problems += [f"missing {sid}" for sid in sorted(set(by_id) - seen)]
    return Check("buildings_heights", not problems, _summary(problems))
```

with `ROOF_TOL_M = 0.01` near the other module constants. In `verify(...)`, after the existing quick checks:

```python
    if (pkg / "features" / "buildings" / "tileset.json").exists():
        checks.append(_guard("buildings_tileset", _check_buildings_tileset, pkg))
```

and at the end of `_deep(...)` before `return`:

```python
    if m.layers.get("buildings") and (pkg / "features" / "buildings" / "tileset.json").exists():
        checks.append(_guard("buildings_heights", _check_buildings_heights, pkg, m, st))
```

- [ ] **Step 5: Run the tests**

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests -q`
Expected: all pass (validator test passes, or is skipped without Node).

- [ ] **Step 6: Commit**

```bash
git add scripts/scene/camsim_scene/verify.py scripts/scene/tests/test_buildings_verify.py \
  scripts/scene/tests/validator/package.json scripts/scene/tests/validator/package-lock.json .gitignore
git commit -m "feat(scene): verify buildings tiles (presence, decode-and-compare) and run 3d-tiles-validator (REALISM R3)"
```

---

### Task 7: Runtime config and package resolution

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.h` (`FSceneConfig`), `Config/CamSimConfig.cpp` (YAML ~l.821, env ~l.1134, `Validate` ~l.1574), `Config/ScenePackage.h` (doc comment), `Config/ScenePackage.cpp` (`ResolvePackage`, `ValidateSources`)
- Test: `Tests/ScenePackageTest.cpp`

**Interfaces:**
- Produces on `FCamSimConfig::FSceneConfig`: `bool bBuildingsEnabled = true; double BuildingsMaximumScreenSpaceError = 16.0; bool bBuildingsCreatePhysicsMeshes = true; FString BuildingsUrl;` (`BuildingsUrl` filled by `ResolvePackage`: `file:///…/features/buildings/tileset.json`, empty when absent). Helper `bool CamSimScene::ShouldSpawnBuildings(const FCamSimConfig& Cfg)` = `Cfg.Scene.bBuildingsEnabled && !Cfg.Scene.BuildingsUrl.IsEmpty()`.

- [ ] **Step 1: Write the failing tests** — append to `Tests/ScenePackageTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneResolveBuildingsTest,
	"CamSim.Scene.Resolve.Buildings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneResolveBuildingsTest::RunTest(const FString& Parameters)
{
	const FString D = MakePackage(TEXT("buildings"), true, true, false);
	{
		const FCamSimConfig Cfg = ResolvedConfigFor(D);
		TestTrue(TEXT("no tileset: no url"), Cfg.Scene.BuildingsUrl.IsEmpty());
		TestFalse(TEXT("no tileset: no spawn"), CamSimScene::ShouldSpawnBuildings(Cfg));
	}
	WriteFile(D / TEXT("features/buildings/tileset.json"), TEXT("{}"));
	{
		const FCamSimConfig Cfg = ResolvedConfigFor(D);
		TestEqual(TEXT("url"), Cfg.Scene.BuildingsUrl, CamSimScene::PathToFileUrl(D / TEXT("features/buildings/tileset.json")));
		TestTrue(TEXT("spawn"), CamSimScene::ShouldSpawnBuildings(Cfg));
		FCamSimConfig Off = Cfg;
		Off.Scene.bBuildingsEnabled = false;
		TestFalse(TEXT("disabled: no spawn"), CamSimScene::ShouldSpawnBuildings(Off));
	}
	{
		FCamSimConfig Cfg = ResolvedConfigFor(D);
		Cfg.Scene.Dir.Reset();
		CamSimScene::ResolvePackage(Cfg);
		TestTrue(TEXT("cleared when scene.dir is unset"), Cfg.Scene.BuildingsUrl.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneBuildingsConfigTest,
	"CamSim.Scene.Buildings.Config",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneBuildingsConfigTest::RunTest(const FString& Parameters)
{
	{
		const FCamSimConfig Cfg;
		TestTrue(TEXT("default enabled"), Cfg.Scene.bBuildingsEnabled);
		TestEqual(TEXT("default sse"), Cfg.Scene.BuildingsMaximumScreenSpaceError, 16.0);
		TestTrue(TEXT("default physics"), Cfg.Scene.bBuildingsCreatePhysicsMeshes);
	}
	{
		const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT(
			"scene:\n  buildings:\n    enabled: false\n    maximum_screen_space_error: 32\n    create_physics_meshes: false\n"));
		TestFalse(TEXT("yaml enabled"), Cfg.Scene.bBuildingsEnabled);
		TestEqual(TEXT("yaml sse"), Cfg.Scene.BuildingsMaximumScreenSpaceError, 32.0);
		TestFalse(TEXT("yaml physics"), Cfg.Scene.bBuildingsCreatePhysicsMeshes);
	}
	{
		ON_SCOPE_EXIT
		{
			FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_SCENE_BUILDINGS_ENABLED"), TEXT(""));
			FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_SCENE_BUILDINGS_MAXIMUM_SCREEN_SPACE_ERROR"), TEXT(""));
			FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_SCENE_BUILDINGS_CREATE_PHYSICS_MESHES"), TEXT(""));
		};
		FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_SCENE_BUILDINGS_ENABLED"), TEXT("0"));
		FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_SCENE_BUILDINGS_MAXIMUM_SCREEN_SPACE_ERROR"), TEXT("8"));
		FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_SCENE_BUILDINGS_CREATE_PHYSICS_MESHES"), TEXT("0"));
		const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT("{}"));
		TestFalse(TEXT("env enabled"), Cfg.Scene.bBuildingsEnabled);
		TestEqual(TEXT("env sse"), Cfg.Scene.BuildingsMaximumScreenSpaceError, 8.0);
		TestFalse(TEXT("env physics"), Cfg.Scene.bBuildingsCreatePhysicsMeshes);
	}
	{
		FCamSimConfig Cfg;
		Cfg.Scene.BuildingsMaximumScreenSpaceError = 0.0;
		TestTrue(TEXT("sse 0 rejected"), Cfg.Validate().ContainsByPredicate(
			[](const FString& E) { return E.Contains(TEXT("scene.buildings.maximum_screen_space_error")); }));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneOfflineBuildingsTest,
	"CamSim.Scene.Offline.Buildings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneOfflineBuildingsTest::RunTest(const FString& Parameters)
{
	const FString D = MakePackage(TEXT("offline_buildings"), true, true, false);
	WriteFile(D / TEXT("features/buildings/tileset.json"), TEXT("{}"));
	FCamSimConfig Cfg = ResolvedConfigFor(D);
	Cfg.Scene.bOffline = true;
	TestEqual(TEXT("package buildings: ok"), CamSimScene::ValidateSources(Cfg).Num(), 0);
	Cfg.Scene.BuildingsUrl = TEXT("https://example.com/tileset.json");
	TestEqual(TEXT("network buildings rejected"), CamSimScene::ValidateSources(Cfg).Num(), 1);
	Cfg.Scene.bBuildingsEnabled = false;
	TestEqual(TEXT("disabled: not checked"), CamSimScene::ValidateSources(Cfg).Num(), 0);
	return true;
}
```

- [ ] **Step 2: Build; it fails to compile**

Run: `scripts/run.sh --build-only`
Expected: FAIL (`no member named 'BuildingsUrl'`, `no member named 'ShouldSpawnBuildings'`).

- [ ] **Step 3: Implement**

`Config/CamSimConfig.h`, in `FSceneConfig` after `bOffline`:

```cpp
		// REALISM R3: buildings from <dir>/features/buildings/tileset.json, one extra Cesium tileset spawned
		// at world init. CIGI LOS/HAT/HOT hit it (material code 230); the DIS clamp ignores it.
		// Env: CAMSIM_SCENE_BUILDINGS_ENABLED
		bool   bBuildingsEnabled = true;
		// Cesium MaximumScreenSpaceError of the buildings tileset (its own, not maximum_screen_space_error).
		// Env: CAMSIM_SCENE_BUILDINGS_MAXIMUM_SCREEN_SPACE_ERROR
		double BuildingsMaximumScreenSpaceError = 16.0;
		// Collision for the buildings tileset (CIGI LOS/HAT/HOT). Env: CAMSIM_SCENE_BUILDINGS_CREATE_PHYSICS_MESHES
		bool   bBuildingsCreatePhysicsMeshes = true;
```

and after `SeaLevelStation`:

```cpp
		// file:/// URL of <dir>/features/buildings/tileset.json; empty when the package has none (ResolvePackage).
		FString BuildingsUrl;
```

`Config/CamSimConfig.cpp`, in the `scene` YAML block after `offline`:

```cpp
			if (YamlHas(Sc, "buildings"))
			{
				ryml::ConstNodeRef Bd = Sc["buildings"];
				YamlBool  (Bd, "enabled",                    Cfg.Scene.bBuildingsEnabled);
				YamlDouble(Bd, "maximum_screen_space_error", Cfg.Scene.BuildingsMaximumScreenSpaceError);
				YamlBool  (Bd, "create_physics_meshes",      Cfg.Scene.bBuildingsCreatePhysicsMeshes);
			}
```

env, after `CAMSIM_SCENE_OFFLINE`:

```cpp
	Cfg.Scene.bBuildingsEnabled = GetEnvBool(TEXT("CAMSIM_SCENE_BUILDINGS_ENABLED"), Cfg.Scene.bBuildingsEnabled);
	Cfg.Scene.BuildingsMaximumScreenSpaceError = GetEnvDouble(
		TEXT("CAMSIM_SCENE_BUILDINGS_MAXIMUM_SCREEN_SPACE_ERROR"), Cfg.Scene.BuildingsMaximumScreenSpaceError);
	Cfg.Scene.bBuildingsCreatePhysicsMeshes = GetEnvBool(
		TEXT("CAMSIM_SCENE_BUILDINGS_CREATE_PHYSICS_MESHES"), Cfg.Scene.bBuildingsCreatePhysicsMeshes);
```

`Validate()`, before `Errors.Append(Scene.ResolveErrors);`:

```cpp
	if (!(Scene.BuildingsMaximumScreenSpaceError > 0.0 && Scene.BuildingsMaximumScreenSpaceError <= 1024.0))
		Errors.Add(FString::Printf(TEXT("scene.buildings.maximum_screen_space_error=%.3f out of range (0, 1024]"),
			Scene.BuildingsMaximumScreenSpaceError));
```

`Config/ScenePackage.h`: add to the namespace

```cpp
	/** Whether SetUpCesiumWorld spawns the package's buildings tileset (REALISM R3). */
	bool ShouldSpawnBuildings(const FCamSimConfig& Cfg);
```

and extend `ResolvePackage`'s comment with "features/buildings/tileset.json sets Cfg.Scene.BuildingsUrl".

`Config/ScenePackage.cpp`: in `ResolvePackage` add `Scene.BuildingsUrl.Reset();` with the other resets, and after the land-cover block:

```cpp
	const FString BuildingsFile = Dir / TEXT("features/buildings/tileset.json");
	if (FPaths::FileExists(BuildingsFile))
	{
		Scene.BuildingsUrl = PathToFileUrl(BuildingsFile);
	}
```

In `ValidateSources`, before `return Errors;` at the end (offline branch):

```cpp
	if (ShouldSpawnBuildings(Cfg))
	{
		CheckLocal(TEXT("features/buildings/tileset.json"), Cfg.Scene.BuildingsUrl);
	}
```

and inside `namespace CamSimScene` (after `ShouldExitOnConfigErrors`, before the closing `} // namespace CamSimScene`; the file ends with that closing brace, so appending at the literal end of file would define a global function and `CamSimScene::ShouldSpawnBuildings` would fail to link):

```cpp
bool ShouldSpawnBuildings(const FCamSimConfig& Cfg)
{
	return Cfg.Scene.bBuildingsEnabled && !Cfg.Scene.BuildingsUrl.IsEmpty();
}
```

- [ ] **Step 4: Build and run the tests**

Run: `scripts/run.sh --build-only`, then the headless automation command with filter `CamSim.Scene`.
Expected: build succeeds; all `CamSim.Scene.*` pass (3 new).

- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Config unreal_project/CamSimTest/Source/CamSimTest/Tests/ScenePackageTest.cpp
git commit -m "feat(scene): scene.buildings config and package resolution of features/buildings (REALISM R3)"
```

---

### Task 8: Spawn the buildings tileset

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Geospatial/SceneBuildings.h`, `Geospatial/SceneBuildings.cpp`, `Tests/SceneBuildingsTest.cpp`
- Modify: `Geospatial/CesiumTuning.h/.cpp` (extract `TuneTileset`), `Geospatial/CesiumWorldSetup.cpp`, `Camera/CamSimCamera.cpp:262-270` (frame-stats `Sse`)

**Interfaces:**
- Consumes: `CamSimScene::ShouldSpawnBuildings`, `Cfg.Scene.*` (Task 7).
- Produces (`namespace CamSim::Geospatial`):
  - `extern const FName BuildingsTag;` (= `"CamSimBuildings"`)
  - `bool IsBuildingsActor(const AActor* Actor);`
  - `ACesium3DTileset* FindBuildingsTileset(UWorld* World);`
  - `ACesium3DTileset* SpawnBuildingsTileset(UWorld* World, const FCamSimConfig& Cfg);` (idempotent; nullptr when `!ShouldSpawnBuildings`)
  - `void TuneTileset(ACesium3DTileset& Tileset, const FCamSimConfig& Cfg);` (one tileset; `ApplyCesiumTilesetTuning` loops over it)

- [ ] **Step 1: Write the failing tests** — `Tests/SceneBuildingsTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Config/CamSimConfig.h"
#include "Config/ScenePackage.h"
#include "Geospatial/CesiumWorldSetup.h"
#include "Geospatial/SceneBuildings.h"
#include "Cesium3DTileset.h"
#include "CesiumGeoreference.h"
#include "CesiumRasterOverlay.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "EngineUtils.h"

namespace
{
	/** A valid, empty 3D Tiles 1.1 tileset on disk, so Cesium loads it without a request or an error. */
	FString WriteEmptyTileset()
	{
		const FString D = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("Scene"), TEXT("buildings_spawn")));
		IFileManager::Get().MakeDirectory(*(D / TEXT("features/buildings")), true);
		const FString File = D / TEXT("features/buildings/tileset.json");
		FFileHelper::SaveStringToFile(TEXT(
			"{\"asset\":{\"version\":\"1.1\"},\"geometricError\":0,\"root\":{\"boundingVolume\":{\"region\":"
			"[-2.05,0.579,-2.049,0.58,0,10]},\"geometricError\":0,\"refine\":\"ADD\"}}"), *File);
		return CamSimScene::PathToFileUrl(File);
	}

	struct FTestWorld
	{
		UWorld* World = nullptr;
		FTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, false);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->GetWorldSettings()->bEnableWorldBoundsChecks = false;
			World->SpawnActor<ACesiumGeoreference>();
			ACesium3DTileset* T = World->SpawnActorDeferred<ACesium3DTileset>(
				ACesium3DTileset::StaticClass(), FTransform::Identity);
			T->SetTilesetSource(ETilesetSource::FromEllipsoid);
			T->FinishSpawning(FTransform::Identity);
		}
		~FTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}
		int32 CountTilesets() const
		{
			int32 N = 0;
			for (TActorIterator<ACesium3DTileset> It(World); It; ++It) { ++N; }
			return N;
		}
	};

	FCamSimConfig BuildingsConfig(const FString& Url)
	{
		FCamSimConfig Cfg;
		Cfg.Render.OriginShiftDistanceM = 20000.0;
		Cfg.CesiumBackend.Terrain.Source = TEXT("flat");
		Cfg.CesiumBackend.Imagery.Source = TEXT("none");
		Cfg.Scene.BuildingsUrl = Url;
		Cfg.Scene.BuildingsMaximumScreenSpaceError = 24.0;
		Cfg.Scene.bBuildingsCreatePhysicsMeshes = true;
		return Cfg;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneBuildingsSpawnTest,
	"CamSim.Scene.Buildings.SpawnAfterDestroyLoop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneBuildingsSpawnTest::RunTest(const FString& Parameters)
{
	AddExpectedMessage(TEXT("No game viewport was found"), EAutomationExpectedErrorFlags::Contains, 0);
	FTestWorld W;
	const FCamSimConfig Cfg = BuildingsConfig(WriteEmptyTileset());
	CamSim::Geospatial::SetUpCesiumWorld(W.World, Cfg);
	TestEqual(TEXT("terrain + buildings"), W.CountTilesets(), 2);
	ACesium3DTileset* B = CamSim::Geospatial::FindBuildingsTileset(W.World);
	if (!TestNotNull(TEXT("buildings tileset"), B)) return false;
	TestTrue(TEXT("tagged"), CamSim::Geospatial::IsBuildingsActor(B));
	TestTrue(TEXT("FromUrl"), B->GetTilesetSource() == ETilesetSource::FromUrl);
	TestEqual(TEXT("url"), B->GetUrl(), Cfg.Scene.BuildingsUrl);
	TestEqual(TEXT("own sse"), B->MaximumScreenSpaceError, 24.0);
	TestTrue(TEXT("physics"), B->GetCreatePhysicsMeshes());
	TestTrue(TEXT("movable"), B->GetRootComponent()->Mobility == EComponentMobility::Movable);
	TArray<UCesiumRasterOverlay*> Overlays;
	B->GetComponents<UCesiumRasterOverlay>(Overlays);
	TestEqual(TEXT("no overlays"), Overlays.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneBuildingsIdempotentTest,
	"CamSim.Scene.Buildings.SpawnIsIdempotent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneBuildingsIdempotentTest::RunTest(const FString& Parameters)
{
	AddExpectedMessage(TEXT("No game viewport was found"), EAutomationExpectedErrorFlags::Contains, 0);
	FTestWorld W;
	const FCamSimConfig Cfg = BuildingsConfig(WriteEmptyTileset());
	ACesium3DTileset* First = CamSim::Geospatial::SpawnBuildingsTileset(W.World, Cfg);
	ACesium3DTileset* Second = CamSim::Geospatial::SpawnBuildingsTileset(W.World, Cfg);
	TestTrue(TEXT("same actor"), First != nullptr && First == Second);
	TestEqual(TEXT("one buildings tileset"), W.CountTilesets(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneBuildingsDisabledTest,
	"CamSim.Scene.Buildings.DisabledSpawnsNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneBuildingsDisabledTest::RunTest(const FString& Parameters)
{
	AddExpectedMessage(TEXT("No game viewport was found"), EAutomationExpectedErrorFlags::Contains, 0);
	FTestWorld W;
	FCamSimConfig Cfg = BuildingsConfig(WriteEmptyTileset());
	Cfg.Scene.bBuildingsEnabled = false;
	CamSim::Geospatial::SetUpCesiumWorld(W.World, Cfg);
	TestEqual(TEXT("terrain only"), W.CountTilesets(), 1);
	TestNull(TEXT("none found"), CamSim::Geospatial::FindBuildingsTileset(W.World));
	TestFalse(TEXT("null actor is not buildings"), CamSim::Geospatial::IsBuildingsActor(nullptr));
	return true;
}
```

- [ ] **Step 2: Build; it fails to compile** — `scripts/run.sh --build-only` → missing `Geospatial/SceneBuildings.h`.

- [ ] **Step 3: Implement**

`Geospatial/CesiumTuning.h` — add next to `ApplyCesiumTilesetTuning`:

```cpp
	/** The tuning ApplyCesiumTilesetTuning applies, for one tileset (the buildings tileset spawned later uses it). */
	void TuneTileset(ACesium3DTileset& Tileset, const FCamSimConfig& Cfg);
```

(forward-declare `class ACesium3DTileset;`). In `CesiumTuning.cpp`, move the loop body into `TuneTileset(ACesium3DTileset& T, const FCamSimConfig& Cfg)` (replace `It->` with `T.`, compute `CulledSSE` inside), and make the loop `for (TActorIterator<ACesium3DTileset> It(World); It; ++It) { TuneTileset(**It, Cfg); }`. Behaviour and log line unchanged.

`Geospatial/SceneBuildings.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class AActor;
class ACesium3DTileset;
class UWorld;
struct FCamSimConfig;

/**
 * The scene package's buildings tileset (REALISM R3): one ACesium3DTileset spawned at world init after the
 * terrain set-up's destroy loop, recognised everywhere by its actor tag (CIGI material code 230, the DIS surface
 * probe's ignore list).
 */
namespace CamSim::Geospatial
{
	extern CAMSIMTEST_API const FName BuildingsTag;

	CAMSIMTEST_API bool IsBuildingsActor(const AActor* Actor);
	CAMSIMTEST_API ACesium3DTileset* FindBuildingsTileset(UWorld* World);

	/**
	 * Spawn (once) the tileset at Cfg.Scene.BuildingsUrl with the terrain's tuning, then the buildings' own SSE and
	 * physics; Movable when origin shifting is on; no raster overlays. nullptr when CamSimScene::ShouldSpawnBuildings
	 * is false. Returns the existing one on a repeat call.
	 */
	CAMSIMTEST_API ACesium3DTileset* SpawnBuildingsTileset(UWorld* World, const FCamSimConfig& Cfg);
}
```

`Geospatial/SceneBuildings.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Geospatial/SceneBuildings.h"
#include "CamSimTest.h"
#include "Config/CamSimConfig.h"
#include "Config/ScenePackage.h"
#include "Geospatial/CesiumTuning.h"

#include "Cesium3DTileset.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace CamSim::Geospatial
{
const FName BuildingsTag(TEXT("CamSimBuildings"));

bool IsBuildingsActor(const AActor* Actor)
{
	return Actor != nullptr && Actor->ActorHasTag(BuildingsTag);
}

ACesium3DTileset* FindBuildingsTileset(UWorld* World)
{
	if (!World) return nullptr;
	for (TActorIterator<ACesium3DTileset> It(World); It; ++It)
	{
		if (IsBuildingsActor(*It)) return *It;
	}
	return nullptr;
}

ACesium3DTileset* SpawnBuildingsTileset(UWorld* World, const FCamSimConfig& Cfg)
{
	if (!World || !CamSimScene::ShouldSpawnBuildings(Cfg)) return nullptr;
	if (ACesium3DTileset* Existing = FindBuildingsTileset(World)) return Existing;

	FActorSpawnParameters Params;
	Params.Name = TEXT("CamSimBuildings");
	Params.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
	Params.bDeferConstruction = true;
	ACesium3DTileset* T = World->SpawnActor<ACesium3DTileset>(ACesium3DTileset::StaticClass(), FTransform::Identity, Params);
	if (!T) return nullptr;
	T->Tags.Add(BuildingsTag);
	T->SetTilesetSource(ETilesetSource::FromUrl);
	T->SetUrl(Cfg.Scene.BuildingsUrl);
	TuneTileset(*T, Cfg);
	T->MaximumScreenSpaceError = Cfg.Scene.BuildingsMaximumScreenSpaceError;
	T->SetCreatePhysicsMeshes(Cfg.Scene.bBuildingsCreatePhysicsMeshes);
	T->FinishSpawning(FTransform::Identity);
	if (Cfg.Render.OriginShiftDistanceM > 0.0)
	{
		if (USceneComponent* Root = T->GetRootComponent()) Root->SetMobility(EComponentMobility::Movable);
	}
	UE_LOG(LogCamSim, Log, TEXT("CamSim: buildings tileset %s (SSE=%.1f physicsMeshes=%d)"),
		*Cfg.Scene.BuildingsUrl, Cfg.Scene.BuildingsMaximumScreenSpaceError, (int)Cfg.Scene.bBuildingsCreatePhysicsMeshes);
	return T;
}
}
```

If `ACesium3DTileset::MaximumScreenSpaceError` is not publicly writable in Cesium 2.29, use `T->SetMaximumScreenSpaceError(...)` (check `Cesium3DTileset.h`; `CesiumTuning.cpp` already assigns it directly, so direct assignment compiles).

`Geospatial/CesiumWorldSetup.cpp` — replace the final `return ::ApplyCesiumBackendConfig(...)` with:

```cpp
	UCesiumIonServer* Server = ::ApplyCesiumBackendConfig(World, Cfg.CesiumBackend);   // declared at global scope
	SpawnBuildingsTileset(World, Cfg);   // after the destroy loop, which would remove it (REALISM R3)
	return Server;
```

(`#include "Geospatial/SceneBuildings.h"`). Update the header comment of `SetUpCesiumWorld` in `CesiumWorldSetup.h`: "…every other tileset destroyed, then the scene package's buildings tileset spawned, in that order."

`PrepareCesiumWorld` already resets the tileset cache after `SetUpCesiumWorld`, so the lazy rebuild picks up the new actor: the terrain gate and culled-SSE scaling cover it with no change.

`Camera/CamSimCamera.cpp` (frame stats, ~l.262): keep `Sse` from the terrain:

```cpp
		if (ACesium3DTileset* T = Weak.Get())
		{
			MinLoad = FMath::Min(MinLoad, T->GetLoadProgress());  // already 0-100
			if (!CamSim::Geospatial::IsBuildingsActor(T))
			{
				Sse = T->GetMaximumScreenSpaceError();
			}
		}
```

(`#include "Geospatial/SceneBuildings.h"`).

- [ ] **Step 4: Build and run the tests**

Run: `scripts/run.sh --build-only`; automation filter `CamSim.Scene+CamSim.CesiumBackend`.
Expected: all pass (3 new; `CamSim.CesiumBackend.SetUpBeforeBeginPlay` unchanged — no buildings URL there). If Cesium logs an error while loading the empty test tileset, fix the JSON (it must be a valid 1.1 tileset), don't suppress it.

- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Geospatial unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/SceneBuildingsTest.cpp
git commit -m "feat(scene): spawn the package's buildings tileset after the terrain set-up (REALISM R3)"
```

---

### Task 9: Material code 230 for buildings

**Files:**
- Modify: `Thermal/LandCoverLookup.h/.cpp` (`QueryMaterialCode`), `CIGI/CigiQueryHandler.cpp` (two call sites, ~l.174 and ~l.458)
- Test: `Tests/LandCoverLookupTest.cpp` (`CamSim.CIGI.MaterialCode.Rule`)

**Interfaces:**
- Consumes: `CamSim::Geospatial::IsBuildingsActor` (Task 8).
- Produces: `CamSimLandCover::BuildingCode = 230`; `QueryMaterialCode(bool bValid, bool bWater, bool bEntityHit, bool bBuildingHit, double LatDeg, double LonDeg, FLandCoverCodeLookup* Lookup)`. Rule order: invalid → 0; entity → 0; water → 80; building → 230; else land cover.

- [ ] **Step 1: Update the test** — replace the body's calls in `FLandCoverQueryMaterialTest::RunTest` with the new signature and add building cases:

```cpp
	TestEqual(TEXT("invalid"), QueryMaterialCode(false, false, false, false, 33.32, -117.48, &L), 0u);
	TestEqual(TEXT("sea surface"), QueryMaterialCode(true, true, false, false, 33.32, -117.48, &L), 80u);
	TestEqual(TEXT("entity"), QueryMaterialCode(true, false, true, false, 33.32, -117.48, &L), 0u);
	TestEqual(TEXT("terrain"), QueryMaterialCode(true, false, false, false, 33.32, -117.48, &L), 200u);
	TestEqual(TEXT("no lookup"), QueryMaterialCode(true, false, false, false, 33.32, -117.48, nullptr), 0u);
	TestEqual(TEXT("sea without lookup"), QueryMaterialCode(true, true, false, false, 33.32, -117.48, nullptr), 80u);
	TestEqual(TEXT("building"), QueryMaterialCode(true, false, false, true, 33.32, -117.48, &L), 230u);
	TestEqual(TEXT("building without lookup"), QueryMaterialCode(true, false, false, true, 33.32, -117.48, nullptr), 230u);
	TestEqual(TEXT("sea above a roof wins"), QueryMaterialCode(true, true, false, true, 33.32, -117.48, &L), 80u);
	TestEqual(TEXT("invalid building"), QueryMaterialCode(false, false, false, true, 33.32, -117.48, &L), 0u);
```

- [ ] **Step 2: Build; it fails to compile** (too many arguments).

- [ ] **Step 3: Implement**

`LandCoverLookup.h`:

```cpp
	inline constexpr uint8 SeaSurfaceCode = 80;   // WorldCover water
	inline constexpr uint8 BuildingCode = 230;    // REALISM R3: the scene package's buildings tileset
	/** REALISM R2/R3 rule for CIGI HAT/HOT and LOS extended material codes. Lookup may be null (land cover off). */
	CAMSIMTEST_API uint32 QueryMaterialCode(bool bValid, bool bWater, bool bEntityHit, bool bBuildingHit,
		double LatDeg, double LonDeg, FLandCoverCodeLookup* Lookup);
```

`LandCoverLookup.cpp`:

```cpp
uint32 CamSimLandCover::QueryMaterialCode(bool bValid, bool bWater, bool bEntityHit, bool bBuildingHit,
	double LatDeg, double LonDeg, FLandCoverCodeLookup* Lookup)
{
	if (!bValid || bEntityHit) return 0;
	if (bWater) return SeaSurfaceCode;
	if (bBuildingHit) return BuildingCode;
	return Lookup ? Lookup->Code(LatDeg, LonDeg) : 0;
}
```

`CigiQueryHandler.cpp` (`#include "Geospatial/SceneBuildings.h"`): HAT/HOT call:

```cpp
			const bool bBuildingHit = bHit && CamSim::Geospatial::IsBuildingsActor(HitResult.GetActor());
			const uint32 Material = CamSimLandCover::QueryMaterialCode(bValid, R.bWater, /*bEntityHit=*/false,
				bBuildingHit, Req.Lat, Req.Lon, Subsystem ? Subsystem->GetLandCoverLookup() : nullptr);   // REALISM R2/R3
```

LOS call:

```cpp
	Resp.Material = CamSimLandCover::QueryMaterialCode(Hit != nullptr, /*bWater=*/false,
		Hit && Cast<ACamSimEntity>(Hit->GetActor()) != nullptr,
		Hit && CamSim::Geospatial::IsBuildingsActor(Hit->GetActor()),
		Lat, Lon, Subsystem->GetLandCoverLookup());   // REALISM R2/R3
```

`grep -rn "QueryMaterialCode(" unreal_project/CamSimTest/Source` must show no other caller left on the old signature.

- [ ] **Step 4: Build and run the tests** — filter `CamSim.CIGI+CamSim.Thermal.LandCover`. Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Thermal/LandCoverLookup.h unreal_project/CamSimTest/Source/CamSimTest/Thermal/LandCoverLookup.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/CIGI/CigiQueryHandler.cpp unreal_project/CamSimTest/Source/CamSimTest/Tests/LandCoverLookupTest.cpp
git commit -m "feat(cigi): material code 230 for hits on the buildings tileset (REALISM R3)"
```

---

### Task 10: The DIS surface clamp ignores buildings

**Files:**
- Modify: `Entity/SurfaceProbe.h/.cpp`
- Test: `Tests/SurfaceClampTest.cpp`

**Interfaces:**
- Consumes: `FindBuildingsTileset`, `IsBuildingsActor` (Task 8).
- Produces: `FCollisionQueryParams CamSimSurface::MakeProbeQueryParams(const AActor* IgnoredBuildings)` (complex trace, buildings ignored when non-null); `FCesiumSurfaceProbe` resolves the buildings tileset once per world (cached weak pointer, re-resolved if it becomes invalid).

- [ ] **Step 1: Write the failing test** — append to `Tests/SurfaceClampTest.cpp`:

```cpp
#include "Cesium3DTileset.h"
#include "CesiumGeoreference.h"
#include "Engine/Engine.h"
#include "GameFramework/WorldSettings.h"
#include "Geospatial/SceneBuildings.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfaceProbeIgnoresBuildingsTest,
	"CamSim.Entity.SurfaceProbe.IgnoresBuildings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSurfaceProbeIgnoresBuildingsTest::RunTest(const FString& Parameters)
{
	const FCollisionQueryParams None = CamSimSurface::MakeProbeQueryParams(nullptr);
	TestTrue(TEXT("complex"), None.bTraceComplex);
	TestEqual(TEXT("nothing ignored"), None.GetIgnoredActors().Num(), 0);

	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
	AddExpectedMessage(TEXT("No game viewport was found"), EAutomationExpectedErrorFlags::Contains, 0);
	World->GetWorldSettings()->bEnableWorldBoundsChecks = false;
	World->SpawnActor<ACesiumGeoreference>();
	ACesium3DTileset* B = World->SpawnActorDeferred<ACesium3DTileset>(ACesium3DTileset::StaticClass(), FTransform::Identity);
	B->SetTilesetSource(ETilesetSource::FromEllipsoid);
	B->Tags.Add(CamSim::Geospatial::BuildingsTag);
	B->FinishSpawning(FTransform::Identity);

	TestTrue(TEXT("found"), CamSim::Geospatial::FindBuildingsTileset(World) == B);
	const FCollisionQueryParams P = CamSimSurface::MakeProbeQueryParams(B);
	TestTrue(TEXT("buildings ignored"), P.GetIgnoredActors().Contains(B->GetUniqueID()));
	return true;
}
```

- [ ] **Step 2: Build; it fails to compile** (`MakeProbeQueryParams` missing).

- [ ] **Step 3: Implement**

`SurfaceProbe.h`: in `namespace CamSimSurface` add

```cpp
	/** The probe's trace parameters: complex collision, and the scene package's buildings tileset (REALISM R3)
	 *  ignored, so ground vehicles clamp to terrain (bridges, when they come, are a separate tileset). */
	FCollisionQueryParams MakeProbeQueryParams(const AActor* IgnoredBuildings);
```

(`#include "CollisionQueryParams.h"`), and in `FCesiumSurfaceProbe` add `mutable TWeakObjectPtr<AActor> Buildings; mutable bool bLookedUpBuildings = false;`. Update the class comment: "Traces the rendered Cesium tiles except the buildings tileset (needs create_physics_meshes); other actors are ignored."

`SurfaceProbe.cpp` (`#include "Geospatial/SceneBuildings.h"`):

```cpp
FCollisionQueryParams CamSimSurface::MakeProbeQueryParams(const AActor* IgnoredBuildings)
{
	FCollisionQueryParams Params(SCENE_QUERY_STAT(CamSimSurfaceProbe), /*bTraceComplex=*/true);
	if (IgnoredBuildings)
	{
		Params.AddIgnoredActor(IgnoredBuildings);
	}
	return Params;
}
```

In `TraceHeight`, replace the `FCollisionQueryParams Params(...)` line with:

```cpp
	if (!bLookedUpBuildings || (Buildings.IsStale()))
	{
		Buildings = CamSim::Geospatial::FindBuildingsTileset(W);
		bLookedUpBuildings = true;
	}
	const FCollisionQueryParams Params = CamSimSurface::MakeProbeQueryParams(Buildings.Get());
```

`IsStale()` is true only for a pointer that was set and whose actor died, so a package without buildings looks up once.

- [ ] **Step 4: Build and run the tests** — filter `CamSim.Entity+CamSim.DIS`. Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Entity/SurfaceProbe.h unreal_project/CamSimTest/Source/CamSimTest/Entity/SurfaceProbe.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/SurfaceClampTest.cpp
git commit -m "feat(dis): the surface clamp ignores the buildings tileset (REALISM R3)"
```

---

### Task 11: Docs and config reference

**Files:**
- Modify: `docs/configuration.md` ("Scene Package" + env quick reference), `docs/terrain-feedback.md` ("Material codes"), `docs/scene-packages.md` (new "Buildings (R3)" section, `scene.toml reference`, "Using a package in CamSim"), `deploy/camsim_config.yaml` (`scene:`), `scripts/scene/examples/pendleton.toml`, `scripts/scene/README.md` (if it lists layers/sources), `CLAUDE.md`

- [ ] **Step 1: `deploy/camsim_config.yaml`** — under `scene:`:

```yaml
  buildings:                          # REALISM R3: the package's features/buildings/ (no-op without one)
    enabled: true                     # CAMSIM_SCENE_BUILDINGS_ENABLED
    maximum_screen_space_error: 16    # CAMSIM_SCENE_BUILDINGS_MAXIMUM_SCREEN_SPACE_ERROR (set from the dense_pass sweep)
    create_physics_meshes: true       # CAMSIM_SCENE_BUILDINGS_CREATE_PHYSICS_MESHES (CIGI LOS/HAT/HOT hit buildings)
```

- [ ] **Step 2: `docs/configuration.md`** — in "Scene Package": add the three keys to the YAML block and table (key, env, default, description as in Global Constraints); add a row `features/buildings/tileset.json` → "one extra Cesium tileset spawned at world init (REALISM R3); `/ready` waits for it; CIGI LOS/HAT/HOT hit it (material 230); the DIS clamp ignores it"; under `scene.offline`, note the buildings URL must be an existing `file:///`. Add the three env vars to the quick reference.

- [ ] **Step 3: `docs/terrain-feedback.md`** — rule list: insert after rule 3 "4. The hit is the scene package's buildings tileset: **230**." and renumber; table row `| 230 | building (scene package features/buildings, REALISM R3) | Overture Maps (scene package [buildings]) |`; limits: "A building in thermal IR still takes the land-cover class under it; only CIGI reports 230."

- [ ] **Step 4: `docs/scene-packages.md`** — new section "Buildings (R3)" covering: `[buildings]` keys (`enabled`, `release`, `min_area_m2`) and the ODbL allow entry; the snapshot (fetch cache, 60-day releases, rebuild without network); height and base rules; tiers and geometric errors; GLB layout (ENU per tile, y-up, `EXT_mesh_features`/`EXT_structural_metadata` schema `camsim_buildings`); verify checks; runtime behaviour; Known limits (spec's list). Add `[buildings]` to the `scene.toml` reference.

- [ ] **Step 5: `scripts/scene/examples/pendleton.toml`** — add after `[features]`:

```toml
[buildings]                # REALISM R3: Overture footprints -> features/buildings/ (3D Tiles 1.1); ODbL, in allow above
release = "2026-09-23.1"
```

- [ ] **Step 6: `CLAUDE.md`** — Gotchas: change "One terrain tileset" to say "one terrain tileset plus, with a scene package that has `features/buildings/`, the buildings tileset spawned after the destroy loop (`Geospatial/SceneBuildings.h`, tag `CamSimBuildings`): CIGI hits it (material 230), the DIS clamp ignores it, `/ready` waits for it". Commands table: none new. Test counts are updated in Task 12.

- [ ] **Step 7: Commit**

```bash
git add docs/configuration.md docs/terrain-feedback.md docs/scene-packages.md deploy/camsim_config.yaml \
  scripts/scene/examples/pendleton.toml CLAUDE.md scripts/scene/README.md
git commit -m "docs(realism): buildings layer, scene.buildings config, material code 230 (REALISM R3)"
```

---

### Task 12: Acceptance on Pendleton and status

**Files:**
- Create: `scripts/scene/tools/footprint_iou.py`, `scripts/scene/tests/test_footprint_iou.py`, `scripts/scene/tools/data/pendleton_building_truth.geojson` (human-traced; see Step 3)
- Modify: `docs/scene-packages.md` ("Measured"), `ROADMAP.md`, `REALISM.md` (R3 status), `CLAUDE.md` (test counts), `FOR_LINUX.md` (Linux items)

**Interfaces:**
- Consumes: everything above; the bench plan's `scripts/bench/sweep.py` and `dense_pass` (if that plan has not landed yet, run Steps 1–7 and leave Step 8 open, recorded as such).
- Produces: `footprint_iou.py --snapshot PARQUET --truth GEOJSON` → prints per-building IoU and the median; exits 0 always (a report, the gate is read by a human).

- [ ] **Step 1: IoU tool, test first** — `scripts/scene/tests/test_footprint_iou.py`:

```python
import json
import sys
from pathlib import Path

import shapely
from overture_fixture import box_lonlat, write_overture_parquet

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import footprint_iou  # noqa: E402


def test_iou_matches_best_overlap(tmp_path):
    snap = write_overture_parquet(tmp_path / "s.parquet", [
        {"id": "a", "geom": box_lonlat(10.0, 10.0, 20, 20)},
        {"id": "b", "geom": box_lonlat(10.01, 10.0, 20, 20)},
    ])
    truth = box_lonlat(10.0, 10.0, 20, 10)  # half of a
    (tmp_path / "t.geojson").write_text(json.dumps({"type": "FeatureCollection", "features": [
        {"type": "Feature", "properties": {"name": "t1"}, "geometry": shapely.geometry.mapping(truth)}]}))
    rows = footprint_iou.ious(snap, tmp_path / "t.geojson")
    assert [r["match"] for r in rows] == ["a"] and abs(rows[0]["iou"] - 0.5) < 1e-6
```

`scripts/scene/tools/footprint_iou.py`:

```python
"""Acceptance gate 2 (REALISM R3 buildings): IoU of hand-traced NAIP outlines (GeoJSON, lon/lat) against the Overture
snapshot's footprints. Each outline is matched to the footprint it overlaps most. Prints one line per outline and the
median; the gate (median >= 0.6) is read by a human."""

from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path

import shapely
from shapely.geometry import shape

from camsim_scene.buildings import read_snapshot


def ious(snapshot: Path, truth: Path) -> list[dict]:
    fps = read_snapshot(snapshot)
    tree = shapely.STRtree([shapely.make_valid(f.geometry) for f in fps])
    out = []
    for feat in json.loads(Path(truth).read_text())["features"]:
        t = shapely.make_valid(shape(feat["geometry"]))
        best, match = 0.0, None
        for i in tree.query(t, predicate="intersects"):
            g = tree.geometries[i]
            iou = t.intersection(g).area / t.union(g).area
            if iou > best:
                best, match = iou, fps[i].source_id
        out.append({"name": feat.get("properties", {}).get("name", ""), "match": match, "iou": best})
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--snapshot", type=Path, required=True)
    ap.add_argument("--truth", type=Path, required=True)
    a = ap.parse_args()
    rows = ious(a.snapshot, a.truth)
    for r in rows:
        print(f"{r['name']:<24} {r['match'] or '-':<40} {r['iou']:.3f}")
    print(f"median IoU {statistics.median(r['iou'] for r in rows):.3f} over {len(rows)} outlines")


if __name__ == "__main__":
    main()
```

Run: `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_footprint_iou.py -q` → PASS.

- [ ] **Step 2: Build the Pendleton package** (clone R2's to skip terrain/imagery, as R2 did):

```bash
cp -c -R /path/to/pendleton-r2 /path/to/pendleton-r3        # APFS clone; Linux: cp -a --reflink=auto
uv run --project scripts/scene camsim-scene plan scripts/scene/examples/pendleton.toml /path/to/pendleton-r3
uv run --project scripts/scene camsim-scene build /path/to/pendleton-r3
uv run --project scripts/scene camsim-scene verify --deep /path/to/pendleton-r3
(cd scripts/scene/tests/validator && npm ci && npx 3d-tiles-validator --tilesetFile /path/to/pendleton-r3/features/buildings/tileset.json --reportFile /tmp/r3-validator.json)
```
(Use the exact CLI forms in `docs/scene-packages.md` "Quick start" if they differ.) Expected: `verify --deep` all ok; validator `numErrors` 0; `build.json` `buildings` stats. Rebuild into a second directory and compare `hashes.txt` (identical). Build the R2 toml without `[buildings]` and check `features/` is absent and `hashes.txt` equals R2's. Record building count, height-source mix, tiles per level, layer size and build time.

- [ ] **Step 3: Footprint IoU (human-assisted)** — a person traces ~30 building outlines from NAIP (Mainside, family housing, a hangar) into `scripts/scene/tools/data/pendleton_building_truth.geojson` (QGIS over the package imagery). Then:

```bash
uv run --project scripts/scene python scripts/scene/tools/footprint_iou.py \
  --snapshot "$(ls ~/.cache/... )" --truth scripts/scene/tools/data/pendleton_building_truth.geojson
```
(the snapshot path is `<cache>/blobs/sha256/<ab>/<sha>` for the sha in the package manifest's `overture_buildings` asset). Report the median (target ≥ 0.6).

- [ ] **Step 4: CIGI checks** — run CamSim on the package (`CAMSIM_SCENE_DIR=/path/to/pendleton-r3 scripts/run.sh --log`), pick a Mainside building from the snapshot (record its id, centroid, roof height from `verify` decode or `build.json`), then:

```bash
# HAT on the roof, and 5 m outside the footprint
uv run scripts/send_cigi_test.py --lat <roof lat> --lon <roof lon> --alt 300 --pitch -90 \
  --hat-extended <roof lat>,<roof lon> --hat-extended <outside lat>,<outside lon>
# LOS through the building at 3 m above its base, sensor 30 m west of the wall
uv run scripts/send_cigi_test.py --lat <west lat> --lon <west lon> --alt <base+3> --pitch 0 \
  --los-extended <east lat>,<east lon>,<base+3>
```
Expected: roof HOT within 0.5 m of the roof height, material 230; outside HOT = ground, material = land-cover code; LOS hits within 1 m of the west wall with material 230.

- [ ] **Step 5: DIS check** — `uv run scripts/send_dis_test.py truck-loop --location <Mainside road lat,lon beside buildings>` while logging entity altitude; expected no tick-to-tick jump > 0.5 m and the truck on the road. Then `uv run scripts/dis_vehicle_check.py` and `uv run scripts/gt_occlusion_check.py` (their usual arguments; on the SF scene as today) → pass.

- [ ] **Step 6: Off switch** — the same CIGI queries and the bench reference shots with `CAMSIM_SCENE_BUILDINGS_ENABLED=0` on `pendleton-r3` vs `pendleton-r2`: identical responses (material codes and HOT) and shots within run-to-run SSIM.

- [ ] **Step 7: Full test suites**

```bash
uv run --project scripts/scene --with pytest pytest scripts/scene/tests -q
# headless UE automation with filter CamSim (all), then:
scripts/run_gpu_tests.sh
```
Expected: all pass; update `CLAUDE.md`'s test counts ("N tests across M files") from the report.

- [ ] **Step 8: Bench sweep** — per the bench plan (`scripts/bench/sweep.py --site pendleton --scene /path/to/pendleton-r3 --axis buildings=off,on --axis buildings_sse=8,16,32,64 --axis buildings_physics=on,off --axis trees=off`; `--axis trees=off` is required: R4's trees default on by now, and without it the `buildings-off` row would be treated as the all-off baseline while still carrying trees). Pick `maximum_screen_space_error` with the user from `sweep.md` and the contact sheet. If the pick differs from 16, change the default everywhere in this step: `FSceneConfig::BuildingsMaximumScreenSpaceError` in `Config/CamSimConfig.h`, `deploy/camsim_config.yaml`, `docs/configuration.md`, and the `default sse` assertion in the Task 7 test `CamSim.Scene.Buildings.Config` (`Tests/ScenePackageTest.cpp`); rebuild and rerun `CamSim.Scene` so it passes.

- [ ] **Step 9: Visual review** — send the user EO shots from `dense_pass` and a low oblique over Mainside; record sign-off (or the requested changes) in `docs/scene-packages.md`.

- [ ] **Step 10: Status docs** — `docs/scene-packages.md` "Measured": a "R3 buildings slice" table (gates 1–8 with results). `REALISM.md` R3: status line (slice done, measured, what is deferred). `ROADMAP.md`: R3 slice entry; no editor work needed for the slice (note: facade/roof materials remain the R3 deepening pass's human editor work). `FOR_LINUX.md`: buildings on Linux/Vulkan (spawn, LOS, bench at 1/2/4 instances).

- [ ] **Step 11: Commit**

```bash
git add scripts/scene/tools/footprint_iou.py scripts/scene/tests/test_footprint_iou.py scripts/scene/tools/data \
  docs/scene-packages.md ROADMAP.md REALISM.md CLAUDE.md FOR_LINUX.md \
  unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.h unreal_project/CamSimTest/Source/CamSimTest/Tests/ScenePackageTest.cpp \
  deploy/camsim_config.yaml docs/configuration.md
git commit -m "docs(realism): R3 buildings slice measured on Pendleton"
```

---

## Self-Review

**Spec coverage.** §1 source/snapshot/licence → Tasks 1–2. §2 config → Task 2. §3 clean/height/base/mesh/colours → Tasks 3–4. §4 layout, tiers, ADD refinement, regions, geometric error, GLB ENU/y-up, metadata, deterministic writer → Tasks 3, 5. §5 verify + validator → Task 6. §6 resolution, config keys, spawn order, tuning, `/ready` + culled SSE (via the cache, no change needed), offline validation, material 230, clamp ignore, thermal unchanged → Tasks 7–10. Testing section → tests in Tasks 1–10. Acceptance 1–8 → Task 12 (bench in Step 8). Known limits → Task 11 (docs). Spec's `UCamSimSubsystem::GetBuildingsTileset()` is replaced by tag lookup helpers (listed below).

**Placeholders.** Paths to the user's Pendleton packages and the snapshot blob are environment-specific and named as such; the validator version is chosen by a command in Task 6 Step 1 and pinned. The traced truth GeoJSON is human work (spec: "hand-checked").

**Type consistency.** `Layer.BUILDINGS`, `snapshot_url`, `fetch_blob`, `Cache.put_file`/`lookup`, `Footprint`, `Building`, `plan_buildings(footprints, heights_fn, min_area_m2)`, `tile_mesh(buildings, origin)`, `tile_origin`, `tileset_json`, `walk_tileset`, `plan_from_manifest(m, ctx, st=None)`, `build(pkg, m, ctx) -> LayerStats`, `plan_stats(pkg) -> dict | None`, `QueryMaterialCode(bValid, bWater, bEntityHit, bBuildingHit, Lat, Lon, Lookup)`, `MakeProbeQueryParams`, `SpawnBuildingsTileset`, `FindBuildingsTileset`, `IsBuildingsActor`, `BuildingsTag`, `ShouldSpawnBuildings`, `TuneTileset` are used with the same names and signatures throughout.

**Spec gaps / decisions taken here (for the reviewer):**
1. Spec §6 names `UCamSimSubsystem::GetBuildingsTileset()`; the plan uses an actor tag with `IsBuildingsActor`/`FindBuildingsTileset` instead, because the surface probe holds only a `UWorld` and the query handler only needs the hit actor's identity.
2. Spec §6 says the spawn happens in `ApplyCesiumBackendConfig`; that function receives only `FCesiumBackendConfig`, so the plan spawns in `SetUpCesiumWorld` right after it (same moment, full config available).
3. Spec §5 "per-building roof heights match … (exact)": positions are float32 relative to the tile origin, so the deep check uses 0.01 m.
4. Spec §4 "z15 = 6 m, leaves 0": z15 nodes are always leaves here; the plan writes 6 m as listed (it never gates a refinement).
5. 3D Tiles node transforms compose down the tree, so a content node's `transform` is inv(parent absolute) × its own ENU→ECEF; content-less nodes have none.
6. The frame-stats `Sse` field would otherwise report the buildings tileset's SSE (last cached tileset); Task 8 keeps it on the terrain.
7. `layer_credit`'s ODbL notice hard-codes "land-cover layer"; Task 5 parameterises the layer name.
