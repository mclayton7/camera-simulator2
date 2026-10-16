// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Time/SimClock.h"

/**
 * FSimDelta
 *
 * The time step for one per-tick integration (dead reckoning, gimbal slew;
 * ROADMAP 2.1/2.4). In lockstep the sim clock moves only by Step(), so the
 * step is the clock's advance since this instance's last call (0 between
 * steps, never negative, and 0 across a SetUtc jump: setting the time of day
 * must not dead-reckon entities through the hours in between). Otherwise it is the engine delta, unchanged: a
 * frozen clock outside lockstep is CIGI's "static time of day", and entities
 * keep moving under it. One instance per consumer, so the order of consumers
 * within a frame doesn't matter.
 */
class FSimDelta
{
public:
	explicit FSimDelta(FSimClock* InClock = nullptr) : Clock(InClock) {}

	double Next(double EngineDeltaSec)
	{
		FSimClock& C = Clock ? *Clock : FSimClock::Get();
		const uint64 Epoch = C.GetEpoch();
		const uint64 Now   = C.NowMicros();
		const uint64 Prev  = LastMicros;
		const bool bJumped = Epoch != LastEpoch;
		LastMicros = Now;
		LastEpoch  = Epoch;
		if (!C.IsLockstep()) return EngineDeltaSec;
		if (Prev == 0 || bJumped || Now <= Prev) return 0.0;
		return static_cast<double>(Now - Prev) * 1e-6;
	}

private:
	FSimClock* Clock = nullptr;
	uint64 LastMicros = 0;
	uint64 LastEpoch  = 0;
};

namespace CamSimTime
{
	/**
	 * The sensor AE/AGC step: frame time in lockstep (a stepped clock is frozen
	 * while a capture settles, and the exposure must still adapt to the new
	 * view), else the sim-time step.
	 */
	inline double SensorStepSeconds(bool bLockstep, double SimDeltaSec, float FrameRate)
	{
		if (!bLockstep) return SimDeltaSec;
		return 1.0 / static_cast<double>(FrameRate > 0.f ? FrameRate : 30.f);
	}
}
