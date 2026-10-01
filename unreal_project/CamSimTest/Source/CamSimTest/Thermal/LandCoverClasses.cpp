// Copyright CamSim Contributors. All Rights Reserved.

#include "Thermal/LandCoverClasses.h"
#include "Thermal/ThermalMaterials.h"

namespace CamSimLandCover
{
	const TCHAR* DefaultMaterialName(uint8 Code)
	{
		switch (Code)
		{
		case 10:  return TEXT("tree_canopy");
		case 20:  return TEXT("shrubland");
		case 30:  return TEXT("grassland");
		case 40:  return TEXT("cropland");
		case 50:  return TEXT("built_up");
		case 60:  return TEXT("bare_soil");
		case 70:  return TEXT("snow_ice");
		case 80:  return TEXT("water");
		case 90:  return TEXT("wetland");
		case 95:  return TEXT("wetland");
		case 100: return TEXT("grassland");
		default:  return TEXT("terrain_default");
		}
	}

	ELandCoverFamily FamilyForCode(uint8 Code)
	{
		switch (Code)
		{
		case 10: case 20: case 30: case 40: case 90: case 95: case 100: return ELandCoverFamily::Vegetation;
		case 50: return ELandCoverFamily::BuiltUp;
		case 60: return ELandCoverFamily::Bare;
		default: return ELandCoverFamily::None;
		}
	}

	TArray<FString> ValidateClassSpecs(const TArray<FLandCoverClassSpec>& Specs)
	{
		TArray<FString> Errors;
		TSet<int32> Seen;
		for (const FLandCoverClassSpec& S : Specs)
		{
			if (S.Code < 0 || S.Code > 255)
			{
				Errors.Add(FString::Printf(TEXT("thermal.land_cover.classes: key '%s' is not a WorldCover code 0..255"), *S.Key));
				continue;
			}
			bool bName = !S.Material.IsEmpty();
			for (const TCHAR C : S.Material)
			{
				bName &= (C >= TEXT('a') && C <= TEXT('z')) || (C >= TEXT('0') && C <= TEXT('9')) || C == TEXT('_');
			}
			if (!bName)
			{
				Errors.Add(FString::Printf(TEXT("thermal.land_cover.classes.%d: material '%s' must be lower-case letters, digits or '_'"), S.Code, *S.Material));
			}
			if (Seen.Contains(S.Code))
			{
				Errors.Add(FString::Printf(TEXT("thermal.land_cover.classes.%d is listed twice"), S.Code));
			}
			Seen.Add(S.Code);
		}
		return Errors;
	}
}

TArray<FString> FLandCoverClassTable::Build(const TArray<FLandCoverClassSpec>& Specs, const FThermalMaterialTable& Materials)
{
	TArray<FString> Warnings;
	for (int32 C = 0; C < 256; ++C)
	{
		const int32 I = Materials.Find(CamSimLandCover::DefaultMaterialName(static_cast<uint8>(C)));
		Class[C]  = static_cast<uint8>(I == INDEX_NONE ? FThermalMaterialTable::TerrainDefault : I);
		Family[C] = static_cast<uint8>(CamSimLandCover::FamilyForCode(static_cast<uint8>(C)));
	}
	for (const FLandCoverClassSpec& S : Specs)
	{
		if (S.Code < 0 || S.Code > 255) continue;   // reported by FCamSimConfig::Validate
		const int32 I = Materials.Find(S.Material);
		if (I == INDEX_NONE)
		{
			Warnings.Add(FString::Printf(TEXT("thermal.land_cover.classes.%d: '%s' is not a thermal class; keeping %s"),
				S.Code, *S.Material, CamSimLandCover::DefaultMaterialName(static_cast<uint8>(S.Code))));
			continue;
		}
		Class[S.Code] = static_cast<uint8>(I);
	}
	return Warnings;
}
