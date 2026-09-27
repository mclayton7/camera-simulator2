# Sensor Model on the GPU — Design Spec (ROADMAP 3B)

**Date:** 2026-09-27
**Status:** Design approved section by section 2026-09-27; awaiting written-spec review
**Scope:** Second sub-project of Milestone 3. Replace UE's tonemapper with an RDG compute sensor
model on HDR scene-linear input, own exposure with a sensor AE/AGC loop, output NV12, port every
existing sensor effect, and retire the CPU pipeline, the SceneCapture path and the material path.
GPU-texture encode (3C), the CI performance gate (3D) and thermal radiance (Milestone 4) are out
of scope.
**Platform:** macOS (Apple M1 Pro, Metal) only, as in 3A. Linux/NVIDIA (RTX 5090) runs are
deferred; nothing here may be Metal-specific: plain compute only, integer atomics only, no float
atomics, no wave intrinsics.

## Goals

3B is judged on **performance and fidelity together**:

- **Performance:** the sensor model moves off the CPU. 1080p30 holds with every effect enabled, and
  the per-frame CPU cost of the sensor falls to a small controller update. (Today the CPU model has
  ~33 ms per frame and frames are skipped when it falls behind.)
- **Fidelity:** EO, IR and NVG are exposed from HDR radiance by a sensor auto-exposure loop with
  physical limits. This fixes "night renders as daylight" (ROADMAP 2.1 open item) and gives
  Milestone 4's thermal pass a detector model to plug into.
- **One model:** the GPU implementation matches a readable CPU reference implementation in tests;
  the legacy pipeline is deleted.

### Change from the 3A spec

The 3A spec recorded that "UE post-processing (exposure, bloom, motion blur, DOF, flare) still
runs". Exposure is now owned by the sensor (decision 2026-09-27): UE runs with a fixed manual
exposure in the sensor path and its eye adaptation is off. Bloom, DOF and motion blur still run in
UE; the sensor model owns everything from the tonemapper on.

## Current state (2026-09-27)

- Both view sources read back the final **8-bit tone-mapped BGRA** image into `TArray<FColor>`:
  the primary view via `FCamSimFrameGrabExtension::PostRenderViewFamily_RenderThread` (a copy of
  the backbuffer), and `view_source: scene_capture` via `SceneCapture2D`.
- `Sensor/SensorPostProcess.cpp` (~2,030 lines, fused and unfused variants) runs every effect on a
  background task, 8 row bands. State across frames: AGC smoothing, thermal drift and NUC timing,
  previous frame (rolling shutter). Built once: FPN map, noise ring, defect list, vignette, LUTs,
  distortion remap.
- `IFrameSink::EncodeFrame` takes BGRA; `FVideoEncoder` converts with `sws_scale` to YUV420P
  (IR/NVG take Y from the R channel).
- The material path (`performance.gpu_sensor_effects`, `M_SensorPostProcess`) is dead: the
  material asset does not exist, so it always falls back to the CPU.
- The project has no shaders and its only module loads at `Default`, too late for global shaders.
- The bench measures no sensor timing, and its snapshots are taken before the sensor.

## Architecture and data flow

```
Game thread                         Render thread (RDG, inside ReplacingTonemapper)
───────────                         ────────────────────────────────────────────────
FSensorController ──params──► ENQUEUE ─► FSensorFrameParams (gain, offset, mode, polarity,
 (AE/AGC, NUC, seeds)                        seeds, frame index, effect toggles)
       ▲                                         │
       │ histogram                   HDR SceneColor ÷ PreExposure, depth, bloom
       │                                         ▼
       │                            1. Stats:     log2 histogram (uint atomics, 256 bins)
       │                            2. PerPixel:  waveband → gain/offset → noise/FPN/defects/vignette
       │                            3. Spatial:   MTF, distortion, vibration, rolling shutter
       │                            4. Output:    response curve → quantize/dither → NV12 (packed
       │                                          uint buffer) + 8-bit RGB returned to the viewport
       │                                         │ (NV12 only if a ring slot is free)
ReadbackRing ◄── FRHIGPUBufferReadback (NV12)    │
Stats ring   ◄── FRHIGPUBufferReadback (histogram, every frame)
       │
Encoder thread: NV12 → FFmpeg (no sws_scale) → MPEG-TS + KLV
```

### Hook point

UE 5.8's `ISceneViewExtension::EPostProcessingPass::ReplacingTonemapper`
(`Renderer/Private/PostProcess/PostProcessing.cpp`, ~line 1600). The highest-priority delegate
replaces `AddTonemapPass` and receives `FPostProcessMaterialInputs` with:

- `SceneColor`: HDR, output resolution, after TSR, DOF and motion blur, pre-exposed by
  `View.PreExposure`;
- `CombinedBloom`: UE's bloom (plus lens flare if enabled);
- `SceneTextures` (depth → per-pixel slant range for atmospheric effects) and custom
  depth/stencil (Milestone 4 class IDs);
- `OverrideOutput` when the tonemapper is the last pass.

Its return value is what the rest of the chain and the viewport see.

### Units

| Unit | Thread | Responsibility | Depends on |
| --- | --- | --- | --- |
| `CamSimShaders` module (new, `LoadingPhase: PostConfigInit`, same project) | — | `.usf` files in `unreal_project/CamSimTest/Shaders/`, shader directory mapping `/CamSim`, `FGlobalShader` classes. No game logic. | RenderCore, RHI, Projects |
| `FSensorGraph` | Render | `AddSensorPasses(GraphBuilder, FSensorGraphInputs, FSensorFrameParams) → {DisplayTexture, NV12Buffer, HistogramBuffer}`. Pure function of its inputs. | `CamSimShaders` |
| `FSensorController` | Game | Histogram → next-frame gain/offset (AE/AGC with lag), drift/NUC timing on sim time, per-frame seeds, mode-switch and camera-cut snaps. Produces `FSensorFrameParams`. Plain C++, no RHI. | config, `FSimClock` |
| `FSensorPathSelector` | Game | Chooses `Gpu` or `Legacy` from the enabled effects and the GPU-supported set; logs the reason once. | config |
| `FCamSimFrameGrabExtension` | Render | Subscribes to `ReplacingTonemapper` when the path is `Gpu`; runs the sensor graph every frame; enqueues the NV12 readback when a ring slot is free and the stats readback always. | `FSensorGraph` |
| `FSensorReference` | any | Scalar float C++ implementation of the same model, for tests. | — |

### Exposure in UE

In the sensor path the view's exposure is fixed (manual, constant), so `PreExposure` is stable. The
graph divides it out, so gain is applied to absolute scene-linear values.
`render.exposure_compensation_ev` keeps its meaning as a bias on the EO AE target.

### NV12 layout

Metal's support for NV12 UAVs is patchy, so the output pass writes a `RWBuffer<uint>`: the Y
plane (W×H bytes) followed by the interleaved UV plane (W×H/2 bytes), four bytes per `uint`. This
needs capture width divisible by 4 and even height (config validation). One
`FRHIGPUBufferReadback` moves 1.5 bytes/px (~3.1 MB at 1080p, instead of ~8.3 MB of BGRA). EO
chroma uses BT.709 limited range; IR and NVG write U = V = 128. NVG's green tint is a display
matter: the stream carries luma only, as today (the viewport copy is tinted).

The readback ring's slots hold NV12 bytes (`TArray<uint8>`) in place of `TArray<FColor>`. The
histogram (256 × uint32) has its own small ring, read every frame, so AE never stalls when the
encoder is busy.

### Encoder interface

`IFrameSink::EncodeFrame` takes an `FSensorFrame`: pixel format (`NV12`, or `BGRA8` for the legacy
path until 3B.4), plane pointers and strides, telemetry, frame index.

- NV12 goes to libx264, VideoToolbox and NVENC without `sws_scale` (all accept NV12).
- `FMultiViewFrameSink` digital zoom: `sws_scale` NV12→NV12 crop-and-scale, only for a zoomed view.
- `/snapshot?stage=sensor` returns a PNG converted from NV12 on the CPU (snapshots only). The
  existing pre-sensor snapshot stays as `stage=scene` (default): in the GPU path the output pass
  also writes, only while a snapshot is pending, the HDR scene colour with the current EO exposure
  and the sRGB curve and no sensor effects, so 3A-style render-path shots remain possible.

### Legacy path during 3B.1–3B.3

When the config enables an unported effect, the extension does not subscribe to
`ReplacingTonemapper` for the session: UE's tonemapper and eye adaptation run, and the backbuffer
grab and CPU model are unchanged. The choice is made at startup and on hot-reload and switches at a
frame boundary; a frame is never split between paths.

## AE/AGC control loop

One loop drives every mode: the GPU measures, the CPU decides gain and offset for the next frame.

### Measure (GPU, every frame)

The stats pass builds a 256-bin histogram of **log2(detector signal)** over a fixed 32-stop range
in absolute scene-linear units. Detector signal per mode:

- **EO:** linear luminance Y (BT.709 weights).
- **IR:** the same luminance until Milestone 4 supplies radiance; then radiance. Only the measured
  quantity changes, not the loop.
- **NVG:** luminance weighted toward red/near-IR (weights in config, default R-heavy).

Integer atomics only. Inputs are sanitized first (see Error handling).

### Decide (CPU, `FSensorController`)

A pure function of (histogram, config, sim-time delta, controller state):

- **EO and NVG auto-exposure:** choose the exposure that puts the histogram median at mid-grey
  (`target_grey`, default 0.18, biased by `render.exposure_compensation_ev` for EO), capped so the
  `highlight_percentile` (default 0.99) does not clip. Clamp to `[min_ev, max_ev]`: the camera's
  physical limits (shortest integration, highest gain). At night the loop saturates at `max_ev` and
  the image goes dark. NVG has a much higher `max_ev` (intensifier).
- **IR AGC:** percentile stretch with the existing `agc_low_percentile` / `agc_high_percentile`,
  giving gain and offset that map the band to [0, 1]. Manual mode stays
  (`agc_manual_level` / `agc_manual_gain`). Polarity applies after AGC.
- **Lag:** `lag_frames` becomes a time constant in sim time (τ = `lag_frames` × 1/30 s), applied
  in log space, so convergence is frame-rate independent. The existing `agc_lag_frames` maps onto it for
  IR.
- **Snaps:** the first frame, a camera cut (the 3A camera-cut signal that resets TSR history) and a
  sensor mode switch go straight to the target.
- **Drift and NUC:** drift accumulates on `FSimClock` sim time; the NUC interval resets it.
- **Seeds:** noise, jitter, banding phase, vibration and precipitation from the frame index, as
  today. Random fields are generated on the GPU from an integer hash; nothing is uploaded per frame.

### Apply (GPU, next frame)

`signal × gain + offset` → effects → **response curve** (EO: BT.709 OETF with a soft highlight
knee; IR/NVG: linear, then polarity) → quantize to 8-bit. Frame N uses the newest histogram
available when frame N is set up, typically 1–3 frames old (the readback depth), which is how a
real camera's AE behaves.

### Config

Per mode, new: `exposure: {auto: true, min_ev, max_ev, target_grey: 0.18,
highlight_percentile: 0.99, lag_frames}`. IR AGC keys unchanged. CIGI Sensor Control Gain/Level
are out of scope (Gain still selects an FOV preset).

## Effect staging

Every effect in today's CPU model is ported. Each stage is shippable; a config that needs an effect
not yet ported runs the legacy path.

| Stage | Contents |
| --- | --- |
| **3B.1 Pipeline** | `CamSimShaders` module; `ReplacingTonemapper` hook and HDR input; stats pass, `FSensorController`, sensor AE for EO/IR/NVG; waveband and response curve; NV12 output and NV12 encoder input; stats ring; `FSensorPathSelector` and `sensor_path` reporting; bench sensor timing and post-sensor shots. |
| **3B.2 Core effects** | Noise (NETD), FPN, defect pixels, vignette, MTF (Gaussian) and box blur, lens distortion, quantization with Bayer dither, atmospheric attenuation / IR extinction (from scene depth). `FSensorReference` grows alongside; GPU-vs-reference tests per effect. |
| **3B.3 Remaining effects** | Gain/offset jitter, thermal drift/NUC, AC banding, scan lines, colour temperature, contrast/brightness, sun glint, vibration, rolling shutter (previous-frame texture kept in the graph), laser designator, NVG IR pointer, precipitation, dynamic IR extinction, HUD overlay (bitmap font atlas as a texture), chromatic aberration (UE 5.8 applies `SceneFringeIntensity` inside the tonemapper, which the graph replaces). |
| **3B.4 Retirement** | Delete `SensorPostProcess.{h,cpp}` (fused and unfused), the backbuffer-grab path, `view_source: scene_capture` and the `SceneCapture2D` capture, `performance.gpu_sensor_*` and `SetGpuSensorEffectsActive`, the BGRA encoder input; rename the capture holder. Update ROADMAP (including Milestone 3 item 5's stale readback-ring text). |

## Error handling

- **Path selection:** `FSensorPathSelector` logs once, e.g. "sensor path: legacy (unported:
  rolling_shutter, laser_designator)". `/health` gains `sensor_path`; `/metrics` gains
  `camsim_sensor_path{path="gpu"} 1`.
- **GPU path cannot run** (missing shader map, RHI without compute): until 3B.4, fall back to
  `Legacy` with an error log. After 3B.4, a startup error: `/ready` stays false and the reason is
  logged. Streaming tonemapped video as sensor output would be silently wrong.
- **Config validation errors:** capture width not divisible by 4 or odd height; `min_ev > max_ev`;
  percentiles outside [0, 1] or low ≥ high.
- **Bad input pixels:** NaN, Inf and negative HDR values are clamped to [0, 65504] (NaN → 0)
  before the stats and gain; log2(0) lands in bin 0.
- **Stale histogram:** if no new histogram arrives within 10 frames (readback failure/timeout,
  already counted by the ring), the controller holds its last parameters and increments
  `sensor_stats_stale` in `frame_drops`. It never resets to defaults.
- **View size ≠ capture size:** the output pass resamples bilinearly to capture size; the 3A
  "stretched grab" warning is logged once.
- **Threading:** controller state is game-thread only; `FSensorFrameParams` is copied by value into
  `ENQUEUE_RENDER_COMMAND` with the grab request; histograms return through in-order delivery in
  `Poll()`. No shared mutable state beyond the existing ring atomics.

## Reference model

`Sensor/SensorReference.{h,cpp}`: single-threaded, scalar, float C++ implementation of the model.
Input: HDR float pixels, depth plane, `FSensorFrameParams`. Output: NV12 bytes and histogram.
Written for readability, not speed; it is the specification in code, used only by tests after 3B.4.

Shared with the shader by mirroring, not by shared source:

- `FSensorFrameParams` and the HLSL constant-buffer struct match; a `static_assert` checks size.
- Noise, FPN and defects use the same integer hash (PCG) in both, so random fields are
  bit-identical; only float rounding differs.
- The order of operations is documented once, in a comment block both files reference.

## Testing

1. **Controller** (NullRHI, CI), `CamSim.Sensor.Controller.*`: median at mid-grey after
   convergence; night histogram clamps at `max_ev` (output mean below a darkness threshold);
   highlight cap prevents clipping; lag reaches 63% at its time constant at both 30 and 60 Hz;
   camera cut and mode switch snap; IR percentile stretch and manual mode; drift/NUC on sim time;
   stale-histogram hold.
2. **Reference model** (NullRHI, CI), `CamSim.Sensor.Reference.*`: the meaningful existing sensor
   tests rewritten against the reference (EO passthrough, IR grayscale/polarity, NVG, vignette
   symmetry, noise determinism, AGC, quantization, defects, blur, distortion, banding, …), each as
   its effect is ported. The legacy tests go in 3B.4.
3. **GPU vs reference** (real RHI; skipped under NullRHI; run on macOS/Metal with
   `-RenderOffscreen`), `CamSim.GPU.Sensor.*`: synthetic HDR inputs (a 20-stop log gradient, a step
   edge, point sources, a night scene) through `FSensorGraph`, read back, compared with the
   reference: Y ≤ 1 DN, chroma ≤ 2 DN, histogram bin-exact. Per effect as ported.
   `scripts/run_gpu_tests.sh` wraps the command line; CLAUDE.md documents it.
4. **Pipeline:** NV12 encode → decode round trip, PSNR ≥ 40 dB on a synthetic frame;
   `scripts/ci_validate.sh --native` passes (video, 30 fps, KLV conformant with misb.js).

## Bench

- **Sensor timing:** GPU timestamp queries around the sensor graph → `sensor_gpu_ms` in each
  frame-stats line; the legacy path writes `sensor_cpu_ms`. `analyze.py` reports p50/p95 per phase;
  `compare.py` diffs them.
- **Path assertion:** the bench reads `sensor_path` from `/health` and fails if it is not the
  expected path.
- **Post-sensor shots:** `GET /snapshot?stage=sensor` at every reference pose: the 8 existing poses
  in EO, plus IR and NVG at `nadir_3km`, `dusk_slant` and a new `night_slant` (sun 10° below the
  horizon): 14 shots in `shots/macos/3b/`. The 3A pre-sensor set stays as the render-path reference.
- **Baselines:** `baselines/macos-m1pro-3b-{720p,1080p}.json`.

## Exit criteria

macOS, M1 Pro, 1080p30, default config with every sensor effect the config enables:

1. `sensor_path=gpu` for the whole run; the legacy CPU pipeline, SceneCapture path and material
   path are deleted, with the line-count change recorded in the ROADMAP.
2. 30 fps output, 0 dropped frames in every bench phase; frame-time p95 no worse than the 3A.1
   baseline + 1 ms.
3. `sensor_gpu_ms` p95 ≤ 4 ms at 1080p with all effects on; controller CPU < 0.2 ms per frame;
   readback of 1.5 bytes/px plus the histogram.
4. `night_slant` EO mean luma < 40; daylight EO shots mean luma 90–170 with < 1% clipped pixels; a
   camera cut converges within one frame.
5. All automation tests pass under NullRHI; `CamSim.GPU.Sensor.*` passes on Metal;
   `ci_validate.sh --native` passes.
6. Visual review by the user of the EO, IR and NVG post-sensor shots.
7. Docs: ROADMAP 3B results; `docs/configuration.md` (`exposure:` keys, `sensor_path`); CLAUDE.md
   gotchas (`ReplacingTonemapper`, fixed UE exposure in the sensor path, `CamSimShaders` module, GPU
   test command; the "CPU sensor model has ~33 ms" note removed).

**Deferred:** RTX 5090 and Linux runs and the ROADMAP's "< 50% of frame budget" criterion (3D);
GPU-texture encode (3C); thermal radiance input (Milestone 4 — the stats pass input is its plug-in
point).
