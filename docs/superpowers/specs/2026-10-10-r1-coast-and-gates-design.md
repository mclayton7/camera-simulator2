# R1 completion: seabed, local sea level and the R1 gates (macOS)

Date: 2026-10-10. Track: `REALISM.md` R1 (chunks 1 and 2 accepted 2026-10-09/10). Scope agreed with the user
in conversation on 2026-10-10. Linux/Docker items stay open (below).

## Why

A probe of the chunk-2 package (`pendleton-ndvi`) along four transects off the Pendleton coast found:

- USGS 3DEP 1/3" fills ~5 km of ocean with a **hydro-flattened plate at ~0 NAVD88**, 0.2-1.1 m below CamSim's
  tide-0 sea (EGM96; the gap grows offshore because GEOID18 and EGM96 tilt differently). At 3DEP's coverage edge
  the terrain drops in a **cliff to ETOPO bathymetry** (-8 to -150 m within 500 m). This is the cause of the
  "turquoise shallow shelf with a jagged hard outline" known limit (`docs/scene-packages.md`, chunk 1): the ocean
  material reads the plate as shallow water. With waves (Beaufort 3+) or a negative CIGI tide the plate can show
  through troughs.
- CamSim's tide-0 sea is EGM96, but local mean sea level is **+0.774 m NAVD88** at La Jolla (NOAA CO-OPS 9410230,
  1983-2001 NTDE; Oceanside Harbor 9410396 is tidally identical but has no NAVD88 tie), ~0.4 m above EGM96 here.
  The rendered waterline sits ~10-20 m seaward of local MSL on gentle beaches. NOAA's CUSP shoreline is MHW
  (+1.344 m NAVD88).

NOAA CUDEM (the obvious topobathy source) has **no Southern California tiles** (checked 2026-10-10: S3 listings,
both tile indexes, NCEI THREDDS). Two NOAA products do cover Pendleton.

## Goal

1. **Seabed.** Replace 3DEP's plate over tidal water with real NOAA topobathy, continuous with the beach, across
   the bbox and the ring.
2. **Local sea level.** With a scene package, CamSim's tide-0 sea is local MSL, not EGM96.
3. **R1 gates** run on the rebuilt Pendleton package on macOS/Metal, with numbers in `docs/scene-packages.md`.

## Non-goals

- Linux/Docker: `file://` from a mounted volume, the `--network none` offline run (R0 gate), SquashFS vs plain
  directory cold start (R0). Recorded as open in `REALISM.md`; the user runs them later on the RTX 5080 box.
- A CUDEM adapter (no Pendleton tiles to test it on; add it with the first area that has them).
- Spatially varying sea-surface topography (one constant per package), tide prediction, a MHW shoreline.
- Reducing triangle counts over water (follow-up if the bench shows a cost).
- KLV Tags 15/25: they stay EGM96 MSL by definition.

## Part A: seabed (build tooling, `scripts/scene`)

### Sources

| Source id | Product | Extent | Datum | Max zoom | Region |
|---|---|---|---|---|---|
| `noaa_sd13` | NOAA NGDC San Diego, CA 1/3" NAVD 88 Tsunami Inundation DEM (2012). `https://www.ngdc.noaa.gov/thredds/fileServer/regional/san_diego_13_navd88_2012.nc`, 445,596,180 B, Last-Modified 2016-07-29 | lon -117.83..-117.00, lat 32.45..33.60 | NAD83(2011) + NAVD88, `nad83_2011_navd88_geoid18` | 14 | ring |
| `noaa_crm_socal` | NOAA NGDC U.S. Coastal Relief Model, Southern California v2, 3". `https://www.ngdc.noaa.gov/thredds/fileServer/crm/crm_socal_3as_vers2.nc`, 524,449,536 B, Last-Modified 2018-02-09 | lon -128..-115, lat 30..37 | NAD83 + **MSL**; converted to NAVD88 by adding the package's station MSL - NAVD88, then `nad83_2011_navd88_geoid18` | 12 | ring |

- One asset each, pinned by URL + size (the files have been static since 2016/2018); sha256 from `fetch` as for
  every asset. `discover` returns nothing when the area doesn't intersect the product's extent.
- `prepare` converts the netCDF to a COG with `prepare_raster` (overviews, 512 px blocks); the netCDF itself has no
  overviews. The bundled rasterio (1.5.2, GDAL 3.12.2) has the netCDF driver (checked).
- Nodata: `-99999` (sd13), the CRM's `_FillValue` (read from the file's metadata, `-9999` for the 1" file).
- The CRM's netCDF `crs` variable says WGS 84; its ISO metadata says NAD83 (EPSG:4269) + MSL (EPSG:5715). We follow
  the ISO record. sd13's attributes say WGS 84 horizontal; the source list (VDatum 2.3.3, NAVD88 topobathy from US
  surveys) says NAD83. We declare NAD83(2011) for both; the difference at Pendleton is ~1.35 m horizontal, below
  either product's cell size (10 m / 90 m).
- GEOID18 for sd13: the file names no geoid model. Geoid models of NAVD88 differ by centimetres; documented.
- The CRM's MSL -> NAVD88 constant is a source option `msl_above_navd88_m` filled at `plan` from the package's tide
  station (Part B) and stored in the manifest's source record; `open` adds it to every value. A package that uses
  `noaa_crm_socal` without a `[sea_level]` station is a config error.
- Licence `LicenseRef-PublicDomain-USGov` (both: "Not subject to copyright protection within the United States").
  Attribution: "NOAA National Geophysical Data Center (2012): San Diego, CA 1/3 arc-second NAVD 88 Coastal Digital
  Elevation Model" and "National Geophysical Data Center, 2012. U.S. Coastal Relief Model - Southern California
  vers. 2. NOAA. doi:10.7289/V5V985ZM". Both are "not for navigation" (noted in `docs/scene-packages.md`).

### Priority and the tidal-water mask

- `sim` and `preview` terrain priorities become
  `dep3_1m > dep3_13 > noaa_sd13 > noaa_crm_socal > etopo2022` (preview without `dep3_1m`).
- **Tidal-water mask:** a 3DEP sample (`dep3_1m`, `dep3_13`) is invalid where all three hold:
  1. WorldCover class 80 (permanent water), nearest-neighbour at full resolution (the `water.py` lookup);
  2. the 3DEP **source value** (NAVD88, before the vertical offset) is <= `tidal_max_navd88_m` = **+1.5 m** (just
     above MHW +1.34 m), so lakes and reservoirs above sea level keep 3DEP's flattened surface;
  3. a topobathy source (`noaa_sd13` or `noaa_crm_socal`) is valid at the point.
- Masked samples fall through to the next group as nodata does; the existing 30 m feather (distance into the valid
  region) then blends 3DEP into sd13 on the land side of the water edge.
- Recorded in the manifest as `layers.terrain.tidal_mask = {"classes_source": "worldcover", "class": 80,
  "max_navd88_m": 1.5, "sources": ["dep3_1m", "dep3_13"], "topobathy": ["noaa_sd13", "noaa_crm_socal"]}`. A
  manifest without the key (R0, chunk 1, chunk 2) builds exactly as before: byte-identical rebuild of an older
  manifest is a test. With the mask, the terrain tile inputs hash includes the WorldCover asset sha256s (as the
  imagery leaves do with the water mask).
- `verify --deep` applies the same mask in `point_heights`, so its vertex-vs-source comparison covers the new
  sources.
- Tile counts: a tile's depth is min(region limit, best covering source's limit). sd13 (z14) lies under dep3_13
  (z14) and the CRM (z12) under the ring limit (z10), so the tile set is unchanged; `plan` confirms it before the
  build (182,602 terrain tiles for Pendleton).

## Part B: local sea level

### Build

- `scene.toml` gains:

  ```toml
  [sea_level]
  station = "9410230"   # NOAA CO-OPS station with a NAVD88 tie; omit for an inland package
  ```

  `pendleton.toml` sets it. `default_scene_toml` leaves it out.
- `plan` reads `https://api.tidesandcurrents.noaa.gov/mdapi/prod/webapi/stations/<id>/datums.json?units=metric`
  and the station's position (`.../stations/<id>.json`), and writes the manifest section

  ```json
  "sea_level": {"station": "9410230", "name": "La Jolla", "lat": 32.8669, "lon": -117.2571,
                "epoch": "1983-2001", "msl_m": 2.163, "navd88_m": 1.389, "msl_above_navd88_m": 0.774,
                "geoid": "nad83_2011_navd88_geoid18", "egm96_grid": "us_nga_egm96_15.tif"}
  ```

  (numbers as published; the values are stored, so a later rebuild does not query the API). A station without a
  NAVD88 datum is a plan error naming the problem.
- The build computes, at the station's position, with the pinned grids (`PROJ_NETWORK=OFF`):
  `offset_m = h_ell(NAVD88 0, GEOID18 chain) + msl_above_navd88_m - N_EGM96`, where `N_EGM96` is bilinear on the
  15' grid `us_nga_egm96_15.tif` (cdn.proj.org; the same NGA grid CamSim's `WW15MGH.DAC` holds; added to the
  manifest's `datum.grids`). It writes `sea_level.json` at the package root:

  ```json
  {"offset_m": <float>, "station": "9410230", "msl_above_navd88_m": 0.774, "navd88_ellipsoid_m": <float>, "egm96_n_m": <float>}
  ```

  (the 2026-10-10 probe suggests `offset_m` ~ +0.39 m at La Jolla; the build's number is the authority).

  `sea_level.json` is in `hashes.txt`; the build rewrites it every run (cheap) and `verify` recomputes and compares
  it (1 mm).

### Runtime

- `CamSimScene::ResolvePackage` reads `<scene.dir>/sea_level.json` when present: `offset_m` must be finite and in
  [-3, 3] m, else a resolve error. It sets `Cfg.Scene.SeaLevelOffsetM` (0 when the file is absent: inland packages
  and packages built before this change).
- `FOceanSurface` gains `SetDatumOffsetM(double)`; `SeaLevelM(Lat, Lon) = EGM96(Lat, Lon) + DatumOffsetM +
  TideOffsetM`. The subsystem sets it once at startup from the config. The ocean mesh (`OceanMeshBuilder`), HAT/HOT
  (`OceanQueries`), the boat clamp (`SurfaceClamp::ClampWater`), the MPC writes and wave queries all read
  `SeaLevelM`; the implementation audits every `GetGeoidUndulation` caller outside KLV and routes sea-surface uses
  through `SeaLevelM`.
- Logged once at startup: `Scene package sea level: EGM96 + <offset_m> m (NOAA 9410230)`.
- Tests (automation, NullRHI): `sea_level.json` present / absent / out of range / malformed; `FOceanSurface` with a
  datum offset and a tide (sum); `ClampWater` and HOT with an offset.

## Part C: R1 gates (macOS / Metal, final package)

The final package is `pendleton-r1c`: an APFS clone (`cp -cR`) of `pendleton-ndvi` re-planned with the new
priorities, `[sea_level]` and the mask, then built (terrain rebuilds; imagery, land cover and NDVI skip).

| # | Gate | Tool | Pass |
|---|---|---|---|
| 1 | Build | `camsim-scene build --replan -j 6` | Terrain tile count = 182,602 (unchanged); imagery / land cover / NDVI all skipped; terrain size and build time reported vs `pendleton-ndvi` |
| 2 | Vertices vs sources | `verify --deep` | Every check `ok`, including `sea_level` |
| 3 | Coast | new `scripts/scene/tools/coast_check.py` (the 2026-10-10 transect probe made a gate): >= 12 transects across the bbox coast + 4 in the ring, 10 m steps, 6 km offshore | (a) over WorldCover water >= 100 m from land, terrain <= sea - 0.5 m for >= 99 % of samples (sea = EGM96 + offset); (b) no step > 3 m between adjacent 30 m samples within 1 km of the old 3DEP coverage edge; (c) median distance between the terrain/local-MSL crossing and the WorldCover water edge <= 30 m; (d) `sea_level.json` `offset_m` equals an independent pyproj computation within 1 cm; prints the before (`pendleton-ndvi`) numbers too |
| 4 | Heights | `hot_check.py` (frame-centre, 7 points, as R0) + new CIGI HAT/HOT requests (opcode 24) at the same 7 points and 3 sea points | Land: <= 0.25 m (1 m data), <= 1.0 m (1/3"), frame-centre and HOT alike; sea (`CAMSIM_OCEAN_BEAUFORT=0`, tide 0): HOT = EGM96 + offset within 5 cm |
| 5 | Registration | `render_check.py` (package and CWT) + `registration.py` | All EO land shots < 1 px |
| 6 | Offline + visual | `render_check.py --offline`, new shots `coast_low` (300 m, along the beach looking out to sea) and `coast_waves` (Beaufort 5) | No outbound request; user review of `sea_offshore`, `bbox_edge`, `rivermouth`, `ring_edge`, `coast_low`, `coast_waves`: no turquoise shelf, no seabed through troughs, waterline at the beach |
| 7 | Terrain readiness (ROADMAP 0.7) | `render_check.py` records ready time per shot | Every shot reaches `terrain_ready` by load, not the gate's 30 s timeout (no timeout line in the log); cold start and > 5 km teleports reported |
| 8 | Bench | `run_bench.py --site pendleton` (new: base at Pendleton plus a `coast_pass` phase, 300 m along the coast, gimbal toward the sea), 720p, 2 runs each of CWT + Bing, `pendleton-ndvi`, `pendleton-r1c` | Game-thread p50 and p95 of `pendleton-r1c` within max(run-to-run spread, 5 %) of CWT + Bing and of `pendleton-ndvi`, in every phase; 30 fps, 0 drops |

The default SF bench scenario and its baselines are unchanged (`--site sf` is the default).

## Documentation

- `docs/scene-packages.md`: sources table, priorities, "Seabed" section (mask, products, accuracy), "Sea level"
  section, `scene.toml` reference, a "R1 completion" Measured table.
- `docs/configuration.md` ("Scene Package"): `sea_level.json` and its effect.
- `CLAUDE.md` Ocean gotcha: sea level = EGM96 + package offset + CIGI tide.
- `REALISM.md`: status, R1 bullets (seabed, sea level done), open Linux items; the chunk-1 shelf limit closed.
- `ROADMAP.md`: nothing needs the editor.

## Risks

- **sd13's datum labels disagree** (WGS 84 vs NAD83). Gate 3(c) catches a large error; a 1.35 m horizontal error
  is below its cell size.
- **CRM whole-metre values / 1 m accuracy** near shore north of 33.6 N (outside sd13): the ring is z9-10 (~150 m
  samples), coarser than the error.
- **WorldCover's water edge vs the real waterline** (10 m pixels, 2021): the feather absorbs it; gate 3(c) measures
  it.
- **Triangle growth offshore**: measured by gate 8's `coast_pass`.
