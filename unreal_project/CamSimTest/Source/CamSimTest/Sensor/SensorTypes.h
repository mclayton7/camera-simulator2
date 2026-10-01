// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// ---------------------------------------------------------------------------
// ESensorMode — waveband selector driven by CIGI Sensor Control SensorId
// ---------------------------------------------------------------------------
enum class ESensorMode : uint8
{
	EO = 0,   // Electro-optical: colour photon detector (default)
	IR = 1,   // Thermal: mono detector (cooled MWIR or uncooled LWIR preset) + AGC + polarity
};

// ---------------------------------------------------------------------------
// ESensorDetectorType — how a detector converts incident signal to counts.
// ---------------------------------------------------------------------------
enum class ESensorDetectorType : uint8
{
	Photon         = 0,  // photon-counting detector (EO CMOS, cooled MWIR InSb)
	Microbolometer = 1,  // uncooled thermal detector (LWIR VOx)
};

// ---------------------------------------------------------------------------
// FSensorOpticsConfig — lens model (ROADMAP 3B.2). Preset defaults in
// Sensor/SensorPresets.h; per-mode overrides in camsim_config.yaml.
// ---------------------------------------------------------------------------
struct FSensorOpticsConfig
{
	float FNumber            = 4.0f;
	float PixelPitchUm       = 2.9f;
	float WavelengthUm       = 0.55f;
	float ExtraBlurPx        = 0.0f;
	float VignettingExponent = 4.0f;
	float K1 = 0.0f, K2 = 0.0f;

	bool operator==(const FSensorOpticsConfig&) const = default;
};

// ---------------------------------------------------------------------------
// FSensorDetectorConfig — detector noise/response model (ROADMAP 3B.2).
// Photon fields (FullWellE, ReadNoiseE, Prnu, DsnuE, DarkCurrentEs,
// MaxAnalogGainDb) apply when Type == Photon; microbolometer fields
// (TemporalNoise, PixelFpn, ColumnFpn, RowFpn) apply when Type == Microbolometer.
// ---------------------------------------------------------------------------
struct FSensorDetectorConfig
{
	ESensorDetectorType Type = ESensorDetectorType::Photon;
	float FullWellE      = 10000.0f;   // photon
	float ReadNoiseE     = 2.0f;
	float Prnu           = 0.01f;
	float DsnuE          = 1.0f;
	float DarkCurrentEs  = 5.0f;
	float MaxAnalogGainDb = 30.0f;
	float TemporalNoise  = 0.0f;       // microbolometer, fractions of full scale
	float PixelFpn = 0.0f, ColumnFpn = 0.0f, RowFpn = 0.0f;
	int32 AdcBits        = 12;
	float HotPixelFraction  = 1e-5f;
	float DeadPixelFraction = 1e-5f;

	/** Spectral band (micrometres) of the thermal radiance integral (ROADMAP 4A). The preset sets it. */
	float BandLoUm          = 0.4f;
	float BandHiUm          = 0.7f;
};

// ---------------------------------------------------------------------------
// FSensorExposureConfig — sensor auto-exposure (ROADMAP 3B). Gains are log2 of
// the multiplier applied to absolute scene-linear values: higher = brighter.
// ---------------------------------------------------------------------------
struct FSensorExposureConfig
{
	/** Auto-exposure on; false uses ManualGainEv. */
	bool  bAuto               = true;
	/** Camera limits: shortest integration / lowest gain … highest gain.
	 *  Neutral values; the calibrated per-mode cameras are set in
	 *  FCamSimConfig's built-in defaults and deploy/camsim_config.yaml. */
	float MinGainEv           = -20.0f;
	/** Highest photon-stage (integration) gain; past it AE adds analog gain up to
	 *  the detector's max_analog_gain_db (FSensorController::TotalGainCapEv). */
	float MaxPhotonGainEv     = -6.0f;
	/** Linear value the histogram median is exposed to. */
	float TargetGrey          = 0.18f;
	/** This percentile of the histogram is kept below clipping. */
	float HighlightPercentile = 0.99f;
	/** Convergence time constant in frames at 30 Hz (sim time); 0 = instant. */
	int32 LagFrames           = 2;
	float ManualGainEv        = -12.0f;
};

// ---------------------------------------------------------------------------
// FSensorModeConfig — per-mode tuning, loaded from camsim_config.yaml
// ---------------------------------------------------------------------------
struct FSensorModeConfig
{
	/** Detector spectral response: signal = dot(scene RGB, SignalWeights). Default BT.709 luminance. */
	FVector3f SignalWeights = FVector3f(0.2126f, 0.7152f, 0.0722f);

	/** Sensor auto-exposure (GPU sensor path, ROADMAP 3B). */
	FSensorExposureConfig Exposure;

	/** ROADMAP 4A: the AE for the thermal radiance input (signal = L / B(300 K), ~1 for a 300 K scene):
	 *  the photon gain exposes a 300 K scene to mid-range. Separate from Exposure, which the
	 *  thermal.enabled: false luminance proxy keeps using. */
	FSensorExposureConfig ThermalExposure = []
	{
		FSensorExposureConfig E;
		E.MinGainEv       = -8.0f;
		E.MaxPhotonGainEv = 0.0f;
		E.TargetGrey      = 0.5f;
		E.ManualGainEv    = -1.0f;
		return E;
	}();

	// Radiance-Based AGC (GPU sensor path, IR percentile stretch)
	/** Enable histogram-stretch AGC (typically IR). */
	bool  bAGCEnabled        = false;
	/** Percentile for black point [0, 1] (default 0.01 = 1st percentile). */
	float AGCLowPercentile   = 0.01f;
	/** Percentile for white point [0, 1] (default 0.99 = 99th percentile). */
	float AGCHighPercentile  = 0.99f;
	/** Number of frames for AGC convergence (0 = instant, 1-3 typical). */
	int32 AGCLagFrames       = 0;
	/** ROADMAP 4A: highest display gain (normalised DN) the thermal (radiance) IR AGC may use; the band is
	 *  centred on mid-grey when the cap binds. The 3B.2 luminance proxy AGC is not capped. */
	float AGCMaxDisplayGain  = 40.0f;

	// Sensor-class preset (ROADMAP 3B.2 Task 5): "eo_hd_cmos", "mwir_cooled",
	// "lwir_uncooled". Supplies Optics/Detector defaults; yaml optics:/detector:
	// blocks override individual fields. See Sensor/SensorPresets.h.
	FString Preset;
	/** detector.type as written in yaml (empty = not given); Validate() rejects an unknown name. */
	FString DetectorTypeName;
	/** PCG noise stream seed for this mode's detector/defect patterns. Combined with the mode
	 *  (FSensorController::ModeSeed), so EO and IR never share a fixed pattern. */
	uint32 Seed = 1;
	FSensorOpticsConfig   Optics;
	FSensorDetectorConfig Detector;
};
