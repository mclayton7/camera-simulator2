// Copyright CamSim Contributors. All Rights Reserved.

#include "Camera/CamSimSensorComponent.h"
#include "CamSimTest.h"
#include "CIGI/CigiReceiver.h"
#include "Config/CamSimConfig.h"
#include "Components/SceneCaptureComponent2D.h"

UCamSimSensorComponent::UCamSimSensorComponent()
{
	PrimaryComponentTick.bCanEverTick = false;  // driven explicitly by ACamSimCamera
}

// -------------------------------------------------------------------------
// TickSensor — drain sensor control queue and update state
// -------------------------------------------------------------------------

void UCamSimSensorComponent::TickSensor(FCigiReceiver* Receiver, const FCamSimConfig& Config,
                                         USceneCaptureComponent2D* SceneCapture)
{
	if (!Receiver) return;

	FCigiSensorControl Sensor;
	while (Receiver->DequeueSensorControl(Sensor))
	{
		bSensorOn      = Sensor.bSensorOn;
		SensorPolarity = Sensor.Polarity;

		// Map SensorId → waveband (0=EO, 1=IR, 2=NVG; clamp unknown IDs to EO)
		const uint8 ClampedId = static_cast<uint8>(FMath::Clamp((int32)Sensor.SensorId, 0, 2));
		CurrentSensorMode = static_cast<ESensorMode>(ClampedId);

		// Map Gain (0.0=wide → 1.0=narrow) to configured FOV preset
		const TArray<float>& Presets = Config.SensorFovPresets;
		if (Presets.Num() > 0 && SceneCapture)
		{
			const int32 Idx = FMath::Clamp(
				FMath::FloorToInt(Sensor.Gain * Presets.Num()), 0, Presets.Num() - 1);
			const float NewFov = FMath::Clamp(Presets[Idx], 1.0f, 179.0f);
			if (NewFov != SceneCapture->FOVAngle)
			{
				SceneCapture->FOVAngle = NewFov;
				UE_LOG(LogCamSim, Log,
					TEXT("UCamSimSensorComponent: SensorCtrl gain=%.2f -> preset[%d]=%.1f°"),
					Sensor.Gain, Idx, NewFov);
			}
		}

		UE_LOG(LogCamSim, Log,
			TEXT("UCamSimSensorComponent: sensor=%u mode=%u on=%d polarity=%u gain=%.2f"),
			Sensor.SensorId, ClampedId, bSensorOn ? 1 : 0, SensorPolarity, Sensor.Gain);
	}
}
