// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// ---------------------------------------------------------------------------
// ESensorMode — waveband selector driven by CIGI Sensor Control SensorId
// ---------------------------------------------------------------------------
enum class ESensorMode : uint8
{
	EO = 0,   // Electro-optical: color RGB passthrough (default)
	IR = 1,   // LWIR thermal: grayscale + S-curve tone mapping + polarity
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
	float MaxGainEv           = -6.0f;
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

	// Radiance-Based AGC (GPU sensor path, IR percentile stretch)
	/** Enable histogram-stretch AGC (typically IR). */
	bool  bAGCEnabled        = false;
	/** Percentile for black point [0, 1] (default 0.01 = 1st percentile). */
	float AGCLowPercentile   = 0.01f;
	/** Percentile for white point [0, 1] (default 0.99 = 99th percentile). */
	float AGCHighPercentile  = 0.99f;
	/** Number of frames for AGC convergence (0 = instant, 1-3 typical). */
	int32 AGCLagFrames       = 0;

	// optics/detector/preset added in Task 5
};
