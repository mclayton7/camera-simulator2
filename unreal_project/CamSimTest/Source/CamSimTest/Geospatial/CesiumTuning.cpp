// Copyright CamSim Contributors. All Rights Reserved.

#include "Geospatial/CesiumTuning.h"
#include "CamSimTest.h"
#include "Config/CamSimConfig.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Cesium3DTileset.h"

namespace CamSim::Geospatial
{
	double ComputeCulledScreenSpaceError(double HFovDeg)
	{
		// Narrow-FOV sensors (e.g. 5° EO) can tolerate far more aggressive off-frustum
		// culling than the default 60° tuning. Scale proportionally and clamp so a
		// zoomed-out view doesn't collapse to nothing.
		constexpr double ReferenceFovDeg      = 60.0;   // baseline horizontal FOV for tuning
		constexpr double BaseCulledSseAtRef   = 200.0;  // SSE applied when HFovDeg == ReferenceFovDeg
		constexpr double MinCulledSse         = 100.0;  // floor so wide-FOV tilesets still cull off-frustum geometry
		constexpr double MinHFovClampDeg      = 1.0;    // guard against div-by-zero / runaway scaling

		const double FovScale = FMath::Max(HFovDeg, MinHFovClampDeg) / ReferenceFovDeg;
		return FMath::Max(BaseCulledSseAtRef * FovScale, MinCulledSse);
	}

	void ApplyCesiumTilesetTuning(UWorld* World, const FCamSimConfig& Cfg)
	{
		if (!World) return;

		const double CulledSSE = ComputeCulledScreenSpaceError(static_cast<double>(Cfg.HFovDeg));

		for (TActorIterator<ACesium3DTileset> It(World); It; ++It)
		{
			It->MaximumSimultaneousTileLoads = Cfg.MaxSimultaneousTileLoads;
			It->MaximumScreenSpaceError      = Cfg.MaximumScreenSpaceError;
			if (Cfg.MaximumCachedBytesMB > 0)
			{
				It->MaximumCachedBytes =
					static_cast<int64>(Cfg.MaximumCachedBytesMB) * 1024LL * 1024LL;
			}
			It->PreloadAncestors       = true;
			It->PreloadSiblings        = false;  // prefetch camera already covers adjacent tiles
			It->ForbidHoles            = false;  // true grows the render set linearly during camera motion
			It->LoadingDescendantLimit = Cfg.LoadingDescendantLimit;

			// Aggressively cull off-screen tiles (low-res placeholders outside frustum).
			It->EnforceCulledScreenSpaceError = true;
			It->CulledScreenSpaceError        = CulledSSE;

			// Non-player ISR camera never collides — skip physics-mesh cook (saves VRAM + CPU).
			// HAT/HOT terrain feedback uses Cesium's own sampling API, not physics queries.
			It->SetCreatePhysicsMeshes(false);

			It->SetUseLodTransitions(Cfg.bUseLodTransitions);
			It->LodTransitionLength = Cfg.LodTransitionLength;
			It->LogSelectionStats   = Cfg.bLogTileSelectionStats;

			UE_LOG(LogCamSim, Log,
				TEXT("CamSim: tuned tileset '%s' (maxLoads=%d SSE=%.1f culledSSE=%.0f@hfov=%.0f° cacheMB=%d descLimit=%d lodBlend=%d logStats=%d physicsMeshes=off)"),
				*It->GetName(), Cfg.MaxSimultaneousTileLoads,
				Cfg.MaximumScreenSpaceError, CulledSSE, Cfg.HFovDeg,
				Cfg.MaximumCachedBytesMB, Cfg.LoadingDescendantLimit,
				(int)Cfg.bUseLodTransitions, (int)Cfg.bLogTileSelectionStats);
		}
	}
}
