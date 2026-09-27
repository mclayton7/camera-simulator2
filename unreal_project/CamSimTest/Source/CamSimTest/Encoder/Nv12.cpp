// Copyright CamSim Contributors. All Rights Reserved.

#include "Encoder/Nv12.h"

namespace CamSimNv12
{
	void SplitToYuv420p(const uint8* Nv12, int32 W, int32 H,
		uint8* Y, int32 YStride, uint8* U, int32 UStride, uint8* V, int32 VStride)
	{
		for (int32 Row = 0; Row < H; ++Row)
		{
			FMemory::Memcpy(Y + Row * YStride, Nv12 + Row * W, W);
		}
		const uint8* UV = Nv12 + W * H;
		for (int32 Row = 0; Row < H / 2; ++Row)
		{
			const uint8* Src = UV + Row * W;
			uint8* DU = U + Row * UStride;
			uint8* DV = V + Row * VStride;
			for (int32 X = 0; X < W / 2; ++X) { DU[X] = Src[2 * X]; DV[X] = Src[2 * X + 1]; }
		}
	}

	void ToBgra(const uint8* Nv12, int32 W, int32 H, TArray<FColor>& Out)
	{
		Out.SetNumUninitialized(W * H);
		const uint8* UV = Nv12 + W * H;
		for (int32 Row = 0; Row < H; ++Row)
		{
			for (int32 X = 0; X < W; ++X)
			{
				const float Yn = (Nv12[Row * W + X] - 16.0f) / 219.0f;
				const int32 C = (Row / 2) * W + (X / 2) * 2;
				const float Cb = (UV[C] - 128.0f) / 224.0f;
				const float Cr = (UV[C + 1] - 128.0f) / 224.0f;
				// BT.709 inverse (Kr = 0.2126, Kb = 0.0722)
				const float R = Yn + 1.5748f * Cr;
				const float G = Yn - 0.1873f * Cb - 0.4681f * Cr;
				const float B = Yn + 1.8556f * Cb;
				auto To8 = [](float V) { return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt32(V * 255.0f), 0, 255)); };
				Out[Row * W + X] = FColor(To8(R), To8(G), To8(B), 255);
			}
		}
	}
}
