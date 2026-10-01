// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "GroundTruth/FEntityProjection.h"
#include "Entity/StencilSlotAllocator.h"
#include "GroundTruth/AnnotationTypes.h"
#include "GroundTruth/FGroundTruthCollector.h"
#include "Config/CamSimConfig.h"
#include "Sensor/SensorOptics.h"
#include "Metadata/CamSimTelemetry.h"
#include "Sim/Commands.h"  // FEntityKey, EHostSource
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

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

	TestEqual(TEXT("EntityId default 0"),   Data.EntityId,   static_cast<uint32>(0));
	TestEqual(TEXT("EntityType default 0"), Data.EntityType, static_cast<uint16>(0));
	TestFalse(TEXT("bVisible default false"),    Data.bVisible);
	TestFalse(TEXT("bTruncated default false"),  Data.bTruncated);
	TestTrue(TEXT("ClassName default empty"),    Data.ClassName.IsEmpty());

	return true;
}

// 8. FAnnotationIdAllocator — IDs never repeat; SplitSourceKey disambiguates sources
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthAnnotationIdsTest,
	"CamSim.GroundTruth.AnnotationIdsNeverRepeat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundTruthAnnotationIdsTest::RunTest(const FString& Parameters)
{
	FAnnotationIdAllocator Ids;
	TSet<uint32> Seen;
	for (int32 i = 0; i < 1000; ++i)
	{
		const uint32 Id = Ids.Allocate();
		TestTrue(TEXT("non-zero"), Id != 0);
		TestFalse(TEXT("never repeats"), Seen.Contains(Id));
		Seen.Add(Id);
	}

	// A DIS key and a CIGI key with the same low 16 bits stay distinguishable.
	FString Src, SrcId;
	CamSimGroundTruth::SplitSourceKey(FEntityKey(EHostSource::Dis, (1ull << 32) | (1ull << 16) | 7).ToString(), Src, SrcId);
	TestEqual(TEXT("dis source"), Src, FString(TEXT("dis")));
	TestEqual(TEXT("dis source id"), SrcId, FString(TEXT("1.1.7")));
	CamSimGroundTruth::SplitSourceKey(FEntityKey(EHostSource::Cigi, 7).ToString(), Src, SrcId);
	TestEqual(TEXT("cigi source"), Src, FString(TEXT("cigi")));
	TestEqual(TEXT("cigi source id"), SrcId, FString(TEXT("7")));
	return true;
}

// 9. Per-frame ground-truth snapshots ride in the readback-ring slot: two
// frames in flight, delivered out of order, each writes its own entities
// (the bug this task fixes: FGroundTruthCollector used to overwrite a single
// PendingEntities member shared across up to three in-flight frames).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthPerFrameSnapshotTest,
	"CamSim.GroundTruth.PerFrameSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundTruthPerFrameSnapshotTest::RunTest(const FString& Parameters)
{
	const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("gt_per_frame_test"));
	IFileManager::Get().DeleteDirectory(*Dir, false, true);

	FCamSimConfig Cfg;
	Cfg.MLTraining.bEnabled = true;
	Cfg.MLTraining.OutputDir = Dir;
	Cfg.MLTraining.bBoundingBoxes = true;
	Cfg.MLTraining.bCocoExport = true;
	Cfg.MLTraining.bVocExport = false;
	Cfg.MLTraining.bDepthMap = false;
	Cfg.MLTraining.AnnotationIntervalFrames = 1;

	auto Make = [](uint32 Id, const TCHAR* Cls, const TCHAR* SrcId)
	{
		FEntityAnnotationData E;
		E.EntityId = Id; E.EntityType = 2001; E.ClassName = Cls;
		E.Source = TEXT("dis"); E.SourceId = SrcId;
		E.ScreenBBox = FBox2D(FVector2D(10, 20), FVector2D(50, 60));
		E.bVisible = true;
		return E;
	};
	{
		FGroundTruthCollector Collector(Cfg);
		TestTrue(TEXT("opened"), Collector.Open());
		FCamSimTelemetry Tel;
		// Two frames in flight, delivered out of order: each writes its own entities.
		const TArray<FEntityAnnotationData> Frame1 = { Make(70000, TEXT("truck"), TEXT("1.1.1")) };
		TArray<FEntityAnnotationData> Frame2 = { Make(70001, TEXT("boat"), TEXT("1.1.2")) };
		Frame2[0].bHasGeo = true; Frame2[0].Lat = 37.815; Frame2[0].Lon = -122.44; Frame2[0].AltM = -32.125;
		Collector.WriteAnnotationFrame(Frame2, Tel, 2);
		Collector.WriteAnnotationFrame(Frame1, Tel, 1);
		Collector.Close();
	}

	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *Dir, TEXT("*.jsonl"), true, false);
	if (!TestEqual(TEXT("one COCO file"), Files.Num(), 1)) return false;
	TArray<FString> Lines;
	FFileHelper::LoadFileToStringArray(Lines, *Files[0]);
	if (!TestEqual(TEXT("two lines"), Lines.Num(), 2)) return false;
	TestTrue(TEXT("frame 2 has the boat"), Lines[0].Contains(TEXT("\"frame_id\":2")) && Lines[0].Contains(TEXT("\"entity_id\":70001")) && Lines[0].Contains(TEXT("\"name\":\"boat\"")));
	TestTrue(TEXT("frame 1 has the truck"), Lines[1].Contains(TEXT("\"frame_id\":1")) && Lines[1].Contains(TEXT("\"entity_id\":70000")) && Lines[1].Contains(TEXT("\"name\":\"truck\"")));
	TestTrue(TEXT("source fields"), Lines[1].Contains(TEXT("\"source\":\"dis\",\"source_id\":\"1.1.1\"")));
	TestTrue(TEXT("geo pose written when known"), Lines[0].Contains(TEXT("\"truncated\":0,\"geo\":{\"lat\":37.81500000,\"lon\":-122.44000000,\"alt_m\":-32.125}}")));
	TestFalse(TEXT("no geo when unknown"), Lines[1].Contains(TEXT("\"geo\"")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthBox3DCentredTest, "CamSim.GroundTruth.Box3D.Centred",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGroundTruthBox3DCentredTest::RunTest(const FString&)
{
	const FMatrix VP = FEntityProjection::BuildViewProjectionMatrix(FVector::ZeroVector, FRotator::ZeroRotator, 60.0f, 1920, 1080);
	const FBox Local(FVector(-300, -100, 0), FVector(300, 100, 250));   // 6 x 2 x 2.5 m
	const FTransform At(FRotator::ZeroRotator, FVector(10000, 0, -125));  // 100 m ahead, centred
	const FProjectedBox3D P = FEntityProjection::ProjectOrientedBox(Local, At, VP, 1920, 1080, 0.0f, 0.0f, 0.0f);
	TestTrue(TEXT("valid"), P.bValid);
	TestNearlyEqual(TEXT("no truncation"), P.Truncation, 0.0, 1e-9);
	// corner 0 = bottom rear-left (X min, Y min, Z min) is left of and below the centre
	TestTrue(TEXT("rear-left is left"), P.Corners[0].X < 960.0);
	TestTrue(TEXT("bottom is below"), P.Corners[0].Y > 540.0);
	TestTrue(TEXT("top above bottom"), P.Corners[4].Y < P.Corners[0].Y);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthBox3DHalfOffTest, "CamSim.GroundTruth.Box3D.HalfOffEdge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGroundTruthBox3DHalfOffTest::RunTest(const FString&)
{
	const FMatrix VP = FEntityProjection::BuildViewProjectionMatrix(FVector::ZeroVector, FRotator::ZeroRotator, 60.0f, 1920, 1080);
	// Flat face-on box (thin in X) centred on the right image edge: x_ndc = 1 at y = tan(30°) * depth
	const double Depth = 10000.0, EdgeY = FMath::Tan(FMath::DegreesToRadians(30.0)) * Depth;
	const FBox Local(FVector(-1, -200, -200), FVector(1, 200, 200));
	const FProjectedBox3D P = FEntityProjection::ProjectOrientedBox(Local, FTransform(FVector(Depth, EdgeY, 0)), VP, 1920, 1080, 0.0f, 0.0f, 0.0f);
	TestTrue(TEXT("valid"), P.bValid);
	TestNearlyEqual(TEXT("half truncated"), P.Truncation, 0.5, 0.02);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthBox3DBehindTest, "CamSim.GroundTruth.Box3D.BehindCamera",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGroundTruthBox3DBehindTest::RunTest(const FString&)
{
	const FMatrix VP = FEntityProjection::BuildViewProjectionMatrix(FVector::ZeroVector, FRotator::ZeroRotator, 60.0f, 1920, 1080);
	const FBox Local(FVector(-500, -100, -100), FVector(500, 100, 100));   // straddles the camera plane
	const FProjectedBox3D P = FEntityProjection::ProjectOrientedBox(Local, FTransform(FVector(200, 0, 0)), VP, 1920, 1080, 0.0f, 0.0f, 0.0f);
	TestFalse(TEXT("invalid"), P.bValid);
	TestTrue(TEXT("truncation unknown"), P.Truncation < 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthDistortPixelTest, "CamSim.GroundTruth.Box3D.DistortPixel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGroundTruthDistortPixelTest::RunTest(const FString&)
{
	const int32 W = 1920, H = 1080;
	const float F = CamSimOptics::FocalPx(W, 60.0f), K1 = -0.15f, K2 = 0.02f;
	const FVector2D Pin(1700.0, 900.0);
	const FVector2D D = FEntityProjection::DistortPixel(Pin, W, H, F, K1, K2);
	TestTrue(TEXT("barrel (k1<0) pulls inward"), FVector2D::Distance(D, FVector2D(960, 540)) < FVector2D::Distance(Pin, FVector2D(960, 540)));
	// Round trip with the sensor's inverse
	const double Xd = (D.X - 0.5 * W) / F, Yd = (D.Y - 0.5 * H) / F, Rd = FMath::Sqrt(Xd * Xd + Yd * Yd);
	float Ru = 0.0f;
	TestTrue(TEXT("converges"), CamSimOptics::UndistortRadius(static_cast<float>(Rd), K1, K2, Ru));
	const FVector2D Back(0.5 * W + Xd * (Ru / Rd) * F, 0.5 * H + Yd * (Ru / Rd) * F);
	TestNearlyEqual(TEXT("round trip x"), Back.X, Pin.X, 0.05);
	TestNearlyEqual(TEXT("round trip y"), Back.Y, Pin.Y, 0.05);
	TestEqual(TEXT("no focal -> identity"), FEntityProjection::DistortPixel(Pin, W, H, 0.0f, K1, K2), Pin);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthStencilAllocTest, "CamSim.GroundTruth.StencilAllocator.Order",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGroundTruthStencilAllocTest::RunTest(const FString&)
{
	FStencilSlotAllocator A;
	TestEqual(TEXT("first"), A.Allocate(0), uint8(1));
	TestEqual(TEXT("second"), A.Allocate(0), uint8(2));
	for (int32 I = 3; I <= 255; ++I) A.Allocate(0);
	TestEqual(TEXT("exhausted"), A.Allocate(0), uint8(0));
	A.Release(0, 0);  // ignored
	TestEqual(TEXT("still exhausted"), A.Allocate(1), uint8(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthStencilReuseTest, "CamSim.GroundTruth.StencilAllocator.DelayedReuse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGroundTruthStencilReuseTest::RunTest(const FString&)
{
	FStencilSlotAllocator A;
	const uint8 V1 = A.Allocate(10), V2 = A.Allocate(10);
	A.Release(V1, 100);
	TestEqual(TEXT("released value not reused within the delay"), A.Allocate(101), uint8(3));
	TestEqual(TEXT("reused once the delay has passed"), A.Allocate(100 + FStencilSlotAllocator::ReuseDelayFrames), V1);
	TestTrue(TEXT("delay covers the ring"), FStencilSlotAllocator::ReuseDelayFrames > 3);
	(void)V2;
	return true;
}
