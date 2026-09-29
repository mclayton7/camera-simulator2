// Copyright CamSim Contributors. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Ocean/OceanWaves.h"

/**
 * The sea: EGM96 sea level (+ CIGI tide offset) plus the active wave set.
 * Waves come from the config's Beaufort state, or — while any CIGI Wave
 * Control wave is enabled — from the host's waves (IDs 0..3, by ID).
 * Game thread only. Owned by UCamSimSubsystem; read by boat placement,
 * HAT/HOT and FOceanManager (rendering).
 */
class CAMSIMTEST_API FOceanSurface
{
public:
	using FGeoidFn = TFunction<TOptional<double>(double Lat, double Lon)>;

	/** Empty InGeoid → CamSim::Geospatial::GetGeoidUndulation. */
	explicit FOceanSurface(FGeoidFn InGeoid = FGeoidFn());

	void SetBeaufort(double Beaufort, double FromDeg, double Choppiness);
	void SetHostWave(int32 WaveId, const TOptional<FOceanWave>& Wave);
	bool HasHostWaves() const { return HostWaves.Num() > 0; }

	void   SetTideOffsetM(double M) { TideOffsetM = FMath::IsFinite(M) ? M : 0.0; }
	double GetTideOffsetM() const { return TideOffsetM; }
	void   SetClarity(double C) { Clarity = FMath::IsFinite(C) ? FMath::Clamp(C, 0.0, 1.0) : 1.0; }
	double GetClarity() const { return Clarity; }
	void   SetWaterTempC(double C) { WaterTempC = C; }
	double GetWaterTempC() const { return WaterTempC; }

	void SetTime(double SimSeconds) { Waves.SetTime(SimSeconds); }
	void SetAnchor(double Lat, double Lon) { Waves.SetAnchor(Lat, Lon); }
	const FOceanWaves& GetWaves() const { return Waves; }

	TOptional<double>  SeaLevelM(double Lat, double Lon) const;
	TOptional<double>  SurfaceHeightM(double Lat, double Lon) const;
	TOptional<FVector> SurfaceNormalNeu(double Lat, double Lon) const;

private:
	void RebuildWaves();

	FGeoidFn Geoid;
	FOceanWaves Waves;
	TSortedMap<int32, FOceanWave> HostWaves;
	double Beaufort = 0.0, FromDeg = 270.0, Choppiness = 0.5;
	double TideOffsetM = 0.0, Clarity = 1.0, WaterTempC = 15.0;
};
