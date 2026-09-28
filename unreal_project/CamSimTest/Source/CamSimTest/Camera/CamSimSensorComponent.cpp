// Copyright CamSim Contributors. All Rights Reserved.

#include "Camera/CamSimSensorComponent.h"
#include "CamSimTest.h"
#include "CIGI/CigiPacketTypes.h"
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
		ApplySensorControl(Sensor, Config, SceneCapture);
	}
}

float UCamSimSensorComponent::ApplySensorControl(const FCigiSensorControl& Sensor,
                                                  const FCamSimConfig& Config,
                                                  USceneCaptureComponent2D* SceneCapture)
{
	bSensorOn      = Sensor.bSensorOn;
	SensorPolarity = Sensor.Polarity;

	// Map SensorId → waveband: 0 EO, 1 IR; anything else defaults to EO (NVG,
	// formerly SensorId 2, was removed — ROADMAP 3B.2).
	if (Sensor.SensorId == 1)
	{
		CurrentSensorMode = ESensorMode::IR;
	}
	else
	{
		CurrentSensorMode = ESensorMode::EO;
		if (Sensor.SensorId != 0)
		{
			static bool bWarnedUnmappedSensorId = false;
			if (!bWarnedUnmappedSensorId)
			{
				bWarnedUnmappedSensorId = true;
				UE_LOG(LogCamSim, Warning,
					TEXT("UCamSimSensorComponent: SensorId %u has no EO/IR mapping (NVG, formerly SensorId 2, was removed) — defaulting to EO"),
					Sensor.SensorId);
			}
		}
	}

	float AppliedFov = SceneCapture ? SceneCapture->FOVAngle : 0.0f;

	// Map Gain (0.0=wide → 1.0=narrow) to configured FOV preset. Hosts resend
	// Sensor Control every frame, so only a gain that selects a different
	// preset changes the FOV; otherwise a View Definition FOV would be
	// overwritten every frame.
	const TArray<float>& Presets = Config.SensorFovPresets;
	if (Presets.Num() > 0 && SceneCapture)
	{
		const int32 Idx = FMath::Clamp(
			FMath::FloorToInt(Sensor.Gain * Presets.Num()), 0, Presets.Num() - 1);
		if (Idx != LastPresetIdx)
		{
			LastPresetIdx = Idx;
			SceneCapture->FOVAngle = FMath::Clamp(Presets[Idx], 1.0f, 179.0f);
			UE_LOG(LogCamSim, Log,
				TEXT("UCamSimSensorComponent: SensorCtrl gain=%.2f -> preset[%d]=%.1f°"),
				Sensor.Gain, Idx, SceneCapture->FOVAngle);
		}
		AppliedFov = SceneCapture->FOVAngle;
	}

	UE_LOG(LogCamSim, Log,
		TEXT("UCamSimSensorComponent: sensor=%u mode=%u on=%d polarity=%u gain=%.2f"),
		Sensor.SensorId, static_cast<uint8>(CurrentSensorMode), bSensorOn ? 1 : 0, SensorPolarity, Sensor.Gain);

	return AppliedFov;
}
