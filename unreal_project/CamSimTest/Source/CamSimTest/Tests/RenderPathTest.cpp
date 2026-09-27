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
#include "Camera/CamSimRenderPath.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
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

// Each render grabs the newest request: it was enqueued during this tick, so
// it's for the frame being drawn now. Older requests were for frames that
// never rendered; they're dropped (their slots time out), never grabbed late.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFrameGrabQueueTest,
	"CamSim.Render.FrameGrab.RequestQueue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFrameGrabQueueTest::RunTest(const FString& Parameters)
{
	FFrameGrabRequestQueue Q;
	FFrameGrabRequest Out;
	TestFalse(TEXT("empty queue pops nothing"), Q.PopLatest(Out));

	Q.Push({ 10, 1, 0 });
	TestTrue(TEXT("one request"), Q.PopLatest(Out));
	TestEqual(TEXT("that request"), Out.FrameIndex, (uint64)10);
	TestFalse(TEXT("grabbed once"), Q.PopLatest(Out));

	Q.Push({ 11, 2, 1 });  // its frame never rendered
	Q.Push({ 12, 3, 2 });
	TestTrue(TEXT("pops"), Q.PopLatest(Out));
	TestEqual(TEXT("newest request wins"), Out.FrameIndex, (uint64)12);
	TestEqual(TEXT("older request dropped"), Q.Num(), 0);
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

// A pose jump beyond either threshold in one frame resets TSR history. This
// covers CIGI teleports and Cesium origin rebases, which move the camera's UE
// location by kilometres even though the view doesn't change.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraCutThresholdTest,
	"CamSim.Render.CameraCut.Thresholds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraCutThresholdTest::RunTest(const FString& Parameters)
{
	using CamSimRender::ShouldCutCamera;
	const FQuat Level = FRotator(-30.0, 0.0, 0.0).Quaternion();
	const FVector Origin(0.0);
	const double DistM = 500.0, AngDeg = 30.0;

	TestFalse(TEXT("still"), ShouldCutCamera(Origin, Level, Origin, Level, DistM, AngDeg));
	// 100 m/s at 30 fps = 3.3 m per frame: normal flight.
	TestFalse(TEXT("normal flight"), ShouldCutCamera(Origin, Level, FVector(333.0, 0, 0), Level, DistM, AngDeg));
	// 60 deg/s gimbal slew = 2 deg per frame.
	TestFalse(TEXT("fast slew"), ShouldCutCamera(Origin, Level, Origin, FRotator(-30.0, 2.0, 0.0).Quaternion(), DistM, AngDeg));
	// Teleport 300 km.
	TestTrue(TEXT("teleport"), ShouldCutCamera(Origin, Level, FVector(3.0e7, 0, 0), Level, DistM, AngDeg));
	// Origin rebase: the camera's UE location jumps back toward zero.
	TestTrue(TEXT("rebase"), ShouldCutCamera(FVector(2.0e6, -1.5e6, 3.0e5), Level, FVector(0, 0, 3.0e5), Level, DistM, AngDeg));
	// Snap the view 90 deg.
	TestTrue(TEXT("view snap"), ShouldCutCamera(Origin, Level, Origin, FRotator(-30.0, 90.0, 0.0).Quaternion(), DistM, AngDeg));
	// Just under / just over the distance threshold (cm).
	TestFalse(TEXT("499 m"), ShouldCutCamera(Origin, Level, FVector(49900.0, 0, 0), Level, DistM, AngDeg));
	TestTrue(TEXT("501 m"), ShouldCutCamera(Origin, Level, FVector(50100.0, 0, 0), Level, DistM, AngDeg));
	// A non-positive threshold skips that check instead of cutting every frame
	// (which would silently turn TSR into no anti-aliasing).
	TestFalse(TEXT("zero distance: normal flight"), ShouldCutCamera(Origin, Level, FVector(333.0, 0, 0), Level, 0.0, AngDeg));
	TestFalse(TEXT("negative angle: fast slew"), ShouldCutCamera(Origin, Level, Origin, FRotator(-30.0, 2.0, 0.0).Quaternion(), DistM, -1.0));
	TestTrue(TEXT("zero distance: angle still cuts"), ShouldCutCamera(Origin, Level, Origin, FRotator(-30.0, 90.0, 0.0).Quaternion(), 0.0, AngDeg));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraCutValidateTest,
	"CamSim.Render.CameraCut.ValidateRejectsNonPositive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraCutValidateTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	TestEqual(TEXT("defaults valid"), Cfg.Validate().Num(), 0);
	Cfg.Render.CameraCutDistanceM = 0.0f;
	Cfg.Render.CameraCutAngleDeg  = -5.0f;
	const FString All = FString::Join(Cfg.Validate(), TEXT("\n"));
	TestTrue(TEXT("distance reported"), All.Contains(TEXT("camera_cut_distance_m")));
	TestTrue(TEXT("angle reported"), All.Contains(TEXT("camera_cut_angle_deg")));
	return true;
}

// The engine updates player cameras BEFORE TG_PostUpdateWork, where the sensor
// applies gimbal and FOV. Without a refresh the primary view renders last
// frame's pose while KLV describes this frame's (final review, 3A).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPrimaryViewRefreshTest,
	"CamSim.Render.PrimaryView.RefreshUsesFinalPose",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPrimaryViewRefreshTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);

	APlayerController* PC = World->SpawnActor<APlayerController>();
	AActor* Sensor = World->SpawnActor<AActor>();
	UCameraComponent* Cam = NewObject<UCameraComponent>(Sensor);
	Sensor->SetRootComponent(Cam);
	Cam->RegisterComponent();
	Cam->Activate();  // bAutoActivate does this once play begins; this world never begins play
	Cam->SetFieldOfView(60.0f);

	// A bare test world has no local player: spawn the camera manager and let
	// it update without one (a real run's PC is local, so it always updates).
	if (PC && !PC->PlayerCameraManager) PC->SpawnPlayerCameraManager();
	if (PC && PC->PlayerCameraManager) PC->PlayerCameraManager->bUseClientSideCameraUpdates = false;
	bool bOk = TestNotNull(TEXT("camera manager"), PC ? PC->PlayerCameraManager.Get() : nullptr);
	if (bOk)
	{
		PC->SetViewTarget(Sensor);
		PC->PlayerCameraManager->UpdateCamera(0.0f);  // the engine's update, early in the frame

		// The sensor tick (TG_PostUpdateWork) then slews and zooms.
		Cam->SetWorldRotation(FRotator(-30.0, 90.0, 0.0));
		Cam->SetFieldOfView(20.0f);
		TestFalse(TEXT("cache is stale before the refresh"),
			PC->PlayerCameraManager->GetCameraCacheView().Rotation.Equals(FRotator(-30.0, 90.0, 0.0), 0.01));

		CamSimRender::RefreshPlayerView(PC, 0.0f);
		const FMinimalViewInfo& View = PC->PlayerCameraManager->GetCameraCacheView();
		TestTrue(TEXT("rendered rotation = final gimbal"), View.Rotation.Equals(FRotator(-30.0, 90.0, 0.0), 0.01));
		TestEqual(TEXT("rendered FOV = final FOV"), View.FOV, 20.0f);

		PC->PlayerCameraManager->SetGameCameraCutThisFrame();
		CamSimRender::RefreshPlayerView(PC, 0.0f);
		TestTrue(TEXT("refresh keeps a cut requested this frame"), PC->PlayerCameraManager->bGameCameraCutThisFrame);
	}

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

// Readback polling: a slot's fence is only trusted once the grab for THIS
// capture was issued (a fence left signalled by an earlier cycle would hand
// back a frame from three captures ago), and a grab that never happens times
// out instead of freezing the pipeline in DMAQueued (final review, 3A).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadbackPollDecisionTest,
	"CamSim.Render.FrameGrab.PollDecision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReadbackPollDecisionTest::RunTest(const FString& Parameters)
{
	using namespace CamSimReadback;
	constexpr uint32 Max = 60;
	int32 FenceChecks = 0;
	auto Ready    = [&FenceChecks]() { ++FenceChecks; return true; };
	auto NotReady = [&FenceChecks]() { ++FenceChecks; return false; };

	// SceneCapture: no grab step; the fence decides.
	TestTrue(TEXT("capture: ready consumes"), DecidePoll(false, 0, 7, Ready, 1, Max) == EPollDecision::Consume);
	TestTrue(TEXT("capture: not ready waits"), DecidePoll(false, 0, 7, NotReady, 1, Max) == EPollDecision::Wait);

	// Primary: grab for this capture (gen 7) not issued yet; old fence says ready.
	FenceChecks = 0;
	TestTrue(TEXT("stale fence from an earlier cycle is not consumed"),
		DecidePoll(true, /*Grabbed*/ 4, /*Capture*/ 7, Ready, 1, Max) == EPollDecision::Wait);
	TestEqual(TEXT("fence not even checked before the grab"), FenceChecks, 0);

	TestTrue(TEXT("grabbed + ready consumes"), DecidePoll(true, 7, 7, Ready, 2, Max) == EPollDecision::Consume);
	TestTrue(TEXT("grabbed, copy in flight waits"), DecidePoll(true, 7, 7, NotReady, 2, Max) == EPollDecision::Wait);

	// Never grabbed (viewport not drawn, request dropped): give up.
	TestTrue(TEXT("budget spent times out"), DecidePoll(true, 4, 7, Ready, Max, Max) == EPollDecision::TimedOut);
	TestTrue(TEXT("stuck fence times out too"), DecidePoll(false, 0, 7, NotReady, Max, Max) == EPollDecision::TimedOut);
	TestTrue(TEXT("a ready frame on the last attempt still wins"), DecidePoll(true, 7, 7, Ready, Max, Max) == EPollDecision::Consume);
	return true;
}

// Exposure compensation is relative to the engine's default bias (+1 EV in
// UE 5.8 with the extended luminance range): 0 must leave UE's metering
// unchanged, -1 must be one stop darker than it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExposureBiasTest,
	"CamSim.Render.Exposure.CompensationIsRelativeToEngineDefault",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExposureBiasTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("0 = engine default"), CamSimRender::AutoExposureBias(1.0f, 0.0f), 1.0f);
	TestEqual(TEXT("-1 = one stop below the default"), CamSimRender::AutoExposureBias(1.0f, -1.0f), 0.0f);
	TestEqual(TEXT("follows a different project default"), CamSimRender::AutoExposureBias(0.5f, -1.0f), -0.5f);
	return true;
}
