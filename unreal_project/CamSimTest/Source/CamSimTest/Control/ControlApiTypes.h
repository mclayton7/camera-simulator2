// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Geospatial/CigiFrames.h"   // CamSimFrames::FGeoPose
#include "Sim/Commands.h"

/** The sensor part of a camera request: each field optional; unset fields keep the current sensor state. */
struct FControlSensorRequest
{
	TOptional<bool>        bOn;
	TOptional<ESensorMode> Mode;
	TOptional<uint8>       Polarity;   // 0 white hot, 1 black hot
};

/**
 * What a control API camera request sets (ROADMAP 2.4, docs/control-api.md). Each part is optional: unset parts keep
 * their current state. Applied by ACamSimCamera::ApplyControlCamera on the game thread.
 */
struct FControlCameraCommand
{
	TOptional<CamSimFrames::FGeoPose> Pose;   // platform pose (orientation in local NEU)
	TOptional<FViewCommand>           View;   // horizontal FOV and gimbal
	TOptional<FControlSensorRequest>  Sensor; // on/off, waveband, polarity (merged with the current state)
};
