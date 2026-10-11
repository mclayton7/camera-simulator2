# Vector features into the land-cover window (REALISM R2) — design

2026-10-10. Scene tooling (`scripts/scene`) plus a small runtime change (class defaults, one new thermal material, CIGI
material codes). No shader change, no editor work.

## Goal

Roads are among the most visible features in LWIR and are 4B's main gap ("roads layer is a non-goal"). R2 burns OSM roads,
rail and paved areas, and NHD water bodies, into the scene package's land-cover tiles as new codes. Thermal IR then draws
them, and CIGI HAT/HOT and LOS extended responses report the code under the hit as their material code. Nothing new is
drawn in EO: the codes only change what `ThermalCS` and the query handler read.

Scope (agreed 2026-10-10): REALISM R2's rasterisation, classes and material codes. The NAIP NDVI evaluation in `ThermalCS`
(R2's fourth bullet) is a separate follow-up: it needs its own runtime window on the NDVI layer's TMS grid and is a
measured VRAM/ThermalCS-time trade, unlike everything here.

Success, on the Camp Pendleton `sim` package:

- Roads, rail, paved areas and perennial water are in `landcover/` as codes 200–220. Rebuilds are byte-identical, and with
  the features off the package is byte for byte today's.
- OSM road centrelines sit within 10 m (median) of the roads in NAIP.
- At noon in MWIR and LWIR, I-5 is warmer than its verge, and the gap is large enough to see. It is still warmer at 02:00.
- HAT/HOT and LOS extended responses carry the land-cover code at the hit point.
- `ThermalCS` time and `thermal.land_cover.enabled: false` (4A bit for bit) are unchanged.

## Findings this rests on

- **The format already fits.** Land-cover tiles are 600×600 uint8 PNGs of raw codes (`layers/landcover.py`,
  `camsim-landcover-1`; 0.05° tiles, 12,000 cells per degree = 9.28 m N-S, 7.77 m E-W at 33° N). The runtime window is
  `PF_R8_UINT`, and the 256-entry code→class and code→family tables are packed for the shader (`CamSimThermalCommon.ush`).
  Any unused code works end to end. The runtime checks only `format`, `tile_deg` and `tile_px` in `index.json`.
- **Refinement does the sharpening.** The `Bare` family (`ThermalReference.cpp`, `RefinedClassData`) blends the texel's own
  class towards `vegetation` by the GBuffer greenness `V` at each pixel. A road texel in the `Bare` family is asphalt where
  the imagery is grey and vegetation where it is green, so a green verge separates from the road at imagery resolution,
  not at 10 m. Brown, dry verges (most of Pendleton's year) stay asphalt inside the road texel: see "Known limits".
- **Asphalt and concrete exist as materials** (4A built-ins 3 and 5, used by the `BuiltUp` family). A road is asphalt.
  REALISM's `road_asphalt`/`road_concrete` would duplicate them, so the road codes map to the existing materials and
  users override them with `thermal.land_cover.classes.<code>`. Only `rail_ballast` is new (porous crushed rock: low
  thermal inertia, unlike any built-in). That leaves 17 free class slots.
- **No sub-texel width.** A 7 m road in a 9 m texel is one texel wide. Making roads sharper than that is REALISM's named
  follow-up (a finer road raster), not R2.
- **Sources** (`docs/realism/data-sources.md`):
  - **OSM** comes from Geofabrik extracts (ODbL 1.0, daily builds plus dated snapshots; `socal` is 639 MB).
    `licences.toml` lists ODbL-1.0, but it isn't in `DEFAULT_ALLOW`.
  - **3DHP** has only a CONUS-wide annual file and an ArcGIS service whose bbox queries time out. At Pendleton it is
    cross-walked NHD anyway.
  - **NHD HR** is retired, so it is static and reproducible. TNM still serves it per HU4 as GeoPackage (1807 covers
    Pendleton).
  - The fetch cache is content-addressed (`cache.py`, sha256 per asset in the manifest), so a downloaded extract *is* the
    snapshot.
- **No existing material path.** HAT/HOT sends material 0 (`CigiQueryHandler.cpp:133`), and LOS has `Material = 0`
  (`CigiPacketTypes.h:264`). `FLandCoverWindow` updates only while thermal IR runs, so it can't serve EO-mode queries.
  `docs/terrain-feedback.md` still says extended requests are downgraded (stale since ROADMAP 1.9, 2026-09-26).

## Design

### 1. Codes

| Code | Feature | Default class | Family |
|---|---|---|---|
| 200 | paved, asphalt or unknown surface (roads, parking, aprons) | `asphalt` | Bare |
| 201 | paved, concrete surface | `concrete` | Bare |
| 210 | rail bed | `rail_ballast` (new) | Bare |
| 220 | inland water (perennial lakes, reservoirs, river areas) | `water` | None |

Codes 200–239 are reserved for scene-package vector features. WorldCover stays 10–100 and 0 is still no data. The
code table is documented in `docs/scene-packages.md` and `docs/terrain-feedback.md`, and written into `index.json` as
`"codes": {"200": "paved_asphalt", ...}`. The runtime ignores that key, so the format string stays `camsim-landcover-1`
and older CamSim builds read R2 packages (unknown codes become `terrain_default`).

### 2. Sources (scene tooling)

**`osm_geofabrik`** (layer `landcover`, licence `ODbL-1.0`, attribution "© OpenStreetMap contributors"):

- **Picking the extract.** Discovery reads Geofabrik's `index-v1.json`, which has the polygon of every extract. It picks the
  smallest extract whose polygon contains the package bbox. The config can force one with `[sources.osm_geofabrik] extract = "socal"`.
- **Snapshot.** The asset URL is the dated file (`<id>-<yymmdd>.osm.pbf`) for the date discovery ran, or for
  `[sources.osm_geofabrik] date` when set. The cache stores it by sha256 and the manifest records URL and hash, so a rebuild
  uses exactly that file even after Geofabrik deletes it. A rebuild with an empty cache and a vanished URL fails loudly.
  That is R0's snapshot rule.
- **Reading.** `pyosmium` (BSD-2; wheels for macOS arm64 and Linux, Python 3.12–3.13) streams the PBF with node locations
  and a bbox prefilter, plus a margin of half the widest feature. It keeps only the tags below. The extracted features are
  cached as a derived file keyed by (PBF sha256, filter version), so a rebuild after a settings change doesn't re-read
  600 MB.

**`nhd_hr`** (layer `landcover`, licence `LicenseRef-PublicDomain-USGov`, attribution "USGS National Hydrography Dataset"):

- Discovery uses `tnm_products` with the NHD Best Resolution dataset, GeoPackage format, for every HU4 that touches the bbox.
  The GPKG is read with the standard library's `sqlite3`, and shapely parses the WKB after the GeoPackage binary header is
  stripped. That needs no GDAL and no new dependency.
- Kept: `NHDWaterbody` FType 390 (LakePond) and 436 (Reservoir), and `NHDArea` FType 460 (StreamRiver), each only with a
  perennial FCode (39004, 39009, 39010, 39011, 39012, 43612–43624 as applicable, 46006). The plan's first task confirms the
  list against the FCode table.
- Dropped:
  - intermittent and ephemeral features (dry most of the year in Southern California);
  - SeaOcean, Estuary, Foreshore (the ocean is WorldCover 80 plus the ocean mesh);
  - all flowlines: they have no width, and a 2 m creek burned as a 9 m water stripe is worse than nothing.

3DHP replaces `nhd_hr` when USGS publishes per-unit downloads. The adapter interface is the same, and the change is noted
in REALISM. OSM water as a gap filler is left out (YAGNI until a package shows a gap).

### 3. Features to geometry

Every feature becomes a polygon in a local metric frame per land-cover tile (equirectangular about the tile centre, error
< 0.1 % over 5.5 km). Linear features are buffered by width / 2 with flat caps.

**Roads** (`highway=*`):

- Kept: `motorway`, `trunk`, `primary`, `secondary`, `tertiary` (each with its `_link`), `unclassified`, `residential`.
  The list is configurable; `service` and `track` are out by default, since at 3–4 m they would be inflated 2–3×.
- Width = `width` tag if present, else `lanes` × 3.5 m + shoulders (3.0 m for motorway/trunk, 1.0 m otherwise).
- Default `lanes`: 2 for motorway and trunk (OSM draws dual carriageways as two ways), otherwise 2 for two-way ways and
  1 for one-way ways.

**Paved areas:**

- `area:highway=*`, `highway=*` with `area=yes`, `amenity=parking` (not `parking=underground|multi-storey`), and
  `aeroway=apron`.
- `aeroway=runway|taxiway` ways are buffered by `width`, else 45 m or 23 m.

**Rail:** `railway=rail|light_rail`, 4 m per track (`tracks` tag, default 1).

**Skipped everywhere:** `bridge=*` (not `no`), `tunnel=*` (not `no`), `covered=yes`, `layer<0`, and `location=underground`.
R2 has no deck geometry (that is R3), so a bridge burned onto the river below would turn the water into asphalt.

**Surface → code:**

| Surface tag | Result |
|---|---|
| `concrete`, `concrete:plates`, `concrete:lanes` | 201 |
| `unpaved`, `gravel`, `dirt`, `ground`, `compacted`, `fine_gravel`, `sand`, `grass`, `earth`, `mud` | skipped (WorldCover stays) |
| any other surface, or none | 200 |

Runways with no surface tag are 201 (US military and large runways are mostly concrete). This is the one class-level
default; it can be overridden.

**Water:** NHD polygons as-is.

### 4. Burning

`layers/features.py` runs after the WorldCover cut, in the same isolated process. It loads each tile's codes, burns the
feature polygons in priority order (later wins), and rewrites the PNG:

1. water (220)
2. paved areas
3. rail (210)
4. roads (200/201)

- **Raster rule:** `rasterio.features.rasterize` with the centre-sample rule (not `all_touched`), on line features whose width
  is first raised to at least 1.05 × the cell's longer side (9.74 m; polygons are burned as they are). A strip that wide covers at least one cell centre in every
  cross-section at any orientation, so roads never break into dashes. The centre rule keeps them about one texel wide.
  `all_touched` would make them two.
- **No-data texels:** 0 is burned like any other texel.
- **Determinism:** features are sorted by (priority, OSM/NHD id) before burning. Shapely and rasterio versions are pinned
  by `uv.lock`, so output is byte-identical.
- **Inputs hash:** the burn's settings (classes kept, widths, surface table, filter version) go into
  `m.layer_settings_hash("landcover")` together with the feature sources' asset hashes. A change rebuilds only `landcover/`.

### 5. Config

```toml
[features]             # REALISM R2; all off by default (the package is then byte for byte R1's)
roads = true           # OSM (sources.osm_geofabrik): needs allow = ["ODbL-1.0"]
paved_areas = true     # OSM
rail = true            # OSM
water = true           # NHD HR (sources.nhd_hr), public domain
road_classes = ["motorway", "trunk", "primary", "secondary", "tertiary", "unclassified", "residential"]
```

- **Licence:** any OSM feature without `ODbL-1.0` in `allow` fails at `plan`, naming the key to add. `examples/pendleton.toml`
  turns everything on and allows ODbL.
- **Plumbing:** the feature sources form a fourth source layer, `features`. Their priorities are derived from the flags,
  not set by hand. Discovery and the burn use the imagery margin area (bbox + `imagery_margin_km`, where NAIP is), not the
  100 km land-cover ring: outside the margin, WorldCover is untouched. The settings go into the manifest as
  `layers.landcover.features`, so with the features off the manifest and `landcover/` are unchanged.
- **Licence register:** the existing per-source checks cover it. `check_allowed` already rejects ODbL without `allow`,
  and `attribution` already lists every source. The land-cover `index.json` and `ATTRIBUTION.txt` gain the OSM and NHD
  credits. A raster burned from OSM is treated as a derived database, so the notice says the layer is ODbL.

### 6. Runtime

**Classes** (`LandCoverClasses.cpp`): add the defaults from section 1 (codes 200, 201, 210 → `Bare`; 220 → `None`).
`classes.<code>` overrides work as they do today. Remapping to a different material drops the family, as it does for the
WorldCover codes.

**Material** (`ThermalMaterials.cpp`): `rail_ballast` is appended as built-in 14. Index order is fixed, so existing indices
don't move. Proposed values: albedo 0.20, emissivity 0.93, thermal inertia 900, h_c 10, k_fast 0.025. These are dark,
porous, dry crushed rock: hotter than bare soil by day, cooler at night. The plan's thermal task checks them against
published ballast surface-temperature data and sets them before the gate run.

**CIGI material codes** (`Thermal/LandCoverLookup.{h,cpp}`, new):

- **`FLandCoverCodeLookup`:** game thread only, with its own `FLandCoverTileCache` (16 tiles), independent of the thermal
  window. It works in EO and IR and anywhere on the globe. `Code(LatDeg, LonDeg)` returns the code of the cell containing
  the point, or 0. A cache miss decodes one 600² PNG on the game thread (~1–2 ms, measured in the plan). Queries cluster
  near the platform, so misses are rare. The subsystem owns the lookup and configures it from `thermal.land_cover.dir` when
  `thermal.land_cover.enabled`; otherwise every query returns 0.
- **Rule:** one pure function, `CamSimLandCover::QueryMaterialCode(bValid, bWater, bEntityHit, Lat, Lon, Lookup)`, so the
  rule is unit-tested without a world:
  - invalid → 0;
  - a sea-surface result (HAT/HOT `R.bWater`) → 80 (WorldCover water);
  - an entity hit (any `ACamSimEntity`, CIGI or DIS) → 0, since an entity has no land-cover code (the LOS response already
    carries the entity ID);
  - otherwise → `Code(Lat, Lon)` at the hit point.

  HAT/HOT extended and LOS extended both use it.
- **Docs:** `docs/terrain-feedback.md` gets the code table (WorldCover 10–100, R2 200–220, 0 = unknown) and its stale
  "extended is downgraded" text is fixed. ROADMAP 1.9's "material code is 0" note is closed.

### 7. Verify

- `verify --deep` checks that `index.json`'s `codes` legend covers every code the tiles contain (WorldCover codes and 0
  are implicit).
- The existing hash and attribution checks cover the rest.

## Testing

**pytest** (`scripts/scene/tests`, no network; small fixtures: a hand-made `.osm.pbf` written with pyosmium's writer, a
tiny GPKG):

- Width rules: lanes, defaults, `width` tag, oneway.
- Surface table: concrete, unpaved skip, runway default.
- Bridge, tunnel and underground skipped.
- Minimum width: a 3 m line at 0°, 30°, 45°, 60° and 90° burns a continuous one-texel strip with no gaps; a 30 m road
  burns about 3 texels.
- Burn order: a road over water wins, and a bridged road over water doesn't burn.
- NHD filter: perennial lake kept, intermittent dropped, SeaOcean dropped.
- Extract selection from a fixture `index-v1.json`: smallest containing, and forced `extract`.
- Licence failure without ODbL.
- Features-off package byte-identical to the pre-R2 build; two builds byte-identical; settings change rebuilds only
  `landcover/`.
- `verify` catches a code missing from the legend.

**UE automation** (`CamSim.Thermal.LandCover.*`, `CamSim.CIGI.*`, NullRHI):

- Default classes and families for 200/201/210/220.
- `rail_ballast` is built-in 14 and the materials count is 15 (existing count tests are updated).
- `FLandCoverCodeLookup` against a temp-dir fixture tile (cell edges, the antimeridian, a missing tile → 0, LRU eviction).
- The query handler fills the material for terrain, sea surface and no hit (extending the existing HAT/HOT/LOS tests).

**GPU:** no shader change. `CamSim.GPU.Thermal.*` must still pass unchanged (run as a regression check).

## Acceptance (Pendleton `sim`, macOS/Metal; Linux joins the FOR_LINUX list)

1. **Package.** Build with features on, then:
   - report the burned texel counts per code and the build time;
   - rebuild and confirm it is byte-identical;
   - build with features off and confirm it matches the R1 package's `landcover/` hashes;
   - `verify --deep` passes.
2. **Road registration** (`scripts/scene/tools/road_registration.py`, new):
   - Sample 200 points on motorway…tertiary centrelines, at least 50 m from junctions and bridges, seeded.
   - At each point, read a ±30 m NAIP luminance profile perpendicular to the road and find the offset of the best match to
     a dark box of the road's width.
   - Gate: median |offset| ≤ 10 m. Report p90 and the share of profiles with no clear road (bright concrete, shade),
     which are excluded and counted.
3. **Thermal gate r (road vs verge)** in `scripts/thermal_check.py`, new run `roads` (`--scene <pkg>`):
   - Pose: nadir over a straight I-5 segment with the heading along the road, so the road is a vertical strip through the
     image centre. The pose is computed once from OSM and hard-coded with its source way id.
   - Metric: mean Y (display DN, white-hot, like gates i and j) of the carriageway minus mean Y of a verge strip on its
     outer side (away from the other carriageway). Both are temporal means over 10 frames in the central 60 % of the
     frame.
   - Rows: MWIR and LWIR at 12:00 and 02:00 (`thermal_check`'s December times).
   - Thresholds: noon ≥ +6 DN; 02:00 ≥ +2 DN (asphalt's higher inertia keeps it warmer than grass overnight). Gates
     i and j use +3 and +2 DN for a weaker contrast.
   - The closed-form temperatures (asphalt vs grassland, from the `camsim.Thermal.Log 1` class lines) are reported beside
     each row as info, and checked against published asphalt-vs-grass contrasts (about +10–20 K at summer noon, less in
     December). If they disagree, it is the model to look at, not the gate.
4. **Unchanged:**
   - `thermal_check.py` gates a–q pass;
   - the bench ThermalCS time is within noise of R1's;
   - `thermal.land_cover.enabled: false` is 4A bit for bit (the existing test).
5. **CIGI.** `send_cigi_test.py` HAT/HOT extended queries return:
   - 200 on I-5;
   - 220 or 80 on Lake O'Neill (WorldCover may already call it water; either is right);
   - 80 offshore;
   - a WorldCover code in open country.

   An LOS extended query from the sensor down to I-5 returns 200.
6. **Visual review (human)** of MWIR/LWIR frames along I-5, a residential grid (Oceanside) and the runway at MCAS Camp
   Pendleton:
   - do roads read as continuous lines;
   - is the 6 m ground warp's wobble (`warp_amplitude_m`) acceptable on road edges;
   - is the brown-verge widening acceptable?

   If the wobble isn't acceptable, the follow-up is a per-family warp switch or the finer road raster (both out of R2).

## Known limits (go into `docs/scene-packages.md` and `docs/thermal.md`)

- Roads are at least one texel (~9 m) wide in IR wherever the verge isn't green. A dry verge inside a road texel is drawn
  as asphalt.
- The land-cover warp moves road edges by up to `warp_amplitude_m` (6 m), so long straight roads may wiggle slightly in IR.
- Bridges and tunnels are not burned. A road on a bridge is drawn as whatever lies below the bridge until R3 adds decks.
- Unpaved roads and tracks are left to WorldCover.
- OSM completeness varies, and OSM inside military bases can be sparse. The registration report shows it.
- Intermittent water is never water, even in a wet season.

## Out of scope

- NAIP NDVI in `ThermalCS` (the R2 follow-up spec).
- A finer road raster.
- OSM water as a gap filler.
- 3DHP (swap in when per-unit downloads exist).
- Roof classes (R3).
- Semantic class in ground truth (4D).
- Any EO change.
