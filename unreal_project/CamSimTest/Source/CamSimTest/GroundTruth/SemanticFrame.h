// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/StaticArray.h"
#include "SemanticFrameParams.h"

struct FLandCoverWindowData;

/** What one frame's semantic parameters are built from (game thread, ROADMAP 4D). */
struct FSemanticBuildInputs
{
	const FLandCoverWindowData* Window = nullptr;   // null or empty: no land cover (terrain unlabeled)
	FVector EastWorld = FVector::ZeroVector;        // window-centre East/North (CamSimThermal::LandCoverAxesWorld)
	FVector NorthWorld = FVector::ZeroVector;
	FVector UpWorld = FVector::UpVector;            // geodetic up at the camera
	double CamLatDeg = 0.0, CamLonDeg = 0.0, CamAltHaeM = 0.0;
	TOptional<double> SeaLevelHaeM;                 // unset: no ocean
	double MaxWaveAmplitudeM = 0.0;
	TStaticArray<uint8, 256> StencilClass = TStaticArray<uint8, 256>(InPlace, static_cast<uint8>(0));
};

namespace CamSimSemantic
{
	/** Window geometry as ThermalCS's (no warp), sea band as ThermalCS's, the code LUT and the stencil table. */
	CAMSIMTEST_API FSemanticFrameParams BuildFrameParams(const FSemanticBuildInputs& In);
}
