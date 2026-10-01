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
/** One GPU profiler stat whose newest busy time is kept for the bench (CamSimSensor, CamSimThermal). */
struct FCamSimGpuStat final : public UE::RHI::GPUProfiler::FGPUStat
{
	FCamSimGpuStat(const TCHAR* InName, const TCHAR* InDisplayName) : FGPUStat(InName, InDisplayName, nullptr) {}
	virtual EOnTimingResultsAction OnTimingResults(UE::RHI::GPUProfiler::FQueue Queue, double BusyMs, double IdleMs, double WaitMs) override;
	/** Newest frame's busy time in ms; -1 until the first result. Any thread. */
	TAtomic<float> LatestMs { -1.0f };  // writer: GPU profiler thread (relaxed)
};

/** RDG_EVENT_SCOPE_STAT(GraphBuilder, CamSimSensor, ...) / (…, CamSimThermal, ...) resolve to these (HAS_GPU_STATS builds). */
extern FCamSimGpuStat GPUStat_CamSimSensor;
extern FCamSimGpuStat GPUStat_CamSimThermal;

/**
 * Reads the sensor graph's GPU time. The graph is wrapped in
 * RDG_EVENT_SCOPE_STAT(GraphBuilder, CamSimSensor, ...) by the grab extension.
 */
class FSensorGpuTimer
{
public:
	/** Newest completed measurement in ms; -1 when GPU stats are compiled out or none finished yet. Any thread. */
	float GetLatestMs() const;

	/** ThermalCS's newest GPU time (ROADMAP 4A); -1 as GetLatestMs. Stale while the pass does not run: callers gate it (UCamSimCaptureComponent::GetThermalGpuMs). */
	float GetThermalLatestMs() const;
};
