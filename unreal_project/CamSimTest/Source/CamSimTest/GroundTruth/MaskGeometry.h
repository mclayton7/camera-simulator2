// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Pure 2D helpers for mask-based ground truth (sub-project 2 of "boats and trucks for ATR").
 * Image coordinates: x right, y down, pixel i spans [i, i + 1).
 */
namespace CamSimMask
{
	/** Rotated rectangle: w >= h; AngleDeg is the w axis from +x toward +y, in [-90, 90) ([-45, 45) when w == h). */
	struct FOrientedBox { double Cx = 0, Cy = 0, W = 0, H = 0, AngleDeg = 0; };

	/** Andrew's monotone chain. Duplicates and collinear points dropped; 0, 1 or 2 points for degenerate input. */
	TArray<FVector2D> ConvexHull(TArray<FVector2D> Points);

	/** Minimum-area enclosing rectangle of a convex hull (rotating calipers over hull edges). Ties prefer the smaller |angle|. */
	FOrientedBox MinAreaRect(const TArray<FVector2D>& Hull);

	/**
	 * The rectangle enclosing a convex hull with one side along Axis (any non-zero length): extents are the hull
	 * projected on Axis and its normal, then normalised as MinAreaRect (w >= h, else swapped and +90°; angle folded).
	 * A zero or non-finite Axis falls back to MinAreaRect.
	 */
	FOrientedBox RectAlongAxis(const TArray<FVector2D>& Hull, const FVector2D& Axis);

	/** Absolute shoelace area. */
	double PolygonArea(const TArray<FVector2D>& Poly);

	/** Sutherland–Hodgman clip of a convex or simple polygon against an axis-aligned rect. */
	TArray<FVector2D> ClipToRect(const TArray<FVector2D>& Poly, const FBox2D& Rect);

	/** pycocotools rleToString: Runs are column-major run lengths, alternating, starting with a (possibly 0) zero run. */
	FString EncodeCocoRle(const TArray<uint32>& Runs);
}
