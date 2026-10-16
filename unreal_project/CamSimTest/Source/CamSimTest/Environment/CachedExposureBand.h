// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** Thresholds for the Lumen cached-lighting exposure band (ROADMAP 6A). */
struct FCachedExposureBandConfig
{
	float DayEv = 8.0f;
	float NightEv = -6.0f;
	float SwitchMedianLog2 = 0.0f;   // scene median luminance, log2 cd/m2
	float HysteresisEv = 2.0f;
};

/** Day/night band chosen from the EO AE's scene median, with hysteresis. Pure; game thread. */
class FCachedExposureBand
{
public:
	/** Feed the EO AE's scene median (log2 cd/m2). Returns true when the band changed. bValid false (IR, no histogram) holds. */
	bool Update(float SceneMedianLog2, bool bValid, const FCachedExposureBandConfig& Cfg)
	{
		if (!bValid || !FMath::IsFinite(SceneMedianLog2))
		{
			return false;
		}
		if (!bNight && SceneMedianLog2 < Cfg.SwitchMedianLog2)
		{
			bNight = true;
			return true;
		}
		if (bNight && SceneMedianLog2 > Cfg.SwitchMedianLog2 + Cfg.HysteresisEv)
		{
			bNight = false;
			return true;
		}
		return false;
	}

	bool  IsNight() const { return bNight; }
	float CachedEv(const FCachedExposureBandConfig& Cfg) const { return bNight ? Cfg.NightEv : Cfg.DayEv; }

private:
	bool bNight = false;
};
