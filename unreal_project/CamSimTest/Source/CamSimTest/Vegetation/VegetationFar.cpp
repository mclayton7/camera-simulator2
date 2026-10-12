// Copyright CamSim Contributors. All Rights Reserved.

#include "Vegetation/VegetationFar.h"

#include "SensorHash.h"

namespace CamSimVegetation
{
	namespace
	{
		constexpr uint32 SubFarKeep   = 2u * CamSimHash::VegetationStream + 12u;   // 172
		constexpr uint32 SubFarBucket = 2u * CamSimHash::VegetationStream + 13u;   // 173
	}

	TArray<FFarTree> FarFilter(uint32 Seed, FVegCellId Cell, const TArray<FTreeInstance>& Trees, const FFarParams& P)
	{
		TArray<FFarTree> Out;
		const uint32 X = static_cast<uint32>(Cell.Col), Y = static_cast<uint32>(Cell.Row);
		const int32 Buckets = FMath::Max(P.Buckets, 1);
		for (int32 K = 0; K < Trees.Num(); ++K)
		{
			const FTreeInstance& T = Trees[K];
			if (!P.TypeHasFar.IsValidIndex(T.Type) || !P.TypeHasFar[T.Type]) continue;
			if (CamSimHash::Uniform(CamSimHash::Hash(X, Y, static_cast<uint32>(K), Seed, SubFarKeep)) >= P.Keep) continue;
			FFarTree F;
			F.Tree = T;
			F.Tree.Scale = T.Scale * P.Scale;
			F.Bucket = static_cast<uint8>(FMath::Min(Buckets - 1,
				FMath::FloorToInt32(CamSimHash::Uniform(CamSimHash::Hash(X, Y, static_cast<uint32>(K), Seed, SubFarBucket)) * Buckets)));
			Out.Add(F);
		}
		return Out;
	}

	uint32 HashFarTrees(const TArray<FFarTree>& Trees)
	{
		TArray<FTreeInstance> T;
		T.Reserve(Trees.Num());
		uint32 B = CamSimHash::Pcg(static_cast<uint32>(Trees.Num()));
		for (const FFarTree& F : Trees) { T.Add(F.Tree); B = CamSimHash::Pcg(B ^ F.Bucket); }
		return CamSimHash::Pcg(HashInstances(T) ^ B);
	}

	float BucketCullM(float FarM, int32 Bucket, int32 Buckets)
	{
		if (Buckets <= 1) return FarM;
		return FarM * (0.7f + 0.3f * static_cast<float>(Bucket) / static_cast<float>(Buckets - 1));
	}

	FFarHeightGrid FFarHeightGrid::ForBlock(FVegBlockId Block, int32 BlockCells, double CellM, double SpacingM)
	{
		double S = 90.0, N = -90.0, W = 360.0, E = -360.0;
		for (const FVegCellId& C : CellsOfBlock(Block, BlockCells, CellM))
		{
			const FVegCellBounds B = CellBounds(C, CellM);
			S = FMath::Min(S, B.SouthDeg); N = FMath::Max(N, B.NorthDeg);
			W = FMath::Min(W, B.WestDeg);  E = FMath::Max(E, B.EastDeg);
		}
		return ForBounds(S, N, W, E, SpacingM);
	}

	FFarHeightGrid FFarHeightGrid::ForBounds(double S, double N, double W, double E, double SpacingM)
	{
		FFarHeightGrid G;
		if (!(N > S && E > W)) return G;
		const double HeightM = (N - S) * MetresPerDegLat;
		const double WidthM = GroundDistanceM(0.5 * (S + N), W, 0.5 * (S + N), E);
		G.N = FMath::Max(2, FMath::RoundToInt32(FMath::Max(HeightM, WidthM) / FMath::Max(SpacingM, 1.0)) + 1);
		G.SouthDeg = S; G.WestDeg = W;
		G.DLatDeg = (N - S) / (G.N - 1);
		G.DLonDeg = (E - W) / (G.N - 1);
		return G;
	}

	TArray<FVector> FFarHeightGrid::LonLatPoints() const
	{
		TArray<FVector> Out;
		Out.Reserve(N * N);
		for (int32 J = 0; J < N; ++J)
			for (int32 I = 0; I < N; ++I) Out.Add(FVector(WestDeg + I * DLonDeg, SouthDeg + J * DLatDeg, 0.0));
		return Out;
	}

	TOptional<double> FFarHeightGrid::Sample(double LatDeg, double LonDeg) const
	{
		if (N < 2 || H.Num() != N * N || !(DLatDeg > 0.0) || !(DLonDeg > 0.0)) return {};
		const double Fy = (LatDeg - SouthDeg) / DLatDeg, Fx = (LonDeg - WestDeg) / DLonDeg;
		if (!(Fx >= 0.0 && Fy >= 0.0 && Fx <= N - 1 && Fy <= N - 1)) return {};
		const int32 I = FMath::Min(FMath::FloorToInt32(Fx), N - 2), J = FMath::Min(FMath::FloorToInt32(Fy), N - 2);
		const TOptional<double>& A = H[J * N + I];
		const TOptional<double>& B = H[J * N + I + 1];
		const TOptional<double>& C = H[(J + 1) * N + I];
		const TOptional<double>& D = H[(J + 1) * N + I + 1];
		if (!A.IsSet() || !B.IsSet() || !C.IsSet() || !D.IsSet()) return {};
		const double U = Fx - I, V = Fy - J;
		return (1.0 - V) * ((1.0 - U) * *A + U * *B) + V * ((1.0 - U) * *C + U * *D);
	}

	bool FRetryingTraceSampler::TraceMissing(FBatch& B) const
	{
		bool bAll = true;
		for (int32 K = 0; K < B.Points.Num(); ++K)
		{
			if (B.H[K].IsSet()) continue;
			const TOptional<double> T = Trace(B.Points[K].Y, B.Points[K].X);
			if (T.IsSet() && FMath::IsFinite(*T)) B.H[K] = T;
			else bAll = false;
		}
		return bAll;
	}

	void FRetryingTraceSampler::Sample(const TArray<FVector>& LonLatH, TFunction<void(TArray<TOptional<double>>)> Done)
	{
		FBatch B;
		B.Points = LonLatH;
		B.H.SetNum(LonLatH.Num());
		B.Done = MoveTemp(Done);
		B.TicksLeft = RetryTicks;
		if (TraceMissing(B) || MaxRetries <= 0) { B.Done(MoveTemp(B.H)); return; }
		Pending.Add(MoveTemp(B));
	}

	void FRetryingTraceSampler::Tick()
	{
		TArray<FBatch> Finished;
		for (int32 I = Pending.Num() - 1; I >= 0; --I)
		{
			FBatch& B = Pending[I];
			if (--B.TicksLeft > 0) continue;
			B.TicksLeft = RetryTicks;
			const bool bAll = TraceMissing(B);
			if (bAll || ++B.Retries >= MaxRetries)
			{
				Finished.Add(MoveTemp(B));
				Pending.RemoveAt(I);
			}
		}
		// After the loop: a Done may start a new batch.
		for (FBatch& B : Finished) B.Done(MoveTemp(B.H));
	}
}
