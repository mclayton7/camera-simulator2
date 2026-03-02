// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * FCigiEntityState
 *
 * Flattened representation of a CIGI Entity Control packet (v3.3 CigiEntityCtrlV3).
 * Angles are in degrees; position in decimal degrees (lat/lon) and metres (alt).
 */
struct FCigiEntityState
{
	uint16 EntityId    = 0;
	uint8  EntityState = 0;  // CCL enum: 0=Standby, 1=Active, 2=Remove
	uint16 EntityType  = 0;  // Model lookup key (maps to FEntityTypeEntry)

	double Latitude  = 0.0;   // WGS-84 decimal degrees
	double Longitude = 0.0;   // WGS-84 decimal degrees
	float  Altitude  = 0.0f;  // metres above ellipsoid

	float  Yaw       = 0.0f;  // CIGI heading, degrees [0,360)
	float  Pitch      = 0.0f;  // degrees, positive = nose up
	float  Roll       = 0.0f;  // degrees, positive = right wing down
};

/**
 * FCigiRateControl
 *
 * Flattened CIGI Rate Control packet (v3.3, opcode 8).
 * Rates are body-frame: X=forward, Y=right, Z=down (m/s, deg/s).
 */
struct FCigiRateControl
{
	uint16 EntityId        = 0;
	uint8  ArtPartId       = 0;
	bool   bApplyToArtPart = false;
	float  XRate           = 0.0f;   // m/s body-frame forward
	float  YRate           = 0.0f;   // m/s body-frame right
	float  ZRate           = 0.0f;   // m/s body-frame down
	float  RollRate        = 0.0f;   // deg/s
	float  PitchRate       = 0.0f;   // deg/s
	float  YawRate         = 0.0f;   // deg/s
};

/**
 * FCigiArtPartControl
 *
 * Flattened CIGI Articulated Part Control packet (v3.3, opcode 6).
 * Enable flags gate individual DOF updates.
 */
struct FCigiArtPartControl
{
	uint16 EntityId   = 0;
	uint8  ArtPartId  = 0;
	bool   bArtPartEn = false;
	bool   bXOffEn    = false;
	bool   bYOffEn    = false;
	bool   bZOffEn    = false;
	bool   bRollEn    = false;
	bool   bPitchEn   = false;
	bool   bYawEn     = false;
	float  XOff       = 0.0f;
	float  YOff       = 0.0f;
	float  ZOff       = 0.0f;
	float  Roll       = 0.0f;
	float  Pitch      = 0.0f;
	float  Yaw        = 0.0f;
};

/**
 * FCigiComponentControl
 *
 * Flattened CIGI Component Control packet (v3.3, opcode 4).
 * CompClass=0 → entity; CompId selects the sub-system (lights, damage, etc.).
 */
struct FCigiComponentControl
{
	uint16 EntityId   = 0;
	uint16 CompId     = 0;
	uint8  CompClass  = 0;  // 0 = entity component
	uint8  CompState  = 0;  // 0=off/intact, 1=on/damaged, 2=destroyed
};

/**
 * FCigiViewDefinition
 *
 * Flattened representation of a CIGI View Definition packet (v3.3 CigiViewDefV3).
 * All angles in degrees.
 */
struct FCigiViewDefinition
{
	uint16 ViewId    = 0;
	uint8  GroupId   = 0;
	bool   bMirrorJ  = false;  // y-axis mirror
	bool   bMirrorI  = false;  // x-axis mirror

	float  FovLeft   = -30.0f;   // degrees
	float  FovRight  =  30.0f;   // degrees
	float  FovTop    =  17.0f;   // degrees (approx 16:9 half-angle)
	float  FovBottom = -17.0f;   // degrees

	float  NearPlane = 0.1f;     // metres
	float  FarPlane  = 1e6f;     // metres

	/** Returns total horizontal FOV in degrees. */
	float HFovDeg() const { return FovRight - FovLeft; }

	/** Returns total vertical FOV in degrees. */
	float VFovDeg() const { return FovTop - FovBottom; }
};

/**
 * FCigiSensorControl
 *
 * Flattened CIGI Sensor Control packet (v3.3, opcode 17).
 * Controls sensor on/off, polarity, track mode, and gain.
 */
struct FCigiSensorControl
{
	uint16 ViewId    = 0;
	uint8  SensorId  = 0;
	bool   bSensorOn = true;
	uint8  Polarity  = 0;   // 0=WhiteHot, 1=BlackHot
	uint8  TrackMode = 0;   // 0=TrackOff, 3=Target, see CigiBaseSensorCtrl::TrackModeGrp
	float  Gain      = 1.0f;
};

/**
 * FCigiViewControl
 *
 * Flattened CIGI View Control packet (v3.3, opcode 16).
 * Specifies eye-point position and orientation relative to a parent entity.
 * Enable flags gate individual DOF updates (same pattern as FCigiArtPartControl).
 */
struct FCigiViewControl
{
	uint16 ViewId   = 0;
	uint16 EntityId = 0;
	uint8  GroupId  = 0;
	bool   bXOffEn  = false;
	bool   bYOffEn  = false;
	bool   bZOffEn  = false;
	bool   bRollEn  = false;
	bool   bPitchEn = false;
	bool   bYawEn   = false;
	float  XOff     = 0.0f;   // body-frame offset, metres
	float  YOff     = 0.0f;
	float  ZOff     = 0.0f;
	float  Roll     = 0.0f;   // body-frame rotation, degrees
	float  Pitch    = 0.0f;
	float  Yaw      = 0.0f;
};

/**
 * FCigiCelestialState
 *
 * Flattened representation of a CIGI Celestial Sphere Control packet (v3.3, opcode 9).
 * Provides time-of-day and date for sun/moon positioning.
 */
struct FCigiCelestialState
{
	uint8  Hour       = 12;    // 0-23
	uint8  Minute     = 0;     // 0-59
	uint8  Month      = 6;     // 1-12
	uint8  Day        = 21;    // 1-31
	uint16 Year       = 2024;

	float  StarInt    = 0.0f;  // Star field intensity (0.0 - 1.0)

	bool   bEphemerisEn = true;   // Enable ephemeris model
	bool   bSunEn       = true;   // Enable sun
	bool   bMoonEn      = true;   // Enable moon
	bool   bStarEn      = false;  // Enable stars
	bool   bDateVld     = true;   // Date fields are valid
};

/**
 * FCigiAtmosphereState
 *
 * Flattened representation of a CIGI Atmosphere Control packet (v3.3, opcode 10).
 * Controls visibility, humidity, temperature, and wind.
 */
struct FCigiAtmosphereState
{
	bool   bAtmosEn    = true;

	float  Humidity    = 30.0f;    // percent (0-100)
	float  AirTemp     = 20.0f;    // degrees Celsius
	float  Visibility  = 50000.0f; // metres (50 km clear day)
	float  HorizWindSp = 0.0f;    // m/s
	float  VertWindSp  = 0.0f;    // m/s
	float  WindDir     = 0.0f;    // degrees from north [0,360)
	float  BaroPress   = 1013.25f; // millibars
};

/**
 * FCigiHatHotRequest
 *
 * Flattened CIGI HAT/HOT Request packet (v3.3, opcode 24).
 * Requests the height above terrain (HAT) or height of terrain (HOT) at
 * a geodetic point.  ReqType: 0=HAT, 1=HOT, 2=Extended.
 */
struct FCigiHatHotRequest
{
	uint16 HatHotId    = 0;
	uint8  ReqType     = 0;    // 0=HAT, 1=HOT, 2=Extended
	uint8  UpdatePeriod = 0;   // frames between periodic updates (0=one-shot)
	uint16 EntityId    = 0;
	double Lat         = 0.0;  // WGS-84 decimal degrees
	double Lon         = 0.0;
	double Alt         = 0.0;  // metres above ellipsoid
};

/**
 * FCigiLosSegRequest
 *
 * Flattened CIGI LOS Segment Request packet (v3.3, opcode 25).
 * Tests line-of-sight between two geodetic points.
 * ReqType: 0=Basic, 1=Extended.
 */
struct FCigiLosSegRequest
{
	uint16 LosId             = 0;
	uint8  ReqType           = 0;    // 0=Basic, 1=Extended
	uint8  UpdatePeriod      = 0;
	bool   bDestEntityIDValid = false;
	uint16 EntityId          = 0;
	uint16 DestEntityId      = 0;
	double SrcLat = 0.0, SrcLon = 0.0, SrcAlt = 0.0;
	double DstLat = 0.0, DstLon = 0.0, DstAlt = 0.0;
};

/**
 * FCigiLosVectRequest
 *
 * Flattened CIGI LOS Vector Request packet (v3.3, opcode 26).
 * Tests line-of-sight along a vector from a geodetic source point.
 * VectAz = true-north azimuth (degrees), VectEl = elevation (degrees, +up).
 * ReqType: 0=Basic, 1=Extended.
 */
struct FCigiLosVectRequest
{
	uint16 LosId        = 0;
	uint8  ReqType      = 0;     // 0=Basic, 1=Extended
	uint8  UpdatePeriod = 0;
	uint16 EntityId     = 0;
	float  VectAz       = 0.0f;  // true-north azimuth, degrees
	float  VectEl       = 0.0f;  // elevation angle, degrees (+up)
	float  MinRange     = 0.0f;  // metres
	float  MaxRange     = 10000.0f; // metres
	double SrcLat = 0.0, SrcLon = 0.0, SrcAlt = 0.0;
};

/**
 * FCigiWeatherState
 *
 * Flattened representation of a CIGI Weather Control packet (v3.3, opcode 12).
 * Controls cloud layers, precipitation, and weather-related visibility.
 */
struct FCigiWeatherState
{
	uint16 RegionId      = 0;
	uint8  LayerId       = 0;
	uint8  Severity      = 0;       // 0=clear, 1-5 increasing severity

	bool   bWeatherEn    = false;

	uint8  CloudType     = 0;       // 0=none, 1=cirrus, 2=stratus, ..., 9=other
	uint8  Scope         = 0;       // 0=global, 1=regional, 2=entity
	float  Coverage      = 0.0f;    // percent (0-100)
	float  BaseElev      = 2000.0f; // metres above MSL
	float  Thickness     = 500.0f;  // metres
	float  Transition    = 500.0f;  // metres (edge fade)
	float  VisibilityRng = 50000.0f;// metres within weather layer
	float  HorizWindSp   = 0.0f;   // m/s
	float  VertWindSp    = 0.0f;   // m/s
	float  WindDir       = 0.0f;   // degrees from north [0,360)
};
