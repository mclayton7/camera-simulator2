// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Camera/ReadbackRing.h"

// -------------------------------------------------------------------------
// ROADMAP 3A.1: up to three readbacks in flight (30 fps output instead of 15),
// delivered strictly in frame order, with a full ring dropping the new frame.
// -------------------------------------------------------------------------

namespace
{
	using EState = EReadbackSlotState;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadbackRingFillTest,
	"CamSim.Render.ReadbackRing.ThreeInFlightThenFull",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReadbackRingFillTest::RunTest(const FString& Parameters)
{
	FReadbackRing Ring;
	TestTrue(TEXT("empty ring can capture"), Ring.CanAcquire());
	const int32 A = Ring.Acquire(10);
	const int32 B = Ring.Acquire(11);
	const int32 C = Ring.Acquire(12);
	TestTrue(TEXT("three distinct slots"), A != B && B != C && A != C && A >= 0 && B >= 0 && C >= 0);
	TestEqual(TEXT("three in flight"), Ring.NumInFlight(), 3);
	TestFalse(TEXT("full ring can't capture"), Ring.CanAcquire());
	TestEqual(TEXT("fourth frame is dropped"), Ring.Acquire(13), INDEX_NONE);
	TestTrue(TEXT("acquired slot is in flight"), Ring.GetState(A) == EState::InFlight);
	TestEqual(TEXT("frame index remembered"), Ring.GetFrameIndex(B), (uint64)11);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadbackRingOrderTest,
	"CamSim.Render.ReadbackRing.DeliversInFrameOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReadbackRingOrderTest::RunTest(const FString& Parameters)
{
	FReadbackRing Ring;
	const int32 A = Ring.Acquire(10);
	const int32 B = Ring.Acquire(11);

	int32 Slot = INDEX_NONE;
	bool bFailed = false;
	TestFalse(TEXT("nothing finished yet"), Ring.PeekFinished(Slot, bFailed));

	Ring.MarkComplete(B);  // the newer frame's readback lands first
	TestFalse(TEXT("newer frame waits for the older one"), Ring.PeekFinished(Slot, bFailed));

	Ring.MarkComplete(A);
	TestTrue(TEXT("oldest delivered first"), Ring.PeekFinished(Slot, bFailed));
	TestEqual(TEXT("slot A first"), Slot, A);
	TestFalse(TEXT("A succeeded"), bFailed);
	TestTrue(TEXT("peek doesn't consume"), Ring.PeekFinished(Slot, bFailed) && Slot == A);

	Ring.Release(A);
	TestTrue(TEXT("then B"), Ring.PeekFinished(Slot, bFailed));
	TestEqual(TEXT("slot B second"), Slot, B);
	Ring.Release(B);
	TestFalse(TEXT("drained"), Ring.PeekFinished(Slot, bFailed));
	TestEqual(TEXT("none in flight"), Ring.NumInFlight(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadbackRingFailedTest,
	"CamSim.Render.ReadbackRing.FailedSlotDoesNotBlock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReadbackRingFailedTest::RunTest(const FString& Parameters)
{
	FReadbackRing Ring;
	const int32 A = Ring.Acquire(10);
	const int32 B = Ring.Acquire(11);
	Ring.MarkComplete(B);
	Ring.MarkFailed(A);  // e.g. timed out: never grabbed

	int32 Slot = INDEX_NONE;
	bool bFailed = false;
	TestTrue(TEXT("failed slot is reported"), Ring.PeekFinished(Slot, bFailed));
	TestEqual(TEXT("in order: A"), Slot, A);
	TestTrue(TEXT("marked failed"), bFailed);
	Ring.Release(A);
	TestTrue(TEXT("B follows"), Ring.PeekFinished(Slot, bFailed) && Slot == B && !bFailed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadbackRingReuseTest,
	"CamSim.Render.ReadbackRing.ReleasedSlotsAreReused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FReadbackRingReuseTest::RunTest(const FString& Parameters)
{
	FReadbackRing Ring;
	// Steady state at 30 fps: capture one, deliver one, forever.
	int32 Slot = INDEX_NONE;
	bool bFailed = false;
	TArray<int32> Seen;
	for (uint64 Frame = 0; Frame < 9; ++Frame)
	{
		const int32 S = Ring.Acquire(Frame);
		if (!TestTrue(FString::Printf(TEXT("frame %llu gets a slot"), Frame), S != INDEX_NONE)) return false;
		Seen.AddUnique(S);
		Ring.MarkComplete(S);
		TestTrue(TEXT("delivered"), Ring.PeekFinished(Slot, bFailed) && Slot == S);
		TestEqual(TEXT("frame index matches"), Ring.GetFrameIndex(Slot), Frame);
		Ring.Release(Slot);
	}
	TestEqual(TEXT("all three slots used"), Seen.Num(), 3);

	// A held (complete, not yet released) oldest frame backs up the ring.
	const int32 A = Ring.Acquire(100);
	Ring.MarkComplete(A);
	Ring.Acquire(101);
	Ring.Acquire(102);
	TestFalse(TEXT("held frame + two in flight = full"), Ring.CanAcquire());
	return true;
}
