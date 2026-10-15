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
  ndvi/tilemapresource.xml      # TMS 1.0, image/png (packages with an NDVI layer)
  ndvi/fit.json                 # NAIP -> Sentinel-2 NDVI fit (gain, offset, 2 km offset grid)
  ndvi/{z}/{x}/{y}.png          # 8-bit grayscale NDVI (0 nodata, 1..255 = -1..+1); tiles without data are absent
  sea_level.json                # local MSL offset from EGM96 (packages with a [sea_level] station)
  landcover/index.json
  landcover/<lat>_<lon>.png     # the 4B thermal format (0.05° tiles, 600 × 600, 8-bit WorldCover codes; + 200-220 with [features])
  features/buildings/tileset.json   # 3D Tiles 1.1 (packages with [buildings] that keep at least one building; Buildings (R3))
  features/buildings/{z}/{x}/{y}.glb  # z13-z15, one GLB per tile that holds buildings
  features/buildings/mask/          # footprint mask: 0.05 deg 4800^2 1-bit PNGs + index.json (R3b; with [buildings])
  canopy/cover/, canopy/height/     # CHMv2 canopy cover % and p90 height, land-cover grid (R4; with [canopy])
  vegetation/mix.json               # species mix per land-cover code (R4; with [vegetation])
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
| `detail:<name>` | a `[detail]` polygon (R5) | the detail profile's (16) | the detail profile's (17) |
| `detail_margin:<name>` | the polygon + `imagery_margin_km` | ring's (z10) | the detail profile's (17) |

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
| TMS 1.0 (OSGeo Tile Map Service) `tilemapresource.xml` | `imagery/` and `ndvi/` (`SRS EPSG:4326`, `profile geodetic`) |
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
ndvi = true                                 # NDVI layer (default true in sim, false in preview; NDVI section)
ndvi_max_zoom = 15                          # [10, 17]: NDVI pyramid depth (z15 ~ 2.4 m)
allow = []                                  # extra licence ids (SPDX, see licences.toml); [features] OSM layers and [buildings] need "ODbL-1.0"

[features]                                  # optional, all off by default: vector features burned into landcover/ (Vector features (R2))
roads = true                                # OSM highway=* (road_classes) and runways/taxiways -> 200/201
paved_areas = true                          # OSM area:highway, highway + area=yes, amenity=parking, aeroway=apron -> 200/201
rail = true                                 # OSM railway=rail / light_rail -> 210
water = true                                # USGS NHD HR perennial lakes, ponds, reservoirs, stream/river areas -> 220
road_classes = ["motorway", "trunk", "primary", "secondary", "tertiary", "unclassified", "residential"]  # default; also service, track, living_street

[buildings]                                 # optional: Overture buildings -> features/buildings/ (Buildings (R3)); needs "ODbL-1.0" in allow
enabled = true                              # false = as if the table were absent
release = "2026-09-23.1"                    # Overture release id (YYYY-MM-DD.N), snapshotted on first fetch
min_area_m2 = 10                            # [0, 10000]: drop footprints smaller than this
roof_textures = true                        # REALISM R3b: per-tile roof atlas from the imagery sources; false = flat grey roofs
roof_texel_m = 0.5                          # [0.25, 2.0]: atlas texel in metres; steps up 0.75 / 1.0 / 1.5 / 2.0 when a tile's roofs do not fit
roof_texture_max_px = 2048                  # 256 .. 8192, a power of two: largest atlas side

[bridges]                                   # optional: OSM bridge decks -> features/bridges/ (Bridges (R3)); needs [features] roads or rail
enabled = true                              # false = as if the table were absent

[priorities]                                # optional; defaults from the profile
terrain = ["dep3_1m", "dep3_13", "noaa_sd13", "noaa_crm_socal", "etopo2022"]
imagery = ["naip_pc", "wc_s2", "bmng"]
landcover = ["worldcover"]

[sea_level]                                 # optional: local mean sea level (Sea level section)
station = "9410230"                         # NOAA CO-OPS station id with a NAVD88 datum tie; required when the bbox or ring meets the CRM

[sources.naip_pc]                           # per-source options; `adapter` picks a non-default class ("module:Class")
year = "2022"

[zoom.bbox]                                 # optional zoom overrides per region (globe | ring | bbox | detail), [0, 22]
imagery = 17
terrain = 16

[detail]                                    # optional (REALISM R5): GeoJSON polygons built at `profile` here inside a coarser box (Detail polygons (R5))
file = "areas.geojson"                      # FeatureCollection of named Polygon / MultiPolygon features, lon/lat; relative to this file
profile = "sim"                             # the profile inside the polygons ("sim" only); the box's `profile` must be "preview"
scoped = { terrain = ["dep3_1m"], imagery = ["naip_pc"] }  # optional: sources limited to the polygons (default: in the detail profile, not the box's)
```

Unknown keys are errors. **Profiles** (Decision 3):

| Profile | Terrain priority | Imagery priority | Land cover | bbox zoom (terrain / imagery) |
|---|---|---|---|---|
| `preview` | `dep3_13` → `noaa_sd13` → `noaa_crm_socal` → `etopo2022` | `wc_s2` → `bmng` | `worldcover` | 14 / 13 |
| `sim` | `dep3_1m` → `dep3_13` → `noaa_sd13` → `noaa_crm_socal` → `etopo2022` | `naip_pc` → `wc_s2` → `bmng` | `worldcover` | 16 / 17 |

`globe` is z8 and `ring` z10 for both layers in both profiles.

**Source options** (`[sources.<id>]`):

| Option | Meaning |
|---|---|
| `naip_pc.year` | NAIP year on Planetary Computer (default `"2022"`, California's newest there) |
| `dep3_13.as_of` | `YYYYMMDD`: use the newest dated `historical/` copy of each 1° cell on or before this date (default: the newest). A cell with no copy by then is skipped with a warning |
| `dep3_1m.geoid_overrides` | `{ "<project>" = "GEOID18" }`: the project's geoid when the USGS WESM index doesn't give exactly one. If the WESM service is down, planning proceeds only when the overrides cover **every** project; otherwise it fails naming the uncovered ones |
| `noaa_crm_socal.msl_above_navd88_m` | set by `plan` from `[sea_level] station` (don't set it directly). A package that uses `noaa_crm_socal` without a station is a config error |
| `bmng.month` | set from `bmng_month` (don't set it directly) |
| `osm_geofabrik.extract` | Geofabrik extract id (default: the smallest extract whose polygon contains the margin area; Pendleton: `socal`) |
| `osm_geofabrik.date` | `YYYY-MM-DD` dated daily file to pin (default: the newest of today back 7 days that exists; a configured date must exist) |
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
  Layer Water ocean. **Explained**: the shelf is 3DEP's hydro-flattened plate (Seabed section). **Fixed (R1)**: the
  seabed now sits below sea level. Offshore terrain is 0.0 % above sea - 0.5 m (was 65.6 %), and the median waterline
  is 20 m from WorldCover's shoreline (was 140 m). See coast_check (a) and (c) in "Measured".

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

**Fit (`ndvi/fit.json`).** NAIP NDVI is computed from uncalibrated DN. Two stages put it on Sentinel-2's scale, both on
the colour match's 10 m land lattice (WorldCover 0 and 80 excluded, even nodes fitted, odd held out): one linear map,
`gain x ndvi + offset` (least squares, >= 500 samples), then a smooth offset per 2 km cell: the median residual
(Sentinel-2 minus fitted NAIP) of each cell with >= 500 samples, 3 x 3 median-filtered and filled outward with a
10 km decay exactly as the colour match's offsets (`balance.cell_medians`, `fill_offsets`, `offset_at`). Applied to
NAIP samples only: `clip(gain x ndvi + offset + cell_offset(lon, lat), -1, 1)`; Sentinel-2 is never changed. The
offset removes the regional residual one global map leaves near NAIP's edge (Pendleton: a +0.047 step before it).
`fit.json` (format 2) holds integers only: `gain_e6`, `offset_e6`, `grid` (`west_e9`, `south_e9`, `cell_lon_e9`,
`cell_lat_e9` in nanodegrees, `nx`, `ny`) and `cell_offsets_e6` (`ny` rows of `nx` millionths, row 0 southernmost);
it is hashed and joined to every NDVI leaf's inputs. `build.json` -> `ndvi`: samples, gain, offset, grid, `cells`
(fitted) and `cells_filled`, the cell offset range, held-out MAE before / global only / after and median bias
before/after, and the residual median bias per NAIP acquisition date. No shared land: no `fit.json`, NAIP NDVI
unfitted (warning). A rebuild with unchanged inputs skips the fit; a package built before the grid refits (and
rebuilds its NDVI tiles) after `build --replan`.

**Off switch.** `ndvi = false` (the `preview` default): no `ndvi/`; every other file is unchanged. Setting
`ndvi = true` without `naip_pc` or `wc_s2` in the imagery priorities is an error; without NAIP the layer is
Sentinel-2 alone and builds without a fit (a fallback, not a supported mode). A package planned before this layer has
none until `build --replan`, which rebuilds only the NDVI tiles. Since the `sim` default is `ndvi = true`, an existing
`sim` package needs `--replan` (or `ndvi = false` in its scene.toml) before its next `build --config`; otherwise the
build stops with "differs … in layers; pass --replan".

**Acquisition date and sun.** Each NAIP asset in `manifest.json` records `acquired` (the date), `sun_noon`
(`elevation_deg`, `azimuth_deg` at local solar noon) and `sun_window` (`min_elevation_deg` 30, `azimuth_deg`
[morning, afternoon]: where the sun is when it crosses 30 degrees), at the centre of the quad's bbox, NOAA equations,
no refraction. Planetary Computer's NAIP times are a placeholder (`T16:00:00Z` on every quad), so the true sun is
unknown: NAIP flies with the sun at 30 degrees or more, so its elevation was between 30 degrees and
`sun_noon.elevation_deg`, and its azimuth within `sun_window.azimuth_deg`. At Pendleton on 2022-05-30 that window is
82-278 degrees, so in summer the azimuth is barely constrained. Imagery shadows are baked in for that sun (a known
limit, not corrected). Sentinel-2 assets record `composite: "2021"` (a year's median: no single sun). Asset metadata
doesn't feed tile hashes, so re-planning adds it without rebuilding tiles.

**Known limits.** NAIP NDVI is from DN: the fit matches Sentinel-2 on average, not per pixel. One global gain:
regional (per-quad, per-flight-date) differences are removed only as a smooth 2 km offset field, and
coastal cells with little shared land are extrapolated, not fitted; per-date residuals are reported. NDVI is leaf-on
and dated (NAIP flies in the growing season; the composite is 2021). Outside NAIP it is 10 m Sentinel-2.

`scripts/scene/tools/ndvi_check.py PKG --cache DIR [--overview OUT.png]` computes the plausibility and seam gates.

## Seabed (R1)

**Why.** Probing four transects off the Pendleton coast found that 3DEP 1/3″ fills about 5 km of ocean with a
hydro-flattened plate at about 0 NAVD88. That is 0.2 to 1.1 m below CamSim's tide-0 sea (EGM96); the gap grows
offshore because GEOID18 and EGM96 tilt differently. At 3DEP's coverage edge the terrain drops in a cliff to ETOPO
bathymetry (-8 to -150 m within 500 m). CamSim's ocean material reads the plate as shallow water: that is the
turquoise shelf with the jagged outline (Imagery edge, known limits). With waves (Beaufort 3+) or a negative CIGI
tide the plate can show through the troughs.

**Sources.** `noaa_sd13` (10 m) and `noaa_crm_socal` (90 m) sit below 3DEP and above ETOPO in the terrain priority
(Sources). Where 3DEP is masked, the next valid source takes over: San Diego where it covers, the CRM beyond, ETOPO
past the CRM.

**Tidal-water mask.** A 3DEP sample (`dep3_1m`, `dep3_13`) is invalid, and falls through as nodata does, where all
three hold:

1. WorldCover class 80 (permanent water) at the point, nearest neighbour at full resolution.
2. The 3DEP source value (NAVD88, before the vertical offset) is at most **+1.5 m**. MHW at La Jolla is +1.34 m, so
   1.5 m leaves a margin; lakes and reservoirs above it keep 3DEP's flattened surface.
3. A topobathy source (`noaa_sd13` or `noaa_crm_socal`) is valid at the point.

The existing 30 m feather then blends 3DEP into San Diego on the land side of the water edge. The rule is recorded in
the manifest as `layers.terrain.tidal_mask` (`classes_source`, `class`, `max_navd88_m`, `sources`, `topobathy`), and
the WorldCover hashes join the terrain tiles' inputs hash. `verify --deep` applies the same mask when it recomputes
vertex heights.

**Tile set.** A tile's depth is min(region limit, best covering source's limit). San Diego (z14) lies under 3DEP 1/3″
(z14) and the CRM (z12) under the ring limit (z10), so the tile set should not change: the build is expected to
match the pendleton-ndvi build. The pendleton-ndvi build had 182,602 terrain tiles; the R1 build has 182,794
(+192), see "Measured".

**Older manifests.** A manifest without `tidal_mask` (R0, chunk 1, chunk 2) builds exactly as before. `build --replan`
with the new priorities rebuilds only the terrain: imagery, land cover and NDVI are skipped (unchanged hashes), as
measured in "Measured". Replanning any package, also a non-SoCal one such as the SF sample, rebuilds its terrain once
because the tidal mask setting enters the tile hash; the output is identical.

**Known limits.** WorldCover's water edge (10 m pixels, 2021) is not the instantaneous waterline; the feather absorbs
it. The CRM is accurate to about 1 m and is coarser than the ring's z10 samples (about 150 m). Piers and other thin
features follow their source. Acceptance: coast_check (a)-(c) in "Measured".

## Vector features (R2)

**Why.** Roads are among the most visible features in LWIR and WorldCover has none (a motorway is `built_up` or whatever
its verge is). R2 burns vector features into the package's existing `landcover/` tiles as codes above WorldCover's, so
thermal IR draws them and CIGI reports them as material codes, with no new runtime geometry. Design:
[`superpowers/specs/2026-10-10-r2-vector-features-design.md`](superpowers/specs/2026-10-10-r2-vector-features-design.md).

| Code | Feature | Source | Thermal material (`docs/thermal.md`) |
|---|---|---|---|
| 200 | Paved road or area, asphalt or no/unknown `surface` | OSM | `asphalt` |
| 201 | Paved, `surface=concrete`, `concrete:plates`, `concrete:lanes`; runways without a `surface` | OSM | `concrete` |
| 210 | Rail bed (`railway=rail`, `light_rail`) | OSM | `rail_ballast` (new built-in) |
| 220 | Inland water: perennial lake/pond (FType 390), reservoir (436), stream/river area (460) | NHD HR | `water` |

**Rules** (`camsim_scene/features.py`, `RULES_VERSION` 1):

- Lines are buffered to their width: `width=*` when tagged; runways 45 m, taxiways 23 m; otherwise lanes × 3.5 m plus
  shoulders (3 m on motorways and trunks, 1 m otherwise; 2 lanes when untagged on motorways/trunks, 1 on one-way roads,
  else 2); rail 4 m per track. Every line is at least 1.05 cells (9.74 m) wide, so it covers a cell centre in every
  cross-section: a 7 m road is one texel wide. Polygons (water, paved areas) are burned as they are.
- Cells are 1/12000° (9.28 m N–S), the WorldCover grid; a cell takes a feature when its centre is inside the polygon.
- Priority, later wins: water < paved areas < rail < roads; within one priority, sorted by source id, so the output is
  deterministic.
- Skipped: `bridge`, `tunnel`, `covered=yes`, `location=underground`, negative `layer` (no deck geometry before R3),
  unpaved `surface` values (`unpaved`, `gravel`, `dirt`, `ground`, `compacted`, `fine_gravel`, `sand`, `grass`, `earth`,
  `mud`: left to WorldCover), road classes outside `road_classes`, intermittent/ephemeral water, the sea, estuaries and every
  NHD flowline.
- Features are read over the margin area (bbox + `imagery_margin_km`); tiles in the ring keep WorldCover only.

**Licence.** OSM is ODbL 1.0 (share-alike, attribution "© OpenStreetMap contributors", added to `ATTRIBUTION.txt`).
It isn't in `DEFAULT_ALLOW`: a package with any OSM flag on needs `allow = ["ODbL-1.0"]` (or `--allow ODbL-1.0`), a
deliberate opt-in because the derived land-cover tiles then fall under the ODbL. NHD is public domain.

**Sources.**

- OSM comes from Geofabrik's dated daily extracts (`<extract>-<yymmdd>.osm.pbf`); the fetch cache stores the file by
  sha256, so the downloaded extract *is* the snapshot and the manifest pins it. Dated files appear about two days late
  (the current and previous day return 404), so `plan` probes back up to 7 days for the newest that exists. Reading the
  `socal` extract (639 MB) takes ~68 s and peaks at ~3 GB RSS (pyosmium with node locations), in its own process; size
  CI runners for it. The parsed features are cached per extract and settings, so a rebuild reads them in seconds.
- USGS 3DHP (NHD's successor) has only a CONUS-wide annual file and an ArcGIS service whose bbox queries time out, and at
  Pendleton it is cross-walked NHD anyway. **NHD HR** is retired (static since 2023), so it is reproducible as is; TNM
  serves it per HU4 as GeoPackage (1807 covers Pendleton, 166 MB). `nhd_hr` is replaced by a 3DHP adapter when per-unit
  downloads exist. NHD's NAD83 is taken as WGS 84 (about 1 m, under a texel).

**Off switch.** `[features]` absent or all false: the land-cover layer is byte-identical to a package built before R2
(checked on Pendleton against `pendleton-r1c`).

**Road registration** (`scripts/scene/tools/road_registration.py`, gate 2). The spec's method (a dark box of the road's
width matched along ±30 m perpendicular NAIP profiles) measured vegetation, not roads, at Pendleton: most asphalt there is
*brighter* than its dry verges (84 of 200 centrelines darker than their surroundings), and a single profile locks onto
tree lines and shadows. The gate is instead a **2-D windowed correlation**: the bbox is cut into 1 km windows; in each
window with at least 1 km of paved motorway…residential road (up to 60, seeded), the OSM road-boundary template (edges
of the buffered roads minus their interior) is slid over the smoothed NAIP luminance gradient magnitude by ±20 m at 1 m;
the peak must have contrast ≥ 1.0 DN/m, not lie on the search border, and stand out from the rest of the surface. Gate:
median |shift| ≤ 10 m over the windows used, with at least 20 used windows and half of those tried. Many roads per
window average out tree and shadow edges that defeat single profiles. Limits: a window dominated by one road direction
can slide along it, or onto a parallel carriageway (aperture problem: the p90 tail); a periodic street grid can match one
block off (a strict `xfail` test documents it); the constants were tuned on Pendleton only.

**Known limits** (also in `docs/thermal.md`, "Limits (R2)"):

- Roads are at least one texel (~9 m) wide in IR wherever the verge isn't green. A dry verge inside a road texel is drawn
  as asphalt.
- The land-cover warp moves road edges by up to `warp_amplitude_m` (6 m), so long straight roads may wiggle slightly in IR.
- Bridges and tunnels are not burned. A road on a bridge is drawn as whatever lies below the bridge until R3 adds decks.
- Unpaved roads and tracks are left to WorldCover.
- OSM completeness varies, and OSM inside military bases can be sparse. The registration report shows it.
- Intermittent water is never water, even in a wet season.
- The road codes follow OSM's `surface`, but thermal IR's fast term follows the imagery's brightness: a road that looks
  light in NAIP reads cooler at noon than its class (`docs/thermal.md`, "Limits (R2)"). Most of I-5 at Pendleton is
  `surface=concrete` in OSM (code 201).
- `rail_ballast`'s properties come from material-property reasoning, not a published measurement.
- Road registration: see its limits above.

## Buildings (R3)

**Why.** Buildings are the largest missing structure in the rendered scene and the thing CIGI hosts hit with LOS and
HAT/HOT queries. This is a thin slice of REALISM R3: extruded footprints with flat roofs and one neutral material, so
the realism/cost trade can be measured before investing in roofs, facades and bridges. Design:
[`superpowers/specs/2026-10-10-r3-buildings-slice-design.md`](superpowers/specs/2026-10-10-r3-buildings-slice-design.md).

**Config.** A `[buildings]` table (scene.toml reference) turns the layer on; without the table the package, and its
`manifest.json`, are byte for byte what they were before R3.

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `true` | `false` is the same as no table |
| `release` | `"2026-09-23.1"` | Overture release to read |
| `min_area_m2` | `10` | footprints (parts of a multipolygon, after cleaning) below this are dropped |
| `roof_textures` | `true` | per-tile roof atlas from the imagery sources; `false` = R3's flat grey roofs |
| `roof_texel_m` | `0.5` | atlas texel, m, [0.25, 2.0] |
| `roof_texture_max_px` | `2048` | largest atlas side; 256 … 8192, a power of two |

Overture's buildings theme is **ODbL-1.0**, which is not in the default allow-list: a package with `[buildings]` fails
to plan without `allow = ["ODbL-1.0"]` (as with the R2 OSM features). `ATTRIBUTION.txt` carries Overture's credit and the
derived-database notice. The layer is a derived database under the ODbL.

**Snapshot.** Overture deletes releases after 60 days, so the first `fetch` runs one DuckDB query over the bbox against
`s3://overturemaps-us-west-2/release/<release>/theme=buildings/type=building/*` (anonymous; columns `id`, `geometry`,
`height`, `num_floors`, `class`, `subtype`, `sources`) and writes the result as GeoParquet in the fetch cache, keyed by
(release, bbox). Its sha256 is in the manifest's `sources[]`. A build reads only the snapshot, never S3, so a rebuild
needs no network; a missing snapshot with a deleted release fails naming the release.

**Footprints to buildings.**

- *Clean:* `make_valid`, parts smaller than `min_area_m2` dropped, duplicate Overture ids dropped; a multipolygon is one
  building with several parts. A footprint with no bare-earth height under it is dropped (counted in the layer stats).
- *Height:* Overture `height`, else `num_floors` × 3.2 m, else 4.0 m; clamped to [2.5, 150] m. `height_source` is
  `overture_height`, `overture_floors` or `default`.
- *Base:* the lowest bare-earth DEM height over the footprint's exterior vertices and its centroid, as a WGS-84 ellipsoid
  height (through the terrain layer's own height sampler, so datums match the terrain tiles), minus 0.5 m so walls reach
  the ground on slopes. Roof = base + 0.5 m + height.
- *Mesh:* roof by constrained Delaunay triangulation (holes kept); walls one quad per ring edge with outward normals; flat
  shading; no floor. Linear `baseColorFactor` roof (0.30, 0.30, 0.31), walls (0.42, 0.41, 0.40), metallic 0, roughness 0.9.

**Tiers and geometric errors.** Tiles use the geographic scheme of the package's terrain (EPSG:4326, 2 × 1 roots,
`tiling.tile_bounds`). A building lives in exactly one tile, chosen by footprint area: >= 2,000 m² at z13, >= 300 m² at
z14, the rest at z15; the tile is the one containing the footprint centroid. Refinement is `ADD`, so large buildings
stay visible from far away and small ones load close in. Empty tiles are omitted, and a content-less ancestor exists
where a descendant has content. Geometric error: root 200 m, z13 60 m, z14 20 m, z15 6 m (z15 nodes have no children).
They are starting values for the SSE sweep: changing them (or any rule above) is a package rebuild; changing
`scene.buildings.maximum_screen_space_error` is not. Each tile's bounding volume is a `region` from its buildings' real
extents.

**Draw distance outside detail polygons (REALISM R5).** In a package with `[detail]` polygons, every z13 node whose
region misses all `detail_margin:*` regions sits under content-less z12 → z9 ancestors (no transform, region = union of
their children) with geometric error 150 m (`buildings.OUTSIDE_GEOMETRIC_ERROR`). Cesium refines them only within
about 2 km of the camera at the default SSE 64, so the box's buildings outside the polygons cost game-thread time only
near the camera; inside the margins the tree is the one above. A package without `[detail]` keeps the plain tree, byte
for byte. Measured on `socal-r5` (R5 detail polygons below).

**Build by chunk.** The layer never holds the whole snapshot (spec
[`superpowers/specs/2026-10-14-buildings-chunked-design.md`](superpowers/specs/2026-10-14-buildings-chunked-design.md)).
A building's chunk is the z12 tile (0.0439°, ~4.9 × 4.1 km at 33° N) of its cleaned centroid, so every z13–z15 tile
and every height group lies in one chunk. The snapshot is staged in the worker pool (100,000 rows per task) and
written as one Parquet sorted by chunk (`.state/buildings/partition/`, a DuckDB `POSITIONAL JOIN` at 500 MB / 2
threads); then each chunk is planned (`.state/buildings/plan/<cx>_<cy>.pkl`, resumable chunk by chunk), rasterised
(OR-merged into `.state/buildings-footprints/`, one `.npy` per land-cover tile) and written as GLBs in the pool. The
main process writes `tileset.json` from per-tile summaries. The output is byte-identical to the whole-box build
(`pendleton-r5veg` rebuilt: `hashes.txt` identical). `verify --deep` replans chunk by chunk and never reads the build's
plan.

**GLB layout.** Per tile: positions float32 in a local ENU frame at the tile's centre, glTF y-up (the 3D Tiles
y-up to z-up convention), so the tile's `transform` = inverse(parent's world transform) x ENU-to-ECEF at that centre
(transforms compose down the tree). One feature id per building per vertex (`EXT_mesh_features`, `_FEATURE_ID_0`), and an
`EXT_structural_metadata` property table `building` in the schema `camsim_buildings`: `class` (string), `height_m`
(float32), `height_source` (enum `heightSource`), `source_id` (string, the Overture id). An Overture `class` column that
is empty for every building of a tile is omitted from that tile's property table (glTF forbids zero-length buffer
views), so a reader treats a missing `class` as `""`. The metadata is written so the thermal deepening pass needs no
rebuild; the runtime does not read it yet. The writer is in-house (`camsim_scene/gltf.py`, numpy + struct) and
deterministic: buildings sorted by Overture id, fixed JSON formatting, no timestamps, so rebuilds are byte-identical.

**A bbox with no buildings.** If no building survives planning, the layer writes no tileset and no `features/buildings/`
directory; `verify` accepts that, and the runtime treats the package as having no buildings layer.

**verify.**

| Check | Passes when |
|---|---|
| `buildings_tileset` | (quick, when `tileset.json` exists) every `.glb` it lists exists; their hashes are covered by `hashes` |
| `buildings_heights` | (`--deep`, decodes every tile) the building ids and tile assignment match a recomputation from the snapshot and the package's terrain sources exactly, each roof's top is within 0.01 m of the recomputed roof (float32 positions relative to the tile centre), and no tile holds an unexpected or misses an expected building |

The test suite also runs CesiumGS `3d-tiles-validator` 0.6.1 (Node, pinned in `scripts/scene/tests/validator/`) on a
fixture tileset, and skips that test when `npx` is missing. The validator's `.npmrc` sets `ignore-scripts=true`
(`better-sqlite3` does not compile on current Node), so the first run does `npm ci` and then
`npm rebuild sharp --ignore-scripts=false`; run those two commands by hand to prepare it.

**Runtime.** `CAMSIM_SCENE_DIR` pointing at a package with `features/buildings/tileset.json` sets the buildings URL
(`file:///`); `scene.buildings.*` (`docs/configuration.md`, "Scene Package") tunes it.

- One extra `ACesium3DTileset`, tagged `CamSimBuildings` and found by that tag, is spawned at world init in
  `SetUpCesiumWorld` right after `ApplyCesiumBackendConfig`'s destroy loop (`Geospatial/SceneBuildings.h`): the same
  tuning as the terrain, `scene.buildings.maximum_screen_space_error` and `create_physics_meshes`, no raster overlays.
- It is in the tileset cache, so `/ready` waits for it (frames never ship without buildings) and the FOV-scaled culled
  SSE covers it; the culled SSE is max(terrain-derived, the buildings' own SSE), never finer than configured. The
  frame-stats `Sse` keeps reporting the terrain tileset.
- CIGI LOS and HAT/HOT hit it (with physics meshes) and report material code **230** (`docs/terrain-feedback.md`).
- The DIS surface clamp ignores it (`Entity/SurfaceProbe.cpp`): footprints are misregistered by a few metres in places and
  a truck on a road beside a building must not climb onto the roof. Ground vehicles clamp to terrain.
- Thermal IR: burned cells (code 230) take the `building_roof` class (R3b, below); a building over a cell less than half covered still takes that cell's class.
- `scene.buildings.enabled: false`, or a package without the layer, is R2 bit for bit.

**Known limits.**

- Roofs are flat (no pitch), walls one neutral colour, no facades, no bridges; the R3b limits are at the end of the next
  subsection.
- Footprints and heights are only as good as Overture. At Pendleton (release 2026-09-23.1), 73.2 % of the 82,260 kept
  buildings have an Overture height (OSM-mapped heights inside the base are dense), 0.5 % a floor count, and 26.3 % take
  the 4 m default.
- Imagery shows roofs at their NAIP position; where Overture is offset, the roof texture and the building disagree.
- Overture's ODbL terms apply to the layer and to anything derived from it.

### Footprint mask, land-cover burn and roof atlas (R3b)

Design: [`superpowers/specs/2026-10-11-r3b-buildings-look-design.md`](superpowers/specs/2026-10-11-r3b-buildings-look-design.md).
All three come from the buildings the layer keeps (same filter, same `source_id` order), planned once per build and
cached under `.state/` (`buildings-plan.pkl`, `buildings-footprints.npz`, keyed by the snapshot, `min_area_m2`, the
rules and the terrain).

- **Footprint mask** (`features/buildings/mask/`, format `camsim-buildings-mask-1`): the land-cover tile grid at 8 x its
  resolution, 0.05° tiles of 4800 x 4800 cells (~1.16 m x 0.97 m at 33° N) named by their SW corner, row 0 = north,
  1-bit PNG. A cell is set when its centre is inside a footprint (holes excluded). Tiles without a set cell are not
  written; `index.json` lists the others with `tile_px`, `cell_factor` and `rules`. CamSim's tree scatter reads it
  (`docs/vegetation.md`).
- **Land-cover burn**: a land-cover cell becomes **230** (`building`) when at least 32 of its 64 mask cells are set.
  Order, later wins: WorldCover, water, paved areas, buildings, rail, roads (buildings overwrite parking lots and
  structures; a road or rail burned over a misregistered footprint stays road or rail). `index.json` `codes` gains
  `"230": "building"` and `credits` the Overture notice. CIGI terrain hits in those cells report 230, and thermal IR
  gives them the `building_roof` class.
- **Roof atlas**: each GLB carries one square power-of-two JPEG (quality 85, 4:2:0, ≤ `roof_texture_max_px`) of its roofs,
  sampled from the imagery layer's sources and colour match (not from the built JPEGs) at `roof_texel_m` over each
  roof's box in the tile's ENU frame, with a 2 px edge-extended gutter, packed by height then `source_id`. When the
  rectangles do not fit, the tile's texel steps 0.5 → 0.75 → 1.0 → 1.5 → 2.0 m; the tile's root `extras.roof_texel_m`
  records it (`null` when every roof is flat). A roof keeps the flat grey material when its best source (the one
  under most of its triangles) is coarser than 2 m (no NAIP), or when it does not fit even at 2.0 m. Walls stay flat.
  Roof vertices carry `TEXCOORD_0`; wall and flat-roof vertices (0, 0). Cesium's default material samples the texture;
  nothing changes at runtime. `build.json` `buildings.roofs` counts textured and flat roofs and tiles per texel.
- **Hashing and reproducibility**: the landcover layer's inputs gain the buildings snapshot, the buildings settings hash,
  the buildings rules version and the footprint raster's key, and its settings a `buildings` entry, only with
  `[buildings]`: packages without it are byte-identical to R3 (the landcover `RULES_VERSION` is not bumped). Because the
  buildings settings hash includes the roof keys, a roof-only change (`roof_texel_m`, `roof_textures`,
  `roof_texture_max_px`) re-cuts the land cover too, with byte-identical output, and rebuilds the GLBs but not the mask.
  Roof textures add the imagery sources, settings and colour match to the buildings layer's inputs. The atlas JPEG bytes
  are deterministic for a given Pillow / libjpeg build, so use the pinned build container (`scripts/scene/build.sh`)
  when a byte-identical package matters. A bbox that keeps zero buildings writes no tileset and no mask, but still
  credits Overture in `ATTRIBUTION.txt` and the land-cover `index.json`.
- **verify**: the mask tiles are files of the package, so `hashes` covers them. `--deep` adds `buildings_mask` (every
  land-cover cell against the mask: 230 exactly where ≥ 32 of 64 mask cells are set, unless a road or rail was burned
  over it) and `buildings_roofs` (every tile's `roof_texel_m` is on the texel ladder, and its texture is a square
  power-of-two JPEG within `roof_texture_max_px` with texcoords in [0, 1]).

Known limits (R3b): NAIP is not a true orthophoto, so tall buildings' roofs carry their lean (fine for low-rise, not
corrected); roof texture is only as good as NAIP (0.6 m, one date, shadows baked in); a 9 m land-cover cell is
"building" only when at least half covered, so small buildings keep their WorldCover class in IR; walls stay flat
coloured and roofs flat (no pitch); no wall textures, no KTX2 / Basis compression.

Build memory headroom (R3b): the buildings layer peaked at 1,988 MB of the 2,048 MB `--max-worker-rss-mb` limit on Pendleton at
`roof_texel_m` 0.5 (1,588 MB at 1.0; R3 was 1,630). A larger site may exceed the limit; raise it with
`camsim-scene build ... --max-worker-rss-mb N` (MB per worker, default 2048, `scripts/scene/camsim_scene/cli.py`) or lower `roof_texel_m`.

## Bridges (R3)

The terrain is bare earth: 3DEP removes bridge decks, so without this layer a road or railway over a river or valley
drops to the valley floor in the render, in CIGI HAT/HOT and for DIS vehicles. `[bridges]` (spec
`docs/superpowers/specs/2026-10-15-bridges-ripples-design.md`) builds decks from the same OSM extract as
[Vector features (R2)](#vector-features-r2), so it needs `[features]` with `roads` or `rail` (and `"ODbL-1.0"` in
`allow`; the layer's glTF copyright carries the ODbL notice).

- **Which ways:** R2's roads (its `road_classes`) and rail (`rail`, `light_rail`) tagged `bridge` (any value but `no`),
  not tunnels, `covered=yes` or `layer < 0`.
- **Heights:** approach embankments are ground, so the bare-earth terrain at a bridge's ends is the road's height. Way
  ends that meet no other bridge way are *grounded* at the terrain there (the same exact-offset sampler the buildings
  and `verify --deep` use, WGS-84 ellipsoid); ends where bridge ways join (a ramp on a viaduct) take the
  1/length-weighted mean of their neighbours (one linear solve per connected network). Height is linear along each
  way. A way never more than 2 m above the terrain (sampled every 5 m) is dropped as *low*: the terrain already
  carries it (culverts, small creek crossings). A network with no grounded end is dropped (`dropped_ungrounded`).
- **Geometry:** one cross-section swept along the densified centreline (stations every ≤ 10 m, mitred joins capped
  at 2×): a 1.5 m deck slab, the way's R2 width (`width` tag, else lanes × 3.5 m + shoulder; rail 4 m per track), a
  0.3 × 1.0 m parapet on each side, end caps. Piers (1.2 m along × 0.6 of the width) every 30 m, centred, at least 10 m
  from the ends, where the deck bottom is at least 3 m above the terrain, down to 0.5 m below it. Colours: asphalt
  driving surface (ballast on rail), concrete everything else. No imagery texture: NAIP is orthorectified to bare
  earth, so a deck's image is displaced by metres.
- **Tiles:** `features/bridges/tileset.json` (3D Tiles 1.1, ADD): root (geometric error 100000, always refined) →
  content-less z10 groups (geometric error 700: a group's decks draw within ~10 km of it at SSE 64, 1080p, 60° FOV) →
  one GLB per z13 tile holding the decks whose midpoint is in it (geometric error 0, own ENU transform).
  `EXT_structural_metadata` class `bridge` (schema `camsim_bridges`): `source_id` (OSM way), `kind` (`road` / `rail`),
  `max_clearance_m`.
- **Land cover:** with `[bridges]`, bridge ways are burned into land cover like other roads and rail (200 / 201 / 210;
  R2 skips them otherwise), so thermal IR reads the deck as road, not the river under it, CIGI reports the road code
  on a deck, and no trees grow through it. Known limit: the strip *under* a deck reads as road too (seen only
  obliquely under the deck).
- **Build:** in the main process after the buildings (hundreds of ways; Pendleton: 2 s, 602 MB peak), heights read
  per 0.05° cell; skipped when its inputs (settings, features cache key, terrain, credit) are unchanged. Stats in
  `build.json` `bridges` (`input`, `kept`, `dropped_low`, `dropped_ungrounded`, `dropped_no_height`). Without
  `[bridges]` every layer is byte-identical to a package built before the layer existed.
- **verify:** `bridges_tileset` (every GLB the tileset names exists); `--deep` adds `bridges_heights` (each deck's
  driving surface spans its re-planned end heights within 0.01 m, every deck present, in its tile).
- **Runtime:** `scene.bridges.*` (`docs/configuration.md`): one more tagged tileset (`CamSimBridges`); DIS ground
  vehicles drive onto decks from their approaches but are never lifted onto one above them (`docs/dis.md`, "Bridge
  decks"). Acceptance: `scripts/scene/tools/bridges_check.py` (below, "R3 bridges").

Not covered: bridge types and structure (truss, arch, suspension towers), bridges missing from OSM, tunnels.

## Canopy (R4)

`[canopy] enabled = true` adds `canopy/`: canopy cover and canopy height from Meta/WRI **CHMv2** (source id `chmv2`,
CC BY 4.0, on the default allow-list), which CamSim's vegetation scatter turns into tree density and size
([`vegetation.md`](vegetation.md)).

```toml
[canopy]
enabled = true
```

- **Source.** One GeoTIFF per level-10 web-mercator quadkey
  (`https://dataforgood-fb-data.s3.amazonaws.com/forests/v2/global/dinov3_global_chm_v2_ml3/chm/<quadkey>.tif`): uint8
  metres, EPSG:3857 at 1.194 m (≈ 1.0 m on the ground at 33° N), 512² tiles, deflate, ≈ 141 MB each. `plan` HEADs every
  quadkey over the package `bbox` (a missing key, e.g. open ocean, is skipped) and records each tile's acquisition date
  range from `metadata/<quadkey>.geojson` (`acq_date`); `fetch` downloads whole files into the cache.
- **Layer** (`camsim-canopy-1`): two tile sets on the land-cover grid (0.05° tiles, 600 × 600 8-bit greyscale PNG, row
  0 north, the same file names): `canopy/cover/` (percent of the CHM pixels ≥ 3 m whose centres fall in the texel) and
  `canopy/height/` (the nearest-rank p90, `sorted[floor(0.9 (n − 1))]`, of those pixels, metres), each with an
  `index.json` (`format`, `band`, `units`, `tile_deg`, `tile_px`, `bbox`, `tiles`, `acquired`, licence and attribution),
  plus `canopy/ATTRIBUTION.txt`. Tiles cover the package `bbox` only (not the 100 km land-cover ring): outside it CamSim
  falls back to the land-cover rule. A texel no CHM file covers is 0 / 0.
- **Rebuild:** skipped when the settings, the CHM files and the credit are unchanged (`.state/canopy.json`); tiles run
  on `-j` worker processes (a tile reads ≈ 26 M CHM pixels).
- **verify** (`canopy`): both indexes parse with the same tile list, every tile is a 600 × 600 mode-L PNG, cover ≤ 100.
- Attribution: "Meta and World Resources Institute (WRI), 2026. Version 2 High Resolution Canopy Height Maps (CHMv2),
  https://registry.opendata.aws/dataforgood-fb-forestsv2. Source imagery for CHM © 2016 Vantor" (the registry's "How
  to Cite").

## Vegetation mix (R4)

An optional `[vegetation]` table chooses which vegetation library types stand on each land-cover code, and how many
shrubs per 100 m² (guide: [`vegetation.md`](vegetation.md)):

```toml
[vegetation]
mix.10 = { tree_broadleaf_a = 1.0 }                          # code = { library type = weight, ... }
mix.20 = { shrub_chaparral = 0.8, shrub_desert = 0.2 }
mix.30 = { shrub_chaparral = 0.5, shrub_desert = 0.5, tree_broadleaf_a = 0.05 }
shrub_density.20 = 8.0                                        # shrubs per 100 m^2 of that code, [0, 100]
shrub_density.30 = 1.0
```

- Codes are land-cover codes 1..255 (WorldCover's and the burned R2/R3b codes); type names match `[a-z0-9_]+`, weights
  are > 0. A code may list trees and shrubs: CamSim splits them by the library's roles and normalises each list. A
  code with shrubs but no `shrub_density` gets 5 per 100 m².
- `build` writes it, codes sorted numerically and names alphabetically, to `vegetation/mix.json`
  (`{"format": "camsim-vegmix-1", "mix": ..., "shrub_density": ...}`), listed in `hashes.txt`; without the table no
  file is written (and a stale one is removed). `verify` checks the file equals the manifest's settings
  (`vegetation_mix`).
- The tooling doesn't know the library: a type name CamSim's library lacks is a runtime warning (the type dropped, the
  code renormalised), and with `scene.offline: true` CamSim exits 1.
- Without a mix, CamSim's built-in default applies: code 10 → `tree_broadleaf_a` (R4's trees bit for bit), 20 and 30 →
  `shrub_chaparral` at 6 and 1 per 100 m².

## Detail polygons (R5)

A `[detail]` table builds named polygons at `sim` inside a box built at `preview`, so a ~200 km × 200 km area stays
buildable: NAIP and 3DEP 1 m are fetched and built only for the polygons (spec
`docs/superpowers/specs/2026-10-14-r5-detail-polygons-design.md`). Example: `examples/socal-r5.toml` (200 km around
Camp Pendleton, the Pendleton bbox as the one polygon).

- **The file:** a GeoJSON FeatureCollection; each feature has a unique `properties.name` (`[a-z0-9][a-z0-9_-]{0,63}`)
  and a valid Polygon or MultiPolygon in lon/lat (a `crs` other than EPSG:4326 / CRS84 is an error) inside the bbox.
  `plan` snaps coordinates to 1e-7°, normalises the rings, sorts by name and copies the polygons into the manifest
  (`detail`: profile, polygons as WKT, sha256). Builds and `verify` read the manifest only; edit the file and re-plan
  with `--replan` (`build --config` without it refuses: the regions differ).
- **Regions:** per polygon (per part of a MultiPolygon, named `<name>/<i>`), `detail:<name>` at the detail profile's
  zooms and `detail_margin:<name>` (grown by `imagery_margin_km`) at its imagery zoom, as `bbox` and `margin` are for
  the box. The box keeps `preview`'s zooms (14 / 13). Defaults (priorities, `ndvi`) come from the detail profile.
- **Scoped sources:** sources in the detail profile but not in the box's (`dep3_1m` for terrain, `naip_pc` for
  imagery) are limited to the polygons: terrain sources to `detail:`, imagery sources to `detail_margin:`.
  `[detail] scoped` names them explicitly. They are discovered once per polygon, their footprints are clipped to
  the scope at `fetch`, and every tile job ignores their samples outside it, so NAIP's feather and colour match
  (Imagery edge) and the terrain feather work at the polygon edges as they do at the bbox edge.
- **NDVI** defaults on (from the detail profile) and covers the whole ring from Sentinel-2, with NAIP's values (fitted
  onto Sentinel-2) inside the polygons; set `ndvi = false` to skip the whole-box layer.
- **Whole box:** land cover with the R2 burns and building footprints (230), the buildings tileset and mask, canopy
  and the vegetation mix cover the bbox as usual. Roof atlases need a ≤ 2 m source, so roofs outside the NAIP scope
  are flat grey.
- **`verify`** adds a `detail` check: the hash matches the polygons, and every scoped footprint lies in its scope.
- **Gate tools:** `tools/detail_check.py PKG --reference REF` (no tile deeper than the box's zooms outside the scopes;
  leaves inside the polygon shrunk by 1 km byte-identical to a full-`sim` package of the same area, imagery leaves near
  water exempt and listed), `tools/edge_shots.py --crop NAME:Z:W:S:E:N` (imagery crops at a polygon's edge) and
  `tools/render_check.py --shots detail` (CamSim shots across the Pendleton polygon's edges and outside it).

## Sea level (R1)

**Why.** CamSim's tide-0 sea is the EGM96 geoid. Local mean sea level at La Jolla is +0.774 m NAVD88 (NOAA CO-OPS
9410230, 1983-2001 tidal epoch), which is only +0.110 m above EGM96 at La Jolla but 0.3 to 0.66 m above it along
the Pendleton coast (see "Limit"). The drawn waterline sat 10 to 20 m seaward of local
MSL on gentle beaches. (NOAA's CUSP shoreline is MHW, +1.344 m NAVD88, not MSL.)

**`[sea_level] station`.** A CO-OPS station id with a NAVD88 datum. Pendleton uses La Jolla: Oceanside Harbor
(9410396) is tidally identical but has no NAVD88 tie. A station without a NAVD88 datum is a `plan` error naming the
problem. Any package whose bbox or 100 km ring meets the CRM (`noaa_crm_socal`, lon -128 to -115, lat 30 to 37, which
includes land) needs a station, inland ones too: `plan` fails with a config error otherwise. Only a package with no
`noaa_crm_socal` coverage (outside the CRM, or the source removed from the priorities) can leave the table out; a
`--bbox` scene in Southern California needs the table added to its generated `<name>.scene.toml`.

**Manifest snapshot.** `plan` reads the station's `datums.json` and position from the CO-OPS API and stores them in
`manifest.json` under `sea_level`: `station`, `name`, `lat`, `lon`, `epoch`, `msl_m`, `navd88_m`,
`msl_above_navd88_m`, `geoid` and the EGM96 grid (`us_nga_egm96_15.tif`, stored under `sea_level.egm96_grid`, not in `datum.grids`). The values are
stored as published, so a later rebuild does not query the API. `noaa_crm_socal` takes `msl_above_navd88_m` from it.

**`sea_level.json`.** The build writes, at the station's position (pinned grids, `PROJ_NETWORK=OFF`):

```
offset_m = h_ell(NAVD88 0, GEOID18 chain) + msl_above_navd88_m - N_EGM96
```

with `N_EGM96` bilinear on the 15′ NGA grid, the same data as CamSim's `WW15MGH.DAC`. The file holds `offset_m`,
`station`, `msl_above_navd88_m`, `navd88_ellipsoid_m` and `egm96_n_m`, is listed in `hashes.txt`, and is rewritten on
every build. `verify` (the `sea_level` check, deep) recomputes it and compares to within 1 mm.

**In CamSim.** With a scene package, the sea level is EGM96 + `offset_m` + the CIGI tide: the ocean mesh, HAT/HOT, the
boat clamp, the material parameters and wave queries all use it. `offset_m` outside [-3, 3] m, or not finite, is a
resolve error; no file means 0. See "Scene Package" in [`configuration.md`](configuration.md). KLV Tags 15 and 25
stay EGM96 MSL by definition.

**Limit.** One constant per package, evaluated at the station. It is exact there and wrong along the coast: the offset
local MSL needs (same MSL - NAVD88, GEOID18 and EGM96 evaluated at the point) runs from +0.08 m at Oceanside to +0.56 m
at San Onofre and +0.66 m at the Dana Point end of the ring, against the +0.110 m shipped (La Jolla). The cause is
EGM96's own tilt against an almost flat GEOID18 (N goes -34.83 to -35.35 m along 25 km), not sea-surface topography.
So the north half of the Pendleton coast sits 0.3 to 0.47 m below local MSL. Follow-up (REALISM.md, ROADMAP.md):
evaluate the offset at the package coast, or add a per-package NAVD88 + MSL - EGM96 correction grid read by
`FOceanSurface`. Acceptance: coast_check (d) and the sea rows of
hot_check in "Measured".

## Sources

| Id | Dataset | Licence | Area | Zoom limit | Datum (horizontal / vertical) |
|---|---|---|---|---|---|
| `dep3_1m` | USGS 3DEP 1 m project DEMs (10 × 10 km UTM tiles, via The National Map) | `LicenseRef-PublicDomain-USGov` | US, where a 1 m project exists | z16 | project CRS (NAD83(2011)) / NAVD88 with the project's geoid (GEOID18 or GEOID12B) |
| `dep3_13` | USGS 3DEP 1/3 arc-second DEM (1 × 1° GeoTIFFs, dated `historical/` URLs) | `LicenseRef-PublicDomain-USGov` | US | z14 | NAD83(2011) geographic / NAVD88, GEOID18 |
| `noaa_sd13` | NOAA NGDC San Diego, CA 1/3 arc-second NAVD 88 tsunami-inundation DEM (2012), one netCDF, 445,596,180 B | `LicenseRef-PublicDomain-USGov` | lon -117.83 to -117.00, lat 32.45 to 33.60 | z14 | NAD83(2011) / NAVD88, GEOID18 (the file names no geoid; models differ by centimetres) |
| `noaa_crm_socal` | NOAA NGDC U.S. Coastal Relief Model, Southern California v2, 3 arc-second (90 m), one netCDF, 524,449,536 B | `LicenseRef-PublicDomain-USGov` | lon -128 to -115, lat 30 to 37 | z12 | NAD83 / **MSL**; the package's station MSL - NAVD88 is added, then as NAVD88 with GEOID18 |
| `etopo2022` | NOAA ETOPO 2022 30″ surface elevation (land + bathymetry) + its geoid-height grid | `LicenseRef-PublicDomain-USGov` | globe | z8 | WGS 84 / EGM2008 via ETOPO's own geoid grid |
| `naip_pc` | USDA NAIP 0.6 m RGB(N) COGs, Microsoft Planetary Computer STAC | `LicenseRef-PublicDomain-USGov` | conterminous US (to a few km offshore) | z17 | NAD83 UTM (from the file) / — |
| `wc_s2` | ESA WorldCover 2021 Sentinel-2 RGBNIR composite (10 m, 1 × 1° COGs) | `CC-BY-4.0` (checked against each file's licence tag) | land (no open-ocean tiles) | z13 | WGS 84 / — |
| `bmng` | NASA Blue Marble Next Generation, topography + bathymetry, 500 m (2004 monthly) | `LicenseRef-PublicDomain-USGov` | globe | z8 | WGS 84 / — |
| `worldcover` | ESA WorldCover 10 m 2021 v200 land cover (3 × 3° COGs) | `CC-BY-4.0` | land | — (0.05° land-cover tiles over bbox + ring) | WGS 84 / — |
| `osm_geofabrik` | OpenStreetMap, Geofabrik extract, one dated daily `.osm.pbf` (`[features]` roads / paved areas / rail) | `ODbL-1.0` (not in the default allow-list) | Geofabrik extract | — (burned over bbox + `imagery_margin_km`) | WGS 84 / — |
| `overture_buildings` | Overture Maps buildings theme, pinned release, one bbox query snapshotted as GeoParquet (`[buildings]`) | `ODbL-1.0` (not in the default allow-list) | global | — (buildings over the bbox) | WGS 84 / — (heights are above ground; the base comes from the package's terrain) |
| `nhd_hr` | USGS NHD High Resolution, HU4 GeoPackage from The National Map (`[features]` water) | `LicenseRef-PublicDomain-USGov` | US | — (as above) | NAD83 (EPSG:4269, taken as WGS 84: about 1 m) / — |

**NOAA topobathy.** Both are single files from `ngdc.noaa.gov/thredds/fileServer/` (`regional/san_diego_13_navd88_2012.nc`,
`crm/crm_socal_3as_vers2.nc`), unchanged since 2016 and 2018; they are pinned by URL and size, and `fetch` records the
sha256. NCEI's HEAD has no Content-Length, so `discover` takes the size from a 1-byte ranged GET. `discover` returns
nothing when the area misses the product's extent. `prepare` converts each netCDF to a COG with overviews. Nodata is
-99999 (San Diego) and the file's `_FillValue` (CRM). The CRM is 90 m with about 1 m vertical accuracy, and whole
metres near shore; it is MSL-referenced, which is why a station is required. Both products are **not for
navigation**. NOAA's CUDEM would be the first choice, but has no Southern California tiles (checked 2026-10-10: S3
listings, both tile indexes, NCEI THREDDS), so there is no adapter. Attribution: NOAA National Geophysical Data
Center (2012) for both; the CRM adds doi:10.7289/V5V985ZM.

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
| NOAA San Diego 1/3″ + CRM Southern California | 0.45 GB + 0.52 GB |
| WC S2 composite | ≈ 0.5 GB per 1° cell; ≈ 9 cells for Pendleton + 100 km |

The global base (ETOPO, Blue Marble) is shared by every package that uses the same cache. `plan` prints the
estimate for the scene at hand before anything is downloaded. Before `fetch` the tile counts in that line come from
each source's discovery bounding box, so they are an upper-bound estimate (Pendleton `sim`: terrain 189,410, imagery
277,514); the build uses the fetched footprints, so its counts differ (the pendleton-ndvi build had 182,602 terrain tiles).

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
| `ndvi_tilemapresource` | (packages with NDVI tiles) `ndvi/tilemapresource.xml` levels and BoundingBox equal the NDVI tiles present |
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
| `ndvi_values` | 64 pixels of each sampled NDVI leaf (same 2 % / `--all` sampling) against NDVI recomputed from the sources at the pixel centre (fit applied to NAIP); pixels purely NAIP or purely Sentinel-2 (feather pixels left out): p99 ≤ 1 code step in each group, and no data where the sources have none |

`verify.json` records the height errors overall (`terrain_error_m`: `n`, `p50`, `p99`, `max`), over the scene zooms
(`terrain_error_m.scene`, the gate numbers; `base_max_zoom` is where the base stops) and per zoom
(`terrain_error_m.by_zoom`, with `p99_limit` on base zooms), and the `terrain_heights` line names the worst zoom. A check that can't run on
a malformed package (a missing `layer.json`, an unparsable `hashes.txt` line, a corrupt tile) fails with the error
as its detail instead of aborting `verify`. `ndvi_error_steps` records the NDVI code errors (`n`, `p50`, `p99`,
`max`, `leaves`, and `naip` / `s2`: `n` and `p99` per group).

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
  package, as `file:///` URLs. A package with `features/buildings/` also gets its buildings tileset (Buildings (R3),
  `scene.buildings.*`; `CAMSIM_SCENE_BUILDINGS_ENABLED=0` leaves it out). The vegetation scatter reads
  `vegetation/mix.json` (species per code) and `canopy/` (tree density and size; `vegetation.canopy_enabled`) when
  present (R4, [`vegetation.md`](vegetation.md)). Reference: `docs/configuration.md`, "Scene Package".
- `scene.offline` (`CAMSIM_SCENE_OFFLINE`) refuses any network source and exits with status 1 on any config
  error, instead of falling back to Cesium ion.
- The buildings tileset is spawned after that clean-up, in the same world-init step; under `scene.offline` it needs an
  existing local `file:///`.
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
| Review | Screenshots (visual gate 4) | **Pass** (reviewed by the user, 2026-10-09). Residuals are the known limits above (pier disc; shallow shelf from the ocean, present in R0; fixed in R1, see below) |

The first R1 build (before the water mask) failed the visual gate over sea: raw NAIP quarter-quads over open water
and a land-fitted match that darkened Sentinel-2 water ([64,85,90] -> [1,24,43]). That led to the water mask.

### R1 chunk 2 (NDVI layer, gridded fit, sun metadata)

Camp Pendleton `sim`, acceptance 2026-10-10 (M1 Pro / macOS native, `-j 6`). Run 1 (`pendleton-ndvi`, global fit
only) failed the seam gate; the gridded offset was added (Task 9, 2026-10-10, user decision) and run 2 (final code,
head d4b16b8) is the accepted run. Both start from a copy of the chunk-1 package `pendleton-r1b` made with
`cp -cR` (APFS clone; `cp -a` ran at ~200 MB/h).

| Gate | What | Result |
|---|---|---|
| Build | `--replan -j 6` on the chunk-1 copy: terrain, imagery and land cover untouched, NDVI built | **Pass** (run 2). Wall 277 s, peak RSS 4.0 GB (parent + pool). Terrain 182,602, imagery 277,286 and land cover 2,288 tiles all skipped. NDVI: 7,043 tiles / files, 264.1 MB (264,103,840 B), 75.8 s (~93 tiles/s), peak worker RSS 690 MB; fit 25.6 s. z15 4,972 tiles / 184.4 MB. Run 1 was the same (74.4 s, 264.1 MB, fit 24.7 s) |
| Fit | Held-out NAIP vs Sentinel-2 NDVI: before, global, after the match; bias per date | **Pass** (run 2, `fit.json` format 2: gain 0.92666, offset 0.328702, 24x25 grid of 2 km cells, 168 of 432 filled, cell offset -0.1525 … +0.0833). Held-out MAE 0.3238 before, 0.0801 global, **0.0687** after (< global); bias 0.3297 -> **0.0006**. Per date: 2022-04-25 0.0000 (n 581,436), 05-12 -0.0022 (n 883,610), 05-30 +0.0010 (n 6,310,469); none beyond 0.03. Run 1 (global only): MAE 0.0801, bias 0.006, but 04-25 -0.0711 and 05-12 -0.0494 |
| Sun | NAIP acquisition date and sun window per asset | **Pass.** 44 `naip_pc` assets on 2022-04-25 / 05-12 / 05-30; solar noon elevation 70.16-70.23 / 74.75-74.94 / 78.32-78.70 deg (azimuth 180); windows ~[93.3, 266.7] / [87, 273] / [82.2, 277.8] deg; `min_elevation_deg` 30. The plan expected 76-79 deg; actual is 70-79 (the April date is lower), same in both runs |
| Seam | `ndvi_check.py`: mean NDVI difference across NAIP/Sentinel-2 seam pairs (limit 0.03) | **Run 1 fail, run 2 pass.** Run 1 +0.0472 (1,160 pairs of 1,292 candidates; unfitted -0.2797). Run 2 **0.0000** (global-fit-only comparison 0.0433). Cause of run 1: the global fit's residual near NAIP's footprint edge; all seam pairs lie on the outer envelope of the NAIP union, 5-7 km outside the bbox, and per-date fits would have given +0.032, still over. The gridded offset (Task 9) removes it |
| Classes | NDVI per land-cover class (sanity ordering) | **Pass** (run 2). Median: tree 0.4961, shrub 0.3386, grass 0.3071, crop 0.2835, built 0.2441, bare 0.2677, water -0.1024, wetland 0.3543 (WorldCover 10/20/30/40/50/60/80/90; n 42,006 / 19,105 / 42,060 / 1,027 / 12,060 / 1,506 / 29,759 / 121). Zooms 0-15 present |
| Review | NDVI overview `.cache/ndvi_check/pendleton.png` | **Pass** (reviewed by the user for both runs, accepted 2026-10-10) |
| Verify | `verify --deep -j 6` | **Pass.** Every check `ok` (hashes, `ndvi_tilemapresource`, terrain, imagery decode); `ndvi_values` 6,368 px in 109 leaves, error steps p99 0 (NAIP 4,692 px, Sentinel-2 1,676 px). 233 s, RSS 1.84 GB |
| Determinism | Rebuild with a different `-j`; second build over the same package | **Pass, with a ruling.** `-j 1` vs `-j 6` (the spec says `-j 16`; measured on this Mac): `hashes.txt` identical (469,227 lines each). `-j 1` build 7,847 s (2.2 h). A second `-j 6` build of `pendleton-ndvi` skipped everything (169 s; terrain 182,602, imagery 277,286, NDVI 7,043, land cover 2,288) |
| Off switch | Layers other than `ndvi/` unchanged | **Pass, with a ruling.** Every non-`ndvi/` line of `hashes.txt` equals the chunk-1 package `pendleton-r1b`. Equivalent by construction to an `ndvi = false` build, which was not built separately |

Follow-up: the `-j 1` build exits 2 with "peak worker RSS 4318 MB exceeds 2048 MB (the package is complete)". At
`-j 1` the build runs in-process, so the main process's fit and index state counts as the worker's RSS (4318 MB for
every layer); at `-j 6` the workers peak at 80-900 MB. The package is complete and byte-identical; the RSS accounting
for in-process builds still needs fixing.

### R1 completion (2026-10-10, macOS)

Camp Pendleton `sim`, M1 Pro / Metal. Before is `pendleton-ndvi` (R1 chunk 2); after is `pendleton-r1c`, built with
`--replan -j 6` from a copy of `pendleton-ndvi`. Linux items are not tested (see below).

| Gate | Tool | Result | Before -> after |
|---|---|---|---|
| Build, replan skip | `build --replan -j 6` | **Pass (+192 tiles from the new seabed coverage).** Wall 904 s. Terrain 182,794 tiles (vs 182,602: the extra tiles are the seabed sources' new coverage; the pre-fetch estimate uses discovery bboxes) built in 604 s, 979.8 MB, peak RSS 914 MB. Imagery 277,286, NDVI 7,043 and land cover 2,288 tiles skipped. Only `ATTRIBUTION.txt` changed outside terrain; `sea_level.json` added. Station NOAA 9410230 La Jolla: offset +0.110 m (NAVD88 0 at ellipsoid height -35.7764 m, EGM96 N -35.1124 m) | terrain 182,602 -> 182,794 (+192) |
| Verify | `verify --deep` | **Pass.** Every check `ok`, including `sea_level` (239 s). Scene z9+ height error p50 0.001 m, p99 0.162 m, max 2.08 m over 346,640 vertices (z9 p99 0.436, z10 p99 0.231, z14 max 2.08 m) | p99 0.049 m -> 0.162 m |
| coast_check (a) | `coast_check.py`: offshore terrain <= sea - 0.5 m for >= 99 % | **Pass.** Median depth 24.5 m below sea | 65.6 % failing -> 0.0 % |
| coast_check (b) | steps <= 3 m per 30 m within 1 km of the dep3_13 edge | **Literal FAIL**, 4.146 m. The step is the natural continental slope on transect coast_00, about 6 km out at -137 to -157 m. It has the same gradient as the NOAA SD 1/3" source (about 4.3 m per 30 m), so it is not a seam. Follow-up: compare against the source's own gradient | 58.4 m cliff -> 4.146 m |
| coast_check (c) | median waterline offset vs WorldCover shoreline <= 30 m | **Pass.** Median 20 m, max 80 m | 140 m / 4,270 m -> 20 m / 80 m |
| coast_check (d) | `sea_level.json` offset vs an independent pyproj chain, within 1 cm | **Pass.** 0.110 m vs 0.107 m (3 mm) | none -> 3 mm |
| HOT / HAT, land | `hot_check.py` | **Pass.** 7/7 frame-centre and 7/7 HOT. Same values as R0: 1 m points -0.10 to +0.20 m; 1/3" points -0.48 and -0.05 m | unchanged |
| HOT, sea | `hot_check.py` | **Pass.** 3/3 equal EGM96 + 0.110 m exactly (-34.908 / -35.074 / -35.270 m) | new |
| Render readiness | `render_check.py` | **Pass.** Every shot `terrain_ready`; no "terrain gate timed out" in any run (package, offline package, CWT, ndvi). New shots `coast_low` and `coast_waves` (Beaufort 5) | - |
| Cold start | `render_check.py` | First ready at 17 s package, 32 s offline, 30 s CWT, 30 s ndvi | - |
| Offline | `scene.offline: true` | **Pass.** No network request logged (macOS) | - |
| Registration | vs CWT + Bing | **Pass.** <= 0.16 px on all 7 land shots; online and offline identical | unchanged |
| Visual gate | before/after review | **Pass** (user, 2026-10-10). No turquoise shelf, no seabed through wave troughs, waterline at the beach | - |
| Bench | `run_bench.py --site pendleton`, 2 runs each | **Pass.** Game-thread p50 of r1c within noise of ndvi and below CWT in every phase (table below). 30 fps and 0 dropped frames in every run and phase. Wall p95 for r1c 34.4-41.8 ms, within the ndvi range. `results.json` has no game-thread p95, so wall p95 is the tail check | see below |

Game-thread p50, ms (range over 2 runs):

| Phase | CWT + Bing | pendleton-ndvi | pendleton-r1c |
|---|---|---|---|
| orbit | 3.44-3.66 | 3.15-3.16 | 3.16 |
| slew | 3.54-3.56 | 3.09-3.12 | 3.10-3.14 |
| low_pass | 3.88-3.89 | 3.41-3.48 | 3.40-3.44 |
| far_origin | 3.74-3.81 | 2.55-2.59 | 2.22-2.56 |
| coast_pass | 4.21-4.43 | 3.65-3.70 | 3.41-3.67 |

The Linux items (`file://` from a mounted volume in Docker, a `docker run --network none` offline run, SquashFS vs
plain-directory cold start) are in "R1 on Linux" below.

Follow-ups: coast_check (b) should compare against the source gradient. Verify vertex error at tidal-mask edges (z14
max 2.08 m: the mask flips between a vertex and a sample). `point_heights` no longer breaks early (it samples every
group); the cost is only in verify and the hot tools.

### R1 on Linux (2026-10-10)

Reference box: Core Ultra 9 285K (24 cores), RTX 5080, driver 595.99.02, Vulkan SM6, UE 5.8.3, kernel 6.17. The box
had no R1 packages (`pendleton-r1`, `pendleton-r1b`), so `pendleton-r1c` was built with `--replan -j 16` in the
reference container from a copy of the R0 package `pendleton-b`: every layer is rebuilt, nothing is skipped. CamSim is
the packaged Development image `camsim:linux-r1` (`package_for_docker.sh`, head `3a1d1e7`). The package goes in by
bind mount, `-v /mnt/pendleton:/data/scene/pendleton:ro` (`.sqfs` loop-mounted read-only) with
`CAMSIM_SCENE_DIR=/data/scene/pendleton`. `render_check.py` and `hot_check.py` take `--docker IMAGE` for this.

| Gate | Tool | Result |
|---|---|---|
| Build | `build --replan -j 16` (from `pendleton-b`) | **Package complete; exit 2 is a false RSS fail.** Wall 27 min 40 s. Terrain 182,794 (473 s, 979.8 MB), imagery 277,286 (547 s, 2.17 GB), NDVI 7,043 (45 s), land cover 2,288 (91 s): the counts the Mac got. `sea_level.json` +0.110 m (NOAA 9410230 La Jolla). Exits 2 with "peak worker RSS 3249 MB exceeds 2048 MB": every layer reports the same 3249 MB, which is the main process's peak. On Linux a forked worker inherits the parent's `ru_maxrss`, so the known `-j 1` RSS follow-up also hits `-j 16` here. The log also has 2,966 bare "Error in sys.excepthook" lines (no traceback) near the end of the terrain layer, from worker teardown (cached datasets, since fixed: see below); the layer's tiles all verify |
| Verify | `verify --deep -j 16` | **Pass.** Every check `ok`, incl. `sea_level` (0.11 m, recomputed 0.11 m) and `ndvi_values` (6,368 px, p99 0 steps). Scene z9+ height error p50 0.001 m, p99 0.162 m, max 2.08 m over 346,670 vertices: the Mac's values. 162 s |
| Pack | `pack` | `.sqfs` 3.51 GB in 10 s, sha256 `8b8d92cc21606d30…` |
| Image | `ci_validate.sh --docker camsim:linux-r1` | **Pass** (NVENC, `--gpus all`) |
| Render readiness | `render_check.py … package /mnt/pendleton --docker camsim:linux-r1` | **Pass.** 11/11 shots `terrain_ready`, no "terrain gate timed out". Shots reviewed by the user (`nadir_2km`, `coast_low`, `coast_waves`, `slant_ne_ir`) |
| HOT / HAT | `hot_check.py --docker` | **Pass.** Land 7/7 frame-centre and 7/7 HOT (1 m points -0.10 to +0.20 m, 1/3" points -0.48 and -0.05 m), sea 3/3 equal EGM96 + 0.110 m (-34.908 / -35.074 / -35.270 m): the Mac's values |
| Registration | package vs a Linux CWT run (`render_check.py … cwt --docker`) | **Pass.** <= 0.15 px on all 7 land shots (limit 1 px) |
| Offline | `render_check.py … --offline --docker` | **Pass.** The container runs with `--network none` and `CAMSIM_SCENE_OFFLINE=1`; the CIGI host and `/ready` reach it through a socat bridge (a sidecar in CamSim's network namespace, Unix sockets in a shared directory, host socat on 127.0.0.1:8080 and :8888). 11/11 shots ready, no `LogHttp` or `cesium.com` line, no exit 1 |
| Offline, negative | same isolation, `CAMSIM_SCENE_DIR` unset | **Fails as it should.** 16 `LogHttp` and 6 `cesium.com` lines, the terrain gate times out, the tileset stays at 0 % (1 tile loaded) |
| Cold start | `render_check.py --docker`, 3 runs each, page cache dropped before each cold run | **SquashFS = plain directory.** `/ready` at 6 s in all 12 runs (loop-mounted `.sqfs` vs the unpacked directory bind-mounted `:ro`, cold and warm). Every shot's terrain gate passed 0.0-0.1 s after its 1 s settle. `/ready` is polled every 2 s, so differences under 2 s don't show. The Mac (native, plain directory) took 17 s |
| Bench | `run_bench.py --docker camsim:linux-r1 --site pendleton --volume /mnt/pendleton:/data/scene/pendleton:ro`, 2 runs each vs CWT + Bing | **Pass.** Game-thread p50 of the package below CWT in every phase (table below). 30 fps and 0 dropped frames in every run and phase; wall p95 33.6-38.2 ms (package) vs 33.8-38.2 ms (CWT). One CWT run is excluded and was rerun: ion's `layer.json` request timed out in the TLS handshake (30 s), Cesium did not retry, and the run drew no terrain (0 tiles; game thread a flat 0.87 ms) |

Game-thread p50, ms (range over 2 runs), Linux / Vulkan / Docker:

| Phase | CWT + Bing | pendleton-r1c (`.sqfs` mount) |
|---|---|---|
| orbit | 2.11 | 1.79-1.80 |
| slew | 2.24-2.27 | 1.91-1.94 |
| low_pass | 2.47-2.50 | 2.06-2.08 |
| far_origin | 2.21-2.22 | 1.39 |
| coast_pass | 2.91-2.93 | 2.23-2.24 |

Cold start, a measurement trap: back-to-back sessions first read 63-65 s. Linux keeps the previous run's port 8080 in
TIME_WAIT for 60 s and CamSim's health server retries the bind every 2 s ("port 8080 is busy"), so `/ready` was
unreachable for a minute. `camsim_session.py` and `render_check.py` now wait the port out before launching (as
`run_bench.py` already did), and `ready_s` starts after the wait.

Follow-up: fix the build's RSS accounting for forked workers (measure each worker's own peak, not `ru_maxrss`
inherited from the parent). The bare `sys.excepthook` lines are found and fixed (2026-10-11): `sources/base.py` caches
open rasterio datasets (`_dataset`, `lru_cache`), which were freed only at interpreter teardown, one error per cached
dataset per worker. They were read-only, so no tile was affected; an `atexit` handler now clears the cache.

### R2 vector features (2026-10-10, macOS)

Camp Pendleton `sim`, M1 Pro / Metal, native build. `pendleton-r2` is an APFS clone of `pendleton-r1c` planned with
`examples/pendleton.toml` (`[features]` all on, `allow = ["ODbL-1.0"]`): OSM `socal-261008` (2026-10-08), NHD HR
`NHD_H_1807_HU4_GPKG`. Only `layers.landcover` and the two new sources differ from r1c, so terrain, imagery and NDVI
were skipped. Details: the R2 task 8–10 reports in the (untracked) session notes.

| Gate | Tool | Result |
|---|---|---|
| 1 Package | `build`, `verify --deep` | **Pass.** Burned texels: 200 = 511,761; 201 = 18,473; 210 = 12,828; 220 = 40,507. Land cover 2,288 tiles, 42.0 MB; build 136 s cold (incl. the OSM/NHD read), 62 s with the features cached, 55 s with features off; peak RSS 156 MB for the cut, 3,079 MB for the cold build's process tree (the OSM read). `verify --deep` all `ok`, including `landcover_codes`. Rebuild: `landcover/` identical. Features off: `landcover/` identical to `pendleton-r1c` (fresh cut) |
| 2 Road registration | `tools/road_registration.py` (2-D windowed, "Vector features") | **Pass.** Median \|shift\| **2.0 m**, p90 9.99 m (gate 10 m); 54 of 60 windows used (excluded: 3 peak at the search edge, 2 low contrast, 1 not prominent); mean (dE, dN) = (−0.06, +0.65) m. The spec's dark-box profiles gave 14.5 m (and an edge-pair matcher 12.0 m), both measuring tree lines and shadows, not misregistration (crops in `.cache/r2/road_crops/`) |
| 3 Thermal gate (r) | `thermal_check.py --runs roads --scene pendleton-r2` | **FAIL** on the selected pose (MWIR night and noon, LWIR noon; table below). Thresholds unchanged. Open: needs a decision |
| 4 Unchanged | `thermal_check.py` (a)–(q); UE tests; bench | **Pass.** Gates (a)–(q) all pass on the SF scene, MWIR and LWIR: (f) ThermalCS p95 0.421 ms at 1080p (4B: 0.408); (g) EO \|dY\| 0.013 DN. UE automation 494/494 (`-nullrhi`), `CamSim.GPU.*` 26/26 (Metal), scene tooling 444 passed / 2 skipped / 1 xfailed. Bench (`run_bench.py --site pendleton`, `.cache/bench-r2/`): ThermalCS identical to R1 in a same-session IR pair (table below); EO game-thread p50 and wall p95 within noise of R1, 30 fps and 0 dropped frames in every run and phase |
| 5 CIGI | `send_cigi_test.py --hat-extended / --los-extended` | **Pass** (table below) |
| 6 Visual review | MWIR/LWIR/EO noon frames, `.cache/r2/visual/` | **Signed off** by the user 2026-10-10 (runway and Oceanside grid read as warm lines; I-5 cooler than its verge, the gate (r) finding; dark IR patches absent from EO are a follow-up) |

Gate (r): carriageway minus outer verge, mean Y over 10 frames (white-hot), nadir 300 m (GSD 0.083 m), 21 December.
**Pose selection** (fix round 1; `scripts/scene/tools/select_road_pose.py` reproduces it, `ROAD_POSE` in `thermal_check.py` holds the criterion and the numbers; the rule was relaxed from width 10 m / length 200 m / vegetation 80 % to 8 m / 120 m / 60 %, both changes before any gate run): straight pieces
(≥ 120 m, bearings within 1°) of OSM motorway…tertiary ways that burn to 200, ≥ 8 m wide, no crossing or bridge within
60 m; carriageway strip ≥ 90 % code 200; verge strip (8–28 m beyond the edge, one side) free of road/water codes and
≥ 60 % vegetation/bare codes; and *dark pavement*: NAIP luma of the verge ≥ 15 DN above the carriageway. 282 pieces pass
the land-cover tests and 30 the luma test; ranked by road class, then width, then the luma difference. Rank 1 is the
gate pose: I-5 northbound in San Clemente, OSM way 487165554 (asphalt, 5 lanes, 20.5 m; luma 75.3 vs verge 97.9).

| Pose | Band | 02:00 (≥ +2) | 12:00 (≥ +6) | Class temperatures, asphalt / verge class (K), 02:00 · 12:00 |
|---|---|---|---|---|
| **Gate: rank 1**, I-5 NB San Clemente (way 487165554) | MWIR | **−1.15 Fail** | **−25.29 Fail** | 285.9 / 283.6 · 307.1 / 298.3 (`tree_canopy`; dry, so refined toward `bare_soil` 285.4 · 306.1) |
| | LWIR | +13.32 Pass | **−10.83 Fail** | |
| Rank 2 (diagnostic), CA 76 (way 215015826, 17 m, verge bare 60) | MWIR | +38.43 | +15.58 | 285.9 / 285.4 · 307.2 / 306.1 (`bare_soil`) |
| | LWIR | +27.48 | +18.23 | |
| Rank 3 (diagnostic), Avenida Pico (way 399485028, 13.5 m, verge 30/10) | MWIR | −9.57 | −6.48 | 285.9 / 282.4 · 307.2 / 300.9 (`grassland`) |
| | LWIR | +7.90 | +2.52 | |
| First pose (task 9; info row `r_t9`), I-5 SB (way 1009956653), OSM asphalt but light NAIP pavement | MWIR | +38.16 | −40.06 | 285.9 / 282.4 · 307.1 / 300.8 (`grassland`) |
| | LWIR | +28.96 | −19.39 | |

Ranks 2 and 3 were run as diagnostics after rank 1 failed, to see whether the result depends on the pose; the gate pose
was not changed to the passing one (that would be choosing by outcome). On the first pose, `k_fast` 0 for every terrain
class gave noon +21.61 DN MWIR (pass), so its failure is the fast term reading the light pavement. On rank 1 the pavement is
dark, but the verge is dry scrub on a 9 m bank (ground 47.8 m at the road, 56.7 m mid-verge): dry vegetation refines to
`bare_soil`, whose closed-form temperature is only 1.1 K below asphalt at noon and 0.5 K at 02:00, and the sunward bank
and light dry ground add a positive fast term on the verge. The closed-form asphalt-vs-grass contrast (+6.3 K at noon in
December, +3.5 K at 02:00) agrees in sign with published values (+10–20 K at summer noon); what the gate sees depends on
the verge's imagery (green or dry) and the pavement's brightness. Overlays: `.cache/r2/thermal_roads_v2/overlays/`.

CIGI extended responses (`send_cigi_test.py`, sensor 300–400 m above each point, noon):

| Point | Query | Material | HOT / hit |
|---|---|---|---|
| I-5 SB, first gate (r) pose (`r_t9`) 33.299358, −117.465474 | HAT/HOT 103 | **200** | HOT −8.50 m (gate ground −8.23 m) |
| same, LOS from 300 m above to −500 m | LOS 105 | **200** | hit −8.50 m, range 300.3 m, not visible |
| Lake O'Neill, NHD water 33.3274, −117.3308 | HAT/HOT 103 | **220** | HOT −1.36 m (WorldCover calls this cell grassland, 30) |
| Lake O'Neill per the plan, 33.3250, −117.3330 | HAT/HOT 103 | 10 | HOT −5.46 m: the plan's point is tree cover on the shore |
| Offshore 33.25, −117.50 | HAT/HOT 103 | **80** | HOT −35.27 m (the sea surface) |
| Open country 33.36, −117.40 | HAT/HOT 103 | **30** | HOT 62.57 m |

Visual review frames (MWIR, LWIR and EO reference, noon 21 December): `.cache/r2/visual/{mwir,lwir,eo}_{i5_gate,i5_wide,
oceanside_grid,runway}.png` (I-5 at the first gate (r) pose, OSM way 1009956653, with 20° and 60° FOV; an Oceanside residential grid at 33.1985,
−117.3760, ~300 m up; the MCAS Camp Pendleton runway at 33.3017, −117.3550, ~600 m up). Questions (spec Acceptance 6):
do roads read as continuous lines; is the 6 m warp's wobble on road edges acceptable; is the brown-verge widening
acceptable.

Bench (`run_bench.py --site pendleton --env CAMSIM_SCENE_DIR=…`), 2026-10-10, M1 Pro. EO: two R2 runs against the R1 runs
named above (`.cache/bench-r1/pendleton-r1c-{a,b}`, an earlier session). IR (`--sensor ir`): `pendleton-r1c` and
`pendleton-r2` back to back in one session, the comparison that isolates R2 (ThermalCS from `thermal_gpu_ms` in
`frames.jsonl`). Every run: 30 fps, 0 dropped frames.

| Phase | EO game p50, ms: r1c (2 runs) / r2 (2 runs) | IR game p50: r1c / r2 | ThermalCS p50 / p95, ms: r1c | r2 |
|---|---|---|---|---|
| orbit | 3.16 / 2.81-2.82 | 2.82 / 2.84 | 0.367 / 0.416 | 0.369 / 0.416 |
| slew | 3.10-3.14 / 2.96-2.99 | 3.02 / 3.03 | 0.353 / 0.416 | 0.354 / 0.417 |
| low_pass | 3.40-3.44 / 3.18 | 3.21 / 3.22 | 0.399 / 0.413 | 0.401 / 0.414 |
| far_origin | 2.22-2.56 / 2.22 | 2.23 / 2.24 | 0.159 / 0.165 | 0.159 / 0.167 |
| coast_pass | 3.41-3.67 / 3.43-3.47 | 3.45 / 3.48 | 0.273 / 0.431 | 0.274 / 0.431 |

The EO game-thread drop (~0.3 ms) is session-to-session: the same-session IR pair shows r1c and r2 equal. Wall p95 for r2
33.8-41.0 ms (r1c 34.0-41.8). GPU frame p50 17.7-21.5 ms for r2 vs 16.1-21.5 ms for r1c (R1's coast_pass 16.1 ms in run a
is the outlier; run b 21.5). Shot SSIM r1c-a vs r2-a 0.960-0.994 (EO is unchanged by R2; the differences are run to run).

### R3 buildings slice (2026-10-11, macOS)

Camp Pendleton `sim`, M1 Pro / Metal, native build. `pendleton-r3` is an APFS clone of `pendleton-r2` built with
`examples/pendleton.toml` (`[buildings]`, Overture release `2026-09-23.1`, `--replan -j 6`): terrain, imagery, NDVI and
land cover were skipped and only the buildings layer was built. Spec acceptance:
[`superpowers/specs/2026-10-10-r3-buildings-slice-design.md`](superpowers/specs/2026-10-10-r3-buildings-slice-design.md).
Details: the R3 task 12 report in the (untracked) session notes.

| Gate | Tool | Result |
|---|---|---|
| 1 Package | `build`, `verify --deep`, `3d-tiles-validator` 0.6.1 | **Pass.** Snapshot 82,283 footprints (14.8 MB GeoParquet, sha256 `3bdadde5…`); 82,260 buildings kept (23 dropped small, 0 invalid, 0 without a height). Tiers z13 780 / z14 16,430 / z15 65,050 buildings in **85 / 394 / 1,286 tiles** (1,765 GLBs + `tileset.json`, 4.9 / 70.9 / 167.7 MB); layer **247.9 MB**; layer build **36 s**, peak RSS 1,630 MB (whole `--replan` run 307 s including the snapshot fetch and the skip scans of the other layers). `verify --deep` all `ok` (318 s at `-j 6`), incl. `buildings_tileset` and `buildings_heights`. Validator: 0 errors, 0 warnings, 1,765 infos (`EXT_mesh_features` / `EXT_structural_metadata` not validated by 0.6.1; "may be unused" on the feature-id accessors). Rebuild into a second clone: `hashes.txt` identical (471,186 lines). The toml without `[buildings]` on a clone of `pendleton-r2`: no `features/`, `manifest.json` and `hashes.txt` byte-identical to `pendleton-r2`'s. `pendleton-r3` vs `pendleton-r2`: only `ATTRIBUTION.txt` and the 1,766 `features/buildings/` lines differ |
| 2 Footprints vs imagery | `tools/footprint_iou.py` | **Open: needs the human-traced outlines** (`scripts/scene/tools/data/pendleton_building_truth.geojson`, ~30 buildings at Mainside, family housing and a hangar, traced in QGIS over the package's NAIP). The tool and its test are in place; run it with `--snapshot ~/…/.cache/scene/blobs/sha256/3b/3bdadde5565c9b96bb310133e8aba02c6d20f3e658830162ea83dfb87c01122b` |
| 3 Heights | `verify --deep` (`buildings_heights`) | **Pass.** Every roof within 0.01 m of the recomputation from the snapshot and the terrain sources. Height source: `overture_height` 60,207 (73.2 %), `overture_floors` 417 (0.5 %), `default` 21,636 (26.3 %) |
| 4 CIGI | `send_cigi_test.py` packets (session script) | **Pass** (table below): roof HOT within 0.01 m of the built roof, code 230; 5 m outside = ground with the land-cover code; LOS hits 30.03 m along a 30.0 m path to the west wall, code 230 |
| 5 DIS | `send_dis_test.py truck-loop --location 33.305,-117.314`; `dis_vehicle_check.py`; `gt_occlusion_check.py` | **Pass.** The re-centred loop crosses 12 Mainside footprints (231 of 1,942 annotated frames inside one). COCO `geo.alt_m` every frame (ML interval 1): largest frame-to-frame change **0.085 m** (p99 0.078 m); the truck stays on the terrain inside footprints (roof at least 3.3 m above it), truck minus DEM ground median −0.06 m, max \|·\| 1.20 m. `dis_vehicle_check.py` PASS (one stable id each for truck and boat); `gt_occlusion_check.py` PASS (both on the SF scene) |
| 6 Bench | `bench/sweep.py` (dense_pass, repeat 2); `run_bench.py` all phases (1 run each) | **Chosen default SSE 64** (2026-10-11, from the combined sweep below, when the memory budget was still +381 MiB and SSE 64 was the only both-on setting inside it; the user then raised the budget to +1024 MiB on 2026-10-11, under which SSE 16 passes the sweep too, and SSE 64 stays the default. With trees on and the footprint check on, the game p95 budget is exceeded in `dense_pass`, an overrun the user accepted: "Full bench, both on" below). 30 fps and 0 dropped frames in every sweep row and every phase. Game thread and GPU within budget in every row. Memory: +588 MiB at SSE 16 with physics, the pre-sweep starting value (it failed the original +381 MiB budget; every sweep row passes the current +1024 MiB budget). Existing phases at SSE 16: game p50 +0.02 to +0.70 ms (slew the most), not within noise but within budget; memory grows to +1.1 GiB over the full run as the tile cache fills (tables below) |
| 7 Off switch | `CAMSIM_SCENE_BUILDINGS_ENABLED=0` on `pendleton-r3` vs `pendleton-r2` | **Pass.** All six HAT/HOT and both LOS responses identical (HOT, HAT, material code, normal, range). Shots SSIM 0.975 (Mainside nadir), 0.981 (Mainside low oblique), 0.992 (dense_oblique), within run to run (R2: 0.960–0.994) |
| 8 Visual review | EO shots | **Pending user review.** dense_pass contact sheet `.cache/bench-r3r4/r3-buildings-sweep/sweep_sheet.png`; `.cache/r3/visual/`: `mainside_low_oblique_buildings_{on,off}.png`, `mainside_nadir_buildings_{on,off}.png`, `dense_oblique_buildings_{on,off}.png` (and `_r2`), `truck_side_*.png` (the DIS truck between buildings) |

Live runtime checks (2026-10-11, `Main.umap`): the log shows `CamSim: buildings tileset 'CamSimBuildings' file:///…
(maxLoads=20 SSE=16.0 culledSSE=16 … physicsMeshes=1)` and then `CamSimSubsystem: cached 2 tileset actor(s)`, so the
world-init spawn survives `InitializeActorsForPlay`. 1,454 of the 1,765 tiles omit the all-empty `class` property; Cesium
loaded them (260–369 buildings tiles resident during the runs) with no `LogCesium` warning or error.

CIGI (`.cache/r3/live_on/result.json`; the sensor 300 m above the HAT points, beside the wall for LOS, noon):

| Point | Query | Built (verify recompute) | Response |
|---|---|---|---|
| Warehouse `010048df-280f-483f-97a8-bddb71d1b61b` (z13, Overture height 6.42 m) roof, 33.3029558, −117.3144625 | HAT/HOT 103 | roof 94.67 m | **230**, HOT 94.665 m |
| 5 m west of its wall, 33.3029558, −117.3147197 | HAT/HOT 103 | DEM 88.53 m | 50, HOT 88.465 m |
| LOS from 30 m west of the wall at 90.75 m (base + 3) due east | LOS 105 | wall at 30.0 m | **230**, range 30.03 m, hit 90.75 m |
| Commercial `9cc2e6cd-5694-4810-8df7-533da6a510b5` (z14, 7.54 m) roof, 33.3064210, −117.3139776 | HAT/HOT 103 | roof 91.28 m | **230**, HOT 91.276 m |
| 5 m west of its wall, 33.3064210, −117.3144953 | HAT/HOT 103 | DEM 86.35 m | 50, HOT 86.120 m |
| LOS from 30 m west at 90.0 m (the site slopes: 3 m above the ground at the sensor, not base + 3) | LOS 105 | wall at 30.0 m | **230**, range 30.03 m |

With buildings off (and on `pendleton-r2`) the roof points answer the ground (88.65 m / 85.97 m, code 50) and both LOS
segments run their full length without a hit.

Bench sweep (`scripts/bench/sweep.py --site pendleton --axis buildings=off,on --axis buildings_sse=8,16,32,64 --axis
buildings_physics=on,off --repeat 2`, `.cache/bench-r3r4/r3-buildings-sweep/`): only the dense_pass phase (300 m HAE,
100 m/s, 20° down, over Oceanside housing and commerce), trees off in every row (the sweep always sends both master
switches), 720p EO. Increase of each row's median over the all-off row (game 3.47–3.52 ms p50, GPU 19.4–20.5 ms,
memory 8,904–8,911 MiB). Memory is the graphics-attributed part of the macOS process footprint (`IOAccelerator*` +
`(graphics)` rows of `footprint`; CPU heap excluded); unified memory, a VRAM proxy. Budget (bench spec §4): game p50 +1.5 ms, p95 +3.0 ms, GPU p50 +5.0 ms,
memory +1024 MiB (raised 2026-10-11 by the user from 381 MiB / 400 MB; the table's verdicts are against +1024).

| SSE | Physics | Game p50 | Game p95 | GPU p50 | Memory, MiB | Budget |
|---|---|---|---|---|---|---|
| 8 | on | +0.68 | +0.64 | +3.29 | +764 | pass |
| 8 | off | +0.57 | +0.51 | +3.29 | +557 | pass |
| 16 (pre-sweep start) | on | +0.61 | +0.54 | +2.63 | +588 | pass |
| 16 | off | +0.54 | +0.52 | +1.90 | +426 | pass |
| 32 | on | +0.53 | +0.41 | +1.72 | +396 | pass |
| 32 | off | +0.46 | +0.40 | +1.00 | +276 | pass |
| **64 (default)** | **on** | +0.45 | +0.39 | +0.47 | **+272** | pass |
| 64 | off | +0.44 | +0.59 | +0.48 | +198 | pass |

Physics meshes cost 70–210 MiB and ~0.1 ms of game thread; without them CIGI LOS and HAT/HOT don't see buildings, so a
physics-off default would give up gate 4. In the contact sheet, SSE 64 thins the far field (beyond ~2 km) but leaves
the near field as at 16.

Full bench (`run_bench.py --site pendleton --gpu-mem`, all phases, trees off, SSE 16 with physics vs buildings off, one
run each, back to back; `.cache/bench-r3r4/r3-full-b{1,0}/`). Every phase 30 fps, 0 dropped frames:

| Phase | Game p50, ms: on / off | Game p95 | GPU p50 | Wall p95 | Memory max, MiB |
|---|---|---|---|---|---|
| orbit | 3.19 / 2.78 | 5.14 / 4.51 | 19.07 / 19.10 | 35.0 / 34.6 | 9,445 / 8,853 |
| slew | 3.70 / 3.00 | 12.24 / 11.02 | 19.33 / 19.30 | 41.1 / 40.9 | 9,475 / 8,881 |
| low_pass | 3.67 / 3.20 | 8.51 / 7.41 | 19.36 / 18.90 | 37.0 / 36.9 | 9,501 / 8,921 |
| far_origin | 2.20 / 2.18 | 3.46 / 2.99 | 17.03 / 16.99 | 33.9 / 33.9 | 10,011 / 8,880 |
| coast_pass | 3.96 / 3.51 | 10.53 / 9.28 | 20.31 / 19.46 | 37.7 / 37.2 | 10,008 / 8,917 |
| dense_pass | 4.29 / 3.59 | 11.04 / 9.21 | 21.98 / 15.43 | 37.9 / 37.3 | 10,070 / 8,968 |

The memory keeps rising after the buildings leave the view (far_origin, 300 km east, is +1.1 GiB): loaded buildings
tiles stay in Cesium's cache (`cacheMB=2048`, the terrain's tuning). The dense_pass GPU difference here (+6.6 ms) is larger
than the sweep's (+2.6 ms at the same setting) because the off run's GPU p50 (15.4 ms) is low against the sweep's
all-off rows (19.4–20.5 ms); one run each, so the sweep is the cost to quote.

Stability: the first live session crashed (SIGSEGV in `FMeshDrawShaderBindings::SetOnCommandList` on a renderer draw task,
5 s after a 215 m camera cut from 300 m nadir to a ground-level pose, buildings and trees on). It did not recur in 27
later Pendleton sessions, including the same pose sequence three times (buildings on, buildings off, trees off). Cause
unknown; a crashed run leaves `CrashReportClient` holding port 8080, so the next launch can't bind its health port.


### R3 and R3b on Linux (2026-10-13)

Reference box: Core Ultra 9 285K, RTX 5080 (16 GB), driver 595.99.02, Vulkan SM6, UE 5.8.3, head `538eb5b`. Packages built
in the reference container (`scripts/scene/build.sh`, `-j 16`) from a copy of the box's `pendleton-r1c`: `pendleton-r2`
(R2 features, the toml without `[buildings]`; land cover 140 s), then `pendleton-r4` (`examples/pendleton.toml`,
`--replan`: land cover with the 230 burn, buildings with mask and roof atlases), and `pendleton-r4-noroof`
(`roof_textures = false`). Outputs under `.cache/linux-5/` (untracked).

| Check | Result |
|---|---|
| Package | **Pass.** Overture snapshot 82,283 footprints, 82,260 kept (23 dropped small); 1,765 tiles; all 82,260 roofs textured at 0.5 m; buildings layer 368.3 MB (Mac 368.2), built in 141 s; mask 56 tiles + `index.json`; land cover **307,606 cells burned to 230 in 54 tiles** (Mac: the same); `hashes.txt` 471,243 lines (Mac: the same). `verify --deep` all `ok` (3 min 14 s), incl. `buildings_tileset`, `buildings_heights`, `buildings_roofs`, `buildings_mask`. A second clean build: `hashes.txt` identical. Not compared file by file with the Mac's `hashes.txt` (not on this box); the mask totals 3,151,675 bytes vs the Mac's 3,081,704, so the mask PNGs at least differ in bytes (the Mac built natively, this box in the container: different Pillow / zlib); the GLBs are expected to differ (JPEG atlases) |
| Build memory | **Found and fixed.** The first build exited 2: peak RSS 4,247 MB over the 2,048 MB limit (Mac 1,988). Sampled from `/proc`, the 4.2 GB was real and in the main process (workers ≤ 363 MB): the buildings layer runs there, where GDAL's block cache had its default of 5 % of RAM (3.1 GB on this 62 GB box); pool workers already capped it at 256 MB. `build_scene` now applies the same cap (a caller's `GDAL_CACHEMAX` wins): peak 1,027 MB sampled / 1,154 MB `ru_maxrss`, exit 0, `hashes.txt` identical, buildings 146 s. The no-roof build peaks at 714 MB. Also fixed: in the container DuckDB could not install `httpfs` (the `-u` user's `HOME` was `/`), so the Overture fetch failed; `build.sh` sets `HOME=/tmp` |
| Spawn | **Pass** (native, `CAMSIM_SCENE_DIR`, offline): `buildings tileset 'CamSimBuildings' … SSE=64.0 culledSSE=64 cacheMB=512 … physicsMeshes=1`, `cached 2 tileset actor(s)`, no `LogCesium` error; ready 14 s. In Docker (`camsim:linux-r4`, the package bind-mounted `:ro`) the same lines |
| CIGI | **Pass**, as the Mac's table (sensor 300 m above, noon): warehouse `010048df…` roof **230**, HOT 94.67 m; 5 m west 50, 88.46 m; commercial `9cc2e6cd…` roof **230**, 91.27 m; 5 m west 50, 86.12 m; LOS from 30 m west of the warehouse wall at 90.75 m due east: **230**, range 30.0 m, hit 90.75 m |
| Infrared | **Pass** (`thermal_check.py --band mwir --runs roofs,roads --scene pendleton-r4`): (s) noon roof − grass **+40.78 DN** (Mac +38.74); (s_air) 02:00 `building_roof` 282.1 K vs air 284.2 K, **2.1 K** below (Mac: the same); (r) night +7.55, noon +26.82 DN (thresholds 2.0 / 6.0) |
| Sweep, buildings | **Pass** (`sweep.py --axis buildings=off,on --axis buildings_sse=16,64 --axis buildings_physics=on --repeat 2`, `dense_pass`, 720p EO; VRAM = `nvidia-smi` `memory.used`, the whole GPU). All-off: game 3.23–3.25 ms p50, GPU 2.73 ms, 4,844–4,988 MiB. SSE 16: +0.64 / +1.40 ms game p50 / p95, +0.22 ms GPU, +932 MiB. **SSE 64 (default): +0.46 / +0.84 ms, +0.11 ms GPU, +618 MiB.** 30 fps, 0 dropped in every run |
| Sweep, combined | **Both on: FAIL, game p50 +1.84 ms** over the +1.5 ms budget (`--axis buildings=off,on --axis trees=off,on --repeat 2`; package without a species mix, so trees are the built-in default). All-off 3.24–3.26 ms p50. Trees: +1.40 / +1.86 ms, +0.27 ms GPU, +18 MiB, pass. Buildings: +0.44 / +0.74 ms, +0.12 ms GPU, +763 MiB, pass. Both: +1.84 / +2.45 ms, +0.37 ms GPU, +777 MiB. 30 fps, 0 dropped in every run. The Mac's both-on row was +1.16 ms p50 (pass); on Metal the game thread has more headroom over this box's 3.2 ms base, and trees cost more here (+1.40 vs the Mac's trees-only row) |
| Full bench | **Memory pass, game p50 over in two phases** (`run_bench.py --site pendleton --gpu-mem`, both on vs both off, one run each; table below). Every phase 30 fps, 0 dropped. Memory over the whole run **+675 MiB** (5,935 vs 5,260 MiB; Mac +1,243, over budget) |
| Instances | **Roof atlases at 0.5 m don't fit 4 instances on 16 GB** (`scale_bench.py camsim:linux-r4 … 1 2 4 --path dense`, the package mounted; table below). With buildings off or roof textures off, 4 instances hold 30 fps with 0 dropped |

Full bench, both on / both off (Mac, buildings only: "R3 buildings slice" above):

| Phase | Game p50, ms | Game p95 | GPU p50 | Wall p95 | VRAM max, MiB |
|---|---|---|---|---|---|
| orbit | 3.11 / 2.54 | 6.99 / 6.45 | 2.81 / 2.75 | 34.5 / 34.5 | 5,606 / 4,844 |
| slew | 3.35 / 2.81 | 10.39 / 10.02 | 2.92 / 2.82 | 39.6 / 39.6 | 5,606 / 4,843 |
| low_pass | 4.24 / 3.00 | 8.34 / 7.07 | 2.97 / 2.71 | 35.9 / 36.2 | 5,779 / 4,958 |
| far_origin | 2.06 / 2.01 | 5.80 / 5.70 | 2.50 / 2.48 | 33.7 / 33.7 | 5,791 / 4,974 |
| coast_pass | 4.19 / 3.36 | 10.77 / 9.92 | 2.98 / 2.82 | 36.3 / 36.4 | 5,838 / 5,259 |
| dense_pass | 5.63 / 3.42 | 11.53 / 10.19 | 3.12 / 2.75 | 36.2 / 36.5 | 5,935 / 5,260 |
| long_range | 5.37 / 3.67 | 11.33 / 10.29 | 2.79 / 2.65 | 36.2 / 36.4 | 5,829 / 5,216 |

Game p50 is over +1.5 ms in dense_pass (+2.21) and long_range (+1.70), within elsewhere (low_pass +1.24); p95 within
+3.0 ms in every phase.

Instances (`scale_bench.py`, Docker, each instance shuttling along the `dense_pass` line, 45 s warm-up, 90 s
measured; VRAM is the whole GPU, 16,303 MiB):

| N | Buildings off | Buildings on, roof atlases 0.5 m | Buildings on, no roof textures |
|---|---|---|---|
| 1 | 30.00 fps, 0 dropped, 3,885 MiB, GPU 3.06 ms | 30.00, 0, 4,600 MiB, 3.15 ms | 30.00, 0, 3,973 MiB, 3.14 ms |
| 2 | 30.00, 0, 7,443 MiB, 3.07 ms | 30.00, 0, 8,927 MiB, 3.15 ms | 30.00, 0, 7,610 MiB, 3.78 ms |
| 4 | 30.00, 0, 14,900 MiB, 4.03 ms, GPU busy 46 % | **28.68 fps min, 21 dropped**, 15,833 MiB, **8.90 ms**, GPU busy 67 % (p95 83 %), wall p99 43.1 ms | 30.00, 0, 15,269 MiB, 3.47 ms, GPU busy 44 % |

The roof atlases cost ~630 MiB of VRAM per instance; at 4 they leave under 500 MiB free and the GPU time per frame
more than doubles. For four instances on a 16 GB card, use a `roof_textures = false` or `roof_texel_m = 1.0` variant
(the user's 2026-10-11 rule for multi-instance runs); one or two instances run the 0.5 m default comfortably.

### R3/R4 bench check and combined cost (2026-10-11, macOS)

Same machine and method as the R3 sweep above (M1 Pro / Metal, 720p EO, `dense_pass`, 2 repeats per row, medians). The
runs after the bench docs landed (`scripts/bench/README.md`, "Realism vs cost sweep"). Memory (`--gpu-mem`) is
the graphics-attributed part of the macOS process footprint (`IOAccelerator*` + `(graphics)` rows; CPU heap excluded);
unified memory, a VRAM proxy. The sweep's memory figures, and its budget verdicts, are each row's peak over one 90 s
`dense_pass` after the warm-up, not over a whole run (the full bench below is). The older
`memcheck` reference of 4,258 MiB was taken under the previous rule (IOAccelerator rows only) and is not comparable;
under the current rule the all-off `dense_pass` reference is **8,889-8,901 MiB** (`.cache/bench-r3r4/reference/`).

R2 reference for `dense_pass` (`sweep.py --scene pendleton-r2 --axis trees=off --axis buildings=off --repeat 2`; the
row is named `trees-off_buildings-off`, not `baseline`, because the row names both axes): `ok (2/2)`, game p50 3.42-3.47 /
p95 5.99-6.20 ms, GPU p50 19.38-20.54 ms, wall p95 35.0-35.2 ms, 0 dropped, 29.89 fps, memory 8,889-8,901 MiB.

Full bench (`run_bench.py --site pendleton --gpu-mem`, both features off, `pendleton-r2`, one run;
`.cache/bench-r3r4/full-check/`): phases `orbit, slew, low_pass, far_origin, coast_pass, dense_pass`, every phase 29.9
fps and 0 dropped; 13 shots plus 13 `_sensor` copies. Game p50 of the five old phases against the R2 table above:

| Phase | This run, ms | R2 EO (2 runs), ms |
|---|---|---|
| orbit | 2.80 | 2.81-2.82 |
| slew | 2.98 | 2.96-2.99 |
| low_pass | 3.22 | 3.18 |
| far_origin | 2.20 | 2.22 |
| coast_pass | 3.52 | 3.43-3.47 |

Differences from R2's EO values: orbit +0.0, slew +0.0, low_pass +0.04, far_origin -0.02, coast_pass +0.05 over the top of its range; all within noise. `dense_pass` 3.58 / p95 8.66 / GPU 19.30 ms; memory
8,905-8,950 MiB across phases.

Combined sweep (`sweep.py --scene pendleton-r3 --axis buildings=off,on --axis buildings_sse=16,64 --axis trees=off,on
--repeat 2`, `.cache/bench-r3r4/combined/`; trees at their defaults: radius 1000 m, density 1.0, shadows on; buildings with
physics meshes). All-off row: game 3.44-3.45 / 6.02-6.11 ms, GPU 19.30-19.53 ms, memory 8,887-8,913 MiB. Every run 29.7-30.0
fps and 0 dropped frames. Increase of the row median over the all-off row:

| Buildings SSE | Trees | Game p50 | Game p95 | GPU p50 | Memory, MiB | Budget |
|---|---|---|---|---|---|---|
| off | on | +0.61 | +0.58 | +1.12 | +52 | pass |
| 16 | off | +0.63 | +0.76 | +2.41 | +607 | pass |
| 16 | on | +1.24 | +1.24 | +4.34 | +674 | pass (GPU within 0.7 ms of its limit) |
| 64 | off | +0.45 | +0.49 | +1.00 | +290 | pass |
| 64 | on | +1.05 | +1.09 | +2.98 | +359 | pass |

Game-thread and memory costs add (both-on = the two single-feature rows to within 0.1 ms / 17 MiB); GPU time is 0.8-0.9 ms above the sum. Under the original +381 MiB memory budget only the SSE 64 both-on row fitted, by 22 MiB; under the current +1024 MiB budget every row passes
(SSE 16 both-on: +674 MiB, GPU +4.34 ms of +5.0). Contact sheet: `.cache/bench-r3r4/combined/sweep_sheet.png`. Defaults chosen 2026-10-11 from
this sweep: buildings SSE 64 with physics meshes (the only both-on setting inside the +381 MiB budget then in force; 16 was the pre-sweep
starting value; it stays the default under the raised +1024 MiB budget), trees unchanged at radius 1000 m, density 1.0, shadows on.

Full bench, both on at the defaults (2026-10-11, after the final review: buildings SSE 64 with physics and the new
512 MB buildings tile cache, `scene.buildings.maximum_cached_bytes_mb`; trees 1000 m / 1.0 / shadows on with the
footprint check, `vegetation.drop_in_buildings`). `run_bench.py --site pendleton --gpu-mem` on `pendleton-r3`, one
run (`.cache/bench-r3r4/full-both-sse64/`), against the both-off full bench above (`full-check`, `pendleton-r2`). Every
phase 29.8-30.0 fps and 0 dropped frames:

| Phase | Game p50, ms: on / off | Game p95 | GPU p50 | Memory max, MiB: on / off | Memory increase |
|---|---|---|---|---|---|
| orbit | 3.49 / 2.80 | 5.90 / 4.35 | 17.06 / 18.59 | 9,292 / 8,908 | +384 |
| slew | 3.62 / 2.98 | 11.71 / 10.76 | 16.18 / 19.30 | 9,279 / 8,917 | +362 |
| low_pass | 4.04 / 3.22 | 7.38 / 7.19 | 19.70 / 19.03 | 9,318 / 8,919 | +399 |
| far_origin | 2.30 / 2.20 | 3.03 / 3.04 | 17.00 / 17.15 | 9,276 / 8,905 | +371 |
| coast_pass | 4.12 / 3.52 | 10.10 / 8.65 | 20.36 / 18.98 | 9,822 / 8,915 | +907 |
| dense_pass | 5.60 / 3.58 | 11.84 / 8.66 | 21.90 / 19.30 | 9,902 / 8,950 | +952 |

- The buildings cache cap works where it can: `far_origin` (300 km east, no buildings in view) is +371 MiB, against
  +1,131 MiB in the R3 full bench at SSE 16 with the terrain's 2 GiB cache (trees off there).
- Over the built-up phases the memory grows over a whole run: +907 MiB in `coast_pass` and
  +952 MiB in `dense_pass`, against the sweep's +359 MiB for the same setting. That broke the old +381 MiB budget; the user raised the budget to +1024 MiB over a whole run on 2026-10-11, and the peak is inside it. Cesium's `MaximumCachedBytes` only
  evicts tiles that are not in use, so tiles in view, their physics meshes and the trees are outside the cap. The
  sweep's 90 s `dense_pass` window, right after the warm-up, does not see this growth.
- `dense_pass` game p50 +2.01 ms and p95 +3.18 ms are over the game budget (+1.5 / +3.0). One run each, and part of it
  is the footprint check (next paragraph); the p95 overrun is accepted (decision below).

Footprint check cost (`vegetation.drop_in_buildings`, A/B; `run_bench.py --phases dense_pass --shots dense_oblique`,
both features on at the defaults, 2 repeats each, `.cache/bench-r3r4/i1-ab/`), increase over the combined sweep's
all-off row:

| Footprint check | Game p50 | Game p95 | GPU p50 | Memory, MiB | Budget |
|---|---|---|---|---|---|
| off | +1.12 | +1.07 | +2.93 | +374 | pass |
| on (default) | +1.40 | +3.61 | +3.54 | +408 | FAIL: game p95 (accepted); memory passes |

The off rows reproduce the combined sweep's SSE 64 both-on row (+1.05 / +1.09 / +2.98 / +359). The check costs about
+0.3 ms game p50, +2.5 ms game p95 and +34 MiB: `SampleHeightMostDetailed` on the buildings tileset loads its most
detailed (z15) tiles under every tree point out to 1 km, which SSE 64 would otherwise not load. It drops 5.4 % of the
trees in a Mainside/Oceanside session (373 of 6,892, below). So with the check on, buildings SSE 64 + trees is at +3.61 ms game p95, over the 3.0 ms limit.
**Decision (user, 2026-10-11): keep the check on by default** (`vegetation.drop_in_buildings: true`) and accept the
measured cost (+0.3 ms p50, +2.5 ms p95, +34 MiB); the p95 overrun is accepted and tracked. The next step is a
build-time fix: mark building footprints in the scene package so trees are excluded without runtime sampling, then
remove the runtime check (ROADMAP Realism track; it touches the thermal class under buildings, so it needs its own
design). `vegetation.drop_in_buildings: false` stays available as a switch.

### R3b buildings look (2026-10-11, macOS)

Camp Pendleton `sim`, M1 Pro / Metal, native build, commit `826c967`. `pendleton-r4` is an APFS clone of `pendleton-r3`
built with `examples/pendleton.toml` (`--replan -j 6`; roof keys at their defaults: `roof_textures = true`,
`roof_texel_m = 0.5`, `roof_texture_max_px = 2048`): terrain, imagery and NDVI were skipped; land cover (the 230 burn)
and buildings (mask and roof atlases) were built. Spec:
[`superpowers/specs/2026-10-11-r3b-buildings-look-design.md`](superpowers/specs/2026-10-11-r3b-buildings-look-design.md).
Outputs under `.cache/r3b/` and `.cache/bench-r3b/` (untracked).

| Gate | Tool | Result |
|---|---|---|
| 1 Package | `build`, `verify --deep`, `3d-tiles-validator` 0.6.1 | **Pass.** 82,260 buildings kept (82,283 in, 23 dropped small; unchanged from R3). Roofs: **82,260 textured**, 0 `flat_coarse`, 0 `flat_overflow`; all 1,765 tiles at `roof_texel_m` 0.5. Mask: 56 tiles + `index.json`, 3.1 MB (3,081,704 bytes). Land cover: **307,606 cells burned to 230** in 54 tiles; `index.json` `codes` lists 230 `building`. Buildings layer **368.2 MB** (R3: 247.9 MB, +120.3 MB for the atlases); layer build 184 s (R3: 36 s), land cover 71 s; whole `--replan` run 446 s; peak RSS **1,988 MB** (buildings worker; the limit is 2,048 MB). `verify --deep` all `ok`, incl. `buildings_tileset`, `buildings_heights`, `buildings_roofs`, `buildings_mask`, `landcover_codes`. Validator: 0 errors, 0 warnings, 1,765 infos (as R3) |
| 2 Determinism | second clone rebuilt; toml without `[buildings]` on a clone of `pendleton-r2` | **Pass.** Rebuild: `hashes.txt` identical (471,243 lines). Without `[buildings]`: `hashes.txt` and `manifest.json` byte-identical to `pendleton-r2`, no `features/`. `pendleton-r4` vs `pendleton-r3` by hash: 1,765 GLBs changed, 57 `features/buildings/mask/` files added, 54 `landcover/` tiles changed (exactly the tiles with 230), `landcover/index.json` and `landcover/ATTRIBUTION.txt` changed; nothing else (the root `ATTRIBUTION.txt` is unchanged). By path only, 57 lines differ (the new mask files) |
| 3 Trees | `vegetation_check.py --stages footprints,approach,off` | **Pass.** 4,895 trees, 0 probe hits, 0 trunks inside a footprint, 36 within the 2 m clearance (info); log `buildings: mask (clearance 2.0 m)`; placement, determinism, off switch pass. `pendleton-r2` approach vs the R4 run: 217 + 368 common cells, 0 hash mismatches, `buildings: none`. Details: `vegetation.md`, "Trees vs building footprints (R3b mask)" |
| 4 Roofs | `vegetation_check.py --stages footprints` (pose shots), bench `dense_oblique` | **Pending the user's visual review** (row 8). Textured vs flat (`pendleton-r4-flat`, `roof_textures = false`, layer 251.0 MB): `.cache/r3b/veg/footprints/pose{0,1}.png` vs `.cache/r3b/veg-flat/footprints/pose{0,1}.png`, `.cache/bench-r3b/{textured,flat}-dense/shots/dense_oblique.png`. Observed (not a judgement of the 1–2 m fit): the textured roofs carry the NAIP roof colours (terracotta housing roofs, white commercial roofs, dark solar arrays) where the flat package is uniform grey; on Mainside (pose1) a red patch of imagery shows on the ground beside the large building at the bottom centre in both packages (the imagery's roof drawn on the terrain) |
| 5 Infrared | `thermal_check.py --band mwir --runs roofs,roads --scene pendleton-r4`; default `--band both` | **Pass.** (s) noon roof − grass **+38.74 DN** (≥ +3); (s_air) 02:00 `building_roof` 282.1 K vs air 284.2 K, **2.1 K** below (≥ 0.5); (r) night +10.54, noon +35.95 DN. The default San Francisco run passes every gate a–q. Details: `thermal.md`, "Acceptance" |
| 6 CIGI | `burn_probe_points.py --near 33.3000,-117.3175`; `send_cigi_test.py --hat-extended` | **Pass** (table below): with buildings off, the burned terrain point answers **230** and the point outside answers its land-cover code (30); with buildings on, the R3 warehouse roof answers 230 with HOT 94.67 m, as in R3 |
| 7 Bench | `bench/sweep.py` (`dense_pass`, repeat 2); `run_bench.py` all phases (1 run each) | Sweep: **pass** (both on +1.16 ms game p50, +1.39 ms p95, +3.04 ms GPU, +782 MiB). Full runs at the defaults: game p95 ≤ +1.57 ms, every phase 29.8–30.0 fps and 0 dropped, but **memory FAIL: +1,243 MiB** (`coast_pass`; +1,196 MiB in `dense_pass`) over the +1,024 MiB budget. Contingency rebuild at `roof_texel_m = 1.0`: **+971 MiB** at most, inside the budget. Tables below; the choice of `roof_texel_m` is the user's |
| 8 Visual review | EO and MWIR shots | **Pending the user** (shots listed below) |

CIGI (`.cache/r3b/cigi.txt`, `cigi-on.txt`; the sensor at 400 m HAE above the point, looking down, noon):

| Point | Buildings | Response |
|---|---|---|
| Burned cell centre 33.3000417, −117.3177917 (`probe.json` `inside`) | off | **230**, HOT 77.12 m (terrain) |
| 0.0001492° east of it, 33.3000417, −117.3176425 (`outside`, `outside_code` 30) | off | **30**, HOT 75.02 m |
| Warehouse `010048df…` roof, 33.3029558, −117.3144625 (R3's roof 94.67 m) | on | **230**, HOT 94.67 m |
| The burned cell centre again | on | **230**, HOT 85.77 m (its roof, 8.65 m above the terrain) |
| The outside point again | on | 30, HOT 75.02 m |

Bench sweep (`sweep.py --scene pendleton-r4 --axis buildings=off,on --axis trees=off,on --repeat 2`,
`.cache/bench-r3b/combined/`; buildings SSE 64 with physics, trees 1000 m / 1.0 / shadows, both at the defaults; 720p EO).
All-off row: game 3.48 / 6.14–6.30 ms, GPU 19.37–19.58 ms, memory 8,890–8,900 MiB. Increase of the row median over it;
0 dropped frames in every run:

| Buildings | Trees | Game p50 | Game p95 | GPU p50 | Memory, MiB | fps (runs) | Budget |
|---|---|---|---|---|---|---|---|
| off | on | +0.63 | +0.67 | +1.73 | +103 | 29.86 / 29.41 | n/a (not gated; one run 29.41 fps) |
| on | off | +0.56 | +0.82 | +2.08 | +729 | 29.46 / 29.75 | n/a (not gated; one run 29.46 fps) |
| **on** | **on** | **+1.16** | **+1.39** | **+3.04** | **+782** | 29.67 / 29.53 | **pass** |

For comparison, the R3/R4 footprint-check A/B run above (before R3b; runtime check on, `run_bench.py --phases dense_pass`, against the combined sweep's all-off row) had both on at
+1.40 / **+3.61** / +3.54 ms / +408 MiB. The mask takes the game p95 back inside the limit; the roof atlases add about
+370–440 MiB in this 90 s window (buildings-on rows +729 vs +290 MiB). Two single-feature runs emitted 29.41 and 29.46
fps (0 dropped); the gate's row, both on, stayed at 29.53 or above.

Atlas cost alone (`run_bench.py --phases dense_pass --shots dense_oblique`, both features on, one run each,
`.cache/bench-r3b/{textured,flat}-dense/`): textured minus flat: game p50 +0.00 ms, p95 +0.23 ms, GPU p50 −1.35 ms
(run-to-run noise), memory **+383 MiB** (9,669 vs 9,286).

Full bench (`run_bench.py --site pendleton --gpu-mem`, all phases, one run each; `.cache/bench-r3b/full-{off,on}/`,
`compare.py`), both features on at the defaults vs both off on `pendleton-r4`, and the contingency package at
`roof_texel_m = 1.0` (`pendleton-r4-t1`, `.cache/bench-r3b/full-on-t1/`; its mask and land cover are byte-identical
to `pendleton-r4`'s; buildings layer 335.5 MB, all 1,765 tiles at 1.0 m, peak RSS 1,588 MB). Every phase 29.8–30.0
fps and 0 dropped frames in all three runs:

| Phase | Game p50, ms: on / off | Game p95: on / off | GPU p50: on / off | Memory max, MiB: on / off | Memory increase: 0.5 m / 1.0 m | Game p95 increase at 1.0 m |
|---|---|---|---|---|---|---|
| orbit | 3.55 / 2.81 | 5.58 / 4.41 | 19.73 / 19.46 | 9,598 / 8,879 | +719 / +928 | +1.29 |
| slew | 3.66 / 2.97 | 11.53 / 10.77 | 19.56 / 19.75 | 9,631 / 8,885 | +746 / +937 | +0.89 |
| low_pass | 4.11 / 3.25 | 7.69 / 7.32 | 19.79 / 18.99 | 9,636 / 8,937 | +699 / +891 | +0.32 |
| far_origin | 2.32 / 2.21 | 3.38 / 3.10 | 17.07 / 17.01 | 9,618 / 8,911 | +707 / +877 | +0.08 |
| coast_pass | 4.17 / 3.54 | 10.04 / 8.79 | 20.46 / 20.13 | 10,156 / 8,913 | **+1,243** / +971 | +1.01 |
| dense_pass | 4.98 / 3.60 | 10.34 / 8.78 | 23.25 / 20.94 | 10,196 / 9,000 | **+1,196** / +852 | +1.41 |

- Game p95 passes in every phase at both texels (largest +1.57 ms, `dense_pass` at 0.5 m).
- **Memory at 0.5 m fails the whole-run budget** in `coast_pass` (+1,243 MiB) and `dense_pass` (+1,196 MiB); the peak
  of the run is 10,196 MiB against 9,000 MiB off (+1,196). The R3/R4 full bench at the same settings, before R3b,
  peaked at +907 / +952 MiB in those phases, so the atlases add ~250–340 MiB over a whole run. At 1.0 m the largest
  increase is +971 MiB (`coast_pass`; the run's peak 9,884 MiB, +884), inside the budget.
- One run each, and memory moves between runs: the 1.0 m run is *higher* than the 0.5 m run in the first four phases
  (+877 to +937 against +699 to +746 MiB) and lower only in the two built-up phases. A repeat of each would separate the
  texel's effect from run-to-run variation; the default (`roof_texel_m = 0.5`) is unchanged pending the user's choice.

Shots for the visual review (pending the user):

- Roof close-ups, textured vs flat: `.cache/r3b/veg/footprints/pose0.png` (the `dense_pass` midpoint, 357.7 m HAE,
  heading 105°, −30°) and `pose1.png` (Mainside, 350 m, north, −30°) vs `.cache/r3b/veg-flat/footprints/pose{0,1}.png`.
- `dense_oblique`: `.cache/bench-r3b/textured-dense/shots/dense_oblique.png` vs `.cache/bench-r3b/flat-dense/shots/dense_oblique.png`;
  the sweep's contact sheet `.cache/bench-r3b/combined/sweep_sheet.png`.
- Trees near buildings before (runtime check, R3/R4: `.cache/veg/i1-footprints/close_oblique.png`) and after (mask:
  `.cache/r3b/veg/footprints/pose1.png`).
- MWIR day and night roofs: `.cache/r3b/thermal-pendleton/shots/mwir_roofs_{noon,night}_roof.png` and the masks in
  `.cache/r3b/thermal-pendleton/overlays/mwir_roofs_{noon,night}_roof.png`.

### R5 detail polygons (2026-10-14, Linux)

Reference box (Core Ultra 9 285K, 24 cores, 62 GB), the reference container (`scripts/scene/build.sh`, `-j 16`).
`examples/socal-r5.toml`: the 200 km box `[-118.51, 32.65, -116.35, 34.44]` (39,664 km²) at `preview` with the
Pendleton bbox (1,289 km²) as the one `sim` polygon; R2 features, buildings, canopy and the species mix over the whole
box. Reference for gate 3: `pendleton-r5veg` (`examples/pendleton-r5veg.toml`, the same polygon as a `sim` bbox).
Outputs under `.cache/linux-r5/` (untracked).

| Gate | Result |
|---|---|
| Plan | 169 assets, cache 30.6 GB (+45 of unknown size); estimate terrain 232,294 / imagery 288,906 / land cover 6,408 / NDVI 18,910 tiles, ~7.0 GB without buildings. `naip_pc` 44 and `dep3_1m` 12 assets, the same as Pendleton: the scope holds. OSM from the Geofabrik `socal` extract (0.67 GB) |
| Build | **Package complete, exit 2: peak RSS over the 2,048 MB limit** (below). 2 h 22 min, fetch included. Terrain 221,530 tiles, 2.07 GB, 539 s; imagery 286,358, 2.26 GB, 546 s; NDVI 16,257, 0.53 GB; land cover 6,408, 118 MB, **2,798 s**; canopy 1,584, 233 MB, 56 s; buildings **5,737,615 kept** of 5,792,715 (55,100 small), 57,585 tiles, **15.6 GB, 3,257 s**, 143,238 roofs textured (the polygon's NAIP), 5,594,377 flat. Package 21 GB (26 GB with `.state`, which `pack` leaves out) |
| 4: imagery balance | **Pass**: held-out cell bias median 2 / 2 / 2 DN (limit 5; before the fit 33 / 26 / 26), p90 6 / 5 / 5; NDVI fit held-out bias 0.0007 (mae 0.068); `ndvi_check` and `water_check` exit 0 (NDVI seam bias 0.000; water within ~1 DN of the source at z10–z17) |
| 2: scope | **Pass**: no tile deeper than the box's zooms outside the scopes, no scoped footprint outside its scope |
| 3: identity | **Pass**: inside the polygon shrunk by 1 km, 3,652 terrain leaves (z16) and 51,443 imagery leaves (z17) byte-identical to `pendleton-r5veg`; 449 imagery leaves near water exempt |
| 5: verify | **Pass** after two fixes (below): `verify --deep` all `ok`, incl. `detail` and `terrain_edges` (10 min 45 s); scene z9+ p50 0.003 m, p99 0.061 m over 1,777,184 vertices |
| 5: rebuild, resume | **Pass**: `imagery/17` (75,880 tiles) deleted and rebuilt with `--replan`: only those tiles built (210,478 skipped), byte-identical to before; a further build built nothing |

Against a full-`sim` box, scaled from `pendleton-r5veg`'s scene zooms (z9+) by 39,664 / 1,289; the whole-globe z0–z8
(174,762 terrain + 174,762 imagery tiles, 1.5 GB) is the same in both:

| Layer (z9+) | socal-r5 | Full `sim`, estimated |
|---|---|---|
| Terrain | 46,768 tiles, 1.35 GB | 247,000 tiles, 8.0 GB |
| Imagery | 111,596 tiles, 1.48 GB | 3,150,000 tiles, 42.9 GB |
| NDVI | 16,152 tiles, 0.53 GB | 216,000 tiles, 7.8 GB |
| Together | **174,516 tiles, 3.4 GB** | **3,600,000 tiles, 59 GB** (17× the bytes) |

Findings from the build:

- **Buildings at 200 km don't fit the memory limit** (fixed 2026-10-15: built by chunk, below). The buildings layer plans and writes in the main process with
  every building in memory: 5.74 M buildings took the main process to 15.8 GB (sampled from `/proc`; ~12 GB already
  after planning). The land-cover process, which burns the 3.0 GB footprint raster, peaked at 4.5 GB. Tile workers
  stayed under 1.1 GB. The limit is unchanged; streaming the buildings layer per tile is ROADMAP work. The buildings
  tileset is also three quarters of the package (2.7 KB per building, flat roofs).
- **Fixed: per-worker peak RSS was the main process's.** `ru_maxrss` survives `exec`, so every spawned tile worker
  reported the main process's peak at spawn time (all of terrain, imagery and NDVI "peaked" at 12,114 MB). `build.json`
  now reads `VmHWM`: 606 / 743 / 1,043 MB for terrain / imagery / NDVI on the rebuild.
- **Fixed: quantized-mesh header heights rounded to nearest.** Under NumPy 2, `np.float32(x) <= x` compares in
  float32, so a tile's min / max height could sit half a float32 ulp inside its lowest / highest vertex (2 µm at
  38 m; `terrain_edges` caught it on two flat sea tiles). New builds re-encode most terrain headers, so they are not
  byte-identical to packages built before the fix; both packages here were rebuilt with it.
- **Fixed: TNM error bodies.** tnmaccess sometimes answers HTTP 200 with an error body and no items (2 of 6 calls on
  2026-10-14); the planner read that as no products, and a `--replan` planned `dep3_1m` with 0 assets (no 1 m
  terrain in the polygon) with only a warning. Now retried, then an error.
- `dep3_1m` skips the `San_Diego_CA_2014_LiDAR` project (geoid GEOID12A unknown), as for Pendleton.
- `SourceRaster.sample` logs `RuntimeWarning: invalid value encountered in multiply` once per build (also on
  Pendleton): points outside a source CRS's valid area come in as `inf`; they are masked as invalid and the next
  source answers.

Gates 5 (offline), 6 and 7 (RTX 5080, driver 595.99.02, Vulkan; CamSim `camsim:linux-r4` for the renders, the native
editor build for the bench):

| Gate | Result |
|---|---|
| 5: offline | **Pass**: `render_check.py --docker camsim:linux-r4 --offline` (`--network none`), both shot sets: no network line, no gate timeout, ready in 13 / 11 s |
| 6: render | Every shot `terrain_ready` (`--shots detail`: `edge_inland`, `edge_coast`, `outside_high`, `outside_low`; the R0 set: 11 shots). PNGs in `.cache/linux-r5/render/` and `render-r0/`, crops in `.cache/linux-r5/edges/`. No seam at the polygon edges in the crops or the shots; outside the polygon the box is Sentinel-2 10 m (soft at low altitude, `outside_low`) with flat grey roofs, and `outside_low` shows a faint straight horizontal break across its left half. **Visual review by the user: open** |
| 7: bench | **FAIL (fixed 2026-10-15, below): game thread +3.6 to +3.9 ms p50** against `pendleton-r5veg` in every phase but `far_origin` (`run_bench.py --site pendleton --gpu-mem`, one run each; table below). GPU time unchanged, VRAM +430 to +790 MiB, 30 fps and 0 dropped everywhere. The cause is the buildings tileset beyond the polygon (below) |

| Phase | Game p50, ms (pendleton-r5veg / socal-r5) | Game p95 | GPU p50 | VRAM max, MiB |
|---|---|---|---|---|
| orbit | 3.2 / 6.8 | 7.0 / 12.4 | 2.8 / 2.8 | 5,483 / 6,009 |
| slew | 3.3 / 7.1 | 10.5 / 14.2 | 2.9 / 2.9 | 5,467 / 5,900 |
| low_pass | 4.1 / 7.8 | 8.2 / 12.3 | 3.0 / 3.0 | 5,614 / 6,055 |
| far_origin | 2.1 / 2.2 | 5.9 / 6.4 | 2.5 / 2.5 | 5,628 / 6,077 |
| coast_pass | 4.2 / 7.9 | 10.6 / 14.9 | 3.0 / 3.0 | 5,684 / 6,442 |
| dense_pass | 6.1 / 10.0 | 11.9 / 16.0 | 3.3 / 3.3 | 5,792 / 6,583 |
| long_range | 5.5 / 9.4 | 11.7 / 15.8 | 2.8 / 2.9 | 5,690 / 6,523 |

Attribution (`--phases orbit,dense_pass`, one run each), game p50 in ms, pendleton-r5veg / socal-r5:

| Setting | orbit | dense_pass |
|---|---|---|
| All on | 3.15 / 6.82 | 6.09 / 9.97 |
| Buildings off (`CAMSIM_SCENE_BUILDINGS_ENABLED=0`) | 2.81 / 2.85 | 5.05 / 5.59 |
| Vegetation off (`CAMSIM_VEGETATION_ENABLED=0`) | 2.93 / 6.56 | 3.75 / 7.52 |

With buildings off the two packages cost the same (+0.5 ms in `dense_pass` from vegetation now growing beyond the
old bbox); with vegetation off the gap stays. Pendleton's package had buildings only inside its bbox; socal-r5 has
the 5.7 M buildings of the whole box, and from the bench's altitudes the suburbs around Pendleton put many more
buildings tiles in view. Cesium's game-thread cost tracks the rendered tile count (CLAUDE.md, "One terrain tileset").
Options: buildings only inside the polygons (or a `[detail]` margin), a coarser screen-space error for buildings
outside them, or accepting the cost (30 fps held, 0 dropped, game thread ≤ 10 ms p50 of the 33 ms frame).

### R5 follow-up: buildings by chunk and the outside draw distance (2026-10-15, Linux)

RTX 5080, driver 595.99.02, Vulkan; native editor build for the bench; build in `camsim-scene:r0` rebuilt from branch
`buildings-chunked`.

Spike (2026-10-14, `--phases orbit,dense_pass`, one run each, game p50 ms orbit / dense): as built 6.81 / 9.74;
buildings SSE 256 6.82 / 9.65; no buildings physics meshes 6.22 / 9.02; tileset pruned to the polygon + 2 km
3.23 / 6.12; content-less z9–z12 ancestors over every z13 node at geometric error 1400 / 400 / 150: 7.98 / 10.85,
7.92 / 10.85, 3.10 / 6.23. The cost was the buildings tiles near the camera in the suburbs around the polygon (every
z13 tile was a child of the always-refined root), not their detail. Error 150 everywhere dropped the camp's 3D
buildings inside the polygon in `slant_10km` (~10 km), so the cut-off applies only outside the margins; the user
accepted ~2 km there.

| Check | Result |
|---|---|
| `pendleton-r5veg` buildings + land cover rebuilt | **Pass**: `hashes.txt` identical; buildings 33 s, peak 672 MB; land cover 109 s, 456 MB |
| `socal-r5` buildings + land cover rebuilt | Only `features/buildings/tileset.json` changed (the draw distance). **Buildings: 528 s (was 3,257 s), peak 837 MB (was 15.8 GB)**. Partition 18 s, 776 MB; densest chunk (41,910 buildings): plan 273 MB, raster 354 MB, GLBs 319 MB |
| `socal-r5` build gate | **FAIL, now land cover: 2,494 MB** (2,412 s). The main process loads the R2 OSM / NHD features for the whole box (592,951 records: 1,814 MB) and pickles them to the cut process (2,405 MB). It was always there, behind the buildings' 15.8 GB; the build now charges each layer the main process's peak during that layer (`VmHWM` reset between layers) and names it |
| `verify --deep -j 16` | **Pass** both: `pendleton-r5veg` 193 s, `socal-r5` 653 s |
| Gate 7 bench (full, against `pendleton-r5veg`) | **Pass**: game p50 orbit 3.25 / 3.15, dense_pass 6.36 / 6.09 (within 0.3 ms); every phase within +0.5 ms (long_range 5.94 / 5.46, the largest). GPU unchanged, VRAM within ±130 MiB, 30 fps, 0 dropped |
| `slant_10km` | The camp inside the polygon keeps its 3D buildings (`.cache/linux-r5/camp_compare.png`: before / after / error 150 everywhere) |

### R3 bridges (2026-10-15, Linux)

`pendleton-bridges` = `pendleton-r5veg` + `[bridges]` (`scripts/scene/examples/pendleton-bridges.toml`; built
incrementally from a copy: land cover rebuilt with the bridge burn, 142 s, everything else skipped). Linux, RTX 5080.

| | Result |
|---|---|
| Layer | 203 OSM bridge ways → **185 decks** (18 dropped low, 0 ungrounded, 0 without height) in 50 z13 tiles, 4.4 MB; 2 s, 612 MB peak (heights per 0.05° cell; one whole-box call read 1,615 MB) |
| B1 build | 3d-tiles-validator clean (synthetic tileset); `verify --deep` passes (`bridges_tileset`, `bridges_heights`; rules 1 build); rebuild byte-identical; a package without `[bridges]` byte-identical (scene tests) |
| B2 deck | **Pass**: CIGI HOT at mid-span of the 10 highest decks (clearance 19.9–26.2 m) within **0.007 m** of the built deck top, material 201 (9) / 200 (1) |
| B3 ends | **Fail** at 4 of 18 grounded ends (> 0.5 m): HOT at the end vs 5 m out on the approach, 0.01–0.92 m (median 0.28). Two ends (0.66, 0.92 m) are at an at-grade junction 8 m past the bridge (Avenida Vista Hermosa / Avenue Costa Azul): the extension stops short of the crossing road, so the end keeps part of 3DEP's bridge-removal dip. Two (0.51, 0.56 m) match the DEM within 0.1–0.2 m and differ from the *rendered* terrain mesh (TIN error ≤ 0.6 m at z15) |
| B4 drive over | **Pass**: a DIS truck along I-5 at Vista Hermosa (230 m deck) both ways at 10 m/s, COCO `geo.alt_m` every frame: mid-span within **0.013 m** of the deck top, largest frame-to-frame step **0.075 m** |
| B5 drive under | **Pass**: a truck on Old River Road under Mission Avenue stays on the road, its highest point 6.7 m below the deck |
| B6 boat | Not run live: the boat path never takes a deck (deck ceiling −∞, `CamSim.Entity.SurfaceClamp.DeckCeiling`) |
| B7 bench | **Pass**: `run_bench.py --site pendleton --phases dense_pass,coast_pass`, bridges off / on, two runs each (buildings, trees and everything else at defaults): game p50 `dense_pass` 6.03, 6.04 → 6.13, 6.18 ms (**+0.12 ms**), `coast_pass` 4.23, 4.21 → 4.27, 4.33 ms (**+0.08 ms**); GPU p50 unchanged (3.20 / 2.87 ms); 30 fps, 0 dropped in every run; GPU memory max within run-to-run spread (5,584–5,894 MiB) |
| B8 shots | `.cache/r3/bridges/shots/`: EO + MWIR, oblique and nadir, of the three highest decks, for the user's review |

How the gates moved the design (each a rules version): **rules 1** grounded a deck end at the bare earth under the OSM
end node, and B3 found approaches 0.7–1.4 m *above* the deck at 9 of 20 ends: 3DEP's bridge removal cuts past the node,
so the bare earth dips 3–5 m in the last 10–15 m and the embankment top is 15–30 m out. **Rules 2** grounds the end on
the embankment (the highest terrain within 30 m out, deck extended straight to it). **Rules 3** stops an extension
short of another road or rail it would cross (half its width + 1 m), so a deck never covers an at-grade junction.
Drive-over steps went from 0.37 m (rules 1) to 0.075 m.
