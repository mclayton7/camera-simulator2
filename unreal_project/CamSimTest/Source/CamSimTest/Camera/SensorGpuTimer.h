// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GPUProfiler.h"
#include "RenderGraphDefinitions.h"

/**
 * GPU time of the sensor graph (ROADMAP 3B bench), from UE's GPU profiler.
 *
 * Timestamp render queries can't do this on Metal: MetalRHI resolves
 * RQT_AbsoluteTime to the command buffer's GPUEndTime truncated to whole
 * seconds, so two queries around the graph read 0 or 1000 ms. The GPU
 * profiler times breadcrumb scopes with Metal counter samples (and with
 * timestamps elsewhere) and reports each stat's busy time per frame through
 * FGPUStat::OnTimingResults, which this stat captures.
 */
struct FCamSimSensorGpuStat final : public UE::RHI::GPUProfiler::FGPUStat
{
	FCamSimSensorGpuStat() : FGPUStat(TEXT("CamSimSensor"), TEXT("CamSim sensor"), nullptr) {}
	virtual EOnTimingResultsAction OnTimingResults(UE::RHI::GPUProfiler::FQueue Queue, double BusyMs, double IdleMs, double WaitMs) override;
	/** Newest frame's busy time in ms; -1 until the first result. Any thread. */
	TAtomic<float> LatestMs { -1.0f };  // writer: GPU profiler thread (relaxed)
};

/** RDG_EVENT_SCOPE_STAT(GraphBuilder, CamSimSensor, ...) resolves to this (HAS_GPU_STATS builds). */
extern FCamSimSensorGpuStat GPUStat_CamSimSensor;

/**
 * Reads the sensor graph's GPU time. The graph is wrapped in
 * RDG_EVENT_SCOPE_STAT(GraphBuilder, CamSimSensor, ...) by the grab extension.
 */
class FSensorGpuTimer
{
public:
	/** Newest completed measurement in ms; -1 when GPU stats are compiled out or none finished yet. Any thread. */
	float GetLatestMs() const;
};
