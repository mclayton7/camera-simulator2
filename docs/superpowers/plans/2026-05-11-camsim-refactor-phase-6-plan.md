# CamSim Refactor Phase 6 — P2 Module Reorganization + Comment Debt Sweep — Implementation Plan

**Status:** Deferred in entirety to Phase 6B. This document captures the planned scope; execution is held until a UE5 build environment is available.

## Why deferred

Every Phase 6 item below either touches ≥60 include paths (directory renames) or requires editorial judgment on individual comments that benefits from compile/run feedback to verify nothing load-bearing was deleted. Doing these inline without a build would silently break the project.

## Scope held for Phase 6B

### 6B.1 — `Protocols/` parent directory

- Move `unreal_project/CamSimTest/Source/CamSimTest/CIGI/*` → `Protocols/CIGI/*`.
- Move `unreal_project/CamSimTest/Source/CamSimTest/DIS/*` → `Protocols/DIS/*`.
- Move `unreal_project/CamSimTest/Source/CamSimTest/Streaming/CotSender.{h,cpp}` → `Protocols/CoT/CotSender.{h,cpp}`. Delete the now-empty `Streaming/` directory.
- Update every `#include "CIGI/…"` / `"DIS/…"` / `"Streaming/CotSender.h"` path across the project (~60 files).
- Verify the full build before merging.

### 6B.2 — `Observability/` merge

- Move `Diagnostics/PipelineLatencyTracker.{h,cpp}` → `Observability/PipelineLatencyTracker.{h,cpp}`.
- Move `Health/CamSimHealthServer.{h,cpp}` → `Observability/CamSimHealthServer.{h,cpp}`.
- Move `Logging/CamSimJsonLogger.{h,cpp}` → `Observability/CamSimJsonLogger.{h,cpp}`.
- Delete the three old directories.
- Update every include path.

### 6B.3 — `CesiumBackend.h` split

- Move `ApplyCesiumBackendConfig` (a Cesium-concrete free function) from the provider-neutral `Geospatial/CamSimGeospatialProvider.h` into a new `Geospatial/CesiumBackend.h`.
- The provider façade stays Cesium-agnostic at the header level.

### 6B.4 — `IFrameSink::Close()` routing

The interface already has `virtual void Close() = 0;` (added in an earlier phase). The remaining work: route `UCamSimSubsystem::FSubsystemImpl::~FSubsystemImpl` through the interface pointer instead of dereferencing the concrete `FMultiViewFrameSink` type.

Trivial change once Phase 4B/5B land and a build is verifying.

### 6B.5 — Comment debt sweep

Sweep `// Phase NN[A-Z]?` banner comments across the codebase. Editorial rules:

- **Delete** comments that purely narrate completed work (`// Phase 21D.2 — inject ATAK FMV output view`).
- **Keep + reword** comments that explain WHY a branch exists (`// Phase 26C: compute ground speed from position delta (Tag 8)`) — strip the Phase tag, keep the rationale.
- **Keep verbatim**: load-bearing comments like the `FEncoderThreadDeleter` block in `CamSimCamera.h:12-19` that explains a non-obvious WHY (UHT codegen constraint).

Roughly 100+ candidates; requires per-site judgment that benefits from compile/test feedback.

### 6B.6 — Documentation update

- `CLAUDE.md` "Architecture" file-tree section → reflect new `Protocols/` and `Observability/` layout.
- `docs/architecture.md` "Key Source Files" table → new paths.
- Any other path-referencing docs sync'd.

## Verification gate (when run)

All UE5 Automation tests pass + smoke harness passes + `Phase27PerformanceTest` no regression. Build correctness IS the test for this phase.

## Why this phase still matters

The directory renames don't change runtime behaviour, but they reduce reviewer cognitive load for everyone joining the project. The comment sweep removes diff-noise from every future PR that touches these files. Worth doing once a UE build is available.
