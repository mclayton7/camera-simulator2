# CamSim Refactor Phase 5 — P2 Include Hygiene — Implementation Plan

**Goal:** Stop public headers from leaking implementation-detail dependencies into every TU.

**Architecture:** Phase 5 is split into two PRs. **Phase 5A** (this plan) extracts `FCamSimTelemetry` to its own header — a safe, contained move that breaks the Overlay → KLV transitive dependency. **Phase 5B** (deferred) handles the three larger items (VideoEncoder Pimpl, SensorPostProcess.h slim-down, ScenarioConfig nested-type extraction) which all need UE build feedback.

---

## Task 1 — Extract `FCamSimTelemetry` to its own header

**Goal:** Move the `FCamSimTelemetry` struct from `Metadata/KlvBuilder.h` to a new `Metadata/CamSimTelemetry.h`. `KlvBuilder.h` re-includes the new header so all existing consumers compile unchanged. The win: a future caller that only needs telemetry (e.g. `Overlay/FHudOverlay`, `Camera/CamSimCamera`) can switch its include to the new path and stop pulling in the entire KLV builder API.

**Files:** `Metadata/CamSimTelemetry.h` (new), `Metadata/KlvBuilder.h` (modify).

- [ ] Create `Metadata/CamSimTelemetry.h` with the `FCamSimTelemetry` struct verbatim from `KlvBuilder.h`.
- [ ] In `KlvBuilder.h`, replace the struct definition with `#include "Metadata/CamSimTelemetry.h"`.
- [ ] Verify all existing consumers still compile by inspecting that `KlvBuilder.h` is the canonical include path (`grep -rln '#include "Metadata/KlvBuilder.h"' …` and confirming the count is unchanged).
- [ ] Commit: `refactor(camsim): extract FCamSimTelemetry to its own header`.

---

## Deferred to Phase 5B

1. **`FVideoEncoder` Pimpl.** Move every FFmpeg type from `VideoEncoder.h`'s private section into a `struct FVideoEncoderImpl` defined inside `VideoEncoder.cpp`. The header forward-declares only what the public API exposes. Requires careful Pimpl construction + custom deleter pattern (mirror `FEncoderThreadDeleter`).

2. **`SensorPostProcess.h` slim-down.** Forward-declare `class FHudOverlay` instead of including `Overlay/FHudOverlay.h`. Extract `FPhase18Config` sub-struct to its own header.

3. **`ScenarioConfig` extraction.** Move `FScenarioEntityConfig`, `FScenarioTrigger`, `FScenarioCondition`, `FScenarioAction`, etc. from nested-inside-`FCamSimConfig` into `Scenario/ScenarioConfig.h`. Requires re-qualifying every caller's reference (10+ files).

All three need UE build feedback before merging — Pimpl construction errors and qualified-name mismatches surface only at compile time.
