// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"
#include "Vegetation/VegetationBlocks.h"
#include "Vegetation/VegetationFar.h"
#include "Vegetation/VegetationStreaming.h"

/** Everything a far-block actor needs (REALISM R4). Local is in the block centre's ESU frame, cm. */
struct FVegFarSpawn
{
	double CentreLatDeg = 0.0;
	double CentreLonDeg = 0.0;
	double BaseHeightM = 0.0;
	TArray<CamSimVegetation::FFarTree> Trees;   // the grounded ones
	TArray<double> HeightsM;                     // ellipsoid heights (grid, sunk SinkM), same order
	TArray<FTransform> Local;
};

/** The far tier's side effects (real ones from CreateLiveManager, fakes in tests). Done callbacks run on the game thread. */
struct FVegetationFarHooks
{
	TFunction<void(CamSimVegetation::FVegBlockId, TFunction<void(TArray<CamSimVegetation::FFarTree>)>)> Generate;
	TFunction<void(const TArray<FVector>& LonLatH, TFunction<void(TArray<TOptional<double>>)>)> SampleHeights;
	TFunction<void(CamSimVegetation::FVegBlockId, const FVegFarSpawn&)> Spawn;
	TFunction<void(CamSimVegetation::FVegBlockId)> Destroy;
};

/**
 * Far-tier pipeline (REALISM R4, docs/vegetation.md): per far block, generate (FarFilter over its cells' trees) ->
 * one height batch on a GridSpacingM grid (bilinear, trees sunk SinkM; a tree whose quad has a failed point is dropped)
 * -> spawn. The caller (FVegetationManager) chooses the wanted blocks and when one may drop. Requests carry tokens and
 * time out after StuckTicks, as the near tier's. Game thread.
 */
class CAMSIMTEST_API FVegetationFarTier : public TSharedFromThis<FVegetationFarTier>
{
public:
	static constexpr double SinkM = 0.3;
	static constexpr double GridSpacingM = 25.0;
	static constexpr int32 StuckTicks = 1800;

	/** Notify(event, block): "spawn", "destroy" or "empty" (the events dump). */
	FVegetationFarTier(int32 BlockCells, double CellSizeM, FVegetationFarHooks InHooks,
		TFunction<void(const TCHAR*, CamSimVegetation::FVegBlockId)> InNotify = nullptr);
	~FVegetationFarTier();

	void Tick(TConstArrayView<CamSimVegetation::FVegBlockId> Wanted, TFunctionRef<bool(CamSimVegetation::FVegBlockId)> MayDrop,
		int32 InFlightCap, int32 SpawnCap, int64 TickCount);
	bool IsSettled(CamSimVegetation::FVegBlockId B) const { return Streaming.IsSettled(B); }
	int32 NumInFlight() const { return Streaming.NumInFlight(); }
	/** Spawns this tick (set by Tick). */
	int32 NumSpawnedLastTick() const { return SpawnedLastTick; }
	const TMap<CamSimVegetation::FVegBlockId, FVegFarSpawn>& GetSpawned() const { return Spawned; }

private:
	struct FRequest
	{
		uint32 Token = 0;
		int64 StartTick = 0;
		TArray<CamSimVegetation::FFarTree> Trees;
		CamSimVegetation::FFarHeightGrid Grid;
	};

	bool IsCurrent(CamSimVegetation::FVegBlockId B, uint32 Token, EVegCellState Stage) const;
	void OnGenerated(CamSimVegetation::FVegBlockId B, uint32 Token, TArray<CamSimVegetation::FFarTree> Trees);
	void OnHeights(CamSimVegetation::FVegBlockId B, uint32 Token, TArray<TOptional<double>> Heights);
	void Notify(const TCHAR* Ev, CamSimVegetation::FVegBlockId B) const { if (NotifyFn) NotifyFn(Ev, B); }

	int32 BlockCells;
	double CellSizeM;
	FVegetationFarHooks Hooks;
	TFunction<void(const TCHAR*, CamSimVegetation::FVegBlockId)> NotifyFn;
	FVegetationStreaming Streaming;
	int64 Now = 0;
	uint32 NextToken = 1;
	int32 SpawnedLastTick = 0;
	TMap<CamSimVegetation::FVegBlockId, FRequest> Requests;
	TMap<CamSimVegetation::FVegBlockId, FVegFarSpawn> Ready;
	TMap<CamSimVegetation::FVegBlockId, FVegFarSpawn> Spawned;
};
