// Copyright CamSim Contributors. All Rights Reserved.

#include "Control/ControlCaptureGate.h"

void FControlCaptureGate::Begin(double NowSec, int32 InSettleFrames, double InTimeoutSec, float InSettleLoadPct)
{
	State = EState::Waiting;
	BeginSec = NowSec;
	SettleFrames = FMath::Max(0, InSettleFrames);
	TimeoutSec = InTimeoutSec;
	SettleLoadPct = InSettleLoadPct;
	bSeenFrame = false;
	FirstFrames = LastFrames = SettleStart = 0;
	Reason = TEXT("waiting");
}

void FControlCaptureGate::Reset()
{
	State = EState::Idle;
}

FControlCaptureGate::EState FControlCaptureGate::Update(const FControlCaptureInputs& In)
{
	if (State != EState::Waiting && State != EState::Settling)
	{
		State = EState::Idle;   // Fire and TimedOut are returned once
		return State;
	}
	if (!bSeenFrame)
	{
		bSeenFrame = true;
		FirstFrames = In.FramesCaptured;
	}
	LastFrames = In.FramesCaptured;

	if (In.NowSec - BeginSec >= TimeoutSec)
	{
		State = EState::TimedOut;   // Reason keeps what it was waiting for
		return State;
	}

	if (!In.bTerrainReady)
	{
		Reason = TEXT("terrain");
	}
	else if (In.MinLoadProgressPct >= 0.0f && In.MinLoadProgressPct < SettleLoadPct)
	{
		Reason = FString::Printf(TEXT("tiles %.1f%%"), In.MinLoadProgressPct);
	}
	else if (!In.bVegetationIdle)
	{
		Reason = TEXT("vegetation");
	}
	else if (!In.bLandCoverSettled)
	{
		Reason = TEXT("land cover");
	}
	else if (In.bCommandThisTick)
	{
		Reason = TEXT("camera or entity command");
	}
	else
	{
		if (State == EState::Waiting)
		{
			State = EState::Settling;
			SettleStart = In.FramesCaptured;
		}
		const uint64 Settled = In.FramesCaptured - SettleStart;
		Reason = FString::Printf(TEXT("settling %llu/%d"), Settled, SettleFrames);
		if (Settled >= static_cast<uint64>(SettleFrames))
		{
			State = EState::Fire;
		}
		return State;
	}
	State = EState::Waiting;
	return State;
}
