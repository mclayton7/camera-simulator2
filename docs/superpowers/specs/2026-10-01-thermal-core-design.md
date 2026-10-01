# Thermal core (ROADMAP 4A) — design

Status: approved by the user's delegation (2026-10-01: "take your recommendations through to
completion of 4A"). Milestone 4 is split into 4A thermal core (this spec), 4B terrain
classification, 4C entity thermal state, 4D labels + validation.

## Goal

IR mode stops being the luminance of the visible image. Each pixel gets an in-band (MWIR or
LWIR) radiance computed from a surface temperature and emissivity, and that radiance goes
through the existing Milestone 3 optics → detector → ADC → AGC chain.

Purpose (from the user): **ATR/ML training data** (thermal contrast consistent across scenes,
no EO cues, right diurnal behaviour) and **operator-training realism** (looks like a real
turret feed: night IR works, vehicles and roads read thermally, crossover happens).
Traceable radiometry is not a goal; physically shaped behaviour is.

Success, in one sentence: a night MWIR/LWIR shot of a DIS truck and boat at San Francisco is a
plausible thermal image (not dark, not an EO negative), a noon shot shows warm sunlit ground
and cooler shadows, and water is warmer than land at night and cooler at noon.

## Non-goals (later sub-projects)

- Land-cover classes for terrain (4B). 4A has one default terrain class plus water.
- Per-part entity temperatures, engine/exhaust/tyre hot spots, running state (4C). 4A has one
  class plus a configurable offset per entity type.
- Semantic class in ground truth, validation suite against published data (4D).
- Thermal shadow lag (a moved shadow's cool footprint persisting), sun glint in MWIR, heating
  from artificial lights, MODTRAN-grade atmosphere.

## Architecture

```
game thread                                    render thread (IR mode, thermal on)
FThermalFrameBuilder ── FThermalFrameParams ──► ThermalCS @ BeforeDOF (pre-TSR, render resolution)
  ├ FThermalModel (closed form, per class)          │  inputs: SceneDepth, CustomDepth/Stencil,
  ├ FThermalSky (sky temperature)                   │  GBuffer base colour, SceneColor, View UB
  ├ FBandRadiance (in-band Planck LUT)              ▼  writes float4(L, L, L, 1) into scene colour
  └ stencil → class/offset table (entities)    DOF → TSR (RGBA16F, resolves the radiance)
                                                    ▼
                                               SensorCS @ ReplacingTonemapper (unchanged maths; input =
                                               resolved radiance, no pre-exposure division, no bloom,
                                               weights (1,0,0))
```

(Task 17 moved ThermalCS from `ReplacingTonemapper`, where it mixed jittered render-resolution
inputs with TSR-resolved scene colour and shimmered at class edges, to `BeforeDOF`. TSR then
resolves the radiance with the same jitter as its inputs. While thermal IR runs
`r.TSR.AlphaChannel=1` makes TSR's output and history RGBA16F; its default R11G11B10 would
quantise radiance to 0.2-0.4 K (MWIR) / 0.5-1 K (LWIR), above the detector NETD.)

New code:

| Unit | Where | Purpose |
|---|---|---|
| `FThermalMaterial`, `FThermalMaterialTable` | `CamSimTest/Thermal/ThermalMaterials.h/.cpp` | built-in classes + `thermal.materials` overrides; name → index |
| `FBandRadiance` | `CamSimTest/Thermal/BandRadiance.h/.cpp` | in-band Planck integral; LUT T → L (W m⁻² sr⁻¹) |
| `FThermalModel` | `CamSimTest/Thermal/ThermalModel.h/.cpp` | closed-form surface temperature per class at a sim time |
| `FThermalSky` | `CamSimTest/Thermal/ThermalSky.h/.cpp` | effective sky temperature by elevation and cloud cover |
| `FThermalFrameBuilder` | `CamSimTest/Thermal/ThermalFrameBuilder.h/.cpp` | fills `FThermalFrameParams` each frame from clock, environment, camera, entities |
| `CamSimThermalRef` | `CamSimTest/Thermal/ThermalReference.h/.cpp` | CPU reference of `ThermalCS` (per pixel), mirrored expression for expression |
| `FThermalFrameParams` | `CamSimShaders/Public/ThermalFrameParams.h` | POD: class table, stencil table, LUT, sky, atmosphere, sea sphere, solar terms |
| `AddThermalPass` | `CamSimShaders/Public/ThermalPass.h`, `Private/ThermalPass.cpp` | RDG pass |
| `ThermalCS` | `Shaders/Private/CamSimThermal.usf` + `CamSimThermalCommon.ush` | per-pixel classify → temperature → radiance |

Changed: `CamSimFrameGrabExtension` (runs the thermal pass at `BeforeDOF` in IR mode; `SensorCS`
reads the TSR-resolved radiance as scene colour), `SensorGraph` (an input flag: radiance input, no pre-exposure, no bloom),
`SensorPresets` (`band_lo_um`/`band_hi_um`), config (`thermal:` section, `entity_types.*.thermal_*`),
the IR AE seed (radiance input changes the signal scale).

## Temperature model (closed form, per class)

A surface is a semi-infinite solid of thermal inertia I (J m⁻² K⁻¹ s⁻½) with a linearised
surface exchange coefficient h (W m⁻² K⁻¹, convection + linearised longwave, h = h_c + 4 ε σ T_air³).
For a periodic net forcing F(t) = Σ F_n e^{i n ω t} (ω = 2π / day), the exact surface response is

    T(t) = Σ Re[ F_n e^{i n ω t} · H(n ω) ],      H(ω) = 1 / (h + I √(i ω)),  H(0) = 1/h

The forcing is `F(t) = (1 − a)·S(t)·(1 − 0.75 c^3.4) + h·T_air(t) + ε (L_sky(t) − σ T_air(t)⁴)`
where S is clear-sky horizontal solar irradiance from the sun elevation over the day at the
camera's location and date, c is cloud cover (Kasten–Czeplak attenuation), a is albedo, and
T_air(t) = T_air_mean + A_air · cos(ω (t − 15 h local solar)).

- F_n for n = 0..6 come from 96 samples of the day (every 15 min), recomputed when the date
  or the camera's latitude/longitude changes by more than 0.5°, or cloud cover / air temperature
  change. That costs well under 1 ms and is cached; per frame it is 7 complex terms per class.
- No integration state: any sim time evaluates directly. Clock jumps, frozen clocks and
  lockstep are free and deterministic.
- Limits that tests hold it to: I → 0 tracks the instantaneous equilibrium F(t)/h; larger I
  lowers the amplitude and moves the peak later (toward the 2-3 h lag of soil, ~6 h of water
  if it were modelled that way); the daily mean equals F_0/h for every I.
- Classes can instead be **fixed** temperature (water: `T = water temperature`, from CIGI /
  `ocean.water_temperature_c`; a diurnal swing of ±0.5 K is added).

Per pixel, a fast term adds local shading on top of the class temperature:

    T_pix = T_class(t) + k_fast · (S_abs,pix − S_abs,ref(t)) + T_offset(stencil)

S_abs,ref(t) = (1 − a)·S(t)·cloud factor is what the class model assumed (a sunlit horizontal
surface). S_abs,pix is the absorbed flux the pixel actually saw (next section), so shadows,
slopes facing away and overcast regions come out cooler, sun-facing slopes warmer. k_fast is the
skin's quick response (≈ 1/(h + I_skin √ω_fast)); defaults per class (asphalt 0.02 K/(W m⁻²),
vegetation 0.008, metal paint 0.04).

### Per-pixel absorbed solar flux from the EO render

The visible render already contains the sun, sky, clouds, terrain shading and shadows. For a
diffuse surface of albedo a lit by irradiance E, its luminance is a·E/π. So

    E_pix = π · Lum(SceneColor / PreExposure) / (max(BaseLum, 0.03) · K_lum)
    S_abs,pix = (1 − a_pix) · E_pix,   a_pix = BaseLum

with BaseLum the luminance of the GBuffer base colour and K_lum the luminous efficacy that
maps the scene's photometric units to W m⁻². K_lum divides the sun light's illuminance (at the
frame's sun elevation) by S_clear × the cloud factor, so an unshadowed horizontal surface gives
S_abs,pix = S_abs,ref exactly (ruling R7; this assumes the UE sun light's illuminance is not
itself cloud-dimmed: cloud shadows reach the pixel through scene colour). E_pix is clamped to [0, 1.5 · S_clear(t)] so specular highlights
and emissive surfaces do not create hot spots.

**Fallback** (if Substrate's blendable GBuffer does not expose base colour at the tonemapper,
decided by the spike task): the fast term is dropped (k_fast = 0) and a warning is logged once;
everything else is unchanged.

## Classification (per pixel, in order)

ThermalCS runs at `BeforeDOF`, before the temporal upscaler: depth, custom depth / stencil, base
colour and scene colour are all render-resolution and share the TSR jitter, so classes and the
fast term move together; TSR then resolves the radiance. It writes the radiance to scene colour
(`WRITE_SCENE_COLOR` permutation, `float4(L, L, L, 1)`, raw W m⁻² sr⁻¹, alpha 1).

1. **Sky**: device depth at the far plane. Radiance = B(T_sky(elevation)) where elevation comes
   from the view ray. No surface term, no path term (T_sky already includes the atmosphere).
2. **Entity**: custom stencil s > 0 *and* custom depth within 1 % of scene depth (the entity is
   the visible surface, not occluded). Class and offset come from the 256-entry stencil table the
   game thread fills from the entity manager (`entity_types.<id>.thermal_material`, default
   `vehicle_paint`; `thermal_offset_k`, default +8 K for land/sea vehicles, 0 otherwise).
3. **Water**: geodetic height of the reconstructed world position below the local sea surface
   + 0.5 m + the current max wave amplitude. The sea surface is a sphere fitted at the camera
   nadir (centre and radius in UE units from Cesium + EGM96 + tide), which is accurate to
   centimetres over a view footprint of tens of km.
4. **Terrain**: `terrain_default` (4B replaces this with a land-cover lookup).

## Radiance

    L_surf = ε B(T_pix) + (1 − ε) L_sky,hemi
    L      = τ L_surf + (1 − τ) B(T_air),   τ = exp(−β · range_km)

- B(T) is the in-band radiance from a 1024-entry LUT over 150–1000 K (upper range left for 4C
  exhausts), built by numerically integrating Planck's law over the band (MWIR 3–5 µm, LWIR
  8–12 µm; preset keys `band_lo_um`, `band_hi_um`; ≤ 0.1 % interpolation error).
- Sky temperature: clear-sky zenith Swinbank T_clear = 0.0552 T_air^1.5, angular emissivity
  ε(θ) = 1 − (1 − ε_z)^(1/ max(sin el, 0.05)) with ε_z = (T_clear/T_air)⁴, cloud cover blends
  T_sky⁴ = (1 − c) T_clear,θ⁴ + c T_air⁴. L_sky,hemi uses the view-factor average of ε(θ)
  (precomputed per frame).
- β per band: `thermal.extinction_per_km` (default MWIR 0.15, LWIR 0.10) plus a fog term from
  the environment visibility, β_fog = 3.912 / V_km · `thermal.fog_ir_factor` (default 0.4; IR
  sees farther than visible through haze).
- ThermalCS writes raw L into scene colour; TSR carries it in RGBA16F (alpha channel enabled
  only while thermal IR runs). SensorCS then reads the resolved scene colour as radiance.
- The detector signal is L / B(300 K). The IR AE/AGC then set gain and stretch as today.
  Preset sensitivity (`max_photon_gain_ev`) is re-tuned so a 300 K scene sits mid-range.

## Inputs, units, edge cases

- T_air: `FCamSimEnvironment` air temperature (CIGI Atmosphere Control; config default 15 °C),
  diurnal swing `thermal.air_diurnal_swing_k` (default 8 K). Cloud cover: environment cloud
  coverage (0 if unset). Humidity is unused in 4A.
- NaN / Inf scene colour or depth → treated as sky-free terrain at T_air (no NaN reaches the
  detector, matching `SensorCS`'s sanitise rule).
- Night: S = 0, so the fast term is ≤ 0 everywhere and only exists where E_pix > 0 (artificial
  lights give E_pix ≈ 0.1 W m⁻², negligible).
- `thermal.enabled: false` restores the 3B.2 luminance proxy (kept for A/B comparison).

## Performance budget

ThermalCS at 1080p on the M1 Pro: ≤ 0.5 ms p95 (one 8×8-thread pass, ~6 texture reads, a LUT
lookup, no groupshared). CPU: Fourier coefficients cached; per frame < 50 µs for ≤ 32 classes and
the 256-entry stencil table. Measured with an `FGPUStat` scope like `SensorGpuTimer`.

## Testing

- `CamSim.Thermal.Planck.*`: band integral vs. closed form (whole-spectrum limit = σT⁴/π
  within 0.1 %), LUT interpolation error, monotonic.
- `CamSim.Thermal.Model.*`: zero inertia tracks equilibrium; amplitude falls and lag rises with
  inertia; daily mean = F_0/h; determinism under clock jumps; crossover (water vs soil) exists;
  clouds damp amplitude; fixed-temperature classes.
- `CamSim.Thermal.Sky.*`: zenith colder than horizon; overcast → T_air.
- `CamSim.Thermal.Reference.*`: per-pixel reference: classification order, fast term sign
  (shadow cooler), path term (far = T_air), NaN handling.
- `CamSim.Thermal.Config.*`: `thermal:` section and `entity_types` keys through the loader,
  validation, canonical config has no unknown keys.
- `CamSim.GPU.Thermal.MatchesCpu`: `ThermalCS` vs. `CamSimThermalRef` on synthetic textures
  (sky, entity occluded/visible, water, terrain, shadow, NaN) within 1e-4 relative radiance.
- Live acceptance (`scripts/thermal_check.py`): MWIR and LWIR at local noon and 02:00 with the
  DIS truck and boat; checks (a) night IR mean luma 60–180 and < 5 % black, (b) vehicle box mean
  vs. surrounding terrain ≥ 3 AGC-stretched DN brighter at night (white-hot), (c) water vs land
  sign flips between noon and night, (d) shadowed terrain cooler than sunlit at noon, (e) sky
  darker than terrain, (f) ThermalCS p95 ≤ 0.5 ms, (g) EO unchanged (thermal off for EO), (h) coast-edge temporal std / interior std <= 2 (interior
  floored at the 8-bit snapshot's rounding noise, 0.289 DN).
  Shots go to the user for visual review.

## Risks

- **Substrate GBuffer base colour** at `ReplacingTonemapper` is unverified → first plan task is
  a spike; fallback above.
- **Jitter** (resolved in Task 17): at `ReplacingTonemapper` the jittered render-resolution
  inputs and the resolved scene colour disagreed by the sub-pixel jitter, giving 6.7-7.0x edge
  shimmer in MWIR. ThermalCS now runs at `BeforeDOF` so TSR resolves the radiance; acceptance gate
  (h) holds edge / interior temporal std <= 2 (measured 1.0-1.4, floored).
- **AE retune**: radiance has a large offset and small contrast; the AGC's percentile stretch
  handles it, but the IR AGC's missing max-gain cap (3B.3) can amplify a flat night scene. 4A
  adds the cap (`agc.max_display_gain`, default 40).
