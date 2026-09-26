// Copyright CamSim Contributors. All Rights Reserved.

#include "Hosts/CigiHostAdapter.h"
#include "Hosts/CigiCommands.h"
#include "CIGI/CigiReceiver.h"
#include "Sim/CommandSink.h"

void FCigiHostAdapter::PollEntities(ISimCommandSink& Sink)
{
	if (!Receiver) return;

	// Entity Control: the latest state per entity this frame.
	TMap<uint16, FCigiEntityState> Latest;
	FCigiEntityState State;
	while (Receiver->DequeueEntityState(State))
	{
		Latest.Add(State.EntityId, State);
	}
	for (const TPair<uint16, FCigiEntityState>& Pair : Latest)
	{
		Sink.Submit(CamSim::Cigi::ToEntityCommand(Pair.Value));
	}

	FCigiConfClampEntityState Clamp;
	while (Receiver->DequeueConfClampEntity(Clamp))
	{
		Sink.Submit(CamSim::Cigi::ToEntityCommand(Clamp));
	}

	FCigiRateControl Rate;
	while (Receiver->DequeueRateControl(Rate))
	{
		if (const TOptional<FEntityMotionCommand> Motion = CamSim::Cigi::ToMotionCommand(Rate))
		{
			Sink.Submit(*Motion);
		}
	}

	FCigiArtPartControl Art;
	while (Receiver->DequeueArtPart(Art))
	{
		Sink.Submit(CamSim::Cigi::ToArticulationCommand(Art));
	}

	FCigiComponentControl Comp;
	while (Receiver->DequeueCompCtrl(Comp))
	{
		Sink.Submit(CamSim::Cigi::ToComponentCommand(Comp));
	}
}
