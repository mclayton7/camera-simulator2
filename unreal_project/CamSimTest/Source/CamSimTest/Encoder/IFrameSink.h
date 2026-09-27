// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Metadata/KlvBuilder.h"  // FCamSimTelemetry

/** Pixel format carried by FSensorFrame. */
enum class ESensorPixelFormat : uint8 { BGRA8, NV12 };

/**
 * FSensorFrame
 *
 * One sensor frame for the encoder: BGRA8 (legacy CPU path) or NV12
 * (GPU sensor path, ROADMAP 3B). Only the array matching Format is
 * populated; the other stays empty.
 */
struct FSensorFrame
{
	ESensorPixelFormat Format = ESensorPixelFormat::BGRA8;
	TArray<FColor>     Bgra;   // W*H
	TArray<uint8>      Nv12;   // W*H*3/2, BT.709 limited range
	bool IsEmpty() const { return Format == ESensorPixelFormat::NV12 ? Nv12.Num() == 0 : Bgra.Num() == 0; }
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
	 * Encode one sensor frame (BGRA8 or NV12).
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
