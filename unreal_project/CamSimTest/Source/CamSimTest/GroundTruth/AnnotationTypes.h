// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GroundTruth/MaskGeometry.h"

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

	// Geodetic pose of the entity's origin at capture (WGS-84 degrees, ellipsoid metres).
	// For a clamped surface vehicle this is the waterline / ground contact point.
	bool    bHasGeo    = false;
	double  Lat        = 0.0;
	double  Lon        = 0.0;
	double  AltM       = 0.0;

	bool    bVisible   = false;  // false → entity fully outside frustum; omit from annotations
	bool    bTruncated = false;  // true → bbox was clamped to image boundary

	bool    bWaterSurface = false;  // surface vessel (placed on water): its hidden hull is cut at the sea
	uint8   StencilValue  = 0;      // custom-depth stencil 1..255; 0 = untagged (projection fallback)
	// Oriented 3D box (game thread, Task 4)
	bool    bHasBox3D     = false;
	FVector Box3DSizeM    = FVector::ZeroVector;   // body X (length), Y (width), Z (height), metres
	double  YawDeg = 0.0, PitchDeg = 0.0, RollDeg = 0.0;  // CIGI convention (heading from true north)
	bool    bCornersValid = false;                 // false: a corner is behind the near plane
	FVector2D CornersPx[8];                         // output pixels, order in Global Constraints
	double  Truncation    = -1.0;                  // [0,1]; < 0 = unknown
	// Mask analysis (task thread, FInstanceMaskAnalyzer)
	bool    bMaskMeasured = false;                 // true: ScreenBBox and below come from the rendered mask
	int32   VisiblePixels = 0;
	int32   AmodalPixels  = 0;
	FBox2D  AmodalBBox    = FBox2D(ForceInit);     // pixel-edge coords, like ScreenBBox when measured
	CamSimMask::FOrientedBox Obb, ObbAmodal;
	FString SegmentationRle;                        // empty when segmentation is off
};

/** Output-space instance IDs (ROADMAP 2.7): two pixels per word, pixel 2k in the low 16 bits;
 *  each pixel = visible stencil | (amodal stencil << 8). */
struct FInstanceIdImage
{
	int32 Width = 0, Height = 0;
	TArray<uint32> Words;
	bool IsValid() const { return Width > 0 && Height > 0 && (Width % 2) == 0 && Words.Num() == Width * Height / 2; }
	uint16 At(int32 X, int32 Y) const { const int32 I = Y * Width + X; const uint32 W = Words[I >> 1]; return static_cast<uint16>((I & 1) ? (W >> 16) : (W & 0xFFFF)); }
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

	/** Sensor optics for projecting 3D boxes through the lens distortion. FocalPx 0 = pinhole. */
	float   FocalPx = 0.0f;   // output pixels
	float   K1 = 0.0f, K2 = 0.0f;
};

/** An entity's oriented 3D box projected to the image. Corners: bottom face then top, each rear-left, rear-right, front-right, front-left. */
struct FProjectedBox3D
{
	bool bValid = false;
	FVector2D Corners[8];
	double Truncation = -1.0;   // fraction of the projected hull outside the frame; -1 = unknown
};
