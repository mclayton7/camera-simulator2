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

	double ScaleCulledScreenSpaceErrorForFov(double CulledSse, double HFovDeg)
	{
		if (!FMath::IsFinite(HFovDeg)) return CulledSse;
		const double HalfFov = FMath::DegreesToRadians(FMath::Clamp(HFovDeg, 1.0, 170.0) * 0.5);
		const double Scale   = FMath::Tan(FMath::DegreesToRadians(CulledReferenceHFovDeg * 0.5)) / FMath::Tan(HalfFov);
		return CulledSse * FMath::Max(Scale, 1.0);
	}

	double ResolveTilesetCulledScreenSpaceError(double TerrainCulledSse, bool bIsBuildings, double TilesetMaximumSse)
	{
		return bIsBuildings ? FMath::Max(TerrainCulledSse, TilesetMaximumSse) : TerrainCulledSse;
	}

	bool UseLodTransitions(const FCamSimConfig& Cfg)
	{
		return Cfg.bUseLodTransitions;
	}

	void TuneTileset(ACesium3DTileset& T, const FCamSimConfig& Cfg)
	{
		const double CulledSSE = ResolveCulledScreenSpaceError(Cfg);

		T.MaximumSimultaneousTileLoads = Cfg.MaxSimultaneousTileLoads;
		T.MaximumScreenSpaceError      = Cfg.MaximumScreenSpaceError;
		if (Cfg.MaximumCachedBytesMB > 0)
		{
			T.MaximumCachedBytes =
				static_cast<int64>(Cfg.MaximumCachedBytesMB) * 1024LL * 1024LL;
		}
		T.PreloadAncestors       = true;
		T.PreloadSiblings        = false;  // prefetch camera already covers adjacent tiles
		T.ForbidHoles            = false;  // true grows the render set linearly during camera motion
		T.LoadingDescendantLimit = Cfg.LoadingDescendantLimit;

		// Off-screen tiles stay loaded at the on-screen detail by default. The old
		// coarse setting (SSE 200) left a third of the view blurry for ~2 s after
		// a 90° gimbal snap; SSE 16 costs ~60% more tiles (needs frustum culling off).
		T.EnforceCulledScreenSpaceError = true;
		T.CulledScreenSpaceError        = CulledSSE;
		// CulledScreenSpaceError only reaches tiles culled by a *disabled* culling
		// stage, so with frustum culling on, tiles outside the view stay coarse
		// and a gimbal snap shows them for ~2 s (Yosemite snap test, 2026-09-28).
		T.EnableFrustumCulling = Cfg.bFrustumCulling;

		// CIGI HAT/HOT + LOS queries and the KLV frame centre are line traces,
		// which need tile collision. Turning it off saves cook time and memory
		// but makes every one of them miss the terrain.
		T.SetCreatePhysicsMeshes(Cfg.bCreatePhysicsMeshes);

		T.SetUseLodTransitions(UseLodTransitions(Cfg));
		T.LodTransitionLength = Cfg.LodTransitionLength;
		T.LogSelectionStats   = Cfg.bLogTileSelectionStats;
	}

	void ApplyCesiumTilesetTuning(UWorld* World, const FCamSimConfig& Cfg)
	{
		if (!World) return;

		const double CulledSSE = ResolveCulledScreenSpaceError(Cfg);
		for (TActorIterator<ACesium3DTileset> It(World); It; ++It)
		{
			TuneTileset(**It, Cfg);
			UE_LOG(LogCamSim, Log,
				TEXT("CamSim: tuned tileset '%s' (maxLoads=%d SSE=%.1f culledSSE=%.0f cacheMB=%d descLimit=%d frustumCull=%d lodBlend=%d logStats=%d physicsMeshes=%d)"),
				*It->GetName(), Cfg.MaxSimultaneousTileLoads,
				Cfg.MaximumScreenSpaceError, CulledSSE,
				Cfg.MaximumCachedBytesMB, Cfg.LoadingDescendantLimit,
				(int)Cfg.bFrustumCulling, (int)UseLodTransitions(Cfg), (int)Cfg.bLogTileSelectionStats, (int)Cfg.bCreatePhysicsMeshes);
		}
	}
}
