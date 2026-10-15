// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Modules/ModuleManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Control/CapturePng.h"
#include "GroundTruth/FCocoAnnotationWriter.h"
#include "GroundTruth/FDepthMapWriter.h"
#include "Metadata/CamSimTelemetry.h"

// -------------------------------------------------------------------------
// Control API capture (ROADMAP 2.4): the record, depth PNG and image PNG a
// capture returns are what the ground-truth files contain.
// -------------------------------------------------------------------------

namespace
{
	TArray<FEntityAnnotationData> TwoEntities()
	{
		TArray<FEntityAnnotationData> E;
		FEntityAnnotationData M;
		M.EntityId = 3; M.EntityType = 2001; M.ClassName = TEXT("truck"); M.Source = TEXT("api"); M.SourceId = TEXT("7");
		M.ScreenBBox = FBox2D(FVector2D(10, 20), FVector2D(40, 50));
		M.bVisible = true; M.bMaskMeasured = true; M.VisiblePixels = 700; M.AmodalPixels = 800;
		M.AmodalBBox = FBox2D(FVector2D(9, 19), FVector2D(41, 52));
		M.Obb = { 25.f, 35.f, 30.f, 30.f, 12.f };
		M.ObbAmodal = { 25.f, 35.f, 32.f, 33.f, 12.f };
		M.SegmentationRle = TEXT("abc\"d");
		M.Truncation = 0.0;
		M.bHasBox3D = true; M.Box3DSizeM = FVector(7, 2.5, 3); M.YawDeg = 90; M.bCornersValid = true;
		for (int32 C = 0; C < 8; ++C) M.CornersPx[C] = FVector2D(C, C + 1);
		M.bHasGeo = true; M.Lat = 33.3; M.Lon = -117.3; M.AltM = 40.25;
		E.Add(M);
		FEntityAnnotationData P;
		P.EntityId = 4; P.EntityType = 3001; P.ClassName = TEXT("boat"); P.Source = TEXT("api"); P.SourceId = TEXT("8");
		P.ScreenBBox = FBox2D(FVector2D(100, 100), FVector2D(110, 120));
		P.bVisible = true;
		E.Add(P);
		return E;
	}

	bool DecodePng(const TArray64<uint8>& Png, ERGBFormat Format, int32 BitDepth, TArray64<uint8>& OutRaw, int32& W, int32& H)
	{
		IImageWrapperModule& M = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> Wr = M.CreateImageWrapper(EImageFormat::PNG);
		if (!Wr || !Wr->SetCompressed(Png.GetData(), Png.Num())) return false;
		W = Wr->GetWidth();
		H = Wr->GetHeight();
		return Wr->GetRaw(Format, BitDepth, OutRaw);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlCaptureRecordTest,
	"CamSim.ControlApi.Capture.RecordMatchesJsonl",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FControlCaptureRecordTest::RunTest(const FString& Parameters)
{
	FCamSimTelemetry T;
	T.TimestampUs = 1234567;
	T.Latitude = 33.0; T.Longitude = -117.0; T.Altitude = 900.0;
	const TArray<FEntityAnnotationData> E = TwoEntities();

	const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("ControlCaptureRecord"));
	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	FCocoAnnotationWriter W;
	W.SetImageSize(640, 360);
	if (!TestTrue(TEXT("open"), W.Open(Dir))) return false;
	W.WriteFrame(E, T, 42);
	W.Close();
	FString File;
	TestTrue(TEXT("read jsonl"), FFileHelper::LoadFileToString(File, *FPaths::Combine(Dir, TEXT("camsim_coco.jsonl"))));

	const FString Record = FCocoAnnotationWriter::BuildFrameRecord(E, T, 42, 640, 360);
	TestEqual(TEXT("record = the JSONL line"), Record + TEXT("\n"), File);

	TSharedPtr<FJsonObject> Obj;
	TestTrue(TEXT("record is JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Record), Obj) && Obj.IsValid());
	if (Obj.IsValid())
	{
		TestEqual(TEXT("frame_id"), Obj->GetIntegerField(TEXT("frame_id")), 42);
		const TArray<TSharedPtr<FJsonValue>>& A = Obj->GetArrayField(TEXT("annotations"));
		TestEqual(TEXT("two annotations"), A.Num(), 2);
		if (A.Num() == 2)
		{
			TestEqual(TEXT("masked"), A[0]->AsObject()->GetStringField(TEXT("mask_source")), FString(TEXT("render")));
			TestEqual(TEXT("projected"), A[1]->AsObject()->GetStringField(TEXT("mask_source")), FString(TEXT("projection")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlCaptureDepthPngTest,
	"CamSim.ControlApi.Capture.DepthPngRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FControlCaptureDepthPngTest::RunTest(const FString& Parameters)
{
	const TArray<float> Depth = { 0.0f, 2500.0f, 5000.0f, 9999.0f };
	const TArray64<uint8> Png = FDepthMapWriter::EncodeDepthPng(Depth, 2, 2, 5000.0f);
	TestTrue(TEXT("encoded"), Png.Num() > 0);
	TArray64<uint8> Raw;
	int32 W = 0, H = 0;
	if (!TestTrue(TEXT("decodes"), DecodePng(Png, ERGBFormat::Gray, 16, Raw, W, H))) return false;
	TestEqual(TEXT("width"), W, 2);
	TestEqual(TEXT("height"), H, 2);
	const uint16* P = reinterpret_cast<const uint16*>(Raw.GetData());
	TestEqual(TEXT("0 m"), static_cast<int32>(P[0]), 0);
	TestTrue(TEXT("half far ~32768"), FMath::Abs(static_cast<int32>(P[1]) - 32768) <= 1);
	TestEqual(TEXT("far"), static_cast<int32>(P[2]), 65535);
	TestEqual(TEXT("beyond far clamps"), static_cast<int32>(P[3]), 65535);
	TestEqual(TEXT("wrong size: empty"), FDepthMapWriter::EncodeDepthPng(Depth, 3, 2, 5000.0f).Num(), static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlCaptureNv12PngTest,
	"CamSim.ControlApi.Capture.Nv12PngSize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FControlCaptureNv12PngTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Nv12;
	Nv12.Init(128, 4 * 2 * 3 / 2);   // mid grey, neutral chroma
	const TArray64<uint8> Png = CamSimControl::EncodeNv12Png(Nv12, 4, 2);
	TArray64<uint8> Raw;
	int32 W = 0, H = 0;
	if (!TestTrue(TEXT("decodes"), DecodePng(Png, ERGBFormat::RGBA, 8, Raw, W, H))) return false;
	TestEqual(TEXT("width"), W, 4);
	TestEqual(TEXT("height"), H, 2);
	TestTrue(TEXT("grey"), FMath::Abs(static_cast<int32>(Raw[0]) - static_cast<int32>(Raw[1])) <= 1);
	TestEqual(TEXT("wrong size: empty"), CamSimControl::EncodeNv12Png(Nv12, 8, 8).Num(), static_cast<int64>(0));
	return true;
}
