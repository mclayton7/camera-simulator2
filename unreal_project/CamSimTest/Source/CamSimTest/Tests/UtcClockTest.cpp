// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/DateTime.h"
#include "Metadata/UtcClock.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUtcClockMatchesWallClockTest,
	"CamSim.UtcClock.MatchesWallClock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUtcClockMatchesWallClockTest::RunTest(const FString& Parameters)
{
	const uint64 Clock = FUtcClock::NowMicros();
	const int64  Wall  = (FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTicks() / ETimespan::TicksPerMicrosecond;

	// KLV Tag 2 must be Unix-epoch UTC; the old monotonic source was ~1970.
	const double DiffSec = FMath::Abs(static_cast<double>(static_cast<int64>(Clock) - Wall)) / 1e6;
	TestTrue(FString::Printf(TEXT("UTC clock within 2 s of wall clock (diff %.3f s)"), DiffSec), DiffSec < 2.0);
	TestTrue(TEXT("UTC clock is after 2020-01-01"), Clock > 1577836800000000ULL);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUtcClockMonotonicTest,
	"CamSim.UtcClock.Monotonic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUtcClockMonotonicTest::RunTest(const FString& Parameters)
{
	uint64 Prev = FUtcClock::NowMicros();
	for (int32 i = 0; i < 1000; ++i)
	{
		const uint64 Now = FUtcClock::NowMicros();
		if (!TestTrue(TEXT("Never goes backwards"), Now >= Prev))
		{
			return false;
		}
		Prev = Now;
	}
	return true;
}
