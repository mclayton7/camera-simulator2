// Copyright CamSim Contributors. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class FOceanSurface;

namespace CamSimOcean
{
	struct FHotResult
	{
		bool    bValid    = false;
		double  HotM      = 0.0;
		bool    bWater    = false;
		FVector NormalNeu = FVector(0.0, 0.0, 1.0);   // only meaningful when bWater
	};

	/**
	 * HOT = max(terrain hit, sea surface incl. waves) when the terrain hit is valid; a terrain
	 * miss stays invalid even with the ocean on. Ocean == nullptr: the terrain hit alone.
	 */
	CAMSIMTEST_API FHotResult CombineHot(TOptional<double> TerrainHotM, const FOceanSurface* Ocean, double Lat, double Lon);
}
