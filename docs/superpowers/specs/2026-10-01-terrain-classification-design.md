# Terrain classification for thermal IR (ROADMAP 4B) — design

Status: draft for review (2026-10-01). Builds on 4A (`2026-10-01-thermal-core-design.md`).
User decision: hybrid source — ESA WorldCover gives the regional class, the imagery's base
colour refines it per pixel.

## Goal

Terrain stops being one thermal class. Each terrain pixel gets a material from land cover, so:

- night terrain has thermal texture (forest canopy warmer than open grass and fields, built-up
  and roads warmer than vegetation, inland water warm);
- at noon vegetation reads cool (transpiration, low k_fast) and built-up / bare ground hot, so
  dark trees no longer read warm just because they are dark (4A's "EO negative" look);
- inland water (lakes, rivers, reservoirs above sea level) is water, not terrain.

Success: in the 4A acceptance views at San Francisco, (a) noon vegetation pixels are cooler than
built-up/bare pixels, (b) at night built-up pixels are warmer than open vegetation, (c) night
terrain has visible structure (spatial std well above 4A's single class), (d) class boundaries
follow features in the imagery (trees, lawns, roads), not 10 m blocks, (e) no hitch when the
land-cover window re-centres, (f) EO and `thermal.land_cover.enabled: false` unchanged.

## Non-goals

- Roads as a vector layer (OSM). Inside built-up and bare areas a dark/bright split of the base
  colour stands in for asphalt vs concrete; a road layer is a later option.
- Per-building or roof materials, photogrammetry-specific classes, seasonal phenology
  (WorldCover 2021 is one epoch), snow dynamics.
- Semantic class in ground truth (4D).

## Data pipeline

```
ESA WorldCover 2021 v200 (10 m, uint8 codes, 3°×3° COGs, open S3 bucket)
  └─ scripts/landcover/fetch_worldcover.py --bbox W S E N --out DIR     (offline, once)
       └─ DIR/index.json + DIR/<lat>_<lon>.png   (0.05° tiles, 8-bit grey = WorldCover code, lossless)
            └─ FLandCoverTileCache (load on a task thread, LRU)  ─┐
                                                                   ├─ FLandCoverWindow: resample to a
camera geodetic pose ──────────────────────────────────────────────┘   camera-centred East/North grid
                                                                        (2048², 10 m/texel = 20.48 km)
                                                                        → R8 texture + mapping params
```

- The script reads only the windows it needs from the COGs (HTTP range reads; `rasterio` via
  `uv run --with rasterio`), writes 0.05° tiles at native 10 m (~555×440 px, a few hundred KB
  each as PNG) and an `index.json` (tile list, bbox, source version, licence string
  "© ESA WorldCover project 2021 / Contains modified Copernicus Sentinel data (2021)", CC BY 4.0).
- CamSim reads only local files (`thermal.land_cover.dir`, default
  `Content/NonUFS/LandCover`, staged as loose files like the geoid grid). No network at runtime.
- A San Francisco sample (bbox −122.56, 37.69, −122.35, 37.84) is committed via git LFS for
  tests and acceptance.

## Runtime

**Window.** `FLandCoverWindow` keeps a 2048×2048 R8 texture centred near the camera nadir on a
local East/North grid (10 m texels). When the nadir moves more than 25 % of the window from its
centre, a new window is built on a task thread (tile loads + resample, target < 100 ms) and
swapped in atomically on the game thread; the old one keeps rendering meanwhile (no hitch). The
resample is a small-area mapping: lat = lat₀ + N/M(φ₀), lon = lon₀ + E/(N(φ₀) cos φ₀), nearest
source texel. Missing tiles → code 0 ("no data").

**Lookup in ThermalCS.** The pass already reconstructs a camera-relative world position. Two new
parameters, the East and North unit vectors in UE world at the window centre and the camera's
offset from the window centre (metres, computed in doubles on the CPU), give
`(E, N) = (dot(P, East), dot(P, North)) / 100 + CamOffset`, then a texel. Only terrain pixels
(after sky / entity / water-band classification) look it up.

**Soft boundaries.** The four nearest texels are read; each maps through a 256-entry table
(WorldCover code → material index) and the pixel's temperature, emissivity and k_fast are the
bilinear blend of the four materials' values (not of the codes). 10 m boundaries become smooth
ramps instead of blocks.

**Imagery refinement (per pixel).** From the GBuffer base colour (linear, via its SRV as in 4A):
- vegetation index `v = saturate((ExG − v₀) / (v₁ − v₀))`, `ExG = (2G − R − B) / (R + G + B + ε)`;
- in a non-vegetation region (built-up, bare): blend toward `vegetation` by v (street trees,
  lawns, parks);
- in a vegetation region (tree, shrub, grass, crop): blend toward `bare_soil` by (1 − v) (trails,
  clearings, dirt);
- in built-up and bare, the non-vegetation part splits by base luminance into `asphalt`
  (dark, < `asphalt_max_luma`) and `concrete` (bright), with a soft ramp.
Thresholds are config keys with defaults tuned on the SF sample. Without base colour (4A
fallback) the refinement is off and the WorldCover blend alone is used.

**Classes.** WorldCover codes map to thermal materials (config `thermal.land_cover.classes`,
code → material name; defaults below). New built-in materials join 4A's (`terrain_default`,
`water`, `vehicle_paint`, `asphalt`, `vegetation`, `concrete`):

| WorldCover | Code | Material (default) | Notes |
|---|---|---|---|
| Tree cover | 10 | `tree_canopy` | low k_fast, high h (transpiration), canopy keeps warmer at night |
| Shrubland | 20 | `shrubland` | |
| Grassland | 30 | `grassland` | low inertia, strong night cooling |
| Cropland | 40 | `cropland` | |
| Built-up | 50 | `built_up` → asphalt / concrete split | |
| Bare / sparse | 60 | `bare_soil` | high k_fast, hot at noon |
| Snow and ice | 70 | `snow_ice` | high emissivity, fixed ≤ 0 °C |
| Permanent water | 80 | `water` | inland water gets 4A's water class |
| Herbaceous wetland | 90 | `wetland` | high inertia (wet) |
| Mangroves | 95 | `wetland` | |
| Moss and lichen | 100 | `grassland` | |
| No data | 0 | `terrain_default` | 4A behaviour |

Material values (emissivity, albedo for the class model's forcing, thermal inertia, convection h,
k_fast) are literature-typical, tuned so the SF diurnal contrasts are plausible; they stay
overridable in `thermal.materials`. The material table grows past 4A's 6 built-ins but stays
within `MaxClasses = 32`.

## Interfaces

- `FThermalFrameParams` gains: `uint8 LandCoverClass[256]` (code → material index),
  land-cover mapping (`East`, `North` float3, `CamOffsetM` float2, `TexelM`, `WindowTexels`,
  `bLandCover`), refinement thresholds, and the material indices for `vegetation`, `bare_soil`,
  `asphalt`, `concrete`.
- `FThermalPassInputs` gains `FRDGTextureRef LandCover` (R8; null → off, black dummy bound).
- `CamSimThermalRef::EvaluatePixel` takes the land-cover sample (4 codes + bilinear weights) as
  input so the CPU reference stays a pure function and the shader stays its mirror.
- New: `Thermal/LandCoverTiles.{h,cpp}` (tile index + PNG decode + LRU),
  `Thermal/LandCoverWindow.{h,cpp}` (async window build, double-buffered, RHI texture),
  `scripts/landcover/fetch_worldcover.py` (+ pytest for the tiling maths), config
  `thermal.land_cover.{enabled, dir, classes, window_texels, recentre_fraction, veg_index_lo,
  veg_index_hi, asphalt_max_luma}`.

## Edge cases

- No tiles for the area / missing directory: land cover off, one warning, 4A behaviour.
- Camera over open ocean: window is all "no data" or water; sea-band rule still wins first.
- Very high altitude (window smaller than the footprint): pixels outside the window use
  `terrain_default`; window size is configurable.
- Antimeridian / poles: the small-area mapping is per window; tiles are fetched by their own
  lat/lon; windows within 1° of a pole fall back to `terrain_default`.
- Window swap mid-flight: the render thread uses a ref-counted window (texture + mapping params)
  captured with that frame's thermal params, so a frame never pairs one window's texture with
  another's mapping.

## Performance

ThermalCS: + 4 R8 texel reads and a small blend for terrain pixels (budget + 0.1 ms at 1080p).
Window build on a task thread, < 100 ms, never on the game or render thread. Memory: 4 MB per
window ×2 (double buffer) + tile LRU (default 64 tiles).

## Testing

- `CamSim.Thermal.LandCover.*`: tile index parsing, PNG decode, LRU; window resample against an
  analytic pattern (orientation, scale, centre); re-centre trigger; missing tiles → 0; antimeridian.
- `CamSim.Thermal.Reference.*` additions: bilinear material blend; vegetation-index refinement
  both directions; asphalt/concrete split; no-base-colour fallback; land cover off = 4A output.
- `CamSim.GPU.Thermal.MatchesCpu` additions: land-cover cases (synthetic window, rotated East/North).
- `scripts/tests/test_fetch_worldcover.py`: tile naming/bbox maths, code passthrough.
- `thermal_check.py` additions (live, SF sample): (i) noon: IR mean of vegetation pixels (masked
  by greenness in the EO frame of the same pose) < IR mean of non-vegetation ground by ≥ 3 DN,
  both bands; (j) night: built-up/bare (non-green) > vegetation by ≥ 2 DN; (k) night terrain
  spatial std (nadir view, entities masked) ≥ 3× 4A's; (l) window re-centre during a 1 km/min pan
  causes no frame-time spike > 5 ms over baseline; existing gates (a)–(h) still pass.

## Risks

- **Greenness on dry Californian grass** (brown in summer imagery) reads as non-vegetation; the
  WorldCover prior (grassland) keeps it a vegetation-family class, and the refinement only blends
  toward `bare_soil` — acceptable, tune `veg_index_lo`.
- **Imagery baked shadows** lower base colour luminance; the asphalt/concrete split may call
  shadowed concrete asphalt. The ramp is soft; acceptable.
- **WorldCover licence** (CC BY 4.0) requires attribution: the index carries it and
  `docs/thermal.md` and the README credit it.
