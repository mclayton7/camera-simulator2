# Scene packages: build tooling (REALISM R0) — design

Status: approved design (2026-10-07), not yet implemented. Roadmap: `REALISM.md` R0. Evidence:
`docs/realism-r0-spike.md` (spike), `docs/realism/` (data sources, Cesium findings, offline hosting).

User decisions (2026-10-07): R0 is **tooling only** (runtime pieces move to R1); designed for
40,000 km², proven on Camp Pendleton; a **pinned Linux build container** is the reference for
byte-identical builds; NAIP **2022 from Planetary Computer** for now, behind source adapters so other
datasets plug in; **per-tile Python engine** (not a GDAL command-line pipeline, not a workflow
framework); GDAL may be used where it helps.

## Goal

`camsim-scene build` turns a bounding box into a **scene package**: a self-contained directory of
quantized-mesh terrain, TMS imagery and land cover that covers the whole Earth (a coarse global base
under the detailed area), with a manifest that pins every input, an attribution file, and a
verification report. `camsim-scene pack` turns it into one SquashFS image for offline transfer.

Success (the R0 exit gates, "Testing" below):

1. A full Camp Pendleton package builds in the reference container (bbox W -117.62 S 33.19
   E -117.24 N 33.52, `sim` profile, 100 km ring, global base) and its time, peak memory, size and
   file count per layer are recorded.
2. A build killed at ~50 % resumes without rebuilding finished tiles.
3. A second clean build in the same container gives an identical `hashes.txt` and `.sqfs`.
4. `verify --deep` passes.
5. CamSim (with the spike's local TMS patch) renders it: < 1 px registration vs CWT + Bing,
   frame-centre heights within 0.25 m on 1 m data and 1 m on 10 m data, no holes or untextured tiles
   in a high oblique view, and a run with outbound network blocked works.
6. A 10,000 km² `preview` build is timed and sized.

## Non-goals (R0)

- Any CamSim runtime change: `imagery.source: tms`, an offline config profile, removing the ion
  actors from `Main.umap`, reading land cover from a package (all R1).
- NAIP 2024 (USDA image service / EarthExplorer adapters), range-read fetching, NAIP colour
  balancing across flight lines and states, an NIR/NDVI layer.
- Corridors derived from CIGI recordings (R5; the region model is ready for them).
- Buildings, roads, water rasters, vegetation (R2–R4). LiDAR point clouds (only the 1 m DEM product).

## Package format

```
<name>/                         # shipped as <name>.sqfs
  manifest.json                 # deterministic content description
  build.json                    # build-time facts only: UTC times, host, tool git commit, image digest, per-layer stats
  ATTRIBUTION.txt               # generated from the sources used
  hashes.txt                    # "sha256  path" per file except build.json and hashes.txt, sorted by path
  terrain/layer.json
  terrain/{z}/{x}/{y}.terrain   # quantized-mesh-1.0, gzip, octvertexnormals
  imagery/tilemapresource.xml
  imagery/{z}/{x}/{y}.jpg       # geodetic TMS, 256 px, JPEG q85 4:2:0
  landcover/index.json
  landcover/<lat>_<lon>.png     # today's 4B format (0.05° tiles, 600×600, 8-bit WorldCover codes)
```

- **One tile grid:** Cesium's geographic TMS (EPSG:4326 lon/lat in ITRF2014, 2 × 1 root tiles,
  tile size 180°/2^z, y from the south) for terrain and imagery.
- **Regions** set the maximum zoom per layer: `globe` (terrain and imagery z0–8), `ring` (bbox +
  `ring_km`, default 100: z9–10), `bbox` (terrain to the source limit, imagery to z17). A region is
  an ordered list of polygons with per-layer max zoom; the highest-detail region containing a tile
  wins. R5 corridors become another region type.
- **Terrain depth** per tile = min(region limit, best source's limit): z16 on 1 m data, z14 on
  1/3″, z8 on ETOPO.
- **Complete siblings:** if any child of a tile is built, all four are, in both layers (a sibling
  past its own source's limit is sampled from the coarser source). Cesium then never upsamples a
  missing child (the spike's NaN bounds), and every imagery parent has four children.
- **`manifest.json`**: `schema_version` (1), `name`, `bbox`, `seed`, `regions`, `tiling`,
  per-layer settings (zoom limits, terrain max-error rule, JPEG quality, priorities), `datum`
  (PROJ pipeline strings per source datum, coordinate epoch 2010.0, grid file names and sha256),
  `sources[]` (`id`, `adapter`, `dataset`, `version`/`date`, `licence`, `attribution`,
  `assets[]` of `{url, sha256, size, metadata}`), `hashes_sha256`. Nothing time- or host-dependent.
- **Reproducibility contract:** same `manifest.json` inputs + same build-image digest ⇒ every file
  except `build.json` byte-identical. Native (non-container) builds must pass `verify` but needn't
  match bytes.

## Architecture

```
scene.toml ─► plan ─► manifest (inputs frozen) ─► fetch ─► cache (content-addressed)
                                                     │
                     build: scheduler ─► tile jobs ──┤  layers: terrain | imagery | landcover
                                                     │    └─ sources (adapters) ─► datum ─► writers
                                                     ▼
                                   package dir ─► verify ─► pack (.sqfs)
```

### Module layout (`scripts/scene/`, uv project `camsim-scene`, Python 3.12–3.13)

| Module | Responsibility |
|---|---|
| `camsim_scene/cli.py` | `plan`, `fetch`, `build`, `verify`, `pack`, `attribution` (`build` runs missing earlier steps) |
| `config.py` | `scene.toml` → `ScenePlan` (validation, defaults; `--bbox` writes a default file) |
| `manifest.py` | Schema (dataclasses + JSON), canonical JSON writing (sorted keys, fixed float format), `hashes.txt` |
| `tiling.py` | Tile bounds, bbox → tiles, regions → per-tile zoom limits, `layer.json` `available` ranges |
| `datum.py` | Datum declarations → pinned PROJ pipelines; grid fetching into the cache; `PROJ_NETWORK=OFF` during builds |
| `cache.py` | Content-addressed blobs (`.cache/scene/blobs/sha256/ab/…`), URL index (hash, size, ETag), atomic downloads |
| `licences.py` + `licences.toml` | Licence register, allow-list check, attribution text |
| `sources/base.py` | `Source` protocol, `Asset`, `SourceRaster` (windowed read in its own CRS + datum declaration) |
| `sources/*.py` | `dep3_13` (3DEP 1/3″, dated `historical/` URLs), `dep3_1m` (3DEP 1 m project DEMs via TNM), `naip_pc` (Planetary Computer STAC), `worldcover`, `etopo2022`, `bmng` (Blue Marble NG), `wc_s2` (WorldCover S2 composite) |
| `layers/terrain.py` | Tile job: sample grid → per-source windows → ellipsoid heights → priority + feather → TIN → writer |
| `layers/imagery.py` | Leaf jobs from sources, parent jobs from four children |
| `layers/landcover.py` | WorldCover → 0.05° PNG tiles (port of `fetch_worldcover.py`) |
| `qmesh.py` | Quantized-mesh writer and decoder |
| `tms.py` | `tilemapresource.xml` writer |
| `engine.py` | Scheduler, process pool, resume markers, atomic writes, stats |
| `verify.py` | Quick and deep checks, JSON report |
| `pack.py` | `mksquashfs` with sorted entries, zero timestamps, no compression |

`scripts/landcover/fetch_worldcover.py` becomes a thin wrapper over the WorldCover adapter + land-cover
layer, with the same arguments and output (so `Content/NonUFS/LandCover` and its tests don't change).
`scripts/scene/spike/` is deleted when R0 lands.

## Sources and cache

```python
class Source(Protocol):
    id: str
    layer: Layer                      # TERRAIN | IMAGERY | LANDCOVER
    licence: str                      # key in licences.toml
    def discover(self, area: Area) -> list[Asset]
    def open(self, path: Path, asset: Asset) -> SourceRaster
```

- **Datum declarations** (adapters declare, `datum.py` converts; no datum maths in adapters):

  | Source | Horizontal | Vertical |
  |---|---|---|
  | 3DEP 1/3″ | NAD83(2011) geographic | NAVD88, GEOID18 |
  | 3DEP 1 m | project CRS (from the file; NAD83(2011) realisation) | NAVD88 with the project's geoid (GEOID18/12B/12A → `us_noaa_g2018u0` / `g2012bu0` / `g2012au0`); unknown geoid ⇒ project skipped with a warning |
  | NAIP | NAD83 UTM (from the file) | — |
  | ETOPO 2022 | WGS84 geographic | EGM2008 via ETOPO's own geoid-height grid |
  | Blue Marble NG, WorldCover, WC S2 | WGS84 geographic | — |

  Target: ITRF2014 geographic + ellipsoid height (`EPSG:7912`), coordinate epoch 2010.0, through
  explicit pipeline strings stored in the manifest. Known-point test: 100 m NAVD88 at UTM 11N
  470000 E 3685000 N → 65.283 m.
- **Priorities** (configurable in `scene.toml`): terrain 3DEP 1 m (newest project first) → 3DEP 1/3″
  → ETOPO; imagery NAIP → WC S2 → Blue Marble NG; land cover WorldCover.
- **Fetch:** whole assets, hashed, into the cache; rebuilds use cached bytes, refetch on a miss and
  fail on a hash mismatch. Planetary Computer URLs are signed at fetch time (re-signed on expiry);
  the manifest stores unsigned URLs. Expected volumes: Pendleton ≈ 16 GB NAIP + ≈ 3 GB elevation +
  global base (ETOPO 30″ surface 1.6 GB + geoid 1.5 GB, one Blue Marble month 2.3 GB).
- **Licences:** default allow-list `public-domain`, `cc-by-4.0`; anything else fails unless
  `--allow <id>`. The WC S2 composite's CC BY 4.0 is confirmed against ESA's licence page during
  implementation; if it can't be, the ring falls back to Blue Marble only.

## Layers

### Terrain tile job (z, x, y)

1. Sample a 257 × 257 grid over the tile bounds (ITRF lon/lat), plus a 32-sample margin.
2. For each source in priority order whose assets intersect: inverse-transform the grid points to
   the source CRS through the pinned pipeline, read the bounding window (with a small pad), sample
   bilinearly, convert to ellipsoid heights (vertical model evaluated on a coarse sub-grid and
   interpolated, since geoid grids are smooth; the error bound is unit-tested), mark nodata.
3. Merge: the highest-priority valid sample wins. At the edge of a higher-priority source's valid
   area its weight ramps from 0 (at the edge) to 1 (30 m inside), blending into the next source, using
   a distance field computed over the margined grid (so neighbouring tiles agree at shared edges).
4. TIN: pydelatin, `max_error = max(0.1 m, 0.25 × 77067 m / 2^z)` (a quarter of Cesium's assumed
   geometric error at that level). pydelatin's `y` counts up from the last row; triangles are CCW.
5. Write: `qmesh.py`.

### Quantized-mesh writer (`qmesh.py`)

- u/v from float64 lon/lat with rounding; edge vertices snapped to exactly 0 / 32767; heights
  quantised between the tile's min and max (rounded).
- Vertices renumbered by first use (high-water-mark order), indices high-water-mark encoded,
  16-bit or 32-bit by vertex count with the spec's alignment.
- Edge lists (W, S, E, N) sorted along the edge.
- Octahedral normals from area-weighted face normals in ECEF; degenerate or non-finite → the
  ellipsoid normal.
- Header: centre, min/max height, bounding sphere and horizon-occlusion point computed
  deterministically (fixed algorithm, float64).
- gzip level 9 with mtime 0.
- `layer.json`: `tilejson 2.1.0`, `format quantized-mesh-1.0`, `scheme tms`, `projection EPSG:4326`,
  `extensions ["octvertexnormals"]`, `available` from the tiles actually written (z0–8 complete over
  the globe, so Cesium never upsamples, which is where the spike's NaN came from), attribution.

### Imagery

- Leaf tiles (each tile at its region's max zoom): sources in priority order, warped to the tile
  (bilinear when upsampling, area-average when downsampling), nodata falling through (NAIP nodata is
  0 in all bands).
- Parents: 2 × 2 box filter over the four children (always four, see "Complete siblings"), level by
  level after their children.
- JPEG q85 4:2:0 via Pillow (libjpeg-turbo pinned in the container); `tilemapresource.xml`
  (`SRS EPSG:4326`, `profile geodetic`, levels and BoundingBox from what was written).

### Land cover

WorldCover adapter + the existing tile format over bbox + ring (Pendleton + 100 km ≈ 2,200 tiles,
≈ 33 MB). The runtime keeps reading `thermal.land_cover.dir`; pointing it at a package is R1.

### Global base

Terrain from ETOPO 2022 30″ surface (bathymetry included) to z8 globally; imagery from Blue Marble
NG 500 m (`bmng_month` in `scene.toml`, default 7) to z8 globally and as the gap filler below the
WC S2 ring and NAIP.

## Engine

- **Plan:** regions → tile sets per layer → for each tile, the intersecting assets (STRtree over
  asset footprints).
- **Jobs:** a `ProcessPoolExecutor` (`-j`, default = cores); each worker keeps an LRU of open source
  datasets. Terrain tiles are independent; imagery levels run leaves first, then parents bottom-up.
- **Resume markers:** `<pkg>/.state/<layer>/<z>/<x>/<y>` = JSON `{inputs, output}`: `inputs` hashes
  the layer settings, the intersecting assets' sha256s and the tool version; `output` is the tile's
  sha256. A tile is skipped when the marker's `inputs` match and the file's hash equals `output`.
  `.state/` is excluded from `hashes.txt` and from `pack`.
- **Atomic writes:** temp file in the same directory + `os.replace`.
- **Stats** to `build.json`: tiles, bytes, files, wall time per layer, peak RSS per worker.
- **Logging:** human progress on stderr; `--json-progress` for machines.

## Verify

- **Quick** (always): `hashes.txt` ↔ files ↔ `manifest.hashes_sha256`; `layer.json` `available` ==
  terrain tiles present; `tilemapresource.xml` levels/BoundingBox == imagery present; every source on
  the allow-list; `ATTRIBUTION.txt` == generated text.
- **Deep** (`--deep`, sampled 2 % of terrain tiles per zoom, `--all` for every tile): decode, compare
  vertex heights with the sources resampled through the same pipeline (p50 ≤ 5 cm, p99 ≤ 0.5 m,
  excluding the 30 m seam bands);
  same-zoom neighbours agree along shared edges (max |Δh| ≤ one height quantisation step of either tile);
  no NaNs; normals unit-length; every JPEG decodes at 256 × 256.
- Output: exit code + `verify.json`.

## Pack

`mksquashfs <pkg> <pkg>.sqfs -noI -noD -noF -noX -all-root -all-time 0 -mkfs-time 0 -sort <generated>
-e .state` (exact flags pinned by the container's squashfs-tools), then `<pkg>.sqfs.sha256`.
`docs/realism/offline-hosting.md` documents mounting (`mount -o loop,ro` or `squashfuse`, bind-mount `:ro`).

## Build container

`scripts/scene/Dockerfile`: Ubuntu 24.04, `gdal-bin` + `proj-bin` (cross-checks, `projinfo`),
`squashfs-tools`, `uv`, Python 3.12, `uv sync --frozen`. Built and referenced by digest; the digest
goes into `build.json`. `scripts/scene/build.sh` runs `camsim-scene` inside it with the fetch cache
and output directories mounted.

## Edge cases

- **Poles and antimeridian:** z0–8 tiles cover them; pole-degenerate triangles get ellipsoid normals.
  The bbox must not cross the antimeridian (validated).
- **Coast:** 3DEP is hydro-flattened and has nodata offshore; ETOPO bathymetry fills beyond. NAIP stops
  a few km offshore; WC S2 has no ocean tiles; Blue Marble fills.
- **Overlapping 1 m projects:** newest collection first; projects with unknown geoids are skipped.
- **Missing source coverage** (e.g. the Pendleton interior has no 1 m DEM): falls through to the next
  source; the zoom limit follows the source actually used.
- **Planetary Computer token expiry / HTTP errors:** retries with backoff; an asset that can't be
  fetched fails the build with its URL (no silent gaps).
- **Interrupted fetch:** temp file + hash check before the cache entry exists.
- **Disk space:** `plan` prints the estimated cache and package size before `fetch`.

## Performance

No hard time target in R0; gates 1 and 6 measure it. Bounds that are enforced: peak RSS per worker
≤ 2 GB; memory independent of area size (verified by gate 6 vs gate 1 per-worker RSS).

## Testing

**Unit** (`uv run --project scripts/scene --with pytest pytest scripts/scene/tests`; no network,
small recorded raster fixtures and recorded TNM/STAC responses; added to CI next to the HITL tests):

- `qmesh`: round trip through an independent decoder (`quantized-mesh-tile` or our decoder checked
  against it); high-water-mark validity; complete edge lists with exact 0/32767; finite normals at
  the poles; identical bytes across runs.
- `tiling`: bounds, bbox coverage, region zoom limits, `layer.json` ranges, `tilemapresource.xml`.
- `datum`: the known point (65.283 m); the Pendleton horizontal shift (≈ 1.35 m); a missing grid
  fails the build; the vertical sub-grid interpolation error bound.
- `engine`: resume skips everything; a settings change dirties one layer; a new asset dirties only
  its tiles; a killed job leaves no partial file.
- `licences`: refusal outside the allow-list; attribution text.
- `sources`: discovery parses recorded responses; datum declarations map to the right pipelines.
- `layers/terrain`: a synthetic DEM (known function) → decoded vertices within max_error; seam
  feather continuity across a tile edge.

**R0 exit gates:** the six in "Goal", recorded in `ROADMAP.md`/`REALISM.md` with numbers. Gate 5 uses
`scripts/scene/spike/spike_run.py`/`hot_check.py` logic (moved to `scripts/scene/tools/` before the
spike directory is deleted) and the spike TMS patch applied locally.

## Risks

- **Planetary Computer** availability or policy changes: adapters isolate it; NAIP 2024 adapters are
  the fallback path (R1).
- **Global base URLs** (NCEI, NASA Visible Earth) moving: assets are cached by hash; the manifest keeps
  the URL for provenance.
- **WC S2 composite licence** unconfirmed: fallback is Blue Marble for the ring.
- **Wheel window** (Python 3.12–3.13 until pydelatin 0.4 reaches PyPI); rasterio arm64 needs macOS 15+.
- **Byte-identical builds** depend on the container digest: apt packages aren't snapshot-pinned, so a
  rebuilt image may differ; the reference image is kept by digest.
- **SquashFS in Docker on macOS** (no loop mount in Docker Desktop): packing works with Homebrew
  `squashfs`; mounting is a Linux deployment step.
