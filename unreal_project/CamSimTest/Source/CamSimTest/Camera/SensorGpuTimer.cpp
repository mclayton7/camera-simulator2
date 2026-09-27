// Copyright CamSim Contributors. All Rights Reserved.

#include "Camera/SensorGpuTimer.h"
#include "RenderGraphBuilder.h"
#include "RHICommandList.h"
#include "DynamicRHI.h"

void FSensorGpuTimer::Harvest()
{
	for (FPair& P : Pairs)
	{
		if (!P.bPending) continue;
		uint64 StartUs = 0, StopUs = 0;
		if (RHIGetRenderQueryResult(P.Start, StartUs, /*bWait=*/false) && RHIGetRenderQueryResult(P.Stop, StopUs, /*bWait=*/false))
		{
			P.bPending = false;
			if (StopUs >= StartUs) LatestMs.Store(static_cast<float>(StopUs - StartUs) / 1000.0f, EMemoryOrder::Relaxed);
		}
	}
}

void FSensorGpuTimer::Begin(FRDGBuilder& GraphBuilder)
{
	if (!GSupportsTimestampRenderQueries) return;
	Current = (Current + 1) % NumFrames;
	GraphBuilder.AddPass(RDG_EVENT_NAME("CamSimSensorTimerBegin"), ERDGPassFlags::None | ERDGPassFlags::NeverCull,
		[this, Index = Current](FRHICommandListImmediate& RHICmdList)
	{
		Harvest();
		FPair& P = Pairs[Index];
		if (P.bPending) return;  // still unread after NumFrames: skip this measurement
		if (!P.Start)
		{
			P.Start = RHICreateRenderQuery(RQT_AbsoluteTime);
			P.Stop  = RHICreateRenderQuery(RQT_AbsoluteTime);
		}
		RHICmdList.EndRenderQuery(P.Start);
	});
}

void FSensorGpuTimer::End(FRDGBuilder& GraphBuilder)
{
	if (!GSupportsTimestampRenderQueries) return;
	GraphBuilder.AddPass(RDG_EVENT_NAME("CamSimSensorTimerEnd"), ERDGPassFlags::None | ERDGPassFlags::NeverCull,
		[this, Index = Current](FRHICommandListImmediate& RHICmdList)
	{
		FPair& P = Pairs[Index];
		if (!P.Stop || P.bPending) return;
		RHICmdList.EndRenderQuery(P.Stop);
		P.bPending = true;
	});
}
