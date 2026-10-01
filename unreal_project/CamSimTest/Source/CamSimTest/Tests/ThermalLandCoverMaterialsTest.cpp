// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Thermal/ThermalMaterials.h"
#include "Thermal/ThermalModel.h"

// CamSim.Thermal.Materials.*: the land-cover classes (ROADMAP 4B) and their diurnal contrasts at San Francisco.

namespace
{
	FThermalSite SanFrancisco(int32 DayOfYear)
	{
		FThermalSite S;
		S.Year = 2026; S.DayOfYear = DayOfYear; S.LatDeg = 37.80; S.LonDeg = -122.45;
		S.TairMeanK = 288.15; S.AirSwingK = 8.0; S.Cloud = 0.0;
		return S;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalLandCoverBuiltInsTest, "CamSim.Thermal.Materials.LandCoverBuiltIns",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalLandCoverBuiltInsTest::RunTest(const FString& Parameters)
{
	const TArray<FThermalMaterial>& B = FThermalMaterialTable::BuiltIns();
	TestEqual(TEXT("14 built-ins"), B.Num(), 14);
	struct FExpect { int32 Index; const TCHAR* Name; };
	const FExpect Expected[] = {
		{ FThermalMaterialTable::TerrainDefault, TEXT("terrain_default") }, { FThermalMaterialTable::Water, TEXT("water") },
		{ FThermalMaterialTable::VehiclePaint, TEXT("vehicle_paint") }, { FThermalMaterialTable::Asphalt, TEXT("asphalt") },
		{ FThermalMaterialTable::Vegetation, TEXT("vegetation") }, { FThermalMaterialTable::Concrete, TEXT("concrete") },
		{ FThermalMaterialTable::TreeCanopy, TEXT("tree_canopy") }, { FThermalMaterialTable::Shrubland, TEXT("shrubland") },
		{ FThermalMaterialTable::Grassland, TEXT("grassland") }, { FThermalMaterialTable::Cropland, TEXT("cropland") },
		{ FThermalMaterialTable::BuiltUp, TEXT("built_up") }, { FThermalMaterialTable::BareSoil, TEXT("bare_soil") },
		{ FThermalMaterialTable::SnowIce, TEXT("snow_ice") }, { FThermalMaterialTable::Wetland, TEXT("wetland") },
	};
	for (const FExpect& E : Expected)
	{
		if (TestTrue(*FString::Printf(TEXT("index %d exists"), E.Index), E.Index < B.Num()))
		{
			TestEqual(*FString::Printf(TEXT("index %d"), E.Index), B[E.Index].Name, FString(E.Name));
		}
	}
	const FThermalMaterialTable T;
	TestEqual(TEXT("Find bare_soil"), T.Find(TEXT("bare_soil")), FThermalMaterialTable::BareSoil);
	EThermalTemperatureSource Src = EThermalTemperatureSource::Model;
	TestTrue(TEXT("snow parses"), FThermalMaterialTable::ParseSource(TEXT("Snow"), Src) && Src == EThermalTemperatureSource::Snow);
	TestTrue(TEXT("snow_ice uses it"), B[FThermalMaterialTable::SnowIce].Source == EThermalTemperatureSource::Snow);
	TestTrue(TEXT("vegetation family keeps its heat in the air: tree h_c > grass h_c"),
		B[FThermalMaterialTable::TreeCanopy].ConvectionWm2K > B[FThermalMaterialTable::Grassland].ConvectionWm2K);
	FThermalMaterialSpec Lava;
	Lava.Name = TEXT("asphalt");
	Lava.Temperature = TEXT("lava");
	const TArray<FString> Errors = FThermalMaterialTable::Validate({ Lava });
	TestTrue(TEXT("error lists the three sources"), Errors.Num() == 1 && Errors[0].Contains(TEXT("model, water or snow")));
	for (const FThermalMaterial& M : B)
	{
		FThermalMaterialSpec S;
		S.Name = M.Name; S.Albedo = M.Albedo; S.Emissivity = M.Emissivity; S.ThermalInertia = M.ThermalInertia;
		S.ConvectionWm2K = M.ConvectionWm2K; S.KFast = M.KFast;
		TestEqual(*FString::Printf(TEXT("%s is within the config ranges"), *M.Name), FThermalMaterialTable::Validate({ S }).Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalSnowTest, "CamSim.Thermal.Materials.SnowNeverAboveFreezing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalSnowTest::RunTest(const FString& Parameters)
{
	const FThermalMaterialTable T;
	FThermalModel M;
	M.Update(SanFrancisco(172), T);   // June: the model alone would be well above 0 C
	double Hi = -1e9, Lo = 1e9;
	for (int32 Min = 0; Min < 1440; Min += 5)
	{
		const double K = M.TemperatureK(FThermalMaterialTable::SnowIce, Min * 60.0, 288.15);
		Hi = FMath::Max(Hi, K);
		Lo = FMath::Min(Lo, K);
	}
	TestTrue(*FString::Printf(TEXT("snow max %.2f K <= 273.15 K"), Hi), Hi <= FThermalModel::SnowMaxK + 1e-9);
	TestTrue(TEXT("terrain_default at noon is above freezing (the cap matters)"),
		M.TemperatureK(FThermalMaterialTable::TerrainDefault, 12.0 * 3600.0, 288.15) > FThermalModel::SnowMaxK);
	FThermalSite Cold = SanFrancisco(15);
	Cold.TairMeanK = 255.0;
	M.Update(Cold, T);
	const double Night = M.TemperatureK(FThermalMaterialTable::SnowIce, 2.0 * 3600.0, 271.0);
	TestTrue(*FString::Printf(TEXT("cold night: snow follows the model below 0 C (%.2f K)"), Night), Night < FThermalModel::SnowMaxK - 1.0);
	return true;
}

// The spec's success criteria (a) and (b) at class level: noon vegetation cooler than built-up/bare, night built-up warmer
// than open vegetation, canopy warmer than grass at night. 21 Dec, Presidio, clear sky.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalLandCoverContrastTest, "CamSim.Thermal.Materials.DiurnalContrastsAtSanFrancisco",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalLandCoverContrastTest::RunTest(const FString& Parameters)
{
	using T = FThermalMaterialTable;
	const T Table;
	FThermalModel M;
	M.Update(SanFrancisco(355), Table);
	const double Noon = 12.0 * 3600.0, Night = 2.0 * 3600.0;
	auto K = [&M](int32 C, double Sec) { return M.TemperatureK(C, Sec, 288.15); };
	FString Line;
	for (int32 C = 0; C < Table.Num(); ++C) Line += FString::Printf(TEXT(" %s %.1f/%.1f"), *Table.Get(C).Name, K(C, Noon), K(C, Night));
	AddInfo(TEXT("noon/02:00 K:") + Line);
	TestTrue(TEXT("noon: asphalt warmer than tree canopy by >= 1 K"), K(T::Asphalt, Noon) - K(T::TreeCanopy, Noon) >= 1.0);
	TestTrue(TEXT("noon: bare soil warmer than tree canopy by >= 1 K"), K(T::BareSoil, Noon) - K(T::TreeCanopy, Noon) >= 1.0);
	TestTrue(TEXT("noon: built-up warmer than tree canopy by >= 1 K"), K(T::BuiltUp, Noon) - K(T::TreeCanopy, Noon) >= 1.0);
	TestTrue(TEXT("night: asphalt warmer than grassland by >= 1 K"), K(T::Asphalt, Night) - K(T::Grassland, Night) >= 1.0);
	TestTrue(TEXT("night: concrete warmer than grassland by >= 1 K"), K(T::Concrete, Night) - K(T::Grassland, Night) >= 1.0);
	TestTrue(TEXT("night: built-up warmer than grassland by >= 1 K"), K(T::BuiltUp, Night) - K(T::Grassland, Night) >= 1.0);
	TestTrue(TEXT("night: tree canopy warmer than grassland by >= 0.5 K"), K(T::TreeCanopy, Night) - K(T::Grassland, Night) >= 0.5);
	return true;
}
