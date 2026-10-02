# Thermal radiance (IR)

ROADMAP 4A. In IR mode each pixel gets an in-band (MWIR or LWIR) radiance computed from a surface
temperature and emissivity, plus sky and path terms. The radiance goes through the unchanged
Milestone 3 sensor model (optics, detector, ADC, AGC). The goal is physically shaped behaviour for
ATR/ML training and operator training (night IR works, vehicles and roads read thermally, water
and land cross over diurnally, terrain classes come from land cover), not traceable radiometry. Design:
[`superpowers/specs/2026-10-01-thermal-core-design.md`](superpowers/specs/2026-10-01-thermal-core-design.md).
Terrain classes (ROADMAP 4B): [Land cover](#land-cover-roadmap-4b). Configuration keys:
[`configuration.md`](configuration.md#thermal-thermal).

## Pipeline

```
game thread                                   render thread (IR mode, thermal available and enabled)
FThermalFrameBuilder -- FThermalFrameParams --> ThermalCS @ BeforeDOF (pre-TSR, render resolution)
  (+ land-cover window id, East/North, warp)       + land-cover window (R8_UINT, paired by id)
  FThermalModel   closed form per class            inputs: SceneDepth, CustomDepth/Stencil,
  FThermalSky     sky temperature                  GBuffer base colour (sRGB SRV), SceneColor, View UB
  FBandRadiance   in-band Planck LUT               writes float4(L, L, L, 1) into scene colour
  stencil table   entity -> class, offset                   |
                                                   DOF -> TSR (RGBA16F) resolves the radiance
                                                           |
                                                   SensorCS @ ReplacingTonemapper (input = radiance)
```

- **Game thread, per tick** (`FThermalFrameBuilder`, about 2.5 us for 32 classes): closed-form
  class temperatures at the sim time, sky terms, extinction, solar terms and the stencil table.
  No state is integrated, so clock jumps and frozen clocks are free and deterministic. Inputs come
  from `CamSimThermal::GatherFrameInputs` (CIGI atmosphere/weather, the atmosphere sun light, the
  ocean, live entities); every value handed to the builder is finite.
- **`ThermalCS`** (`Shaders/Private/CamSimThermal.usf`, `CamSimThermalCommon.ush`): per pixel
  classify, temperature, radiance. `CamSimThermalRef::EvaluatePixel` (`Thermal/ThermalReference.cpp`)
  is its CPU mirror, expression for expression; change both together.
  `CamSim.GPU.Thermal.MatchesCpu` holds them to 1e-4 relative radiance (measured 2.5e-6).
- **`SensorCS`** reads the TSR-resolved scene colour as radiance (`bRadianceInput`, no bloom,
  signal weights (1,0,0), `InputScale = 1 / B(300 K)`). The thermal AE slot is separate from the
  EO/proxy one; UE's own exposure is fixed at -12 EV in thermal mode.
- EO mode and `thermal.enabled: false` leave the 3B.2 path unchanged (bit for bit; gate (g)).

## Temperature model

A surface is a semi-infinite solid of thermal inertia I (J m^-2 K^-1 s^-1/2) with a linearised
exchange coefficient h = h_c + 4 eps sigma T_air^3. For a periodic net forcing F(t) (absorbed
sun through the cloud factor, air temperature, net longwave to the sky) the exact response is
`T(t) = sum_n Re[F_n e^{i n w t} / (h + I sqrt(i n w))]`, n = 0..6, w = 2 pi / day. The Fourier
coefficients come from 96 samples of the day, refit when the date or the camera lat/lon changes by
more than 0.5 degrees or cloud/air change (cloud quantised to 0.01, air to 0.05 K so it does not
refit every frame). Per frame it is 7 complex terms per class. Air temperature is
`T_air_mean + swing/2 * cos(w (t - 15 h local solar))`.

Per pixel a fast term adds local shading to the class temperature:
`T_pix = T_class + k_fast * (S_abs,pix - S_abs,ref) + T_offset(stencil)`.

- `S_abs,ref = (1 - albedo) S_clear(t) * cloud factor`: what the class model assumed (a sunlit
  horizontal surface).
- `S_abs,pix` is estimated from the EO render: `E_pix = pi * Lum(SceneColor / PreExposure) /
  (max(BaseLum, 0.03) * K_lum)`, `S_abs,pix = (1 - BaseLum) E_pix`, with `E_pix` clamped to
  `[0, 1.5 S_clear]` so highlights make no hot spots. `K_lum` divides the sun light illuminance by
  `S_clear * cloud factor`, so an unshadowed horizontal surface gives `S_abs,pix = S_abs,ref`
  exactly. It assumes the UE sun light's illuminance is not itself cloud-dimmed (cloud shadows come
  through scene colour). Shadows, slopes facing away and cloud shadows read cooler.
- **Base colour source**: GBuffer base colour at the tonemapper (spike, ROADMAP 4A): GBufferC is
  `PF_B8G8R8A8` with the sRGB flag, so ThermalCS samples it through the scene-texture SRV (hardware
  decode gives linear). Raw byte loads would be several K off. Metal's hardware decode is not
  bit-exact (3e-4 relative radiance through the fast term).
- **Fallback**: if the inputs are missing at runtime (no base colour or depth bound), the fast term
  is dropped (`k_fast = 0`), one warning is logged, and everything else is unchanged.
- Night: S = 0, so the fast term is zero or negative and only classes and offsets texture the image.

## Classification (per pixel, in order)

1. **Sky**: depth at the far plane. `L = B(T_sky(elevation))` from the view ray.
2. **Entity**: custom stencil `s > 0` and custom depth within 1 % of scene depth. Class and offset
   come from the 256-entry stencil table.
3. **Water**: geodetic height of the reconstructed position below the sea surface + 0.5 m + the
   current max wave amplitude, against a sea sphere fitted at the camera nadir.
4. **Terrain**: the land-cover blend inside the window ([below](#land-cover-roadmap-4b)), `terrain_default`
   outside it or with land cover off.

NaN / Inf inputs are treated as terrain at the air temperature (tests hold them to bit tests,
never `x != x`).

## Land cover (ROADMAP 4B)

Terrain pixels take their thermal class from ESA WorldCover 2021 v200 (10 m, 11 classes), refined per pixel by the
imagery's base colour. Design: [`superpowers/specs/2026-10-01-terrain-classification-design.md`](superpowers/specs/2026-10-01-terrain-classification-design.md).

**Data.** `scripts/landcover/fetch_worldcover.py --bbox W S E N [--out DIR]` reads ESA's 3 x 3 degree COGs
(`https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/2021/map/ESA_WorldCover_10m_2021_v200_N36W123_Map.tif`, named by the
SW corner) with HTTP range requests and writes 0.05 degree tiles: 600 x 600 8-bit greyscale PNGs (a WorldCover cell is
1/12000 degree; codes pass through, no resampling) plus `index.json` and `ATTRIBUTION.txt`. The San Francisco sample
(-122.56 37.69 -122.35 37.84, 20 tiles, 300 KB) is committed in `Content/NonUFS/LandCover` (git LFS; staged as loose files). CamSim
reads only `thermal.land_cover.dir`; there is no network access at runtime.

**Window.** `FLandCoverWindow` keeps a camera-centred window of `window_texels`^2 texels of 10 m (2048 = 20.48 km) on a local
East/North grid, row 0 = north. When the camera is more than `recentre_fraction` of the window from its centre, a new window is
resampled on a task thread (nearest cell; missing tiles = code 0) and swapped in on the game thread; the old window renders
meanwhile. Its `R8_UINT` texture is uploaded on the render thread and travels with each frame's thermal parameters, paired by
window id (`CamSimLandCover::ShouldBindWindow`): a frame never pairs one window's texture with another's mapping. Within 1 degree
of a pole, with no index, or with no data in range, land cover is off (one warning).

**Lookup.** For a terrain pixel (after the sky, entity and sea-band rules), `(E, N) = (P . East, P . North) / 100 + CamOffset`,
with P the camera-relative world position, East/North the unit vectors at the window centre (recomputed every frame from the
Cesium georeference, because origin shifts rotate the UE axes) and CamOffset the camera's offset from the centre (CPU doubles, same
small-area mapping as the resample). The lookup position is first moved by a ground-fixed domain warp, then the four nearest
texels each map through the code -> class table, are refined, and their class data (temperature, emissivity, k_fast, S_abs,ref) is
blended bilinearly with smoothstep fractions S(f) = f^2 (3 - 2f), so 10 m boundaries become C1 ramps with no creases at texel centres.
Outside the window, and in the half-texel border of it (the four-texel stencil needs a full neighbourhood): `terrain_default`.

**Domain warp** (`warp_amplitude_m` 6, `warp_cell_m` 20). Two smooth value-noise fields (integer PCG hash on a lattice, own stream 64)
move the lookup position by up to 6 m East and North, so class boundaries are wavy instead of axis-aligned 10 m staircases.
The noise is fixed to the ground: it is evaluated in a frame anchored at the session's first window (re-latched when `dir`
changes or a window is more than 200 km from the anchor; a per-axis anchor scale makes it exactly the same ground point whichever
window renders it), so it does not move when the camera pans or the window re-centres. The amplitude must stay below
`warp_cell_m / 3` or the lookup folds back on itself.

**Refinement** (needs the GBuffer base colour, not sunlight, so it also runs at night): `v = saturate((ExG - veg_index_lo) / (veg_index_hi - veg_index_lo))`,
`ExG = (2G - R - B) / (R + G + B)` of the linear base colour. The ExG is taken from the **mean of 5 base-colour samples** over a
world-space footprint (the pixel and four diagonal taps `veg_blur_m` = 2 m away, 1 to 32 px from the depth and pixel angle). Why: Cesium
imagery is JPEG, and its ~16 m chroma blocks drive ExG into square patches that the narrow green ramp turns into a block mosaic in
IR (visible in the 1200 m close view, and not removable by the class-lookup warp). Diagonal taps never share the centre's row or
column, so a block edge flips one tap at a time. 4 to 8 m radii hid the blocks entirely but the 5 sparse taps showed as shifted
copies of roads, so the default is 2 m. The asphalt/concrete split and the fast term use the pixel's own full-resolution luminance.

| Family (codes) | Refined material |
|---|---|
| Vegetation (10, 20, 30, 40, 90, 95, 100) | v x own class + (1 - v) x `bare_soil` (trails, clearings) |
| Built-up (50) | v x `vegetation` + (1 - v) x (`asphalt` below `asphalt_max_luma`, `concrete` above; 0.04 soft ramp) |
| Bare (60) | v x `vegetation` + (1 - v) x own class |
| None (0, 70, 80) | own class |

The asphalt/concrete split applies in built-up only (bare ground keeps its own class: a dark/bright split there would turn bare
soil into asphalt). Refinement is off when no base colour is bound (the 4A fallback); the WorldCover blend alone is used.

**Classes** (`thermal.land_cover.classes.<code>` overrides):

| WorldCover | Code | Default material |
|---|---|---|
| Tree cover | 10 | `tree_canopy` |
| Shrubland | 20 | `shrubland` |
| Grassland | 30 | `grassland` |
| Cropland | 40 | `cropland` |
| Built-up | 50 | `built_up` (asphalt / concrete split with base colour) |
| Bare / sparse vegetation | 60 | `bare_soil` |
| Snow and ice | 70 | `snow_ice` (capped at 0 C) |
| Permanent water bodies | 80 | `water` |
| Herbaceous wetland | 90 | `wetland` |
| Mangroves | 95 | `wetland` |
| Moss and lichen | 100 | `grassland` |
| No data / other | 0 | `terrain_default` |

Vegetation classes fold evapotranspiration into an *effective* albedo (the closed-form model has no latent heat term), so they stay
cool at noon. Class temperatures at San Francisco, 21 December (`CamSim.Thermal.LandCover.DiurnalContrastsAtSanFrancisco` pins
the orderings: every vegetation class at least 1 K below `built_up` and `bare_soil` at noon; `built_up` and `bare_soil` at least 1 K
above `tree_canopy` at night; `tree_canopy` at least 0.5 K above `grassland` at night):

| Class | Noon (K) | 02:00 (K) |
|---|---|---|
| `terrain_default` | 303.0 | 284.8 |
| `water` | 288.5 | 287.7 |
| `vehicle_paint` | 303.4 | 283.3 |
| `asphalt` | 303.5 | 285.6 |
| `vegetation` | 299.1 | 283.1 |
| `concrete` | 297.9 | 285.5 |
| `tree_canopy` | 296.7 | 283.7 |
| `shrubland` | 298.7 | 283.3 |
| `grassland` | 298.9 | 282.6 |
| `cropland` | 299.3 | 283.4 |
| `built_up` | 301.1 | 285.7 |
| `bare_soil` | 302.5 | 285.2 |
| `snow_ice` | 273.1 | 273.1 |
| `wetland` | 294.7 | 285.6 |

**Snow.** `snow_ice` uses the `snow` temperature source: the model temperature capped at 273.15 K (the class value is already
below it; the per-pixel fast term, `k_fast` 0.005, can still lift sunlit snow slightly above 0 C: known).

**Keys** (all in [`configuration.md`](configuration.md#thermal-thermal), under `thermal.land_cover`): `enabled`, `dir`,
`window_texels`, `recentre_fraction`, `veg_index_lo/hi`, `asphalt_max_luma`, `warp_amplitude_m`, `warp_cell_m`, `veg_blur_m`,
`classes.<code>`. `enabled: false` is 4A bit for bit (`CamSim.Thermal.Reference.LandCoverOffIs4A`,
`CamSim.Thermal.Builder.LandCoverDisabledIs4A`, the GPU "off" case).

**Another area.** Run `uv run scripts/landcover/fetch_worldcover.py --bbox W S E N --out DIR` (needs network and `rasterio`, which
`uv run` fetches), point `thermal.land_cover.dir` at DIR (relative to the project directory). Open-ocean COGs that don't exist
are listed under `missing` and read as no data. Keep `ATTRIBUTION.txt` with the data.

**Edge cases.** No index / missing directory / no data in range: land cover off, one warning, 4A behaviour. An LFS pointer file
instead of a PNG fails the tile decode (a warning; run `git lfs pull`). Outside the window (very high altitude): `terrain_default`.
Within 1 degree of a pole: off. Antimeridian: tiles are fetched by their own lat/lon, longitude does not wrap inside a window.
Axes that fail the orthonormality/handedness check, or a missing georeference: off for that frame (one warning). A null window
texture (before its upload runs) is off for that frame.

### Land-cover performance (M1 Pro, Metal, SF sample)

| Item | Measured |
|---|---|
| Window build (task thread, 2048^2, cold cache incl. PNG decode) | 27.0-27.3 ms for the first window (20 tiles with data, 10 outside the sample); 6-7 ms for later windows (cached tiles); 14.8 ms for the synthetic 30-tile test directory. Target < 100 ms |
| Window swap | game thread: `MakeShared` + render-command enqueue; the 4 MB upload runs on the render thread and left no trace in frame times |
| `ThermalCS` p95 at 1080p, land cover on | 0.408 ms (gate f, 959 frames; median 0.400, max 0.418; 0.407 at 720p) vs 4A's 0.141 ms |
| Cost breakdown | lookup + blend +0.134 ms (Task 10, 0.305), smoothstep + warp +0.045 ms, 5-tap vegetation blur +0.06 ms |
| Frame builder | about 16 us per frame with land cover (4A: 2.5 us) |
| GPU vs CPU reference (`CamSim.GPU.Thermal.LandCoverMatchesCpu`) | worst relative radiance error 4.7e-6 over every case (limit 1e-4); land cover off is bit-identical to no window |

The land-cover cost is above the +0.1 ms the spec planned (total +0.27 ms over 4A) but inside the 0.5 ms gate (f). Possible
follow-ups: skip the vegetation taps for the None family and non-terrain pixels, read fewer class tables per pixel.

### Limits (4B)

- **10 m class-edge staircase between same-colour classes.** Tree cover next to grassland is green on both sides, so the
  imagery cannot refine the edge: the smoothstep blend and the 6 m warp soften it but a diagonal staircase can still show at close
  range (1200 m at 40 degrees, 0.45 m/px). Boundaries that the imagery distinguishes (trees vs lawn vs road vs bare) follow the imagery.
- Small-area mapping error grows with tan(latitude) and distance from the window centre: about one texel at the SF window corners,
  9 % at 89 degrees (hence the 1 degree pole cut-off).
- No roads layer: asphalt versus concrete is a luminance split of the base colour inside built-up. Imagery shadows bias it (shaded
  concrete reads asphalt).
- WorldCover 2021 is one epoch (no seasons, no change since); a CPU copy of the window is kept next to its texture.
- The warp amplitude is not validated against `warp_cell_m / 3` (documented only).
- Snow can exceed 0 C through the per-pixel fast term until that is fixed (ruling: set `snow_ice.k_fast` 0 or cap after the fast term).
- A pre-existing engine hitch of about +7 ms every ~30 s is visible in frame times, with land cover on or off; it is not 4B.
- Linux/Vulkan is unverified for land cover (as for thermal).

Data attribution (CC BY 4.0): © ESA WorldCover project 2021 / Contains modified Copernicus Sentinel data (2021) processed by
ESA WorldCover consortium.

## Radiance

    L_surf = eps B(T_pix) + (1 - eps) L_sky,hemi
    L      = tau L_surf + (1 - tau) B(T_air),   tau = exp(-beta * range_km)

`B(T)` is a 1024-entry LUT over 150-1000 K of the in-band Planck integral (MWIR 3-5 um, LWIR
8-12 um, from `detector.band_lo_um/band_hi_um`; interpolation error <= 0.1 %). Sky temperature is
Swinbank with an angular emissivity and a cloud blend toward T_air. `beta` is the band
extinction plus, with CIGI fog and visibility under 10 km, `3.912 / V_km * fog_ir_factor`.

## Classes and keys

Built-in classes (index order fixed; 6-13 added by 4B land cover), overridable and extensible under `thermal.materials`
(unset fields copy `terrain_default`, at most 32 classes):

| Class | albedo | emissivity | inertia | h_c | k_fast | temperature |
|---|---|---|---|---|---|---|
| `terrain_default` | 0.20 | 0.95 | 1200 | 10 | 0.015 | model |
| `water` | 0.06 | 0.98 | - | - | 0 | water temperature +/- 0.5 K |
| `vehicle_paint` | 0.30 | 0.90 | 600 | 12 | 0.04 | model |
| `asphalt` | 0.10 | 0.95 | 1500 | 10 | 0.02 | model |
| `vegetation` | 0.40\* | 0.98 | 500 | 18 | 0.008 | model |
| `concrete` | 0.35 | 0.92 | 1800 | 10 | 0.015 | model |
| `tree_canopy` | 0.40\* | 0.98 | 800 | 25 | 0.004 | model |
| `shrubland` | 0.40\* | 0.97 | 600 | 18 | 0.008 | model |
| `grassland` | 0.50\* | 0.97 | 300 | 15 | 0.010 | model |
| `cropland` | 0.40\* | 0.97 | 700 | 15 | 0.010 | model |
| `built_up` | 0.20 | 0.93 | 1650 | 10 | 0.018 | model |
| `bare_soil` | 0.25 | 0.93 | 1300 | 8 | 0.025 | model |
| `snow_ice` | 0.75 | 0.99 | 600 | 10 | 0.005 | snow (model, capped at 273.15 K) |
| `wetland` | 0.40\* | 0.98 | 2500 | 15 | 0.004 | model |

\* Effective albedo: folds evapotranspiration into the absorbed solar (the model has no latent heat term).

Every key, range and env var is in [`configuration.md`](configuration.md#thermal-thermal):
`thermal.enabled`, `air_temperature_c`, `air_diurnal_swing_k`, `extinction_per_km.{mwir,lwir}`,
`fog_ir_factor`, `materials.<name>.*`, `entity_types.<id>.thermal_material` / `thermal_offset_k`,
`ocean.water_temperature_c`, `sensor_modes.<mode>.agc_max_display_gain` (default 40, thermal AGC
only) and `sensor_modes.<mode>.thermal_exposure`.

### Adding a material

Add a block to the config; no editor work or code is needed:

```yaml
thermal:
  materials:
    gravel: {albedo: 0.25, emissivity: 0.93, thermal_inertia: 1000, convection_w_m2k: 10, k_fast: 0.015}
```

Names are lower-case letters, digits and `_`. Validation errors (range, name, `temperature` not
`model`/`water`/`snow`, more than 32 classes) are logged at startup.
Terrain pixels pick a class from land cover (below) or fall back to `terrain_default`; entities
reach classes through their type.

### Entity types

`entity_types.<id>.thermal_material` names the class for that type's pixels (default
`vehicle_paint`; an unknown name falls back with one warning). `thermal_offset_k` adds kelvin
(default +8 K for land and surface vehicles, 0 otherwise; outside `[-50, 500]` it is ignored with a
warning). Entities must be stencil-tagged: they are whenever thermal is available, not only for
ground truth (stencil values 1..255 from `FStencilSlotAllocator`; exhaustion is logged).

## TSR and the BeforeDOF path

At `ReplacingTonemapper` the jittered render-resolution inputs (depth, stencil, base colour) were
mixed with TSR-resolved scene colour, so class edges and the fast term shimmered against each
other by the sub-pixel jitter (MWIR edge std 6.7-7.0 times the interior; EO 0.99). ThermalCS now
runs at `EPostProcessingPass::BeforeDOF`, subscribed per frame only while thermal parameters are
set, and writes radiance into scene colour so TSR resolves it with the same jitter as its inputs.

- **Precision**: TSR's output is `PF_FloatR11G11B10` without the alpha channel, which quantises
  radiance to 0.2-0.4 K (MWIR) / 0.5-1 K (LWIR), above the detector NETD. `FThermalTsrAlpha`
  sets `r.TSR.AlphaChannel=1` (SetByCode) only while thermal IR is active and restores the saved
  value otherwise (a console override wins), so TSR output and history are RGBA16F. Measured:
  MWIR night interior std 0.79 to 0.32 DN. The format change drops TSR history at every
  EO to IR switch, which doubles as a free cut.
- **Cost**: IR GPU frame time +1.6 ms on the M1 Pro (720p median `gpu_ms` 26.83 to 28.42 ms,
  1080p 27.58 to 29.18 ms; EO unchanged), and the first EO to IR switch costs one 84 ms GPU frame
  (a PSO compile; not precached) with no dropped frames. The frame stays under 33 ms (ruling R12).
  Alternative if a weaker GPU needs it: keep R11G11B10 and write a signed two-channel encoding
  decoded by SensorCS.
- **Particles**: post-DOF translucency (particles) is composited by TSR in visible colour over the
  radiance; particles are not thermally modelled in 4A.
- `thermal_gpu_ms` on Metal: a stat scope is timed by the encoders that begin inside it, and RDG
  kept ThermalCS in one compute encoder with DOF/TSR after it. Never-culled 1-texel copies before
  and inside the scope (`Camera/SensorGpuTimer.h`) give the pass its own encoder.

## Performance (M1 Pro, Metal)

| Item | Measured |
|---|---|
| `ThermalCS` GPU p95 | 0.137 ms (MWIR) / 0.136 ms (LWIR) at 720p, over about 4.8k IR frames; 0.141 ms at 1080p, 958 frames (gate f, budget 0.5 ms). The cost barely depends on resolution. |
| Builder | 2.5 us per frame, 32 classes (budget 50 us) |
| IR TSR alpha path | +1.6 ms GPU per frame (see above) |
| GPU vs CPU reference | 2.5e-6 max relative radiance error (limit 1e-4) |

## Limits (4A)

- A MWIR truck clips white at night (offset +8 K on a cool background, AGC centred on terrain).
- The thermal cloud term follows CIGI weather only; EO cloud layers do not cool the scene.
- Particles are composited in visible colour (above).
- Entities have one class plus an offset, no engine, exhaust or tyre detail (4C).
- At most 255 entities are stencil-tagged at once (shared with ground truth); the rest render as
  terrain in IR (and get projected ground-truth boxes), with a one-time EntityManager warning.
- No thermal shadow lag, sun glint in MWIR or heating from artificial lights.
- Linux/Vulkan is unverified, including sRGB decode precision of the base colour.
- After an EO/IR mode switch one in-flight histogram can nudge the new slot's gain before the
  snap (the 3B.2 cut convergence, 1 to 3 frames).

## Acceptance

`uv run scripts/thermal_check.py --band both` (use `caffeinate -ims`; 9 launches or more: per band
`bands` and `lcoff`, then `hd`, `eo` (2), `pan` (with its control); MWIR + LWIR, 21 Dec noon and 02:00,
DIS truck and boat) gates:

| Gate | Check |
|---|---|
| a | night mean Y in [60, 180], black (Y <= 16) under 5 % |
| b | truck box minus surrounding ring >= +3 DN at night (white-hot) |
| c | water minus land (radius-matched) changes sign between night and noon |
| d | noon shadow minus sunlit < 0 |
| e | sky minus terrain < 0 |
| f | `thermal_gpu_ms` p95 <= 0.5 ms at 1080p |
| g | EO mean Y with thermal on vs off within 1 DN; ThermalCS never runs in EO |
| h | coast edge temporal std / max(interior std, 0.289 DN) <= 2 |
| i | noon `nadir_mixed` (800 m, 40 deg, Presidio): non-vegetation minus vegetation mean Y >= +3 DN (masks from the EO frame's excess green: > 0.06 / < 0.02, central 60 %, eroded 2 px, entities removed) |
| j | night, same masks: non-vegetation minus vegetation >= +2 DN |
| k | night class structure (ruling S12): temporal mean, 3x3 median, band-pass 15-150 m (box 33 px minus box 331 px at 0.455 m/px), spatial std over the central 60 %: land cover on >= 3x the land-cover-off launch, both bands |
| l1 | 1.2 km pan with `recentre_fraction` 0.02 (>= 3 re-centres): median over re-centres of (max wall time within 15 frames minus the pan median) <= 2 ms |
| l2 | same pan: max over re-centres of the excess over a land-cover-off control pan at the same view times <= 5 ms |
| m | grid peak ratio, info only (no gate: a random block field has no peak at the grid fundamental) |

Gate (h)'s floor is the 8-bit snapshot's own rounding noise, 1/sqrt(12) DN (ruling R13). The run
writes `report.md`/`report.json`, shots and mask overlays under the output directory
(default `.cache/thermal_check/`). Latest results are in ROADMAP.md, 4A and 4B.
