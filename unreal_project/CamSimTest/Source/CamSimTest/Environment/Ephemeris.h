// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Sun and moon positions, moon phase and sidereal time for an observer on the WGS-84 ellipsoid
 * (ROADMAP 6A). Pure and stateless; game thread.
 *
 * Sources: J. Meeus, *Astronomical Algorithms* (2nd ed.): ch. 12 (sidereal time), ch. 22
 * (nutation, low accuracy), ch. 25 (sun, low accuracy), ch. 40 (topocentric parallax),
 * ch. 47 (moon, tables 47.A/47.B), ch. 48 (illuminated fraction); ΔT from the Espenak–Meeus
 * polynomials (NASA eclipse site). Accuracy vs JPL DE421 (scripts/sky/make_ephemeris_refs.py):
 * sun ≤ 0.05°, moon ≤ 0.2°, illuminated fraction ≤ 0.01, moon distance ≤ 300 km, LAST ≤ 0.01°.
 * Az/el are geometric (no atmospheric refraction).
 */
struct FEphemerisResult
{
	double SunAzDeg = 0, SunElDeg = 0;        // topocentric, compass azimuth from north, geometric (no refraction)
	double MoonAzDeg = 0, MoonElDeg = 0;      // topocentric (parallax applied), geometric
	double MoonPhaseAngleDeg = 180;           // sun-moon-observer angle α: 0 = full, 180 = new
	double MoonIlluminatedFraction = 0;       // (1 + cos α) / 2
	double MoonDistanceKm = 384400;           // observer to moon centre (topocentric)
	double SunDistanceAu = 1;
	double LocalSiderealDeg = 0;              // local apparent sidereal time, degrees [0, 360)
	FVector3d SunEnu = FVector3d::ZeroVector; // unit vectors, East-North-Up at the observer
	FVector3d MoonEnu = FVector3d::ZeroVector;
};

namespace FEphemeris
{
	/** Sun/moon for an observer at geodetic LatDeg/LonDeg (east positive), HeightM above the ellipsoid. */
	FEphemerisResult Compute(const FDateTime& Utc, double LatDeg, double LonDeg, double HeightM);
	/** TT − UT in seconds (Espenak–Meeus polynomial; years clamped into [1986, 2050]). */
	double DeltaTSeconds(int32 Year, int32 Month);
	/** Julian day of the UTC instant. */
	double JulianDayUtc(const FDateTime& Utc);
	/** Compass azimuth / elevation → East-North-Up unit vector: (sin az cos el, cos az cos el, sin el). */
	FVector3d AzElToEnu(double AzDeg, double ElDeg);
}
