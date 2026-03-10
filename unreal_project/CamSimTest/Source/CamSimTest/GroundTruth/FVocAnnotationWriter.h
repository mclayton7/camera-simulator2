// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GroundTruth/IAnnotationWriter.h"

/**
 * FVocAnnotationWriter
 *
 * Writes one Pascal VOC XML file per frame to <OutputDir>/annotations/.
 * Follows VOC2007 schema with bndbox in pixel coordinates.
 *
 * Output: <OutputDir>/annotations/frame_NNNNNNNN.xml
 */
class FVocAnnotationWriter : public IAnnotationWriter
{
public:
	// IAnnotationWriter interface
	virtual bool Open(const FString& OutputDir) override;
	virtual void WriteFrame(const TArray<FEntityAnnotationData>& Entities,
	                        const FCamSimTelemetry& Telemetry,
	                        uint64 FrameIdx) override;
	virtual void Close() override;
	virtual bool IsOpen() const override { return bIsOpen; }

	/** Set the image dimensions so VOC <size> is correct. */
	void SetImageSize(int32 InWidth, int32 InHeight) { ImageWidth = InWidth; ImageHeight = InHeight; }

private:
	bool    bIsOpen      = false;
	FString AnnotationsDir;
	int32   ImageWidth   = 1920;
	int32   ImageHeight  = 1080;
};
