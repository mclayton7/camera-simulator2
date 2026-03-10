// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GroundTruth/IAnnotationWriter.h"

/**
 * FCocoAnnotationWriter
 *
 * Writes per-frame annotation records to a streaming JSONL file.
 * Each line is a self-contained JSON object with COCO-compatible fields.
 * A companion script (scripts/coco_finalize.py) converts JSONL to full COCO JSON.
 *
 * Output: <OutputDir>/camsim_coco.jsonl
 * Format per line:
 *   {"frame_id":N,"timestamp_us":T,"image":{"width":W,"height":H,"file_name":"..."},
 *    "annotations":[{"entity_id":E,"category":{"id":C,"name":"..."},
 *                    "bbox":[x,y,w,h],"area":A,"iscrowd":0},...]}
 */
class FCocoAnnotationWriter : public IAnnotationWriter
{
public:
	// IAnnotationWriter interface
	virtual bool Open(const FString& OutputDir) override;
	virtual void WriteFrame(const TArray<FEntityAnnotationData>& Entities,
	                        const FCamSimTelemetry& Telemetry,
	                        uint64 FrameIdx) override;
	virtual void Close() override;
	virtual bool IsOpen() const override { return bIsOpen; }

private:
	bool       bIsOpen    = false;
	FString    JsonlPath;
	IFileHandle* FileHandle = nullptr;  // persistent append handle; owned by this writer
};
