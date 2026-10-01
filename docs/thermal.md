# Thermal radiance (IR)

ROADMAP 4A. In IR mode each pixel gets an in-band (MWIR or LWIR) radiance computed from a surface
temperature and emissivity, plus sky and path terms. The radiance goes through the unchanged
Milestone 3 sensor model (optics, detector, ADC, AGC). The goal is physically shaped behaviour for
ATR/ML training and operator training (night IR works, vehicles and roads read thermally, water
and land cross over diurnally), not traceable radiometry. Design:
[`superpowers/specs/2026-10-01-thermal-core-design.md`](superpowers/specs/2026-10-01-thermal-core-design.md).
Configuration keys: [`configuration.md`](configuration.md#thermal-thermal).

## Pipeline

```
game thread                                   render thread (IR mode, thermal available and enabled)
FThermalFrameBuilder -- FThermalFrameParams --> ThermalCS @ BeforeDOF (pre-TSR, render resolution)
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
4. **Terrain**: `terrain_default` (4B replaces this with a land-cover lookup).

NaN / Inf inputs are treated as terrain at the air temperature (tests hold them to bit tests,
never `x != x`).

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
Nothing selects a material per terrain location yet: terrain pixels use `terrain_default`
until 4B land cover. Classes are reached today through entity types.

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

- One terrain class. Night terrain is nearly uniform; the AGC stretches the optics' vignetting.
  Noon contrast is albedo-driven, so dark vegetation reads warm. 4B land cover fixes both.
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

`uv run scripts/thermal_check.py --band both` (use `caffeinate -ims`; about 5 launches per band,
MWIR + LWIR, 21 Dec noon and 02:00, DIS truck and boat) gates:

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

Gate (h)'s floor is the 8-bit snapshot's own rounding noise, 1/sqrt(12) DN (ruling R13). The run
writes `report.md`/`report.json`, shots and mask overlays under the output directory
(default `.cache/thermal_check/`). Latest results are in ROADMAP.md, 4A.
