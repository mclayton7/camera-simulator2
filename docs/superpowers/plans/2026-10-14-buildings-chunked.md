# Buildings Layer by Chunk + Outside Draw Distance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the buildings layer one 5 km chunk at a time (every process under the 2048 MB RSS gate on `socal-r5`,
package bytes unchanged), and draw buildings outside the detail polygons only near the camera (game thread back to
`pendleton-r5veg`'s).

**Architecture:** One parallel pass assigns every snapshot row to the z12 chunk of its cleaned centroid and DuckDB
writes one Parquet per chunk. The pool then plans each chunk (`plan_buildings`, unchanged), rasterises its footprints
(OR-merged per land-cover tile on disk) and writes its GLBs; the main process writes `tileset.json` from per-tile
summaries, grouping z13 nodes outside the `detail_margin:*` regions under content-less ancestors with geometric error
150.

**Tech Stack:** Python 3.12, DuckDB (no pyarrow/pandas: `fetchall`, CSV keys), shapely 2, numpy, rasterio,
`engine.run_pool` (spawn pool), pytest + pytest-xdist.

**Spec:** `docs/superpowers/specs/2026-10-14-buildings-chunked-design.md`

## Global Constraints

- The RSS gate stays at 2048 MB (`build_scene(max_worker_rss_mb=2048.0)`); never raise it.
- Package bytes do not change except `tileset.json` of a package with `[detail]` polygons. GLBs,
  `features/buildings/mask/`, `landcover/` are byte-identical; `tileset.json` of a package without `[detail]` is
  byte-identical. `BUILDINGS_RULES_VERSION` is not bumped.
- `CHUNK_ZOOM = 12` (z12 tile of `tiling.tile_size_deg`, 0.0439°).
- `OUTSIDE_GEOMETRIC_ERROR = 150`; root geometric error stays 200.
- `TILESET_RULES_VERSION = "1"` goes into the buildings layer inputs hash only (not land cover).
- DuckDB in the partition step: `memory_limit = '1GB'`, `temp_directory` under `.state/`.
- Every command uses `TMPDIR=/opt/mac/tmp`. Test command (from repo root):
  `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests -n auto -q`
- One commit per task on branch `buildings-chunked`; trailer:
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_01TPvVBPPLAKNsjK5cEiaGJT`. No push.
- Only one CamSim at a time; no CPU-heavy work while a bench runs.

## Review Focus

1. **Duplicate ids across a row-range boundary** (the first non-null row of an id wins, as `read_snapshot`'s
   `seen` set) — pinned in Task 3 (`test_partition_dedups_across_range_boundary`).
2. **A building whose raw bbox and cleaned centroid fall in different chunks** (make_valid moves the centroid): chunk
   must follow the cleaned centroid, or a GLB would need two chunks — pinned in Task 3
   (`test_chunk_follows_cleaned_centroid`).
3. **A crash mid-build** (partition without `partition.json`, chunk plans partly written, raster index missing): the
   next build redoes only what is incomplete and the bytes are identical — pinned in Task 4
   (`test_partition_without_marker_is_redone`, `test_stale_chunk_plan_is_replanned_alone`).
4. **A package with zero kept buildings or an empty chunk list** (all small): no tileset, no mask, no crash in the pool
   with zero work items — existing `test_zero_kept_buildings_writes_no_tileset_and_skips` must stay green (Task 5).
5. **`verify --deep` on a package whose `.state/` was removed** (packed or copied package): it re-partitions into a temp
   dir and passes — pinned in Task 6 (`test_deep_passes_without_state`).

---

## File Structure

- `scripts/scene/camsim_scene/buildings.py` — pure: add `stage`, `CHUNK_ZOOM`, `chunk_of`, `tile_summary`,
  `OUTSIDE_GEOMETRIC_ERROR`; `tileset_json(tiles, near_only=None)` takes `(region, origin)` per tile.
- `scripts/scene/camsim_scene/footprints.py` — raster as a directory of per-tile `.npy` + `index.json`:
  `save_raster(raster, out_dir, key)`, `or_tile(out_dir, ij, packed)`, `write_raster_index(out_dir, key)`,
  `FootprintRaster(out_dir)`; remove `load_raster`.
- `scripts/scene/camsim_scene/buildings_chunks.py` (new) — the partition: `partition_keys` (worker),
  `partition(...)`, `read_partition`, `read_chunk`.
- `scripts/scene/camsim_scene/layers/buildings.py` — orchestration: `init_worker`, `plan_chunks`, `footprint_raster`,
  `build(pkg, m, ctx, jobs)`, `near_only_fn(m)`.
- `scripts/scene/camsim_scene/pipeline.py` — pass `jobs` to `_build_landcover` / `buildings_layer.build`; the cut reads
  `FootprintRaster`.
- `scripts/scene/camsim_scene/verify.py` — `_check_buildings_heights` by chunk.
- Tests: `tests/test_buildings.py`, `tests/test_footprints.py`, `tests/test_buildings_chunks.py` (new),
  `tests/test_buildings_layer.py`, `tests/test_landcover_buildings.py`, `tests/test_buildings_verify.py`.
- Docs: `docs/scene-packages.md`, `ROADMAP.md`, `REALISM.md` (R5 open items), `CLAUDE.md` (Buildings gotcha line).

---

### Task 1: Pure building helpers and the tileset from summaries + draw distance

**Files:**
- Modify: `scripts/scene/camsim_scene/buildings.py` (`plan_buildings`, `tileset_json`; add helpers)
- Modify: `scripts/scene/camsim_scene/layers/buildings.py:207-225` (call `tile_summary`)
- Test: `scripts/scene/tests/test_buildings.py`

**Interfaces:**
- Produces:
  - `B.CHUNK_ZOOM: int = 12`, `B.OUTSIDE_GEOMETRIC_ERROR: float = 150.0`
  - `B.stage(fp: Footprint, min_area_m2: float) -> str | tuple[list, float, float, float]` — `"invalid"`, `"small"`,
    or `(parts, cx, cy, area_m2)`
  - `B.chunk_of(cx: float, cy: float) -> tuple[int, int]` = `tile_of(cx, cy, CHUNK_ZOOM)`
  - `B.tile_summary(key: tuple[int,int,int], blds: list[Building]) -> tuple[list[float], tuple[float,float,float]]`
    = `(_region(blds), tile_origin(*key, blds))`
  - `B.tileset_json(tiles: dict[key, (region, origin)], near_only: Callable[[list[float]], bool] | None = None) -> dict`

- [ ] **Step 1: Write the failing tests** (append to `tests/test_buildings.py`; change the `_tiles` helper)

Replace the `_tiles` helper:

```python
def _tiles(blds):
    by = {}
    for b in blds:
        by.setdefault(b.tile, []).append(b)
    return {k: B.tile_summary(k, v) for k, v in by.items()}
```

and in `test_tileset_tree_tiers_errors_and_transforms` change the last loop to:

```python
    for (z, x, y), (region, origin) in _tiles(blds).items():
        assert np.allclose(walked[f"{z}/{x}/{y}.glb"], geodesy.enu_to_ecef_matrix(*origin), atol=1e-3, rtol=0)
```

Append:

```python
def test_stage_matches_plan_and_reports_drops():
    good = B.Footprint("g", box_lonlat(10.0, 10.0, 20, 20), None, None, "")
    small = B.Footprint("s", box_lonlat(10.0, 10.0, 2, 2), None, None, "")
    line = B.Footprint("l", shapely.LineString([(10, 10), (10.001, 10)]), None, None, "")
    parts, cx, cy, area = B.stage(good, 10.0)
    assert len(parts) == 1 and abs(cx - 10.0) < 1e-9 and abs(cy - 10.0) < 1e-9 and 390 < area < 410
    assert B.stage(small, 10.0) == "small" and B.stage(line, 10.0) == "invalid"
    assert B.chunk_of(cx, cy) == B.tile_of(cx, cy, B.CHUNK_ZOOM)


def test_building_tiles_nest_in_their_chunk():
    for lon, lat in ((10.0, 10.0), (-117.4, 33.3), (0.0219, -0.0001)):
        cx, cy = B.chunk_of(lon, lat)
        for z in (13, 14, 15):
            x, y = B.tile_of(lon, lat, z)
            assert (x >> (z - B.CHUNK_ZOOM), y >> (z - B.CHUNK_ZOOM)) == (cx, cy)


def _two_far_tiles():
    a = B.Footprint("osm:a", box_lonlat(10.0, 10.0, 50, 50), 20.0, None, "")  # z13
    b = B.Footprint("osm:b", box_lonlat(10.5, 10.5, 50, 50), 20.0, None, "")  # z13, ~70 km away
    small = B.Footprint("osm:c", box_lonlat(10.5005, 10.5005, 10, 10), 5.0, None, "")  # z15 under b's z13
    blds, _ = B.plan_buildings([a, b, small], flat(0.0), 10.0)
    return blds


def test_tileset_without_near_only_is_unchanged_by_the_argument():
    tiles = _tiles(_two_far_tiles())
    assert B.tileset_json(tiles) == B.tileset_json(tiles, near_only=None)
    assert all(c["geometricError"] == 60.0 for c in B.tileset_json(tiles)["root"]["children"])


def test_tileset_outside_nodes_sit_under_contentless_ancestors():
    blds = _two_far_tiles()
    tiles = _tiles(blds)
    lon_a = np.radians(10.25)  # a (lon 10.0) is inside, b (lon 10.5) outside
    doc = B.tileset_json(tiles, near_only=lambda r: r[0] > lon_a)
    root = doc["root"]
    assert root["geometricError"] == 200.0 and doc["geometricError"] == 200.0
    inside = [c for c in root["children"] if c["geometricError"] == 60.0]
    outer = [c for c in root["children"] if c["geometricError"] == B.OUTSIDE_GEOMETRIC_ERROR]
    assert len(inside) == 1 and inside[0]["content"]["uri"].startswith("13/") and len(outer) == 1
    node, depth = outer[0], 0
    while node["geometricError"] == B.OUTSIDE_GEOMETRIC_ERROR:
        assert "content" not in node and "transform" not in node and len(node["children"]) == 1
        node, depth = node["children"][0], depth + 1
    assert depth == 4 and node["content"]["uri"].startswith("13/")  # z9, z10, z11, z12 ancestors
    assert dict(B.walk_tileset(doc)).keys() == dict(B.walk_tileset(B.tileset_json(tiles))).keys()
    for (u1, m1), (u2, m2) in zip(sorted(B.walk_tileset(doc), key=lambda t: t[0]),
                                  sorted(B.walk_tileset(B.tileset_json(tiles)), key=lambda t: t[0])):
        assert u1 == u2 and np.array_equal(m1, m2)
    r = outer[0]["boundingVolume"]["region"]
    child = node["boundingVolume"]["region"]
    assert r[0] <= child[0] and r[2] >= child[2] and r[4] <= child[4] and r[5] >= child[5]
```

(`shapely`, `box_lonlat`, `flat` are already imported in `test_buildings.py`; add `import shapely` if missing.)

- [ ] **Step 2: Run to verify they fail**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests/test_buildings.py -q`
Expected: FAIL — `AttributeError: module 'camsim_scene.buildings' has no attribute 'tile_summary'` (and `stage`).

- [ ] **Step 3: Implement in `buildings.py`**

Constants after `LEAF_ZOOM = 15`:

```python
CHUNK_ZOOM = 12  # build chunk (z12 tile, ~4.9 x 4.1 km at 33 N): every z13-z15 building tile nests in one
OUTSIDE_GEOMETRIC_ERROR = 150.0  # ancestors of z13 nodes outside the detail margins: ~2 km draw distance at SSE 64
```

Extract `stage` and use it in `plan_buildings`:

```python
def stage(fp: Footprint, min_area_m2: float):
    """"invalid", "small", or (parts, centroid lon, centroid lat, area m2): the per-footprint half of plan_buildings,
    shared with the chunk partition so a building's chunk is the chunk of the centroid it is planned with."""
    if fp.geometry is None or fp.geometry.is_empty or fp.geometry.geom_type not in (
        "Polygon", "MultiPolygon", "GeometryCollection"
    ):
        return "invalid"
    parts = clean(fp.geometry, min_area_m2)
    if not parts:
        return "small"
    c = shapely.MultiPolygon(parts).centroid
    return parts, c.x, c.y, sum(_area_m2(p, c.y) for p in parts)


def chunk_of(cx: float, cy: float) -> tuple[int, int]:
    return tile_of(cx, cy, CHUNK_ZOOM)
```

In `plan_buildings`, replace the staging loop body with:

```python
    for fp in footprints:
        s = stage(fp, min_area_m2)
        if s == "invalid":
            stats["dropped_invalid"] += 1
        elif s == "small":
            stats["dropped_small"] += 1
        else:
            staged.append((fp, *s))
```

Add after `tile_origin`:

```python
def tile_summary(key: tuple[int, int, int], blds: list[Building]) -> tuple[list[float], tuple[float, float, float]]:
    """What tileset_json needs of a content tile: its bounding region (radians, m) and ENU origin."""
    return _region(blds), tile_origin(*key, blds)
```

Change `tileset_json`:

```python
def tileset_json(tiles: dict, near_only=None) -> dict:
    """tiles: (z, x, y) -> (region, ENU origin) (tile_summary). Every ancestor of a content tile down from z13 exists
    in the tree (content-less when it holds no building of its own tier). near_only(region) true: that z13 node is
    outside every detail margin and is grouped under content-less z12..z9 ancestors with OUTSIDE_GEOMETRIC_ERROR (no
    transform, so the kept nodes' relative transforms are unchanged), so Cesium skips it beyond ~2 km."""
```

and inside `node()` replace

```python
            blds, origin = tiles[key]
            ...
            regions.append(_region(blds))
```

with

```python
            region, origin = tiles[key]
            ...
            regions.append(region)
```

Replace the `top = [...]` / `root = {...}` lines with:

```python
    z0 = min(TIER_MIN_AREA_M2)
    top = [node(k, np.eye(4)) for k in sorted(k for k in nodes if k[0] == z0)]
    if near_only is not None:
        keys13 = sorted(k for k in nodes if k[0] == z0)
        kept, groups = [], {}
        for k, t in zip(keys13, top):
            if near_only(t["boundingVolume"]["region"]):
                groups.setdefault((k[1], k[2]), t)
            else:
                kept.append(t)
        for z in range(z0 - 1, z0 - 5, -1):  # z12 .. z9
            up: dict = {}
            for (x, y), t in sorted(groups.items()):
                up.setdefault((x // 2, y // 2), []).append(t)
            groups = {
                k: {"boundingVolume": {"region": _union([c["boundingVolume"]["region"] for c in v])},
                    "geometricError": OUTSIDE_GEOMETRIC_ERROR, "children": v}
                for k, v in up.items()
            }
        top = kept + [groups[k] for k in sorted(groups)]
    root = {
        "boundingVolume": {"region": _union([t["boundingVolume"]["region"] for t in top])},
        "geometricError": GEOMETRIC_ERROR["root"],
        "refine": "ADD",
        "children": top,
    }
```

In `layers/buildings.py` `build()`, replace `tiles = {k: (v, B.tile_origin(*k, v)) ...}` with

```python
    groups = dict(sorted(by.items()))
    tiles = {k: B.tile_summary(k, v) for k, v in groups.items()}
```

and the GLB loop with `for (z, x, y), bl in groups.items(): origin = tiles[(z, x, y)][1]; mesh, table = B.tile_mesh(bl, origin, roof, sampler, roof_stats)`.

- [ ] **Step 4: Run tests**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests -n auto -q`
Expected: all pass (656 + 4 new passed, 3 skipped, 1 xfailed).

- [ ] **Step 5: Commit**

```bash
git add scripts/scene/camsim_scene/buildings.py scripts/scene/camsim_scene/layers/buildings.py scripts/scene/tests/test_buildings.py
git commit -m "feat(scene): building stage/chunk helpers; tileset from summaries with an outside draw distance"
```

---

### Task 2: Footprint raster as a directory of tiles

**Files:**
- Modify: `scripts/scene/camsim_scene/footprints.py` (replace `save_raster` / `load_raster`)
- Modify: `scripts/scene/camsim_scene/pipeline.py:542-551` (`_cut_landcover`)
- Modify: `scripts/scene/camsim_scene/layers/buildings.py` (`footprint_raster`, `write_mask`, `RASTER_*` names)
- Test: `scripts/scene/tests/test_footprints.py`, `scripts/scene/tests/test_landcover_buildings.py`

**Interfaces:**
- Produces (in `footprints.py`):
  - `RASTER_INDEX = "index.json"`
  - `save_raster(raster: dict[(i,j), packed], out_dir: Path, key: str) -> None` (clears `out_dir`, writes every tile
    and the index)
  - `or_tile(out_dir: Path, ij: tuple[int,int], packed: np.ndarray) -> None` (OR into an existing tile file or create)
  - `write_raster_index(out_dir: Path, key: str) -> None` (lists the tile files present, sorted; written last)
  - `raster_key(out_dir: Path) -> str | None` (the index's key, None when absent)
  - `class FootprintRaster: __init__(out_dir)`, `keys() -> list[(i,j)]` sorted, `get(i, j) -> np.ndarray | None`
- `layers.buildings.footprint_raster(...)` returns `(dir: Path, key: str)` (a directory now).

- [ ] **Step 1: Write the failing tests**

In `tests/test_footprints.py` replace `test_raster_file_round_trip` with:

```python
def test_raster_dir_round_trip(tmp_path):
    r = F.raster_tiles([_bld("a", shapely.Polygon(_ring(1.25, 1.25, 3.25, 3.25)))])
    F.save_raster(r, tmp_path / "r", "k1")
    back = F.FootprintRaster(tmp_path / "r")
    assert back.keys() == list(r) and np.array_equal(back.get(I0, J0), r[(I0, J0)])
    assert back.get(I0 + 1, J0) is None and F.raster_key(tmp_path / "r") == "k1"
    F.save_raster({}, tmp_path / "e", "k2")
    assert F.FootprintRaster(tmp_path / "e").keys() == [] and F.raster_key(tmp_path / "e") == "k2"
    assert F.raster_key(tmp_path / "missing") is None


def test_or_tile_merge_is_order_independent(tmp_path):
    a = F.raster_tiles([_bld("a", shapely.Polygon(_ring(1.25, 1.25, 3.25, 3.25)))])[(I0, J0)]
    b = F.raster_tiles([_bld("b", shapely.Polygon(_ring(2.25, 2.25, 6.25, 6.25)))])[(I0, J0)]
    whole = F.raster_tiles([_bld("a", shapely.Polygon(_ring(1.25, 1.25, 3.25, 3.25))),
                            _bld("b", shapely.Polygon(_ring(2.25, 2.25, 6.25, 6.25)))])[(I0, J0)]
    for name, order in (("ab", (a, b)), ("ba", (b, a))):
        for packed in order:
            F.or_tile(tmp_path / name, (I0, J0), packed)
        F.write_raster_index(tmp_path / name, "k")
        assert np.array_equal(F.FootprintRaster(tmp_path / name).get(I0, J0), whole)
```

In `tests/test_landcover_buildings.py` `_with_buildings`, replace the npz lines:

```python
    rdir = tmp_path / "fp"
    F.save_raster({(200, 200): np.packbits(mask, axis=1)}, rdir, "k")
    ...
    def fake_raster(pkg_, m_, ctx_, wst=None, jobs=1):
        state["calls"] += 1
        return rdir, state["key"]
```

- [ ] **Step 2: Run to verify they fail**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests/test_footprints.py scripts/scene/tests/test_landcover_buildings.py -q`
Expected: FAIL — `AttributeError: ... has no attribute 'FootprintRaster'` / `'or_tile'`.

- [ ] **Step 3: Implement**

In `footprints.py` replace `save_raster` / `load_raster` with:

```python
RASTER_INDEX = "index.json"


def _tile_path(out_dir: Path, ij: tuple[int, int]) -> Path:
    return Path(out_dir) / f"{ij[0]}_{ij[1]}.npy"


def _npy_bytes(packed: np.ndarray) -> bytes:
    buf = io.BytesIO()
    np.save(buf, np.ascontiguousarray(packed, np.uint8))
    return buf.getvalue()


def or_tile(out_dir: Path, ij: tuple[int, int], packed: np.ndarray) -> None:
    """OR one tile's packed rows into out_dir (binary union: the result does not depend on the order)."""
    p = _tile_path(out_dir, ij)
    if p.exists():
        packed = np.bitwise_or(np.load(p), packed)
    atomic_write(p, _npy_bytes(packed))


def write_raster_index(out_dir: Path, key: str) -> None:
    """Written last: the directory is complete for `key` only when the index names it."""
    tiles = sorted(tuple(int(v) for v in p.stem.split("_")) for p in Path(out_dir).glob("*.npy"))
    atomic_write(Path(out_dir) / RASTER_INDEX, json.dumps({"inputs": key, "tiles": tiles}).encode())


def raster_key(out_dir: Path) -> str | None:
    p = Path(out_dir) / RASTER_INDEX
    return json.loads(p.read_text())["inputs"] if p.exists() else None


def save_raster(raster: dict[tuple[int, int], np.ndarray], out_dir: Path, key: str) -> None:
    out_dir = Path(out_dir)
    shutil.rmtree(out_dir, ignore_errors=True)
    out_dir.mkdir(parents=True)
    for ij, packed in sorted(raster.items()):
        atomic_write(_tile_path(out_dir, ij), _npy_bytes(packed))
    write_raster_index(out_dir, key)


class FootprintRaster:
    """A footprint raster directory (index.json + <i>_<j>.npy packed rows), read one tile at a time."""

    def __init__(self, out_dir: Path):
        self.dir = Path(out_dir)
        self._keys = [tuple(k) for k in json.loads((self.dir / RASTER_INDEX).read_text())["tiles"]]

    def keys(self) -> list[tuple[int, int]]:
        return list(self._keys)

    def get(self, i: int, j: int) -> np.ndarray | None:
        p = _tile_path(self.dir, (i, j))
        return np.load(p) if (i, j) in self._keys else None
```

(add `import shutil` at the top.)

In `layers/buildings.py`: `RASTER_DIR = "buildings-footprints"` (replaces `RASTER_MARKER, RASTER_FILE`);
`LEGACY_STATE = ("buildings-footprints.json", "buildings-footprints.npz")`. `footprint_raster` becomes (whole-box for
now; Task 4 makes it chunked):

```python
def footprint_raster(pkg: Path, m: Manifest, ctx: BuildContext, wst: WorkerState | None = None,
                     jobs: int = 1) -> tuple[Path, str]:
    wst = wst or WorkerState(ctx)
    key = inputs_hash(footprint_inputs(m, wst), F.MASK_RULES_VERSION)
    out = Path(pkg) / STATE_DIR / RASTER_DIR
    if F.raster_key(out) != key:
        blds, _ = plan_cached(pkg, m, ctx, wst)
        F.save_raster(F.raster_tiles(blds), out, key)
    for name in LEGACY_STATE:
        (Path(pkg) / STATE_DIR / name).unlink(missing_ok=True)
    return out, key
```

`write_mask(pkg, raster: F.FootprintRaster)` iterates `raster.keys()` and writes `raster.get(i, j)`; the call becomes
`write_mask(pkg, F.FootprintRaster(raster_dir))`. In `build()`'s "no buildings" branch, remove
`Path(pkg) / STATE_DIR / RASTER_DIR` with `shutil.rmtree(..., ignore_errors=True)` and unlink `LEGACY_STATE` too.

In `pipeline._cut_landcover` replace the raster lines:

```python
    raster = F.FootprintRaster(Path(footprints)) if footprints else None
    ...
        packed = raster.get(i, j) if raster is not None else None
        burn = F.burn_cells(F.unpack(packed)) if packed is not None else None
```

- [ ] **Step 4: Run tests**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests -n auto -q`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add -A scripts/scene
git commit -m "refactor(scene): footprint raster as a directory of tiles, read one tile at a time"
```

---

### Task 3: Snapshot partition into chunks

**Files:**
- Create: `scripts/scene/camsim_scene/buildings_chunks.py`
- Test: `scripts/scene/tests/test_buildings_chunks.py`

**Interfaces:**
- Consumes: `B.stage`, `B.chunk_of`, `B.Footprint`, `overture_buildings.connect`, `engine.run_pool`
- Produces:
  - `RANGE_ROWS = 100_000`; `PARTITION_MARKER = "partition.json"`
  - `partition_keys(item: tuple[str, int, int, float, str]) -> list[dict]` — worker; item =
    `(snapshot, start, stop, min_area_m2, keys_dir)`; writes `keys_dir/<start>.csv` (`row,cx,cy`), returns
    `[{"input": n, "dropped_invalid": n, "dropped_small": n, "chunks": [[cx, cy], ...], "rss_mb": f}]`
  - `partition(snapshot: Path, min_area_m2: float, out_dir: Path, key: str, jobs: int, range_rows: int = RANGE_ROWS)
    -> dict` — returns the marker dict `{"inputs", "counts": {input, dropped_invalid, dropped_small},
    "chunks": [[cx, cy], ...] sorted, "peak_rss_mb"}`; reuses an existing complete partition with the same key
  - `read_partition(out_dir: Path) -> dict | None`
  - `read_chunk(out_dir: Path, cx: int, cy: int) -> list[B.Footprint]` (snapshot row order)

- [ ] **Step 1: Write the failing tests** (`tests/test_buildings_chunks.py`)

```python
import duckdb
import shapely
from overture_fixture import box_lonlat, write_overture_parquet

from camsim_scene import buildings as B
from camsim_scene import buildings_chunks as C
from camsim_scene.tiling import tile_size_deg


def _snapshot(tmp_path, rows):
    src = write_overture_parquet(tmp_path / "src.parquet", rows)
    out = tmp_path / "snap.parquet"
    duckdb.connect().execute(
        f"COPY (SELECT id, geometry, height, num_floors, class, subtype, to_json(sources) AS sources "
        f"FROM read_parquet('{src}') ORDER BY id) TO '{out}' (FORMAT parquet)"
    )
    return out


def _edge_lon():
    cx, _ = B.chunk_of(10.2, 10.2)
    return (cx + 1) * tile_size_deg(B.CHUNK_ZOOM) - 180.0  # east edge of 10.2's chunk


def _rows():
    e = _edge_lon()
    return [
        {"id": "a1", "geom": box_lonlat(10.2, 10.2, 40, 30), "height": 9.0},
        {"id": "a2", "geom": box_lonlat(e - 3e-5, 10.2, 30, 20)},  # straddles the chunk edge, centroid west
        {"id": "a3", "geom": box_lonlat(e + 3e-5, 10.2, 30, 20)},  # straddles it, centroid east
        {"id": "a4", "geom": box_lonlat(10.3, 10.3, 2, 2)},  # small
        {"id": "a5", "geom": shapely.LineString([(10.2, 10.2), (10.201, 10.2)])},  # invalid
        {"id": "a6", "geom": box_lonlat(10.25, 10.25, 20, 20), "num_floors": 3},
        {"id": "a6", "geom": box_lonlat(10.26, 10.26, 20, 20)},  # duplicate id: the first row wins
    ]


def _reference(snap, min_area):
    """The whole-box path: read_snapshot + stage, chunk by centroid."""
    want, counts = {}, {"input": 0, "dropped_invalid": 0, "dropped_small": 0}
    for fp in B.read_snapshot(snap):
        counts["input"] += 1
        s = B.stage(fp, min_area)
        if isinstance(s, str):
            counts[f"dropped_{s}"] += 1
        else:
            want.setdefault(B.chunk_of(s[1], s[2]), []).append(fp.source_id)
    return want, counts


def test_partition_matches_whole_box_staging(tmp_path):
    snap = _snapshot(tmp_path, _rows())
    p = C.partition(snap, 10.0, tmp_path / "part", "k", jobs=1, range_rows=3)
    want, counts = _reference(snap, 10.0)
    assert p["counts"] == counts and p["inputs"] == "k"
    assert [tuple(c) for c in p["chunks"]] == sorted(want) and len(want) == 2
    for (cx, cy), ids in want.items():
        got = C.read_chunk(tmp_path / "part", cx, cy)
        assert [f.source_id for f in got] == ids
    [a6] = [f for c in want for f in C.read_chunk(tmp_path / "part", *c) if f.source_id == "a6"]
    assert a6.num_floors == 3  # first row of the duplicate id


def test_partition_dedups_across_range_boundary(tmp_path):
    snap = _snapshot(tmp_path, _rows())
    for n in (1, 2, 5, 6):  # the a6 pair split across ranges at n = 6 (rows 5 | 6), and kept together otherwise
        p = C.partition(snap, 10.0, tmp_path / f"p{n}", "k", jobs=1, range_rows=n)
        ids = sorted(f.source_id for c in p["chunks"] for f in C.read_chunk(tmp_path / f"p{n}", *c))
        assert ids == ["a1", "a2", "a3", "a6"] and p["counts"]["input"] == 6


def test_chunk_follows_cleaned_centroid(tmp_path):
    e = _edge_lon()
    # a bow-tie whose raw bbox centre is east of the edge, but make_valid keeps a big west triangle: centroid west
    bow = shapely.Polygon([(e - 6e-4, 10.2), (e + 2e-4, 10.2008), (e + 2e-4, 10.2), (e - 6e-4, 10.2008)])
    snap = _snapshot(tmp_path, [{"id": "bow", "geom": bow}])
    [s] = [B.stage(fp, 10.0) for fp in B.read_snapshot(snap)]
    p = C.partition(snap, 10.0, tmp_path / "part", "k", jobs=1)
    assert [tuple(c) for c in p["chunks"]] == [B.chunk_of(s[1], s[2])]


def test_partition_reuses_a_complete_partition_and_redoes_a_partial_one(tmp_path):
    snap = _snapshot(tmp_path, _rows())
    first = C.partition(snap, 10.0, tmp_path / "part", "k", jobs=1)
    marker = tmp_path / "part" / C.PARTITION_MARKER
    stamp = marker.stat().st_mtime_ns
    assert C.partition(snap, 10.0, tmp_path / "part", "k", jobs=1) == first and marker.stat().st_mtime_ns == stamp
    marker.unlink()  # crashed before the marker
    assert C.partition(snap, 10.0, tmp_path / "part", "k", jobs=1)["chunks"] == first["chunks"]
    assert C.partition(snap, 10.0, tmp_path / "part", "k2", jobs=1)["inputs"] == "k2"  # new key: redone
```

- [ ] **Step 2: Run to verify they fail**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests/test_buildings_chunks.py -q`
Expected: FAIL — `ModuleNotFoundError: No module named 'camsim_scene.buildings_chunks'`.

- [ ] **Step 3: Implement `buildings_chunks.py`**

```python
"""Building snapshot partition (spec docs/superpowers/specs/2026-10-14-buildings-chunked-design.md section 4.1): every
kept row goes to the z12 chunk (buildings.CHUNK_ZOOM) of the centroid it is planned with (buildings.stage), one
Parquet file per chunk, so planning, rasterising and GLB writing hold one chunk at a time."""

from __future__ import annotations

import json
import shutil
from pathlib import Path

import shapely

from . import buildings as B
from .engine import peak_rss_mb, run_pool
from .fsutil import atomic_write

RANGE_ROWS = 100_000
PARTITION_MARKER = "partition.json"
COLS = "id, geometry, height, num_floors, class"


def _q(text: str) -> str:
    return "'" + str(text).replace("'", "''") + "'"


def _connect():
    from .sources.overture_buildings import connect

    return connect()


def _already_kept(con, snapshot: str, start: int, sid: str) -> bool:
    """Whether an earlier row of `sid` (rows are sorted by id) had a geometry: read_snapshot kept that one."""
    if start == 0:
        return False
    n = con.execute(
        f"SELECT count(*) FROM read_parquet({_q(snapshot)}, file_row_number = true) "
        f"WHERE id = ? AND file_row_number < ? AND geometry IS NOT NULL", [sid, start]
    ).fetchone()[0]
    return n > 0


def partition_keys(item) -> list[dict]:
    snapshot, start, stop, min_area_m2, keys_dir = item
    con = _connect()
    try:
        rows = con.execute(
            f"SELECT file_row_number, {COLS} FROM read_parquet({_q(snapshot)}, file_row_number = true) "
            f"WHERE file_row_number >= ? AND file_row_number < ? ORDER BY file_row_number", [start, stop]
        ).fetchall()
        last = rows[0][1] if rows and _already_kept(con, snapshot, start, rows[0][1]) else None
    finally:
        con.close()
    counts = {"input": 0, "dropped_invalid": 0, "dropped_small": 0}
    lines, chunks = [], set()
    for row, sid, wkb, height, floors, cls in rows:
        if wkb is None or sid == last:
            continue
        last = sid
        counts["input"] += 1
        s = B.stage(B.Footprint(sid, shapely.from_wkb(bytes(wkb)), height, floors, cls or ""), min_area_m2)
        if isinstance(s, str):
            counts[f"dropped_{s}"] += 1
            continue
        cx, cy = B.chunk_of(s[1], s[2])
        chunks.add((cx, cy))
        lines.append(f"{row},{cx},{cy}\n")
    atomic_write(Path(keys_dir) / f"{start}.csv", "".join(lines).encode())
    return [{**counts, "chunks": sorted(chunks), "rss_mb": peak_rss_mb()}]


def read_partition(out_dir: Path) -> dict | None:
    p = Path(out_dir) / PARTITION_MARKER
    return json.loads(p.read_text()) if p.exists() else None


def _noop(*_):
    pass


def partition(snapshot: Path, min_area_m2: float, out_dir: Path, key: str, jobs: int,
              range_rows: int = RANGE_ROWS) -> dict:
    out_dir = Path(out_dir)
    prev = read_partition(out_dir)
    if prev and prev["inputs"] == key:
        return prev
    shutil.rmtree(out_dir, ignore_errors=True)
    keys_dir, chunk_dir, tmp = out_dir / "keys", out_dir / "chunks", out_dir / "duckdb-tmp"
    keys_dir.mkdir(parents=True)
    con = _connect()
    try:
        n = con.execute(f"SELECT count(*) FROM read_parquet({_q(snapshot)})").fetchone()[0]
    finally:
        con.close()
    items = [(str(snapshot), a, min(a + range_rows, n), float(min_area_m2), str(keys_dir))
             for a in range(0, n, range_rows)]
    counts = {"input": 0, "dropped_invalid": 0, "dropped_small": 0}
    chunks, peak = set(), peak_rss_mb()

    def on_result(r):
        nonlocal peak
        for k in counts:
            counts[k] += r[k]
        chunks.update(tuple(c) for c in r["chunks"])
        peak = max(peak, r["rss_mb"])

    run_pool(_batch_keys, [[[it] for it in items]], jobs, _noop, (), on_result)
    if chunks:
        con = _connect()
        try:
            con.execute("SET memory_limit = '1GB'")
            con.execute(f"SET temp_directory = {_q(tmp)}")
            con.execute(
                f"COPY (SELECT s.file_row_number AS row, s.id, s.geometry, s.height, s.num_floors, s.class, "
                f"k.cx, k.cy FROM read_parquet({_q(snapshot)}, file_row_number = true) s "
                f"JOIN read_csv({_q(keys_dir / '*.csv')}, header = false, "
                f"columns = {{'row': 'BIGINT', 'cx': 'INTEGER', 'cy': 'INTEGER'}}) k ON s.file_row_number = k.row) "
                f"TO {_q(chunk_dir)} (FORMAT parquet, PARTITION_BY (cx, cy))"
            )
        finally:
            con.close()
        peak = max(peak, peak_rss_mb())
    shutil.rmtree(keys_dir, ignore_errors=True)
    shutil.rmtree(tmp, ignore_errors=True)
    marker = {"inputs": key, "counts": counts, "chunks": [list(c) for c in sorted(chunks)], "peak_rss_mb": peak}
    atomic_write(out_dir / PARTITION_MARKER, json.dumps(marker).encode())
    return json.loads(json.dumps(marker))


def _batch_keys(batch) -> list[dict]:
    return [x for it in batch for x in partition_keys(it)]


def read_chunk(out_dir: Path, cx: int, cy: int) -> list[B.Footprint]:
    files = Path(out_dir) / "chunks" / f"cx={cx}" / f"cy={cy}" / "*.parquet"
    con = _connect()
    try:
        rows = con.execute(f"SELECT {COLS} FROM read_parquet({_q(files)}) ORDER BY row").fetchall()
    finally:
        con.close()
    return [B.Footprint(sid, shapely.from_wkb(bytes(wkb)), h, f, c or "") for sid, wkb, h, f, c in rows]
```

(`run_pool` needs module-level functions under spawn; it calls `fn(chunk)` and passes each item of the returned list to `on_result`.)

- [ ] **Step 4: Run tests**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests/test_buildings_chunks.py -q`
Expected: 4 passed. If `test_chunk_follows_cleaned_centroid` shows the bow-tie's raw bbox centre and cleaned centroid
in the same chunk, widen the east lobe (`e + 2e-4` → `e + 4e-4` with the west lobe at `e - 1.2e-3`) until the
assertion `B.chunk_of(bbox centre) != B.chunk_of(centroid)` (add it to the test) holds, then rerun.

- [ ] **Step 5: Commit**

```bash
git add scripts/scene/camsim_scene/buildings_chunks.py scripts/scene/tests/test_buildings_chunks.py
git commit -m "feat(scene): partition the buildings snapshot into z12 chunks"
```

---

### Task 4: Plan and rasterise by chunk (land cover path)

**Files:**
- Modify: `scripts/scene/camsim_scene/layers/buildings.py` (replace `plan_cached` / whole-box `footprint_raster`)
- Modify: `scripts/scene/camsim_scene/pipeline.py` (`_build_landcover(pkg, m, ctx, jobs)`, call sites)
- Test: `scripts/scene/tests/test_buildings_layer.py`, `scripts/scene/tests/test_footprints.py`

**Interfaces:**
- Consumes: `C.partition`, `C.read_chunk`, `B.plan_buildings`, `F.raster_tiles`, `F.or_tile`, `F.write_raster_index`
- Produces (in `layers/buildings.py`):
  - `STATE_SUB = Path("buildings")` under `.state/`: `partition/`, `plan/<cx>_<cy>.pkl` + `.json`, `plan.json`
  - `init_worker(ctx: BuildContext) -> None` (module global `_W = WorkerState(ctx)`)
  - `partition_inputs(m) -> str`: `inputs_hash(__version__, B.BUILDINGS_RULES_VERSION, snapshot sha256, repr(min_area))`
  - `ensure_partition(pkg, m, ctx, jobs, out_dir=None) -> dict`
  - `plan_chunks(pkg, m, ctx, jobs, wst=None) -> tuple[list[tuple[int,int]], dict, float]` — chunk list, plan stats
    (same keys and order as `plan_buildings`' stats), peak RSS MB
  - `load_chunk_plan(pkg, cx, cy) -> list[Building]`
  - `plan_chunk(st: WorkerState, m: Manifest, fps: list[Footprint]) -> tuple[list[Building], dict]`
  - `footprint_raster(pkg, m, ctx, wst=None, jobs=1) -> tuple[Path, str]` (chunked)

- [ ] **Step 1: Write the failing tests**

In `tests/test_footprints.py` replace `test_plan_runs_once_per_build` with:

```python
def test_each_chunk_is_planned_once_per_build(tmp_path, monkeypatch):
    calls = []
    real = B.plan_buildings
    monkeypatch.setattr(B, "plan_buildings", lambda fps, *a, **k: (calls.append(len(fps)), real(fps, *a, **k))[1])
    build_synthetic(tmp_path, _scene(tmp_path, ROWS))
    chunks = {B.chunk_of(*B.stage(B.Footprint(r["id"], r["geom"], None, None, ""), 10.0)[1:3]) for r in ROWS}
    assert len(calls) == len(chunks) and sum(calls) == len(ROWS)  # the GLBs and the raster share the chunk plans
```

Append to `tests/test_buildings_layer.py`:

```python
from test_buildings_chunks import _edge_lon

EDGE_ROWS = [
    *ROWS,
    {"id": "e1", "geom": box_lonlat(_edge_lon() - 3e-5, 10.2, 30, 20), "height": 7.0},
    {"id": "e2", "geom": box_lonlat(_edge_lon() + 3e-5, 10.2, 30, 20)},
    {"id": "e3", "geom": box_lonlat(10.2499, 10.2, 40, 40)},  # straddles the 10.25 land-cover tile edge
]


def _whole_box(pkg, cache):
    """Reference: the pre-chunk path (one plan of the whole snapshot)."""
    from camsim_scene import footprints as F
    from camsim_scene.context import WorkerState
    from camsim_scene.layers import buildings as layer
    from camsim_scene.pipeline import make_context

    m = Manifest.load(pkg / "manifest.json")
    ctx = make_context(pkg, m, cache)
    st = WorkerState(ctx)
    rec = m.source(m.layers["buildings"]["priorities"][0])
    blds, stats = B.plan_buildings(B.read_snapshot(ctx.asset_paths[f"{rec.id}/{rec.assets[0].id}"]),
                                   layer.heights_fn(st, m), float(m.layers["buildings"]["min_area_m2"]))
    return blds, stats, F.raster_tiles(blds)


def test_chunked_plan_and_raster_equal_the_whole_box(tmp_path):
    from camsim_scene import footprints as F
    from camsim_scene.layers import buildings as layer
    from camsim_scene.manifest import STATE_DIR

    pkg, cache, info = build_synthetic(tmp_path, _scene(tmp_path, EDGE_ROWS))
    blds, stats, raster = _whole_box(pkg, cache)
    chunks = json.loads((pkg / STATE_DIR / "buildings" / "partition" / "partition.json").read_text())["chunks"]
    assert len(chunks) >= 2
    got = sorted((b for c in chunks for b in layer.load_chunk_plan(pkg, *c)), key=lambda b: b.source_id)
    assert [(b.source_id, b.tile, b.base_m, b.roof_m, [p.wkb for p in b.parts]) for b in got] == \
           [(b.source_id, b.tile, b.base_m, b.roof_m, [p.wkb for p in b.parts]) for b in blds]
    assert {k: v for k, v in info["buildings"].items() if k != "roofs"} == stats
    fr = F.FootprintRaster(pkg / STATE_DIR / "buildings-footprints")
    assert fr.keys() == list(raster) and all(np.array_equal(fr.get(*k), raster[k]) for k in raster)


def test_partition_without_marker_is_redone(tmp_path):
    from camsim_scene.manifest import STATE_DIR

    pkg, cache, _ = build_synthetic(tmp_path, _scene(tmp_path, EDGE_ROWS))
    before = (pkg / "hashes.txt").read_bytes()
    (pkg / STATE_DIR / "buildings" / "partition" / "partition.json").unlink()
    (pkg / STATE_DIR / "buildings-footprints" / "index.json").unlink()
    (pkg / STATE_DIR / "landcover.json").unlink()  # force the land-cover cut, which needs the raster
    build_scene(pkg, cache, jobs=1)
    assert (pkg / "hashes.txt").read_bytes() == before


def test_stale_chunk_plan_is_replanned_alone(tmp_path, monkeypatch):
    from camsim_scene.manifest import STATE_DIR

    pkg, cache, _ = build_synthetic(tmp_path, _scene(tmp_path, EDGE_ROWS))
    plan_dir = pkg / STATE_DIR / "buildings" / "plan"
    victim = sorted(plan_dir.glob("*.json"))[0]
    victim.write_text(json.dumps({"inputs": "stale"}))
    (pkg / STATE_DIR / "buildings-footprints" / "index.json").unlink()
    (pkg / STATE_DIR / "landcover.json").unlink()
    calls = []
    real = B.plan_buildings
    monkeypatch.setattr(B, "plan_buildings", lambda fps, *a, **k: (calls.append(1), real(fps, *a, **k))[1])
    before = (pkg / "hashes.txt").read_bytes()
    build_scene(pkg, cache, jobs=1)
    assert len(calls) == 1 and (pkg / "hashes.txt").read_bytes() == before
```

- [ ] **Step 2: Run to verify they fail**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests/test_buildings_layer.py scripts/scene/tests/test_footprints.py -q`
Expected: FAIL — `AttributeError: ... has no attribute 'load_chunk_plan'` and a missing `buildings/partition/partition.json`.

- [ ] **Step 3: Implement in `layers/buildings.py`**

Remove `PLAN_MARKER, PLAN_FILE`, `plan_cached`. Add (`from .. import buildings_chunks as C`):

```python
STATE_SUB = Path("buildings")
LEGACY_STATE = ("buildings-footprints.json", "buildings-footprints.npz", "buildings-plan.json", "buildings-plan.pkl")
_W: WorkerState | None = None


def init_worker(ctx: BuildContext) -> None:
    global _W
    _W = WorkerState(ctx)


def _state(pkg: Path) -> Path:
    return Path(pkg) / STATE_DIR / STATE_SUB


def _snapshot(m: Manifest, ctx: BuildContext) -> Path:
    rec = m.source(m.layers["buildings"]["priorities"][0])
    return Path(ctx.asset_paths[f"{rec.id}/{rec.assets[0].id}"])


def partition_inputs(m: Manifest) -> str:
    s = m.layers["buildings"]
    rec = m.source(s["priorities"][0])
    return inputs_hash(__version__, B.BUILDINGS_RULES_VERSION, rec.assets[0].sha256, repr(float(s["min_area_m2"])))


def ensure_partition(pkg: Path, m: Manifest, ctx: BuildContext, jobs: int, out_dir: Path | None = None) -> dict:
    return C.partition(_snapshot(m, ctx), float(m.layers["buildings"]["min_area_m2"]),
                       out_dir or _state(pkg) / "partition", partition_inputs(m), jobs)


def plan_chunk(st: WorkerState, m: Manifest, fps: list) -> tuple[list, dict]:
    return B.plan_buildings(fps, heights_fn(st, m), float(m.layers["buildings"]["min_area_m2"]))


def _chunk_name(cx: int, cy: int) -> str:
    return f"{cx}_{cy}"


def _plan_one(item) -> list[dict]:
    pkg, cx, cy, key = item
    st = _W
    blds, stats = plan_chunk(st, st.manifest, C.read_chunk(_state(pkg) / "partition", cx, cy))
    d = _state(pkg) / "plan"
    atomic_write(d / f"{_chunk_name(cx, cy)}.pkl", pickle.dumps(blds, protocol=pickle.HIGHEST_PROTOCOL))
    rec = {"inputs": key, "kept": stats["kept"], "dropped_no_height": stats["dropped_no_height"]}
    atomic_write(d / f"{_chunk_name(cx, cy)}.json", json.dumps(rec).encode())
    return [{"chunk": [cx, cy], **rec, "rss_mb": peak_rss_mb()}]


def plan_batch(batch) -> list[dict]:
    return [r for it in batch for r in _plan_one(it)]


def load_chunk_plan(pkg: Path, cx: int, cy: int) -> list:
    return pickle.loads((_state(pkg) / "plan" / f"{_chunk_name(cx, cy)}.pkl").read_bytes())


def plan_chunks(pkg: Path, m: Manifest, ctx: BuildContext, jobs: int, wst: WorkerState | None = None):
    """Partition (cached), then plan every chunk whose cached plan is missing or stale, in the pool. Returns
    (chunks, stats, peak RSS MB); stats has plan_buildings' keys in its order."""
    wst = wst or WorkerState(ctx)
    part = ensure_partition(pkg, m, ctx, jobs)
    chunks = [tuple(c) for c in part["chunks"]]
    base = footprint_inputs(m, wst)
    d = _state(pkg) / "plan"
    d.mkdir(parents=True, exist_ok=True)
    keep = {f"{_chunk_name(*c)}{ext}" for c in chunks for ext in (".pkl", ".json")}
    for p in d.iterdir():
        if p.name not in keep:
            p.unlink()
    todo, recs, peak = [], {}, part["peak_rss_mb"]
    for c in chunks:
        key = inputs_hash(base, _chunk_name(*c))
        mk, pk = d / f"{_chunk_name(*c)}.json", d / f"{_chunk_name(*c)}.pkl"
        rec = json.loads(mk.read_text()) if mk.exists() else None
        if rec and rec.get("inputs") == key and pk.exists():
            recs[c] = rec
        else:
            todo.append((str(pkg), *c, key))

    def on_result(r):  # results arrive in completion order with jobs > 1: key them by chunk
        nonlocal peak
        recs[tuple(r["chunk"])] = r
        peak = max(peak, r["rss_mb"])

    run_pool(plan_batch, [[[t] for t in todo]], jobs, init_worker, (ctx,), on_result)
    counts = part["counts"]
    stats = {"input": counts["input"], "kept": sum(r["kept"] for r in recs.values()),
             "dropped_small": counts["dropped_small"], "dropped_invalid": counts["dropped_invalid"],
             "dropped_no_height": sum(r["dropped_no_height"] for r in recs.values())}
    atomic_write(_state(pkg) / "plan.json", json.dumps({"inputs": base, "stats": stats}).encode())
    return chunks, stats, peak
```

Raster:

```python
RASTER_DIR = "buildings-footprints"


def _raster_one(item) -> list[dict]:
    pkg, cx, cy = item
    return [{"tiles": F.raster_tiles(load_chunk_plan(pkg, cx, cy)), "rss_mb": peak_rss_mb()}]


def raster_batch(batch) -> list[dict]:
    return [r for it in batch for r in _raster_one(it)]


def footprint_raster(pkg: Path, m: Manifest, ctx: BuildContext, wst: WorkerState | None = None,
                     jobs: int = 1) -> tuple[Path, str, float]:
    wst = wst or WorkerState(ctx)
    key = inputs_hash(footprint_inputs(m, wst), F.MASK_RULES_VERSION)
    out = Path(pkg) / STATE_DIR / RASTER_DIR
    for name in LEGACY_STATE:
        (Path(pkg) / STATE_DIR / name).unlink(missing_ok=True)
    if F.raster_key(out) == key:
        return out, key, 0.0
    chunks, _, peak = plan_chunks(pkg, m, ctx, jobs, wst)
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)

    def on_result(r):
        nonlocal peak
        for ij, packed in r["tiles"].items():
            F.or_tile(out, ij, packed)
        peak = max(peak, r["rss_mb"])

    run_pool(raster_batch, [[[(str(pkg), *c)] for c in chunks]], jobs, _noop, (), on_result)
    F.write_raster_index(out, key)
    return out, key, peak
```

with `def _noop(*_): pass`. **Return type change:** `footprint_raster` now returns `(dir, key, peak_rss_mb)`; update
the Interfaces of callers: `pipeline._build_landcover` does
`fp_path, fp_key, fp_peak = buildings_layer.footprint_raster(pkg, m, ctx, jobs=jobs)` and after the cut sets
`st.peak_rss_mb = max(st.peak_rss_mb, fp_peak)`; the test fake in `test_landcover_buildings.py` returns
`rdir, state["key"], 0.0`.

In `build()` (still writing GLBs in the main process until Task 5): replace `blds, stats = plan_cached(...)` with

```python
    chunks, stats, plan_peak = plan_chunks(pkg, m, ctx, jobs, wst)
    blds = [b for c in chunks for b in load_chunk_plan(pkg, *c)]
```

`build(pkg, m, ctx, jobs: int = 1)`; the mask call becomes
`raster_dir, _, _ = footprint_raster(pkg, m, ctx, wst, jobs)`. The "no buildings" branch also removes `_state(pkg)`
(`shutil.rmtree(..., ignore_errors=True)`) and `LEGACY_STATE`. `st.peak_rss_mb = max(peak_rss_mb(), plan_peak)`.

In `pipeline.py`: `_build_landcover(pkg, m, ctx, jobs: int = 1)`; `build_scene` calls
`_build_landcover(pkg, m, ctx, jobs)` and `buildings_layer.build(pkg, m, ctx, jobs)`.

- [ ] **Step 4: Run tests**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests -n auto -q`
Expected: all pass (the existing identity tests `test_rebuild_is_identical_and_skips`,
`test_rebuild_from_manifest_after_release_is_gone`, `test_rebuild_with_textures_and_burn_is_identical` included).

- [ ] **Step 5: Commit**

```bash
git add -A scripts/scene
git commit -m "feat(scene): plan and rasterise buildings by chunk in the pool (R5 build memory)"
```

---

### Task 5: GLBs by chunk, tileset from summaries, draw distance from the manifest

**Files:**
- Modify: `scripts/scene/camsim_scene/layers/buildings.py` (`build`)
- Test: `scripts/scene/tests/test_buildings_layer.py`, `scripts/scene/tests/test_detail_build.py`

**Interfaces:**
- Consumes: `plan_chunks`, `load_chunk_plan`, `init_worker`, `B.tile_summary`, `B.tileset_json(tiles, near_only)`
- Produces:
  - `TILESET_RULES_VERSION = "1"`
  - `near_only_fn(m: Manifest) -> Callable[[list[float]], bool] | None` — None without `detail_margin:*` regions
  - `glb_batch(batch) -> list[dict]` (worker; per chunk `{"tiles": {key: (region, origin)}, "roofs": dict,
    "rss_mb": f}`)

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_buildings_layer.py`:

```python
def test_chunked_glbs_and_tileset_equal_the_whole_box(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path, _scene(tmp_path, EDGE_ROWS))
    blds, _, _ = _whole_box(pkg, cache)
    by = {}
    for b in blds:
        by.setdefault(b.tile, []).append(b)
    tiles = {k: B.tile_summary(k, v) for k, v in sorted(by.items())}
    from camsim_scene.manifest import canonical_json

    assert (pkg / "features/buildings/tileset.json").read_text() == canonical_json(B.tileset_json(tiles))
    for (z, x, y), bl in by.items():
        mesh, table = B.tile_mesh(bl, tiles[(z, x, y)][1])
        got = (pkg / "features/buildings" / f"{z}/{x}/{y}.glb").read_bytes()
        assert gltf.decode_tile(got).table.source_ids == table.source_ids
        assert np.array_equal(gltf.decode_tile(got).positions, mesh.positions)


def test_near_only_from_detail_margins():
    from types import SimpleNamespace

    from camsim_scene.layers import buildings as layer

    box = [[10, 10], [11, 10], [11, 11], [10, 11], [10, 10]]
    small = [[10, 10], [10.1, 10], [10.1, 10.1], [10, 10.1], [10, 10]]
    assert layer.near_only_fn(SimpleNamespace(regions=[{"name": "bbox", "polygon": box}])) is None
    fn = layer.near_only_fn(SimpleNamespace(regions=[{"name": "bbox", "polygon": box},
                                                     {"name": "detail_margin:a", "polygon": small}]))
    inside = [np.radians(v) for v in (10.05, 10.05, 10.06, 10.06)] + [0.0, 10.0]
    outside = [np.radians(v) for v in (10.5, 10.5, 10.6, 10.6)] + [0.0, 10.0]
    assert fn(inside) is False and fn(outside) is True
```

In `tests/test_detail_build.py` (R5 detail packages; it builds a synthetic `[detail]` package), add:

```python
def test_detail_package_tileset_groups_outside_nodes(tmp_path):
    ...  # use the module's existing detail-package helper with buildings rows inside and outside the polygon
    doc = json.loads((pkg / "features/buildings/tileset.json").read_text())
    errs = sorted({c["geometricError"] for c in doc["root"]["children"]})
    assert errs == [60.0, 150.0]
```

Fill the `...` from the helper that `test_detail_build.py` already uses to build a detail package (read it first;
add the Overture rows the way `test_buildings_layer._scene` does: `scene["allow"] = ["ODbL-1.0"]`,
`scene["buildings"] = {}`, `scene["sources"]["overture_buildings"] = {"parquet": ...}` with one row inside the
detail polygon and one ≥ 0.2° outside).

- [ ] **Step 2: Run to verify they fail**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests/test_buildings_layer.py scripts/scene/tests/test_detail_build.py -q`
Expected: FAIL — `AttributeError: ... has no attribute 'near_only_fn'`; the detail tileset has only 60.0.

- [ ] **Step 3: Implement**

```python
TILESET_RULES_VERSION = "1"  # 1: z13 nodes outside the detail margins under ancestors (OUTSIDE_GEOMETRIC_ERROR)


def near_only_fn(m: Manifest):
    margins = [shapely.Polygon(r["polygon"], r.get("holes") or None) for r in m.regions
               if r["name"].startswith("detail_margin:")]
    if not margins:
        return None
    union = shapely.union_all(margins)

    def near_only(region) -> bool:
        w, s, e, n = (math.degrees(v) for v in region[:4])
        return not union.intersects(shapely.box(w, s, e, n))

    return near_only


def _glb_one(item) -> list[dict]:
    pkg, cx, cy, credit = item
    st, m = _W, _W.manifest
    s = m.layers["buildings"]
    roof = RoofOptions.from_settings(s) if s.get("roof_textures") else None
    sampler = ImageryRoofSampler(st) if roof is not None else None
    by: dict = {}
    for b in load_chunk_plan(pkg, cx, cy):
        by.setdefault(b.tile, []).append(b)
    out, tiles, roofs = Path(pkg) / OUT, {}, {}
    for key, bl in sorted(by.items()):
        tiles[key] = B.tile_summary(key, bl)
        mesh, table = B.tile_mesh(bl, tiles[key][1], roof, sampler, roofs)
        z, x, y = key
        p = out / str(z) / str(x) / f"{y}.glb"
        p.parent.mkdir(parents=True, exist_ok=True)
        atomic_write(p, write_glb(mesh, table, credit))
    return [{"tiles": tiles, "roofs": roofs, "rss_mb": peak_rss_mb()}]


def glb_batch(batch) -> list[dict]:
    return [r for it in batch for r in _glb_one(it)]
```

In `build()`: add `TILESET_RULES_VERSION` to the first `inputs_hash(...)` call; then replace the GLB section with

```python
    shutil.rmtree(out, ignore_errors=True)
    chunks, stats, peak = plan_chunks(pkg, m, ctx, jobs, wst)
    tiles, roof_stats = {}, {}

    def on_result(r):
        nonlocal peak
        tiles.update(r["tiles"])
        for k, v in r["roofs"].items():
            if k == "tiles_by_texel_m":
                by = roof_stats.setdefault(k, {})
                for t, n in v.items():
                    by[t] = by.get(t, 0) + n
            else:
                roof_stats[k] = roof_stats.get(k, 0) + v
        peak = max(peak, r["rss_mb"])

    flat = credit.replace("\n", " ")
    run_pool(glb_batch, [[[(str(pkg), *c, flat)] for c in chunks]], jobs, init_worker, (ctx,), on_result)
    tiles = dict(sorted(tiles.items()))
```

and write the tileset with `B.tileset_json(tiles, near_only_fn(m))`. `roof_stats["tiles_by_texel_m"]` keys sorted
(`dict(sorted(...))`) before it goes into `stats` so the marker is order-independent. `st.peak_rss_mb =
max(peak_rss_mb(), peak, mask_peak)` where `raster_dir, _, mask_peak = footprint_raster(...)`.
Add `import math` and `import shapely` to the module.

- [ ] **Step 4: Run tests**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests -n auto -q`
Expected: all pass (including `test_zero_kept_buildings_writes_no_tileset_and_skips`: zero chunks → `run_pool` with an
empty phase → no tileset).

- [ ] **Step 5: Commit**

```bash
git add -A scripts/scene
git commit -m "feat(scene): write building GLBs by chunk in the pool; outside draw distance from the detail margins"
```

---

### Task 6: `verify --deep` by chunk

**Files:**
- Modify: `scripts/scene/camsim_scene/verify.py:368-395` (`_check_buildings_heights`), call at line 552
- Test: `scripts/scene/tests/test_buildings_verify.py`

**Interfaces:**
- Consumes: `layer.ensure_partition(pkg, m, ctx, jobs, out_dir)`, `C.read_partition`, `C.read_chunk`,
  `layer.plan_chunk(st, m, fps)`, `layer.partition_inputs(m)`, `B.CHUNK_ZOOM`
- Produces: `_check_buildings_heights(pkg, m, st, jobs=1) -> Check` (same check name and messages)

- [ ] **Step 1: Write the failing tests**

```python
def test_deep_passes_without_state(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path, _scene(tmp_path, ROWS))
    shutil.rmtree(pkg / ".state")
    c = _checks(verify(pkg, cache, deep=True))
    assert c["buildings_heights"]["ok"], c["buildings_heights"]
    assert not (pkg / ".state" / "buildings").exists()  # verify partitions in a temporary directory


def test_deep_never_reads_the_build_plan(tmp_path, monkeypatch):
    from camsim_scene.layers import buildings as layer

    pkg, cache, _ = build_synthetic(tmp_path, _scene(tmp_path, ROWS))
    monkeypatch.setattr(layer, "load_chunk_plan", lambda *a: (_ for _ in ()).throw(AssertionError("read plan")))
    assert _checks(verify(pkg, cache, deep=True))["buildings_heights"]["ok"]
```

and the one that pins the chunking (the two above are regressions that the whole-box check also passes):

```python
def test_deep_plans_chunk_by_chunk(tmp_path, monkeypatch):
    from camsim_scene import buildings as B
    from test_buildings_layer import EDGE_ROWS

    pkg, cache, _ = build_synthetic(tmp_path, _scene(tmp_path, EDGE_ROWS))
    chunks = json.loads((pkg / ".state/buildings/partition/partition.json").read_text())["chunks"]
    calls = []
    real = B.plan_buildings
    monkeypatch.setattr(B, "plan_buildings", lambda fps, *a, **k: (calls.append(len(fps)), real(fps, *a, **k))[1])
    assert _checks(verify(pkg, cache, deep=True))["buildings_heights"]["ok"]
    assert len(chunks) >= 2 and len(calls) == len(chunks)
```

- [ ] **Step 2: Run to verify they fail**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests/test_buildings_verify.py -q`
Expected: `test_deep_plans_chunk_by_chunk` FAILS (`len(calls) == 1`: the whole box planned once); the other two pass.

- [ ] **Step 3: Implement**

```python
def _check_buildings_heights(pkg: Path, m: Manifest, st, jobs: int = 1) -> Check:
    import tempfile

    from . import buildings_chunks as C
    from . import geodesy
    from .buildings import CHUNK_ZOOM, walk_tileset
    from .gltf import decode_tile
    from .layers import buildings as layer
    from .layers.buildings import OUT

    root = pkg / OUT
    by_chunk: dict = {}
    for uri, mat in walk_tileset(json.loads((root / "tileset.json").read_text())):
        z, x, y = (int(v) for v in uri[: -len(".glb")].split("/"))
        by_chunk.setdefault((x >> (z - CHUNK_ZOOM), y >> (z - CHUNK_ZOOM)), []).append((uri, (z, x, y), mat))
    problems: list[str] = []
    state_part = pkg / ".state" / "buildings" / "partition"
    prev = C.read_partition(state_part)
    with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as tmp:
        part_dir = state_part if prev and prev["inputs"] == layer.partition_inputs(m) else Path(tmp) / "partition"
        part = layer.ensure_partition(pkg, m, st.ctx, jobs, out_dir=part_dir)
        chunks = [tuple(c) for c in part["chunks"]]
        for c in sorted(set(chunks) | set(by_chunk)):
            want, _ = layer.plan_chunk(st, m, C.read_chunk(part_dir, *c)) if c in chunks else ([], {})
            by_id = {b.source_id: b for b in want}
            seen = set()
            for uri, key, mat in by_chunk.get(c, []):
                t = decode_tile((root / uri).read_bytes())
                enu = np.stack([t.positions[:, 0], -t.positions[:, 2], t.positions[:, 1]], axis=1).astype(np.float64)
                _, _, h = geodesy.ecef_to_geodetic((mat[:3, :3] @ enu.T).T + mat[:3, 3])
                for f, sid in enumerate(t.table.source_ids):
                    b = by_id.get(sid)
                    if b is None:
                        problems.append(f"{uri}: unexpected {sid}")
                        continue
                    seen.add(sid)
                    if b.tile != key:
                        problems.append(f"{sid}: in {uri}, expected {b.tile}")
                    roof = float(h[t.feature_ids == f].max())
                    if abs(roof - b.roof_m) > ROOF_TOL_M:
                        problems.append(f"{sid}: roof {roof:.3f} m, expected {b.roof_m:.3f} m")
            problems += [f"missing {sid}" for sid in sorted(set(by_id) - seen)]
    return Check("buildings_heights", not problems, _summary(problems))
```

(`import os` at the top of `verify.py` if missing.) The call becomes
`_guard("buildings_heights", _check_buildings_heights, pkg, m, st, jobs)`. Delete `plan_from_manifest` from
`layers/buildings.py` if nothing else uses it (`grep -rn plan_from_manifest scripts/scene`).

- [ ] **Step 4: Run tests**

Run: `TMPDIR=/opt/mac/tmp uv run --project scripts/scene pytest scripts/scene/tests -n auto -q`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add -A scripts/scene
git commit -m "feat(scene): verify --deep checks building heights chunk by chunk"
```

---

### Task 7: Measure on Pendleton and socal-r5; docs

No new code. Machine rules: one CamSim at a time; no other heavy work during the bench.

- [ ] **Step 1: Pendleton identity (no `[detail]`: every byte unchanged)**

```bash
export TMPDIR=/opt/mac/tmp SP=$PWD/.cache/scene-packages
cp $SP/pendleton-r5veg/hashes.txt /opt/mac/tmp/pendleton-hashes-before.txt
rm -rf $SP/pendleton-r5veg/features $SP/pendleton-r5veg/landcover \
  $SP/pendleton-r5veg/.state/buildings* $SP/pendleton-r5veg/.state/landcover.json
scripts/scene/build.sh build /out/pendleton-r5veg 2>&1 | tail -5   # the pinned container, as the R5 builds
diff /opt/mac/tmp/pendleton-hashes-before.txt $SP/pendleton-r5veg/hashes.txt && echo IDENTICAL
```

Expected: `IDENTICAL`; build exit 0. (If `build.sh` takes the package path differently, use the exact invocation from
`.cache/linux-r5/rebuild-pendleton.log`'s first lines.)

- [ ] **Step 2: socal-r5 rebuild of buildings + land cover**

```bash
cp $SP/socal-r5/hashes.txt /opt/mac/tmp/socal-hashes-before.txt
rm -rf $SP/socal-r5/features $SP/socal-r5/landcover $SP/socal-r5/.state/buildings* $SP/socal-r5/.state/landcover.json
/usr/bin/time -v scripts/scene/build.sh build /out/socal-r5 > .cache/linux-r5/rebuild-chunked.log 2>&1; echo exit=$?
diff /opt/mac/tmp/socal-hashes-before.txt $SP/socal-r5/hashes.txt
python3 -c "import json;d=json.load(open('$SP/socal-r5/build.json'));print({k:(v['peak_rss_mb'],v['seconds']) for k,v in d['layers'].items()})"
```

Expected: exit 0 (no RSS gate failure); the diff shows only `features/buildings/tileset.json`; buildings and land-cover
peak RSS < 2048 MB (target < 1024); record both layers' seconds.

- [ ] **Step 3: verify --deep on both**

```bash
uv run --project scripts/scene camsim-scene verify --deep $SP/pendleton-r5veg > .cache/linux-r5/verify-pendleton-chunked.log 2>&1; echo exit=$?
/usr/bin/time -v uv run --project scripts/scene camsim-scene verify --deep $SP/socal-r5 > .cache/linux-r5/verify-chunked.log 2>&1; echo exit=$?
```

Expected: both exit 0, every check ok; record socal's wall time and max RSS.

- [ ] **Step 4: Bench and shots**

```bash
uv run -q --with numpy --with pillow python scripts/bench/run_bench.py --site pendleton --label socal-r5-chunked \
  --out .cache/linux-r5/bench-chunked --gpu-mem --env CAMSIM_SCENE_DIR=$SP/socal-r5
uv run -q python scripts/bench/compare.py .cache/linux-r5/bench-pendleton-r5veg .cache/linux-r5/bench-chunked
```

Expected: game p50 within 0.3 ms of `pendleton-r5veg` in orbit (3.15) and dense_pass (6.09); 0 dropped. Look at
`.cache/linux-r5/bench-chunked/shots/slant_10km.png`: the camp (inside the polygon, ~10 km) shows 3D buildings as in
`.cache/linux-r5/spike/shots-socal-r5/shots/slant_10km.png`. If the bench misses the 0.3 ms target, record the
numbers as a FAIL (do not change the gate) and report.

- [ ] **Step 5: Docs**

- `docs/scene-packages.md` "Buildings (R3)": the build is by z12 chunk in the worker pool (`.state/buildings/`:
  `partition/`, `plan/`; footprint raster `.state/buildings-footprints/` one `.npy` per land-cover tile); with
  `[detail]`, z13 nodes outside every `detail_margin:*` sit under content-less z12–z9 ancestors with geometric error
  150 (~2 km at SSE 64). R5 section: replace the RSS FAIL and gate-7 FAIL lines with the new numbers (date
  2026-10-14, Linux, RTX 5080), keeping the old ones as "before".
- `ROADMAP.md`: mark the two R5 checkboxes `~~…~~ **Done 2026-10-14**` with the measured numbers (or record the FAIL).
- `REALISM.md` R5: the bench and RSS results.
- `CLAUDE.md` Buildings gotcha: add "built by z12 chunk in the pool; with `[detail]`, buildings outside the margins
  draw within ~2 km (`OUTSIDE_GEOMETRIC_ERROR`)".
- Remove the spike packages: `rm -rf "$SP/socal-r5-prune2k" "$SP/socal-r5-lod"`.

- [ ] **Step 6: Commit**

```bash
git add docs ROADMAP.md REALISM.md CLAUDE.md
git commit -m "docs(scene): buildings by chunk and the outside draw distance measured on socal-r5"
```
