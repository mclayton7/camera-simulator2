# Scene packages

`camsim-scene` (REALISM R0, `scripts/scene/`) turns a bounding box into a **scene package**: one directory of
quantized-mesh terrain, TMS imagery and land cover that covers the whole Earth (a coarse global base under a
detailed area), with a manifest that pins every input, an attribution file and a hash list. `camsim-scene pack`
turns it into one SquashFS image for offline transfer.

R0 is tooling only. CamSim can already read the terrain (`cesium.terrain.source: url`); package imagery and land
cover reach the runtime in R1 (see [Using a package in CamSim](#using-a-package-in-camsim-until-r1)).

Design: [`docs/superpowers/specs/2026-10-07-scene-package-r0-design.md`](superpowers/specs/2026-10-07-scene-package-r0-design.md).
Background: [`docs/realism/`](realism/) (data sources, Cesium findings, offline hosting), [`docs/realism-r0-spike.md`](realism-r0-spike.md).

## What a package is

```
<name>/                         # shipped as <name>.sqfs
  manifest.json                 # deterministic content description (inputs, settings, datum pipelines, hashes_sha256)
  build.json                    # build-time facts only: UTC times, host, tool git commit, image digest, per-layer stats
  ATTRIBUTION.txt               # generated from the sources used
  hashes.txt                    # "sha256  path" per file except build.json, hashes.txt and manifest.json, sorted by path
  terrain/layer.json
  terrain/{z}/{x}/{y}.terrain   # quantized-mesh-1.0, gzip, octvertexnormals
  imagery/tilemapresource.xml
  imagery/{z}/{x}/{y}.jpg       # geodetic TMS, 256 px, JPEG q85 4:2:0
  landcover/index.json
  landcover/<lat>_<lon>.png     # the 4B thermal format (0.05° tiles, 600 × 600, 8-bit WorldCover codes)
  .state/                       # resume markers and the build lock (not hashed, not packed)
```

`manifest.json` covers itself through `hashes_sha256` (the sha256 of `hashes.txt`), which is why `hashes.txt`
leaves it out.

**One tile grid** for terrain and imagery: OGC TMS 2.0 **`WorldCRS84Quad`** (OGC 17-083r4): EPSG:4326
longitude/latitude (here in ITRF2014), 2 × 1 tiles at level 0, tile size `180° / 2^z`, 256 px. Files are
addressed TMS-style as Cesium expects, with `y` counted **from the south**; OGC's row counts from the top:

```
row_ogc = 2^z - 1 - y_tms
```

The manifest's `tiling` block names the matrix set (`tile_matrix_set`, `tile_matrix_set_uri`) next to the TMS
addressing (`scheme: tms`, `y_origin: south`).

**Regions** set the maximum zoom per layer; the highest-detail region a tile overlaps wins:

| Region | Area | Terrain | Imagery |
|---|---|---|---|
| `globe` | whole Earth | z0–8 | z0–8 |
| `ring` | bbox + `ring_km` (default 100) on every side | z9–10 | z9–10 |
| `bbox` | the scene's bbox | to the source limit: z16 on 3DEP 1 m, z14 on 1/3″, z8 on ETOPO (`preview`: 14) | z17 on NAIP, z13 on WC S2, z8 on Blue Marble (`preview`: 13) |

A tile's depth is min(region limit, the limit of the best source actually covering it), for imagery as for
terrain: ocean tiles inside the bbox stop where NAIP stops instead of upsampling Blue Marble to z17, and the
D24 1 m DEM stops along its own (diagonal) data edge. Source footprints are valid-data polygons computed at
`fetch` from each asset's coarse overview.

**Complete siblings:** a tile has either 0 or 4 children, in both layers. A sibling past its own source's limit
is sampled from the coarser source. Cesium then never upsamples a missing child, and every imagery parent has
four children.

**Whole globe:** z0–8 is built everywhere (terrain from ETOPO 2022, imagery from Blue Marble), so a package needs
no network at all and `layer.json`'s `available` is complete to z8.

**Standards used:**

| Standard | Where |
|---|---|
| OGC TMS 2.0 `WorldCRS84Quad` (OGC 17-083r4) | the tile grid (TMS 1.0 addressing, `y` from the south) |
| quantized-mesh-1.0 ([Cesium spec](https://github.com/CesiumGS/quantized-mesh)) | `terrain/**/*.terrain`, with the `octvertexnormals` extension; `layer.json` is TileJSON 2.1.0 |
| TMS 1.0 (OSGeo Tile Map Service) `tilemapresource.xml` | `imagery/` (`SRS EPSG:4326`, `profile geodetic`) |
| Cloud Optimized GeoTIFF (OGC 21-026) | derived rasters in the fetch cache (`.cache/scene/derived/`) |
| SPDX licence ids | `licence` fields, `--allow`, `manifest.licence_allow` (`LicenseRef-` for licences SPDX doesn't list) |

## Quick start

The **reference build** runs in the pinned build container (Ubuntu 24.04, `scripts/scene/Dockerfile`); only it
gives byte-identical packages. `build.sh` mounts the repo read-only at `/work`, the fetch cache at `/cache`
(`CAMSIM_SCENE_CACHE`, default `.cache/scene`) and outputs at `/out` (`CAMSIM_SCENE_OUT`, default
`.cache/scene-packages`):

```bash
scripts/scene/build.sh --build-image build /out/pendleton --config scripts/scene/examples/pendleton.toml -j 16
scripts/scene/build.sh verify /out/pendleton --deep
scripts/scene/build.sh pack /out/pendleton
# native (macOS/Linux; passes verify, bytes may differ from the container):
uv run --project scripts/scene camsim-scene build .cache/scene-packages/pendleton --config scripts/scene/examples/pendleton.toml
```

The steps can also be run one at a time:

| Command | What it does |
|---|---|
| `camsim-scene plan PKG --config scene.toml` (or `--bbox W S E N [--name N] [--profile sim]`) | Discovers each source's assets, writes `manifest.json` (unhashed) and prints the estimated cache and package size. `--bbox` writes a default `<name>.scene.toml` beside the package (`PKG/../`), or reuses one that matches; an existing file with a different bbox, profile or name is an error |
| `camsim-scene fetch PKG` | Downloads every asset into the cache, hashes it, prepares derived COGs, computes footprints, hashes the datum grids |
| `camsim-scene build PKG [--config scene.toml] [--replan] [-j N] [--json-progress] [--max-worker-rss-mb 2048]` | Runs the missing earlier steps, then the tile jobs (resumable) |
| `camsim-scene verify PKG [--deep [--all]] [-j N]` | Checks the package, writes `PKG.verify.json`; exit 1 when a check fails |
| `camsim-scene pack PKG [--out FILE]` | `PKG.sqfs` + `PKG.sqfs.sha256` + `PKG.sqfs.build.json` |
| `camsim-scene attribution PKG` | Prints the attribution text |

Every command takes `--cache DIR` (else `$CAMSIM_SCENE_CACHE`, else `<repo>/.cache/scene`) and `-v`. Errors
exit with code 2 and a one-line message.

`build` plans **only when `manifest.json` is missing**. After editing `scene.toml` for an existing package, pass
`--replan` (with `--config` or `--bbox`) to re-plan; tiles that left the new plan are removed and unchanged tiles
are kept. Without `--replan`, a `--config`/`--bbox`/`--allow` that differs from `manifest.json` (name, seed,
regions, layer settings, licence allow-list, source options) is an error rather than silently ignored.
`--name` and `--profile` only apply with `--bbox`. `plan`, `fetch` and `build` all take the package's build lock
before writing `manifest.json`, so none of them can change it under a running build.

Examples: `scripts/scene/examples/pendleton.toml` (gates 1–5, `sim`) and `pendleton-preview-10k.toml` (gate 6,
~10,000 km², `preview`).

## scene.toml reference

```toml
name = "pendleton"                          # [a-z0-9][a-z0-9_-]{0,63}
bbox = [-117.62, 33.19, -117.24, 33.52]     # W S E N, degrees; W < E (must not cross the antimeridian), S < N
profile = "sim"                             # preview | sim
seed = 0                                    # integer, recorded in the manifest
ring_km = 100                               # [0, 1000]: ring = bbox grown by this much on every side
bmng_month = 7                              # [1, 12]: Blue Marble NG month (sets sources.bmng.month)
jpeg_quality = 85                           # [1, 95]
allow = []                                  # extra licence ids (SPDX, see licences.toml)

[priorities]                                # optional; defaults from the profile
terrain = ["dep3_1m", "dep3_13", "etopo2022"]
imagery = ["naip_pc", "wc_s2", "bmng"]
landcover = ["worldcover"]

[sources.naip_pc]                           # per-source options; `adapter` picks a non-default class ("module:Class")
year = "2022"

[zoom.bbox]                                 # optional zoom overrides per region (globe | ring | bbox), [0, 22]
imagery = 17
terrain = 16
```

Unknown keys are errors. **Profiles** (Decision 3):

| Profile | Terrain priority | Imagery priority | Land cover | bbox zoom (terrain / imagery) |
|---|---|---|---|---|
| `preview` | `dep3_13` → `etopo2022` | `wc_s2` → `bmng` | `worldcover` | 14 / 13 |
| `sim` | `dep3_1m` → `dep3_13` → `etopo2022` | `naip_pc` → `wc_s2` → `bmng` | `worldcover` | 16 / 17 |

`globe` is z8 and `ring` z10 for both layers in both profiles.

**Source options** (`[sources.<id>]`):

| Option | Meaning |
|---|---|
| `naip_pc.year` | NAIP year on Planetary Computer (default `"2022"`, California's newest there) |
| `dep3_13.as_of` | `YYYYMMDD`: use the newest dated `historical/` copy of each 1° cell on or before this date (default: the newest). A cell with no copy by then is skipped with a warning |
| `dep3_1m.geoid_overrides` | `{ "<project>" = "GEOID18" }`: the project's geoid when the USGS WESM index doesn't give exactly one. If the WESM service is down, planning proceeds only when the overrides cover **every** project; otherwise it fails naming the uncovered ones |
| `bmng.month` | set from `bmng_month` (don't set it directly) |
| `adapter` | any source: a built-in id or `"module:Class"` (an importable adapter class) |

**`allow`** adds licence ids to the default allow-list (`LicenseRef-PublicDomain-USGov`, `CC-BY-4.0`); `--allow
<id>` on the command line does the same. Check the terms first: `licences.toml` lists the known ids and whether
they're redistributable.

## Sources

| Id | Dataset | Licence | Area | Zoom limit | Datum (horizontal / vertical) |
|---|---|---|---|---|---|
| `dep3_1m` | USGS 3DEP 1 m project DEMs (10 × 10 km UTM tiles, via The National Map) | `LicenseRef-PublicDomain-USGov` | US, where a 1 m project exists | z16 | project CRS (NAD83(2011)) / NAVD88 with the project's geoid (GEOID18 or GEOID12B) |
| `dep3_13` | USGS 3DEP 1/3 arc-second DEM (1 × 1° GeoTIFFs, dated `historical/` URLs) | `LicenseRef-PublicDomain-USGov` | US | z14 | NAD83(2011) geographic / NAVD88, GEOID18 |
| `etopo2022` | NOAA ETOPO 2022 30″ surface elevation (land + bathymetry) + its geoid-height grid | `LicenseRef-PublicDomain-USGov` | globe | z8 | WGS 84 / EGM2008 via ETOPO's own geoid grid |
| `naip_pc` | USDA NAIP 0.6 m RGB(N) COGs, Microsoft Planetary Computer STAC | `LicenseRef-PublicDomain-USGov` | conterminous US (to a few km offshore) | z17 | NAD83 UTM (from the file) / — |
| `wc_s2` | ESA WorldCover 2021 Sentinel-2 RGBNIR composite (10 m, 1 × 1° COGs) | `CC-BY-4.0` (checked against each file's licence tag) | land (no open-ocean tiles) | z13 | WGS 84 / — |
| `bmng` | NASA Blue Marble Next Generation, topography + bathymetry, 500 m (2004 monthly) | `LicenseRef-PublicDomain-USGov` | globe | z8 | WGS 84 / — |
| `worldcover` | ESA WorldCover 10 m 2021 v200 land cover (3 × 3° COGs) | `CC-BY-4.0` | land | — (0.05° land-cover tiles over bbox + ring) | WGS 84 / — |

Assets are fetched whole into a content-addressed cache (`.cache/scene/blobs/sha256/…`), hashed, and reused
across packages; a rebuild refetches on a miss and fails on a hash mismatch. Planetary Computer URLs are signed
at fetch time and re-signed when the token expires (the manifest keeps unsigned URLs; tokens are never logged).
ETOPO and Blue Marble get a derived COG with overviews in `.cache/scene/derived/`.

**Expected cache volumes** (Camp Pendleton, `sim`):

| Data | Size |
|---|---|
| NAIP 2022 | ≈ 16 GB |
| 3DEP elevation (1 m + 1/3″) | ≈ 3 GB |
| ETOPO 2022 30″ surface + geoid | 1.6 GB + 1.5 GB |
| Blue Marble NG, one month | 2.3 GB |
| WC S2 composite | ≈ 0.5 GB per 1° cell; ≈ 9 cells for Pendleton + 100 km |

The global base (ETOPO, Blue Marble) is shared by every package that uses the same cache. `plan` prints the
estimate for the scene at hand before anything is downloaded.

**Sampling.** Terrain samples a 257 × 257 grid (plus a 32-sample margin) per tile; imagery warps sources to the
256 px tile. Each source is read from the **coarsest average overview at or finer than the target sample
spacing**, chosen from the zoom alone (never the tile's position, so neighbouring tiles read the same pixels),
then sampled bilinearly. This approximates area-averaging to within the 2× step between overview levels.
Higher-priority sources fade into the next one over 30 m inside their valid-data edge (measured in degrees of
latitude on both axes, so ≈ 25 m east–west at Pendleton). Imagery parents are a 2 × 2 box filter of their four
children.

## Datums

Target: **ITRF2014** geographic + ellipsoid height (`EPSG:7912`), coordinate epoch **2010.0**. WGS 84 (G2139)
equals ITRF2014 at the centimetre level, so WGS 84 sources (ETOPO, Blue Marble, WorldCover, WC S2) need no
horizontal shift.

Adapters only declare a datum; `datum.py` turns it into an explicit PROJ pipeline that is stored in the manifest
(PROJ's defaults are wrong here: `EPSG:4979` picks a GEOID03 chain 0.78 m off, `EPSG:9755` silently drops the
geoid). The US chain (3DEP):

```
NAD83(2011) lon/lat + NAVD88 height
  -> vgridshift (GEOID18: us_noaa_g2018u0.tif; GEOID12B: us_noaa_g2012bu0.tif)   NAVD88 -> NAD83(2011) ellipsoid
  -> cartesian (GRS80) -> inverse Helmert "ITRF2014 to NAD83(2011) (1)", t_epoch 2010, coordinate frame
  -> geographic (GRS80)                                                        = ITRF2014 lon/lat/h
```

Sources labelled NAD83 (EPSG:4269, 269xx) are taken as the NAD83(2011) realisation. Horizontally the shift at
Pendleton is ≈ 1.35 m. ETOPO heights (EGM2008) become ellipsoid heights through ETOPO's own geoid-height raster.

- Grids are named by their cdn.proj.org file name, fetched into the cache at `fetch`, hashed into the manifest,
  and substituted by absolute path at build time with `PROJ_NETWORK=OFF`; a missing grid fails the build.
- **Known point:** 100 m NAVD88 at UTM 11N 470000 E 3685000 N → **65.283 m** ITRF2014 ellipsoid height
  (unit-tested).
- **GEOID12A is unsupported** (Decision 2): `us_noaa_g2012au0.tif` isn't on cdn.proj.org. 1 m projects on
  GEOID12A, or on an unknown geoid, are skipped with a warning (for Pendleton that is
  `San_Diego_CA_2014_LiDAR`, which the newer `CA_SanDiegoCo_D24` on GEOID18 covers anyway). If you know a
  project's geoid, set `sources.dep3_1m.geoid_overrides`.
- **Vertical offset lattice:** the NAVD88 → ellipsoid offset (and the ETOPO geoid) is evaluated exactly on nodes
  spaced `tile_size(z) / 32`, aligned to the global tile grid, and interpolated bilinearly in between. Neighbours
  share the nodes on their common edge, so edge heights agree exactly. Error: ≈ 5 mm at z9 (measured 5.01 mm),
  ≤ 0.3 mm at z12 and deeper (GEOID18, Pendleton). The geoid is read at a resolution that follows the zoom, so
  memory stays bounded at z0–z3 (where the geoid's own detail is far below the tile's sample spacing).

## Resume, rebuild, reproducibility

- **Markers:** `.state/<layer>/<z>/<x>/<y>` holds `{inputs, output}`. A tile is skipped when the marker's `inputs`
  match and the file's sha256 equals `output`. Files are written to a temp name in the same directory and
  renamed, so a killed job never leaves a partial tile.
- **What dirties what:**
  - terrain tile: the terrain layer settings (priorities, grid, feather, max-error rule, format), the tool version,
    and the sha256s of the assets intersecting that tile;
  - imagery leaf: the imagery layer settings, the tool version and the intersecting assets' sha256s;
  - imagery parent: the imagery settings and its four children's output hashes (so a changed leaf rebuilds its
    ancestors only);
  - land cover: rebuilt as a whole unless `.state/landcover.json`'s inputs hash (settings + assets) matches (a few
    seconds).
  Changing one layer's settings rebuilds only that layer; a new asset rebuilds only the tiles it touches.
- **Stale removal:** tiles (and markers) that aren't in the current plan, e.g. after a smaller bbox or lower zoom,
  are deleted before the jobs run, so the package holds exactly the plan.
- **Lock:** `build` holds an exclusive lock on `.state/lock` (so do `plan` and `fetch` while they write
  `manifest.json`); a second build, plan or fetch of the same package refuses with a clear message instead of
  interleaving writes.
- **`manifest.json` vs `build.json`:** the manifest is deterministic (inputs, settings, pipelines, hashes; nothing
  time- or host-dependent). `build.json` holds the build facts: UTC times, host, tool git commit, the build-image
  digest (`CAMSIM_SCENE_IMAGE_DIGEST`, set by `build.sh`), and per-layer stats (tiles, built/skipped, bytes, wall
  time, peak RSS per worker). When the peak RSS of any worker exceeds `--max-worker-rss-mb` (default 2048), `build` exits with an error after finishing (the package is complete; the number is in `build.json`).
- **Reproducibility contract:** the same `manifest.json` inputs + the same build-image digest ⇒ every file except
  `build.json` byte-identical, and so the same `hashes.txt` and the same `.sqfs`. The image's apt packages aren't
  snapshot-pinned, so a rebuilt image may differ: keep the reference image by digest. Native (non-container)
  builds must pass `verify` but needn't match bytes.

## verify

`camsim-scene verify PKG` writes `<pkg>.verify.json` **beside** the package (the package directory never changes
after `build`) and exits 1 when any check fails.

**Quick** (always):

| Check | Passes when |
|---|---|
| `hashes` | `hashes.txt` matches the files on disk and `manifest.hashes_sha256` |
| `terrain_available` | `layer.json`'s `available` equals the terrain tiles present |
| `tilemapresource` | `tilemapresource.xml` levels and BoundingBox equal the imagery present |
| `licences` | every source's licence is on the manifest's allow-list |
| `attribution` | `ATTRIBUTION.txt` equals the text generated from `manifest.json` |

**Deep** (`--deep`: a 2 % sample of the terrain tiles per zoom, at least one; `--all`: every tile):

| Check | Passes when |
|---|---|
| `terrain_heights` | decoded vertex heights vs the highest-priority source resampled independently (exact datum offsets, 30 m feather bands excluded): p50 ≤ 5 cm, p99 ≤ 0.5 m |
| `terrain_edges` | same-zoom neighbours agree along shared edges within one height-quantisation step of either tile |
| `terrain_finite` | no NaN or infinite values |
| `terrain_normals` | octahedral normals decode to unit length |
| `imagery_decode` | every JPEG decodes at 256 × 256 |

## Packing and mounting

`camsim-scene pack PKG [--out FILE]` needs `mksquashfs` (squashfs-tools ≥ 4.6; in the container, or
`brew install squashfs` / `apt install squashfs-tools`). It runs

```
mksquashfs PKG PKG.sqfs -noappend -noI -noD -noF -noX -all-root -all-time 0 -mkfs-time 0 -sort <generated> -e .state build.json
```

uncompressed (JPEG and gzip tiles don't compress further), owner root, every timestamp 0, metadata and coarse
tiles stored first. It writes `PKG.sqfs`, `PKG.sqfs.sha256` and **`PKG.sqfs.build.json`**: `build.json` holds
build timestamps, so it is left out of the image (as is `.state/`) to keep two builds of the same manifest
byte-identical, and copied beside it instead.

Mounting (Linux):

```bash
sudo mount -o loop,ro pendleton.sqfs /mnt/pendleton     # or, without root: squashfuse pendleton.sqfs /mnt/pendleton
docker run ... -v /mnt/pendleton:/data/scene/pendleton:ro ...
```

Docker Desktop on macOS can't loop-mount: use the package directory itself (bind-mounted `:ro`). Background on
why one image beats millions of loose files: [`docs/realism/offline-hosting.md`](realism/offline-hosting.md).

## Using a package in CamSim (until R1)

- **Terrain** works today: `cesium.terrain.source: url` with `cesium.terrain.url:
  file:///…/pendleton/terrain/layer.json` (`CAMSIM_CESIUM_TERRAIN_SOURCE=url`, `CAMSIM_CESIUM_TERRAIN_URL`).
- **Imagery** needs a TMS overlay, which lands in R1. Until then apply `scripts/scene/tools/tms_overlay.patch`
  locally (never commit it) and set `CAMSIM_CESIUM_IMAGERY_SOURCE=tms`,
  `CAMSIM_CESIUM_IMAGERY_WMS_URL=file:///…/pendleton/imagery/tilemapresource.xml`.
- **Land cover** for thermal IR: `thermal.land_cover.dir` (`CAMSIM_THERMAL_LAND_COVER_DIR`) → `PKG/landcover`.
- Set `CAMSIM_CESIUM_ION_TOKEN=""` so nothing is fetched from ion. `Main.umap` still holds the ion actors and
  there is no offline config profile yet (both R1).

`scripts/scene/tools/render_check.py` sets exactly this environment (see `scripts/scene/tools/README.md`).

## Measured

The R0 exit gates on the reference build. Filled in by gates 1–6.

| Gate | What | Result |
|---|---|---|
| 1 | Pendleton `sim` build in the reference container: wall time, peak RSS per worker, size and file count per layer | *pending* |
| 2 | Build killed at ~50 % resumes without rebuilding finished tiles | *pending* |
| 3 | Second clean build: identical `hashes.txt` and `.sqfs` | *pending* |
| 4 | `verify --deep` | *pending* |
| 5 | CamSim render: registration vs CWT + Bing (< 1 px), frame-centre heights (≤ 0.25 m on 1 m, ≤ 1 m on 1/3″), high oblique (no holes / untextured tiles), offline run | *pending* |
| 6 | 10,000 km² `preview` build: time and size | *pending* |

Gate 5 tools: `scripts/scene/tools/` (`render_check.py`, `hot_check.py`, `registration.py`).
