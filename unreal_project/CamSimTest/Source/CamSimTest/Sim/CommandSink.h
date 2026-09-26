// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Sim/Commands.h"

/**
 * Where host adapters deliver canonical commands, on the game thread, during
 * the entity manager's ordered pass. Commands take effect immediately.
 */
class ISimCommandSink
{
public:
	virtual ~ISimCommandSink() = default;

	virtual void Submit(const FEntityCommand& Command) = 0;
	virtual void Submit(const FEntityMotionCommand& Command) = 0;
	virtual void Submit(const FArticulationCommand& Command) = 0;
	virtual void Submit(const FComponentCommand& Command) = 0;
};
