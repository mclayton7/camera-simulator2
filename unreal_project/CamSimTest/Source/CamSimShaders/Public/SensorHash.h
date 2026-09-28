// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** Stateless sensor noise: a PCG hash of (x, y, frame, seed, stream). Integer-exact, so the HLSL
 *  mirror (CamSimSensorCommon.ush) produces the same bits; keep the two in step.
 *  Stream ids: 1 PRNU, 2 shot, 3 DSNU, 4 read, 5 temporal (bolometer), 6 pixel FPN, 7 column FPN,
 *  8 row FPN, 9 defects; photon-detector channel c adds c * 16. Fixed-pattern streams pass
 *  Frame = FixedFrame. */
namespace CamSimHash
{
	constexpr uint32 FixedFrame = 0xFFFFFFFFu;

	/** PCG-RXS-M-XS 32-bit output permutation (Jarzynski & Olano 2020). */
	inline uint32 Pcg(uint32 V)
	{
		const uint32 S = V * 747796405u + 2891336453u;
		const uint32 W = ((S >> ((S >> 28u) + 4u)) ^ S) * 277803737u;
		return (W >> 22u) ^ W;
	}

	inline uint32 Hash(uint32 X, uint32 Y, uint32 Frame, uint32 Seed, uint32 Stream)
	{
		return Pcg(X ^ Pcg(Y ^ Pcg(Frame ^ Pcg(Seed ^ Pcg(Stream)))));
	}

	/** Top 24 bits -> open interval (0, 1). */
	inline float Uniform(uint32 H) { return (float(H >> 8) + 0.5f) * (1.0f / 16777216.0f); }

	/** Standard normal: Box-Muller of two hashed uniforms (sub-streams 2*Stream, 2*Stream + 1). */
	inline float Gaussian(uint32 X, uint32 Y, uint32 Frame, uint32 Seed, uint32 Stream)
	{
		const float U1 = Uniform(Hash(X, Y, Frame, Seed, Stream * 2u));
		const float U2 = Uniform(Hash(X, Y, Frame, Seed, Stream * 2u + 1u));
		return FMath::Sqrt(-2.0f * FMath::Loge(U1)) * FMath::Cos(2.0f * PI * U2);
	}
}
