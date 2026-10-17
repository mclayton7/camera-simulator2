// Copyright CamSim Contributors. All Rights Reserved.

#include "Sensor/Radiometry.h"
#include "Thermal/BandRadiance.h"

namespace CamSimRadiometryDetail
{
	constexpr double SourceTempK   = 5800.0;           // daylight / solar blackbody
	constexpr double LumensPerWatt = 683.0;            // photopic peak efficacy

	/** CIE 1931 photopic luminosity, Gaussian approximation. */
	double Photopic(double LambdaUm) { return 1.019 * FMath::Exp(-285.4 * FMath::Square(LambdaUm - 0.559)); }
}

bool CamSimRadiometry::IsRadiometric(const FSensorModeConfig& M)
{
	return M.Detector.Type == ESensorDetectorType::Photon && M.Optics.Transmission > 0.0f && M.Detector.Qe > 0.0f;
}

double CamSimRadiometry::PhotonsPerLumen(float BandLoUm, float BandHiUm)
{
	// Game thread only (config load, sensor controller), so the cache is unguarded.
	static TMap<TPair<float, float>, double> Cache;
	const TPair<float, float> Key(BandLoUm, BandHiUm);
	if (const double* Hit = Cache.Find(Key)) return *Hit;

	const int32 Steps = FMath::RoundToInt32((static_cast<double>(BandHiUm) - BandLoUm) * 1000.0);   // 1 nm
	double Photons = 0.0, Lumens = 0.0;
	for (int32 I = 0; I < Steps; ++I)
	{
		const double LambdaUm = BandLoUm + (I + 0.5) * 1e-3;   // midpoint of each 1 nm step (Δλ cancels)
		const double LambdaM  = LambdaUm * 1e-6;
		// Planck B(λ, T) in W / (m² sr m) (FBandRadiance's); the overall scale cancels in photons / lumen.
		const double B        = FBandRadiance::SpectralRadiance(LambdaM, CamSimRadiometryDetail::SourceTempK);
		Photons += B * LambdaM / (FBandRadiance::PlanckH * FBandRadiance::LightC);
		Lumens  += CamSimRadiometryDetail::LumensPerWatt * B * CamSimRadiometryDetail::Photopic(LambdaUm);
	}
	const double Result = Lumens > 0.0 ? Photons / Lumens : 0.0;
	Cache.Add(Key, Result);
	return Result;
}

double CamSimRadiometry::ElectronsPerSecondPerNit(const FSensorModeConfig& M)
{
	if (!IsRadiometric(M)) return 0.0;
	const double N      = M.Optics.FNumber;
	const double PitchM = static_cast<double>(M.Optics.PixelPitchUm) * 1e-6;
	return UE_DOUBLE_PI / (4.0 * N * N) * M.Optics.Transmission * PitchM * PitchM * M.Detector.Qe
		* PhotonsPerLumen(M.Detector.BandLoUm, M.Detector.BandHiUm);
}

FSensorExposureConfig CamSimRadiometry::EffectiveExposure(const FSensorModeConfig& M)
{
	FSensorExposureConfig E = M.Exposure;
	const double R = ElectronsPerSecondPerNit(M);
	if (R > 0.0 && M.Detector.FullWellE > 0.0f)
	{
		const double TMaxS = static_cast<double>(M.Detector.IntegrationMaxMs) * 1e-3;
		const double TMinS = static_cast<double>(M.Detector.IntegrationMinUs) * 1e-6;
		E.MaxPhotonGainEv = static_cast<float>(FMath::Log2(R * TMaxS / M.Detector.FullWellE));
		E.MinGainEv       = static_cast<float>(FMath::Log2(R * TMinS / M.Detector.FullWellE));
	}
	return E;
}

double CamSimRadiometry::IntegrationSeconds(const FSensorModeConfig& M, float PhotonGain)
{
	const double R = ElectronsPerSecondPerNit(M);
	return R > 0.0 ? static_cast<double>(PhotonGain) * M.Detector.FullWellE / R : 0.0;
}
