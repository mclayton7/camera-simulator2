// Copyright CamSim Contributors. All Rights Reserved.

#include "Encoder/EncoderThread.h"
#include "Encoder/IFrameSink.h"
#include "CamSimTest.h"

// -------------------------------------------------------------------------
// Construction / Destruction
// -------------------------------------------------------------------------

FEncoderThread::FEncoderThread(IFrameSink* InEncoder, float OutputFps)
	: Encoder(InEncoder)
	, FrameIntervalSec(OutputFps > 0.0f ? 1.0 / static_cast<double>(OutputFps) : 1.0 / 30.0)
{
}

FEncoderThread::~FEncoderThread()
{
	Stop();
}

// -------------------------------------------------------------------------
// Start / Stop
// -------------------------------------------------------------------------

void FEncoderThread::Start()
{
	if (Thread.IsValid()) return;
	bRunning = true;
	Thread.Reset(FRunnableThread::Create(this, TEXT("CamSimEncoder"), 0, TPri_Normal));
	UE_LOG(LogCamSim, Log, TEXT("FEncoderThread: started"));
}

void FEncoderThread::Stop()
{
	if (!Thread.IsValid()) return;
	bRunning = false;
	WakeEvent->Trigger();
	Thread->WaitForCompletion();
	Thread.Reset();
	UE_LOG(LogCamSim, Log, TEXT("FEncoderThread: stopped"));
}

// -------------------------------------------------------------------------
// Enqueue — called from sensor async task (producer)
// -------------------------------------------------------------------------

void FEncoderThread::Enqueue(FProcessedFrame&& Frame)
{
	// Returns false on overflow — we drop the newest frame and bump the queue's
	// internal drop counter, so live video stays close to real-time instead of
	// accumulating stale backlog when the encoder can't keep up.
	if (!Queue.Enqueue(MoveTemp(Frame)))
	{
		UE_LOG(LogCamSim, Verbose,
			TEXT("FEncoderThread: queue saturated (cap=%d); dropped frame (total=%llu)"),
			Queue.GetCapacity(), (uint64)Queue.GetDropCount());
	}
	WakeEvent->Trigger();
}

// -------------------------------------------------------------------------
// Run — persistent consumer thread
// -------------------------------------------------------------------------

uint32 FEncoderThread::Run()
{
	while (bRunning)
	{
		FProcessedFrame Frame;
		while (Queue.Dequeue(Frame))
		{
			// Output pacing — wait until at least FrameIntervalSec has elapsed
			// since the last send.  This prevents UDP packet bunching that causes
			// VLC stutter even when PTS is monotonically correct.
			if (LastSendTimeSec > 0.0)
			{
				const double NowSec = FPlatformTime::Seconds();
				const double ElapsedSec = NowSec - LastSendTimeSec;
				if (ElapsedSec < FrameIntervalSec)
				{
					const double SleepMs = (FrameIntervalSec - ElapsedSec) * 1000.0;
					FPlatformProcess::SleepNoStats(static_cast<float>(SleepMs * 0.001));
				}
			}

			if (Encoder && Encoder->IsOpen())
			{
				Encoder->EncodeFrame(Frame.Pixels, Frame.Telemetry, Frame.FrameIndex);
			}
			LastSendTimeSec = FPlatformTime::Seconds();

			// Phase 28G: mark encode complete and commit latency record
			if (LatencyTracker)
			{
				LatencyTracker->Mark(EPipelineStage::EncodeComplete);
				LatencyTracker->CommitFrame();
			}
		}

		// Sleep until next Enqueue() or Stop()
		if (bRunning)
		{
			WakeEvent->Wait(50);  // 50ms timeout — poll safety net
		}
	}

	// Drain remaining frames on shutdown
	FProcessedFrame Remaining;
	while (Queue.Dequeue(Remaining))
	{
		if (Encoder && Encoder->IsOpen())
		{
			Encoder->EncodeFrame(Remaining.Pixels, Remaining.Telemetry, Remaining.FrameIndex);
		}
	}

	return 0;
}
