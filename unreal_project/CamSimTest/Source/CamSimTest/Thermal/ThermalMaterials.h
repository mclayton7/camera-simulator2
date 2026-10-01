// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Thermal/ThermalTypes.h"

/** One thermal class (ROADMAP 4A): optical and thermal properties for FThermalModel and ThermalCS. */
struct FThermalMaterial
{
	FString Name;
	float Albedo         = 0.20f;    // solar
	float Emissivity     = 0.95f;    // in band and broadband
	float ThermalInertia = 1200.0f;  // J m^-2 K^-1 s^-1/2
	float ConvectionWm2K = 10.0f;    // h_c; h = h_c + 4 eps sigma T_air^3
	float KFast          = 0.015f;   // K per W m^-2 of absorbed flux above the class reference (per-pixel fast term)
	EThermalTemperatureSource Source = EThermalTemperatureSource::Model;
};

/**
 * Built-in classes plus thermal.materials overrides, name -> index (<= MaxClasses). Built-in indices are
 * fixed (constants below): terrain_default (every terrain pixel in 4A), water, vehicle_paint (entities by default), ...
 * Game thread.
 */
class CAMSIMTEST_API FThermalMaterialTable
{
public:
	static constexpr int32 MaxClasses     = 32;   // == FThermalFrameParams::MaxClasses (static_assert in ThermalFrameBuilder.cpp)
	// Built-in indices (order fixed). 0-5 are ROADMAP 4A's; 6-13 are the land-cover classes (ROADMAP 4B).
	static constexpr int32 TerrainDefault = 0;
	static constexpr int32 Water          = 1;
	static constexpr int32 VehiclePaint   = 2;
	static constexpr int32 Asphalt        = 3;
	static constexpr int32 Vegetation     = 4;
	static constexpr int32 Concrete       = 5;
	static constexpr int32 TreeCanopy     = 6;
	static constexpr int32 Shrubland      = 7;
	static constexpr int32 Grassland      = 8;
	static constexpr int32 Cropland       = 9;
	static constexpr int32 BuiltUp        = 10;
	static constexpr int32 BareSoil       = 11;
	static constexpr int32 SnowIce        = 12;
	static constexpr int32 Wetland        = 13;

	static const TArray<FThermalMaterial>& BuiltIns();
	/** "model" / "water" / "snow" (case-insensitive). */
	static bool ParseSource(const FString& Name, EThermalTemperatureSource& Out);
	/** Errors for thermal.materials as FCamSimConfig::Validate reports them (names, ranges, sources, class count). */
	static TArray<FString> Validate(const TArray<FThermalMaterialSpec>& Specs);

	FThermalMaterialTable();
	/** Built-ins, then each valid spec applied in order (a new name starts as a copy of terrain_default). Returns the skipped specs' errors. */
	TArray<FString> Build(const TArray<FThermalMaterialSpec>& Specs);
	int32 Find(const FString& Name) const;
	int32 Num() const { return Classes.Num(); }
	const FThermalMaterial& Get(int32 Index) const { return Classes[Index]; }
	uint32 GetVersion() const { return Version; }

private:
	TArray<FThermalMaterial> Classes;
	uint32 Version = 0;
};
