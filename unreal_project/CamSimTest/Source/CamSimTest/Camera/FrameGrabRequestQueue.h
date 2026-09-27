// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** One frame the game thread wants copied out of the game viewport (ROADMAP 3A). */
struct FFrameGrabRequest
{
	uint64 FrameIndex  = 0;
	uint32 Generation  = 0;           // UCamSimCaptureComponent::PollGeneration at request time
	int32  TargetIndex = INDEX_NONE;  // grab render target / readback slot
};

/**
 * FIFO of grab requests. Render-thread only: the game thread reaches it through
 * ENQUEUE_RENDER_COMMAND, so no locking. Requests from an older generation were
 * abandoned by the game thread and are dropped rather than grabbed.
 */
class FFrameGrabRequestQueue
{
public:
	void Push(const FFrameGrabRequest& R) { Requests.Add(R); }

	bool PopCurrent(uint32 CurrentGeneration, FFrameGrabRequest& Out)
	{
		while (Requests.Num() > 0)
		{
			const FFrameGrabRequest Front = Requests[0];
			Requests.RemoveAt(0, EAllowShrinking::No);
			if (Front.Generation >= CurrentGeneration)
			{
				Out = Front;
				return true;
			}
		}
		return false;
	}

	int32 Num() const { return Requests.Num(); }

private:
	TArray<FFrameGrabRequest> Requests;
};
