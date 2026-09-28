// Copyright CamSim Contributors. All Rights Reserved.

#include "Sensor/SensorReference.h"

namespace CamSimSensorRef
{
	float Sanitize(float V)
	{
		if (FMath::IsNaN(V)) return 0.0f;
		return FMath::Clamp(V, 0.0f, 65504.0f);   // +Inf -> 65504
	}

	float Knee(float X, float K)
	{
		if (X <= K || K >= 1.0f) return X;
		return K + (1.0f - K) * (1.0f - FMath::Exp(-(X - K) / (1.0f - K)));
	}

	float Oetf709(float L)
	{
		L = FMath::Clamp(L, 0.0f, 1.0f);
		return L < 0.018f ? 4.5f * L : 1.099f * FMath::Pow(L, 0.45f) - 0.099f;
	}

	static uint8 ToLimited(float V, float Scale) { return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt32(16.0f + Scale * V), 0, 255)); }
	static uint8 ToChroma(float C) { return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt32(128.0f + 224.0f * C), 0, 255)); }

	FResult Run(const TArray<FLinearColor>& Scene, int32 W, int32 H, const FSensorFrameParams& P)
	{
		check(Scene.Num() == W * H && W % 4 == 0 && H % 2 == 0);
		FResult R;
		R.Nv12.SetNumZeroed(W * H * 3 / 2);
		TArray<FVector4f> Out;   // xyz = R'G'B' (EO) or v,v,v; w = luma source (IR)
		Out.SetNumUninitialized(W * H);
		const bool bEo = P.Mode == ESensorGraphMode::EO;
		const float Gain = P.PhotonGain * P.AnalogGain;   // normalised DN, until the detector model lands
		for (int32 I = 0; I < W * H; ++I)
		{
			const FVector3f C(Sanitize(Scene[I].R) * P.InputScale, Sanitize(Scene[I].G) * P.InputScale, Sanitize(Scene[I].B) * P.InputScale);
			const float S = C.X * P.SignalWeights.X + C.Y * P.SignalWeights.Y + C.Z * P.SignalWeights.Z;
			++R.Histogram.Bins[FSensorHistogram::BinOf(S)];
			if (bEo)
			{
				Out[I] = FVector4f(Oetf709(Knee(C.X * Gain, P.KneeStart)), Oetf709(Knee(C.Y * Gain, P.KneeStart)),
					Oetf709(Knee(C.Z * Gain, P.KneeStart)), 1.0f);
			}
			else
			{
				float V = FMath::Clamp(P.DisplayGain * (S * Gain) + P.DisplayOffset, 0.0f, 1.0f);
				if (P.bBlackHot) V = 1.0f - V;
				Out[I] = FVector4f(V, V, V, V);
			}
		}
		uint8* Y = R.Nv12.GetData();
		uint8* UV = Y + W * H;
		for (int32 Row = 0; Row < H; Row += 2)
		{
			for (int32 X = 0; X < W; X += 2)
			{
				FVector3f Sum(0, 0, 0);
				for (int32 Dy = 0; Dy < 2; ++Dy)
				{
					for (int32 Dx = 0; Dx < 2; ++Dx)
					{
						const FVector4f& O = Out[(Row + Dy) * W + X + Dx];
						const float Luma = bEo ? (0.2126f * O.X + 0.7152f * O.Y + 0.0722f * O.Z) : O.W;
						Y[(Row + Dy) * W + X + Dx] = ToLimited(Luma, 219.0f);
						Sum += FVector3f(O.X, O.Y, O.Z);
					}
				}
				const FVector3f M = Sum * 0.25f;
				const float Ym = 0.2126f * M.X + 0.7152f * M.Y + 0.0722f * M.Z;
				UV[(Row / 2) * W + X]     = bEo ? ToChroma((M.Z - Ym) / 1.8556f) : 128;
				UV[(Row / 2) * W + X + 1] = bEo ? ToChroma((M.X - Ym) / 1.5748f) : 128;
			}
		}
		return R;
	}
}
