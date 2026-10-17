// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * The moon's surface for the sky dome's disc (ROADMAP 6A, docs/night.md "Moon disc"): an albedo map, the IAU body
 * orientation and the disc model (lunar-Lambert photometry x albedo) that M_SkyDome's CamSimMoonDisc mirrors
 * (Shaders/Private/CamSimSky.ush). Pure; game thread.
 */

/** Relative albedo, equirectangular in the IAU mean Earth / polar axis frame (Content/NonUFS/Sky/moon_albedo.pgm). */
struct CAMSIMTEST_API FMoonAlbedoMap
{
	int32 Width = 0, Height = 0;
	TArray<uint8> Texels;   // row 0 = latitude +90, column 0 = longitude -180; linear, 255 = the brightest texel

	bool IsEmpty() const { return Width <= 0 || Height <= 0 || Texels.Num() != Width * Height; }
	/** Bilinear at (longitude, latitude) in radians as the GPU samples mip 0 (texel centres, longitude wraps, latitude
	 *  clamps), in [0, 1]; 1 for an empty map (a uniform disc). */
	double Sample(double LonRad, double LatRad) const;

	/** Binary PGM (P5, maxval 255, '#' comments allowed). Empty on any error, with OutError set. */
	static FMoonAlbedoMap ParsePgm(const TArray<uint8>& Bytes, FString& OutError);
	/** Loads Path; missing, an LFS pointer or unreadable: one warning and an empty map. */
	static FMoonAlbedoMap Load(const FString& Path);
	static FString DefaultPath();
};

namespace FMoonOrientation
{
	/**
	 * The moon's body axes in the J2000 equatorial frame (x toward RA 0, z the north celestial pole): OutAxes[0] toward
	 * (longitude 0, latitude 0), [1] toward longitude 90 E, [2] the north pole. IAU WGCCRE 2009 rotation model (Archinal
	 * et al. 2011, Celest. Mech. Dyn. Astron. 109, 101, table 2: pole and prime meridian with the E1..E13 terms; the mean
	 * Earth / polar axis frame of the LROC maps). DaysTt = days since J2000.0 (JD 2451545.0 TT; TDB - TT is < 2 ms).
	 * Optical libration follows from the geometry; the physical libration is in the periodic terms.
	 */
	void IauAxesJ2000(double DaysTt, FVector3d OutAxes[3]);
	/** Selenographic longitude / latitude (degrees) of the point facing direction ToObserver (J2000, from the moon). */
	FVector2d SubPointDeg(const FVector3d Axes[3], const FVector3d& ToObserver);
}

/** The disc's geometry as M_SkyDome sees it (any common orthonormal frame: UE world on the GPU). */
struct FMoonDiscFrame
{
	FVector3d MoonDir = FVector3d::UnitZ();   // observer -> moon centre, unit
	FVector3d SunDir  = FVector3d::UnitX();   // observer -> sun, unit (the sun seen from the moon, to 0.15 deg)
	FVector3d Axes[3] = { FVector3d::UnitX(), FVector3d::UnitY(), FVector3d::UnitZ() };   // moon body axes
	double L = 1.0;                           // lunar-Lambert weight, FMoonDisc::LunarLambertL(phase angle)
};

namespace FMoonDisc
{
	/**
	 * Relative luminance (albedo x lunar-Lambert) of the disc point (u, v) in the unit disc, u along the disc-plane
	 * direction toward the sun and v = MoonDir x u, as CamSimMoonDisc computes it: the surface normal n = u Right + v Up +
	 * sqrt(1 - u^2 - v^2) (-MoonDir), mu0 = n.SunDir, mu = n_z, albedo at n's selenographic coordinates. 0 off the disc
	 * or unlit.
	 */
	CAMSIMTEST_API double Radiance(const FMoonDiscFrame& Frame, const FMoonAlbedoMap& Map, double U, double V);
	/** Mean of Radiance over the unit disc on an N x N midpoint grid; N = 0 picks 128, or 256 past phase 100 deg (a thin
	 *  crescent: within 1 % at 135 deg). The dome's luminance scale is E / (Omega * this). */
	CAMSIMTEST_API double MeanRadiance(const FMoonDiscFrame& Frame, const FMoonAlbedoMap& Map, int32 N = 0);
	/** The disc-plane basis the shader builds (CamSimMoonRight): Right toward the sun (any perpendicular when the sun is
	 *  on the line of sight), Up = MoonDir x Right. */
	CAMSIMTEST_API void DiscBasis(const FMoonDiscFrame& Frame, FVector3d& OutRight, FVector3d& OutUp);
	/** Bound on max(Radiance) / MeanRadiance for the shipped map at any phase and libration (the brightest crater
	 *  rays over a disc mean of ~0.4): FNightIllumination::MaxMoonDiscLuminanceCdM2 uses it. */
	constexpr double MaxPeakToMean = 2.75;
}
