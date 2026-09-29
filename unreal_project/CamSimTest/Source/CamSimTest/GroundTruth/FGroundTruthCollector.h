// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GroundTruth/AnnotationTypes.h"

class IAnnotationWriter;
class FDepthMapWriter;
struct FCamSimConfig;
struct FCamSimTelemetry;

/**
 * FGroundTruthCollector
 *
 * Orchestrator for ML training data output.
 * Owns the annotation writers (COCO, VOC) and depth map writer.
 * Lifetime: owned by UCamSimSubsystem::FSubsystemImpl via TUniquePtr.
 *
 * Thread model:
 *   The game thread builds each frame's entity snapshot into that frame's
 *   readback-ring slot; the background task passes it here. No state is
 *   shared between frames.
 *   WriteAnnotationFrame()     — called on background task thread (inside EncodeFrame)
 *   WriteDepthFrame()          — called on background task thread (after color task)
 *
 * Both task-thread calls are serialised by bEncoderBusy in ACamSimCamera,
 * so no additional locking is needed.
 */
class FGroundTruthCollector
{
public:
	explicit FGroundTruthCollector(const FCamSimConfig& Config);
	~FGroundTruthCollector();

	bool Open();
	void Close();
	bool IsOpen() const { return bIsOpen; }
	bool IsEnabled() const { return bEnabled; }

	/**
	 * Write annotation records for the current frame.
	 * Called on background task thread inside FMultiViewFrameSink::EncodeFrame().
	 * Entities is this frame's own snapshot (built on the game thread into the
	 * readback-ring slot at capture time); no state is retained across calls.
	 */
	void WriteAnnotationFrame(const TArray<FEntityAnnotationData>& Entities,
	                          const FCamSimTelemetry& Telemetry, uint64 FrameIdx);

	/**
	 * Write a 16-bit PNG depth map for the current frame.
	 * Called on background task thread after color readback dispatch.
	 */
	void WriteDepthFrame(const TArray<float>& DepthMetres,
	                     int32 Width, int32 Height, uint64 FrameIdx);

private:
	const FCamSimConfig& Config;
	bool  bEnabled    = false;
	bool  bIsOpen     = false;
	int32 AnnotationIntervalFrames = 1;

	TArray<TUniquePtr<IAnnotationWriter>> Writers;
	TUniquePtr<FDepthMapWriter>           DepthWriter;
};
