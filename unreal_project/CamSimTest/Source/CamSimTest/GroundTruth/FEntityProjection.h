// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GroundTruth/AnnotationTypes.h"

/**
 * FEntityProjection
 *
 * Pure-static math helpers for projecting world-space AABBs into screen space.
 * No UE scene access — safe to test without a running world.
 *
 * Used by FCamSimEntityManager::GetEntitySnapshot() on the game thread.
 */
struct FEntityProjection
{
	/**
	 * Project all 8 corners of WorldAABB through ViewProjectionMatrix onto the
	 * image plane. Returns true if any corner lands inside the frustum.
	 *
	 * OutScreenBBox is set to the union of all projected corners, clamped to
	 * [0, ImageWidth) x [0, ImageHeight).  OutbTruncated is set to true when
	 * any corner was outside the image before clamping.
	 *
	 * UE uses a reversed-Z projection (depth 1.0 = near plane, 0.0 = far).
	 * Points with w <= 0 are behind the camera and are excluded.
	 */
	static bool ProjectAABB(const FBox& WorldAABB,
	                        const FMatrix& ViewProjectionMatrix,
	                        int32 ImageWidth, int32 ImageHeight,
	                        FBox2D& OutScreenBBox,
	                        bool& OutbTruncated);

	/**
	 * Build a combined view-projection matrix from SceneCapture state.
	 *
	 * HFovDeg is the horizontal field of view; VFovDeg is derived from the
	 * image aspect ratio when 0 is passed.
	 *
	 * NearClipCm matches UE's default scene near clip (10 cm).
	 */
	static FMatrix BuildViewProjectionMatrix(
	    const FVector&  CameraLocation,
	    const FRotator& CameraRotation,
	    float  HFovDeg,
	    int32  ImageWidth,
	    int32  ImageHeight,
	    float  NearClipCm = 10.0f);

	/** Pinhole pixel (continuous, pixel i spans [i, i+1)) -> distorted output pixel: rd = ru (1 + K1 ru^2 + K2 ru^4)
	 *  about the image centre, normalised by FocalPx. FocalPx <= 0 returns the input. */
	static FVector2D DistortPixel(const FVector2D& Pinhole, int32 W, int32 H, float FocalPx, float K1, float K2);

	/** LocalBox (actor space, cm) through ActorToWorld and ViewProj, then DistortPixel. bValid false (Truncation -1) when
	 *  any corner has clip w <= 0. Truncation = 1 - area(hull ∩ [0,W]x[0,H]) / area(hull), clamped to [0, 1]. */
	static FProjectedBox3D ProjectOrientedBox(const FBox& LocalBox, const FTransform& ActorToWorld,
	    const FMatrix& ViewProjectionMatrix, int32 W, int32 H, float FocalPx, float K1, float K2);
};
