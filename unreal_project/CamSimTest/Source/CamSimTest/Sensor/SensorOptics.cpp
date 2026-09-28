// Copyright CamSim Contributors. All Rights Reserved.

#include "Sensor/SensorOptics.h"
#include "Sensor/SensorTypes.h"

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

	float PsfSigmaPx(const FSensorOpticsConfig& O)
	{
		const float Airy = 0.42f * O.WavelengthUm * O.FNumber / O.PixelPitchUm;
		return FMath::Sqrt(Airy * Airy + 0.29f * 0.29f + O.ExtraBlurPx * O.ExtraBlurPx);
	}

	void PsfTaps(float SigmaPx, TArray<float>& OutTaps)
	{
		OutTaps.Reset();
		if (!(SigmaPx > 0.0f))
		{
			OutTaps.Add(1.0f);
			return;
		}
		const int32 Radius = FMath::Min(FMath::CeilToInt32(3.0f * SigmaPx), MaxPsfRadius);
		OutTaps.SetNumUninitialized(Radius + 1);
		float Total = 0.0f;
		for (int32 K = 0; K <= Radius; ++K)
		{
			OutTaps[K] = FMath::Exp(-static_cast<float>(K * K) / (2.0f * SigmaPx * SigmaPx));
			Total += K == 0 ? OutTaps[K] : 2.0f * OutTaps[K];
		}
		for (float& T : OutTaps) T /= Total;
	}
}
