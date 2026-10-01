// Copyright CamSim Contributors. All Rights Reserved.

#include "Thermal/ThermalMaterials.h"

namespace
{
	uint32 GNextMaterialsVersion = 1;   // game thread

	FThermalMaterial MakeMaterial(const TCHAR* Name, float Albedo, float Emissivity, float Inertia, float Convection, float KFast,
		EThermalTemperatureSource Source = EThermalTemperatureSource::Model)
	{
		FThermalMaterial M;
		M.Name = Name; M.Albedo = Albedo; M.Emissivity = Emissivity; M.ThermalInertia = Inertia;
		M.ConvectionWm2K = Convection; M.KFast = KFast; M.Source = Source;
		return M;
	}

	bool IsValidName(const FString& N)
	{
		if (N.IsEmpty()) return false;
		for (const TCHAR C : N)
		{
			if (!((C >= TEXT('a') && C <= TEXT('z')) || (C >= TEXT('0') && C <= TEXT('9')) || C == TEXT('_'))) return false;
		}
		return true;
	}

	/** Range errors of one spec. Written !(x in range) so NaN is reported too. */
	TArray<FString> ValidateOne(const FThermalMaterialSpec& S)
	{
		TArray<FString> E;
		const FString P = FString::Printf(TEXT("thermal.materials.%s"), *S.Name);
		if (!IsValidName(S.Name))
			E.Add(FString::Printf(TEXT("thermal.materials: name '%s' must be lower-case letters, digits or '_'"), *S.Name));
		if (S.Albedo.IsSet() && !(*S.Albedo >= 0.0f && *S.Albedo < 1.0f))
			E.Add(FString::Printf(TEXT("%s.albedo=%.3f out of range [0, 1)"), *P, *S.Albedo));
		if (S.Emissivity.IsSet() && !(*S.Emissivity > 0.0f && *S.Emissivity <= 1.0f))
			E.Add(FString::Printf(TEXT("%s.emissivity=%.3f out of range (0, 1]"), *P, *S.Emissivity));
		if (S.ThermalInertia.IsSet() && !(*S.ThermalInertia >= 0.0f && *S.ThermalInertia <= 20000.0f))
			E.Add(FString::Printf(TEXT("%s.thermal_inertia=%.1f out of range [0, 20000]"), *P, *S.ThermalInertia));
		if (S.ConvectionWm2K.IsSet() && !(*S.ConvectionWm2K > 0.0f && *S.ConvectionWm2K <= 200.0f))
			E.Add(FString::Printf(TEXT("%s.convection_w_m2k=%.2f out of range (0, 200]"), *P, *S.ConvectionWm2K));
		if (S.KFast.IsSet() && !(*S.KFast >= 0.0f && *S.KFast <= 0.2f))
			E.Add(FString::Printf(TEXT("%s.k_fast=%.4f out of range [0, 0.2]"), *P, *S.KFast));
		EThermalTemperatureSource Unused;
		if (!S.Temperature.IsEmpty() && !FThermalMaterialTable::ParseSource(S.Temperature, Unused))
			E.Add(FString::Printf(TEXT("%s.temperature '%s' must be model, water or snow"), *P, *S.Temperature));
		return E;
	}

	bool IsBuiltIn(const FString& Name)
	{
		return FThermalMaterialTable::BuiltIns().ContainsByPredicate([&Name](const FThermalMaterial& M) { return M.Name == Name; });
	}
}

const TArray<FThermalMaterial>& FThermalMaterialTable::BuiltIns()
{
	// k_fast per the 4A spec (asphalt 0.02, vegetation 0.008, metal paint 0.04). Index order is fixed (see the header).
	// ROADMAP 4B land-cover classes: vegetation albedos are effective values that fold evapotranspiration into the absorbed
	// solar (the model has no latent term); a high h_c keeps canopies near air temperature (cool at noon, warm at night).
	static const TArray<FThermalMaterial> B = {
		MakeMaterial(TEXT("terrain_default"), 0.20f, 0.95f, 1200.0f, 10.0f, 0.015f),
		MakeMaterial(TEXT("water"),           0.06f, 0.98f,    0.0f, 10.0f, 0.0f, EThermalTemperatureSource::Water),
		MakeMaterial(TEXT("vehicle_paint"),   0.30f, 0.90f,  600.0f, 12.0f, 0.04f),
		MakeMaterial(TEXT("asphalt"),         0.10f, 0.95f, 1500.0f, 10.0f, 0.02f),
		MakeMaterial(TEXT("vegetation"),      0.20f, 0.98f,  300.0f, 15.0f, 0.008f),
		MakeMaterial(TEXT("concrete"),        0.35f, 0.92f, 1800.0f, 10.0f, 0.015f),
		MakeMaterial(TEXT("tree_canopy"),     0.40f, 0.98f,  800.0f, 25.0f, 0.004f),
		MakeMaterial(TEXT("shrubland"),       0.30f, 0.97f,  600.0f, 15.0f, 0.008f),
		MakeMaterial(TEXT("grassland"),       0.30f, 0.97f,  300.0f,  8.0f, 0.010f),
		MakeMaterial(TEXT("cropland"),        0.30f, 0.97f,  700.0f, 12.0f, 0.010f),
		MakeMaterial(TEXT("built_up"),        0.20f, 0.93f, 1650.0f, 10.0f, 0.018f),
		MakeMaterial(TEXT("bare_soil"),       0.25f, 0.93f,  900.0f, 10.0f, 0.025f),
		MakeMaterial(TEXT("snow_ice"),        0.75f, 0.99f,  600.0f, 10.0f, 0.005f, EThermalTemperatureSource::Snow),
		MakeMaterial(TEXT("wetland"),         0.40f, 0.98f, 2500.0f, 15.0f, 0.004f),
	};
	return B;
}

bool FThermalMaterialTable::ParseSource(const FString& Name, EThermalTemperatureSource& Out)
{
	if (Name.Equals(TEXT("model"), ESearchCase::IgnoreCase)) { Out = EThermalTemperatureSource::Model; return true; }
	if (Name.Equals(TEXT("water"), ESearchCase::IgnoreCase)) { Out = EThermalTemperatureSource::Water; return true; }
	if (Name.Equals(TEXT("snow"),  ESearchCase::IgnoreCase)) { Out = EThermalTemperatureSource::Snow;  return true; }
	return false;
}

TArray<FString> FThermalMaterialTable::Validate(const TArray<FThermalMaterialSpec>& Specs)
{
	TArray<FString> Errors;
	TSet<FString> Seen;
	int32 NewNames = 0;
	for (const FThermalMaterialSpec& S : Specs)
	{
		Errors.Append(ValidateOne(S));
		if (Seen.Contains(S.Name))
		{
			Errors.Add(FString::Printf(TEXT("thermal.materials.%s is listed twice"), *S.Name));
		}
		else
		{
			Seen.Add(S.Name);
			NewNames += IsBuiltIn(S.Name) ? 0 : 1;
		}
	}
	if (BuiltIns().Num() + NewNames > MaxClasses)
	{
		Errors.Add(FString::Printf(TEXT("thermal.materials: %d built-in + %d new classes exceed %d"), BuiltIns().Num(), NewNames, MaxClasses));
	}
	return Errors;
}

FThermalMaterialTable::FThermalMaterialTable()
	: Classes(BuiltIns())
	, Version(GNextMaterialsVersion++)
{
}

TArray<FString> FThermalMaterialTable::Build(const TArray<FThermalMaterialSpec>& Specs)
{
	TArray<FString> Errors;
	Classes = BuiltIns();
	for (const FThermalMaterialSpec& S : Specs)
	{
		const TArray<FString> E = ValidateOne(S);
		if (E.Num() > 0)
		{
			Errors.Append(E);
			continue;
		}
		int32 Index = Find(S.Name);
		if (Index == INDEX_NONE)
		{
			if (Classes.Num() >= MaxClasses)
			{
				Errors.Add(FString::Printf(TEXT("thermal.materials.%s skipped: more than %d classes"), *S.Name, MaxClasses));
				continue;
			}
			FThermalMaterial M = Classes[TerrainDefault];
			M.Name = S.Name;
			Index = Classes.Add(M);
		}
		FThermalMaterial& M = Classes[Index];
		if (S.Albedo.IsSet())         M.Albedo = *S.Albedo;
		if (S.Emissivity.IsSet())     M.Emissivity = *S.Emissivity;
		if (S.ThermalInertia.IsSet()) M.ThermalInertia = *S.ThermalInertia;
		if (S.ConvectionWm2K.IsSet()) M.ConvectionWm2K = *S.ConvectionWm2K;
		if (S.KFast.IsSet())          M.KFast = *S.KFast;
		if (!S.Temperature.IsEmpty()) ParseSource(S.Temperature, M.Source);
	}
	Version = GNextMaterialsVersion++;
	return Errors;
}

int32 FThermalMaterialTable::Find(const FString& Name) const
{
	for (int32 I = 0; I < Classes.Num(); ++I)
	{
		if (Classes[I].Name == Name) return I;
	}
	return INDEX_NONE;
}
