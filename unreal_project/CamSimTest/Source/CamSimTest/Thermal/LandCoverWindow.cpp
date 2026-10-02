// Copyright CamSim Contributors. All Rights Reserved.

#include "Thermal/LandCoverWindow.h"
#include "CamSimTest.h"
#include "HAL/PlatformTime.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Thermal/LandCoverTiles.h"

int32 FLandCoverWindow::EstimateTiles(const CamSimLandCover::FWindowSpec& S)
{
	// The small-area mapping makes latitude a function of North only and longitude a function of East only (Resample
	// evaluates columns at the centre latitude), so the E/W edges at N = 0 give the exact longitude extent at any latitude.
	// WindowENToGeodetic does not wrap longitude, so the span is continuous across the antimeridian; > 360 deg (only
	// possible at very high latitude) means every tile column.
	const double Half = 0.5 * S.Texels * S.TexelM;
	double LatN = 0.0, LatS = 0.0, LonE = 0.0, LonW = 0.0, Unused = 0.0;
	CamSimLandCover::WindowENToGeodetic(S, 0.0, Half, LatN, Unused);
	CamSimLandCover::WindowENToGeodetic(S, 0.0, -Half, LatS, Unused);
	CamSimLandCover::WindowENToGeodetic(S, Half, 0.0, Unused, LonE);
	CamSimLandCover::WindowENToGeodetic(S, -Half, 0.0, Unused, LonW);
	const double Rows = FMath::Abs(LatN - LatS) / FLandCoverTileCache::TileDeg + 2.0;
	const double Cols = FMath::Min(FMath::Abs(LonE - LonW) / FLandCoverTileCache::TileDeg + 2.0, 360.0 / FLandCoverTileCache::TileDeg);
	return static_cast<int32>(FMath::Min(Rows * Cols, 65536.0));
}

FLandCoverWindow::~FLandCoverWindow()
{
	// The task holds the tile cache, not this object, but waiting keeps a running build from outliving the module (unload,
	// live coding) whose code it is executing.
	if (bBuildInFlight) Build.Wait();
}

bool FLandCoverWindow::IsAvailable() const
{
	return Cache.IsValid() && Cache->IsValid();
}

void FLandCoverWindow::Configure(const FSettings& S)
{
	check(IsInGameThread());
	if (bConfigured && S == Settings) return;
	Settings = S;
	bConfigured = true;
	++Generation;
	Cache = MakeShared<FLandCoverTileCache, ESPMode::ThreadSafe>(S.Dir, S.MaxCachedTiles);
	Current.Reset();
	CurrentGpu.Reset();
	LastSpec.Reset();
	bWarnedUnavailable = bWarnedPole = bWarnedNoData = false;
}

void FLandCoverWindow::Update(double CamLatDeg, double CamLonDeg)
{
	check(bConfigured && IsInGameThread());
	if (bBuildInFlight && Build.IsCompleted()) Harvest();
	if (!IsAvailable())
	{
		if (!bWarnedUnavailable)
		{
			bWarnedUnavailable = true;
			Warnings.Add(FString::Printf(TEXT("land cover off: %s (terrain uses terrain_default)"), Cache.IsValid() ? *Cache->GetError() : TEXT("no cache")));
		}
		return;
	}
	if (!bBuildInFlight) Warnings.Append(Cache->TakeWarnings());   // during a build, Harvest collects them
	if (!CamSimLandCover::IsWindowAllowed(CamLatDeg))
	{
		if (!bWarnedPole)
		{
			bWarnedPole = true;
			Warnings.Add(FString::Printf(TEXT("camera within 1 deg of a pole (lat %.3f): land cover off"), CamLatDeg));
		}
		Current.Reset();
		CurrentGpu.Reset();
		LastSpec.Reset();
		return;
	}
	bWarnedPole = false;
	if (bBuildInFlight) return;
	if (!LastSpec.IsSet() || CamSimLandCover::NeedsRecentre(*LastSpec, CamLatDeg, CamLonDeg, Settings.RecentreFraction))
	{
		StartBuild(CamLatDeg, CamLonDeg);
	}
}

void FLandCoverWindow::StartBuild(double CamLatDeg, double CamLonDeg)
{
	CamSimLandCover::FWindowSpec Spec;
	Spec.CentreLatDeg = CamLatDeg;
	Spec.CentreLonDeg = CamLonDeg;
	Spec.Texels = FMath::Max(2, Settings.Texels & ~1);
	Spec.TexelM = TexelM;
	LastSpec = Spec;   // the recentre box moves now: no second build for the same move
	const uint32 Id = NextId++;
	const TSharedPtr<FLandCoverTileCache, ESPMode::ThreadSafe> Tiles = Cache;
	BuildGeneration = Generation;
	bBuildInFlight = true;
	++BuildsStarted;
	Build = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Tiles, Spec, Id]() -> FBuildResult
	{
		const double T0 = FPlatformTime::Seconds();
		const FBuildResult Data = MakeShared<FLandCoverWindowData, ESPMode::ThreadSafe>();
		Data->Id = Id;
		Data->Spec = Spec;
		// Every tile the window touches stays pinned (alive) for the whole resample: Resample keeps a raw pointer into the
		// current tile's codes, and the window can touch more tiles than the LRU holds (high latitude, small MaxCachedTiles),
		// so each tile is fetched once and outlives any eviction.
		TMap<FIntPoint, TSharedPtr<const FLandCoverTile, ESPMode::ThreadSafe>> Pinned;
		Pinned.Reserve(EstimateTiles(Spec));
		Data->NonZeroTexels = CamSimLandCover::Resample(Spec, [&Pinned, &Tiles, &Data](int32 LatIndex, int32 LonIndex) -> const uint8*
		{
			const FIntPoint Key(LatIndex, LonIndex);
			if (const TSharedPtr<const FLandCoverTile, ESPMode::ThreadSafe>* P = Pinned.Find(Key))
			{
				return P->IsValid() ? (*P)->Codes.GetData() : nullptr;
			}
			const TSharedPtr<const FLandCoverTile, ESPMode::ThreadSafe> T = Tiles->Get(LatIndex, LonIndex);
			Pinned.Add(Key, T);
			++(T.IsValid() ? Data->TilesUsed : Data->TilesMissing);
			return T.IsValid() ? T->Codes.GetData() : nullptr;
		}, Data->Codes);
		Data->BuildMs = (FPlatformTime::Seconds() - T0) * 1000.0;
		return Data;
	});
}

void FLandCoverWindow::Harvest()
{
	bBuildInFlight = false;
	const FBuildResult Data = Build.GetResult();
	Build = UE::Tasks::TTask<FBuildResult>();
	if (Cache.IsValid()) Warnings.Append(Cache->TakeWarnings());   // tiles that failed during this build
	if (BuildGeneration != Generation || !Data.IsValid()) return;   // settings changed while it was building
	if (Data->NonZeroTexels == 0)
	{
		if (!bWarnedNoData)
		{
			bWarnedNoData = true;
			Warnings.Add(FString::Printf(TEXT("no land-cover data within %.1f km of (%.5f, %.5f) in %s: terrain uses terrain_default"),
				0.0005 * Data->Spec.Texels * Data->Spec.TexelM, Data->Spec.CentreLatDeg, Data->Spec.CentreLonDeg, *Cache->GetDir()));
		}
		Current.Reset();
		CurrentGpu.Reset();
		return;
	}
	bWarnedNoData = false;
	Publish(Data);
	UE_LOG(LogCamSim, Log, TEXT("LandCover: window #%u at (%.5f, %.5f), %d^2 x %.0f m, built in %.1f ms (%d tiles, %d without data)"),
		Data->Id, Data->Spec.CentreLatDeg, Data->Spec.CentreLonDeg, Data->Spec.Texels, Data->Spec.TexelM, Data->BuildMs,
		Data->TilesUsed, Data->TilesMissing);
}

void FLandCoverWindow::Publish(const FBuildResult& Data)
{
	const TSharedRef<FLandCoverGpuWindow, ESPMode::ThreadSafe> Gpu = MakeShared<FLandCoverGpuWindow, ESPMode::ThreadSafe>();
	Gpu->Id = Data->Id;
	Gpu->Texels = Data->Spec.Texels;
	if (Settings.bCreateGpuTexture)
	{
		// Enqueued before the render command carrying any frame's parameters that reference this window (same game
		// thread, in order), so the texture exists by the time a frame binds it.
		const TSharedPtr<const FLandCoverWindowData, ESPMode::ThreadSafe> Src = Data;
		ENQUEUE_RENDER_COMMAND(CamSimLandCoverUpload)([Gpu, Src](FRHICommandListImmediate& RHICmdList)
		{
			const int32 N = Src->Spec.Texels;
			const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(TEXT("CamSimLandCover"), N, N, PF_R8_UINT)
				.SetFlags(ETextureCreateFlags::ShaderResource).SetInitialState(ERHIAccess::SRVMask);
			FTextureRHIRef Tex = RHICmdList.CreateTexture(Desc);
			RHICmdList.UpdateTexture2D(Tex, 0, FUpdateTextureRegion2D(0, 0, 0, 0, N, N), N, Src->Codes.GetData());
			Gpu->Texture = Tex;
		});
	}
	Current = Data;      // the previous window (and its texture) lives on while the render thread still holds it
	CurrentGpu = Gpu;
}

void FLandCoverWindow::FinishBuildForTest()
{
	if (!bBuildInFlight) return;
	Build.Wait();
	Harvest();
}
