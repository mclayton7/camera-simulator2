// Copyright CamSim Contributors. All Rights Reserved.

#include "Vegetation/VegetationMix.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Vegetation/VegetationLibrary.h"

namespace CamSimVegetation
{
	namespace
	{
		bool ParseCode(const FString& Key, uint8& Out)
		{
			const int32 C = FCString::Atoi(*Key);
			if (C < 1 || C > 255 || FString::FromInt(C) != Key) return false;
			Out = static_cast<uint8>(C);
			return true;
		}

		/** (type, weight) pairs -> cumulative entries; the last forced to exactly 1. */
		TArray<FMixEntry> Cumulative(const TArray<TPair<int32, double>>& Items)
		{
			TArray<FMixEntry> Out;
			double Total = 0.0;
			for (const auto& It : Items) Total += It.Value;
			double Sum = 0.0;
			for (const auto& It : Items)
			{
				Sum += It.Value;
				Out.Add({ It.Key, static_cast<float>(Sum / Total) });
			}
			if (Out.Num() > 0) Out.Last().CumWeight = 1.0f;
			return Out;
		}

		void FillTypeData(const FVegetationLibrary& Lib, FVegetationMix& M)
		{
			for (const FVegetationType& T : Lib.Types)
			{
				M.TypeHeightM.Add(T.HeightM);
				M.TypeCrownRadiusM.Add(T.CrownRadiusM);
			}
		}
	}

	int32 PickType(const TArray<FMixEntry>& Entries, float U)
	{
		for (int32 I = 0; I < Entries.Num(); ++I) if (U < Entries[I].CumWeight) return I;
		return Entries.Num() - 1;
	}

	bool ParseMix(const FString& Json, const FVegetationLibrary& Lib, FVegetationMix& Out, TArray<FString>& OutUnknown, FString& OutError)
	{
		Out = FVegetationMix();
		OutUnknown.Reset();
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
		{
			OutError = TEXT("mix.json is not valid JSON"); return false;
		}
		FString Format;
		if (!Root->TryGetStringField(TEXT("format"), Format) || Format != TEXT("camsim-vegmix-1"))
		{
			OutError = FString::Printf(TEXT("mix.json format '%s' is not camsim-vegmix-1"), *Format); return false;
		}
		TMap<uint8, float> Density;
		const TSharedPtr<FJsonObject>* Dens = nullptr;
		if (Root->TryGetObjectField(TEXT("shrub_density"), Dens))
		{
			for (const auto& Kv : (*Dens)->Values)
			{
				const FString Key(*Kv.Key);
				uint8 C = 0; double N = 0.0;
				if (!ParseCode(Key, C)) { OutError = FString::Printf(TEXT("shrub_density: code '%s' is not 1..255"), *Key); return false; }
				if (!Kv.Value->TryGetNumber(N) || !(N >= 0.0 && N <= 100.0))
				{
					OutError = FString::Printf(TEXT("shrub_density.%s must be a number in [0, 100]"), *Key); return false;
				}
				Density.Add(C, static_cast<float>(N));
			}
		}
		const TSharedPtr<FJsonObject>* MixObj = nullptr;
		if (!Root->TryGetObjectField(TEXT("mix"), MixObj)) { OutError = TEXT("mix.json has no mix object"); return false; }
		FillTypeData(Lib, Out);
		for (const auto& Kv : (*MixObj)->Values)
		{
			const FString Key(*Kv.Key);
			uint8 C = 0;
			if (!ParseCode(Key, C)) { OutError = FString::Printf(TEXT("mix: code '%s' is not 1..255"), *Key); return false; }
			const TSharedPtr<FJsonObject>* Names = nullptr;
			if (!Kv.Value->TryGetObject(Names)) { OutError = FString::Printf(TEXT("mix.%s must be an object"), *Key); return false; }
			TArray<TPair<FString, TSharedPtr<FJsonValue>>> Sorted;
			for (const auto& Nv : (*Names)->Values) Sorted.Emplace(FString(*Nv.Key), Nv.Value);
			// The JSON object's key order is not kept; sorting makes the cumulative weights independent of it.
			Sorted.Sort([](const auto& A, const auto& B) { return A.Key < B.Key; });
			TArray<TPair<int32, double>> Trees, Shrubs;
			for (const auto& Nv : Sorted)
			{
				const FString& Name = Nv.Key;
				double W = 0.0;
				if (!Nv.Value.IsValid() || !Nv.Value->TryGetNumber(W) || !(W > 0.0) || !FMath::IsFinite(W))
				{
					OutError = FString::Printf(TEXT("mix.%s.%s: weight must be > 0"), *Key, *Name); return false;
				}
				const int32 Idx = FindType(Lib, Name);
				if (Idx == INDEX_NONE) { OutUnknown.AddUnique(Name); continue; }
				(Lib.Types[Idx].Role == EVegRole::Shrub ? Shrubs : Trees).Emplace(Idx, W);
			}
			if (Trees.Num() == 0 && Shrubs.Num() == 0) continue;
			FCodeMix M;
			M.Trees = Cumulative(Trees);
			M.Shrubs = Cumulative(Shrubs);
			if (Shrubs.Num() > 0) M.ShrubsPer100M2 = Density.Contains(C) ? Density[C] : DefaultShrubsPer100M2;
			Out.Codes.Add(C, MoveTemp(M));
		}
		return true;
	}

	FVegetationMix DefaultMix(const FVegetationLibrary& Lib)
	{
		FVegetationMix M;
		FillTypeData(Lib, M);
		if (Lib.Types.Num() > 0 && Lib.Types[0].Role == EVegRole::Tree)
		{
			FCodeMix C10;
			C10.Trees.Add({ 0, 1.0f });
			M.Codes.Add(10, C10);
		}
		const int32 Shrub = FindType(Lib, TEXT("shrub_chaparral"));
		if (Shrub != INDEX_NONE && Lib.Types[Shrub].Role == EVegRole::Shrub)
		{
			for (const TPair<uint8, float>& CD : { TPair<uint8, float>(20, 6.0f), TPair<uint8, float>(30, 1.0f) })
			{
				FCodeMix C;
				C.Shrubs.Add({ Shrub, 1.0f });
				C.ShrubsPer100M2 = CD.Value;
				M.Codes.Add(CD.Key, C);
			}
		}
		return M;
	}

	FVegetationMix SingleTypeMix(uint8 Code, int32 Type, float HeightM, float CrownRadiusM)
	{
		FVegetationMix M;
		M.TypeHeightM.SetNumZeroed(Type + 1);
		M.TypeCrownRadiusM.SetNumZeroed(Type + 1);
		M.TypeHeightM[Type] = HeightM;
		M.TypeCrownRadiusM[Type] = CrownRadiusM;
		FCodeMix C;
		C.Trees.Add({ Type, 1.0f });
		M.Codes.Add(Code, C);
		return M;
	}
}
