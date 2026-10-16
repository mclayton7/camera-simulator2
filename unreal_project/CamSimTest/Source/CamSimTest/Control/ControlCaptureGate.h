// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** One tick's view of the scene, for FControlCaptureGate. */
struct FControlCaptureInputs
{
	double NowSec             = 0.0;     // wall clock
	bool   bTerrainReady      = false;   // the camera's terrain gate is open
	float  MinLoadProgressPct = -1.0f;   // lowest tileset load progress; < 0: no tilesets (counts as loaded)
	bool   bVegetationIdle    = true;    // no vegetation cell or block in flight
	bool   bLandCoverSettled  = true;    // ROADMAP 4D: the land-cover window is built for this view (when the class image is wanted)
	bool   bCommandThisTick   = false;   // a camera or entity command was applied this tick
	uint64 FramesCaptured     = 0;       // the camera's capture counter
};

/**
 * FControlCaptureGate
 *
 * When a control API capture may take its frame (ROADMAP 2.4). After Begin it waits until, on one tick, the terrain
 * gate is open, every tileset is loaded to SettleLoadPct, vegetation is idle and no command was applied; then it
 * counts SettleFrames rendered frames (any failed condition or command starts over) and returns Fire once. A request
 * that doesn't get there within TimeoutSec returns TimedOut once. Either way the gate is Idle afterwards. Pure; the
 * service ticks it on the game thread.
 */
namespace CamSimControl
{
	/**
	 * ROADMAP 4D: the land-cover window the semantic image reads is settled — nothing to wait for when land cover or
	 * semantic output is off; else a build has started (the window is configured) and none is in flight (launch,
	 * re-centre after a move). A finished build without data is settled too: its terrain is honestly unlabeled.
	 */
	inline bool IsLandCoverSettled(bool bLandCoverEnabled, bool bSemanticPossible, uint32 BuildsStarted, bool bBuildInFlight)
	{
		return !bLandCoverEnabled || !bSemanticPossible || (BuildsStarted > 0 && !bBuildInFlight);
	}
}

class CAMSIMTEST_API FControlCaptureGate
{
public:
	enum class EState : uint8 { Idle, Waiting, Settling, Fire, TimedOut };

	void Begin(double NowSec, int32 InSettleFrames, double InTimeoutSec, float InSettleLoadPct);
	EState Update(const FControlCaptureInputs& In);
	void Reset();

	EState GetState() const { return State; }
	/** Captured frames since the first tick after Begin. */
	int32 FramesWaited() const { return static_cast<int32>(LastFrames - FirstFrames); }
	/** What the request is waiting for ("terrain", "tiles 97.2%", "vegetation", "camera or entity command", "settling 12/30"). */
	FString PendingReason() const { return Reason; }

private:
	EState State = EState::Idle;
	double BeginSec = 0.0;
	double TimeoutSec = 0.0;
	int32  SettleFrames = 0;
	float  SettleLoadPct = 100.0f;
	bool   bSeenFrame = false;
	uint64 FirstFrames = 0;
	uint64 LastFrames = 0;
	uint64 SettleStart = 0;
	FString Reason;
};
