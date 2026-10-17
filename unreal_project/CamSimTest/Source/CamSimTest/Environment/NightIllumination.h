// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Environment/Ephemeris.h"

/**
 * Sun/moon/night-floor illuminance for the night sky (ROADMAP 6A). Pure; game thread.
 * Moon: Krisciunas & Schaefer 1991 (PASP 103, 1033). Extinction: Kasten & Young 1989 air mass,
 * k_V = 0.172. Cloud: Kasten & Czeplak 1980.
 */
struct FNightIlluminationInput
{
	FEphemerisResult Ephem;
	double CloudCover01   = 0.0;    // CIGI Weather Control coverage / 100
	double NightFloorLux  = 0.002;  // environment.celestial.night_floor_lux
	bool   bSunEnabled = true, bMoonEnabled = true;
};

struct FNightIlluminationResult
{
	double SunToaLux  = 0;          // set on the sun light (atmosphere applies transmittance)
	double MoonToaLux = 0;          // set on the moon light
	double FloorRadianceCdM2 = 0;   // dome floor term = floor lux x cloud factor / (pi x FloorTransmittanceMean)
	double SunGroundLux  = 0;       // expected horizontal illuminance at the ground (tests, log, night_check)
	double MoonGroundLux = 0;
	FLinearColor MoonColor = FLinearColor::White;   // normalised (max channel 1)
};

namespace FNightIllumination
{
	FNightIlluminationResult Compute(const FNightIlluminationInput& In);
	double KastenYoungAirMass(double ElDeg);            // 40 at/below the horizon (-0.5 deg)
	double Transmittance(double ElDeg);                  // 10^(-0.4 * 0.172 * X), 0 below el -0.5 deg
	double MoonToaLux(double PhaseAngleDeg, double DistanceKm);   // Krisciunas & Schaefer 1991
	double CloudFactor(double Cover01);                  // 1 - 0.75 * cover^3.4 (Kasten & Czeplak)
	/** Cosine-weighted mean of Transmittance over the upper hemisphere (~0.755): the dome draws the floor as L * T(view
	 *  elevation), so L = E / (pi * this) delivers E at the ground. */
	double FloorTransmittanceMean();
	/** The brightest moon-disc luminance the dome can draw (full moon at perigee, Lambert peak, zenith transmittance),
	 *  cd/m2: the brightest sky content at night. */
	double MaxMoonDiscLuminanceCdM2();
	/** Cap on UE's pre-exposure (EV) so MaxMoonDiscLuminanceCdM2 stays at 16384 (1/4 of fp16's max) in scene colour. */
	double SceneExposureCapEv();
	/** The moon light's CloudScatteredLuminanceScale: white while the moon is up and the sun below the horizon, else
	 *  black (UE's second cloud light is expensive). */
	FLinearColor MoonCloudScatterScale(double SunElDeg, bool bMoonUp);
}

namespace FMoonDisc
{
	/** Mean over the moon's projected disc of the Lambert shading max(n.s, 0), s = sun direction seen from the moon,
	 *  for a phase angle alpha (0 = full). The dome divides the disc luminance by it so the disc integrates to the
	 *  input illuminance: L(p) = E / Omega * shade(p) / MeanShading(alpha). */
	double MeanShading(double PhaseAngleDeg);
	/** Lambert shading of the disc point (u, v) in [-1, 1]^2 (u toward the sun's side, v up), 0 outside. */
	double Shade(double U, double V, double PhaseAngleDeg);
}
