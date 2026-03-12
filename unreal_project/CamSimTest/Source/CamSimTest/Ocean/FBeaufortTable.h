// Copyright CamSim Contributors. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * Maps Beaufort scale (0–12) to physical wave parameters.
 * Values are linearly interpolated between table entries.
 * WaveLen = 0 at Beaufort 0 is intentional (no waves = flat surface).
 */
struct FBeaufortEntry
{
	float WaveHtM;   // significant wave height, metres
	float WaveLenM;  // dominant wave length, metres
	float Choppiness;// [0,1] Gerstner choppiness
};

struct FBeaufortTable
{
	static constexpr int32 NumEntries = 13; // 0..12

	/**
	 * Beaufort scale 0–12 reference values.
	 * `inline static constexpr` (C++17) ensures a single definition across TUs,
	 * avoiding ODR violations in MSVC non-optimised builds when the array address is taken.
	 */
	inline static constexpr FBeaufortEntry Entries[NumEntries] = {
		{  0.0f,   0.0f, 0.0f },  //  0 — Calm
		{  0.1f,   3.0f, 0.1f },  //  1 — Light air
		{  0.3f,   8.0f, 0.2f },  //  2 — Light breeze
		{  0.6f,  16.0f, 0.3f },  //  3 — Gentle breeze
		{  1.0f,  30.0f, 0.4f },  //  4 — Moderate breeze
		{  1.5f,  50.0f, 0.5f },  //  5 — Fresh breeze
		{  2.5f,  70.0f, 0.6f },  //  6 — Strong breeze
		{  4.0f, 100.0f, 0.7f },  //  7 — Near gale
		{  5.0f, 140.0f, 0.8f },  //  8 — Gale
		{  7.0f, 190.0f, 0.85f}, //  9 — Strong gale
		{  9.0f, 250.0f, 0.9f },  // 10 — Storm
		{ 11.5f, 320.0f, 0.95f}, // 11 — Violent storm
		{ 14.0f, 400.0f, 1.0f },  // 12 — Hurricane
	};

	/**
	 * Linearly interpolate wave parameters for a fractional Beaufort value.
	 * Clamps to [0, 12].
	 */
	static FBeaufortEntry Sample(float Beaufort)
	{
		Beaufort = FMath::Clamp(Beaufort, 0.0f, 12.0f);
		const int32 Lo = FMath::FloorToInt(Beaufort);
		const int32 Hi = FMath::Min(Lo + 1, NumEntries - 1);
		const float T  = Beaufort - static_cast<float>(Lo);

		FBeaufortEntry Result;
		Result.WaveHtM    = FMath::Lerp(Entries[Lo].WaveHtM,    Entries[Hi].WaveHtM,    T);
		Result.WaveLenM   = FMath::Lerp(Entries[Lo].WaveLenM,   Entries[Hi].WaveLenM,   T);
		Result.Choppiness = FMath::Lerp(Entries[Lo].Choppiness, Entries[Hi].Choppiness, T);
		return Result;
	}

	/** Convenience: sample from an integer Beaufort state. */
	static FBeaufortEntry Sample(int32 Beaufort)
	{
		return Sample(static_cast<float>(Beaufort));
	}
};
