// Copyright CamSim Contributors. All Rights Reserved.

#include "Thermal/EntityThermal.h"

namespace CamSimEntityThermal
{
	bool IsBurning(const FEntityThermalState& St, const FEntityThermalInputs& In, const FEntityThermalSettings& S)
	{
		if (In.Cmd.bFlaming) return true;
		return In.Cmd.Damage >= 2 && St.BurnStartSec >= 0.0 && (In.SimSec - St.BurnStartSec) < static_cast<double>(S.BurnS);
	}

	bool IsRunning(const FEntityThermalState& St, const FEntityThermalInputs& In, const FEntityThermalSettings& S)
	{
		if (In.Cmd.Damage >= 2 || In.Cmd.bFlaming) return false;
		if (In.Cmd.bEngineOn || In.SpeedMps > S.MovingMps) return true;
		return (In.SimSec - St.LastMovingSec) <= static_cast<double>(S.IdleHoldS);
	}

	float ConvectionFactor(float SpeedMps, float V0)
	{
		return V0 / (V0 + FMath::Max(SpeedMps, 0.0f));
	}

	void Targets(const FEntityThermalState& St, const FEntityThermalInputs& In, const FEntityThermalSettings& S,
		TConstArrayView<FEntityThermalPartSpec> Parts, float& OutSkin, float OutParts[FEntityThermalSettings::MaxParts])
	{
		const float B = In.bHasEnv ? In.BaselineK : NoEnvBaselineK;
		const float D = In.bHasEnv ? In.TairK - In.BaselineK : 0.0f;
		const int32 N = FMath::Min(Parts.Num(), FEntityThermalSettings::MaxParts);
		for (int32 K = 0; K < FEntityThermalSettings::MaxParts; ++K) OutParts[K] = 0.0f;
		if (IsBurning(St, In, S))
		{
			OutSkin = S.BurnK - B;
			for (int32 K = 0; K < N; ++K) OutParts[K] = OutSkin;
			return;
		}
		const float C = ConvectionFactor(In.SpeedMps, S.ConvectionV0Mps);
		const bool bRun = IsRunning(St, In, S);
		OutSkin = D * (1.0f - C) + (bRun ? S.SkinRunningK * C : 0.0f);
		for (int32 K = 0; K < N; ++K)
		{
			const FEntityThermalPartSpec& P = Parts[K];
			const FEntityThermalKindParams& Kp = S.Kind(P.Kind);
			if (!bRun)
			{
				OutParts[K] = OutSkin;
				continue;
			}
			switch (P.Kind)
			{
			case EEntityThermalPartKind::Engine:
				OutParts[K] = D + P.DeltaK.Get(Kp.DeltaK);
				break;
			case EEntityThermalPartKind::Exhaust:
				OutParts[K] = P.TempK.Get(Kp.TempK) - B;
				break;
			case EEntityThermalPartKind::RunningGear:
				OutParts[K] = D + FMath::Min(P.KPerMps.Get(Kp.KPerMps) * FMath::Max(In.SpeedMps, 0.0f), P.MaxK.Get(Kp.MaxK));
				break;
			default:
				OutParts[K] = OutSkin;
				break;
			}
		}
	}

	static float Relax(float T, float Target, double Dt, float TauUp, float TauDown)
	{
		const double Tau = FMath::Max(static_cast<double>(Target > T ? TauUp : TauDown), 1e-3);
		const float A = static_cast<float>(1.0 - FMath::Exp(-Dt / Tau));
		return FMath::Clamp(T + (Target - T) * A, MinExcessK, MaxExcessK);
	}

	void Step(FEntityThermalState& St, const FEntityThermalInputs& In, const FEntityThermalSettings& S,
		TConstArrayView<FEntityThermalPartSpec> Parts)
	{
		// Burn bookkeeping (before the targets). A destroyed burn starts at the first destroyed sim time and ends after burn_s;
		// flaming burns while set. Either ending switches the skin to the hull cooling constant; a repair (intact or damaged,
		// not flaming) clears it all.
		const bool bDestroyed = In.Cmd.Damage >= 2;
		if (bDestroyed)
		{
			if (St.BurnStartSec < 0.0) { St.BurnStartSec = In.SimSec; St.bBurnDone = false; }
			if (!St.bBurnDone && (In.SimSec - St.BurnStartSec) >= static_cast<double>(S.BurnS))
			{
				St.bBurnDone = true;
				St.bHullCooling = true;
			}
		}
		else if (St.BurnStartSec >= 0.0)
		{
			St.BurnStartSec = -1.0;
			St.bBurnDone = false;
			St.bHullCooling = false;
		}
		if (St.bWasFlaming && !In.Cmd.bFlaming) St.bHullCooling = true;
		St.bWasFlaming = In.Cmd.bFlaming;
		if (In.SpeedMps > S.MovingMps) St.LastMovingSec = In.SimSec;

		float SkinT = 0.0f;
		float PartT[FEntityThermalSettings::MaxParts] = {};
		Targets(St, In, S, Parts, SkinT, PartT);
		const int32 N = FMath::Min(Parts.Num(), FEntityThermalSettings::MaxParts);
		const double Dt = In.SimSec - St.LastSimSec;
		if (!St.bInitialized || Dt < 0.0 || Dt > MaxStepS)
		{
			St.SkinExcessK = FMath::Clamp(SkinT, MinExcessK, MaxExcessK);
			for (int32 K = 0; K < FEntityThermalSettings::MaxParts; ++K)
			{
				St.PartExcessK[K] = K < N ? FMath::Clamp(PartT[K], MinExcessK, MaxExcessK) : 0.0f;
			}
			St.bInitialized = true;
			St.LastSimSec = In.SimSec;
			return;
		}
		if (Dt == 0.0) return;

		const float SkinDown = St.bHullCooling ? S.HullCoolTauS : S.SkinTauS;
		St.SkinExcessK = Relax(St.SkinExcessK, SkinT, Dt, S.SkinTauS, SkinDown);
		for (int32 K = 0; K < N; ++K)
		{
			const FEntityThermalKindParams& Kp = S.Kind(Parts[K].Kind);
			St.PartExcessK[K] = Relax(St.PartExcessK[K], PartT[K], Dt, Kp.TauUpS, Kp.TauDownS);
		}
		St.LastSimSec = In.SimSec;
		// Hull cooling ends once the skin has (nearly) reached its target, unless still destroyed (burnt-out hulk).
		if (St.bHullCooling && !bDestroyed && !In.Cmd.bFlaming && FMath::Abs(St.SkinExcessK - SkinT) < 1.0f) St.bHullCooling = false;
	}

	bool ApplyComponent(FEntityThermalCommanded& C, uint16 ComponentId, uint8 State)
	{
		switch (ComponentId)
		{
		case 10: C.Damage = FMath::Min<uint8>(State, 2); return true;
		case 11: C.bEngineOn = State != 0; return true;
		case 12: C.bFlaming = State != 0; return true;
		default: return false;
		}
	}
}

bool CamSimEntityThermal::LatchEnv(FEntityThermalLatch& Latch, bool bEnvValid, float TairK, float BaselineK)
{
	if (!bEnvValid || !FMath::IsFinite(TairK) || !FMath::IsFinite(BaselineK) || BaselineK < 150.0f) return false;
	Latch.bValid = true;
	Latch.TairK = TairK;
	Latch.BaselineK = BaselineK;
	return true;
}

FEntityThermalInputs CamSimEntityThermal::InputsFromLatch(const FEntityThermalLatch& Latch, double SimSec)
{
	FEntityThermalInputs In;
	In.SimSec = SimSec;
	In.bHasEnv = Latch.bValid;
	In.TairK = Latch.TairK;
	In.BaselineK = Latch.BaselineK;
	return In;
}

bool CamSimEntityThermal::ShouldDeferFirstStep(const FEntityThermalState& St, const FEntitySpeedTracker& Speed)
{
	return !St.bInitialized && !Speed.HasMeasurement();
}

float FEntitySpeedTracker::Update(const FVector& EcefM, double SimSec)
{
	if (EcefM.ContainsNaN() || !FMath::IsFinite(SimSec)) return SpeedMps;
	if (!bHasLast)
	{
		LastEcefM = RefEcefM = EcefM;
		LastSec = RefSec = SimSec;
		bHasLast = true;
		SpeedMps = 0.0f;
		return SpeedMps;
	}
	const double Dt = SimSec - LastSec;
	if (Dt <= 0.0) return SpeedMps;
	const double Step = FVector::Dist(EcefM, LastEcefM);
	LastEcefM = EcefM;
	LastSec = SimSec;
	if (Step > FMath::Max(TeleportMinM, TeleportMaxMps * Dt))
	{
		RefEcefM = EcefM;
		RefSec = SimSec;
		SpeedMps = 0.0f;
		return SpeedMps;
	}
	const double Window = SimSec - RefSec;
	if (Window >= WindowS)
	{
		const double Raw = FVector::Dist(EcefM, RefEcefM) / Window;
		const double A = bMeasured ? 1.0 - FMath::Exp(-Window / EmaTauS) : 1.0;   // the first measurement seeds the EMA
		SpeedMps = static_cast<float>(SpeedMps + (Raw - SpeedMps) * A);
		bMeasured = true;
		RefEcefM = EcefM;
		RefSec = SimSec;
	}
	return SpeedMps;
}
