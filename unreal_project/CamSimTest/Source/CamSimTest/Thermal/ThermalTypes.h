// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** Where a thermal class's temperature comes from (ROADMAP 4A). */
enum class EThermalTemperatureSource : uint8
{
	Model = 0,   // closed-form diurnal response (FThermalModel)
	Water = 1,   // the water temperature (CIGI Maritime Surface / ocean.water_temperature_c) +- 0.5 K diurnal
};

/** thermal.materials.<name>: overrides of one built-in class, or a new class (unset fields copy terrain_default). */
struct FThermalMaterialSpec
{
	FString Name;
	TOptional<float> Albedo;           // [0, 1)
	TOptional<float> Emissivity;       // (0, 1]
	TOptional<float> ThermalInertia;   // J m^-2 K^-1 s^-1/2, [0, 20000]
	TOptional<float> ConvectionWm2K;   // W m^-2 K^-1, (0, 200]
	TOptional<float> KFast;            // K per W m^-2, [0, 0.2]
	FString Temperature;               // "" (keep), "model", "water"

	bool operator==(const FThermalMaterialSpec&) const = default;
};
