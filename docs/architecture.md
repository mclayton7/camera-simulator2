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
│  • ACamSimCamera::Tick()    — drains CameraEntityQueue          │
│  • FCamSimEntityManager::Tick() — drains EntityStateQueue       │
│                                   RateCtrlQueue                 │
│                                   ArtPartQueue                  │
│                                   CompCtrlQueue                 │
│                                   scenario.entities             │
│  • ACamSimEnvironment::Tick() — drains Celestial/Atmos/Weather  │
│  • FCigiQueryHandler::Tick() — drains HatHotReqQueue            │
│                                        LosSegReqQueue           │
│                                        LosVectReqQueue          │
│                               UE line traces → FCigiSender      │
│  • FCigiSender::FlushFrame() — SOF + response datagram → host   │
│  • Calls SceneCaptureComponent2D::CaptureScene()                │
│  • Enqueues render command for GPU readback                     │
└──────────────────────────┬──────────────────────────────────────┘
                           │ ENQUEUE_RENDER_COMMAND
┌──────────────────────────▼──────────────────────────────────────┐
│  Render Thread                                                  │
│  • RHICmdList.ReadSurfaceData() → TArray<FColor>                │
│  • Dispatches async task for encoding                           │
└──────────────────────────┬──────────────────────────────────────┘
                           │ AsyncTask(AnyBackgroundThreadNormalTask)
┌──────────────────────────▼──────────────────────────────────────┐
│  Task Thread (pool)                                             │
│  • Sensor post-process (EO/IR/NVG + quality profile)            │
│  • Fan-out to one or more output views                          │
│  • Per-view: sws_scale + libx264 + KLV + MPEG-TS UDP send       │
│  • Optional JSONL ground-truth sidecar write                    │
└─────────────────────────────────────────────────────────────────┘
```

`bEncoderBusy` (atomic bool) prevents the game thread from issuing a new
capture while the previous frame is still encoding, maintaining a natural
back-pressure that keeps encoding load at exactly one frame in flight.

## SPSC Queue Routing

`FCigiReceiver` maintains twelve SPSC queues. The receiver thread is the sole
producer for all queues. Each queue has exactly one game-thread consumer:

| Queue | Producer | Consumer |
|-------|----------|----------|
| `CameraEntityQueue` | `FEntityCtrlProcessor` (when `EntityId == CameraEntityId`) | `ACamSimCamera` |
| `EntityStateQueue` | `FEntityCtrlProcessor` (all other entity IDs) | `FCamSimEntityManager` |
| `ViewDefQueue` | `FViewDefProcessor` | `ACamSimCamera` |
| `SensorCtrlQueue` | `FSensorCtrlProcessor` (opcode 17) | `ACamSimCamera` |
| `ViewCtrlQueue` | `FViewCtrlProcessor` (opcode 16) | `ACamSimCamera` |
| `CameraArtPartQueue` | `FArtPartProcessor` (camera entity art parts) | `ACamSimCamera` |
| `CelestialQueue` | `FCigiRawEnvParser` (raw bytes, bypasses CCL hold) | `ACamSimEnvironment` |
| `AtmosphereQueue` | `FCigiRawEnvParser` | `ACamSimEnvironment` |
| `WeatherQueue` | `FCigiRawEnvParser` | `ACamSimEnvironment` |
| `RateCtrlQueue` | `FRateCtrlProcessor` | `FCamSimEntityManager` |
| `ArtPartQueue` | `FArtPartProcessor` | `FCamSimEntityManager` |
| `CompCtrlQueue` | `FCompCtrlProcessor` | `FCamSimEntityManager` |
| `HatHotReqQueue` | `FHatHotReqProcessor` (opcode 24) | `FCigiQueryHandler` |
| `LosSegReqQueue` | `FLosSegReqProcessor` (opcode 25) | `FCigiQueryHandler` |
| `LosVectReqQueue` | `FLosVectReqProcessor` (opcode 26) | `FCigiQueryHandler` |

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

ACamSimCamera  (AActor — placed in level or spawned by GameMode)
└── UCesiumGlobeAnchorComponent
└── USceneCaptureComponent2D → UTextureRenderTarget2D

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
| `Camera/CamSimCamera.h/.cpp` | Capture, GPU readback, encoder dispatch |
| `Entity/CamSimEntityManager.h/.cpp` | Entity lifecycle management |
| `Entity/CamSimEntity.h/.cpp` | Per-entity actor, DR, art parts, lights |
| `Entity/EntityTypeTable.h/.cpp` | Type ID → asset path lookup |
| `Environment/CamSimEnvironment.h/.cpp` | Sky, fog, weather from CIGI |
| `Encoder/MultiViewFrameSink.h/.cpp` | Multi-view fan-out and optional ground-truth sidecar |
| `Encoder/VideoEncoder.h/.cpp` | Per-view FFmpeg H.264 + MPEG-TS + KLV |
| `Metadata/KlvBuilder.h/.cpp` | MISB ST 0601 KLV |
| `Config/CamSimConfig.h/.cpp` | JSON + env var config |
| `Subsystem/CamSimSubsystem.h/.cpp` | Lifetime owner for all subsystems |
