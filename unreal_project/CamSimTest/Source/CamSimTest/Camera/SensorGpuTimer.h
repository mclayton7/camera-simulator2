// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RHIResources.h"
#include "RenderGraphDefinitions.h"

class FRHICommandListImmediate;

/**
 * GPU time of the sensor graph from timestamp queries (ROADMAP 3B bench).
 * Begin/End add RDG passes around the graph; their lambdas run on the render
 * thread. Render thread only, except GetLatestMs.
 */
class FSensorGpuTimer
{
public:
	void Begin(FRDGBuilder& GraphBuilder);
	void End(FRDGBuilder& GraphBuilder);
	/** Newest completed measurement in ms; -1 when timestamps are unsupported or none finished yet. Any thread. */
	float GetLatestMs() const { return LatestMs.Load(EMemoryOrder::Relaxed); }

private:
	static constexpr int32 NumFrames = 4;
	struct FPair { FRenderQueryRHIRef Start, Stop; bool bPending = false; };
	FPair Pairs[NumFrames];      // render thread
	int32 Current = 0;           // render thread
	TAtomic<float> LatestMs { -1.0f };  // writer: render → readers: any (relaxed)
	void Harvest();
};
