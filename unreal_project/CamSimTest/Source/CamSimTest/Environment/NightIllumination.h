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
	double MoonToaLux = 0;          // top of atmosphere (the dome's disc luminance)
	double MoonLightLux = 0;        // set on the moon light: MoonToaLux x the cloud factor (the moon casts no cloud shadows)
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
	/** The brightest moon-disc luminance the dome can draw (full moon at perigee, the brightest albedo x lunar-Lambert
	 *  point: FMoonDisc::MaxPeakToMean x the disc mean, zenith transmittance),
	 *  cd/m2: the brightest sky content at night. */
	double MaxMoonDiscLuminanceCdM2();
	/** Cap on UE's pre-exposure (EV) so MaxMoonDiscLuminanceCdM2 stays at 16384 (1/4 of fp16's max) in scene colour. */
	double SceneExposureCapEv();
	/** The moon light's CloudScatteredLuminanceScale: white while the moon is up and the sun below the horizon, else
	 *  black (UE's second cloud light is expensive). */
	FLinearColor MoonCloudScatterScale(double SunElDeg, bool bMoonUp);
	/** Ozone (Chappuis band) absorption per km at the peak of SkyAtmosphere's "other absorption" tent (25 km, 15 km half
	 *  width), integrated over the linear sRGB channels (scripts/sky/ozone_rgb.py): R 2.46e-3, G 1.77e-3, B 0. UE's
	 *  default (0.65e-3, 1.881e-3, 0.085e-3) samples the cross section at 680/550/440 nm, which puts the Chappuis peak
	 *  (~600 nm) in green: over twilight's long ozone paths it filtered the sky and the ground under it magenta
	 *  (docs/night.md "Twilight colour"). */
	FLinearColor OzoneAbsorptionPerKm();
}

namespace FMoonDisc
{
	/**
	 * Lunar-Lambert weight L(alpha) (McEwen 1991, Icarus 92, 298; polynomial of McEwen 1996, LPSC XXVII, 841, alpha in
	 * degrees): 1 + A alpha + B alpha^2 + C alpha^3, A = -0.019, B = 2.42e-4, C = -1.46e-6, clamped to [0, 1]. 1 at full
	 * moon (pure Lommel-Seeliger), falling with phase toward Lambert.
	 */
	double LunarLambertL(double PhaseAngleDeg);
	/** Lunar-Lambert reflectance (relative): 2 L mu0 / (mu0 + mu) + (1 - L) mu0 for mu0 = cos(incidence) > 0, else 0. */
	double Photometric(double Mu0, double Mu, double L);
	/** Lunar-Lambert shading of the disc point (u, v) in [-1, 1]^2 (u toward the sun's side, v up), uniform albedo, sun
	 *  at s = (sin alpha, 0, cos alpha) in the (u, v, toward-viewer) frame; 0 outside the disc or unlit. */
	double Shade(double U, double V, double PhaseAngleDeg);
	/** Mean of Shade over the projected disc (numerical, 1 at full moon). The dome normalises with the albedo-weighted
	 *  FMoonDisc::MeanRadiance (MoonSurface.h); this is its uniform-albedo case (tests, docs). */
	double MeanShading(double PhaseAngleDeg);
}
