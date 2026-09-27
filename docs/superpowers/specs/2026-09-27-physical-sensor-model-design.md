# Physical Sensor Model — Design Spec (ROADMAP 3B.2, revises 3B.2–3B.4)

**Date:** 2026-09-27
**Status:** Design approved section by section 2026-09-27; awaiting written-spec review
**Revises:** `2026-09-27-gpu-sensor-model-design.md`. That spec's 3B.1 (pipeline: HDR input,
sensor AE/AGC, response curve, NV12) is built and stays. Its 3B.2–3B.4 plan — port every legacy
CPU effect, then delete the legacy path — is replaced by this document.
**Platform:** macOS (M1 Pro, Metal) verified; nothing Metal-specific: plain compute, integer
atomics only, no float atomics, no wave intrinsics (Linux/Vulkan follows).

## Why the change

The user confirmed the legacy CPU effects were notional and unused, so there is nothing to stay
compatible with. They were also not worth porting: in the fused path that actually ran, NETD noise
was 510× smaller than intended (effectively none), dither was ignored, atmospheric fade could
overshoot and invert, and every effect ran on 8-bit tone-mapped display values, where noise and
blur are not physically meaningful.

## Goals

- **Purpose: ATR/ML training data.** Image statistics must follow the physics of a real camera,
  with parameters taken from datasheets, so models trained on CamSim imagery transfer. Output is
  deterministic and seedable.
- **Sensor types: EO and IR (LWIR) only.** NVG is removed.
- **One path.** The GPU sensor model is the only sensor path; the legacy CPU model and everything
  that only it needed are deleted (this was 3B.4's job, pulled forward).
- **No burned-in overlays.** The HUD, laser spot and precipitation overlay are deleted; the stream
  is the sensor image and the telemetry travels in the KLV.

## Model and data flow

```
HDR scene-linear RGB (÷ View.OneOverPreExposure, as in 3B.1)
  │
  ├─► Stats: histogram of the noiseless signal ──► CPU AE/AGC (3B.1 controller)
  ▼  1. OPTICS (scene-linear, spatial)
     distortion resample → relative illumination (cos^n) → PSF blur (separable)
  ▼  2. EXPOSURE → ELECTRONS / SIGNAL
     e = signal × photon_gain × full_well_e ; clip at full well
  ▼  3. DETECTOR (per pixel; deterministic hash noise)
  ▼  4. ADC  (N-bit)
  ▼  5. DISPLAY (3B.1): EO knee + BT.709 OETF; IR AGC stretch + polarity → NV12
```

- The histogram measures the noiseless scene signal, so AE/AGC never chases noise.
- EO runs the detector per RGB channel (a 3-chip / ideal-demosaic approximation; no Bayer
  demosaicing). IR is monochrome: `signal = dot(RGB, signal_weights)` as in 3B.1, the
  visible-light proxy until Milestone 4 supplies radiance.
- **Randomness:** every random field is a PCG hash of (pixel x, pixel y, frame index, seed,
  stream id) turned into a unit Gaussian (Box-Muller from two hashed uniforms). Temporal fields
  (shot, read, temporal noise) include the frame index; fixed-pattern fields (PRNU, DSNU,
  pixel/column/row FPN, defects) do not. The same seed and frame index give the same fields on
  the GPU and in the CPU reference. No noise textures are uploaded.

### 1. Optics

- **Lens distortion (Brown-Conrady radial `k1`, `k2`).** Each output (distorted) pixel is mapped
  to its ideal pinhole position by inverting `r_d = r_u (1 + k1 r_u² + k2 r_u⁴)` with 3 Newton
  iterations; the scene is sampled bilinearly there. Coordinates are normalised by the focal
  length in pixels derived from HFOV (OpenCV convention), so calibration values drop in directly.
  Samples outside the rendered image read black (UE renders a pinhole view at the sensor FOV; an
  overscan margin is future work). Default k1 = k2 = 0 (off). If Newton fails to converge the
  pixel reads black and a warning is logged once.
- **Relative illumination.** `E(θ) = cos^n θ`, θ the pixel's field angle from HFOV/VFOV;
  `vignetting_exponent` n defaults to 4 (natural falloff), 0 disables it.
- **PSF.** A Gaussian whose σ (pixels) combines in quadrature: diffraction
  `0.42·λ·N / pitch` (from `wavelength_um`, `f_number`, `pixel_pitch_um`), pixel aperture
  `0.29 px`, and `extra_blur_px` (defocus/turbulence/jitter, default 0). Separable horizontal and
  vertical passes, kernel radius `ceil(3σ)` capped at 8 px, groupshared tiles.
- Not included: chromatic aberration, spectral PSF, atmospheric attenuation (UE's SkyAtmosphere
  renders visible haze; IR transmission belongs to Milestone 4).

### 2. Exposure split

The AE/AGC output gain is split as a real camera spends it:

- **Photon gain** (integration time/aperture), capped at `max_photon_gain_ev`, sets the collected
  signal: `e = signal × photon_gain × full_well_e` (EO), clipped at `full_well_e`.
- **Analog gain**, up to `max_analog_gain_db`, is applied after the detector noise.

At night AE exhausts photon gain first, then adds analog gain: the image brightens but gets
noisy, as real cameras do. `FSensorFrameParams` carries both gains.

### 3. Detector

**CMOS (EO, per channel):**

```
e1 = e × (1 + prnu·n1)                      fixed
e2 = e1 + sqrt(max(e1,0))·n2                temporal (shot)
e3 = e2 + dark_current_e_s·t_int + dsnu_e·n3    n3 fixed
e4 = e3 + read_noise_e·n4                   temporal
e5 = clamp(e4, 0, full_well_e) × analog_gain
```

**Microbolometer (IR, mono; fractions of full scale until Milestone 4 supplies temperatures):**

```
v = signal_norm + temporal_noise·n2 + pixel_fpn·n1 + column_fpn·nc(x) + row_fpn·nr(y)
```

No shot noise (thermal detector). Column/row FPN is the characteristic microbolometer striping.
`signal_norm = signal × photon_gain`, where the controller computes the IR photon gain the same way
as EO's (median to `target_grey` of full scale, so the 14-bit range is used). The IR AGC stretch
(3B.1 percentiles) then runs in the display stage on `DN / (2^adc_bits − 1)`, with its band taken
from the noiseless histogram scaled by that photon gain.

**Defects (both):** per pixel, a hash against `hot_pixel_fraction` / `dead_pixel_fraction`: hot
reads full scale, dead reads 0.

### 4. ADC

`DN = round(e5 × (2^adc_bits − 1) / full_well_e)` (CMOS) or `round(v × (2^adc_bits − 1))`
(microbolometer), clamped to `[0, 2^adc_bits − 1]`, using `floor(x + 0.5)`. The display stage
(3B.1) takes `DN / (2^adc_bits − 1)` as its linear input.

## Presets and configuration

```yaml
sensor_modes:
  eo: {preset: eo_cmos, seed: 1, optics: {...}, detector: {...}, exposure: {...}}
  ir: {preset: lwir_microbolometer, seed: 1, optics: {...}, detector: {...}, exposure: {...}}
```

A preset supplies defaults; any key overrides them.

| Parameter | `eo_cmos` (2/3" industrial CMOS) | `lwir_microbolometer` (640×512 VOx) |
|---|---|---|
| full_well_e / read_noise_e | 10,000 / 2.5 | — |
| prnu / dsnu_e / dark_current_e_s | 0.01 / 1 / 5 | — |
| temporal / pixel / column / row noise | — | 0.004 / 0.003 / 0.0015 / 0.001 |
| adc_bits | 12 | 14 |
| max_analog_gain_db | 24 | — (AGC) |
| f_number / pixel_pitch_um / wavelength_um | 4 / 3.45 / 0.55 | 1.2 / 12 / 10 |
| hot / dead pixel fraction | 1e-5 / 1e-5 | 1e-4 / 1e-4 |
| vignetting_exponent / extra_blur_px / k1 / k2 | 4 / 0 / 0 / 0 | 4 / 0 / 0 / 0 |

The `exposure:` block and IR AGC keys from 3B.1 stay; `max_photon_gain_ev` joins them. Presets
are typical datasheet values; the display targets are calibrated on the bench.

**Validation errors:** unknown preset; `full_well_e ≤ 0`; `adc_bits` outside 8–16; negative noise
parameters or f-number; defect fractions outside [0, 0.01]; `|k1|` or `|k2|` > 1. Removed effect
keys and `sensor_modes.nvg` produce the standard unknown-key warning.

## Removals

- NVG: `ESensorMode::NVG`; CIGI Sensor ID 2+ maps to EO with a one-time warning; KLV Tag 11 is
  "EO" or "IR"; the encoder's grey-frame path.
- The CPU sensor model (`Sensor/SensorPostProcess.*`, `IPixelPipeline`) and its tests.
- The legacy path, `render.sensor_path`, `FSensorPathSelector`, the backbuffer grab.
- `render.view_source: scene_capture` and the SceneCapture capture path.
- The material path (`performance.gpu_sensor_*`).
- The HUD overlay (`Overlay/`), the laser-spot drawing, the precipitation overlay (the DIS laser
  designator's telemetry and UE's Niagara weather stay).
- Every legacy effect key (noise, FPN, vignetting, scan lines, blur, AC banding, drift, jitter,
  sun glint, colour temperature, contrast, quality presets, …).
- With no CPU fallback, a machine without SM5 compute gets the existing startup error (`/ready`
  false, reason logged).

## Testing

1. **Physics tests on the CPU reference** (NullRHI, CI):
   - photon transfer (EMVA 1288 style): flat fields at 8 levels, two frames each; temporal variance
     `e + read_noise²` within 5%; fixed-pattern variance `(prnu·e)² + dsnu²` within 5%;
   - MTF: step edge; fitted edge-spread σ equals the configured PSF σ within 5%;
   - distortion: grid points match the forward Brown-Conrady model within 0.1 px;
   - vignetting: flat field falloff equals cos^n θ within 1%;
   - determinism: same seed + frame bit-identical; consecutive-frame temporal correlation < 0.05;
     fixed patterns identical across frames; a new seed changes them;
   - microbolometer: std of column means = `column_fpn` within 10% (rows likewise); no signal
     dependence;
   - defects: counts match fractions within binomial tolerance;
   - exposure split: photon gain exhausted before analog gain; SNR falls as analog gain rises.
2. **GPU vs reference** on Metal: Y ≤ 1 DN, UV ≤ 2 DN; covering partial thread groups (e.g.
   1080p), scene + bloom, NaN input, view-rect clamping.
3. **Live:** full bench (720p/1080p), `ci_validate --native`, the Yosemite snap test, a new EO/IR
   shot set.

Also carried from the 3B.1 reviews: a NaN test that survives fast-math (bit test), `floor(x+0.5)`
rounding, clamping bilinear scene and bloom UVs to their view rects.

## Exit criteria (3B.2, macOS M1 Pro)

1. Legacy removed; the default config runs the GPU sensor model with every stage on.
2. Physics tests pass; GPU matches the reference on Metal.
3. 30 fps, 0 dropped frames in every bench phase at 720p and 1080p; frame p95 ≤ the
   post-crossfade baseline + 1 ms.
4. Sensor graph GPU p95 ≤ 2 ms at 1080p (the spec's 4 ms stays the RTX 5090 check in 3D).
5. Daylight EO mean luma 90–170 with < 1% clipped; night EO darker than day with measurably higher
   temporal noise; IR column striping present and < 2 DN; camera cuts converge in 1–3 frames.
6. `ci_validate --native` passes; Yosemite snaps still 0% coarse.
7. Visual review by the user of the new EO/IR shot set.
8. Docs: ROADMAP, `docs/configuration.md` (presets, parameters), CLAUDE.md.

## Later

- **3B.3:** rolling shutter and platform jitter; the LWIR NUC/drift cycle; Bayer demosaicing if EO
  colour-noise realism turns out to matter.
- **3C** (GPU-texture encode) and **3D** (CI performance gate) unchanged.
- **Milestone 4:** thermal radiance feeds the microbolometer detector; its noise moves to NETD in
  kelvin.
