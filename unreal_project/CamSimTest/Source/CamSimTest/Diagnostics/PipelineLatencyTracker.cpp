// Copyright CamSim Contributors. All Rights Reserved.

#include "Diagnostics/PipelineLatencyTracker.h"

FPipelineLatencyTracker::FPipelineLatencyTracker(int32 InBufferSize)
	: BufferCapacity(FMath::Max(10, InBufferSize))
{
	Records.SetNum(BufferCapacity);
	FMemory::Memzero(CurrentFrame);
}

void FPipelineLatencyTracker::SetStageTimestamp(EPipelineStage Stage, uint64 Cycles)
{
	CurrentFrame.Stages[static_cast<int32>(Stage)] = Cycles;
}

void FPipelineLatencyTracker::CommitFrame()
{
	const uint32 Idx = WriteIndex.Load(EMemoryOrder::Relaxed);
	Records[Idx % BufferCapacity] = CurrentFrame;
	WriteIndex.Store(Idx + 1, EMemoryOrder::Relaxed);

	const int32 Count = CommittedCount.Load(EMemoryOrder::Relaxed);
	if (Count < BufferCapacity)
	{
		CommittedCount.Store(Count + 1, EMemoryOrder::Relaxed);
	}

	FMemory::Memzero(CurrentFrame);
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
	const int32 Count = CommittedCount.Load(EMemoryOrder::Relaxed);
	if (Count == 0) return Result;

	// Collect deltas
	TArray<float> ReadbackDeltas, SensorDeltas, EncodeDeltas, TotalDeltas;
	ReadbackDeltas.Reserve(Count);
	SensorDeltas.Reserve(Count);
	EncodeDeltas.Reserve(Count);
	TotalDeltas.Reserve(Count);

	const uint32 WIdx = WriteIndex.Load(EMemoryOrder::Relaxed);
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
