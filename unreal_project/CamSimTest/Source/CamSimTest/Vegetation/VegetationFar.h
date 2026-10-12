// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"
#include "Vegetation/VegetationBlocks.h"
#include "Vegetation/VegetationScatter.h"

/**
 * Far-tier content (REALISM R4, docs/vegetation.md): the far tier draws a deterministic subset of the same trees the
 * near tier would (same GenerateCellContent), enlarged, split into dissolve buckets with stepped cull distances, grounded
 * on a coarse height grid per block. Pure; any thread.
 */
namespace CamSimVegetation
{
	struct FFarParams
	{
		float Keep = 0.35f;         // vegetation.far_keep
		float Scale = 1.3f;         // vegetation.far_scale
		int32 Buckets = 4;          // dissolve steps
		TArray<bool> TypeHasFar;    // per library index; false or missing = never drawn far
	};

	struct FFarTree
	{
		FTreeInstance Tree;
		uint8 Bucket = 0;
		bool operator==(const FFarTree&) const = default;
	};

	/** The far subset of one cell's trees: kept when Uniform(Hash(col, row, k, seed, 172)) < Keep (k = index in Trees),
	 *  scale x Scale, bucket = floor(Uniform(Hash(col, row, k, seed, 173)) x Buckets). Types without a far mesh dropped. */
	CAMSIMTEST_API TArray<FFarTree> FarFilter(uint32 Seed, FVegCellId Cell, const TArray<FTreeInstance>& Trees, const FFarParams& P);
	/** Order-sensitive hash of the far trees (positions, yaw, scale, type, bucket). */
	CAMSIMTEST_API uint32 HashFarTrees(const TArray<FFarTree>& Trees);
	/** Cull distance of bucket b: FarM x (0.7 + 0.3 b / (Buckets - 1)) -> 0.7, 0.8, 0.9, 1.0 x FarM for 4; FarM for 1. */
	CAMSIMTEST_API float BucketCullM(float FarM, int32 Bucket, int32 Buckets);

	/** A regular lat/lon grid of heights over a block, bilinear between points. */
	struct CAMSIMTEST_API FFarHeightGrid
	{
		double SouthDeg = 0.0, WestDeg = 0.0, DLatDeg = 0.0, DLonDeg = 0.0;
		int32  N = 0;                       // points per axis
		TArray<TOptional<double>> H;        // row-major from the south-west, N * N

		/** Covers the block's cells' bounds with points about SpacingM apart (N = round(extent / spacing) + 1, >= 2). */
		static FFarHeightGrid ForBlock(FVegBlockId Block, int32 BlockCells, double CellM, double SpacingM);
		/** Covers a lat/lon box (a cell's bounds: the near tier's shrub heights) the same way. */
		static FFarHeightGrid ForBounds(double SouthDeg, double NorthDeg, double WestDeg, double EastDeg, double SpacingM);
		/** (lon, lat, 0) per point in H's order. */
		TArray<FVector> LonLatPoints() const;
		/** Unset outside the grid or when any corner of the point's quad is unset. */
		TOptional<double> Sample(double LatDeg, double LonDeg) const;
	};

	/**
	 * Far-tier heights from the rendered terrain (REALISM R4): one Trace (a line trace on the drawn tiles' collision) per
	 * point, so no tile is loaded for them. SampleHeightMostDetailed on a 100 m grid per far block loaded the finest
	 * terrain tiles over the whole 4 km disk; after a low pass over woodland the next pass ran +0.3 ms game p50, +2.5 ms
	 * p95 and +500 MiB (more tile loads and physics cooking; measured 2026-10-12). Points
	 * that miss (tiles not loaded yet, e.g. after a cut) are traced again every RetryTicks Tick() calls; after MaxRetries
	 * the batch answers with them unset. Game thread.
	 */
	class CAMSIMTEST_API FRetryingTraceSampler : public TSharedFromThis<FRetryingTraceSampler>
	{
	public:
		FRetryingTraceSampler(TFunction<TOptional<double>(double LatDeg, double LonDeg)> InTrace, int32 InRetryTicks = 30, int32 InMaxRetries = 20)
			: Trace(MoveTemp(InTrace)), RetryTicks(InRetryTicks), MaxRetries(InMaxRetries) {}
		/** LonLatH as SampleHeightMostDetailed takes it (X lon, Y lat). Done runs now when every point hits. */
		void Sample(const TArray<FVector>& LonLatH, TFunction<void(TArray<TOptional<double>>)> Done);
		void Tick();
		int32 NumPending() const { return Pending.Num(); }
	private:
		struct FBatch
		{
			TArray<FVector> Points;
			TArray<TOptional<double>> H;
			TFunction<void(TArray<TOptional<double>>)> Done;
			int32 TicksLeft = 0;
			int32 Retries = 0;
		};
		/** Traces the unset points; true when none is left unset. */
		bool TraceMissing(FBatch& B) const;
		TFunction<TOptional<double>(double, double)> Trace;
		int32 RetryTicks;
		int32 MaxRetries;
		TArray<FBatch> Pending;
	};
}
