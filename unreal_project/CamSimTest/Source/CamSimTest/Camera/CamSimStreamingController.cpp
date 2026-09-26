// Copyright CamSim Contributors. All Rights Reserved.

#include "Camera/CamSimStreamingController.h"
#include "CamSimTest.h"
#include "Config/CamSimConfig.h"
#include "Geospatial/GroundSpeedEstimator.h"
#include "Components/SceneCaptureComponent2D.h"
#include "GameFramework/Actor.h"

#include "CesiumCameraManager.h"
#include "CesiumCamera.h"
#include "Cesium3DTileset.h"
#include <Cesium3DTilesSelection/Tileset.h>

void FCamSimStreamingController::Initialize(AActor* Owner, const FCamSimConfig& Cfg)
{
	FTerrainReadinessGate::FSettings GateSettings;
	GateSettings.bEnabled           = Cfg.TerrainGate.bEnabled;
	GateSettings.MinLoadProgressPct = Cfg.TerrainGate.MinLoadProgressPct;
	GateSettings.TimeoutSec         = Cfg.TerrainGate.TimeoutSec;
	GateSettings.TeleportDistanceM  = Cfg.TerrainGate.TeleportDistanceM;
	TerrainGate.Configure(GateSettings);

	// Two cameras: primary (actual FOV) drives accurate SSE/LOD for visible
	// tiles, prefetch (inflated FOV) preloads tiles outside the frustum for
	// gimbal slews.
	ACesiumCameraManager* CamMgr = ACesiumCameraManager::GetDefaultCameraManager(Owner);
	if (!CamMgr) return;

	const FVector2D Viewport(Cfg.CaptureWidth, Cfg.CaptureHeight);
	PrimaryCameraSlot = CamMgr->AdditionalCameras.Add(
		FCesiumCamera(Viewport, Owner->GetActorLocation(), Owner->GetActorRotation(), Cfg.HFovDeg));

	const float PreloadFov = FMath::Clamp(Cfg.HFovDeg * Cfg.TilePreloadFovScale, Cfg.HFovDeg, 179.0f);
	PrefetchCameraSlot = CamMgr->AdditionalCameras.Add(
		FCesiumCamera(Viewport, Owner->GetActorLocation(), Owner->GetActorRotation(), PreloadFov));

	UE_LOG(LogCamSim, Log,
		TEXT("ACamSimCamera: registered with CesiumCameraManager (primary id=%d FOV=%.0f, prefetch id=%d FOV=%.0f)"),
		PrimaryCameraSlot, Cfg.HFovDeg, PrefetchCameraSlot, PreloadFov);
}

void FCamSimStreamingController::Shutdown(AActor* Owner)
{
	if (ACesiumCameraManager* CamMgr = ACesiumCameraManager::GetDefaultCameraManager(Owner))
	{
		// Remove the higher slot first so the lower index stays valid.
		TArray<FCesiumCamera>& Cameras = CamMgr->AdditionalCameras;
		for (const int32 Slot : { FMath::Max(PrimaryCameraSlot, PrefetchCameraSlot),
		                          FMath::Min(PrimaryCameraSlot, PrefetchCameraSlot) })
		{
			if (Cameras.IsValidIndex(Slot))
			{
				Cameras.RemoveAt(Slot);
			}
		}
	}
	PrimaryCameraSlot  = -1;
	PrefetchCameraSlot = -1;
}

void FCamSimStreamingController::UpdateCameras(AActor* Owner, const USceneCaptureComponent2D& Sensor,
	const FCamSimConfig& Cfg)
{
	ACesiumCameraManager* CamMgr = ACesiumCameraManager::GetDefaultCameraManager(Owner);
	if (!CamMgr) return;

	// The live FOV (View Definition / sensor presets), not the config default,
	// so tiles stream at the resolution of the current zoom level.
	const float LiveHFov = Sensor.FOVAngle;
	const FVector2D Viewport(Cfg.CaptureWidth, Cfg.CaptureHeight);
	const FVector   Location = Sensor.GetComponentLocation();
	const FRotator  Rotation = Sensor.GetComponentRotation();

	TArray<FCesiumCamera>& Cameras = CamMgr->AdditionalCameras;
	if (Cameras.IsValidIndex(PrimaryCameraSlot))
	{
		Cameras[PrimaryCameraSlot] = FCesiumCamera(Viewport, Location, Rotation, LiveHFov);
	}
	if (Cameras.IsValidIndex(PrefetchCameraSlot))
	{
		const float PreloadFov = FMath::Clamp(LiveHFov * Cfg.TilePreloadFovScale, LiveHFov, 179.0f);
		Cameras[PrefetchCameraSlot] = FCesiumCamera(Viewport, Location, Rotation, PreloadFov);
	}
}

void FCamSimStreamingController::UpdateLevelOfDetail(float DeltaTime, float GimbalYawDeg, float GimbalPitchDeg,
	const FCamSimConfig& Cfg, const FTilesets& Tilesets)
{
	const FCamSimConfig::FPerformanceConfig& Perf = Cfg.Performance;

	// 27E — sharper tiles while the gimbal slews fast, held for a few frames.
	if (Perf.TilePrefetchSlewThresholdDegPerSec > 0.0f)
	{
		if (DeltaTime > KINDA_SMALL_NUMBER && bHasPrevGimbal)
		{
			const float PanVel  = FMath::Abs(GimbalYawDeg   - PrevGimbalYawDeg)   / DeltaTime;
			const float TiltVel = FMath::Abs(GimbalPitchDeg - PrevGimbalPitchDeg) / DeltaTime;
			if (FMath::Max(PanVel, TiltVel) >= Perf.TilePrefetchSlewThresholdDegPerSec)
			{
				// Reset (or extend) the boost window on every above-threshold frame
				PrefetchBoostFramesRemaining = Perf.TilePrefetchBoostFrames;
			}
			else if (PrefetchBoostFramesRemaining > 0)
			{
				PrefetchBoostFramesRemaining--;
			}

			SetScreenSpaceError(Tilesets, (PrefetchBoostFramesRemaining > 0)
				? Cfg.MaximumScreenSpaceError / FMath::Max(Perf.TilePrefetchFovBoost, 1.0f)
				: Cfg.MaximumScreenSpaceError);
		}
		PrevGimbalYawDeg   = GimbalYawDeg;
		PrevGimbalPitchDeg = GimbalPitchDeg;
		bHasPrevGimbal     = true;
	}

	// Adaptive SSE — coarser when over the frame budget, sharper after 30
	// frames under 75% of it. Doesn't fight the prefetch boost.
	if (Perf.bAdaptiveSSE && DeltaTime > KINDA_SMALL_NUMBER && PrefetchBoostFramesRemaining <= 0)
	{
		const float BudgetSec = 1.0f / FMath::Max(Perf.OutputFrameRateHz, 1.0f);
		if (AdaptiveSse <= 0.0f)
		{
			AdaptiveSse = Cfg.MaximumScreenSpaceError;
		}

		if (DeltaTime > BudgetSec)
		{
			AdaptiveSse = FMath::Min(AdaptiveSse + 1.0f, Perf.AdaptiveSSEMax);
			UnderBudgetStreakFrames = 0;
		}
		else if (DeltaTime < BudgetSec * 0.75f)
		{
			if (++UnderBudgetStreakFrames >= 30)
			{
				AdaptiveSse = FMath::Max(AdaptiveSse - 0.5f, Perf.AdaptiveSSEMin);
				UnderBudgetStreakFrames = 0;
			}
		}
		else
		{
			UnderBudgetStreakFrames = 0;
		}

		SetScreenSpaceError(Tilesets, AdaptiveSse);
	}
}

bool FCamSimStreamingController::UpdateTerrainGate(double Lat, double Lon, const FTilesets& Tilesets)
{
	float MinProgressPct = -1.0f;  // < 0 = no tilesets
	for (const TWeakObjectPtr<ACesium3DTileset>& Weak : Tilesets)
	{
		if (const ACesium3DTileset* T = Weak.Get())
		{
			const float P = T->GetLoadProgress();
			MinProgressPct = (MinProgressPct < 0.0f) ? P : FMath::Min(MinProgressPct, P);
		}
	}

	const double MovedM = bHasGatePosition ? FGroundSpeedEstimator::DistanceM(GatePrevLat, GatePrevLon, Lat, Lon) : 0.0;
	GatePrevLat = Lat;
	GatePrevLon = Lon;
	bHasGatePosition = true;

	const bool bWasReady = TerrainGate.IsReady();
	const bool bReady = TerrainGate.Update(FPlatformTime::Seconds(), MinProgressPct, MovedM);
	if (bReady != bWasReady)
	{
		if (bReady && TerrainGate.DidTimeOut())
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("ACamSimCamera: terrain gate timed out at %.1f%% loaded — streaming anyway"), MinProgressPct);
		}
		else
		{
			UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: terrain %s (load progress %.1f%%)"),
				bReady ? TEXT("ready — streaming frames") : TEXT("reloading after teleport — holding frames"),
				MinProgressPct);
		}
	}
	return bReady;
}

void FCamSimStreamingController::LogTilesetStats(const FTilesets& Tilesets)
{
	for (const TWeakObjectPtr<ACesium3DTileset>& Weak : Tilesets)
	{
		ACesium3DTileset* T = Weak.Get();
		if (!T) continue;
		int32 TilesLoaded = 0;
		int64 DataBytes = 0;
		if (auto* Tileset = T->GetTileset())
		{
			TilesLoaded = Tileset->getNumberOfTilesLoaded();
			DataBytes = Tileset->getTotalDataBytes();
		}
		UE_LOG(LogCamSim, Log,
			TEXT("ACamSimCamera: tileset='%s' progress=%.1f%% SSE=%.1f loaded=%d dataMB=%.1f maxLoads=%d"),
			*T->GetName(), T->GetLoadProgress(),  // GetLoadProgress() is already 0-100
			T->MaximumScreenSpaceError,
			TilesLoaded, DataBytes / (1024.0 * 1024.0),
			T->MaximumSimultaneousTileLoads);
	}
}

void FCamSimStreamingController::SetScreenSpaceError(const FTilesets& Tilesets, double Sse)
{
	for (const TWeakObjectPtr<ACesium3DTileset>& Weak : Tilesets)
	{
		if (ACesium3DTileset* T = Weak.Get())
		{
			T->MaximumScreenSpaceError = Sse;
		}
	}
}
