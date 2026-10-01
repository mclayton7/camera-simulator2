// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Thermal/ThermalMaterials.h"

/** Where and when the class temperatures are evaluated (ROADMAP 4A). Local solar time throughout. */
struct FThermalSite
{
	int32  Year      = 2026;
	int32  DayOfYear = 172;
	double LatDeg    = 0.0;
	double LonDeg    = 0.0;
	double TairMeanK = 288.15;   // daily mean air temperature
	double AirSwingK = 8.0;      // peak-to-peak
	double Cloud     = 0.0;      // [0, 1]
};

/**
 * Closed-form surface temperature per class (ROADMAP 4A). A class is a semi-infinite solid of thermal inertia
 * I with a linearised surface exchange h = h_c + 4 eps sigma T_mean^3. For the periodic forcing
 *   F(t) = (1 - a) S(t) (1 - 0.75 c^3.4) + h T_air(t) + eps (L_sky(t) - sigma T_air(t)^4)
 * sampled every 15 min over the day (S: Haurwitz clear-sky GHI at the sun elevation of
 * ACamSimEnvironment::ComputeSunPosition; L_sky: FThermalSky::DownwellingIrradiance) with Fourier
 * coefficients F_n (n = 0..6), the exact response is T(t) = F_0 / h + sum 2 Re[F_n e^{i n w t} H(n w)],
 * H(w) = 1 / (h + I sqrt(i w)). No integration state: any sim time evaluates directly. Water-source classes
 * are the water temperature +- WaterSwingK (peak 15:00). Game thread.
 */
class CAMSIMTEST_API FThermalModel
{
public:
	static constexpr int32  NumSamples   = 96;
	static constexpr int32  NumHarmonics = 6;
	static constexpr double DaySeconds   = 86400.0;
	static constexpr double Omega        = 2.0 * UE_DOUBLE_PI / DaySeconds;
	static constexpr double AirPeakHour  = 15.0;
	static constexpr double WaterSwingK  = 0.5;
	static constexpr double Sigma        = 5.670374419e-8;
	static constexpr double RefitDegrees = 0.5;

	/** F_n = Re[n] + i Im[n], n = 0..NumHarmonics (F_0 real). */
	struct FHarmonics
	{
		double Re[NumHarmonics + 1] = {};
		double Im[NumHarmonics + 1] = {};
	};

	static double SunElevationDeg(const FThermalSite& S, double LocalSolarHour);
	static double ClearSkyGhi(double SunElevationDeg);
	static double CloudFactor(double Cloud);
	static double AirTemperatureK(const FThermalSite& S, double LocalSolarHour);
	static double ExchangeCoefficient(const FThermalSite& S, const FThermalMaterial& M);
	static double Forcing(const FThermalSite& S, const FThermalMaterial& M, double LocalSolarHour);
	static FHarmonics Fit(TConstArrayView<double> Samples);
	static double Response(const FHarmonics& F, double H, double Inertia, double LocalSolarSec);

	/** Refit when the date, cloud cover, air temperature or materials change, or lat/lon move by more than RefitDegrees. */
	bool Update(const FThermalSite& NewSite, const FThermalMaterialTable& Materials);
	/** Class temperature at local solar second LocalSolarSec. Class is clamped to the fitted range. */
	double TemperatureK(int32 Class, double LocalSolarSec, double WaterTempK) const;
	const FThermalSite& GetSite() const { return Site; }
	bool IsFitted() const { return bFitted; }

private:
	struct FClass
	{
		bool   bWater  = false;
		double H       = 1.0;
		double Inertia = 0.0;
		FHarmonics F;
	};
	FThermalSite   Site;
	bool           bFitted = false;
	uint32         MaterialsVersion = 0;
	TArray<FClass> Classes;
};
