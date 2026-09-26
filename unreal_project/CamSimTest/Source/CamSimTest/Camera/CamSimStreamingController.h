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
 *   - registers two streaming cameras with ACesiumCameraManager (the sensor's
 *     real FOV, and an inflated one that prefetches for gimbal slews) and
 *     moves them with the sensor every frame;
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

	/** Remove the streaming cameras. */
	void Shutdown(AActor* Owner);

	/** Move the streaming cameras to the sensor's current pose and FOV. */
	void UpdateCameras(AActor* Owner, const USceneCaptureComponent2D& Sensor, const FCamSimConfig& Cfg);

	/**
	 * Gimbal-slew prefetch boost and adaptive SSE. Sets
	 * MaximumScreenSpaceError on the tilesets when either is active.
	 */
	void UpdateLevelOfDetail(float DeltaTime, float GimbalYawDeg, float GimbalPitchDeg,
		const FCamSimConfig& Cfg, const FTilesets& Tilesets);

	/**
	 * Update the terrain gate from tileset load progress and the platform's
	 * movement. Returns true if frames may be emitted.
	 */
	bool UpdateTerrainGate(double Lat, double Lon, const FTilesets& Tilesets);

	bool IsTerrainReady() const { return TerrainGate.IsReady(); }

	/** Log per-tileset load progress and memory (heartbeat). */
	static void LogTilesetStats(const FTilesets& Tilesets);

	int32 GetPrefetchBoostFramesRemaining() const { return PrefetchBoostFramesRemaining; }
	float GetAdaptiveSse() const { return AdaptiveSse; }

private:
	static void SetScreenSpaceError(const FTilesets& Tilesets, double Sse);

	// Slots in ACesiumCameraManager::AdditionalCameras (-1 = not registered).
	// The array has no stable IDs, so these are only valid while nothing else
	// inserts/removes entries ahead of ours — true for CamSim's single camera.
	int32 PrimaryCameraSlot  = -1;
	int32 PrefetchCameraSlot = -1;

	// Gimbal-slew prefetch
	float PrevGimbalYawDeg   = 0.0f;
	float PrevGimbalPitchDeg = 0.0f;
	bool  bHasPrevGimbal     = false;  // no slew is measured from the first sample
	int32 PrefetchBoostFramesRemaining = 0;

	// Adaptive SSE (0 = not initialised)
	float AdaptiveSse             = 0.0f;
	int32 UnderBudgetStreakFrames = 0;

	// Terrain readiness gate
	FTerrainReadinessGate TerrainGate;
	double GatePrevLat      = 0.0;
	double GatePrevLon      = 0.0;
	bool   bHasGatePosition = false;
};
