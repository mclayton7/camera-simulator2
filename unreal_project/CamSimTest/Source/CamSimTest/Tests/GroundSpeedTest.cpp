// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "CIGI/CigiHostClock.h"
#include "Geospatial/GroundSpeedEstimator.h"

// -------------------------------------------------------------------------
// Ground speed must use the host's time between fixes and hold between them.
// Previously it divided by the IG frame time and dropped to 0 on any tick
// without a host update, so a 20 Hz host on a 30 Hz IG flickered to zero.
// -------------------------------------------------------------------------

namespace
{
	constexpr double MetresPerDegLatAtEquator = 6371008.8 * PI / 180.0;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundSpeedHostRateTest,
	"CamSim.GroundSpeed.UsesHostTimeAndHolds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundSpeedHostRateTest::RunTest(const FString& Parameters)
{
	// Host at 20 Hz, flying north at 50 m/s along the equator.
	const double SpeedMps = 50.0;
	const double HostDt   = 1.0 / 20.0;
	FGroundSpeedEstimator Est;
	TestEqual(TEXT("No estimate before two fixes"), Est.GetSpeedMps(), 0.0f);

	for (int32 i = 0; i < 10; ++i)
	{
		const double T   = i * HostDt;
		const double Lat = (SpeedMps * T) / MetresPerDegLatAtEquator;
		Est.AddFix(Lat, 0.0, T);
		// The IG ticks at 30 Hz and polls every tick; between host updates it
		// sees the held value, never zero.
		if (i >= 1)
		{
			TestTrue(FString::Printf(TEXT("Fix %d: %.3f m/s ~= 50"), i, Est.GetSpeedMps()),
				FMath::IsNearlyEqual(Est.GetSpeedMps(), static_cast<float>(SpeedMps), 0.1f));
		}
	}

	// Duplicate fix (same host time, e.g. two entity packets in one message): hold.
	const float Before = Est.GetSpeedMps();
	Est.AddFix(1.0, 0.0, 9 * HostDt);
	TestEqual(TEXT("Same host time is ignored"), Est.GetSpeedMps(), Before);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundSpeedDiscontinuityTest,
	"CamSim.GroundSpeed.TeleportResets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundSpeedDiscontinuityTest::RunTest(const FString& Parameters)
{
	FGroundSpeedEstimator Est;
	Est.AddFix(0.0, 0.0, 0.0);
	Est.AddFix(0.001, 0.0, 1.0);
	TestTrue(TEXT("Normal motion measured"), Est.GetSpeedMps() > 100.0f);
	Est.AddFix(10.0, 10.0, 1.1);  // ~1500 km in 0.1 s
	TestEqual(TEXT("Teleport resets to 0"), Est.GetSpeedMps(), 0.0f);

	// Antimeridian crossing is a short hop, not half the planet.
	FGroundSpeedEstimator Wrap;
	Wrap.AddFix(0.0, 179.9995, 0.0);
	Wrap.AddFix(0.0, -179.9995, 1.0);
	TestTrue(FString::Printf(TEXT("Antimeridian crossing ~111 m/s (%.1f)"), Wrap.GetSpeedMps()),
		FMath::IsNearlyEqual(Wrap.GetSpeedMps(), 111.2f, 1.0f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCigiHostClockTest,
	"CamSim.GroundSpeed.CigiHostClock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCigiHostClockTest::RunTest(const FString& Parameters)
{
	FCigiHostClock Clock;

	// Without a valid IG Control timestamp, arrival time is used.
	Clock.BeginMessage(100.0);
	Clock.OnIgControl(false, 12345);
	TestEqual(TEXT("Arrival time when timestamp invalid"), Clock.Now(), 100.0);

	// Valid timestamps: 10 µs ticks, host time wins over arrival jitter.
	Clock.BeginMessage(200.0);
	Clock.OnIgControl(true, 1'000'000);          // 10.0 s
	const double T0 = Clock.Now();
	Clock.BeginMessage(200.9);                   // arrived late
	Clock.OnIgControl(true, 1'005'000);          // +0.05 s
	TestTrue(TEXT("Host delta is 50 ms"), FMath::IsNearlyEqual(Clock.Now() - T0, 0.05, 1e-9));

	// 32-bit rollover (~11.9 h) keeps advancing.
	Clock.OnIgControl(true, 0xFFFFFF00u);
	const double BeforeWrap = Clock.Now();
	Clock.OnIgControl(true, 0x00000100u);        // +0x200 ticks
	TestTrue(TEXT("Wraps forward by 5.12 ms"),
		FMath::IsNearlyEqual(Clock.Now() - BeforeWrap, 0x200 * FCigiHostClock::SecondsPerTick, 1e-9));
	return true;
}
