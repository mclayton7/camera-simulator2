// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** One frame the game thread wants copied out of the game viewport (ROADMAP 3A). */
struct FFrameGrabRequest
{
	uint64 FrameIndex  = 0;
	uint32 Generation  = 0;           // the slot's capture generation at request time
	int32  TargetIndex = INDEX_NONE;  // grab render target / readback slot
	bool   bInstanceIds = false;      // also run InstanceIdCS and read its buffer back (ground truth, ROADMAP 2.7)
};

/**
 * Grab requests, render-thread only (the game thread reaches it through
 * ENQUEUE_RENDER_COMMAND, so no locking). Each render grabs the newest
 * request: the game thread enqueues it during the tick whose frame is being
 * drawn. Older requests are for frames that never rendered (viewport not
 * drawn); they're dropped and their readback slots time out.
 */
class FFrameGrabRequestQueue
{
public:
	void Push(const FFrameGrabRequest& R) { Requests.Add(R); }

	bool PopLatest(FFrameGrabRequest& Out)
	{
		if (Requests.Num() == 0) return false;
		Out = Requests.Last();
		Requests.Reset();
		return true;
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
	 *  - The copy is issued by the grab extension; the slot's fence is
	 *    trusted only once GrabbedGeneration equals CaptureGeneration (a
	 *    fence signalled by an earlier cycle would otherwise return an old
	 *    frame).
	 *  - Attempt/MaxAttempts: a grab that never happens (viewport not drawn,
	 *    request dropped) times out instead of holding DMAQueued forever.
	 */
	template <typename FIsFenceReady>
	EPollDecision DecidePoll(uint32 GrabbedGeneration, uint32 CaptureGeneration,
	                         FIsFenceReady&& IsFenceReady, uint32 Attempt, uint32 MaxAttempts)
	{
		const bool bCopyIssued = GrabbedGeneration == CaptureGeneration;
		if (bCopyIssued && IsFenceReady())
		{
			return EPollDecision::Consume;
		}
		return Attempt >= MaxAttempts ? EPollDecision::TimedOut : EPollDecision::Wait;
	}

	/**
	 * Whether the poll must also wait for the slot's instance-ID copy (ground
	 * truth, ROADMAP 2.7). Only when the capture asked for IDs and the grab
	 * extension really issued that copy for this generation (IdGrabbed); a pass
	 * that couldn't run leaves IdGrabbed behind, and the frame goes out without
	 * IDs instead of waiting for a copy that never comes.
	 */
	inline bool ShouldWaitForIds(bool bWantIds, uint32 IdGrabbedGeneration, uint32 CaptureGeneration)
	{
		return bWantIds && IdGrabbedGeneration == CaptureGeneration;
	}
}
