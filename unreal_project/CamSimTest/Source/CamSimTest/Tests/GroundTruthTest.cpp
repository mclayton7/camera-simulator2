// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "GroundTruth/FEntityProjection.h"
#include "GroundTruth/AnnotationTypes.h"
#include "Config/CamSimConfig.h"

// -------------------------------------------------------------------------
// Ground Truth / ML Training Data Automation Tests (Phase 17)
// -------------------------------------------------------------------------

// 1. ProjectAABB — box fully behind camera returns false (not visible)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthProjectBehindCameraTest,
	"CamSim.GroundTruth.ProjectAABB.BehindCamera",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundTruthProjectBehindCameraTest::RunTest(const FString& Parameters)
{
	const FMatrix VPMatrix = FEntityProjection::BuildViewProjectionMatrix(
		FVector(0, 0, 1000),     // camera at origin (z=10m in UE)
		FRotator::ZeroRotator,   // looking +X
		60.0f, 1920, 1080);

	// Box placed entirely behind the camera (negative X from camera looking +X)
	const FBox BehindBox(FVector(-5000, -100, 900), FVector(-4000, 100, 1100));

	FBox2D ScreenBBox(ForceInit);
	bool bTruncated = false;
	const bool bVisible = FEntityProjection::ProjectAABB(
		BehindBox, VPMatrix, 1920, 1080, ScreenBBox, bTruncated);

	TestFalse(TEXT("Box behind camera is not visible"), bVisible);
	return true;
}

// 2. ProjectAABB — box directly in front of camera is visible
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthProjectInFrontTest,
	"CamSim.GroundTruth.ProjectAABB.InFront",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundTruthProjectInFrontTest::RunTest(const FString& Parameters)
{
	// Camera at world origin looking in +X direction
	const FMatrix VPMatrix = FEntityProjection::BuildViewProjectionMatrix(
		FVector::ZeroVector,
		FRotator::ZeroRotator,
		60.0f, 1920, 1080);

	// Small box directly in front of camera, centered on boresight
	const FBox FrontBox(FVector(5000, -200, -200), FVector(5200, 200, 200));

	FBox2D ScreenBBox(ForceInit);
	bool bTruncated = false;
	const bool bVisible = FEntityProjection::ProjectAABB(
		FrontBox, VPMatrix, 1920, 1080, ScreenBBox, bTruncated);

	TestTrue(TEXT("Box in front of camera is visible"), bVisible);
	TestTrue(TEXT("Screen bbox has positive area"), ScreenBBox.GetArea() > 0.0f);
	return true;
}

// 3. ProjectAABB — box that fills the whole screen is marked truncated
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthProjectTruncatedTest,
	"CamSim.GroundTruth.ProjectAABB.TruncatedBox",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundTruthProjectTruncatedTest::RunTest(const FString& Parameters)
{
	const FMatrix VPMatrix = FEntityProjection::BuildViewProjectionMatrix(
		FVector::ZeroVector,
		FRotator::ZeroRotator,
		60.0f, 1920, 1080);

	// Giant box centered on camera (extends beyond all frustum edges)
	const FBox HugeBox(FVector(100, -100000, -100000), FVector(200, 100000, 100000));

	FBox2D ScreenBBox(ForceInit);
	bool bTruncated = false;
	const bool bVisible = FEntityProjection::ProjectAABB(
		HugeBox, VPMatrix, 1920, 1080, ScreenBBox, bTruncated);

	TestTrue(TEXT("Huge box is visible"), bVisible);
	TestTrue(TEXT("Huge box is truncated"), bTruncated);
	return true;
}

// 4. BuildViewProjectionMatrix — non-degenerate for typical ISR geometry
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthBuildVPMatrixTest,
	"CamSim.GroundTruth.BuildVPMatrix.NonDegenerate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundTruthBuildVPMatrixTest::RunTest(const FString& Parameters)
{
	const FMatrix M = FEntityProjection::BuildViewProjectionMatrix(
		FVector(0, 0, 500000),   // 5 km altitude in cm
		FRotator(-45, 0, 0),     // 45-degree depression
		45.0f, 1920, 1080);

	// Matrix should not be all zeros and should not contain NaN
	bool bHasNaN = false;
	for (int32 R = 0; R < 4; ++R)
		for (int32 C = 0; C < 4; ++C)
			if (FMath::IsNaN(M.M[R][C])) bHasNaN = true;

	TestFalse(TEXT("VP matrix contains no NaN"), bHasNaN);
	TestNotEqual(TEXT("VP matrix is non-zero"), M, FMatrix::Identity);
	return true;
}

// 5. Depth quantization — linear mapping 0m → 0, FarPlane → 65535
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthDepthQuantizationTest,
	"CamSim.GroundTruth.Depth.Quantization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundTruthDepthQuantizationTest::RunTest(const FString& Parameters)
{
	constexpr float FarPlaneM = 1000.0f;

	// Simulate the quantization logic from FDepthMapWriter
	auto Quantize = [FarPlaneM](float DepthM) -> uint16
	{
		const float Clamped = FMath::Clamp(DepthM, 0.0f, FarPlaneM);
		return static_cast<uint16>(FMath::RoundToInt((Clamped / FarPlaneM) * 65535.0f));
	};

	TestEqual(TEXT("0m maps to 0"),          Quantize(0.0f),      static_cast<uint16>(0));
	TestEqual(TEXT("FarPlane maps to 65535"), Quantize(1000.0f),   static_cast<uint16>(65535));
	TestEqual(TEXT("Beyond far is clamped"),  Quantize(99999.0f),  static_cast<uint16>(65535));

	// Mid-range should be approximately half scale
	const uint16 Mid = Quantize(500.0f);
	TestTrue(TEXT("500m is ~half scale"), Mid > 32000 && Mid < 33000);

	return true;
}

// 6. MLTrainingConfig defaults — bEnabled false, sensible defaults
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthConfigDefaultsTest,
	"CamSim.GroundTruth.Config.Defaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundTruthConfigDefaultsTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;

	TestFalse(TEXT("ML training disabled by default"),  Cfg.MLTraining.bEnabled);
	TestTrue(TEXT("Depth map enabled by default"),      Cfg.MLTraining.bDepthMap);
	TestTrue(TEXT("Bounding boxes enabled by default"), Cfg.MLTraining.bBoundingBoxes);
	TestTrue(TEXT("COCO export enabled by default"),    Cfg.MLTraining.bCocoExport);
	TestFalse(TEXT("VOC export disabled by default"),   Cfg.MLTraining.bVocExport);
	TestEqual(TEXT("AnnotationIntervalFrames default"),
		Cfg.MLTraining.AnnotationIntervalFrames, 1);
	TestEqual(TEXT("DepthFarPlaneM default"),
		Cfg.MLTraining.DepthFarPlaneM, 5000.0f);
	TestTrue(TEXT("OutputDir default empty"),
		Cfg.MLTraining.OutputDir.IsEmpty());

	return true;
}

// 7. FEntityAnnotationData — default-constructed struct is safe to inspect
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthAnnotationDataDefaultTest,
	"CamSim.GroundTruth.AnnotationData.DefaultState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundTruthAnnotationDataDefaultTest::RunTest(const FString& Parameters)
{
	FEntityAnnotationData Data;

	TestEqual(TEXT("EntityId default 0"),   Data.EntityId,   static_cast<uint16>(0));
	TestEqual(TEXT("EntityType default 0"), Data.EntityType, static_cast<uint16>(0));
	TestFalse(TEXT("bVisible default false"),    Data.bVisible);
	TestFalse(TEXT("bTruncated default false"),  Data.bTruncated);
	TestTrue(TEXT("ClassName default empty"),    Data.ClassName.IsEmpty());

	return true;
}
