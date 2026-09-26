// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Metadata/CamSimTelemetry.h"

class AActor;
class UWorld;
class USceneCaptureComponent2D;
class FCamSimGeospatialProvider;

/**
 * FCamSimTelemetryAssembler
 *
 * Builds the per-frame telemetry snapshot (the source of every KLV tag) for
 * ACamSimCamera from the platform pose, gimbal, sensor, field of view,
 * environment, and the sensor boresight's intersection with the terrain.
 * Game thread only; Snapshot() is taken immediately before each capture.
 */
class FCamSimTelemetryAssembler
{
public:
	const FCamSimTelemetry& Get() const { return Telemetry; }

	/** Platform geodetic pose (altitude above the WGS-84 ellipsoid) and ground speed. */
	void SetPlatformPose(double Lat, double Lon, double Alt, float Yaw, float Pitch, float Roll);
	void SetGroundSpeed(float Mps) { Telemetry.GroundSpeedMps = Mps; }
	void SetGimbal(float Yaw, float Pitch, float Roll);
	void SetSensor(uint8 Mode, uint8 Polarity);
	void SetFieldOfView(float HFovDeg, float VFovDeg);

	/** Sun elevation and the Phase 18 atmospheric snapshot from ACamSimEnvironment. */
	void ReadEnvironment(UWorld* World);

	/**
	 * Slant range and frame centre (Tags 21, 23-25, 78): a line trace along the
	 * sensor boresight against the terrain, or a flat-earth estimate from the
	 * platform altitude and gimbal angles when nothing is hit.
	 */
	void UpdateFrameCenter(UWorld* World, const USceneCaptureComponent2D& Sensor, const AActor* IgnoreActor,
		const FCamSimGeospatialProvider* GeoProvider);

	/** Stamp the sim time (UTC, Tag 2) and return the snapshot for the frame being captured. */
	FCamSimTelemetry Snapshot();

	/**
	 * Flat-earth frame centre for a boresight at WorldPitchDeg (negative =
	 * below the horizon) and true WorldYawDeg from a platform at AltM. Returns
	 * false, and a zero slant range, when the boresight is at or above the horizon.
	 */
	static bool FlatEarthFrameCenter(double Lat, double Lon, double AltM, float WorldPitchDeg, float WorldYawDeg,
		double& OutSlantRangeM, double& OutLat, double& OutLon);

private:
	FCamSimTelemetry Telemetry;
};
