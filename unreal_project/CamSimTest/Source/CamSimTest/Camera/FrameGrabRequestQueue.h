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

namespace CamSimReadback
{
	enum class EPollDecision : uint8 { Wait, Consume, TimedOut };

	/**
	 * One render-thread poll of an in-flight readback (ROADMAP 3A).
	 *  - bNeedsGrab: primary view, where the copy is issued by the grab
	 *    extension; the slot's fence is trusted only once GrabbedGeneration
	 *    equals CaptureGeneration (a fence signalled by an earlier cycle would
	 *    otherwise return an old frame).
	 *  - Attempt/MaxAttempts: a grab that never happens (viewport not drawn,
	 *    request dropped) times out instead of holding DMAQueued forever.
	 */
	template <typename FIsFenceReady>
	EPollDecision DecidePoll(bool bNeedsGrab, uint32 GrabbedGeneration, uint32 CaptureGeneration,
	                         FIsFenceReady&& IsFenceReady, uint32 Attempt, uint32 MaxAttempts)
	{
		const bool bCopyIssued = !bNeedsGrab || GrabbedGeneration == CaptureGeneration;
		if (bCopyIssued && IsFenceReady())
		{
			return EPollDecision::Consume;
		}
		return Attempt >= MaxAttempts ? EPollDecision::TimedOut : EPollDecision::Wait;
	}
}
