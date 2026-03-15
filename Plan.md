# CamSim Roadmap — VRSG Feature Parity

Target: feature parity with MetaVR VRSG for ISR sensor simulation, entity rendering,
environmental effects, and interoperability — plus ML training data generation (beyond VRSG).

---

## Implemented (Phases 1–15)

| Phase | Feature                                                                                                                                      | Status          |
| ----- | -------------------------------------------------------------------------------------------------------------------------------------------- | --------------- |
| 1–6   | Core pipeline: CIGI input, Cesium terrain, H.264/KLV MPEG-TS output                                                                          | ✅ Done          |
| 7     | Environment: day/night cycle, sky atmosphere, fog, cloud/weather layers                                                                      | ✅ Done          |
| 8     | Entity rendering: aircraft/vehicles, dead-reckoning, articulated parts, lights                                                               | ✅ Done          |
| 9     | Gimbal & sensor: 3-DOF gimbal, slew limits, FOV presets, polarity                                                                            | ✅ Done          |
| 10    | Terrain feedback: HAT/HOT and LOS line traces, SOF heartbeat, IG→Host UDP                                                                    | ✅ Done          |
| 11    | Sensor simulation: EO/IR/NVG post-process, noise, vignetting, extinction                                                                     | ✅ Done          |
| 12A   | MISB ST 0102 security classification in every KLV packet                                                                                     | ✅ Done          |
| 12B   | STANAG 4609: PID allocation, KLV rate, PTS sync, H.265/HEVC encoder                                                                          | ✅ Done          |
| 12C   | Multi-channel: multiple simultaneous output streams with digital zoom                                                                        | ✅ Done          |
| 12D   | Health & monitoring: health file, Prometheus metrics, IG mode in SOF                                                                         | ✅ Done          |
| 12E   | Recording & playback: CIGI recording/replay, local .ts video recording                                                                       | ✅ Done          |
| 13    | Code hardening: 29 unit tests, Pimpl pattern, error propagation                                                                              | ✅ Done          |
| 14    | CI/CD: GitHub Actions, pre-commit hooks, Docker integration test                                                                             | ✅ Done          |
| 15    | Optical realism: motion blur, lens distortion, bloom, chromatic aberration, DoF                                                              | ✅ Done          |
| 17*   | ML training data: depth maps (17A), 2D bboxes (17D), COCO JSONL (17G), VOC (17H)                                                             | ✅ Sprint 1 Done |
| 18*   | Weather/atmosphere: second fog (18C), precipitation overlay (18D), god rays (18E), atmospheric scattering (18J), dynamic IR extinction (18K) | ✅ Sprint 1 Done |
| 20*   | HUD/OSD: crosshair (20A), Az/El readout (20B), FOV indicator (20C), slant range (20D), timestamp/classification (20E), toggle (20I)        | ✅ Sprint 1 Done |

## CIGI 3.3 Packet Coverage

| Opcode | Packet                    | Status                   |
| ------ | ------------------------- | ------------------------ |
| 1      | IG Control                | ✅ Parsed (sender)        |
| 2      | Entity Control            | ✅ Full multi-entity      |
| 4      | Component Control         | ✅ Lights / damage states |
| 6      | Articulated Part Control  | ✅ Skeletal mesh bones    |
| 8      | Rate Control              | ✅ Dead-reckoning         |
| 9      | Celestial Sphere Control  | ✅ Sun/moon/stars         |
| 10     | Atmosphere Control        | ✅ Fog / visibility       |
| 12     | Weather Control           | ✅ Cloud / precipitation  |
| 16     | View Control              | ✅ Gimbal pan/tilt        |
| 17     | Sensor Control            | ✅ EO/IR/NVG polarity     |
| 20     | View Definition           | ✅ FOV presets            |
| 24     | HAT/HOT Request           | ✅ Terrain line trace     |
| 25–26  | LOS Request               | ✅ Line-of-sight query    |
| 101    | Start of Frame (response) | ✅ SOF + IG mode          |
| 102    | HAT/HOT Response          | ✅ Terrain height reply   |
| 103    | LOS Response              | ✅ LOS reply              |

---

## VRSG Feature Gap Analysis

Features VRSG has that CamSim lacks, organized by priority:

| VRSG Capability                       | CamSim Status       | Addressed In |
| ------------------------------------- | ------------------- | ------------ |
| IR AGC (radiance-based) + level/gain  | ✅ Done              | Phase 16     |
| IR AC banding, hot/dead pixels, MTF   | ✅ Done              | Phase 16     |
| Volumetric ray-traced clouds          | ✅ Done              | Phase 18     |
| Volumetric precipitation (rain/snow)  | ✅ CPU overlay done  | Phase 18     |
| Particle FX (dust, rotor wash, smoke) | ✅ Done              | Phase 18     |
| Dynamic terrain cratering             | ✅ Done              | Phase 18     |
| 3D ocean (waves, wakes, sea states)   | Missing             | Phase 19     |
| HUD/OSD burn-in overlays              | Missing             | Phase 20     |
| DIS protocol support                  | Missing (CIGI only) | Phase 21     |
| Character animation / FPS mode        | Missing             | Phase 22     |
| Pattern-of-life scenarios             | Missing             | Phase 23     |
| Laser rangefinder/designator viz      | Missing             | Phase 21     |
| ATAK/ROVER FMV streaming              | Missing             | Phase 21     |
| Light point system (thousands)        | Basic UE lights     | Phase 19     |
| Object-on-object dynamic shadows      | Missing             | Phase 24     |
| VR/HMD headset rendering              | Missing             | Phase 25     |
| Scenario editor GUI                   | Missing             | Phase 23     |
| Entity model library (hundreds)       | Small set           | Phase 22     |
| After-action review / DIS log replay  | CIGI replay only    | Phase 21     |
| Radar simulation (SAR/ISAR)           | Missing             | Phase 26     |
| Edge blending / dome display          | Missing             | Phase 25     |
| FBX/OpenFlight model import           | Missing             | Phase 22     |
| NVG IR pointer                        | ✅ Done              | Phase 16     |

---

## Phase 16 — Sensor Fidelity

IR output matches real LWIR FPA detector behavior. Closes the biggest sensor gap vs VRSG.

| Item                             | Description                                                                    | Effort | VRSG Parity |
| -------------------------------- | ------------------------------------------------------------------------------ | ------ | ----------- |
| **16A** Radiance-Based AGC       | IR auto gain control from scene radiance histogram; manual level/gain override | M      | ✓ ✅ Done    |
| **16B** Quantization & Bit Depth | 14-bit thermal → 8-bit display with dither; configurable A/D bit depth         | S      | ✓ ✅ Done    |
| **16C** Hot/Dead Pixels          | Procedural defect map per detector spec (~50–100 per Mpixel)                   | S      | ✓ ✅ Done    |
| **16D** MTF Degradation          | Gaussian PSF separable blur; configurable sigma                                | M      | ✓ ✅ Done    |
| **16E** Thermal Drift            | IR baseline shift over time; periodic auto-NUC reset events                    | S      | ✓ ✅ Done    |
| **16F** AC Banding               | Simulated AC coupling artifacts in IR detector readout                         | S      | ✓ ✅ Done    |
| **16G** Auto-Exposure Lag        | Histogram-driven gain with 1–3 frame convergence delay                         | M      | ✓ ✅ Done    |
| **16H** Rolling Shutter          | Per-row temporal offset simulating CMOS sequential readout                     | L      | ✓ ✅ Done    |
| **16I** Platform Vibration       | Subpixel random displacement per frame (microdynamics)                         | S      | ✓ ✅ Done    |
| **16J** Sensor Gain/Offset Noise | Electronic instability in IR detector bias voltage                             | S      | ✓ ✅ Done    |
| **16K** Sun Glint (EO)           | Specular reflections from water/metal based on sun-camera-surface geometry     | M      | ✓ ✅ Done    |
| **16L** NVG IR Pointer           | Visible IR laser dot in NVG mode for night target marking                      | S      | ✓ ✅ Done    |

**Files**: `Sensor/SensorPostProcess.cpp`, `Sensor/SensorTypes.h`, `Config/CamSimConfig.cpp`, `Tests/SensorFidelityTest.cpp`, `deploy/camsim_config.yaml`
**Validation**: Compare IR noise floor to real FLIR Boson/Tau2 NETD specs; verify AGC matches VRSG radiance-based behavior

---

## Phase 17 — ML Training Data Generation

Export rich ground-truth annotations alongside video. **Beyond VRSG** — differentiator.

| Item                          | Description                                                                           | Effort |
| ----------------------------- | ------------------------------------------------------------------------------------- | ------ |
| **17A** Depth Map Output      | ✅ Second SceneCaptureComponent2D with `SCS_SceneDepth`; export 16-bit PNG per frame   | M      |
| **17B** Semantic Segmentation | Custom stencil buffer with per-material class IDs (terrain, building, water, vehicle) | L      |
| **17C** Instance Segmentation | Per-entity unique color ID in stencil buffer; mapped to entity type table             | L      |
| **17D** 2D Bounding Boxes     | ✅ Project entity mesh AABB to screen space; per-frame annotation file                 | M      |
| **17E** 3D Bounding Boxes     | Entity OBB in geodetic coordinates + camera intrinsics matrix                         | M      |
| **17F** Optical Flow          | Dense motion vectors via UE velocity buffer; export as .flo or 16-bit PNG pairs       | L      |
| **17G** COCO JSON Export      | ✅ Standard JSONL annotation format for direct ML ingestion                            | M      |
| **17H** Pascal VOC XML        | ✅ Alternative annotation format for legacy toolchains                                 | S      |
| **17I** Dataset Randomization | Stochastic weather, TOD, entity spawn variance; batch generation mode                 | M      |

**Sprint 1 status**: 17A, 17D, 17G, 17H implemented and reviewed (Phase 17 Sprint 1 complete).
17B, 17C, 17E, 17F, 17I remain for future sprints.

**Files created/modified**:
- `GroundTruth/FEntityProjection.h/.cpp` — `BuildViewProjectionMatrix`, `ProjectAABB`
- `GroundTruth/AnnotationTypes.h` — `FEntityAnnotationData`
- `GroundTruth/IAnnotationWriter.h` — pure-virtual writer interface
- `GroundTruth/FCocoAnnotationWriter.h/.cpp` — streaming COCO JSONL (persistent IFileHandle)
- `GroundTruth/FVocAnnotationWriter.h/.cpp` — Pascal VOC XML per frame
- `GroundTruth/FDepthMapWriter.h/.cpp` — 16-bit PNG depth (ImageWrapper)
- `GroundTruth/FGroundTruthCollector.h/.cpp` — orchestrator owned by UCamSimSubsystem
- `Camera/CamSimCamera.h/.cpp` — depth capture component, async readback, entity snapshot
- `Config/CamSimConfig.h` — `FMLTrainingConfig` struct
- `deploy/camsim_config.yaml` — `ml_training:` block
- `Tests/GroundTruthTest.cpp` — 7 automation tests
- `CamSimTest.Build.cs` — added `ImageWrapper` dependency

**Validation**: Load COCO JSONL in FiftyOne/CVAT; bounding boxes align with entities; depth matches LOS slant range

---

## Phase 18 — Weather, Atmosphere & Particle Effects

VRSG's volumetric clouds, precipitation, and particle system are a major visual gap.

| Item                           | Description                                                                | Effort | VRSG Parity | Status          |
| ------------------------------ | -------------------------------------------------------------------------- | ------ | ----------- | --------------- |
| **18A** Volumetric Clouds      | Ray-marched volumetric clouds; respond to CIGI Weather layer type/coverage | L      | ✓           | ✅ Sprint 2 Done |
| **18B** Dynamic Cloud Shadows  | Moving shadow projections from cloud layer onto terrain                    | M      | ✓           | ✅ Sprint 2 Done |
| **18C** Second Fog Layer       | Low-lying ground mist via UE ExponentialHeightFog SecondFogData            | M      | ✓           | ✅ Sprint 1 Done |
| **18D** Rain & Snow Overlay    | CPU pixel-pass precipitation (rain streaks / snow dots) on task thread     | M      | ✓           | ✅ Sprint 1 Done |
| **18E** God Rays               | UE DirectionalLight bloom + occlusion light shafts                         | S      | ✓           | ✅ Sprint 1 Done |
| **18F** Dust & Rotor Wash      | Niagara particle FX: dust trails, rotor downwash, blown sand/snow          | M      | ✓           | ✅ Sprint 2 Done |
| **18G** Tactical Smoke/Fire    | Smoke plumes, fire effects, explosions as entity-attached Niagara systems  | M      | ✓           | ✅ Sprint 2 Done |
| **18H** Contrails              | Aircraft engine exhaust trails responding to altitude/temperature          | S      | ✓           | ✅ Sprint 2 Done |
| **18I** Dynamic Cratering      | Terrain deformation from munitions impact (runtime mesh modification)      | L      | ✓           | ✅ Sprint 2 Done |
| **18J** Atmospheric Scattering | USkyAtmosphereComponent Rayleigh/Mie coefficient overrides                 | M      |             | ✅ Sprint 1 Done |
| **18K** IR-Specific Atmosphere | Dynamic IR extinction from atmospheric visibility (Koschmieder)            | M      |             | ✅ Sprint 1 Done |
| **18L** Regional Weather Zones | CIGI opcode 13 — localized weather zones with boundaries                   | L      | ✓           | ✅ Sprint 2 Done |

**Sprint 1 files**: `Config/CamSimConfig.h`, `Config/CamSimConfig.cpp`, `Metadata/KlvBuilder.h`,
  `Environment/CamSimEnvironment.h/.cpp`, `Camera/CamSimCamera.h/.cpp`,
  `Sensor/SensorPostProcess.h/.cpp`, `Tests/Phase18Test.cpp`, `deploy/camsim_config.yaml`
**Sprint 1 validation**: Visible rain/fog in output; IR extinction attenuates under simulated haze; god rays visible with high-contrast sun scenes
**Sprint 2 files**: `CIGI/CigiPacketTypes.h`, `CIGI/CigiReceiver.cpp`,
  `Environment/CamSimParticleManager.h/.cpp`, `Subsystem/CamSimSubsystem.h/.cpp`,
  `Entity/CamSimEntityManager.cpp`, `CamSimTest.Build.cs`, `Tests/Phase18WeatherTest.cpp`
**Sprint 2 validation**: Rotary-wing CIGI entity spawns rotor wash Niagara FX; Component Control CompId=10 places crater decal; regional weather zone fog blends by camera distance; volumetric cloud visibility and shadows driven by CIGI coverage value

---

## Phase 19 — Ocean & Lighting

VRSG's 3D ocean and thousands-of-light-points system are distinctive features.

| Item                          | Description                                                                      | Effort | VRSG Parity | Status          |
| ----------------------------- | -------------------------------------------------------------------------------- | ------ | ----------- | --------------- |
| **19A** 3D Ocean Surface      | Wave motion via Gerstner/FFT ocean shader; 12 Beaufort sea states configurable   | L      | ✓           | ✅ Sprint 1 Done |
| **19B** Vessel Wakes          | Ship/boat wake particle trails attached to maritime entities                     | M      | ✓           | ✅ Sprint 1 Done |
| **19C** Vessel Surface Motion | Pitch/roll/heave driven by sea state for shipboard entities                      | M      | ✓           | ✅ Sprint 1 Done |
| **19D** Ocean Reflections     | Environment reflections on water surface (SSR or planar reflection)              | M      | ✓           | ✅ Sprint 1 Done |
| **19E** Bathymetry            | Shallow water transparency and shoreline wave deformation from depth data        | M      | ✓           |                 |
| **19F** Light Point System    | High-performance light points with per-pixel axial/radial attenuation            | M      | ✓           |                 |
| **19G** Steerable Light Lobes | Thousands of concurrent independent light sources (runway, city, vehicle lights) | L      | ✓           |                 |

**Sprint 1 status**: 19A, 19B, 19C, 19D implemented.

**Sprint 1 files**:
- `Ocean/IOceanSurface.h` — pure C++ abstract interface; future-proofed for UE5 Water plugin
- `Ocean/FBeaufortTable.h` — Beaufort 0–12 → WaveHt/WaveLen/Choppiness lookup + linear interp
- `Ocean/FGerstnerOceanSurface.h/.cpp` — GPU MPC writes + CPU analytic eval for vessel motion
- `Ocean/FOceanManager.h/.cpp` — lifecycle owner; BeginPlay init; CIGI wave drain; SSR setup
- `Config/CamSimConfig.h/.cpp` — FPhase19Config struct, YAML `phase19:` parsing, env var overrides
- `CIGI/CigiPacketTypes.h` — FCigiWaveState (opcode 14)
- `CIGI/CigiReceiver.h/.cpp` — FWaveCtrlProcessor + DequeueWaveState
- `Environment/CamSimEnvironment.h/.cpp` — FOceanManager wiring; OnAtmosphereChanged()
- `Environment/CamSimParticleManager.h/.cpp` — NS_VesselWake FX (19B)
- `Entity/EntityTypeTable.h/.cpp` — HalfLengthCm, HalfBeamCm vessel geometry fields
- `Entity/CamSimEntity.h/.cpp` — ApplyVesselMotion() with 4-point height sampling
- `Entity/CamSimEntityManager.h/.cpp` — SetOceanSurface(); sea-domain dispatch
- `Tests/Phase19OceanTest.cpp` — 15 unit tests
- `deploy/camsim_config.yaml` — phase19: block

**Validation**: Ocean visible from altitude with Beaufort scaling; wake trails follow entities; lights visible at distance in NVG

---

## Phase 20 — HUD/OSD Symbology

Burned-in sensor display symbology matching real ISR platform output. VRSG ships 2D overlays for multiple UAV/RPA platforms.

| Item                               | Description                                                              | Effort | VRSG Parity | Status          |
| ---------------------------------- | ------------------------------------------------------------------------ | ------ | ----------- | --------------- |
| **20A** Crosshair/Reticle          | Configurable targeting crosshair at frame center (multiple styles)       | S      | ✓           | ✅ Sprint 1 Done |
| **20B** Azimuth/Elevation Readout  | Numeric gimbal angles on-screen                                          | S      | ✓           | ✅ Sprint 1 Done |
| **20C** FOV/Zoom Indicator         | Current zoom level / FOV arc display                                     | S      | ✓           | ✅ Sprint 1 Done |
| **20D** Slant Range Display        | Computed range overlay from terrain LOS                                  | S      | ✓           | ✅ Sprint 1 Done |
| **20E** Timestamp & Classification | UTC time + MISB ST 0102 classification banner                            | S      | ✓           | ✅ Sprint 1 Done |
| **20F** Compass Rose               | Heading indicator on screen edge                                         | M      | ✓           | ✅ Sprint 2 Done |
| **20G** Platform-Specific Presets  | Pre-built overlay layouts for MQ-9, MQ-1C, RQ-7B, etc.                   | M      | ✓           | ✅ Sprint 2 Done |
| **20H** Configurable Layout        | YAML-driven symbology placement, font, color, per-element enable/disable | M      |             | ✅ Sprint 2 Done |
| **20I** Symbology Toggle           | Config flag for clean frames (ML) vs overlay (ISR)                       | S      |             | ✅ Sprint 1 Done |

**Sprint 1 status**: 20A–20E, 20I implemented.

**Sprint 2 status**: 20F, 20G, 20H implemented.

**Sprint 2 files**:
- `Overlay/FHudOverlay.h` — `FHudElementConfig`, updated `FHudOverlayConfig`, `LoadPreset()` declaration
- `Overlay/FHudOverlay.cpp` — `DrawCompassRose()` (20F), `DrawPlatformLabel()` (20G), `LoadPreset()` (20G), `ResolveColor()`, updated `Render()` + all Draw methods
- `Config/CamSimConfig.cpp` — preset loading, new YAML keys, new env vars
- `deploy/camsim_config.yaml` — `compass_rose:`, `platform_label:`, `preset:`, per-element position keys
- `Tests/OverlayTest.cpp` — 7 new tests (tests 9–15); updated `MakeOverlay()` helper

**Sprint 1 files**:
- `Overlay/FBitmapFont.h/.cpp` — 5×7 pixel font + drawing primitives (SetPixel, DrawHLine/VLine/Rect/FillRect, DrawString, DrawStringWithShadow)
- `Overlay/FHudOverlay.h/.cpp` — crosshair (20A), Az/El readout (20B), FOV indicator (20C), slant range (20D), timestamp + classification banner (20E); master toggle (20I)
- `Sensor/SensorPostProcess.h/.cpp` — FHudOverlay member + SetOverlayConfig(); Render() called last in Process()
- `Config/CamSimConfig.h/.cpp` — FHudOverlayConfig field, overlay: YAML block, CAMSIM_OVERLAY_* env vars (all disabled by default)
- `Camera/CamSimCamera.cpp` — SetOverlayConfig(Cfg.OverlayConfig) at BeginPlay
- `deploy/camsim_config.yaml` — overlay: block (enabled: false default)
- `Tests/OverlayTest.cpp` — 8 unit tests

**Validation**: Visual comparison to real FLIR/L3Harris sensor display; symbology disabled = clean frame

---

## Phase 21 — Interoperability & Streaming

VRSG natively supports DIS and streams FMV to ATAK/ROVER. This is a critical interop gap.

| Item                         | Description                                                                    | Effort | VRSG Parity |
| ---------------------------- | ------------------------------------------------------------------------------ | ------ | ----------- |
| **21A** DIS Protocol Support | IEEE 1278.1 Entity State PDU — custom minimal parser (no open-dis-cpp)        | XL     | ✓           | ✅ Sprint 1 Done |
| **21B** DIS ↔ Entity Manager | ECEF→geodetic, ID translation, DR algo 2/5, timeout sweep, type mapping       | XL     | ✓           | ✅ Sprint 1 Done |
| **21C** DIS PDU Logging      | PDU binary recording (same pattern as CIGI 12E)                                | S      | ✓           | ✅ Sprint 1 Done |
| **21D** ATAK FMV Streaming   | Stream H.264 + KLV to ATAK-compatible endpoints (CoT + RTSP/UDP)               | M      | ✓           |                  |
| **21E** ROVER Compatibility  | ROVER-format video feed for ground force terminals                             | M      | ✓           |                  |
| **21F** Laser Designator Viz | Visible laser spot (EO) and IR laser marker (NVG mode); Designator PDU support | M      | ✓           |                  |
| **21G** HLA Gateway          | Deferred — DIS covers >90% of exercise interop needs                           | L      | ✓           |                  |

**Sprint 1 (Done)**: DIS protocol core — custom PDU parser, FDisReceiver (FRunnable), FDisEntityAdapter (ECEF→geodetic, ID translation, DR, timeout), FDisConfig, 10 unit tests.
**Sprint 2 (Planned)**: ATAK/ROVER streaming (CoT, RTSP), laser designator visualization.

**New Files**: `DIS/DisPduTypes.h/.cpp`, `DIS/DisReceiver.h/.cpp`, `DIS/DisEntityAdapter.h/.cpp`, `Tests/DisProtocolTest.cpp`
**Modified Files**: `Config/CamSimConfig.h/.cpp`, `Subsystem/CamSimSubsystem.h/.cpp`, `Entity/CamSimEntityManager.h/.cpp`, `deploy/camsim_config.yaml`
**Validation**: CamSim joins OneSAF/VBS exercise via DIS; ATAK displays live FMV feed; laser spot visible in NVG

---

## Phase 22 — Entity Content & Characters

VRSG ships hundreds of entity models. CamSim needs a broader content pipeline.

| Item                            | Description                                                                       | Effort | VRSG Parity |
| ------------------------------- | --------------------------------------------------------------------------------- | ------ | ----------- |
| **22A** FBX/glTF Model Import   | Runtime or editor-time import pipeline for FBX/glTF models into entity type table | M      | ✓           |
| **22B** OpenFlight Converter    | Batch convert OpenFlight (.flt) databases/models to UE-compatible format          | L      | ✓           |
| **22C** Damage State Models     | Multi-stage visual damage (pristine → damaged → destroyed) per entity type        | M      | ✓           |
| **22D** Character Animation     | Human characters with walk/run/crouch/prone skeletal animation                    | L      | ✓           |
| **22E** Civilian Population     | Ambient pedestrian/vehicle traffic using UE AI navigation                         | L      | ✓           |
| **22F** Standard Entity Library | Baseline set of 50+ military/civilian models (air, ground, maritime, personnel)   | L      | ✓           |
| **22G** First-Person View       | FPS mode: camera follows character entity at eye height with look controls        | M      | ✓           |

**Files**: `Entity/`, new `Content/` pipeline, `Config/CamSimConfig.h`
**Validation**: FBX model loads at runtime; characters animate correctly; damage states transition on Component Control

---

## Phase 23 — Scenario Engine

Complex scripted scenarios without external CIGI host. VRSG has a built-in scenario editor.

| Item                          | Description                                                                         | Effort | VRSG Parity |
| ----------------------------- | ----------------------------------------------------------------------------------- | ------ | ----------- |
| **23A** Waypoint Trajectories | Entity follows lat/lon/alt waypoints with speed/acceleration curves                 | M      | ✓           |
| **23B** Event Triggers        | Actions fired on conditions (entity visible, range < threshold, timer, frame count) | M      | ✓           |
| **23C** Pattern-of-Life       | Repeating daily activity cycles for characters/vehicles (YAML-scripted)             | M      | ✓           |
| **23D** Formation Flying      | Multiple entities maintain relative positions with configurable offsets             | M      |             |
| **23E** Randomization Engine  | Stochastic entity count, spawn area, weather, TOD for training robustness           | M      |             |
| **23F** Batch Runner          | Run N scenarios unattended; output ground truth + video per scenario                | M      |             |
| **23G** Scenario Editor GUI   | Web-based or ImGui editor for culture placement and scenario authoring              | L      | ✓           |
| **23H** Mission Scripting     | Lua or Python scripting API for complex multi-phase missions                        | L      | ✓           |

**Files**: new `Scenario/` module, extend `Config/CamSimConfig.cpp`
**Validation**: Run 10-scenario batch; entity paths match YAML waypoints; pattern-of-life repeats correctly

---

## Phase 24 — Rendering Quality

Close the visual fidelity gap with VRSG's advanced rendering pipeline.

| Item                            | Description                                                                  | Effort | VRSG Parity |
| ------------------------------- | ---------------------------------------------------------------------------- | ------ | ----------- |
| **24A** Object-on-Object Shadow | Dynamic shadowing between entities (tanker-receiver, vehicle-building)       | M      | ✓           |
| **24B** Screen Space AO         | Ambient occlusion for grounded visual contact (Lumen GTAO)                   | S      | ✓           |
| **24C** Normal/Light Maps       | Support normal maps and lightmaps on terrain and entity materials            | M      | ✓           |
| **24D** Ray Tracing Integration | RTX reflections on SceneCapture via `r.RayTracing.Reflections` CVar          | M      |             |
| **24E** Shadow Quality Tuning   | Virtual shadow map cascade optimization for high-altitude ISR viewing angles | S      | ✓           |
| **24F** Model-Edge Anti-Alias   | TSR screen percentage override; shadow+normal ShowFlags on SceneCapture      | S      | ✓           |

**Status**: Sprint 1 complete. All 24A–24F implemented.

**Files**:
- `Config/CamSimConfig.h` — `FRenderingQualityConfig` struct (11 fields)
- `Config/CamSimConfig.cpp` — YAML `rendering_quality:` block + env var overrides
- `Camera/CamSimCamera.cpp` — constructor ShowFlags (24A/C); BeginPlay PP/CVar block (24B/D/E/F)
- `Entity/CamSimEntity.h/.cpp` — explicit `CastShadow`/`bCastDynamicShadow`; `SetShadowCasting(bool)`
- `Entity/CamSimEntityManager.cpp` — calls `SetShadowCasting()` at spawn
- `deploy/camsim_config.yaml` — `rendering_quality:` config block
- `TODO.md` — Phase 24C normal map content note

**Validation**: Log line `ACamSimCamera: RenderingQuality — shadows=1 contactShadow=0 AO=0.50 RTRefl=0 shadowDist=2.0 VSMBias=-1 TSR%=100` on startup; `r.Shadow.Virtual.MaxPhysicalPages` reports 4096 in console

---

## Phase 25 — Display & VR

VRSG supports VR headsets, dome displays, and multi-monitor setups.

| Item                            | Description                                                                     | Effort | VRSG Parity |
| ------------------------------- | ------------------------------------------------------------------------------- | ------ | ----------- |
| **25A** VR/HMD Rendering        | Stereo rendering for Varjo, HTC Vive, Meta Quest via UE OpenXR plugin           | L      | ✓           |
| **25B** Multi-Monitor/Dome      | Nvidia Surround / multi-projector warping for dome and multi-screen setups      | L      | ✓           |
| **25C** Edge Blend & Distortion | Integration with Scalable Display Technologies / VIOSO for projection alignment | M      | ✓           |
| **25D** Rear-View Mirrors       | Horizontal mirror viewport for vehicle rear-view simulation                     | S      | ✓           |
| **25E** PIP Compositing         | Picture-in-picture: wide FOV + narrow FOV composited into single output         | M      | ✓           |
| **25F** Stereo Pair             | Baseline-separated cameras for 3D reconstruction / stereo ML training           | M      |             |
| **25G** Eye Tracking            | Varjo eye-tracking data capture and 3D gaze visualization                       | M      | ✓           |

**Files**: `Camera/CamSimCamera.h/.cpp`, `Subsystem/CamSimSubsystem.cpp`, UE project settings
**Validation**: Stereo render in VR headset; PIP overlay visible; eye tracking data exported

---

## Phase 26 — Standards Compliance

Full interoperability with ISR ecosystem tools and standards validators.

| Item                           | Description                                                                        | Effort | VRSG Parity |
| ------------------------------ | ---------------------------------------------------------------------------------- | ------ | ----------- |
| **26A** ST 0601 Missing Tags   | Tag 4 (frame #), 26 (target width), 31–34 (microdynamics), 42–45 (target location) | M      |             |
| **26B** BCC-16 Checksum        | Switch from CRC-16/CCITT to standard BCC-16 (or make configurable)                 | S      |             |
| **26C** STANAG 4609 Validation | Verify PAT/PMT structure, PID allocation, KLV sync timing per spec                 | M      | ✓           |
| **26D** Remaining CIGI Opcodes | Opcode 3 (conformal clutter), 7 (collision detection), 13 (regional weather)       | M      | ✓           |
| **26E** KLV Uncertainty Tags   | Tags 27–30 (slant range, cross-range, HFOV, VFOV uncertainty)                      | S      |             |
| **26F** MISB ST 0903 VMTI      | Video Moving Target Indicator metadata for tracked entities                        | L      |             |
| **26G** MISB 0601.9 + 0104.5   | Update to latest MISB standard versions (VRSG ships 0601.9 / 0104.5)               | M      | ✓           |

**Files**: `Metadata/KlvBuilder.cpp`, `CIGI/CigiReceiver.cpp`, `CIGI/CigiPacketTypes.h`
**Validation**: Pass MISB ST 0601 compliance checker; validate with external KLV decoder

---

## Phase 27 — Performance & Optimization

Maximize throughput and minimize glass-to-glass latency.

| Item                              | Description                                                                  | Effort | Status           |
| --------------------------------- | ---------------------------------------------------------------------------- | ------ | ---------------- |
| **27A** GPU Sensor Pipeline       | Move IR/NVG effects to post-process materials (eliminate CPU pixel copy)     | L      | ✅ Sprint 1 Done |
| **27B** Frame Drop Categorization | Track drops by cause: encoder busy, readback timeout, socket error           | S      | ✅ Sprint 1 Done |
| **27C** DDC Pre-warming           | Script to compile base shaders offline; reduce cold-start from ~180s to ~60s | S      | ✅ Sprint 1 Done |
| **27D** Hot-Reload Config         | Apply gimbal limits, bitrate, sensor mode changes without restart            | L      | ✅ Sprint 1 Done |
| **27E** Tile Prefetch             | Predictive tile loading based on entity trajectory and gimbal sweep          | M      | ✅ Sprint 1 Done |
| **27F** 60 Hz Rendering           | Option to render at 60 Hz (VRSG default) with configurable output frame rate | M      | ✅ Sprint 1 Done |
| **27G** Texture Paging Budget     | Configurable texture memory budget (VRSG addresses up to 2 TB)               | M      | ✅ Sprint 1 Done |

**Status**: Sprint 1 complete. All 27A–27G implemented.

**Sprint 1 files**: `Config/CamSimConfig.h`, `Config/CamSimConfig.cpp`, `deploy/camsim_config.yaml`, `Camera/CamSimCamera.h`, `Camera/CamSimCamera.cpp`, `Encoder/VideoEncoder.cpp`, `Sensor/SensorPostProcess.h`, `Sensor/SensorPostProcess.cpp`, `Subsystem/CamSimSubsystem.h`, `Subsystem/CamSimSubsystem.cpp`, `Tests/Phase27PerformanceTest.cpp`, `scripts/prewarm_shaders.sh`

**Sprint 1 validation**: Log line on startup confirms renderFPS/outputFPS/texturePoolMB/dropTracking/hotReload; frame drop counters in `camsim_health.json`; DDC cold-start < 90s; GPU sensor falls back gracefully when editor material absent

---

## Phase 28 — Operational Hardening

Production-grade reliability for 24/7 deployment.

| Item                               | Description                                                                    | Effort |
| ---------------------------------- | ------------------------------------------------------------------------------ | ------ |
| **28A** Unit Tests in CI           | Run UE automation tests in GitHub Actions (UE test runner step)                | M      |
| **28B** Structured JSON Logging    | Machine-parseable log output for ELK/Datadog aggregation                       | M      |
| **28C** HTTP Health Endpoints      | `/ready` and `/live` for Kubernetes probes                                     | M      |
| **28D** Config Validation          | Pre-flight range checks on all config values with meaningful error messages    | S      |
| **28E** Graceful Degradation       | Reduce resolution or disable effects under GPU/CPU pressure                    | L      |
| **28F** Encoder Reconnection       | Socket reopen on UDP failure (3 retries); failover to local .ts recording      | M      |
| **28G** Per-Frame Latency Tracking | Timestamp each pipeline stage; export P50/P95/P99 to Prometheus                | M      |
| **28H** After-Action Review        | DIS/CIGI log replay with entity visualization, fire/shot lines, viewpoint save | L      |
| **28I** Virtual World Sound        | Positional audio (engine sounds, weapons) for VR/training                      | M      |

**Files**: `Tests/`, `.github/workflows/ci.yml`, `Subsystem/CamSimSubsystem.cpp`
**Validation**: CI passes all tests; health endpoint returns JSON; latency metrics in Prometheus

---

## Phase 29 — Radar Simulation

VRSG supports SAR, ISAR, and F-16 DRLMS radar. Lower priority — niche requirement.

| Item                    | Description                                                                    | Effort | VRSG Parity |
| ----------------------- | ------------------------------------------------------------------------------ | ------ | ----------- |
| **29A** SAR Simulation  | Synthetic aperture radar image generation from terrain elevation + backscatter | XL     | ✓           |
| **29B** ISAR Simulation | Inverse SAR imaging of moving targets from relative motion                     | XL     | ✓           |
| **29C** Radar Display   | B-scope / PPI display rendering with range rings, azimuth lines                | L      | ✓           |

**Files**: new `Radar/` module
**Validation**: SAR image matches terrain geometry; ISAR resolves moving target features

---

## Recommended Execution Order

```
Sprint 1 ──── Phase 16A-D,F,L (Sensor fidelity core: AGC, pixels, MTF, AC band, IR pointer)
              Phase 20A-E,I (HUD/OSD core: crosshair, readouts, timestamp, toggle)

Sprint 2 ──── Phase 18A-E (Weather core: volumetric clouds, fog, rain/snow, god rays)
              Phase 24A-C,F (Rendering: shadows, SSAO, normal maps, anti-alias)

Sprint 3 ──── Phase 18F-H (Particle FX: dust, smoke/fire, contrails)
              Phase 17A-D,G (ML core: depth, segmentation, bounding boxes, COCO)

Sprint 4 ──── Phase 21A-C (DIS protocol: PDUs, entity mapping, logging)
              Phase 19A-D (Ocean core: waves, wakes, vessel motion, reflections)

Sprint 5 ──── Phase 20F-H (HUD: compass, platform presets, config layout)
              Phase 21D-F (Streaming: ATAK, ROVER, laser designator)

Sprint 6 ──── Phase 22A,C-D (Content: FBX import, damage states, characters)
              Phase 23A-C (Scenario: waypoints, triggers, pattern-of-life)

Sprint 7 ──── Phase 16E,G-K ✅ DONE (Sensor advanced: thermal drift, exposure, vibration, glint)
              Phase 26A-E,G (Standards: ST 0601 tags, checksum, CIGI opcodes, MISB update)

Sprint 8 ──── Phase 23D-H (Scenario: formations, randomization, batch, editor, scripting)
              Phase 22E-G (Content: civilian population, entity library, FPS mode)

Sprint 9 ──── Phase 17E-F,H-I (ML advanced: 3D boxes, optical flow, VOC, randomization)
              Phase 27 (Performance: GPU sensor, prefetch, 60Hz, hot-reload)

Sprint 10 ─── Phase 25 (Display & VR: HMD, dome, edge blend, PIP, eye tracking)
              Phase 28 (Ops hardening: CI tests, logging, health, latency, AAR, sound)

Sprint 11 ─── Phase 18I,J-L (Advanced: cratering, atmospheric scattering, IR atmo, regional wx)
              Phase 19E-G (Ocean advanced: bathymetry, light points, steerable lobes)

Sprint 12 ─── Phase 22B,F (Content: OpenFlight, entity library expansion)
              Phase 29 (Radar: SAR/ISAR — if required)
              Phase 21G (HLA gateway)
              Phase 26F (VMTI)
```

## Effort Key

- **S** = Small (1–3 days)
- **M** = Medium (3–7 days)
- **L** = Large (1–2 weeks)
- **XL** = Extra Large (2–4 weeks)

## Notes

- Phases 16–20 close the most visible VRSG gaps (sensor, weather, ocean, HUD)
- Phase 21 (DIS) is critical for exercise interoperability — VRSG's primary use case
- Phase 17 (ML training data) is a **differentiator** — VRSG does not offer this
- Phase 29 (Radar) is lowest priority — only needed for specific F-16/UAV radar training
- VRSG runs at 60 Hz on DirectX/Windows; CamSim targets 30 Hz on UE5 cross-platform (configurable in Phase 27F)
