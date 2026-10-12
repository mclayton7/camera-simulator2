// Copyright CamSim Contributors. All Rights Reserved.

#include "Vegetation/VegetationBlocks.h"

namespace CamSimVegetation
{
	namespace
	{
		int32 FloorDiv(int32 A, int32 N) { return A >= 0 ? A / N : -((-A + N - 1) / N); }

		/** The row whose column width sets a block row's block width (its middle row). */
		int32 MidRow(int32 BlockRow, int32 N) { return BlockRow * N + N / 2; }

		/** Width of a block in degrees of longitude in block row BlockRow: N cells of its middle row. */
		double BlockDLonDeg(int32 BlockRow, double CellM, int32 N) { return N * CellDLonDeg(MidRow(BlockRow, N), CellM); }

		int32 NumBlockCols(int32 BlockRow, double CellM, int32 N)
		{
			return FMath::Max(1, FMath::CeilToInt32(360.0 / BlockDLonDeg(BlockRow, CellM, N)));
		}

		/** A cell's centre longitude measured from 180 W, unclamped (the last column of a row is narrower). */
		double CellCentreFromWest(FVegCellId C, double CellM) { return (C.Col + 0.5) * CellDLonDeg(C.Row, CellM); }

		bool ByDistance(const TPair<double, FVegCellId>& A, const TPair<double, FVegCellId>& B)
		{
			if (A.Key != B.Key) return A.Key < B.Key;
			return A.Value.Row != B.Value.Row ? A.Value.Row < B.Value.Row : A.Value.Col < B.Value.Col;
		}
	}

	FVegBlockId BlockOf(FVegCellId Cell, int32 BlockCells, double CellM)
	{
		// Not Col / N: column widths differ per row, so equal column indices drift apart row to row (tens to hundreds of
		// metres per row away from 180 W). A cell belongs to the block holding its centre, so blocks stay ~square.
		const int32 BR = FloorDiv(Cell.Row, BlockCells);
		const int32 BC = FMath::FloorToInt32(CellCentreFromWest(Cell, CellM) / BlockDLonDeg(BR, CellM, BlockCells));
		return { BR, FMath::Clamp(BC, 0, NumBlockCols(BR, CellM, BlockCells) - 1) };
	}

	TArray<FVegCellId> CellsOfBlock(FVegBlockId Block, int32 BlockCells, double CellM)
	{
		TArray<FVegCellId> Out;
		const double W = Block.Col * BlockDLonDeg(Block.Row, CellM, BlockCells);
		const double E = W + BlockDLonDeg(Block.Row, CellM, BlockCells);
		for (int32 R = Block.Row * BlockCells; R < (Block.Row + 1) * BlockCells; ++R)
		{
			double La, Lo; CellCentre({ R, 0 }, CellM, La, Lo);
			if (!IsLatAllowed(La)) continue;
			const double DLon = CellDLonDeg(R, CellM);
			const int32 Cols = NumCols(R, CellM);
			const int32 C0 = FMath::Max(0, FMath::FloorToInt32(W / DLon) - 1);
			const int32 C1 = FMath::Min(Cols - 1, FMath::CeilToInt32(E / DLon) + 1);
			for (int32 C = C0; C <= C1; ++C)
				if (BlockOf({ R, C }, BlockCells, CellM) == Block) Out.Add({ R, C });   // the same predicate as BlockOf
		}
		return Out;
	}

	void BlockCentre(FVegBlockId Block, int32 BlockCells, double CellM, double& OutLatDeg, double& OutLonDeg)
	{
		OutLatDeg = -90.0 + (Block.Row + 0.5) * BlockCells * CellDLatDeg(CellM);
		OutLonDeg = FMath::Min(-180.0 + (Block.Col + 0.5) * BlockDLonDeg(Block.Row, CellM, BlockCells), 180.0);
	}

	TArray<FVegBlockId> BlocksWithin(double LatDeg, double LonDeg, double RadiusM, double CellM, int32 BlockCells)
	{
		TArray<FVegBlockId> Out;
		if (!IsLatAllowed(LatDeg) || !(RadiusM > 0.0) || !(CellM > 0.0) || BlockCells < 1) return Out;
		const double DLatR = RadiusM / MetresPerDegLat;
		const int32 Row0 = FloorDiv(CellOf(LatDeg - DLatR, LonDeg, CellM).Row, BlockCells);
		const int32 Row1 = FloorDiv(CellOf(LatDeg + DLatR, LonDeg, CellM).Row, BlockCells);
		TSet<FVegBlockId> Seen;
		TArray<TPair<double, FVegBlockId>> Hits;
		for (int32 BR = Row0; BR <= Row1; ++BR)
		{
			double CLat, Unused; BlockCentre({ BR, 0 }, BlockCells, CellM, CLat, Unused);
			if (!IsLatAllowed(CLat)) continue;
			const double DLon = BlockDLonDeg(BR, CellM, BlockCells);
			const int32 Cols = NumBlockCols(BR, CellM, BlockCells);
			const double SpanDeg = RadiusM / (MetresPerDegLat * FMath::Max(FMath::Cos(FMath::DegreesToRadians(CLat)), 0.01)) + DLon;
			double W = FMath::Fmod(LonDeg - SpanDeg + 180.0, 360.0);
			if (W < 0.0) W += 360.0;
			const int32 C0 = FMath::FloorToInt32(W / DLon);
			const int32 Count = FMath::Min(FMath::CeilToInt32(2.0 * SpanDeg / DLon) + 2, Cols);
			for (int32 K = 0; K < Count; ++K)
			{
				const FVegBlockId Id{ BR, ((C0 + K) % Cols + Cols) % Cols };
				if (Seen.Contains(Id)) continue;
				Seen.Add(Id);
				double A, O; BlockCentre(Id, BlockCells, CellM, A, O);
				const double D = GroundDistanceM(LatDeg, LonDeg, A, O);
				if (D <= RadiusM) Hits.Emplace(D, Id);
			}
		}
		Hits.Sort(ByDistance);
		for (const auto& H : Hits) Out.Add(H.Value);
		return Out;
	}
}

using namespace CamSimVegetation;

TArray<FVegetationTierPlanner::FChange> FVegetationTierPlanner::Update(double CamLatDeg, double CamLonDeg)
{
	TArray<FChange> Changes;
	if (bPlanned && GroundDistanceM(LastLat, LastLon, CamLatDeg, CamLonDeg) < Params.MoveM) return Changes;
	bPlanned = true;
	LastLat = CamLatDeg;
	LastLon = CamLonDeg;
	const int32 N = Params.BlockCells;
	const bool bFar = Params.FarM > 0.0;
	const double Reach = FMath::Max(Params.NearM * Params.NearDropFactor, bFar ? Params.FarM * Params.FarDropFactor : 0.0);
	TMap<FVegBlockId, EVegTier> Next;
	TArray<TPair<double, FVegCellId>> Near, Far;
	for (const FVegBlockId& B : BlocksWithin(CamLatDeg, CamLonDeg, Reach, Params.CellSizeM, N))
	{
		double A, O; BlockCentre(B, N, Params.CellSizeM, A, O);
		const double D = GroundDistanceM(CamLatDeg, CamLonDeg, A, O);
		const EVegTier* Prev = Tiers.Find(B);
		TOptional<EVegTier> T;
		if (D < Params.NearM) T = EVegTier::Near;
		else if (Prev && *Prev == EVegTier::Near && D < Params.NearM * Params.NearDropFactor) T = EVegTier::Near;
		else if (bFar && D < Params.FarM) T = EVegTier::Far;
		else if (bFar && Prev && *Prev == EVegTier::Far && D < Params.FarM * Params.FarDropFactor) T = EVegTier::Far;
		if (!T.IsSet()) continue;
		Next.Add(B, *T);
		if (*T == EVegTier::Far) Far.Emplace(D, B);
		else for (const FVegCellId& C : CellsOfBlock(B, N, Params.CellSizeM))
		{
			double CA, CO; CellCentre(C, Params.CellSizeM, CA, CO);
			Near.Emplace(GroundDistanceM(CamLatDeg, CamLonDeg, CA, CO), C);
		}
	}
	for (const auto& Kv : Tiers)
	{
		const EVegTier* To = Next.Find(Kv.Key);
		if (!To) Changes.Add({ Kv.Key, Kv.Value, {} });
		else if (*To != Kv.Value) Changes.Add({ Kv.Key, Kv.Value, *To });
	}
	for (const auto& Kv : Next) if (!Tiers.Contains(Kv.Key)) Changes.Add({ Kv.Key, {}, Kv.Value });
	Tiers = MoveTemp(Next);
	Near.Sort(ByDistance);
	Far.Sort(ByDistance);
	NearCells.Reset(Near.Num());
	for (const auto& P : Near) NearCells.Add(P.Value);
	FarBlocks.Reset(Far.Num());
	for (const auto& P : Far) FarBlocks.Add(P.Value);
	return Changes;
}

TOptional<EVegTier> FVegetationTierPlanner::GetTier(FVegBlockId B) const
{
	const EVegTier* T = Tiers.Find(B);
	return T ? TOptional<EVegTier>(*T) : TOptional<EVegTier>();
}
