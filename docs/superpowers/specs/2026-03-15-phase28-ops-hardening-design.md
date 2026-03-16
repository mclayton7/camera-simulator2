# Phase 28 — Operational Hardening (Sprint 1) Design Spec

**Date:** 2026-03-15
**Status:** Draft
**Scope:** 28A (CI tests), 28B (structured logging), 28C (HTTP health), 28D (config validation), 28G (latency tracking)

## Problem

CamSim is approaching production deployment (Docker Compose now, Kubernetes later). The core pipeline is feature-complete through Phase 27, but operational infrastructure has gaps:

- All registered unit tests exist but don't run in CI
- All logging is unstructured UE_LOG text — not ingestible by ELK/Datadog
- Health checks are file-based only — no HTTP probes for K8s
- Config values aren't validated — invalid values silently fall back to defaults
- No per-frame latency visibility across the 4-thread pipeline

## Design

### 28A — Unit Tests in CI

New `unit-tests` job in `.github/workflows/ci.yml`, runs after `docker-build` on **all PRs and pushes**.

Launches the Docker image with the full `.uproject` path:
```bash
UnrealEditor-Cmd /opt/camsim/CamSimTest/CamSimTest.uproject \
  -ExecCmds="Automation RunAll; Quit" -NullRHI -NoSound -Unattended
```

`-NullRHI` means no GPU — all registered tests are logic-only (config, KLV, sensor math, entity, overlay, etc.). The job parses the automation log for test failure patterns and exits non-zero on any failure. Test log uploaded as artifact on failure.

**Note:** The exact log-parse regex should be validated against the UE5.7 automation output format in the Docker image before merging. Common patterns: `LogAutomationController: ...Fail` or `Test Completed. Result={Fail}`.

### 28B — Structured JSON Logging

**New files:** `Logging/CamSimJsonLogger.h/.cpp`

`FCamSimJsonLogger` — non-UObject struct owned by `FSubsystemImpl`. Writes one JSON line per event to a configurable file path.

```cpp
struct FCamSimJsonLogger
{
    bool Open(const FString& FilePath);
    void Close();
    void Log(const TCHAR* Severity, const TCHAR* Category,
             const TCHAR* Message, const TMap<FString, FString>& Fields = {});
};
```

Output format:
```json
{"ts":"2026-03-15T12:00:00.000Z","severity":"info","category":"encoder","msg":"opened","codec":"h264","bitrate":"4000000"}
```

Key events: startup config digest, config validation errors, encoder open/close/error, CIGI bind, watchdog reconnect, frame drop spikes, entity spawn/destroy, shutdown.

Uses `IFileHandle` for append-only writes (same pattern as `FCocoAnnotationWriter`). Thread-safe via a lock-free SPSC queue: producers enqueue `FStructuredLogEntry` structs, and the game-thread tick flushes the queue to disk. This matches the project's established lock-free architecture (no `FCriticalSection` anywhere in the codebase). UE_LOG remains unchanged for console output.

**Log rotation:** Max file size configurable via `operational.structured_log_max_mb` (default 100, env: `CAMSIM_STRUCTURED_LOG_MAX_MB`). On overflow: close, rename to `.1`, reopen. Prevents disk exhaustion in long-running Docker containers.

Config: `operational.structured_log_path` (YAML) / `CAMSIM_STRUCTURED_LOG_PATH` (env). Empty = disabled.

### 28C — HTTP Health Endpoints

**New files:** `Health/CamSimHealthServer.h/.cpp`

Uses UE5's `HTTPServer` module (`FHttpServerModule::GetModule()` → `IHttpRouter`). Owned by `FSubsystemImpl`.

**HTTPServer module availability:** This module is available in the `UnrealEditor-Cmd` target used in Docker headless builds. If unavailable in a future shipping target, fallback is a raw `FSocket`-based TCP listener (same pattern as `FCigiReceiver`).

**Routes:**

| Route | 200 when | 503 when |
|-------|----------|----------|
| `GET /live` | Game loop ticked within 5s | Tick stalled |
| `GET /ready` | Encoder open AND CIGI running AND first frame encoded | Any subsystem not ready |

Response bodies:
- `/live` 200: `{"status":"ok"}`
- `/live` 503: `{"status":"stalled","last_tick_ago_s":12.3}`
- `/ready` 200: `{"status":"ready","encoder":true,"cigi":true,"first_frame":true}`
- `/ready` 503: `{"status":"not_ready","encoder":false,"cigi":true,"first_frame":false}`

Config: `operational.health_http_enabled` (default false) / `CAMSIM_HEALTH_HTTP_ENABLED`, `operational.health_http_port` (default 8080) / `CAMSIM_HEALTH_HTTP_PORT`.

Optional `GET /metrics` endpoint returns the same Prometheus exposition format currently written to the textfile, eliminating the file-based textfile-collector dependency for K8s deployments.

**Build.cs:** Add `"HTTPServer"` to `PublicDependencyModuleNames`.

### 28D — Config Validation

New method `TArray<FString> FCamSimConfig::Validate() const`, called after `Load()` in `UCamSimSubsystem::Initialize()`.

Returns human-readable error strings. If non-empty, all errors logged (UE_LOG + structured logger) and `bLoadedSuccessfully = false`.

Rules:

| Field | Valid Range |
|-------|-------------|
| `CaptureWidth` | [64, 7680] |
| `CaptureHeight` | [64, 4320] |
| `VideoBitrate` | [100000, 100000000] |
| `FrameRate` | [1.0, 120.0] |
| `CigiPort`, `CigiResponsePort`, `MulticastPort` | [1, 65535] |
| `HFovDeg` | (0.0, 180.0] |
| `GimbalPitchMin < GimbalPitchMax` | — |
| `GimbalYawMin < GimbalYawMax` | — |
| `MaxEntities` | [1, 10000] |
| `StartHour` | [0.0, 24.0] |
| `WatchdogMaxReconnects` | [0, 100] |
| `EncoderWatchdogIntervalTicks` | [30, 9000] |
| `ReadbackReadyPolls` | [0, 10] |
| `VideoCodec` | one of: `h264`, `h265` |
| `Encoder` | one of: `auto`, `nvenc`, `libx264`, `libx265` |
| `CaptureWidth`, `CaptureHeight` | must be even (H.264 requirement) |
| `Performance.RenderFrameRateHz` | [1.0, 120.0] |
| `Performance.OutputFrameRateHz` | [1.0, RenderFrameRateHz] |

Each error includes: field name, current value, valid range.

### 28G — Per-Frame Latency Tracking

**New files:** `Diagnostics/PipelineLatencyTracker.h/.cpp`

`FPipelineLatencyTracker` — lightweight struct. Timestamps 7 pipeline stages with `FPlatformTime::Cycles64()`:

1. CigiDequeue
2. GameTickStart
3. ReadbackIssue
4. ReadbackComplete
5. SensorStart
6. SensorEnd
7. EncodeComplete

Ring buffer of `10 * OutputFrameRateHz` frames (always 10 seconds, adapts to 30/60fps). `ComputePercentiles()` returns P50/P95/P99 for readback, sensor, encode, and total (CigiDequeue → EncodeComplete) deltas in microseconds.

**Threading model — which thread records which stage:**
- **Game thread**: MarkCigiDequeue (in `ApplyCigiState()`), MarkGameTickStart (top of `Tick()`)
- **Render thread**: MarkReadbackIssue (inside `ENQUEUE_RENDER_COMMAND`), MarkReadbackComplete (after `Lock()` returns)
- **Task thread**: MarkSensorStart, MarkSensorEnd (in `SubmitFrameToEncoder` async lambda)
- **Encoder thread**: MarkEncodeComplete + CommitFrame (in `FEncoderThread::Run()`)

Every 90 ticks, subsystem reads the committed buffer and writes percentiles to health JSON and Prometheus file.

**Thread safety:** Double-buffer pattern (no locks, matching project's lock-free architecture). Each frame's staging area is a `FPipelineLatencyRecord` indexed by `TAtomic<uint32> WriteIndex`. The encoder thread writes to `Records[WriteIndex % N]` and atomically increments the index. The subsystem reads the `N` most recent records from the opposite side of the buffer. This matches the existing `FFrameDropStats` pattern (atomic counters, game-thread reads) and avoids the spinlock/mutex that would deviate from codebase conventions.

Config: `performance.track_pipeline_latency` (default false) / `CAMSIM_TRACK_PIPELINE_LATENCY`.

## New Config

```yaml
operational:
  structured_log_path: ""        # empty = disabled
  health_http_enabled: false     # enable for K8s
  health_http_port: 8080

performance:
  track_pipeline_latency: false  # added alongside existing performance fields
```

## Files

| Action | File |
|--------|------|
| New | `Logging/CamSimJsonLogger.h/.cpp` |
| New | `Health/CamSimHealthServer.h/.cpp` |
| New | `Diagnostics/PipelineLatencyTracker.h/.cpp` |
| New | `Tests/Phase28OpsTest.cpp` (10 tests) |
| Modify | `CamSimTest.Build.cs` — add `HTTPServer` |
| Modify | `Config/CamSimConfig.h` — `FOperationalConfig`, `Validate()`, `track_pipeline_latency` |
| Modify | `Config/CamSimConfig.cpp` — parse YAML/env, implement `Validate()` |
| Modify | `Subsystem/CamSimSubsystem.cpp` — own new components, call `Validate()`, structured log events, latency percentiles |
| Modify | `Camera/CamSimCamera.h/.cpp` — latency tracker member + stage timestamps |
| Modify | `Encoder/EncoderThread.cpp` — encode-complete timestamp |
| Modify | `deploy/camsim_config.yaml` — `operational:` block |
| Modify | `.github/workflows/ci.yml` — `unit-tests` job |

## Verification

1. **28A**: PR push → `unit-tests` job passes
2. **28B**: `CAMSIM_STRUCTURED_LOG_PATH=/tmp/camsim.jsonl` → valid JSON lines with `jq`
3. **28C**: `CAMSIM_HEALTH_HTTP_ENABLED=1` → `curl :8080/live` → 200, `curl :8080/ready` → 200 after init
4. **28D**: `CAMSIM_MULTICAST_PORT=99999` → validation error logged, `bLoadedSuccessfully=false`
5. **28G**: `CAMSIM_TRACK_PIPELINE_LATENCY=1` → health JSON + Prometheus include latency percentiles

## Implementation Order

1. 28D — Config Validation (smallest, no new files)
2. 28B — Structured JSON Logging (foundation for observability)
3. 28G — Per-Frame Latency Tracking (integrates into health/prometheus)
4. 28C — HTTP Health Endpoints (builds on health infra)
5. 28A — Unit Tests in CI (CI workflow, independent of C++)
