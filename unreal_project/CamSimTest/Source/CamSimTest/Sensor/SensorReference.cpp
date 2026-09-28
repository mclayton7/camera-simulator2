// Copyright CamSim Contributors. All Rights Reserved.

#include "Sensor/SensorReference.h"
#include "SensorHash.h"

namespace CamSimSensorRef
{
	float Sanitize(float V)
	{
		if (FMath::IsNaN(V)) return 0.0f;
		return FMath::Clamp(V, 0.0f, 65504.0f);   // +Inf -> 65504
	}

	float Knee(float X, float K)
	{
		// Normalised so [K, 1] maps onto [K, 1]: full scale (DN = AdcMax) reaches white.
		if (X <= K || K >= 1.0f) return X;
		return K + (1.0f - K) * (1.0f - FMath::Exp(-(X - K) / (1.0f - K))) / (1.0f - FMath::Exp(-1.0f));
	}

	float Oetf709(float L)
	{
		L = FMath::Clamp(L, 0.0f, 1.0f);
		return L < 0.018f ? 4.5f * L : 1.099f * FMath::Pow(L, 0.45f) - 0.099f;
	}

	float DetectPixel(float Signal, int32 X, int32 Y, uint32 Channel, const FSensorFrameParams& P)
	{
		using namespace CamSimHash;
		// NaN guard: FMath::Clamp(NaN) returns the upper bound on the CPU but HLSL clamp returns 0.
		// Everything below is finite for a finite signal (Gaussians are bounded), so guard the input.
		if (FMath::IsNaN(Signal)) Signal = 0.0f;
		const uint32 Ux = static_cast<uint32>(X), Uy = static_cast<uint32>(Y);
		const uint32 Fixed = FixedFrame, Frame = P.FrameIndex, Seed = P.Seed;
		float Dn;
		if (P.DetectorType == 0)
		{
			const uint32 Base = Channel * 16u;
			const float E  = Signal * P.PhotonGain * P.FullWellE;
			const float E1 = E * (1.0f + P.Prnu * Gaussian(Ux, Uy, Fixed, Seed, Base + 1u));
			float E2 = E1 + P.DarkE;
			E2 += FMath::Sqrt(FMath::Max(E2, 0.0f)) * Gaussian(Ux, Uy, Frame, Seed, Base + 2u);   // shot: signal + dark
			const float E3 = E2 + P.DsnuE * Gaussian(Ux, Uy, Fixed, Seed, Base + 3u);
			const float E4 = E3 + P.ReadNoiseE * Gaussian(Ux, Uy, Frame, Seed, Base + 4u);
			const float E5 = FMath::Clamp(E4, 0.0f, P.FullWellE) * P.AnalogGain;
			Dn = FMath::Clamp(FMath::FloorToFloat(E5 * P.AdcMax / P.FullWellE + 0.5f), 0.0f, P.AdcMax);
		}
		else
		{
			const float V = Signal * P.PhotonGain
				+ P.TemporalNoise * Gaussian(Ux, Uy, Frame, Seed, 5u)
				+ P.PixelFpn * Gaussian(Ux, Uy, Fixed, Seed, 6u)
				+ P.ColumnFpn * Gaussian(Ux, 0u, Fixed, Seed, 7u)
				+ P.RowFpn * Gaussian(0u, Uy, Fixed, Seed, 8u);
			Dn = FMath::Clamp(FMath::FloorToFloat(V * P.AdcMax + 0.5f), 0.0f, P.AdcMax);
		}
		const float U = Uniform(Hash(Ux, Uy, Fixed, Seed, 2u * 9u));   // raw draw of stream 9: sub-stream 18
		if (U < P.HotFraction) return P.AdcMax;
		if (U > 1.0f - P.DeadFraction) return 0.0f;
		return Dn;
	}

	TArray<float> DetectImage(const TArray<float>& SignalRgbOrMono, int32 W, int32 H, int32 Channels, const FSensorFrameParams& P)
	{
		check(SignalRgbOrMono.Num() == W * H * Channels);
		TArray<float> Dn;
		Dn.SetNumUninitialized(W * H * Channels);
		for (int32 C = 0; C < Channels; ++C)
		{
			const int32 Plane = C * W * H;
			for (int32 Y = 0; Y < H; ++Y)
			{
				for (int32 X = 0; X < W; ++X)
				{
					Dn[Plane + Y * W + X] = DetectPixel(SignalRgbOrMono[Plane + Y * W + X], X, Y, static_cast<uint32>(C), P);
				}
			}
		}
		return Dn;
	}

	float DisplayEo(float N, const FSensorFrameParams& P) { return Oetf709(Knee(N, P.KneeStart)); }

	float DisplayIr(float N, const FSensorFrameParams& P)
	{
		const float V = FMath::Clamp(N * P.DisplayGain + P.DisplayOffset, 0.0f, 1.0f);
		return P.bBlackHot ? 1.0f - V : V;
	}

	static uint8 ToLimited(float V, float Scale) { return static_cast<uint8>(FMath::Clamp(FMath::FloorToInt32(16.0f + Scale * V + 0.5f), 0, 255)); }
	static uint8 ToChroma(float C) { return static_cast<uint8>(FMath::Clamp(FMath::FloorToInt32(128.0f + 224.0f * C + 0.5f), 0, 255)); }

	/** Steps 1-3: planar sanitised RGB (3*W*H) and signal s (W*H); fills the histogram. */
	static void PrepareScene(const TArray<FLinearColor>& Scene, int32 W, int32 H, const FSensorFrameParams& P,
		TArray<float>& Rgb, TArray<float>& Signal, FSensorHistogram& Histogram)
	{
		check(Scene.Num() == W * H && W % 4 == 0 && H % 2 == 0);
		const int32 N = W * H;
		Rgb.SetNumUninitialized(3 * N);
		Signal.SetNumUninitialized(N);
		for (int32 I = 0; I < N; ++I)
		{
			const float R = Sanitize(Scene[I].R) * P.InputScale, G = Sanitize(Scene[I].G) * P.InputScale, B = Sanitize(Scene[I].B) * P.InputScale;
			Rgb[I] = R; Rgb[N + I] = G; Rgb[2 * N + I] = B;
			Signal[I] = R * P.SignalWeights.X + G * P.SignalWeights.Y + B * P.SignalWeights.Z;
			++Histogram.Bins[FSensorHistogram::BinOf(Signal[I])];
		}
	}

	/** Step 7. Out: xyz = R'G'B' (EO) or v,v,v; w = luma source (IR). */
	static void PackNv12(const TArray<FVector4f>& Out, int32 W, int32 H, bool bEo, TArray<uint8>& Nv12)
	{
		Nv12.SetNumZeroed(W * H * 3 / 2);
		uint8* Y = Nv12.GetData();
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
	}

	FResult Run(const TArray<FLinearColor>& Scene, int32 W, int32 H, const FSensorFrameParams& P)
	{
		FResult R;
		TArray<float> Rgb, Signal;
		PrepareScene(Scene, W, H, P, Rgb, Signal, R.Histogram);
		// Step 4, optics: pass-through until Task 8 (FocalPx 0 and PsfSigmaPx 0 are "off").
		const bool bEo = P.Mode == ESensorGraphMode::EO;
		const int32 N = W * H;
		const TArray<float> Dn = bEo ? DetectImage(Rgb, W, H, 3, P) : DetectImage(Signal, W, H, 1, P);
		const float InvAdc = 1.0f / P.AdcMax;
		TArray<FVector4f> Out;
		Out.SetNumUninitialized(N);
		for (int32 I = 0; I < N; ++I)
		{
			if (bEo)
			{
				Out[I] = FVector4f(DisplayEo(Dn[I] * InvAdc, P), DisplayEo(Dn[N + I] * InvAdc, P), DisplayEo(Dn[2 * N + I] * InvAdc, P), 1.0f);
			}
			else
			{
				const float V = DisplayIr(Dn[I] * InvAdc, P);
				Out[I] = FVector4f(V, V, V, V);
			}
		}
		PackNv12(Out, W, H, bEo, R.Nv12);
		return R;
	}

	FResult RunDisplayOnly(const TArray<FLinearColor>& Scene, int32 W, int32 H, const FSensorFrameParams& P)
	{
		FResult R;
		TArray<float> Rgb, Signal;
		PrepareScene(Scene, W, H, P, Rgb, Signal, R.Histogram);
		const bool bEo = P.Mode == ESensorGraphMode::EO;
		const int32 N = W * H;
		const float Gain = P.PhotonGain * P.AnalogGain;
		TArray<FVector4f> Out;
		Out.SetNumUninitialized(N);
		for (int32 I = 0; I < N; ++I)
		{
			if (bEo)
			{
				Out[I] = FVector4f(DisplayEo(Rgb[I] * Gain, P), DisplayEo(Rgb[N + I] * Gain, P), DisplayEo(Rgb[2 * N + I] * Gain, P), 1.0f);
			}
			else
			{
				const float V = DisplayIr(Signal[I] * Gain, P);
				Out[I] = FVector4f(V, V, V, V);
			}
		}
		PackNv12(Out, W, H, bEo, R.Nv12);
		return R;
	}
}
