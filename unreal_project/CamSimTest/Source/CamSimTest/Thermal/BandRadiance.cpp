// Copyright CamSim Contributors. All Rights Reserved.

#include "Thermal/BandRadiance.h"
#include <cmath>

namespace
{
	constexpr double PlanckH = 6.62607015e-34;   // J s
	constexpr double LightC  = 2.99792458e8;     // m s^-1
	constexpr double BoltzK  = 1.380649e-23;     // J K^-1
}

double FBandRadiance::SpectralRadiance(double LambdaM, double TK)
{
	if (!(LambdaM > 0.0) || !(TK > 0.0)) return 0.0;
	const double X = PlanckH * LightC / (LambdaM * BoltzK * TK);
	if (X > 700.0) return 0.0;
	const double L5 = LambdaM * LambdaM * LambdaM * LambdaM * LambdaM;
	return 2.0 * PlanckH * LightC * LightC / (L5 * std::expm1(X));
}

double FBandRadiance::IntegrateBand(double TK, double InLoUm, double InHiUm, int32 Steps)
{
	if (!(InLoUm > 0.0) || !(InHiUm > InLoUm)) return 0.0;
	const int32 N = FMath::Max(2, Steps + (Steps & 1));
	const double U0 = FMath::Loge(InLoUm * 1e-6), U1 = FMath::Loge(InHiUm * 1e-6);
	const double Hs = (U1 - U0) / N;
	auto F = [TK](double U) { const double L = FMath::Exp(U); return SpectralRadiance(L, TK) * L; };
	double Sum = F(U0) + F(U1);
	for (int32 I = 1; I < N; ++I) Sum += ((I & 1) ? 4.0 : 2.0) * F(U0 + I * Hs);
	return Sum * Hs / 3.0;
}

void FBandRadiance::Build(double InLoUm, double InHiUm)
{
	LoUm = InLoUm;
	HiUm = InHiUm;
	const double MinK = FThermalFrameParams::LutMinK, MaxK = FThermalFrameParams::LutMaxK;
	const double Step = (MaxK - MinK) / (FThermalFrameParams::LutSize - 1);
	for (int32 I = 0; I < FThermalFrameParams::LutSize; ++I)
	{
		const double B = IntegrateBand(MinK + I * Step, LoUm, HiUm, LutSteps);
		LogLut[I] = static_cast<float>(FMath::Loge(FMath::Max(B, 1e-300)));   // never ln 0 (NaN in the interpolation)
	}
	bBuilt = true;
}
