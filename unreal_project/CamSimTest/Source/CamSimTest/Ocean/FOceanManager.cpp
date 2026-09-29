// Copyright CamSim Contributors. All Rights Reserved.

#include "Ocean/FOceanManager.h"
#include "Sim/Commands.h"

void FOceanManager::Init(UWorld* World, AActor* Owner, UCamSimSubsystem* InSubsystem)
{
	Subsystem = InSubsystem;
}

void FOceanManager::Tick()
{
}

void FOceanManager::ApplyWave(const FOceanWaveCommand& Cmd)
{
	// Task 4 fills this in.
}

void FOceanManager::ApplyMaritimeSurface(const FMaritimeSurfaceCommand& Cmd)
{
	// Task 4 fills this in.
}
