# CamSim Roadmap

## Plan metadata
- **Target file**: `Plan.md` (repo root)
- **Priorities**: ISR simulation + ML training data equally important
- **Sensor focus**: EO + IR primary, NVG secondary
- **All phases retained**, ordered by impact

## Implementation plan

Write the following content to `/Users/mclayton/developer/camsim/Plan.md`:

---

# CamSim Roadmap — High-Quality Synthetic Video Generator

## Current State (Phases 1–14 Complete)

Production-grade synthetic sensor simulator: CIGI 3.3 input (13 opcodes) → EO/IR/NVG sensor
simulation → H.264/H.265 MPEG-TS + MISB ST 0601 KLV → UDP multicast. Async GPU readback,
encoder watchdog, multi-view output, CIGI recording/playback, entity rendering with dead-reckoning,
gimbal slew-rate limiting, terrain feedback (HAT/HOT/LOS), Docker deployment, CI/CD, 23 unit tests.

~10K LOC | 30+ source files | Cesium + UE5.7

---

## Phase 15 — Optical Realism

Make output look like real camera footage, not a game engine render.

| Item                         | Description                                                                                    | Effort |
| ---------------------------- | ---------------------------------------------------------------------------------------------- | ------ |
| **15A** Motion Blur          | Enable on SceneCapture; platform velocity → per-pixel exposure smear                           | S      |
| **15B** Lens Distortion      | Barrel/pincushion warp via post-process material (radial polynomial coefficients configurable) | S      |
| **15C** Bloom & Glare        | Lens scatter halos on bright sources; enable ShowFlags.SetBloom() + luminance threshold        | S      |
| **15D** Chromatic Aberration | RGB channel displacement toward frame edges (radial offset)                                    | S      |
| **15E** Depth of Field       | Focal plane + aperture simulation; configurable focus distance (infinity default)              | M      |
| **15F** Lens Flare           | Procedural flare triggered by bright pixel clusters (sun, specular highlights)                 | M      |

**Files**: `Camera/CamSimCamera.cpp`, new UE post-process materials
**Validation**: Compare frames to real DJI/FLIR footage; motion blur visible on fast gimbal pan

---

## Phase 16 — Sensor Fidelity

IR output matches real LWIR FPA detector behavior. EO gains realistic camera artifacts.

| Item                             | Description                                                                             | Effort |
| -------------------------------- | --------------------------------------------------------------------------------------- | ------ |
| **16A** Quantization & Bit Depth | 14-bit thermal → 8-bit display with dither; configurable A/D bit depth                  | S      |
| **16B** Hot/Dead Pixels          | Procedural defect map per detector spec (~50–100 per Mpixel)                            | S      |
| **16C** MTF Degradation          | Gaussian PSF frequency-domain blur (replaces box filter); configurable cutoff           | M      |
| **16D** Thermal Drift            | IR baseline shift over time; periodic auto-NUC (non-uniformity correction) reset events | S      |
| **16E** Auto-Exposure Lag        | Histogram-driven gain with 1–3 frame convergence delay                                  | M      |
| **16F** Rolling Shutter          | Per-row temporal offset simulating CMOS sequential readout                              | L      |
| **16G** Platform Vibration       | Subpixel random displacement per frame (microdynamics simulation)                       | S      |
| **16H** Sensor Gain/Offset Noise | Electronic instability in IR detector bias voltage                                      | S      |
| **16I** Sun Glint (EO)           | Specular reflections from water/metal based on sun-camera-surface geometry              | M      |

**Files**: `Sensor/SensorPostProcess.cpp`, `Camera/CamSimCamera.cpp`, `Config/CamSimConfig.h`
**Validation**: Compare IR noise floor to real FLIR Boson/Tau2 NETD specs; verify NUC event visible in recording

---

## Phase 17 — ML Training Data Generation

Export rich ground-truth annotations alongside video for computer vision model training.

| Item                          | Description                                                                                            | Effort |
| ----------------------------- | ------------------------------------------------------------------------------------------------------ | ------ |
| **17A** Depth Map Output      | Second SceneCaptureComponent2D with `SCS_SceneDepth`; export 16-bit PNG per frame                      | M      |
| **17B** Semantic Segmentation | Custom stencil buffer with per-material class IDs (terrain, building, water, vehicle, sky, vegetation) | L      |
| **17C** Instance Segmentation | Per-entity unique color ID in stencil buffer; mapped back to entity type table                         | L      |
| **17D** 2D Bounding Boxes     | Project entity mesh OBB to screen space; per-frame annotation file                                     | M      |
| **17E** 3D Bounding Boxes     | Entity OBB in geodetic coordinates + camera intrinsics matrix for 3D detection                         | M      |
| **17F** Optical Flow          | Dense motion vectors via UE velocity buffer; export as .flo or 16-bit PNG pairs                        | L      |
| **17G** COCO JSON Export      | Standard annotation format (images + annotations + categories) for direct ML ingestion                 | M      |
| **17H** Pascal VOC XML        | Alternative annotation format for legacy toolchains                                                    | S      |
| **17I** Dataset Randomization | Stochastic weather, TOD, entity spawn variance; batch generation mode                                  | M      |

**Files**: new `GroundTruth/` module, `Camera/CamSimCamera.cpp`, `Entity/CamSimEntity.cpp`
**Validation**: Load exported COCO JSON in FiftyOne/CVAT; verify bounding boxes align with entities; depth map matches LOS slant range

---

## Phase 18 — Weather & Atmosphere

Dynamic, physically-based atmospheric effects responding to CIGI weather packets.

| Item                           | Description                                                                              | Effort |
| ------------------------------ | ---------------------------------------------------------------------------------------- | ------ |
| **18A** Volumetric Fog         | 3D density-aware fog (not just height fog); responds to CIGI Atmosphere visibility field | M      |
| **18B** Rain & Snow Particles  | Niagara particle FX driven by CIGI Weather precipitation type/intensity                  | M      |
| **18C** Dynamic Cloud Shadows  | Moving shadow projections from cloud layer onto terrain                                  | M      |
| **18D** God Rays               | Volumetric light shafts through clouds/atmosphere                                        | S      |
| **18E** Atmospheric Scattering | Rayleigh/Mie wavelength-dependent color shift (blue haze at distance, warm at horizon)   | M      |
| **18F** IR-Specific Atmosphere | Wavelength-dependent IR extinction (LWIR 8–12μm band); real MODTRAN-like lookup          | M      |
| **18G** Regional Weather       | CIGI opcode 13 support — localized weather zones with boundaries                         | L      |

**Files**: `Environment/CamSimEnvironment.cpp`, Niagara systems in UE editor
**Validation**: Visible rain/fog in output video; IR extinction increases with range; cloud shadows move across terrain

---

## Phase 19 — HUD/OSD Symbology

Burned-in sensor display symbology matching real ISR platform output.

| Item                               | Description                                                                         | Effort |
| ---------------------------------- | ----------------------------------------------------------------------------------- | ------ |
| **19A** Crosshair/Reticle          | Configurable targeting crosshair at frame center (multiple styles)                  | S      |
| **19B** Azimuth/Elevation Readout  | Numeric gimbal angles on-screen                                                     | S      |
| **19C** FOV/Zoom Indicator         | Current zoom level / FOV arc display                                                | S      |
| **19D** Slant Range Display        | Computed range overlay (from terrain LOS)                                           | S      |
| **19E** Timestamp & Classification | UTC time + MISB ST 0102 classification banner (top/bottom)                          | S      |
| **19F** Compass Rose               | Heading indicator on screen edge                                                    | M      |
| **19G** Configurable Layout        | YAML-driven symbology placement, font size, color, per-element enable/disable       | M      |
| **19H** Symbology Toggle           | Config flag to render with/without symbology (clean frames for ML, overlay for ISR) | S      |

**Files**: new `Overlay/` module (UMG widgets or direct pixel draw before encode)
**Validation**: Visual comparison to real FLIR/L3Harris sensor display; verify symbology disabled = clean frame for ML

---

## Phase 20 — Multi-Sensor Platform

Multiple independent sensors on one platform (e.g., EO daylight + LWIR on same gimbal).

| Item                         | Description                                                                                | Effort |
| ---------------------------- | ------------------------------------------------------------------------------------------ | ------ |
| **20A** Multi-Camera Actors  | Spawn N ACamSimCamera actors with independent gimbal & sensor mode                         | L      |
| **20B** Per-View Sensor Mode | Each output view has its own EO/IR pipeline instance                                       | M      |
| **20C** Independent Gimbals  | Each camera has separate gimbal state, slew limits, CIGI View Control routing              | L      |
| **20D** PIP Compositing      | Picture-in-picture: wide FOV + narrow FOV composited into single output stream             | M      |
| **20E** Stereo Pair          | Baseline-separated cameras for 3D reconstruction / stereo ML training                      | M      |
| **20F** Per-Camera KLV       | Independent ST 0601 metadata streams per output (separate gimbal angles, FOV, sensor mode) | M      |

**Files**: `Camera/CamSimCamera.h/.cpp`, `Subsystem/CamSimSubsystem.cpp`, `Encoder/MultiViewFrameSink.cpp`
**Validation**: Two simultaneous UDP streams with different sensor modes; verify independent gimbal control via CIGI

---

## Phase 21 — Scenario Engine

Complex scripted scenarios without external CIGI host. Enables batch dataset generation.

| Item                          | Description                                                                          | Effort |
| ----------------------------- | ------------------------------------------------------------------------------------ | ------ |
| **21A** Waypoint Trajectories | Entity follows lat/lon/alt waypoints with speed/acceleration curves                  | M      |
| **21B** Event Triggers        | Actions fired on conditions (entity visible, range < threshold, timer, frame count)  | M      |
| **21C** Formation Flying      | Multiple entities maintain relative positions with configurable offsets              | M      |
| **21D** Randomization Engine  | Stochastic entity count, spawn area, weather, TOD for robustness training            | M      |
| **21E** Batch Runner          | Run N scenarios unattended; output ground truth + video per scenario; summary report | M      |
| **21F** Mission Scripting     | Lua or Python scripting API for complex multi-phase missions                         | L      |

**Files**: new `Scenario/` module, extend `Config/CamSimConfig.cpp`
**Validation**: Run 10-scenario batch; verify each produces unique video + ground truth; entity paths match YAML waypoints

---

## Phase 22 — Operational Hardening

Production-grade reliability for 24/7 deployment and developer velocity.

| Item                               | Description                                                                              | Effort |
| ---------------------------------- | ---------------------------------------------------------------------------------------- | ------ |
| **22A** Unit Tests in CI           | Run existing 23 UE automation tests in GitHub Actions (add UE test runner step)          | M      |
| **22B** Integration Tests          | End-to-end CIGI→video→KLV validation in CI with headless Docker                          | M      |
| **22C** Structured JSON Logging    | Machine-parseable log output for ELK/Datadog aggregation                                 | M      |
| **22D** HTTP Health Endpoints      | `/ready` and `/live` for Kubernetes probes (supplement file-based health)                | M      |
| **22E** Config Validation          | Pre-flight range checks on all config values; meaningful error messages with field names | S      |
| **22F** Graceful Degradation       | Reduce resolution or disable effects under GPU/CPU pressure instead of crashing          | L      |
| **22G** Encoder Reconnection       | Socket reopen on UDP failure (3 retries); failover to local .ts recording                | M      |
| **22H** Performance Regression CI  | Track encode latency + frame drops across commits; gate merge on regression              | L      |
| **22I** Per-Frame Latency Tracking | Timestamp each pipeline stage (readback, scale, encode, send); export P50/P95/P99        | M      |
| **22J** Troubleshooting Guide      | Common issues, root causes, fixes for deployment/integration problems                    | S      |

**Files**: `Tests/`, `.github/workflows/ci.yml`, `Subsystem/CamSimSubsystem.cpp`, `Encoder/VideoEncoder.cpp`
**Validation**: CI pipeline passes with all 23+ tests green; health endpoint returns JSON; latency metrics visible in Prometheus

---

## Phase 23 — Standards Compliance

Full interoperability with ISR ecosystem tools and standards validators.

| Item                           | Description                                                                                 | Effort |
| ------------------------------ | ------------------------------------------------------------------------------------------- | ------ |
| **23A** ST 0601 Missing Tags   | Tag 4 (frame #), 26 (target width), 31–34 (microdynamics), 42–45 (target location/accuracy) | M      |
| **23B** BCC-16 Checksum        | Switch from CRC-16/CCITT to standard BCC-16 (or make configurable for compat)               | S      |
| **23C** STANAG 4609 Validation | Verify PAT/PMT structure, PID allocation, KLV sync timing per spec                          | M      |
| **23D** Remaining CIGI Opcodes | Opcode 3 (conformal clutter), 7 (collision detection), 13 (regional weather)                | M      |
| **23E** KLV Uncertainty Tags   | Tags 27–30 (slant range, cross-range, HFOV, VFOV uncertainty values)                        | S      |
| **23F** MISB ST 0903 VMTI      | Video Moving Target Indicator metadata for tracked entities                                 | L      |

**Files**: `Metadata/KlvBuilder.cpp`, `CIGI/CigiReceiver.cpp`, `CIGI/CigiPacketTypes.h`
**Validation**: Pass MISB ST 0601 compliance checker; validate with external KLV decoder (e.g., impleotv, Kitware KWIVER)

---

## Phase 24 — Performance & GPU Optimization

Maximize throughput and minimize glass-to-glass latency.

| Item                               | Description                                                                       | Effort |
| ---------------------------------- | --------------------------------------------------------------------------------- | ------ |
| **24A** GPU Sensor Pipeline        | Move IR/NVG effects to post-process materials (eliminate CPU pixel copy entirely) | L      |
| **24B** Ray Tracing Integration    | Enable RTX reflections/shadows on SceneCapture for Cesium terrain                 | M      |
| **24C** Frame Drop Categorization  | Track drops by cause: encoder busy, readback timeout, socket error; log + metric  | S      |
| **24D** Shadow Quality Tuning      | Virtual shadow map cascade optimization for high-altitude ISR viewing angles      | S      |
| **24E** DDC Pre-warming            | Script to compile base shaders offline; reduce cold-start from ~180s to ~60s      | S      |
| **24F** Hot-Reload Config          | Apply gimbal limits, bitrate, sensor mode changes without restart                 | L      |
| **24G** Tile Prefetch Optimization | Predictive tile loading based on entity trajectory and gimbal sweep pattern       | M      |

**Files**: `Sensor/SensorPostProcess.cpp`, `Camera/CamSimCamera.cpp`, UE post-process materials
**Validation**: Benchmark before/after: encode latency P99 < 15ms; zero frame drops at 30fps sustained; cold start < 90s

---

## Recommended Execution Order

```
Sprint 1 ──── Phase 15 (Optical Realism)           ← biggest visual quality jump
              Phase 22A-E (CI tests, config validation, structured logging)

Sprint 2 ──── Phase 16A-E,G,H (Sensor Fidelity, skip rolling shutter)
              Phase 18A-D (Core weather effects)

Sprint 3 ──── Phase 17A-D,G (Depth, segmentation, bounding boxes, COCO export)
              Phase 19 (HUD/OSD symbology)

Sprint 4 ──── Phase 18E-F (Atmospheric scattering, IR atmosphere)
              Phase 16F,I (Rolling shutter, sun glint)
              Phase 23A-B,E (ST 0601 tag gaps, checksum fix)

Sprint 5 ──── Phase 17E-F,I (3D boxes, optical flow, randomization)
              Phase 22F-I (Graceful degradation, encoder reconnect, latency tracking)

Sprint 6 ──── Phase 20 (Multi-sensor platform)
              Phase 21 (Scenario engine)

Sprint 7 ──── Phase 23C-D,F (STANAG validation, remaining CIGI opcodes, VMTI)
              Phase 24 (GPU optimization, ray tracing, hot reload)
```

## Effort Key

- **S** = Small (1–3 days)
- **M** = Medium (3–7 days)
- **L** = Large (1–2 weeks)
