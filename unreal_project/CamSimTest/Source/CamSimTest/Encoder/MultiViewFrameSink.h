// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Config/CamSimConfig.h"
#include "Encoder/IFrameSink.h"
#include "Encoder/VideoEncoder.h"

/**
 * FMultiViewFrameSink
 *
 * Wraps one or more FVideoEncoder instances and fans out each captured frame
 * to multiple output routes. Per-view HFOV can be narrowed using a center crop
 * (digital zoom) so each stream can carry independent FOV metadata.
 *
 * Also supports optional JSONL ground-truth sidecar output.
 */
class FMultiViewFrameSink : public IFrameSink
{
public:
	explicit FMultiViewFrameSink(const FCamSimConfig& InConfig);
	virtual ~FMultiViewFrameSink() override;

	// IFrameSink interface
	virtual bool Open() override;
	virtual void EncodeFrame(const TArray<FColor>& PixelData,
	                         const FCamSimTelemetry& Telemetry,
	                         uint64 FrameIdx) override;
	virtual void Close() override;
	virtual bool IsOpen() const override { return bIsOpen; }
	virtual uint64 GetSuccessfulFrameCount() const override { return (uint64)SuccessfulFrameCount; }

private:
	struct FViewRuntime
	{
		int32 ViewId = 0;
		FCamSimConfig ViewConfig;
		float OutputHFovDeg = 0.0f;
		FString RouteLabel;
		TUniquePtr<FVideoEncoder> Encoder;

		// Phase 2: per-view zoom scratch — sized lazily on first zoomed frame
		// and reused thereafter. EncodeFrame writes into this instead of
		// allocating a fresh TArray<FColor> per view per frame.
		TArray<FColor> ZoomedScratch;
	};

	const FCamSimConfig& Config;
	bool bIsOpen = false;
	TAtomic<uint64> SuccessfulFrameCount { 0 };
	TArray<FViewRuntime> Views;

	bool bGroundTruthEnabled = false;
	FString GroundTruthPath;
	int32 GroundTruthIntervalFrames = 1;

	// Phase 2: persistent JSONL sidecar handle. Opened once in Open() and
	// closed in Close(); WriteGroundTruthLine appends through this handle
	// instead of FFileHelper::SaveStringToFile which would open/seek/write/
	// close the OS file descriptor on every record (30 syscalls/sec at 30 fps).
	class IFileHandle* GroundTruthHandle_ = nullptr;

	void BuildViewRuntimes();
	void WriteGroundTruthLine(const FCamSimTelemetry& Telemetry, uint64 FrameIdx, int32 EncodedViewCount) const;
	static void ApplyDigitalZoom(const TArray<FColor>& SourcePixels,
	                             int32 Width, int32 Height,
	                             float SourceHFovDeg, float TargetHFovDeg,
	                             TArray<FColor>& OutPixels);
};
