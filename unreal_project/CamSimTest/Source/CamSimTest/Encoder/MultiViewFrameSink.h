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
 */
class FMultiViewFrameSink : public IFrameSink
{
public:
	explicit FMultiViewFrameSink(const FCamSimConfig& InConfig);
	virtual ~FMultiViewFrameSink() override;

	// IFrameSink interface
	virtual bool Open() override;
	virtual void EncodeFrame(const FSensorFrame& Frame,
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
		// allocating a fresh TArray<FColor>/TArray<uint8> per view per frame.
		FSensorFrame ZoomedScratch;
	};

	const FCamSimConfig& Config;
	bool bIsOpen = false;
	TAtomic<uint64> SuccessfulFrameCount { 0 };
	TArray<FViewRuntime> Views;

	void BuildViewRuntimes();
	/** Crop/nearest-neighbour digital zoom on an NV12 frame: applied to the Y
	 *  plane at full resolution and the UV plane at half resolution. */
	static void ApplyDigitalZoomNv12(const TArray<uint8>& SourceNv12,
	                             int32 Width, int32 Height,
	                             float SourceHFovDeg, float TargetHFovDeg,
	                             TArray<uint8>& OutNv12);
};
