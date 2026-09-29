// Copyright CamSim Contributors. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Ocean/OceanWaves.h"

struct FOceanWaveCommand;

namespace CamSimOcean
{
	/** Enabled, finite command → wave (FromDeg = CIGI direction + 180); otherwise unset (= remove). */
	CAMSIMTEST_API TOptional<FOceanWave> ToOceanWave(const FOceanWaveCommand& Cmd);
}
