// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Per-frame inputs of InstanceIdCS's WRITE_SEMANTIC output (ROADMAP 4D, docs/ground-truth.md). Built on the game thread
 * by CamSimSemantic::BuildFrameParams; CamSimSemanticRef::ClassifyTexel is the CPU mirror of the shader.
 */
struct FSemanticFrameParams
{
	// Land-cover window (the thermal IR window, also built in EO when semantic output is possible)
	uint32    bLandCover = 0;                         // a window of LandCoverTexels^2 is bound for LandCoverWindowId
	uint32    LandCoverWindowId = 0;
	FVector3f LandCoverEast = FVector3f(1.0f, 0.0f, 0.0f);    // unit, world (= translated world directions)
	FVector3f LandCoverNorth = FVector3f(0.0f, 1.0f, 0.0f);
	FVector2f LandCoverCamOffsetM = FVector2f::ZeroVector;    // camera (E, N) from the window centre, m
	float     LandCoverTexelM = 10.0f;
	uint32    LandCoverTexels = 0;

	// Sea band (ThermalCS's): CamHeightCm + Vert + D^2 / (2 SeaRadiusCm) < WaterBandCm
	uint32    bSea = 0;
	FVector3f Up = FVector3f(0.0f, 0.0f, 1.0f);
	float     CamHeightCm = 0.0f;
	float     SeaRadiusCm = 6.371e8f;
	float     WaterBandCm = 50.0f;

	uint8     CodeClass[256] = {};      // land-cover code -> class
	uint8     StencilClass[256] = {};   // entity stencil -> class (0: no entity, terrain decides)
};
