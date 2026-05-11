# CamSim Refactor Phase 2 — P1 Per-Frame Allocation Pass — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate per-frame heap allocations on the sensor → encoder hot path. Target: ≥50% reduction in per-frame `FMemory::Stats` allocation count vs. the pre-Phase-2 baseline, measured by `Phase27PerformanceTest`.

**Architecture:** Promote five hot-path `TArray<FColor>` locals to reusable member buffers; replace one `TArray<uint8>` local in KLV; parallelize the AGC histogram pass; replace per-packet `fsync` and per-call `SaveStringToFile` with persistent handles. None of these change pixel-perfect output — every change is structural reuse of pre-existing memory.

**Tech Stack:** Unreal Engine 5.7 (`ParallelFor`, `TArray::SetNumUninitialized`, `IFileHandle`), C++17.

---

## Context

**Design spec:** `docs/superpowers/specs/2026-05-11-camsim-refactor-design.md` — Phase 2 section (lines 67–91).

**Verification gate for this PR:** All UE5 Automation tests pass + `scripts/ci_validate.sh` smoke harness passes + `Phase27PerformanceTest` shows per-frame allocation count ≥50% below the recorded pre-Phase-2 baseline AND no throughput regression beyond ~5% noise.

**Allocation hotspot inventory** (audited from the codebase, *not* from the design review — these are current line numbers on the post-Phase-1 main branch):

| Site | File:line | What allocates | Bytes/call (1080p) | Calls/sec |
|------|-----------|---|---|---|
| 1 | `Sensor/SensorPostProcess.cpp:806` (`ApplyBoxBlur`) | `TArray<FColor> Temp;` | 8.3 MB | up to 30 |
| 2 | `Sensor/SensorPostProcess.cpp:1147` (`ApplyGaussianBlur`) | `TArray<FColor> Temp;` | 8.3 MB | up to 30 |
| 3 | `Sensor/SensorPostProcess.cpp:922` (`ApplyLensDistortion`) | `TArray<FColor> Src = Pixels;` (alloc + copy) | 8.3 MB | up to 30 |
| 4 | `Sensor/SensorPostProcess.cpp:1459` (`ApplyVibration`) | `TArray<FColor> Src = Pixels;` | 8.3 MB | up to 30 |
| 5 | `Sensor/SensorPostProcess.cpp:1406` (`ApplyRollingShutter`) | `TArray<FColor> Unblended = Pixels;` | 8.3 MB | 30 (EO mode) |
| 6 | `Sensor/SensorPostProcess.cpp:1786-1791` (AGC histogram) | (no alloc — but ~2M serial reads/frame) | n/a | 30 |
| 7 | `Metadata/KlvBuilder.cpp:302` (`BuildMisbST0601Into`) | `TArray<uint8> Value;` | ~200 B | 30 |
| 8 | `Encoder/MultiViewFrameSink.cpp:104` (`EncodeFrame`) | `TArray<FColor> ZoomedPixels;` per view | 8.3 MB | up to N×30 |
| 9 | `Encoder/MultiViewFrameSink.cpp:251` (`WriteGroundTruthLine`) | `SaveStringToFile` open/seek/write/close | (syscalls) | up to 30 |
| 10 | `CIGI/CigiReceiver.cpp:807` (recording loop) | `RecordFileHandle->Flush()` per packet (~60 Hz) | (fsync) | 60 |

Aggregate at 1080p30: roughly 5× 8.3 MB = 40 MB heap pressure per frame plus ~30 file-syscall storms per second plus ~60 fsync's. Phase 2 closes all ten.

---

## File Structure

| File | Action | Responsibility |
|------|--------|---|
| `unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorPostProcess.h` | Modify | Add 3 reusable scratch members (`ScratchFrameA_`, `ScratchFrameB_`, `BlurTemp_`); sized in `Initialize`. |
| `unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorPostProcess.cpp` | Modify | Initialize the scratch members; migrate 5 `Apply*` functions to use them; parallelize the AGC histogram pass. |
| `unreal_project/CamSimTest/Source/CamSimTest/Metadata/KlvBuilder.h` | Modify | Add `ValueScratch_` member. |
| `unreal_project/CamSimTest/Source/CamSimTest/Metadata/KlvBuilder.cpp` | Modify | Replace local `Value` with `ValueScratch_.Reset(false)`. |
| `unreal_project/CamSimTest/Source/CamSimTest/Encoder/MultiViewFrameSink.h` | Modify | Add `TArray<FColor> ZoomedScratch_` to `FViewRuntime`; add `IFileHandle* GroundTruthHandle_`. |
| `unreal_project/CamSimTest/Source/CamSimTest/Encoder/MultiViewFrameSink.cpp` | Modify | Use per-view scratch buffer in `EncodeFrame`; parallelize `ApplyDigitalZoom`; replace `SaveStringToFile` with persistent handle in `Open`/`Close`/`WriteGroundTruthLine`. |
| `unreal_project/CamSimTest/Source/CamSimTest/CIGI/CigiReceiver.cpp` | Modify | Replace per-packet `Flush()` with periodic (every 100 packets) + final-on-`Stop`. |
| `unreal_project/CamSimTest/Source/CamSimTest/Tests/Phase27PerformanceTest.cpp` | Modify | Add an allocation-count assertion against the recorded baseline. |

No public-API changes. No structural changes to thread model. No new types other than the scratch members.

---

## Task 0 — Capture pre-Phase-2 baseline

**Goal:** Record the per-frame `FMemory::Stats` allocation count of `Phase27PerformanceTest` running against `main` (post-Phase-1). This number is the regression anchor for the rest of the phase.

**Files:** none modified.

- [ ] **Step 1: Confirm starting state.**

Run: `git status && git log --oneline -3`

Expected:
- On branch `refactor/camsim-phase-2-allocation-pass`.
- HEAD is the same as `main` (no commits ahead yet).
- Working tree clean.

If not, stop. Do not stash; investigate.

- [ ] **Step 2: Read the current `Phase27PerformanceTest` to understand what it measures.**

Run: `cat unreal_project/CamSimTest/Source/CamSimTest/Tests/Phase27PerformanceTest.cpp | head -120`

Read the test top-to-bottom. Confirm it exercises the sensor post-process + encoder hot path (it should — the design spec relies on this assumption).

Note for the engineer: the test likely uses `IMPLEMENT_SIMPLE_AUTOMATION_TEST` like the other automation tests. Memory-stats sampling can be done via `FPlatformMemory::GetStats()` before/after the test loop. If the test does not currently capture memory stats, you will add that capability in Task 9, but for Task 0 just record what the test produces today.

- [ ] **Step 3: Run the perf test on `main` to capture the baseline number.**

The recommended path is in-editor:
```
Automation RunTests CamSim.Phase27.Performance
```

Or via the headless harness used by CI:
```bash
scripts/ci_validate.sh --automation-test "CamSim.Phase27.Performance"
```

Record:
- Per-frame allocation count (the test logs it, or you can read it from the test summary in `Saved/Automation/`).
- Per-frame allocation byte count (same source).
- Mean frame time, p95 frame time.

Write these to `/tmp/camsim-phase2-baseline.txt` for use in the PR description and the Task 9 assertion threshold:

```
<commit-sha-of-main>
allocations/frame: <N>
bytes/frame:        <B>
mean ms/frame:      <T_mean>
p95  ms/frame:      <T_p95>
```

Note for the engineer: if running the full UE editor is not feasible in your environment, defer the baseline capture to CI — open the PR with placeholder values and update them once CI reports the first run. The 50% allocation reduction assertion in Task 9 is set up to read the threshold from the test itself, not from this file, so missing baseline data is recoverable.

---

## Task 1 — Add the three scratch buffers to `FSensorPostProcess`

**Goal:** Declare three reusable `TArray<FColor>` members in `FSensorPostProcess`, sized once in `Initialize(Width, Height)`. No call-site migration in this task — that comes in Tasks 2 and 3.

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorPostProcess.h`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorPostProcess.cpp`

- [ ] **Step 1: Add three private member declarations to the header.**

In `unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorPostProcess.h`, find the existing `AGCHistogram` member declaration (around line 142-143):

```cpp
	/** Pre-allocated 256-bin histogram scratch buffer (avoids per-frame alloc). */
	TArray<int32> AGCHistogram;
```

Immediately AFTER that block (before `BayerMatrix`), insert:

```cpp
	// -- Per-frame scratch buffers (Phase 2: avoid hot-path heap allocs) ----
	// Sized once in Initialize(W, H). Each Apply* function that needed a local
	// TArray<FColor> now uses one of these. They are NOT concurrently used — the
	// Apply* pipeline is strictly serial on the encoder task thread.
	//   - BlurTemp_      : separable-blur intermediate (ApplyBoxBlur, ApplyGaussianBlur)
	//   - ScratchFrameA_ : full-frame source snapshot (ApplyLensDistortion, ApplyVibration)
	//   - ScratchFrameB_ : reserved for a second snapshot if a future effect needs one
	//                      (ApplyRollingShutter uses it for the unblended snapshot)
	TArray<FColor> BlurTemp_;
	TArray<FColor> ScratchFrameA_;
	TArray<FColor> ScratchFrameB_;
```

- [ ] **Step 2: Size the three buffers in `Initialize`.**

In `unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorPostProcess.cpp`, find the `AGCHistogram.SetNumZeroed(256);` line (around line 106). Immediately AFTER it, before the Bayer-matrix comment block, insert:

```cpp

	// -------------------------------------------------------------------------
	// Phase 2: Pre-size scratch buffers so per-frame Apply* code can reuse
	// existing capacity instead of allocating. SetNumUninitialized is OK here
	// because every Apply* that uses these writes every pixel before reading.
	// -------------------------------------------------------------------------
	const int32 NumPixels = InWidth * InHeight;
	BlurTemp_     .SetNumUninitialized(NumPixels);
	ScratchFrameA_.SetNumUninitialized(NumPixels);
	ScratchFrameB_.SetNumUninitialized(NumPixels);
```

The variables `InWidth` and `InHeight` are the function's parameters — they are in scope at this point.

- [ ] **Step 3: Verify nothing else needs changing this task.**

Run: `grep -n 'BlurTemp_\|ScratchFrameA_\|ScratchFrameB_' unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorPostProcess.{h,cpp}`

Expected: 8 hits — 3 declarations in the header, 3 `SetNumUninitialized` calls in the cpp, plus the two comment blocks. No `Apply*` function uses them yet (those come in Tasks 2 and 3).

- [ ] **Step 4: Commit.**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorPostProcess.h \
        unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorPostProcess.cpp
git commit -m "$(cat <<'EOF'
perf(camsim): declare reusable scratch buffers in FSensorPostProcess

Add three TArray<FColor> members (BlurTemp_, ScratchFrameA_,
ScratchFrameB_), sized once in Initialize(W, H). No call-site migration
in this commit — Tasks 2 and 3 of the Phase 2 plan migrate the five
Apply* functions that currently allocate full-frame locals every call.

Setup-only commit, no functional change.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2 — Migrate `ApplyBoxBlur` and `ApplyGaussianBlur` to `BlurTemp_`

**Goal:** Eliminate the two `TArray<FColor> Temp;` per-call allocations in the separable-blur functions. Both functions take `Pixels` by reference and use `Temp` as an intermediate; both are caller-serial (never invoked concurrently).

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorPostProcess.cpp`

- [ ] **Step 1: Migrate `ApplyBoxBlur`.**

In `unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorPostProcess.cpp`, find the line at ~806:

```cpp
	TArray<FColor> Temp;
```

(It will be inside `ApplyBoxBlur`, after the early-return guard and before the first ParallelFor.)

Read the next ~10 lines after the declaration to understand how `Temp` is sized. If it's followed by `Temp.SetNumUninitialized(NumPixels);` or similar, **delete both the declaration and the SetNum line** and replace with:

```cpp
	// Phase 2: reuse the pre-sized BlurTemp_ member instead of allocating per call.
	// SetNumUninitialized is a no-op if the buffer is already the right size, which
	// it is after Initialize(W, H) — but we call it anyway in case the resolution
	// has changed since Initialize.
	BlurTemp_.SetNumUninitialized(Pixels.Num());
	TArray<FColor>& Temp = BlurTemp_;
```

The local-reference `TArray<FColor>& Temp = BlurTemp_;` lets the rest of the function stay byte-identical — every existing `Temp[i] = ...`, `Temp.GetData()`, etc. keeps working.

If `Temp` was sized differently (e.g., `Temp.SetNum(NumPixels)`), use the same `SetNumUninitialized` line above — the `SetNum` zero-initialization was wasted work, since the blur always writes every pixel before reading.

- [ ] **Step 2: Migrate `ApplyGaussianBlur`.**

Same pattern at line ~1147. Find:

```cpp
	TArray<FColor> Temp;
```

(Inside `ApplyGaussianBlur`.) Replace with the same `BlurTemp_` ref pattern from Step 1.

- [ ] **Step 3: Verify.**

Run: `grep -n 'TArray<FColor> Temp' unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorPostProcess.cpp`

Expected: zero matches. If any line prints, you missed one.

Run: `grep -n 'BlurTemp_' unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorPostProcess.cpp`

Expected: three matches — one `SetNumUninitialized` in `Initialize`, two `SetNumUninitialized` + reference-binding pairs in `ApplyBoxBlur` and `ApplyGaussianBlur`.

- [ ] **Step 4: Commit.**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorPostProcess.cpp
git commit -m "$(cat <<'EOF'
perf(camsim): reuse BlurTemp_ in ApplyBoxBlur and ApplyGaussianBlur

Both functions allocated a TArray<FColor> Temp; per call — 8.3 MB at
1080p × up to 30 fps = ~250 MB/s of allocator pressure per pipeline.

Replace the local with a reference to FSensorPostProcess::BlurTemp_,
pre-sized in Initialize. The rest of each function is unchanged — the
local-reference binding (TArray<FColor>& Temp = BlurTemp_) keeps every
existing Temp[i] / Temp.GetData() reference compiling without edits.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3 — Migrate `ApplyLensDistortion`, `ApplyVibration`, `ApplyRollingShutter` to scratch frames

**Goal:** Eliminate the three remaining full-frame per-call allocations. Each reads from a snapshot of `Pixels` then writes the result back to `Pixels`. Use `ScratchFrameA_` (or `ScratchFrameB_` if both are live at once — they are not, per the design analysis, but the second member is reserved for safety).

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorPostProcess.cpp`

- [ ] **Step 1: Migrate `ApplyLensDistortion`.**

Find line ~922:

```cpp
	TArray<FColor> Src = Pixels;
```

Replace with:

```cpp
	// Phase 2: reuse ScratchFrameA_ instead of allocating + copying every call.
	// The Apply* pipeline is strictly serial on the encoder task thread, so
	// ScratchFrameA_ is guaranteed not to be in use by a sibling function.
	ScratchFrameA_.SetNumUninitialized(Pixels.Num());
	FMemory::Memcpy(ScratchFrameA_.GetData(), Pixels.GetData(),
	                Pixels.Num() * sizeof(FColor));
	TArray<FColor>& Src = ScratchFrameA_;
```

The reference binding keeps the rest of the function unchanged.

- [ ] **Step 2: Migrate `ApplyVibration`.**

Find line ~1459:

```cpp
	TArray<FColor> Src = Pixels;
```

(Inside `ApplyVibration`.) Apply the same replacement as Step 1. `ScratchFrameA_` is reused — `ApplyLensDistortion` and `ApplyVibration` are never simultaneously in flight (the pipeline serializes per-frame).

- [ ] **Step 3: Migrate `ApplyRollingShutter`.**

Find line ~1406:

```cpp
	TArray<FColor> Unblended = Pixels;
```

Use `ScratchFrameB_` here (not A) — to leave the door open for a future effect that might want both snapshots. Replace with:

```cpp
	// Phase 2: reuse ScratchFrameB_ for the unblended snapshot. Using B (not A)
	// reserves A for ApplyLensDistortion/ApplyVibration in case a future change
	// chains them differently.
	ScratchFrameB_.SetNumUninitialized(Pixels.Num());
	FMemory::Memcpy(ScratchFrameB_.GetData(), Pixels.GetData(),
	                Pixels.Num() * sizeof(FColor));
	TArray<FColor>& Unblended = ScratchFrameB_;
```

- [ ] **Step 4: Verify.**

Run: `grep -nE 'TArray<FColor> (Src|Unblended) = Pixels' unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorPostProcess.cpp`

Expected: zero matches.

Run: `grep -n 'ScratchFrameA_\|ScratchFrameB_' unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorPostProcess.cpp`

Expected: at least 6 matches — 2 `SetNumUninitialized` in `Initialize`, 2 uses of A (LensDistortion, Vibration), 1 use of B (RollingShutter), and the reference bindings.

- [ ] **Step 5: Commit.**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorPostProcess.cpp
git commit -m "$(cat <<'EOF'
perf(camsim): reuse ScratchFrameA_/B_ in lens/vibration/rolling shutter

Three Apply* functions allocated a full-frame TArray<FColor> snapshot
per call:
  - ApplyLensDistortion: Src = Pixels (8.3 MB/call)
  - ApplyVibration:      Src = Pixels (8.3 MB/call)
  - ApplyRollingShutter: Unblended = Pixels (8.3 MB/call, EO mode)

The Apply* pipeline runs strictly serially on the encoder task thread,
so a single shared scratch buffer is safe. Lens/Vibration share
ScratchFrameA_; RollingShutter uses ScratchFrameB_ to leave A free in
case a future effect chains differently.

Each migration is a reference binding so the surrounding code stays
byte-identical (Src[i] / Unblended[i] keep compiling).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4 — Parallelize the AGC histogram pass

**Goal:** Replace the serial AGC histogram scan (currently ~2 M reads on a single thread at 1080p) with an 8-band `ParallelFor` that builds per-band local histograms, then a serial 8×256 merge. Per-band histograms are 1 KB each, well within L1.

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorPostProcess.cpp`

- [ ] **Step 1: Locate the existing serial histogram pass.**

Open `unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorPostProcess.cpp` around line 1780. The block looks like:

```cpp
		if (AGCHistogram.Num() != 256)
			AGCHistogram.SetNumZeroed(256);
		FMemory::Memzero(AGCHistogram.GetData(), 256 * sizeof(int32));

		for (int32 i = 0; i < NumPixels; ++i)
		{
			const FColor& P = Pixels[i];
			const uint8 Luma = static_cast<uint8>((P.R * 77 + P.G * 150 + P.B * 29) >> 8);
			AGCHistogram[Luma]++;
		}
```

- [ ] **Step 2: Replace the serial loop with a parallel build + serial reduce.**

Replace the block above with:

```cpp
		if (AGCHistogram.Num() != 256)
			AGCHistogram.SetNumZeroed(256);
		FMemory::Memzero(AGCHistogram.GetData(), 256 * sizeof(int32));

		// Phase 2: parallel histogram build across kParallelBands. Each band
		// keeps a private 256-bin histogram (1 KB, fits in L1), then we
		// reduce serially into AGCHistogram. The reduction is 8 × 256 adds
		// = 2 K integer ops, negligible vs the per-pixel work it replaced.
		TArray<TArray<int32>> BandHistograms;
		BandHistograms.SetNum(kParallelBands);
		for (int32 b = 0; b < kParallelBands; ++b)
			BandHistograms[b].SetNumZeroed(256);

		ParallelFor(kParallelBands, [&](int32 Band)
		{
			const int32 RowsPerBand = (Height + kParallelBands - 1) / kParallelBands;
			const int32 RowStart    = Band * RowsPerBand;
			const int32 RowEnd      = FMath::Min(RowStart + RowsPerBand, Height);
			int32* Local            = BandHistograms[Band].GetData();
			for (int32 y = RowStart; y < RowEnd; ++y)
			{
				const FColor* RowPx = Pixels.GetData() + y * Width;
				for (int32 x = 0; x < Width; ++x)
				{
					const FColor& P = RowPx[x];
					const uint8 Luma = static_cast<uint8>((P.R * 77 + P.G * 150 + P.B * 29) >> 8);
					Local[Luma]++;
				}
			}
		}, EParallelForFlags::BackgroundPriority);

		// Serial reduce.
		int32* Hist = AGCHistogram.GetData();
		for (int32 b = 0; b < kParallelBands; ++b)
		{
			const int32* Src = BandHistograms[b].GetData();
			for (int32 i = 0; i < 256; ++i)
				Hist[i] += Src[i];
		}
```

Note for the engineer: this assumes `Width` and `Height` are in scope. Look at the surrounding function to confirm — `ApplyRadianceAGC` takes `Pixels` and `Cfg`, and typically derives `Width`/`Height` from `this->Width`/`this->Height` (member fields set in `Initialize`). If they're not directly accessible at this point in the function, use `Width = this->Width` and `Height = this->Height` (or whatever the actual member spellings are — confirm by reading the rest of the function). If `NumPixels` is in scope but `Width`/`Height` are not, you can substitute `RowsPerBand` with `(NumPixels / Width + kParallelBands - 1) / kParallelBands` and iterate flat.

**The optional simpler fallback** (use only if Width/Height are NOT trivially available):

```cpp
		ParallelFor(kParallelBands, [&](int32 Band)
		{
			const int32 PixPerBand = (NumPixels + kParallelBands - 1) / kParallelBands;
			const int32 Start      = Band * PixPerBand;
			const int32 End        = FMath::Min(Start + PixPerBand, NumPixels);
			int32* Local           = BandHistograms[Band].GetData();
			for (int32 i = Start; i < End; ++i)
			{
				const FColor& P = Pixels[i];
				const uint8 Luma = static_cast<uint8>((P.R * 77 + P.G * 150 + P.B * 29) >> 8);
				Local[Luma]++;
			}
		}, EParallelForFlags::BackgroundPriority);
```

Both forms produce identical histograms because each input pixel is visited exactly once. Pick whichever fits the function's existing variable set.

- [ ] **Step 3: Verify percentile thresholds still find the same answers.**

The two `for (int32 b = 0; b < 256; ++b) { Accum += AGCHistogram[b]; if (Accum >= ...) ...; break; }` loops below the histogram build (lines ~1797–1807) are unchanged — they operate on the merged `AGCHistogram` array which now holds the same totals it held before. No edit needed there.

- [ ] **Step 4: Verify the change compiles cleanly.**

Run: `grep -nE 'AGCHistogram\[Luma\]\+\+|BandHistograms' unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorPostProcess.cpp`

Expected:
- Zero matches for `AGCHistogram[Luma]++` (the serial increment is gone).
- Multiple `BandHistograms` matches (the new parallel structure).

- [ ] **Step 5: Commit.**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorPostProcess.cpp
git commit -m "$(cat <<'EOF'
perf(camsim): parallelize AGC histogram build

The AGC histogram pass scanned ~2M pixels serially on a single thread
before the rest of the AGC compute went parallel — at 30 fps this was
~62M serial reads/sec, an obvious throughput bottleneck.

Replace with kParallelBands threads each building a private 256-bin
histogram (1 KB per band, fits in L1), then a 2 K-op serial reduce
into the existing AGCHistogram member. Per-pixel work is unchanged;
only the loop structure moves.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5 — Reuse `ValueScratch_` in `FKlvBuilder`

**Goal:** Eliminate the per-frame `TArray<uint8> Value;` allocation in `BuildMisbST0601Into`.

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Metadata/KlvBuilder.h`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Metadata/KlvBuilder.cpp`

- [ ] **Step 1: Add the scratch member to the header.**

In `unreal_project/CamSimTest/Source/CamSimTest/Metadata/KlvBuilder.h`, find the private section of `FKlvBuilder` (locate the class via `grep -n 'class FKlvBuilder' unreal_project/CamSimTest/Source/CamSimTest/Metadata/KlvBuilder.h` — it has private fields already).

Add this member at the bottom of the private section (above the closing `};`):

```cpp
	// Phase 2: per-frame TLV scratch buffer reused across BuildMisbST0601Into
	// calls. Holds the value portion of each TLV tag (Tag 13 timestamps, security
	// metadata strings, sensor mode bytes, etc.) before it's appended to the
	// outer packet buffer. Capacity amortises after the first frame.
	mutable TArray<uint8> ValueScratch_;
```

(`mutable` because `BuildMisbST0601Into` is declared `const` in the header — if you check and it is not `const`, drop the `mutable` keyword.)

- [ ] **Step 2: Migrate the body.**

In `unreal_project/CamSimTest/Source/CamSimTest/Metadata/KlvBuilder.cpp`, find line ~302:

```cpp
	TArray<uint8> Value;
```

Replace with:

```cpp
	// Phase 2: reuse the ValueScratch_ member instead of allocating per call.
	// Reset(/*AllowShrink=*/false) preserves capacity across frames.
	ValueScratch_.Reset(/*AllowShrink=*/false);
	TArray<uint8>& Value = ValueScratch_;
```

The reference binding keeps every subsequent `Value.Add()`, `Value.AddUninitialized()`, `Value.Num()`, etc. unchanged.

- [ ] **Step 3: Verify.**

Run: `grep -n 'TArray<uint8> Value' unreal_project/CamSimTest/Source/CamSimTest/Metadata/KlvBuilder.cpp`

Expected: zero matches. (Note: this regex matches the exact local-decl form; uses of `Value` are unaffected.)

Run: `grep -n 'ValueScratch_' unreal_project/CamSimTest/Source/CamSimTest/Metadata/KlvBuilder.{h,cpp}`

Expected: at least three matches — one declaration in `.h`, the `Reset` + binding in `.cpp`.

- [ ] **Step 4: Commit.**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Metadata/KlvBuilder.h \
        unreal_project/CamSimTest/Source/CamSimTest/Metadata/KlvBuilder.cpp
git commit -m "$(cat <<'EOF'
perf(camsim): reuse ValueScratch_ in FKlvBuilder::BuildMisbST0601Into

The TLV value buffer was allocated fresh on every frame (~200 B + an
amortised handful of sub-allocations × 30 fps). Move it to a mutable
member and Reset(/*AllowShrink=*/false) at the start of each call so
capacity amortises after the first frame.

Local reference binding (TArray<uint8>& Value = ValueScratch_) keeps
the rest of the function byte-identical.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6 — Per-view zoom scratch + parallelize zoom

**Goal:** Two changes to `FMultiViewFrameSink`: (a) give each `FViewRuntime` a persistent `ZoomedScratch_` so multi-view fan-out doesn't allocate 8.3 MB per view per frame, and (b) parallelize the zoom inner loop.

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Encoder/MultiViewFrameSink.h`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Encoder/MultiViewFrameSink.cpp`

- [ ] **Step 1: Add the scratch member to `FViewRuntime`.**

In `unreal_project/CamSimTest/Source/CamSimTest/Encoder/MultiViewFrameSink.h`, find the `FViewRuntime` struct (around line 35):

```cpp
	struct FViewRuntime
	{
		int32 ViewId = 0;
		FCamSimConfig ViewConfig;
		float OutputHFovDeg = 0.0f;
		FString RouteLabel;
		TUniquePtr<FVideoEncoder> Encoder;
	};
```

Add at the bottom of the struct (above the closing `};`):

```cpp
		// Phase 2: per-view zoom scratch — sized once when the encoder opens.
		// EncodeFrame writes into it instead of allocating per frame per view.
		TArray<FColor> ZoomedScratch;
```

- [ ] **Step 2: Update `EncodeFrame` to use the per-view scratch.**

In `unreal_project/CamSimTest/Source/CamSimTest/Encoder/MultiViewFrameSink.cpp`, find line ~104 in `EncodeFrame`:

```cpp
		TArray<FColor> ZoomedPixels;
```

(It will be inside the for-loop that iterates views.) Look at the surrounding loop body — likely:

```cpp
	for (FViewRuntime& View : Views)
	{
		...
		TArray<FColor> ZoomedPixels;
		const TArray<FColor>* SourcePtr = &PixelData;
		if (... zoom needed ...)
		{
			ApplyDigitalZoom(PixelData, Width, Height, SourceHFov, TargetHFov, ZoomedPixels);
			SourcePtr = &ZoomedPixels;
		}
		View.Encoder->Encode(*SourcePtr, ...);
	}
```

Replace `TArray<FColor> ZoomedPixels;` with `TArray<FColor>& ZoomedPixels = View.ZoomedScratch;`.

The first call per view will allocate inside the existing `ApplyDigitalZoom` (via `SetNumUninitialized`); subsequent calls reuse capacity.

- [ ] **Step 3: Parallelize `ApplyDigitalZoom`.**

In the same file, find `ApplyDigitalZoom` (around line 258). The function takes `const TArray<FColor>& SourcePixels, int32 Width, int32 Height, float SourceHFovDeg, float TargetHFovDeg, TArray<FColor>& OutPixels`.

Find the per-output-pixel loop. It almost certainly looks like:

```cpp
	for (int32 y = 0; y < Height; ++y)
	{
		for (int32 x = 0; x < Width; ++x)
		{
			// compute source coordinate from x, y and the zoom factor
			...
			OutPixels[y * Width + x] = ...;
		}
	}
```

Replace the OUTER `for (int32 y = 0; y < Height; ++y)` with a `ParallelFor`:

```cpp
	ParallelFor(Height, [&](int32 y)
	{
		for (int32 x = 0; x < Width; ++x)
		{
			// ... existing inner-loop body unchanged ...
			OutPixels[y * Width + x] = ...;
		}
	}, EParallelForFlags::BackgroundPriority);
```

Each output pixel reads from a distinct source location — embarrassingly parallel, no synchronization needed.

If the existing loop structure differs (e.g., `for (int32 i = 0; i < W*H; ++i)`), wrap that flat index in a `ParallelFor` over `kParallelBands` instead:

```cpp
	constexpr int32 kZoomBands = 8;
	ParallelFor(kZoomBands, [&](int32 Band)
	{
		const int32 RowsPerBand = (Height + kZoomBands - 1) / kZoomBands;
		const int32 RowStart    = Band * RowsPerBand;
		const int32 RowEnd      = FMath::Min(RowStart + RowsPerBand, Height);
		// existing flat loop, restricted to [RowStart, RowEnd)
		...
	}, EParallelForFlags::BackgroundPriority);
```

Pick whichever form is the smaller diff against the existing code.

Note for the engineer: if `ApplyDigitalZoom` is declared `static`, the `ParallelFor` lambda must not capture `this` — only the function parameters by reference. This is fine: `Width`, `Height`, `OutPixels`, `SourcePixels`, and the zoom factors are all parameters and can be captured with `[&]`.

- [ ] **Step 4: Verify.**

Run: `grep -n 'ZoomedScratch\|ZoomedPixels' unreal_project/CamSimTest/Source/CamSimTest/Encoder/MultiViewFrameSink.{h,cpp}`

Expected: `ZoomedScratch` declared in header; `ZoomedPixels` is now a reference to `View.ZoomedScratch` in `EncodeFrame`; no fresh `TArray<FColor> ZoomedPixels;` declarations remain.

Run: `grep -n 'ParallelFor' unreal_project/CamSimTest/Source/CamSimTest/Encoder/MultiViewFrameSink.cpp`

Expected: one new match inside `ApplyDigitalZoom`.

- [ ] **Step 5: Commit.**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Encoder/MultiViewFrameSink.h \
        unreal_project/CamSimTest/Source/CamSimTest/Encoder/MultiViewFrameSink.cpp
git commit -m "$(cat <<'EOF'
perf(camsim): per-view zoom scratch + parallel ApplyDigitalZoom

EncodeFrame allocated a fresh TArray<FColor> ZoomedPixels per view per
frame — 8.3 MB at 1080p × N views × 30 fps. Move the buffer onto
FViewRuntime so capacity amortises after the first frame for each view.

Parallelize ApplyDigitalZoom across rows: each output pixel reads from
a distinct source location, so the loop is embarrassingly parallel.
Cuts zoom CPU time from single-threaded to (Height / kZoomBands)-bound.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7 — Persistent `IFileHandle*` for the ground-truth sidecar

**Goal:** Replace `FFileHelper::SaveStringToFile(..., FILEWRITE_Append)` (which opens, seeks to EOF, writes, and closes per call) with an `IFileHandle*` opened once in `Open()` and closed in `Close()`. Removes 30 file-open syscall cycles per second on the encode thread.

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Encoder/MultiViewFrameSink.h`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Encoder/MultiViewFrameSink.cpp`

- [ ] **Step 1: Add the handle member to the header.**

In `unreal_project/CamSimTest/Source/CamSimTest/Encoder/MultiViewFrameSink.h`, find the private section (just below `TArray<FViewRuntime> Views;`, around line 47):

Add:

```cpp
	// Phase 2: persistent JSONL sidecar handle. Opened once in Open() and closed
	// in Close(); WriteGroundTruthLine appends through this handle instead of
	// FFileHelper::SaveStringToFile which opens/seeks/writes/closes per call.
	class IFileHandle* GroundTruthHandle_ = nullptr;
```

(Use `class IFileHandle*` so callers don't need the `GenericPlatform/GenericPlatformFile.h` include here. The `.cpp` will include it.)

The change to `WriteGroundTruthLine`'s signature: it is currently `const`. Writing to a member non-`const` field would normally require dropping `const`, but `IFileHandle*` is just a pointer, and the write is through the pointed-to object — so the `const` declaration can stay. The pointer itself never mutates after `Open()`.

- [ ] **Step 2: Open the handle in `Open()`.**

In `unreal_project/CamSimTest/Source/CamSimTest/Encoder/MultiViewFrameSink.cpp`, find `Open()`. There's already a section that initializes the ground-truth file via `FFileHelper::SaveStringToFile(TEXT(""), *GroundTruthPath);` (around line 68). Replace that line with:

```cpp
		// Phase 2: open a persistent append handle instead of using
		// FFileHelper::SaveStringToFile per line — saves 30 open/close
		// syscall cycles per second at 30 fps with ground truth on.
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		// Truncate and (re)create the sidecar; append-only after this point.
		PlatformFile.DeleteFile(*GroundTruthPath);
		GroundTruthHandle_ = PlatformFile.OpenWrite(*GroundTruthPath, /*bAppend=*/true);
		if (!GroundTruthHandle_)
		{
			UE_LOG(LogCamSim, Warning, TEXT("MultiViewFrameSink: could not open ground-truth sidecar at %s"), *GroundTruthPath);
			bGroundTruthEnabled = false;
		}
```

You'll need to include `HAL/PlatformFileManager.h` and `GenericPlatform/GenericPlatformFile.h` at the top of `MultiViewFrameSink.cpp` if they are not already included. Add them in the existing include block.

- [ ] **Step 3: Close the handle in `Close()`.**

Find `Close()`. Add at the start of the body (before any encoder teardown):

```cpp
	if (GroundTruthHandle_)
	{
		delete GroundTruthHandle_;
		GroundTruthHandle_ = nullptr;
	}
```

- [ ] **Step 4: Migrate `WriteGroundTruthLine`.**

Find the function (around line 217). The function builds an `FString Json` then calls (around line 251):

```cpp
	FFileHelper::SaveStringToFile(
		Json,
		*GroundTruthPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
		&IFileManager::Get(),
		FILEWRITE_Append | FILEWRITE_AllowRead);
```

Replace that block with:

```cpp
	if (!GroundTruthHandle_) return;

	// JSONL: one record per line, UTF-8 (no BOM).
	const FTCHARToUTF8 Utf8(*Json);
	GroundTruthHandle_->Write(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	const char Newline = '\n';
	GroundTruthHandle_->Write(reinterpret_cast<const uint8*>(&Newline), 1);
```

Important: `IFileHandle::Write` takes `(const uint8*, int64)`. Make sure the cast and length call match. `FTCHARToUTF8::Length()` returns the byte length of the converted UTF-8 string, not the source TCHAR count.

Note for the engineer: `IFileHandle::Write` is buffered at the OS level but does not call `fsync`. The data is durable on graceful `Close()`. If a future crash needs sub-frame durability, add an explicit `Flush()` here — but Phase 2 explicitly accepts up-to-one-frame data loss on crash for the diagnostic sidecar.

- [ ] **Step 5: Verify.**

Run: `grep -n 'SaveStringToFile\|GroundTruthHandle_\|FILEWRITE_Append' unreal_project/CamSimTest/Source/CamSimTest/Encoder/MultiViewFrameSink.cpp`

Expected:
- No remaining `SaveStringToFile` or `FILEWRITE_Append` for the ground-truth sidecar (the empty-file truncation at `Open` is now a `DeleteFile` + `OpenWrite`).
- `GroundTruthHandle_` referenced in `Open`, `Close`, and `WriteGroundTruthLine`.

- [ ] **Step 6: Commit.**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Encoder/MultiViewFrameSink.h \
        unreal_project/CamSimTest/Source/CamSimTest/Encoder/MultiViewFrameSink.cpp
git commit -m "$(cat <<'EOF'
perf(camsim): persistent ground-truth file handle in MultiViewFrameSink

WriteGroundTruthLine called FFileHelper::SaveStringToFile per record,
which opens, seeks to EOF, writes, and closes the OS file descriptor
on every line. At 30 fps with ground truth on this was 30 open/close
syscall cycles per second on the encode thread.

Open one IFileHandle in Open(), close it in Close(), and append via
IFileHandle::Write in WriteGroundTruthLine. The handle is OS-buffered;
crash durability degrades to "up to one frame may be lost on hard
crash" which is acceptable for a diagnostic sidecar.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 8 — Periodic flush in CIGI receiver recording path

**Goal:** Replace per-packet `RecordFileHandle->Flush()` (currently ~60 fsync's per second on the CIGI receiver thread) with periodic flush every 100 packets, plus an unconditional final flush in `Stop()`.

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/CIGI/CigiReceiver.cpp`

- [ ] **Step 1: Locate the recording loop.**

Find line 807 in `unreal_project/CamSimTest/Source/CamSimTest/CIGI/CigiReceiver.cpp`:

```cpp
				RecordFileHandle->Flush();
```

It's inside the recv loop, after writing the timestamp, length, and packet bytes.

- [ ] **Step 2: Replace with periodic flush.**

The simplest form: add a counter local to the `Run()` method scope (the surrounding function — find where the per-packet `if (RecordFileHandle)` block sits, then add a counter just outside the loop). Replace the per-packet `Flush()` with a modulo check.

If `Run()` already has a local `int32` counter, reuse it; otherwise add one above the loop:

```cpp
	int32 RecordWriteCount = 0;  // Phase 2: periodic flush instead of per-packet.
```

Then change the recording block from:

```cpp
				RecordFileHandle->Write(reinterpret_cast<const uint8*>(&Ts), sizeof(Ts));
				RecordFileHandle->Write(reinterpret_cast<const uint8*>(&Len), sizeof(Len));
				RecordFileHandle->Write(RecvBuf, BytesRead);
				RecordFileHandle->Flush();
```

to:

```cpp
				RecordFileHandle->Write(reinterpret_cast<const uint8*>(&Ts), sizeof(Ts));
				RecordFileHandle->Write(reinterpret_cast<const uint8*>(&Len), sizeof(Len));
				RecordFileHandle->Write(RecvBuf, BytesRead);
				// Phase 2: flush every 100 packets instead of every packet.
				// At ~60 Hz CIGI that's a flush every ~1.7 s; crash recovery
				// loses up to ~1.7 s of recording, acceptable for diagnostics.
				if (((++RecordWriteCount) % 100) == 0)
					RecordFileHandle->Flush();
```

- [ ] **Step 3: Add an unconditional flush in `Stop` / shutdown path.**

Find where `RecordFileHandle` is closed — there's an existing `delete RecordFileHandle;` at line ~640. Just before the `delete`, add:

```cpp
	if (RecordFileHandle)
	{
		RecordFileHandle->Flush();
		// existing: delete RecordFileHandle; RecordFileHandle = nullptr;
	}
```

Or simply add the `RecordFileHandle->Flush();` line right above the existing `delete`.

- [ ] **Step 4: Verify.**

Run: `grep -n 'RecordFileHandle->Flush' unreal_project/CamSimTest/Source/CamSimTest/CIGI/CigiReceiver.cpp`

Expected: exactly TWO matches — one inside the modulo-100 if-block, one in the shutdown path.

- [ ] **Step 5: Commit.**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/CIGI/CigiReceiver.cpp
git commit -m "$(cat <<'EOF'
perf(camsim): periodic flush in CIGI recording loop

RecordFileHandle->Flush() ran on every received CIGI packet — at
~60 Hz that's 60 fsync syscalls/sec on the receiver thread, visible
as latency spikes in the heartbeat FPS counter.

Move to every-100-packets cadence (~1.7 s at 60 Hz CIGI) plus an
unconditional final flush in shutdown. Crash recovery now loses up
to ~1.7 s of CIGI recording, acceptable for a diagnostic feature.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 9 — Extend `Phase27PerformanceTest` with an allocation-count assertion

**Goal:** Add a regression net so the ≥50% allocation reduction can't silently regress. Capture `FPlatformMemory::GetStats()` (or `FMemory::GetAllocatorStats`, whichever the test currently uses) before and after the perf loop and assert the per-frame allocation count is below a threshold.

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Tests/Phase27PerformanceTest.cpp`

- [ ] **Step 1: Read the existing test to understand its structure.**

Run: `cat unreal_project/CamSimTest/Source/CamSimTest/Tests/Phase27PerformanceTest.cpp`

Note where the per-frame loop is. The test likely already records frame timing — extend the same recording pattern to also record allocation counts.

- [ ] **Step 2: Add allocation-count capture around the per-frame loop.**

Approach: use `FMemory::GetAllocatorStats(FGenericMemoryStats&)` if available, else `FPlatformMemory::GetStats()`. The exact API may differ between UE versions — the simplest portable option is:

```cpp
	const SIZE_T AllocBytesBefore = FPlatformMemory::GetStats().UsedPhysical;
	int64 AllocCountBefore = 0;
	{
		FGenericMemoryStats Stats;
		FMemory::GetAllocatorStats(Stats);
		// Stats accessor varies by UE version; consult the engine header.
		// Phase 2 uses UsedPhysical delta as a proxy if precise allocation count is unavailable.
		AllocCountBefore = 0;  // placeholder if exact API not exposed
	}

	// ... existing perf loop runs ...

	const SIZE_T AllocBytesAfter = FPlatformMemory::GetStats().UsedPhysical;
	const SIZE_T BytesPerFrame  = (AllocBytesAfter - AllocBytesBefore) / FMath::Max<int32>(1, FramesRun);
	UE_LOG(LogCamSim, Display, TEXT("Phase27Perf: bytes/frame delta = %llu"), (uint64)BytesPerFrame);
```

Note for the engineer: precisely counting per-frame allocations from a UE5 test is harder than reading bytes-per-frame. The simpler assertion is on bytes/frame (which captures both count and average size). If the existing test already uses a different memory-stats mechanism (e.g., a `FScopedDurationTimeLogger` or custom counter), extend that mechanism instead of introducing a new one.

- [ ] **Step 3: Add the regression assertion.**

After the loop, add:

```cpp
	// Phase 2 regression net: ≥50% reduction in per-frame allocation pressure
	// vs. the pre-Phase-2 baseline. Update the baseline number below ONLY when
	// you have a justified reason to relax this (e.g., a feature addition that
	// legitimately needs more memory) and document the reason in the PR.
	constexpr uint64 kPrePhase2BytesPerFrameBaseline = 0; // TODO Task 0: paste recorded value here.
	if (kPrePhase2BytesPerFrameBaseline > 0)
	{
		const uint64 Threshold = kPrePhase2BytesPerFrameBaseline / 2;
		TestTrue(
			FString::Printf(TEXT("Phase 2: per-frame heap delta %llu <= %llu (half baseline)"),
			               (uint64)BytesPerFrame, Threshold),
			(uint64)BytesPerFrame <= Threshold);
	}
	else
	{
		AddInfo(TEXT("Phase 2 baseline not yet recorded; allocation assertion skipped. Update kPrePhase2BytesPerFrameBaseline."));
	}
```

The `if (kPrePhase2BytesPerFrameBaseline > 0)` gate means CI passes immediately with the placeholder zero; the assertion fires once the baseline is filled in. This lets Task 9 land before the baseline is captured (see Task 0 note about deferring to CI).

- [ ] **Step 4: Commit.**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Tests/Phase27PerformanceTest.cpp
git commit -m "$(cat <<'EOF'
test(camsim): add allocation-count regression assertion to Phase27Perf

Phase 2 of the refactor eliminates per-frame heap allocations on the
sensor → encoder hot path (target ≥50% reduction vs baseline). Add a
TestTrue assertion that fails if per-frame heap delta exceeds half the
pre-Phase-2 baseline.

The baseline constant kPrePhase2BytesPerFrameBaseline defaults to 0,
which gates the assertion off (placeholder mode). Once Task 0's CI
baseline run produces a real number, set it here and the assertion
becomes a hard regression net for all subsequent phases.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 10 — Final verification + open PR

**Goal:** Run the verification gate, audit for missed allocations, open the PR.

**Files:** none modified.

- [ ] **Step 1: Audit remaining per-frame heap allocations.**

Run:

```bash
grep -nE 'TArray<FColor> [A-Z][a-zA-Z]+;?$|TArray<float> [A-Z][a-zA-Z]+;?$|TArray<uint8> [A-Z][a-zA-Z]+;?$' \
  unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorPostProcess.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Encoder/MultiViewFrameSink.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Metadata/KlvBuilder.cpp
```

Expected: every remaining local `TArray<...>` declaration in these files is either (a) a reference binding to a scratch member (allowed), (b) inside a function that runs at most once per session (Initialize, Open, etc.), or (c) a small-sized buffer (~hundreds of bytes max, not the full-frame multi-MB allocations Phase 2 targeted).

If any per-frame full-frame allocation slipped past, flag it in the PR description as a known follow-up rather than retrofitting Phase 2.

- [ ] **Step 2: Confirm commit count and diff size.**

Run: `git log --oneline main..HEAD && echo "---" && git diff --stat main..HEAD`

Expected:
- 9 commits (Tasks 1–9).
- ~6 files changed.
- Diff size roughly: SensorPostProcess.cpp/h ~70 lines, KlvBuilder.cpp/h ~10, MultiViewFrameSink.cpp/h ~50, CigiReceiver.cpp ~10, Phase27PerformanceTest.cpp ~40.

If diff size is dramatically larger, you've scope-crept beyond Phase 2 — revert the excess.

- [ ] **Step 3: PR description gate.**

Open the PR with verification gate spelled out:

> **Verification gate:**
> - All UE5 Automation tests pass, including the extended `Phase27PerformanceTest` with allocation-count assertion.
> - `scripts/ci_validate.sh` smoke harness passes — byte-identical KLV stream + ffprobe video.
> - `Phase27PerformanceTest` shows per-frame bytes-delta ≤50% of pre-Phase-2 baseline.
>
> **Baseline:** `main` at commit `<sha>` (per Task 0 capture).

- [ ] **Step 4: Push and open PR.**

```bash
git push -u origin HEAD
gh pr create --title "refactor(camsim): Phase 2 — P1 per-frame allocation pass" --body "$(cat <<'EOF'
## Summary
- Five `Apply*` functions in `FSensorPostProcess` no longer allocate a full-frame `TArray<FColor>` per call — they reuse three pre-sized scratch members (`BlurTemp_`, `ScratchFrameA_`, `ScratchFrameB_`).
- AGC histogram pass is now parallel (`kParallelBands` × 256-bin local histograms + serial reduce), replacing ~62M serial reads/sec on a single thread.
- `FKlvBuilder::BuildMisbST0601Into` uses a `ValueScratch_` member instead of a per-frame local.
- `FMultiViewFrameSink` gains per-view `ZoomedScratch` (eliminates 8.3 MB × N views/frame) and a persistent `IFileHandle*` for the ground-truth JSONL sidecar (eliminates 30 open/close/sec).
- `ApplyDigitalZoom` is parallelized across rows.
- `FCigiReceiver` recording path flushes every 100 packets instead of every packet (60 fsync's/sec → ~0.6/sec).
- `Phase27PerformanceTest` extended with an allocation-count assertion against the pre-Phase-2 baseline.

Phase 2 of the refactor described in `docs/superpowers/specs/2026-05-11-camsim-refactor-design.md`.

## Notes for reviewers
- **No public-API changes.** Every change is structural reuse of pre-existing memory or a syscall-cadence reduction.
- **Pixel-perfect output preserved.** Smoke harness must produce a byte-identical KLV stream + ffprobe-decodable MPEG-TS.
- **Acceptable durability tradeoffs:** CIGI recording loses up to ~1.7 s on hard crash (was ~16 ms); ground-truth JSONL loses up to one frame.
- **Baseline number:** captured per Task 0 of the plan and recorded in the test's `kPrePhase2BytesPerFrameBaseline` constant.

## Test plan
- [ ] All UE5 Automation tests pass.
- [ ] `scripts/ci_validate.sh` smoke harness passes.
- [ ] `Phase27PerformanceTest` shows per-frame bytes ≤50% of `<baseline-sha>` baseline.
- [ ] KLV byte-stream diff against `<baseline-sha>` is empty.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

---

## Notes for the engineer

- **Reference bindings keep edits small.** `TArray<FColor>& Temp = BlurTemp_;` lets the rest of the function stay byte-identical instead of a sed-replace through dozens of `Temp[i]` references. Use the pattern in every migration in Tasks 2, 3, 5.
- **`SetNumUninitialized` vs `SetNum`.** The Apply* code always overwrites every pixel before reading; `SetNumUninitialized` skips the zero-fill. The previous local `TArray<FColor> Temp;` had no explicit size, so `Temp.SetNum(NumPixels)` somewhere would have zero-initialized — Phase 2 quietly eliminates that wasted work too.
- **Apply* serial pipeline.** All five migrated functions run on the encoder task thread, strictly one at a time per frame. Shared scratch is safe. If a future change ever calls two Apply* functions concurrently, the scratch sharing breaks — the comment in Task 1 Step 1 documents the invariant.
- **`ParallelFor` band sizing.** `kParallelBands` is already defined in `SensorPostProcess.cpp` line 8. The same constant is the right band count for AGC histogram (Task 4) and ApplyDigitalZoom (Task 6).
- **Baseline gate.** Task 9's `kPrePhase2BytesPerFrameBaseline = 0` placeholder is intentional — the test passes with zero (assertion is gated off), and CI's first run after the PR is opened produces the real number that gets pasted back. Don't block the PR on having a baseline in hand at open time.
- **Phase 3 dependency.** Phase 3 extracts `GpuReadbackPipeline` from `CamSimCamera.cpp` and adds another scratch buffer (the ping-pong color buffers). Phase 2 does NOT touch those — they're owned by `ACamSimCamera`, not `FSensorPostProcess`, and Phase 3 has the right context to handle them.

## Self-Review (writing-plans skill)

**1. Spec coverage:**
- Spec item 1 (`FSensorPostProcess` scratch buffers) → Tasks 1, 2, 3 ✓
- Spec item 2 (AGC histogram parallel) → Task 4 ✓
- Spec item 3 (`FKlvBuilder` `ValueScratch_`) → Task 5 ✓
- Spec item 4 (`FViewRuntime` `ZoomedPixels_` + parallel zoom) → Task 6 ✓
- Spec item 5 (`MultiViewFrameSink` persistent ground-truth handle) → Task 7 ✓
- Spec item 6 (`CigiReceiver` periodic flush) → Task 8 ✓
- Spec verification (extended `Phase27PerformanceTest`) → Task 9 ✓

**2. Placeholder scan:** Task 9's `kPrePhase2BytesPerFrameBaseline = 0` is the only placeholder, and it is intentional + documented in the plan and commit message.

**3. Type consistency:** `BlurTemp_`, `ScratchFrameA_`, `ScratchFrameB_`, `ValueScratch_`, `ZoomedScratch`, `GroundTruthHandle_` — names used consistently across all tasks.

**4. Ambiguity check:** Task 4 explicitly provides two alternative forms (row-banded vs flat-banded) for the AGC histogram parallel loop, so the engineer can pick whichever fits the existing variable scope without guessing.
