// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UWorld;
class UCamSimSubsystem;
class FOceanWaves;
struct FThermalFrameInputs;

/**
 * The running world → FThermalFrameInputs (ROADMAP 4A): CIGI atmosphere/weather (via ACamSimEnvironment's snapshot),
 * the atmosphere sun light, the ocean and the live stencil-tagged entities. The helpers are pure; GatherFrameInputs is
 * the thin world-reading wrapper (game thread). Every value it hands the builder is finite (ruling R6).
 */
namespace CamSimThermal
{
	/** Illuminance (lux) on a horizontal surface from the atmosphere sun light: intensity × lum(colour)
	 *  × lum(ground transmittance) × max(sin elevation, 0). 0 when no light / below horizon / non-finite. */
	CAMSIMTEST_API double SunIlluminanceLux(double IntensityLux, const FLinearColor& Color, const FLinearColor& Transmittance,
		double SunElevationDeg);

	/** Max wave amplitude (sum of component amplitudes, m) — the water band's margin. */
	CAMSIMTEST_API double MaxWaveAmplitudeM(const FOceanWaves& Waves);

	/** Writes the camera's geodetic pose when all three values are finite; otherwise leaves Out's defaults. Returns accepted. */
	CAMSIMTEST_API bool SetCameraPose(FThermalFrameInputs& Out, double LatDeg, double LonDeg, double AltHaeM);
	/** Unit up vector; +Z when Up is non-finite or degenerate. */
	CAMSIMTEST_API FVector SanitizeUpWorld(const FVector& Up);
	/** bHasSea only for a finite sea level; a non-finite or negative amplitude becomes 0. */
	CAMSIMTEST_API void SetSea(FThermalFrameInputs& Out, TOptional<double> SeaLevelHaeM, double MaxWaveAmplitudeM);
	/** Non-finite or negative illuminance → 0. */
	CAMSIMTEST_API double SanitizeLux(double Lux);

	/** World → FThermalFrameInputs (game thread). Fields it cannot read keep their defaults. */
	CAMSIMTEST_API void GatherFrameInputs(UWorld* World, const UCamSimSubsystem& Subsystem, double CamLatDeg, double CamLonDeg,
		double CamAltHaeM, const FVector& UpWorld, FThermalFrameInputs& Out);
}
