# CamSim Production Readiness & Image Quality Plan

## Context

CamSim is a synthetic sensor simulator (Cesium + UE5.7) targeting parity with commercial IGs
(MetaVR VRSG, Bohemia BlueIG). After a thorough review of the entire codebase — rendering
pipeline, Cesium integration, CIGI protocol, entity management, sensor simulation, encoder,
and deployment infrastructure — this plan identifies the highest-impact improvements needed
for production readiness, visual quality, and scalability.

**Current state**: Functional prototype (~6/10 production readiness). Strong foundations in
CIGI processing, async GPU readback, KLV metadata, and multi-view output. Key gaps in visual
quality (no AA, untuned Cesium LOD), operational robustness (no graceful shutdown, weak health
monitoring), and scalability (synchronous mesh loading, CPU-only sensor pipeline, software-only
encoding).

---

## Phase 1 — Image Quality: Cesium LOD Tuning + Anti-Aliasing

**Why**: The single largest visual quality win per line of code. `MaximumScreenSpaceError` is
not configured (Cesium default ~16 causes blurry terrain at ISR altitudes). No anti-aliasing
of any kind is enabled — no TAA, no TSR, no MSAA.

**Effort**: M | **Impact**: Critical (directly visible to end users)

### Changes

1. **Expose Cesium LOD controls** — `Config/CamSimConfig.h` + `.cpp`
   - Add `float MaximumScreenSpaceError = 4.0f` (Cesium default is 16; 4 = high quality)
   - Add `int32 MaximumCachedBytesMB = 2048` (2 GB tile cache; currently uncapped)
   - JSON keys: `"maximum_screen_space_error"`, `"maximum_cached_bytes_mb"`
   - Env vars: `CAMSIM_MAX_SSE`, `CAMSIM_MAX_CACHED_MB`

2. **Apply in tileset loop** — `Camera/CamSimCamera.cpp:153–162`
   - Add `It->MaximumScreenSpaceError = Cfg.MaximumScreenSpaceError;`
   - Add `It->MaximumCachedBytes = Cfg.MaximumCachedBytesMB * 1024LL * 1024LL;`

3. **Enable Temporal Super Resolution** — `Config/DefaultEngine.ini`
   - Add under `[/Script/Engine.RendererSettings]`:
     ```
     r.AntiAliasingMethod=4
     r.TemporalAA.Upscaling=1
     r.ScreenPercentage=100
     ```
   - TSR accumulates temporal history across frames with motion-compensated reprojection,
     reducing shimmer on thin geometry (power lines, fences, antennas). It runs at native
     resolution (no upscaling cost), producing cleaner edges than raw Lumen sampling alone.

4. **SceneCapture show flags** — `Camera/CamSimCamera.cpp` constructor (~line 64)
   - `SceneCapture->ShowFlags.SetTemporalAA(true);`
   - `SceneCapture->ShowFlags.SetAntiAliasing(true);`
   - `bAlwaysPersistRenderingState = true` (already set) is required for TSR history.

5. **Deploy config** — `deploy/camsim_config.json`
   - Add `"maximum_screen_space_error": 4.0` and `"maximum_cached_bytes_mb": 2048`

### Verification
- Capture a still frame at 5000ft AGL over a city. Compare SSE=16 vs SSE=4 — roads/buildings
  should be visibly sharper at 4.
- `stat SceneRendering` should show "TemporalSuperResolution" as the AA method.
- Verify 30fps holds (TSR is cheaper than supersampling).

---

## Phase 2 — Operational Reliability: Graceful Shutdown + Health Monitoring

**Why**: Production deployment requires clean container start/stop, structured health data,
and CIGI frame echo. Right now SIGTERM kills the process without flushing the H.264 stream
(corrupt final segment), the health file is just a frame counter, and SOF always sends
`LastHostFrame=0`.

**Effort**: M | **Impact**: Critical (blocks production deployment)

### Changes

1. **SIGTERM handler** — `Subsystem/CamSimSubsystem.cpp` Initialize()
   - Register `FCoreDelegates::ApplicationWillTerminateDelegate`
   - Delegate: log shutdown, call `VideoEncoder->Close()` (flushes H.264 trailer),
     `CigiSender->Close()`, `CigiReceiver->Stop()`, then `FPlatformMisc::RequestExit(false)`
   - Ensures MPEG-TS stream is finalized (receivers see clean EOF)

2. **Echo LastHostFrame in SOF** — `CIGI/CigiReceiver.h` + `CIGI/CigiSender.cpp`
   - In receiver: add `FIGCtrlProcessor` for opcode 1 (IG Control). Extract host frame
     counter, store in `TAtomic<uint32> LastHostFrameCntr`.
   - In `CamSimSubsystem::Tick()`: pass `CigiReceiver->GetLastHostFrame()` to
     `CigiSender->FlushFrame()`.
   - In sender: `SofPacket->SetLastRcvdIGFrame(LastHostFrame)` instead of 0.

3. **Structured health JSON** — `Subsystem/CamSimSubsystem.cpp`
   - Replace frame-counter text file with JSON:
     `{"frame":N, "encoder_ok":bool, "cigi_rx":N, "dropped":N, "uptime_s":N.N, "last_host_frame":N}`
   - Write to `camsim_health.json`.

4. **Watchdog escalation** — `Subsystem/CamSimSubsystem.cpp`
   - Add `WatchdogMaxReconnects` config field (default 3).
   - After N consecutive failed reconnects → escalate to `FailFast` (exit code 1 for
     container restart).

5. **Dockerfile HEALTHCHECK** — `deploy/Dockerfile`
   - `HEALTHCHECK --interval=10s --timeout=5s CMD test -f .../camsim_health.json && ...`

### Verification
- `docker kill --signal=TERM camsim` → ffprobe the captured .ts shows no truncation error.
- `docker inspect --format='{{.State.Health.Status}}' camsim` → "healthy".
- `send_cigi_test.py` → SOF response echoes host frame counter (not 0).

---

## Phase 3 — Hardware Encoding (NVENC) with Software Fallback

**Why**: libx264 `ultrafast` uses one full CPU core at 1920×1080 30fps. On NVIDIA GPU servers,
NVENC provides near-zero CPU encoding with sub-millisecond latency, freeing cores for CIGI
processing and sensor post-processing. Fallback to libx264 on CPU-only platforms.

**Effort**: L | **Impact**: High (CPU freed, latency reduced)

### Changes

1. **Encoder selection logic** — `Encoder/VideoEncoder.cpp:86–97`
   - Currently hardcodes `avcodec_find_encoder_by_name("libx264")`.
   - Change to: try `"h264_nvenc"` first if `Config.Encoder == "auto"` or `"nvenc"`.
     Fall back to `"libx264"` if NVENC unavailable.
   - NVENC options: `preset=p4`, `tune=ll` (low latency), `rc=cbr`, `gpu=0`.
   - Log which encoder was selected.

2. **Config field** — `Config/CamSimConfig.h`
   - Add `FString Encoder = TEXT("auto");` — values: `auto`, `nvenc`, `libx264`.
   - JSON: `"encoder"`, env: `CAMSIM_ENCODER`.

3. **Build script** — `scripts/build_thirdparty.sh`
   - Add `--enable-nvenc --enable-encoder=h264_nvenc` to FFmpeg configure when CUDA headers
     are present.

4. **Dockerfile** — `deploy/Dockerfile`
   - Add `libnvidia-encode-*` to apt-get (NVENC runtime library exposed by
     nvidia-container-toolkit).

### Verification
- `CAMSIM_ENCODER=nvenc` → log shows `using encoder h264_nvenc`.
- CPU usage drops from ~100% on one core to <10%.
- `CAMSIM_ENCODER=auto` on CPU-only → log shows `using encoder libx264` (graceful fallback).
- ffplay the stream — no visual degradation at 4 Mbps.

---

## Phase 4 — Entity Scalability: Async Loading + Mesh Caching

**Why**: `LoadStaticMeshFromPath()` / `LoadSkeletalMeshFromPath()` (CamSimEntity.cpp:38–62)
are synchronous — a 50MB glTF blocks the game thread for 200–500ms. No mesh caching means
50 F-16s re-parse the same glTF 50 times. No instanced rendering means 50 draw calls instead
of 1.

**Effort**: L | **Impact**: High (required for 100+ entity scenarios)

### Changes

1. **Async mesh loading** — `Entity/CamSimEntity.cpp`
   - For `/Game/...` paths: use `FStreamableManager::RequestAsyncLoad()`.
   - For `.glb/.gltf` paths: run `glTFLoadAssetFromFilename` via `Async(EAsyncExecution::ThreadPool, ...)`,
     apply mesh on game thread via `AsyncTask(ENamedThreads::GameThread, ...)`.
   - Show invisible/placeholder actor while loading.

2. **Mesh cache in EntityTypeTable** — `Entity/EntityTypeTable.h`
   - Add `TMap<uint16, TWeakObjectPtr<UStaticMesh>> StaticMeshCache` and similar for skeletal.
   - First load of a type populates the cache; subsequent entities reuse it.

3. **Hierarchical Instanced Static Mesh (HISM)** — `Entity/CamSimEntityManager.h/.cpp`
   - For `bSkeletal=false` types: maintain `TMap<uint16, UHierarchicalInstancedStaticMeshComponent*>`
     on a pool actor. Each CIGI entity maps to an instance index. `ApplyPose` updates the
     instance transform. Reduces draw calls from O(N) to O(unique_types).
   - Skeletal mesh entities (needing per-bone art-part control) remain individual actors.

4. **Entity budget** — `Config/CamSimConfig.h`
   - Add `int32 MaxEntities = 500;` and `bool bUseInstancedRendering = true;`.
   - Reject spawns beyond MaxEntities with a warning.

### Verification
- `send_cigi_test.py --sweep --count=100` → frame time stays <33ms, no stall spike.
- `stat SceneRendering` draw calls ≈ O(unique_types) not O(N) for identical static meshes.
- Entity spawn of unknown type shows async load in progress, mesh appears next frame.

---

## Phase 5 — GPU Sensor Post-Processing Pipeline

**Why**: `FSensorPostProcess::Process()` runs 10 sequential CPU effects with 8-band
ParallelFor per effect over 2M pixels. This costs 5–15ms on the encode thread, directly
competing with H.264 encoding. These are embarrassingly parallel per-pixel operations that
complete in <1ms on GPU.

**Effort**: XL | **Impact**: Medium-High (encode thread freed)

### Changes

1. **Post-process material approach** (preferred for UE5 maintainability):
   - Create post-process materials: `M_SensorIR`, `M_SensorNVG`, `M_SensorVignette`, `M_SensorNoise`.
   - In `CamSimCamera.cpp`: conditionally add via `SceneCapture->PostProcessSettings.AddBlendable()`
     based on `SensorComp->GetMode()`.
   - IR material: sample SceneColor → BT.601 luma → S-curve LUT → polarity inversion.
   - NVG material: luma → gamma 0.45 → green phosphor tint (`R=0, G=I, B=I*0.3`).
   - Vignetting: radial texture multiply.
   - Effects run on GPU before readback — CPU `SensorFX->Process()` becomes a no-op.

2. **Config toggle** — `Config/CamSimConfig.h`
   - Add `bool bGpuSensorEffects = true;`. When false → fall back to CPU pipeline
     (Mesa llvmpipe compatibility).

3. **Skip CPU pipeline** — `Camera/CamSimCamera.cpp` SubmitFrameToEncoder
   - When `bGpuSensorEffects`, skip `SensorFX->Process()` call.

### Verification
- Compare GPU and CPU outputs: should be visually identical for IR/NVG/EO.
- `SCOPE_CYCLE_COUNTER(STAT_CamSimEncode)` drops from ~15ms to <5ms.
- `bGpuSensorEffects=false` on llvmpipe → CPU pipeline still works.

---

## Phase 6 — CIGI Frame Synchronization + Wall-Clock PTS

**Why**: Commercial IGs guarantee SOF-to-render frame correlation and wall-clock PTS for
KLV alignment. CamSim's PTS is monotonic frame-index-based (not wall-clock), and there's
no binding between the CIGI frame and the render frame.

**Effort**: M | **Impact**: Medium (CIGI compliance, KLV accuracy)

### Changes

1. **IG Control processing** — `CIGI/CigiReceiver.cpp`
   - Add `FIGCtrlProcessor` for opcode 1 (IG Control).
   - Extract `FrameCntr` and `DatabaseID` from each IG Control packet.
   - Store `TAtomic<uint32> LastHostFrameCntr` on receiver.

2. **Wall-clock PTS** — `Encoder/VideoEncoder.cpp:363`
   - Change `YuvFrame->pts = FrameIdx` to wall-clock relative:
     ```cpp
     const double NowSec = FPlatformTime::Seconds();
     if (StartTimeSec == 0.0) StartTimeSec = NowSec;
     YuvFrame->pts = (int64)((NowSec - StartTimeSec) * Config.FrameRate);
     ```
   - KLV Tag 2 and video PTS become temporally correlated.

3. **KLV PTS alignment** — `Encoder/VideoEncoder.cpp` WriteKlvPacket
   - Derive KLV PTS from video PTS via `av_rescale_q()` instead of `FrameIdx * 3000`.

4. **Frame-sync diagnostic** — `Subsystem/CamSimSubsystem.cpp` health log
   - Add `host_frame_delta` to detect frame slip.

### Verification
- `validate_klv.py --count 10` → KLV Tag 2 timestamps increment at ~33ms intervals.
- ffplay shows no PTS discontinuity warnings.
- SOF response echoes host frame counter (covered by Phase 2).

---

## Phase 7 — Docker Hardening & CI Pipeline

**Why**: No CI pipeline, no automated validation, no versioned releases. Customers
evaluating against MetaVR need reproducible, tested builds.

**Effort**: L | **Impact**: Medium (release engineering)

### Changes

1. **Multi-stage Dockerfile** — `deploy/Dockerfile`
   - Stage 1: build FFmpeg + CCL. Stage 2: runtime only.
   - Add OCI labels: `org.opencontainers.image.version`, `.revision` (git SHA).
   - Add HEALTHCHECK instruction.

2. **CI validation script** — new: `scripts/ci_validate.sh`
   - Start container headless (Mesa llvmpipe), wait for health file (60s timeout).
   - `ffprobe` verify H.264 + KLVA streams.
   - `validate_klv.py --check-crc --check-tags` verify KLV integrity.
   - Capture 3s with ffmpeg, verify no decode errors. Exit 0/1.

3. **Readiness probe** — `deploy/entrypoint.sh`
   - After launching UE, wait for `camsim_health.json` before signaling readiness.
   - For k8s: readiness probe checks health file freshness.

4. **docker-compose.yml** — `deploy/docker-compose.yml`
   - Add `healthcheck`, `restart: unless-stopped`, `logging: max-size: 100m, max-file: 3`.

### Verification
- `scripts/ci_validate.sh` exits 0 on a CI runner (no GPU).
- `docker inspect` shows revision label matching git SHA.
- `docker kill --signal=TERM` → graceful shutdown log messages appear.

---

## Summary

| Phase | Theme | Key Files | Effort | Impact |
|-------|-------|-----------|--------|--------|
| 1 | Cesium LOD + TSR | CamSimCamera.cpp, CamSimConfig.h, DefaultEngine.ini | M | Critical — largest visual win |
| 2 | Graceful Lifecycle | CamSimSubsystem.cpp, CigiReceiver.h, CigiSender.cpp | M | Critical — blocks prod deploy |
| 3 | NVENC Encoding | VideoEncoder.cpp, CamSimConfig.h, Dockerfile | L | High — frees CPU |
| 4 | Entity Scalability | CamSimEntity.cpp, EntityTypeTable.h, EntityManager | L | High — 100+ entities |
| 5 | GPU Sensor Pipeline | SensorPostProcess, CamSimCamera.cpp, new materials | XL | Med-High — encode freed |
| 6 | CIGI Frame Sync | CigiReceiver.cpp, VideoEncoder.cpp | M | Medium — CIGI compliance |
| 7 | Docker/CI | Dockerfile, ci_validate.sh, docker-compose.yml | L | Medium — release eng |

### Files Modified Across All Phases
- `Camera/CamSimCamera.cpp` — Phases 1, 5
- `Config/CamSimConfig.h/.cpp` — Phases 1, 2, 3, 4, 5
- `Config/DefaultEngine.ini` — Phase 1
- `Subsystem/CamSimSubsystem.cpp` — Phases 2, 6
- `CIGI/CigiReceiver.h/.cpp` — Phases 2, 6
- `CIGI/CigiSender.cpp` — Phase 2
- `Encoder/VideoEncoder.cpp` — Phases 3, 6
- `Entity/CamSimEntity.cpp` — Phase 4
- `Entity/EntityTypeTable.h` — Phase 4
- `Entity/CamSimEntityManager.h/.cpp` — Phase 4
- `Sensor/SensorPostProcess.h/.cpp` — Phase 5
- `deploy/Dockerfile` — Phases 2, 3, 7
- `deploy/camsim_config.json` — Phases 1, 3
- `deploy/docker-compose.yml` — Phase 7
- `deploy/entrypoint.sh` — Phase 7
- `scripts/ci_validate.sh` (new) — Phase 7
- `scripts/build_thirdparty.sh` — Phase 3
