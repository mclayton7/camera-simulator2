// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Control/ControlCaptureGate.h"

// -------------------------------------------------------------------------
// FControlCaptureGate (ROADMAP 2.4): a capture waits for terrain, tiles and
// vegetation, then a fixed number of rendered frames with no command, then
// fires once; or times out once.
// -------------------------------------------------------------------------

namespace
{
	using EState = FControlCaptureGate::EState;

	FControlCaptureInputs Settled(double Now, uint64 Frames)
	{
		FControlCaptureInputs In;
		In.NowSec = Now;
		In.bTerrainReady = true;
		In.MinLoadProgressPct = 100.0f;
		In.bVegetationIdle = true;
		In.FramesCaptured = Frames;
		return In;
	}
}

#define CAPTURE_GATE_TEST(Class, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(Class, "CamSim.ControlApi.CaptureGate." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

CAPTURE_GATE_TEST(FCaptureGateFiresTest, "FiresAfterSettleFrames")
bool FCaptureGateFiresTest::RunTest(const FString& Parameters)
{
	FControlCaptureGate G;
	TestEqual(TEXT("idle before Begin"), G.Update(Settled(0, 100)), EState::Idle);
	G.Begin(0.0, 30, 60.0, 99.9f);
	TestEqual(TEXT("first settled tick starts settling"), G.Update(Settled(0.0, 100)), EState::Settling);
	for (uint64 F = 101; F < 130; ++F)
	{
		if (G.Update(Settled(0.0, F)) != EState::Settling)
		{
			AddError(FString::Printf(TEXT("left Settling early at frame %llu"), F));
			return false;
		}
	}
	TestEqual(TEXT("fires on the 30th frame"), G.Update(Settled(1.0, 130)), EState::Fire);
	TestEqual(TEXT("frames waited"), G.FramesWaited(), 30);
	TestEqual(TEXT("fires once, then idle until the next Begin"), G.Update(Settled(1.1, 131)), EState::Idle);
	G.Begin(2.0, 30, 60.0, 99.9f);
	G.Reset();
	TestEqual(TEXT("idle after Reset"), G.GetState(), EState::Idle);
	return true;
}

CAPTURE_GATE_TEST(FCaptureGateTilesTest, "WaitsForTiles")
bool FCaptureGateTilesTest::RunTest(const FString& Parameters)
{
	FControlCaptureGate G;
	G.Begin(0.0, 0, 60.0, 99.9f);
	FControlCaptureInputs In = Settled(0.0, 10);
	In.MinLoadProgressPct = 99.0f;
	TestEqual(TEXT("99.0 < 99.9: waiting"), G.Update(In), EState::Waiting);
	TestTrue(TEXT("reason names tiles"), G.PendingReason().Contains(TEXT("tiles")));
	In.MinLoadProgressPct = -1.0f;
	TestEqual(TEXT("no tilesets counts as loaded"), G.Update(In), EState::Fire);
	return true;
}

CAPTURE_GATE_TEST(FCaptureGateTerrainVegTest, "WaitsForTerrainAndVegetation")
bool FCaptureGateTerrainVegTest::RunTest(const FString& Parameters)
{
	FControlCaptureGate G;
	G.Begin(0.0, 0, 60.0, 99.9f);
	FControlCaptureInputs In = Settled(0.0, 10);
	In.bTerrainReady = false;
	TestEqual(TEXT("terrain gate closed: waiting"), G.Update(In), EState::Waiting);
	TestTrue(TEXT("reason names terrain"), G.PendingReason().Contains(TEXT("terrain")));
	In.bTerrainReady = true;
	In.bVegetationIdle = false;
	TestEqual(TEXT("vegetation streaming: waiting"), G.Update(In), EState::Waiting);
	TestTrue(TEXT("reason names vegetation"), G.PendingReason().Contains(TEXT("vegetation")));
	In.bVegetationIdle = true;
	TestEqual(TEXT("all settled, 0 frames: fire"), G.Update(In), EState::Fire);
	return true;
}

CAPTURE_GATE_TEST(FCaptureGateCommandTest, "ResetsOnCommand")
bool FCaptureGateCommandTest::RunTest(const FString& Parameters)
{
	FControlCaptureGate G;
	G.Begin(0.0, 30, 60.0, 99.9f);
	G.Update(Settled(0.0, 0));
	for (uint64 F = 1; F <= 20; ++F) G.Update(Settled(0.0, F));
	FControlCaptureInputs Cmd = Settled(0.0, 21);
	Cmd.bCommandThisTick = true;
	TestEqual(TEXT("a command drops back to waiting"), G.Update(Cmd), EState::Waiting);
	TestEqual(TEXT("restarts settling"), G.Update(Settled(0.0, 22)), EState::Settling);
	EState Last = EState::Settling;
	uint64 F = 23;
	for (; F < 60 && Last == EState::Settling; ++F) Last = G.Update(Settled(0.0, F));
	TestEqual(TEXT("fires 30 frames after the command, not 30 after Begin"), F - 1, static_cast<uint64>(52));
	TestEqual(TEXT("fired"), Last, EState::Fire);
	return true;
}

CAPTURE_GATE_TEST(FCaptureGateZeroTest, "ZeroSettleFiresImmediately")
bool FCaptureGateZeroTest::RunTest(const FString& Parameters)
{
	FControlCaptureGate G;
	G.Begin(0.0, 0, 60.0, 99.9f);
	TestEqual(TEXT("fires on the first settled tick"), G.Update(Settled(0.0, 5)), EState::Fire);
	return true;
}

CAPTURE_GATE_TEST(FCaptureGateTimeoutTest, "TimesOutOnce")
bool FCaptureGateTimeoutTest::RunTest(const FString& Parameters)
{
	FControlCaptureGate G;
	G.Begin(10.0, 30, 5.0, 99.9f);
	FControlCaptureInputs In = Settled(12.0, 0);
	In.bTerrainReady = false;
	TestEqual(TEXT("still waiting at 2 s"), G.Update(In), EState::Waiting);
	In.NowSec = 15.0;
	TestEqual(TEXT("times out at 5 s"), G.Update(In), EState::TimedOut);
	TestTrue(TEXT("reason kept for the 503"), G.PendingReason().Contains(TEXT("terrain")));
	In.NowSec = 16.0;
	TestEqual(TEXT("then idle, not TimedOut again"), G.Update(In), EState::Idle);
	return true;
}

CAPTURE_GATE_TEST(FCaptureGateReasonTest, "ReasonText")
bool FCaptureGateReasonTest::RunTest(const FString& Parameters)
{
	FControlCaptureGate G;
	G.Begin(0.0, 30, 60.0, 99.9f);
	G.Update(Settled(0.0, 0));
	G.Update(Settled(0.0, 12));
	TestEqual(TEXT("settling progress"), G.PendingReason(), FString(TEXT("settling 12/30")));
	return true;
}

#undef CAPTURE_GATE_TEST
