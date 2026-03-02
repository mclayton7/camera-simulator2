// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "CamSimGameMode.generated.h"

/**
 * ACamSimGameMode
 *
 * Minimal game mode for the synthetic sensor simulator.
 * Fixed 30 fps is enforced via DefaultEngine.ini (bUseFixedFrameRate / FixedFrameRate).
 * Audio is disabled via -nosound command-line arg and by nullifying the sound class.
 */
UCLASS()
class CAMSIMTEST_API ACamSimGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ACamSimGameMode();

	virtual void BeginPlay() override;
};
