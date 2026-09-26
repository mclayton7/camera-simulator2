// Copyright CamSim Contributors. All Rights Reserved.

#include "Metadata/UtcClock.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"

namespace
{
	struct FUtcAnchor
	{
		double MonotonicSec;
		int64  UnixMicros;
	};

	const FUtcAnchor& GetAnchor()
	{
		// Function-local static: initialised once, thread-safe (C++11).
		static const FUtcAnchor Anchor = []
		{
			const double    Mono = FPlatformTime::Seconds();
			const FDateTime Utc  = FDateTime::UtcNow();
			// FDateTime ticks are 100 ns since 0001-01-01.
			const int64 Micros = (Utc - FDateTime(1970, 1, 1)).GetTicks() / ETimespan::TicksPerMicrosecond;
			return FUtcAnchor{ Mono, Micros };
		}();
		return Anchor;
	}
}

uint64 FUtcClock::NowMicros()
{
	const FUtcAnchor& Anchor = GetAnchor();
	const double ElapsedSec = FPlatformTime::Seconds() - Anchor.MonotonicSec;
	return static_cast<uint64>(Anchor.UnixMicros + static_cast<int64>(ElapsedSec * 1'000'000.0));
}
