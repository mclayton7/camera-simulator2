// Copyright CamSim Contributors. All Rights Reserved.

#include "Environment/NightIllumination.h"

namespace
{
	constexpr double SunSolarIlluminanceLux = 127500.0;   // at 1 AU
	constexpr double ExtinctionKV = 0.172;
	constexpr double HorizonCutoffDeg = -0.5;
}

double FNightIllumination::KastenYoungAirMass(double ElDeg)
{
	if (ElDeg <= HorizonCutoffDeg)
	{
		return 40.0;
	}
	const double SinEl = FMath::Sin(FMath::DegreesToRadians(ElDeg));
	return 1.0 / (SinEl + 0.50572 * FMath::Pow(ElDeg + 6.07995, -1.6364));
}

double FNightIllumination::Transmittance(double ElDeg)
{
	if (ElDeg < HorizonCutoffDeg)
	{
		return 0.0;
	}
	return FMath::Pow(10.0, -0.4 * ExtinctionKV * KastenYoungAirMass(ElDeg));
}

double FNightIllumination::MoonToaLux(double PhaseAngleDeg, double DistanceKm)
{
	const double A = FMath::Abs(PhaseAngleDeg);
	const double Mag = 3.84 + 0.026 * A + 4e-9 * A * A * A * A;
	const double Ratio = 384400.0 / FMath::Max(DistanceKm, 1.0);
	return FMath::Pow(10.0, -0.4 * Mag) * 10.764 * Ratio * Ratio;
}

double FNightIllumination::FloorTransmittanceMean()
{
	// Integral of T(el) sin(el) cos(el) d(el) over [0, 90] divided by that of sin(el) cos(el) (= 1/2); midpoint rule.
	static const double Mean = []
	{
		constexpr int32 N = 4096;
		double Sum = 0.0;
		for (int32 I = 0; I < N; ++I)
		{
			const double El = (I + 0.5) / N * 90.0;
			const double Rad = FMath::DegreesToRadians(El);
			Sum += Transmittance(El) * FMath::Sin(Rad) * FMath::Cos(Rad);
		}
		return Sum * (UE_DOUBLE_PI / 2.0 / N) * 2.0;
	}();
	return Mean;
}

double FNightIllumination::MaxMoonDiscLuminanceCdM2()
{
	constexpr double PerigeeKm = 356000.0;   // the closest a full moon gets (356,375 km in 1912-2100)
	constexpr double MoonRadiusKm = 1737.4;
	const double R = FMath::Asin(MoonRadiusKm / PerigeeKm);
	const double Omega = 2.0 * UE_DOUBLE_PI * (1.0 - FMath::Cos(R));
	// FSkyDomeDriver: disc luminance = TOA / Omega / MeanShading(alpha) x shade (<= 1) x T(view); the largest at alpha 0.
	return MoonToaLux(0.0, PerigeeKm) / Omega / FMoonDisc::MeanShading(0.0) * Transmittance(90.0);
}

double FNightIllumination::SceneExposureCapEv()
{
	return FMath::Log2(16384.0 / MaxMoonDiscLuminanceCdM2());
}

FLinearColor FNightIllumination::MoonCloudScatterScale(double SunElDeg, bool bMoonUp)
{
	return (bMoonUp && SunElDeg < 0.0) ? FLinearColor::White : FLinearColor::Black;
}

FLinearColor FNightIllumination::OzoneAbsorptionPerKm()
{
	return FLinearColor(0.00246016f, 0.00177045f, 0.0f);
}

double FNightIllumination::CloudFactor(double Cover01)
{
	return 1.0 - 0.75 * FMath::Pow(FMath::Clamp(Cover01, 0.0, 1.0), 3.4);
}

FNightIlluminationResult FNightIllumination::Compute(const FNightIlluminationInput& In)
{
	FNightIlluminationResult R;
	const double Cloud = CloudFactor(In.CloudCover01);
	const FEphemerisResult& E = In.Ephem;

	if (In.bSunEnabled)
	{
		const double R2 = FMath::Max(E.SunDistanceAu * E.SunDistanceAu, 1e-6);
		R.SunToaLux = SunSolarIlluminanceLux / R2;
		R.SunGroundLux = R.SunToaLux * Transmittance(E.SunElDeg)
			* FMath::Max(FMath::Sin(FMath::DegreesToRadians(E.SunElDeg)), 0.0) * Cloud;
	}
	if (In.bMoonEnabled)
	{
		R.MoonToaLux = MoonToaLux(E.MoonPhaseAngleDeg, E.MoonDistanceKm);
		// Overcast dims the moon by the floor's factor: the moon light has no cloud shadows (a second cloud shadow map).
		R.MoonLightLux = R.MoonToaLux * Cloud;
		R.MoonGroundLux = R.MoonToaLux * Transmittance(E.MoonElDeg)
			* FMath::Max(FMath::Sin(FMath::DegreesToRadians(E.MoonElDeg)), 0.0) * Cloud;
	}
	// The dome draws the floor attenuated by T(view elevation): normalise so it delivers NightFloorLux at the ground.
	R.FloorRadianceCdM2 = In.NightFloorLux * Cloud / (UE_DOUBLE_PI * FloorTransmittanceMean());
	R.MoonColor = FLinearColor(1.0f, 0.92f, 0.80f);   // ~4,100 K; lunar B-V = 0.92
	return R;
}

double FMoonDisc::Shade(double U, double V, double PhaseAngleDeg)
{
	const double R2 = U * U + V * V;
	if (R2 > 1.0)
	{
		return 0.0;
	}
	const double Alpha = FMath::DegreesToRadians(PhaseAngleDeg);
	const double Nz = FMath::Sqrt(1.0 - R2);
	return FMath::Max(U * FMath::Sin(Alpha) + Nz * FMath::Cos(Alpha), 0.0);
}

double FMoonDisc::MeanShading(double PhaseAngleDeg)
{
	const double Alpha = FMath::DegreesToRadians(FMath::Clamp(PhaseAngleDeg, 0.0, 180.0));
	const double Mean = (2.0 / 3.0) * (FMath::Sin(Alpha) + (UE_DOUBLE_PI - Alpha) * FMath::Cos(Alpha)) / UE_DOUBLE_PI;
	return FMath::Max(Mean, 1e-6);
}
