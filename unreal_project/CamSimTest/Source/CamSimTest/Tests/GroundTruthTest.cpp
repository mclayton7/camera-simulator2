// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "GroundTruth/FEntityProjection.h"
#include "Entity/StencilSlotAllocator.h"
#include "GroundTruth/AnnotationTypes.h"
#include "GroundTruth/FGroundTruthCollector.h"
#include "GroundTruth/MaskGeometry.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Config/CamSimConfig.h"
#include "Sensor/SensorOptics.h"
#include "Metadata/CamSimTelemetry.h"
#include "Sim/Commands.h"  // FEntityKey, EHostSource
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Entity/CamSimEntity.h"
#include "GroundTruth/StillWaterPlane.h"
#include "Ocean/OceanSurface.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BoxComponent.h"

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
	TestTrue(TEXT("geo pose written when known"), Lines[0].Contains(TEXT("\"truncated\":0,\"mask_source\":\"projection\",\"geo\":{\"lat\":37.81500000,\"lon\":-122.44000000,\"alt_m\":-32.125}}")));
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


// -------------------------------------------------------------------------
// Measured fields: collector + COCO writer (ATR ground truth, Task 6)
// -------------------------------------------------------------------------
namespace
{
	FInstanceIdImage GtMakeImage(int32 W, int32 H) { FInstanceIdImage I; I.Width = W; I.Height = H; I.Words.Init(0u, W * H / 2); return I; }
	void GtSet(FInstanceIdImage& I, int32 X, int32 Y, uint8 Visible, uint8 Amodal)
	{
		const int32 Idx = Y * I.Width + X; uint32& W = I.Words[Idx >> 1];
		const uint32 V = uint32(Visible) | (uint32(Amodal) << 8);
		W = (Idx & 1) ? ((W & 0x0000FFFFu) | (V << 16)) : ((W & 0xFFFF0000u) | V);
	}

	/** Runs one frame through a COCO-only collector and returns the parsed single line (null on failure) plus the raw text. */
	TSharedPtr<FJsonObject> GtRunCoco(FAutomationTestBase& T, const TCHAR* DirName, int32 W, int32 H,
		const FInstanceIdImage* Ids, TArray<FEntityAnnotationData> Entities, FString& OutRaw)
	{
		const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), DirName);
		IFileManager::Get().DeleteDirectory(*Dir, false, true);
		FCamSimConfig Cfg;
		Cfg.MLTraining.bEnabled = true; Cfg.MLTraining.OutputDir = Dir;
		Cfg.MLTraining.bBoundingBoxes = true; Cfg.MLTraining.bCocoExport = true;
		Cfg.MLTraining.bVocExport = false; Cfg.MLTraining.bDepthMap = false;
		Cfg.CaptureWidth = W; Cfg.CaptureHeight = H;
		{
			FGroundTruthCollector Collector(Cfg);
			if (!T.TestTrue(TEXT("collector opened"), Collector.Open())) return nullptr;
			FCamSimTelemetry Tel;
			Collector.WriteAnnotationFrame(MoveTemp(Entities), Ids, Tel, 0);
			Collector.Close();
		}
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Dir, TEXT("*.jsonl"), true, false);
		if (!T.TestEqual(TEXT("one COCO file"), Files.Num(), 1)) return nullptr;
		TArray<FString> Lines;
		FFileHelper::LoadFileToStringArray(Lines, *Files[0]);
		if (!T.TestEqual(TEXT("one line"), Lines.Num(), 1)) return nullptr;
		OutRaw = Lines[0];
		TSharedPtr<FJsonObject> Obj;
		if (!T.TestTrue(TEXT("line parses as JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Lines[0]), Obj)) || !Obj) return nullptr;
		return Obj;
	}

	FEntityAnnotationData GtEntity(uint8 Stencil)
	{
		FEntityAnnotationData E;
		E.EntityId = 70000; E.EntityType = 2001; E.ClassName = TEXT("truck");
		E.Source = TEXT("dis"); E.SourceId = TEXT("1.1.1");
		E.StencilValue = Stencil; E.bVisible = true;
		E.ScreenBBox = FBox2D(FVector2D(0, 0), FVector2D(5, 3));
		return E;
	}

	TArray<double> GtNumbers(const TArray<TSharedPtr<FJsonValue>>& A)
	{
		TArray<double> Out; for (const TSharedPtr<FJsonValue>& V : A) Out.Add(V->AsNumber()); return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGtCollectorMeasuredFieldsTest, "CamSim.GroundTruth.Collector.MeasuredFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGtCollectorMeasuredFieldsTest::RunTest(const FString&)
{
	FInstanceIdImage I = GtMakeImage(6, 4);
	for (int32 Y = 1; Y <= 2; ++Y) for (int32 X = 1; X <= 3; ++X) GtSet(I, X, Y, 7, 7);
	FEntityAnnotationData E = GtEntity(7);
	E.bHasBox3D = true; E.Box3DSizeM = FVector(6, 2, 2.5); E.bCornersValid = true; E.Truncation = 0.0;
	// A box heading +x over the mask (rear x 1, front x 4): the OBB follows that axis (I3).
	const FVector2D Bottom[4] = { {1, 1}, {1, 3}, {4, 3}, {4, 1} };
	for (int32 C = 0; C < 8; ++C) E.CornersPx[C] = Bottom[C % 4] - FVector2D(0, C < 4 ? 0.0 : 0.5);
	FString Raw;
	const TSharedPtr<FJsonObject> Root = GtRunCoco(*this, TEXT("gt_measured_test"), 6, 4, &I, { E }, Raw);
	if (!Root) return false;
	AddInfo(Raw);
	const TArray<TSharedPtr<FJsonValue>>& Anns = Root->GetArrayField(TEXT("annotations"));
	if (!TestEqual(TEXT("one annotation"), Anns.Num(), 1)) return false;
	const TSharedPtr<FJsonObject> A = Anns[0]->AsObject();
	TestEqual(TEXT("mask_source"), A->GetStringField(TEXT("mask_source")), FString(TEXT("render")));
	TestEqual(TEXT("bbox"), GtNumbers(A->GetArrayField(TEXT("bbox"))), TArray<double>({ 1, 1, 3, 2 }));
	TestEqual(TEXT("area"), A->GetNumberField(TEXT("area")), 6.0);
	TestNearlyEqual(TEXT("visibility"), A->GetNumberField(TEXT("visibility")), 1.0, 1e-9);
	TestNearlyEqual(TEXT("truncation"), A->GetNumberField(TEXT("truncation")), 0.0, 1e-9);
	TestEqual(TEXT("bbox_amodal"), GtNumbers(A->GetArrayField(TEXT("bbox_amodal"))), TArray<double>({ 1, 1, 3, 2 }));
	const TArray<double> Obb = GtNumbers(A->GetArrayField(TEXT("obb")));
	if (TestEqual(TEXT("obb has 5 numbers"), Obb.Num(), 5))
	{
		TestNearlyEqual(TEXT("obb w"), Obb[2], 3.0, 1e-2);
		TestNearlyEqual(TEXT("obb h"), Obb[3], 2.0, 1e-2);
		TestNearlyEqual(TEXT("obb angle"), Obb[4], 0.0, 1e-2);
	}
	TestEqual(TEXT("obb_amodal has 5 numbers"), A->GetArrayField(TEXT("obb_amodal")).Num(), 5);
	const TSharedPtr<FJsonObject> Seg = A->GetObjectField(TEXT("segmentation"));
	TestEqual(TEXT("segmentation.size"), GtNumbers(Seg->GetArrayField(TEXT("size"))), TArray<double>({ 4, 6 }));
	TestEqual(TEXT("segmentation.counts"), Seg->GetStringField(TEXT("counts")), CamSimMask::EncodeCocoRle({ 5, 2, 2, 2, 2, 2, 9 }));
	const TSharedPtr<FJsonObject> B3 = A->GetObjectField(TEXT("box3d"));
	TestEqual(TEXT("box3d corners_px"), B3->GetArrayField(TEXT("corners_px")).Num(), 8);
	for (const TSharedPtr<FJsonValue>& P : B3->GetArrayField(TEXT("corners_px"))) TestEqual(TEXT("corner is a pair"), P->AsArray().Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGtCollectorFallbackTest, "CamSim.GroundTruth.Collector.FallbackWithoutIds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGtCollectorFallbackTest::RunTest(const FString&)
{
	FInstanceIdImage Wrong = GtMakeImage(8, 4);   // capture size is 6x4: ignored
	for (int32 Y = 1; Y <= 2; ++Y) for (int32 X = 1; X <= 3; ++X) GtSet(Wrong, X, Y, 7, 7);
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		FString Raw;
		const TSharedPtr<FJsonObject> Root = GtRunCoco(*this, TEXT("gt_fallback_test"), 6, 4, Pass ? &Wrong : nullptr, { GtEntity(7) }, Raw);
		if (!Root) return false;
		const TArray<TSharedPtr<FJsonValue>>& Anns = Root->GetArrayField(TEXT("annotations"));
		if (!TestEqual(TEXT("one annotation"), Anns.Num(), 1)) return false;
		const TSharedPtr<FJsonObject> A = Anns[0]->AsObject();
		TestEqual(TEXT("mask_source"), A->GetStringField(TEXT("mask_source")), FString(TEXT("projection")));
		TestEqual(TEXT("bbox from ScreenBBox"), GtNumbers(A->GetArrayField(TEXT("bbox"))), TArray<double>({ 0, 0, 5, 3 }));
		TestFalse(TEXT("no visibility"), A->HasField(TEXT("visibility")));
		TestFalse(TEXT("no segmentation"), A->HasField(TEXT("segmentation")));
		TestFalse(TEXT("no obb"), A->HasField(TEXT("obb")));
		TestFalse(TEXT("no truncation when unknown"), A->HasField(TEXT("truncation")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGtCocoRleEscapedTest, "CamSim.GroundTruth.Coco.RleEscaped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGtCocoRleEscapedTest::RunTest(const FString&)
{
	FInstanceIdImage I = GtMakeImage(40, 40);
	for (int32 Y = 17; Y <= 19; ++Y) GtSet(I, 0, Y, 5, 5);   // column-major run 17..19
	FString Raw;
	const TSharedPtr<FJsonObject> Root = GtRunCoco(*this, TEXT("gt_rle_escape_test"), 40, 40, &I, { GtEntity(5) }, Raw);
	if (!Root) return false;
	const TArray<TSharedPtr<FJsonValue>>& Anns = Root->GetArrayField(TEXT("annotations"));
	if (!TestEqual(TEXT("one annotation"), Anns.Num(), 1)) return false;
	TestEqual(TEXT("counts survives escaping"), Anns[0]->AsObject()->GetObjectField(TEXT("segmentation"))->GetStringField(TEXT("counts")), FString(TEXT("a03\\a1")));
	TestTrue(TEXT("backslash is escaped on the wire"), Raw.Contains(TEXT("a03\\\\a1")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGtCollectorHiddenDroppedTest, "CamSim.GroundTruth.Collector.HiddenDropped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGtCollectorHiddenDroppedTest::RunTest(const FString&)
{
	FInstanceIdImage I = GtMakeImage(6, 4);
	for (int32 Y = 1; Y <= 2; ++Y) for (int32 X = 1; X <= 3; ++X) GtSet(I, X, Y, 0, 7);   // amodal only
	FString Raw;
	const TSharedPtr<FJsonObject> Root = GtRunCoco(*this, TEXT("gt_hidden_test"), 6, 4, &I, { GtEntity(7) }, Raw);
	if (!Root) return false;
	TestEqual(TEXT("no annotations"), Root->GetArrayField(TEXT("annotations")).Num(), 0);
	TestTrue(TEXT("empty array on the wire"), Raw.Contains(TEXT("\"annotations\":[]")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGtConfigNewKeysTest, "CamSim.GroundTruth.Config.NewKeys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGtConfigNewKeysTest::RunTest(const FString&)
{
	const FString Yaml = TEXT("ml_training:\n  min_visible_pixels: 25\n  segmentation: false\n");
	FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(Yaml);
	TestEqual(TEXT("min_visible_pixels"), Cfg.MLTraining.MinVisiblePixels, 25);
	TestFalse(TEXT("segmentation"), Cfg.MLTraining.bSegmentation);
	TestTrue(TEXT("no unknown keys"), Cfg.UnknownYamlKeys.IsEmpty());

	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_ML_MIN_VISIBLE_PIXELS"), TEXT("0"));
	Cfg = FCamSimConfig::LoadFromYamlString(Yaml);
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_ML_MIN_VISIBLE_PIXELS"), TEXT(""));
	TestEqual(TEXT("env 0 clamps to 1"), Cfg.MLTraining.MinVisiblePixels, 1);

	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_ML_SEGMENTATION_ENABLED"), TEXT("1"));
	Cfg = FCamSimConfig::LoadFromYamlString(Yaml);
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_ML_SEGMENTATION_ENABLED"), TEXT(""));
	TestTrue(TEXT("env enables segmentation"), Cfg.MLTraining.bSegmentation);

	const FCamSimConfig Def;
	TestEqual(TEXT("default min_visible_pixels"), Def.MLTraining.MinVisiblePixels, 1);
	TestTrue(TEXT("default segmentation"), Def.MLTraining.bSegmentation);
	return true;
}

// I1 (final review): box3d comes from the shown meshes only — a non-mesh primitive (stands in for rotor wash /
// smoke particles), an empty mesh slot and a hidden mesh never widen it; nested attachment is followed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthMeshLocalBoxTest, "CamSim.GroundTruth.Box3D.MeshesOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGroundTruthMeshLocalBoxTest::RunTest(const FString&)
{
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));   // 100 cm, centred
	if (!TestNotNull(TEXT("engine cube"), Cube)) return false;

	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);

	AActor* A = World->SpawnActor<AActor>();
	USceneComponent* Root = NewObject<USceneComponent>(A);
	A->SetRootComponent(Root);
	Root->RegisterComponent();
	A->SetActorLocationAndRotation(FVector(5000.0, -2000.0, 300.0), FRotator(0.0, 37.0, 0.0));   // actor pose must not leak in

	auto AddMesh = [&](USceneComponent* Parent, UStaticMesh* Mesh, const FVector& At, const FVector& Scale)
	{
		UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(A);
		C->SetStaticMesh(Mesh);
		C->SetupAttachment(Parent);
		C->SetRelativeLocation(At);
		C->SetRelativeScale3D(Scale);
		C->RegisterComponent();
		return C;
	};
	// Body: a 4 x 2 x 1 m box under a nested scene component offset 1 m forward, raised 0.5 m.
	USceneComponent* Mid = NewObject<USceneComponent>(A);
	Mid->SetupAttachment(Root);
	Mid->SetRelativeLocation(FVector(100.0, 0.0, 0.0));
	Mid->RegisterComponent();
	AddMesh(Mid, Cube, FVector(0.0, 0.0, 50.0), FVector(4.0, 2.0, 1.0));
	// A big non-mesh primitive (particles stand-in), an empty mesh slot far away, a hidden mesh far away.
	UBoxComponent* Wash = NewObject<UBoxComponent>(A);
	Wash->SetupAttachment(Root);
	Wash->SetBoxExtent(FVector(2000.0));
	Wash->RegisterComponent();
	AddMesh(Root, nullptr, FVector(-3000.0, 0.0, 0.0), FVector(1.0));
	AddMesh(Root, Cube, FVector(0.0, 3000.0, 0.0), FVector(1.0))->SetVisibility(false);

	const FBox B = ACamSimEntity::ComputeMeshLocalBox(*A);
	TestTrue(TEXT("valid"), B.IsValid != 0);
	TestTrue(*FString::Printf(TEXT("min (-100,-100,0) got %s"), *B.Min.ToString()), B.Min.Equals(FVector(-100.0, -100.0, 0.0), 0.01));
	TestTrue(*FString::Printf(TEXT("max (300,100,100) got %s"), *B.Max.ToString()), B.Max.Equals(FVector(300.0, 100.0, 100.0), 0.01));
	// The old source (every primitive) is what the review caught: the particles stand-in widens it.
	const FBox Old = A->CalculateComponentsBoundingBoxInLocalSpace(/*bNonColliding=*/true);
	TestTrue(TEXT("all-primitive box is wider (the bug)"), Old.GetSize().X > 3000.0);

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

// I2 (final review): the still-water plane InstanceIdCS cuts with — sea level (geoid + tide, no waves) at the
// ocean mesh's centre (frame centre when valid, else nadir), normal = local up; invalid without a sea level.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthStillWaterPlaneTest, "CamSim.GroundTruth.StillWaterPlane.GeoidTideAtCentre",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGroundTruthStillWaterPlaneTest::RunTest(const FString&)
{
	// Geoid -32 m south of 37.801, -30 m north of it: tells the frame centre from the nadir.
	FOceanSurface Ocean([](double Lat, double) { return TOptional<double>(Lat >= 37.801 ? -30.0 : -32.0); });
	Ocean.SetBeaufort(6.0, 270.0, 0.5);   // waves must not move the still-water plane
	Ocean.SetTideOffsetM(1.5);
	// A flat stand-in for the georeference: east = +X, north = -Y (UE), up = +Z, metres -> cm.
	auto Flat = [](double Lat, double Lon, double AltM, FVector& Out)
	{
		Out = FVector((Lon + 122.4) * 1.0e7, -(Lat - 37.8) * 1.0e7, AltM * 100.0);
		return true;
	};
	const FStillWaterPlane Fc = CamSimGroundTruth::ComputeStillWaterPlane(Ocean, 37.8, -122.4, 200.0, 37.802, -122.4, true, 400.0, Flat);
	TestTrue(TEXT("frame centre: valid"), Fc.bValid);
	TestNearlyEqual(TEXT("frame centre: height = geoid + tide there"), Fc.Point.Z, -2850.0, 1e-6);
	TestNearlyEqual(TEXT("frame centre: at the frame centre"), Fc.Point.Y, -20000.0, 1e-3);
	TestTrue(TEXT("normal = up"), Fc.Normal.Equals(FVector::UpVector, 1e-9));

	const FStillWaterPlane Nadir = CamSimGroundTruth::ComputeStillWaterPlane(Ocean, 37.8, -122.4, 200.0, 0.0, 0.0, false, 400.0, Flat);
	TestTrue(TEXT("nadir: valid"), Nadir.bValid);
	TestNearlyEqual(TEXT("nadir: height = geoid + tide at the nadir"), Nadir.Point.Z, -3050.0, 1e-6);

	FOceanSurface NoGrid([](double, double) { return TOptional<double>(); });
	TestFalse(TEXT("no sea level: no cut"), CamSimGroundTruth::ComputeStillWaterPlane(NoGrid, 37.8, -122.4, 200.0, 0.0, 0.0, false, 400.0, Flat).bValid);
	auto NoGeo = [](double, double, double, FVector&) { return false; };
	TestFalse(TEXT("no georeference: no cut"), CamSimGroundTruth::ComputeStillWaterPlane(Ocean, 37.8, -122.4, 200.0, 0.0, 0.0, false, 400.0, NoGeo).bValid);
	return true;
}

// Task-3 gap (final review I3 depends on it): ProjectOrientedBox's corner order under yaw. A nadir camera
// (pitch -90, yaw 0: image right = world +Y, image up = world +X) over boxes yawed 90° and 30°; every corner
// must land where body rear/front (X), left/right (Y), bottom/top (Z) predicts through an independent pinhole.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthBox3DYawCornerOrderTest, "CamSim.GroundTruth.Box3D.YawCornerOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGroundTruthBox3DYawCornerOrderTest::RunTest(const FString&)
{
	constexpr int32 W = 1920, H = 1080;
	constexpr double HFov = 60.0, CamZ = 10000.0;
	const FMatrix VP = FEntityProjection::BuildViewProjectionMatrix(FVector(0, 0, CamZ), FRotator(-90.0, 0.0, 0.0), HFov, W, H);
	const double F = 0.5 * W / FMath::Tan(FMath::DegreesToRadians(0.5 * HFov));
	const FBox Local(FVector(-300, -100, 0), FVector(300, 100, 250));   // 6 x 2 x 2.5 m, X forward, Y right
	// Semantic corners (rear = -X, left = -Y, bottom = -Z), in the documented order.
	struct FSem { double X, Y, Z; const TCHAR* Name; };
	const FSem Sem[8] = {
		{-300, -100, 0, TEXT("bottom rear-left")}, {-300, 100, 0, TEXT("bottom rear-right")},
		{ 300,  100, 0, TEXT("bottom front-right")}, { 300, -100, 0, TEXT("bottom front-left")},
		{-300, -100, 250, TEXT("top rear-left")}, {-300, 100, 250, TEXT("top rear-right")},
		{ 300,  100, 250, TEXT("top front-right")}, { 300, -100, 250, TEXT("top front-left")} };
	for (const double Yaw : { 90.0, 30.0 })
	{
		const FTransform At(FRotator(0.0, Yaw, 0.0), FVector(500.0, -300.0, 0.0));
		const FProjectedBox3D P = FEntityProjection::ProjectOrientedBox(Local, At, VP, W, H, 0.0f, 0.0f, 0.0f);
		if (!TestTrue(*FString::Printf(TEXT("yaw %g: valid"), Yaw), P.bValid)) continue;
		for (int32 K = 0; K < 8; ++K)
		{
			const FVector Wp = At.TransformPosition(FVector(Sem[K].X, Sem[K].Y, Sem[K].Z));
			const double Depth = CamZ - Wp.Z;
			const FVector2D Expect(0.5 * W + F * Wp.Y / Depth, 0.5 * H - F * Wp.X / Depth);
			TestTrue(*FString::Printf(TEXT("yaw %g: corner %d (%s) at %s, predicted %s"), Yaw, K, Sem[K].Name,
				*P.Corners[K].ToString(), *Expect.ToString()), P.Corners[K].Equals(Expect, 0.05));
		}
		if (Yaw == 90.0)
		{
			// Hand check: heading +Y (image right) — front corners right of rear ones; left side (world +X) on top.
			TestTrue(TEXT("yaw 90: front right of rear"), P.Corners[2].X > P.Corners[1].X && P.Corners[3].X > P.Corners[0].X);
			TestTrue(TEXT("yaw 90: left above right"), P.Corners[0].Y < P.Corners[1].Y && P.Corners[3].Y < P.Corners[2].Y);
		}
		// The I3 axis (rear-face centre -> front-face centre) points along the heading in the image.
		const FVector2D Axis = 0.25 * (P.Corners[2] + P.Corners[3] + P.Corners[6] + P.Corners[7])
			- 0.25 * (P.Corners[0] + P.Corners[1] + P.Corners[4] + P.Corners[5]);
		const double ImageDeg = FMath::RadiansToDegrees(FMath::Atan2(Axis.Y, Axis.X));
		TestNearlyEqual(*FString::Printf(TEXT("yaw %g: image axis angle = yaw - 90"), Yaw), ImageDeg, Yaw - 90.0, 0.5);
	}
	return true;
}

// M1 (final review): the collector reads only what Open() cached — a hot reload that changes the config while
// frames are in flight (capture size, min_visible_pixels, segmentation) must not change how they are measured.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGtCollectorCachedConfigTest, "CamSim.GroundTruth.Collector.CachedAtOpen",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGtCollectorCachedConfigTest::RunTest(const FString&)
{
	const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("gt_cached_test"));
	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	FCamSimConfig Cfg;
	Cfg.MLTraining.bEnabled = true; Cfg.MLTraining.OutputDir = Dir;
	Cfg.MLTraining.bBoundingBoxes = true; Cfg.MLTraining.bCocoExport = true;
	Cfg.MLTraining.bVocExport = false; Cfg.MLTraining.bDepthMap = false;
	Cfg.MLTraining.MinVisiblePixels = 1; Cfg.MLTraining.bSegmentation = true;
	Cfg.CaptureWidth = 6; Cfg.CaptureHeight = 4;
	FInstanceIdImage I = GtMakeImage(6, 4);
	for (int32 Y = 1; Y <= 2; ++Y) for (int32 X = 1; X <= 3; ++X) GtSet(I, X, Y, 7, 7);
	{
		FGroundTruthCollector Collector(Cfg);
		if (!TestTrue(TEXT("opened"), Collector.Open())) return false;
		// "Hot reload" after Open: every field the task thread used to read live.
		Cfg.CaptureWidth = 1920; Cfg.CaptureHeight = 1080;
		Cfg.MLTraining.MinVisiblePixels = 1000; Cfg.MLTraining.bSegmentation = false;
		FCamSimTelemetry Tel;
		Collector.WriteAnnotationFrame({ GtEntity(7) }, &I, Tel, 0);
		Collector.Close();
	}
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *Dir, TEXT("*.jsonl"), true, false);
	if (!TestEqual(TEXT("one COCO file"), Files.Num(), 1)) return false;
	FString Text;
	FFileHelper::LoadFileToString(Text, *Files[0]);
	TestTrue(TEXT("still measured from the IDs (cached size)"), Text.Contains(TEXT("\"mask_source\":\"render\"")));
	TestTrue(TEXT("not dropped (cached min_visible_pixels)"), Text.Contains(TEXT("\"entity_id\":70000")));
	TestTrue(TEXT("segmentation still written (cached)"), Text.Contains(TEXT("\"segmentation\"")));
	return true;
}
