// Copyright CamSim Contributors. All Rights Reserved.

#include "Camera/CamSimTelemetryAssembler.h"
#include "Environment/CamSimEnvironment.h"
#include "Geospatial/CigiFrames.h"
#include "Geospatial/CamSimGeospatialProvider.h"
#include "Time/SimClock.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"

void FCamSimTelemetryAssembler::SetPlatformPose(double Lat, double Lon, double Alt, float Yaw, float Pitch, float Roll)
{
	Telemetry.Latitude  = Lat;
	Telemetry.Longitude = Lon;
	Telemetry.Altitude  = Alt;
	Telemetry.Yaw       = Yaw;
	Telemetry.Pitch     = Pitch;
	Telemetry.Roll      = Roll;
}

void FCamSimTelemetryAssembler::SetGimbal(float Yaw, float Pitch, float Roll)
{
	Telemetry.GimbalYaw   = Yaw;
	Telemetry.GimbalPitch = Pitch;
	Telemetry.GimbalRoll  = Roll;
}

void FCamSimTelemetryAssembler::SetSensor(uint8 Mode, uint8 Polarity)
{
	Telemetry.SensorMode     = Mode;
	Telemetry.SensorPolarity = Polarity;
}

void FCamSimTelemetryAssembler::SetFieldOfView(float HFovDeg, float VFovDeg)
{
	Telemetry.HFovDeg = HFovDeg;
	Telemetry.VFovDeg = VFovDeg;
}

void FCamSimTelemetryAssembler::ReadEnvironment(UWorld* World)
{
	if (!World) return;
	TActorIterator<ACamSimEnvironment> It(World);
	if (!It) return;

	Telemetry.SunElevationDeg = It->GetSunElevationDeg();

	const ACamSimEnvironment::FAtmosphericSnapshot Snap = It->GetAtmosphericSnapshot();
	Telemetry.AtmosphericVisibilityM = Snap.AtmosphericVisibilityM;
	Telemetry.RelativeHumidity       = Snap.RelativeHumidity;
	Telemetry.AirTempCelsius         = Snap.AirTempCelsius;
	Telemetry.WeatherSeverity        = Snap.WeatherSeverity;
	Telemetry.WeatherPrecipType      = Snap.WeatherPrecipType;

	// 18L: the environment blends weather zones around the camera position.
	It->SetCameraPosition(Telemetry.Latitude, Telemetry.Longitude);
}

void FCamSimTelemetryAssembler::UpdateFrameCenter(UWorld* World, const USceneCaptureComponent2D& Sensor,
	const AActor* IgnoreActor, const FCamSimGeospatialProvider* GeoProvider)
{
	if (World)
	{
		const FVector RayStart = Sensor.GetComponentLocation();
		const FVector RayEnd   = RayStart + Sensor.GetForwardVector() * 500'000'000.0f;  // 5000 km in cm

		FHitResult Hit;
		FCollisionQueryParams Params(NAME_None, /*bTraceComplex=*/false);
		Params.AddIgnoredActor(IgnoreActor);
		if (World->LineTraceSingleByChannel(Hit, RayStart, RayEnd, ECC_Visibility, Params))
		{
			Telemetry.SlantRangeM = static_cast<double>(Hit.Distance) / 100.0;  // cm -> m
			double HitLat = 0.0, HitLon = 0.0, HitAlt = 0.0;
			if (GeoProvider && GeoProvider->WorldToGeo(World, Hit.Location, HitLat, HitLon, HitAlt))
			{
				Telemetry.FrameCenterLat  = HitLat;
				Telemetry.FrameCenterLon  = HitLon;
				Telemetry.FrameCenterElev = HitAlt;
			}
			return;
		}
	}

	// Nothing hit (boresight above the horizon, or no terrain loaded there).
	double SlantRangeM = 0.0, FcLat = 0.0, FcLon = 0.0;
	if (FlatEarthFrameCenter(Telemetry.Latitude, Telemetry.Longitude, Telemetry.Altitude,
		Telemetry.Pitch + Telemetry.GimbalPitch, Telemetry.Yaw + Telemetry.GimbalYaw, SlantRangeM, FcLat, FcLon))
	{
		Telemetry.FrameCenterLat = FcLat;
		Telemetry.FrameCenterLon = FcLon;
	}
	Telemetry.SlantRangeM = SlantRangeM;
}

FCamSimTelemetry FCamSimTelemetryAssembler::Snapshot()
{
	Telemetry.TimestampUs = FSimClock::Get().NowMicros();
	return Telemetry;
}

bool FCamSimTelemetryAssembler::FlatEarthFrameCenter(double Lat, double Lon, double AltM,
	float WorldPitchDeg, float WorldYawDeg, double& OutSlantRangeM, double& OutLat, double& OutLon)
{
	const float DepressionDeg = -WorldPitchDeg;
	if (DepressionDeg <= 0.0f)
	{
		OutSlantRangeM = 0.0;
		return false;
	}

	const double DepressRad = FMath::DegreesToRadians(static_cast<double>(DepressionDeg));
	const double AzimuthRad = FMath::DegreesToRadians(static_cast<double>(WorldYawDeg));
	OutSlantRangeM = AltM / FMath::Sin(DepressRad);
	const double GroundM = AltM / FMath::Tan(DepressRad);

	double Alt;
	CamSimFrames::OffsetGeodetic(Lat, Lon, AltM,
		FVector(GroundM * FMath::Cos(AzimuthRad), GroundM * FMath::Sin(AzimuthRad), -AltM), OutLat, OutLon, Alt);
	return true;
}
