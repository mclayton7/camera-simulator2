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
	float                   FrameRateHz = 30.0f;      // integration rate: DarkE = DarkCurrentEs / FrameRateHz
};

class FSensorController
{
public:
	static constexpr int32 StaleAfterTicks = 10;
	// Normalised signal at the clip point: full well / ADC full scale, which the
	// normalised knee maps to white (3B.1's knee reached white at 2.0).
	static constexpr float ClipLinear      = 1.0f;
	FSensorFrameParams Update(const FSensorControllerInput& In, const FSensorModeConfig& Cfg);
	/** Log2 of the total gain actually emitted last tick: PhotonGain * AnalogGain
	 *  (* DisplayGain in IR AGC, i.e. the AGC stretch on absolute signal). */
	float  GetGainEv() const          { return LastEmittedGainEv; }
	float  GetLastMedianLog2() const  { return LastMedianLog2; }
	uint32 GetStaleEpisodes() const   { return StaleEpisodes; }
	/** Highest total gain (EV): MaxPhotonGainEv + the analog stage's MaxAnalogGainDb in EV
	 *  (none for a microbolometer). */
	static float TotalGainCapEv(const FSensorModeConfig& Cfg);
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
	/** AE loop state (total gain EV), one per graph mode: an excursion into the
	 *  other waveband never disturbs this one's converged exposure. */
	float  GainEvByMode[2] = { -12.0f, -12.0f };
	float  LastEmittedGainEv = -12.0f; // log2 of the gain actually emitted last tick
	float  IrLoLog2        = 0.0f;
	float  IrHiLog2        = 1.0f;
	float  LastMedianLog2  = 0.0f;
	int32  TicksSinceHistogram = 0;
	double PendingDeltaSimSec  = 0.0;  // sim time since the last histogram was consumed
	uint32 StaleEpisodes   = 0;
};
