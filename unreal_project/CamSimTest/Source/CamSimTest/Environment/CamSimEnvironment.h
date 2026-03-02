// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CIGI/CigiPacketTypes.h"
#include "CamSimEnvironment.generated.h"

class ADirectionalLight;
class ASkyLight;
class ASkyAtmosphere;
class AExponentialHeightFog;
class AVolumetricCloud;
class ACesiumSunSky;
class UCamSimSubsystem;

/**
 * ACamSimEnvironment
 *
 * Drives UE5 environment actors (sun, sky, fog, clouds) from CIGI
 * Celestial Sphere Control, Atmosphere Control, and Weather Control packets.
 *
 * Place one instance in the level or let ACamSimGameMode spawn it.
 */
UCLASS()
class CAMSIMTEST_API ACamSimEnvironment : public AActor
{
	GENERATED_BODY()

public:
	ACamSimEnvironment();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

private:
	// Cached UE environment actors (found via TActorIterator in BeginPlay)
	UPROPERTY(Transient) TObjectPtr<ADirectionalLight>    SunLight;
	UPROPERTY(Transient) TObjectPtr<ASkyLight>            SkyLight;
	UPROPERTY(Transient) TObjectPtr<ASkyAtmosphere>       SkyAtmosphere;
	UPROPERTY(Transient) TObjectPtr<AExponentialHeightFog> HeightFog;
	UPROPERTY(Transient) TObjectPtr<AVolumetricCloud>      CloudActor;
	UPROPERTY(Transient) TObjectPtr<ACesiumSunSky>         CesiumSunSkyActor;

	UPROPERTY(Transient) TObjectPtr<UCamSimSubsystem> Subsystem;

	// Latest state from CIGI queues
	FCigiCelestialState  CurrentCelestial;
	FCigiAtmosphereState CurrentAtmosphere;
	FCigiWeatherState    CurrentWeather;

	bool bReceivedCelestial  = false;
	bool bReceivedAtmosphere = false;
	bool bReceivedWeather    = false;

	// Previous sun elevation for sky-light recapture hysteresis
	float PrevSunElevation = 0.0f;

	void ApplyCelestial();
	void ApplyAtmosphere();
	void ApplyWeather();

	/** Simplified solar position: returns (elevation, azimuth) in degrees. */
	static FVector2D ComputeSunPosition(float Hour, int32 DayOfYear, double Latitude);
};
