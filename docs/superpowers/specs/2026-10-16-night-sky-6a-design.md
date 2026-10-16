# 6A: Night sky, absolute light levels and radiometric EO exposure

Status: approved in conversation 2026-10-16. ROADMAP Milestone 6, first sub-project (6A → 6B NVG → 6C light points).
Includes ROADMAP 9.4 (radiometric photon gain) for the EO photon detector.

## Goal

Night EO frames whose level and noise follow the real sky: moon position, phase and elevation, starlight and airglow
for the sim date, time and place. 6B (image intensifier) builds on the same light levels; 6C (light points) on the
same scale. Users: ATR/ML dataset generation and operator training, physics first (decided 2026-10-16).

## Findings that shape the design (source-checked 2026-10-16)

1. **The scene is already in physical units.** Main.umap's sun is `ACesiumSunSky`'s directional light at 111,000 lux
   (`CesiumSunSky.cpp:59`); `CamSimCaptureComponent.h:270` measured log2 PreExposure = −15.5 in daylight.
   `ACamSimEnvironment` looks for standalone `ADirectionalLight` / `ASkyLight` / `ASkyAtmosphere` /
   `AExponentialHeightFog` actors, which Main.umap doesn't have (the light, sky light and atmosphere are
   CesiumSunSky components; there is no fog actor). Its sun intensity table (10 / 3 / 0.1 / 0.02 "lux"), sky
   recapture, fog and overcast sky-light code never run. Night today is only ~5 EV below day (bench `night_slant`
   median 2^6.6 vs 2^10.9–2^12.4); it should be ~18 EV (full moon) to ~26 EV (moonless).
   **Decision: scene scale k = 1** (scene colour / PreExposure = luminance in cd/m², light intensity in lux).
2. **Lumen's cached lighting and the real-time sky capture use a fixed exposure**,
   `r.EyeAdaptation.CachedLightingPreExposure` (project: 8 → safe view exposure [−4, +16] EV,
   `PostProcessEyeAdaptation.cpp:246-251`). Night needs about −3 (full moon) to −10 EV (moonless): it clips.
   UE's warning is a screen message, which CamSim disables.
3. **SkyAtmosphere supports two atmosphere lights** (`NUM_ATMOSPHERE_LIGHTS 2`; `AtmosphereSunLightIndex` 0/1),
   draws a disc for each, and the real-time sky capture and volumetric fog see both; exponential height fog's
   directional inscattering uses light 0 only. `bAtmosphereSunLight` defaults to true, so a second light must set
   index 1 explicitly. The engine disc is uniform: no phase.
4. **A mesh with an "Is Sky" material replaces the atmosphere in the real-time sky capture**
   (`ReflectionEnvironmentRealTimeCapture.cpp:743`): if any IsSky mesh exists, the capture renders only those. An
   opaque dome also covers SkyAtmosphere and its discs in the main view. So the dome must draw the atmosphere
   itself (`SkyAtmosphereViewLuminance`) plus the discs.
5. CesiumSunSky sets `TransmittanceMinLightElevationAngle = 90` (engine default −90): ground transmittance is
   computed as if the sun were at zenith, so the light never dims or reddens at sunset by itself.
6. MegaLights needs hardware ray tracing (`r.RayTracing=False`, and the M1 Pro has none): a 6C issue, noted here.

## Design

### Units and lights

- **k = 1.** Directional light intensities are top-of-atmosphere illuminance in lux; UE's atmosphere applies the
  transmittance. Set CesiumSunSky's `SkyAtmosphere->TransmittanceMinLightElevationAngle` back to −90 so sun and moon
  dim and redden with air mass and go dark below the horizon (the planet occludes them). Sun TOA illuminance =
  127,500 lux × (1 AU / r)² (r from the ephemeris).
  - Risk: Cesium chose 90 for a reason we haven't found (globe-scale planet centre?). Acceptance N1/N8 catch a
    daytime regression; fallback = keep 90 and scale the sun by the ephemeris-computed transmittance (Kasten–Young air
    mass, same constants as the moon).
- **Moon:** a new `UDirectionalLightComponent` on `ACamSimEnvironment`: `bAtmosphereSunLight` true,
  `AtmosphereSunLightIndex 1`, `ForwardShadingPriority` below the sun's, `CloudScatteredLuminanceScale` white,
  `bCastShadowsOnClouds` false (engine: unsupported at index 1), `LightSourceAngle` 0.52°, colour from a 4,100 K
  blackbody-ish tint (the moon is redder than the sun). Shadows on (VSM) only while it is the brighter light; when the
  moon is below the horizon or disabled, intensity 0 and the component hidden (no shadow cost).
  - Moon TOA illuminance (Krisciunas & Schaefer 1991): I* = 10^(−0.4 (3.84 + 0.026|α| + 4·10⁻⁹ α⁴)) foot-candles
    × 10.764 lux/fc × (384,400 km / d)², α = phase angle in degrees. Full moon ≈ 0.31 lux TOA.
  - The moon's direction is converted from local ENU at the camera to UE world through the Cesium georeference
    (never `SetActorRotation` with az/el: UE axes are ENU only at the origin).
- **Sun direction:** CesiumSunSky keeps positioning its light (`SolarTime`/date, `UpdateSun()`).
  `FEphemeris`'s sun is used for the dome's moon shading and tests; a test holds the two within 0.1°.
- **Night floor (starlight + airglow + zodiacal):** a uniform sky radiance in the dome,
  L = `night_floor_lux` / π cd/m² (E = πL for a uniform hemisphere), default 0.002 lux. The real-time sky light
  captures it and lights the ground.

### Sky dome (`M_SkyDome`, generated)

One camera-following sphere (engine sphere mesh, inward-facing, huge radius), material Unlit / Opaque / Is Sky,
generated by `scripts/sky/make_sky_material.py` (+ `.sh`, same pattern as `scripts/ocean/make_ocean_material.py`;
never hand-edit). Emissive, in cd/m²:

```
SkyAtmosphereViewLuminance
+ SkyAtmosphereLightDiskLuminance(0)                      (sun disc, engine node)
+ MoonDisc(dir, phase)                                    (index 1; replaces the engine's uniform disc)
+ StarTexture(R_sidereal · dir) · StarScale · T(view dir)
+ FloorRadiance · T(view dir)
```

T(view dir) = 10^(−0.4 · 0.172 · X(el)), X = Kasten–Young air mass of the view elevation, computed in the material
(no engine node gives transmittance along an arbitrary direction); 0 below the horizon.

- **Moon disc:** inside the moon's angular radius, a Lambert-lit sphere lit from the ephemeris sun direction. Its mean
  over the disc is normalised so the disc's integrated luminance × solid angle equals the transmitted moon
  illuminance (`SkyAtmosphereLightIlluminance(1)`); the engine's own index-1 disc is zeroed
  (`AtmosphereSunDiskColorScale` black) so nothing is counted twice. Earthshine is ignored.
- **Stars:** the Yale Bright Star Catalogue (BSC5, public domain, ~9,100 stars to V ≈ 6.5) ships as
  `Content/NonUFS/Sky/bsc5.dat` (git LFS). At startup `FStarField` bins it into an equirectangular RGBA16F texture in
  the equatorial frame (4096 × 2048, created at runtime as a transient `UTexture2D`, no editor asset): each star's
  illuminance E = 10^(−0.4 (V + 13.98)) lux goes into its texel as radiance E / Ω_texel, coloured from B−V. The
  material rotates the view direction by local sidereal time and latitude (`MPC_Sky`). Limitation: a star is one
  texel (≈ 5′), so in a narrow FOV it shows as a small square; acceptable for a sky rarely in view (noted in docs).
- **Parameters** come through `MPC_Sky` (generated with the material): moon direction, sun direction (for shading),
  moon illuminance normaliser, sidereal rotation (3 × 3), star scale, floor radiance, enables.
- If `bsc5.dat` is missing or an LFS pointer: one warning, the star term is 0. Not an error under `scene.offline`.

### Cached-lighting exposure bands

Lumen's cached lighting and the sky capture store luminance × 2^−c in R11G11B10 (c =
`r.EyeAdaptation.CachedLightingPreExposure`). With c = 8, night luminance (10⁻²–10⁻⁴ cd/m²) is stored at
~10⁻⁴–10⁻⁶ and underflows, so sky-lit ground goes black. (The view exposure itself stays in UE's "safe range": the EO
AE is at its gain cap at night. So the trigger is the scene's luminance, not the view EV.)

`FCachedExposureBand` (pure) picks c from the EO AE's scene median (log2 cd/m², `FSensorController::GetLastMedianLog2`):
day band `day_ev` (8) and night band `night_ev` (−6), switching to night when the median falls below
`switch_median_log2` (0, ≈ 1 cd/m², late civil twilight) and back to day above `switch_median_log2 + hysteresis_ev`
(2). Set by the game thread through `IConsoleVariable::Set` (read on the render thread). While the sensor is in IR the
band holds (the histogram is radiance there). A switch resets Lumen's radiance cache; acceptance N5 measures the
transient.

### Radiometric EO exposure (ROADMAP 9.4)

The shader and CPU reference stay as they are (e = Signal · PhotonGain · FullWellE). What changes is what
`PhotonGain` means for a photon detector with radiometric optics:

```
R  = (π / (4 N²)) · τ · A_pix · QE · photons_per_lm         [electrons / s per cd/m²]
PhotonGain = R · t_int / FullWellE
```

- N = `optics.f_number`, A_pix = `optics.pixel_pitch_um`², τ = `optics.transmission` (new, 0.9),
  QE = `detector.qe` (new, 0.6), `photons_per_lm` = band photons per lumen for a 5,800 K source over
  [`band_lo_um`, `band_hi_um`], computed at config load (closed-form blackbody integral; ≈ 1.2·10¹⁶ for 0.4–0.7 µm).
- The AE keeps choosing a total gain EV. The photon stage is now integration time:
  `max_photon_gain_ev = log2(R · t_max / FullWellE)`, `t_max = detector.integration_max_ms` (new, default the frame
  period, 33.3 ms), and `min_gain_ev = log2(R · t_min / FullWellE)`, `t_min = detector.integration_min_us` (new, 10 µs).
  Past t_max the AE adds analog gain as before. For `eo_hd_cmos` this gives max ≈ −6.8 EV against today's calibrated
  −12.5: EO at dusk and night integrates up to ~5.7 EV longer before analog gain (intended; daylight, where the AE is
  below the cap, is unchanged by construction: same target, same electrons at the median).
- Dark signal integrates over t_int, not the frame period (the 3B.3 note in `SensorController.cpp`).
- Telemetry: the integration time is logged with the AE state.
- Applies when the mode's detector is `Photon` and its preset (or config) sets `transmission` and `qe`: the EO preset
  does. Thermal IR keeps `thermal_exposure` (calibrated gain): 9.4's IR part stays in Milestone 9. Setting
  `exposure.min_gain_ev` / `max_photon_gain_ev` on a radiometric mode logs one warning and is ignored.

### Ephemeris and illumination (pure, game thread)

- `FEphemeris::Compute(FDateTime Utc, double LatDeg, double LonDeg, double HeightM)` → sun and moon topocentric
  az/el (refraction-free; refraction added as a separate term for elevation > −1°), moon phase angle and illuminated
  fraction, sun and moon distance, local apparent sidereal time. Low-precision Meeus (*Astronomical Algorithms*,
  ch. 25 sun, ch. 47 moon truncated to the main ~60 terms, ch. 48 phase, ch. 12 sidereal), ΔT from the
  Espenak–Meeus polynomial. Accuracy target: moon 0.05°, sun 0.01°.
- `FNightIllumination::Compute(Ephem, Cloud, Visibility)` → TOA sun and moon illuminance, the floor radiance, light
  colours, and (for tests and the log line) the expected horizontal illuminance at the ground with Kasten–Young air
  mass and V-band extinction k = 0.172 (K&S). Cloud cover from CIGI Weather Control scales the floor and moon by the
  existing `FThermalModel::CloudFactor`-style factor (clouds themselves are rendered by VolumetricCloud and lit by both
  lights).
- `ACamSimEnvironment::ComputeSunPosition` stays for the thermal model (`FThermalModel::SunElevationDeg`) in this
  slice: moving thermal onto `FEphemeris` changes its calibration and is a follow-up (ROADMAP open item).

### Environment actor

- `ACamSimEnvironment` drives CesiumSunSky's `DirectionalLight` (intensity, `TransmittanceMinLightElevationAngle`),
  its `SkyLight` (real-time capture stays on; overcast dimming applies to it) and the level's `VolumetricCloud`;
  owns the moon light and the dome. Lookups for standalone light / sky light / atmosphere actors are removed; with no
  CesiumSunSky (test levels) it spawns a plain directional light as today's fallback does.
- The fog path stays but is recorded as dead with Main.umap (no `ExponentialHeightFog`): CIGI visibility currently
  does nothing; carried to ROADMAP 8.6.

### CIGI and config

- Celestial Sphere Control: Sun / Moon / Star Field Enable zero that source (light and disc); Star Field Intensity
  scales stars (1 = physical); Ephemeris Model Enable keeps driving the clock's rate; date/time as today.
- Until the first Celestial Sphere Control packet, `environment.celestial.stars_default` (true) decides stars (CIGI's
  own default is off, and a trainer without stars at night is wrong); after it, the host decides.
- New keys (all in `docs/configuration.md`, env overrides only where marked):
  - `environment.celestial.moon_enabled` (true), `stars_default` (true), `night_floor_lux` (0.002,
    `CAMSIM_NIGHT_FLOOR_LUX`), `star_catalog` (path, default the shipped BSC5)
  - `environment.celestial.cached_exposure.{day_ev, night_ev, switch_median_log2, hysteresis_ev}` (8, −6, 0, 2)
  - `sensor_modes.<mode>.optics.transmission`, `detector.{qe, integration_max_ms, integration_min_us}`

## Testing

Unit (NullRHI):
- `CamSim.Environment.Ephemeris.*`: moon az/el and illuminated fraction vs JPL Horizons at ≥ 10 dates/sites
  (incl. polar and a lunar-standstill date) within 0.2° / 0.01; sun vs NOAA within 0.05°; sidereal time vs the USNO
  formula; ΔT.
- `CamSim.Environment.Illumination.*`: full moon at zenith ≈ 0.25–0.3 lux at the ground, quarter ≈ 0.03, new moon ≈ 0,
  floor continuity, sun below −18° → 0.
- `CamSim.Environment.CachedExposure.*`: band choice, hysteresis, no oscillation at the edge.
- `CamSim.Environment.StarField.*`: binning conserves total illuminance; Sirius lands in the right texel; a missing
  file yields an empty field and one warning.
- `CamSim.Sensor.Radiometric.*`: R from the preset by hand; shot noise = √e at known luminance; AE fills t_int before
  analog gain; dark signal scales with t_int; legacy keys ignored with a warning. `CamSim.Sensor.Physics.*` (16)
  keep passing.
- `CamSim.CIGI.Celestial.*`: enables, star intensity, `stars_default` hand-over.

- `CamSim.Environment.MoonDisc.*`: the disc shading normaliser (computed on the CPU, passed through `MPC_Sky`)
  makes the numerically integrated disc luminance × solid angle equal the input illuminance within 1 % at phase
  angles 0°, 45°, 90°, 135°; the lit limb faces the sun.

GPU: `CamSim.GPU.Sensor.*` unchanged (shader unchanged) and must still pass.

Acceptance `scripts/night_check.py --scene PKG --out DIR [--runs ...]` (Pendleton package, Linux RTX 5080; the M1 Pro
run is recorded as open if not done):

| Gate | Check |
| ---- | ----- |
| N1 | Daytime unchanged: bench day shots (AE below its cap) before/after, EO mean luma within ±3 DN, temporal noise within 10 % |
| N2 | Full moon high vs new moon, same pose: the AE's scene median (log2) differs by log2 of the predicted ground illuminance ratio ±0.5 EV; temporal noise of Y on a flat patch within ±20 % of `CamSimSensorRef::Run` on a flat field at the logged median and gains |
| N3 | Moon in view: disc centroid within 2 px of the ephemeris projection; lit limb toward the sun |
| N4 | Stars at new moon visible (≥ 20 detections at the expected positions within 2 px); none by day |
| N5 | Dusk band switch: consecutive-frame mean luma step ≤ 5 DN through the switch, settled ≤ 30 frames |
| N6 | `thermal_check.py` gates a–l pass |
| N7 | Night cost vs today's night bench: game thread and GPU within +0.3 ms |
| N8 | Sunset: sun light at the ground falls smoothly to 0 by sun elevation −1°; frame mean through civil twilight monotonic |

## Docs

`docs/night.md` (new guide), `docs/configuration.md`, `ROADMAP.md` (6A entry, 9.4 moved, open items),
`CLAUDE.md` gotchas (cached-lighting band, IsSky dome replaces the atmosphere in the capture, moon at atmosphere
index 1, k = 1).

## Out of scope

NVG/I² (6B), light points and MegaLights (6C), planets, the Milky Way as an extended source, earthshine,
moon-phase opposition surge beyond K&S, thermal on `FEphemeris`, radiometric IR (Milestone 9), fog actor in Main.umap.
