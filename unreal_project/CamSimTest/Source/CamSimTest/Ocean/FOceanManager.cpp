// Copyright CamSim Contributors. All Rights Reserved.

#include "Ocean/FOceanManager.h"
#include "Sim/Commands.h"
#include "Ocean/OceanCommands.h"
#include "Ocean/OceanSurface.h"
#include "Ocean/OceanWaves.h"
#include "Subsystem/CamSimSubsystem.h"
#include "CamSimTest.h"

void FOceanManager::Init(UWorld* World, AActor* Owner, UCamSimSubsystem* InSubsystem)
{
	Subsystem = InSubsystem;
}

void FOceanManager::Tick()
{
}

void FOceanManager::ApplyWave(const FOceanWaveCommand& Cmd)
{
	FOceanSurface* Ocean = Subsystem ? Subsystem->GetOceanSurface() : nullptr;
	if (!Ocean) return;
	if (Cmd.Scope != FWeatherCommand::EScope::Global)
	{
		if (!bWarnedScopedWave) { bWarnedScopedWave = true; UE_LOG(LogCamSim, Warning, TEXT("Ocean: regional/entity Wave Control ignored (Global only)")); }
		return;
	}
	if (Cmd.WaveId >= FOceanWaves::MaxWaves)
	{
		if (!bWarnedWaveId) { bWarnedWaveId = true; UE_LOG(LogCamSim, Warning, TEXT("Ocean: Wave ID %u ignored (0-3 supported)"), Cmd.WaveId); }
		return;
	}
	Ocean->SetHostWave(Cmd.WaveId, CamSimOcean::ToOceanWave(Cmd));
}

void FOceanManager::ApplyMaritimeSurface(const FMaritimeSurfaceCommand& Cmd)
{
	FOceanSurface* Ocean = Subsystem ? Subsystem->GetOceanSurface() : nullptr;
	if (!Ocean || !Cmd.bEnabled) return;
	if (Cmd.Scope != FWeatherCommand::EScope::Global)
	{
		if (!bWarnedScopedMaritime) { bWarnedScopedMaritime = true; UE_LOG(LogCamSim, Warning, TEXT("Ocean: regional/entity Maritime Surface Conditions ignored (Global only)")); }
		return;
	}
	Ocean->SetTideOffsetM(Cmd.SurfaceHeightM);
	Ocean->SetClarity(Cmd.Clarity);
	Ocean->SetWaterTempC(Cmd.WaterTempC);
}
