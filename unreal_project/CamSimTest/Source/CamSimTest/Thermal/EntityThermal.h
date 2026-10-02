// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Thermal/EntityThermalTypes.h"

/**
 * Entity thermal state (ROADMAP 4C, docs/superpowers/specs/2026-10-02-entity-thermal-state-design.md §1-2). Pure: no UE objects.
 *
 * Each entity has a skin and up to FEntityThermalSettings::MaxParts part temperatures, stored as EXCESS over the parked
 * baseline B = T_class + thermal_offset_k (absolute T = B + excess; FThermalFrameBuilder adds B). Every value relaxes
 * toward its target on sim time: T += (T_target - T)(1 - exp(-dt / tau)), tau = up or down constant by direction.
 * Targets (absolute; D = T_air - B, 0 without an environment):
 *   skin          D (1 - c) + [running] skin_running_k c,   c = v0 / (v0 + v)
 *   engine        running: D + delta_k                        else the skin target
 *   exhaust       running: temp_k - B                         else the skin target
 *   running_gear  running: D + min(k_per_mps v, max_k)        else the skin target
 *   burning (destroyed for burn_s after it was first seen, or flaming): every target burn_k - B.
 * Running = commanded on, or moving (v > moving_mps), or moved within idle_hold_s; never when destroyed or flaming.
 */

/** Host-commanded inputs (Component Control 10 damage, 11 power plant, 12 flaming; DIS appearance maps onto them). */
struct FEntityThermalCommanded
{
	bool  bEngineOn = false;
	uint8 Damage    = 0;   // 0 intact, 1 damaged (no thermal effect), 2 destroyed
	bool  bFlaming  = false;
};

/** One step's inputs. */
struct FEntityThermalInputs
{
	FEntityThermalCommanded Cmd;
	float  SpeedMps  = 0.0f;
	double SimSec    = 0.0;
	bool   bHasEnv   = false;     // false: D = 0, B taken as 288.15 K for absolute targets (exhaust, burn)
	float  TairK     = 288.15f;
	float  BaselineK = 288.15f;   // B = T_class + offset of this entity
};

/** Per-entity state. */
struct FEntityThermalState
{
	bool   bInitialized  = false;
	double LastSimSec    = 0.0;
	double LastMovingSec = -1.0e300;   // sim time it was last seen moving
	double BurnStartSec  = -1.0;       // < 0: no destroyed burn started
	bool   bBurnDone     = false;      // the destroyed burn ran its burn_s
	bool   bWasFlaming   = false;
	bool   bHullCooling  = false;      // skin cools with hull_cool_tau_s (after a burn)
	float  SkinExcessK   = 0.0f;
	float  PartExcessK[FEntityThermalSettings::MaxParts] = {};
};

namespace CamSimEntityThermal
{
	static constexpr float NoEnvBaselineK = 288.15f;
	static constexpr double MaxStepS = 3600.0;
	static constexpr float MinExcessK = -200.0f, MaxExcessK = 900.0f;

	CAMSIMTEST_API bool  IsRunning(const FEntityThermalState& St, const FEntityThermalInputs& In, const FEntityThermalSettings& S);
	CAMSIMTEST_API bool  IsBurning(const FEntityThermalState& St, const FEntityThermalInputs& In, const FEntityThermalSettings& S);
	CAMSIMTEST_API float ConvectionFactor(float SpeedMps, float V0);
	/** Excess targets (K over B) of the skin and the first min(Parts.Num(), MaxParts) parts; the rest are 0. */
	CAMSIMTEST_API void  Targets(const FEntityThermalState& St, const FEntityThermalInputs& In, const FEntityThermalSettings& S,
		TConstArrayView<FEntityThermalPartSpec> Parts, float& OutSkin, float OutParts[FEntityThermalSettings::MaxParts]);
	/** Advance to In.SimSec. First call, dt < 0 or dt > MaxStepS: snap to the targets; dt == 0: no change. */
	CAMSIMTEST_API void  Step(FEntityThermalState& St, const FEntityThermalInputs& In, const FEntityThermalSettings& S,
		TConstArrayView<FEntityThermalPartSpec> Parts);
	/** Component Control (class 0) 10 damage, 11 power plant, 12 flaming -> commanded inputs. False for other IDs. */
	CAMSIMTEST_API bool  ApplyComponent(FEntityThermalCommanded& C, uint16 ComponentId, uint8 State);
}

/**
 * Smoothed speed from successive ECEF positions (metres) on sim time. The raw speed is the displacement over a window of at
 * least WindowS (per-tick differences would rectify centimetre pose noise into ~1 m/s), then an EMA with EmaTauS.
 * A per-tick jump beyond max(TeleportMinM, TeleportMaxMps dt) resets to 0; dt <= 0 keeps the speed and skips the sample.
 */
class CAMSIMTEST_API FEntitySpeedTracker
{
public:
	static constexpr double WindowS        = 0.5;
	static constexpr double EmaTauS        = 1.0;
	static constexpr double TeleportMinM   = 50.0;
	static constexpr double TeleportMaxMps = 400.0;

	/** Returns the smoothed speed (m/s). */
	float Update(const FVector& EcefM, double SimSec);
	float GetSpeedMps() const { return SpeedMps; }
	void  Reset() { bHasLast = false; SpeedMps = 0.0f; }

private:
	FVector LastEcefM = FVector::ZeroVector;
	double  LastSec   = 0.0;
	FVector RefEcefM  = FVector::ZeroVector;   // window start
	double  RefSec    = 0.0;
	bool    bHasLast  = false;
	float   SpeedMps  = 0.0f;
};
