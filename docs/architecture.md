# CamSim Architecture

## Overview

CamSim is a headless Unreal Engine 5 application. The UE5 rendering pipeline is
driven by CIGI 3.3 packets arriving over UDP. Each rendered frame is read back
from the GPU, encoded to H.264, and emitted as one or more MPEG-TS UDP outputs
with MISB ST 0601 KLV metadata.

## Thread Model

Four threads collaborate with explicit ownership boundaries:

```
┌─────────────────────────────────────────────────────────────────┐
│  CigiReceiverThread  (FRunnable)                                │
│  • Binds UDP socket on cigi_port                                │
│  • Feeds bytes into CCL parser                                  │
│  • Pushes structs into SPSC queues — ONLY PRODUCER              │
└──────────────────────────┬──────────────────────────────────────┘
                           │ TSpscQueue (lock-free)
┌──────────────────────────▼──────────────────────────────────────┐
│  Game Thread                                                    │
│  • FCamSimEntityManager::Tick() — drains EntityStateQueue       │
│      then CameraFrameQueue (ACamSimCamera::ApplyHostPlatform…)  │
│                                   RateCtrlQueue                 │
│                                   ArtPartQueue                  │
│                                   CompCtrlQueue                 │
│  • ACamSimEnvironment::Tick() — drains Celestial/Atmos/Weather  │
│  • ACamSimCamera::Tick() (TG_PostUpdateWork, last) — applies the│
│      held View/Sensor Ctrl, ArtPart; footprint; captures        │
│  • FCigiQueryHandler::Tick() — drains HatHotReqQueue            │
│                                        LosSegReqQueue           │
│                                        LosVectReqQueue          │
│                               UE line traces → FCigiSender      │
│  • FCigiSender::FlushFrame() — SOF + response datagram → host   │
│      (sent from the camera tick, after this frame's centre)     │
│  • Primary view: queues a grab request; the game viewport       │
│    renders the sensor view (TSR) after the tick                 │
│  • Sensor AE/AGC (FSensorController) → sensor graph params      │
│  • IR: FThermalFrameBuilder → FThermalFrameParams (class temps, │
│    sky, stencil table) → render thread with the sensor params   │
└──────────────────────────┬──────────────────────────────────────┘
                           │ ENQUEUE_RENDER_COMMAND
┌──────────────────────────▼──────────────────────────────────────┐
│  Render Thread                                                  │
│  • IR + thermal: ThermalCS at BeforeDOF (pre-TSR) writes        │
│    radiance into scene colour; TSR resolves it (RGBA16F)        │
│  • FCamSimFrameGrabExtension: GPU sensor graph replaces the     │
│    tonemapper → NV12 buffer → FRHIGPUBufferReadback (async)     │
│  • Poll command: readback ready → TArray<uint8> (NV12)          │
│  • Dispatches async task for encoding                           │
└──────────────────────────┬──────────────────────────────────────┘
                           │ AsyncTask(AnyBackgroundThreadNormalTask)
┌──────────────────────────▼──────────────────────────────────────┐
│  Task Thread (pool)                                             │
│  • Fan-out to one or more output views                          │
│  • Per-view: NV12 de-interleave + libx264 + KLV + MPEG-TS send  │
└─────────────────────────────────────────────────────────────────┘
```

`bEncoderBusy` (atomic bool) prevents the game thread from issuing a new
capture while the previous frame is still encoding, maintaining a natural
back-pressure that keeps encoding load at exactly one frame in flight.

## SPSC Queue Routing

`FCigiReceiver` maintains these SPSC queues. The receiver thread is the sole
producer for all queues. Each queue has exactly one game-thread consumer:

| Queue | Producer | Consumer |
|-------|----------|----------|
| `CameraFrameQueue` | `FCigiReceiver::ProcessDatagram`, one `FCigiCameraFrame` per datagram: the camera entity's Entity Control, View Control (16), View Definition (21), Sensor Control (17), camera Art Part and Platform Kinematics (user-defined 201, raw-parsed) | `ACamSimCamera` |
| `EntityStateQueue` | `FEntityCtrlProcessor` (all other entity IDs) | `FCamSimEntityManager` |
| `CelestialQueue` | `FCigiRawEnvParser` (raw bytes, bypasses CCL hold) | `ACamSimEnvironment` |
| `AtmosphereQueue` | `FCigiRawEnvParser` | `ACamSimEnvironment` |
| `WeatherQueue` | `FCigiRawEnvParser` | `ACamSimEnvironment` |
| `RateCtrlQueue` | `FRateCtrlProcessor` | `FCamSimEntityManager` |
| `ArtPartQueue` | `FArtPartProcessor` | `FCamSimEntityManager` |
| `CompCtrlQueue` | `FCompCtrlProcessor` | `FCamSimEntityManager` |
| `HatHotReqQueue` | `FHatHotReqProcessor` (opcode 24) | `FCigiQueryHandler` |
| `LosSegReqQueue` | `FLosSegReqProcessor` (opcode 25) | `FCigiQueryHandler` |
| `LosVectReqQueue` | `FLosVectReqProcessor` (opcode 26) | `FCigiQueryHandler` |

The camera's packets travel as one item per datagram so the game thread reads the
platform pose and the gimbal, FOV and sensor packets at one point
(`ACamSimCamera::ApplyHostPlatformState`, from the entity-manager tick): a datagram
that lands mid-frame can't apply its gimbal a frame before its pose.

The camera/non-camera split is the key invariant: a single SPSC queue can only
have one consumer. Routing at the producer side (`FEntityCtrlProcessor`) keeps
`ACamSimCamera` and `FCamSimEntityManager` as independent consumers with no
shared state.

### Why raw parsing for environment packets?

CCL's `CigiHoldEnvCtrl` mechanism merges Celestial (opcode 9) and Atmosphere
(opcode 10) packets across frames before dispatching to event processors. This
makes per-packet event delivery unreliable. `FCigiRawEnvParser` scans the raw
UDP buffer *before* CCL sees it and enqueues environment structs directly,
bypassing the merge mechanism. Weather (opcode 12) is included for consistency.

## Object Ownership

```
UCamSimSubsystem  (UGameInstanceSubsystem — created with GameInstance)
│
├── FCamSimConfig          (value — loaded once in Initialize)
├── FCigiReceiver*         (raw ptr — started in Initialize, stopped in Deinitialize)
├── FMultiViewFrameSink*   (raw ptr — opened in Initialize, closed in Deinitialize)
├── FCamSimEntityManager*  (raw ptr — created in Initialize, deleted in Deinitialize)
│   └── TMap<uint16, ACamSimEntity*>  (actors owned by UWorld)
├── FCigiSender*           (raw ptr — opened in Initialize; FlushFrame() called via Tick())
└── FCigiQueryHandler*     (raw ptr — Tick() called from FCamSimEntityManager::Tick())

ACamSimCamera  (AActor — placed in level or spawned by GameMode; orchestrates the tick)
└── UCesiumGlobeAnchorComponent
└── USceneCaptureComponent2D                 (sensor pose/FOV/post-process holder; never captured — the GPU sensor graph reads the primary view instead)
    └── UCameraComponent SensorCamera        (the player's view target in primary mode, ROADMAP 3A)
└── UCesiumOriginShiftComponent              (rebases the georeference every render.origin_shift_distance_m)
└── UCamSimGimbalComponent                   (gimbal angles, slew)
└── UCamSimSensorComponent                   (waveband, polarity, FOV presets)
└── UCamSimCaptureComponent                  (render targets, readback, sensor model, encoder thread)
└── FCamSimPlatformRig         (value)       (platform pose, attachment, first-person view)
└── FCamSimTelemetryAssembler  (value)       (KLV telemetry: pose, gimbal, FOV, frame centre)
└── FCamSimStreamingController (value)       (Cesium streaming cameras, LOD, terrain gate)

ACamSimEnvironment  (AActor — spawned by GameMode)
└── references to ADirectionalLight, ASkyAtmosphere, ASkyLight,
    UExponentialHeightFog, AVolumetricCloud in the level

ACamSimEntity  (AActor — spawned at runtime by FCamSimEntityManager)
└── UCesiumGlobeAnchorComponent
└── UStaticMeshComponent   (static entities)
└── UPoseableMeshComponent (articulated entities — allows per-bone transforms)
└── UPointLightComponent × 5 (NavLightRed/Green/White, StrobeLight, LandingLight)
```

`FCamSimEntityManager` is a `FTickableGameObject` — it registers with UE's
global tickable list on construction and unregisters on destruction, so it
receives `Tick()` calls without being an `AActor`.

## Key Source Files

| File | Role |
|------|------|
| `CIGI/CigiReceiver.h/.cpp` | UDP thread, CCL parsing, queue routing |
| `CIGI/CigiSender.h/.cpp` | IG→Host UDP: SOF heartbeat + HAT/HOT + LOS responses |
| `CIGI/CigiQueryHandler.h/.cpp` | Drain query queues, run UE line traces, stage responses via provider-neutral geospatial transforms |
| `Geospatial/CamSimGeospatialProvider.h/.cpp` | Geospatial provider facade (Phase F foundation, Cesium adapter) |
| `CIGI/CigiPacketTypes.h` | All CIGI struct definitions |
| `Camera/CamSimCamera.h/.cpp` | Sensor actor: tick orchestration, CIGI view state |
| `Camera/CamSimPlatformRig.h/.cpp` | Platform pose from CIGI, attachment, first-person view |
| `Camera/CamSimCaptureComponent.h/.cpp` | Capture (grab request), async GPU readback, sensor model dispatch, encoder thread |
| `Camera/CamSimFrameGrabExtension.h/.cpp` | Scene view extension: runs the GPU sensor graph in place of the tonemapper (primary view) and reads its NV12 output back into the readback ring |
| `Thermal/ThermalModel`, `ThermalSky`, `BandRadiance`, `ThermalMaterials` | Thermal IR (ROADMAP 4A): closed-form class temperatures, sky temperature, in-band Planck LUT, material table |
| `Thermal/ThermalFrameBuilder`, `ThermalFrameSources` | Per-tick `FThermalFrameParams` from clock, environment, camera and entities |
| `Thermal/ThermalReference.h/.cpp` | `CamSimThermalRef::EvaluatePixel`, the CPU mirror of `ThermalCS` (`Shaders/Private/CamSimThermal.usf`) |
| `Camera/ThermalTsrAlpha.h` | `r.TSR.AlphaChannel=1` while thermal IR runs (RGBA16F TSR for radiance) |
| `Camera/CamSimFrameStats.h/.cpp` | Per-frame render stats JSONL and scene-render counter (bench harness) |
| `Health/CamSimSnapshotService.h/.cpp` | `GET /snapshot` and `GET /snapshot/sensor` (ROADMAP 3B): the next sensor-graph output frame (the NV12 that is encoded) as PNG; since 3B.2 both serve the same image (there is no pre-sensor frame) |
| `Camera/CamSimTelemetryAssembler.h/.cpp` | Telemetry behind the KLV tags, boresight frame centre |
| `Camera/CamSimStreamingController.h/.cpp` | Cesium streaming cameras, FOV-scaled culled SSE, terrain gate |
| `Entity/CamSimEntityManager.h/.cpp` | Entity lifecycle management |
| `Entity/CamSimEntity.h/.cpp` | Per-entity actor, DR, art parts, lights |
| `Entity/EntityTypeTable.h/.cpp` | Type ID → asset path lookup |
| `Environment/CamSimEnvironment.h/.cpp` | Sky, fog, weather from CIGI |
| `Encoder/MultiViewFrameSink.h/.cpp` | Multi-view fan-out (per-view digital zoom) |
| `Encoder/VideoEncoder.h/.cpp` | Per-view FFmpeg H.264 + MPEG-TS + KLV |
| `Metadata/KlvBuilder.h/.cpp` | MISB ST 0601 KLV |
| `Config/CamSimConfig.h/.cpp` | JSON + env var config |
| `Subsystem/CamSimSubsystem.h/.cpp` | Lifetime owner for all subsystems |
