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
	 * Cesium's dithered LOD crossfade: use_lod_transitions, applied only in the
	 * primary view. It needs TSR to resolve the dither; the scene_capture path
	 * (FXAA) would blur moving views, and it stays the pre-3A A/B baseline.
	 */
	bool UseLodTransitions(const FCamSimConfig& Cfg);

	/**
	 * Detail for tiles outside the view: culled_screen_space_error if set (> 0),
	 * else maximum_screen_space_error, so a gimbal snap lands on tiles already
	 * at full detail instead of coarse placeholders.
	 */
	double ResolveCulledScreenSpaceError(const FCamSimConfig& Cfg);
}
