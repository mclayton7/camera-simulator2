# CamSim Roadmap: Professional Synthetic Scene Generator

## Context
CamSim currently streams H.264 video from a Cesium globe with CIGI-driven camera control and MISB ST 0601 KLV metadata. The core pipeline works (CIGI in → UE5 render → H.264/KLV MPEG-TS out). This plan adds the features needed to make it a professional-grade synthetic scene generator: entity rendering, environment control, gimbal simulation, sensor modes, and standards compliance.

## Current State (Phases 1-6 complete)
- CIGI 3.3 input: Entity Control (opcode 2) + View Definition (opcode 21) — camera only
- Cesium terrain with tile streaming registered to `ACesiumCameraManager`
- H.264 1920x1080 @ 30fps MPEG-TS with KLV ST 0601 → UDP multicast
- Configurable start position, tile preloading, FOV inflation
- No entities rendered, no environment control, no gimbal, no sensor effects

---

## Phase 7 — Environment: Day/Night & Weather

**Goal:** CIGI-driven time-of-day, sun position, atmosphere, and basic weather so the scene looks realistic at any hour.

### 7A: Celestial Sphere (CIGI opcode 9)
- Parse `CigiCelestialCtrlV3` in FCigiReceiver → new `FCigiCelestialState` struct (hour, date, sun enable, moon enable, star field enable)
- Queue → game thread consumption in a new `ACamSimEnvironment` actor (spawned by GameMode alongside camera)
- Drive UE5's `ADirectionalLight` (sun) azimuth/elevation from CIGI ephemeris values
- Drive `ASkyAtmosphere` + `ASkyLight` for physically-based sky color transitions (dawn→day→dusk→night)
- Fallback: if no CIGI celestial packet received, use system clock UTC or config `start_hour`

### 7B: Atmosphere Control (CIGI opcode 10)
- Parse `CigiAtmosCtrlV3` → `FCigiAtmosphereState` (visibility_range, humidity, air_temp, baro_pressure)
- Drive UE5 `ExponentialHeightFog` density from visibility range (fog_density ≈ 3.912 / visibility_m)
- Air temperature → optional thermal haze post-process (subtle heat shimmer at low altitudes)

### 7C: Weather Control (CIGI opcode 12)
- Parse `CigiWeatherCtrlV3` → `FCigiWeatherState` (cloud_type, base_alt, coverage, precip_type, intensity)
- Drive UE5 `VolumetricCloud` layer altitude/coverage, or Niagara particle rain/snow
- Scope for v1: single cloud layer + precipitation particle; multi-layer and region control deferred

### New files
```
CIGI/CigiEnvironmentTypes.h         — FCigiCelestialState, FCigiAtmosphereState, FCigiWeatherState
Environment/CamSimEnvironment.h/.cpp — ACamSimEnvironment actor, owns sky/fog/cloud references
```

### Verification
- Python sender with `--time 0600` / `--time 2200` flags → video shows sunrise / night sky
- `--visibility 500` → dense fog visible in stream
- KLV timestamps still correct; no frame drops

---

## Phase 8 — Entity Rendering: Aircraft, Vehicles, Effects

**Goal:** Spawn and manage 3D models in the scene driven by CIGI Entity Control packets, with proper entity state lifecycle.

### 8A: Entity Registry
- `FCamSimEntityManager` class owned by `UCamSimSubsystem`
- `TMap<uint16, ACamSimEntity*>` mapping CIGI entity IDs to spawned actors
- Entity state machine: EntityState byte 0=Remove (destroy actor), 1=Standby (hide), 2=Active (show + update)
- Separate camera entity (entity ID configurable, default 0) from scene entities

### 8B: Entity Actor (`ACamSimEntity`)
- `AActor` subclass with `UCesiumGlobeAnchorComponent` + `UStaticMeshComponent` (or `USkeletalMeshComponent`)
- Positioned via `MoveToLongitudeLatitudeHeight()` same as camera
- Entity type field (from CIGI) → model lookup table (config JSON maps type IDs to UE asset paths)
- Example: `{ "entity_types": { "1001": "/Game/Models/F16/F16.F16", "2001": "/Game/Models/Truck/Truck.Truck" } }`

### 8C: Rate Control (CIGI opcode 8)
- Parse `CigiRateCtrlV3` → linear/angular velocity per entity
- Dead-reckoning interpolation between CIGI updates (typically 60Hz CIGI → 30fps render)
- Smooth entity motion without jitter from update rate mismatch

### 8D: Articulated Parts (CIGI opcode 6)
- Parse `CigiArtPartCtrlV3` → per-entity, per-part-ID rotation/offset
- Drive skeletal mesh bone transforms or sub-mesh transforms
- Priority parts: landing gear (extend/retract), control surfaces (ailerons/elevator/rudder), rotor RPM

### 8E: Component Control (CIGI opcode 4)
- Parse `CigiCompCtrlV3` → switch lights, damage states
- Navigation lights (red/green/white point lights), anti-collision strobe, landing lights
- Damage state switching (intact → damaged → destroyed mesh variants)

### New files
```
Entity/CamSimEntityManager.h/.cpp  — entity registry, spawn/destroy lifecycle
Entity/CamSimEntity.h/.cpp         — entity actor with GlobeAnchor + mesh + articulation
Entity/EntityTypeTable.h/.cpp      — JSON config mapping entity type IDs to asset paths
```

### Verification
- Python sender spawns entity ID 1 (type 1001 = F-16) at known lat/lon → visible in video
- Entity moves smoothly via Rate Control dead-reckoning
- Entity removed (state=0) → disappears from scene
- Landing gear articulation visible on close-up

---

## Phase 9 — Gimbal & Sensor Control

**Goal:** Separate platform motion from sensor pointing. The camera follows a gimbal model parented to a platform entity, with independent pan/tilt driven by CIGI.

### 9A: Gimbal Architecture
- Camera entity becomes a child of a platform entity via CIGI parent ID
- `ACamSimCamera` gains gimbal state: azimuth, elevation, roll (3-DOF)
- Gimbal angles driven by Articulated Part Control (opcode 6) on the camera entity's DOFs
- SceneCaptureComponent2D rotation = platform rotation + gimbal offset
- Configurable slew rate limits (max °/s) for realistic gimbal dynamics

### 9B: Sensor Control (CIGI opcode 17)
- Parse `CigiSensorCtrlV3` → sensor on/off, track mode, polarity, FOV select
- FOV select: wide/medium/narrow mapped to configurable FOV values
- Track mode: off / manual / target_entity (auto-slew gimbal to point at a tracked entity ID)
- Polarity: normal/inverted (flip luminance for IR white-hot/black-hot)

### 9C: View Control (CIGI opcode 16)
- Parse `CigiViewCtrlV3` → explicit eye-point override, entity attachment
- Allows host to override camera position relative to parent entity
- FOV override independent of View Definition

### 9D: KLV Gimbal Metadata
- Populate additional ST 0601 tags: Tag 18/19 (sensor HFOV/VFOV from actual capture FOV), Tag 20 (sensor relative azimuth), Tag 21 (sensor relative elevation), Tag 22 (sensor relative roll), Tag 23 (slant range from LOS trace)

### Modified files
```
Camera/CamSimCamera.h/.cpp   — add gimbal state, parent entity attachment
Metadata/KlvBuilder.h/.cpp   — add gimbal-related ST 0601 tags
CIGI/CigiPacketTypes.h       — add FCigiSensorControl, FCigiViewControl, FCigiRateControl
CIGI/CigiReceiver.cpp        — register processors for opcodes 16, 17
```

### Verification
- CIGI sender commands platform entity to fly straight while gimbal pans left → video shows smooth gimbal sweep independent of platform heading
- FOV changes (wide → narrow) visible as zoom transitions
- KLV sensor azimuth/elevation tags validated by `validate_klv.py`

---

## Phase 10 — Terrain Feedback: HAT/HOT & LOS

**Goal:** Respond to host terrain queries so the host simulation can solve ground contact, line-of-sight, and laser ranging.

### 10A: HAT/HOT (CIGI opcodes 24/102)
- Parse `CigiHatHotReqV3` → UE5 line trace straight down from query lat/lon
- Return `CigiHatHotRespV3` → terrain height (HAT = query alt minus terrain alt)
- Used by host for ground-clamped vehicles, auto-land altitude solving

### 10B: LOS (CIGI opcodes 25-26 / 103)
- Parse `CigiLosSegReqV3` or `CigiLosVectReqV3` → UE5 line trace between two points or along a vector
- Return `CigiLosRespV3` → hit/miss, range to intersection, entity hit ID, material code
- Material mapping: UE5 physical material → CIGI material code table
- Used by host for laser rangefinder, target visibility, threat engagement queries

### 10C: IG-to-Host Response Channel
- New `FCigiSender` class (counterpart to `FCigiReceiver`)
- CCL `CigiIGSession` outgoing message assembly
- UDP socket sending responses back to host on configurable port
- Start of Frame (opcode 101) sent each tick as sync heartbeat

### New files
```
CIGI/CigiSender.h/.cpp        — outgoing UDP, CCL IG session response assembly
CIGI/CigiQueryHandler.h/.cpp   — HAT/HOT and LOS line trace execution
```

### Verification
- Python sender issues HAT request at known location → receives correct terrain height
- LOS query from high altitude to ground → receives range matching expected slant distance
- Start of Frame packets visible in Wireshark at 30Hz

---

## Phase 11 — Sensor Simulation: EO/IR/NVG

**Goal:** Post-process the rendered scene to simulate different sensor wavebands and physical sensor effects.

### 11A: EO/IR Mode Switching
- UE5 Post-Process Material stack on SceneCaptureComponent2D
- EO mode: standard color output (current behavior)
- IR (LWIR) mode: grayscale luminance remap, entity thermal signatures as material parameter overrides, sky/terrain temperature differential, white-hot / black-hot polarity
- NVG mode: green-tinted monochrome with bloom, reduced dynamic range, scintillation noise

### 11B: Atmospheric Effects
- Distance-dependent haze/extinction applied per-pixel using scene depth buffer
- Visibility range from CIGI Atmosphere Control → extinction coefficient
- Heat shimmer (low-altitude thermal distortion) as animated UV offset material

### 11C: Sensor Noise & Degradation
- Configurable NETD (thermal noise) as Gaussian noise overlay
- Fixed pattern noise (subtle grid artifact)
- Vignetting (darkened corners)
- Scan line artifacts (optional, for legacy sensor simulation)
- All controlled by config JSON parameters, toggled per sensor mode

### 11D: Sensor Effects Post-Process Chain
```
SceneCapture → [Base Render] → [Waveband Remap (EO/IR/NVG)]
    → [Atmospheric Extinction] → [Sensor Noise] → [Vignetting]
    → [Polarity Inversion] → [Output to Encoder]
```

### New files
```
Sensor/SensorPostProcess.h/.cpp     — manages PP material stack per sensor mode
Sensor/SensorConfig.h               — per-mode parameters (NETD, vignetting, etc.)
Content/Materials/M_IR_Remap.uasset — IR post-process material
Content/Materials/M_NVG.uasset      — NVG post-process material
Content/Materials/M_SensorNoise.uasset — noise overlay material
```

### Verification
- CIGI Sensor Control polarity change → video switches white-hot ↔ black-hot
- Entity with thermal signature visible as bright spot in IR, invisible in EO at night
- NVG mode shows green-tinted output with bloom around bright sources

---

## Phase 12 — Standards Compliance & Hardening

### 12A: MISB ST 0102 (Security Metadata)
- Add classification local set to every KLV packet (even "UNCLASSIFIED")
- Tags: classification level, classifying country, object country, caveats
- Configurable via `camsim_config.json`

### 12B: STANAG 4609 Compliance
- Validate MPEG-TS PID allocation per STANAG 4609 Ed3
- Ensure KLV metadata rate ≥ 1 Hz (currently 30 Hz — compliant)
- Verify PTS synchronization between video and KLV within one frame
- Add H.265/HEVC encoder option for STANAG 4609 Ed4

### 12C: Multi-Channel Output
- Support multiple simultaneous camera/sensor views (e.g., wide-area + narrow FOV)
- Each channel gets its own MPEG-TS output stream on a separate multicast group/port
- Driven by CIGI View Definition per view ID

### 12D: Health & Monitoring
- Health file written periodically (for container orchestration liveness probes)
- Prometheus metrics endpoint (frame rate, encode latency, CIGI packet rate, tile load count)
- IG Status message (opcode 115) sent to host with frame timing stats

### 12E: Recording & Playback
- Record CIGI input stream to file for deterministic replay
- Record raw video + KLV to local .ts file for offline analysis
- Playback mode: read recorded CIGI from file instead of UDP socket

---

## Phase Priority & Dependencies

```
Phase 7 (Environment)     ──┐
Phase 8 (Entities)        ──┼── independent, can be developed in parallel
Phase 9 (Gimbal/Sensor)   ──┘        ↓ depends on Phase 8 entities
Phase 10 (HAT/HOT/LOS)   ── depends on Phase 8 (entity hit detection)
Phase 11 (EO/IR/NVG)      ── depends on Phase 9 (sensor mode switching)
Phase 12 (Compliance)      ── depends on all above
```

**Recommended build order:** 7 → 8 → 9 → 10 → 11 → 12

Phases 7 and 8 are independent and could be developed in parallel if desired.

---

## Key Architecture Decisions

### Entity Model Loading
- **Approach:** JSON config maps CIGI entity type IDs → UE asset paths. Models are `UStaticMesh` or `USkeletalMesh` assets loaded via `FSoftObjectPath` / `LoadObject`. For articulated entities (aircraft with control surfaces), use `USkeletalMesh` with named bones matching CIGI articulated part IDs.
- **Asset pipeline:** Models stored as `.uasset` in `Content/Models/`. Imported from FBX/glTF via UE editor. Packaged with the game binary.

### Gimbal vs. Camera
- **Approach:** `ACamSimCamera` gains an optional parent entity ID. When parented, its world position = parent entity position + body-frame offset. Gimbal angles (from Articulated Part Control) are applied as additional rotation on the SceneCaptureComponent2D relative to the parent entity's orientation.

### Sensor Simulation
- **Approach:** UE5 Post-Process Materials on the SceneCaptureComponent2D, not full compute shader pipelines. This keeps it portable (works on any GPU backend) and leverages UE's built-in scene depth buffer for atmospheric extinction. IR thermal signatures are material parameter overrides on entity meshes.

### IG-to-Host Communication
- **Approach:** New `FCigiSender` using CCL's outgoing message API, sending UDP to host address (auto-detected from incoming CIGI source IP, or configured). Responses are queued from game thread and flushed once per frame alongside Start of Frame.

---

## CIGI 3.3 Packet Coverage Summary

| Opcode | Packet | Current | Phase |
|--------|--------|---------|-------|
| 1 | IG Control | Parsed (sender) | — |
| 2 | Entity Control | Camera only | 8: multi-entity |
| 4 | Component Control | — | 8E |
| 6 | Articulated Part Control | — | 8D, 9A |
| 8 | Rate Control | — | 8C |
| 9 | Celestial Sphere Control | — | 7A |
| 10 | Atmosphere Control | — | 7B |
| 12 | Weather Control | — | 7C |
| 16 | View Control | — | 9C |
| 17 | Sensor Control | — | 9B |
| 20 | View Definition | Parsed, unused | 9C |
| 24 | HAT/HOT Request | — | 10A |
| 25-26 | LOS Request | — | 10B |
| 101 | Start of Frame (response) | — | 10C |
| 102 | HAT/HOT Response | — | 10A |
| 103 | LOS Response | — | 10B |
