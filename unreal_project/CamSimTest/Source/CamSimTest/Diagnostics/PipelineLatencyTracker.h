// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"

/** Pipeline stages for latency tracking. */
enum class EPipelineStage : uint8
{
	CigiDequeue = 0,
	GameTickStart,
	ReadbackIssue,
	ReadbackComplete,
	SensorStart,
	SensorEnd,
	EncodeComplete,
	Count
};

/**
 * FPipelineLatencyTracker
 *
 * Records per-frame timestamps at each pipeline stage and computes
 * P50/P95/P99 latency percentiles over a ring buffer.
 *
 * Thread safety: Multiple threads write to the current frame's staging area
 * via SetStageTimestamp() (each stage written by exactly one thread — no
 * contention on the same slot). CommitFrame() atomically advances the write
 * index. ComputePercentiles() reads committed entries (no lock needed — reader
 * only touches entries behind the write index).
 */
struct FPipelineLatencyTracker
{
	explicit FPipelineLatencyTracker(int32 BufferSize = 300);

	/** Record a timestamp for the given pipeline stage in the current frame. */
	void SetStageTimestamp(EPipelineStage Stage, uint64 Cycles);

	/** Convenience: record current time for the given stage. */
	void Mark(EPipelineStage Stage) { SetStageTimestamp(Stage, FPlatformTime::Cycles64()); }

	/** Commit the current frame to the ring buffer. Call once per frame from encoder thread. */
	void CommitFrame();

	struct FLatencyPercentiles
	{
		float ReadbackUs[3] = {};  // P50, P95, P99
		float SensorUs[3]   = {};
		float EncodeUs[3]   = {};
		float TotalUs[3]    = {};  // CigiDequeue -> EncodeComplete
	};

	/** Compute percentiles over committed frames. Safe to call from game thread. */
	FLatencyPercentiles ComputePercentiles() const;

	/** Number of committed frames in the buffer. */
	int32 GetCommittedCount() const;

private:
	// Plain POD record stored in the ring — written only by the committer
	// thread, read only by ComputePercentiles. No atomics required here.
	struct FLatencyRecord
	{
		uint64 Stages[static_cast<int32>(EPipelineStage::Count)] = {};
	};

	// CurrentFrame is the cross-thread staging area: producers call Mark() on
	// the slot for "their" stage; the committer thread snapshots all slots
	// into the ring on CommitFrame(). Per-slot TAtomic with SeqCst on both
	// sides publishes the producer's data writes to the committer. Each slot
	// has exactly one producer thread in practice; the atomics are what makes
	// the cross-thread visibility guarantee explicit at the C++-memory-model
	// level. (UE5's EMemoryOrder enum is just Relaxed/SequentiallyConsistent;
	// SeqCst is the project-wide substitute for release/acquire — see
	// CamSimCamera.h:179-194 for the convention.)
	// Stages[i]: producer thread for stage i  →  committer thread (SeqCst)
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
