// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FBandRadiance;

/**
 * Effective sky temperature (ROADMAP 4A): Swinbank clear-sky zenith T_clear = 0.0552 T_air^1.5, angular
 * emissivity eps(el) = 1 - (1 - eps_z)^(1 / max(sin el, 0.05)) with eps_z = (T_clear / T_air)^4, and cloud
 * cover blending T_sky^4 = (1 - c) eps(el) T_air^4 + c T_air^4. ThermalCS evaluates SkyTemperatureK per
 * sky pixel (CamSimThermalCommon.ush SkyTemperatureK, same expressions). Pure.
 */
struct CAMSIMTEST_API FThermalSky
{
	static constexpr double MinSinEl    = 0.05;
	static constexpr int32  HemiSamples = 64;    // midpoint rule over elevation, cosine-weighted
	static constexpr double Sigma       = 5.670374419e-8;

	static double ClearZenithK(double TairK) { return 0.0552 * FMath::Pow(TairK, 1.5); }
	static double ZenithEmissivity(double TairK);
	static double Emissivity(double EpsZ, double SinEl) { return 1.0 - FMath::Pow(1.0 - EpsZ, 1.0 / FMath::Max(SinEl, MinSinEl)); }
	static double SkyTemperatureK(double TairK, double Cloud, double SinEl);
	static double HemisphericEmissivity(double TairK);
	static double DownwellingIrradiance(double TairK, double Cloud);
	static double HemisphereBandRadiance(const FBandRadiance& Band, double TairK, double Cloud);
};
