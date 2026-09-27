# Render Path and Measurement — Design Spec (ROADMAP 3A)

**Date:** 2026-09-26
**Status:** Draft for review
**Scope:** First sub-project of Milestone 3 (GPU-resident sensor pipeline). Measure the current
pipeline, then render the sensor as the primary view instead of a `SceneCapture2D`, and tune
Cesium streaming and hitches against the measurements. The CPU sensor model, the encoder and
KLV are unchanged by this sub-project.
**Platform:** macOS (Apple M1 Pro, 16-core GPU, 16 GB, Metal) only. Linux/NVIDIA (reference
RTX 5090) and the Docker container are deferred; nothing here may be Metal-specific.

## Milestone 3 decomposition

Milestone 3 is split into four sub-projects, each with its own spec, plan and implementation:

| #  | Sub-project                     | Goal                                                                                                                                          |
| -- | ------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------- |
| 3A | **Render path + measurement**   | This spec.                                                                                                                                    |
| 3B | Sensor model on the GPU         | Replace the tonemapper with RDG compute passes on HDR input (minimal core effects, CPU auto-fallback, HUD on GPU), NV12 output, readback ring. |
| 3C | GPU-texture encode              | UE `AVCodecs` (NVENC / VideoToolbox) from GPU textures; FFmpeg kept for MPEG-TS/KLV muxing.                                                   |
| 3D | CI performance gate             | Turn the 3A harness into a tracked CI threshold.                                                                                              |

Decisions already taken for 3B, recorded here so they are not lost:

- Sensor model reads **FinalColorHDR-equivalent** input: UE post-processing (exposure, bloom,
  motion blur, DOF, flare) still runs; the sensor model owns everything after the tonemapper.
- **Minimal core first:** waveband, AGC, noise/FPN, defects, MTF blur, vignette, distortion,
  quantization. Other effects are ported later.
- **Auto-fallback:** if a config enables an effect or overlay the GPU path does not support yet
  (including laser spot and precipitation), the whole frame runs on the existing CPU path and the
  reason is logged once.
- Graphics code must run natively on macOS/Metal and Linux/Vulkan (NVIDIA), and in a container
  with NVIDIA drivers: plain compute only, no float atomics, no wave intrinsics.

## Problem

The user-visible problems are **tile LOD pop-in** and **frame rate / hitches** with Cesium
terrain. Both trace back to how the sensor is rendered:

- **The scene is probably rendered twice.** `ACamSimGameMode` has no pawn, and nothing disables
  world rendering on the game viewport, so the main viewport renders a default player view
  (Lumen GI + reflections, virtual shadow maps) every frame, and `SceneCapture2D` then renders
  the sensor view again. (To be confirmed by the baseline measurement.)
- **`SceneCapture2D` is a second-class view.** It has no usable temporal history, so it runs
  FXAA instead of TSR (`CamSimCamera.cpp`: *"TSR's temporal history doesn't accumulate on
  off-screen captures (persistent ghosting)"*). Lumen, shadow-cache and auto-exposure history
  are weaker too.
- **Cesium LOD transitions are disabled because of it.** `use_lod_transitions: false` —
  *"crossfade causes blur on moving cameras with SceneCapture2D."* Cesium's dithered crossfade
  needs temporal AA to resolve, so LOD changes pop.
- **Detail is traded for budget.** SSE sits at Cesium's default of 16 (*"2.0 caused 44K+ loaded
  tiles and ~18 fps on macOS"*), and adaptive SSE coarsens it further when the frame budget is
  blown, which the double render makes more likely.
- **Cesium does not see the sensor as its camera.** `FCamSimStreamingController` registers two
  stand-in cameras with `ACesiumCameraManager` to get tiles selected for the capture's view.
- **The georeference origin never moves.** Nothing rebases it as the platform flies. Away from
  the start point, UE "up" tilts from local vertical, precision for shadows and tile placement
  degrades, and the sky atmosphere and sun are laid out around the origin.
- **Nothing measures any of this.** There is no repeatable benchmark and no reference imagery,
  so "does it look right" and "is it smooth" have never been checkable.

Out of scope: the source data (Cesium World Terrain + Bing imagery vs Google Photorealistic 3D
Tiles, whose license terms for dataset generation need checking). That is a separate data
decision.

## Goals

- A repeatable benchmark and reference-shot harness, with a committed macOS baseline of today's
  pipeline.
- One scene render per frame: the sensor is the primary view.
- TSR, Lumen, virtual shadow maps and auto-exposure with proper view history on the sensor.
- Cesium LOD transitions on; tile selection driven by the real view.
- Correct local frame and lighting far from the start point (origin shift).
- Hitches attributed and the top causes fixed, based on traces rather than guesses.

## Non-goals

- Any change to the CPU sensor model, the encoder, multi-view fan-out or KLV.
- Linux/NVIDIA and Docker measurements (deferred; the harness must work there unchanged).
- Moving the ML depth capture off `SceneCapture2D`.
- Multiple sensors per instance.

## Design

### 1. Primary-view render path

**Camera.** `ACamSimCamera` gains a `UCameraComponent` (`SensorCamera`) attached where the
`SceneCapture` is today, under the gimbal rotation. At `BeginPlay` the first local player
controller calls `SetViewTarget(this)`. UE updates player cameras after all tick groups, so the
camera's `TG_PostUpdateWork` tick still sets pose and FOV before the frame renders.

Everything the camera currently writes to the capture moves to the camera component:

- HFOV from CIGI View Definition → `SensorCamera->FieldOfView`.
- Gimbal rotation → `SensorCamera` relative rotation.
- DOF auto-focus distance and the Phase 15 optical-realism settings →
  `SensorCamera->PostProcessSettings` (same `FPostProcessSettings` struct).
- Show-flag toggles (bloom, motion blur, lens flare) become their post-process equivalents
  (intensity/amount 0 when disabled). Contact shadows stay per light.
- Telemetry (`SetFieldOfView`, frame centre, laser designator projection) reads the camera
  component instead of the capture.

**Viewport.** At startup the game viewport is forced to `CaptureWidth × CaptureHeight`
(`FSystemResolution::RequestResolutionChange`, windowed). Headless stays `-RenderOffScreen`.
With a window (`-game`), the window shows the sensor view.

**Frame grab.** A new scene view extension, `FCamSimFrameGrabExtension`, active only for the
game viewport's view family (`IsActiveThisFrame_Internal` checks the family's viewport):

1. Game thread: `UCamSimCaptureComponent::Capture(Telemetry)` enqueues a render command that
   pushes `{FrameIndex, Telemetry, target index}` onto a render-thread-owned
   `FFrameGrabRequestQueue`. It is enqueued during tick, so it runs before this frame's scene
   render command.
2. Render thread, `PostRenderViewFamily_RenderThread`: if a request is pending, pop it,
   register the family's render target in RDG, and `AddDrawTexturePass` it into the grab target
   (`PF_B8G8R8A8`, capture size; the draw scales and converts if the viewport size or format
   ever differs). Then queue the existing `FRHIGPUTextureReadback` copy for that target.
3. From there the existing readback state machine, `FSensorPostProcess`, `FMultiViewFrameSink`
   and encoder are unchanged.

`FFrameGrabRequestQueue` is a small pure class (no RHI), so its ordering rules are unit-tested:
at most one grab per request, FIFO, and requests older than the current poll generation are
dropped.

**Renderer settings.**

- `r.AntiAliasingMethod=4` (TSR) in `DefaultEngine.ini`.
- New key `render.screen_percentage` (default 100, env `CAMSIM_RENDER_SCREEN_PERCENTAGE`).
  Below 100, TSR upscales to the output size.
- `use_lod_transitions` defaults to `true`; `lod_transition_length` default set during tuning.

**TSR camera cuts.** When the pose jumps (CIGI teleport, or position/attitude change above
`render.camera_cut_distance_m` / `render.camera_cut_angle_deg` in one frame), call
`PlayerCameraManager->SetGameCameraCutThisFrame()` so temporal history is reset instead of
smearing the previous scene. The threshold test is a pure function, unit-tested.

**Cesium.** Cesium selects tiles for the player camera natively.
`FCamSimStreamingController` drops its primary-camera slot and keeps only the inflated prefetch
camera for gimbal slews. Adaptive SSE and the terrain gate are unchanged.

**Origin shift.** A `UCesiumOriginShiftComponent` on `ACamSimCamera` rebases the georeference
as the platform moves, using a distance threshold (`render.origin_shift_distance_m`, default
set during tuning). A rebase issues a camera cut on the same frame. Anything that caches UE
world positions across frames (entity manager, streaming controller, telemetry) must be checked
against a rebase during implementation; the CIGI frame conversions already go through
`GlobeAnchor` / `CigiFrames.h` and are unaffected.

**Transition switch.** `render.view_source: primary | scene_capture` (default `primary`, env
`CAMSIM_RENDER_VIEW_SOURCE`). `scene_capture` keeps today's path for A/B benchmarking during 3A.
It is deleted in 3B. In `scene_capture` mode the game viewport's world rendering is disabled
(`GameViewportClient->bDisableWorldRendering`), so the A/B comparison is against a
single-render baseline as well as the current double-render one.

### 2. Benchmark and reference-shot harness

New directory `scripts/bench/`:

- **`scenario.py`** generates the flight deterministically in Python (no binary recordings). It
  pins time of day through CIGI Celestial Sphere Control (as `send_cigi_test.py --time` does)
  and runs named phases:
  1. `warmup` — the whole route, unmeasured, to fill Cesium's disk cache.
  2. `orbit` — 3 km altitude orbit, 2 minutes.
  3. `slew` — gimbal yaw/pitch sweeps at 10–60 °/s. Pop-in and hitch stressor.
  4. `low_pass` — 500 m AGL straight line at 100 m/s. Continuous tile streaming.
  5. `far_origin` — teleport 300 km, settle, orbit. Exercises origin shift and lighting.
  6. `shots` — about eight fixed poses for reference images (nadir 3 km, slant 10 km, horizon,
     dawn, dusk, low oblique, the same view before and after the far-origin teleport).
- **`run_bench.py`** launches CamSim headless through `run.sh` with `-csvprofile`, waits for
  `GET /ready`, drives the scenario over CIGI, collects results, then shuts down. Flags:
  `--label`, `--view-source`, `--smoke` (about 20 s: one short orbit and one shot),
  `--trace` (adds `-trace=cpu,gpu,frame` for Unreal Insights).
- **`compare.py`** diffs a results JSON against a baseline and prints a markdown table.

**Metrics** (per phase, in `results.json`):

- From UE's CSV profiler: frame time p50/p95/p99, game / render / RHI thread time, GPU time,
  and hitch count (frames over 66 ms, and over 100 ms).
- New `CSV_CUSTOM_STAT`s in a `CamSim` category: frames emitted, frames dropped, grab-to-encode
  latency, load progress of tilesets in view, current SSE, origin-shift events.
- **Pop-in proxy:** fraction of `slew` frames rendered while in-view tileset load progress is
  below 100%. The raw `.ts` of the slew phase is kept for human review.
- Run metadata: git SHA, platform, GPU, config overrides, and whether the Cesium cache was warm.

**Reference shots.** New endpoint `GET /snapshot` on the health HTTP server returns the next
grabbed frame as PNG — before sensor effects and encoding, so it is lossless and shows only the
render. Disabled unless `operational.snapshot_endpoint_enabled: true` (env
`CAMSIM_SNAPSHOT_ENDPOINT_ENABLED`); the bench config sets it. At each shot pose the harness
waits for the terrain gate plus 2 s of settle time (TSR / Lumen history), then saves the frame.

**Stored results.**

- Baselines: `scripts/bench/baselines/<platform>-<gpu>-<label>.json`.
- Shots: `scripts/bench/shots/<platform>/<label>/<name>.png`, tracked with git LFS (`*.png`
  under `scripts/bench/shots/`).
- `compare.py` reports SSIM per shot as information only. TSR jitter and tile-streaming order
  make exact image matches unrealistic, so 3A has no image gate; 3D decides thresholds.

Requirements: a Cesium ion token (already in config) and network access for the warm-up.

### 3. Tuning loop

Each lever is its own commit, with before/after harness numbers in the commit message:

1. **Hitches.** Attribute every frame over 66 ms using the CSV data plus an Insights trace. Fix
   what the traces show. Likely candidates, not a work list: tile physics-mesh cooking, glTF
   mesh creation on the game thread, first-use shader/PSO compiles (`prewarm_shaders.sh`, PSO
   precaching), texture-pool thrash, GC.
2. **SSE.** Find the lowest default `maximum_screen_space_error` that holds the frame budget,
   with adaptive SSE kept as the safety net.
3. **`render.screen_percentage`**, only if the GPU is the bottleneck.
4. **LOD transition length** and **origin-shift distance** defaults.

An M1 Pro may not hold 1080p30 with Lumen, virtual shadow maps and TSR at full resolution. If
not, the tuning finds the combination (screen percentage, SSE, Lumen/shadow quality) that does,
and documents it as a macOS dev profile in `docs/configuration.md` rather than degrading the
defaults.

## Exit criteria (macOS, M1 Pro, warm cache, same machine as the baseline)

- The frame contains exactly one scene render (Insights trace).
- GPU time per frame in `orbit` is lower than the baseline's.
- Frames over 66 ms in `orbit` and `low_pass` are at least 50% fewer than the baseline's.
- Pop-in proxy in `slew` is better than the baseline's.
- Default SSE is lower than or equal to 16 without worse p99 than the baseline.
- A configuration that holds p95 ≤ 33.3 ms at 1080p30 in every phase is found and documented
  (the macOS dev profile), or the report shows what prevents it.
- Before/after reference shots reviewed and accepted by the user, including correct lighting and
  atmosphere in the far-origin shot.
- All automation tests, KLV conformance and `ci_validate.sh --native` pass. ROADMAP.md updated.

Deferred to when the Linux/5090 host is available: baseline and after runs on the RTX 5090, and
the absolute target (p99 ≤ 33.3 ms at 1080p30 in every phase, no frames over 100 ms).

## Testing

- **Automation tests (`-nullrhi`):**
  - config parsing and env overrides for the new `render.*` and snapshot keys;
  - `FFrameGrabRequestQueue` ordering, one grab per request, stale-request drop;
  - camera-cut threshold function;
  - streaming controller with only the prefetch slot registered;
  - `/snapshot`: 404 when disabled, 503 before the first frame, PNG with the right size after
    one is supplied (two-ticker HTTP pattern from `HttpServerLifecycleTest.cpp`).
- **Python tests** in `scripts/tests/`: scenario generation is deterministic (same packets for
  the same seed), phase timing, and `compare.py` output on fixture results.
- **Real-RHI smoke:** `run_bench.py --smoke` is added to `ci_validate.sh --native`. It fails if a
  snapshot never arrives, is the wrong size, or is black.

## Risks

| Risk | Mitigation |
| ---- | ---------- |
| Offscreen viewport sizing or grab format misbehaves under `-RenderOffScreen` on Metal | First implementation task is a macOS check of the grab path; `AddDrawTexturePass` handles size/format mismatch; `scene_capture` switch stays as fallback until 3B. |
| Origin shift causes a one-frame jump or breaks cached world positions | Distance-threshold shifting with a camera cut on the same frame; audit of world-position caches; `far_origin` phase and shots cover it. |
| TSR ghosting on fast slews or pose jumps | Camera cuts on jumps; the `slew` phase and its saved `.ts` show residual ghosting. |
| Cesium ion network variance skews numbers | Warm-up pass; runs record warm/cold cache; compare only warm runs. |
| M1 Pro cannot hold 1080p30 | Relative exit criteria plus a documented macOS dev profile. |

## Files touched (expected)

- `Camera/CamSimCamera.{h,cpp}` — camera component, view target, camera cuts, origin shift.
- `Camera/CamSimCaptureComponent.{h,cpp}` — grab requests instead of `CaptureScene` in primary
  mode; view-source switch.
- `Camera/CamSimFrameGrabExtension.{h,cpp}` (new), `Camera/FrameGrabRequestQueue.h` (new).
- `Camera/CamSimStreamingController.{h,cpp}` — primary slot removed in primary mode.
- `Camera/CamSimTelemetryAssembler.{h,cpp}` — read FOV/pose from the camera component.
- `Health/CamSimHealthServer.{h,cpp}` — `/snapshot` endpoint.
- `Config/CamSimConfig.{h,cpp}`, `deploy/camsim_config.yaml`, `docs/configuration.md` — new keys.
- `Config/DefaultEngine.ini` — TSR.
- `scripts/bench/` (new), `scripts/tests/`, `scripts/ci_validate.sh`, `.gitattributes`.
- `ROADMAP.md` — Milestone 3 decomposition and 3A progress.
