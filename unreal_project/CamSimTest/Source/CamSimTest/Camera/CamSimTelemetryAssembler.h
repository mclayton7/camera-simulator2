// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Metadata/CamSimTelemetry.h"

class AActor;
class UWorld;
class USceneCaptureComponent2D;
class FCamSimGeospatialProvider;
struct FCigiPlatformKinematics;

/**
 * FCamSimTelemetryAssembler
 *
 * Builds the per-frame telemetry snapshot (the source of every KLV tag) for
 * ACamSimCamera from the platform pose, gimbal, sensor, field of view,
 * environment, host kinematics (CIGI packet 201), and the sensor's footprint
 * on the ground (frame centre and image corners). Game thread only; Snapshot()
 * is taken immediately before each capture.
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

	/**
	 * The host's camera state for this frame, after the pose was applied
	 * (FCigiCameraFrame). A new pose without a valid sample time drops the
	 * previous one, so Tag 2 falls back to the sim clock; kinematics fields
	 * with their valid flag set replace the held values (a velocity also
	 * replaces the ground-speed estimate). AppTimeSec is the engine's frame
	 * time (FApp::GetCurrentTime()).
	 */
	void ApplyHostFrame(bool bHasPose, const FCigiPlatformKinematics* Kinematics, double AppTimeSec);

	/** Sun elevation and the Phase 18 atmospheric snapshot from ACamSimEnvironment. */
	void ReadEnvironment(UWorld* World);

	/**
	 * The sensor's footprint on the ground. Slant range and frame centre
	 * (Tags 21, 23-25, 78): a line trace along the boresight against the
	 * terrain, or, when nothing is hit, the boresight's intersection with the
	 * ellipsoid raised to the last terrain-hit height. Image corners (Tags
	 * 82-89): the same for the four corner rays, the fallback surface being
	 * the ellipsoid at this frame's frame-centre height; a corner ray that
	 * meets neither (above the horizon) is left out. World may be null (no
	 * traces: the ellipsoid fallback only).
	 */
	void UpdateFootprint(UWorld* World, const USceneCaptureComponent2D* Sensor, const AActor* IgnoreActor,
		const FCamSimGeospatialProvider* GeoProvider);

	/** Stamp the time (Tag 2) and return the snapshot for the frame being captured. */
	FCamSimTelemetry Snapshot();
	/** Snapshot at an explicit engine frame time (tests). */
	FCamSimTelemetry Snapshot(double AppTimeSec);

	/** Held kinematics fields older than this (engine time) are dropped from the KLV. */
	static constexpr double KinematicsTimeoutSec = 1.0;

	/**
	 * The sensor boresight's orientation in the local North-East-Up frame: the
	 * platform's CIGI attitude composed with the gimbal's airframe-relative
	 * angles, exactly as the scene does it (SceneCapture's relative rotation
	 * under the globe-anchored actor). Body axes: X forward, Y right, Z up.
	 */
	static FQuat SensorToNeu(const FCamSimTelemetry& T);

	/**
	 * Camera-frame (X forward, Y right, Z up) direction of image corner Index:
	 * ST 0601 order, 0 upper left, 1 upper right, 2 lower right, 3 lower left.
	 */
	static FVector CornerDirection(int32 Index, float HFovDeg, float VFovDeg);

	/**
	 * First intersection of a ray from (Lat, Lon, Alt) along DirNeu (North,
	 * East, Up) with the WGS-84 ellipsoid raised by SurfaceHeightM. Returns
	 * false, and a zero range, when the ray misses (at or above the horizon)
	 * or starts below that surface.
	 */
	static bool IntersectEllipsoid(double Lat, double Lon, double AltM, const FVector& DirNeu, double SurfaceHeightM,
		double& OutRangeM, double& OutLat, double& OutLon);

private:
	/** Trace one ray against the scene; on a hit, its range (m) and geodetic position. */
	static bool TraceGround(UWorld* World, const FVector& StartCm, const FVector& DirWorld, const AActor* IgnoreActor,
		const FCamSimGeospatialProvider* GeoProvider, double& OutRangeM, double& OutLat, double& OutLon, double& OutAlt);

	FCamSimTelemetry Telemetry;

	/** Height (ellipsoid) of the last terrain hit: the fallback surface. */
	double GroundHeightM = 0.0;

	// Host kinematics (packet 201), with the engine time each group was last set.
	double AirspeedTimeSec        = -1.0;
	double MagneticHeadingTimeSec = -1.0;
	double VelocityTimeSec        = -1.0;

	// Tag 2 from the host: the sample time of the current pose and the engine time it was applied.
	bool   bHostSampleValid    = false;
	double HostSampleUtcSec    = 0.0;
	double HostSampleAppSec    = 0.0;
	uint64 LastTimestampUs     = 0;
};
