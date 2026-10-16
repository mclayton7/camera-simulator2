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
	float                   FrameRateHz = 30.0f;      // non-radiometric modes: DarkE = DarkCurrentEs / FrameRateHz
	/** ROADMAP 4A: IR mode with the thermal radiance input (signal = L / B(300 K)). The AE uses ThermalExposure and its
	 *  own state; the AGC interpolates percentiles, keeps a margin and is capped at AGCMaxDisplayGain. */
	bool                    bRadianceInput = false;
	/** REALISM R6 determinism (camsim.Sensor.FixedNoiseFrame): >= 0 keys the temporal noise to this frame instead of
	 *  Serial, so a still view renders the same noise every frame. -1 (default): FrameIndex = Serial. */
	int64                   FixedNoiseFrame = -1;
};

class FSensorController
{
public:
	static constexpr int32 StaleAfterTicks = 10;
	// Normalised signal at the clip point: full well / ADC full scale, which the
	// normalised knee maps to white (3B.1's knee reached white at 2.0).
	static constexpr float ClipLinear      = 1.0f;
	/** Thermal AE seed: a 300 K scene (signal ~1) at half full scale. */
	static constexpr float RadianceSeedGainEv = -1.0f;
	/** The thermal AGC maps its percentile band onto [margin, 1 - margin], so the pixels at the percentiles (half the frame in a
	 *  two-level night scene) are not black or white. */
	static constexpr float ThermalAgcMargin = 0.1f;
	FSensorFrameParams Update(const FSensorControllerInput& In, const FSensorModeConfig& Cfg);
	/** Log2 of the total gain actually emitted last tick: PhotonGain * AnalogGain
	 *  (* DisplayGain in IR AGC, i.e. the AGC stretch on absolute signal). */
	float  GetGainEv() const          { return LastEmittedGainEv; }
	float  GetLastMedianLog2() const  { return LastMedianLog2; }
	/** UE's pre-exposure for the scene buffers (AutoExposureBias, EV; ROADMAP 6A): the AE's target without the photon /
	 *  analog cap, at most MaxSceneExposureBoostEv above the emitted gain. Equal to GetGainEv() while the AE is inside its
	 *  limits (daylight); at night, with the gain capped, it keeps scene colour near target grey, so UE's pre-exposed fp16 /
	 *  R11G11B10 buffers and TSR history do not quantize (they held stale moonlight after the moon set). The sensor graph
	 *  divides PreExposure back out, so the image does not depend on it. IR AGC: the emitted gain. */
	float  GetSceneExposureEv() const { return LastSceneExposureEv; }
	/** Cap on the scene exposure above the sensor gain: anything that overflows fp16 (65504) after the boost is past the
	 *  sensor's clip anyway (2^15 < 65504). */
	static constexpr float MaxSceneExposureBoostEv = 15.0f;
	uint32 GetStaleEpisodes() const   { return StaleEpisodes; }
	/** Integration time (ms) of the photon gain emitted last tick on a radiometric mode (ROADMAP 9.4); 0 otherwise. */
	float  GetIntegrationMs() const   { return LastIntegrationMs; }
	/** Highest total gain (EV): E.MaxPhotonGainEv + the analog stage's MaxAnalogGainDb in EV (none for a microbolometer). */
	static float TotalGainCapEv(const FSensorModeConfig& Cfg, const FSensorExposureConfig& E);
	/** TotalGainCapEv(Cfg, CamSimRadiometry::EffectiveExposure(Cfg)): the EO / IR luminance-proxy AE's cap. */
	static float TotalGainCapEv(const FSensorModeConfig& Cfg);
	/** Hash seed of a mode's focal plane: ConfigSeed ^ Pcg(mode index + 1). EO and IR are different
	 *  detectors, so the same config seed must not give them the same PRNU/DSNU/defect pattern; the
	 *  same (mode, seed) always gives the same pattern. Derived here, on the CPU, so the reference
	 *  and the GPU graph both receive the final FSensorFrameParams::Seed. */
	static uint32 ModeSeed(ESensorGraphMode Mode, uint32 ConfigSeed);
	/** Percentile P in [0,1] of H as log2 signal; false for an empty histogram. */
	static bool PercentileLog2(const FSensorHistogram& H, float P, float& OutLog2);
	/** Percentile P of H as log2 signal, interpolated linearly inside the bin (counts uniform across it); false when empty. */
	static bool PercentileLog2Interp(const FSensorHistogram& H, float P, float& OutLog2);

private:
	void UpdateAe(const FSensorHistogram& H, const FSensorModeConfig& Cfg, const FSensorControllerInput& In, bool bSnap);
	void UpdateIrAgc(const FSensorHistogram& H, const FSensorModeConfig& Cfg, const FSensorControllerInput& In, bool bSnap);
	static float Smoothing(double DeltaSimSec, int32 LagFrames, bool bSnap);

	bool   bInitialized    = false;
	bool   bSnapPending    = false;
	uint32 SnapAfterSerial = 0;
	/** AE state slot: 0 EO, 1 IR luminance proxy, 2 IR thermal radiance. */
	static int32 SlotOf(const FSensorControllerInput& In);
	/** The slot's AE limits: ThermalExposure in slot 2, else CamSimRadiometry::EffectiveExposure (the calibrated Exposure
	 *  unless the mode is radiometric). */
	static FSensorExposureConfig ExposureOf(const FSensorModeConfig& Cfg, const FSensorControllerInput& In);
	int32  LastSlot        = 0;
	/** AE loop state (total gain EV), one per slot: an excursion into another waveband or input never
	 *  disturbs this one's converged exposure. */
	float  GainEvByMode[3] = { -12.0f, -12.0f, RadianceSeedGainEv };
	float  LastEmittedGainEv = -12.0f; // log2 of the gain actually emitted last tick
	/** Per slot: the AE target before the gain cap (smoothed like GainEvByMode), for GetSceneExposureEv. */
	float  SceneEvByMode[3] = { -12.0f, -12.0f, RadianceSeedGainEv };
	float  LastSceneExposureEv = -12.0f;
	float  IrLoLog2        = 0.0f;
	float  IrHiLog2        = 1.0f;
	float  LastMedianLog2  = 0.0f;
	float  LastIntegrationMs = 0.0f;
	int32  TicksSinceHistogram = 0;
	double PendingDeltaSimSec  = 0.0;  // sim time since the last histogram was consumed
	uint32 StaleEpisodes   = 0;
};
