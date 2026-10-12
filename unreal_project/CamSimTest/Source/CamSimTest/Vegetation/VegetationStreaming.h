// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"
#include "Vegetation/VegetationGrid.h"

enum class EVegCellState : uint8
{
	Generating,     // GenerateCell running on a worker
	Sampling,       // heights requested from the terrain tileset
	ReadyToSpawn,   // grounded, waiting for a spawn slot
	Spawned,        // actor exists
	Empty,          // no grounded trees: nothing to spawn, never retried while tracked
};

/**
 * Which vegetation cells to generate, spawn and drop around the camera (REALISM R4 slice, docs/vegetation.md). Pure
 * state machine; the manager does the work and reports back. In flight (Generating + Sampling) is capped at
 * MaxCellsInFlight, spawns at MaxSpawnsPerTick, both nearest first; tracked cells beyond RadiusM * DropFactor drop.
 */
class CAMSIMTEST_API FVegetationStreaming
{
public:
	struct FParams
	{
		double RadiusM = 1000.0;
		double CellSizeM = 100.0;
		int32  MaxCellsInFlight = 4;
		int32  MaxSpawnsPerTick = 2;
		double DropFactor = 1.2;
	};

	struct FActions
	{
		TArray<CamSimVegetation::FVegCellId> ToGenerate;
		TArray<CamSimVegetation::FVegCellId> ToSpawn;
		TArray<CamSimVegetation::FVegCellId> ToDrop;   // cells that had an actor
	};

	explicit FVegetationStreaming(const FParams& InParams) : Params(InParams) {}

	FActions Tick(double CamLatDeg, double CamLonDeg);
	/**
	 * The caller picks the cells (REALISM R4 blocks and tiers): tracked ids not in Wanted are dropped when MayDrop(id) (else
	 * kept as they are, never spawned); ReadyToSpawn ids in Wanted spawn in Wanted's order, at most SpawnCap; untracked
	 * Wanted ids start generating while this streaming's in-flight count is below InFlightCap. Wanted is nearest first.
	 */
	FActions TickWanted(TConstArrayView<CamSimVegetation::FVegCellId> Wanted,
		TFunctionRef<bool(CamSimVegetation::FVegCellId)> MayDrop, int32 InFlightCap, int32 SpawnCap);
	/** Spawned or Empty: nothing more will happen to it while tracked. */
	bool IsSettled(CamSimVegetation::FVegCellId Id) const;
	void OnGenerated(CamSimVegetation::FVegCellId Id, int32 NumInstances);
	void OnHeightsSampled(CamSimVegetation::FVegCellId Id, int32 NumGrounded);
	TOptional<EVegCellState> GetState(CamSimVegetation::FVegCellId Id) const;
	int32 NumInFlight() const;
	int32 NumTracked() const { return Cells.Num(); }

private:
	FParams Params;
	TMap<CamSimVegetation::FVegCellId, EVegCellState> Cells;
};
