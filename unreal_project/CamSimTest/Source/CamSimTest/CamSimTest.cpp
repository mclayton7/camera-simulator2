// Copyright Epic Games, Inc. All Rights Reserved.

#include "CamSimTest.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogCamSim);

void FCamSimModule::StartupModule()
{
	UE_LOG(LogCamSim, Log, TEXT("CamSim module started"));
}

void FCamSimModule::ShutdownModule()
{
	UE_LOG(LogCamSim, Log, TEXT("CamSim module shut down"));
}

IMPLEMENT_PRIMARY_GAME_MODULE(FCamSimModule, CamSimTest, "CamSimTest");
