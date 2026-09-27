// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Camera/CamSimFrameStats.h"
#include "Camera/FrameGrabRequestQueue.h"
#include "Camera/CamSimStreamingController.h"
#include "Config/CamSimConfig.h"

// -------------------------------------------------------------------------
// ROADMAP 3A: frame-stats rows are valid JSON with the keys the bench
// harness reads, and a bad path turns stats off instead of failing.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFrameStatsRowTest,
	"CamSim.Render.FrameStats.RowFormat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFrameStatsRowTest::RunTest(const FString& Parameters)
{
	FCamSimFrameStatsSample S;
	S.UtcSeconds = 1790000000.25;
	S.WallMs = 33.5; S.GameMs = 4.0; S.RenderMs = 8.0; S.RhiMs = 2.0; S.GpuMs = 21.0;
	S.FramesEmitted = 42; S.FramesDropped = 1;
	S.MinLoadProgressPct = 87.5f; S.Sse = 16.0; S.bCameraCut = true; S.ViewFamilies = 2;

	const FString Row = CamSimFormatFrameStatsRow(S);
	TestFalse(TEXT("no newline"), Row.Contains(TEXT("\n")));

	TSharedPtr<FJsonObject> Obj;
	if (!TestTrue(TEXT("valid JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Row), Obj) && Obj.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("t"), Obj->GetNumberField(TEXT("t")), 1790000000.25);
	TestEqual(TEXT("wall_ms"), Obj->GetNumberField(TEXT("wall_ms")), 33.5);
	TestEqual(TEXT("gpu_ms"), Obj->GetNumberField(TEXT("gpu_ms")), 21.0);
	TestEqual(TEXT("emitted"), static_cast<int32>(Obj->GetNumberField(TEXT("emitted"))), 42);
	TestEqual(TEXT("dropped"), static_cast<int32>(Obj->GetNumberField(TEXT("dropped"))), 1);
	TestEqual(TEXT("load_pct"), Obj->GetNumberField(TEXT("load_pct")), 87.5);
	TestEqual(TEXT("sse"), Obj->GetNumberField(TEXT("sse")), 16.0);
	TestTrue(TEXT("cut"), Obj->GetBoolField(TEXT("cut")));
	TestEqual(TEXT("families"), static_cast<int32>(Obj->GetNumberField(TEXT("families"))), 2);
	for (const TCHAR* Key : { TEXT("game_ms"), TEXT("render_ms"), TEXT("rhi_ms") })
	{
		TestTrue(FString::Printf(TEXT("has %s"), Key), Obj->HasField(Key));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFrameStatsRecorderTest,
	"CamSim.Render.FrameStats.RecorderWritesLines",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFrameStatsRecorderTest::RunTest(const FString& Parameters)
{
	const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Tests"), TEXT("frame_stats_test.jsonl"));
	IFileManager::Get().Delete(*Path);

	FCamSimFrameStatsRecorder Recorder;
	TestTrue(TEXT("opens"), Recorder.Open(Path));
	FCamSimFrameStatsSample S;
	for (int32 I = 0; I < 3; ++I) { S.FramesEmitted = I; Recorder.Record(S); }
	Recorder.Close();
	TestFalse(TEXT("closed"), Recorder.IsOpen());

	TArray<FString> Lines;
	TestTrue(TEXT("readable"), FFileHelper::LoadFileToStringArray(Lines, *Path));
	TestEqual(TEXT("three rows"), Lines.Num(), 3);
	IFileManager::Get().Delete(*Path);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFrameStatsOpenFailureTest,
	"CamSim.Render.FrameStats.OpenFailureIsSoft",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFrameStatsOpenFailureTest::RunTest(const FString& Parameters)
{
	AddExpectedMessage(TEXT("frame stats disabled"), EAutomationExpectedErrorFlags::Contains, 1);
	FCamSimFrameStatsRecorder Recorder;
	TestFalse(TEXT("unwritable path fails"), Recorder.Open(TEXT("/nonexistent-root-dir/camsim/frames.jsonl")));
	FCamSimFrameStatsSample S;
	Recorder.Record(S);  // must be a silent no-op
	TestFalse(TEXT("still closed"), Recorder.IsOpen());
	return true;
}

// The render thread grabs each requested frame exactly once, oldest first, and
// never a request the game thread has already given up on.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFrameGrabQueueTest,
	"CamSim.Render.FrameGrab.RequestQueue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFrameGrabQueueTest::RunTest(const FString& Parameters)
{
	FFrameGrabRequestQueue Q;
	FFrameGrabRequest Out;
	TestFalse(TEXT("empty queue pops nothing"), Q.PopCurrent(1, Out));

	Q.Push({ 10, 1, 0 });
	Q.Push({ 11, 2, 1 });
	Q.Push({ 12, 3, 2 });

	TestTrue(TEXT("pops current"), Q.PopCurrent(3, Out));
	TestEqual(TEXT("stale generations 1-2 dropped, 3 returned"), Out.FrameIndex, (uint64)12);
	TestEqual(TEXT("queue drained"), Q.Num(), 0);

	Q.Push({ 20, 4, 0 });
	Q.Push({ 21, 4, 1 });
	TestTrue(TEXT("first of same generation"), Q.PopCurrent(4, Out));
	TestEqual(TEXT("FIFO"), Out.FrameIndex, (uint64)20);
	TestTrue(TEXT("second"), Q.PopCurrent(4, Out));
	TestEqual(TEXT("FIFO second"), Out.FrameIndex, (uint64)21);
	TestFalse(TEXT("each request grabbed once"), Q.PopCurrent(4, Out));
	return true;
}

// Primary view: Cesium already streams for the player camera, so only the
// inflated prefetch camera is registered. SceneCapture needs both.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStreamingCamerasTest,
	"CamSim.Render.Streaming.CamerasPerViewSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStreamingCamerasTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	TestEqual(TEXT("primary: prefetch only"), FCamSimStreamingController::NumStreamingCameras(Cfg), 1);
	Cfg.Render.ViewSourceMode = FCamSimConfig::FRenderConfig::EViewSource::SceneCapture;
	TestEqual(TEXT("scene capture: primary + prefetch"), FCamSimStreamingController::NumStreamingCameras(Cfg), 2);
	return true;
}
