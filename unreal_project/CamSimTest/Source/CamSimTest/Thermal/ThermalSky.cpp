// Copyright CamSim Contributors. All Rights Reserved.

#include "Thermal/ThermalSky.h"
#include "Thermal/BandRadiance.h"

namespace
{
	/** Midpoint elevations of the hemisphere integral and their cosine weights 2 sin(el) cos(el) d(el), normalised to sum 1. */
	template <typename TFn>
	double CosineWeightedMean(TFn&& Fn)
	{
		double Sum = 0.0, Weights = 0.0;
		const double DEl = 0.5 * UE_DOUBLE_PI / FThermalSky::HemiSamples;
		for (int32 I = 0; I < FThermalSky::HemiSamples; ++I)
		{
			const double El = (I + 0.5) * DEl;
			const double W = 2.0 * FMath::Sin(El) * FMath::Cos(El) * DEl;
			Sum += W * Fn(FMath::Sin(El));
			Weights += W;
		}
		return Sum / Weights;
	}
}

double FThermalSky::ZenithEmissivity(double TairK)
{
	const double R = ClearZenithK(TairK) / TairK;
	return FMath::Clamp(R * R * R * R, 0.0, 1.0);
}

double FThermalSky::SkyTemperatureK(double TairK, double Cloud, double SinEl)
{
	const double C = FMath::Clamp(Cloud, 0.0, 1.0);
	const double Ratio = (1.0 - C) * Emissivity(ZenithEmissivity(TairK), SinEl) + C;
	return TairK * FMath::Sqrt(FMath::Sqrt(Ratio));
}

double FThermalSky::HemisphericEmissivity(double TairK)
{
	const double EpsZ = ZenithEmissivity(TairK);
	return CosineWeightedMean([EpsZ](double SinEl) { return Emissivity(EpsZ, SinEl); });
}

double FThermalSky::DownwellingIrradiance(double TairK, double Cloud)
{
	const double C = FMath::Clamp(Cloud, 0.0, 1.0);
	return Sigma * ((1.0 - C) * HemisphericEmissivity(TairK) + C) * TairK * TairK * TairK * TairK;
}

double FThermalSky::HemisphereBandRadiance(const FBandRadiance& Band, double TairK, double Cloud)
{
	return CosineWeightedMean([&Band, TairK, Cloud](double SinEl)
	{
		return static_cast<double>(Band.Radiance(static_cast<float>(SkyTemperatureK(TairK, Cloud, SinEl))));
	});
}
