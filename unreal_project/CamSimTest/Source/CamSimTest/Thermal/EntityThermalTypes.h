// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** Entity thermal state (ROADMAP 4C, docs/thermal.md): part kinds, part geometry and the model settings. */

enum class EEntityThermalPartKind : uint8 { Engine = 0, Exhaust = 1, RunningGear = 2, Count = 3 };
enum class EEntityThermalPartShape : uint8 { Box = 0, Ellipsoid = 1 };

/** One hot-spot volume of an entity type (entity_types.<id>.thermal_parts[i]). Body frame: X forward, Y right, Z down, metres. */
struct FEntityThermalPartSpec
{
	static constexpr float MinExtentM = 0.01f;

	EEntityThermalPartKind  Kind  = EEntityThermalPartKind::Engine;
	EEntityThermalPartShape Shape = EEntityThermalPartShape::Box;
	FVector3f CentreM  = FVector3f::ZeroVector;
	FVector3f HalfM    = FVector3f(0.5f, 0.5f, 0.5f);   // >= MinExtentM per axis
	float     FalloffM = 0.2f;                          // >= MinExtentM
	// Per-part overrides of the kind's temperature parameters (thermal.entity.<kind>)
	TOptional<float> DeltaK;    // engine
	TOptional<float> TempK;     // exhaust
	TOptional<float> KPerMps;   // running_gear
	TOptional<float> MaxK;      // running_gear

	bool operator==(const FEntityThermalPartSpec&) const = default;
};

/** A part kind's defaults (thermal.entity.<kind>). */
struct FEntityThermalKindParams
{
	float DeltaK   = 0.0f;   // engine: T_air + DeltaK while running
	float TempK    = 0.0f;   // exhaust: absolute while running
	float KPerMps  = 0.0f;   // running_gear: T_air + min(KPerMps v, MaxK) while running
	float MaxK     = 0.0f;
	float TauUpS   = 60.0f;  // first-order lag toward a warmer / cooler target
	float TauDownS = 60.0f;

	bool operator==(const FEntityThermalKindParams&) const = default;
};

/** thermal.entity (ROADMAP 4C). */
struct FEntityThermalSettings
{
	static constexpr int32 MaxParts = 4;

	bool  bEnabled        = true;
	float MovingMps       = 0.5f;     // faster = moving (the engine counts as running)
	float IdleHoldS       = 120.0f;   // ... and for this long after it stopped
	float SkinRunningK    = 4.0f;
	float ConvectionV0Mps = 10.0f;    // skin excess over air scales by v0 / (v0 + v)
	float SkinTauS        = 600.0f;
	float BurnK           = 700.0f;   // destroyed / flaming: every surface
	float BurnS           = 300.0f;
	float HullCoolTauS    = 1800.0f;  // skin cooling constant after a burn
	FEntityThermalKindParams Kinds[static_cast<int32>(EEntityThermalPartKind::Count)] = {
		{ 45.0f, 0.0f,   0.0f, 0.0f,  300.0f, 900.0f },   // engine
		{ 0.0f,  450.0f, 0.0f, 0.0f,  20.0f,  60.0f  },   // exhaust
		{ 0.0f,  0.0f,   1.5f, 30.0f, 180.0f, 600.0f },   // running_gear
	};

	const FEntityThermalKindParams& Kind(EEntityThermalPartKind K) const { return Kinds[static_cast<int32>(K)]; }
	FEntityThermalKindParams&       Kind(EEntityThermalPartKind K)       { return Kinds[static_cast<int32>(K)]; }

	bool operator==(const FEntityThermalSettings&) const = default;
};

namespace CamSimEntityThermal
{
	/** "engine" | "exhaust" | "running_gear" (case-insensitive). */
	inline bool ParseKind(const FString& S, EEntityThermalPartKind& Out)
	{
		if (S.Equals(TEXT("engine"), ESearchCase::IgnoreCase))       { Out = EEntityThermalPartKind::Engine; return true; }
		if (S.Equals(TEXT("exhaust"), ESearchCase::IgnoreCase))      { Out = EEntityThermalPartKind::Exhaust; return true; }
		if (S.Equals(TEXT("running_gear"), ESearchCase::IgnoreCase)) { Out = EEntityThermalPartKind::RunningGear; return true; }
		return false;
	}

	/** "box" | "ellipsoid" (case-insensitive). */
	inline bool ParseShape(const FString& S, EEntityThermalPartShape& Out)
	{
		if (S.Equals(TEXT("box"), ESearchCase::IgnoreCase))       { Out = EEntityThermalPartShape::Box; return true; }
		if (S.Equals(TEXT("ellipsoid"), ESearchCase::IgnoreCase)) { Out = EEntityThermalPartShape::Ellipsoid; return true; }
		return false;
	}
}
