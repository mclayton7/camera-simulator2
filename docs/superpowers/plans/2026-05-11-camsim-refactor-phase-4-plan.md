# CamSim Refactor Phase 4 — P1 Protocol + Config Cleanup — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development or superpowers:executing-plans.

**Goal:** Remove the last asymmetries in protocol adapters and replace stringly-typed config + magic-int comparisons with proper enums + helpers.

**Architecture:** Five design-spec items split across two PRs:
- **This phase (Phase 4A):** three low-risk items — `EEncoderPreference` enum, `ESensorMode` magic-int cleanup, `ConfigureKlvStream` helper.
- **Deferred to Phase 4B:** DIS adapter `TArray` → SPSC migration (threading change, needs build feedback), `ForEachPixelBand` helper (27 mechanical sites, needs build feedback).

---

## Context

**Design spec:** `docs/superpowers/specs/2026-05-11-camsim-refactor-design.md` — Phase 4 section.

**Verification gate:** All UE5 Automation tests pass + smoke harness passes + `Phase27PerformanceTest` shows no regression.

---

## Task 1 — `EEncoderPreference` enum

**Goal:** Parse `Cfg.Encoder` (raw `FString`) into a typed `enum class EEncoderPreference { Auto, Nvenc, LibX264, LibX265 }` at config-load. Replace inline `EncoderPref == TEXT("nvenc")` comparisons with the enum.

**Files:** `Config/CamSimConfig.{h,cpp}`, `Encoder/VideoEncoder.cpp`.

- [ ] Add `enum class EEncoderPreference` near the top of `Config/CamSimConfig.h`.
- [ ] Add a `ParseEncoderPreference(const FString&)` helper that maps `"auto"|"nvenc"|"libx264"|"libx265"` to the enum (unknown → `Auto` with a log warning).
- [ ] Replace the `if (EncoderPref == ...)` cascade in `Encoder/VideoEncoder.cpp` lines 133–155 with a `switch (Cfg.EncoderPref)`. The raw `FString` field stays for backward compat with the YAML loader; `EncoderPref` is derived once at load.
- [ ] Update `FCamSimConfig::Validate` to use the enum.
- [ ] Commit: `refactor(camsim): parse Encoder string into EEncoderPreference enum`.

## Task 2 — `ESensorMode` magic-int cleanup

**Goal:** Replace `Telemetry.SensorMode == 1 || == 2` magic-int comparison at `VideoEncoder.cpp:500` with a `static_cast<ESensorMode>(...)` comparison.

**Files:** `Encoder/VideoEncoder.cpp`.

- [ ] Add `#include "Sensor/SensorTypes.h"` to `Encoder/VideoEncoder.cpp` if not already present.
- [ ] Replace the magic-int comparison with `(SM == ESensorMode::IR || SM == ESensorMode::NVG)` where `SM = static_cast<ESensorMode>(Telemetry.SensorMode)`.
- [ ] Commit: `refactor(camsim): use ESensorMode enum in VideoEncoder grayscale check`.

## Task 3 — `ConfigureKlvStream` helper

**Goal:** Extract the duplicate KLV stream configuration in `Encoder/VideoEncoder.cpp` (the 2 sites that set `codec_type`, `codec_id`, `codec_tag`, `time_base` on an `AVStream*`) into one `static void ConfigureKlvStream(AVStream*)` helper.

**Files:** `Encoder/VideoEncoder.cpp`.

- [ ] Locate the two duplicate blocks (around lines 414–417 and 447–452).
- [ ] Add a file-static helper near the top of the .cpp.
- [ ] Replace each duplicate block with a single helper call.
- [ ] Commit: `refactor(camsim): extract ConfigureKlvStream helper`.

## Task 4 — Final verification + open PR

- [ ] Confirm 4 commits on top of `main`.
- [ ] Push branch + open PR.

---

## Deferred to Phase 4B

The two remaining design-spec items that need UE5 build feedback to land safely:

1. **DIS adapter SPSC migration.** Replace `FDisEntityAdapter::PendingEntityStates` and `PendingRateControls` (currently `TArray` ping-pong, heap-allocating) with `TBoundedSpscQueue<>`. Move the adapter's `Tick()` out of the drain loop in `FCamSimEntityManager::ProcessEntityStates`. Adds an `FDisAdapterSpscRoundtripTest` test.

2. **`ForEachPixelBand` helper in `Sensor/SensorPostProcess.cpp`.** 27 `ParallelFor(kParallelBands, …)` blocks share an identical row-band header (RowsPerBand / RowStart / RowEnd). Extract a templated helper; rewrite all 27 sites.

Both need a real UE build before merging because:
- The DIS migration introduces a new SPSC queue with overflow-drop semantics — wrong queue size would silently lose PDUs.
- The 27 mechanical rewrites can't reliably be done without compile verification on each site.

Plan-3B should be authored when a UE build is available; the work isn't load-bearing for the design spec's safety claims (Phase 1 atomic correctness covered the cross-thread invariants).
