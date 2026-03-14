# CamSim

Synthetic sensor simulator: CIGI 3.3 UDP → Cesium/UE5 render → H.264 MPEG-TS + MISB ST 0601 KLV → UDP multicast.

## Goals

- Cross platform: Linux, MacOS, Cloud (Docker)
- Rich synthetic image generator suitable for user training, simulation, and aided target recognition (ATR) training.
- Standards compliant (CIGI 3.3)
- Realism: WGS-84 earth model
- Open source alternative to MetaVR's Virtual Reality Scene Generator (VRSG)

## Notes

- Keep the `Plan.md` file up to date as phases are implemented.
- Document any editor changes that need to be made by a human in `TODO.md`. These may include new assets, textures, materials, etc.

## Commands

| Command                        | Description                                                   |
| ------------------------------ | ------------------------------------------------------------- |
| `scripts/repo_setup.sh`        | One-time: fetch Cesium + glTFRuntime plugins                  |
| `scripts/build_thirdparty.sh`  | Build CCL + FFmpeg static libs (cached in .build_tmp/)        |
| `scripts/run.sh`               | Launch UE5 in game mode (supports --build, --headless, --log) |
| `scripts/run.sh --build`       | Build + launch                                                |
| `scripts/send_cigi_test.py`    | Send CIGI 3.3 test packets (--sweep, --circle)                |
| `scripts/validate_klv.py`      | Decode/validate MISB ST 0601 KLV from UDP or .ts file         |
| `scripts/test_video_output.sh` | ffprobe/ffplay stream validation                              |
| `scripts/ci_validate.sh`       | Integration test (health wait + video/KLV validation)         |

## Architecture

```
camsim/
  unreal_project/CamSimTest/       # UE5.7 project root
    Source/CamSimTest/             # C++ module
      Camera/                      # SceneCapture2D, GPU readback, gimbal, sensor components
      CIGI/                        # UDP receiver/sender, packet parsing, terrain queries
      Config/                      # YAML config loader (rapidyaml) + env var overrides
      Encoder/                     # FFmpeg H.264/H.265 → MPEG-TS → UDP multicast
      Entity/                      # Actor lifecycle, dead-reckoning, articulated parts
      Environment/                 # Sky, fog, weather, day/night
      Geospatial/                  # Cesium terrain queries, WGS84 conversions
      Metadata/                    # MISB ST 0601/ST 0102 KLV builder
      Sensor/                      # CPU post-process: EO/IR/NVG effects
      Subsystem/                   # UGameInstanceSubsystem lifecycle owner
      GameMode/                    # Minimal game mode, no pawn
      Tests/                       # UE5 Automation tests (29 tests across 5 files)
    Source/ThirdParty/
      CCL/                         # CIGI Class Library (static lib)
      FFmpeg/                      # libavcodec/format/util/swscale + libx264
    Config/                        # DefaultEngine.ini, DefaultGame.ini
  deploy/                          # Dockerfile, docker-compose.yml, entrypoint.sh, camsim_config.yaml
  scripts/                         # Build, run, test, validation scripts
  docs/                            # architecture.md, configuration.md, klv-tags.md, etc.
```

## Data Flow

```
CIGI UDP → FCigiReceiver (FRunnable thread) → TSpscQueue
Game Thread → ACamSimCamera::Tick() → GlobeAnchor + SceneCapture
Render Thread → GPU readback (FRHIGPUTextureReadback)
Task Thread → FSensorPostProcess → FVideoEncoder → MPEG-TS + KLV → UDP multicast
```

Four threads: CIGI Receiver, Game, Render, Task (encoding). Communication via lock-free SPSC queues.

## Key Files

- `Source/CamSimTest/CamSimTest.Build.cs` — module build config, thirdparty linking
- `deploy/camsim_config.yaml` — canonical runtime config (100+ params, all have `CAMSIM_*` env overrides)
- `Source/CamSimTest/Config/CamSimConfig.h` — `FCamSimConfig` struct, YAML + env var loading
- `Source/CamSimTest/Subsystem/CamSimSubsystem.h` — lifecycle owner, Pimpl pattern (`FSubsystemImpl`)
- `Source/CamSimTest/Camera/CamSimCamera.h` — main actor: capture, gimbal, sensor, encode dispatch

## Code Style

- UE5 naming: `A` (Actor), `U` (UObject), `F` (struct/POD), `E` (enum), `I` (interface)
- PascalCase everywhere; verb-first functions (`CaptureAndEncode`, `DequeueEntityState`)
- `#include "CoreMinimal.h"` first, `.generated.h` last
- Forward declarations preferred over includes in headers
- `bEnableUndefinedIdentifierWarnings = false` in Build.cs — intentional for FFmpeg C headers
- Copyright: `// Copyright CamSim Contributors. All Rights Reserved.`
- CI runs `clang-format-17` (no local .clang-format file — uses default)

## Testing

- **C++ tests**: UE5 Automation framework in `Source/CamSimTest/Tests/` (29 tests across 5 files)
  - Run in editor: `Ctrl+Alt+F11` or `Automation` console command
- **Python validation**: `scripts/validate_klv.py`, `scripts/test_video_output.sh`
- **Integration**: `scripts/ci_validate.sh` (Docker headless + health wait + ffprobe + KLV check)
- **CIGI testing**: `scripts/send_cigi_test.py --sweep` or `--circle` for motion patterns

## Gotchas

- **ThirdParty must be built first**: Run `scripts/build_thirdparty.sh` before UE build — CCL + FFmpeg are static libs not checked in
- **Cesium coord order**: `TransformLongitudeLatitudeHeightPositionToUnreal(FVector(Lon, Lat, Alt))` — Longitude first, not Latitude
- **UE unit scale**: 1 UE unit = 1 cm — divide `FVector::Dist()` by 100 for metres
- **macOS multicast**: UDP multicast to 239.x.x.x on loopback requires `sudo route add -net 239.0.0.0/8 -interface lo0`, or use unicast: `CAMSIM_MULTICAST_ADDR=127.0.0.1`
- **CCL API quirk**: `GetDestEntityIDValid()`/`GetDestEntityID()` only in `CigiLosSegReqV3_2`, not V3
- **Fixed framerate**: Engine locked to 30fps via DefaultEngine.ini (`bUseFixedFrameRate=True`)
- **IDE false positives**: clang diagnostics for UE types are wrong — UBT handles includes at build time
- **Docker networking**: `network_mode: host` required for UDP multicast routing
- **rapidyaml bundled**: Source in `Config/ryml/` — excluded from pre-commit linting
- **KLV checksum**: Uses CRC-16/CCITT (not BCC-16 per spec) — intentional, matches validate_klv.py

## Environment

Config via `deploy/camsim_config.yaml` or env vars (env takes precedence):
- `CAMSIM_CIGI_PORT` — CIGI listen port (default 8888)
- `CAMSIM_MULTICAST_ADDR` — output address (default 239.1.1.1)
- `CAMSIM_MULTICAST_PORT` — output port (default 5004)
- `CAMSIM_VIDEO_CODEC` — `h264` | `h265` (default h264)
- `CAMSIM_ENCODER_TYPE` — `auto` | `nvenc` | `libx264` (default auto)
- `CAMSIM_OPTICAL_REALISM_ENABLED` — enable lens effects (default false)
- Full list: see `docs/configuration.md`

## Docker

```bash
cd deploy && docker compose up        # GPU (NVIDIA)
CAMSIM_ENCODER_TYPE=libx264 docker compose up  # CPU fallback (Mesa llvmpipe)
```

- Non-root user (uid 1000)
- Entrypoint auto-detects NVIDIA vs Mesa
- CPU path disables ray tracing via `-ini` flag
- Health: `camsim_health.json` written every 90 ticks
