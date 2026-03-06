// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * FCamSimTelemetry
 *
 * Per-frame telemetry snapshot consumed by FKlvBuilder to produce MISB ST 0601 KLV.
 */
struct FCamSimTelemetry
{
	uint64 TimestampUs = 0;    // POSIX microseconds (UTC)

	double Latitude    = 0.0;  // WGS-84 decimal degrees
	double Longitude   = 0.0;  // WGS-84 decimal degrees
	double Altitude    = 0.0;  // metres above WGS-84 ellipsoid

	float  Yaw         = 0.0f; // degrees [0, 360) — platform heading
	float  Pitch       = 0.0f; // degrees — platform elevation angle
	float  Roll        = 0.0f; // degrees — platform roll

	float  HFovDeg     = 60.0f; // degrees — horizontal field of view
	float  VFovDeg     = 33.75f;// degrees — vertical field of view (default 16:9)

	// Gimbal angles relative to platform body frame (0 = boresighted to platform)
	float  GimbalYaw   = 0.0f; // degrees — gimbal azimuth offset from platform heading
	float  GimbalPitch = 0.0f; // degrees — gimbal elevation offset
	float  GimbalRoll  = 0.0f; // degrees — gimbal roll offset

	// Geometric line-of-sight outputs (computed from platform pose + gimbal angles)
	double SlantRangeM    = 0.0;  // metres — Tag 23 (0 = sensor at/above horizon)
	double FrameCenterLat = 0.0;  // WGS-84 decimal degrees — Tag 24
	double FrameCenterLon = 0.0;  // WGS-84 decimal degrees — Tag 25

	// Active sensor state snapshot (for optional sidecar ground-truth output).
	uint8 SensorMode      = 0;    // 0=EO, 1=IR, 2=NVG
	uint8 SensorPolarity  = 0;    // 0=white-hot, 1=black-hot (IR)
};

/**
 * FKlvBuilder
 *
 * Stateless helper that encodes an FCamSimTelemetry into a MISB ST 0601
 * KLV Local Set byte buffer ready to be written to an FFmpeg data-stream packet.
 *
 * Tags implemented:
 *   Tag  1  – Checksum (CRC-16/CCITT)
 *   Tag  2  – UNIX Time Stamp (μs, 8 bytes)
 *   Tag 13  – Sensor Latitude  (4-byte fixed-point)
 *   Tag 14  – Sensor Longitude (4-byte fixed-point)
 *   Tag 15  – Sensor True Altitude (2-byte fixed-point, metres)
 *   Tag 18  – Sensor Horizontal Field of View (2-byte fixed-point)
 *   Tag 19  – Sensor Vertical Field of View   (2-byte fixed-point)
 *   Tag 20  – Sensor Relative Azimuth Angle   (4-byte fixed-point, gimbal yaw)
 *   Tag 21  – Sensor Relative Elevation Angle (4-byte fixed-point, gimbal pitch)
 *   Tag 22  – Sensor Relative Roll Angle      (4-byte fixed-point, gimbal roll)
 *   Tag 23  – Slant Range                     (8-byte uint64, IMAPB 0..5000000 m)
 *   Tag 24  – Frame Center Latitude           (4-byte fixed-point, same as Tag 13)
 *   Tag 25  – Frame Center Longitude          (4-byte fixed-point, same as Tag 14)
 *   Tag 26  – Sensor Relative Azimuth Angle   (4-byte fixed-point, duplicate for compat)
 *   Tag 27  – Sensor Relative Elevation Angle (4-byte fixed-point, duplicate for compat)
 *   Tag 28  – Sensor Relative Roll Angle      (4-byte fixed-point, duplicate for compat)
 */
class FKlvBuilder
{
public:
	/**
	 * Build a complete MISB ST 0601 Local Set KLV packet.
	 * The returned buffer starts with the 16-byte Universal Label key,
	 * followed by a BER-OID length, followed by tag-length-value triplets,
	 * ending with the CRC-16 checksum (Tag 1).
	 */
	static TArray<uint8> BuildMisbST0601(const FCamSimTelemetry& Telemetry);

	// -----------------------------------------------------------------------
	// Public helpers used by the tag-descriptor table in KlvBuilder.cpp.
	// These pure-math converters are also useful for testing individual tags.
	// -----------------------------------------------------------------------
	static void AppendTag(TArray<uint8>& Buf, uint8 Tag, const uint8* Value, uint8 Len);

	// MISB fixed-point mapping
	static int32  MapLatLon(double Degrees, double Range);
	static int32  MapAngle360(float Degrees);    // signed, 4-byte, range ±360°
	static int16  MapAltitude(double Metres);    // 2-byte, range -900..19000 m
	static uint16 MapFov(float Degrees);         // 2-byte unsigned, 0..180°
	static uint64 MapSlantRange(double Metres);  // 8-byte unsigned, 0..5000000 m

private:
	// CRC-16/CCITT (poly 0x1021, init 0xFFFF) over the entire KLV set
	// including the UL key and length, excluding the checksum tag itself.
	static uint16 ComputeCrc16(const uint8* Data, int32 Len);
};
