// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Geospatial/CigiFrames.h"   // CamSimFrames::FGeoPose
#include "Sensor/SensorTypes.h"      // ESensorMode

/**
 * Canonical simulation commands (ROADMAP 2.3; docs/superpowers/specs/
 * 2026-09-26-host-adapter-layer-design.md).
 *
 * Host adapters (CIGI, DIS, scenario, later the control API and MAVLink)
 * translate their protocols into these; the simulation consumes only these.
 * Conventions: WGS-84 geodetic positions with ellipsoid heights, orientation
 * as a local North-East-Up quaternion (see CamSimFrames), SI units, degrees
 * for angles.
 */

/** Where a command came from. Each source has its own entity ID space. */
enum class EHostSource : uint8
{
	Cigi,
	Dis,
	Scenario,
	ControlApi,
	Mavlink,
};

/** An entity's identity: its ID within its source's own namespace. */
struct FEntityKey
{
	EHostSource Source = EHostSource::Cigi;
	uint64      Id     = 0;   // CIGI entity ID; DIS site:application:entity packed into 48 bits; …

	FEntityKey() = default;
	FEntityKey(EHostSource InSource, uint64 InId) : Source(InSource), Id(InId) {}

	bool operator==(const FEntityKey& Other) const { return Source == Other.Source && Id == Other.Id; }
	bool operator!=(const FEntityKey& Other) const { return !(*this == Other); }
	/** "cigi:7", "dis:1.2.3", … for logs. */
	FString ToString() const
	{
		switch (Source)
		{
		case EHostSource::Cigi:       return FString::Printf(TEXT("cigi:%llu"), Id);
		case EHostSource::Dis:        return FString::Printf(TEXT("dis:%llu.%llu.%llu"), (Id >> 32) & 0xFFFF, (Id >> 16) & 0xFFFF, Id & 0xFFFF);
		case EHostSource::Scenario:   return FString::Printf(TEXT("scenario:%llu"), Id);
		case EHostSource::ControlApi: return FString::Printf(TEXT("api:%llu"), Id);
		case EHostSource::Mavlink:    return FString::Printf(TEXT("mavlink:%llu"), Id);
		}
		return FString::Printf(TEXT("?:%llu"), Id);
	}

	friend uint32 GetTypeHash(const FEntityKey& Key)
	{
		return HashCombine(::GetTypeHash(static_cast<uint8>(Key.Source)), ::GetTypeHash(Key.Id));
	}
};

enum class EEntityLifecycle : uint8
{
	Active,   // exists and is drawn
	Hidden,   // exists but is not drawn (CIGI Standby)
	Remove,   // destroy
};

/** DIS-style classification (kind / domain / category), used for effects. */
struct FEntityClassification
{
	uint8 Kind     = 0;   // 1 = platform
	uint8 Domain   = 0;   // 1 = land, 2 = air, 3 = surface, …
	uint8 Category = 0;
};

/** A child entity's placement relative to its parent. */
struct FEntityAttachment
{
	FEntityKey Parent;
	FVector    OffsetFrd = FVector::ZeroVector;     // metres, parent body frame (forward, right, down)
	FRotator   Rotation  = FRotator::ZeroRotator;   // relative to the parent's axes (pitch, yaw, roll)
};

/**
 * How an entity moves between updates. World: velocity/acceleration
 * North-East-Down along the geoid, angular rates are heading/pitch/roll rates.
 * Body: forward-right-down, rotation about the body axes.
 */
struct FMotionModel
{
	enum class EFrame : uint8 { World, Body };

	EFrame  LinearFrame  = EFrame::World;
	FVector Velocity     = FVector::ZeroVector;   // m/s
	FVector Acceleration = FVector::ZeroVector;   // m/s^2
	EFrame  AngularFrame = EFrame::Body;
	FVector AngularRate  = FVector::ZeroVector;   // deg/s as (roll, pitch, yaw)

	bool IsStationary() const
	{
		return Velocity.IsZero() && Acceleration.IsZero() && AngularRate.IsZero();
	}
};

/** Where an entity sits: as sent, on the terrain, or on the water surface. */
enum class ESurfaceMode : uint8
{
	None,    // pose as sent (aircraft, entities with true heights)
	Ground,  // height, pitch and roll from the terrain under the footprint
	Water,   // height from the rendered water surface (EGM96 sea level before the first hit)
};

/** Create, update, hide or remove an entity. */
struct FEntityCommand
{
	FEntityKey             Key;
	EEntityLifecycle       Lifecycle = EEntityLifecycle::Active;
	uint16                 TypeId    = 0;   // CamSim entity type (entity type table)
	FEntityClassification  Classification;

	/** Geodetic pose; ignored while Attachment is set. */
	CamSimFrames::FGeoPose Pose;
	TOptional<FEntityAttachment> Attachment;
	/** Surface placement, applied to every pose the entity commits (DIS domain, CIGI conformal clamp). */
	ESurfaceMode SurfaceMode = ESurfaceMode::None;

	/** Motion carried with the update (DIS dead reckoning). CIGI sends it separately (FEntityMotionCommand). */
	TOptional<FMotionModel> Motion;

	/** The source's time for this update, seconds; only differences are meaningful. */
	double SourceTimeSec = 0.0;
};

/** The type an entity has after a command: TypeId 0 (e.g. CIGI Conformal Clamp, which carries none) keeps it. */
inline uint16 ResolveEntityTypeId(uint16 CurrentTypeId, const FEntityCommand& Command)
{
	return Command.TypeId != 0 ? Command.TypeId : CurrentTypeId;
}

/** Set how an entity moves between pose updates (CIGI Rate Control). */
struct FEntityMotionCommand
{
	FEntityKey   Key;
	FMotionModel Motion;
};

/** Pose of an articulated part (or its rates) relative to its entity. */
struct FArticulationCommand
{
	FEntityKey Key;
	uint8      PartId   = 0;
	bool       bEnabled = true;
	bool bXEn = false, bYEn = false, bZEn = false;
	bool bRollEn = false, bPitchEn = false, bYawEn = false;
	FVector  Offset   = FVector::ZeroVector;    // metres
	FRotator Rotation = FRotator::ZeroRotator;  // degrees
};

/** Set a component (lights, damage state, …) of an entity. */
struct FComponentCommand
{
	FEntityKey Key;
	uint8      ComponentClass = 0;   // 0 = entity
	uint16     ComponentId    = 0;
	uint8      State          = 0;
};

/** Field of view, gimbal and eye point of the sensor view. */
struct FViewCommand
{
	TOptional<float> HFovDeg;
	TOptional<float> VFovDeg;

	enum class EGimbal : uint8 { None, Snap, Slew };
	EGimbal Gimbal = EGimbal::None;           // Snap: go now; Slew: rate-limited target
	bool bYawEn = false, bPitchEn = false, bRollEn = false;
	float Yaw = 0.0f, Pitch = 0.0f, Roll = 0.0f;   // degrees, relative to the platform

	/** First-person view: unset = no change; a key = ride that entity; bClearEyeEntity = stop. */
	TOptional<FEntityKey> EyeEntity;
	bool bClearEyeEntity = false;
	bool bXOffEn = false, bYOffEn = false, bZOffEn = false;
	FVector EyeOffsetFrd = FVector::ZeroVector;   // metres, eye-entity body frame
};

/** Sensor state. */
struct FSensorCommand
{
	bool        bOn      = true;
	ESensorMode Waveband = ESensorMode::EO;
	uint8       Polarity = 0;      // IR: 0 = white hot, 1 = black hot
	float       Zoom     = 0.0f;   // 0 = widest FOV preset … 1 = narrowest
};

/** Set the sim clock. */
struct FTimeCommand
{
	TOptional<FDateTime> Utc;   // jump to this date and time
	bool bRunning = true;       // false = static time of day
};

struct FAtmosphereCommand
{
	bool  bEnabled         = true;
	float HumidityPct      = 30.0f;
	float AirTempC         = 20.0f;
	float VisibilityM      = 50000.0f;
	float HorizWindMps     = 0.0f;
	float VertWindMps      = 0.0f;
	float WindDirDeg       = 0.0f;
	float BaroPressureMb   = 1013.25f;
};

struct FWeatherCommand
{
	enum class EScope : uint8 { Global, Regional, Entity };
	EScope Scope     = EScope::Global;
	uint16 RegionId  = 0;
	uint8  LayerId   = 0;
	bool   bEnabled  = false;
	uint8  Severity  = 0;       // 0 clear … 5
	uint8  CloudType = 0;       // CIGI cloud type
	float  CoveragePct   = 0.0f;
	float  BaseElevM     = 2000.0f;
	float  ThicknessM    = 500.0f;
	float  TransitionM   = 500.0f;
	float  VisibilityM   = 50000.0f;
	float  HorizWindMps  = 0.0f;
	float  VertWindMps   = 0.0f;
	float  WindDirDeg    = 0.0f;
};

struct FOceanWaveCommand
{
	FWeatherCommand::EScope Scope = FWeatherCommand::EScope::Global;
	uint16 RegionId       = 0;
	uint8  WaveId         = 0;
	bool   bEnabled       = false;
	float  HeightM        = 0.0f;
	float  LengthM        = 0.0f;
	float  PeriodS        = 0.0f;
	float  DirectionDeg   = 0.0f;  // propagates toward, true north
	float  PhaseOffsetDeg = 0.0f;
	uint8  Breaker        = 0;
};

struct FMaritimeSurfaceCommand
{
	FWeatherCommand::EScope Scope = FWeatherCommand::EScope::Global;
	uint16 RegionId        = 0;
	bool   bEnabled        = false;
	bool   bWhitecaps      = false;
	float  SurfaceHeightM  = 0.0f;
	float  WaterTempC      = 15.0f;
	float  Clarity         = 1.0f;
};
