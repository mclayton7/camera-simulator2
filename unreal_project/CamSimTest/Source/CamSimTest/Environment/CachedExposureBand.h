// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"

/** Thresholds for the Lumen cached-lighting exposure band (ROADMAP 6A). */
struct FCachedExposureBandConfig
{
	float DayEv = 8.0f;
	float NightEv = -6.0f;
	float SwitchMedianLog2 = 0.0f;   // scene median luminance, log2 cd/m2
	float HysteresisEv = 2.0f;
};

/** Day/night band chosen from the EO AE's scene median, with hysteresis. Pure; game thread. */
class FCachedExposureBand
{
public:
	/** Feed the EO AE's scene median (log2 cd/m2). Returns true when the band changed. bValid false (IR, no histogram) holds. */
	bool Update(float SceneMedianLog2, bool bValid, const FCachedExposureBandConfig& Cfg)
	{
		if (!bValid || !FMath::IsFinite(SceneMedianLog2))
		{
			return false;
		}
		if (!bNight && SceneMedianLog2 < Cfg.SwitchMedianLog2)
		{
			bNight = true;
			return true;
		}
		if (bNight && SceneMedianLog2 > Cfg.SwitchMedianLog2 + Cfg.HysteresisEv)
		{
			bNight = false;
			return true;
		}
		return false;
	}

	bool  IsNight() const { return bNight; }
	float CachedEv(const FCachedExposureBandConfig& Cfg) const { return bNight ? Cfg.NightEv : Cfg.DayEv; }

private:
	bool bNight = false;
};

/**
 * Whole-cubemap sky captures after a band switch (ROADMAP 6A). The real-time sky capture is time-sliced over about a dozen
 * frames, and the cubemap it keeps meanwhile was encoded at the old cached pre-exposure (14 EV away), so Lumen's sky
 * lighting dipped ~3 EV and snapped back ~1.5 s later. Pure; the environment sets
 * r.SkyLight.RealTimeReflectionCapture.TimeSlice 0 while Tick returns true and restores the previous value after.
 */
class FSkyRecaptureHold
{
public:
	static constexpr int32 Frames = 15;
	/** Once per tick; bSwitched: the band changed this tick. True while the hold lasts (the switch tick included). */
	bool Tick(bool bSwitched)
	{
		if (bSwitched) Left = Frames;
		const bool bActive = Left > 0;
		if (Left > 0) --Left;
		return bActive;
	}

private:
	int32 Left = 0;
};

/** FSkyRecaptureHold wired to r.SkyLight.RealTimeReflectionCapture.TimeSlice: 0 while held, the value it found restored
 *  when the hold ends or on Release (EndPlay mid-hold). Game thread. */
struct FSkyRecaptureHoldState
{
	void Tick(bool bSwitched)
	{
		const bool bHold = Hold.Tick(bSwitched);
		if (bHold == bHeld) return;
		if (IConsoleVariable* Var = Find())
		{
			if (bHold) Saved = Var->GetInt();
			Var->Set(bHold ? 0 : Saved, ECVF_SetByCode);
		}
		bHeld = bHold;
	}
	void Release()
	{
		if (!bHeld) return;
		if (IConsoleVariable* Var = Find()) Var->Set(Saved, ECVF_SetByCode);
		bHeld = false;
		Hold = FSkyRecaptureHold();
	}

private:
	static IConsoleVariable* Find()
	{
		return IConsoleManager::Get().FindConsoleVariable(TEXT("r.SkyLight.RealTimeReflectionCapture.TimeSlice"));
	}
	FSkyRecaptureHold Hold;
	bool  bHeld = false;
	int32 Saved = 1;
};
