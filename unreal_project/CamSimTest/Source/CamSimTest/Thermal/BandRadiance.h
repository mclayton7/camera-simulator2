// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ThermalFrameParams.h"

/**
 * In-band blackbody radiance (ROADMAP 4A): Planck's spectral radiance integrated over a detector band
 * [LoUm, HiUm] in W m^-2 sr^-1, and its LUT over FThermalFrameParams::LutMinK..LutMaxK (ln B, interpolated
 * linearly: <= 0.02 % error from 150 K up, CamSim.Thermal.Planck.LutMatchesIntegral). Pure; game thread.
 */
class CAMSIMTEST_API FBandRadiance
{
public:
	/** Simpson steps per LUT entry (smooth integrand over a narrow band: far below the LUT's interpolation error). */
	static constexpr int32 LutSteps = 512;

	/** Planck spectral radiance B_lambda(T), W m^-2 sr^-1 m^-1; 0 for non-positive inputs or an underflowing exponent. */
	static double SpectralRadiance(double LambdaM, double TK);
	/** Integral of B_lambda over [LoUm, HiUm]: Simpson in u = ln(lambda) of B_lambda * lambda (Steps rounded up to even). 0 for an empty band. */
	static double IntegrateBand(double TK, double LoUm, double HiUm, int32 Steps = 4096);

	/** Fill the LUT for the band (1024 * LutSteps evaluations, a few ms, once per band change). */
	void Build(double InLoUm, double InHiUm);
	bool IsBuilt() const { return bBuilt; }
	double GetLoUm() const { return LoUm; }
	double GetHiUm() const { return HiUm; }
	float Radiance(float TK) const { return ThermalLutRadiance(LogLut, TK); }
	const float* GetLogLut() const { return LogLut; }

private:
	float  LogLut[FThermalFrameParams::LutSize] = {};
	double LoUm = 0.0, HiUm = 0.0;
	bool   bBuilt = false;
};
