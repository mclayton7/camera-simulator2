// Copyright CamSim Contributors. All Rights Reserved.

#include "Sensor/SensorOptics.h"
#include "Sensor/SensorTypes.h"
#include "SensorFrameParams.h"
#include <cmath>

namespace CamSimOptics
{
	float FocalPx(int32 Width, float HFovDeg)
	{
		return 0.5f * Width / FMath::Tan(FMath::DegreesToRadians(0.5f * HFovDeg));
	}

	bool UndistortRadius(float Rd, float K1, float K2, float& OutRu)
	{
		float R = Rd;
		for (int32 I = 0; I < NewtonIterations; ++I)
		{
			const float R2 = R * R;
			R -= (R * (1.0f + K1 * R2 + K2 * R2 * R2) - Rd) / (1.0f + 3.0f * K1 * R2 + 5.0f * K2 * R2 * R2);
		}
		OutRu = R;
		const float R2 = R * R;
		const float Residual = R * (1.0f + K1 * R2 + K2 * R2 * R2) - Rd;
		const float Slope = 1.0f + 3.0f * K1 * R2 + 5.0f * K2 * R2 * R2;
		return FMath::IsFinite(R) && FMath::Abs(Residual) <= NewtonTolerance && Slope > 0.0f;
	}

	float CornerRadius(int32 W, int32 H, float FocalPx)
	{
		return FMath::Sqrt(0.25f * W * W + 0.25f * H * H) / FocalPx;
	}

	bool DistortionConverges(int32 W, int32 H, float HFovDeg, float K1, float K2)
	{
		if (K1 == 0.0f && K2 == 0.0f) return true;
		const float Corner = CornerRadius(W, H, FocalPx(W, HFovDeg));
		if (!FMath::IsFinite(Corner)) return false;
		for (int32 I = 0; I < ConvergenceSamples; ++I)
		{
			float Ru;
			if (!UndistortRadius(Corner * I / (ConvergenceSamples - 1), K1, K2, Ru)) return false;
		}
		return true;
	}

	int32 PsfRadius(float SigmaO)
	{
		TArray<float> Taps;
		PsfTaps(SigmaO, Taps);
		return Taps.Num() - 1;
	}

	bool SetOptics(FSensorFrameParams& P, const FSensorOpticsConfig& O, int32 W, int32 H, float HFovDeg)
	{
		const bool bConverges = DistortionConverges(W, H, HFovDeg, O.K1, O.K2);
		P.FocalPx            = FocalPx(W, HFovDeg);
		P.K1                 = bConverges ? O.K1 : 0.0f;
		P.K2                 = bConverges ? O.K2 : 0.0f;
		P.VignettingExponent = O.VignettingExponent;
		SetPsf(P, PsfOpticalSigmaPx(O));
		return bConverges;
	}

	float PsfSigmaPx(const FSensorOpticsConfig& O)
	{
		const float Airy = 0.42f * O.WavelengthUm * O.FNumber / O.PixelPitchUm;
		return FMath::Sqrt(Airy * Airy + 0.29f * 0.29f + O.ExtraBlurPx * O.ExtraBlurPx);
	}

	float PsfOpticalSigmaPx(const FSensorOpticsConfig& O)
	{
		const float Airy = 0.42f * O.WavelengthUm * O.FNumber / O.PixelPitchUm;
		return FMath::Sqrt(Airy * Airy + O.ExtraBlurPx * O.ExtraBlurPx);
	}

	void PsfTaps(float SigmaO, TArray<float>& OutTaps)
	{
		OutTaps.Reset();
		if (!(SigmaO > 0.0f))
		{
			OutTaps.Add(1.0f);
			return;
		}
		const int32 Radius = FMath::Min(FMath::CeilToInt32(3.0f * SigmaO) + 1, MaxPsfRadius);
		auto Phi = [S = static_cast<double>(SigmaO)](double X) { return 0.5 * (1.0 + std::erf(X / (S * UE_DOUBLE_SQRT_2))); };
		TArray<double> T;
		T.SetNumUninitialized(Radius + 1);
		double Total = 0.0;
		for (int32 K = 0; K <= Radius; ++K)
		{
			T[K] = Phi(K + 0.5) - Phi(K - 0.5);
			Total += K == 0 ? T[K] : 2.0 * T[K];
		}
		OutTaps.SetNumUninitialized(Radius + 1);
		for (int32 K = 0; K <= Radius; ++K) OutTaps[K] = static_cast<float>(T[K] / Total);
	}

	void SetPsf(FSensorFrameParams& P, float SigmaO)
	{
		static_assert(FSensorFrameParams::MaxPsfTaps == MaxPsfRadius + 1, "tap array holds radius MaxPsfRadius");
		TArray<float> Taps;
		PsfTaps(SigmaO, Taps);
		check(Taps.Num() >= 1 && Taps.Num() <= FSensorFrameParams::MaxPsfTaps);
		P.PsfSigmaPx = SigmaO;
		for (int32 K = 0; K < FSensorFrameParams::MaxPsfTaps; ++K) P.PsfTaps[K] = K < Taps.Num() ? Taps[K] : 0.0f;
		P.NumPsfTaps = static_cast<uint32>(Taps.Num());
	}
}
