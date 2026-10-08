# Realism spike: data sources and tooling for scene packages (US, first area Camp Pendleton)

Research date: 2026-10-07. Test bbox: W -117.62, S 33.19, E -117.24, N 33.52 (Camp Pendleton plus
Oceanside, San Clemente and offshore water).

How it was checked: live queries against primary services where possible (TNM Access API, the
3DEP index ArcGIS service, the USGS 3DHP feature service, the Planetary Computer STAC API, the USDA
NAIP image service, S3 bucket listings, PyPI and GitHub APIs, Overture GeoParquet through DuckDB,
PROJ through pyproj 3.8.0 / PROJ 9.8.1). The ad-hoc scripts were not kept; the spike's are in `scripts/scene/spike/`. **[verified]** = checked against a primary source or live data today. **[secondary]** =
from a secondary source only. **[unconfirmed]** = could not confirm.

---

## 0. Headline findings (decisions and contradictions with REALISM.md)

1. **No public 3DEP LiDAR or 1 m DEM over the Camp Pendleton interior.** [verified] Every
   3DEP LiDAR work unit (WESM) around the base stops at its boundary. The interior (about
   -117.58…-117.33 E, 33.21…33.47 N) has no LPC, no 1 m project DEM, no S1M and no COPC/EPT. The
   1 m tiles that do intersect the bbox are edge slivers (e.g. `x45y369` 3.7 % valid,
   `x46y370` 13 % valid). Only the 1/3 arc-second (~10 m) seamless DEM covers the base. The 3DEP
   1/3" source index credits the interior to `San_Diego_CA_2014_LiDAR`, so the base was probably
   flown but its point cloud isn't public. That is an inference: I found no statement naming
   Pendleton. WESM does have a "Restricted area (military, tribal…)" reason code. This breaks
   REALISM R1's `high` profile (1 m DEM), its LiDAR-vs-terrain gate, and the R3/R4 LiDAR nDSM
   heights *inside the base*. They still work on the fringes (Oceanside, San Clemente, Fallbrook,
   the east side). **Decision needed:** keep Pendleton as the first area but treat the base
   interior as the 1/3" fallback case (which R1 needs to exercise anyway), or add a second area
   with full QL1 coverage for the LiDAR path.
2. **Microsoft Global ML Building Footprints is no longer ODbL.** [verified] Microsoft relicensed
   it to **CDLA Permissive 2.0** on 2026-03-11 (LICENSE commit "Updating Readme and License file to
   reflect the change in license to CDLA Permissive 2.0"). The older US-only
   `microsoft/USBuildingFootprints` repo is still **ODbL**. Overture's buildings theme is still
   **ODbL** as a whole, because of OSM. REALISM's licence row ("OSM, Microsoft footprints and
   Overture are ODbL") needs that correction.
3. **NHD is retired.** [verified/secondary] USGS retired NHD (and NHDPlus HR / WBD editing) on
   2023-10-01. It is still downloadable but static (national file republished 2025-09-18,
   California 2023-12-27). Its successor is the **3D Hydrography Program (3DHP)**: an annual
   download (FY26 CONUS, published 2026-01-23) plus a quarterly-updated feature service. Over
   Pendleton, 3DHP is still the NHD cross-walk (`workunitid = "NHD"`, feature date 2023-09-14),
   with no elevation-derived work unit. REALISM says "NHD High Resolution": change it to "3DHP
   (falls back to static NHD HR)".
4. **HIFLD Open is gone.** [secondary, several consistent sources] DHS took the HIFLD Open portal
   offline on **2025-08-26**. Transmission lines exist only as archives (DataLumos
   doi:10.3886/E240591V1, Data Rescue Project) or behind the secure HIFLD (DHS partners only). Use
   OSM `power=*` as the primary source. REALISM's "verify" note can be closed: not openly
   available any more.
5. **NAIP: California's latest is 2024 at 0.6 m (4-band), but not on PC or AWS.** [verified] The
   USDA image service has CA 2024 tiles over Pendleton (e.g. `m_3311745_ne_11_060_20240820`, flown
   2024-08-20, 4 bands). Planetary Computer's `naip` collection stops at CA 2022, and AWS's three
   NAIP buckets are all requester-pays and list 2010–2023. **NAIP is not masked over the base**
   (MCAS Camp Pendleton's airfield is clearly visible in 2024 imagery). It has no coverage far
   offshore.
6. **The datum conversion needs an explicit pipeline (REALISM principle 7).** [verified] With
   pyproj 3.8/PROJ 9.8, `EPSG:6340+5703 → EPSG:4979` ("WGS 84" ensemble) picks a NADCON5/HARN
   chain through **GEOID03**, not GEOID18: 0.78 m horizontal and 0.14 m vertical off the GEOID18 +
   ITRF2014 result. Going to `EPSG:9755` (WGS 84 G2139) as a compound silently **dropped the geoid**
   (34 m error). Pin the pipeline (GEOID18 `vgridshift` + NAD83(2011)→ITRF2014 Helmert) and test
   it. The source projects also mix geoids (GEOID99/09/12A/12B/18) and CRSs, and 1 m project DEMs
   keep their native geoid.
7. **Overture keeps releases for only 60 days.** [verified] Principle 3 (reproducibility) can't
   point at an Overture release URL. The pipeline must snapshot what it fetches and hash it.
   Current release: `2026-09-23.1`, schema v2.0.0.
8. **Canopy height v2 exists.** [verified] Meta/WRI **CHMv2** (DINOv3, Vantor imagery, CC BY 4.0)
   is on `s3://dataforgood-fb-data/forests/v2/` (global, plus a California set). Prefer it over v1.
9. **Python version window.** [verified] Today's PyPI wheels intersect only on **Python 3.12–3.13**
   on macOS arm64: rasterio 1.5.2 and pyproj 3.8.0 are cp312+, and pydelatin 0.3.0 is cp39–cp313.
   pydelatin 0.4.0, with 3.14 wheels, is tagged on GitHub (2026-10-06) but isn't on PyPI yet.
   rasterio's arm64 wheel is `macosx_15_0`, so it needs macOS 15+.
10. Aside: Cesium for Unreal **v2.30.0** came out on 2026-09-29. CLAUDE.md pins 2.29.1. That only
    matters for open question 2 (`file://` terrain) if behaviour changed.

---

## 1. USGS 3DEP elevation

### 1.1 Access: TNM Access API [verified by use]
- Base `https://tnmaccess.nationalmap.gov/api/v1/`; `GET /products?bbox=W,S,E,N&datasets=<name>&max=&offset=&outputFormat=JSON`
  and `GET /datasets`. No auth. Pages up to 1000 items. Rate limits aren't documented
  [unconfirmed]; the docs page is a JS app I couldn't render.
- Dataset names (from `/datasets`): `Digital Elevation Model (DEM) 1 meter`,
  `Seamless 1-m DEM (S1M)` (titled "Seamless 1-meter DEM (Limited Availability)"),
  `National Elevation Dataset (NED) 1/3 arc-second`, `Original Product Resolution (OPR) Digital Elevation Model (DEM)`,
  `Lidar Point Cloud (LPC)`, `3D Hydrography Program (3DHP)`, `National Hydrography Dataset (NHD) Best Resolution`,
  `National Structures Dataset (NSD)`.
- Files are plain HTTPS on `prd-tnm.s3.amazonaws.com/StagedProducts/...` (anonymous S3; no
  requester-pays). LAZ comes from `rockyweb.usgs.gov/vdelivery/Datasets/Staged/Elevation/LPC/Projects/...`.
- Spatial metadata: `https://index.nationalmap.gov/arcgis/rest/services/3DEPElevationIndex/MapServer`
  (layer 24 = LiDAR work units with `ql`, `collect_start/end`, `horiz_crs`, `vert_crs`, `geoid`,
  `*_category/*_reason`; layer 21 = 1/3" source). **Use this for the manifest's per-project QL
  and year.**
- Licence: "All 3DEP products are available, free of charge and without use restrictions"
  (https://www.usgs.gov/3d-elevation-program/about-3dep-products-services). USGS public domain;
  AWS registry: "US Government Public Domain".

### 1.2 Products

| Product | Format / CRS / vertical | Resolution | Status 2026 | Pendleton |
|---|---|---|---|---|
| 1 m project DEM | GeoTIFF with overviews (tiled COG-like), 10×10 km UTM tiles; **EPSG:26911** (NAD83 UTM 11N) seen on 2014 tiles; NAVD88, project geoid | 1 m | Per project, grows as LiDAR lands | 37 tiles from 6 projects, **all on the fringes**; interior empty |
| S1M seamless 1 m | COG, **EPSG:6350** (NAD83(2011) CONUS Albers), NAVD88 **GEOID18**, voids backfilled from 1/9" or 1/3", nodata -999999, GeoPackage source metadata | 1 m | Production since mid-2025, "limited availability"; ScienceBase DOI 10.5066/P13LJKFS | **0 tiles** in bbox |
| 1/3 arc-second seamless | GeoTIFF, LZW, tiled, overviews; **EPSG:4269** (NAD83), NAVD88; 1°×1° tiles (`n34w118`) | ~10 m (9.26e-5°) | Updated continuously; `current/n34w118/USGS_13_n34w118.tif`, latest version 2026-09-15 (dated copies under `historical/`) | **Full coverage**; interior source = San_Diego_CA_2014_LiDAR per index; sampled valid values (e.g. 194.8 m at 33.33 N, -117.40 E) |
| OPR source DEMs | GeoTIFF, project native CRS | 0.5–1 m | Per project | 894 tiles in bbox (fringes) |

Projects intersecting the bbox (WESM layer 24) [verified]:

| Work unit | QL | Collected | Spec | CRS / geoid | Notes |
|---|---|---|---|---|---|
| CA_SanDiegoCo_1_D24 | **QL1** | 2024-11-08 → 2024-12-01 | LBS 2024 rev A | EPSG 6340 / 5703, GEOID18 | LPC published 2026-09-10; 1 m DEM 2026-09-14/15. East and north-east of the base, Oceanside/Fallbrook. **Not yet in EPT** (404) |
| CA_CaliforniaGaps_4_B23 | **QL1** | 2023-09-08 → 2023-12-11 | LBS 2023 rev A | 6340 / 5703, GEOID18 | NE corner; in EPT (149 G pts) |
| CA_SoCal_Wildfires_B1_2018 | QL2 | 2018-05 → 2018-10 | LBS 2.0 | 6350 / 5703, GEOID12B | North/NW strip (San Clemente) |
| CA_E_SanDiegoCo_2016 | QL2 | 2016-10 → 2017-01 | LBS 1.2 | 6426 / 6360 (ftUS), GEOID12B | NE |
| CA_SanDiego_2015_C17_1 | QL2 | 2015-10 → 2016-11 | LBS 1.2 | 6426 / 6360, GEOID12B | East edge |
| CA_SanDiegoQL2_2014 | QL2 | 2014-10 → 2015-02 | LBS 1.0 ("meets with variance") | 6426 / 6360, GEOID12A | South (Oceanside); 0.61 m DEM GSD |
| CA_WestCoastElNinoUTM11_2016 | QL2 | 2016-04 → 2016-05 | LBS 1.2, **"Does not meet" (breaklines not delivered)** | 6340 / 5703, GEOID12B | Thin beach strip along the coast |
| CA_SCRIPPS_* (2002–2006), CA_ORANGECO_2011 | legacy "Other" | 2002–2011 | — | GEOID99/09 | Coastal legacy; ignore |

ASCII map of WESM coverage (`.` = no 3DEP LiDAR; grid 48×34 over the bbox, north up): the
interior hole runs from about 33.21 to 33.47 N and -117.58 to -117.33 E, with only the diagonal
El Niño beach strip crossing it.

### 1.3 Point clouds on AWS / PC [verified]
- `s3://usgs-lidar-public` (us-west-2, **not** requester-pays, anonymous): Entwine Point Tiles
  (EPT, LAZ). Index `https://raw.githubusercontent.com/hobuinc/usgs-lidar/master/boundaries/resources.geojson`.
  It covers the bbox for: CaliforniaGaps_4_B23, SoCal_Wildfires_B1_2018, E_SanDiegoCo_2016,
  SanDiego_2015_C17_1, SanDiegoQL2_2014, WestCoastElNinoUTM11_2016, OrangeCo_2011 and the Scripps
  legacy sets. **CA_SanDiegoCo_1_D24 isn't there yet.**
- `s3://usgs-lidar` (us-west-2, **requester-pays**): raw LAZ 1.4. More complete than EPT, but not a
  full mirror.
- Planetary Computer `3dep-lidar-copc` (COPC, SAS-token access): the collection extent is frozen
  at 2012–2022 (no 2023/2024 QL1). There are 500+ items in the bbox, and **none at interior
  points**.
- Recommendation: stream EPT (PDAL `readers.ept` with bounds) or LAZ from rockyweb for the newest
  projects. Don't depend on PC COPC for currency.

### 1.4 Vertical datum and geoid [verified]
- NAVD88 (EPSG:5703) is still the official US vertical datum. NSRS modernisation (NATRF2022 /
  NAPGD2022 / GEOID2022) is in phased beta in 2026, with official release expected in 2027
  [secondary: Esri blog citing the April 2026 FGCS meeting; NGS FAQ says the current NSRS stays
  official until then]. The manifest should record the datum so a later migration is visible.
- GEOID18 grid: `https://cdn.proj.org/us_noaa_g2018u0.tif` (HTTP 200, 16.7 MB). PROJ fetched and
  used it automatically with network enabled (pipeline step `vgridshift grids=us_noaa_g2018u0.tif`).
  For reproducibility, vendor the grid into the fetch cache (`projsync` or direct download) with a
  sha256, and run with the network off.
- **PROJ pipeline test** (point 470000 E, 3685000 N, 100 m NAVD88; source `EPSG:6340+5703`):

  | Target | First pipeline PROJ picks | Result h (m) | Horizontal vs ITRF2014 |
  |---|---|---|---|
  | EPSG:7912 ITRF2014 | GEOID18 vgridshift + NAD83(2011)→ITRF2014 time-dependent Helmert (acc 0.015 m) | 65.283 | 0 |
  | EPSG:9989 ITRF2020 | same family | 65.285 | ~0 |
  | EPSG:4979 WGS 84 (ensemble) | NADCON5 2011→2007→FBN, **GEOID03**, HARN… (acc 1.14 m) | 65.423 | **0.78 m** |
  | EPSG:4979, 2nd candidate | GEOID18 + "NAD83(2011) to WGS 84 (1)" null transform | 66.022 | ~1 m (null) |
  | EPSG:9755 WGS 84 (G2139) | **geoid dropped** (NAVD88 treated as ellipsoidal) | 99.261 | 0 |

  Decision: target ITRF2014 (≈ WGS 84 G2139 at cm level) through an explicit, tested PROJ pipeline
  string stored in the manifest. Note that the Helmert is time-dependent (`t_epoch=2010`). Pendleton
  is on the Pacific plate side, moving several cm/yr relative to NAD83, so choose and record a
  coordinate epoch (2010.0 by default, or the collection epoch). That is decimetres over 15 years:
  small next to the 1 m-class error of picking the wrong pipeline, but record it. Cesium World
  Terrain's own epoch/realisation is undocumented [unconfirmed], so expect a decimetre-level step
  at the package edge feather anyway.

---

## 2. NAIP imagery

| Item | Finding |
|---|---|
| Latest CA year / GSD | **2024, 0.6 m, 4-band (RGB+NIR, "CNIR")** [verified: USDA image service `apps.geo.fpac.usda.gov/geo-imagery/rest/services/naip/conus_naip/ImageServer`; Pendleton tiles dated 2024-08-20; CA Dept of Technology "NAIP 2024 60cm California" item]. CA history on PC: 2012/2014 at 1 m, 2016–2022 at 0.6 m. 2025: about half the states at 0.3 m [secondary: NAIP hub]. Whether CA flies in 2026, and at what GSD, is [unconfirmed]. |
| Format / CRS | DOQQ GeoTIFF/COG, UTM NAD83 per zone (EPSG:26911 here), 8-bit, about 3.75′ quarter-quads; from 2024 the tile buffer is 12 m on all sides [secondary]. |
| Planetary Computer | **Still operating** (STAC API answered live searches today). The `naip` collection ends at 2023; for CA the newest is **2022** (37 tiles in bbox). Assets on `naipeuwest.blob.core.windows.net` (needs PC SAS token). The STAC `license` field says "proprietary", a metadata quirk; FSA is the licensor. |
| AWS | `naip-analytic` (4-band MRF + COG), `naip-source` (raw 4-band GeoTIFF), `naip-visualization` (3-band JPEG COG): **all requester-pays**, us-west-2. Anonymous listing returns 403. Registry: "2010 through 2023", licence "Public Domain with Attribution", managed by Esri. |
| Source Cooperative | No official NAIP mirror found. One third-party collection (`portolan/.../naip-mosaic`) links to PC files. |
| USGS | EarthExplorer download (USGS NAIP index lists CA 2022 0.6 m 4-band tiles with EE download URLs). Needs an EROS login (M2M API). Whether CA 2024 is on EE yet is [unconfirmed]. |
| USDA | The `conus_naip` ImageServer (4-band, latest per area) supports `exportImage`, verified. That makes a 2024 path that needs no EE login, but it's a mosaic service, so per-tile provenance has to come from the `query` (Name / QQDATE). Also the USDA Geospatial Data Gateway [not checked]. |
| Licence | Public domain [secondary: Google Earth Engine catalog "considered public domain information", USGS "Sources/Usage: Public Domain", AWS registry "Public Domain with Attribution"]. The 2017–18 proposal to move NAIP to a licensed model was deferred, and NAIP stayed public domain at least through 2019 acquisitions. I couldn't open FSA/FPAC's own terms page (timed out) [unconfirmed for 2024+]. Credit line requested: "USDA Farm Production and Conservation – Business Center, Geospatial Enterprise Operations". |
| Pendleton | **Not masked**: 2024 imagery shows MCAS Camp Pendleton in full detail. Offshore coverage stops a few km out (black no-data blocks in the export). |

Decision: source NAIP 2024 from the USDA image service or EarthExplorer. PC and AWS only serve
2022 for CA, and AWS costs egress. Record `QQDATE` per tile for the sun-angle metadata.

---

## 3. Water and shoreline

### 3DHP / NHD [verified]
- **NHD**: retired 2023-10-01 ("NHD data will continue to be available, but no longer
  maintained", usgs.gov/national-hydrography). Downloads still on TNM: national NHD HR
  (GDB/GPKG, published 2025-09-18), California state (2023-12-27), HU4 1807, HU8
  18070301/18070302 (2024-01-05).
- **3DHP**: annual CONUS download, FY26 published 2026-01-23 (GDB and GPKG,
  `StagedProducts/Hydrography/3DHP/Annual/...`), plus a quarterly-updated service at
  `hydro.nationalmap.gov/arcgis/rest/services/3DHP_all/FeatureServer` (Flowline 50, Waterbody 60,
  Catchment 80, work-unit metadata 92/94; served in EPSG:3857). The service is slow (bbox queries
  timed out at 120 s); use the file download for builds.
- Pendleton: no 3DHP elevation-derived work unit in the bbox; flowlines are `workunitid = NHD`
  (cross-walked NHD). Effectively the same geometry as NHD HR. Public domain (USGS).
- Recommendation: the pipeline reads 3DHP (current model, and it gets better automatically when
  USGS replaces NHD-crosswalk units), with NHD HR as a fallback. OSM water as a gap filler, per
  REALISM.

### NOAA shoreline (CUSP) [verified InPort, partly]
- NOAA NGS **Continually Updated Shoreline Product**. Download through the NOAA Shoreline Data
  Explorer (`https://nsde.ngs.noaa.gov/`, "Download CUSP by Region": West, etc.; shapefile/KML)
  or `https://www.ngs.noaa.gov/CUSP/`. Vector, **NAD83 (EPSG:4269)**. Status "On Going",
  maintenance "None Planned"; InPort record last modified 2026-03-04.
- Use constraints: none for access; "not for use in litigation", not for navigation; credit NOAA.
  Public domain (US Gov).
- The tidal datum isn't stated on the InPort record. CUSP is generally described as a mean high
  water shoreline [unconfirmed here]. This matters for R1's coast check: CamSim's sea level is
  EGM96 + tide, while MHW sits above local MSL (about +0.7 m at San Diego [unconfirmed]).

---

## 4. Buildings and transportation

### Overture Maps [verified]
- Latest release **2026-09-23.1** (S3 `s3://overturemaps-us-west-2/release/2026-09-23.1/`, also
  Azure). Schema v2.0.0. Next releases 2026-10-21.0, 2026-11-18.0, 2026-12-16.0 (major-change
  month). **Retention: 60 days** (two monthly releases), then deleted. Bridge files/changelogs kept.
- Licences (docs.overturemaps.org/attribution): **buildings = ODbL** (© OpenStreetMap contributors;
  other sources credited: Esri Community Maps CC BY 4.0, Microsoft Global ML Building Footprints,
  Google Open Buildings CC BY 4.0, USGS 3DEP…), **transportation = ODbL** (OSM + TomTom credit),
  **base = ODbL** (+ ESA WorldCover CC BY 4.0), divisions ODbL, places and addresses per-source
  (CDLA-P 2.0 / Apache 2.0 / CC0 / various). Overture's attribution page still describes the
  Microsoft source as ODbL. That page lags Microsoft's March 2026 change, but it doesn't matter
  here: the theme is ODbL anyway.
- Pendleton (DuckDB over S3, bbox predicate, ~80 s): **81,825 buildings** in the bbox. 70,888 come
  from OSM and 10,937 from Microsoft ML. **59,841 (73 %) have `height`**: 51,978 OSM (median 4.6 m)
  and 7,863 Microsoft (median 3.8 m). Only 1,098 have `num_floors`. Inner-base sub-box
  (-117.50…-117.33, 33.22…33.45): 6,223 buildings, 4,049 with height. OSM heights this dense
  suggest a San Diego county (SANGIS) import [unconfirmed]. Since there's no LiDAR inside the base,
  Overture/OSM heights become the **primary** height source there, not the second choice as in
  R3's order.
- Python: `overturemaps` 1.0.2 (MIT, pure Python) or DuckDB 1.5.6 (MIT; macOS universal2 wheel).

### Microsoft footprints [verified]
- `microsoft/USBuildingFootprints`: **ODbL**. GeoJSON per state, imagery mostly 2019–2020 (older
  elsewhere, averaging about 2012), **no heights**, legacy `usbuildings-v2` paths, no recent releases.
- `microsoft/GlobalMLBuildingFootprints`: **CDLA Permissive 2.0** since 2026-03-11. Last refresh
  2026-08-13. Hosting moved on 2026-07-24 to `https://bfppub.blob.core.windows.net/$web/<date>/dataset-links.csv`
  (old `minedbuildings.z5.web.core.windows.net` frozen). Files are `.csv.gz` holding line-delimited
  GeoJSON, EPSG:4326. Height is -1 where unknown. 2026 releases added several million US
  footprints with heights (Vexcel/Maxar 2020–2025). The CDLA-P 2.0 licence fits redistributed
  training data better than ODbL.

### OSM via Geofabrik [verified]
- `https://download.geofabrik.de/north-america/us/california.html`: `california-latest.osm.pbf`
  1.2 GB, `socal-latest.osm.pbf` 639 MB, daily builds plus dated snapshots, `.osc.gz` updates.
  **ODbL 1.0**, "© OpenStreetMap contributors". Public extracts strip user metadata. For pinning,
  store the dated snapshot and its hash.
- I couldn't count OSM `power=line` in the bbox (both Overpass instances failed) [unconfirmed].

---

## 5. Vegetation

| Dataset | Access | Format / res | Licence | Status |
|---|---|---|---|---|
| **Meta/WRI CHM v1** | `s3://dataforgood-fb-data/forests/v1/` (us-east-1, `--no-sign-request`, not requester-pays): `alsgedi_global_v6_float/` (chm/, msk/, metadata, `CHM_acquisition_date.tif`, `tiles.geojson`), `California/alsgedi_ca_v5_float/`, `.../v6_float/` | GeoTIFF + GeoJSON obs. dates; ~1 m (paper: "sub-meter"); Maxar 2016-era source imagery | Data **CC BY 4.0** (AWS registry); code/weights Apache-2.0 (GitHub) | Superseded by v2 |
| **Meta/WRI CHMv2** | `s3://dataforgood-fb-data/forests/v2/global/dinov3_global_chm_v2_ml3/` and `forests/v2/California/dinov3_global_chm_v2_ml3/chm/*.tif` (quadkey-named, uploaded 2025-12-09); registry `registry.opendata.aws/dataforgood-fb-forestsv2` | GeoTIFF + GeoJSON obs. dates; meter-scale; Vantor (ex-Maxar) imagery | **CC BY 4.0** | Paper: Sci. Data 2026 (s41597-026-07882-0). Code: facebookresearch/dinov3 (code licence not checked [unconfirmed]) |
| **ETH Global Canopy Height 2020** (Lang et al.) | ETH Research Collection doi:10.3929/ethz-b-000609802; GEE `users/nlang/ETH_GlobalCanopyHeight_2020_10m_v1`; tile browser on langnico.github.io/globalcanopyheight | 10 m, year 2020, + SD layer | **CC BY 4.0** ("free of charge, without restriction of use") | Static |
| **Annual NLCD Collection 1.2** (USGS/MRLC) | MRLC viewer/downloads, ScienceBase, web services, **`s3://usgs-landcover/annual-nlcd/c1/v0/...` requester-pays** (us-west-2) | 30 m, CONUS Albers; products LndCov, LndChg, LndCnf, **FctImp (fractional impervious %)**, ImpDsc, SpcChg | No use restrictions (USGS; citation requested) | C1.2 published 2026-06-10, covers 1985–**2025** |
| **NLCD Tree Canopy Cover v2025-6** (USFS) | `https://data.fs.usda.gov/geodata/rastergateway/treecanopycover/`, USFS ImageServers, GEE `projects/gtac-data-publish/assets/TCC_Product_Version_2025-6` | 30 m %, 1985–2025; "Science" (raw + SE) vs "NLCD" (post-processed) versions | US Gov funded, usable without permission/fees | Current |

Recommendation for R4: CHMv2 (CC BY 4.0) as the canopy-height prior everywhere (it's the only
fine-scale option inside the base), NLCD TCC v2025-6 (NLCD version) for canopy %, and Annual NLCD
C1.2 FctImp for impervious %. REALISM's "NLCD tree canopy cover %" should name **USFS TCC
v2025-6**: it isn't part of the Annual NLCD collection.

---

## 6. Infrastructure

- **HIFLD Open**: offline since **2025-08-26** (announced June 2025 for 2025-09-30, then brought
  forward) [secondary: atcoordinates.info; Data Rescue Project; DataLumos]. DHS published a
  crosswalk to source portals [not opened]. Archived copies: DataLumos "HIFLD OPEN Transmission
  Lines" doi:10.3886/E240591V1 (GeoJSON/SHP); Data Rescue Project portal (0.08 GB, page updated
  2026-01-21). The secure HIFLD is for DHS partners only. The archive's licence isn't stated
  clearly. HIFLD Open layers were generally public (US Gov / ORNL) but carried mixed source terms
  [unconfirmed].
- EIA's U.S. Energy Atlas transmission layer is itself HIFLD-derived [secondary: EIA FAQ], so it
  isn't independent.
- USGS National Structures Dataset (NSD): California file 2026-02-26, national 2026-02-28
  (points: schools, fire stations, etc.; public domain). Useful for building class hints.
- **Recommendation:** OSM `power=line/minor_line/tower/pole` (ODbL) as primary. A frozen HIFLD
  archive snapshot only as an optional cross-check, flagged `licence: unclear` in the register.

---

## 7. ESA WorldCover [verified]
- Licence **CC BY 4.0** (Zenodo record 7254221, `license: cc-by-4.0`, v200, published 2022-10-28).
  Attribution: "© ESA WorldCover project / Contains modified Copernicus Sentinel data (2021)
  processed by ESA WorldCover consortium".
- **No newer version.** `s3://esa-worldcover` has only `v100/` (2020) and `v200/2021/`. Secondary
  catalogs (Planet, Sentinel Hub) list 2020 and 2021 only. v200 2021 stays current.

---

## 8. Tooling

| Tool | Licence | Latest | Maintenance | macOS arm64 | Notes |
|---|---|---|---|---|---|
| **pydelatin** (kylebarron) | MIT | PyPI 0.3.0 (2025-06-25); GitHub v0.4.0 (2026-10-06, adds 3.14 wheels; not on PyPI yet) | Active (pushed 2026-10-06) | Wheels cp39–cp313 | Delatin TIN from a height grid, error-bounded. Its PyPI metadata has an empty licence field, but the repo is MIT |
| **quantized-mesh-encoder** | MIT | 0.5.0 (2025-06-24) | Repo active (pushed 2026-10-05), no release since | Wheels cp39–cp313 | Encodes quantized-mesh-1.0 (+ oct-encoded normals ext.). Doesn't write `layer.json` or tile pyramids: we write those |
| pymartini | MIT | 0.5.1 (2025-06-24) | Active | Wheels | RTIN alternative (needs 2ⁿ+1 grids) |
| quantized-mesh-tile (loicgasser) | MIT | — | Pushed 2026-03 | Pure Py | Read/write QM tiles; useful for verify/tests |
| cesium-terrain-builder (geo-data) | Apache-2.0 per project [LICENSE path not found; unconfirmed] | — | **Dormant** (last push 2023-03) | C++/GDAL build | Heightmap-only upstream |
| CTB fork ahuarte47 (quantized-mesh branch) | as upstream | — | Active (pushed 2026-08-18) | Build from source / Docker | Quantized-mesh output; tum-gis/cesium-terrain-builder-docker (Apache-2.0) dormant since 2023-08 |
| Cesium ion (tiling) | Proprietary SaaS / commercial self-hosted | — | — | — | Not usable for an open, offline pipeline |
| **pg2b3dm** (Geodan) | MIT | v2.28.0 (2026-10-01) | Very active | .NET tool, needs **PostGIS** | 3D Tiles 1.1, **EXT_mesh_features + EXT_structural_metadata**, implicit tiling, LOD column, extrudes polygons (with i3dm.export for instances, MIT) |
| **py3dtiles** (now gitlab.com/py3dtiles/py3dtiles) | Apache-2.0 | 12.1.1 (2026-03-20) | Active | Pure Py (needs py ≥3.10, <3.15) | Point clouds (las/laz/xyz/ply) and WKB → b3dm/pnts with **batch tables**; glTF tile content per 1.1 since 12.1.1. **No EXT_structural_metadata** found in its changelog |
| **3d-tiles-tools** (CesiumGS) | Apache-2.0 | npm 0.5.4 (2026-07-15) | Active | Node | Utilities: upgrade to 1.1 (b3dm→glTF incl. metadata), merge/combine, glb↔b3dm/i3dm, tilesetToDatabase, `createTilesetJson`. Not a footprint extruder |
| Hand-rolled (trimesh 5.1.1 MIT + mapbox-earcut 2.1.0 ISC + pygltflib 1.16.5 MIT) | permissive | — | Active | Wheels | Write glTF with `EXT_mesh_features` (feature IDs) + `EXT_structural_metadata` property tables ourselves. Small schema (class, height_source, source_id); avoids a PostGIS dependency |
| rasterio | BSD-3 | 1.5.2 (2026-09-30) | Active | **Wheel bundles GDAL**, `macosx_15_0_arm64`, cp312–cp315 | Verified working via `uv run --with rasterio` (vsicurl reads of 3DEP COGs) |
| rio-cogeo | BSD-3 | 7.0.3 (2026-09-24) | Active | Pure Py | COG creation/validation |
| rio-tiler | BSD-3 | 9.4.10 (2026-10-06) | Active | Pure Py | Tile reads from COGs (XYZ/TMS) with morecantile 7.1.0 (MIT) TMS definitions |
| gdal2tiles | MIT (GDAL) | GDAL 3.13.3 | Active | **No PyPI wheel**: needs Homebrew/conda GDAL | Use rio-tiler/morecantile in-process, or GDAL from conda-forge if gdal2tiles is wanted |
| pyproj | MIT | 3.8.0 (2026-09-05), PROJ 9.8.1 | Active | Wheel cp312+ | Grids **not bundled**: GEOID18 `us_noaa_g2018u0.tif` from cdn.proj.org (network or `projsync`) |
| PDAL Python (`pdal`) | BSD | 3.5.5 (2026-07-31) | Active | **No wheel**: needs conda/Homebrew PDAL | Alternative pure-wheel stack: laspy 2.7.0 (BSD-2) + lazrs 0.8.2 (MIT, arm64 wheel) for LAZ; EPT/COPC readers then need custom code |
| duckdb | MIT | 1.5.6 | Active | universal2 wheel | Overture GeoParquet straight from S3 (verified) |
| shapely 2.2.0 / pyogrio 0.13.0 / geopandas 1.2.0 | BSD/MIT | 2026 | Active | Wheels (pyogrio arm64 wheels cp310/311/314 only listed: **check 3.12/3.13**) [partly unconfirmed] | |

Decisions:
- Terrain: pydelatin + quantized-mesh-encoder (both MIT, wheels, maintained) with our own
  `layer.json`/tile pyramid writer. Skip CTB (dormant, C++ build).
- Buildings: either pg2b3dm (proven EXT_mesh_features/EXT_structural_metadata, but drags in .NET +
  PostGIS) or a small in-house glTF writer. The in-house writer fits the "uv project, no system
  deps" principle, and 3d-tiles-tools `validate`/`upgrade` can serve as the reference checker
  (CesiumGS 3d-tiles-validator, not checked here).
- Python pinned to **3.12 or 3.13** until pydelatin 0.4.0 reaches PyPI. Needs macOS 15+ for
  rasterio's arm64 wheel.
- LiDAR: PDAL has no wheels. If R1/R3 want EPT, either accept a conda/Homebrew PDAL or use
  laspy+lazrs with a small EPT reader.

---

## 9. Restrictions over military installations (Camp Pendleton)

| Dataset | Restricted over the base? | Evidence |
|---|---|---|
| 3DEP LiDAR (LPC, EPT, COPC), 1 m DEM, S1M | **Yes, in effect: no public data inside the base** | WESM work-unit polygons wrap around the base; 1 m tiles are slivers; no S1M; no PC COPC at interior points. WESM has a "Restricted area" reason code, but the work units here don't carry it (their categories are "Meets"); the base is simply outside their footprints. Cause [unconfirmed] |
| 3DEP 1/3 arc-second | No | Valid values everywhere; source index says San_Diego_CA_2014_LiDAR |
| NAIP 2022/2024 | No | Unmasked imagery of MCAS Camp Pendleton and the cantonment areas |
| Overture/OSM buildings | No | 6,223 buildings in an inner-base sub-box; OSM covers the cantonments |
| Microsoft footprints | Present (part of Overture's 10,937) | — |
| HIFLD | n/a (portal gone) | — |

Other notes:
- Distribution of imagery of DoD installations isn't restricted for NAIP in practice (it's
  public). I found no NAIP policy text on masking, and Pendleton isn't masked.
- ATR/training use of imagery of a named US military base isn't a licence issue for
  public-domain data. Whether the project wants a named base as its showcase area is a policy
  question, not a data one.

---

## 10. Sources (primary unless noted)

- TNM Access API: https://tnmaccess.nationalmap.gov/api/v1/products , /datasets
- 3DEP index service: https://index.nationalmap.gov/arcgis/rest/services/3DEPElevationIndex/MapServer
- 3DEP products/licence: https://www.usgs.gov/3d-elevation-program/about-3dep-products-services
- WESM reason codes: https://www.usgs.gov/ngp-standards-and-specifications/wesm-data-dictionary-reason-codes-lidar-point-cloud
- S1M ScienceBase: https://www.sciencebase.gov/catalog/item/67af83c1d34e5020fb91f170
- USGS LiDAR on AWS: https://registry.opendata.aws/usgs-lidar/ ; EPT index https://github.com/hobuinc/usgs-lidar
- Planetary Computer STAC: https://planetarycomputer.microsoft.com/api/stac/v1 (naip, 3dep-lidar-copc, 3dep-seamless)
- D24 LPC report: https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/metadata/CA_SanDiegoCo_D24/CA_SanDiegoCo_1_D24/reports/
- NSRS modernisation: https://geodesy.noaa.gov/datums/newdatums/FAQNewDatums.shtml ; Esri blog (secondary) https://www.esri.com/arcgis-blog/products/arcgis-pro/data-management/prepare-your-data-for-the-nsrs-2022
- GEOID18 grid: https://cdn.proj.org/us_noaa_g2018u0.tif
- NAIP on AWS: https://registry.opendata.aws/naip/ ; USDA NAIP ImageServer: https://apps.geo.fpac.usda.gov/geo-imagery/rest/services/naip/conus_naip/ImageServer ; USGS NAIP index https://index.nationalmap.gov/arcgis/rest/services/USGSNAIPImageryIndex/MapServer ; NAIP hub https://naip-usdaonline.hub.arcgis.com/ ; GEE NAIP (secondary, licence) https://developers.google.com/earth-engine/datasets/catalog/USDA_NAIP_DOQQ
- National Hydrography / NHD retirement: https://www.usgs.gov/national-hydrography ; 3DHP service https://hydro.nationalmap.gov/arcgis/rest/services/3DHP_all/FeatureServer
- CUSP: https://www.fisheries.noaa.gov/inport/item/60812 ; https://nsde.ngs.noaa.gov/
- Overture: https://docs.overturemaps.org/release-calendar/ ; https://docs.overturemaps.org/attribution/ ; s3://overturemaps-us-west-2/release/
- Microsoft: https://github.com/microsoft/GlobalMLBuildingFootprints (LICENSE commit 2026-03-11) ; https://github.com/microsoft/USBuildingFootprints
- Geofabrik: https://download.geofabrik.de/north-america/us/california.html
- Meta CHM: https://registry.opendata.aws/dataforgood-fb-forests/ ; https://registry.opendata.aws/dataforgood-fb-forestsv2/ ; https://github.com/facebookresearch/HighResCanopyHeight
- ETH canopy height: https://langnico.github.io/globalcanopyheight/ ; https://doi.org/10.3929/ethz-b-000609802
- Annual NLCD: https://www.mrlc.gov/data/project/annual-nlcd ; https://www.usgs.gov/centers/eros/science/annual-nlcd-data-access ; TCC: https://www.mrlc.gov/data/type/nlcd-tree-canopy-cover , https://data.fs.usda.gov/geodata/rastergateway/treecanopycover/
- HIFLD (secondary): https://atcoordinates.info/2025/08/08/hifld-open-gis-portal-shuts-down-aug-26-2025/ ; https://portal.datarescueproject.org/datasets/hifld-open-transmission-lines/ ; https://www.datalumos.org/datalumos/project/240591/version/V1/view ; EIA FAQ https://www.eia.gov/tools/faqs/faq.php?id=567&t=1
- WorldCover: https://zenodo.org/records/7254221 ; s3://esa-worldcover/
- Tooling: PyPI JSON API; GitHub API (kylebarron/pydelatin, kylebarron/quantized-mesh-encoder, Geodan/pg2b3dm, CesiumGS/3d-tiles-tools, geo-data/cesium-terrain-builder, ahuarte47/cesium-terrain-builder); https://gitlab.com/py3dtiles/py3dtiles ; npm 3d-tiles-tools

## 11. Not confirmed
- Why the base interior lacks public LiDAR (restricted vs never collected for 3DEP).
- CA NAIP 2026 flight and its GSD; whether CA 2024 is on EarthExplorer; FSA's current terms page.
- CUSP tidal datum (MHW assumed).
- TNM API rate limits.
- OSM `power=*` counts in the bbox (Overpass unavailable).
- The licence terms of the HIFLD archive copies.
- The cesium-terrain-builder LICENSE file; the CHMv2 code licence; pyogrio cp312/cp313 arm64 wheels.
- Cesium World Terrain's datum realisation/epoch.
