// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HttpResultCallback.h"

/**
 * FCamSimSnapshotService
 *
 * Answers GET /snapshot (and /snapshot/sensor) with the next grabbed frame as
 * PNG (ROADMAP 3A/3B). The frame is the sensor graph's NV12 output before
 * encoding, so the bench's reference shots are free of compression. HTTP handlers run on the game thread (the
 * listener is pumped by the core ticker), so every public call is game-thread
 * only. PNG encoding runs on the thread pool; Tick() delivers finished
 * encodes and times out requests that never got a frame.
 */
class FCamSimSnapshotService
{
public:
	FCamSimSnapshotService() = default;
	FCamSimSnapshotService(const FCamSimSnapshotService&) = delete;
	FCamSimSnapshotService& operator=(const FCamSimSnapshotService&) = delete;

	/** Answers 503 to every request still waiting or encoding, so each callback completes once. */
	~FCamSimSnapshotService();

	/** Queue a request; answered by a Tick() after the next OfferFrame(), or 503 after TimeoutSec. */
	void Request(FHttpResultCallback OnComplete);

	/** True while at least one request is waiting for a frame. */
	bool WantsFrame() const { return Pending.Num() > 0; }

	/** Hand over a finished frame; copies and encodes only when requests are waiting. */
	void OfferFrame(const TArray<FColor>& Pixels, int32 Width, int32 Height);

	/** Deliver finished PNGs and answer 503 to requests older than TimeoutSec. */
	void Tick(double NowSec);

	double TimeoutSec = 5.0;

private:
	struct FPendingRequest
	{
		FHttpResultCallback OnComplete;
		double RequestedAtSec = 0.0;
	};

	/** Written by the encode task, read by Tick() once bDone is set. */
	struct FEncodeJob
	{
		TArray<uint8>  Png;
		TAtomic<bool>  bDone { false };
	};

	struct FInFlight
	{
		TSharedRef<FEncodeJob, ESPMode::ThreadSafe> Job;
		TArray<FPendingRequest> Waiting;
	};

	TArray<FPendingRequest> Pending;
	TArray<FInFlight>       InFlight;
};
