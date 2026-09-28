// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Metadata/KlvBuilder.h"  // FCamSimTelemetry

/**
 * FSensorFrame
 *
 * One sensor frame for the encoder: NV12 (GPU sensor graph, ROADMAP 3B),
 * BT.709 limited range.
 */
struct FSensorFrame
{
	TArray<uint8> Nv12;   // W*H*3/2, BT.709 limited range
	bool IsEmpty() const { return Nv12.Num() == 0; }
};

/**
 * IFrameSink
 *
 * Pure-virtual interface for encoded video output destinations.
 * FVideoEncoder implements per-view H.264/MPEG-TS output and
 * FMultiViewFrameSink composes one or more FVideoEncoder instances.
 *
 * Having this interface enables:
 *   - FNullFrameSink  : no-op sink for unit tests
 *   - FDiskFrameSink  : dump raw frames to disk for debug
 *   - FMultiplexFrameSink : fan-out to multiple sinks
 *   - Encoder watchdog in UCamSimSubsystem without depending on FFmpeg types
 */
class IFrameSink
{
public:
	virtual ~IFrameSink() = default;

	/** Open output pipeline. Returns false on failure. */
	virtual bool Open() = 0;

	/**
	 * Encode one sensor frame (NV12).
	 * Called from a single background task thread (serialised by bEncoderBusy).
	 */
	virtual void EncodeFrame(const FSensorFrame& Frame,
	                         const FCamSimTelemetry& Telemetry,
	                         uint64 FrameIdx) = 0;

	/** Flush, finalise, and close the output pipeline. */
	virtual void Close() = 0;

	/** Returns true if the pipeline was successfully opened. */
	virtual bool IsOpen() const = 0;

	/**
	 * Returns the number of frames that have been successfully written
	 * since the last Open() call.  Thread-safe (atomic read).
	 * Used by the encoder watchdog to detect silent stream death.
	 */
	virtual uint64 GetSuccessfulFrameCount() const = 0;
};
