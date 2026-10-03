# CamSim

Synthetic sensor simulator: CIGI 3.3 UDP → Cesium/UE5 render → H.264 MPEG-TS + MISB ST 0601 KLV → UDP multicast.

## Goals

- Cross platform: Linux, MacOS, Cloud (Docker)
- Rich synthetic image generator suitable for user training, simulation, and aided target recognition (ATR) training.
- Standards compliant (CIGI 3.3)
- Realism: WGS-84 earth model
- Open source alternative to MetaVR's Virtual Reality Scene Generator (VRSG)

## Notes

- Keep `ROADMAP.md` up to date as milestones are implemented.
- Record editor changes that need to be made by a human (new assets, textures, materials, etc.) in `ROADMAP.md`.

## Commands

| Command                        | Description                                                   |
| ------------------------------ | ------------------------------------------------------------- |
| `scripts/repo_setup.sh`        | One-time: fetch glTFRuntime, install Cesium into the engine   |
| `scripts/build_thirdparty.sh`  | Build CCL + FFmpeg static libs (cached in .build_tmp/)        |
| `scripts/run.sh`               | Launch UE5 in game mode (supports --build, --headless, --log) |
| `scripts/run.sh --build`       | Build + launch                                                |
| `scripts/send_cigi_test.py`    | Send CIGI 3.3 test packets (--sweep, --circle)                |
| `scripts/send_dis_test.py` | Send DIS Entity State PDUs: scripted truck + boat (`both`, `truck-loop`, `boat-circle`; 4C thermal states `truck-park`, `truck-parked`, `truck-destroyed` set appearance bits) |
| `scripts/dis_vehicle_check.py` | End-to-end DIS vehicle check (shots + COCO labels) |
| `scripts/ocean_check.py` | Ocean acceptance: DIS boat at Beaufort 0/3/6 (+ `--cigi` Wave Control): COCO, boat altitude vs sea level, HOT, frame times, shots |
| `scripts/gt_occlusion_check.py` | Ground-truth acceptance (ROADMAP 2.7): nadir / edge / Beaufort 6 crest / terrain views, COCO checks, mask/box overlays, frame time ML on vs off |
| `scripts/thermal_check.py` | Thermal IR acceptance (ROADMAP 4A/4B/4C): MWIR/LWIR at noon and 02:00 with the DIS truck + boat — night IR level, white-hot vehicle, water/land crossover, cool shadows, cold sky, land-cover contrast, structure and re-centre hitch, ThermalCS time, EO untouched; 4C (`--runs entity`): running vs parked-cold truck, hot parts, cool-down after parking, burning when destroyed |
| `scripts/landcover/fetch_worldcover.py` | Cut ESA WorldCover 2021 COGs (HTTP range reads) into land-cover tiles for thermal IR (`--bbox W S E N`; SF sample committed) |
| `scripts/klv_conformance/check.js` | Check KLV against misb.js (packets.jsonl, .ts, or udp://) |
| `scripts/test_video_output.sh` | ffprobe/ffplay stream validation                              |
| `scripts/package_for_docker.sh` | BuildCookRun → `deploy/staged/Linux/` (+ `entities/`) for the Docker image |
| `scripts/ci_validate.sh`       | Integration test: runs the image with `--gpus all` + CIGI host, `/ready`, video/KLV |
| `scripts/ci_validate.sh --native` | Same, without Docker (macOS): launch headless + CIGI host + checks |
| `scripts/bench/run_bench.py` | Render benchmark + reference shots (`--smoke`, `--trace`); `compare.py` diffs two runs |
| `scripts/run_gpu_tests.sh`     | `CamSim.GPU.*` automation tests on the real RHI (Metal / Vulkan) |
| `uv run --project hitl python -m camsim_hitl --config <file>.toml` | HITL IG host: X-Plane truth + PX4 gimbal/camera (MAVLink) → CIGI (`hitl/README.md`, design `HITL.md`) |
| `uv run --project hitl --with pytest pytest hitl/tests` | IG host tests |
| `hitl/xplane_plugin/` (`fetch_sdk.sh`, CMake, ctest) | CamSimTruth X-Plane 11 plugin: read-only truth datagram (`hitl/PROTOCOL.md`) |

## Documentation

- Unreal Engine Cesium Plugin Documentation - https://cesium.com/learn/cesium-unreal/ref-doc/api-design.html and https://cesium.com/learn/cesium-unreal/ref-doc/index.html

## Architecture

```
camsim/
  unreal_project/CamSimTest/       # UE5.8 project root
    Source/CamSimTest/             # C++ module
      Camera/                      # Sensor actor + rig, capture pipeline, telemetry, Cesium streaming
      CIGI/                        # UDP receiver/sender, packet parsing, terrain queries
      Config/                      # YAML config loader (rapidyaml) + env var overrides
      Encoder/                     # FFmpeg H.264/H.265 → MPEG-TS → UDP multicast
      Entity/                      # Actor lifecycle, dead-reckoning, articulated parts
      Environment/                 # Sky, fog, weather, day/night
      Geospatial/                  # Cesium terrain queries, WGS84 conversions
      GroundTruth/                 # COCO annotations from rendered instance masks (bbox, OBB, visibility, RLE, box3d), depth maps
      Ocean/                       # Sea level + Gerstner waves (FOceanWaves), ocean mesh, MPC writes
      Metadata/                    # MISB ST 0601/ST 0102 KLV builder
      Thermal/                     # IR radiance (4A): class model, sky, Planck LUT, frame builder, ThermalCS CPU mirror; land cover (4B): tiles, window, code -> class; entity state (4C): EntityThermal model, speed tracker
      Sensor/                      # Physical sensor model: presets, optics, AE/AGC controller, CPU reference (SensorReference)
      Subsystem/                   # UGameInstanceSubsystem lifecycle owner
      GameMode/                    # Minimal game mode, no pawn
      Tests/                       # UE5 Automation tests (472 tests across 80 files)
    Source/CamSimShaders/          # PostConfigInit module: /CamSim shader dir, GPU sensor RDG graph, SensorFrameParams/SensorHash
    Shaders/Private/               # CamSimSensor.usf + CamSimSensorCommon.ush (virtual path /CamSim)
    Source/ThirdParty/
      CCL/                         # CIGI Class Library (static lib)
      FFmpeg/                      # libavcodec/format/util/swscale + libx264
    Config/                        # DefaultEngine.ini, DefaultGame.ini
  hitl/                            # HITL rig (HITL.md): Python IG host (camsim_hitl), X-Plane truth plugin, wire formats (PROTOCOL.md)
  deploy/                          # Dockerfile, docker-compose.yml, entrypoint.sh, camsim_config.yaml
  scripts/                         # Build, run, test, validation scripts
  docs/                            # architecture.md, configuration.md, klv-tags.md, etc.
```

## Data Flow

```
CIGI UDP → FCigiReceiver (FRunnable thread) → TSpscQueue
Game Thread → FCamSimEntityManager (entities + camera platform) → ACamSimCamera::Tick() → game viewport (primary view)
Render Thread → GPU sensor graph (replaces the tonemapper) → NV12 readback (FRHIGPUBufferReadback)
Task Thread → ground truth → FVideoEncoder → MPEG-TS + KLV → UDP multicast
```

Four threads: CIGI Receiver, Game, Render, Task (encoding). Communication via lock-free SPSC queues.

## Key Files

- `Source/CamSimTest/CamSimTest.Build.cs` — module build config, thirdparty linking
- `deploy/camsim_config.yaml` — canonical runtime config (100+ params, all have `CAMSIM_*` env overrides)
- `Source/CamSimTest/Config/CamSimConfig.h` — `FCamSimConfig` struct, YAML + env var loading
- `Source/CamSimTest/Subsystem/CamSimSubsystem.h` — lifecycle owner, Pimpl pattern (`FSubsystemImpl`)
- `Source/CamSimTest/Camera/CamSimCamera.h` — sensor actor; delegates to `CamSimPlatformRig`, `CamSimCaptureComponent`, `CamSimTelemetryAssembler`, `CamSimStreamingController`

## Code Style

- UE5 naming: `A` (Actor), `U` (UObject), `F` (struct/POD), `E` (enum), `I` (interface)
- PascalCase everywhere; verb-first functions (`CaptureAndEncode`, `DequeueEntityState`)
- `#include "CoreMinimal.h"` first, `.generated.h` last
- Forward declarations preferred over includes in headers
- `CppCompileWarningSettings.UndefinedIdentifierWarningLevel = WarningLevel.Off` in Build.cs — intentional for FFmpeg C headers
- Copyright: `// Copyright CamSim Contributors. All Rights Reserved.`

## Testing

- **C++ tests**: UE5 Automation framework in `Source/CamSimTest/Tests/` (472 tests across 80 files, 26 of them `CamSim.GPU.*` and skipped under NullRHI; all under `CamSim.*`)
  - Run in editor: `Ctrl+Alt+F11` or `Automation` console command
  - Run headlessly (any host with UE5.8 installed):
    ```bash
    "$UE_BIN" unreal_project/CamSimTest/CamSimTest.uproject \
      -ExecCmds="Automation RunTests CamSim+Quit" \
      -TestExit="Automation Test Queue Empty" \
      -ReportExportPath=.cache/automation-report \
      -unattended -nullrhi -nosound -nosplash -log -stdout -FullStdOutLogOutput
    ```
    `NullRHI` keeps it under 10 s end-to-end (no shader compile, no display). JSON results land at `.cache/automation-report/index.json` (UTF-8 BOM — read with `encoding="utf-8-sig"`).
  - Filter narrower: replace `CamSim` in `RunTests` with e.g. `CamSim.Sensor` or a single test path.
- **GPU tests** (`CamSim.GPU.*`, real RHI; skipped under NullRHI): `scripts/run_gpu_tests.sh [filter]` (Metal on macOS, Vulkan + Xvfb on Linux; first run compiles shaders). Read GPU results back with `FRHIGPUTextureReadback`, never `ReadLinearColorPixels`: Vulkan asserts on R32F ("Unsupported format")
- **KLV conformance**: `node scripts/klv_conformance/check.js` (misb.js; run `npm ci` in that directory first)
- **Python validation**: `scripts/test_video_output.sh`
- **Integration**: `scripts/ci_validate.sh` (Docker headless + health wait + ffprobe + KLV check)
- **CIGI testing**: `scripts/send_cigi_test.py --sweep` or `--circle` for motion patterns

### UE5 toolchain

- **Linux**: UE5.8 lives at `/opt/UE`. `UE_BIN=/opt/UE/Engine/Binaries/Linux/UnrealEditor`. `scripts/run.sh` auto-discovers this via `find /opt $HOME -path '*/Binaries/Linux/UnrealEditor'`, so no env vars are needed for `--build` / `--build-only`.
- **Build**: `scripts/run.sh --build-only` invokes `/opt/UE/Engine/Build/BatchFiles/Linux/Build.sh CamSimTestEditor Linux Development`. Incremental rebuilds finish in ~10 s; cold rebuilds depend on UBA cache.
- **Don't pipe `run.sh` through `tee` without `set -o pipefail`** — the script propagates UBT failures via exit code, but `tee` succeeding will mask them. Either run `run.sh` foreground or wrap the pipeline with `set -o pipefail` so a failed compile actually surfaces.
- **CI coverage** (Phase 28A): `.github/workflows/ci.yml` `unit-tests` runs the headless automation suite on every PR and push; `integration-test` runs the packaged Docker stack on push-to-`main`; both target the `[self-hosted, camsim-ue5]` runner. Failures are parsed from `.cache/automation-report/index.json` by `scripts/parse_automation_report.py` and surfaced as `::error::` annotations. Kill-switch: set `vars.CAMSIM_UE5_RUNNER_AVAILABLE=false` to skip the four UE5-dependent jobs. Runner registration: `docs/ci-runner-setup.md`.

## Gotchas

- **Targets must stay on `BuildSettingsVersion.V7`**: UE 5.8 refuses an editor target whose build settings differ from the installed engine's ("modifies the values of properties … not allowed").
- **Cesium lives in the engine, not the project**: `repo_setup.sh` installs it into `$UE_ROOT/Engine/Plugins/Marketplace/CesiumForUnreal` (auto-detected, or set `UE_ROOT`), where UBT uses the release zip's prebuilt binaries. Never put it back in `unreal_project/CamSimTest/Plugins/`: a project plugin overrides the engine one and gets rebuilt from source with the project's settings (~20 min), and Cesium 2.29.1 doesn't compile that way under Apple clang 21 (`IonQuickAddPanel.cpp` self-capture). Bump `CESIUM_VERSION` in `repo_setup.sh` to upgrade.
- **ThirdParty must be built first**: Run `scripts/build_thirdparty.sh` before UE build — CCL + FFmpeg are static libs not checked in
- **One terrain tileset**: `ApplyCesiumBackendConfig` configures the level's terrain tileset (first Cesium World Terrain, ion 1) and destroys every other `ACesium3DTileset` (it used to turn `Main.umap`'s OSM Buildings into a duplicate terrain). Cesium's game-thread cost tracks the rendered tile count (per-tile collision/visibility every frame, ROADMAP 3B exit check), so extra tilesets and `frustum_culling: false` are not free
- **Cesium coord order**: `TransformLongitudeLatitudeHeightPositionToUnreal(FVector(Lon, Lat, Alt))` — Longitude first, not Latitude
- **Never pass CIGI angles to `SetActorRotation`**: UE world axes are Cesium East-South-Up only at the georeference origin (+X = East, so heading 0 would face east). Use `GlobeAnchor->SetEastSouthUpRotation(CamSimFrames::CigiToEastSouthUp(...))` from `Geospatial/CigiFrames.h`
- **CIGI entity-relative fields**: when Attach State = Attach or a request's coordinate system = Entity, the Lat/Lon/Alt fields are X/Y/Z metre offsets in the reference entity's body frame (X fwd, Y right, Z down). Resolve via `UCamSimSubsystem::GetEntityGeoPose()`
- **macOS editor log**: `~/Library/Logs/CamSimTest/CamSimTest.log`, not `Saved/Logs/`. A cold `run.sh` start spends a few minutes compiling shaders before `LogCamSim` appears
- **Headless tests on macOS**: add `-DisablePython` — Python's startup type generation deadlocks under `-nullrhi` on macOS. Xcode 27 needs `MaxVersion` raised in the engine's `Engine/Config/Apple/Apple_SDK.json`, and a real (non-nullrhi) run needs `xcodebuild -downloadComponent MetalToolchain`
- **DIS vehicles sit on the rendered surface**: land (domain 1) and surface (domain 3) entities are clamped at every pose commit by traces against Cesium tiles (`Entity/SurfaceClamp.h`, `SurfaceProbe.h`); needs `create_physics_meshes`. The sender's altitude is ignored unless `dis.clamp_to_surface: false`. Guide: `docs/dis.md`
- **Ocean** (ROADMAP 2.6, `ocean:`, on by default): sea level = EGM96 geoid + CIGI tide; `FOceanWaves` (Gerstner, sim time) is the single source for boat placement (`ClampWater`), HAT/HOT (max(Cesium hit, sea surface incl. waves)) and the drawn sea (`UProceduralMeshComponent` warped grid + `M_Ocean` WPO via `MPC_Ocean`; the CPU/GPU mirror is `Shaders/Private/CamSimOcean.ush`, held to 2 cm by `CamSim.GPU.Ocean.MatchesCpu`). `M_Ocean`/`MPC_Ocean` are generated — edit `scripts/ocean/make_ocean_material.py` (or the .ush) and rerun `scripts/ocean/make_ocean_material.sh`, never hand-edit the assets. Piers Cesium drapes below sea level flood (known)
- **Ground-truth masks** (ROADMAP 2.7, guide `docs/ground-truth.md`): entities render custom depth with a stencil value 1..255 (`FStencilSlotAllocator`, reuse delayed 4 frames); `r.CustomDepth=3`; `InstanceIdCS` runs in the sensor graph only on annotated frames and shares `UndistortScale` with `SensorCS` — change both together; `FInstanceMaskAnalyzer` turns the readback into boxes/OBBs/visibility/RLE on the task thread; only meshes are tagged (never particles) and box3d is their union; the hidden silhouette below each entity's sea-surface plane (waves included, one plane per stencil) is cut as submerged hull; OBBs follow the projected box3d axis. `ml_training.depth_map` comes out of the same pass (`WRITE_DEPTH` permutation: linear view depth of the same source texel, read back per slot like the IDs); its PNGs are written by separate tasks (≤ 4 in flight) so they never hold `bSensorBusy`
- **Altitudes are WGS-84 ellipsoid heights everywhere** (CIGI 3.3 defines its "MSL" as the ellipsoid, and Cesium uses HAE). Only KLV Tags 15/25 are true MSL, via the EGM96 grid in `Geospatial/Geoid.h` (`Content/NonUFS/Geoid/WW15MGH.DAC`, git LFS — run `git lfs pull` if it's a pointer file)
- **UE unit scale**: 1 UE unit = 1 cm — divide `FVector::Dist()` by 100 for metres
- **macOS multicast**: UDP multicast to 239.x.x.x on loopback requires `sudo route add -net 239.0.0.0/8 -interface lo0`, or use unicast: `CAMSIM_MULTICAST_ADDR=127.0.0.1`
- **CCL API quirk**: `GetDestEntityIDValid()`/`GetDestEntityID()` only in `CigiLosSegReqV3_2`, not V3
- **Fixed framerate**: Engine locked to 30fps via DefaultEngine.ini (`bUseFixedFrameRate=True`). `DeltaTime` is therefore constant: measure frame time with the wall clock (the bench does)
- **The sensor is the primary view** (the only render path since 3B.2, ROADMAP 3A): the game viewport renders it with TSR and `FCamSimFrameGrabExtension` grabs the result. `SceneCapture` only holds pose/FOV/post-process — it is never captured; don't call `CaptureScene()` on it. Screen messages are disabled, since the viewport canvas would be burned into the video
- **Readback ring** (`Camera/ReadbackRing.h`): up to three captures in flight, delivered strictly in capture order; a full ring skips the new frame (counted as `EncoderBusy`). The ring carries the sensor graph's NV12 (1.5 bytes/px); the CPU only de-interleaves UV before encoding
- **GPU sensor graph** (ROADMAP 3B; the only sensor path since 3B.2 — no CPU sensor model, no burned-in overlays): replaces UE's tonemapper via `ISceneViewExtension::EPostProcessingPass::ReplacingTonemapper`; UE exposure is manual and driven by the sensor AE (`AutoExposureBias` = sensor gain, so `View.PreExposure` tracks it and the shader divides it out). `UCamSimSubsystem::IsSensorGraphAvailable()` is decided once at startup (primary view, NV12 dims, `IsSensorGraphSupported`); without it (e.g. NullRHI) no frames are produced and `/ready` stays false. There is no CPU fallback. Linux/Vulkan is verified on NVIDIA (2026-09-30); Mesa lavapipe (the CPU Docker path) does not work: it segfaults compiling UE 5.8 SM6 pipelines (ROADMAP 1.15). Streams are tagged BT.709 transfer
- **Physical sensor model** (ROADMAP 3B.2): one fused compute pass, `SensorCS` — optics (distortion resample, cos⁴ vignetting, PSF blur) → electrons → detector noise (photon: PRNU/shot/dark/DSNU/read, full-well clip, analog gain; microbolometer: temporal + pixel/column/row FPN) → ADC → defects → display → NV12. `CamSimSensorRef::Run` (`Sensor/SensorReference.cpp`) is the CPU reference: the shader mirrors it expression for expression, and `CamSim.GPU.Sensor.*` hold them to Y ≤ 1 DN, UV ≤ 2 DN. Change both together. Noise is a PCG hash of (x, y, frame, seed, stream) (`CamSimShaders/Public/SensorHash.h`), never a GPU RNG; round with `floor(x + 0.5)`, never HLSL `round`; no float atomics or wave intrinsics (portable to Vulkan)
- **Sensor presets** (`Sensor/SensorPresets.cpp`): `sensor_modes.<mode>.preset` (`eo_hd_cmos`, `mwir_cooled` default IR, `lwir_uncooled`) fills optics/detector; `optics:`/`detector:` blocks override single fields. Physics tests `CamSim.Sensor.Physics.*` (16) check the reference against closed-form photon-transfer, FPN and PSF results — keep them passing when retuning presets
- **Sensor AE clips at full well**: `FSensorController::ClipLinear` = 1.0 (normalised full scale = white after the knee). The PSF radius R = min(ceil(3σ_o)+1, 8); R ≥ 4 (σ_o > 2/3 px) switches to the large-tile shader and is over the 2 ms 1080p budget (a startup warning, not an error)
- **Thermal IR** (ROADMAP 4A, guide `docs/thermal.md`): in IR, `ThermalCS` runs at `EPostProcessingPass::BeforeDOF` (pre-TSR, subscribed only while thermal params are set) and writes `float4(L, L, L, 1)` radiance into scene colour; TSR resolves it and `SensorCS` (at the tonemapper) reads the resolved radiance (`bRadianceInput`, weights (1,0,0), `InputScale = 1/B(300 K)`). `CamSimThermalRef::EvaluatePixel` (`Thermal/ThermalReference.cpp`) is its CPU mirror: change both together; `CamSim.GPU.Thermal.MatchesCpu` holds them to 1e-4. Class temperatures are closed-form per class on the game thread (`FThermalModel`, sim time, no state, cached Fourier fit), built per tick by `FThermalFrameBuilder`. UE exposure is fixed at −12 EV in thermal mode (the thermal AE slot has its own `thermal_exposure`). `r.TSR.AlphaChannel=1` is set (`FThermalTsrAlpha`) only while thermal IR is active: TSR's default R11G11B10 output would quantise radiance to 0.2–0.4 K (MWIR), above NETD; it costs +1.6 ms in IR and a first-switch PSO hitch. The per-pixel solar term reads GBuffer base colour through its sRGB SRV (raw loads are not linear); `K_lum` divides by `S_clear × cloud factor` and assumes the UE sun light isn't itself cloud-dimmed. Entity stencils are tagged whenever thermal is available (not only for ML). `thermal.enabled: false` restores the 3B.2 luminance proxy. Terrain classes come from land cover (next entry); acceptance is `scripts/thermal_check.py` (gates a–l). Verified on Metal and on NVIDIA Vulkan (2026-10-02: every `thermal_check.py` gate, `CamSim.GPU` 25/25). Non-finite guards must bit-test a loaded value, not arithmetic on it: NVIDIA's Vulkan compiler folds `IsNonFinite(x * s)` away
- **Land cover for thermal IR** (ROADMAP 4B, `thermal.land_cover`, guide `docs/thermal.md`): terrain pixels take their thermal class from ESA WorldCover tiles in `Content/NonUFS/LandCover` (git LFS — `git lfs pull` if `CamSim.Thermal.LandCover.SanFranciscoSample` fails; other areas: `scripts/landcover/fetch_worldcover.py`). `FLandCoverWindow` builds a camera-centred 2048² window of 10 m texels on a task thread and swaps it in; its GPU copy is paired with each frame's `FThermalFrameParams` by window id (`CamSimLandCover::ShouldBindWindow`). `ThermalCS` blends the four nearest texels' class data (smoothstep, ground-anchored warp) and refines it by the GBuffer base colour (vegetation index from a 5-tap world-space blur, `veg_blur_m`, because imagery JPEG chroma blocks otherwise show as squares); `CamSimThermalRef` mirrors it (`CamSim.GPU.Thermal.LandCoverMatchesCpu`). East/North are recomputed every frame (origin shifts rotate UE axes). `thermal.land_cover.enabled: false` is 4A bit for bit with 4A's default classes (the 4A `vegetation` class was retuned; 14 built-ins leave 18 of 32 class slots for users). A `classes.<code>` override to a non-default material drops that code's refinement family. `snow_ice` has `k_fast` 0 (never above 0 °C). Data is CC BY 4.0: keep the attribution
- **Entity thermal state** (ROADMAP 4C, `thermal.entity`, guide `docs/thermal.md`): each entity steps a skin + ≤ 4 part temperatures (engine / exhaust / running gear) on sim time in the entity manager tick (`Thermal/EntityThermal.h`, pure), stored as the excess over the 4A baseline B = T_class + `thermal_offset_k` (parked-cold = 4A exactly); the builder publishes T_air and B per stencil after each IR frame (`FEntityThermalEnv`), latched into each entity through the stencil it holds at that build (`FEntityThermalLatch`; D = 0 without one, never a stencil's later owner's B). Inputs are canonical Component Control 10/11/12 (damage, power plant, flaming); DIS appearance bits 3-4/22/15 are mapped onto them on change; moving (horizontal ECEF speed, windowed) also counts as running. Hot spots are body-frame volumes (`entity_types.<id>.thermal_parts`, X fwd / Y right / Z down): 256 per-stencil records in `FThermalFrameParams::EntityRecords`, whose translation is filled on the render thread from the entity origin + `PreViewTranslation` in doubles (`FinalizeEntityRecords`); `ThermalCS`'s `EntityPartTemp` and `CamSimThermalRef::EntityPartTemp` must change together (`CamSim.GPU.Thermal.EntityPartsMatchesCpu`). Invalid records fall back to 4A's stencil table, so `thermal.entity.enabled: false` is 4A bit for bit (incl. its +8 K vehicle default). A spawn defers its first step until the speed is measured (0.5 s), so a vehicle appearing mid-drive starts running. The IR AGC band stops at full well, so a burning (clipped) vehicle displays white
- **Shaders** live in `unreal_project/CamSimTest/Shaders/` (virtual path `/CamSim`), compiled by the `CamSimShaders` module (`PostConfigInit`)
- **GPU pass timing on Metal**: `RQT_AbsoluteTime` render queries resolve to the command buffer's end time truncated to whole seconds, so they can't time a pass. Use an `RDG_EVENT_SCOPE_STAT` with an `FGPUStat` subclass (`OnTimingResults`) as `Camera/SensorGpuTimer.h` does. The stat scope is timed by the encoders that *begin inside it*: if RDG keeps the pass in an encoder opened earlier, it reads the whole encoder (ThermalCS read ~7.3 ms at any resolution). Put never-culled 1-texel copies around the pass so it gets its own encoder
- **Health port restart**: restarting CamSim within ~30 s of the last run finds :8080 in TIME_WAIT. The health server logs "port 8080 is busy … NOT listening" and retries every 2 s until it binds (no reuse flag: UE's only option also sets SO_REUSEPORT, which would let two CamSims share the port). `run_bench.py` still waits the port out before launching
- **HITL packet 201**: CIGI user-defined packet 201 (Platform Kinematics, `hitl/PROTOCOL.md` §2) feeds KLV Tags 2/8/9/56/64/79/80; the receiver publishes each datagram's camera packets as one `FCigiCameraFrame` (`CameraFrameQueue`), so pose and gimbal always come from the same datagram. Change the parser and `hitl/camsim_hitl/cigi.py` together
- **pymavlink `get_payload()` is wrong on MAVLink 2 frames** (it assumes the v1 header); the IG host takes the payload from `get_msgbuf()[10:10+len]`
- **IDE false positives**: clang diagnostics for UE types are wrong — UBT handles includes at build time
- **Docker networking**: `network_mode: host` required for UDP multicast routing
- **Hybrid CPUs (P/E cores)**: `deploy/entrypoint.sh` and `scripts/run.sh` (Linux) pin UE to the P-cores (`taskset`, `/sys/devices/cpu_core/cpus`; `CAMSIM_PIN_PCORES=0` to disable). Unpinned, the game thread can sit on an E-core for a whole run (~45% slower), which looked like run-to-run benchmark noise
- **Linux UDP receive buffers**: each IDR goes out as one burst (~180 KB with NVENC at 4 Mbit/s), larger than Linux's default 208 KB socket buffer, so receivers drop it and the keyframe decodes corrupt. Raise `net.core.rmem_max` (`sudo sysctl -w net.core.rmem_max=26214400`; persist in `/etc/sysctl.d/`); `check.js` requests 16 MB and warns when capped. Proper fix tracked as ROADMAP 1.14
- **NVENC on Linux** needs only nv-codec-headers at build time (`build_thirdparty.sh` pins and installs them), not CUDA; FFmpeg dlopens `libnvidia-encode` at runtime and `CAMSIM_ENCODER=auto` falls back to libx264 without it
- **rapidyaml bundled**: Source in `Config/ryml/` — excluded from pre-commit linting
- **KLV reference decoder is misb.js**: [vidterra/misb.js](https://github.com/vidterra/misb.js) (v0.1.30, commit `52c3837`) is what parses our KLV downstream, so it is the gold standard for ST 0601/0102 encoding — not the spec PDF, and not `validate_klv.py`. Checksum = 16-bit running sum over everything from the UL key through the checksum's own `01 02` bytes. misb.js throws on any tag shorter than its expected size (the whole packet is lost) and marks bad checksums `valid: false` rather than throwing. See ROADMAP.md 0.3. Never capture or extract KLV with ffmpeg: up to at least 6.1 (Ubuntu 24.04's) its demuxer strips 5 bytes from every stream_id 0xFC KLV packet (taking our stream_type 0x06 for ST 1402 synchronous metadata), which eats the UL key. `check.js` demuxes TS itself (`mpegts.js`) and `check.js capture` records raw UDP.
- **RHI readback is render-thread only**: `FRHIGPUTextureReadback::IsReady()/Lock()/Unlock()` assert `IsInRenderingThread()`. Use `ENQUEUE_RENDER_COMMAND` + `FlushRenderingCommands()` for synchronous game-thread access (see CamSimCamera.cpp Phase 1 readback pattern)
- **UE5 TAtomic uses EMemoryOrder, except `Exchange`**: `TAtomic<T>::Load()/Store()` take `EMemoryOrder` enum, NOT `std::memory_order`. But `TAtomic<T>::Exchange(T)` is single-arg in 5.7 and 5.8 (delegates to `std::atomic::exchange`, default seq_cst) — passing `EMemoryOrder` to `Exchange` is a compile error.
- **HTTP health server is on by default**: `operational.health_http_enabled` defaults to `true` for sim-environment REST orchestrator compatibility. Binds `0.0.0.0:8080`. To disable: `CAMSIM_HEALTH_HTTP_ENABLED=0`. `/live` and `/health` are route aliases pointing at the same 5-second watchdog. K8s probes that target `/live` still work unchanged.
- **HTTP automation tests need two tickers** (load-bearing test invariant — if you skip this, the test hangs silently instead of failing):
  - `FCamSimHealthServer` is built on `FHttpServerModule`. The **listener** is pumped by `FTSTicker::GetCoreTicker()`.
  - The HTTP **client** (`FHttpModule::Get().ProcessRequest`) is pumped by `FHttpModule::Get().GetHttpManager()`.
  - These are two independent tickers. An automation test running both ends has to tick BOTH inside its wait loop:
    ```cpp
    FHttpModule::Get().GetHttpManager().Tick(DeltaSec);
    FTSTicker::GetCoreTicker().Tick(DeltaSec);
    ```
  - Ticking only the HTTP manager means the server never accepts connections; ticking only the core ticker means the client never dispatches the request. See [`Tests/HttpServerLifecycleTest.cpp`](unreal_project/CamSimTest/Source/CamSimTest/Tests/HttpServerLifecycleTest.cpp) for a working template.

## Environment

Config via `deploy/camsim_config.yaml` or env vars (env takes precedence).
The full reference — every key, env var, default, and phase annotation —
lives in [`docs/configuration.md`](docs/configuration.md). That is the
single source of truth; keep new env vars documented there, not here.

Only the vars you need at the console every day are listed below:

- `CAMSIM_CIGI_PORT` — CIGI listen port (default 8888)
- `CAMSIM_MULTICAST_ADDR` — output address (default 239.1.1.1)
- `CAMSIM_ENCODER` — `auto` | `nvenc` | `videotoolbox` | `libx264` (default auto: NVENC → libx264; `videotoolbox` is opt-in on macOS)
- `CAMSIM_HEALTH_HTTP_ENABLED` / `CAMSIM_HEALTH_HTTP_PORT` — K8s probe server (default 1, 8080)
- `CAMSIM_STRUCTURED_LOG_PATH` — JSONL logs for ELK/Datadog (empty = disabled)

## Docker

Guide: [`docs/docker.md`](docs/docker.md). Verified on RTX 5080 / driver 595 / toolkit 1.20 (2026-10-01).

```bash
scripts/package_for_docker.sh && docker compose -f deploy/docker-compose.yml up --build
scripts/ci_validate.sh --docker camsim:latest   # end-to-end check on the GPU
```

- Packaged Development build on Ubuntu 24.04; the NVIDIA driver comes from the host via the Container Toolkit (`NVIDIA_DRIVER_CAPABILITIES=all`: `graphics` = Vulkan, `video` = NVENC). Never install `libnvidia-*` in the image
- The image needs `libegl1`: the NVIDIA Vulkan ICD dlopens `libEGL.so.1`, which the toolkit doesn't inject (else `ERROR_INCOMPATIBLE_DRIVER`)
- The entrypoint detects the GPU by `/dev/nvidia*`: under CDI injection `NVIDIA_VISIBLE_DEVICES` reads `void` even with `--gpus all`. No GPU → it refuses to start: lavapipe crashes compiling UE 5.8 SM6 pipelines (`CAMSIM_ALLOW_SOFTWARE_RENDERING=1` tries anyway)
- Non-root user (uid 1000); `-userdir=/var/lib/camsim` puts `Saved/` and Cesium's tile cache on a volume
- Never let BuildCookRun stage into `Saved/StagedBuilds/` (its default): once that directory exists, `run.sh` silently switches to packaged Shipping mode, so `--build-only` packages instead of building the editor and the bench/check scripts launch a stale package. `package_for_docker.sh` stages under `.cache/staging`
- Assets loaded by path at runtime aren't found by the cook: list their directories in `DirectoriesToAlwaysCook` (`DefaultGame.ini`). The Game target needs `bEnableExceptions` (Editor targets force it on), so a packaging break can hide behind a clean editor build
- Health: HTTP server on port `8080` exposes `GET /live`, `GET /health` (alias for `/live`), `GET /ready`, and `GET /metrics` (Prometheus format).
