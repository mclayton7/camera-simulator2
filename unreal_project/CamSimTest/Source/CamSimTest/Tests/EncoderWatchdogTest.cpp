// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Subsystem/EncoderWatchdog.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEncoderWatchdogStallTest,
	"CamSim.Encoder.Watchdog.StallOnlyWhenFramesWereSubmitted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEncoderWatchdogStallTest::RunTest(const FString& Parameters)
{
	using CamSimWatchdog::IsStalled;
	// Terrain gate holding frames (or sensor off): nothing submitted, nothing written.
	TestFalse(TEXT("frames held back is not a stall"), IsStalled(/*PrevWritten*/ 0, 0, /*PrevSubmitted*/ 0, 0));
	TestFalse(TEXT("held after streaming is not a stall"), IsStalled(500, 500, 520, 520));
	// Frames went in, none came out: the stream died.
	TestTrue(TEXT("submitted but none written is a stall"), IsStalled(500, 500, 520, 600));
	// Normal streaming.
	TestFalse(TEXT("frames flowing"), IsStalled(500, 580, 520, 600));
	return true;
}
