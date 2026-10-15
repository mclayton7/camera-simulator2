# Buildings layer by chunk, and draw distance outside detail polygons (REALISM R5 follow-up): design

Date: 2026-10-14. Status: approved in chat (2026-10-14), written for review.

## 1. Problem

The R5 `socal-r5` build (200 km box, 5.79 M Overture buildings) exits 2 on the build's peak-RSS gate (2048 MB):

- **Main process: 15.8 GB.** `layers/buildings.plan_cached` reads the whole snapshot (`buildings.read_snapshot`),
  plans every building (`buildings.plan_buildings`) and pickles the result (`.state/buildings-plan.pkl`, 1.8 GB).
  `footprint_raster` rasterises every footprint into one dict (`.state/buildings-footprints.npz`, 3.2 GB), and the
  layer writes all 57,585 GLBs serially from the full list (3,257 s).
- **Land-cover cut process: 4.5 GB.** `_cut_landcover` loads the whole footprint raster (`F.load_raster`) to burn
  code 230.
- **`verify --deep`** (`_check_buildings_heights`) plans the whole box again in one process.

The gate stays at 2048 MB. The layer must fit under it at any box size.

## 2. Goal and invariant

- Every process that touches buildings holds one **chunk** at a time: peak RSS per process well under 2048 MB on
  `socal-r5` (target: under 1 GB).
- **Package bytes do not change** except `tileset.json` of a package with `[detail]` polygons (section 4.6). GLBs,
  `features/buildings/mask/` and `landcover/` are byte-identical to the current code's output for the same manifest,
  and so is `tileset.json` of a package without `[detail]`. `BUILDINGS_RULES_VERSION` is not bumped.
- **Bench (gate 7 of R5):** buildings outside the detail polygons are drawn only near the camera (section 4.6), so
  `socal-r5`'s game thread is back to `pendleton-r5veg`'s.
- The buildings work runs in the build's worker pool (`engine.run_pool`, `jobs` workers), so the layer gets faster
  too.

## 3. Chunks

A chunk is a z12 tile of the package's geographic tiling (`tiling.tile_size_deg(12)` = 0.0439°, about 4.9 × 4.1 km at
33° N): `CHUNK_ZOOM = 12` in `buildings.py`. A building belongs to the chunk holding its centroid (the centroid
`plan_buildings` already computes from the cleaned parts). Building tiles (z13–z15, assigned by the same centroid)
nest inside z12 tiles, so every GLB's buildings are in one chunk, and `plan_buildings`' height groups (z15 tiles) are
too: a chunk's plan is exactly the whole-box plan's buildings in that chunk.

The densest socal chunk is estimated at about 60 k buildings (about 200 MB planned).

## 4. Data flow

### 4.1 Partition (one streaming pass, own process)

`buildings.stage(fp, min_area_m2)` is extracted from `plan_buildings`: invalid / small / `(parts, cx, cy, area)`.
`plan_buildings` calls it, so the two cannot disagree.

`partition_snapshot(snapshot, min_area_m2, out_dir)`:

1. Reads the snapshot in record batches (DuckDB, `file_row_number`), in file order (the snapshot is written
   `ORDER BY id`). Duplicate ids are adjacent: the first row of each id is kept, as `read_snapshot` keeps the first
   seen. Rows with a null geometry are skipped, as now.
2. Stages each row, counting `input`, `dropped_invalid` and `dropped_small` as `plan_buildings` does, and writes
   `(file_row_number, chunk x, chunk y)` for kept rows to a keys file.
3. One DuckDB `COPY ... PARTITION_BY (cx, cy)` joins the snapshot to the keys on the row number: one Parquet file per
   chunk, rows in snapshot order. DuckDB runs with `memory_limit = '1GB'` and its temp directory under `.state/`.
4. Writes `partition.json` (the counts, the chunk list) last. It is the completeness marker, keyed by the inputs
   hash (`footprint_inputs` without the terrain parts: the snapshot sha256, `min_area_m2`, rules, tool version).

It lives at `.state/buildings/partition/`. A stale or incomplete partition is deleted and redone.

### 4.2 Plan (pool, per chunk)

Each worker reads its chunk's Parquet, rebuilds the `Footprint`s, and runs `plan_buildings` on them (heights from
`heights_fn` as now). It writes `.state/buildings/plan/<cx>_<cy>.pkl` (the chunk's buildings and its `dropped_no_height`
count) with a per-chunk marker keyed by `footprint_inputs` plus the chunk's Parquet sha256, so a crashed build replans
only the chunks it lacks. The plan stats are the partition counts plus the summed `dropped_no_height` and `kept`,
written to `.state/buildings-plan.json` (same shape as today).

### 4.3 Footprint raster (pool, per chunk; merge in the main process)

Each worker loads one chunk plan and runs `footprints.raster_tiles` on it: a dict of at most a few land-cover tiles
(0.05°), packed. The main process ORs each returned tile into `.state/buildings/footprints/<i>_<j>.npy` (read,
OR, atomic write). A cell is set when its centre is inside any footprint (`rasterise_tile`, `all_touched=False`), so
the OR of per-chunk rasters equals the whole-box raster bit for bit, in any order. Only tiles with a set cell exist.
`index.json` (the tile list and the existing raster inputs key) is written last as the completeness marker.

`footprints.load_raster` is replaced by a per-tile reader (`FootprintRaster(dir)`: `keys()`, `get(i, j)`), used by
the land-cover burn (`_cut_landcover`) and `write_mask`, so each loads one tile at a time.

### 4.4 GLBs (pool, per chunk)

Each worker loads one chunk plan, groups it by tile, and writes those GLBs exactly as the layer does now
(`B.tile_origin`, `B.tile_mesh`, roof sampler per worker). It returns, per tile, `(key, origin, region)` and its roof
stats. `B.tileset_json` takes `(region, origin)` per tile instead of the building list (`_region` moves to the
worker), so the main process writes `tileset.json` from the summaries alone.

The layer's skip-when-unchanged check (`.state/buildings.json`) and its outputs are unchanged. Tiles are not written
when the layer is unchanged; when it is rebuilt, `features/buildings/` is cleared first, as now.

### 4.5 `verify --deep`

`_check_buildings_heights` replans chunk by chunk and checks each chunk's GLBs against that fresh plan; it never reads
the build's plan pickles. It reuses the `.state/` partition when its marker matches (the partition only regroups the
snapshot's rows), else partitions into a temporary directory.
Every planned building must be in its expected tile, and every GLB building must be planned in some chunk, as now.

### 4.6 Draw distance outside the detail polygons (bench fix)

Measured 2026-10-14 on `socal-r5` (`--phases orbit,dense_pass`, game p50, orbit / dense): as built 6.81 / 9.74 ms;
buildings SSE 256: 6.82 / 9.65; no physics meshes: 6.22 / 9.02; tileset pruned to the polygon + 2 km: 3.23 / 6.12;
content-less z9–z12 ancestors over every z13 node with geometric error 1400 / 400 / 150: 7.98 / 10.85, 7.92 / 10.85,
3.10 / 6.23; `pendleton-r5veg` 3.15 / 6.09. GPU time unchanged throughout. The cost is the building tiles near the
camera in the towns around the polygon (every z13 tile is a child of the always-refined root, so all are selected).
Ancestors at 150 everywhere removed the camp's 3D buildings inside the polygon in `slant_10km` (about 10 km away), so
the cut-off applies only outside. The user accepted about 2 km as the draw distance outside the polygons (2026-10-14).

`B.tileset_json(tiles, near_only=None)`: `near_only(region) -> bool` says whether a z13 node (its bounding region in
radians) is outside every `detail_margin:*` region of the manifest. Nodes for which it is false stay children of the
root exactly as now. Nodes for which it is true are grouped under content-less z12 → z9 ancestors (no transform, so
the kept nodes' relative transforms are unchanged; region = union of the children), each with geometric error
`OUTSIDE_GEOMETRIC_ERROR = 150`, which are children of the root. The root's geometric error stays 200. A manifest
without `[detail]` passes `near_only=None`: the tileset is byte-identical to today's.

The buildings layer inputs hash gains `TILESET_RULES_VERSION = "1"` (in `layers/buildings.py`; not in the land-cover
inputs), so packages rebuild their tileset once; GLBs come out identical.

## 5. Memory and time accounting

- The layer's `LayerStats.peak_rss_mb` is the max over the workers and the main process (as the raster layers do), so
  the gate sees the chunk workers.
- The partition step runs in its own process (`_run_isolated`); its peak counts too.
- `build.json` keeps its fields.

## 6. Old `.state` files

`buildings-plan.pkl` and `buildings-footprints.npz` are removed on the next build. The land-cover inputs key and the
buildings layer inputs are unchanged, so a package already built is not rebuilt: only its `.state/` caches are
regenerated when a layer needs them.

## 7. Testing

- **Unit, chunked = whole:** a synthetic snapshot (Parquet, built in the test) with buildings straddling chunk edges, a
  building straddling a land-cover tile edge, duplicate ids, invalid and small footprints, and one without terrain
  height. The chunked path gives the same plan (as a set, by `source_id`), stats, raster tiles and GLB/tileset bytes
  as the current whole-box path, kept in the test as the reference.
- **Unit:** the raster OR merge is order-independent; a partition without `partition.json` is redone; a chunk plan
  with a stale marker is replanned and the others are skipped.
- **Pendleton identity:** rebuild `pendleton-r5veg`'s buildings and land cover from scratch with the new code;
  `hashes.txt` identical.
- **socal-r5:** rebuild buildings and land cover; `hashes.txt` identical to the current package; the build exits 0
  (no RSS gate failure); per-layer peak RSS and wall time recorded in `docs/scene-packages.md` and ROADMAP.
- **`verify --deep`** on both packages: ok, with its peak RSS recorded.

- **Draw distance:** unit test: without `near_only` the tileset is byte-identical to the current code; with it, nodes
  inside a margin region stay root children, outside ones sit under z12…z9 ancestors with error 150 and no transform,
  and `walk_tileset` returns the same (uri, matrix) pairs either way.
- **Bench on `socal-r5`:** `run_bench.py --site pendleton --phases orbit,dense_pass` game p50 within 0.3 ms of
  `pendleton-r5veg` (3.15 / 6.09 ms); full bench recorded against R5 gate 7. Reference shots: the camp in
  `slant_10km` keeps its 3D buildings (inside the polygon).

## 8. Out of scope

- Main-process RSS from other layers (terrain / imagery planning); the gate measures them as now.
