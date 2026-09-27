// Copyright CamSim Contributors. All Rights Reserved.

#include "Geospatial/CesiumTuning.h"
#include "CamSimTest.h"
#include "Config/CamSimConfig.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Cesium3DTileset.h"

namespace CamSim::Geospatial
{
	double ResolveCulledScreenSpaceError(const FCamSimConfig& Cfg)
	{
		return Cfg.CulledScreenSpaceError > 0.0f
			? static_cast<double>(Cfg.CulledScreenSpaceError)
			: static_cast<double>(Cfg.MaximumScreenSpaceError);
	}

	bool UseLodTransitions(const FCamSimConfig& Cfg)
	{
		return Cfg.bUseLodTransitions && Cfg.Render.IsPrimary();
	}

	void ApplyCesiumTilesetTuning(UWorld* World, const FCamSimConfig& Cfg)
	{
		if (!World) return;

		const double CulledSSE = ResolveCulledScreenSpaceError(Cfg);

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

			// Off-screen tiles stay loaded at the on-screen detail by default. The old
			// coarse setting (SSE 200) left a third of the view blurry for ~2 s after
			// a 90° gimbal snap; SSE 16 costs ~60% more tiles and no frame time.
			It->EnforceCulledScreenSpaceError = true;
			It->CulledScreenSpaceError        = CulledSSE;

			// CIGI HAT/HOT + LOS queries and the KLV frame centre are line traces,
			// which need tile collision. Turning it off saves cook time and memory
			// but makes every one of them miss the terrain.
			It->SetCreatePhysicsMeshes(Cfg.bCreatePhysicsMeshes);

			It->SetUseLodTransitions(UseLodTransitions(Cfg));
			It->LodTransitionLength = Cfg.LodTransitionLength;
			It->LogSelectionStats   = Cfg.bLogTileSelectionStats;

			UE_LOG(LogCamSim, Log,
				TEXT("CamSim: tuned tileset '%s' (maxLoads=%d SSE=%.1f culledSSE=%.0f cacheMB=%d descLimit=%d lodBlend=%d logStats=%d physicsMeshes=%d)"),
				*It->GetName(), Cfg.MaxSimultaneousTileLoads,
				Cfg.MaximumScreenSpaceError, CulledSSE,
				Cfg.MaximumCachedBytesMB, Cfg.LoadingDescendantLimit,
				(int)UseLodTransitions(Cfg), (int)Cfg.bLogTileSelectionStats, (int)Cfg.bCreatePhysicsMeshes);
		}
	}
}
