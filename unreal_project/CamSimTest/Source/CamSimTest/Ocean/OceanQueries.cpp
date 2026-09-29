// Copyright CamSim Contributors. All Rights Reserved.

#include "Ocean/OceanQueries.h"
#include "Ocean/OceanSurface.h"

namespace CamSimOcean
{
	FHotResult CombineHot(TOptional<double> TerrainHotM, const FOceanSurface* Ocean, double Lat, double Lon)
	{
		FHotResult R;
		if (TerrainHotM.IsSet() && FMath::IsFinite(*TerrainHotM)) { R.bValid = true; R.HotM = *TerrainHotM; }
		const TOptional<double> Water = Ocean ? Ocean->SurfaceHeightM(Lat, Lon) : TOptional<double>();
		if (Water.IsSet() && (!R.bValid || *Water > R.HotM))
		{
			R.bValid = true; R.HotM = *Water; R.bWater = true;
			R.NormalNeu = Ocean->SurfaceNormalNeu(Lat, Lon).Get(FVector(0.0, 0.0, 1.0));
		}
		return R;
	}
}
