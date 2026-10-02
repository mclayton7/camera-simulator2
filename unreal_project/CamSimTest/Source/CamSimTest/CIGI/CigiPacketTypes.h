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

	// When bAttached is true these are the X/Y/Z offsets (metres) in the
	// parent's body frame (X forward, Y right, Z down), and Yaw/Pitch/Roll are
	// relative to the parent's axes. Otherwise they are geodetic.
	bool   bAttached = false;
	uint16 ParentId  = 0;

	double Latitude  = 0.0;   // WGS-84 decimal degrees
	double Longitude = 0.0;   // WGS-84 decimal degrees
	float  Altitude  = 0.0f;  // metres above ellipsoid

	float  Yaw       = 0.0f;  // CIGI heading, degrees [0,360)
	float  Pitch      = 0.0f;  // degrees, positive = nose up
	float  Roll       = 0.0f;  // degrees, positive = right wing down

	// Host time of the message carrying this update (seconds; only differences
	// are meaningful). See FCigiHostClock.
	double HostTimeSec = 0.0;

	// Entity classification from CIGI EntityCtrlV3 (DIS-style kind / domain / category)
	uint8 EntityKind     = 0;
	uint8 EntityDomain   = 0;
	uint8 EntityCategory = 0;
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
	// Frame of the rates below. Local: body frame (X forward, Y right, Z down),
	// rotation about body axes. World/Parent (CIGI 3.2+ default): X north,
	// Y east, Z down, and heading/pitch/roll rates. CIGI 3.0/3.1 is always Local.
	bool   bLocalFrame     = true;
	// Frame of the angular rates alone; CIGI uses bLocalFrame for both. DIS
	// world-frame dead reckoning has world velocity but body angular rates.
	bool   bAngularLocalFrame = true;
	float  XRate           = 0.0f;   // m/s
	float  YRate           = 0.0f;   // m/s
	float  ZRate           = 0.0f;   // m/s
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
	// Coordinate System = Entity: Lat/Lon/Alt hold X/Y/Z offsets (metres) in
	// the body frame of EntityId (X forward, Y right, Z down).
	bool   bEntityRelative = false;
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
/**
 * FCigiLosExtendedResponse
 *
 * Contents of a CIGI 3.3 Line of Sight Extended Response (opcode 105).
 */
struct FCigiLosExtendedResponse
{
	uint16 LosId          = 0;
	bool   bValid         = false;  // an intersection (or, for a clear segment, the destination) is reported
	bool   bRangeValid    = false;  // always false for segment requests
	bool   bVisible       = false;  // segment requests only
	bool   bEntityIdValid = false;
	uint16 EntityId       = 0;
	bool   bEntityCs      = false;  // position is X/Y/Z offsets in EntityId's body frame
	double Range          = 0.0;    // metres from the source point
	double LatOrX = 0.0, LonOrY = 0.0, AltOrZ = 0.0;
	uint32 Material       = 0;
	float  NormalAzDeg    = 0.0f;   // true north, -180..180
	float  NormalElDeg    = 0.0f;   // above the local horizontal
};

struct FCigiLosSegRequest
{
	uint16 LosId             = 0;
	uint8  ReqType           = 0;    // 0=Basic, 1=Extended
	uint8  UpdatePeriod      = 0;
	bool   bDestEntityIDValid = false;
	uint16 EntityId          = 0;
	uint16 DestEntityId      = 0;
	// Entity-relative endpoints hold X/Y/Z body-frame offsets (metres). The
	// source is relative to EntityId; the destination to DestEntityId when
	// bDestEntityIDValid, otherwise to EntityId.
	bool   bSrcEntityRelative = false;
	bool   bDstEntityRelative = false;
	// Response Coordinate System = Entity (extended responses): report the
	// intersection as X/Y/Z offsets in the intersected entity's body frame.
	bool   bResponseEntityCs  = false;
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
	// Entity-relative: Src* hold X/Y/Z body-frame offsets from EntityId, and
	// VectAz/VectEl are measured from its +X axis / XY plane (+El = up).
	bool   bEntityRelative = false;
	bool   bResponseEntityCs = false;  // see FCigiLosSegRequest
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

/**
 * FCigiConfClampEntityState
 *
 * Flattened CIGI Conformal Clamped Entity Control packet (v3.3, opcode 3).
 * Entity is clamped to terrain — altitude is determined by terrain query.
 */
struct FCigiConfClampEntityState
{
	uint16 EntityId  = 0;
	double Latitude  = 0.0;   // WGS-84 decimal degrees
	double Longitude = 0.0;   // WGS-84 decimal degrees
	float  Yaw       = 0.0f;  // degrees [0,360)
};

/**
 * FCigiCollisionDetSegDef
 *
 * Minimal stub for CIGI Collision Detection Segment Definition (v3.3, opcode 7).
 * Logged at Verbose level only — no game-thread processing.
 */
struct FCigiCollisionDetSegDef
{
	uint16 EntityId = 0;
	uint8  SegId    = 0;
	bool   bEnabled = false;
};

/**
 * FCigiMaritimeSurfaceState
 *
 * Flattened CIGI Maritime Surface Conditions Control packet (v3.3, opcode 13).
 * Controls ocean surface parameters for a given scope.
 */
struct FCigiMaritimeSurfaceState
{
	uint16 EntityRgnId    = 0;
	bool   bSurfaceCondEn = false;
	bool   bWhitecapEn    = false;
	uint8  Scope          = 0;        // 0=Global, 1=Regional, 2=Entity
	float  SurfaceHeight  = 0.0f;     // metres
	float  WaterTemp      = 15.0f;    // degrees Celsius
	float  Clarity        = 100.0f;   // percent, 0-100 (CIGI 3.3; CCL bounds-checks it)
};

/**
 * FCigiPlatformKinematics
 *
 * User-defined CIGI packet 201, Platform Kinematics (hitl/PROTOCOL.md section 2):
 * the camera platform's airspeeds, magnetic heading, NED velocity and the UTC
 * time of the pose in the same datagram. CIGI 3.3 has no fields for them.
 * Parsed raw (like Celestial), in the byte order of the datagram's IG Control.
 * A field whose valid flag is clear is ignored.
 */
struct FCigiPlatformKinematics
{
	static constexpr uint8 Opcode     = 201;
	static constexpr uint8 PacketSize = 48;

	// Flags byte (offset 4)
	static constexpr uint8 FlagAirspeeds       = 0x01;
	static constexpr uint8 FlagMagneticHeading = 0x02;
	static constexpr uint8 FlagNedVelocity     = 0x04;
	static constexpr uint8 FlagSampleTime      = 0x08;

	uint16 EntityId = 0;
	uint8  Flags    = 0;

	float  TrueAirspeedMps      = 0.0f;  // KLV Tag 8
	float  IndicatedAirspeedMps = 0.0f;  // KLV Tag 9
	float  MagneticHeadingDeg   = 0.0f;  // 0-360, KLV Tag 64
	float  VelNorthMps          = 0.0f;  // KLV Tag 79
	float  VelEastMps           = 0.0f;  // KLV Tag 80
	float  VelDownMps           = 0.0f;
	double SampleUtcSec         = 0.0;   // Unix epoch seconds of the pose in this datagram (KLV Tag 2)

	bool HasAirspeeds()       const { return (Flags & FlagAirspeeds) != 0; }
	bool HasMagneticHeading() const { return (Flags & FlagMagneticHeading) != 0; }
	bool HasNedVelocity()     const { return (Flags & FlagNedVelocity) != 0; }
	bool HasSampleTime()      const { return (Flags & FlagSampleTime) != 0; }
};

/**
 * FCigiCameraFrame
 *
 * Everything one CIGI datagram says about the camera: the platform pose
 * (Entity Control for camera_entity_id), the gimbal (View Control, Articulated
 * Part on the camera entity), the field of view (View Definition), the sensor
 * (Sensor Control) and Platform Kinematics (packet 201). The receiver publishes
 * it as one queue item after the whole datagram is parsed, so the game thread
 * reads pose and gimbal together: a datagram can never apply its gimbal a frame
 * before its pose (HITL.md gap 2). Packets keep their order within each type.
 */
struct FCigiCameraFrame
{
	bool                         bHasPose = false;
	FCigiEntityState             Pose;          // the last camera Entity Control in the datagram
	TArray<FCigiViewControl>     ViewControls;
	TArray<FCigiArtPartControl>  ArtParts;      // camera entity only
	TArray<FCigiViewDefinition>  ViewDefinitions;
	TArray<FCigiSensorControl>   SensorControls;
	bool                         bHasKinematics = false;
	FCigiPlatformKinematics      Kinematics;    // the last packet 201 for the camera entity

	bool IsEmpty() const
	{
		return !bHasPose && !bHasKinematics && ViewControls.Num() == 0 && ArtParts.Num() == 0
			&& ViewDefinitions.Num() == 0 && SensorControls.Num() == 0;
	}

	void Reset()
	{
		bHasPose = false;
		bHasKinematics = false;
		ViewControls.Reset();
		ArtParts.Reset();
		ViewDefinitions.Reset();
		SensorControls.Reset();
	}

	/**
	 * Fold a later datagram in: its pose and kinematics win, its packets append.
	 * A sample time only describes the pose of its own datagram, so a later pose
	 * without kinematics drops the earlier sample time.
	 */
	void Append(const FCigiCameraFrame& Later)
	{
		if (Later.bHasPose)
		{
			bHasPose = true;
			Pose = Later.Pose;
			Kinematics.Flags &= ~FCigiPlatformKinematics::FlagSampleTime;
		}
		if (Later.bHasKinematics) { bHasKinematics = true; Kinematics = Later.Kinematics; }
		ViewControls.Append(Later.ViewControls);
		ArtParts.Append(Later.ArtParts);
		ViewDefinitions.Append(Later.ViewDefinitions);
		SensorControls.Append(Later.SensorControls);
	}
};

/** CIGI Wave Control (opcode 14) — ocean wave parameters from the host. */
struct FCigiWaveState
{
	uint16 EntityRgnId    = 0;
	uint8  WaveID         = 0;
	bool   bEnabled       = false;
	uint8  Scope          = 0;     // 0 Global, 1 Regional, 2 Entity
	uint8  Breaker        = 0;     // 0 plunging, 1 spilling, 2 surging (ignored)
	float  WaveHtM        = 0.0f;  // crest-to-trough, metres
	float  WaveLenM       = 0.0f;  // metres
	float  PeriodS        = 0.0f;  // seconds
	float  DirectionDeg   = 0.0f;  // direction the wave propagates, true north (CIGI 3.3)
	float  PhaseOffsetDeg = 0.0f;
};
