// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/Event.h"
#include "Containers/SpscQueue.h"
#include "Metadata/KlvBuilder.h"

class IFrameSink;

/**
 * FProcessedFrame — payload deposited by the sensor task thread into
 * the SPSC queue, consumed by the persistent encoder thread.
 */
struct FProcessedFrame
{
	TArray<FColor>        Pixels;
	FCamSimTelemetry      Telemetry;
	uint64                FrameIndex = 0;
};

/**
 * FEncoderThread — persistent FRunnable that drains a lock-free SPSC queue
 * and feeds each frame to FVideoEncoder::EncodeFrame().
 *
 * Decouples the sensor post-process task (producer) from FFmpeg encode
 * (consumer), removing encode latency from the critical path.
 *
 * Thread safety: single producer (sensor async task), single consumer
 * (this thread). The queue is lock-free; the wake event is edge-triggered.
 */
class FEncoderThread : public FRunnable
{
public:
	explicit FEncoderThread(IFrameSink* InEncoder, float OutputFps = 30.0f);
	virtual ~FEncoderThread() override;

	/** Enqueue a processed frame for encoding. Non-blocking. */
	void Enqueue(FProcessedFrame&& Frame);

	/** Start the background thread. Call once after construction. */
	void Start();

	/** Signal the thread to stop and wait for exit. */
	void Stop();

	/** True if the queue is non-empty (encoder has work pending). */
	bool HasPendingFrames() const { return !Queue.IsEmpty(); }

	// FRunnable interface
	virtual uint32 Run() override;

private:
	IFrameSink*                    Encoder = nullptr;
	TSpscQueue<FProcessedFrame>    Queue;
	FEventRef                      WakeEvent { EEventMode::AutoReset };
	TAtomic<bool>                  bRunning  { false };
	FRunnableThread*               Thread    = nullptr;
	double                         FrameIntervalSec = 1.0 / 30.0;  // output pacing interval
	double                         LastSendTimeSec  = 0.0;          // wall-clock of last encode
};
