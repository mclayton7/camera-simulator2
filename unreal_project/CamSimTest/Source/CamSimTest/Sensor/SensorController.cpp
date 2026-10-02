// Copyright CamSim Contributors. All Rights Reserved.

#include "Sensor/SensorController.h"
#include "SensorHash.h"

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

bool FSensorController::PercentileLog2Interp(const FSensorHistogram& H, float P, float& OutLog2)
{
	const uint64 Total = H.Total();
	if (Total == 0) return false;
	const double Target = FMath::Clamp(static_cast<double>(P), 0.0, 1.0) * static_cast<double>(Total);
	uint64 Cum = 0;
	for (int32 B = 0; B < FSensorHistogram::NumBins; ++B)
	{
		const uint64 Next = Cum + H.Bins[B];
		if (H.Bins[B] > 0 && static_cast<double>(Next) >= Target)
		{
			const double F = FMath::Clamp((Target - static_cast<double>(Cum)) / static_cast<double>(H.Bins[B]), 0.0, 1.0);
			OutLog2 = FSensorHistogram::MinLog2 + static_cast<float>((B + F) / FSensorHistogram::BinsPerStop);
			return true;
		}
		Cum = Next;
	}
	OutLog2 = FSensorHistogram::MinLog2 + FSensorHistogram::NumBins / FSensorHistogram::BinsPerStop;
	return true;
}

float FSensorController::Smoothing(double DeltaSimSec, int32 LagFrames, bool bSnap)
{
	if (bSnap || LagFrames <= 0) return 1.0f;
	if (!(DeltaSimSec > 0.0)) return 0.0f;                  // frozen or backwards: hold
	const double Tau = static_cast<double>(LagFrames) / 30.0;
	return static_cast<float>(1.0 - FMath::Exp(-DeltaSimSec / Tau));
}

namespace
{
	int32 ModeIndex(ESensorGraphMode Mode) { return Mode == ESensorGraphMode::EO ? 0 : 1; }
}

uint32 FSensorController::ModeSeed(ESensorGraphMode Mode, uint32 ConfigSeed)
{
	return ConfigSeed ^ CamSimHash::Pcg(static_cast<uint32>(ModeIndex(Mode)) + 1u);
}

int32 FSensorController::SlotOf(const FSensorControllerInput& In)
{
	if (In.Mode == ESensorGraphMode::EO) return 0;
	return In.bRadianceInput ? 2 : 1;
}

const FSensorExposureConfig& FSensorController::ExposureOf(const FSensorModeConfig& Cfg, const FSensorControllerInput& In)
{
	return SlotOf(In) == 2 ? Cfg.ThermalExposure : Cfg.Exposure;
}

float FSensorController::TotalGainCapEv(const FSensorModeConfig& Cfg, const FSensorExposureConfig& E)
{
	const bool bAnalogStage = Cfg.Detector.Type != ESensorDetectorType::Microbolometer;
	const float AnalogEv = bAnalogStage ? FMath::Max(Cfg.Detector.MaxAnalogGainDb, 0.0f) / 20.0f * FMath::Log2(10.0f) : 0.0f;
	return E.MaxPhotonGainEv + AnalogEv;
}

void FSensorController::UpdateAe(const FSensorHistogram& H, const FSensorModeConfig& Cfg,
	const FSensorControllerInput& In, bool bSnap)
{
	const FSensorExposureConfig& E = ExposureOf(Cfg, In);
	float Target = E.ManualGainEv;
	float Median = 0.0f, High = 0.0f;
	if (PercentileLog2(H, 0.5f, Median) && PercentileLog2(H, E.HighlightPercentile, High))
	{
		LastMedianLog2 = Median;
		if (E.bAuto)
		{
			const float ToGrey = FMath::Log2(FMath::Max(E.TargetGrey, 1e-6f)) - Median;
			const float NoClip = FMath::Log2(ClipLinear) - High;
			Target = FMath::Min(ToGrey, NoClip);
		}
	}
	else if (E.bAuto)
	{
		return;  // empty histogram: keep the current gain
	}
	Target = FMath::Clamp(Target, E.MinGainEv, TotalGainCapEv(Cfg, E));
	float& GainEv = GainEvByMode[SlotOf(In)];
	GainEv += (Target - GainEv) * Smoothing(In.DeltaSimSec, E.LagFrames, bSnap);
}

void FSensorController::UpdateIrAgc(const FSensorHistogram& H, const FSensorModeConfig& Cfg,
	const FSensorControllerInput& In, bool bSnap)
{
	float Lo = 0.0f, Hi = 0.0f, Median = 0.0f;
	// Thermal radiance has a large offset and a small contrast: interpolated percentiles keep the stretch from jumping a
	// whole 9 % bin at a time.
	auto Percentile = [&](float P, float& Out) { return In.bRadianceInput ? PercentileLog2Interp(H, P, Out) : PercentileLog2(H, P, Out); };
	if (!Percentile(Cfg.AGCLowPercentile, Lo) || !Percentile(Cfg.AGCHighPercentile, Hi)) return;
	if (PercentileLog2(H, 0.5f, Median)) LastMedianLog2 = Median;
	Hi = FMath::Max(Hi, Lo + (In.bRadianceInput ? 1e-3f : 1.0f / FSensorHistogram::BinsPerStop));  // never a zero-width band
	const float A = Smoothing(In.DeltaSimSec, Cfg.AGCLagFrames, bSnap);
	IrLoLog2 += (Lo - IrLoLog2) * A;
	IrHiLog2 += (Hi - IrHiLog2) * A;
}

FSensorFrameParams FSensorController::Update(const FSensorControllerInput& In, const FSensorModeConfig& Cfg)
{
	const int32 Slot = SlotOf(In);
	if (Slot != LastSlot || In.bCameraCut)
	{
		bSnapPending = true;
		SnapAfterSerial = In.Serial;
		LastSlot = Slot;
	}
	const bool bIrAgc = (In.Mode == ESensorGraphMode::IR) && Cfg.bAGCEnabled;

	// Smoothing runs only when a histogram arrives, so it uses all the sim time
	// since the last one it consumed: the lag doesn't depend on histogram timing.
	// A frozen or backwards clock adds nothing (hold).
	if (In.DeltaSimSec > 0.0) PendingDeltaSimSec += In.DeltaSimSec;

	if (In.NewHistogram)
	{
		TicksSinceHistogram = 0;
		const bool bSnap = !bInitialized || (bSnapPending && In.NewHistogram->Serial >= SnapAfterSerial);
		if (In.NewHistogram->Total() > 0)
		{
			FSensorControllerInput Step = In;
			Step.DeltaSimSec = PendingDeltaSimSec;
			PendingDeltaSimSec = 0.0;
			UpdateAe(*In.NewHistogram, Cfg, Step, bSnap);   // photon/analog gain, IR AGC included
			if (bIrAgc) UpdateIrAgc(*In.NewHistogram, Cfg, Step, bSnap);
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
	P.FrameIndex    = In.Serial;
	P.Seed          = ModeSeed(In.Mode, Cfg.Seed);

	const FSensorDetectorConfig& D = Cfg.Detector;
	P.DetectorType  = static_cast<uint32>(D.Type);
	P.FullWellE     = D.FullWellE;
	P.ReadNoiseE    = D.ReadNoiseE;
	P.Prnu          = D.Prnu;
	P.DsnuE         = D.DsnuE;
	// Dark signal integrates over the frame time (1 / frame rate), not the AE's integration time: the
	// photon gain is not yet radiometric (ROADMAP 3B.3). At preset values it is negligible either way
	// (EO: 5 e/s / 30 Hz = 0.17 e against 2 e read noise; cooled MWIR 0).
	P.DarkE         = D.DarkCurrentEs / (In.FrameRateHz > 0.0f ? In.FrameRateHz : 30.0f);
	P.TemporalNoise = D.TemporalNoise;
	P.PixelFpn      = D.PixelFpn;
	P.ColumnFpn     = D.ColumnFpn;
	P.RowFpn        = D.RowFpn;
	P.AdcMax        = static_cast<float>((1u << FMath::Clamp(D.AdcBits, 1, 24)) - 1u);
	P.HotFraction   = D.HotPixelFraction;
	P.DeadFraction  = D.DeadPixelFraction;

	// Total AE gain, clamped against the CURRENT mode's config (the state may
	// have converged under different limits), then split: the photon stage
	// (integration time) first, analog gain only past its limit.
	const FSensorExposureConfig& E = ExposureOf(Cfg, In);
	const float TotalEv  = FMath::Clamp(GainEvByMode[Slot], E.MinGainEv, TotalGainCapEv(Cfg, E));
	const float PhotonEv = FMath::Min(TotalEv, E.MaxPhotonGainEv);
	const float AnalogEv = FMath::Max(TotalEv - PhotonEv, 0.0f);
	P.PhotonGain = FMath::Exp2(PhotonEv);
	P.AnalogGain = FMath::Exp2(AnalogEv);

	if (bIrAgc)
	{
		// Percentile band in signal units -> normalised DN, then stretched to [0, 1] (luminance proxy) or to
		// [margin, 1 - margin] with the gain capped and the band centred when the cap binds (thermal radiance).
		// The band stops at full well (ClipLinear): the detector clips there, so a hot object beyond the AE's range (a
		// burning vehicle, ROADMAP 4C) must stretch to white, not to the unclipped signal it never outputs.
		const float N    = P.PhotonGain * P.AnalogGain;
		const float LoN  = FMath::Min(FMath::Exp2(IrLoLog2) * N, ClipLinear - 1e-6f);
		const float HiN  = FMath::Max(FMath::Min(FMath::Exp2(IrHiLog2) * N, ClipLinear), LoN + 1e-6f);
		if (In.bRadianceInput)
		{
			const float Span = 1.0f - 2.0f * ThermalAgcMargin;
			const float Gain = Span / (HiN - LoN);
			const float Cap  = FMath::Max(Cfg.AGCMaxDisplayGain, 1.0f);
			if (Gain > Cap)
			{
				P.DisplayGain   = Cap;
				P.DisplayOffset = 0.5f - 0.5f * (LoN + HiN) * Cap;
			}
			else
			{
				P.DisplayGain   = Gain;
				P.DisplayOffset = ThermalAgcMargin - LoN * Gain;
			}
		}
		else
		{
			P.DisplayGain   = 1.0f / (HiN - LoN);
			P.DisplayOffset = -LoN * P.DisplayGain;
		}
		LastEmittedGainEv = FMath::Log2(N * P.DisplayGain);
	}
	else
	{
		LastEmittedGainEv = TotalEv;
	}
	return P;
}
