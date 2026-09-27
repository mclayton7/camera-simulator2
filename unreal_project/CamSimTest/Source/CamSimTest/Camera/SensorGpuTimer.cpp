// Copyright CamSim Contributors. All Rights Reserved.

#include "Camera/SensorGpuTimer.h"

FCamSimSensorGpuStat GPUStat_CamSimSensor;

FCamSimSensorGpuStat::EOnTimingResultsAction FCamSimSensorGpuStat::OnTimingResults(
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
