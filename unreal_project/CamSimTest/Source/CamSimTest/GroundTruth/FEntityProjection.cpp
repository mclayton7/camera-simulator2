// Copyright CamSim Contributors. All Rights Reserved.

#include "GroundTruth/FEntityProjection.h"
#include "Math/Matrix.h"
#include "GroundTruth/MaskGeometry.h"

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
	// View matrix: world → camera space → graphics-camera axes.
	// UE world is X-forward, Y-right, Z-up; the perspective matrix below
	// expects Z-forward, X-right, Y-up. Apply the standard axis swap so a
	// point straight ahead in UE (+X) lands on the projection's depth axis (+Z)
	// — without it, every Clip.W is negative and ProjectAABB rejects every corner.
	// Matches the swap in USceneCaptureComponent2D::CalcSceneView.
	static const FMatrix UEToGraphics(
	    FPlane(0, 0, 1, 0),  // UE +X → +Z
	    FPlane(1, 0, 0, 0),  // UE +Y → +X
	    FPlane(0, 1, 0, 0),  // UE +Z → +Y
	    FPlane(0, 0, 0, 1));
	const FMatrix ViewMatrix =
	    FTranslationMatrix(-CameraLocation) *
	    FInverseRotationMatrix(CameraRotation) *
	    UEToGraphics;

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

FVector2D FEntityProjection::DistortPixel(const FVector2D& Pinhole, int32 W, int32 H, float FocalPx, float K1, float K2)
{
	if (!(FocalPx > 0.0f)) return Pinhole;
	const double Cx = 0.5 * W, Cy = 0.5 * H;
	const double Xu = (Pinhole.X - Cx) / FocalPx, Yu = (Pinhole.Y - Cy) / FocalPx;
	const double R2 = Xu * Xu + Yu * Yu;
	const double S = 1.0 + K1 * R2 + K2 * R2 * R2;
	return FVector2D(Cx + Xu * S * FocalPx, Cy + Yu * S * FocalPx);
}

FProjectedBox3D FEntityProjection::ProjectOrientedBox(const FBox& L, const FTransform& ActorToWorld,
	const FMatrix& VP, int32 W, int32 H, float FocalPx, float K1, float K2)
{
	// bottom face then top; each rear-left, rear-right, front-right, front-left (body X fwd, Y right)
	const FVector Local[8] = {
		{L.Min.X, L.Min.Y, L.Min.Z}, {L.Min.X, L.Max.Y, L.Min.Z}, {L.Max.X, L.Max.Y, L.Min.Z}, {L.Max.X, L.Min.Y, L.Min.Z},
		{L.Min.X, L.Min.Y, L.Max.Z}, {L.Min.X, L.Max.Y, L.Max.Z}, {L.Max.X, L.Max.Y, L.Max.Z}, {L.Max.X, L.Min.Y, L.Max.Z} };
	FProjectedBox3D Out;
	for (int32 I = 0; I < 8; ++I)
	{
		const FVector4 Clip = VP.TransformFVector4(FVector4(ActorToWorld.TransformPosition(Local[I]), 1.0));
		if (Clip.W <= 0.0) return FProjectedBox3D();   // behind the camera: invalid, truncation unknown
		const FVector2D Pin((Clip.X / Clip.W + 1.0) * 0.5 * W, (1.0 - Clip.Y / Clip.W) * 0.5 * H);
		Out.Corners[I] = DistortPixel(Pin, W, H, FocalPx, K1, K2);
	}
	Out.bValid = true;
	const TArray<FVector2D> Hull = CamSimMask::ConvexHull(TArray<FVector2D>(Out.Corners, 8));
	const double Area = CamSimMask::PolygonArea(Hull);
	const double Inside = CamSimMask::PolygonArea(CamSimMask::ClipToRect(Hull, FBox2D(FVector2D(0, 0), FVector2D(W, H))));
	Out.Truncation = Area > 0.0 ? FMath::Clamp(1.0 - Inside / Area, 0.0, 1.0) : 0.0;
	return Out;
}
