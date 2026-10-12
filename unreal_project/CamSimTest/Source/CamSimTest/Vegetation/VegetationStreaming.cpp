// Copyright CamSim Contributors. All Rights Reserved.

#include "Vegetation/VegetationStreaming.h"

using namespace CamSimVegetation;

FVegetationStreaming::FActions FVegetationStreaming::Tick(double CamLatDeg, double CamLonDeg)
{
	FActions A;
	auto DistTo = [&](FVegCellId Id) { double La, Lo; CellCentre(Id, Params.CellSizeM, La, Lo); return GroundDistanceM(CamLatDeg, CamLonDeg, La, Lo); };

	const double DropM = Params.RadiusM * Params.DropFactor;
	for (auto It = Cells.CreateIterator(); It; ++It)
	{
		if (DistTo(It.Key()) > DropM)
		{
			if (It.Value() == EVegCellState::Spawned) A.ToDrop.Add(It.Key());
			It.RemoveCurrent();
		}
	}

	TArray<TPair<double, FVegCellId>> Ready;
	for (const auto& Kv : Cells) if (Kv.Value == EVegCellState::ReadyToSpawn) Ready.Emplace(DistTo(Kv.Key), Kv.Key);
	Ready.Sort([](const auto& X, const auto& Y)
	{
		if (X.Key != Y.Key) return X.Key < Y.Key;
		return X.Value.Row != Y.Value.Row ? X.Value.Row < Y.Value.Row : X.Value.Col < Y.Value.Col;
	});
	for (int32 I = 0; I < Ready.Num() && I < Params.MaxSpawnsPerTick; ++I)
	{
		Cells[Ready[I].Value] = EVegCellState::Spawned;
		A.ToSpawn.Add(Ready[I].Value);
	}

	int32 InFlight = NumInFlight();
	if (InFlight < Params.MaxCellsInFlight)
	{
		for (const FVegCellId& Id : CellsWithin(CamLatDeg, CamLonDeg, Params.RadiusM, Params.CellSizeM))
		{
			if (InFlight >= Params.MaxCellsInFlight) break;
			if (Cells.Contains(Id)) continue;
			Cells.Add(Id, EVegCellState::Generating);
			A.ToGenerate.Add(Id);
			++InFlight;
		}
	}
	return A;
}

FVegetationStreaming::FActions FVegetationStreaming::TickWanted(TConstArrayView<FVegCellId> Wanted, TFunctionRef<bool(FVegCellId)> MayDrop,
	int32 InFlightCap, int32 SpawnCap)
{
	FActions A;
	const TSet<FVegCellId> WantedSet(Wanted);
	for (auto It = Cells.CreateIterator(); It; ++It)
	{
		if (WantedSet.Contains(It.Key()) || !MayDrop(It.Key())) continue;
		if (It.Value() == EVegCellState::Spawned) A.ToDrop.Add(It.Key());
		It.RemoveCurrent();
	}
	int32 InFlight = NumInFlight();
	for (const FVegCellId& Id : Wanted)
	{
		EVegCellState* S = Cells.Find(Id);
		if (S && *S == EVegCellState::ReadyToSpawn && A.ToSpawn.Num() < SpawnCap)
		{
			*S = EVegCellState::Spawned;
			A.ToSpawn.Add(Id);
		}
		else if (!S && InFlight < InFlightCap)
		{
			Cells.Add(Id, EVegCellState::Generating);
			A.ToGenerate.Add(Id);
			++InFlight;
		}
	}
	return A;
}

bool FVegetationStreaming::IsSettled(FVegCellId Id) const
{
	const EVegCellState* S = Cells.Find(Id);
	return S && (*S == EVegCellState::Spawned || *S == EVegCellState::Empty);
}

void FVegetationStreaming::OnGenerated(FVegCellId Id, int32 NumInstances)
{
	EVegCellState* S = Cells.Find(Id);
	if (S && *S == EVegCellState::Generating) *S = NumInstances > 0 ? EVegCellState::Sampling : EVegCellState::Empty;
}

void FVegetationStreaming::OnHeightsSampled(FVegCellId Id, int32 NumGrounded)
{
	EVegCellState* S = Cells.Find(Id);
	if (S && *S == EVegCellState::Sampling) *S = NumGrounded > 0 ? EVegCellState::ReadyToSpawn : EVegCellState::Empty;
}

TOptional<EVegCellState> FVegetationStreaming::GetState(FVegCellId Id) const
{
	const EVegCellState* S = Cells.Find(Id);
	return S ? TOptional<EVegCellState>(*S) : TOptional<EVegCellState>();
}

int32 FVegetationStreaming::NumInFlight() const
{
	int32 N = 0;
	for (const auto& Kv : Cells) N += Kv.Value == EVegCellState::Generating || Kv.Value == EVegCellState::Sampling;
	return N;
}
