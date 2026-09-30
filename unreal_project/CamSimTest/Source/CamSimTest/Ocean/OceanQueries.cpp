// Copyright CamSim Contributors. All Rights Reserved.

#include "Ocean/OceanQueries.h"
#include "Ocean/OceanSurface.h"

namespace CamSimOcean
{
	FHotResult CombineHot(TOptional<double> TerrainHotM, const FOceanSurface* Ocean, double Lat, double Lon)
	{
		FHotResult R;
		// A terrain miss (tiles not loaded, off the tileset) stays invalid: the host must not get a
		// confident sea-level answer over land it can't see. Water only raises a valid hit.
		if (!TerrainHotM.IsSet() || !FMath::IsFinite(*TerrainHotM)) return R;
		R.bValid = true; R.HotM = *TerrainHotM;
		const TOptional<double> Water = Ocean ? Ocean->SurfaceHeightM(Lat, Lon) : TOptional<double>();
		if (Water.IsSet() && *Water > R.HotM)
		{
			R.HotM = *Water; R.bWater = true;
			R.NormalNeu = Ocean->SurfaceNormalNeu(Lat, Lon).Get(FVector(0.0, 0.0, 1.0));
		}
		return R;
	}
}
