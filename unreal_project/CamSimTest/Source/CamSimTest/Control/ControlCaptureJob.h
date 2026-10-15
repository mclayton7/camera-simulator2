// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * One control API capture in flight (ROADMAP 2.4). The game thread creates it and hands it to the capture component,
 * which tags the next captured frame with it; the task thread fills it and sets bDone (release); the game thread
 * reads it only after seeing bDone (acquire).
 */
struct FControlCaptureJob
{
	bool bWantDepth = true;

	uint64 FrameIdx     = 0;
	uint64 TimestampUs  = 0;
	int32  Width        = 0;
	int32  Height       = 0;
	uint8  SensorMode   = 0;      // 0 EO, 1 IR (the frame's telemetry)
	float  DepthFarPlaneM = 0.0f;
	FString Record;              // the frame's camsim_coco.jsonl record; "" without ground truth
	TArray64<uint8> Png;         // sensor output
	TArray64<uint8> DepthPng;    // 16-bit grey (metres * 65535 / far); empty without depth
	FString Error;

	TAtomic<bool> bDone { false };
};
