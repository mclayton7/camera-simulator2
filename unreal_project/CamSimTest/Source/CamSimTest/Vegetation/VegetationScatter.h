// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Vegetation/VegetationGrid.h"

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
		uint8  TreeCode = 10;       // WorldCover tree cover
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

	CAMSIMTEST_API int32 CandidatesPerAxis(const FScatterParams& P);
	/** Accepted trees per m^2 on all-tree land cover. */
	CAMSIMTEST_API double ExpectedTreesPerM2(const FScatterParams& P);
	/** The vegetation seed from a package / config seed (mixed with a constant so it differs from other users of the seed). */
	CAMSIMTEST_API uint32 VegetationSeed(uint32 BaseSeed);
	CAMSIMTEST_API TArray<FTreeInstance> GenerateCell(uint32 Seed, FVegCellId Cell, const FScatterParams& P, FCodeSampler Sampler);
	/** Order-sensitive hash of the instances' bits (dumps, determinism checks). */
	CAMSIMTEST_API uint32 HashInstances(const TArray<FTreeInstance>& Instances);
}
