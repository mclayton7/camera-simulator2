// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"
#include "Templates/SharedPointer.h"
#include "Vegetation/VegetationGrid.h"
#include "Vegetation/VegetationScatter.h"
#include "Vegetation/VegetationStreaming.h"

class ACesium3DTileset;
class FCesiumSurfaceProbe;
class UWorld;
struct FCamSimConfig;

/** Everything a cell actor needs (REALISM R4 slice). LocalInstances are in the cell's ESU frame, cm. */
struct FVegCellSpawn
{
	double CentreLatDeg = 0.0;
	double CentreLonDeg = 0.0;
	double BaseHeightM = 0.0;
	TArray<FTransform> LocalInstances;
	TArray<CamSimVegetation::FTreeInstance> Instances;   // the grounded ones, same order
	TArray<double> HeightsM;                              // ellipsoid heights, same order
};

/** The manager's side effects; real ones from CreateLiveManager, fakes in tests. Done callbacks run on the game thread. */
struct FVegetationHooks
{
	TFunction<void(CamSimVegetation::FVegCellId, TFunction<void(TArray<CamSimVegetation::FTreeInstance>)>)> Generate;
	TFunction<void(const TArray<FVector>& LonLatH, TFunction<void(TArray<TOptional<double>>)>)> SampleHeights;
	TFunction<void(CamSimVegetation::FVegCellId, const FVegCellSpawn&)> Spawn;
	TFunction<void(CamSimVegetation::FVegCellId)> Destroy;
};

/**
 * Vegetation streaming around the camera (REALISM R4 slice, docs/vegetation.md): FVegetationStreaming decides, the hooks
 * act. Game thread. Every started cell gets its OnGenerated / OnHeightsSampled report: a failure reports 0, and a stage
 * whose Done never runs (Cesium drops the callback when the tileset actor goes away) reports 0 after StuckTicks. Each
 * request carries a token, so a result for a dropped, timed-out or re-requested cell's older request is ignored.
 */
class CAMSIMTEST_API FVegetationManager : public TSharedFromThis<FVegetationManager>
{
public:
	/** Ticks a generation or height request may stay unanswered before its cell is reported empty (60 s at 30 fps). */
	static constexpr int32 StuckTicks = 1800;

	FVegetationManager(const FVegetationStreaming::FParams& Params, FVegetationHooks InHooks);
	~FVegetationManager();

	void Tick(double CamLatDeg, double CamLonDeg);
	int32 NumSpawned() const { return Spawned.Num(); }
	FVegetationStreaming& GetStreaming() { return Streaming; }
	/** Spawned cells (for the ground check and dumps). */
	const TMap<CamSimVegetation::FVegCellId, FVegCellSpawn>& GetSpawned() const { return Spawned; }
	/** Every cell within the radius is settled (none in flight, none waiting for a spawn slot). */
	bool IsIdle() const { return Streaming.NumInFlight() == 0 && Ready.Num() == 0; }

private:
	struct FRequest
	{
		uint32 Token = 0;
		int64  StartTick = 0;   // of the current stage
	};

	void ExpireStuckRequests();
	bool IsCurrent(CamSimVegetation::FVegCellId Id, uint32 Token, EVegCellState Stage) const;
	void OnGenerated(CamSimVegetation::FVegCellId Id, uint32 Token, TArray<CamSimVegetation::FTreeInstance> Instances);
	void OnHeights(CamSimVegetation::FVegCellId Id, uint32 Token, TArray<CamSimVegetation::FTreeInstance> Instances,
		TArray<TOptional<double>> Heights);

	FVegetationStreaming Streaming;
	FVegetationHooks Hooks;
	double CellSizeM = 100.0;
	int64  TickCount = 0;
	uint32 NextToken = 1;
	int32  NumExpired = 0;
	TMap<CamSimVegetation::FVegCellId, FRequest> Requests;       // cells in Generating / Sampling
	TMap<CamSimVegetation::FVegCellId, FVegCellSpawn> Ready;     // grounded, waiting for a spawn slot
	TMap<CamSimVegetation::FVegCellId, FVegCellSpawn> Spawned;
};

namespace CamSimVegetation
{
	CAMSIMTEST_API TArray<FTransform> ToCellLocal(double CentreLatDeg, double CentreLonDeg, double BaseHeightM,
		const TArray<FTreeInstance>& Instances, const TArray<double>& HeightsM);

	/** The scatter seed: VegetationSeed of the package seed when the manifest has one (int64 folded to 32 bits as
	 *  low ^ high, so manifests' 0..2^31-1 seeds pass unchanged), else of vegetation.seed's 32 bits. */
	CAMSIMTEST_API uint32 ResolveSeed(bool bHasSceneSeed, int64 SceneSeed, int32 ConfigSeed);

	/** Real hooks: UE tasks + FVegetationLandCover, SampleHeightMostDetailed on Terrain, ACamSimVegetationCell. Game thread.
	 *  Null (with OutWhyNot) when vegetation is off, land cover is missing or the library doesn't load. */
	CAMSIMTEST_API TSharedPtr<FVegetationManager> CreateLiveManager(UWorld* World, ACesium3DTileset* Terrain,
		const FCamSimConfig& Cfg, FString& OutWhyNot);

	/** Game thread, once per tick after Tick(): camsim.Vegetation.GroundCheck. Waits until streaming is idle and 200
	 *  spawned tree bases hit the terrain when traced with Probe (the DIS clamp's complex trace, buildings ignored),
	 *  logs |dz|, resets the CVar. */
	CAMSIMTEST_API void RunDebugHooks(const FCesiumSurfaceProbe& Probe, FVegetationManager& Manager);
}
