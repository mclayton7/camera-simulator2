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

void CamSimRender::ApplyMotionBlur(const FCamSimConfig::FOpticalRealismConfig& O, FPostProcessSettings& PP, FEngineShowFlags& Flags)
{
	const bool bOn = O.bEnabled && O.bMotionBlur;
	Flags.SetMotionBlur(bOn);
	PP.bOverride_MotionBlurAmount = true;
	PP.MotionBlurAmount = bOn ? O.MotionBlurAmount : 0.0f;
	if (bOn)
	{
		PP.bOverride_MotionBlurMax = true;
		PP.MotionBlurMax = static_cast<float>(O.MotionBlurMax);
	}
}
