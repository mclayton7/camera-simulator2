// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FCigiReceiver;
class ISimCommandSink;

/**
 * FCigiHostAdapter
 *
 * The CIGI host's side of the simulation: drains FCigiReceiver's queues and
 * submits canonical commands (Hosts/CigiCommands.h). Entity traffic so far;
 * the camera, environment and queries move here in later phases (ROADMAP 2.3).
 */
class FCigiHostAdapter
{
public:
	explicit FCigiHostAdapter(FCigiReceiver* InReceiver) : Receiver(InReceiver) {}

	/** Entity Control (latest per entity), conformal clamp, rate, articulated part and component control. */
	void PollEntities(ISimCommandSink& Sink);

private:
	FCigiReceiver* Receiver = nullptr;
};
