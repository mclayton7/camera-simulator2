// Copyright CamSim Contributors. All Rights Reserved.

#include "Vegetation/VegetationScatter.h"

#include "SensorHash.h"

namespace CamSimVegetation
{
	namespace
	{
		constexpr uint32 SubJitterE = 2u * CamSimHash::VegetationStream + 0u;   // 160
		constexpr uint32 SubJitterN = 2u * CamSimHash::VegetationStream + 1u;
		constexpr uint32 SubAccept  = 2u * CamSimHash::VegetationStream + 2u;
		constexpr uint32 SubYaw     = 2u * CamSimHash::VegetationStream + 3u;
		constexpr uint32 SubScale   = 2u * CamSimHash::VegetationStream + 4u;   // 164
		constexpr uint32 SeedMix    = 0x56454745u;   // "VEGE"
	}

	int32 CandidatesPerAxis(const FScatterParams& P)
	{
		return FMath::Max(1, FMath::RoundToInt32(P.CellSizeM / FMath::Max(P.SpacingM, 0.5)));
	}

	double ExpectedTreesPerM2(const FScatterParams& P)
	{
		const double N = CandidatesPerAxis(P);
		return FMath::Clamp(double(P.DensityScale) * P.PTree, 0.0, 1.0) * N * N / (P.CellSizeM * P.CellSizeM);
	}

	uint32 VegetationSeed(uint32 BaseSeed) { return CamSimHash::Pcg(BaseSeed ^ SeedMix); }

	namespace
	{
		TArray<FTreeInstance> GenerateCellImpl(uint32 Seed, FVegCellId Cell, const FScatterParams& P, FCodeSampler Sampler,
			const FClearPredicate* IsClear)
		{
			TArray<FTreeInstance> Out;
			const float Accept = FMath::Clamp(P.DensityScale * P.PTree, 0.0f, 1.0f);
			if (Accept <= 0.0f) return Out;
			const FVegCellBounds B = CellBounds(Cell, P.CellSizeM);
			const int32 N = CandidatesPerAxis(P);
			const uint32 X = static_cast<uint32>(Cell.Col), Y = static_cast<uint32>(Cell.Row);
			for (int32 J = 0; J < N; ++J)
				for (int32 I = 0; I < N; ++I)
				{
					const uint32 K = static_cast<uint32>(J * N + I);
					if (CamSimHash::Uniform(CamSimHash::Hash(X, Y, K, Seed, SubAccept)) >= Accept) continue;
					const double Fx = (I + CamSimHash::Uniform(CamSimHash::Hash(X, Y, K, Seed, SubJitterE))) / N;
					const double Fy = (J + CamSimHash::Uniform(CamSimHash::Hash(X, Y, K, Seed, SubJitterN))) / N;
					FTreeInstance T;
					T.LatDeg = B.SouthDeg + Fy * (B.NorthDeg - B.SouthDeg);
					T.LonDeg = B.WestDeg + Fx * (B.EastDeg - B.WestDeg);
					if (Sampler(T.LatDeg, T.LonDeg) != P.TreeCode) continue;
					if (IsClear && !(*IsClear)(T.LatDeg, T.LonDeg)) continue;   // REALISM R3b: inside a footprint + clearance
					T.YawDeg = 360.0f * CamSimHash::Uniform(CamSimHash::Hash(X, Y, K, Seed, SubYaw));
					if (T.YawDeg >= 360.0f) T.YawDeg = 0.0f;
					T.Scale = P.ScaleMin + (P.ScaleMax - P.ScaleMin) * CamSimHash::Uniform(CamSimHash::Hash(X, Y, K, Seed, SubScale));
					Out.Add(T);
				}
			return Out;
		}
	}

	TArray<FTreeInstance> GenerateCell(uint32 Seed, FVegCellId Cell, const FScatterParams& P, FCodeSampler Sampler)
	{
		return GenerateCellImpl(Seed, Cell, P, Sampler, nullptr);
	}

	TArray<FTreeInstance> GenerateCell(uint32 Seed, FVegCellId Cell, const FScatterParams& P, FCodeSampler Sampler, FClearPredicate IsClear)
	{
		return GenerateCellImpl(Seed, Cell, P, Sampler, &IsClear);
	}

	uint32 HashInstances(const TArray<FTreeInstance>& Instances)
	{
		uint32 H = CamSimHash::Pcg(static_cast<uint32>(Instances.Num()));
		for (const FTreeInstance& T : Instances)
		{
			uint64 Lat, Lon; uint32 Yaw, Scale;
			FMemory::Memcpy(&Lat, &T.LatDeg, 8); FMemory::Memcpy(&Lon, &T.LonDeg, 8);
			FMemory::Memcpy(&Yaw, &T.YawDeg, 4); FMemory::Memcpy(&Scale, &T.Scale, 4);
			for (uint32 W : { uint32(Lat), uint32(Lat >> 32), uint32(Lon), uint32(Lon >> 32), Yaw, Scale, uint32(T.Type) })
				H = CamSimHash::Pcg(H ^ W);
		}
		return H;
	}
}
