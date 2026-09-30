// Copyright CamSim Contributors. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Ocean/OceanWaves.h"

struct FOceanWaveCommand;

namespace CamSimOcean
{
	/**
	 * Enabled, finite command → wave (FromDeg = CIGI direction + 180); otherwise unset (= remove).
	 * Length <= 0 with a period > 0: length from deep-water dispersion, g T^2 / (2 pi).
	 */
	CAMSIMTEST_API TOptional<FOceanWave> ToOceanWave(const FOceanWaveCommand& Cmd);
}
