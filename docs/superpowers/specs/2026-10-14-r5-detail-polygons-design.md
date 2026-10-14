# Detail polygons: large areas at preview, named areas at sim (REALISM R5) — design

2026-10-14. Narrows `REALISM.md` R5 ("Mission-aware generation") to what the user needs now. Builds on R0's
large-area engine (`scripts/scene/`, `docs/scene-packages.md`) and R1's imagery edge
(`2026-10-09-naip-edge-design.md`). Branch `realism-r5`, from `linux/verify-2026-10-13` (it carries the R3b build
fixes: GDAL cache cap, writable `HOME` in the build container).

## Goal

**Make operational-size areas buildable.** A package covers a box of up to ~200 km × 200 km (`REALISM.md` section 3,
"Large areas") at the `preview` profile, and a handful of named polygons inside it (ranges, bases, target areas,
together a few thousand km²) at `sim`. NAIP and 3DEP 1 m are fetched and built only for the polygons, which is what
keeps a 40,000 km² package within hours and tens of GB instead of ~48 GB of NAIP alone.

Decisions agreed with the user (2026-10-14):

- **Goal:** tiering for buildability. No new `high` level: the polygons get today's `sim`.
- **Input:** polygons only (GeoJSON). No CIGI-recording footprints, routes or waypoint files; a route can be drawn
  as a polygon.
- **Scale:** a ~200 km box with a few polygons.
- **Layers outside the polygons:** every per-area layer covers the whole box (land-cover burns of roads, paved
  areas, rail and water; building footprints burned as 230; the buildings tileset and mask; canopy; vegetation mix).
  Only roof textures are limited to the polygons, because they need NAIP.
- **Approach A** (of three): polygon-scoped sources, not two builds merged, not a per-tile profile.

## Non-goals

CIGI-recording or route corridors; a `high` profile (deeper zooms, LiDAR-derived terrain); cache warming for online
runs; several boxes per package; per-polygon layer settings (e.g. buildings only in one polygon); the sea-level
offset over a long coast (existing follow-up: one tide-station constant, +0.08 to +0.66 m off along Pendleton's
coast, growing with area). No runtime (CamSim C++) change.

## 1. Config and manifest

```toml
name = "socal"
bbox = [-118.51, 32.65, -116.35, 34.44]   # the whole area, built at `profile`
profile = "preview"
[detail]                                   # REALISM R5
file = "socal-detail.geojson"              # FeatureCollection of Polygon / MultiPolygon, EPSG:4326 lon/lat
profile = "sim"                            # the profile inside the polygons; only "sim" is accepted
```

- `[detail]` keys: `file` (required; relative to the toml's directory), `profile` (default and only value `"sim"`),
  `scoped` (optional, section 2).
  `[detail]` with `profile = "sim"` at the top level is a `ConfigError` (nothing to tier).
- **Validation** (`plan` fails with `ConfigError`, naming the feature): every feature has a unique `name` property
  matching `NAME_RE`; geometry is Polygon or MultiPolygon, valid (`shapely.is_valid`), non-empty, inside the bbox
  (`bbox.contains`), and does not cross the antimeridian; at least one feature. A `crs` member other than
  EPSG:4326 / CRS84 is an error.
- **Snapshot:** `plan` normalises the polygons (features sorted by name, coordinates rounded to 7 decimals,
  orientation normalised with `shapely.normalize`) and writes them into the manifest as a `detail` section:
  `{"profile": "sim", "polygons": [{"name", "geometry" (WKT)}], "sha256"}` (hash of the canonical JSON of the
  polygons). Builds and `verify` read the manifest, never the GeoJSON file again, as for every other source.
  `plan --replan` re-reads the file.
- **Priorities:** without `[detail]`, nothing changes: the planned manifest of every existing toml is byte-identical
  (gate 1). With `[detail]`, the default priorities are the detail profile's (`sim`) lists. They already end in the
  `preview` sources, which fill outside the polygons. `[priorities]` overrides still apply. Profile-derived switches (`ndvi`) also default from the detail profile (`sim`: on);
  NDVI covers the ring from Sentinel-2 and takes NAIP's values only where NAIP is (section 2; corrected after review).
- **Zooms:** the `bbox` region takes `profile`'s zooms (`preview`: terrain 14, imagery 13); the detail regions take
  the detail profile's (`sim`: terrain 16, imagery 17). `[zoom.detail]` overrides the latter, as `[zoom.bbox]` does
  the former.

## 2. Planning and source scope

**Regions.** `globe`, `ring`, `margin` and `bbox` are unchanged. Per polygon, two more, after them:

- `detail:<name>`: the polygon, at the detail zooms (terrain 16, imagery 17);
- `detail_margin:<name>`: the polygon buffered by `imagery_margin_km` (geodesic approximation as `ring_bounds`:
  degrees of latitude, longitude scaled by the polygon centroid's latitude), at the detail imagery zoom and the ring's
  terrain zoom, as today's `margin` region is for the bbox.

`Region` already holds a polygon and `plan_tiles` already takes the highest limit of the regions a tile overlaps, so
the tile expansion needs no change.

**Scoped sources.** For each tiled layer (terrain, imagery), a source in the detail profile's priorities but not in
the box profile's is *scoped*: today `dep3_1m` (terrain) and `naip_pc` (imagery). NOAA topobathy, `dep3_13`,
`wc_s2`, ETOPO and BMNG are in both and stay unscoped.

- **Scope polygon:** by layer, so plan-time settings are computed from the config alone: a scoped terrain source
  (`dep3_1m`) → the union of the `detail` polygons; a scoped imagery source (`naip_pc`) → the union of the
  `detail_margin` polygons (the same split as today's `bbox` / `margin` areas of these two sources). The manifest's
  `layers.terrain.scoped` / `layers.imagery.scoped` map each scoped source to its kind (present only with
  `[detail]`). `[detail.scoped]` (`terrain = [...]`, `imagery = [...]`) overrides the default rule for sources outside
  the built-in profiles (test sources, custom adapters); each listed id must be in that layer's priorities.
- **Discovery:** a scoped source's `discover` runs once per polygon (on its scope polygon's bounds) and the assets
  are merged by id, so one polygon in each corner of the box does not discover NAIP over the whole box.
- **Clipped footprints:** when `fetch` computes a scoped asset's footprint, it intersects it with the scope polygon
  and stores the clipped WKT in `a.metadata["footprint"]` (an asset whose clip is empty is dropped from the
  manifest). Before fetch, `coverage(use_bbox=True)` clips the discovery bboxes the same way.
- **Every consumer reads the clipped footprint:** `coverage` (tile expansion), the layer index (`LayerIndex`: a tile
  outside the scope never sees the asset), the feather (`feather_weight`), the NAIP-only interior (`ref_interior`),
  and the colour-balance fit (`reference_footprints`). Inside a tile, samples of a scoped asset outside its clipped
  footprint are no data: the sampler masks them with the footprint (rasterised on the sample grid), so a NAIP COG
  that overhangs the scope never leaks past it.
- **Unscoped per-area sources** keep their area kinds over the whole box: Overture (`bbox`), OSM and NHD
  (`margin`), CHMv2 (`bbox`), WorldCover (`ring`).

**NDVI** follows its sources' coverage (`_ndvi_area` intersects the ring with it): Sentinel-2 over the whole ring and
box, NAIP (fitted onto Sentinel-2) only inside its scope. A `preview` box with `[detail]` therefore gains a whole-box
NDVI layer, because `ndvi` defaults from the detail profile (corrected after the final review; set `ndvi = false` to
skip it). `layers.ndvi.scoped` carries the imagery scope so a changed scope re-keys NDVI tiles.

## 3. Layers

- **Terrain:** one pyramid. Leaves reach z16 from 3DEP 1 m inside each polygon and z14 from 1/3″ (or NOAA
  topobathy) elsewhere in the box, with the global base beyond the ring. At a polygon's edge the existing source
  feather (`FEATHER_M`, 30 m) blends 1 m into 1/3″ as it does at today's bbox edge; the feather band stays excluded
  from `verify`'s height comparison.
- **Imagery:** NAIP to z17 inside `detail_margin`, with R1's colour match and 200 m feather into Sentinel-2 at its
  clipped edge, and NAIP clipped 200 m off WorldCover water; Sentinel-2 to z13 elsewhere. The balance fit
  (`balance.json`) runs once per build over every NAIP / Sentinel-2 overlap of all polygons; its gridded offset is
  per cell, so polygons in different NAIP states keep local offsets.
- **NDVI:** where NAIP is (above).
- **Land cover, vector burns, building footprints (230), canopy, vegetation mix, buildings tileset and mask:** the
  whole box, unchanged code.
- **Roof atlases:** sampled from the imagery sources as today. Outside the NAIP scope only Sentinel-2 (10 m) is
  there, which fails the ≤ 2 m rule, so those roofs are flat grey (existing behaviour). The build log and the
  buildings stats report textured and flat roof counts.
- **`verify`:** checks the manifest's `detail.sha256` against its polygons, and that every scoped source's asset
  footprints lie inside its scope polygon. `--deep` is unchanged.
- **Runtime:** none. Cesium reads availability per tile (`layer.json`, `tilemapresource.xml`); mixed depths are what
  every package already has (bbox vs ring).
- **Docs:** `docs/scene-packages.md` gets a "Detail polygons (R5)" section (config, scope rule, what stays
  whole-box); `docs/configuration.md` is unaffected (no runtime key).

## 4. Gates

Test package `socal-r5`: `bbox = [-118.51, 32.65, -116.35, 34.44]` (200 km × 200 km around Camp Pendleton: Long
Beach to La Jolla, Orange County, northern San Diego, western Riverside; 0.19° north of centred, as built, so the OSM
margin stays inside the Geofabrik `socal` extract), `profile = "preview"`, one polygon
`pendleton` = the Pendleton bbox `[-117.62, 33.19, -117.24, 33.52]`; every other setting as
`pendleton-r5veg.toml` (features, buildings with 0.5 m roof atlases, canopy, the Pendleton vegetation mix, sea level
La Jolla, NAIP 2022, `allow = ["ODbL-1.0"]`, Overture release `2026-09-23.1`). Built in the reference container on
the Linux box from the same fetch cache as the Pendleton packages.

1. **No regression without `[detail]`:** `plan` of each Pendleton toml in the test fixtures gives a manifest
   byte-identical to the one planned before the change (pytest); the scene test suite passes.
2. **Identity inside the polygon:** z17 imagery leaves inside NAIP's interior (the polygon shrunk by the 200 m
   feather plus `FOOTPRINT_SLOP_M`) and z16 terrain leaves whose tile plus feather margin lies inside the polygon
   are byte-identical to `pendleton-r5veg`'s (`hashes.txt`), compared over the polygon shrunk by 1 km. Exception,
   listed in the report: imagery leaves with WorldCover water (code 80 or 220 in the package's land cover) within
   1 km, where NAIP is clipped off the water and the colour-matched Sentinel-2 shows; the balance fit's samples and
   grid differ between the two packages, so those pixels may differ. Terrain leaves have no exception.
   `tools/detail_check.py` runs gates 2 and 3.
3. **Scope:** no `naip_pc` or `dep3_1m` asset footprint outside its scope polygon; no imagery tile deeper than z13
   outside `detail_margin` and no terrain tile deeper than z14 outside `detail` (from the manifest and
   `hashes.txt`).
4. **Edge seam:** R1's imagery-edge gates at the polygon edge: the balance fit report's cell bias median ≤ 5 DN per
   band (R1's limit; Pendleton 2/2/2), `ndvi_check.py`'s seam and held-out gates, `water_check.py` (open water
   consistent with raw Sentinel-2), and `edge_shots.py` crops of the polygon edge for review (the tool takes the
   polygon's bounds instead of the hard-coded Pendleton bbox).
5. **Determinism and offline:** a rebuild is byte-identical; a killed build resumes without redoing finished tiles;
   `verify --deep` is clean; an egress-blocked run (`docker run --network none`, package mounted `:ro`) logs no
   request.
6. **Render:** an offline CamSim run on `socal-r5`: R1's Pendleton shots, a low oblique across the polygon edge
   inland and one on the coast, and a high oblique outside the polygon, for the user's visual review.
7. **Bench:** `run_bench.py --site pendleton` on `socal-r5` within noise of `pendleton-r5veg` (no runtime change).

**Reported, not gated:** package size, build time and peak RSS per layer, against a full-`sim` estimate for the box
from Pendleton's per-km² figures; building count; textured and flat roof counts; fetch cache growth.

## 5. Risks

- **Buildings at ~2–3 M** (~30 × Pendleton's 82,260) are the scale test this package adds. The layer runs in the
  main build process (Pendleton peaks at 1.0 GB on Linux with the GDAL cap) and its footprint raster and mask cover
  the whole box. If its time or memory scale badly, record it and open a follow-up; R5 does not redesign the
  buildings layer.
- **The Overture snapshot** for the box is a larger GeoParquet in the fetch cache (Pendleton's is small); fetch time
  and size are reported.
- **The balance fit** over a polygon's 3 km margin band works on Pendleton (R1); a small polygon gives few samples.
  The fit already reports its cell count; a polygon too small to fit falls back as R1 does where NAIP and
  Sentinel-2 share no land.

## Testing

pytest under `scripts/scene/tests` (no network): config validation (each error above), normalisation and hash
stability (feature order, ring orientation, coordinate noise below 1e-7), region list, scope rule by area kind,
footprint clipping and asset dropping, per-polygon discovery merge, coverage and plan with a synthetic polygon
(tiles deeper than the box zoom only inside it), the sampler's footprint mask (a source raster larger than its
scope yields no data outside it), the manifest identity of existing tomls (gate 1), and an end-to-end build of a
synthetic package with one polygon (as the existing synthetic-package tests).
