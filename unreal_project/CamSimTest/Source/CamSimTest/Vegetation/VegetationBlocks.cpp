// Copyright CamSim Contributors. All Rights Reserved.

#include "Vegetation/VegetationBlocks.h"

namespace CamSimVegetation
{
	namespace
	{
		int32 FloorDiv(int32 A, int32 N) { return A >= 0 ? A / N : -((-A + N - 1) / N); }

		/** The row whose column width a block uses for its centre (its middle row). */
		int32 MidRow(FVegBlockId B, int32 N) { return B.Row * N + N / 2; }

		int32 NumBlockCols(int32 Row, double CellM, int32 N) { return (NumCols(Row, CellM) + N - 1) / N; }

		bool ByDistance(const TPair<double, FVegCellId>& A, const TPair<double, FVegCellId>& B)
		{
			if (A.Key != B.Key) return A.Key < B.Key;
			return A.Value.Row != B.Value.Row ? A.Value.Row < B.Value.Row : A.Value.Col < B.Value.Col;
		}
	}

	FVegBlockId BlockOf(FVegCellId Cell, int32 BlockCells)
	{
		return { FloorDiv(Cell.Row, BlockCells), FloorDiv(Cell.Col, BlockCells) };
	}

	TArray<FVegCellId> CellsOfBlock(FVegBlockId Block, int32 BlockCells, double CellM)
	{
		TArray<FVegCellId> Out;
		for (int32 R = Block.Row * BlockCells; R < (Block.Row + 1) * BlockCells; ++R)
		{
			double La, Lo; CellCentre({ R, 0 }, CellM, La, Lo);
			if (!IsLatAllowed(La)) continue;
			const int32 Cols = NumCols(R, CellM);
			for (int32 C = Block.Col * BlockCells; C < (Block.Col + 1) * BlockCells; ++C)
				if (C >= 0 && C < Cols) Out.Add({ R, C });
		}
		return Out;
	}

	void BlockCentre(FVegBlockId Block, int32 BlockCells, double CellM, double& OutLatDeg, double& OutLonDeg)
	{
		// The mean of the corner cells' centres: column widths differ per row, so a block's rows are offset from each other
		// (tens of metres per row far from 180 W) and its middle row alone would misplace the centre.
		const int32 R0 = Block.Row * BlockCells, R1 = R0 + BlockCells - 1;
		const int32 C0 = Block.Col * BlockCells, C1 = C0 + BlockCells - 1;
		double Lat = 0.0, Lon = 0.0;
		for (const FVegCellId C : { FVegCellId{ R0, C0 }, FVegCellId{ R0, C1 }, FVegCellId{ R1, C0 }, FVegCellId{ R1, C1 } })
		{
			const FVegCellBounds B = CellBounds(C, CellM);   // unclamped column (CellCentre would clamp the east edge)
			Lat += 0.25 * 0.5 * (B.SouthDeg + B.NorthDeg);
			Lon += 0.25 * (-180.0 + (C.Col + 0.5) * CellDLonDeg(C.Row, CellM));
		}
		OutLatDeg = Lat;
		OutLonDeg = FMath::Min(Lon, 180.0);
	}

	TArray<FVegBlockId> BlocksWithin(double LatDeg, double LonDeg, double RadiusM, double CellM, int32 BlockCells)
	{
		TArray<FVegBlockId> Out;
		if (!IsLatAllowed(LatDeg) || !(RadiusM > 0.0) || !(CellM > 0.0) || BlockCells < 1) return Out;
		const double BlockM = CellM * BlockCells;
		const double DLatR = RadiusM / MetresPerDegLat;
		const int32 Row0 = FloorDiv(CellOf(LatDeg - DLatR, LonDeg, CellM).Row, BlockCells);
		const int32 Row1 = FloorDiv(CellOf(LatDeg + DLatR, LonDeg, CellM).Row, BlockCells);
		TSet<FVegBlockId> Seen;
		TArray<TPair<double, FVegBlockId>> Hits;
		for (int32 BR = Row0; BR <= Row1; ++BR)
		{
			double CLat, Unused; BlockCentre({ BR, 0 }, BlockCells, CellM, CLat, Unused);
			if (!IsLatAllowed(CLat)) continue;
			const int32 Row = MidRow({ BR, 0 }, BlockCells);
			const double DLon = CellDLonDeg(Row, CellM) * BlockCells;
			const int32 Cols = NumBlockCols(Row, CellM, BlockCells);
			const double SpanDeg = (RadiusM + BlockM) / (MetresPerDegLat * FMath::Max(FMath::Cos(FMath::DegreesToRadians(CLat)), 0.01));
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
