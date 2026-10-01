// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RHIResources.h"
#include "Tasks/Task.h"
#include "Thermal/LandCoverGeometry.h"

class FLandCoverTileCache;

/** One built window (CPU), immutable once published (ROADMAP 4B). */
struct FLandCoverWindowData
{
	uint32 Id = 0;                        // 1, 2, ... per FLandCoverWindow; 0 = none
	CamSimLandCover::FWindowSpec Spec;
	TArray<uint8> Codes;                  // Spec.Texels^2 WorldCover codes, row 0 = north
	int64  NonZeroTexels = 0;
	int32  TilesUsed = 0;
	int32  TilesMissing = 0;              // tiles the window touches that have no (usable) data
	double BuildMs = 0.0;
};

/**
 * The GPU side of one window (ROADMAP 4B): an R8_UINT texture created and filled on the render thread by the upload
 * command FLandCoverWindow::Update enqueues before any frame can use it. Ref-counted: the render thread keeps the window a
 * frame's thermal parameters were built against (paired by Id) until it receives the next frame's.
 */
struct FLandCoverGpuWindow
{
	uint32 Id = 0;
	int32  Texels = 0;
	FTextureRHIRef Texture;               // render thread only; null until uploaded (or with bCreateGpuTexture = false)
};

/**
 * Camera-centred land-cover window (ROADMAP 4B). Update (game thread, every thermal tick) harvests a finished build and
 * swaps it in, and starts a new build on a task thread when the camera is more than RecentreFraction of the window from its
 * centre. The previous window keeps rendering until the swap: nothing blocks the game or render thread except the 4 MB
 * texture upload, once per re-centre. No window (missing index, pole, no data in range) means land cover off (4A).
 */
class CAMSIMTEST_API FLandCoverWindow
{
public:
	static constexpr float TexelM = 10.0f;

	struct FSettings
	{
		FString Dir;                       // thermal.land_cover.dir
		int32   Texels = 2048;             // thermal.land_cover.window_texels
		float   RecentreFraction = 0.25f;  // thermal.land_cover.recentre_fraction
		int32   MaxCachedTiles = 64;
		bool    bCreateGpuTexture = true;  // false: CPU only (NullRHI tests)
		bool operator==(const FSettings&) const = default;
	};

	/** Upper bound on the tiles a window touches: sizes the build's pin set. */
	static int32 EstimateTiles(const CamSimLandCover::FWindowSpec& S);

	FLandCoverWindow() = default;
	~FLandCoverWindow();
	FLandCoverWindow(const FLandCoverWindow&) = delete;
	FLandCoverWindow& operator=(const FLandCoverWindow&) = delete;

	/** Game thread. New settings drop the windows and the tile cache; a build still in flight is discarded when it lands. */
	void Configure(const FSettings& S);
	/** Game thread, every thermal tick. Never blocks. */
	void Update(double CamLatDeg, double CamLonDeg);

	TSharedPtr<const FLandCoverWindowData, ESPMode::ThreadSafe> GetCurrent() const { return Current; }
	TSharedPtr<const FLandCoverGpuWindow, ESPMode::ThreadSafe> GetCurrentGpu() const { return CurrentGpu; }
	bool IsAvailable() const;
	bool IsBuildInFlight() const { return bBuildInFlight; }
	/** Tests: wait for the in-flight build and harvest it as Update would. */
	void FinishBuildForTest();
	/** New warnings (each condition once per Configure / per data loss). */
	TArray<FString> TakeWarnings() { return MoveTemp(Warnings); }
	uint32 GetBuildsStarted() const { return BuildsStarted; }

private:
	using FBuildResult = TSharedPtr<FLandCoverWindowData, ESPMode::ThreadSafe>;

	void StartBuild(double CamLatDeg, double CamLonDeg);
	void Harvest();
	void Publish(const FBuildResult& Data);

	FSettings Settings;
	bool bConfigured = false;
	TSharedPtr<FLandCoverTileCache, ESPMode::ThreadSafe> Cache;
	TSharedPtr<const FLandCoverWindowData, ESPMode::ThreadSafe> Current;
	TSharedPtr<const FLandCoverGpuWindow, ESPMode::ThreadSafe> CurrentGpu;   // the upload command holds the mutable ref
	TOptional<CamSimLandCover::FWindowSpec> LastSpec;   // the last window started (published or not): the recentre box
	UE::Tasks::TTask<FBuildResult> Build;
	bool   bBuildInFlight = false;
	uint32 Generation = 0;        // bumped by Configure
	uint32 BuildGeneration = 0;   // Generation when the in-flight build started
	uint32 NextId = 1;
	uint32 BuildsStarted = 0;
	TArray<FString> Warnings;
	bool bWarnedUnavailable = false;
	bool bWarnedPole = false;
	bool bWarnedNoData = false;
};
