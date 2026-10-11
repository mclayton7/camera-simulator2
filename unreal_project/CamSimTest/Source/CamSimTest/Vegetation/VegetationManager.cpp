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
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Tasks/Task.h"
#include "Thermal/LandCoverGeometry.h"
#include "UObject/StrongObjectPtr.h"
#include "Vegetation/VegetationCellActor.h"
#include "Vegetation/VegetationLandCover.h"
#include "Vegetation/VegetationLibrary.h"
#include "CamSimTest.h"

// Task 2 (procedural tree fallback) was skipped: the CC0 asset passed its checks.
#define CAMSIM_VEGETATION_PROCEDURAL 0

using namespace CamSimVegetation;

static TAutoConsoleVariable<FString> CVarVegDumpPath(TEXT("camsim.Vegetation.DumpPath"), TEXT(""),
	TEXT("When set, append one JSON line per spawned vegetation cell (row, col, hash, instances) to this file (REALISM R4)."));
static TAutoConsoleVariable<int32> CVarVegGroundCheck(TEXT("camsim.Vegetation.GroundCheck"), 0,
	TEXT("1: trace 200 spawned tree bases against the terrain once and log the height error (REALISM R4)."));

FVegetationManager::FVegetationManager(const FVegetationStreaming::FParams& Params, FVegetationHooks InHooks)
	: Streaming(Params), Hooks(MoveTemp(InHooks)), CellSizeM(Params.CellSizeM)
{
}

FVegetationManager::~FVegetationManager()
{
	for (const auto& Kv : Spawned) if (Hooks.Destroy) Hooks.Destroy(Kv.Key);
}

void FVegetationManager::Tick(double CamLatDeg, double CamLonDeg)
{
	++TickCount;
	ExpireStuckRequests();   // first, so the freed slots are refilled this tick

	const FVegetationStreaming::FActions A = Streaming.Tick(CamLatDeg, CamLonDeg);
	for (const FVegCellId& Id : A.ToDrop)
	{
		Spawned.Remove(Id);
		Hooks.Destroy(Id);
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
		}
	}
	for (const FVegCellId& Id : A.ToGenerate)
	{
		const uint32 Token = NextToken++;
		Requests.Add(Id, FRequest{ Token, TickCount });
		TWeakPtr<FVegetationManager> Weak = AsWeak();
		Hooks.Generate(Id, [Weak, Id, Token](TArray<FTreeInstance> T)
		{
			if (TSharedPtr<FVegetationManager> M = Weak.Pin()) M->OnGenerated(Id, Token, MoveTemp(T));
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

void FVegetationManager::OnGenerated(FVegCellId Id, uint32 Token, TArray<FTreeInstance> Instances)
{
	if (!IsCurrent(Id, Token, EVegCellState::Generating)) return;
	Streaming.OnGenerated(Id, Instances.Num());
	if (Instances.Num() == 0)
	{
		Requests.Remove(Id);
		return;
	}
	Requests[Id].StartTick = TickCount;   // the height stage gets its own StuckTicks
	TArray<FVector> LonLatH;
	LonLatH.Reserve(Instances.Num());
	for (const FTreeInstance& T : Instances) LonLatH.Add(FVector(T.LonDeg, T.LatDeg, 0.0));
	TWeakPtr<FVegetationManager> Weak = AsWeak();
	Hooks.SampleHeights(LonLatH, [Weak, Id, Token, Instances = MoveTemp(Instances)](TArray<TOptional<double>> H) mutable
	{
		if (TSharedPtr<FVegetationManager> M = Weak.Pin()) M->OnHeights(Id, Token, MoveTemp(Instances), MoveTemp(H));
	});
}

void FVegetationManager::OnHeights(FVegCellId Id, uint32 Token, TArray<FTreeInstance> Instances, TArray<TOptional<double>> Heights)
{
	if (!IsCurrent(Id, Token, EVegCellState::Sampling)) return;
	Requests.Remove(Id);
	FVegCellSpawn S;
	CellCentre(Id, CellSizeM, S.CentreLatDeg, S.CentreLonDeg);
	for (int32 K = 0; K < Instances.Num() && K < Heights.Num(); ++K)
	{
		if (!Heights[K].IsSet() || !FMath::IsFinite(Heights[K].GetValue())) continue;
		S.Instances.Add(Instances[K]);
		S.HeightsM.Add(Heights[K].GetValue());
	}
	Streaming.OnHeightsSampled(Id, S.Instances.Num());
	if (S.Instances.Num() == 0) return;
	S.BaseHeightM = S.HeightsM[0];
	S.LocalInstances = ToCellLocal(S.CentreLatDeg, S.CentreLonDeg, S.BaseHeightM, S.Instances, S.HeightsM);
	Ready.Add(Id, MoveTemp(S));
}

namespace CamSimVegetation
{
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

	TSharedPtr<FVegetationManager> CreateLiveManager(UWorld* World, ACesium3DTileset* Terrain, const FCamSimConfig& Cfg, FString& OutWhyNot)
	{
		const FCamSimConfig::FVegetationConfig& V = Cfg.Vegetation;
		if (!V.bEnabled) { OutWhyNot = TEXT("vegetation.enabled is false"); return nullptr; }
		if (!World || !Terrain) { OutWhyNot = TEXT("no world or terrain tileset"); return nullptr; }
		TSharedPtr<FVegetationLandCover, ESPMode::ThreadSafe> LandCover = MakeShared<FVegetationLandCover, ESPMode::ThreadSafe>(Cfg.Thermal.LandCover.Dir);
		if (!LandCover->IsValid()) { OutWhyNot = FString::Printf(TEXT("no land cover (%s)"), *LandCover->GetError()); return nullptr; }
		FVegetationLibrary Library; FString LibError;
		if (!LoadLibrary(ResolveLibraryDir(V.LibraryDir), Library, LibError)) { OutWhyNot = LibError; return nullptr; }
		if (Library.Types.Num() == 0) { OutWhyNot = TEXT("the vegetation library has no types"); return nullptr; }
		const FVegetationType& Type = Library.Types[0];
		UStaticMesh* Mesh = nullptr;
#if CAMSIM_VEGETATION_PROCEDURAL
		if (Type.bProcedural) Mesh = CreateProceduralTreeMesh(Type.HeightM);
#endif
		if (!Type.bProcedural) Mesh = CamSimMeshLoader::LoadStaticMesh(Type.GltfPath);
		if (!Mesh) { OutWhyNot = FString::Printf(TEXT("could not load tree type '%s'"), *Type.Name); return nullptr; }

		FScatterParams P;
		P.CellSizeM = V.CellSizeM;
		P.DensityScale = V.DensityScale;
		const uint32 Seed = ResolveSeed(Cfg.Scene.bHasSeed, Cfg.Scene.Seed, V.Seed);
		FVegetationStreaming::FParams SP;
		SP.RadiusM = V.RadiusM;
		SP.CellSizeM = V.CellSizeM;

		TStrongObjectPtr<UStaticMesh> KeepMesh(Mesh);
		TWeakObjectPtr<UWorld> WeakWorld(World);
		TWeakObjectPtr<ACesium3DTileset> WeakTerrain(Terrain);
		TSharedRef<TMap<FVegCellId, TWeakObjectPtr<ACamSimVegetationCell>>> Actors = MakeShared<TMap<FVegCellId, TWeakObjectPtr<ACamSimVegetationCell>>>();
		const bool bShadows = V.bCastShadows;
		const float CullM = V.RadiusM;

		FVegetationHooks H;
		// Always answers (an empty land cover lookup yields no trees); the manager's weak pointer drops it after teardown.
		H.Generate = [LandCover, P, Seed](FVegCellId Id, TFunction<void(TArray<FTreeInstance>)> Done)
		{
			UE::Tasks::Launch(UE_SOURCE_LOCATION, [LandCover, P, Seed, Id, Done = MoveTemp(Done)]() mutable
			{
				TArray<FTreeInstance> T = GenerateCell(Seed, Id, P, [&LandCover](double La, double Lo) { return LandCover->Code(La, Lo); });
				AsyncTask(ENamedThreads::GameThread, [Done = MoveTemp(Done), T = MoveTemp(T)]() mutable { Done(MoveTemp(T)); });
			});
		};
		// Answers with all-unset heights when the tileset is gone or the result count is off. If the tileset actor is
		// destroyed with a request outstanding, Cesium never calls back (Cesium3DTileset.cpp: `if (!IsValid(this)) return;`):
		// the manager's StuckTicks reports the cell empty.
		H.SampleHeights = [WeakTerrain](const TArray<FVector>& LonLatH, TFunction<void(TArray<TOptional<double>>)> Done)
		{
			const int32 N = LonLatH.Num();
			ACesium3DTileset* T = WeakTerrain.Get();
			if (!T) { TArray<TOptional<double>> None; None.SetNum(N); Done(MoveTemp(None)); return; }
			T->SampleHeightMostDetailed(LonLatH, FCesiumSampleHeightMostDetailedCallback::CreateLambda(
				[N, Done = MoveTemp(Done)](ACesium3DTileset*, const TArray<FCesiumSampleHeightResult>& R, const TArray<FString>& Warnings) mutable
				{
					static bool bWarned = false;
					if (Warnings.Num() > 0 && !bWarned)
					{
						bWarned = true;
						UE_LOG(LogCamSim, Warning, TEXT("Vegetation: height sampling warning (failed samples drop their trees; logged once): %s"), *Warnings[0]);
					}
					TArray<TOptional<double>> Out;
					Out.SetNum(N);   // unset = failed
					if (R.Num() == N)
						for (int32 K = 0; K < N; ++K)
							if (R[K].SampleSuccess) Out[K] = R[K].LongitudeLatitudeHeight.Z;
					if (IsInGameThread()) { Done(MoveTemp(Out)); return; }
					AsyncTask(ENamedThreads::GameThread, [Done = MoveTemp(Done), Out = MoveTemp(Out)]() mutable { Done(MoveTemp(Out)); });
				}));
		};
		H.Spawn = [WeakWorld, KeepMesh, Actors, bShadows, CullM](FVegCellId Id, const FVegCellSpawn& S)
		{
			UWorld* W = WeakWorld.Get();
			if (!W) return;
			ACamSimVegetationCell* Cell = W->SpawnActor<ACamSimVegetationCell>();
			if (!Cell) return;
			Cell->Init(S.CentreLatDeg, S.CentreLonDeg, S.BaseHeightM, KeepMesh.Get(), S.LocalInstances, bShadows, CullM);
			Actors->Add(Id, Cell);
			const FString Dump = CVarVegDumpPath.GetValueOnGameThread();
			if (!Dump.IsEmpty())
			{
				FString Line = FString::Printf(TEXT("{\"row\":%d,\"col\":%d,\"hash\":%u,\"instances\":["), Id.Row, Id.Col, HashInstances(S.Instances));
				for (int32 K = 0; K < S.Instances.Num(); ++K)
					Line += FString::Printf(TEXT("%s[%.9f,%.9f,%.3f,%.4f,%.4f]"), K ? TEXT(",") : TEXT(""), S.Instances[K].LatDeg,
						S.Instances[K].LonDeg, S.HeightsM[K], S.Instances[K].YawDeg, S.Instances[K].Scale);
				Line += TEXT("]}\n");
				FFileHelper::SaveStringToFile(Line, *Dump, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
			}
		};
		H.Destroy = [Actors](FVegCellId Id)
		{
			TWeakObjectPtr<ACamSimVegetationCell> A;
			if (Actors->RemoveAndCopyValue(Id, A) && A.IsValid()) A->Destroy();
		};
		UE_LOG(LogCamSim, Log, TEXT("Vegetation: on (type '%s', radius %.0f m, density %.2f, shadows %d, seed %u, land cover %s)"),
			*Type.Name, V.RadiusM, V.DensityScale, bShadows ? 1 : 0, Seed, *Cfg.Thermal.LandCover.Dir);
		return MakeShared<FVegetationManager>(SP, MoveTemp(H));
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
				const TOptional<double> G = Probe.TraceHeight(T.LatDeg, T.LonDeg, H + 50.0, H - 50.0);
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
