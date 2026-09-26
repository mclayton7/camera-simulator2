// Copyright CamSim Contributors. All Rights Reserved.

#include "Time/SimClock.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"

namespace
{
	const int64 UnixEpochTicks = FDateTime(1970, 1, 1).GetTicks();
	constexpr int64 TicksPerMicro = ETimespan::TicksPerMicrosecond;
}

FSimClock& FSimClock::Get()
{
	static FSimClock Clock;
	return Clock;
}

FSimClock::FSimClock()
{
	AnchorMicros = ToMicros(FDateTime::UtcNow());
	AnchorMono   = MonotonicNow();
}

uint64 FSimClock::ToMicros(const FDateTime& Utc)
{
	return static_cast<uint64>(FMath::Max<int64>(0, (Utc.GetTicks() - UnixEpochTicks) / TicksPerMicro));
}

FDateTime FSimClock::FromMicros(uint64 Micros)
{
	return FDateTime(UnixEpochTicks + static_cast<int64>(Micros) * TicksPerMicro);
}

double FSimClock::MonotonicNow() const
{
	return MonotonicSource ? MonotonicSource() : FPlatformTime::Seconds();
}

uint64 FSimClock::MicrosAtLocked(double Mono) const
{
	if (bLockstep || Rate <= 0.0) return AnchorMicros;
	const double Elapsed = FMath::Max(0.0, Mono - AnchorMono) * Rate;
	return AnchorMicros + static_cast<uint64>(Elapsed * 1e6);
}

void FSimClock::RebaseLocked()
{
	const double Mono = MonotonicNow();
	AnchorMicros = MicrosAtLocked(Mono);
	AnchorMono   = Mono;
}

uint64 FSimClock::NowMicros() const
{
	FScopeLock ScopeLock(&Lock);
	return MicrosAtLocked(MonotonicNow());
}

FDateTime FSimClock::NowUtc() const
{
	return FromMicros(NowMicros());
}

bool FSimClock::Start(const FString& StartDatetime, float StartHourUtc, double InRate)
{
	bool bOk = true;
	if (!StartDatetime.IsEmpty())
	{
		FDateTime Parsed;
		bOk = FDateTime::ParseIso8601(*StartDatetime, Parsed);
		if (bOk) SetUtc(Parsed);
	}
	else if (StartHourUtc >= 0.0f)
	{
		const FDateTime Today = NowUtc().GetDate();
		SetUtc(Today + FTimespan::FromHours(FMath::Clamp(StartHourUtc, 0.0f, 24.0f)));
	}
	SetRate(InRate);
	return bOk;
}

void FSimClock::SetUtc(const FDateTime& Utc)
{
	FScopeLock ScopeLock(&Lock);
	AnchorMicros = ToMicros(Utc);
	AnchorMono   = MonotonicNow();
}

void FSimClock::SetRate(double InRate)
{
	FScopeLock ScopeLock(&Lock);
	RebaseLocked();
	Rate = FMath::Max(0.0, InRate);
}

double FSimClock::GetRate() const
{
	FScopeLock ScopeLock(&Lock);
	return Rate;
}

void FSimClock::SetLockstep(bool bInLockstep)
{
	FScopeLock ScopeLock(&Lock);
	RebaseLocked();
	bLockstep = bInLockstep;
}

bool FSimClock::IsLockstep() const
{
	FScopeLock ScopeLock(&Lock);
	return bLockstep;
}

void FSimClock::Step(double Seconds)
{
	FScopeLock ScopeLock(&Lock);
	AnchorMicros += static_cast<uint64>(FMath::Max(0.0, Seconds) * 1e6);
}

void FSimClock::SetMonotonicSource(TFunction<double()> Source)
{
	FScopeLock ScopeLock(&Lock);
	RebaseLocked();
	MonotonicSource = MoveTemp(Source);
	AnchorMono = MonotonicNow();
}
