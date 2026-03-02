// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Sensor/SensorTypes.h"   // ESensorMode
#include "CamSimSensorComponent.generated.h"

class FCigiReceiver;
class USceneCaptureComponent2D;
struct FCamSimConfig;

/**
 * UCamSimSensorComponent
 *
 * Owns sensor state (on/off, waveband, polarity, FOV preset) for ACamSimCamera.
 * Each Tick it drains FCigiReceiver::SensorCtrlQueue (opcode 17).
 *
 * Drives SceneCaptureComponent2D::FOVAngle when the gain field in a
 * SensorControl packet selects a new preset from FCamSimConfig::SensorFovPresets.
 */
UCLASS(ClassGroup=CamSim, meta=(BlueprintSpawnableComponent))
class CAMSIMTEST_API UCamSimSensorComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCamSimSensorComponent();

	/**
	 * Drain sensor control queue and update internal state.
	 * Pass the SceneCapture component so FOV changes are applied immediately.
	 * Call from ACamSimCamera::ApplyCigiState() each game tick.
	 */
	void TickSensor(FCigiReceiver* Receiver, const FCamSimConfig& Config,
	                USceneCaptureComponent2D* SceneCapture);

	bool        IsOn()       const { return bSensorOn; }
	uint8       GetPolarity() const { return SensorPolarity; }
	ESensorMode GetMode()    const { return CurrentSensorMode; }

private:
	bool        bSensorOn          = true;
	uint8       SensorPolarity     = 0;   // 0 = WhiteHot, 1 = BlackHot (IR only)
	ESensorMode CurrentSensorMode  = ESensorMode::EO;
};
