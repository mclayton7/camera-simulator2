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
| `scripts/klv_conformance/check.js` | Check KLV against misb.js (packets.jsonl, .ts, or udp://) |
| `scripts/test_video_output.sh` | ffprobe/ffplay stream validation                              |
| `scripts/ci_validate.sh`       | Integration test (health wait + video/KLV validation)         |
| `scripts/ci_validate.sh --native` | Same, without Docker (macOS): launch headless + CIGI host + checks |

## Documentation

- Unreal Engine Cesium Plugin Documentation - https://cesium.com/learn/cesium-unreal/ref-doc/api-design.html and https://cesium.com/learn/cesium-unreal/ref-doc/index.html

## Architecture

```
camsim/
  unreal_project/CamSimTest/       # UE5.8 project root
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
      Tests/                       # UE5 Automation tests (209 tests across 31 files)
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
- `CppCompileWarningSettings.UndefinedIdentifierWarningLevel = WarningLevel.Off` in Build.cs — intentional for FFmpeg C headers
- Copyright: `// Copyright CamSim Contributors. All Rights Reserved.`

## Testing

- **C++ tests**: UE5 Automation framework in `Source/CamSimTest/Tests/` (209 tests across 31 files, all under `CamSim.*`)
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
- **Cesium coord order**: `TransformLongitudeLatitudeHeightPositionToUnreal(FVector(Lon, Lat, Alt))` — Longitude first, not Latitude
- **Never pass CIGI angles to `SetActorRotation`**: UE world axes are Cesium East-South-Up only at the georeference origin (+X = East, so heading 0 would face east). Use `GlobeAnchor->SetEastSouthUpRotation(CamSimFrames::CigiToEastSouthUp(...))` from `Geospatial/CigiFrames.h`
- **CIGI entity-relative fields**: when Attach State = Attach or a request's coordinate system = Entity, the Lat/Lon/Alt fields are X/Y/Z metre offsets in the reference entity's body frame (X fwd, Y right, Z down). Resolve via `UCamSimSubsystem::GetEntityGeoPose()`
- **macOS editor log**: `~/Library/Logs/CamSimTest/CamSimTest.log`, not `Saved/Logs/`. A cold `run.sh` start spends a few minutes compiling shaders before `LogCamSim` appears
- **Headless tests on macOS**: add `-DisablePython` — Python's startup type generation deadlocks under `-nullrhi` on macOS. Xcode 27 needs `MaxVersion` raised in the engine's `Engine/Config/Apple/Apple_SDK.json`, and a real (non-nullrhi) run needs `xcodebuild -downloadComponent MetalToolchain`
- **UE unit scale**: 1 UE unit = 1 cm — divide `FVector::Dist()` by 100 for metres
- **macOS multicast**: UDP multicast to 239.x.x.x on loopback requires `sudo route add -net 239.0.0.0/8 -interface lo0`, or use unicast: `CAMSIM_MULTICAST_ADDR=127.0.0.1`
- **CCL API quirk**: `GetDestEntityIDValid()`/`GetDestEntityID()` only in `CigiLosSegReqV3_2`, not V3
- **Fixed framerate**: Engine locked to 30fps via DefaultEngine.ini (`bUseFixedFrameRate=True`)
- **IDE false positives**: clang diagnostics for UE types are wrong — UBT handles includes at build time
- **Docker networking**: `network_mode: host` required for UDP multicast routing
- **rapidyaml bundled**: Source in `Config/ryml/` — excluded from pre-commit linting
- **KLV reference decoder is misb.js**: [vidterra/misb.js](https://github.com/vidterra/misb.js) (v0.1.30, commit `52c3837`) is what parses our KLV downstream, so it is the gold standard for ST 0601/0102 encoding — not the spec PDF, and not `validate_klv.py`. Checksum = 16-bit running sum over everything from the UL key through the checksum's own `01 02` bytes. misb.js throws on any tag shorter than its expected size (the whole packet is lost) and marks bad checksums `valid: false` rather than throwing. See ROADMAP.md 0.3.
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

```bash
cd deploy && docker compose up        # GPU (NVIDIA)
CAMSIM_ENCODER=libx264 docker compose up  # CPU fallback (Mesa llvmpipe)
```

- Non-root user (uid 1000)
- Entrypoint auto-detects NVIDIA vs Mesa
- CPU path disables ray tracing via `-ini` flag
- Health: HTTP server on port `8080` exposes `GET /live`, `GET /health` (alias for `/live`), `GET /ready`, and `GET /metrics` (Prometheus format). Legacy `camsim_health.json` file also still written every 90 ticks for backward compatibility.
