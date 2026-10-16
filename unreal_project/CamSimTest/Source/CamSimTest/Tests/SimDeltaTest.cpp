// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Time/SimClock.h"
#include "Time/SimDelta.h"

// -------------------------------------------------------------------------
// FSimDelta (ROADMAP 2.1/2.4): per-tick integration steps. Lockstep: the
// clock's advance since the last call; otherwise the engine delta.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimDeltaRealTimeTest,
	"CamSim.SimDelta.RealTimeUsesEngineDelta",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSimDeltaRealTimeTest::RunTest(const FString& Parameters)
{
	FSimClock Clock;
	double Mono = 0.0;
	Clock.SetMonotonicSource([&Mono] { return Mono; });
	FSimDelta D(&Clock);
	TestEqual(TEXT("real time: engine delta"), D.Next(1.0 / 30.0), 1.0 / 30.0);
	Clock.SetRate(0.0);
	TestEqual(TEXT("frozen clock outside lockstep: still the engine delta (fixed-sun hosts keep entities moving)"),
		D.Next(0.5), 0.5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimDeltaLockstepTest,
	"CamSim.SimDelta.LockstepUsesClockSteps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSimDeltaLockstepTest::RunTest(const FString& Parameters)
{
	FSimClock Clock;
	double Mono = 0.0;
	Clock.SetMonotonicSource([&Mono] { return Mono; });
	Clock.SetLockstep(true);
	FSimDelta D(&Clock);
	TestEqual(TEXT("first call: 0"), D.Next(1.0 / 30.0), 0.0);
	Mono += 5.0;
	TestEqual(TEXT("no step: 0 whatever the engine delta"), D.Next(1.0 / 30.0), 0.0);
	Clock.Step(2.5);
	TestTrue(TEXT("step: exactly the step"), FMath::IsNearlyEqual(D.Next(1.0 / 30.0), 2.5, 1e-6));
	TestEqual(TEXT("consumed"), D.Next(1.0 / 30.0), 0.0);
	Clock.SetUtc(FDateTime(2020, 1, 1));
	TestEqual(TEXT("a backward jump is 0, never negative"), D.Next(1.0 / 30.0), 0.0);
	Clock.SetUtc(FDateTime(2020, 1, 1, 6, 0, 0));
	TestEqual(TEXT("a forward time-of-day jump is 0 too: only Step() integrates (6 h of DR in one tick otherwise)"),
		D.Next(1.0 / 30.0), 0.0);
	Clock.Step(0.5);
	TestTrue(TEXT("steps after the jump integrate"), FMath::IsNearlyEqual(D.Next(1.0 / 30.0), 0.5, 1e-6));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimDeltaSensorStepTest,
	"CamSim.SimDelta.SensorStepSeconds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSimDeltaSensorStepTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("real time: sim delta"), CamSimTime::SensorStepSeconds(false, 0.2, 30.f), 0.2);
	TestTrue(TEXT("lockstep: one frame"), FMath::IsNearlyEqual(CamSimTime::SensorStepSeconds(true, 0.0, 30.f), 1.0 / 30.0, 1e-9));
	TestTrue(TEXT("lockstep, bad frame rate: 1/30"), FMath::IsNearlyEqual(CamSimTime::SensorStepSeconds(true, 0.0, 0.f), 1.0 / 30.0, 1e-9));
	return true;
}
