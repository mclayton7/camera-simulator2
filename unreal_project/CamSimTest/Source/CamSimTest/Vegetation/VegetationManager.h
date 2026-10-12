// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"
#include "Templates/SharedPointer.h"
#include "Vegetation/VegetationGrid.h"
#include "Vegetation/VegetationScatter.h"
#include "Vegetation/VegetationBlocks.h"
#include "Vegetation/VegetationCanopy.h"
#include "Vegetation/VegetationFarTier.h"
#include "Vegetation/VegetationStreaming.h"

class ACesium3DTileset;
class FCesiumSurfaceProbe;
class UWorld;
struct FCamSimConfig;
struct FVegetationLibrary;

/** Everything a cell actor needs (REALISM R4). LocalInstances / ShrubLocal are in the cell's ESU frame, cm. */
struct FVegCellSpawn
{
	double CentreLatDeg = 0.0;
	double CentreLonDeg = 0.0;
	double BaseHeightM = 0.0;                             // the first grounded tree's, else the first shrub's
	TArray<FTransform> LocalInstances;                    // trees
	TArray<CamSimVegetation::FTreeInstance> Instances;   // the grounded trees, same order
	TArray<double> HeightsM;                              // ellipsoid heights, same order
	TArray<FTransform> ShrubLocal;
	TArray<CamSimVegetation::FTreeInstance> Shrubs;      // the grounded shrubs, same order
	TArray<double> ShrubHeightsM;
	int32 NumInBuildings = 0;                             // candidates dropped inside building footprints
};

/** The manager's side effects; real ones from CreateLiveManager, fakes in tests. Done callbacks run on the game thread. */
struct FVegetationHooks
{
	TFunction<void(CamSimVegetation::FVegCellId, TFunction<void(CamSimVegetation::FCellContent)>)> Generate;
	TFunction<void(const TArray<FVector>& LonLatH, TFunction<void(TArray<TOptional<double>>)>)> SampleHeights;
	/** Optional: heights on the package's buildings tileset (unset = no building there). Unbound = no buildings. */
	TFunction<void(const TArray<FVector>& LonLatH, TFunction<void(TArray<TOptional<double>>)>)> SampleBuildingHeights;
	TFunction<void(CamSimVegetation::FVegCellId, const FVegCellSpawn&)> Spawn;
	TFunction<void(CamSimVegetation::FVegCellId)> Destroy;
	/** Optional: the scatter's tile caches' summary warnings (land cover, canopy), drained every WarningTicks. */
	TFunction<TArray<FString>()> TakeWarnings;
	/** Optional: one JSON line per tier change, spawn, destroy and empty settle (camsim.Vegetation.EventsPath). */
	TFunction<void(const FString&)> OnEvent;
};

/** The shrub ring (REALISM R4): the cells within RadiusM (plus half a cell diagonal, so every cell overlapping the
 *  circle) get a shrub-only actor through Hooks; the near cells then carry trees only. */
struct FVegetationShrubRing
{
	double RadiusM = 500.0;
	FVegetationHooks Hooks;
};

/**
 * Vegetation streaming around the camera (REALISM R4 slice, docs/vegetation.md): FVegetationStreaming decides, the hooks
 * act. Game thread. Every started cell gets its OnGenerated / OnHeightsSampled report: a failure reports 0, and a stage
 * whose Done never runs (Cesium drops the callback when the tileset actor goes away) reports 0 after StuckTicks. Each
 * request carries a token, so a result for a dropped, timed-out or re-requested cell's older request is ignored.
 * With SampleBuildingHeights bound, the height stage runs two batches (terrain and buildings) and ends when both have
 * answered (or after StuckTicks); KeepClearOfBuildings drops the trees inside building footprints.
 */
class CAMSIMTEST_API FVegetationManager : public TSharedFromThis<FVegetationManager>
{
public:
	/** Ticks a generation or height request may stay unanswered before its cell is reported empty (60 s at 30 fps). */
	static constexpr int32 StuckTicks = 1800;
	/** Shrub heights come from a grid of this spacing per cell (6 x 6 at 100 m cells), bilinear, sunk ShrubSinkM: Cesium
	 *  answers every height point on the game thread (Tileset::loadTiles); ~1,000 shrubs per cell sampled one by one cost
	 *  +8 ms game p95 in the long_range bench, a 10 m grid still +4 ms. Trees keep exact per-tree samples. */
	static constexpr double ShrubGridSpacingM = 20.0;
	static constexpr double ShrubSinkM = 0.15;
	/** At most one "trees dropped inside building footprints" line per this many ticks (30 s at 30 fps). */
	static constexpr int32 DropLogTicks = 900;
	/** Tile-cache warnings are drained to the log this often (1 s at 30 fps). */
	static constexpr int32 WarningTicks = 30;

	/** R4 slice streaming: the cells within Params.RadiusM (no blocks, no far tier). */
	FVegetationManager(const FVegetationStreaming::FParams& Params, FVegetationHooks InHooks);
	/** Blocks (REALISM R4): near blocks' cells through InHooks, far blocks through FarHooks (Tiers.FarM 0 = none). The
	 *  in-flight (4) and spawn (2) budgets are shared, near first; a block switching tier keeps its old actors until the
	 *  new tier has settled (spawned, or empty). */
	FVegetationManager(const FVegetationTierPlanner::FParams& Tiers, FVegetationHooks InHooks, FVegetationFarHooks FarHooks,
		TOptional<FVegetationShrubRing> ShrubRing = {});
	~FVegetationManager();

	/** HeightM: the camera's height above the ground; blocks and the shrub ring are planned by 3D distance. */
	void Tick(double CamLatDeg, double CamLonDeg, double HeightM = 0.0);
	int32 NumSpawned() const { return Spawned.Num(); }
	FVegetationStreaming& GetStreaming() { return Streaming; }
	/** Spawned cells (for the ground check and dumps). */
	const TMap<CamSimVegetation::FVegCellId, FVegCellSpawn>& GetSpawned() const { return Spawned; }
	/** Trees dropped inside building footprints so far (all cells, including ones that ended up empty). */
	int64 NumDroppedInBuildings() const { return DroppedInBuildings; }
	/** Every cell within the radius is settled (none in flight, none waiting for a spawn slot), far blocks and the shrub
	 *  ring too. */
	bool IsIdle() const
	{
		return Streaming.NumInFlight() == 0 && Ready.Num() == 0 && (!Far || Far->NumInFlight() == 0) && (!Ring || Ring->IsIdle());
	}
	/** The far tier, or null (R4 constructor, or FarM 0). */
	FVegetationFarTier* GetFarTier() const { return Far.Get(); }
	const FVegetationTierPlanner& GetPlanner() const { return Planner; }
	/** The shrub ring's own manager (radius streaming, shrub-only cells), or null. */
	FVegetationManager* GetShrubRing() const { return Ring.Get(); }

private:
	struct FRequest
	{
		uint32 Token = 0;
		int64  StartTick = 0;   // of the current stage
		// Height stage: the generated candidates (trees, then shrubs, in one batch) and each batch's answer.
		TArray<CamSimVegetation::FTreeInstance> Instances;
		TArray<CamSimVegetation::FTreeInstance> Shrubs;
		CamSimVegetation::FFarHeightGrid ShrubGrid;   // its points follow the trees' in the height batch
		TOptional<TArray<TOptional<double>>> TerrainH;
		TOptional<TArray<TOptional<double>>> BuildingsH;
		bool bWantBuildings = false;
	};

	void ExpireStuckRequests();
	bool IsCurrent(CamSimVegetation::FVegCellId Id, uint32 Token, EVegCellState Stage) const;
	void OnGenerated(CamSimVegetation::FVegCellId Id, uint32 Token, CamSimVegetation::FCellContent Content);
	void OnHeights(CamSimVegetation::FVegCellId Id, uint32 Token, bool bBuildings, TArray<TOptional<double>> Heights);
	void FinishHeights(CamSimVegetation::FVegCellId Id);
	FVegetationStreaming::FActions TickBlocks(double CamLatDeg, double CamLonDeg, double HeightM, int32 InFlightCap);
	void TickFar(int32 Spawns);
	/** The shrub ring's tick (on the ring's manager): the cells within RingRadiusM, budgets from the owner; returns its spawns. */
	int32 TickRing(double CamLatDeg, double CamLonDeg, double HeightM, int32 InFlightCap, int32 SpawnCap);
	/** Drops, forgets, spawns and starts generating as A says. */
	void Apply(const FVegetationStreaming::FActions& A);
	void EmitCell(const TCHAR* Ev, CamSimVegetation::FVegCellId Id) const;
	void EmitBlock(const TCHAR* Ev, const TCHAR* Tier, CamSimVegetation::FVegBlockId B) const;

	FVegetationStreaming Streaming;
	FVegetationHooks Hooks;
	FVegetationTierPlanner Planner;
	bool bBlocks = false;
	int32 MaxInFlight = 4;
	int32 MaxSpawnsPerTick = 2;
	TSharedPtr<FVegetationFarTier> Far;
	TSharedPtr<FVegetationManager> Ring;
	double RingRadiusM = 0.0;   // on the ring's manager: cells whose centre is within it (3D) are wanted
	double CellSizeM = 100.0;
	int64  TickCount = 0;
	uint32 NextToken = 1;
	int32  NumExpired = 0;
	int64  DroppedInBuildings = 0;
	int64  LoggedDroppedInBuildings = 0;
	int64  LastDropLogTick = -DropLogTicks;
	bool   bWasIdle = true;
	TMap<CamSimVegetation::FVegCellId, FRequest> Requests;       // cells in Generating / Sampling
	TMap<CamSimVegetation::FVegCellId, FVegCellSpawn> Ready;     // grounded, waiting for a spawn slot
	TMap<CamSimVegetation::FVegCellId, FVegCellSpawn> Spawned;
};

namespace CamSimVegetation
{
	/** A tree is inside a building when the buildings tileset answers more than this above its terrain height. */
	constexpr double BuildingClearanceM = 0.5;

	/**
	 * The trees to keep, as indices (ascending): terrain height set and finite, and no building over it. A building is
	 * over it when BuildingsM (null = the package has no buildings) has a finite height there more than
	 * BuildingClearanceM above the terrain; an unset buildings sample is a miss (no building), so it keeps the tree.
	 * OutNumInBuildings (optional) = trees with a terrain height dropped for a building.
	 */
	CAMSIMTEST_API TArray<int32> KeepClearOfBuildings(const TArray<TOptional<double>>& TerrainM,
		const TArray<TOptional<double>>* BuildingsM, int32* OutNumInBuildings = nullptr);

	/** How trees are kept out of buildings (REALISM R3b): the package's footprint mask, else (an R3 package) the runtime
	 *  height check on the buildings tileset, else nothing. vegetation.drop_in_buildings false turns both off. */
	enum class EBuildingExclusion : uint8 { Off, None, Mask, RuntimeCheck };

	CAMSIMTEST_API EBuildingExclusion ChooseBuildingExclusion(bool bDropInBuildings, bool bHasMask, bool bHasBuildingsTileset);
	/** The `buildings:` part of the "Vegetation: on (...)" log line. */
	CAMSIMTEST_API FString DescribeBuildingExclusion(EBuildingExclusion E, float ClearanceM);

	CAMSIMTEST_API TArray<FTransform> ToCellLocal(double CentreLatDeg, double CentreLonDeg, double BaseHeightM,
		const TArray<FTreeInstance>& Instances, const TArray<double>& HeightsM);

	/** The scatter seed: VegetationSeed of the package seed when the manifest has one (int64 folded to 32 bits as
	 *  low ^ high, so manifests' 0..2^31-1 seeds pass unchanged), else of vegetation.seed's 32 bits. */
	CAMSIMTEST_API uint32 ResolveSeed(bool bHasSceneSeed, int64 SceneSeed, int32 ConfigSeed);

	/** The scatter's view of a point (REALISM R4): the land-cover code, plus the canopy where the layer covers it (unset
	 *  Canopy = outside the canopy layer, so the scatter uses the land-cover rule there). */
	CAMSIMTEST_API FVegPoint CombinePoint(uint8 Code, const TOptional<FCanopySample>& Canopy);

	/** The species mix: the package's vegetation/mix.json, else vegetation.mix_file (OutSource "package" / "file"), else
	 *  DefaultMix ("default"). Unknown type names or an unreadable/unparsable file: a warning (the names dropped, or the
	 *  default mix), or with scene.offline null + bOutFatal + OutWhyNot. Game thread (logs). */
	CAMSIMTEST_API TSharedPtr<FVegetationMix, ESPMode::ThreadSafe> ResolveMix(const FCamSimConfig& Cfg, const FVegetationLibrary& Library,
		FString& OutSource, FString& OutWhyNot, bool& bOutFatal);

	/** Real hooks: UE tasks + FVegetationLandCover, SampleHeightMostDetailed on Terrain; trees are kept out of buildings by
	 *  the package's footprint mask (FVegetationBuildingMask, on the scatter worker) or, without one, by a second batch
	 *  on the buildings tileset (tag CamSimBuildings), per ChooseBuildingExclusion; ACamSimVegetationCell. Game thread.
	 *  Null (with OutWhyNot) when vegetation is off, land cover is missing or the library doesn't load; bOutFatal also set
	 *  when scene.offline is on and the species mix is unusable (unknown types, unparsable file): the caller exits. */
	CAMSIMTEST_API TSharedPtr<FVegetationManager> CreateLiveManager(UWorld* World, ACesium3DTileset* Terrain,
		const FCamSimConfig& Cfg, FString& OutWhyNot, bool& bOutFatal);

	/** Game thread, once per tick after Tick(): camsim.Vegetation.GroundCheck. Waits until the view has settled
	 *  (bViewSettled: the caller's terrain gate open for a while), streaming is idle and 200 spawned tree bases hit the
	 *  terrain when traced with Probe (the DIS clamp's complex trace, buildings ignored), logs |dz|, resets the CVar. */
	CAMSIMTEST_API void RunDebugHooks(const FCesiumSurfaceProbe& Probe, FVegetationManager& Manager, bool bViewSettled);
}
