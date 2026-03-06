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
// FSensorModeConfig — per-mode tuning, loaded from camsim_config.json
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
};
