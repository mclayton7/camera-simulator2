// Copyright CamSim Contributors. All Rights Reserved.

#include "GroundTruth/FEntityProjection.h"
#include "Math/Matrix.h"

bool FEntityProjection::ProjectAABB(
    const FBox& WorldAABB,
    const FMatrix& ViewProjectionMatrix,
    int32 ImageWidth, int32 ImageHeight,
    FBox2D& OutScreenBBox,
    bool& OutbTruncated)
{
	// 8 corners of the AABB
	FVector Corners[8];
	Corners[0] = FVector(WorldAABB.Min.X, WorldAABB.Min.Y, WorldAABB.Min.Z);
	Corners[1] = FVector(WorldAABB.Max.X, WorldAABB.Min.Y, WorldAABB.Min.Z);
	Corners[2] = FVector(WorldAABB.Min.X, WorldAABB.Max.Y, WorldAABB.Min.Z);
	Corners[3] = FVector(WorldAABB.Max.X, WorldAABB.Max.Y, WorldAABB.Min.Z);
	Corners[4] = FVector(WorldAABB.Min.X, WorldAABB.Min.Y, WorldAABB.Max.Z);
	Corners[5] = FVector(WorldAABB.Max.X, WorldAABB.Min.Y, WorldAABB.Max.Z);
	Corners[6] = FVector(WorldAABB.Min.X, WorldAABB.Max.Y, WorldAABB.Max.Z);
	Corners[7] = FVector(WorldAABB.Max.X, WorldAABB.Max.Y, WorldAABB.Max.Z);

	const float W  = static_cast<float>(ImageWidth);
	const float H  = static_cast<float>(ImageHeight);

	float MinX =  FLT_MAX, MinY =  FLT_MAX;
	float MaxX = -FLT_MAX, MaxY = -FLT_MAX;
	bool  bAnyVisible  = false;
	bool  bTruncated   = false;

	for (const FVector& C : Corners)
	{
		// Transform to clip space
		const FVector4 Clip = ViewProjectionMatrix.TransformFVector4(FVector4(C, 1.0f));

		// Skip corners behind camera (w <= 0 means behind near plane)
		if (Clip.W <= 0.0f) continue;

		// Perspective divide → NDC [-1,1]
		const float NdcX =  Clip.X / Clip.W;
		const float NdcY =  Clip.Y / Clip.W;

		// NDC to pixel (UE Y-axis: +Y is up in NDC, +Y is down in pixels)
		const float Px = (NdcX + 1.0f) * 0.5f * W;
		const float Py = (1.0f - NdcY) * 0.5f * H;  // flip Y

		MinX = FMath::Min(MinX, Px);
		MinY = FMath::Min(MinY, Py);
		MaxX = FMath::Max(MaxX, Px);
		MaxY = FMath::Max(MaxY, Py);
		bAnyVisible = true;

		// Mark truncated if any corner projects outside image bounds
		if (Px < 0.0f || Px >= W || Py < 0.0f || Py >= H)
			bTruncated = true;
	}

	if (!bAnyVisible)
	{
		OutScreenBBox  = FBox2D(ForceInit);
		OutbTruncated  = false;
		return false;
	}

	// Clamp to image bounds
	const float ClampedMinX = FMath::Clamp(MinX, 0.0f, W - 1.0f);
	const float ClampedMinY = FMath::Clamp(MinY, 0.0f, H - 1.0f);
	const float ClampedMaxX = FMath::Clamp(MaxX, 0.0f, W - 1.0f);
	const float ClampedMaxY = FMath::Clamp(MaxY, 0.0f, H - 1.0f);

	OutScreenBBox  = FBox2D(FVector2D(ClampedMinX, ClampedMinY),
	                        FVector2D(ClampedMaxX, ClampedMaxY));
	OutbTruncated  = bTruncated;
	return true;
}

FMatrix FEntityProjection::BuildViewProjectionMatrix(
    const FVector&  CameraLocation,
    const FRotator& CameraRotation,
    float  HFovDeg,
    int32  ImageWidth,
    int32  ImageHeight,
    float  NearClipCm)
{
	// View matrix: world → camera space.
	// UE row-major convention: translate world point to camera-local origin first,
	// then rotate into camera axes.  Order must be translation * rotation.
	const FMatrix ViewMatrix =
	    FTranslationMatrix(-CameraLocation) *
	    FInverseRotationMatrix(CameraRotation);

	// Perspective projection matching UE5's SceneCapture2D convention.
	// HalfFovH is half the horizontal FOV in radians.
	const float AspectRatio = (ImageHeight > 0)
	    ? static_cast<float>(ImageWidth) / static_cast<float>(ImageHeight)
	    : 1.0f;
	const float HalfFovH = FMath::DegreesToRadians(FMath::Clamp(HFovDeg, 1.0f, 179.0f) * 0.5f);

	// Build a standard (non-reversed-Z) perspective matrix for projection math.
	// We only use this for NDC X/Y; depth correctness is not needed here.
	const float Near = FMath::Max(NearClipCm, 1.0f);
	const float Far  = 50'000'000.0f; // 500 km in cm

	// Column-major perspective — same layout UE uses internally.
	const float XScale = 1.0f / FMath::Tan(HalfFovH);
	const float YScale = XScale * AspectRatio;

	FMatrix ProjMatrix = FMatrix::Identity;
	ProjMatrix.M[0][0] = XScale;
	ProjMatrix.M[1][1] = YScale;
	ProjMatrix.M[2][2] = Far / (Far - Near);
	ProjMatrix.M[2][3] = 1.0f;
	ProjMatrix.M[3][2] = -(Far * Near) / (Far - Near);
	ProjMatrix.M[3][3] = 0.0f;

	return ViewMatrix * ProjMatrix;
}
