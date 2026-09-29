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
	uint32  EntityId   = 0;      // session-unique annotation ID (FAnnotationIdAllocator), stable for the entity's life
	uint16  EntityType = 0;
	FString ClassName;       // from FEntityTypeEntry::ClassName; falls back to "type_NNNN"
	FString Source;           // host source: "dis", "cigi", "scenario", ...
	FString SourceId;         // the source's own ID: "1.1.3" (DIS site.app.entity), "7" (CIGI)

	// Screen-space 2D bounding box in pixel coordinates (top-left origin).
	// Clamped to [0, ImageWidth) x [0, ImageHeight).
	FBox2D  ScreenBBox = FBox2D(ForceInit);

	bool    bVisible   = false;  // false → entity fully outside frustum; omit from annotations
	bool    bTruncated = false;  // true → bbox was clamped to image boundary
};

/** Hands out annotation IDs: from 1, never repeated within a session (game thread). */
struct FAnnotationIdAllocator
{
	uint32 Allocate() { return Next++; }
private:
	uint32 Next = 1;
};

namespace CamSimGroundTruth
{
	/** "dis:1.1.3" (FEntityKey::ToString) → "dis", "1.1.3". */
	inline void SplitSourceKey(const FString& KeyString, FString& OutSource, FString& OutSourceId)
	{
		if (!KeyString.Split(TEXT(":"), &OutSource, &OutSourceId))
		{
			OutSource = KeyString;
			OutSourceId.Reset();
		}
	}
}

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

	/**
	 * Optional fast cone-cull fields — when CullConeHalfAngleCos > 0.0,
	 * GetEntitySnapshot() skips the expensive AABB projection for any entity
	 * whose direction from CameraLocation lies outside the cone. Set to 0.0f
	 * to disable the cull.
	 */
	FVector CameraLocation       = FVector::ZeroVector;
	FVector CameraForward        = FVector::ForwardVector;
	float   CullConeHalfAngleCos = 0.0f;
};
