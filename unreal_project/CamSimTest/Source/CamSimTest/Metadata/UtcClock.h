// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * FUtcClock
 *
 * UTC wall-clock time with monotonic-clock stability. The first call anchors
 * FPlatformTime::Seconds() (monotonic, arbitrary epoch) to FDateTime::UtcNow();
 * later calls advance from that anchor, so timestamps never jump backwards if
 * the system clock is adjusted mid-run.
 *
 * Used for KLV Tag 2 (Precision Time Stamp), which must be microseconds since
 * the Unix epoch (UTC). Stand-in until the sim clock (ROADMAP 2.1) exists.
 */
class CAMSIMTEST_API FUtcClock
{
public:
	/** Microseconds since 1970-01-01T00:00:00Z. Thread-safe. */
	static uint64 NowMicros();
};
