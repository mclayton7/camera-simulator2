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
		W.PeriodS  = FMath::Max(0.0f, Cmd.PeriodS);
		// Period only (no length): deep-water dispersion, lambda = g T^2 / (2 pi).
		W.LengthM  = (Cmd.LengthM <= 0.0f && W.PeriodS > 0.0)
			? FOceanWaves::G * W.PeriodS * W.PeriodS / (2.0 * UE_DOUBLE_PI)
			: static_cast<double>(Cmd.LengthM);
		W.FromDeg  = FRotator::NormalizeAxis(Cmd.DirectionDeg + 180.0);
		if (W.FromDeg < 0.0) W.FromDeg += 360.0;
		W.PhaseRad = FMath::DegreesToRadians(static_cast<double>(Cmd.PhaseOffsetDeg));
		return W;
	}
}
