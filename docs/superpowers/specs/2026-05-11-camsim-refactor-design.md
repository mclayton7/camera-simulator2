# CamSim Repository Refactor — Design Spec

**Date:** 2026-05-11
**Status:** Draft (awaiting review)
**Scope:** Repository-wide refactor across six sequential phases on `main`.

## Problem

A four-agent review of the CamSim codebase (~22 k lines of production C++ across 22 modules) surfaced 35 findings spanning correctness, performance, clarity, and module cohesion. The findings cluster into themes that are individually small but collectively material to a high-performance scene generator:

- Cross-thread state with relaxed atomics that is correct on x86 but latent on ARM (devs work on Apple Silicon).
- Per-frame heap allocations in the sensor post-process and encoder paths (~250 MB/s allocator pressure at 1080p30).
- A 1629-line `ACamSimCamera` actor that orchestrates 17+ concerns and runs three independent `TActorIterator` world sweeps per tick.
- Public headers leaking FFmpeg, the entire `FCamSimConfig` mega-struct, and KLV-encoding types into TUs that have no business including them.
- Module layout that lists 22 flat directories with no grouping signal — protocol adapters (`CIGI`, `DIS`, `Streaming/CotSender`) and observability modules (`Diagnostics`, `Health`, `Logging`) live as peers of unrelated concerns.
- Comment debt: dozens of `// Phase NN[A-Z]?` banner comments narrating completed work, which now narrate WHAT rather than WHY.

None of these is a P0-blocker today, but together they raise the cost of every future change — incremental build times, reviewer cognitive load, and the risk that a future ARM Linux deployment surfaces latent races that were invisible in x86 testing.

## Goals

- **Eliminate latent cross-thread races** that today rely on x86 memory ordering guarantees.
- **Reduce per-frame allocator pressure by ≥50%** in the rendering hot path, measured by `Phase27PerformanceTest`.
- **Shrink `CamSimCamera.cpp`** from 1629 lines to ≤900 lines by extracting cohesive helpers.
- **Make public headers slim** — no FFmpeg, no full config, no encoding internals leaking via includes.
- **Reorganize modules** into a structure where every top-level directory has a clear purpose.
- **Sweep comment debt** so remaining comments explain WHY, never WHAT.

## Non-Goals

- Rewriting the CCL CIGI parser.
- Replacing FFmpeg with a different encoder.
- Adding multi-camera or multi-IG topology.
- Migrating to UE5.8+.
- Touching scenario engine simulation internals.
- Adding new features.

If any of these surface as side effects, they go into a separate follow-up spec.

## Constraints

- **Single-developer flow.** Six sequential PRs on `main`, no long-lived branches. Each PR independently revertable.
- **Verification gate per phase:** all 29 UE5 Automation tests + `scripts/ci_validate.sh` smoke + `Phase27PerformanceTest` perf benchmark must pass.
- **Target platforms:** x86_64 Linux production, Apple Silicon dev workstations. Atomic ordering must be correct on both. ARM Linux production is not a current target but is plausible.
- **No regression in KLV / video output.** Smoke harness verifies MPEG-TS + MISB ST 0601 KLV byte-stream integrity.

## Design

### Phase 1 — P0 cross-thread correctness

**Goal:** remove every cross-thread race that today is masked by x86's strong memory model.

**Files touched:** `Camera/CamSimCamera.{h,cpp}`, `Subsystem/CamSimSubsystem.cpp`, `Diagnostics/PipelineLatencyTracker.{h,cpp}`.

**Changes:**

1. `RenderReadyStreak_` and `RenderDepthReadyStreak_` (currently plain `uint8`, touched by both game thread pre-enqueue zeroing and render thread incrementing) → `TAtomic<uint8>` with `EMemoryOrder::Relaxed`. They are compared only for equality/streak-counting; relaxed is sufficient. Add `// writer: render, init-only: game` comments.

2. `bReadbackDMAIssued`: change render-thread store from `EMemoryOrder::Relaxed` → `EMemoryOrder::SequentiallyConsistent`. Change game-thread load → `EMemoryOrder::SequentiallyConsistent`. The SeqCst pair establishes happens-before from the render-side `AsyncPixels_` / `AsyncDepth_` writes to the game-thread reads. The existing `bPollComplete_` SeqCst path remains the primary signal; this hardens the secondary gate. (Note: this flag is deleted in Phase 3 as part of the state-machine collapse, but the correct ordering rule transfers to the new `ReadbackState_` atomic. Fixing it here ensures correctness if Phase 3 slips.)

3. `bReadbackResultReady_`, `CompletedPixels_`, `CompletedDepth_`, `CompletedTelemetry_`, `CompletedFrameIndex_`: annotate explicitly as `// game-thread-only — not atomic`. Add `checkSlow(IsInGameThread())` in `DispatchQueuedResultIfFree` and any other accessor.

4. **Audit `FPipelineLatencyTracker`.** Inspect `Mark()` (called from encoder thread + sensor task thread) and `ComputePercentiles()` (called from game thread in the health JSON writer). If the underlying storage is a plain ring buffer, add either: (a) `FRWLock` with read-lock around `ComputePercentiles` and write-lock around `Mark` — but `Mark` is hot-path so this is acceptable only if the lock is uncontended (single writer at a time); or (b) per-stage `TAtomic<int64>` slots with a snapshot copy under SeqCst. Pick whichever measures faster against `Phase27PerformanceTest`.

**Memory ordering convention.** UE5's `TAtomic<T>::Load/Store` take an `EMemoryOrder` enum that has exactly two values: `Relaxed` and `SequentiallyConsistent` (see `Engine/Source/Runtime/Core/Public/Templates/Atomic.h:31-40`). C++-standard `acquire`/`release` semantics are not directly expressible. The project convention (documented at `CamSimCamera.h:191-193`) is to use SeqCst on both ends of a producer/consumer pair whenever the pair gates data — SeqCst is a strict superset of acq/rel, so correctness is preserved at a small fence cost that is negligible at our 30 fps target. All Phase 1 "Release/Acquire" tightening in this spec is implemented as SeqCst in code.

**Verification:** Full perf + smoke + automation suite. New test `FPipelineLatencyTrackerConcurrencyTest`: spawn N writer threads doing `Mark()` while one reader does `ComputePercentiles()`, run for 2 seconds, assert no asserts trip and percentile results stay within expected bounds. On platforms where ThreadSanitizer is available, run the test under TSan and require zero races.

### Phase 2 — P1 per-frame allocation pass

**Goal:** eliminate per-frame heap allocations on the sensor → encoder hot path.

**Files touched:** `Sensor/SensorPostProcess.{h,cpp}`, `Encoder/MultiViewFrameSink.{h,cpp}`, `Metadata/KlvBuilder.{h,cpp}`, `CIGI/CigiReceiver.cpp`.

**Changes:**

1. `FSensorPostProcess` gains three reusable scratch buffers as members:
   - `TArray<FColor> ScratchFrameA_` — sized in `Initialize(Width, Height)`.
   - `TArray<FColor> ScratchFrameB_` — sized in `Initialize`.
   - `TArray<FColor> BlurTemp_` — sized in `Initialize`.
   `ApplyBoxBlur` and `ApplyGaussianBlur` use `BlurTemp_`. `ApplyLensDistortion`, `ApplyVibration`, `ApplyRollingShutter` use `ScratchFrameA_`/`ScratchFrameB_` (whichever is free at the call site — they don't run concurrently). The expected saving: ~5× 8.3 MB heap allocations per frame eliminated = ~1.2 GB/s allocator pressure removed at 30 fps.

2. AGC histogram pass (`SensorPostProcess.cpp:1786-1820`): wrap in `ParallelFor(kParallelBands, [&](int32 Band) { … local 256-bin histogram … })`, then a serial reduce. Per-band histograms are 1 KB each, well within L1.

3. `FKlvBuilder` gains a `TArray<uint8> ValueScratch_` member; `BuildMisbST0601Into` calls `ValueScratch_.Reset(/*AllowShrink=*/false)` instead of declaring a local `TArray<uint8> Value`. Capacity amortizes after first frame.

4. `FViewRuntime` (inside `MultiViewFrameSink`) gains `TArray<FColor> ZoomedPixels_` member. `ApplyDigitalZoom` writes into it instead of allocating. The inner zoom loop wraps in `ParallelFor` — each output pixel reads from a distinct source location (embarrassingly parallel).

5. `MultiViewFrameSink::WriteGroundTruthLine` replaces `FFileHelper::SaveStringToFile` with a persistent `IFileHandle*` opened in `Open()` and closed in `Close()`. Writes via `IFileHandle::Write(ANSICHAR*, int32)`. Removes 30 open/close cycles per second from the encode thread.

6. `CigiReceiver::Run` recording path: replace per-packet `RecordFileHandle->Flush()` with periodic flush (every 100 packets) and an unconditional flush in `Stop()`. The recording feature is diagnostic — losing ≤100 ms on crash is acceptable.

**Verification:** Before any code change in this phase, run `Phase27PerformanceTest` against the current `main` and record per-frame `FMemory::Stats` allocation count in the PR description as baseline. After the changes, extend `Phase27PerformanceTest` with an inline allocation-count assertion that fails if the count exceeds 50% of the recorded baseline. Smoke harness must produce a byte-identical KLV stream.

### Phase 3 — P1 `CamSimCamera` decomposition

**Goal:** shrink `CamSimCamera.cpp` from 1629 lines to ≤900 lines by extracting cohesive helpers, and collapse the readback flag-cluster into a single state machine.

**Files touched:** `Camera/CamSimCamera.{h,cpp}` (main work), new `Camera/GpuReadbackPipeline.{h,cpp}`, new `Geospatial/CesiumTuning.{h,cpp}`, `Subsystem/CamSimSubsystem.{h,cpp}`, new `Encoder/ProcessedFrame.h`.

**Changes:**

1. **Readback state machine.** Replace four flags (`bReadbackPending`, `bReadbackDMAIssued`, `bPollComplete_`, `bPollFailed_`) with:
   ```cpp
   enum class EReadbackState : uint8 { Idle, DMAQueued, Complete, Failed };
   TAtomic<EReadbackState> ReadbackState_{ EReadbackState::Idle };
   ```
   Transitions: game thread `Idle → DMAQueued` (Release) at `CaptureAndEncode`; render thread `DMAQueued → Complete` or `→ Failed` (Release) after DMA + poll; game thread `Complete → Idle` (Acquire) after dispatch. Impossible combinations are eliminated by construction.

2. **`SubmitFrameToEncoder` signature.** `FProcessedFrame` (currently in `EncoderThread.h`) is moved to its own header `Encoder/ProcessedFrame.h`. `SubmitFrameToEncoder(TArray<FColor>, FCamSimTelemetry, uint64, TArray<float>)` becomes `SubmitFrameToEncoder(FProcessedFrame&&)`. Callers construct the struct once.

3. **`GpuReadbackPipeline` extraction.** New plain C++ class `FGpuReadbackPipeline` (not a UObject). Owns:
   - `ColorReadbackPool` (array of `TUniquePtr<FRHIGPUTextureReadback>`)
   - `DepthReadbackPool`
   - `AsyncPixels_`, `AsyncDepth_`
   - `ReadbackState_` (the new atomic)
   - `PollGeneration_`
   - `RenderReadyStreak_`, `RenderDepthReadyStreak_`

   Public API:
   ```cpp
   void EnqueueCapture(FTextureRHIRef Color, FTextureRHIRef Depth, uint32 Generation);
   bool TryConsume(TArray<FColor>& OutPixels, TArray<float>& OutDepth);
   void Reset();
   ```
   `ACamSimCamera` owns one instance; `Tick()` calls `TryConsume` and dispatches if true.

4. **`CesiumTuning` extraction.** Move `ApplyCesiumTilesetTuning` and `ComputeCulledScreenSpaceError` from `ACamSimCamera` (static functions today) into `Geospatial/CesiumTuning.{h,cpp}`. Public functions `ApplyCesiumTilesetTuning(UWorld*, const FCamSimConfig&)` and `ComputeCulledScreenSpaceError(double HFovDeg)`. The static-on-actor signal is the give-away that these don't belong on the actor.

5. **Tileset iterator cache.** `UCamSimSubsystem` gains `TArray<TWeakObjectPtr<ACesium3DTileset>> CachedTilesets_` populated in `BeginPlay` (one `TActorIterator` sweep at init). The adaptive-SSE + tile-prefetch loops in `CamSimCamera::Tick()` iterate this cached list instead of running `TActorIterator` per tick. Merge adaptive-SSE and prefetch-boost into a single resolve+apply loop.

6. **Hot-reload move.** Delete `PollHotReloadConfig` from `ACamSimCamera`. `UCamSimSubsystem::Tick` runs the polling on its existing periodic schedule. The `IFileManager::GetTimeStamp` call is posted to `AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, …)`; the task atomically sets `bHotReloadPending_` when the mtime changes; the next game-thread tick checks the flag and runs the existing `UCamSimSubsystem::HotReloadConfig`.

7. **Dead-code purge.** Confirm via grep that `PrevGimbalPanDeg_` and `PrevGimbalTiltDeg_` (CamSimCamera.h:297-298) are never written, then delete them. Remove duplicate `#include "EngineUtils.h"` at CamSimCamera.cpp:24/46.

**Verification:** Perf test must not regress (this is structural; perf is the bystander). Smoke harness output must be byte-identical pre/post. New unit test `FGpuReadbackPipelineStateTest`: exercises every legal state transition + verifies illegal transitions trigger `ensureMsgf`, no UE world required.

### Phase 4 — P1 protocol + config cleanup

**Goal:** remove the last asymmetries between protocol adapters and replace stringly-typed config with enums.

**Files touched:** `DIS/DisEntityAdapter.{h,cpp}`, `Entity/CamSimEntityManager.{h,cpp}`, `Config/CamSimConfig.{h,cpp}`, `Encoder/VideoEncoder.{h,cpp}`, `Sensor/SensorPostProcess.cpp`.

**Changes:**

1. **DIS SPSC migration.** Replace `FDisEntityAdapter::PendingEntityStates` / `PendingRateControls` (`TArray` ping-pong, heap-allocating) with `TBoundedSpscQueue<FCigiEntityState>` and `TBoundedSpscQueue<FCigiRateCtrl>`. Move `FDisEntityAdapter::Tick()` invocation out of `FCamSimEntityManager::ProcessEntityStates` (where it currently lives as a side effect of the drain loop) and to the top of `FCamSimEntityManager::Tick()` alongside the CIGI drain. The adapter's tick now has no entity-manager dependency.

2. **`EEncoderPreference` enum.** Define `enum class EEncoderPreference : uint8 { Auto, Nvenc, LibX264 }` in `Config/CamSimConfig.h`. Add `ParseEncoderPreference(const FString&)` mirroring `ParseReadbackFormat` / `ParseWatchdogPolicy`. Replace `Cfg.Encoder` (raw `FString`) with `Cfg.EncoderPref` (`EEncoderPreference`). Every comparison site in `VideoEncoder.cpp` updated to use the enum.

3. **`ESensorMode` cleanup.** Replace `Telemetry.SensorMode == 1 || Telemetry.SensorMode == 2` (and similar magic-int checks) with `static_cast<ESensorMode>(Telemetry.SensorMode) == ESensorMode::IR || … == ESensorMode::NVG`. `FCamSimTelemetry::SensorMode` stays `uint8` for KLV/wire compatibility; the comparison site is what changes.

4. **`ForEachPixelBand` helper.** New file-static lambda or free function in `SensorPostProcess.cpp`:
   ```cpp
   template<typename FnT>
   static void ForEachPixelBand(int32 Height, FnT&& Fn) {
       ParallelFor(kParallelBands, [&](int32 Band) {
           const int32 RowsPerBand = (Height + kParallelBands - 1) / kParallelBands;
           const int32 RowStart = Band * RowsPerBand;
           const int32 RowEnd   = FMath::Min(RowStart + RowsPerBand, Height);
           Fn(RowStart, RowEnd);
       }, EParallelForFlags::BackgroundPriority);
   }
   ```
   The 12+ duplicated band-header blocks (`ApplyIR`, `ApplyNVG`, `ApplyIRExtinction`, `ApplyNoise`, `ApplyFixedPatternNoise`, `ApplyVignetting`, `ApplyScanLines`, `ApplyAtmosphericAttenuation`, `ApplyColorTemperature`, `ApplyContrastBrightness`, `ApplyBoxBlur` ×2) rewrite to call `ForEachPixelBand`.

5. **`ConfigureKlvStream` helper.** Extract the duplicate KLV stream configuration block in `VideoEncoder.cpp` (lines 414-417 and 447-452) into a `static void ConfigureKlvStream(AVStream*)` and call it from `OpenKlvStream()` and `OpenRecordingContext()`.

**Verification:** Smoke + perf. The DIS SPSC migration is the highest-risk change in this phase; the smoke harness must continue to route DIS Designator PDUs to the camera laser-spot projection. New test `FDisAdapterSpscRoundtripTest`: producer thread pushes synthetic PDUs, consumer thread drains, assert exact pass-through of every field with no allocations after warmup.

### Phase 5 — P2 include hygiene

**Goal:** stop public headers from leaking implementation-detail dependencies into every TU.

**Files touched:** `Encoder/VideoEncoder.h`, `Sensor/SensorPostProcess.h`, `Metadata/KlvBuilder.h`, new `Metadata/CamSimTelemetry.h`, `Config/CamSimConfig.h`, new `Config/ScenarioConfig.h`, new `Config/Phase18Config.h`, plus every downstream consumer.

**Changes:**

1. **`FVideoEncoder` Pimpl.** Move every FFmpeg type from `VideoEncoder.h`'s private section into a `struct FVideoEncoderImpl` defined inside `VideoEncoder.cpp`. The header forward-declares only what the public API exposes. `FVideoEncoder` holds a `TUniquePtr<FVideoEncoderImpl> Impl_` with a forward-declared deleter (mirroring the existing pattern from `FEncoderThreadDeleter` in `CamSimCamera.h`). Result: including `VideoEncoder.h` no longer pulls in `libavcodec/avcodec.h` etc.

2. **`SensorPostProcess.h` slim-down.** Forward-declare `class FHudOverlay` instead of including `Overlay/FHudOverlay.h`. Extract the specific `FPhase18Config` sub-struct from `CamSimConfig.h` into `Config/Phase18Config.h`; `SensorPostProcess.h` includes only that.

3. **`FCamSimTelemetry` extraction.** Move the `FCamSimTelemetry` struct from `Metadata/KlvBuilder.h` into a new `Metadata/CamSimTelemetry.h`. Both `KlvBuilder.h` and `Overlay/FHudOverlay.h` include the new header. The overlay module no longer transitively depends on KLV encoding.

4. **`ScenarioConfig.h` extraction.** Move the scenario-specific nested structs from `CamSimConfig.h` into a new `Scenario/ScenarioConfig.h`: `FScenarioEntityConfig`, `FScenarioTrigger`, `FScenarioCondition`, `FScenarioAction`, `FDamageTransitionConfig`, `FActivityScheduleEntry`, `FWaypointConfig`. `CamSimConfig.h` includes the new header. TUs that don't touch the scenario engine no longer have these definitions in their preprocessed output.

**Verification:** Clean build + full perf + smoke + automation suite. Runtime should be byte-identical — this phase is pure build-time hygiene.

### Phase 6 — P2 module reorganization + comment debt sweep

**Goal:** reorganize the 22 flat module directories into a layout with clear top-level grouping, and remove every comment that narrates completed work.

**Files touched:** ~60+ files across all of `Source/CamSimTest/`. Updates to `CLAUDE.md` and `docs/architecture.md`.

**Changes:**

1. **`Protocols/` parent.**
   - `CIGI/` → `Protocols/CIGI/`
   - `DIS/` → `Protocols/DIS/`
   - `Streaming/CotSender.{h,cpp}` → `Protocols/CoT/CotSender.{h,cpp}`. Delete the now-empty `Streaming/` directory.

2. **`Observability/` merge.**
   - `Diagnostics/PipelineLatencyTracker.{h,cpp}` → `Observability/PipelineLatencyTracker.{h,cpp}`.
   - `Health/CamSimHealthServer.{h,cpp}` → `Observability/CamSimHealthServer.{h,cpp}`.
   - `Logging/CamSimJsonLogger.{h,cpp}` → `Observability/CamSimJsonLogger.{h,cpp}`.
   - Delete the three old directories.

3. **`CesiumBackend.h` split from `CamSimGeospatialProvider.h`.** Move `ApplyCesiumBackendConfig` (a Cesium-concrete free function) from the provider-neutral `Geospatial/CamSimGeospatialProvider.h` into `Geospatial/CesiumBackend.h`. The provider façade stays Cesium-agnostic at the header level.

4. **`IFrameSink` decision.** Add `virtual void Close() = 0;` to the interface and route `FSubsystemImpl::~FSubsystemImpl` through the interface. Preserves the test-double seam at zero runtime cost. Resolves item #32 from the review.

5. **Include-path rewrite.** Every `#include "CIGI/…"` becomes `#include "Protocols/CIGI/…"`; same for DIS, CoT, the three observability modules, and the Cesium backend split. A single mechanical search-replace pass; the build is the test.

6. **Comment debt sweep.** Regex-delete `// Phase NN[A-Z]?` banner comments that narrate completed work — most are in `CamSimCamera.{h,cpp}`, `SensorPostProcess.cpp`, `VideoEncoder.cpp`, `KlvBuilder.cpp`, and `MultiViewFrameSink.cpp`. Keep single-line guards that explain WHY a branch exists (rare — these get re-reviewed line by line). The 30-line `FEncoderThreadDeleter` explanation in `CamSimCamera.h:12-19` stays — it explains a non-obvious WHY (UHT codegen constraint).

7. **Documentation update.**
   - `CLAUDE.md` "Architecture" file-tree section updated to reflect new module layout.
   - `docs/architecture.md` "Key Source Files" table updated with new paths.
   - Any other path-referencing docs sync'd.

**Verification:** Clean build + full perf + smoke + all 29 automation tests. This is the highest-churn phase; any missed include-path update surfaces as a build failure, not a runtime bug.

## Data Flow

No data-flow changes across any phase. The CIGI Receiver → Game Thread → Render Thread → Task Thread pipeline is preserved. Phase 3's state-machine collapse changes how readback state is *represented* but not *what flows through it*. Phase 4's DIS SPSC migration changes how DIS PDUs are *queued* but not *what the consumer sees*.

## Error Handling

Each phase has one failure mode: the verification gate does not pass. Recovery is `git revert <phase-commit>` and a re-plan. Because phases are sequential on `main`, this is a clean, contained operation.

Within a phase, error handling follows existing patterns:
- New SPSC queues in Phase 4 use the same overflow-drop semantics as existing CIGI queues.
- Phase 3's readback state machine adds `ensureMsgf` on illegal transitions but never silently masks them.
- Phase 1's `FPipelineLatencyTracker` locking falls back to the previous value on contention rather than blocking the encoder thread.

## Testing

The verification gate is identical for every phase:

1. **UE5 Automation suite** — all 29 in-editor tests pass.
2. **`scripts/ci_validate.sh`** — Docker headless run, ffprobe video stream verification, KLV byte-stream validation.
3. **`Phase27PerformanceTest`** — perf benchmark, no regression beyond noise (~5%).

New tests added per phase:

| Phase | New test | Purpose |
|-------|----------|---------|
| 1 | `FPipelineLatencyTrackerConcurrencyTest` | Multi-threaded `Mark` + `ComputePercentiles`, optional TSan run |
| 2 | `Phase27PerformanceTest` allocation assertion | Asserts allocation count drops ≥50% vs baseline captured at start of phase |
| 3 | `FGpuReadbackPipelineStateTest` | State-machine transitions in isolation, no UE world |
| 4 | `FDisAdapterSpscRoundtripTest` | Producer/consumer round-trip with allocation-free assertion after warmup |
| 5 | (none) | Build correctness IS the test |
| 6 | (none) | Build correctness IS the test |

## Risk Analysis

| Risk | Phase | Likelihood | Impact | Mitigation |
|------|-------|------------|--------|------------|
| `FPipelineLatencyTracker` race fix tanks p99 frame time | 1 | Low | Med | Benchmark both lock and atomic-snapshot approaches; pick faster |
| Scratch-buffer reuse aliases between Apply* calls if invoked concurrently | 2 | Low | High | Document in comments that scratch buffers are caller-serial; `ensureMsgf` on re-entry |
| Readback state machine misses a legal transition observed in production but not in test | 3 | Med | High | Add `ensureMsgf` on every transition, run smoke for 10 minutes before merging |
| DIS SPSC queue size insufficient under burst | 4 | Low | Med | Size to existing `TArray` peak +2× from production logs; queue full counts to health metrics |
| Phase 5 Pimpl deleter misconfigured, causes link error | 5 | Low | Low | Mirror existing `FEncoderThreadDeleter` pattern exactly |
| Phase 6 mass include-path rewrite misses one file | 6 | Med | Low | Full build catches it; revert single commit if so |

The single highest-impact failure mode is a Phase 3 state-machine bug that masks a stale readback as fresh, producing visual artifacts that pass byte-comparison KLV tests but corrupt video. Smoke-harness ffprobe checks frame count and decode integrity, which catches this.

## Out of Scope

Explicitly excluded from this refactor:

- CCL parser rewrite or replacement.
- FFmpeg replacement (NVENC AV1, x265, etc.).
- Multi-camera or multi-IG topology.
- UE5.8+ migration.
- Scenario engine simulation changes.
- New features of any kind.

Any of these that emerge as side effects get their own design spec.

## Rollout

Six PRs to `main`, sequential, no overlap. Each PR's commit message follows the existing convention:

```
refactor(camsim): <phase title> — <one-line summary>

<bullet list of changes>

<verification statement>
```

After Phase 6 merges, this design doc is marked `Status: Implemented`. The full 35-finding review that produced this spec is preserved in this document's `Problem` section and in the per-phase `Changes` lists; no separate archive is needed.

## Implementation Planning

Because the six phases each have substantial, distinct scope, this design spec is the umbrella. Each phase will get its own `writing-plans` output (`docs/superpowers/plans/2026-05-NN-camsim-refactor-phase-N-plan.md`) when its turn comes — Phase 1's plan is produced immediately after this spec is approved; subsequent phases' plans are produced when the previous phase merges and verification passes.
