// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * FEntityAnnotationData
 *
 * Per-entity annotation snapshot captured on the game thread.
 * Passed by value to the task thread alongside the pixel buffer.
 * Contains everything needed to write COCO/VOC bounding box records.
 */
struct FEntityAnnotationData
{
	uint16  EntityId   = 0;
	uint16  EntityType = 0;
	FString ClassName;       // from FEntityTypeEntry::ClassName; falls back to "type_NNNN"

	// Screen-space 2D bounding box in pixel coordinates (top-left origin).
	// Clamped to [0, ImageWidth) x [0, ImageHeight).
	FBox2D  ScreenBBox = FBox2D(ForceInit);

	bool    bVisible   = false;  // false → entity fully outside frustum; omit from annotations
	bool    bTruncated = false;  // true → bbox was clamped to image boundary
};

/**
 * FViewProjectionData
 *
 * Camera view+projection matrices at frame capture time.
 * Computed on the game thread from SceneCapture state.
 * Used by FCamSimEntityManager::GetEntitySnapshot() to project entity AABBs.
 */
struct FViewProjectionData
{
	FMatrix ViewProjectionMatrix = FMatrix::Identity;
	int32   ImageWidth           = 1920;
	int32   ImageHeight          = 1080;
};
