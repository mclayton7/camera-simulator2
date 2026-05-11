// Copyright CamSim Contributors. All Rights Reserved.

#include "Diagnostics/PipelineLatencyTracker.h"

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

int32 FPipelineLatencyTracker::GetCommittedCount() const
{
	return CommittedCount.Load(EMemoryOrder::Relaxed);
}

float FPipelineLatencyTracker::CyclesToUs(uint64 Delta)
{
	return static_cast<float>(FPlatformTime::ToSeconds64(Delta) * 1000000.0);
}

float FPipelineLatencyTracker::PercentileFromSorted(const TArray<float>& Sorted, float Pct)
{
	if (Sorted.Num() == 0) return 0.0f;
	const int32 Idx = FMath::Clamp(
		static_cast<int32>(Pct * Sorted.Num()),
		0, Sorted.Num() - 1);
	return Sorted[Idx];
}

FPipelineLatencyTracker::FLatencyPercentiles FPipelineLatencyTracker::ComputePercentiles() const
{
	FLatencyPercentiles Result;
	const int32 Count = CommittedCount.Load(EMemoryOrder::SequentiallyConsistent);
	if (Count == 0) return Result;

	// Collect deltas
	TArray<float> ReadbackDeltas, SensorDeltas, EncodeDeltas, TotalDeltas;
	ReadbackDeltas.Reserve(Count);
	SensorDeltas.Reserve(Count);
	EncodeDeltas.Reserve(Count);
	TotalDeltas.Reserve(Count);

	const uint64 WIdx = WriteIndex.Load(EMemoryOrder::SequentiallyConsistent);
	const int32 StartIdx = (Count < BufferCapacity) ? 0 : static_cast<int32>(WIdx % BufferCapacity);

	for (int32 i = 0; i < Count; ++i)
	{
		const FLatencyRecord& R = Records[(StartIdx + i) % BufferCapacity];

		const uint64 Readback = R.Stages[static_cast<int32>(EPipelineStage::ReadbackComplete)];
		const uint64 RbIssue  = R.Stages[static_cast<int32>(EPipelineStage::ReadbackIssue)];
		const uint64 SensorS  = R.Stages[static_cast<int32>(EPipelineStage::SensorStart)];
		const uint64 SensorE  = R.Stages[static_cast<int32>(EPipelineStage::SensorEnd)];
		const uint64 EncComp  = R.Stages[static_cast<int32>(EPipelineStage::EncodeComplete)];
		const uint64 CigiDq   = R.Stages[static_cast<int32>(EPipelineStage::CigiDequeue)];

		if (Readback > RbIssue)  ReadbackDeltas.Add(CyclesToUs(Readback - RbIssue));
		if (SensorE > SensorS)   SensorDeltas.Add(CyclesToUs(SensorE - SensorS));
		if (EncComp > SensorE)   EncodeDeltas.Add(CyclesToUs(EncComp - SensorE));
		if (EncComp > CigiDq)    TotalDeltas.Add(CyclesToUs(EncComp - CigiDq));
	}

	ReadbackDeltas.Sort();
	SensorDeltas.Sort();
	EncodeDeltas.Sort();
	TotalDeltas.Sort();

	const float Pcts[] = { 0.50f, 0.95f, 0.99f };
	for (int32 p = 0; p < 3; ++p)
	{
		Result.ReadbackUs[p] = PercentileFromSorted(ReadbackDeltas, Pcts[p]);
		Result.SensorUs[p]   = PercentileFromSorted(SensorDeltas, Pcts[p]);
		Result.EncodeUs[p]   = PercentileFromSorted(EncodeDeltas, Pcts[p]);
		Result.TotalUs[p]    = PercentileFromSorted(TotalDeltas, Pcts[p]);
	}

	return Result;
}
