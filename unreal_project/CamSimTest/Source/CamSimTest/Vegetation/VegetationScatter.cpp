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
		constexpr uint32 SubSpecies = 2u * CamSimHash::VegetationStream + 5u;   // 165 (REALISM R4 scatter v2)
		constexpr uint32 SubShrubE       = 2u * CamSimHash::VegetationStream + 6u;    // 166
		constexpr uint32 SubShrubN       = 2u * CamSimHash::VegetationStream + 7u;
		constexpr uint32 SubShrubAccept  = 2u * CamSimHash::VegetationStream + 8u;
		constexpr uint32 SubShrubYaw     = 2u * CamSimHash::VegetationStream + 9u;
		constexpr uint32 SubShrubScale   = 2u * CamSimHash::VegetationStream + 10u;
		constexpr uint32 SubShrubSpecies = 2u * CamSimHash::VegetationStream + 11u;   // 171
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

	int32 ShrubCandidatesPerAxis(const FScatterParams& P)
	{
		return FMath::Max(1, FMath::RoundToInt32(P.CellSizeM / FMath::Max(double(P.ShrubSpacingM), 0.5)));
	}

	float CanopyAccept(const FScatterParams& P, float CoverFrac, float HeightM, float TypeHeightM, float CrownRadiusM, float& OutScaleBase)
	{
		OutScaleBase = FMath::Clamp(HeightM / FMath::Max(TypeHeightM, 0.1f), 0.5f, 2.0f);
		if (!(HeightM >= P.MinCanopyHeightM) || !(CoverFrac > 0.0f)) return 0.0f;
		const double N = CandidatesPerAxis(P);
		const double ACand = FMath::Square(P.CellSizeM / N);
		const double R = FMath::Max(double(CrownRadiusM), 0.1) * OutScaleBase;
		return static_cast<float>(FMath::Clamp(double(FMath::Min(CoverFrac, 1.0f)) * ACand / (PI * R * R) * P.DensityScale, 0.0, 1.0));
	}

	FCellContent GenerateCellContent(uint32 Seed, FVegCellId Cell, const FScatterParams& P, const FVegetationMix& Mix,
		FPointSampler Sample, const FClearPredicate* IsClear)
	{
		FCellContent Out;
		const FVegCellBounds B = CellBounds(Cell, P.CellSizeM);
		const uint32 X = static_cast<uint32>(Cell.Col), Y = static_cast<uint32>(Cell.Row);
		auto Uniform = [&](uint32 K, uint32 Sub) { return CamSimHash::Uniform(CamSimHash::Hash(X, Y, K, Seed, Sub)); };

		// Trees: R4's grid and expressions, so a no-canopy one-type mix reproduces R4's instances bit for bit.
		const float R4Accept = FMath::Clamp(P.DensityScale * P.PTree, 0.0f, 1.0f);
		const int32 N = CandidatesPerAxis(P);
		for (int32 J = 0; J < N; ++J)
			for (int32 I = 0; I < N; ++I)
			{
				const uint32 K = static_cast<uint32>(J * N + I);
				const double Fx = (I + Uniform(K, SubJitterE)) / N;
				const double Fy = (J + Uniform(K, SubJitterN)) / N;
				FTreeInstance T;
				T.LatDeg = B.SouthDeg + Fy * (B.NorthDeg - B.SouthDeg);
				T.LonDeg = B.WestDeg + Fx * (B.EastDeg - B.WestDeg);
				const FVegPoint Pt = Sample(T.LatDeg, T.LonDeg);
				const FCodeMix* CM = Mix.Find(Pt.Code);
				if (!CM || CM->Trees.Num() == 0) continue;
				const int32 Type = CM->Trees[PickType(CM->Trees, Uniform(K, SubSpecies))].Type;
				float ScaleBase = 1.0f;
				const float Accept = Pt.bHasCanopy && Mix.TypeHeightM.IsValidIndex(Type)
					? CanopyAccept(P, Pt.CoverFrac, Pt.HeightM, Mix.TypeHeightM[Type], Mix.TypeCrownRadiusM[Type], ScaleBase)
					: R4Accept;
				if (Accept <= 0.0f || Uniform(K, SubAccept) >= Accept) continue;
				if (IsClear && !(*IsClear)(T.LatDeg, T.LonDeg)) continue;   // REALISM R3b: inside a footprint + clearance
				T.YawDeg = 360.0f * Uniform(K, SubYaw);
				if (T.YawDeg >= 360.0f) T.YawDeg = 0.0f;
				const float U = Uniform(K, SubScale);
				T.Scale = Pt.bHasCanopy ? ScaleBase * (0.9f + 0.2f * U) : P.ScaleMin + (P.ScaleMax - P.ScaleMin) * U;
				T.Type = static_cast<uint8>(Type);
				Out.Trees.Add(T);
			}

		if (!P.bShrubs) return Out;
		const int32 NS = ShrubCandidatesPerAxis(P);
		const double ACandS = FMath::Square(P.CellSizeM / NS);
		for (int32 J = 0; J < NS; ++J)
			for (int32 I = 0; I < NS; ++I)
			{
				const uint32 K = static_cast<uint32>(J * NS + I);
				const double Fx = (I + Uniform(K, SubShrubE)) / NS;
				const double Fy = (J + Uniform(K, SubShrubN)) / NS;
				FTreeInstance S;
				S.LatDeg = B.SouthDeg + Fy * (B.NorthDeg - B.SouthDeg);
				S.LonDeg = B.WestDeg + Fx * (B.EastDeg - B.WestDeg);
				const FCodeMix* CM = Mix.Find(Sample(S.LatDeg, S.LonDeg).Code);
				if (!CM || CM->Shrubs.Num() == 0) continue;
				const double Accept = FMath::Clamp(double(CM->ShrubsPer100M2) / 100.0 * ACandS * P.DensityScale, 0.0, 1.0);
				if (Uniform(K, SubShrubAccept) >= Accept) continue;
				if (IsClear && !(*IsClear)(S.LatDeg, S.LonDeg)) continue;
				S.Type = static_cast<uint8>(CM->Shrubs[PickType(CM->Shrubs, Uniform(K, SubShrubSpecies))].Type);
				S.YawDeg = 360.0f * Uniform(K, SubShrubYaw);
				if (S.YawDeg >= 360.0f) S.YawDeg = 0.0f;
				S.Scale = P.ShrubScaleMin + (P.ShrubScaleMax - P.ShrubScaleMin) * Uniform(K, SubShrubScale);
				Out.Shrubs.Add(S);
			}
		return Out;
	}

	namespace
	{
		FScatterParams NoShrubs(const FScatterParams& P) { FScatterParams Q = P; Q.bShrubs = false; return Q; }
		/** The R4 slice: one tree type, library index 0 (9 m, crown 2.7 m; neither matters without canopy) on TreeCode. */
		FVegetationMix R4Mix(uint8 Code) { return SingleTypeMix(Code, 0, 9.0f, 2.7f); }
	}

	TArray<FTreeInstance> GenerateCell(uint32 Seed, FVegCellId Cell, const FScatterParams& P, FCodeSampler Sampler)
	{
		return GenerateCellContent(Seed, Cell, NoShrubs(P), R4Mix(P.TreeCode),
			[&Sampler](double La, double Lo) { FVegPoint V; V.Code = Sampler(La, Lo); return V; }).Trees;
	}

	TArray<FTreeInstance> GenerateCell(uint32 Seed, FVegCellId Cell, const FScatterParams& P, FCodeSampler Sampler, FClearPredicate IsClear)
	{
		return GenerateCellContent(Seed, Cell, NoShrubs(P), R4Mix(P.TreeCode),
			[&Sampler](double La, double Lo) { FVegPoint V; V.Code = Sampler(La, Lo); return V; }, &IsClear).Trees;
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
