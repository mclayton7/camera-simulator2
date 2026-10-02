# Terrain classification for thermal IR (ROADMAP 4B) — design

Status: implemented (2026-10-01); this text is aligned with the as-built design (rulings S1, S9-S12; see ROADMAP 4B). Builds on 4A (`2026-10-01-thermal-core-design.md`).
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
terrain has visible structure (band-passed spatial std well above 4A's single class), (d) class boundaries
follow features in the imagery (trees, lawns, roads), not 10 m blocks, (e) no hitch when the
land-cover window re-centres, (f) EO and `thermal.land_cover.enabled: false` unchanged.

## Non-goals

- Roads as a vector layer (OSM). Inside built-up areas a dark/bright split of the base
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
                                                                        → R8_UINT texture + mapping params
```

- The script reads only the windows it needs from the COGs (HTTP range reads; `rasterio` via
  `uv run --with rasterio`), writes 0.05° tiles at native 10 m (a WorldCover cell is 1/12000°, so every tile is exactly
  600×600 cells, a few hundred KB as PNG) and an `index.json` (tile list, bbox, source version,
  licence string "© ESA WorldCover project 2021 / Contains modified Copernicus Sentinel data (2021)
  processed by ESA WorldCover consortium", CC BY 4.0) plus `ATTRIBUTION.txt`. A COG that does not
  exist (open ocean) is listed as missing and reads as no data.
- CamSim reads only local files (`thermal.land_cover.dir`, default
  `Content/NonUFS/LandCover`, staged as loose files like the geoid grid). No network at runtime.
- A San Francisco sample (bbox −122.56, 37.69, −122.35, 37.84) is committed via git LFS for
  tests and acceptance.

## Runtime

**Window.** `FLandCoverWindow` keeps a 2048×2048 `R8_UINT` texture centred near the camera nadir on a
local East/North grid (10 m texels). When the nadir moves more than 25 % of the window from its
centre, a new window is built on a task thread (tile loads + resample, target < 100 ms) and
swapped in atomically on the game thread; the old one keeps rendering meanwhile (no hitch). The
resample is a small-area mapping: lat = lat₀ + N/M(φ₀), lon = lon₀ + E/(N(φ₀) cos φ₀), nearest
source texel. Missing tiles → code 0 ("no data").

**Lookup in ThermalCS.** The pass already reconstructs a camera-relative world position. Two new
parameters, the East and North unit vectors in UE world at the window centre and the camera's
offset from the window centre (metres, computed in doubles on the CPU), give
`(E, N) = (dot(P, East), dot(P, North)) / 100 + CamOffset`, then a texel. East and North are
recomputed from the Cesium georeference every frame (an origin shift rotates the UE axes; ruling
S1), and checked for orthonormality and handedness. Only terrain pixels (after sky / entity /
water-band classification) look it up. The half-texel border of the window, which lacks a full
four-texel neighbourhood, and everything outside it use `terrain_default`.

**Soft boundaries.** The four nearest texels are read; each maps through a 256-entry table
(WorldCover code → material index) and the pixel's temperature, emissivity, k_fast and S_abs,ref
are the blend of the four materials' values (not of the codes). The blend fractions are
smoothstep, S(f) = f²(3 − 2f), so the blend is C1 across texel centres (plain bilinear left
Mach-band creases that the AGC amplified into visible 10 m squares; ruling S8). 10 m boundaries
become smooth ramps instead of blocks.

**Geo-anchored domain warp** (ruling S8; `warp_amplitude_m` 6, `warp_cell_m` 20). The lookup
position is moved by up to 6 m East and North by two smooth value-noise fields (integer PCG hash,
own stream) fixed to the ground: evaluated in a frame anchored at the session's first window, with
a per-axis anchor scale so the same ground point gets the same offset from any window. Class edges
become wavy instead of axis-aligned staircases and do not swim when the camera pans.

**Imagery refinement (per pixel).** From the GBuffer base colour (linear, via its SRV as in 4A).
The refinement is gated on the base colour being bound, not on sunlight, so it also runs at night
(ruling S1):
- vegetation index `v = saturate((ExG − v₀) / (v₁ − v₀))`, `ExG = (2G − R − B) / (R + G + B + ε)`,
  where ExG is taken from the mean of 5 base-colour samples (the pixel and 4 diagonal taps
  `veg_blur_m` = 2 m away, 1 to 32 px) rather than the pixel alone (ruling S9: the ~16 m chroma
  blocks of JPEG imagery otherwise drive ExG into square patches); the default is 2 m because
  4 to 8 m ghosted roads with only 5 taps (ruling S10);
- vegetation family (tree, shrub, grass, crop, wetland, mangrove, moss): blend toward `bare_soil`
  by (1 − v) (trails, clearings, dirt);
- built-up: blend toward `vegetation` by v (street trees, lawns, parks); the non-vegetation part
  splits by the pixel's own base luminance into `asphalt` (dark, < `asphalt_max_luma`) and
  `concrete` (bright), with a soft ramp. The split is in built-up only: in bare ground it would
  turn bare soil into asphalt (ruling S1), so bare blends toward `vegetation` by v and otherwise
  keeps its own class;
- snow, water, no data: own class.
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
| Snow and ice | 70 | `snow_ice` | high emissivity; model temperature capped at 273.15 K (the `snow` source) |
| Permanent water | 80 | `water` | inland water gets 4A's water class |
| Herbaceous wetland | 90 | `wetland` | high inertia (wet) |
| Mangroves | 95 | `wetland` | |
| Moss and lichen | 100 | `grassland` | |
| No data | 0 | `terrain_default` | 4A behaviour |

Material values (emissivity, albedo for the class model's forcing, thermal inertia, convection h,
k_fast) are literature-typical, tuned so the SF diurnal contrasts are plausible; they stay
overridable in `thermal.materials`. Vegetation cooling is an *effective* albedo (0.40–0.50): the
closed-form model has no latent-heat term, so evapotranspiration is folded into the absorbed
solar (ruling S1); the 4A `vegetation` class was retuned the same way (ruling S4). The orderings
the spec's goal needs are pinned by `DiurnalContrastsAtSanFrancisco` (ruling S3): at noon every
vegetation class ≥ 1 K below `built_up` and `bare_soil`; at night `built_up` and `bare_soil` ≥ 1 K
above `tree_canopy`, which is ≥ 0.5 K above `grassland`. The material table grows past 4A's 6 built-ins but stays
within `MaxClasses = 32`.

## Interfaces

- `FThermalFrameParams` gains: `uint8 LandCoverClass[256]` (code → material index),
  land-cover mapping (`East`, `North` float3, `CamOffsetM` float2, `TexelM`, `WindowTexels`,
  `bLandCover`), refinement thresholds, and the material indices for `vegetation`, `bare_soil`,
  `asphalt`, `concrete`.
- `FThermalPassInputs` gains `FRDGTextureRef LandCover` (`PF_R8_UINT`; null → off, the zero-uint dummy
  `GSystemTextures.GetZeroUIntDummy` bound).
- `CamSimThermalRef::EvaluatePixel` takes the land-cover sample (4 codes + smoothstep weights) as
  input so the CPU reference stays a pure function and the shader stays its mirror.
- New: `Thermal/LandCoverTiles.{h,cpp}` (tile index + PNG decode + LRU),
  `Thermal/LandCoverWindow.{h,cpp}` (async window build: the current window renders while at most one build is in flight,
  then swaps in; RHI texture; the CPU copy of the codes is kept),
  `scripts/landcover/fetch_worldcover.py` (+ pytest for the tiling maths), config
  `thermal.land_cover.{enabled, dir, classes, window_texels, recentre_fraction, veg_index_lo,
  veg_index_hi, asphalt_max_luma, warp_amplitude_m, warp_cell_m, veg_blur_m}`
  (`window_texels` is even, `[256, 8192]`; ruling S5).
- `CamSimThermalRef` takes the window codes and the per-pixel sample; sampling (`SampleLandCover`)
  and blending (`BlendLandCover`) are separate testable functions, and the vegetation taps are
  gathered by `Run` into the pixel sample so `EvaluatePixel` stays pure (ruling S1, S9).

## Edge cases

- No tiles for the area / missing directory: land cover off, one warning, 4A behaviour.
- Camera over open ocean: window is all "no data" or water; sea-band rule still wins first.
- Very high altitude (window smaller than the footprint): pixels outside the window use
  `terrain_default`; window size is configurable.
- Open-ocean WorldCover COGs do not exist (404): treated as no data (ruling S1).
- Antimeridian / poles: the small-area mapping is per window; tiles are fetched by their own
  lat/lon; windows within 1° of a pole fall back to `terrain_default`.
- Window swap mid-flight: the render thread uses a ref-counted window (texture + mapping params)
  captured with that frame's thermal params, so a frame never pairs one window's texture with
  another's mapping.

## Performance

ThermalCS: + 4 `R8_UINT` texel reads and a smoothstep blend for terrain pixels (budget + 0.1 ms at 1080p; as
built +0.27 ms: lookup and blend, warp and the 5-tap vegetation blur, p95 0.408 ms, inside gate (f)'s
0.5 ms; rulings S7, S10).
Window build on a task thread, < 100 ms, never on the game or render thread. Memory: as built, the
current window (4 MB texture + its 4 MB CPU copy, kept) plus at most one in-flight build (4 MB CPU) + tile LRU (default 64
tiles).

## Testing

- `CamSim.Thermal.LandCover.*`: tile index parsing, PNG decode, LRU; window resample against an
  analytic pattern (orientation, scale, centre); re-centre trigger; missing tiles → 0; antimeridian.
- `CamSim.Thermal.Reference.*` additions: smoothstep material blend; vegetation-index refinement
  both directions; asphalt/concrete split; no-base-colour fallback; land cover off = 4A output.
- `CamSim.GPU.Thermal.MatchesCpu` additions: land-cover cases (synthetic window, rotated East/North).
- `scripts/tests/test_fetch_worldcover.py`: tile naming/bbox maths, code passthrough.
- `thermal_check.py` additions (live, SF sample): (i) noon: IR mean of vegetation pixels (masked
  by greenness in the EO frame of the same pose) < IR mean of non-vegetation ground by ≥ 3 DN,
  both bands; (j) night: built-up/bare (non-green) > vegetation by ≥ 2 DN; (k) night class
  structure (ruling S12): temporal mean, 3×3 median (defect pixels), band-pass to 15–150 m ground
  scales, spatial std over the central 60 % with entities masked, land cover on ≥ 3× a
  land-cover-off launch at the same pose, both bands (the 33 px high-pass std measured detector
  noise and defects on the flat 4A frame, not scene structure: 1.94× MWIR, 1.05× LWIR); (l) window
  re-centre during a 1.2 km pan with `recentre_fraction` 0.02 (≥ 3 re-centres), split by S12 into
  l1: median over re-centres of the max wall time within 15 frames minus the pan's median ≤ 2 ms,
  and l2: the max over re-centres of the excess over a land-cover-off control pan at the same
  view times ≤ 5 ms (an isolated ~30 s engine hitch, present without land cover, otherwise lands
  on one re-centre by chance); (m) grid peak ratio is reported, not gated (a random-valued block
  field has sinc zeros, not a peak, at the grid fundamental: the visibly blocky build scored 0.68,
  the fixed one 0.67–0.74); criterion (d) is judged visually from the shots; existing gates
  (a)–(h) still pass.

## Risks

- **Greenness on dry Californian grass** (brown in summer imagery) reads as non-vegetation; the
  WorldCover prior (grassland) keeps it a vegetation-family class, and the refinement only blends
  toward `bare_soil` — acceptable, tune `veg_index_lo`.
- **Imagery baked shadows** lower base colour luminance; the asphalt/concrete split may call
  shadowed concrete asphalt. The ramp is soft; acceptable.
- **WorldCover licence** (CC BY 4.0) requires attribution: the index carries it and
  `docs/thermal.md` and the README credit it.
- **Same-colour class edges** (tree vs grass): imagery cannot refine them, so a 10 m staircase can
  remain after the smoothstep blend and warp (ruling S11); documented as a limit.
