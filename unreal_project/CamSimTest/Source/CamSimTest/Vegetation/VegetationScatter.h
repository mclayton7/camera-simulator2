// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Vegetation/VegetationGrid.h"
#include "Vegetation/VegetationMix.h"

/**
 * Deterministic tree scatter in one cell (REALISM R4 slice, docs/vegetation.md). A CandidatesPerAxis^2 jittered grid;
 * candidate i of cell (row, col) draws from CamSimHash::Hash(col, row, i, seed, 160 + k), so a cell's trees depend only
 * on (seed, cell, params, land cover). Each jitter stays inside its sub-cell, so trees never cross cell edges. Pure;
 * any thread (the sampler must be thread-safe if called off the game thread).
 */
namespace CamSimVegetation
{
	struct FScatterParams
	{
		double CellSizeM = 100.0;
		double SpacingM = 6.0;      // nominal; the real spacing is CellSizeM / CandidatesPerAxis
		float  PTree = 0.15f;       // acceptance at density 1 (~1 tree per 240 m^2: open-canopy oak woodland)
		float  DensityScale = 1.0f;
		float  ScaleMin = 0.8f;
		float  ScaleMax = 1.2f;
		uint8  TreeCode = 10;       // WorldCover tree cover (the R4 GenerateCell wrappers)
		// REALISM R4 scatter v2 (GenerateCellContent):
		float  MinCanopyHeightM = 3.0f;   // no tree on a canopy texel lower than this
		bool   bTrees = true;             // false: shrubs only (the shrub ring's cells)
		bool   bShrubs = true;
		float  ShrubSpacingM = 3.0f;      // nominal; the real spacing is CellSizeM / ShrubCandidatesPerAxis
		float  ShrubScaleMin = 0.7f;
		float  ShrubScaleMax = 1.3f;
	};

	struct FTreeInstance
	{
		double LatDeg = 0.0;
		double LonDeg = 0.0;
		float  YawDeg = 0.0f;
		float  Scale = 1.0f;
		uint8  Type = 0;
		bool operator==(const FTreeInstance&) const = default;
	};

	using FCodeSampler = TFunctionRef<uint8(double LatDeg, double LonDeg)>;

	/** What the scatter needs to know about a point (REALISM R4): its land-cover code and, where the package's canopy
	 *  layer covers it, the canopy cover and height there. */
	struct FVegPoint
	{
		uint8 Code = 0;
		bool  bHasCanopy = false;   // the canopy layer covers this point (else the R4 rule)
		float CoverFrac = 0.0f;     // 0..1
		float HeightM = 0.0f;
	};
	using FPointSampler = TFunctionRef<FVegPoint(double LatDeg, double LonDeg)>;

	struct FCellContent
	{
		TArray<FTreeInstance> Trees;
		TArray<FTreeInstance> Shrubs;
	};
	/** REALISM R3b: false = the candidate is too close to a building (FVegetationBuildingMask::IsClear). */
	using FClearPredicate = TFunctionRef<bool(double LatDeg, double LonDeg)>;

	CAMSIMTEST_API int32 CandidatesPerAxis(const FScatterParams& P);
	/** Accepted trees per m^2 on all-tree land cover. */
	CAMSIMTEST_API double ExpectedTreesPerM2(const FScatterParams& P);
	/** The vegetation seed from a package / config seed (mixed with a constant so it differs from other users of the seed). */
	CAMSIMTEST_API uint32 VegetationSeed(uint32 BaseSeed);
	CAMSIMTEST_API TArray<FTreeInstance> GenerateCell(uint32 Seed, FVegCellId Cell, const FScatterParams& P, FCodeSampler Sampler);
	/** GenerateCell with a building clearance test, applied after the land cover; the per-candidate hash draws are
	 *  unchanged, so the trees it keeps are bit-identical to the 4-argument call's. */
	CAMSIMTEST_API TArray<FTreeInstance> GenerateCell(uint32 Seed, FVegCellId Cell, const FScatterParams& P, FCodeSampler Sampler,
		FClearPredicate IsClear);
	CAMSIMTEST_API int32 ShrubCandidatesPerAxis(const FScatterParams& P);
	/** Tree acceptance on a canopy texel: the expected crown area per candidate equals the cover until it saturates at 1;
	 *  0 below MinCanopyHeightM or at no cover. OutScaleBase = clamp(HeightM / TypeHeightM, 0.5, 2). */
	CAMSIMTEST_API float CanopyAccept(const FScatterParams& P, float CoverFrac, float HeightM, float TypeHeightM,
		float CrownRadiusM, float& OutScaleBase);
	/**
	 * Scatter v2 (REALISM R4): trees on R4's grid (sub-streams 160-164 unchanged, species 165) where the code's mix lists
	 * trees, accepted by the canopy rule where Sample reports canopy and by R4's p = 0.15 x density elsewhere; shrubs on
	 * their own grid (166-171) where the code's mix lists shrubs, at its shrub density. IsClear (null = none) drops both.
	 * With no canopy and a one-tree mix of library index 0 on the tree code, Trees equal R4's bit for bit.
	 */
	CAMSIMTEST_API FCellContent GenerateCellContent(uint32 Seed, FVegCellId Cell, const FScatterParams& P,
		const FVegetationMix& Mix, FPointSampler Sample, const FClearPredicate* IsClear = nullptr);
	/** Order-sensitive hash of the instances' bits (dumps, determinism checks). */
	CAMSIMTEST_API uint32 HashInstances(const TArray<FTreeInstance>& Instances);
}
