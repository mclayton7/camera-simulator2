// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"
#include "Vegetation/VegetationGrid.h"

/**
 * Vegetation blocks and tiers (REALISM R4, docs/vegetation.md): a block row is BlockCells cell rows; along it, blocks are
 * BlockCells x the middle row's column width wide, and a cell belongs to the block holding its centre (the grid's column
 * widths differ per row, so cell columns drift row to row; this keeps blocks ~square anywhere). Usually N x N cells, give or
 * take a column. A cell's content never depends on its block. Streaming decides per block whether it is near (its cells at
 * full detail) or far (one low-poly actor). Pure; game thread for the planner.
 */
namespace CamSimVegetation
{
	using FVegBlockId = FVegCellId;

	enum class EVegTier : uint8 { Near, Far };

	/** The block holding the cell's centre. */
	CAMSIMTEST_API FVegBlockId BlockOf(FVegCellId Cell, int32 BlockCells, double CellM);
	/** The block's cells, rows south to north, cols west to east (cols past a row's NumCols and rows past MaxLatDeg left out). */
	CAMSIMTEST_API TArray<FVegCellId> CellsOfBlock(FVegBlockId Block, int32 BlockCells, double CellM);
	CAMSIMTEST_API void BlockCentre(FVegBlockId Block, int32 BlockCells, double CellM, double& OutLatDeg, double& OutLonDeg);
	/** Blocks whose centre is within RadiusM, nearest first (ties Row, then Col). Empty past MaxLatDeg. */
	CAMSIMTEST_API TArray<FVegBlockId> BlocksWithin(double LatDeg, double LonDeg, double RadiusM, double CellM, int32 BlockCells);
	/** The camera's height above the ground: CamAltM minus the terrain under it (NadirGroundM), else under the frame
	 *  centre, else the last one known (InOutGroundM, updated); never negative. */
	CAMSIMTEST_API double HeightAboveGround(double CamAltM, const TOptional<double>& NadirGroundM,
		const TOptional<double>& FrameCentreGroundM, double& InOutGroundM);
}

/**
 * Which blocks are near and which far around the camera (REALISM R4), by 3D distance from the block centre to the camera
 * (ground distance and its height above the ground): a block becomes near inside NearM, far inside FarM
 * (FarM 0 = no far tier); a near block stays near up to NearM * NearDropFactor, a far block stays far up to
 * FarM * FarDropFactor, and a far block turns near only inside NearM. Replans only after the camera moved MoveM.
 */
class CAMSIMTEST_API FVegetationTierPlanner
{
public:
	struct FParams
	{
		double NearM = 1000.0;
		double FarM = 4000.0;
		double NearDropFactor = 1.2;
		double FarDropFactor = 1.1;
		double CellSizeM = 100.0;
		int32  BlockCells = 4;
		double MoveM = 25.0;
	};
	struct FChange
	{
		CamSimVegetation::FVegBlockId Block;
		TOptional<CamSimVegetation::EVegTier> From;
		TOptional<CamSimVegetation::EVegTier> To;   // unset = no longer tracked
	};

	explicit FVegetationTierPlanner(const FParams& P) : Params(P) {}

	/** Recomputes the tiers when the camera moved at least MoveM (along the ground or in height) since the last plan;
	 *  returns the changed blocks. Distances are 3D from a camera HeightM above the ground (HISM culls are 3D). */
	TArray<FChange> Update(double CamLatDeg, double CamLonDeg, double HeightM = 0.0);
	TOptional<CamSimVegetation::EVegTier> GetTier(CamSimVegetation::FVegBlockId B) const;
	/** Cells of the near blocks / the far blocks, each nearest first from the last plan's camera position. */
	const TArray<CamSimVegetation::FVegCellId>& GetNearCells() const { return NearCells; }
	const TArray<CamSimVegetation::FVegBlockId>& GetFarBlocks() const { return FarBlocks; }
	const FParams& GetParams() const { return Params; }

private:
	FParams Params;
	TMap<CamSimVegetation::FVegBlockId, CamSimVegetation::EVegTier> Tiers;
	TArray<CamSimVegetation::FVegCellId> NearCells;
	TArray<CamSimVegetation::FVegBlockId> FarBlocks;
	bool bPlanned = false;
	double LastLat = 0.0, LastLon = 0.0, LastH = 0.0;
};
