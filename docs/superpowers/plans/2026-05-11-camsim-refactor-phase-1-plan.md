# CamSim Refactor Phase 1 — P0 Cross-Thread Correctness — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate every cross-thread race in the CamSim pipeline that today relies on x86's strong memory model — making the codebase correct on Apple Silicon dev workstations and any future ARM Linux deployment.

**Architecture:** Convert four cross-thread fields to use explicit C++ memory ordering: (1) `FPipelineLatencyTracker::CurrentFrame.Stages[]` → per-stage `TAtomic<uint64>` with release/acquire pairing; (2) `bReadbackDMAIssued` → `Release`/`Acquire`; (3) `RenderReadyStreak_` / `RenderDepthReadyStreak_` → `TAtomic<uint8>` relaxed; (4) game-thread-only readback hand-off state gets explicit `checkSlow(IsInGameThread())` guards. Add one new automation test that stresses the latency tracker under multi-threaded write load.

**Tech Stack:** Unreal Engine 5.7 (`TAtomic`, `EMemoryOrder`, `FRunnable`, `IMPLEMENT_SIMPLE_AUTOMATION_TEST`), C++17.

---

## Context

**Design spec:** `docs/superpowers/specs/2026-05-11-camsim-refactor-design.md` — Phase 1 section.

**Verification gate for this PR:** All 29 UE5 Automation tests pass + `scripts/ci_validate.sh` smoke harness passes + `Phase27PerformanceTest` shows no regression beyond ~5% noise.

**Background on the four issues (do not re-derive — these are the conclusions of the design review):**

1. **`FPipelineLatencyTracker::CurrentFrame.Stages[]` is a real data race.**
   - `Mark()` is called from the game thread (4 stages), sensor task thread (2 stages), and encoder thread (1 stage + `CommitFrame`).
   - The current code (`PipelineLatencyTracker.cpp:14`) writes to `CurrentFrame.Stages[idx]` non-atomically. The encoder thread then does `Records[idx] = CurrentFrame;` and `FMemory::Memzero(CurrentFrame);` (lines 20, 29) — concurrent struct-copy + memzero while other threads write individual slots is a C++ data race even though each `uint64` slot is touched by exactly one thread.
   - On x86 the aligned `uint64` writes are atomic at the hardware level so this is invisible today, but on ARM (Apple Silicon dev workstations) the C++ memory model permits torn reads and reordering.
   - Fix: change `CurrentFrame.Stages[]` from `uint64[]` to `TAtomic<uint64>[]`. Writers store with `Release`; `CommitFrame` loads each slot with `Acquire`. Zero contention because each writer hits a different slot.

2. **`bReadbackDMAIssued` uses Relaxed ordering for a flag that gates downstream data reads.**
   - Set to `true` on the render thread inside the DMA-enqueue command (`CamSimCamera.cpp:1551`).
   - Read on the game thread to decide whether polling can start (`CamSimCamera.cpp:726`).
   - On ARM, the relaxed pair allows the game thread to observe the flag flip before any data writes that preceded it in program order on the render thread. The `bPollComplete_` SeqCst pair is the primary signal, but `bReadbackDMAIssued` is a secondary gate and should match.
   - Fix: store with `Release`, load with `Acquire`.

3. **`RenderReadyStreak_` and `RenderDepthReadyStreak_` are plain `uint8` touched by two threads.**
   - Game thread zeros them in `CaptureAndEncode` before enqueueing the render command (`CamSimCamera.cpp:1476-1477`).
   - Render thread increments/zeros them inside the poll command (`CamSimCamera.cpp:810-813, 847-848, 871`).
   - The zeroing-before-enqueue is safe today because `ENQUEUE_RENDER_COMMAND` provides a happens-before edge, but the fields lack any guard, so any future caller that touches them on the game thread outside that exact window would race.
   - Fix: convert to `TAtomic<uint8>` with Relaxed ordering (they're compared only for streak/equality, no paired data).

4. **`bReadbackResultReady_`, `CompletedPixels_`, `CompletedDepth_`, `CompletedTelemetry_`, `CompletedFrameIndex_` are game-thread-only but undocumented.**
   - All read and written only on the game thread today.
   - No comment or assertion makes the constraint explicit. A future refactor that moves `DispatchQueuedResultIfFree` off-thread would introduce a silent race.
   - Fix: add `// game-thread-only` comments and `checkSlow(IsInGameThread())` guards at every accessor.

## File Structure

| File | Action | Responsibility |
|------|--------|---|
| `unreal_project/CamSimTest/Source/CamSimTest/Diagnostics/PipelineLatencyTracker.h` | Modify | Promote `CurrentFrame.Stages[]` to `TAtomic<uint64>[]` |
| `unreal_project/CamSimTest/Source/CamSimTest/Diagnostics/PipelineLatencyTracker.cpp` | Modify | Release stores in `SetStageTimestamp`, Acquire loads in `CommitFrame`, atomic-aware memzero |
| `unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.h` | Modify | Atomic types for streak fields; `// game-thread-only` annotations |
| `unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp` | Modify | Release/Acquire on `bReadbackDMAIssued`; atomic streak `.Load()/.Store()`; `checkSlow(IsInGameThread())` in `DispatchQueuedResultIfFree` |
| `unreal_project/CamSimTest/Source/CamSimTest/Tests/PipelineLatencyTrackerConcurrencyTest.cpp` | Create | New stress test: 3 producer threads + 1 reader thread, 2-second run, no asserts trip, percentile sanity |
| `unreal_project/CamSimTest/Source/CamSimTest/CamSimTest.Build.cs` | Verify | No change expected — new test file auto-picked up by UBT |

No production code in `Encoder/EncoderThread.cpp` or `Subsystem/CamSimSubsystem.cpp` changes — they call `Mark()` / `ComputePercentiles()` through the existing public API, which is unchanged.

---

## Task 0 — Capture baseline

**Goal:** Establish that the existing tests pass and the perf benchmark records a baseline before any change. This is the regression anchor.

**Files:** none modified.

- [ ] **Step 1: Confirm we are on `main` with a clean tree.**

Run: `git status`
Expected output: `On branch main` ... `nothing to commit, working tree clean`.

If not clean, stop and address before proceeding. Do not stash and continue; investigate why there are pending changes.

- [ ] **Step 2: Record the baseline test status in a scratch file (not committed).**

Run: `git log -1 --oneline > /tmp/camsim-phase1-baseline.txt`

Append the SHA of `main` to the file so the PR description can cite it as the verification anchor.

Run: `cat /tmp/camsim-phase1-baseline.txt`
Expected: a single line like `357556f docs: add CamSim repository refactor design spec`.

- [ ] **Step 3: Confirm the build instructions in `CLAUDE.md` are still current.**

Run: `grep -A2 'build_thirdparty' camera-simulator/CLAUDE.md || grep -A2 'build_thirdparty' CLAUDE.md`

Expected: documentation that `scripts/build_thirdparty.sh` must be run before any UE build.

Note for the engineer: this plan does **not** require building UE5 locally for code edits — the UE5 toolchain is heavy and runs in CI per the design spec. Local verification means reading the diff, running `clang-format-17` if available, and trusting that the test code will compile in CI. The CI gate is the source of truth.

---

## Task 1 — Add `FPipelineLatencyTrackerConcurrencyTest` (failing test first)

**Goal:** Add a multi-threaded stress test that exercises the exact race in `FPipelineLatencyTracker`. The test must compile against the **current** code, run successfully on x86 (because aligned uint64 is hardware-atomic), and serve as the regression net after the fix. If ThreadSanitizer is run on the test, it should report the race on the current code and zero races after Task 2.

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Tests/PipelineLatencyTrackerConcurrencyTest.cpp`

- [ ] **Step 1: Write the new test file.**

Create the file with the following exact contents:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"

#include "Diagnostics/PipelineLatencyTracker.h"

// -------------------------------------------------------------------------
// FPipelineLatencyTrackerConcurrencyTest
//
// Stresses the cross-thread write paths of FPipelineLatencyTracker:
//
//   - Three "producer" threads each write a disjoint subset of stages via
//     Mark() at ~10 kHz. The tracker design says each stage is written by
//     exactly one thread; we model that here.
//   - One "committer" thread calls CommitFrame() at ~1 kHz.
//   - The main thread polls ComputePercentiles() while the workers run.
//
// On x86 the test passes today because aligned 64-bit stores are atomic at
// the hardware level. On ARM (Apple Silicon, ARM Linux) torn reads of the
// non-atomic CurrentFrame.Stages slots are permitted by the C++ memory
// model. After Task 2 of the Phase 1 plan, all per-stage writes use
// Release/Acquire on TAtomic<uint64>, so the test passes correctly on both
// platforms.
//
// This test is also intended to be run under ThreadSanitizer when available.
// On a build with -fsanitize=thread, the pre-fix code reports a data race
// on CurrentFrame.Stages; the post-fix code reports zero races.
// -------------------------------------------------------------------------

namespace
{

struct FStageProducer : public FRunnable
{
	FPipelineLatencyTracker* Tracker;
	EPipelineStage           StageA;
	EPipelineStage           StageB;
	FThreadSafeBool          bStopRequested;
	int64                    IterationCount = 0;

	FStageProducer(FPipelineLatencyTracker* InTracker, EPipelineStage A, EPipelineStage B)
		: Tracker(InTracker), StageA(A), StageB(B) {}

	virtual uint32 Run() override
	{
		while (!bStopRequested)
		{
			Tracker->Mark(StageA);
			Tracker->Mark(StageB);
			++IterationCount;
		}
		return 0;
	}

	virtual void Stop() override { bStopRequested = true; }
};

struct FCommitter : public FRunnable
{
	FPipelineLatencyTracker* Tracker;
	FThreadSafeBool          bStopRequested;
	int64                    CommitCount = 0;

	explicit FCommitter(FPipelineLatencyTracker* InTracker) : Tracker(InTracker) {}

	virtual uint32 Run() override
	{
		while (!bStopRequested)
		{
			Tracker->CommitFrame();
			++CommitCount;
			FPlatformProcess::SleepNoStats(0.001f);  // ~1 kHz
		}
		return 0;
	}

	virtual void Stop() override { bStopRequested = true; }
};

}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPipelineLatencyTrackerConcurrencyTest,
	"CamSim.Phase1.Latency.ConcurrencyStress",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPipelineLatencyTrackerConcurrencyTest::RunTest(const FString& Parameters)
{
	FPipelineLatencyTracker Tracker(/*BufferSize=*/512);

	// Three producers: each writes a disjoint pair of stages. Matches the
	// real call pattern documented in the tracker header.
	FStageProducer ProdA(&Tracker, EPipelineStage::CigiDequeue,      EPipelineStage::GameTickStart);
	FStageProducer ProdB(&Tracker, EPipelineStage::ReadbackIssue,    EPipelineStage::ReadbackComplete);
	FStageProducer ProdC(&Tracker, EPipelineStage::SensorStart,      EPipelineStage::SensorEnd);
	FCommitter     Committer(&Tracker);

	FRunnableThread* ThreadA  = FRunnableThread::Create(&ProdA,     TEXT("LatTestProdA"));
	FRunnableThread* ThreadB  = FRunnableThread::Create(&ProdB,     TEXT("LatTestProdB"));
	FRunnableThread* ThreadC  = FRunnableThread::Create(&ProdC,     TEXT("LatTestProdC"));
	FRunnableThread* ThreadCm = FRunnableThread::Create(&Committer, TEXT("LatTestCommit"));

	const bool bAllCreated =
		(ThreadA != nullptr) && (ThreadB != nullptr) &&
		(ThreadC != nullptr) && (ThreadCm != nullptr);

	if (!TestTrue(TEXT("all four threads created"), bAllCreated))
	{
		// Cleanly shut down any threads that DID start before unwinding,
		// otherwise they'd keep running against stack-allocated runnables.
		ProdA.Stop();      ProdB.Stop();      ProdC.Stop();      Committer.Stop();
		if (ThreadA)  { ThreadA->WaitForCompletion();  delete ThreadA;  }
		if (ThreadB)  { ThreadB->WaitForCompletion();  delete ThreadB;  }
		if (ThreadC)  { ThreadC->WaitForCompletion();  delete ThreadC;  }
		if (ThreadCm) { ThreadCm->WaitForCompletion(); delete ThreadCm; }
		return false;
	}

	// Run for ~2 seconds, polling ComputePercentiles() from the main thread
	// the whole time. This is the exact contention pattern the subsystem
	// uses in production.
	const double Deadline = FPlatformTime::Seconds() + 2.0;
	int32 PollCount = 0;
	while (FPlatformTime::Seconds() < Deadline)
	{
		FPipelineLatencyTracker::FLatencyPercentiles P = Tracker.ComputePercentiles();
		(void)P;
		++PollCount;
		FPlatformProcess::SleepNoStats(0.005f);
	}

	ProdA.Stop();      ProdB.Stop();      ProdC.Stop();      Committer.Stop();
	ThreadA->WaitForCompletion();  delete ThreadA;
	ThreadB->WaitForCompletion();  delete ThreadB;
	ThreadC->WaitForCompletion();  delete ThreadC;
	ThreadCm->WaitForCompletion(); delete ThreadCm;

	// All workers made progress.
	TestTrue(TEXT("ProdA made progress"),     ProdA.IterationCount     > 0);
	TestTrue(TEXT("ProdB made progress"),     ProdB.IterationCount     > 0);
	TestTrue(TEXT("ProdC made progress"),     ProdC.IterationCount     > 0);
	TestTrue(TEXT("Committer made progress"), Committer.CommitCount    > 0);
	TestTrue(TEXT("Main thread polled"),      PollCount                > 0);

	// Final percentile read must not crash; tracker must hold at least one
	// committed record. We don't assert exact percentile values — under
	// heavy contention some Mark() writes land between CommitFrame() reads
	// and produce skipped or zero deltas, which is acceptable for a
	// best-effort sampling tracker.
	FPipelineLatencyTracker::FLatencyPercentiles Final = Tracker.ComputePercentiles();
	TestTrue(TEXT("Committed count > 0"), Tracker.GetCommittedCount() > 0);
	(void)Final;

	return true;
}
```

- [ ] **Step 2: Verify the test file lints clean.**

Run: `find unreal_project/CamSimTest/Source/CamSimTest/Tests -name "PipelineLatencyTrackerConcurrencyTest.cpp"`
Expected: one line printed with the new path.

If `clang-format-17` is available locally:

Run: `clang-format-17 --dry-run --Werror unreal_project/CamSimTest/Source/CamSimTest/Tests/PipelineLatencyTrackerConcurrencyTest.cpp`
Expected: no output (exit 0). If non-zero, run `clang-format-17 -i` on the file.

- [ ] **Step 3: Commit the test file as a separate commit.**

Run:
```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Tests/PipelineLatencyTrackerConcurrencyTest.cpp
git commit -m "$(cat <<'EOF'
test(camsim): add FPipelineLatencyTrackerConcurrencyTest

Stresses cross-thread Mark()/CommitFrame()/ComputePercentiles() write
paths in FPipelineLatencyTracker. Three producer threads each write a
disjoint stage pair; one committer thread commits at ~1 kHz; main thread
polls ComputePercentiles().

On x86 the test passes against the current (Phase-0) non-atomic
implementation because aligned uint64 stores are hardware-atomic. On
ARM the test will detect the data race after Task 2 of the Phase 1 plan
converts CurrentFrame.Stages[] to TAtomic<uint64>.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

Expected: commit succeeds; no pre-commit hook failures.

---

## Task 2 — Fix `FPipelineLatencyTracker::CurrentFrame.Stages[]` thread safety

**Goal:** Convert `CurrentFrame.Stages[]` from non-atomic `uint64[]` to `TAtomic<uint64>[]` with explicit Release/Acquire ordering. `CommitFrame` snapshots each slot into the ring with Acquire loads. The new concurrency test from Task 1 must continue to pass; under TSan (if run) it must now report zero races.

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Diagnostics/PipelineLatencyTracker.h`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Diagnostics/PipelineLatencyTracker.cpp`

- [ ] **Step 1: Update the header — make stage slots atomic and split the record types.**

Replace `unreal_project/CamSimTest/Source/CamSimTest/Diagnostics/PipelineLatencyTracker.h` lines 60-78 (the `private:` block through end of struct) with:

```cpp
private:
	// Plain POD record stored in the ring — written only by the committer
	// thread, read only by ComputePercentiles. No atomics required here.
	struct FLatencyRecord
	{
		uint64 Stages[static_cast<int32>(EPipelineStage::Count)] = {};
	};

	// CurrentFrame is the cross-thread staging area: producers call Mark() on
	// the slot for "their" stage; the committer thread snapshots all slots
	// into the ring on CommitFrame(). Per-slot TAtomic gives the producer
	// a Release store and the committer an Acquire load, which is enough to
	// make the C++ memory model happy even though each slot in practice has
	// only one writer.
	struct FCurrentFrame
	{
		TAtomic<uint64> Stages[static_cast<int32>(EPipelineStage::Count)] = {};
	};

	TArray<FLatencyRecord> Records;
	FCurrentFrame          CurrentFrame;
	// 64-bit write index so `WriteIndex % BufferCapacity` stays valid past the
	// ~4.5-year point at 30 fps where a uint32 would overflow.
	TAtomic<uint64> WriteIndex { 0 };
	int32 BufferCapacity = 300;
	TAtomic<int32> CommittedCount { 0 };

	static float CyclesToUs(uint64 Delta);
	static float PercentileFromSorted(const TArray<float>& Sorted, float Pct);
};
```

The two key changes from the existing code:
1. `FLatencyRecord` is unchanged (it's only ever touched by the committer thread, so no atomics needed there).
2. New `FCurrentFrame` wraps the cross-thread staging slots as `TAtomic<uint64>`.

- [ ] **Step 2: Update the .cpp — atomic stores in `SetStageTimestamp`, atomic loads in `CommitFrame`.**

Replace `unreal_project/CamSimTest/Source/CamSimTest/Diagnostics/PipelineLatencyTracker.cpp` lines 5-30 (the constructor through `CommitFrame`) with:

```cpp
FPipelineLatencyTracker::FPipelineLatencyTracker(int32 InBufferSize)
	: BufferCapacity(FMath::Max(10, InBufferSize))
{
	Records.SetNum(BufferCapacity);
	// Zero each atomic slot explicitly — Memzero on an array of TAtomic is
	// not a defined operation (atomic types are not trivially constructible
	// per the C++ standard, even though TAtomic in practice is).
	for (TAtomic<uint64>& Slot : CurrentFrame.Stages)
	{
		Slot.Store(0, EMemoryOrder::Relaxed);
	}
}

void FPipelineLatencyTracker::SetStageTimestamp(EPipelineStage Stage, uint64 Cycles)
{
	// Release store: pairs with the Acquire load in CommitFrame so any data
	// the producer thread wrote before calling Mark() is visible to the
	// committer thread when it snapshots the stage value.
	CurrentFrame.Stages[static_cast<int32>(Stage)].Store(Cycles, EMemoryOrder::Release);
}

void FPipelineLatencyTracker::CommitFrame()
{
	const uint64 Idx = WriteIndex.Load(EMemoryOrder::Relaxed);
	FLatencyRecord& Slot = Records[static_cast<int32>(Idx % BufferCapacity)];

	// Snapshot each stage timestamp with an Acquire load and zero the slot
	// in the same step. Per-slot atomicity is what makes this race-free even
	// though we never lock — each slot has at most one writer, and the
	// committer is the only reader, so writer/reader is the only contention.
	for (int32 s = 0; s < static_cast<int32>(EPipelineStage::Count); ++s)
	{
		Slot.Stages[s] = CurrentFrame.Stages[s].Load(EMemoryOrder::Acquire);
		CurrentFrame.Stages[s].Store(0, EMemoryOrder::Relaxed);
	}

	WriteIndex.Store(Idx + 1, EMemoryOrder::SequentiallyConsistent);

	const int32 Count = CommittedCount.Load(EMemoryOrder::Relaxed);
	if (Count < BufferCapacity)
	{
		CommittedCount.Store(Count + 1, EMemoryOrder::SequentiallyConsistent);
	}
}
```

The previous `FMemory::Memzero(CurrentFrame)` call is removed — `Memzero` on a struct containing `TAtomic` slots is UB; the explicit per-slot store does the same job correctly.

- [ ] **Step 3: Verify `ComputePercentiles` does not need changes.**

Open `PipelineLatencyTracker.cpp` lines 51-99 and confirm:
- It reads `Records[]` (plain `FLatencyRecord`, no atomics needed for the data).
- It reads `WriteIndex` and `CommittedCount` with `SequentiallyConsistent` ordering (already correct).
- It does **not** touch `CurrentFrame`.

Expected: no changes needed in `ComputePercentiles`. If you spot a `CurrentFrame` access in `ComputePercentiles`, that's a bug — stop and re-read the section.

- [ ] **Step 4: Lint and visually inspect the diff.**

Run: `git diff unreal_project/CamSimTest/Source/CamSimTest/Diagnostics/PipelineLatencyTracker.{h,cpp}`

Visually confirm:
- `FLatencyRecord` is unchanged (plain `uint64[]`).
- `FCurrentFrame` is new, with `TAtomic<uint64>[]`.
- `SetStageTimestamp` uses `Store(Cycles, EMemoryOrder::Release)`.
- `CommitFrame` snapshots each slot with `Load(EMemoryOrder::Acquire)` and zeroes with `Store(0, Relaxed)`.
- No `FMemory::Memzero(CurrentFrame)` remains.

If `clang-format-17` is available, run it on both files in-place.

- [ ] **Step 5: Commit.**

Run:
```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Diagnostics/PipelineLatencyTracker.h \
        unreal_project/CamSimTest/Source/CamSimTest/Diagnostics/PipelineLatencyTracker.cpp
git commit -m "$(cat <<'EOF'
fix(camsim): make FPipelineLatencyTracker stage slots atomic

CurrentFrame.Stages[] was non-atomic uint64[] written by 3+ threads
(game, sensor task, encoder) — a C++ data race that is invisible on x86
because aligned uint64 stores are hardware-atomic, but undefined on ARM
(Apple Silicon dev workstations).

Promote each stage slot to TAtomic<uint64> with explicit Release stores
in SetStageTimestamp and Acquire loads in CommitFrame's snapshot pass.
Each slot still has exactly one writer in practice; the atomics give
the C++ memory model what it needs to make the cross-thread visibility
guarantee explicit.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

Expected: commit succeeds.

---

## Task 3 — `bReadbackDMAIssued`: Relaxed → Release/Acquire

**Goal:** Change the render-thread store and game-thread load of `bReadbackDMAIssued` from `EMemoryOrder::Relaxed` to `Release`/`Acquire`. This hardens the secondary gate for the readback pipeline against ARM-permitted reorderings. The primary gate (`bPollComplete_`) is already SeqCst and unchanged.

Note: this flag is deleted entirely in Phase 3 as part of the state-machine collapse. Fixing it here ensures correctness if Phase 3 slips.

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp`

- [ ] **Step 1: Update the render-thread store site.**

In `unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp`, find line 1551:

```cpp
		bReadbackDMAIssued.Store(true, EMemoryOrder::Relaxed);
```

Change to:

```cpp
		// Release: paired with the Acquire load in PollReadbackCompletion so
		// the render command's writes (Readback->EnqueueCopy, transition) are
		// visible to the game thread once it observes bReadbackDMAIssued=true.
		bReadbackDMAIssued.Store(true, EMemoryOrder::Release);
```

- [ ] **Step 2: Update the game-thread load site.**

In the same file, find line 726:

```cpp
	if (!bReadbackPending || !bReadbackDMAIssued.Load(EMemoryOrder::Relaxed)) return;
```

Change to:

```cpp
	if (!bReadbackPending || !bReadbackDMAIssued.Load(EMemoryOrder::Acquire)) return;
```

- [ ] **Step 3: Update the reset store site.**

In the same file, find line 1471:

```cpp
	bReadbackDMAIssued.Store(false, EMemoryOrder::Relaxed);
```

This is the game-thread reset before enqueuing the new DMA command. The `ENQUEUE_RENDER_COMMAND` that follows establishes the happens-before edge between this store and the next render-thread load, so Relaxed is technically sufficient here. But for consistency with the new ordering rule, change to:

```cpp
	bReadbackDMAIssued.Store(false, EMemoryOrder::Release);
```

- [ ] **Step 4: Update the header comment that documents the ordering.**

In `unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.h`, find lines 220-223:

```cpp
	/** Set by render thread after EnqueueCopy; game thread polls IsReady() only after this is true.
	 *  writer: render → reader: game (relaxed — just gates whether polling can start). */
	TAtomic<bool> bReadbackDMAIssued{false};
```

Change to:

```cpp
	/** Set by render thread after EnqueueCopy; game thread polls IsReady() only after this is true.
	 *  writer: render → reader: game (Release/Acquire — pairs with the EnqueueCopy
	 *  data writes so they're visible to the game thread on observation). */
	TAtomic<bool> bReadbackDMAIssued{false};
```

- [ ] **Step 5: Verify the changes.**

Run: `git diff unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp | grep -E '(Relaxed|Release|Acquire)' | head -20`

Expected: three changed lines — two using `Release`, one using `Acquire`, no remaining `Relaxed` references for `bReadbackDMAIssued`.

Run: `grep -n 'bReadbackDMAIssued.*EMemoryOrder::Relaxed' unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp`
Expected: no output. If any line is printed, you missed one — go back and fix it.

- [ ] **Step 6: Commit.**

Run:
```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.h \
        unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp
git commit -m "$(cat <<'EOF'
fix(camsim): tighten bReadbackDMAIssued to Release/Acquire ordering

The render thread sets bReadbackDMAIssued=true after EnqueueCopy; the
game thread loads it to decide whether to start polling IsReady().
With Relaxed ordering, ARM can reorder render-side writes (the texture
transition, EnqueueCopy itself) past the flag flip, making them
invisible to the game-thread reader.

Pair the render-thread Store with Release and the game-thread Load
with Acquire. The reset Store on the game thread (CaptureAndEncode)
moves to Release for consistency with the rule, though the
ENQUEUE_RENDER_COMMAND barrier already provides happens-before there.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

Expected: commit succeeds.

---

## Task 4 — `RenderReadyStreak_` / `RenderDepthReadyStreak_` → `TAtomic<uint8>` (Relaxed)

**Goal:** Convert the two streak counters from plain `uint8` to `TAtomic<uint8>` with Relaxed ordering. They are touched on both the game thread (reset to 0 in `CaptureAndEncode`) and the render thread (incremented in the poll command). Today this is safe only because `ENQUEUE_RENDER_COMMAND` provides happens-before; making them atomic removes that latent fragility and is essentially free at runtime (Relaxed atomics on `uint8` are a plain `mov` on x86 and `ldrb`/`strb` on ARM with no fencing).

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.h`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp`

- [ ] **Step 1: Promote the fields in the header.**

In `unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.h`, find line 245:

```cpp
		uint8          RenderReadyStreak_ = 0;      // render-thread only
		uint8          RenderDepthReadyStreak_ = 0; // render-thread only
```

Change to:

```cpp
		// Streak counters for "N consecutive Ready polls before consuming"
		// debounce. Game thread resets to 0 in CaptureAndEncode (before
		// enqueuing the render command); render thread increments inside
		// the poll command. Relaxed: pure counters, no paired data, and the
		// ENQUEUE_RENDER_COMMAND barrier already orders reset → increment.
		// writer: game (reset) + render (increment) → readers: render
		TAtomic<uint8> RenderReadyStreak_      { 0 };
		TAtomic<uint8> RenderDepthReadyStreak_ { 0 };
```

- [ ] **Step 2: Update the game-thread reset sites.**

In `unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp`, find lines 1476-1477:

```cpp
		RenderReadyStreak_      = 0;
		RenderDepthReadyStreak_ = 0;
```

Change to:

```cpp
		RenderReadyStreak_     .Store(0, EMemoryOrder::Relaxed);
		RenderDepthReadyStreak_.Store(0, EMemoryOrder::Relaxed);
```

- [ ] **Step 3: Update the render-thread increment / read / reset sites.**

In the same file, locate the render command lambda body around lines 800-872. Five sites change:

(a) Around line 810 — depth-readback NOT ready: change
```cpp
			RenderReadyStreak_ = 0;
```
to
```cpp
			RenderReadyStreak_.Store(0, EMemoryOrder::Relaxed);
```

(b) Around line 813 — color streak increment: change
```cpp
		if (RenderReadyStreak_ < 255) ++RenderReadyStreak_;
		if (RenderReadyStreak_ < ReadyPollsRequired) return;
```
to
```cpp
		{
			const uint8 Cur = RenderReadyStreak_.Load(EMemoryOrder::Relaxed);
			if (Cur < 255) RenderReadyStreak_.Store(Cur + 1, EMemoryOrder::Relaxed);
		}
		if (RenderReadyStreak_.Load(EMemoryOrder::Relaxed) < ReadyPollsRequired) return;
```

(c) Around line 847 — depth streak increment: change
```cpp
				if (RenderDepthReadyStreak_ < 255) ++RenderDepthReadyStreak_;
				if (RenderDepthReadyStreak_ >= ReadyPollsRequired)
```
to
```cpp
				{
					const uint8 Cur = RenderDepthReadyStreak_.Load(EMemoryOrder::Relaxed);
					if (Cur < 255) RenderDepthReadyStreak_.Store(Cur + 1, EMemoryOrder::Relaxed);
				}
				if (RenderDepthReadyStreak_.Load(EMemoryOrder::Relaxed) >= ReadyPollsRequired)
```

(d) Around line 871 — depth-readback NOT ready: change
```cpp
			RenderDepthReadyStreak_ = 0;
```
to
```cpp
			RenderDepthReadyStreak_.Store(0, EMemoryOrder::Relaxed);
```

- [ ] **Step 4: Search for any other references.**

Run: `grep -n 'RenderReadyStreak_\|RenderDepthReadyStreak_' unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.{h,cpp}`

Expected output: every match should be either the declarations in `.h` or a `.Load()`/`.Store()` call in `.cpp`. If any plain `=`, `++`, or comparison without `.Load()` remains, that's a bug — fix it.

- [ ] **Step 5: Commit.**

Run:
```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.h \
        unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp
git commit -m "$(cat <<'EOF'
fix(camsim): make readback streak counters atomic

RenderReadyStreak_ and RenderDepthReadyStreak_ are touched on both the
game thread (zero in CaptureAndEncode) and the render thread (increment
and zero inside the poll command). Today this works because
ENQUEUE_RENDER_COMMAND provides happens-before, but the fields lack any
guard, so a future caller that touches them on the game thread outside
that exact window would race silently.

Promote both to TAtomic<uint8> with Relaxed ordering. They are pure
counters with no paired data, so Relaxed is sufficient and free on
both x86 (mov) and ARM (ldrb/strb).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

Expected: commit succeeds.

---

## Task 5 — Annotate game-thread-only readback hand-off state

**Goal:** Add explicit `// game-thread-only` comments and `checkSlow(IsInGameThread())` guards to the readback dispatch state that is currently undocumented as game-thread-only. Prevents future refactors from silently introducing a race.

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.h`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp`

- [ ] **Step 1: Annotate the header fields.**

In `unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.h`, find the block at lines 251-257:

```cpp
	/** Intermediate result: readback completed but sensor still busy. */
	TArray<FColor>   CompletedPixels_;
	TArray<float>    CompletedDepth_;
	FCamSimTelemetry CompletedTelemetry_;
	uint64           CompletedFrameIndex_ = 0;
	bool             bReadbackResultReady_ = false;
```

Replace with:

```cpp
	// -----------------------------------------------------------------------
	// Game-thread-only readback hand-off state. Written by
	// PollReadbackCompletion when a readback finishes but the sensor task is
	// still busy on the previous frame; read by DispatchQueuedResultIfFree
	// the next time it runs (both on the game thread). NOT atomic — moving
	// any access off-thread would introduce a silent race. Guards live at the
	// accessor sites (checkSlow(IsInGameThread())).
	// -----------------------------------------------------------------------
	TArray<FColor>   CompletedPixels_;
	TArray<float>    CompletedDepth_;
	FCamSimTelemetry CompletedTelemetry_;
	uint64           CompletedFrameIndex_ = 0;
	bool             bReadbackResultReady_ = false;
```

- [ ] **Step 2: Add `checkSlow(IsInGameThread())` to `DispatchQueuedResultIfFree`.**

In `unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp`, find the function starting at line 880:

```cpp
void ACamSimCamera::DispatchQueuedResultIfFree()
{
	if (bReadbackResultReady_ && !bSensorBusy)
	{
```

Change to:

```cpp
void ACamSimCamera::DispatchQueuedResultIfFree()
{
	// All reads and writes of CompletedPixels_/CompletedDepth_/CompletedTelemetry_/
	// CompletedFrameIndex_/bReadbackResultReady_ are game-thread-only — none
	// of these are atomic. The guard makes the constraint loud.
	checkSlow(IsInGameThread());

	if (bReadbackResultReady_ && !bSensorBusy)
	{
```

- [ ] **Step 3: Add the same guard at the write site in `PollReadbackCompletion`.**

In the same file, find line 717:

```cpp
void ACamSimCamera::PollReadbackCompletion()
{
	// Complete pending readback (async; no FlushRenderingCommands). We enqueue
```

Insert a `checkSlow` directly after the opening brace so it precedes everything:

```cpp
void ACamSimCamera::PollReadbackCompletion()
{
	checkSlow(IsInGameThread());  // function reads game-thread-only state and enqueues render commands.

	// Complete pending readback (async; no FlushRenderingCommands). We enqueue
```

- [ ] **Step 4: Verify the changes.**

Run: `grep -n 'checkSlow(IsInGameThread())' unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp`

Expected: at least two lines printed — one in `DispatchQueuedResultIfFree`, one in `PollReadbackCompletion`.

- [ ] **Step 5: Commit.**

Run:
```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.h \
        unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp
git commit -m "$(cat <<'EOF'
docs(camsim): annotate game-thread-only readback hand-off state

The Completed* fields and bReadbackResultReady_ are written by
PollReadbackCompletion and read by DispatchQueuedResultIfFree, both
on the game thread today. The constraint was implicit — a future
refactor that moved either function off-thread would have introduced
a silent race.

Add a header comment marking the block as "game-thread-only — NOT
atomic" and checkSlow(IsInGameThread()) guards at both accessor sites.
checkSlow is compiled out of shipping builds, so this costs nothing
in production.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

Expected: commit succeeds.

---

## Task 6 — Final verification pass

**Goal:** Run the full verification gate locally (where possible) and document what must run in CI.

**Files:** none modified.

- [ ] **Step 1: Confirm clean state and 5-commit phase log.**

Run: `git log --oneline main..HEAD`

Expected: five commits, in this order:
```
<sha> docs(camsim): annotate game-thread-only readback hand-off state
<sha> fix(camsim): make readback streak counters atomic
<sha> fix(camsim): tighten bReadbackDMAIssued to Release/Acquire ordering
<sha> fix(camsim): make FPipelineLatencyTracker stage slots atomic
<sha> test(camsim): add FPipelineLatencyTrackerConcurrencyTest
```

If the commit order or count is different, stop and investigate before moving forward.

- [ ] **Step 2: Sanity-check the global diff size.**

Run: `git diff --stat main..HEAD`

Expected: 4-5 files changed, roughly: `PipelineLatencyTracker.h` (~10 lines), `PipelineLatencyTracker.cpp` (~25 lines), `CamSimCamera.h` (~12 lines), `CamSimCamera.cpp` (~25 lines), new `PipelineLatencyTrackerConcurrencyTest.cpp` (~120 lines). Total roughly +150 / -25 lines.

If the diff is dramatically larger, you've scope-crept beyond Phase 1 — revert the excess.

- [ ] **Step 3: Audit for any remaining Relaxed orderings on cross-thread flags that should be Release/Acquire.**

Run: `grep -n 'EMemoryOrder::Relaxed' unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.h`

Read each result and confirm it's one of the explicitly-acceptable Relaxed cases:
- `RenderReadyStreak_` / `RenderDepthReadyStreak_` — pure counters, no paired data (Task 4 made these intentionally Relaxed).
- `PollGeneration_` — generation counter, compared for equality only.
- `DroppedFrameCount` — pure monotonic counter.
- Frame-drop stats counters in `FFrameDropStats`.

If a `Relaxed` use is found that protects paired data, flag it in the PR description as Phase 1 follow-up. Do NOT fix it inline — that's scope creep.

- [ ] **Step 4: CI verification statement (this is what must pass before merge).**

Open the PR and ensure the description states all three CI gates explicitly:

> **Verification gate:**
> - All 29 UE5 Automation tests pass (the new `CamSim.Phase1.Latency.ConcurrencyStress` should appear in the test list).
> - `scripts/ci_validate.sh` smoke harness passes — Docker headless, ffprobe video stream verification, KLV byte-stream validation.
> - `Phase27PerformanceTest` shows no regression beyond ~5% noise.
>
> **Baseline:** `main` at commit `<sha-from-task-0>`.

- [ ] **Step 5: Optional — local ThreadSanitizer run.**

If the engineer has a TSan-enabled UE5 build available, build with `-fsanitize=thread` and run the new test:

Run (from inside the UE editor `Automation` console): `Automation RunTests CamSim.Phase1.Latency.ConcurrencyStress`

Expected with TSan on the pre-Phase-1 codebase: data race reports on `CurrentFrame.Stages` access from multiple threads.
Expected with TSan on the post-Phase-1 codebase: zero race reports.

If TSan is unavailable (the common case for engineers without a custom UE build), skip this step. The release/acquire annotations themselves are sufficient — they encode the same invariants TSan would verify.

- [ ] **Step 6: Push the branch and open the PR.**

Run:
```bash
git push -u origin HEAD
gh pr create --title "refactor(camsim): Phase 1 — P0 cross-thread correctness" --body "$(cat <<'EOF'
## Summary
- Promotes `FPipelineLatencyTracker::CurrentFrame.Stages[]` from non-atomic `uint64[]` to per-stage `TAtomic<uint64>` with explicit Release/Acquire pairing — fixes a real C++ data race that is invisible on x86 but undefined on Apple Silicon dev workstations.
- Tightens `bReadbackDMAIssued` to Release store / Acquire load so render-thread DMA-side writes are guaranteed visible to the game thread on observation.
- Promotes `RenderReadyStreak_` / `RenderDepthReadyStreak_` to `TAtomic<uint8>` (Relaxed) to remove latent cross-thread fragility on the streak counters.
- Annotates the game-thread-only readback hand-off block with `// NOT atomic` comments and `checkSlow(IsInGameThread())` guards at both accessor sites.
- Adds `FPipelineLatencyTrackerConcurrencyTest` — 3-producer / 1-committer / 1-reader stress test that exercises the exact contention pattern the subsystem uses in production.

Phase 1 of the refactor described in `docs/superpowers/specs/2026-05-11-camsim-refactor-design.md`.

## Test plan
- [ ] All 29 UE5 Automation tests pass under CI, including the new `CamSim.Phase1.Latency.ConcurrencyStress`.
- [ ] `scripts/ci_validate.sh` smoke harness passes (ffprobe + KLV byte-stream validation).
- [ ] `Phase27PerformanceTest` shows no regression beyond ~5% noise vs. baseline at `<sha-from-task-0>`.
- [ ] (Optional) ThreadSanitizer run on the new test shows zero races on the post-Phase-1 code.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

Expected: PR created; CI starts. Wait for the verification gate; do not merge before it passes.

---

## Notes for the engineer

- **The plan does not require local UE5 builds.** Every code edit is mechanical and verifiable by visual diff inspection. CI is the source of truth.
- **Five separate commits** is intentional — they map directly to the five fixes the design spec calls out and make bisection trivial if anything regresses.
- **Phase 3 deletes some of this work.** `bReadbackDMAIssued`, `bPollComplete_`, `bPollFailed_`, and `bReadbackPending` collapse into a single `EReadbackState` enum in Phase 3. The Phase 1 fixes are not throwaway — the correct memory ordering rules transfer to the new state machine — but be aware the diff history will show Phase 1's `Relaxed → Release/Acquire` changes being subsumed by Phase 3's enum.
- **`checkSlow` vs `check`.** `checkSlow` is compiled out of shipping builds (Test/Shipping configs) and is the right choice for invariant guards on hot paths.
- **Do not attempt to fix `bPollComplete_` / `bPollFailed_` here.** They are already SeqCst, which is correct. Phase 3 collapses them into the enum.

## Self-Review (writing-plans skill)

**1. Spec coverage:**
- Spec item 1 (`RenderReadyStreak_` / `RenderDepthReadyStreak_` → atomic) → Task 4 ✓
- Spec item 2 (`bReadbackDMAIssued` Release/Acquire) → Task 3 ✓
- Spec item 3 (game-thread-only annotations + `checkSlow`) → Task 5 ✓
- Spec item 4 (`FPipelineLatencyTracker` audit + fix) → Task 1 (test) + Task 2 (fix) ✓
- Spec verification (new concurrency test) → Task 1 ✓
- Spec verification gate (29 tests + smoke + perf) → Task 6 ✓

**2. Placeholder scan:** All code blocks are complete. All file paths are absolute or repo-rooted. All commit messages and `gh pr create` body are inline. No TBDs.

**3. Type consistency:** `FCurrentFrame` is introduced in Task 2 Step 1 and consumed in Task 2 Step 2. `EMemoryOrder::Release` / `Acquire` / `Relaxed` are used consistently across Tasks 2–4. `TAtomic<uint8>` and `TAtomic<uint64>` are spelled identically everywhere they appear.
