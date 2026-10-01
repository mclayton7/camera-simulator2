// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "GroundTruth/InstanceMaskAnalyzer.h"
#include "HAL/PlatformTime.h"
#include "CamSimTest.h"

namespace
{
	FInstanceIdImage MakeImage(int32 W, int32 H) { FInstanceIdImage I; I.Width = W; I.Height = H; I.Words.Init(0u, W * H / 2); return I; }
	void Set(FInstanceIdImage& I, int32 X, int32 Y, uint8 Visible, uint8 Amodal)
	{
		const int32 Idx = Y * I.Width + X; uint32& W = I.Words[Idx >> 1];
		const uint32 V = uint32(Visible) | (uint32(Amodal) << 8);
		W = (Idx & 1) ? ((W & 0x0000FFFFu) | (V << 16)) : ((W & 0xFFFF0000u) | V);
	}
	FEntityAnnotationData Tagged(uint8 Stencil, uint32 Id)
	{
		FEntityAnnotationData E; E.StencilValue = Stencil; E.EntityId = Id; E.bVisible = true;
		E.ScreenBBox = FBox2D(FVector2D(0, 0), FVector2D(99, 99)); E.Truncation = 0.0; return E;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnalyzerPartlyHiddenTest, "CamSim.GroundTruth.Analyzer.PartlyHidden",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnalyzerPartlyHiddenTest::RunTest(const FString&)
{
	// 20x10 image; entity 3 amodal = cols 4..11, rows 2..5 (32 px); visible = cols 4..7 (16 px)
	FInstanceIdImage I = MakeImage(20, 10);
	for (int32 Y = 2; Y <= 5; ++Y) for (int32 X = 4; X <= 11; ++X) Set(I, X, Y, X <= 7 ? 3 : 0, 3);
	TArray<FEntityAnnotationData> E = { Tagged(3, 42) };
	FInstanceMaskAnalyzer::Analyze(I, E, 1, true);
	if (!TestEqual(TEXT("kept"), E.Num(), 1)) return false;
	TestTrue(TEXT("measured"), E[0].bMaskMeasured);
	TestEqual(TEXT("visible px"), E[0].VisiblePixels, 16);
	TestEqual(TEXT("amodal px"), E[0].AmodalPixels, 32);
	TestEqual(TEXT("modal min"), E[0].ScreenBBox.Min, FVector2D(4, 2));
	TestEqual(TEXT("modal max (edge)"), E[0].ScreenBBox.Max, FVector2D(8, 6));
	TestEqual(TEXT("amodal max"), E[0].AmodalBBox.Max, FVector2D(12, 6));
	TestNearlyEqual(TEXT("obb w"), E[0].Obb.W, 4.0, 1e-6);   // 4x4 visible square
	TestNearlyEqual(TEXT("obb amodal w"), E[0].ObbAmodal.W, 8.0, 1e-6);
	TestNearlyEqual(TEXT("obb amodal cx"), E[0].ObbAmodal.Cx, 8.0, 1e-6);
	TestFalse(TEXT("rle present"), E[0].SegmentationRle.IsEmpty());
	TestFalse(TEXT("not truncated"), E[0].bTruncated);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnalyzerRleMatchesCocoTest, "CamSim.GroundTruth.Analyzer.RleMatchesCoco",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnalyzerRleMatchesCocoTest::RunTest(const FString&)
{
	// Same mask as the Mask.CocoRle "rect" fixture: H=4, W=6 (even), rows 1..2, cols 1..3 -> runs 5,2,2,2,2,2,9
	FInstanceIdImage I = MakeImage(6, 4);
	for (int32 Y = 1; Y <= 2; ++Y) for (int32 X = 1; X <= 3; ++X) Set(I, X, Y, 7, 7);
	TArray<FEntityAnnotationData> E = { Tagged(7, 1) };
	FInstanceMaskAnalyzer::Analyze(I, E, 1, true);
	TestEqual(TEXT("rle"), E[0].SegmentationRle, CamSimMask::EncodeCocoRle({ 5, 2, 2, 2, 2, 2, 9 }));
	TArray<FEntityAnnotationData> NoSeg = { Tagged(7, 1) };
	FInstanceMaskAnalyzer::Analyze(I, NoSeg, 1, false);
	TestTrue(TEXT("segmentation off"), NoSeg[0].SegmentationRle.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnalyzerTwoEntitiesTest, "CamSim.GroundTruth.Analyzer.TwoEntitiesOverlap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnalyzerTwoEntitiesTest::RunTest(const FString&)
{
	// Entity 1 at cols 0..5, entity 2 in front at cols 4..9 (custom depth: front one owns 4..5 in both channels)
	FInstanceIdImage I = MakeImage(10, 2);
	for (int32 X = 0; X < 10; ++X) { const uint8 S = X >= 4 ? 2 : 1; Set(I, X, 0, S, S); }
	TArray<FEntityAnnotationData> E = { Tagged(1, 10), Tagged(2, 20) };
	FInstanceMaskAnalyzer::Analyze(I, E, 1, true);
	TestEqual(TEXT("rear visible"), E[0].VisiblePixels, 4);
	TestEqual(TEXT("rear amodal = own coverage"), E[0].AmodalPixels, 4);
	TestEqual(TEXT("front visible"), E[1].VisiblePixels, 6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnalyzerDropsHiddenTest, "CamSim.GroundTruth.Analyzer.DropsHidden",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnalyzerDropsHiddenTest::RunTest(const FString&)
{
	FInstanceIdImage I = MakeImage(8, 2);
	for (int32 X = 0; X < 3; ++X) Set(I, X, 0, 0, 5);   // fully hidden entity 5
	Set(I, 6, 1, 6, 6);                                 // entity 6: 1 visible px
	FEntityAnnotationData Untagged; Untagged.EntityId = 99; Untagged.bVisible = true;
	Untagged.ScreenBBox = FBox2D(FVector2D(1, 1), FVector2D(3, 3));
	TArray<FEntityAnnotationData> E = { Tagged(5, 1), Tagged(6, 2), Untagged, Tagged(9, 3) /* not in image */ };
	FInstanceMaskAnalyzer::Analyze(I, E, 1, true);
	if (!TestEqual(TEXT("kept two"), E.Num(), 2)) return false;
	TestEqual(TEXT("entity 6 kept"), E[0].EntityId, 2u);
	TestEqual(TEXT("untagged kept as projection"), E[1].EntityId, 99u);
	TestFalse(TEXT("untagged not measured"), E[1].bMaskMeasured);
	TArray<FEntityAnnotationData> E2 = { Tagged(6, 2) };
	FInstanceMaskAnalyzer::Analyze(I, E2, 2, true);
	TestEqual(TEXT("min_visible_pixels=2 drops 1-px entity"), E2.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnalyzerEdgeTruncationTest, "CamSim.GroundTruth.Analyzer.EdgeTouchTruncation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnalyzerEdgeTruncationTest::RunTest(const FString&)
{
	FInstanceIdImage I = MakeImage(8, 4);
	for (int32 Y = 0; Y < 2; ++Y) for (int32 X = 5; X < 8; ++X) Set(I, X, Y, 4, 4);  // touches right edge
	FEntityAnnotationData Unknown = Tagged(4, 1); Unknown.Truncation = -1.0;           // corners behind camera
	FEntityAnnotationData Known = Tagged(4, 2);   Known.Truncation = 0.0;              // 3D box says inside
	TArray<FEntityAnnotationData> E = { Unknown };
	FInstanceMaskAnalyzer::Analyze(I, E, 1, true);
	TestTrue(TEXT("unknown truncation: edge touch -> truncated"), E[0].bTruncated);
	TArray<FEntityAnnotationData> E2 = { Known };
	FInstanceMaskAnalyzer::Analyze(I, E2, 1, true);
	TestFalse(TEXT("known truncation wins"), E2[0].bTruncated);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnalyzerSizeMismatchTest, "CamSim.GroundTruth.Analyzer.SizeMismatchFallsBack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnalyzerSizeMismatchTest::RunTest(const FString&)
{
	FInstanceIdImage Bad; Bad.Width = 8; Bad.Height = 4; Bad.Words.Init(0u, 3);  // too short
	TArray<FEntityAnnotationData> E = { Tagged(1, 1) };
	FInstanceMaskAnalyzer::Analyze(Bad, E, 1, true);
	TestEqual(TEXT("kept"), E.Num(), 1);
	TestFalse(TEXT("not measured"), E[0].bMaskMeasured);
	FInstanceIdImage Empty;
	FInstanceMaskAnalyzer::Analyze(Empty, E, 1, true);
	TestEqual(TEXT("still kept"), E.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnalyzerDuplicateStencilTest, "CamSim.GroundTruth.Analyzer.DuplicateStencilFallsBack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnalyzerDuplicateStencilTest::RunTest(const FString&)
{
	FInstanceIdImage I = MakeImage(8, 2);
	for (int32 X = 0; X < 4; ++X) Set(I, X, 0, 3, 3);
	Set(I, 6, 1, 4, 4);
	AddExpectedError(TEXT("stencil value shared"), EAutomationExpectedErrorFlags::Contains, 0);
	TArray<FEntityAnnotationData> E = { Tagged(3, 1), Tagged(3, 2), Tagged(4, 3) };
	FInstanceMaskAnalyzer::Analyze(I, E, 1, true);
	if (!TestEqual(TEXT("all kept"), E.Num(), 3)) return false;
	TestFalse(TEXT("dup A not measured"), E[0].bMaskMeasured);
	TestFalse(TEXT("dup B not measured"), E[1].bMaskMeasured);
	TestTrue(TEXT("unique still measured"), E[2].bMaskMeasured);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnalyzerCornerRleTest, "CamSim.GroundTruth.Analyzer.CornerRle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnalyzerCornerRleTest::RunTest(const FString&)
{
	// W=6, H=4, column-major: (0,0) -> 0,1,23 ; (5,3) -> 23,1
	FInstanceIdImage I = MakeImage(6, 4); Set(I, 0, 0, 2, 2);
	TArray<FEntityAnnotationData> E = { Tagged(2, 1) };
	FInstanceMaskAnalyzer::Analyze(I, E, 1, true);
	TestEqual(TEXT("top-left"), E[0].SegmentationRle, CamSimMask::EncodeCocoRle({ 0, 1, 23 }));
	FInstanceIdImage J = MakeImage(6, 4); Set(J, 5, 3, 2, 2);
	TArray<FEntityAnnotationData> F = { Tagged(2, 1) };
	FInstanceMaskAnalyzer::Analyze(J, F, 1, true);
	TestEqual(TEXT("bottom-right"), F[0].SegmentationRle, CamSimMask::EncodeCocoRle({ 23, 1 }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnalyzerEmptyAmodalTest, "CamSim.GroundTruth.Analyzer.EmptyAmodalUsesModal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnalyzerEmptyAmodalTest::RunTest(const FString&)
{
	FInstanceIdImage I = MakeImage(8, 2);
	Set(I, 2, 1, 3, 0); Set(I, 3, 1, 3, 0);
	TArray<FEntityAnnotationData> E = { Tagged(3, 1) };
	FInstanceMaskAnalyzer::Analyze(I, E, 1, false);
	if (!TestEqual(TEXT("kept"), E.Num(), 1)) return false;
	TestEqual(TEXT("amodal falls back to modal count"), E[0].AmodalPixels, 2);
	TestEqual(TEXT("amodal box max x"), E[0].AmodalBBox.Max.X, 4.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnalyzerPerf1080pTest, "CamSim.GroundTruth.Analyzer.Perf1080p",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnalyzerPerf1080pTest::RunTest(const FString&)
{
	FInstanceIdImage I = MakeImage(1920, 1080);
	for (int32 Y = 300; Y < 400; ++Y) for (int32 X = 400; X < 600; ++X) Set(I, X, Y, 1, 1);
	for (int32 Y = 600; Y < 700; ++Y) for (int32 X = 1200; X < 1400; ++X) Set(I, X, Y, 2, 2);
	TArray<double> Ms;
	for (int32 R = 0; R < 5; ++R)
	{
		TArray<FEntityAnnotationData> E = { Tagged(1, 1), Tagged(2, 2) };
		const double T0 = FPlatformTime::Seconds();
		FInstanceMaskAnalyzer::Analyze(I, E, 1, true);
		Ms.Add((FPlatformTime::Seconds() - T0) * 1000.0);
		if (!TestEqual(TEXT("both measured"), E.Num(), 2)) return false;
		TestEqual(TEXT("visible px"), E[0].VisiblePixels, 20000);
	}
	Ms.Sort();
	UE_LOG(LogCamSim, Display, TEXT("Analyzer 1080p median %.3f ms"), Ms[2]);
	TestTrue(TEXT("median < 15 ms"), Ms[2] < 15.0);
	return true;
}
