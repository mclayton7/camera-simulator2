// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * FCigiHostClock
 *
 * The host time associated with the CIGI message being parsed. When the IG
 * Control packet carries a valid timestamp (CIGI 3.3: uint32, 10 µs ticks) the
 * host's own clock is used, unwrapped across the ~11.9 h rollover; otherwise
 * the message's arrival time stands in for it.
 *
 * Receiver-thread only: BeginMessage() before parsing each datagram, then
 * OnIgControl() when opcode 1 is parsed (it is always first in a message).
 */
class FCigiHostClock
{
public:
	static constexpr double SecondsPerTick = 10e-6;

	void BeginMessage(double ArrivalSec)
	{
		MessageTimeSec = ArrivalSec;
	}

	void OnIgControl(bool bTimestampValid, uint32 Ticks)
	{
		if (!bTimestampValid) return;
		if (bHaveTicks)
		{
			// Unsigned difference is wrap-safe across the 32-bit rollover.
			HostSec += static_cast<double>(static_cast<uint32>(Ticks - LastTicks)) * SecondsPerTick;
		}
		else
		{
			HostSec = static_cast<double>(Ticks) * SecondsPerTick;
			bHaveTicks = true;
		}
		LastTicks = Ticks;
		MessageTimeSec = HostSec;
	}

	/** Time of the current message, in seconds. Only differences are meaningful. */
	double Now() const { return MessageTimeSec; }

private:
	double MessageTimeSec = 0.0;
	double HostSec        = 0.0;
	uint32 LastTicks      = 0;
	bool   bHaveTicks     = false;
};
