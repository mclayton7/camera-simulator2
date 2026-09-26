// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * FTerrainReadinessGate
 *
 * Holds frame output until Cesium has loaded terrain for the current view, so
 * startup and teleports don't stream coarse placeholder tiles as if they were
 * real imagery (bad data for ATR training).
 *
 * Once ready, the gate stays open during normal flight — tiles streaming in
 * the background must not stall video. It re-arms only when the platform
 * jumps further than TeleportDistanceM in one update. A timeout opens it
 * anyway so a tileset that never reports 100% can't block output forever.
 */
class FTerrainReadinessGate
{
public:
	struct FSettings
	{
		bool   bEnabled           = true;
		float  MinLoadProgressPct = 99.0f;   // Cesium GetLoadProgress() is 0-100
		float  TimeoutSec         = 30.0f;
		double TeleportDistanceM  = 5000.0;
	};

	void Configure(const FSettings& In) { Settings = In; }

	/**
	 * @param NowSec              Monotonic time.
	 * @param MinLoadProgressPct  Lowest load progress across tilesets; < 0 if there are none.
	 * @param MovedM              Platform displacement since the previous update.
	 * @return true when frames may be emitted.
	 */
	bool Update(double NowSec, float MinLoadProgressPct, double MovedM)
	{
		bTimedOut = false;
		if (!Settings.bEnabled || MinLoadProgressPct < 0.0f)
		{
			bReady = true;
			return true;
		}
		if (bReady && MovedM > Settings.TeleportDistanceM)
		{
			bReady = false;  // teleport: wait for the new area to load
			WaitStartSec = -1.0;
		}
		if (bReady) return true;

		if (WaitStartSec < 0.0) WaitStartSec = NowSec;
		if (MinLoadProgressPct >= Settings.MinLoadProgressPct)
		{
			bReady = true;
		}
		else if (NowSec - WaitStartSec >= Settings.TimeoutSec)
		{
			bReady = true;
			bTimedOut = true;
		}
		return bReady;
	}

	bool IsReady() const { return bReady; }
	/** True on the update where the gate opened because of the timeout. */
	bool DidTimeOut() const { return bTimedOut; }

private:
	FSettings Settings;
	bool      bReady       = false;
	bool      bTimedOut    = false;
	double    WaitStartSec = -1.0;
};
