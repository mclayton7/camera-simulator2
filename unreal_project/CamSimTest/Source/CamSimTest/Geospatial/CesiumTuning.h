// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UWorld;
struct FCamSimConfig;

namespace CamSim::Geospatial
{
	/**
	 * Apply Cesium tileset streaming parameters (SSE, cache, culling, physics)
	 * to every ACesium3DTileset in the world. Safe to call with a null World.
	 *
	 * Called from ACamSimCamera::BeginPlay and UCamSimSubsystem::HotReloadConfig
	 * (so runtime tuning takes effect without a level reload).
	 */
	void ApplyCesiumTilesetTuning(UWorld* World, const FCamSimConfig& Cfg);

	/**
	 * Pure derivation: scales the off-frustum culled-SSE with horizontal FoV so
	 * a narrow sensor (e.g. 5° EO) gets more aggressive culling and a wide one
	 * gets less. Clamped at ≥100 so very-narrow FoVs don't collapse the cull
	 * threshold to zero. Extracted from ApplyCesiumTilesetTuning so it can be
	 * tested without a live world.
	 */
	double ComputeCulledScreenSpaceError(double HFovDeg);
}
