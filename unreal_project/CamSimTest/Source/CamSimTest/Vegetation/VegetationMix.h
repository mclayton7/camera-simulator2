// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FVegetationLibrary;

/**
 * Species mix per land-cover code (REALISM R4, docs/vegetation.md): which library types stand on a code and in what
 * proportions, trees and shrubs separately, plus the shrub density. From the scene package's vegetation/mix.json
 * (camsim-vegmix-1, written by camsim-scene from the [vegetation] table), vegetation.mix_file, or DefaultMix. Pure.
 */
namespace CamSimVegetation
{
	struct FMixEntry
	{
		int32 Type = 0;            // library index
		float CumWeight = 0.0f;    // cumulative, ascending; the last is exactly 1
	};

	struct FCodeMix
	{
		TArray<FMixEntry> Trees;
		TArray<FMixEntry> Shrubs;
		float ShrubsPer100M2 = 0.0f;
	};

	struct FVegetationMix
	{
		TMap<uint8, FCodeMix> Codes;
		TArray<float> TypeHeightM;        // per library index
		TArray<float> TypeCrownRadiusM;   // per library index
		const FCodeMix* Find(uint8 Code) const { return Codes.Find(Code); }
	};

	/** A code whose mix lists shrubs but no shrub_density gets this (shrubs per 100 m^2). */
	inline constexpr float DefaultShrubsPer100M2 = 5.0f;

	/** Index into Entries (not a library index) for a uniform U in [0, 1): the first entry with U < CumWeight; -1 when
	 *  Entries is empty. */
	CAMSIMTEST_API int32 PickType(const TArray<FMixEntry>& Entries, float U);
	/** {"format":"camsim-vegmix-1","mix":{"<code>":{"<type>":w,...}},"shrub_density":{"<code>":n}} against Lib: each code's
	 *  names in alphabetical order, split into trees and shrubs by role, each list normalised. Names Lib lacks go to
	 *  OutUnknown and are dropped; a code left empty is omitted. Codes 1..255, weights > 0, densities [0, 100]. */
	CAMSIMTEST_API bool ParseMix(const FString& Json, const FVegetationLibrary& Lib, FVegetationMix& Out,
		TArray<FString>& OutUnknown, FString& OutError);
	/** Code 10 -> library index 0 if it is a tree (R4); 20 -> shrub_chaparral at 6 / 100 m^2 and 30 -> at 1 / 100 m^2
	 *  when the library has it. */
	CAMSIMTEST_API FVegetationMix DefaultMix(const FVegetationLibrary& Lib);
	/** One code, one tree type of weight 1, no shrubs (the R4 GenerateCell wrappers). */
	CAMSIMTEST_API FVegetationMix SingleTypeMix(uint8 Code, int32 Type, float HeightM, float CrownRadiusM);
}
