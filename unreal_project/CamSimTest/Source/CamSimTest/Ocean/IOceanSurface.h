// Copyright CamSim Contributors. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * Abstract ocean surface interface.
 *
 * Implemented by FGerstnerOceanSurface (GPU Gerstner + CPU analytic eval).
 * Future: FWaterPluginOceanSurface wrapping UWaterBodyComponent.
 *
 * Wave direction is not parameterised in Sprint 1 — fixed direction in Material.
 * Units: GetSurfaceHeightAt() returns UE units (cm). WorldXY is world-space XY in cm.
 */
class IOceanSurface
{
public:
	virtual ~IOceanSurface() = default;

	/**
	 * Set wave parameters from physical values.
	 * WaveHtM:        wave height in metres (peak-to-trough / 2 = amplitude)
	 * WaveLenM:       wave length in metres
	 * AmplitudeScale: YAML multiplier applied on top of WaveHtM
	 * FrequencyScale: YAML multiplier applied on top of 1/WaveLenM
	 * Choppiness:     [0,1] — 0 = pure sine, 1 = sharp Gerstner peaks
	 */
	virtual void SetWaveParams(float WaveHtM, float WaveLenM,
	                           float AmplitudeScale, float FrequencyScale,
	                           float Choppiness) = 0;

	virtual void Tick(float DeltaTime) = 0;

	/**
	 * Returns ocean surface height at world-space XY in UE units (cm).
	 * Used by vessel motion (19C) to sample bow/stern/port/starboard.
	 * Returns 0.0f if ocean is disabled or WaveHt == 0.
	 */
	virtual float GetSurfaceHeightAt(FVector2D WorldXY) const = 0;

	virtual void SetEnabled(bool bEnabled) = 0;
};
