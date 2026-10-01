// Copyright CamSim Contributors. All Rights Reserved.

#include "Camera/CamSimRenderPath.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/Scene.h"
#include "ShowFlags.h"

void CamSimRender::RefreshPlayerView(APlayerController* PC, float DeltaSeconds)
{
	// UpdateCamera ORs any photography cut into bGameCameraCutThisFrame and
	// never clears it, so a cut requested earlier this frame survives.
	if (PC && PC->PlayerCameraManager)
	{
		PC->PlayerCameraManager->UpdateCamera(DeltaSeconds);
	}
}

void CamSimRender::DisableMotionBlur(FPostProcessSettings& PP, FEngineShowFlags& Flags)
{
	Flags.SetMotionBlur(false);
	PP.bOverride_MotionBlurAmount = true;
	PP.MotionBlurAmount = 0.0f;
}
