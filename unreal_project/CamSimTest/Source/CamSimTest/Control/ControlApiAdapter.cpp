// Copyright CamSim Contributors. All Rights Reserved.

#include "Control/ControlApiAdapter.h"
#include "Control/ControlApiJson.h"
#include "Sim/CommandSink.h"

bool FControlApiAdapter::PutEntity(const CamSimControl::FEntityRequest& Req, FString& OutError)
{
	FState* Existing = Entities.Find(Req.Id);
	if (!Existing)
	{
		if (!Req.TypeId)
		{
			OutError = FString::Printf(TEXT("entity %u is new: type_id is required"), Req.Id);
			return false;
		}
		if (!Req.Pose)
		{
			OutError = FString::Printf(TEXT("entity %u is new: lat, lon and alt are required"), Req.Id);
			return false;
		}
	}
	FState& S = Existing ? *Existing : Entities.Add(Req.Id);
	if (Req.TypeId)  S.TypeId  = *Req.TypeId;
	if (Req.Pose)    S.Pose    = *Req.Pose;
	if (Req.Surface) S.Surface = *Req.Surface;
	if (Req.bHidden) S.bHidden = *Req.bHidden;
	PendingRemovals.Remove(Req.Id);
	for (FComponentCommand C : Req.Components)
	{
		C.Key = KeyFor(Req.Id);
		PendingComponents.Add(C);
	}
	NoteCommand(GFrameCounter);
	return true;
}

bool FControlApiAdapter::RemoveEntity(uint16 Id)
{
	if (Entities.Remove(Id) == 0) return false;
	PendingRemovals.AddUnique(Id);
	PendingComponents.RemoveAll([Id](const FComponentCommand& C) { return C.Key == KeyFor(Id); });
	NoteCommand(GFrameCounter);
	return true;
}

void FControlApiAdapter::RemoveAll()
{
	for (const auto& Pair : Entities) PendingRemovals.AddUnique(Pair.Key);
	Entities.Reset();
	PendingComponents.Reset();
	NoteCommand(GFrameCounter);
}

void FControlApiAdapter::Poll(ISimCommandSink& Sink)
{
	for (const uint16 Id : PendingRemovals)
	{
		FEntityCommand C;
		C.Key = KeyFor(Id);
		C.Lifecycle = EEntityLifecycle::Remove;
		Sink.Submit(C);
	}
	PendingRemovals.Reset();

	for (const auto& Pair : Entities)
	{
		const FState& S = Pair.Value;
		FEntityCommand C;
		C.Key         = KeyFor(Pair.Key);
		C.Lifecycle   = S.bHidden ? EEntityLifecycle::Hidden : EEntityLifecycle::Active;
		C.TypeId      = S.TypeId;
		C.Pose        = S.Pose;
		C.SurfaceMode = S.Surface;
		Sink.Submit(C);
	}

	// After the entity commands, so a component set with a new entity finds it spawned.
	for (const FComponentCommand& C : PendingComponents) Sink.Submit(C);
	PendingComponents.Reset();
}

TArray<uint16> FControlApiAdapter::GetIds() const
{
	TArray<uint16> Ids;
	Entities.GetKeys(Ids);
	Ids.Sort();
	return Ids;
}

uint16 FControlApiAdapter::GetTypeId(uint16 Id) const
{
	const FState* S = Entities.Find(Id);
	return S ? S->TypeId : 0;
}
