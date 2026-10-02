// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Thermal/ThermalTypes.h"

class FThermalMaterialTable;

/** Base-colour refinement family of a WorldCover code (ROADMAP 4B). Values are FThermalFrameParams::LandCoverFamily* and the
 *  shader's LANDCOVER_FAMILY_* (static_assert in ThermalReference.cpp). */
enum class ELandCoverFamily : uint8
{
	None       = 0,   // 0 no data, 70 snow and ice, 80 water: no refinement
	Vegetation = 1,   // 10 tree, 20 shrub, 30 grass, 40 crop, 90/95 wetland/mangrove, 100 moss: blend toward bare_soil by (1 - v)
	BuiltUp    = 2,   // 50: blend toward vegetation by v; the rest splits asphalt / concrete by base luminance
	Bare       = 3,   // 60: blend toward vegetation by v
};

namespace CamSimLandCover
{
	/** The default thermal material of a WorldCover code (spec table); terrain_default for 0 and unknown codes. */
	CAMSIMTEST_API const TCHAR* DefaultMaterialName(uint8 Code);
	CAMSIMTEST_API ELandCoverFamily FamilyForCode(uint8 Code);
	/** Errors of thermal.land_cover.classes as FCamSimConfig::Validate reports them (codes, names, duplicates). */
	CAMSIMTEST_API TArray<FString> ValidateClassSpecs(const TArray<FLandCoverClassSpec>& Specs);
}

/** WorldCover code -> thermal class index and refinement family: defaults plus thermal.land_cover.classes. Game thread. */
struct CAMSIMTEST_API FLandCoverClassTable
{
	uint8 Class[256]  = {};
	uint8 Family[256] = {};

	/** Rebuilds both tables. A spec naming a material the table doesn't have keeps the default and returns a warning. */
	TArray<FString> Build(const TArray<FLandCoverClassSpec>& Specs, const FThermalMaterialTable& Materials);
};
