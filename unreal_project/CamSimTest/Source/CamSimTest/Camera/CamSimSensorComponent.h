// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Sensor/SensorTypes.h"   // ESensorMode
#include "CamSimSensorComponent.generated.h"

class USceneCaptureComponent2D;
struct FCamSimConfig;
struct FCigiSensorControl;

/**
 * UCamSimSensorComponent
 *
 * Owns sensor state (on/off, waveband, polarity, FOV preset) for ACamSimCamera.
 * Each tick it applies the frame's Sensor Control packets (opcode 17).
 *
 * Drives SceneCaptureComponent2D::FOVAngle when the gain field in a
 * SensorControl packet selects a new preset from FCamSimConfig::SensorFovPresets
 * (a change of preset only; View Definition owns the FOV otherwise).
 */
UCLASS(ClassGroup=CamSim, meta=(BlueprintSpawnableComponent))
class CAMSIMTEST_API UCamSimSensorComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCamSimSensorComponent();

	/**
	 * Apply this frame's Sensor Control packets (oldest first). Pass the
	 * SceneCapture component so FOV changes are applied immediately.
	 * Call from ACamSimCamera each game tick.
	 */
	void TickSensor(TConstArrayView<FCigiSensorControl> SensorControls, const FCamSimConfig& Config,
	                USceneCaptureComponent2D* SceneCapture);

	/**
	 * Apply a single SensorControl packet. Updates on/off, polarity, mode and
	 * (if SceneCapture is non-null) selects the FOV preset indexed by Gain,
	 * but only when Gain selects a different preset than the last packet did,
	 * so a resent gain doesn't override a View Definition FOV.
	 *
	 * Returns the SceneCapture's FOV after the packet (unchanged if no presets
	 * are configured or the preset didn't change). Exposed so unit tests can drive the
	 * component without an FCigiReceiver.
	 *
	 * Logs at Log level only when the sensor id, on/off or polarity changes
	 * (hosts resend Sensor Control, up to every frame); otherwise Verbose.
	 */
	float ApplySensorControl(const FCigiSensorControl& Sensor, const FCamSimConfig& Config,
	                         USceneCaptureComponent2D* SceneCapture);

	bool        IsOn()       const { return bSensorOn; }
	uint8       GetPolarity() const { return SensorPolarity; }
	ESensorMode GetMode()    const { return CurrentSensorMode; }

private:
	bool        bSensorOn          = true;
	uint8       SensorPolarity     = 0;   // 0 = WhiteHot, 1 = BlackHot (IR only)
	ESensorMode CurrentSensorMode  = ESensorMode::EO;
	int32       LastPresetIdx      = INDEX_NONE;   // FOV preset the gain last selected

	// Last Sensor Control logged at Log level (sensor id, on, polarity); -1 = none yet.
	int32       LastLoggedSensorId = -1;
	bool        bLastLoggedOn      = true;
	uint8       LastLoggedPolarity = 0;
};
