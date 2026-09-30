// Copyright CamSim Contributors. All Rights Reserved.

#include "Ocean/OceanSurface.h"
#include "Geospatial/Geoid.h"

FOceanSurface::FOceanSurface(FGeoidFn InGeoid)
	: Geoid(InGeoid ? MoveTemp(InGeoid) : FGeoidFn([](double Lat, double Lon) { return CamSim::Geospatial::GetGeoidUndulation(Lat, Lon); }))
{
}

void FOceanSurface::SetBeaufort(double InBeaufort, double InFromDeg, double InChoppiness)
{
	// Non-finite inputs keep the current value (a NaN Beaufort would otherwise clamp to hurricane).
	if (FMath::IsFinite(InBeaufort))   Beaufort   = InBeaufort;
	if (FMath::IsFinite(InFromDeg))    FromDeg    = InFromDeg;
	if (FMath::IsFinite(InChoppiness)) Choppiness = InChoppiness;
	RebuildWaves();
}

void FOceanSurface::SetHostWave(int32 WaveId, const TOptional<FOceanWave>& Wave)
{
	if (WaveId < 0 || WaveId >= FOceanWaves::MaxWaves) return;
	if (Wave.IsSet()) HostWaves.Add(WaveId, *Wave);
	else              HostWaves.Remove(WaveId);
	RebuildWaves();
}

void FOceanSurface::RebuildWaves()
{
	if (HostWaves.Num() == 0)
	{
		Waves.SetWaves(FOceanWaves::FromBeaufort(Beaufort, FromDeg, Choppiness));
		return;
	}
	TArray<FOceanWave> Host;
	for (const TPair<int32, FOceanWave>& P : HostWaves) Host.Add(P.Value);
	FOceanWaves::AssignSteepness(Host, Choppiness);
	Waves.SetWaves(Host);
}

TOptional<double> FOceanSurface::SeaLevelM(double Lat, double Lon) const
{
	const TOptional<double> G = Geoid(Lat, Lon);
	if (!G.IsSet() || !FMath::IsFinite(*G)) return {};
	return *G + TideOffsetM;
}

TOptional<double> FOceanSurface::SurfaceHeightM(double Lat, double Lon) const
{
	const TOptional<double> Sea = SeaLevelM(Lat, Lon);
	if (!Sea.IsSet()) return {};
	return *Sea + Waves.HeightAt(Lat, Lon, *Sea);
}

TOptional<FVector> FOceanSurface::SurfaceNormalNeu(double Lat, double Lon) const
{
	const TOptional<double> Sea = SeaLevelM(Lat, Lon);
	if (!Sea.IsSet()) return {};
	return Waves.NormalAt(Lat, Lon, *Sea);
}
