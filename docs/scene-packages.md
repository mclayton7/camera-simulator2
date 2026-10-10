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
| `margin` | bbox + `imagery_margin_km` (default 3) on every side | ring's (z10) | the bbox's (z17 on NAIP) |
| `bbox` | the scene's bbox | to the source limit: z16 on 3DEP 1 m, z14 on 1/3″, z8 on ETOPO (`preview`: 14) | z17 on NAIP, z13 on WC S2, z8 on Blue Marble (`preview`: 13) |

NAIP is discovered over the margin, so its edge sits outside the bbox wherever NAIP exists there.

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
imagery_margin_km = 3                       # [0, 50], must be <= ring_km: NAIP is built this far beyond the bbox
balance = true                              # colour-match Sentinel-2 to NAIP and feather NAIP's edge (Imagery edge)
naip_water_buffer_m = 200                   # NAIP only this far beyond WorldCover water's edge: 0 (off) or [200, 5000] m (Imagery edge)
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

## Imagery edge (R1)

**Why.** The visible seam is NAIP to Sentinel-2 at the bbox edge. Sentinel-2 decoded the R0 way is brighter, hazier
and less saturated than NAIP. NAIP needs no balancing between its own files: neighbouring quarter-quads are
bit-identical where they overlap (USDA cuts them from one balanced state mosaic), so there is no per-file fitting.

**Margin.** `imagery_margin_km` (default 3, range 0-50) grows the bbox into the `margin` region, where NAIP is
discovered and built at the bbox's zoom, so NAIP's edge lies outside the bbox. It must be `<= ring_km`; a scene with
`ring_km < 3` must set the margin explicitly (the default is rejected).

**Colour match (`imagery/balance.json`).** Where NAIP and Sentinel-2 overlap on land (WorldCover classes 0 and 80
excluded), a 10 m lattice is sampled and fitted on its even half (the odd half is held out for the report): a per-band
tone curve (257 quantiles, Sentinel-2 reflectance to NAIP DN) plus a grid of 2 km cell offsets (>= 500 samples per
cell, 3x3 median over fitted cells, unfitted cells take the nearest fitted offset decayed by exp(-d / 10 km)). Every
Sentinel-2 sample on land, at every zoom, is decoded through it and clamped at the curve's ends; over water it fades back to the R0 decode (see Water). The file is quantised, listed
in `hashes.txt`, covered by `verify`, and its hash joins every imagery leaf's inputs hash; workers read it back from
the file, so `-j 1` and `-j N` give the same bytes. The fit report (samples, cells, held-out MAE, bias and cell bias
before and after, fit seconds) goes to the log and `build.json` under `balance`, never into `balance.json`. A rebuild
with unchanged inputs skips the fit.

**Feather.** NAIP blends into the next imagery source over 200 m, measured inward from NAIP's valid-data edge. The
distance is computed on a lattice with equal degree spacing in latitude and longitude (as terrain), so the ramp is
200 m north-south but 200 cos(lat) m east-west. Leaves well inside NAIP (every pixel NAIP and further than
200 m + 500 m inside the footprints) skip it and are NAIP alone, with the same bytes as the full path.

**Water.** Open water is one source, so boats see a uniform seabed through CamSim's ocean. The mask is WorldCover
class 80 (all permanent water, sea and lakes alike), read pixel by pixel at 10 m from the package's WorldCover
COGs; unknown counts as land. NAIP is kept only within `naip_water_buffer_m` (default 200 m) of land: beaches,
surf and harbours keep NAIP, and the 200 m feather runs from the shore out to that clip line, so NAIP is whole on
land and gone 200 m offshore. Beyond it Sentinel-2 (Blue Marble where the composite stops) is all there is, inside
the bbox too. The colour match is land-only: it fades to the R0 Sentinel-2 decode over the first 100 m of water
(`water_fade_m`), so open water is decoded exactly as in R0. Lakes, rivers and lagoons narrower than ~400 m lie
within the buffer and keep NAIP. Distances use degrees of latitude on both axes, as the feather. Leaves with no
water near them come out byte-identical to the no-mask path; the WorldCover hashes join the leaf inputs.
`build.json`'s `balance.water_mask` says whether the mask was on. `naip_water_buffer_m = 0` keeps NAIP over water
(the fade stays); the range is 200 to 5000 m (0 = clip off); values between 0 and 200, or above 5000, are rejected (below 200 the feather would fade NAIP on land). Without WorldCover
in the package the build warns and neither applies.

**No overlap.** When NAIP and Sentinel-2 share no land there is no `balance.json` and no feather: the legacy hard edge.

**Upgrading.** Re-planning a package built before this change rebuilds all layers once (the new `margin` region changes
every layer's settings hash); the terrain bytes come out identical. Packages planned before the water mask need
`build --replan` to pick up `naip_water_buffer_m` / `water_fade_m`; without them they build as before (no clip, no fade).

**Off switch.** `balance = false` with `imagery_margin_km = 0` reproduces the R0 imagery tiles byte for byte.
Without `balance`, no `balance.json` is written. With no NAIP in the package, or no shared land, there is no
`balance.json` and Sentinel-2 is decoded as before.

**Known limits.** Sentinel-2 stays 10 m, so the edge still loses resolution; flight-line seamlines inside NAIP
remain on land (faint), and NAIP's sea glint and fill blocks within 200 m of the shore; open water is Sentinel-2 at
10 m, with a Sentinel-2 to Blue Marble step where the composite stops at sea; leaves over clipped sea are still built
at NAIP's zoom (they hold upsampled Sentinel-2); the fit is per package, not global.
Acceptance on Pendleton passed 2026-10-09 (`docs/scene-packages.md`, "Measured", R1 chunk 1), with one accepted
deviation: the imagery build costs +20.9 % per tile, over the +10 % gate. `scripts/scene/tools/edge_shots.py OUT PKG
[--before OLD]` writes labelled crops of the bbox edges for review, `scripts/scene/tools/water_check.py PKG --cache DIR`
gates the open water, and `render_check.py` has `bbox_edge` and `sea_offshore` shots.

Known limits found in acceptance (follow-ups, `ROADMAP.md`):

- Thin land features count as land for the 200 m NAIP buffer: a pier or jetty keeps a dark ~200 m NAIP disc around it
  (San Clemente pier, west edge). Follow-up: ignore thin features when measuring the distance to land.
- A turquoise shallow shelf with a jagged hard outline against dark deep water shows in CamSim renders at the coast
  (`bbox_edge`, `ring_edge`; boat scenes). It is not imagery: it is in R0 too, from the seabed depth under the Single
  Layer Water ocean. Separate terrain/ocean follow-up.

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
  digest (`CAMSIM_SCENE_IMAGE_DIGEST`, set by `build.sh`), and per-layer stats (tiles, files, built/skipped,
  bytes, wall time, peak RSS per worker; `files` equals `tiles` for terrain and imagery, and counts the PNGs +
  `index.json` + `ATTRIBUTION.txt` for land cover). At the end of a build every package directory is set to 0755
  and every file to 0644 (`.state/` aside), so the packed image doesn't depend on the host's umask. When the peak RSS of any worker exceeds `--max-worker-rss-mb` (default 2048), `build` exits with an error after finishing (the package is complete; the number is in `build.json`).
- **Reproducibility contract:** the same `manifest.json` inputs + the same build-image digest + the same host CPU
  class ⇒ every file except `build.json` byte-identical, and so the same `hashes.txt` and the same `.sqfs`. The
  CPU class matters because NumPy picks its float64 `sin`/`cos` kernels by CPU features (e.g. AVX-512 or not),
  which can move the last bit of ECEF positions and normals: compare builds from machines of the same class. The image's apt packages aren't
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
| `terrain_heights` | decoded vertex heights vs the highest-priority source resampled independently (exact datum offsets, 30 m feather bands excluded). Scene zooms (above the `globe` region's terrain max zoom): p50 ≤ 5 cm, p99 ≤ 0.5 m. Global base zooms, each on its own: p99 ≤ max(0.5 m, 1 % of the zoom's TIN tolerance) (z8 0.75 m, z7 1.5 m, z3 24 m), because the build applies datum offsets through a lattice (exact every tile/32, bilinear between) that is metres off the exact value at coarse zooms. A wrong or mirrored tile is far above either limit |
| `terrain_edges` | same-zoom neighbours agree along shared edges within one height-quantisation step of either tile |
| `terrain_finite` | no NaN or infinite values |
| `terrain_normals` | octahedral normals decode to unit length |
| `imagery_decode` | every JPEG decodes at 256 × 256 |

`verify.json` records the height errors overall (`terrain_error_m`: `n`, `p50`, `p99`, `max`), over the scene zooms
(`terrain_error_m.scene`, the gate numbers; `base_max_zoom` is where the base stops) and per zoom
(`terrain_error_m.by_zoom`, with `p99_limit` on base zooms), and the `terrain_heights` line names the worst zoom. A check that can't run on
a malformed package (a missing `layer.json`, an unparsable `hashes.txt` line, a corrupt tile) fails with the error
as its detail instead of aborting `verify`.

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

## Using a package in CamSim

```bash
CAMSIM_SCENE_DIR=/abs/path/to/pendleton CAMSIM_SCENE_OFFLINE=1 scripts/run.sh --headless
```

- `scene.dir` (`CAMSIM_SCENE_DIR`, absolute) points terrain (`terrain/layer.json`), imagery
  (`imagery/tilemapresource.xml`, `cesium.imagery.source: tms`) and thermal land cover (`landcover/`) at the
  package, as `file:///` URLs. Reference: `docs/configuration.md`, "Scene Package".
- `scene.offline` (`CAMSIM_SCENE_OFFLINE`) refuses any network source and exits with status 1 on any config
  error, instead of falling back to Cesium ion.
- `Main.umap`'s ion actors (terrain ion 1 with Bing ion 2, OSM Buildings ion 96188) are reconfigured or destroyed
  when the world is initialised, before they load, so an offline run sends no request.

`scripts/scene/tools/render_check.py` sets this environment (`--offline` adds `CAMSIM_SCENE_OFFLINE=1` and fails on
any logged outbound request; see `scripts/scene/tools/README.md`).

## Measured

The R0 exit gates on the reference build. Filled in by gates 1–6.

| Gate | What | Result |
|---|---|---|
| 1 | Pendleton `sim` build in the reference container: wall time, peak RSS per worker, size and file count per layer | **Pass** (Linux reference box (Core Ultra 9 285K, 24 cores, 62 GB, image `sha256:120319c9ab01…`, `-j 16`, 2026-10-09)). Fetch 49 min (36 GB cache); build **12 min 35 s**. Terrain 182,602 tiles / files, 0.98 GB, 327 s, peak RSS 638 MB; imagery 253,766 / 253,766, 1.96 GB, 331 s, 812 MB; land cover 2,288 tiles / 2,290 files, 43 MB, 89 s, 195 MB. Package 2.9 GB (5.5 GB on 4 KB blocks), `.sqfs` 3.03 GB. Tile counts and layer sizes equal the native Mac build |
| 2 | Build killed at ~50 % resumes without rebuilding finished tiles | **Pass** (same box). `docker kill` at terrain 92,481/182,602; the resume took no lock error and built 89,033 + skipped 93,569 = 182,602 terrain tiles (the extra skips finished after the last progress line), 10 min 30 s; `hashes.txt` identical to build A |
| 3 | Second clean build: identical `hashes.txt` and `.sqfs` | **Pass** (same box). Build B from A's `manifest.json`: `hashes.txt` identical (438,661 files), both `.sqfs` sha256 `de89729b9e479e3beb9348f79f0bf72786e014d5ff855c8306c39eb5996ee1e3` |
| 4 | `verify --deep` | **Pass** on the reference build (same box, 2 min 6 s at `-j 16`): every check `ok`; scene z9+ p50 0.001 m, p99 0.049 m (max 0.39 m, 342,731 vertices); base z0–z8 within limits, nearest z7 (p99 0.84 of 1.51 m) and z8 (0.42 of 0.75 m). Native macOS build (2026-10-09): all checks pass, 199 s at `-j 8`; scene z9+ p50 0.001 m, p99 0.049 m (max 0.39 m, 342,704 vertices); base z0–z8 within limits, nearest z7 (p99 0.84 of 1.51 m) and z8 (0.42 of 0.75 m) |
| 5 | CamSim render: registration vs CWT + Bing (< 1 px), frame-centre heights (≤ 0.25 m on 1 m, ≤ 1 m on 1/3″), high oblique (no holes / untextured tiles), offline run | **Pass** (2026-10-09, M1 Pro / Metal, native macOS build of the package). Registration: all six EO shots ≤ 0.16 px (online and offline identical). Heights: 7/7 pass — 1 m points −0.10 … +0.20 m (three within 0.01 m), 1/3″ points −0.48 and −0.05 m; CWT on the same points fails 4/7 (up to +1.36 m). High oblique / ring edge: no holes or untextured tiles. Offline: closed later on 2026-10-09 by the offline profile (`scene.dir` + `scene.offline`, Cesium set up at world init): `render_check.py --offline` logs no outbound request and no `cesium.com` line, the terrain loads once from `file:///`; registration ≤ 0.16 px and heights 7/7 again (same values) |
| 6 | 10,000 km² `preview` build: time and size | **Pass** (same box, `pendleton-preview-10k`, 9,946 km²). Build **11 min 14 s**; terrain 183,378 tiles, 0.96 GB, 318 s, 647 MB; imagery 177,350, 0.81 GB, 219 s, 554 MB; land cover 3,564, 62 MB, 128 s, 195 MB. Package 1.8 GB (4.0 GB on disk). `plan` estimated 186,374 / 178,142 / 3,564 tiles and ~4.4 GB, cache 15.8 GB. `verify --deep` all `ok` (scene p50 0.004 m, p99 0.105 m) |

Notes from gates 1–4 and 6 (Linux):

- The Mac's `verify` counted 27 fewer vertices (342,704 vs 342,731): cross-machine differences are allowed (see the
  reproducibility contract); builds A, B and C on the same box are byte-identical.
- The reference image needed `chmod -R a+rX /opt/camsim-scene` (now in the Dockerfile): under a umask-027 host
  `COPY` kept the sources unreadable by the `-u` build user.

Gate 5 tools: `scripts/scene/tools/` (`render_check.py`, `hot_check.py`, `registration.py`). Notes from the run:

- `ring_edge` looks west over open sea, so its 0.00 px registration says nothing (the window is water and cloud);
  it is a coverage shot, not a registration one.
- `hot_check.py`'s points must sit on flat ground inside the coverage they claim: CA_SanDiegoCo_D24 stops at the
  base boundary (`x46y368` is 57 % nodata), and on a 1/3″ hillside (~20 m relief within 10 m) 1.5 m of
  frame-centre offset reads as 3 m of height. Four points were moved for this (2026-10-09).
- A dark, faintly banded strip ~450 m wide runs down the west edge of NAIP quarter-quad `ca_m_3311736_sw`
  (2022) over the sea: it is in the NAIP file (filled from another flight line, no sun glint), not a build
  artefact. It shows as a seam in `ring_edge`; flight-line colour balance is R1 work.

### R1 chunk 1 (NAIP edge, with the water mask)

Camp Pendleton `sim`, acceptance 2026-10-09 (Linux reference box, `-j 6`). Rows come from different builds: the
water gate, fit time and peak RSS from the water rebuild of `pendleton-r1` (step 2); determinism and build time from
`pendleton-r1b`; the R0-manifest check was measured at commit f43d96f (it holds by construction for later code).

| Gate | What | Result |
|---|---|---|
| Colour | Sentinel-2 vs NAIP over the fit cells, before and after the match (R/G/B) | **Pass.** Cell bias median 37/30/29 DN before, **2/2/2** after (limit 5); cell p90 58/49/48 -> 7/5/6; MAE 36.72/29.98/29.48 -> 13.12/10.22/9.93; bias 35.84/29.14/28.76 -> 0.42/0.45/0.33. Fit 32.7 s, 7,775,508 samples, 421 cells fitted (`pendleton-r1` water rebuild) |
| Water | `water_check.py`: open-water leaves against raw Sentinel-2, z10-z17 (all sampled) | **Pass** (exit 0, no fails; `pendleton-r1` water rebuild). Pooled mean abs <= 1.10 DN (limit 1.5); tile p95 <= 2.09 DN (limit 3); G-std excess <= 0.07 (limit 1). 9,655 leaves changed (3.5 %); 0 changed without water |
| Determinism | Rebuild; `verify --deep`; R0 manifest | **Pass.** Rebuild `hashes.txt` identical; `verify --deep` all `ok`; `pendleton-r1b`. The old R0 manifest rebuilds byte-identical to the original R0 package (measured at f43d96f) |
| Build time | Imagery ms/tile vs the same-session R0 baseline | **Over the +10 % gate, accepted** (`pendleton-r1b`). 3.372 ms/tile (935.0 s / 277,286, uncontended) vs R0 2.790 ms/tile (708 s / 253,766) = **+20.9 %** (+9.8 % before the water mask: the per-leaf water test, not the clip, is the cost). Offline package build only; no simulator runtime effect. User accepted the cost 2026-10-09. Margin adds 23,520 imagery tiles (+9.3 %). Max RSS of any build process 4.05 GB (`pendleton-r1` water rebuild) |
| Render | `render_check.py`: all 7 shots (incl. `bbox_edge`, `sea_offshore`) | **Pass.** All rendered; `sea_offshore` uniform Sentinel-2; land edge matched to NAIP brightness |
| Review | Screenshots (visual gate 4) | **Pass** (reviewed by the user, 2026-10-09). Residuals are the known limits above (pier disc; shallow shelf from the ocean, present in R0) |

The first R1 build (before the water mask) failed the visual gate over sea: raw NAIP quarter-quads over open water
and a land-fitted match that darkened Sentinel-2 water ([64,85,90] -> [1,24,43]). That led to the water mask.
