// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "GameMode/CamSimGameMode.h"

// -------------------------------------------------------------------------
// ROADMAP 1.15: a Shipping build ignores the command-line map URL
// (UGameInstance::StartGameInstance, unless UE_ALLOW_MAP_OVERRIDE_IN_SHIPPING,
// a global define an installed engine can't set), so run.sh's and the
// entrypoint's "/Game/Main?game=…CamSimGameMode" is dropped there. The game
// mode must come from the project's GlobalDefaultGameMode, or Shipping loads
// Main with the engine's default game mode and never spawns the sensor.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameModeGlobalDefaultTest,
	"CamSim.Config.GlobalDefaultGameModeIsCamSim",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameModeGlobalDefaultTest::RunTest(const FString& Parameters)
{
	FString GameMode;
	GConfig->GetString(TEXT("/Script/EngineSettings.GameMapsSettings"), TEXT("GlobalDefaultGameMode"), GameMode,
		GEngineIni);
	TestEqual(TEXT("GlobalDefaultGameMode (DefaultEngine.ini)"), GameMode,
		ACamSimGameMode::StaticClass()->GetPathName());
	return true;
}
