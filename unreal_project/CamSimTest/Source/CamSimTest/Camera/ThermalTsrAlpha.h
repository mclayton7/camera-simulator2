// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"

/**
 * ROADMAP 4A Task 17 (fix round 1): r.TSR.AlphaChannel = 1 while thermal IR runs. Without it TSR's output and history are
 * PF_FloatR11G11B10 (TemporalSuperResolution.cpp:1927/:1965/:1966), dithered by QuantizeForFloatRenderTarget
 * (TSRUpdateHistory.usf:1339): ~0.2-0.4 K radiance steps in MWIR, 0.5-1 K in LWIR, far above the detector NETD. With it
 * both are RGBA16F. The cvar is ECVF_RenderThreadSafe and read per frame; the history-format change drops TSR's history
 * on each switch (a free camera cut). EO keeps the value it had (bit-for-bit unchanged).
 *
 * Acts on transitions only. Entering thermal saves the current value and sets 1 with ECVF_SetByCode, which a
 * higher-priority user value (console) refuses; leaving restores the saved value only if the cvar still holds our 1.
 * Game thread.
 */
class FThermalTsrAlpha
{
public:
	static IConsoleVariable* FindCVar() { return IConsoleManager::Get().FindConsoleVariable(TEXT("r.TSR.AlphaChannel")); }

	void Update(bool bThermal, IConsoleVariable* Var)
	{
		if (!Var || bThermal == bThermalLast) return;
		bThermalLast = bThermal;
		if (bThermal)
		{
			Saved = Var->GetInt();
			if (Saved == 1) return;   // already on: nothing to restore
			Var->Set(1, ECVF_SetByCode);
			bApplied = Var->GetInt() == 1;
		}
		else
		{
			if (bApplied && Var->GetInt() == 1) Var->Set(Saved, ECVF_SetByCode);
			bApplied = false;
		}
	}

	/** Shutdown: as leaving thermal. */
	void Restore(IConsoleVariable* Var) { Update(false, Var); }

	bool IsApplied() const { return bApplied; }

private:
	bool  bThermalLast = false;
	bool  bApplied = false;
	int32 Saved = -1;
};
