// Copyright CamSim Contributors. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/** One Gerstner wave. Heights/lengths in metres, directions true-north degrees. */
struct FOceanWave
{
	double HeightM   = 0.0;  // crest-to-trough; amplitude a = HeightM / 2
	double LengthM   = 0.0;
	double PeriodS   = 0.0;  // 0 → deep-water dispersion
	double FromDeg   = 0.0;  // direction the wave comes FROM (travels toward FromDeg + 180)
	double PhaseRad  = 0.0;
	double Steepness = 0.0;  // Gerstner Q (AssignSteepness keeps sum Q k a <= 1)
};

/**
 * Sum of up to four Gerstner waves on the tangent plane of a fixed anchor.
 * Plane coordinates (N, E) of a point: its ECEF position minus the anchor's,
 * projected on the anchor's north/east axes (exact, rigid — the GPU does the
 * same with UE world positions). Pure: no UObjects, no engine state.
 *
 * theta_i = k_i (d_i . x) + Phase(i), Phase(i) = PhaseRad_i - w_i t (wrapped)
 * displacement: horizontal -sum Q_i a_i d_i sin theta_i, vertical sum a_i cos theta_i
 * M_Ocean's custom HLSL mirrors Displacement/NormalAtPlane expression for expression.
 */
class CAMSIMTEST_API FOceanWaves
{
public:
	static constexpr int32  MaxWaves = 4;
	static constexpr double G        = 9.80665;

	static TArray<FOceanWave> FromBeaufort(double Beaufort, double FromDeg, double Choppiness);
	/** Q_i = min(C, 1) / (k_i a_i n) over the n waves with k a > 0, so sum Q k a = min(C, 1). */
	static void AssignSteepness(TArray<FOceanWave>& Waves, double Choppiness);

	/** Keeps the first MaxWaves; drops waves with non-finite or non-positive height/length. */
	void SetWaves(const TArray<FOceanWave>& InWaves);
	const TArray<FOceanWave>& GetWaves() const { return Waves; }

	void   SetAnchor(double Lat, double Lon);
	bool   HasAnchor() const { return bHasAnchor; }
	double GetAnchorLat() const { return AnchorLat; }
	double GetAnchorLon() const { return AnchorLon; }
	FVector GetAnchorEcef() const { return AnchorEcef; }
	FVector GetAxisNorthEcef() const { return AxisN; }
	FVector GetAxisEastEcef() const { return AxisE; }
	FVector GetAxisUpEcef() const { return AxisU; }

	void   SetTime(double SimSeconds) { TimeS = SimSeconds; }
	double GetTime() const { return TimeS; }

	double    WaveNumber(int32 i) const;         // 2 pi / L
	double    AngularFrequency(int32 i) const;   // sqrt(g k) or 2 pi / T
	FVector2D TravelDir(int32 i) const;          // unit (N, E)
	double    Phase(int32 i) const;              // PhaseRad - w t, wrapped to [0, 2 pi)
	double    Amplitude(int32 i) const { return Waves[i].HeightM * 0.5; }

	FVector2D PlaneCoords(double Lat, double Lon, double AltM) const;
	FVector   Displacement(double N, double E) const;      // (dN, dE, dZ) at a parameter point
	double    HeightAtPlane(double N, double E) const;     // at the point the surface lands on (N, E)
	FVector   NormalAtPlane(double N, double E) const;     // unit (N, E, Up)
	double    HeightAt(double Lat, double Lon, double SeaAltM) const;
	FVector   NormalAt(double Lat, double Lon, double SeaAltM) const;
	double    SignificantHeight() const;               // 4 sqrt(sum a^2 / 2)

private:
	FVector2D Invert(double N, double E) const;          // parameter point landing on (N, E)
	FVector   NormalAtParam(double N, double E) const;

	TArray<FOceanWave> Waves;
	bool    bHasAnchor = false;
	double  AnchorLat = 0.0, AnchorLon = 0.0;
	FVector AnchorEcef = FVector::ZeroVector;
	FVector AxisN = FVector::ZeroVector, AxisE = FVector::ZeroVector, AxisU = FVector::ZeroVector;
	double  TimeS = 0.0;
};
