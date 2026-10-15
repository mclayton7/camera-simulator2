// Copyright CamSim Contributors. All Rights Reserved.

#include "Vegetation/VegetationManager.h"

#include "Async/Async.h"
#include "Cesium3DTileset.h"
#include "CesiumSampleHeightResult.h"
#include "Config/CamSimConfig.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Entity/EntityMeshLoader.h"
#include "Entity/SurfaceProbe.h"
#include "Geospatial/SceneBuildings.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Tasks/Task.h"
#include "Thermal/LandCoverGeometry.h"
#include "UObject/StrongObjectPtr.h"
#include "Vegetation/VegetationBuildingMask.h"
#include "Vegetation/VegetationCellActor.h"
#include "Vegetation/VegetationLandCover.h"
#include "Vegetation/VegetationLibrary.h"
#include "Vegetation/VegetationMix.h"
#include "Vegetation/VegetationFar.h"
#include "Containers/Ticker.h"
#include "CamSimTest.h"

// Task 2 (procedural tree fallback) was skipped: the CC0 asset passed its checks.
#define CAMSIM_VEGETATION_PROCEDURAL 0

using namespace CamSimVegetation;

static TAutoConsoleVariable<FString> CVarVegDumpPath(TEXT("camsim.Vegetation.DumpPath"), TEXT(""),
	TEXT("When set, append one JSON line per spawned vegetation cell (row, col, hash, instances) to this file (REALISM R4)."));
static TAutoConsoleVariable<FString> CVarVegFarDumpPath(TEXT("camsim.Vegetation.FarDumpPath"), TEXT(""),
	TEXT("When set, append one JSON line per spawned far block (block_row, block_col, hash, instances) to this file (REALISM R4)."));
static TAutoConsoleVariable<FString> CVarVegEventsPath(TEXT("camsim.Vegetation.EventsPath"), TEXT(""),
	TEXT("When set, append one JSON line per vegetation tier change, spawn, destroy and empty settle to this file (REALISM R4)."));
static TAutoConsoleVariable<int32> CVarVegGroundCheck(TEXT("camsim.Vegetation.GroundCheck"), 0,
	TEXT("1: trace 200 spawned tree bases against the terrain once and log the height error (REALISM R4)."));

FVegetationManager::FVegetationManager(const FVegetationStreaming::FParams& Params, FVegetationHooks InHooks)
	: Streaming(Params), Hooks(MoveTemp(InHooks)), Planner(FVegetationTierPlanner::FParams()), MaxInFlight(Params.MaxCellsInFlight),
	  MaxSpawnsPerTick(Params.MaxSpawnsPerTick), CellSizeM(Params.CellSizeM)
{
}

FVegetationManager::FVegetationManager(const FVegetationTierPlanner::FParams& Tiers, FVegetationHooks InHooks, FVegetationFarHooks FarHooks,
	TOptional<FVegetationShrubRing> ShrubRing)
	: Streaming(FVegetationStreaming::FParams{ Tiers.NearM, Tiers.CellSizeM }), Hooks(MoveTemp(InHooks)), Planner(Tiers), bBlocks(true),
	  CellSizeM(Tiers.CellSizeM)
{
	const FVegetationStreaming::FParams Defaults;
	MaxInFlight = Defaults.MaxCellsInFlight;
	MaxSpawnsPerTick = Defaults.MaxSpawnsPerTick;
	if (Tiers.FarM > 0.0)
	{
		Far = MakeShared<FVegetationFarTier>(Tiers.BlockCells, Tiers.CellSizeM, MoveTemp(FarHooks),
			[this](const TCHAR* Ev, FVegBlockId B) { EmitBlock(Ev, TEXT("far"), B); });
	}
	if (ShrubRing.IsSet() && ShrubRing->RadiusM > 0.0)
	{
		// Every cell overlapping the circle: centres within RadiusM plus half a cell diagonal.
		const double ReachM = ShrubRing->RadiusM + 0.5 * Tiers.CellSizeM * UE_SQRT_2;
		Ring = MakeShared<FVegetationManager>(FVegetationStreaming::FParams{ ReachM, Tiers.CellSizeM }, MoveTemp(ShrubRing->Hooks));
		Ring->RingRadiusM = ReachM;
	}
}

void FVegetationManager::EmitCell(const TCHAR* Ev, FVegCellId Id) const
{
	if (!Hooks.OnEvent) return;
	const FVegBlockId B = BlockOf(Id, Planner.GetParams().BlockCells, CellSizeM);
	Hooks.OnEvent(FString::Printf(TEXT("{\"tick\":%lld,\"ev\":\"%s\",\"tier\":\"near\",\"row\":%d,\"col\":%d,\"block_row\":%d,\"block_col\":%d}"),
		TickCount, Ev, Id.Row, Id.Col, B.Row, B.Col));
}

void FVegetationManager::EmitBlock(const TCHAR* Ev, const TCHAR* Tier, FVegBlockId B) const
{
	if (!Hooks.OnEvent) return;
	Hooks.OnEvent(FString::Printf(TEXT("{\"tick\":%lld,\"ev\":\"%s\",\"tier\":\"%s\",\"block_row\":%d,\"block_col\":%d}"),
		TickCount, Ev, Tier, B.Row, B.Col));
}

FVegetationStreaming::FActions FVegetationManager::TickBlocks(double CamLatDeg, double CamLonDeg, double HeightM, int32 InFlightCap)
{
	for (const FVegetationTierPlanner::FChange& C : Planner.Update(CamLatDeg, CamLonDeg, HeightM))
		EmitBlock(TEXT("tier"), !C.To.IsSet() ? TEXT("none") : (*C.To == EVegTier::Near ? TEXT("near") : TEXT("far")), C.Block);
	const int32 N = Planner.GetParams().BlockCells;
	// A near cell whose block went far stays until the far actor has settled (no gap); a block that left both tiers drops.
	auto NearMayDrop = [this, N](FVegCellId Cell)
	{
		const FVegBlockId B = BlockOf(Cell, N, CellSizeM);
		return !(Far && Planner.GetTier(B) == TOptional<EVegTier>(EVegTier::Far)) || Far->IsSettled(B);
	};
	return Streaming.TickWanted(Planner.GetNearCells(), NearMayDrop, InFlightCap, MaxSpawnsPerTick);
}

void FVegetationManager::TickFar(int32 Spawns)
{
	if (!Far) return;
	const int32 N = Planner.GetParams().BlockCells;
	// A far block whose block went near stays until every one of its cells has settled near.
	auto FarMayDrop = [this, N](FVegBlockId B)
	{
		if (Planner.GetTier(B) != TOptional<EVegTier>(EVegTier::Near)) return true;
		for (const FVegCellId& C : CellsOfBlock(B, N, CellSizeM))
			if (!Streaming.IsSettled(C)) return false;
		return true;
	};
	const int32 RingInFlight = Ring ? Ring->Streaming.NumInFlight() : 0;
	Far->Tick(Planner.GetFarBlocks(), FarMayDrop, MaxInFlight - Streaming.NumInFlight() - RingInFlight, MaxSpawnsPerTick - Spawns, TickCount);
}

FVegetationManager::~FVegetationManager()
{
	for (const auto& Kv : Spawned) if (Hooks.Destroy) Hooks.Destroy(Kv.Key);
}

void FVegetationManager::Tick(double CamLatDeg, double CamLonDeg, double HeightM)
{
	++TickCount;
	if (Hooks.TakeWarnings && TickCount % WarningTicks == 0)
		for (const FString& W : Hooks.TakeWarnings()) UE_LOG(LogCamSim, Warning, TEXT("Vegetation: %s"), *W);
	ExpireStuckRequests();   // first, so the freed slots are refilled this tick

	if (!bBlocks)
	{
		Apply(Streaming.Tick(CamLatDeg, CamLonDeg));
	}
	else
	{
		// Budgets shared near first, then the shrub ring, then far.
		const int32 RingInFlight = Ring ? Ring->Streaming.NumInFlight() : 0;
		const FVegetationStreaming::FActions A = TickBlocks(CamLatDeg, CamLonDeg, HeightM, MaxInFlight - (Far ? Far->NumInFlight() : 0) - RingInFlight);
		Apply(A);
		int32 Spawns = A.ToSpawn.Num();
		if (Ring)
			Spawns += Ring->TickRing(CamLatDeg, CamLonDeg, HeightM, MaxInFlight - Streaming.NumInFlight() - (Far ? Far->NumInFlight() : 0),
				MaxSpawnsPerTick - Spawns);
		TickFar(Spawns);
	}
	// When streaming settles with new building drops: at most one line per DropLogTicks (a moving camera settles
	// and restarts many times a second).
	const bool bIdle = IsIdle();
	if (bIdle && !bWasIdle && DroppedInBuildings != LoggedDroppedInBuildings && TickCount - LastDropLogTick >= DropLogTicks)
	{
		LastDropLogTick = TickCount;
		UE_LOG(LogCamSim, Log, TEXT("Vegetation: settled, %d cells spawned, %lld trees dropped inside building footprints so far"),
			Spawned.Num(), DroppedInBuildings);
		LoggedDroppedInBuildings = DroppedInBuildings;
	}
	bWasIdle = bIdle;
}

int32 FVegetationManager::TickRing(double CamLatDeg, double CamLonDeg, double HeightM, int32 InFlightCap, int32 SpawnCap)
{
	++TickCount;
	ExpireStuckRequests();
	// 3D, as the tiers: shrubs are culled at shrub_radius_m from the view, so a camera higher than that wants none.
	const double H = FMath::IsFinite(HeightM) ? FMath::Max(0.0, HeightM) : 0.0;
	const double DropM = RingRadiusM * FVegetationStreaming::FParams().DropFactor;
	auto MayDrop = [this, CamLatDeg, CamLonDeg, DropM, H](FVegCellId Id)
	{
		double La, Lo; CellCentre(Id, CellSizeM, La, Lo);
		const double G = GroundDistanceM(CamLatDeg, CamLonDeg, La, Lo);
		return G * G + H * H > DropM * DropM;
	};
	const TArray<FVegCellId> Wanted = RingRadiusM > H
		? CellsWithin(CamLatDeg, CamLonDeg, FMath::Sqrt(RingRadiusM * RingRadiusM - H * H), CellSizeM) : TArray<FVegCellId>();
	const FVegetationStreaming::FActions A = Streaming.TickWanted(Wanted, MayDrop, InFlightCap, SpawnCap);
	Apply(A);
	return A.ToSpawn.Num();
}

void FVegetationManager::Apply(const FVegetationStreaming::FActions& A)
{
	for (const FVegCellId& Id : A.ToDrop)
	{
		Spawned.Remove(Id);
		Hooks.Destroy(Id);
		EmitCell(TEXT("destroy"), Id);
	}
	// Payloads of cells the planner no longer tracks (dropped while ready or in flight): forget them too. A late result
	// for a forgotten request finds no matching token and is ignored, even if the cell is requested again meanwhile.
	for (auto It = Ready.CreateIterator(); It; ++It) if (!Streaming.GetState(It.Key()).IsSet()) It.RemoveCurrent();
	for (auto It = Requests.CreateIterator(); It; ++It) if (!Streaming.GetState(It.Key()).IsSet()) It.RemoveCurrent();
	for (const FVegCellId& Id : A.ToSpawn)
	{
		FVegCellSpawn S;
		if (Ready.RemoveAndCopyValue(Id, S))
		{
			Hooks.Spawn(Id, S);
			Spawned.Add(Id, MoveTemp(S));
			EmitCell(TEXT("spawn"), Id);
		}
	}
	for (const FVegCellId& Id : A.ToGenerate)
	{
		const uint32 Token = NextToken++;
		Requests.Add(Id, FRequest{ Token, TickCount });
		TWeakPtr<FVegetationManager> Weak = AsWeak();
		Hooks.Generate(Id, [Weak, Id, Token](FCellContent C)
		{
			if (TSharedPtr<FVegetationManager> M = Weak.Pin()) M->OnGenerated(Id, Token, MoveTemp(C));
		});
	}
}

void FVegetationManager::ExpireStuckRequests()
{
	for (auto It = Requests.CreateIterator(); It; ++It)
	{
		if (TickCount - It.Value().StartTick <= StuckTicks) continue;
		const FVegCellId Id = It.Key();
		const TOptional<EVegCellState> State = Streaming.GetState(Id);
		if (State == TOptional<EVegCellState>(EVegCellState::Generating)) Streaming.OnGenerated(Id, 0);
		else if (State == TOptional<EVegCellState>(EVegCellState::Sampling)) Streaming.OnHeightsSampled(Id, 0);
		It.RemoveCurrent();
		if (NumExpired++ == 0)
			UE_LOG(LogCamSim, Warning, TEXT("Vegetation: cell (%d, %d) got no %s result in %d ticks; left empty (logged once)"),
				Id.Row, Id.Col, State == TOptional<EVegCellState>(EVegCellState::Generating) ? TEXT("generation") : TEXT("height"),
				StuckTicks);
	}
}

bool FVegetationManager::IsCurrent(FVegCellId Id, uint32 Token, EVegCellState Stage) const
{
	const FRequest* R = Requests.Find(Id);
	return R && R->Token == Token && Streaming.GetState(Id) == TOptional<EVegCellState>(Stage);
}

void FVegetationManager::OnGenerated(FVegCellId Id, uint32 Token, FCellContent Content)
{
	if (!IsCurrent(Id, Token, EVegCellState::Generating)) return;
	const int32 Count = Content.Trees.Num() + Content.Shrubs.Num();
	Streaming.OnGenerated(Id, Count);
	if (Count == 0)
	{
		Requests.Remove(Id);
		EmitCell(TEXT("empty"), Id);
		return;
	}
	// One batch per cell: each tree exactly, then a ShrubGridSpacingM grid over the cell for the shrubs.
	FFarHeightGrid ShrubGrid;
	if (Content.Shrubs.Num() > 0)
	{
		const FVegCellBounds CB = CellBounds(Id, CellSizeM);
		ShrubGrid = FFarHeightGrid::ForBounds(CB.SouthDeg, CB.NorthDeg, CB.WestDeg, CB.EastDeg, ShrubGridSpacingM);
	}
	TArray<FVector> TreePoints;
	TreePoints.Reserve(Content.Trees.Num());
	for (const FTreeInstance& T : Content.Trees) TreePoints.Add(FVector(T.LonDeg, T.LatDeg, 0.0));
	TArray<FVector> LonLatH = TreePoints;
	LonLatH.Append(ShrubGrid.LonLatPoints());
	const bool bWantBuildings = static_cast<bool>(Hooks.SampleBuildingHeights);
	{
		FRequest& R = Requests[Id];
		R.StartTick = TickCount;   // the height stage gets its own StuckTicks, shared by both batches
		R.Instances = MoveTemp(Content.Trees);
		R.Shrubs = MoveTemp(Content.Shrubs);
		R.ShrubGrid = MoveTemp(ShrubGrid);
		R.TerrainH.Reset();
		R.BuildingsH.Reset();
		R.bWantBuildings = bWantBuildings;
	}
	// A hook may answer synchronously (and finish the cell), so no reference into Requests is held across the calls.
	TWeakPtr<FVegetationManager> Weak = AsWeak();
	Hooks.SampleHeights(LonLatH, [Weak, Id, Token](TArray<TOptional<double>> H)
	{
		if (TSharedPtr<FVegetationManager> M = Weak.Pin()) M->OnHeights(Id, Token, /*bBuildings=*/false, MoveTemp(H));
	});
	if (bWantBuildings && IsCurrent(Id, Token, EVegCellState::Sampling))
	{
		Hooks.SampleBuildingHeights(TreePoints, [Weak, Id, Token](TArray<TOptional<double>> H)
		{
			if (TSharedPtr<FVegetationManager> M = Weak.Pin()) M->OnHeights(Id, Token, /*bBuildings=*/true, MoveTemp(H));
		});
	}
}

void FVegetationManager::OnHeights(FVegCellId Id, uint32 Token, bool bBuildings, TArray<TOptional<double>> Heights)
{
	if (!IsCurrent(Id, Token, EVegCellState::Sampling)) return;
	FRequest& R = Requests[Id];
	(bBuildings ? R.BuildingsH : R.TerrainH) = MoveTemp(Heights);
	if (!R.TerrainH.IsSet() || (R.bWantBuildings && !R.BuildingsH.IsSet())) return;   // wait for the other batch
	FinishHeights(Id);
}

void FVegetationManager::FinishHeights(FVegCellId Id)
{
	FRequest R;
	Requests.RemoveAndCopyValue(Id, R);
	const int32 NumTrees = R.Instances.Num();
	const TArray<TOptional<double>>& All = R.TerrainH.GetValue();
	TArray<TOptional<double>> TreeH;
	for (int32 K = 0; K < NumTrees && K < All.Num(); ++K) TreeH.Add(All[K]);
	TreeH.SetNum(NumTrees);
	// The R3 runtime check (no mask) samples the buildings tileset at the trees only; shrubs need the R3b mask.
	int32 InBuildings = 0;
	const TArray<int32> Keep = KeepClearOfBuildings(TreeH, R.bWantBuildings ? &R.BuildingsH.GetValue() : nullptr, &InBuildings);
	DroppedInBuildings += InBuildings;
	FVegCellSpawn S;
	CellCentre(Id, CellSizeM, S.CentreLatDeg, S.CentreLonDeg);
	S.NumInBuildings = InBuildings;
	for (const int32 K : Keep)
	{
		S.Instances.Add(R.Instances[K]);
		S.HeightsM.Add(TreeH[K].GetValue());
	}
	if (R.Shrubs.Num() > 0)
	{
		for (int32 K = NumTrees; K < All.Num(); ++K) R.ShrubGrid.H.Add(All[K]);
		for (const FTreeInstance& Sh : R.Shrubs)
		{
			const TOptional<double> H = R.ShrubGrid.Sample(Sh.LatDeg, Sh.LonDeg);
			if (!H.IsSet()) continue;
			S.Shrubs.Add(Sh);
			S.ShrubHeightsM.Add(*H - ShrubSinkM);
		}
	}
	Streaming.OnHeightsSampled(Id, S.Instances.Num() + S.Shrubs.Num());
	if (S.Instances.Num() + S.Shrubs.Num() == 0)
	{
		EmitCell(TEXT("empty"), Id);
		return;
	}
	S.BaseHeightM = S.HeightsM.Num() > 0 ? S.HeightsM[0] : S.ShrubHeightsM[0];
	S.LocalInstances = ToCellLocal(S.CentreLatDeg, S.CentreLonDeg, S.BaseHeightM, S.Instances, S.HeightsM);
	S.ShrubLocal = ToCellLocal(S.CentreLatDeg, S.CentreLonDeg, S.BaseHeightM, S.Shrubs, S.ShrubHeightsM);
	Ready.Add(Id, MoveTemp(S));
}

namespace CamSimVegetation
{
	TArray<int32> KeepClearOfBuildings(const TArray<TOptional<double>>& TerrainM, const TArray<TOptional<double>>* BuildingsM,
		int32* OutNumInBuildings)
	{
		TArray<int32> Keep;
		Keep.Reserve(TerrainM.Num());
		int32 InBuildings = 0;
		for (int32 K = 0; K < TerrainM.Num(); ++K)
		{
			if (!TerrainM[K].IsSet() || !FMath::IsFinite(TerrainM[K].GetValue())) continue;
			if (BuildingsM && BuildingsM->IsValidIndex(K) && (*BuildingsM)[K].IsSet())
			{
				const double B = (*BuildingsM)[K].GetValue();
				if (FMath::IsFinite(B) && B > TerrainM[K].GetValue() + BuildingClearanceM)
				{
					++InBuildings;
					continue;
				}
			}
			Keep.Add(K);
		}
		if (OutNumInBuildings) *OutNumInBuildings = InBuildings;
		return Keep;
	}

	EBuildingExclusion ChooseBuildingExclusion(bool bDropInBuildings, bool bHasMask, bool bHasBuildingsTileset)
	{
		if (!bDropInBuildings) return EBuildingExclusion::Off;
		if (bHasMask) return EBuildingExclusion::Mask;
		return bHasBuildingsTileset ? EBuildingExclusion::RuntimeCheck : EBuildingExclusion::None;
	}

	FString DescribeBuildingExclusion(EBuildingExclusion E, float ClearanceM)
	{
		switch (E)
		{
		case EBuildingExclusion::Mask:         return FString::Printf(TEXT("mask (clearance %.1f m)"), ClearanceM);
		case EBuildingExclusion::RuntimeCheck: return TEXT("runtime check (buildings tileset, no mask)");
		case EBuildingExclusion::None:         return TEXT("none");
		default:                               return TEXT("off (drop_in_buildings false)");
		}
	}

	TArray<FTransform> ToCellLocal(double CentreLatDeg, double CentreLonDeg, double BaseHeightM,
		const TArray<FTreeInstance>& Instances, const TArray<double>& HeightsM)
	{
		CamSimLandCover::FWindowSpec W;
		W.CentreLatDeg = CentreLatDeg;
		W.CentreLonDeg = CentreLonDeg;
		TArray<FTransform> Out;
		Out.Reserve(Instances.Num());
		for (int32 K = 0; K < Instances.Num() && K < HeightsM.Num(); ++K)
		{
			const FVector2D EN = CamSimLandCover::GeodeticToWindowEN(W, Instances[K].LatDeg, Instances[K].LonDeg);
			// ESU: X = East, Y = South, Z = Up; cm. Yaw about Up (heading convention doesn't matter for a random yaw).
			Out.Add(FTransform(FRotator(0.0, Instances[K].YawDeg, 0.0),
				FVector(EN.X * 100.0, -EN.Y * 100.0, (HeightsM[K] - BaseHeightM) * 100.0), FVector(Instances[K].Scale)));
		}
		return Out;
	}

	uint32 ResolveSeed(bool bHasSceneSeed, int64 SceneSeed, int32 ConfigSeed)
	{
		const uint64 Bits = static_cast<uint64>(SceneSeed);
		const uint32 Base = bHasSceneSeed ? static_cast<uint32>(Bits) ^ static_cast<uint32>(Bits >> 32) : static_cast<uint32>(ConfigSeed);
		return VegetationSeed(Base);
	}

	FVegPoint CombinePoint(uint8 Code, const TOptional<FCanopySample>& Canopy)
	{
		FVegPoint P;
		P.Code = Code;
		if (Canopy.IsSet())
		{
			P.bHasCanopy = true;
			P.CoverFrac = Canopy->CoverFrac;
			P.HeightM = Canopy->HeightM;
		}
		return P;
	}

	TSharedPtr<FVegetationMix, ESPMode::ThreadSafe> ResolveMix(const FCamSimConfig& Cfg, const FVegetationLibrary& Library,
		FString& OutSource, FString& OutWhyNot, bool& bOutFatal)
	{
		const FString Path = !Cfg.Scene.VegetationMixPath.IsEmpty() ? Cfg.Scene.VegetationMixPath : Cfg.Vegetation.MixFile;
		OutSource = !Cfg.Scene.VegetationMixPath.IsEmpty() ? TEXT("package") : (Path.IsEmpty() ? TEXT("default") : TEXT("file"));
		TSharedPtr<FVegetationMix, ESPMode::ThreadSafe> Mix = MakeShared<FVegetationMix, ESPMode::ThreadSafe>();
		if (Path.IsEmpty())
		{
			*Mix = DefaultMix(Library);
			return Mix;
		}
		FString Json, Error;
		TArray<FString> Unknown;
		const bool bRead = FFileHelper::LoadFileToString(Json, *Path);
		if (!bRead || !ParseMix(Json, Library, *Mix, Unknown, Error))
		{
			const FString Why = FString::Printf(TEXT("species mix %s unusable (%s)"), *Path, bRead ? *Error : TEXT("cannot read it"));
			if (Cfg.Scene.bOffline) { bOutFatal = true; OutWhyNot = Why; return nullptr; }
			UE_LOG(LogCamSim, Warning, TEXT("Vegetation: %s; using the built-in default mix"), *Why);
			*Mix = DefaultMix(Library);
			OutSource = TEXT("default");
			return Mix;
		}
		for (const FString& Name : Unknown)
			UE_LOG(LogCamSim, Warning, TEXT("Vegetation: mix names unknown type '%s' (not in the library); dropped"), *Name);
		if (Unknown.Num() > 0 && Cfg.Scene.bOffline)
		{
			bOutFatal = true;
			OutWhyNot = FString::Printf(TEXT("species mix %s names types the library lacks: %s"), *Path, *FString::Join(Unknown, TEXT(", ")));
			return nullptr;
		}
		return Mix;
	}

	/** One batch per type present in Instances (near meshes; Shadows / CullM per call). */
	static void AddTypeBatches(TArray<FVegHismBatch>& Out, const TArray<TStrongObjectPtr<UStaticMesh>>& Meshes,
		const TArray<FTreeInstance>& Instances, const TArray<FTransform>& Local, bool bShadows, float CullM)
	{
		TMap<int32, int32> BatchOf;
		for (int32 K = 0; K < Instances.Num() && K < Local.Num(); ++K)
		{
			const int32 T = Instances[K].Type;
			if (!Meshes.IsValidIndex(T) || !Meshes[T].IsValid()) continue;
			int32* B = BatchOf.Find(T);
			if (!B)
			{
				FVegHismBatch NewB;
				NewB.Mesh = Meshes[T].Get();
				NewB.bCastShadows = bShadows;
				NewB.CullDistanceM = CullM;
				B = &BatchOf.Add(T, Out.Add(MoveTemp(NewB)));
			}
			Out[*B].Instances.Add(Local[K]);
		}
	}

	static FString DumpInstances(const TArray<FTreeInstance>& I, const TArray<double>& H)
	{
		FString Out;
		for (int32 K = 0; K < I.Num(); ++K)
			Out += FString::Printf(TEXT("%s[%.9f,%.9f,%.3f,%.4f,%.4f,%d]"), K ? TEXT(",") : TEXT(""), I[K].LatDeg, I[K].LonDeg, H[K],
				I[K].YawDeg, I[K].Scale, int32(I[K].Type));
		return Out;
	}

	TSharedPtr<FVegetationManager> CreateLiveManager(UWorld* World, ACesium3DTileset* Terrain, const FCamSimGeospatialProvider* Geo,
		const FCamSimConfig& Cfg, FString& OutWhyNot, bool& bOutFatal)
	{
		bOutFatal = false;
		const FCamSimConfig::FVegetationConfig& V = Cfg.Vegetation;
		if (!V.bEnabled) { OutWhyNot = TEXT("vegetation.enabled is false"); return nullptr; }
		if (!World || !Terrain) { OutWhyNot = TEXT("no world or terrain tileset"); return nullptr; }
		TSharedPtr<FVegetationLandCover, ESPMode::ThreadSafe> LandCover = MakeShared<FVegetationLandCover, ESPMode::ThreadSafe>(Cfg.Thermal.LandCover.Dir);
		if (!LandCover->IsValid()) { OutWhyNot = FString::Printf(TEXT("no land cover (%s)"), *LandCover->GetError()); return nullptr; }
		FVegetationLibrary Library; FString LibError;
		if (!LoadLibrary(ResolveLibraryDir(V.LibraryDir), Library, LibError)) { OutWhyNot = LibError; return nullptr; }
		if (Library.Types.Num() == 0) { OutWhyNot = TEXT("the vegetation library has no types"); return nullptr; }
		FString MixSource;
		const TSharedPtr<FVegetationMix, ESPMode::ThreadSafe> Mix = ResolveMix(Cfg, Library, MixSource, OutWhyNot, bOutFatal);
		if (!Mix) return nullptr;
		TArray<TStrongObjectPtr<UStaticMesh>> NearMeshes, FarMeshes;
		int32 NumTrees = 0, NumShrubs = 0;
		for (const FVegetationType& Type : Library.Types)
		{
			UStaticMesh* Mesh = nullptr;
#if CAMSIM_VEGETATION_PROCEDURAL
			if (Type.bProcedural) Mesh = CreateProceduralTreeMesh(Type.HeightM);
#endif
			if (!Type.bProcedural) Mesh = CamSimMeshLoader::LoadStaticMesh(Type.GltfPath);
			if (!Mesh) UE_LOG(LogCamSim, Warning, TEXT("Vegetation: could not load type '%s'; it is not drawn"), *Type.Name);
			NearMeshes.Emplace(Mesh);
			UStaticMesh* FarMesh = (V.FarRadiusM > 0.0f && Type.HasFarMesh()) ? CamSimMeshLoader::LoadStaticMesh(Type.FarGltfPath) : nullptr;
			if (V.FarRadiusM > 0.0f && Type.Role == EVegRole::Tree && !FarMesh)
				UE_LOG(LogCamSim, Log, TEXT("Vegetation: type '%s' has no far mesh; it is not drawn in the far tier"), *Type.Name);
			FarMeshes.Emplace(FarMesh);
			(Type.Role == EVegRole::Shrub ? NumShrubs : NumTrees) += Mesh ? 1 : 0;
		}
		if (NumTrees + NumShrubs == 0) { OutWhyNot = TEXT("no vegetation type loaded"); return nullptr; }

		FScatterParams P;
		P.CellSizeM = V.CellSizeM;
		P.DensityScale = V.DensityScale;
		P.ShrubSpacingM = V.ShrubSpacingM;
		// Shrubs come from the shrub ring (cells within shrub_radius_m only), never from the near cells: ~1,000 per
		// cell generated out to the near radius cost +1.9 ms game p50 in the dense pass whatever the shrub cull.
		bool bAnyShrubCode = false;
		for (const auto& Kv : Mix->Codes) bAnyShrubCode |= Kv.Value.Shrubs.Num() > 0;
		const bool bShrubRing = V.bShrubsEnabled && V.ShrubRadiusM > 0.0f && NumShrubs > 0 && bAnyShrubCode;
		P.bShrubs = false;
		FScatterParams ShrubP = P;
		ShrubP.bTrees = false;
		ShrubP.bShrubs = true;
		const uint32 Seed = ResolveSeed(Cfg.Scene.bHasSeed, Cfg.Scene.Seed, V.Seed);
		TWeakObjectPtr<UWorld> WeakWorld(World);
		TWeakObjectPtr<ACesium3DTileset> WeakTerrain(Terrain);
		TSharedRef<TMap<FVegCellId, TWeakObjectPtr<ACamSimVegetationCell>>> Actors = MakeShared<TMap<FVegCellId, TWeakObjectPtr<ACamSimVegetationCell>>>();
		const bool bShadows = V.bCastShadows;
		// Near blocks are whole: a block within RadiusM keeps its cells up to RadiusM * 1.2 plus a block diagonal away.
		const float CullM = V.RadiusM * 1.2f + V.FarBlockCells * V.CellSizeM * 1.4142f;
		const float ShrubCullM = V.ShrubRadiusM;

		// Trees inside building footprints (REALISM R3b): the package's mask on the scatter worker when it has one (no
		// runtime sampling); else, for an R3 package, the runtime check (a second height batch on the buildings tileset).
		TSharedPtr<FVegetationBuildingMask, ESPMode::ThreadSafe> Mask;
		if (V.bDropInBuildings && !Cfg.Scene.BuildingsMaskDir.IsEmpty())
		{
			Mask = MakeShared<FVegetationBuildingMask, ESPMode::ThreadSafe>(Cfg.Scene.BuildingsMaskDir);
			if (!Mask->IsValid())
			{
				UE_LOG(LogCamSim, Warning, TEXT("Vegetation: building mask unusable (%s); falling back"), *Mask->GetError());
				Mask.Reset();
			}
		}
		// Canopy cover and height (REALISM R4): the package's canopy/ layer; without it (or off) the land-cover rule.
		TSharedPtr<FVegetationCanopy, ESPMode::ThreadSafe> Canopy;
		FString CanopyState = V.bCanopyEnabled ? TEXT("none") : TEXT("off");
		if (V.bCanopyEnabled && !Cfg.Scene.CanopyDir.IsEmpty())
		{
			Canopy = MakeShared<FVegetationCanopy, ESPMode::ThreadSafe>(Cfg.Scene.CanopyDir);
			if (!Canopy->IsValid())
			{
				UE_LOG(LogCamSim, Warning, TEXT("Vegetation: canopy layer unusable (%s); using the land-cover rule"), *Canopy->GetError());
				Canopy.Reset();
			}
			else
			{
				CanopyState = TEXT("chmv2");
			}
		}
		ACesium3DTileset* Buildings = CamSim::Geospatial::FindBuildingsTileset(World);
		const EBuildingExclusion Exclusion = ChooseBuildingExclusion(V.bDropInBuildings, Mask.IsValid(), Buildings != nullptr);
		const double ClearanceM = V.BuildingClearanceM;

		FVegetationHooks H;
		// Always answers (an empty land cover lookup yields no trees); the manager's weak pointer drops it after teardown.
		auto MakeGenerate = [LandCover, Canopy, Mask, Mix, Seed, ClearanceM](const FScatterParams& P)
		{
		return [LandCover, Canopy, Mask, Mix, P, Seed, ClearanceM](FVegCellId Id, TFunction<void(FCellContent)> Done)
		{
			UE::Tasks::Launch(UE_SOURCE_LOCATION, [LandCover, Canopy, Mask, Mix, P, Seed, ClearanceM, Id, Done = MoveTemp(Done)]() mutable
			{
				auto Sample = [&LandCover, &Canopy](double La, double Lo)
				{
					return CombinePoint(LandCover->Code(La, Lo), Canopy.IsValid() ? Canopy->Sample(La, Lo) : TOptional<FCanopySample>());
				};
				auto ClearFn = [&Mask, ClearanceM](double La, double Lo) { return Mask->IsClear(La, Lo, ClearanceM); };
				const FClearPredicate Clear = ClearFn;
				FCellContent C = GenerateCellContent(Seed, Id, P, *Mix, Sample, Mask.IsValid() ? &Clear : nullptr);
				AsyncTask(ENamedThreads::GameThread, [Done = MoveTemp(Done), C = MoveTemp(C)]() mutable { Done(MoveTemp(C)); });
			});
		};
		};
		H.Generate = MakeGenerate(P);
		// Answers with all-unset heights when the tileset is gone or the result count is off. If the tileset actor is
		// destroyed with a request outstanding, Cesium never calls back (Cesium3DTileset.cpp: `if (!IsValid(this)) return;`):
		// the manager's StuckTicks reports the cell empty.
		auto MakeSampler = [](TWeakObjectPtr<ACesium3DTileset> WeakTileset, const TCHAR* What, bool bWarnOnFail)
		{
			return [WeakTileset, What, bWarnOnFail](const TArray<FVector>& LonLatH, TFunction<void(TArray<TOptional<double>>)> Done)
			{
				const int32 N = LonLatH.Num();
				ACesium3DTileset* T = WeakTileset.Get();
				if (!T) { TArray<TOptional<double>> None; None.SetNum(N); Done(MoveTemp(None)); return; }
				T->SampleHeightMostDetailed(LonLatH, FCesiumSampleHeightMostDetailedCallback::CreateLambda(
					[N, What, bWarnOnFail, Done = MoveTemp(Done)](ACesium3DTileset*, const TArray<FCesiumSampleHeightResult>& R, const TArray<FString>& Warnings) mutable
					{
						static bool bWarned = false;
						if (bWarnOnFail && Warnings.Num() > 0 && !bWarned)
						{
							bWarned = true;
							UE_LOG(LogCamSim, Warning, TEXT("Vegetation: %s height sampling warning (failed samples drop their trees; logged once): %s"), What, *Warnings[0]);
						}
						TArray<TOptional<double>> Out;
						Out.SetNum(N);   // unset = failed (on the buildings tileset: no building there)
						if (R.Num() == N)
							for (int32 K = 0; K < N; ++K)
								if (R[K].SampleSuccess) Out[K] = R[K].LongitudeLatitudeHeight.Z;
						if (IsInGameThread()) { Done(MoveTemp(Out)); return; }
						AsyncTask(ENamedThreads::GameThread, [Done = MoveTemp(Done), Out = MoveTemp(Out)]() mutable { Done(MoveTemp(Out)); });
					}));
			};
		};
		H.SampleHeights = MakeSampler(WeakTerrain, TEXT("terrain"), /*bWarnOnFail=*/true);
		// Trees inside building footprints (WorldCover tags canopy over roofs as tree cover): a second batch on the
		// buildings tileset, where a miss is the normal answer, so its warnings stay quiet.
		if (Exclusion == EBuildingExclusion::RuntimeCheck) H.SampleBuildingHeights = MakeSampler(TWeakObjectPtr<ACesium3DTileset>(Buildings), TEXT("buildings"), /*bWarnOnFail=*/false);
		// Near cells (trees) and shrub-ring cells (shrubs) each have their own actor per cell.
		auto MakeSpawn = [WeakWorld, NearMeshes, bShadows, CullM, ShrubCullM](TSharedRef<TMap<FVegCellId, TWeakObjectPtr<ACamSimVegetationCell>>> CellActors, bool bRing)
		{
			return [WeakWorld, NearMeshes, CellActors, bShadows, CullM, ShrubCullM, bRing](FVegCellId Id, const FVegCellSpawn& S)
			{
				UWorld* W = WeakWorld.Get();
				if (!W) return;
				ACamSimVegetationCell* Cell = W->SpawnActor<ACamSimVegetationCell>();
				if (!Cell) return;
				TArray<FVegHismBatch> Batches;
				AddTypeBatches(Batches, NearMeshes, S.Instances, S.LocalInstances, bShadows, CullM);
				AddTypeBatches(Batches, NearMeshes, S.Shrubs, S.ShrubLocal, bShadows, ShrubCullM);
				Cell->Init(S.CentreLatDeg, S.CentreLonDeg, S.BaseHeightM, Batches);
				CellActors->Add(Id, Cell);
				const FString Dump = CVarVegDumpPath.GetValueOnGameThread();
				if (Dump.IsEmpty()) return;
				// "hash" covers the trees only (R4's value for R4's cells); "shrub_hash" the shrubs, on the shrub ring's own
				// line ("layer":"shrubs"). Instances end with the type.
				const FString Line = bRing
					? FString::Printf(TEXT("{\"layer\":\"shrubs\",\"row\":%d,\"col\":%d,\"shrub_hash\":%u,\"shrubs\":[%s]}\n"),
						Id.Row, Id.Col, HashInstances(S.Shrubs), *DumpInstances(S.Shrubs, S.ShrubHeightsM))
					: FString::Printf(TEXT("{\"row\":%d,\"col\":%d,\"hash\":%u,\"shrub_hash\":%u,\"in_buildings\":%d,")
						TEXT("\"instances\":[%s],\"shrubs\":[%s]}\n"), Id.Row, Id.Col, HashInstances(S.Instances), HashInstances(S.Shrubs),
						S.NumInBuildings, *DumpInstances(S.Instances, S.HeightsM), *DumpInstances(S.Shrubs, S.ShrubHeightsM));
				FFileHelper::SaveStringToFile(Line, *Dump, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
			};
		};
		auto MakeDestroy = [](TSharedRef<TMap<FVegCellId, TWeakObjectPtr<ACamSimVegetationCell>>> CellActors)
		{
			return [CellActors](FVegCellId Id)
			{
				TWeakObjectPtr<ACamSimVegetationCell> A;
				if (CellActors->RemoveAndCopyValue(Id, A) && A.IsValid()) A->Destroy();
			};
		};
		H.Spawn = MakeSpawn(Actors, /*bRing=*/false);
		H.TakeWarnings = [LandCover, Canopy]()
		{
			TArray<FString> W = LandCover->TakeWarnings();
			if (Canopy.IsValid()) W.Append(Canopy->TakeWarnings());
			return W;
		};
		H.Destroy = MakeDestroy(Actors);
		H.OnEvent = [](const FString& Line)
		{
			const FString Path = CVarVegEventsPath.GetValueOnGameThread();
			if (!Path.IsEmpty())
				FFileHelper::SaveStringToFile(Line + TEXT("\n"), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
		};

		// Far tier (REALISM R4): the same trees (shrubs off), thinned and enlarged, far meshes, no shadows, 4 dissolve buckets.
		FVegetationFarHooks FH;
		FFarParams FarP;
		FarP.Keep = V.FarKeep;
		FarP.Scale = V.FarScale;
		for (const TStrongObjectPtr<UStaticMesh>& M : FarMeshes) FarP.TypeHasFar.Add(M.IsValid());
		FScatterParams FarScatter = P;   // trees only
		const int32 BlockCells = V.FarBlockCells;
		const double CellM = V.CellSizeM;
		FH.Generate = [LandCover, Canopy, Mask, Mix, FarScatter, FarP, Seed, ClearanceM, BlockCells, CellM](FVegBlockId B, TFunction<void(TArray<FFarTree>)> Done)
		{
			UE::Tasks::Launch(UE_SOURCE_LOCATION, [LandCover, Canopy, Mask, Mix, FarScatter, FarP, Seed, ClearanceM, BlockCells, CellM, B, Done = MoveTemp(Done)]() mutable
			{
				auto Sample = [&LandCover, &Canopy](double La, double Lo)
				{
					return CombinePoint(LandCover->Code(La, Lo), Canopy.IsValid() ? Canopy->Sample(La, Lo) : TOptional<FCanopySample>());
				};
				auto ClearFn = [&Mask, ClearanceM](double La, double Lo) { return Mask->IsClear(La, Lo, ClearanceM); };
				const FClearPredicate Clear = ClearFn;
				TArray<FFarTree> Trees;
				for (const FVegCellId& C : CellsOfBlock(B, BlockCells, CellM))
					Trees.Append(FarFilter(Seed, C, GenerateCellContent(Seed, C, FarScatter, *Mix, Sample, Mask.IsValid() ? &Clear : nullptr).Trees, FarP));
				AsyncTask(ENamedThreads::GameThread, [Done = MoveTemp(Done), Trees = MoveTemp(Trees)]() mutable { Done(MoveTemp(Trees)); });
			});
		};
		// Far heights from the rendered terrain (traces on the drawn tiles' collision): SampleHeightMostDetailed on every far
		// block's grid loaded the finest terrain tiles over the whole far disk (after a low pass over woodland the next pass
		// ran +0.3 ms game p50, +2.5 ms p95, +500 MiB). Needs physics meshes; without them the far tier samples like the near one.
		FString FarHeights = TEXT("most-detailed");
		if (Cfg.bCreatePhysicsMeshes && Geo)
		{
			TSharedRef<FCesiumSurfaceProbe> Probe = MakeShared<FCesiumSurfaceProbe>(World, Geo, true);
			TSharedRef<FRetryingTraceSampler> Sampler = MakeShared<FRetryingTraceSampler>(
				[Probe](double La, double Lo) { return Probe->TraceHeight(La, Lo, FarTraceTopM, FarTraceBottomM, CamSimSurface::NoDecks); });
			TWeakPtr<FRetryingTraceSampler> WeakSampler = Sampler;
			FTSTicker::GetCoreTicker().AddTicker(TEXT("CamSimVegetationFarHeights"), 0.0f, [WeakSampler](float)
			{
				TSharedPtr<FRetryingTraceSampler> S = WeakSampler.Pin();
				if (S) S->Tick();
				return S.IsValid();   // removed once the manager (which owns the sampler) is gone
			});
			FH.SampleHeights = [Sampler](const TArray<FVector>& LonLatH, TFunction<void(TArray<TOptional<double>>)> Done)
			{
				Sampler->Sample(LonLatH, MoveTemp(Done));
			};
			FarHeights = TEXT("rendered");
		}
		else
		{
			FH.SampleHeights = H.SampleHeights;
		}
		TSharedRef<TMap<FVegBlockId, TWeakObjectPtr<ACamSimVegetationCell>>> FarActors = MakeShared<TMap<FVegBlockId, TWeakObjectPtr<ACamSimVegetationCell>>>();
		const float FarM = V.FarRadiusM;
		FH.Spawn = [WeakWorld, FarMeshes, FarActors, FarM, FarP](FVegBlockId B, const FVegFarSpawn& S)
		{
			UWorld* W = WeakWorld.Get();
			if (!W) return;
			ACamSimVegetationCell* Cell = W->SpawnActor<ACamSimVegetationCell>();
			if (!Cell) return;
			TMap<int32, int32> BatchOf;   // type * Buckets + bucket -> batch
			TArray<FVegHismBatch> Batches;
			for (int32 K = 0; K < S.Trees.Num() && K < S.Local.Num(); ++K)
			{
				const FFarTree& T = S.Trees[K];
				if (!FarMeshes.IsValidIndex(T.Tree.Type) || !FarMeshes[T.Tree.Type].IsValid()) continue;
				const int32 Key = int32(T.Tree.Type) * FarP.Buckets + T.Bucket;
				int32* Bi = BatchOf.Find(Key);
				if (!Bi)
				{
					FVegHismBatch NewB;
					NewB.Mesh = FarMeshes[T.Tree.Type].Get();
					NewB.bCastShadows = false;
					NewB.CullDistanceM = BucketCullM(FarM, T.Bucket, FarP.Buckets);
					Bi = &BatchOf.Add(Key, Batches.Add(MoveTemp(NewB)));
				}
				Batches[*Bi].Instances.Add(S.Local[K]);
			}
			Cell->Init(S.CentreLatDeg, S.CentreLonDeg, S.BaseHeightM, Batches);
			FarActors->Add(B, Cell);
			const FString Dump = CVarVegFarDumpPath.GetValueOnGameThread();
			if (!Dump.IsEmpty())
			{
				FString Inst;
				for (int32 K = 0; K < S.Trees.Num(); ++K)
				{
					const FTreeInstance& T = S.Trees[K].Tree;
					Inst += FString::Printf(TEXT("%s[%.9f,%.9f,%.3f,%.4f,%.4f,%d,%d]"), K ? TEXT(",") : TEXT(""), T.LatDeg, T.LonDeg, S.HeightsM[K],
						T.YawDeg, T.Scale, int32(T.Type), int32(S.Trees[K].Bucket));
				}
				FFileHelper::SaveStringToFile(FString::Printf(TEXT("{\"block_row\":%d,\"block_col\":%d,\"hash\":%u,\"instances\":[%s]}\n"),
					B.Row, B.Col, HashFarTrees(S.Trees), *Inst), *Dump, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
			}
		};
		FH.Destroy = [FarActors](FVegBlockId B)
		{
			TWeakObjectPtr<ACamSimVegetationCell> A;
			if (FarActors->RemoveAndCopyValue(B, A) && A.IsValid()) A->Destroy();
		};

		FVegetationTierPlanner::FParams Tiers;
		Tiers.NearM = V.RadiusM;
		Tiers.FarM = V.FarRadiusM;
		Tiers.CellSizeM = V.CellSizeM;
		Tiers.BlockCells = V.FarBlockCells;
		UE_LOG(LogCamSim, Log, TEXT("Vegetation: on (types %d (trees %d, shrubs %d), mix %s, canopy %s, shrubs %s, radius %.0f m, far %s, ")
			TEXT("density %.2f, shadows %d, seed %u, land cover %s, buildings: %s)"),
			NumTrees + NumShrubs, NumTrees, NumShrubs, *MixSource, *CanopyState,
			bShrubRing ? *FString::Printf(TEXT("%.0f m"), V.ShrubRadiusM) : TEXT("off"), V.RadiusM,
			V.FarRadiusM > 0.0f ? *FString::Printf(TEXT("%.0f m (keep %.2f, blocks %d, heights %s)"), V.FarRadiusM, V.FarKeep, V.FarBlockCells, *FarHeights) : TEXT("off"),
			V.DensityScale, bShadows ? 1 : 0, Seed, *Cfg.Thermal.LandCover.Dir,
			*DescribeBuildingExclusion(Exclusion, V.BuildingClearanceM));
		// The shrub ring: shrub-only cells within shrub_radius_m (same land cover, mask, heights), their own actors, no
		// building runtime check (shrubs never had one: the R3b mask covers them), no events (the handoff gate is near/far).
		TOptional<FVegetationShrubRing> Ring;
		if (bShrubRing)
		{
			FVegetationShrubRing R;
			R.RadiusM = V.ShrubRadiusM;
			R.Hooks.Generate = MakeGenerate(ShrubP);
			R.Hooks.SampleHeights = H.SampleHeights;
			TSharedRef<TMap<FVegCellId, TWeakObjectPtr<ACamSimVegetationCell>>> ShrubActors = MakeShared<TMap<FVegCellId, TWeakObjectPtr<ACamSimVegetationCell>>>();
			R.Hooks.Spawn = MakeSpawn(ShrubActors, /*bRing=*/true);
			R.Hooks.Destroy = MakeDestroy(ShrubActors);
			Ring = MoveTemp(R);
		}
		return MakeShared<FVegetationManager>(Tiers, MoveTemp(H), MoveTemp(FH), MoveTemp(Ring));
	}

	void RunDebugHooks(const FCesiumSurfaceProbe& Probe, FVegetationManager& Manager, bool bViewSettled)
	{
		if (CVarVegGroundCheck.GetValueOnGameThread() == 0) return;
		// Streaming can go idle before the terrain has refined (the first live acceptance run measured 2.1 m against
		// tiles still loading, 0.3 s before the terrain gate opened), so also wait for the caller's settled view.
		if (!bViewSettled) return;
		// Only once every cell is settled: right after startup the rendered tiles are still coarse (tens of metres off the
		// most-detailed heights for the first second or two), which would measure tile loading, not placement.
		if (!Manager.IsIdle()) return;
		// Trace spawned trees until 200 hit the terrain. Fewer hits (collision of those tiles not loaded yet, e.g. in the
		// first frames) keep the CVar set and retry next tick, so the check never reports an empty sample.
		TArray<double> Dz;
		for (const auto& Kv : Manager.GetSpawned())
		{
			for (int32 K = 0; K < Kv.Value.Instances.Num() && Dz.Num() < 200; ++K)
			{
				const FTreeInstance& T = Kv.Value.Instances[K];
				const double H = Kv.Value.HeightsM[K];
				const TOptional<double> G = Probe.TraceHeight(T.LatDeg, T.LonDeg, H + 50.0, H - 50.0, CamSimSurface::NoDecks);
				if (G.IsSet()) Dz.Add(FMath::Abs(G.GetValue() - H));
			}
			if (Dz.Num() >= 200) break;
		}
		if (Dz.Num() < 200) return;
		// Reset at the priority it was set with (SetByConsole from -ExecCmds or the console); SetByCode would be ignored.
		CVarVegGroundCheck->Set(0, static_cast<EConsoleVariableFlags>(CVarVegGroundCheck->GetFlags() & ECVF_SetByMask));
		Dz.Sort();
		UE_LOG(LogCamSim, Log, TEXT("Vegetation: ground check n=%d max_dz_m=%.3f p50_dz_m=%.3f"), Dz.Num(), Dz.Last(), Dz[Dz.Num() / 2]);
	}
}
