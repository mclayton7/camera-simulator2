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
 * Owns the annotation writer (COCO) and depth map writer.
 * Lifetime: owned by UCamSimSubsystem::FSubsystemImpl via TUniquePtr.
 *
 * Thread model:
 *   The game thread builds each frame's entity snapshot into that frame's
 *   readback-ring slot; the background task passes it here. No state is
 *   shared between frames.
 *   WriteAnnotationFrame()     — background task (UCamSimCaptureComponent::SubmitFrameToEncoder)
 *   WriteDepthFrame()          — its own background task (at most MaxDepthWritesInFlight)
 *
 * Annotation writes are serialised by bSensorBusy in UCamSimCaptureComponent;
 * depth writes touch only their own file.
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
	/** Every N-th frame is annotated (fixed at Open()). */
	int32 GetAnnotationIntervalFrames() const { return AnnotationIntervalFrames; }

	/**
	 * Write annotation records for the current frame.
	 * Called on the background task thread (UCamSimCaptureComponent::SubmitFrameToEncoder).
	 * bForce (a control API capture frame, ROADMAP 2.4) annotates it whatever the interval and
	 * returns its record (the camsim_coco.jsonl line without the newline); otherwise returns "".
	 * Entities is this frame's own snapshot (built on the game thread into the
	 * readback-ring slot at capture time); no state is retained across calls.
	 * Ids (optional): the frame's output-space instance-ID image. When valid and the
	 * size of the capture, boxes/masks are measured from it; otherwise (null, invalid
	 * or wrong size) the projected boxes are written (mask_source "projection").
	 */
	FString WriteAnnotationFrame(TArray<FEntityAnnotationData> Entities, const FInstanceIdImage* Ids,
	                             const FCamSimTelemetry& Telemetry, uint64 FrameIdx, bool bForce = false);

	/** Projection-only form (no ID image): boxes come from the entity snapshot. */
	void WriteAnnotationFrame(const TArray<FEntityAnnotationData>& Entities,
	                          const FCamSimTelemetry& Telemetry, uint64 FrameIdx)
	{
		WriteAnnotationFrame(Entities, nullptr, Telemetry, FrameIdx);
	}

	/**
	 * Write a 16-bit PNG depth map for the current frame.
	 * Called on background task thread after color readback dispatch.
	 */
	void WriteDepthFrame(const TArray<float>& DepthMetres,
	                     int32 Width, int32 Height, uint64 FrameIdx, bool bForce = false);

	/**
	 * ROADMAP 4D: write the frame's semantic class image (<out>/semantic/semantic_NNNNNNNN.png, 8-bit grey class ids)
	 * when ml_training.semantic is on and the frame is on the interval (or bForce). Pairs: two pixels per word, the
	 * class in the low 8 bits of each 16-bit half. Its own task thread call (as depth).
	 */
	void WriteSemanticFrame(const TArray<uint32>& Pairs, int32 Width, int32 Height, uint64 FrameIdx, bool bForce = false);

	/** The 8-bit grey PNG of a class-pair buffer; empty on a size mismatch. Any thread once ImageWrapper is loaded. */
	static TArray64<uint8> EncodeSemanticPng(const TArray<uint32>& Pairs, int32 Width, int32 Height);

	/** Depth quantization ceiling (ml_training.depth_far_plane_m). */
	float GetDepthFarPlaneM() const { return DepthFarPlaneM; }

private:
	const FCamSimConfig& Config;
	bool  bEnabled    = false;
	bool  bIsOpen     = false;
	// Cached in Open(): the task thread never reads Config.
	int32 AnnotationIntervalFrames = 1;
	int32 MinVisiblePixels = 1;
	bool  bSegmentation = false;
	FIntPoint CaptureSize = FIntPoint::ZeroValue;
	float DepthFarPlaneM = 5000.0f;
	bool  bSemanticFiles = false;
	FString SemanticDir;

	TArray<TUniquePtr<IAnnotationWriter>> Writers;
	TUniquePtr<FDepthMapWriter>           DepthWriter;
};
