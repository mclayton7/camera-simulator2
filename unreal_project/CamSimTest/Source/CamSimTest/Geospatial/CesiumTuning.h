// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class ACesium3DTileset;
class UWorld;
struct FCamSimConfig;

namespace CamSim::Geospatial
{
	/**
	 * Apply Cesium tileset streaming parameters (SSE, cache, culling, physics)
	 * to every ACesium3DTileset in the world. Safe to call with a null World.
	 *
	 * Called from CamSim::Geospatial::SetUpCesiumWorld.
	 */
	void ApplyCesiumTilesetTuning(UWorld* World, const FCamSimConfig& Cfg);

	/**
	 * The tuning ApplyCesiumTilesetTuning applies, for one tileset (the buildings tileset spawned later uses it).
	 * Silent: ApplyCesiumTilesetTuning logs the terrain values, the buildings spawn logs its final ones.
	 */
	void TuneTileset(ACesium3DTileset& Tileset, const FCamSimConfig& Cfg);

	/**
	 * Cesium's dithered LOD crossfade: use_lod_transitions. It needs TSR (the
	 * primary view's anti-aliasing) to resolve the dither.
	 */
	bool UseLodTransitions(const FCamSimConfig& Cfg);

	/**
	 * Detail for tiles outside the view: culled_screen_space_error if set (> 0),
	 * else maximum_screen_space_error, so a gimbal snap lands on tiles already
	 * at full detail instead of coarse placeholders.
	 */
	double ResolveCulledScreenSpaceError(const FCamSimConfig& Cfg);

	/** Field of view whose detail off-screen tiles keep (see ScaleCulledScreenSpaceErrorForFov). */
	constexpr double CulledReferenceHFovDeg = 60.0;

	/**
	 * Culled SSE for the current horizontal FOV. Cesium measures tile detail in
	 * screen pixels, so a narrow FOV asks for far finer tiles; applied to every
	 * off-screen tile (frustum culling off) that loads zoomed-in detail all the
	 * way round the camera. Scale the culled SSE by tan(30 deg) / tan(HFOV / 2) so
	 * off-screen tiles keep the detail a 60-degree view needs; never finer than
	 * CulledSse, so wide views keep full off-screen detail.
	 */
	double ScaleCulledScreenSpaceErrorForFov(double CulledSse, double HFovDeg);

	/**
	 * Culled SSE for one tileset given the terrain's (resolved, FOV-scaled) value. The terrain uses it as is; the
	 * scene buildings tileset never refines off-screen tiles finer than its own maximum SSE, so it takes
	 * max(TerrainCulledSse, TilesetMaximumSse) (with frustum culling off, a finer culled SSE would load off-screen
	 * buildings at more detail than on-screen ones).
	 */
	double ResolveTilesetCulledScreenSpaceError(double TerrainCulledSse, bool bIsBuildings, double TilesetMaximumSse);
}
