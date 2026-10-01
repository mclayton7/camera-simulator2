// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "GroundTruth/MaskGeometry.h"

// CamSim.GroundTruth.Mask.*: pure geometry and COCO RLE (fixtures from pycocotools 2.0,
// `mask.encode(np.asfortranarray(a))["counts"]`).

namespace
{
	TArray<FVector2D> RectCorners(double X0, double Y0, double X1, double Y1)
	{
		return { {X0, Y0}, {X1, Y0}, {X1, Y1}, {X0, Y1} };
	}
	TArray<FVector2D> Rotated(double Cx, double Cy, double W, double H, double Deg)
	{
		const double T = FMath::DegreesToRadians(Deg), C = FMath::Cos(T), S = FMath::Sin(T);
		TArray<FVector2D> P;
		for (const FVector2D& L : { FVector2D(-W/2, -H/2), FVector2D(W/2, -H/2), FVector2D(W/2, H/2), FVector2D(-W/2, H/2) })
			P.Add(FVector2D(Cx + L.X * C - L.Y * S, Cy + L.X * S + L.Y * C));
		return P;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaskMinAreaRectAxisTest, "CamSim.GroundTruth.Mask.MinAreaRectAxis",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMaskMinAreaRectAxisTest::RunTest(const FString&)
{
	const CamSimMask::FOrientedBox B = CamSimMask::MinAreaRect(CamSimMask::ConvexHull(RectCorners(10, 20, 50, 30)));
	TestNearlyEqual(TEXT("cx"), B.Cx, 30.0, 1e-6);
	TestNearlyEqual(TEXT("cy"), B.Cy, 25.0, 1e-6);
	TestNearlyEqual(TEXT("w (long side)"), B.W, 40.0, 1e-6);
	TestNearlyEqual(TEXT("h"), B.H, 10.0, 1e-6);
	TestNearlyEqual(TEXT("angle"), B.AngleDeg, 0.0, 1e-6);
	// Tall rect: w is still the long side, axis along +y => angle -90
	const CamSimMask::FOrientedBox T = CamSimMask::MinAreaRect(CamSimMask::ConvexHull(RectCorners(0, 0, 10, 40)));
	TestNearlyEqual(TEXT("tall w"), T.W, 40.0, 1e-6);
	TestNearlyEqual(TEXT("tall angle"), T.AngleDeg, -90.0, 1e-6);
	// Square: angle folded into [-45, 45)
	const CamSimMask::FOrientedBox Q = CamSimMask::MinAreaRect(CamSimMask::ConvexHull(RectCorners(0, 0, 8, 8)));
	TestNearlyEqual(TEXT("square angle"), Q.AngleDeg, 0.0, 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaskMinAreaRectRotatedTest, "CamSim.GroundTruth.Mask.MinAreaRectRotated",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMaskMinAreaRectRotatedTest::RunTest(const FString&)
{
	for (const double Deg : { 30.0, -60.0, 89.0, -89.0 })
	{
		TArray<FVector2D> Pts = Rotated(100, 80, 60, 20, Deg);
		Pts.Add(FVector2D(100, 80));  // interior points must not matter
		const CamSimMask::FOrientedBox B = CamSimMask::MinAreaRect(CamSimMask::ConvexHull(Pts));
		TestNearlyEqual(*FString::Printf(TEXT("w @%g"), Deg), B.W, 60.0, 1e-6);
		TestNearlyEqual(*FString::Printf(TEXT("h @%g"), Deg), B.H, 20.0, 1e-6);
		TestNearlyEqual(*FString::Printf(TEXT("angle @%g"), Deg), B.AngleDeg, Deg, 1e-6);
		TestNearlyEqual(*FString::Printf(TEXT("cx @%g"), Deg), B.Cx, 100.0, 1e-6);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaskHullDegenerateTest, "CamSim.GroundTruth.Mask.HullDegenerate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMaskHullDegenerateTest::RunTest(const FString&)
{
	TestEqual(TEXT("empty"), CamSimMask::ConvexHull({}).Num(), 0);
	TestEqual(TEXT("one point"), CamSimMask::ConvexHull({ {1, 1}, {1, 1} }).Num(), 1);
	const TArray<FVector2D> Line = CamSimMask::ConvexHull({ {0, 0}, {1, 1}, {2, 2}, {3, 3} });
	TestEqual(TEXT("collinear -> 2 ends"), Line.Num(), 2);
	const CamSimMask::FOrientedBox L = CamSimMask::MinAreaRect(Line);
	TestNearlyEqual(TEXT("line h"), L.H, 0.0, 1e-9);
	TestNearlyEqual(TEXT("line angle"), L.AngleDeg, 45.0, 1e-6);
	const CamSimMask::FOrientedBox P = CamSimMask::MinAreaRect(CamSimMask::ConvexHull({ {5, 6} }));
	TestNearlyEqual(TEXT("point w"), P.W, 0.0, 1e-9);
	TestNearlyEqual(TEXT("point cx"), P.Cx, 5.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaskClipAreaTest, "CamSim.GroundTruth.Mask.ClipArea",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMaskClipAreaTest::RunTest(const FString&)
{
	const TArray<FVector2D> Sq = RectCorners(-10, 0, 10, 10);  // half outside x < 0
	TestNearlyEqual(TEXT("area"), CamSimMask::PolygonArea(Sq), 200.0, 1e-9);
	const TArray<FVector2D> Clipped = CamSimMask::ClipToRect(Sq, FBox2D(FVector2D(0, 0), FVector2D(100, 100)));
	TestNearlyEqual(TEXT("clipped area"), CamSimMask::PolygonArea(Clipped), 100.0, 1e-9);
	const TArray<FVector2D> Outside = CamSimMask::ClipToRect(RectCorners(200, 200, 210, 210), FBox2D(FVector2D(0, 0), FVector2D(100, 100)));
	TestNearlyEqual(TEXT("fully outside"), CamSimMask::PolygonArea(Outside), 0.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaskRleTest, "CamSim.GroundTruth.Mask.CocoRle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMaskRleTest::RunTest(const FString&)
{
	// 4x5 (H x W) image, rows 1..2, cols 1..3 set: column-major runs 5,2,2,2,2,2,5
	TestEqual(TEXT("rect"), CamSimMask::EncodeCocoRle({ 5, 2, 2, 2, 2, 2, 5 }), FString(TEXT("5220003")));
	TestEqual(TEXT("corner"), CamSimMask::EncodeCocoRle({ 0, 1, 19 }), FString(TEXT("01c0")));
	TestEqual(TEXT("empty"), CamSimMask::EncodeCocoRle({ 20 }), FString(TEXT("d0")));
	TestEqual(TEXT("full"), CamSimMask::EncodeCocoRle({ 0, 20 }), FString(TEXT("0d0")));
	// 40x40, a 3-pixel column-major run starting at index 17: the string holds a backslash
	TestEqual(TEXT("backslash"), CamSimMask::EncodeCocoRle({ 17, 3, 1580 }), FString(TEXT("a03\\a1")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaskRectAlongAxisTest, "CamSim.GroundTruth.Mask.RectAlongAxis",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMaskRectAlongAxisTest::RunTest(const FString&)
{
	// A 60 x 20 rectangle rotated 30°: along its own axis (any length, either direction) = the rectangle.
	const TArray<FVector2D> Hull = CamSimMask::ConvexHull(Rotated(100, 80, 60, 20, 30.0));
	const double T = FMath::DegreesToRadians(30.0);
	for (const double Scale : { 1.0, 25.0, -3.0 })
	{
		const CamSimMask::FOrientedBox B = CamSimMask::RectAlongAxis(Hull, Scale * FVector2D(FMath::Cos(T), FMath::Sin(T)));
		TestNearlyEqual(*FString::Printf(TEXT("own axis x%g: w"), Scale), B.W, 60.0, 1e-6);
		TestNearlyEqual(*FString::Printf(TEXT("own axis x%g: h"), Scale), B.H, 20.0, 1e-6);
		TestNearlyEqual(*FString::Printf(TEXT("own axis x%g: angle"), Scale), B.AngleDeg, 30.0, 1e-6);
		TestNearlyEqual(*FString::Printf(TEXT("own axis x%g: cx"), Scale), B.Cx, 100.0, 1e-6);
		TestNearlyEqual(*FString::Printf(TEXT("own axis x%g: cy"), Scale), B.Cy, 80.0, 1e-6);
	}
	// Axis-aligned 40 x 10 box measured along +x: extents are the projections, angle 0.
	const TArray<FVector2D> R = CamSimMask::ConvexHull(RectCorners(10, 20, 50, 30));
	const CamSimMask::FOrientedBox X = CamSimMask::RectAlongAxis(R, FVector2D(5, 0));
	TestNearlyEqual(TEXT("+x: w"), X.W, 40.0, 1e-6);
	TestNearlyEqual(TEXT("+x: h"), X.H, 10.0, 1e-6);
	TestNearlyEqual(TEXT("+x: angle"), X.AngleDeg, 0.0, 1e-6);
	// Along +y (the short side): w < h, so the sides swap and the angle turns +90 (90 -> 180, folded to 0).
	const CamSimMask::FOrientedBox Y = CamSimMask::RectAlongAxis(R, FVector2D(0, 1));
	TestNearlyEqual(TEXT("+y: w"), Y.W, 40.0, 1e-6);
	TestNearlyEqual(TEXT("+y: h"), Y.H, 10.0, 1e-6);
	TestNearlyEqual(TEXT("+y: angle"), Y.AngleDeg, 0.0, 1e-6);
	// Along 45° the enclosing box is larger than the min-area one: (40 + 10) / sqrt 2 on both sides, a square.
	const CamSimMask::FOrientedBox D = CamSimMask::RectAlongAxis(R, FVector2D(1, 1));
	TestNearlyEqual(TEXT("45°: w"), D.W, 50.0 / FMath::Sqrt(2.0), 1e-6);
	TestNearlyEqual(TEXT("45°: h"), D.H, 50.0 / FMath::Sqrt(2.0), 1e-6);
	TestTrue(TEXT("45°: square angle folded into [-45, 45)"), D.AngleDeg >= -45.0 && D.AngleDeg < 45.0);
	// Zero axis: MinAreaRect.
	const CamSimMask::FOrientedBox Z = CamSimMask::RectAlongAxis(Hull, FVector2D::ZeroVector);
	TestNearlyEqual(TEXT("zero axis: min-area angle"), Z.AngleDeg, 30.0, 1e-6);
	return true;
}
