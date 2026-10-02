// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/DateTime.h"
#include "HAL/CriticalSection.h"

/**
 * FSimClock
 *
 * The one authoritative simulation time (UTC date and time of day). Every
 * consumer of "what time is it in the simulation" reads it: KLV Tag 2,
 * ground-truth annotations, and the sun.
 *
 * Time advances from an anchor at Rate × the monotonic clock: 1 = real
 * time, 0 = frozen (CIGI static time of day), >1 faster than real time. In
 * lockstep mode it advances only through Step(), for reproducible runs.
 * By default it starts at the wall-clock time of the first read; config
 * (start_datetime / start_hour) and CIGI Celestial Sphere Control set it.
 *
 * Distinct from FCigiHostClock, which tracks the host's own (arbitrary-epoch)
 * timestamps for ordering and ground-speed deltas.
 *
 * Thread-safe. Never moves backwards except through an explicit SetUtc().
 */
class CAMSIMTEST_API FSimClock
{
public:
	/** The process-wide simulation clock. */
	static FSimClock& Get();

	/** Starts at the wall-clock time, advancing in real time. */
	FSimClock();

	/** Simulation time as UTC microseconds since the Unix epoch. */
	uint64 NowMicros() const;
	FDateTime NowUtc() const;

	/**
	 * Apply the configured start: StartDatetime (ISO 8601) if set, else
	 * StartHourUtc (>= 0) on today's UTC date, else keep the wall-clock time.
	 * Returns false if StartDatetime is set but can't be parsed.
	 */
	bool Start(const FString& StartDatetime, float StartHourUtc, double InRate);

	/** Jump to a UTC date and time; time keeps advancing from there at the current rate. */
	void SetUtc(const FDateTime& Utc);

	/** 1 = real time, 0 = frozen, >1 faster than real time. Negative values are treated as 0. */
	void SetRate(double InRate);
	double GetRate() const;

	/** In lockstep, time advances only through Step(). */
	void SetLockstep(bool bInLockstep);
	bool IsLockstep() const;
	void Step(double Seconds);

	/** Replace the monotonic time source (seconds). For tests. */
	void SetMonotonicSource(TFunction<double()> Source);

	static uint64 ToMicros(const FDateTime& Utc);
	static FDateTime FromMicros(uint64 Micros);

private:
	double MonotonicNow() const;
	/** Sim micros at monotonic time Mono; caller holds Lock. */
	uint64 MicrosAtLocked(double Mono) const;
	/** Move the anchor to now, so a rate or mode change doesn't rewrite the past. */
	void RebaseLocked();

	mutable FCriticalSection Lock;
	TFunction<double()> MonotonicSource;
	uint64 AnchorMicros = 0;    // sim time at AnchorMono
	double AnchorMono   = 0.0;
	double Rate         = 1.0;
	bool   bLockstep    = false;
};
