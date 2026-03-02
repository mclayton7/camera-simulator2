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
};
