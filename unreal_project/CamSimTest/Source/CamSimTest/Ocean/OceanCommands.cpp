// Copyright CamSim Contributors. All Rights Reserved.

#include "Ocean/OceanCommands.h"
#include "Sim/Commands.h"

namespace CamSimOcean
{
	TOptional<FOceanWave> ToOceanWave(const FOceanWaveCommand& Cmd)
	{
		const bool bFinite = FMath::IsFinite(Cmd.HeightM) && FMath::IsFinite(Cmd.LengthM) && FMath::IsFinite(Cmd.PeriodS)
			&& FMath::IsFinite(Cmd.DirectionDeg) && FMath::IsFinite(Cmd.PhaseOffsetDeg);
		if (!Cmd.bEnabled || !bFinite) return {};
		FOceanWave W;
		W.HeightM  = Cmd.HeightM;
		W.LengthM  = Cmd.LengthM;
		W.PeriodS  = FMath::Max(0.0f, Cmd.PeriodS);
		W.FromDeg  = FRotator::NormalizeAxis(Cmd.DirectionDeg + 180.0);
		if (W.FromDeg < 0.0) W.FromDeg += 360.0;
		W.PhaseRad = FMath::DegreesToRadians(static_cast<double>(Cmd.PhaseOffsetDeg));
		return W;
	}
}
