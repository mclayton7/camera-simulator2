// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Geospatial/TerrainReadinessGate.h"

class AActor;
class ACesium3DTileset;
class USceneCaptureComponent2D;
struct FCamSimConfig;

/**
 * FCamSimStreamingController
 *
 * Keeps Cesium tile streaming matched to the sensor for ACamSimCamera:
 *   - registers a prefetch streaming camera with ACesiumCameraManager (an
 *     inflated FOV that preloads tiles for gimbal slews; Cesium already
 *     streams for the primary view's real FOV) and moves it with the sensor
 *     every frame;
 *   - lowers the screen-space error while the gimbal slews fast, and adapts
 *     it to the frame budget (adaptive SSE);
 *   - holds frames until the tiles for the view have loaded (terrain gate).
 *
 * Game thread only.
 */
class FCamSimStreamingController
{
public:
	using FTilesets = TArray<TWeakObjectPtr<ACesium3DTileset>>;

	/** Register the streaming cameras and configure the terrain gate. */
	void Initialize(AActor* Owner, const FCamSimConfig& Cfg);

	/** Cesium stand-in cameras the primary view needs: the prefetch camera only (ROADMAP 3A). */
	static int32 NumStreamingCameras(const FCamSimConfig& Cfg);

	/** Remove the streaming cameras. */
	void Shutdown(AActor* Owner);

	/** Move the streaming cameras to the sensor's current pose and FOV. */
	void UpdateCameras(AActor* Owner, const USceneCaptureComponent2D& Sensor, const FCamSimConfig& Cfg);

	/** Scale the tilesets' culled (off-screen) screen-space error with the live FOV. */
	void UpdateLevelOfDetail(float HFovDeg, const FCamSimConfig& Cfg, const FTilesets& Tilesets);

	/**
	 * Update the terrain gate from tileset load progress and the platform's
	 * movement. Returns true if frames may be emitted.
	 */
	bool UpdateTerrainGate(double Lat, double Lon, const FTilesets& Tilesets);

	bool IsTerrainReady() const { return TerrainGate.IsReady(); }

	/** Log per-tileset load progress and memory (heartbeat). */
	static void LogTilesetStats(const FTilesets& Tilesets);

private:
	static void SetCulledScreenSpaceError(const FTilesets& Tilesets, double Sse);

	double AppliedCulledSse = -1.0;  // last culled SSE written (< 0 = none yet)

	// Slots in ACesiumCameraManager::AdditionalCameras (-1 = not registered).
	// The array has no stable IDs, so these are only valid while nothing else
	// inserts/removes entries ahead of ours — true for CamSim's single camera.
	int32 PrimaryCameraSlot  = -1;
	int32 PrefetchCameraSlot = -1;

	// Terrain readiness gate
	FTerrainReadinessGate TerrainGate;
	double GatePrevLat      = 0.0;
	double GatePrevLon      = 0.0;
	bool   bHasGatePosition = false;
};
