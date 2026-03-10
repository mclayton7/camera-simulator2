// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GroundTruth/AnnotationTypes.h"

// FCamSimTelemetry is defined in Metadata/KlvBuilder.h — forward-declare here to
// avoid pulling the full KLV/FFmpeg headers into every translation unit that
// includes IAnnotationWriter.h.  Concrete writer .cpp files include KlvBuilder.h.
struct FCamSimTelemetry;

/**
 * IAnnotationWriter
 *
 * Pure-virtual interface for per-frame ground-truth annotation output.
 * Implementations: FCocoAnnotationWriter (17G), FVocAnnotationWriter (17H).
 * Future: FSemanticSegWriter (17B), FInstanceSegWriter (17C).
 *
 * All Write() calls happen on the background task thread, serialised by
 * bEncoderBusy in ACamSimCamera. No additional locking is needed.
 */
class IAnnotationWriter
{
public:
	virtual ~IAnnotationWriter() = default;

	/** Create output directory and open any persistent file handles. */
	virtual bool Open(const FString& OutputDir) = 0;

	/** Write one frame's annotations to the output. Task thread only. */
	virtual void WriteFrame(const TArray<FEntityAnnotationData>& Entities,
	                        const FCamSimTelemetry& Telemetry,
	                        uint64 FrameIdx) = 0;

	/** Flush and close output. */
	virtual void Close() = 0;

	virtual bool IsOpen() const = 0;
};
