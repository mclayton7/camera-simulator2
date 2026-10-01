// Copyright CamSim Contributors. All Rights Reserved.

#include "Camera/SensorGpuTimer.h"

FCamSimGpuStat GPUStat_CamSimSensor(TEXT("CamSimSensor"), TEXT("CamSim sensor"));
FCamSimGpuStat GPUStat_CamSimThermal(TEXT("CamSimThermal"), TEXT("CamSim thermal"));

FCamSimGpuStat::EOnTimingResultsAction FCamSimGpuStat::OnTimingResults(
	UE::RHI::GPUProfiler::FQueue Queue, double BusyMs, double /*IdleMs*/, double /*WaitMs*/)
{
	if (Queue.Type == UE::RHI::GPUProfiler::FQueue::EType::Graphics && Queue.Index == 0)
	{
		LatestMs.Store(static_cast<float>(BusyMs), EMemoryOrder::Relaxed);
	}
	return EOnTimingResultsAction::Keep;
}

float FSensorGpuTimer::GetLatestMs() const
{
#if HAS_GPU_STATS
	return GPUStat_CamSimSensor.LatestMs.Load(EMemoryOrder::Relaxed);
#else
	return -1.0f;
#endif
}

float FSensorGpuTimer::GetThermalLatestMs() const
{
#if HAS_GPU_STATS
	return GPUStat_CamSimThermal.LatestMs.Load(EMemoryOrder::Relaxed);
#else
	return -1.0f;
#endif
}
