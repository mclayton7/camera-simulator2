// Copyright CamSim Contributors. All Rights Reserved.

#include "Vegetation/VegetationFarTier.h"

#include "Vegetation/VegetationManager.h"

using namespace CamSimVegetation;

FVegetationFarTier::FVegetationFarTier(int32 InBlockCells, double InCellSizeM, FVegetationFarHooks InHooks,
	TFunction<void(const TCHAR*, FVegBlockId)> InNotify)
	: BlockCells(InBlockCells), CellSizeM(InCellSizeM), Hooks(MoveTemp(InHooks)), NotifyFn(MoveTemp(InNotify)), Streaming({})
{
}

FVegetationFarTier::~FVegetationFarTier()
{
	for (const auto& Kv : Spawned) if (Hooks.Destroy) Hooks.Destroy(Kv.Key);
}

void FVegetationFarTier::Tick(TConstArrayView<FVegBlockId> Wanted, TFunctionRef<bool(FVegBlockId)> MayDrop, int32 InFlightCap,
	int32 SpawnCap, int64 TickCount)
{
	Now = TickCount;
	for (auto It = Requests.CreateIterator(); It; ++It)
	{
		if (Now - It.Value().StartTick <= StuckTicks) continue;
		const TOptional<EVegCellState> S = Streaming.GetState(It.Key());
		if (S == TOptional<EVegCellState>(EVegCellState::Generating)) Streaming.OnGenerated(It.Key(), 0);
		else if (S == TOptional<EVegCellState>(EVegCellState::Sampling)) Streaming.OnHeightsSampled(It.Key(), 0);
		It.RemoveCurrent();
	}
	const FVegetationStreaming::FActions A = Streaming.TickWanted(Wanted, MayDrop, InFlightCap, FMath::Max(SpawnCap, 0));
	for (const FVegBlockId& B : A.ToDrop)
	{
		Spawned.Remove(B);
		Hooks.Destroy(B);
		Notify(TEXT("destroy"), B);
	}
	for (auto It = Ready.CreateIterator(); It; ++It) if (!Streaming.GetState(It.Key()).IsSet()) It.RemoveCurrent();
	for (auto It = Requests.CreateIterator(); It; ++It) if (!Streaming.GetState(It.Key()).IsSet()) It.RemoveCurrent();
	SpawnedLastTick = 0;
	for (const FVegBlockId& B : A.ToSpawn)
	{
		FVegFarSpawn S;
		if (Ready.RemoveAndCopyValue(B, S))
		{
			Hooks.Spawn(B, S);
			Spawned.Add(B, MoveTemp(S));
			++SpawnedLastTick;
			Notify(TEXT("spawn"), B);
		}
	}
	for (const FVegBlockId& B : A.ToGenerate)
	{
		const uint32 Token = NextToken++;
		Requests.Add(B, FRequest{ Token, Now });
		TWeakPtr<FVegetationFarTier> Weak = AsWeak();
		Hooks.Generate(B, [Weak, B, Token](TArray<FFarTree> T)
		{
			if (TSharedPtr<FVegetationFarTier> F = Weak.Pin()) F->OnGenerated(B, Token, MoveTemp(T));
		});
	}
}

bool FVegetationFarTier::IsCurrent(FVegBlockId B, uint32 Token, EVegCellState Stage) const
{
	const FRequest* R = Requests.Find(B);
	return R && R->Token == Token && Streaming.GetState(B) == TOptional<EVegCellState>(Stage);
}

void FVegetationFarTier::OnGenerated(FVegBlockId B, uint32 Token, TArray<FFarTree> Trees)
{
	if (!IsCurrent(B, Token, EVegCellState::Generating)) return;
	Streaming.OnGenerated(B, Trees.Num());
	if (Trees.Num() == 0)
	{
		Requests.Remove(B);
		Notify(TEXT("empty"), B);
		return;
	}
	FRequest& R = Requests[B];
	R.StartTick = Now;
	R.Trees = MoveTemp(Trees);
	R.Grid = FFarHeightGrid::ForBlock(B, BlockCells, CellSizeM, GridSpacingM);
	const TArray<FVector> Points = R.Grid.LonLatPoints();
	TWeakPtr<FVegetationFarTier> Weak = AsWeak();
	Hooks.SampleHeights(Points, [Weak, B, Token](TArray<TOptional<double>> H)
	{
		if (TSharedPtr<FVegetationFarTier> F = Weak.Pin()) F->OnHeights(B, Token, MoveTemp(H));
	});
}

void FVegetationFarTier::OnHeights(FVegBlockId B, uint32 Token, TArray<TOptional<double>> Heights)
{
	if (!IsCurrent(B, Token, EVegCellState::Sampling)) return;
	FRequest R;
	Requests.RemoveAndCopyValue(B, R);
	R.Grid.H = MoveTemp(Heights);
	FVegFarSpawn S;
	BlockCentre(B, BlockCells, CellSizeM, S.CentreLatDeg, S.CentreLonDeg);
	TArray<FTreeInstance> Kept;
	for (const FFarTree& T : R.Trees)
	{
		const TOptional<double> H = R.Grid.Sample(T.Tree.LatDeg, T.Tree.LonDeg);
		if (!H.IsSet()) continue;
		S.Trees.Add(T);
		S.HeightsM.Add(*H - SinkM);
		Kept.Add(T.Tree);
	}
	Streaming.OnHeightsSampled(B, S.Trees.Num());
	if (S.Trees.Num() == 0)
	{
		Notify(TEXT("empty"), B);
		return;
	}
	S.BaseHeightM = S.HeightsM[0];
	S.Local = ToCellLocal(S.CentreLatDeg, S.CentreLonDeg, S.BaseHeightM, Kept, S.HeightsM);
	Ready.Add(B, MoveTemp(S));
}
