# CamSim Refactor Phase 3 — P1 `CamSimCamera` Decomposition — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Shrink `CamSimCamera.cpp` (1629 lines) by extracting cohesive helpers, collapse the 4-flag readback state cluster into a single atomic enum, cache the tileset list, and move hot-reload polling off the actor.

**Architecture:** Seven sequential commits, each scoped narrowly enough to revert independently. The biggest commit extracts `FGpuReadbackPipeline` — a plain C++ class that owns ping-pong readback pools, the state machine, the streak counters, and the async pixel/depth buffers. `ACamSimCamera` becomes the orchestrator that delegates GPU readback to that class.

**Tech Stack:** UE5.7 (`TAtomic`, `ENQUEUE_RENDER_COMMAND`, `AsyncTask`, `TWeakObjectPtr`), C++17.

---

## Context

**Design spec:** `docs/superpowers/specs/2026-05-11-camsim-refactor-design.md` — Phase 3 section.

**Verification gate:** All UE5 Automation tests pass + `scripts/ci_validate.sh` smoke harness passes + `Phase27PerformanceTest` shows no regression.

**Two design-spec items deliberately deferred** (and worth flagging):
- The spec mentioned a `SubmitFrameToEncoder(FProcessedFrame&&)` signature collapse. The existing `FProcessedFrame` in `Encoder/EncoderThread.h` is the encoder-queue payload and does not carry depth; the submit function additionally needs `TArray<float> DepthMetres` which the encoder doesn't consume. Adapting that signature to use a struct that carries depth-not-needed-by-encoder is a YAGNI hazard. **Defer.**
- The spec listed "remove duplicate `#include "EngineUtils.h"`" as a Phase 3 item — actually rolled into Task 1's dead-code purge.

---

## File Structure

| File | Action | Responsibility |
|------|--------|---|
| `Camera/CamSimCamera.h` | Modify | Drop dead `PrevGimbalPan/Tilt` fields, replace 4 readback flags with `TAtomic<EReadbackState>`, replace embedded readback pool members with `TUniquePtr<FGpuReadbackPipeline>` |
| `Camera/CamSimCamera.cpp` | Modify | Strip duplicate include; rewrite readback-flag sites to state-enum transitions; delegate readback pipeline to new class; remove hot-reload polling block; remove static `ApplyCesiumTilesetTuning` / `ComputeCulledScreenSpaceError` definitions (callers now use `Geospatial/CesiumTuning.h`) |
| `Camera/GpuReadbackPipeline.h` | Create | New plain C++ class declaration |
| `Camera/GpuReadbackPipeline.cpp` | Create | DMA enqueue, render-thread poll command, state-machine transitions, hand-off accessors |
| `Geospatial/CesiumTuning.h` | Create | Free-function header for `ApplyCesiumTilesetTuning` and `ComputeCulledScreenSpaceError` |
| `Geospatial/CesiumTuning.cpp` | Create | Definitions moved from `CamSimCamera.cpp` |
| `Subsystem/CamSimSubsystem.h` | Modify | Add `CachedTilesets_` weak-ptr array, add `bHotReloadPending_` atomic, add `LastConfigMTime_` |
| `Subsystem/CamSimSubsystem.cpp` | Modify | Populate `CachedTilesets_` in `BeginPlay`; poll hot-reload off-thread in `Tick` |

---

## Task 1 — Dead-code purge + duplicate include

**Goal:** Remove `PrevGimbalPanDeg_` / `PrevGimbalTiltDeg_` if confirmed dead, and the duplicate `#include "EngineUtils.h"`. This is a setup commit with zero behavior change.

**Files:** `Camera/CamSimCamera.{h,cpp}`

- [ ] **Step 1: Verify the streak fields are dead.**

```bash
grep -n 'PrevGimbalPanDeg_\|PrevGimbalTiltDeg_' unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.{h,cpp}
```

Expected: only the declarations (in `.h`) and any zero-initializer line. NO assignments or reads. If anything else prints, stop and report — the fields are used somewhere and cannot be deleted.

- [ ] **Step 2: Delete the declarations.**

In `Camera/CamSimCamera.h`, find the block (around line 297-298):

```cpp
	// Phase 27E — Tile prefetch during gimbal slew
	float PrevGimbalPanDeg_   = 0.0f;
	float PrevGimbalTiltDeg_  = 0.0f;
	int32 TilePrefetchBoostFramesRemaining_ = 0;
```

Delete the two `PrevGimbal*` lines (keep `TilePrefetchBoostFramesRemaining_` — it's used).

- [ ] **Step 3: Find and remove duplicate `EngineUtils.h` include.**

```bash
grep -n '#include "EngineUtils.h"' unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp
```

Expected: 2 matches. Delete the second one (later line number).

- [ ] **Step 4: Commit.**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.h \
        unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp
git commit -m "$(cat <<'EOF'
refactor(camsim): purge dead streak fields and duplicate include

PrevGimbalPanDeg_ and PrevGimbalTiltDeg_ are declared but never written
anywhere in the codebase — the tile-prefetch slew detection at Phase 27E
reads gimbal velocity through GimbalComp directly, so the standalone
fields are unused.

Also remove the duplicate "EngineUtils.h" include that crept in during
an earlier refactor.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2 — Collapse readback flags into `TAtomic<EReadbackState>`

**Goal:** Replace four cross-thread flags (`bReadbackPending`, `bReadbackDMAIssued`, `bPollComplete_`, `bPollFailed_`) with a single `enum class EReadbackState { Idle, DMAQueued, Complete, Failed }` held in `TAtomic<EReadbackState>`. Eliminates impossible state combinations by construction.

**Files:** `Camera/CamSimCamera.{h,cpp}`

- [ ] **Step 1: Add the enum to the header.**

In `Camera/CamSimCamera.h`, add this BEFORE `class ACamSimCamera`:

```cpp
/**
 * Readback state machine. Replaces the previous four-flag cluster
 * (bReadbackPending, bReadbackDMAIssued, bPollComplete_, bPollFailed_).
 * Transitions:
 *   Idle      → DMAQueued  (game thread, before ENQUEUE_RENDER_COMMAND)
 *   DMAQueued → Complete   (render thread, after pixels copied)
 *   DMAQueued → Failed     (render thread, on Lock/Unlock error)
 *   Complete  → Idle       (game thread, after dispatch)
 *   Failed    → Idle       (game thread, after logging)
 *
 * Ordering: SeqCst on every Store/Load — the state is paired with data
 * writes (AsyncPixels_/AsyncDepth_) on the DMAQueued → Complete edge.
 */
enum class EReadbackState : uint8 { Idle, DMAQueued, Complete, Failed };
```

- [ ] **Step 2: Replace the four `TAtomic<bool>` fields with one `TAtomic<EReadbackState>`.**

In `Camera/CamSimCamera.h`, find and **delete** these four fields (note: `bReadbackPending` is a plain `bool` today, but it's still part of the cluster):

```cpp
	bool bReadbackPending = false;
	TAtomic<bool> bReadbackDMAIssued{false};
	TAtomic<bool>  bPollComplete_{false};
	TAtomic<bool>  bPollFailed_{false};
```

(Surrounding comments referencing those fields should also be removed.)

Replace with:

```cpp
	/** Cross-thread readback state machine — see EReadbackState above.
	 *  writer: game (Idle↔DMAQueued, Complete/Failed→Idle) + render (DMAQueued→Complete/Failed)
	 *  readers: both threads (SeqCst). */
	TAtomic<EReadbackState> ReadbackState_ { EReadbackState::Idle };
```

- [ ] **Step 3: Rewrite each access site in `Camera/CamSimCamera.cpp`.**

Map old flag operations to new state transitions:

| Old | New |
|---|---|
| `bReadbackPending = true` | `ReadbackState_.Store(EReadbackState::DMAQueued, SeqCst)` |
| `bReadbackPending = false` | `ReadbackState_.Store(EReadbackState::Idle, SeqCst)` |
| `if (!bReadbackPending) return` | `if (ReadbackState_.Load(SeqCst) == EReadbackState::Idle) return` |
| `bReadbackDMAIssued.Store(true, …)` | (folded into DMAQueued — leave the existing SeqCst store here, see note below) |
| `bReadbackDMAIssued.Load(...)` | (the secondary gate is no longer needed — the state machine itself replaces it) |
| `bPollComplete_.Store(true, …)` | `ReadbackState_.Store(EReadbackState::Complete, SeqCst)` |
| `bPollComplete_.Load(...)` | `ReadbackState_.Load(SeqCst) == EReadbackState::Complete` |
| `bPollFailed_.Store(true, …)` | `ReadbackState_.Store(EReadbackState::Failed, SeqCst)` |
| `bPollFailed_.Load(...)` | `ReadbackState_.Load(SeqCst) == EReadbackState::Failed` |

**Note on `bReadbackDMAIssued`:** This flag was a secondary gate to prevent polling before the render command had executed `EnqueueCopy`. With the state machine, the render command transitions from `DMAQueued` to `Complete`/`Failed` itself; the game thread polls by reading the state. The secondary gate becomes redundant — drop it.

**Concrete sites to rewrite** (line numbers approximate, locate by content):

1. **`PollReadbackCompletion`** (`CamSimCamera.cpp` ~line 717): the entry guard `if (!bReadbackPending || !bReadbackDMAIssued.Load(SeqCst)) return;` becomes:
   ```cpp
   const EReadbackState State = ReadbackState_.Load(EMemoryOrder::SequentiallyConsistent);
   if (State == EReadbackState::Idle || State == EReadbackState::DMAQueued) {
       // DMAQueued: poll continues below to check for completion
       if (State == EReadbackState::Idle) return;
   }
   ```

   Actually simpler: the function should only proceed when state is `Complete` or `Failed` (consume the result) OR `DMAQueued` (enqueue another poll). Rewrite the function entry to dispatch on state.

2. **`CaptureAndEncode`** (`CamSimCamera.cpp` ~line 1471–1478): the reset block becomes:
   ```cpp
   ReadbackState_.Store(EReadbackState::DMAQueued, EMemoryOrder::SequentiallyConsistent);
   PollGeneration_.Store(PollGeneration_.Load(EMemoryOrder::Relaxed) + 1,
                          EMemoryOrder::SequentiallyConsistent);
   RenderReadyStreak_.Store(0, EMemoryOrder::Relaxed);
   RenderDepthReadyStreak_.Store(0, EMemoryOrder::Relaxed);
   ```

3. **Render command lambda** (`CamSimCamera.cpp` ~line 800–880): the idempotent guards become single-state checks:
   ```cpp
   // Stale poll from a previous frame — game thread has already advanced.
   if (PollGeneration_.Load(EMemoryOrder::Relaxed) != CaptureGen) return;
   if (ReadbackState_.Load(EMemoryOrder::SequentiallyConsistent) != EReadbackState::DMAQueued) return;
   ```
   And the success/failure stores become `ReadbackState_.Store(EReadbackState::Complete, SeqCst)` / `Failed`.

4. **Initial DMA-enqueue render command** (`CamSimCamera.cpp` ~line 1551): the `bReadbackDMAIssued.Store(true, …)` line is **removed** — the state has already been set to `DMAQueued` on the game thread before `ENQUEUE_RENDER_COMMAND`.

- [ ] **Step 4: Verify all old flag references are gone.**

```bash
grep -nE 'bReadbackPending|bReadbackDMAIssued|bPollComplete_|bPollFailed_' \
  unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.{h,cpp}
```

Expected: zero matches.

```bash
grep -n 'ReadbackState_' unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.{h,cpp}
```

Expected: declaration plus multiple Store/Load sites.

- [ ] **Step 5: Commit.**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.h \
        unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp
git commit -m "$(cat <<'EOF'
refactor(camsim): collapse readback flags into EReadbackState atomic

Replace four cross-thread flags (bReadbackPending, bReadbackDMAIssued,
bPollComplete_, bPollFailed_) with one TAtomic<EReadbackState> holding
Idle / DMAQueued / Complete / Failed. Eliminates impossible state
combinations (e.g., bPollComplete_ true while bPollFailed_ also true)
by construction.

The render-thread "DMA issued" gate becomes redundant: the state
machine itself records that the render command ran by transitioning
out of DMAQueued, so the game thread polls by reading the state
directly. SeqCst on every Store/Load — same ordering rule Phase 1
established for the original cluster.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3 — Extract `Geospatial/CesiumTuning`

**Goal:** Move `ApplyCesiumTilesetTuning` and `ComputeCulledScreenSpaceError` from `ACamSimCamera` (where they sit as `static` member functions) into `Geospatial/CesiumTuning.{h,cpp}` as free functions. The `static` modifier was the give-away that these don't belong on the actor.

**Files:** `Geospatial/CesiumTuning.h` (new), `Geospatial/CesiumTuning.cpp` (new), `Camera/CamSimCamera.h` (modify), `Camera/CamSimCamera.cpp` (modify)

- [ ] **Step 1: Create the new header.**

`unreal_project/CamSimTest/Source/CamSimTest/Geospatial/CesiumTuning.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UWorld;
struct FCamSimConfig;

namespace CamSim::Geospatial
{
	/**
	 * Apply Cesium tileset streaming parameters (SSE, cache, culling, physics)
	 * to every ACesium3DTileset in the world. Safe to call with a null World.
	 */
	void ApplyCesiumTilesetTuning(UWorld* World, const FCamSimConfig& Cfg);

	/**
	 * Pure derivation: scales the off-frustum culled-SSE with horizontal FoV so a
	 * narrow sensor gets more aggressive culling. Clamped at ≥100.
	 */
	double ComputeCulledScreenSpaceError(double HFovDeg);
}
```

- [ ] **Step 2: Create the new .cpp.**

Copy the body of `ACamSimCamera::ApplyCesiumTilesetTuning` and `ACamSimCamera::ComputeCulledScreenSpaceError` (currently in `Camera/CamSimCamera.cpp`) into `unreal_project/CamSimTest/Source/CamSimTest/Geospatial/CesiumTuning.cpp` as free functions in the `CamSim::Geospatial` namespace. Preserve the bodies byte-for-byte.

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Geospatial/CesiumTuning.h"

#include "Config/CamSimConfig.h"
#include "EngineUtils.h"
#include "Engine/World.h"
// Cesium includes — copy whichever ones the existing functions need
#include "Cesium3DTileset.h"

namespace CamSim::Geospatial
{
	void ApplyCesiumTilesetTuning(UWorld* World, const FCamSimConfig& Cfg)
	{
		// PASTE existing body verbatim from CamSimCamera.cpp.
	}

	double ComputeCulledScreenSpaceError(double HFovDeg)
	{
		// PASTE existing body verbatim from CamSimCamera.cpp.
	}
}
```

- [ ] **Step 3: Update `CamSimCamera.h` to remove the static declarations.**

Find the block declaring `ApplyCesiumTilesetTuning` and `ComputeCulledScreenSpaceError` as static methods of `ACamSimCamera` (around line 113-120) and **delete those declarations**.

- [ ] **Step 4: Update `CamSimCamera.cpp`.**

a) Add `#include "Geospatial/CesiumTuning.h"` near the top.
b) Delete the definitions of `ACamSimCamera::ApplyCesiumTilesetTuning` and `ACamSimCamera::ComputeCulledScreenSpaceError`.
c) Rewrite the callers inside `CamSimCamera.cpp` to use the namespaced free functions (`CamSim::Geospatial::ApplyCesiumTilesetTuning(...)`).

- [ ] **Step 5: Update existing test references.**

```bash
grep -n 'ACamSimCamera::ApplyCesiumTilesetTuning\|ACamSimCamera::ComputeCulledScreenSpaceError' unreal_project/CamSimTest/Source/CamSimTest/
```

Any matches in `Tests/` need to be rewritten to use `CamSim::Geospatial::...`. The most likely site is `Phase27PerformanceTest.cpp` which has `ACamSimCamera::ComputeCulledScreenSpaceError` calls.

- [ ] **Step 6: Verify and commit.**

```bash
grep -n 'ACamSimCamera::ApplyCesiumTilesetTuning\|ACamSimCamera::ComputeCulledScreenSpaceError' unreal_project/CamSimTest/
```

Expected: zero matches.

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Geospatial/CesiumTuning.{h,cpp} \
        unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.{h,cpp} \
        unreal_project/CamSimTest/Source/CamSimTest/Tests/Phase27PerformanceTest.cpp
git commit -m "$(cat <<'EOF'
refactor(camsim): extract CesiumTuning to Geospatial/

ApplyCesiumTilesetTuning and ComputeCulledScreenSpaceError were
static members of ACamSimCamera — a signal that they didn't belong on
the actor. Move them to Geospatial/CesiumTuning.{h,cpp} as free
functions in the CamSim::Geospatial namespace. No behavior change;
existing callers updated to use the namespaced spelling.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4 — Tileset iterator cache in subsystem

**Goal:** `ACamSimCamera::Tick()` currently runs `TActorIterator<ACesium3DTileset>` sweeps on every tick for adaptive-SSE and tile-prefetch. Cache the list once at `BeginPlay` so per-tick work iterates a pre-built array instead of walking the world.

**Files:** `Subsystem/CamSimSubsystem.{h,cpp}`, `Camera/CamSimCamera.cpp`

- [ ] **Step 1: Add the cache to the subsystem.**

In `Subsystem/CamSimSubsystem.h`, add to the private section:

```cpp
	// Phase 3: cached tileset pointer list. Populated in BeginPlay (once per
	// session) and reused by the adaptive-SSE / tile-prefetch loops in
	// ACamSimCamera::Tick. TWeakObjectPtr so a destroyed tileset is observed
	// as null on next access rather than dangling.
	TArray<TWeakObjectPtr<class ACesium3DTileset>> CachedTilesets_;
```

Add a public accessor:

```cpp
	/** Returns the cached tileset array (Phase 3). Stale entries (destroyed
	 *  tilesets) are filtered as TWeakObjectPtr::IsValid() returns false. */
	const TArray<TWeakObjectPtr<class ACesium3DTileset>>& GetCachedTilesets() const { return CachedTilesets_; }
```

- [ ] **Step 2: Populate the cache in subsystem `Initialize` or via a `RefreshTilesets()` helper.**

In `Subsystem/CamSimSubsystem.cpp`, where the subsystem first has access to a world (likely on a `OnWorldBeginPlay` hook or in the existing setup path), iterate `TActorIterator<ACesium3DTileset>` and store weak pointers into `CachedTilesets_`.

```cpp
void UCamSimSubsystem::RefreshCachedTilesets()
{
	CachedTilesets_.Reset();
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ACesium3DTileset> It(World); It; ++It)
		{
			CachedTilesets_.Add(*It);
		}
	}
	UE_LOG(LogCamSim, Log, TEXT("CamSimSubsystem: cached %d tileset actor(s)"), CachedTilesets_.Num());
}
```

Call it once when the world is ready (or lazily on first access).

- [ ] **Step 3: Update `ACamSimCamera::Tick` to use the cache.**

Find the existing `TActorIterator<ACesium3DTileset>` loops in `CamSimCamera.cpp`. Replace each with:

```cpp
if (Subsystem)
{
	for (const TWeakObjectPtr<ACesium3DTileset>& Weak : Subsystem->GetCachedTilesets())
	{
		ACesium3DTileset* T = Weak.Get();
		if (!T) continue;
		// ... existing per-tileset work ...
	}
}
```

- [ ] **Step 4: Verify and commit.**

```bash
grep -n 'TActorIterator<ACesium3DTileset>' unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp
```

Expected: zero matches in `Camera/CamSimCamera.cpp` (any remaining matches in `Geospatial/CesiumTuning.cpp` are acceptable — that function is called from subsystem code that runs once at hot-reload).

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.{h,cpp} \
        unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp
git commit -m "$(cat <<'EOF'
perf(camsim): cache tileset list in subsystem; drop per-tick TActorIterator

ACamSimCamera::Tick ran TActorIterator<ACesium3DTileset> sweeps every
frame for adaptive-SSE and tile-prefetch. At many actors this is an
O(N) scan on the game-thread hot path.

Cache the tilesets as TWeakObjectPtr<> in UCamSimSubsystem once per
session (via RefreshCachedTilesets() called when the world is ready);
Tick iterates the cached array instead. TWeakObjectPtr handles
destroyed-tileset cleanup naturally — stale entries return null.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5 — Move hot-reload polling to subsystem with off-thread stat

**Goal:** Hot-reload polling (`IFileManager::Get().GetTimeStamp(*CfgPath)`) currently runs on the game thread inside `ACamSimCamera::PollHotReloadConfig`. On slow storage this is a frame-budget hazard. Move the polling to `UCamSimSubsystem::Tick` with the actual `stat` syscall posted to `AsyncTask`.

**Files:** `Subsystem/CamSimSubsystem.{h,cpp}`, `Camera/CamSimCamera.{h,cpp}`

- [ ] **Step 1: Add fields to the subsystem.**

In `Subsystem/CamSimSubsystem.h`, private section:

```cpp
	// Phase 3: hot-reload poll state moved off ACamSimCamera. Game thread
	// reads bHotReloadPending_; an async task does the stat syscall and
	// sets the flag if the config file mtime changed.
	TAtomic<bool> bHotReloadPending_ { false };
	FDateTime     LastConfigMTime_   = FDateTime::MinValue();
	float         HotReloadAccumSec_ = 0.0f;
	TAtomic<bool> bHotReloadStatInFlight_ { false };  // prevents double-spawn
```

- [ ] **Step 2: Implement the polling logic in subsystem `Tick`.**

In `Subsystem/CamSimSubsystem.cpp` `Tick`:

```cpp
void UCamSimSubsystem::Tick(float DeltaTime)
{
	// ... existing tick logic ...

	const FCamSimConfig& Cfg = GetConfig();
	if (Cfg.Performance.bHotReloadConfig)
	{
		HotReloadAccumSec_ += DeltaTime;
		const float Interval = FMath::Max(0.5f, Cfg.Performance.HotReloadPollIntervalSec);
		if (HotReloadAccumSec_ >= Interval && !bHotReloadStatInFlight_.Load(EMemoryOrder::Relaxed))
		{
			HotReloadAccumSec_ = 0.0f;
			bHotReloadStatInFlight_.Store(true, EMemoryOrder::Relaxed);
			const FString CfgPath = GetConfigFilePath();
			AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this, CfgPath]()
			{
				const FDateTime Now = IFileManager::Get().GetTimeStamp(*CfgPath);
				if (Now != FDateTime::MinValue() && Now != LastConfigMTime_)
				{
					LastConfigMTime_ = Now;
					bHotReloadPending_.Store(true, EMemoryOrder::SequentiallyConsistent);
				}
				bHotReloadStatInFlight_.Store(false, EMemoryOrder::Relaxed);
			});
		}

		if (bHotReloadPending_.Exchange(false, EMemoryOrder::SequentiallyConsistent))
		{
			// Existing HotReloadConfig flow — load fresh config, apply diff.
			FCamSimConfig NewCfg = FCamSimConfig::Load();
			HotReloadConfig(NewCfg);
		}
	}
}
```

You will need a `GetConfigFilePath()` helper. If one doesn't exist, derive it from the same logic that's currently in `ACamSimCamera::PollHotReloadConfig` and add it as a private method of the subsystem.

- [ ] **Step 3: Remove hot-reload polling from `ACamSimCamera`.**

Delete:
- `ACamSimCamera::PollHotReloadConfig` definition and declaration.
- Fields `HotReloadAccumSec_` and `LastConfigMTime_` from `CamSimCamera.h`.
- The call site in `Tick()` that invokes `PollHotReloadConfig`.

- [ ] **Step 4: Verify and commit.**

```bash
grep -n 'PollHotReloadConfig\|HotReloadAccumSec_\|LastConfigMTime_' \
  unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.{h,cpp}
```

Expected: zero matches.

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.{h,cpp} \
        unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.{h,cpp}
git commit -m "$(cat <<'EOF'
refactor(camsim): move hot-reload polling to subsystem with off-thread stat

ACamSimCamera::PollHotReloadConfig ran IFileManager::GetTimeStamp on
the game thread every poll interval — a blocking stat syscall that on
NFS or container FUSE overlays can spike tens of milliseconds and miss
a frame.

Move the poll loop to UCamSimSubsystem::Tick. The actual stat call
runs on AsyncTask (any background thread); when the mtime changes the
task flips bHotReloadPending_, which the next subsystem tick observes
and applies via the existing HotReloadConfig path. Game thread sees
only an atomic-bool read in the steady state.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6 — Extract `FGpuReadbackPipeline`

**Goal:** The biggest commit of Phase 3. Move the ping-pong readback pools, the state machine (from Task 2), the streak counters, and the async pixel/depth buffers out of `ACamSimCamera` into a new plain C++ class. `ACamSimCamera` keeps the orchestration (capture trigger, dispatch on completion) but delegates the GPU readback mechanics.

**Files:** `Camera/GpuReadbackPipeline.h` (new), `Camera/GpuReadbackPipeline.cpp` (new), `Camera/CamSimCamera.{h,cpp}` (modify)

- [ ] **Step 1: Create the new header.**

`unreal_project/CamSimTest/Source/CamSimTest/Camera/GpuReadbackPipeline.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RHIGPUReadback.h"
#include "Engine/TextureRenderTarget2D.h"

enum class EReadbackState : uint8;  // forward-declared; full def in CamSimCamera.h
                                     // (or move the enum into this header — see Step 6.)

/**
 * FGpuReadbackPipeline
 *
 * Owns the GPU-side readback machinery for ACamSimCamera: ping-pong
 * FRHIGPUTextureReadback pools, the cross-thread state machine, streak
 * counters, and the staging arrays the render thread writes into.
 *
 * The owning actor calls EnqueueCapture(...) to kick off DMA; the render
 * thread itself transitions the state on completion; the actor polls
 * TryConsume(...) on the game thread to retrieve the result.
 *
 * Threading: state machine + streak counters + async buffers are touched
 * by both threads — see the field-level annotations.
 */
class FGpuReadbackPipeline
{
public:
	FGpuReadbackPipeline();
	~FGpuReadbackPipeline();

	/** One-time pool setup. Call after the camera's render targets exist. */
	void Initialize(int32 NumPoolSlots);

	/** Returns true if the pipeline is idle (no DMA in flight). */
	bool IsIdle() const;

	/**
	 * Game thread: snapshot which RT slots to copy from, bump generation,
	 * transition state to DMAQueued, enqueue the render command.
	 */
	void EnqueueCapture(UTextureRenderTarget2D* ColorRT, UTextureRenderTarget2D* DepthRT,
	                    int32 ColorPoolIdx, int32 DepthPoolIdx,
	                    int32 ReadyPollsRequired, int32 CaptureWidth, int32 CaptureHeight,
	                    EReadbackFormat Format, bool bSwapRB, uint64 FrameIdx);

	/**
	 * Game thread: if state is Complete, move pixels/depth out and reset to Idle.
	 * Returns true if a result was consumed.
	 */
	bool TryConsume(TArray<FColor>& OutPixels, TArray<float>& OutDepth);

	/** Game thread: forcibly return to Idle (used on EndPlay / cleanup). */
	void Reset();

private:
	TArray<TUniquePtr<FRHIGPUTextureReadback>> ColorPool_;
	TArray<TUniquePtr<FRHIGPUTextureReadback>> DepthPool_;
	TArray<FColor>  AsyncPixels_;
	TArray<float>   AsyncDepth_;
	TAtomic<EReadbackState> State_;
	TAtomic<uint32> Generation_ { 0 };
	TAtomic<uint8>  ColorStreak_ { 0 };
	TAtomic<uint8>  DepthStreak_ { 0 };
};
```

- [ ] **Step 2: Move the state-machine enum.**

If the enum was added to `CamSimCamera.h` in Task 2, **move** it (don't copy) to `GpuReadbackPipeline.h`. `CamSimCamera.h` should `#include "Camera/GpuReadbackPipeline.h"` afterwards.

- [ ] **Step 3: Create the .cpp.**

`unreal_project/CamSimTest/Source/CamSimTest/Camera/GpuReadbackPipeline.cpp` — port the contents of the existing DMA-enqueue and poll render-command lambdas from `CamSimCamera.cpp` verbatim. The lambdas capture `this` as the pipeline pointer (instead of the actor); accesses to `AsyncPixels_`, `RenderReadyStreak_`, `bPollComplete_`, etc. become accesses to the corresponding pipeline member.

This is the surgically delicate step — copy the lambdas precisely and adjust ONLY the receiver and the member names. Behaviour must be byte-identical.

- [ ] **Step 4: Update `ACamSimCamera`.**

a) Replace the member declarations for `ColorReadbackPool`, `DepthReadbackPool`, `AsyncPixels_`, `AsyncDepth_`, `ReadbackState_`, `PollGeneration_`, `RenderReadyStreak_`, `RenderDepthReadyStreak_` with a single `TUniquePtr<FGpuReadbackPipeline> Readback_;`.

b) Initialize the pipeline in `BeginPlay`.

c) Replace direct DMA-enqueue code with `Readback_->EnqueueCapture(...)`.

d) Replace `PollReadbackCompletion` with `Readback_->TryConsume(...)` + the dispatch-to-sensor-task code that runs after consumption.

- [ ] **Step 5: Verify and commit.**

```bash
grep -nE 'ColorReadbackPool|DepthReadbackPool|AsyncPixels_|AsyncDepth_' \
  unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.{h,cpp}
```

Expected: zero matches in `Camera/CamSimCamera.{h,cpp}` (all moved into the new class).

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Camera/GpuReadbackPipeline.{h,cpp} \
        unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.{h,cpp}
git commit -m "$(cat <<'EOF'
refactor(camsim): extract FGpuReadbackPipeline from ACamSimCamera

The actor owned eight readback-related fields (ColorPool, DepthPool,
AsyncPixels_, AsyncDepth_, ReadbackState_, PollGeneration_, color/depth
streak counters) and two render-command lambdas that together totalled
~250 lines of GPU readback machinery. Lift them into a plain C++ class
FGpuReadbackPipeline; ACamSimCamera keeps the orchestration (capture
trigger, dispatch on completion) but delegates the DMA mechanics.

The state machine, streak counters, and pixel/depth buffers all move
intact — no behaviour change; the render-command lambdas are textually
the same, just with member names rebound to the new owner.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7 — Final verification + open PR

**Goal:** Audit the diff, confirm CamSimCamera.cpp meaningfully shrunk, write the PR description.

- [ ] **Step 1: Confirm commits and file count.**

```bash
git log --oneline main..HEAD
```

Expected: 6 commits (Tasks 1–6).

```bash
git diff --stat main..HEAD
```

Expected: 8–10 files changed. `CamSimCamera.cpp` should show a meaningful reduction (target: ≤900 lines, down from 1629 — net −600+).

- [ ] **Step 2: Audit remaining responsibilities in `CamSimCamera.cpp`.**

Skim the file end-to-end. Confirm it no longer contains:
- Readback ping-pong pool management.
- DMA-enqueue or poll render command lambdas.
- Hot-reload `IFileManager::GetTimeStamp` calls.
- Cesium tileset tuning function bodies.
- `TActorIterator<ACesium3DTileset>` sweeps.

What it SHOULD still contain: capture trigger (`CaptureAndEncode`), telemetry assembly, `SubmitFrameToEncoder` async-task dispatch, FPS-mode handling, geometric LOS computation, gimbal/sensor MPC parameter updates, decimation logic, `Tick()` orchestration.

- [ ] **Step 3: Push and open PR.**

```bash
git push -u origin HEAD
gh pr create --title "refactor(camsim): Phase 3 — CamSimCamera decomposition" --body "$(cat <<'EOF'
## Summary
- Collapses four cross-thread readback flags (`bReadbackPending`, `bReadbackDMAIssued`, `bPollComplete_`, `bPollFailed_`) into a single `TAtomic<EReadbackState>` — eliminates impossible state combinations by construction.
- Extracts `FGpuReadbackPipeline` — a plain C++ class that owns the ping-pong readback pools, state machine, streak counters, and async pixel/depth buffers (~250 lines lifted out of `ACamSimCamera`).
- Extracts `CesiumTuning` (static functions `ApplyCesiumTilesetTuning` + `ComputeCulledScreenSpaceError`) to `Geospatial/CesiumTuning.{h,cpp}` as namespaced free functions.
- Caches the tileset list in `UCamSimSubsystem` once at world-ready; `ACamSimCamera::Tick` iterates the cached array instead of running `TActorIterator` per frame.
- Moves hot-reload polling from `ACamSimCamera::PollHotReloadConfig` to `UCamSimSubsystem::Tick` with the `stat` syscall posted to `AsyncTask` — no more blocking syscalls on the game thread.
- Deletes confirmed-dead `PrevGimbalPanDeg_` / `PrevGimbalTiltDeg_` fields and a duplicate `EngineUtils.h` include.

Phase 3 of the refactor described in `docs/superpowers/specs/2026-05-11-camsim-refactor-design.md`.

## Test plan
- [ ] All UE5 Automation tests pass.
- [ ] `scripts/ci_validate.sh` smoke harness passes (KLV byte-stream + MPEG-TS decode).
- [ ] `Phase27PerformanceTest` shows no regression beyond ~5% noise.
- [ ] `CamSimCamera.cpp` line count below 1000 (target).

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

---

## Notes

- **Task 6 is the highest-risk commit.** The render-command lambdas captured `this` (the actor) and accessed eight member fields directly. Moving them to a new owner means the captures change. Take this commit slowly; the smoke harness ffprobe check is what catches a regression.
- **Task 2 is the second-highest-risk commit.** Collapsing four flags into one enum touches many sites. Each transition must map to the same external observable behaviour as before. The "bReadbackDMAIssued is redundant" decision is load-bearing — if a regression surfaces, the right path is to add an internal sub-state, not to revive the four-flag cluster.
- **Phase 3 does not change the data flow.** CIGI Receiver → Game Thread → Render Thread → Task Thread is preserved. The structural changes are about who owns what, not what flows where.
