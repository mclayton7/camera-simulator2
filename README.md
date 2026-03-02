# CamSim

Synthetic sensor simulator built on Unreal Engine 5 + Cesium for Unreal.

Receives camera and entity commands over CIGI 3.3 UDP, renders the scene using
Cesium photorealistic terrain and user-supplied 3D models, and emits a live
H.264/MPEG-TS video stream with embedded MISB ST 0601 KLV metadata.

```
CIGI 3.3 UDP ──► FCigiReceiver ──► UE5 Game Thread ──► SceneCaptureComponent2D
     ▲                │                    │                   │
     │                │           FCigiQueryHandler    Render Thread readback
     │                │           (line traces)                │
     │           FCigiSender                          FFmpeg libx264 encode
     │           (SOF + responses)                             │
     └─── IG→Host UDP                         MPEG-TS + KLV ──► UDP multicast
```

## Output

| Parameter | Value |
|-----------|-------|
| Codec | H.264 (libx264, configurable preset) |
| Resolution | 1920 × 1080 (configurable) |
| Frame rate | 30 fps fixed |
| Container | MPEG-TS |
| Metadata | MISB ST 0601 KLV (embedded as KLVA data stream) |
| Transport | UDP multicast (default `239.1.1.1:5004`) |

## CIGI Packet Support

### Host → IG (received)

| Opcode | Packet | Status |
|--------|--------|--------|
| 1 | IG Control | Consumed (sync) |
| 2 | Entity Control | Camera entity + scene entities (Phase 8) |
| 4 | Component Control | Lights, strobe, damage state (Phase 8) |
| 6 | Articulated Part Control | Skeletal mesh bones (Phase 8) |
| 8 | Rate Control | Dead-reckoning (Phase 8) |
| 9 | Celestial Sphere Control | Time of day, sun/moon (Phase 7) |
| 10 | Atmosphere Control | Fog, visibility (Phase 7) |
| 12 | Weather Control | Cloud layers (Phase 7) |
| 16 | View Control | Gimbal orientation (Phase 9) |
| 17 | Sensor Control | Sensor on/off, polarity, FOV preset (Phase 9) |
| 21 | View Definition | Camera FOV |
| 24 | HAT/HOT Request | Height above/of terrain query (Phase 10) |
| 25 | LOS Segment Request | Line-of-sight between two geodetic points (Phase 10) |
| 26 | LOS Vector Request | Line-of-sight along azimuth/elevation vector (Phase 10) |

### IG → Host (sent)

| Opcode | Packet | Status |
|--------|--------|--------|
| 101 | Start of Frame | Sent every tick (SOF heartbeat) |
| 102 | HAT/HOT Response | Terrain height reply to opcode 24 |
| 104 | LOS Response | Line-of-sight reply to opcodes 25/26 |

## Quick Start

### macOS (development)

```bash
# Build third-party libraries (CCL + FFmpeg with libx264)
./scripts/build_thirdparty_mac.sh

# Run the UE5 editor build in game mode
./scripts/run_mac.sh

# In another terminal — send a static camera pose
python3 scripts/send_cigi_test.py --lat 37.6213 --lon -122.379 --alt 1000 --pitch -30

# Validate the output stream
./scripts/test_video_output.sh
```

### Linux (Docker)

```bash
# Build and start the container
cd deploy/
docker compose up --build

# Send CIGI from the host machine
python3 scripts/send_cigi_test.py --host <container-ip>

# Watch the stream
ffplay udp://239.1.1.1:5004
```

Multicast on macOS loopback requires a route:
```bash
sudo route add -net 239.0.0.0/8 -interface lo0
```

## Documentation

| Document | Contents |
|----------|----------|
| [docs/architecture.md](docs/architecture.md) | System architecture, threading model, data flow |
| [docs/configuration.md](docs/configuration.md) | `camsim_config.json` field reference, environment variables |
| [docs/entity-rendering.md](docs/entity-rendering.md) | Phase 8: entity lifecycle, dead-reckoning, articulated parts, lights |
| [docs/terrain-feedback.md](docs/terrain-feedback.md) | Phase 10: HAT/HOT and LOS queries, SOF heartbeat |
| [Plan.md](Plan.md) | Full roadmap for Phases 7–12 |

## Directory Structure

```
camsim/
├── deploy/                         Docker deployment
│   ├── Dockerfile
│   ├── docker-compose.yml
│   ├── entrypoint.sh
│   └── camsim_config.json          Runtime configuration
├── docs/                           Documentation
├── scripts/                        Development and test utilities
│   ├── build_thirdparty_linux.sh   Build CCL + FFmpeg (Linux)
│   ├── build_thirdparty_mac.sh     Build CCL + FFmpeg (macOS)
│   ├── run_mac.sh                  Launch UE5 in game mode
│   ├── send_cigi_test.py           CIGI camera/environment test sender
│   ├── test_entity_rendering.py    CIGI entity/lights/DR test sender
│   ├── test_video_output.sh        ffprobe + ffplay stream validation
│   └── validate_klv.py             MPEG-TS KLV packet decoder
└── unreal_project/CamSimTest/
    └── Source/CamSimTest/
        ├── Camera/                 ACamSimCamera — capture + encode
        ├── CIGI/                   FCigiReceiver, FCigiSender, FCigiQueryHandler, packet structs
        ├── Config/                 FCamSimConfig — JSON + env var loading
        ├── Encoder/                FVideoEncoder — FFmpeg H.264 + MPEG-TS
        ├── Entity/                 ACamSimEntity, FCamSimEntityManager (Phase 8)
        ├── Environment/            ACamSimEnvironment — sky/fog/weather (Phase 7)
        ├── GameMode/               ACamSimGameMode
        ├── Metadata/               FKlvBuilder — MISB ST 0601
        ├── Subsystem/              UCamSimSubsystem — lifecycle owner
        └── ThirdParty/
            ├── CCL/                CIGI Class Library headers + static lib
            └── FFmpeg/             FFmpeg headers + static libs
```

## Links

- [CIGI Class Library (CCL)](https://github.com/Hadron/cigi-ccl)
- [Cesium for Unreal](https://github.com/CesiumGS/cesium-unreal)
- [Cesium for Unreal reference docs](https://cesium.com/learn/cesium-unreal/ref-doc/)
- [glTF Runtime Loading](https://github.com/rdeioris/glTFRuntime)
