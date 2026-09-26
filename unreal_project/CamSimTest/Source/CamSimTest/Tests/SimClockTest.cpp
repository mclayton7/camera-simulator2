// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Time/SimClock.h"

// -------------------------------------------------------------------------
// FSimClock: starts at wall-clock UTC, advances at its rate, can be set,
// frozen, stepped (lockstep) and started from config.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimClockRateTest,
	"CamSim.SimClock.RateAndLockstep",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSimClockRateTest::RunTest(const FString& Parameters)
{
	FSimClock Clock;
	const int64 WallDiffUs = static_cast<int64>(Clock.NowMicros()) - static_cast<int64>(FSimClock::ToMicros(FDateTime::UtcNow()));
	TestTrue(FString::Printf(TEXT("starts at wall-clock UTC (%lld us)"), WallDiffUs), FMath::Abs(WallDiffUs) < 1'000'000);

	double Mono = 100.0;
	Clock.SetMonotonicSource([&Mono] { return Mono; });
	const FDateTime Start(2025, 3, 1, 6, 30, 0);
	Clock.SetUtc(Start);
	auto Elapsed = [&Clock, &Start] { return (Clock.NowUtc() - Start).GetTotalSeconds(); };

	Mono += 10.0;
	TestEqual(TEXT("real time: 10 s later"), Elapsed(), 10.0);

	Clock.SetRate(4.0);
	Mono += 10.0;
	TestEqual(TEXT("rate 4: +40 s, earlier time kept"), Elapsed(), 50.0);

	Clock.SetRate(0.0);
	Mono += 100.0;
	TestEqual(TEXT("rate 0: frozen"), Elapsed(), 50.0);

	Clock.SetRate(1.0);
	Clock.SetLockstep(true);
	Mono += 100.0;
	TestEqual(TEXT("lockstep: monotonic time ignored"), Elapsed(), 50.0);
	Clock.Step(1.0 / 30.0);
	TestTrue(TEXT("lockstep: Step advances exactly"), FMath::IsNearlyEqual(Elapsed(), 50.0 + 1.0 / 30.0, 1e-6));

	Clock.SetLockstep(false);
	Mono += 2.0;
	TestTrue(TEXT("back to real time from the stepped time"), FMath::IsNearlyEqual(Elapsed(), 52.0 + 1.0 / 30.0, 1e-6));

	TestEqual(TEXT("micros round trip"), FSimClock::FromMicros(FSimClock::ToMicros(Start)), Start);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimClockStartTest,
	"CamSim.SimClock.StartFromConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSimClockStartTest::RunTest(const FString& Parameters)
{
	double Mono = 0.0;
	auto Frozen = [&Mono] { return Mono; };

	FSimClock A;
	A.SetMonotonicSource(Frozen);
	TestTrue(TEXT("ISO 8601 start"), A.Start(TEXT("2025-03-01T06:30:00Z"), 12.0f, 1.0));
	TestEqual(TEXT("start_datetime wins over start_hour"), A.NowUtc(), FDateTime(2025, 3, 1, 6, 30, 0));

	FSimClock B;
	B.SetMonotonicSource(Frozen);
	const FDateTime Today = B.NowUtc().GetDate();
	B.Start(FString(), 14.5f, 2.0);
	TestEqual(TEXT("start_hour: that UTC time today"), B.NowUtc(), Today + FTimespan(14, 30, 0));
	TestEqual(TEXT("rate applied"), B.GetRate(), 2.0);

	FSimClock C;
	TestFalse(TEXT("unparseable start_datetime reported"), C.Start(TEXT("yesterday"), -1.0f, 1.0));
	return true;
}

// -------------------------------------------------------------------------
// Sun position from local solar time: San Francisco (37.8 N), June solstice.
// -------------------------------------------------------------------------

#include "Environment/CamSimEnvironment.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSunPositionTest,
	"CamSim.SimClock.SunPosition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSunPositionTest::RunTest(const FString& Parameters)
{
	const int32 Solstice = FDateTime(2024, 6, 21).GetDayOfYear();
	const FVector2D Noon    = ACamSimEnvironment::ComputeSunPosition(12.0f, Solstice, 37.8);
	const FVector2D Morning = ACamSimEnvironment::ComputeSunPosition(6.0f,  Solstice, 37.8);
	const FVector2D Evening = ACamSimEnvironment::ComputeSunPosition(18.0f, Solstice, 37.8);
	const FVector2D Night   = ACamSimEnvironment::ComputeSunPosition(0.0f,  Solstice, 37.8);

	TestTrue(FString::Printf(TEXT("noon: high (%.1f) and due south (%.1f)"), Noon.X, Noon.Y),
		FMath::IsNearlyEqual(Noon.X, 75.6f, 0.5f) && FMath::IsNearlyEqual(Noon.Y, 180.0f, 1.0f));
	TestTrue(FString::Printf(TEXT("06:00: low in the east-north-east (%.1f, %.1f)"), Morning.X, Morning.Y),
		Morning.X > 5.0f && Morning.X < 20.0f && Morning.Y > 55.0f && Morning.Y < 80.0f);
	TestTrue(FString::Printf(TEXT("18:00: mirror image in the west-north-west (%.1f)"), Evening.Y),
		FMath::IsNearlyEqual(Evening.Y, 360.0f - Morning.Y, 0.5f));
	TestTrue(FString::Printf(TEXT("midnight: below the horizon (%.1f)"), Night.X), Night.X < -20.0f);
	return true;
}
