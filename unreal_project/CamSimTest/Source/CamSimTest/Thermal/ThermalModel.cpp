// Copyright CamSim Contributors. All Rights Reserved.

#include "Thermal/ThermalModel.h"
#include "Thermal/ThermalSky.h"
#include "Environment/CamSimEnvironment.h"

double FThermalModel::SunElevationDeg(const FThermalSite& S, double LocalSolarHour)
{
	const double Hour = FMath::Fmod(FMath::Fmod(LocalSolarHour, 24.0) + 24.0, 24.0);
	return ACamSimEnvironment::ComputeSunPosition(static_cast<float>(Hour), S.DayOfYear, S.LatDeg).X;
}

double FThermalModel::ClearSkyGhi(double SunElevationDeg)
{
	const double SinEl = FMath::Sin(FMath::DegreesToRadians(SunElevationDeg));
	if (!(SinEl > 0.0)) return 0.0;
	return 1098.0 * SinEl * FMath::Exp(-0.057 / SinEl);
}

double FThermalModel::CloudFactor(double Cloud)
{
	return 1.0 - 0.75 * FMath::Pow(FMath::Clamp(Cloud, 0.0, 1.0), 3.4);
}

double FThermalModel::AirTemperatureK(const FThermalSite& S, double LocalSolarHour)
{
	return S.TairMeanK + 0.5 * S.AirSwingK * FMath::Cos(Omega * (LocalSolarHour - AirPeakHour) * 3600.0);
}

double FThermalModel::ExchangeCoefficient(const FThermalSite& S, const FThermalMaterial& M)
{
	const double T = S.TairMeanK;
	return M.ConvectionWm2K + 4.0 * M.Emissivity * Sigma * T * T * T;
}

double FThermalModel::Forcing(const FThermalSite& S, const FThermalMaterial& M, double LocalSolarHour)
{
	const double Sun = ClearSkyGhi(SunElevationDeg(S, LocalSolarHour)) * CloudFactor(S.Cloud);
	const double Tair = AirTemperatureK(S, LocalSolarHour);
	const double Lsky = FThermalSky::DownwellingIrradiance(Tair, S.Cloud);
	return (1.0 - M.Albedo) * Sun + ExchangeCoefficient(S, M) * Tair + M.Emissivity * (Lsky - Sigma * Tair * Tair * Tair * Tair);
}

FThermalModel::FHarmonics FThermalModel::Fit(TConstArrayView<double> Samples)
{
	check(Samples.Num() == NumSamples);
	FHarmonics Out;
	for (int32 N = 0; N <= NumHarmonics; ++N)
	{
		double Re = 0.0, Im = 0.0;
		for (int32 K = 0; K < NumSamples; ++K)
		{
			const double Th = 2.0 * UE_DOUBLE_PI * N * K / NumSamples;
			Re += Samples[K] * FMath::Cos(Th);
			Im -= Samples[K] * FMath::Sin(Th);
		}
		Out.Re[N] = Re / NumSamples;
		Out.Im[N] = Im / NumSamples;
	}
	return Out;
}

double FThermalModel::Response(const FHarmonics& F, double H, double Inertia, double LocalSolarSec)
{
	double T = F.Re[0] / H;
	for (int32 N = 1; N <= NumHarmonics; ++N)
	{
		const double W = N * Omega;
		const double Q = Inertia * FMath::Sqrt(0.5 * W);   // I sqrt(i w) = Q (1 + i)
		const double A = H + Q, B = Q, D = A * A + B * B;  // H(w) = (A - i B) / D
		const double Hr = A / D, Hi = -B / D;
		const double Ph = W * LocalSolarSec;
		const double C = FMath::Cos(Ph), S = FMath::Sin(Ph);
		const double Er = F.Re[N] * C - F.Im[N] * S, Ei = F.Re[N] * S + F.Im[N] * C;   // F_n e^{i n w t}
		T += 2.0 * (Er * Hr - Ei * Hi);
	}
	return T;
}

bool FThermalModel::Update(const FThermalSite& NewSite, const FThermalMaterialTable& Materials)
{
	const bool bSame = bFitted
		&& NewSite.Year == Site.Year && NewSite.DayOfYear == Site.DayOfYear
		&& FMath::Abs(NewSite.LatDeg - Site.LatDeg) <= RefitDegrees
		&& FMath::Abs(NewSite.LonDeg - Site.LonDeg) <= RefitDegrees
		&& NewSite.Cloud == Site.Cloud && NewSite.TairMeanK == Site.TairMeanK && NewSite.AirSwingK == Site.AirSwingK
		&& Materials.GetVersion() == MaterialsVersion && Classes.Num() == Materials.Num();
	if (bSame) return false;

	Site = NewSite;
	MaterialsVersion = Materials.GetVersion();
	bFitted = true;

	// The site terms of the forcing, once per sample (the per-class part is a weighted sum of them).
	double Sun[NumSamples], Tair[NumSamples], Lsky[NumSamples];
	for (int32 K = 0; K < NumSamples; ++K)
	{
		const double Hour = K * 24.0 / NumSamples;
		Sun[K]  = ClearSkyGhi(SunElevationDeg(Site, Hour)) * CloudFactor(Site.Cloud);
		Tair[K] = AirTemperatureK(Site, Hour);
		Lsky[K] = FThermalSky::DownwellingIrradiance(Tair[K], Site.Cloud);
	}
	Classes.SetNum(Materials.Num());
	for (int32 C = 0; C < Materials.Num(); ++C)
	{
		const FThermalMaterial& M = Materials.Get(C);
		FClass& Out = Classes[C];
		Out.bWater  = M.Source == EThermalTemperatureSource::Water;
		Out.H       = ExchangeCoefficient(Site, M);
		Out.Inertia = M.ThermalInertia;
		if (Out.bWater) continue;
		double F[NumSamples];
		for (int32 K = 0; K < NumSamples; ++K)
		{
			const double T = Tair[K];
			F[K] = (1.0 - M.Albedo) * Sun[K] + Out.H * T + M.Emissivity * (Lsky[K] - Sigma * T * T * T * T);
		}
		Out.F = Fit(TConstArrayView<double>(F, NumSamples));
	}
	return true;
}

double FThermalModel::TemperatureK(int32 Class, double LocalSolarSec, double WaterTempK) const
{
	if (Classes.Num() == 0) return Site.TairMeanK;
	const FClass& C = Classes[FMath::Clamp(Class, 0, Classes.Num() - 1)];
	if (C.bWater)
	{
		return WaterTempK + WaterSwingK * FMath::Cos(Omega * (LocalSolarSec - AirPeakHour * 3600.0));
	}
	return Response(C.F, C.H, C.Inertia, LocalSolarSec);
}
