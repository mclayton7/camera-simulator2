// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SensorFrameParams.h"
#include "Sensor/SensorTypes.h"

struct FSensorControllerInput
{
	ESensorGraphMode        Mode        = ESensorGraphMode::EO;
	bool                    bBlackHot   = false;
	bool                    bCameraCut  = false;
	double                  DeltaSimSec = 0.0;
	const FSensorHistogram* NewHistogram = nullptr;  // newest delivered this tick, or null
	uint32                  Serial      = 0;          // this tick's params serial (monotonic)
};

class FSensorController
{
public:
	static constexpr int32 StaleAfterTicks = 10;
	static constexpr float ClipLinear      = 2.0f;  // the knee maps 2.0 to ~255/255
	FSensorFrameParams Update(const FSensorControllerInput& In, const FSensorModeConfig& Cfg);
	/** Log2 of the gain actually emitted last tick (AE gain in EO, IR AGC stretch in IR AGC). */
	float  GetGainEv() const          { return LastEmittedGainEv; }
	float  GetLastMedianLog2() const  { return LastMedianLog2; }
	uint32 GetStaleEpisodes() const   { return StaleEpisodes; }
	/** Percentile P in [0,1] of H as log2 signal; false for an empty histogram. */
	static bool PercentileLog2(const FSensorHistogram& H, float P, float& OutLog2);

private:
	void UpdateAe(const FSensorHistogram& H, const FSensorModeConfig& Cfg, const FSensorControllerInput& In, bool bSnap);
	void UpdateIrAgc(const FSensorHistogram& H, const FSensorModeConfig& Cfg, const FSensorControllerInput& In, bool bSnap);
	static float Smoothing(double DeltaSimSec, int32 LagFrames, bool bSnap);

	bool   bInitialized    = false;
	bool   bSnapPending    = false;
	uint32 SnapAfterSerial = 0;
	ESensorGraphMode LastMode = ESensorGraphMode::EO;
	float  GainEv          = -12.0f;   // AE loop state only; never written by IR AGC
	float  LastEmittedGainEv = -12.0f; // log2 of the gain actually emitted last tick
	float  IrLoLog2        = 0.0f;
	float  IrHiLog2        = 1.0f;
	float  LastMedianLog2  = 0.0f;
	int32  TicksSinceHistogram = 0;
	double PendingDeltaSimSec  = 0.0;  // sim time since the last histogram was consumed
	uint32 StaleEpisodes   = 0;
};
