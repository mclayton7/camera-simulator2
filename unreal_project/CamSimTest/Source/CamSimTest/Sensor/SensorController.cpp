// Copyright CamSim Contributors. All Rights Reserved.

#include "Sensor/SensorController.h"

bool FSensorController::PercentileLog2(const FSensorHistogram& H, float P, float& OutLog2)
{
	const uint64 Total = H.Total();
	if (Total == 0) return false;
	const double Target = FMath::Clamp(static_cast<double>(P), 0.0, 1.0) * static_cast<double>(Total);
	uint64 Cum = 0;
	for (int32 B = 0; B < FSensorHistogram::NumBins; ++B)
	{
		Cum += H.Bins[B];
		if (static_cast<double>(Cum) >= Target && H.Bins[B] > 0)
		{
			OutLog2 = FSensorHistogram::BinCentreLog2(B);
			return true;
		}
	}
	OutLog2 = FSensorHistogram::BinCentreLog2(FSensorHistogram::NumBins - 1);
	return true;
}

float FSensorController::Smoothing(double DeltaSimSec, int32 LagFrames, bool bSnap)
{
	if (bSnap || LagFrames <= 0) return 1.0f;
	if (!(DeltaSimSec > 0.0)) return 0.0f;                  // frozen or backwards: hold
	const double Tau = static_cast<double>(LagFrames) / 30.0;
	return static_cast<float>(1.0 - FMath::Exp(-DeltaSimSec / Tau));
}

void FSensorController::UpdateAe(const FSensorHistogram& H, const FSensorModeConfig& Cfg,
	const FSensorControllerInput& In, bool bSnap)
{
	const FSensorExposureConfig& E = Cfg.Exposure;
	float Target = E.ManualGainEv;
	float Median = 0.0f, High = 0.0f;
	if (PercentileLog2(H, 0.5f, Median) && PercentileLog2(H, E.HighlightPercentile, High))
	{
		LastMedianLog2 = Median;
		if (E.bAuto)
		{
			const float Comp = (In.Mode == ESensorGraphMode::EO) ? In.ExposureCompensationEv : 0.0f;
			const float ToGrey = FMath::Log2(FMath::Max(E.TargetGrey, 1e-6f)) + Comp - Median;
			const float NoClip = FMath::Log2(ClipLinear) - High;
			Target = FMath::Min(ToGrey, NoClip);
		}
	}
	else if (E.bAuto)
	{
		return;  // empty histogram: keep the current gain
	}
	Target = FMath::Clamp(Target, E.MinGainEv, E.MaxGainEv);
	GainEv += (Target - GainEv) * Smoothing(In.DeltaSimSec, E.LagFrames, bSnap);
}

void FSensorController::UpdateIrAgc(const FSensorHistogram& H, const FSensorModeConfig& Cfg,
	const FSensorControllerInput& In, bool bSnap)
{
	float Lo = 0.0f, Hi = 0.0f, Median = 0.0f;
	if (!PercentileLog2(H, Cfg.AGCLowPercentile, Lo) || !PercentileLog2(H, Cfg.AGCHighPercentile, Hi)) return;
	if (PercentileLog2(H, 0.5f, Median)) LastMedianLog2 = Median;
	Hi = FMath::Max(Hi, Lo + 1.0f / FSensorHistogram::BinsPerStop);  // never a zero-width band
	const float A = Smoothing(In.DeltaSimSec, Cfg.AGCLagFrames, bSnap);
	IrLoLog2 += (Lo - IrLoLog2) * A;
	IrHiLog2 += (Hi - IrHiLog2) * A;
}

FSensorFrameParams FSensorController::Update(const FSensorControllerInput& In, const FSensorModeConfig& Cfg)
{
	if (In.Mode != LastMode || In.bCameraCut)
	{
		bSnapPending = true;
		SnapAfterSerial = In.Serial;
		LastMode = In.Mode;
	}
	const bool bIrAgc = (In.Mode == ESensorGraphMode::IR) && Cfg.bAGCEnabled;

	if (In.NewHistogram)
	{
		TicksSinceHistogram = 0;
		const bool bSnap = !bInitialized || (bSnapPending && In.NewHistogram->Serial >= SnapAfterSerial);
		if (In.NewHistogram->Total() > 0)
		{
			if (bIrAgc) UpdateIrAgc(*In.NewHistogram, Cfg, In, bSnap);
			else        UpdateAe(*In.NewHistogram, Cfg, In, bSnap);
			if (bSnap) { bInitialized = true; bSnapPending = false; }
		}
	}
	else if (++TicksSinceHistogram == StaleAfterTicks)
	{
		++StaleEpisodes;
	}

	FSensorFrameParams P;
	P.Mode          = In.Mode;
	P.bBlackHot     = In.bBlackHot ? 1u : 0u;
	P.SignalWeights = Cfg.SignalWeights;
	P.Serial        = In.Serial;
	P.DisplayTint   = (In.Mode == ESensorGraphMode::NVG) ? FVector3f(0.3f, 1.0f, 0.3f) : FVector3f(1.0f, 1.0f, 1.0f);
	if (bIrAgc)
	{
		const float Lo = FMath::Exp2(IrLoLog2), Hi = FMath::Exp2(IrHiLog2);
		P.Gain   = 1.0f / FMath::Max(Hi - Lo, 1e-30f);
		P.Offset = -Lo * P.Gain;
		LastEmittedGainEv = FMath::Log2(P.Gain);
	}
	else
	{
		// Clamp against the CURRENT mode's config: GainEv may have converged
		// under a different mode's Min/MaxGainEv (see IR AGC excursion above).
		const float ClampedEv = FMath::Clamp(GainEv, Cfg.Exposure.MinGainEv, Cfg.Exposure.MaxGainEv);
		P.Gain = FMath::Exp2(ClampedEv);
		LastEmittedGainEv = ClampedEv;
	}
	return P;
}
