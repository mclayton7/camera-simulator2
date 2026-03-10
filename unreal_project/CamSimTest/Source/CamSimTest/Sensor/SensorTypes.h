// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// ---------------------------------------------------------------------------
// ESensorMode — waveband selector driven by CIGI Sensor Control SensorId
// ---------------------------------------------------------------------------
enum class ESensorMode : uint8
{
	EO  = 0,   // Electro-optical: color RGB passthrough (default)
	IR  = 1,   // LWIR thermal: grayscale + S-curve tone mapping + polarity
	NVG = 2,   // Night-vision: green phosphor + gamma lift
};

// ---------------------------------------------------------------------------
// FSensorModeConfig — per-mode tuning, loaded from camsim_config.yaml
// ---------------------------------------------------------------------------
struct FSensorModeConfig
{
	/** Thermal noise (NETD): Gaussian stdev as fraction of full range [0, 1]. */
	float NETD               = 0.0f;

	/** Static per-pixel bias amplitude [0, 1]. */
	float FixedPatternNoise  = 0.0f;

	/** Corner darkening strength (0 = off, 1 = full black at corners). */
	float Vignetting         = 0.15f;

	/** Enable alternating row brightness modulation (scan-line flicker). */
	bool  bScanLines         = false;

	/** Row brightness reduction fraction for scan lines (0 = invisible). */
	float ScanLineStrength   = 0.05f;

	/** Koschmieder-style IR haze coefficient per metre (0 = off). */
	float IRExtinctionCoeff  = 0.0f;

	/** Additional scene attenuation model: visibility distance in metres (0 = off). */
	float AtmosphericVisibilityM = 0.0f;

	/** Blend strength for the atmospheric attenuation model [0, 2]. */
	float AtmosphereStrength = 1.0f;

	/** Optional color-temperature shift in Kelvin (0 = disabled). */
	float ColorTemperatureK = 0.0f;

	/** Contrast multiplier applied after waveband conversion (1 = neutral). */
	float Contrast = 1.0f;

	/** Brightness bias in normalized range [-1, 1]. */
	float BrightnessBias = 0.0f;

	/** Post-effect blur radius in pixels (0 = off). */
	int32 BlurRadius = 0;

	// --- Phase 16: Sensor Fidelity ---

	// 16A — Radiance-Based AGC
	/** Enable histogram-stretch AGC for IR/NVG. Disabled when AGCManualLevel >= 0. */
	bool  bAGCEnabled        = false;
	/** Percentile for black point [0, 1] (default 0.01 = 1st percentile). */
	float AGCLowPercentile   = 0.01f;
	/** Percentile for white point [0, 1] (default 0.99 = 99th percentile). */
	float AGCHighPercentile  = 0.99f;
	/** Manual black level [0, 255]. -1 = use AGC. When >= 0, AGC is disabled. */
	float AGCManualLevel     = -1.0f;
	/** Manual gain multiplier [0.1, 10]. Used with ManualLevel when AGC is off. */
	float AGCManualGain      = 1.0f;

	// 16B — Quantization & Bit Depth
	/** Effective A/D bit depth (8 = no-op; 4-7 = visible quantization). */
	int32 QuantizationBits   = 8;
	/** Enable ordered Bayer dither before quantization to reduce banding. */
	bool  bQuantizationDither = false;

	// 16C — Hot/Dead Pixel Defects
	/** Total defect pixels to seed (0 = disabled). ~100-200 typical for 1080p. */
	int32 DefectPixelCount   = 0;
	/** Fraction of defects that are hot (255); remainder are dead (0). [0, 1] */
	float DefectHotRatio     = 0.5f;
	/** RNG seed for deterministic defect placement. */
	int32 DefectSeed         = 42;

	// 16D — MTF Gaussian Blur
	/** Gaussian PSF sigma in pixels (0 = disabled). Replaces box blur when > 0. */
	float GaussianSigma      = 0.0f;

	// 16F — AC Banding (IR only)
	/** Sinusoidal banding amplitude in DN [0, 255] (0 = disabled). */
	float ACBandingAmplitude = 0.0f;
	/** Banding spatial frequency: cycles per frame height. */
	float ACBandingFrequency = 4.0f;

	// 16L — NVG IR Pointer (NVG only)
	/** Enable IR laser pointer dot in NVG mode. */
	bool  bIRPointerEnabled  = false;
	/** Pointer screen position X [0, 1] from left edge. */
	float IRPointerX         = 0.5f;
	/** Pointer screen position Y [0, 1] from top edge. */
	float IRPointerY         = 0.5f;
	/** Gaussian spot radius in pixels (1-sigma). */
	float IRPointerRadius    = 8.0f;
	/** Pointer peak brightness [0, 255]. */
	float IRPointerIntensity = 240.0f;

	// 16E — Thermal Drift (IR only)
	/** Enable gradual IR baseline shift over time. */
	bool  bThermalDriftEnabled = false;
	/** Drift rate in DN per second (positive = warming). */
	float ThermalDriftRate     = 0.5f;
	/** Auto-NUC reset interval in seconds (0 = no auto-NUC). */
	float NUCIntervalSec       = 30.0f;

	// 16G — Auto-Exposure Lag (IR/NVG, modifies AGC)
	/** Number of frames for AGC convergence (0 = instant, 1-3 typical). */
	int32 AGCLagFrames         = 0;

	// 16H — Rolling Shutter (EO only)
	/** Rolling shutter blend strength [0, 1] (0 = disabled/global shutter). */
	float RollingShutterStrength = 0.0f;

	// 16I — Platform Vibration (all modes)
	/** Subpixel vibration amplitude in pixels (Gaussian 1-sigma, 0 = disabled). */
	float VibrationAmplitude   = 0.0f;

	// 16J — Sensor Gain/Offset Noise (IR only)
	/** Per-frame multiplicative gain jitter (fractional, 0 = disabled). */
	float GainJitter           = 0.0f;
	/** Per-frame additive offset jitter in DN (0 = disabled). */
	float OffsetJitter         = 0.0f;

	// 16K — Sun Glint (EO only)
	/** Sun glint boost intensity (0 = disabled). */
	float SunGlintIntensity    = 0.0f;
	/** Pixel brightness threshold above which glint is applied [0, 255]. */
	float SunGlintThreshold    = 220.0f;
	/** Glint highlight spread exponent (higher = sharper falloff). */
	float SunGlintSpread       = 2.0f;
};

// ---------------------------------------------------------------------------
// FSensorQualityConfig — global quality profile applied across sensor modes
// ---------------------------------------------------------------------------
struct FSensorQualityConfig
{
	/** Scales NETD and fixed-pattern noise amplitudes. */
	float NoiseScale = 1.0f;

	/** Scales vignetting strength. */
	float VignettingScale = 1.0f;

	/** Scales scan-line strength. */
	float ScanLineScale = 1.0f;

	/** Scales atmospheric attenuation terms. */
	float AtmosphereScale = 1.0f;

	/** Additional blur radius (pixels) added on top of per-mode blur. */
	int32 BlurRadius = 0;

	/** Global contrast multiplier applied after mode-level contrast. */
	float Contrast = 1.0f;

	/** Global brightness bias in normalized range [-1, 1]. */
	float BrightnessBias = 0.0f;

	/** Scales GaussianSigma for MTF degradation across all modes. */
	float GaussianSigmaScale = 1.0f;
};
