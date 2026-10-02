# Entity Thermal State (ROADMAP 4C) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Vehicles in thermal IR show engine, exhaust and running-gear hot spots, speed-dependent skin, a burning-then-cooling destroyed state and minutes-long cool-down, driven by CIGI Component Control, DIS appearance and measured speed.

**Architecture:** A pure model (`Thermal/EntityThermal.*`) steps each entity's skin and ≤ 4 part temperatures (stored as excess over the 4A baseline `B = T_class + offset`) on sim time in the entity manager tick. The thermal frame builder turns them into 256 per-stencil records (world→body transform + part volumes + temperatures); the render thread finalises the transforms against the view's translation; `ThermalCS` and its CPU mirror blend part temperatures over the skin for entity pixels. Invalid records fall back to 4A's stencil table, so `thermal.entity.enabled: false` is 4A bit for bit.

**Tech Stack:** UE 5.8 C++ (Automation tests), HLSL compute (RDG), rapidyaml config, Python 3 acceptance scripts (numpy).

**Spec:** `docs/superpowers/specs/2026-10-02-entity-thermal-state-design.md`

## Global Constraints

- Copyright header `// Copyright CamSim Contributors. All Rights Reserved.`; `#include "CoreMinimal.h"` first; UE naming (`F`/`A`/`U`/`E`), tabs.
- Body frame: X forward, Y right, Z down, metres from the actor origin; UE actor frame → body = (x, y, −z) / 100; actor scale ignored.
- Part kinds `engine`, `exhaust`, `running_gear`; shapes `box` (0), `ellipsoid` (1); at most 4 parts per type.
- Defaults: `moving_mps` 0.5, `idle_hold_s` 120, `skin_running_k` 4, `convection_v0_mps` 10, `skin_tau_s` 600, `burn_k` 700, `burn_s` 300, `hull_cool_tau_s` 1800; engine `delta_k` 45 / 300 / 900 s; exhaust `temp_k` 450 / 20 / 60 s; running_gear `k_per_mps` 1.5, `max_k` 30 / 180 / 600 s.
- Excess clamped to [−200, 900] K in the model; absolute `B + excess` clamped to [150, 1000] K in the builder.
- Clock: `dt < 0` or `dt > 3600 s` snaps to targets; `dt == 0` changes nothing; spawn starts at targets.
- Speed: ECEF from the geodetic pose, EMA τ = 1 s; teleport `|ΔP| > max(50 m, 400 m/s · dt)` → v = 0; first sample → v = 0; `dt ≤ 0` keeps v.
- Component IDs (class 0): 10 damage (0/1/2), 11 power plant (0/1), 12 flaming (0/1).
- DIS appearance (platform kind 1, domains 1/2/3 only): bit 22 power plant, bits 3–4 damage (3 = destroyed → CompId 10 state 2; 1/2 → 1), bit 15 flaming; emit on change only.
- `ThermalCS` mirror tolerance 1e-4 relative radiance; gate f (ThermalCS p95 ≤ 0.5 ms at 1080p) must hold.
- Never `|| true` a validation step; tests under `CamSim.*`.

## Review Focus

- A truck parked under a jittering surface clamp / DR correction (centimetre pose noise) must not count as moving: covered in Task 3 (`SpeedTracker.JitterStaysParked`).
- A Cesium origin shift or a CIGI teleport must not register as a speed spike: Task 3 (`SpeedTracker.Teleport`), and positions are ECEF (Task 8).
- A stencil value reused by a different entity within the same frame sequence must not inherit the previous owner's parts or excess: Task 5 (`Builder.EntityRecordReuse`) and Task 8 (state lives on the actor, not the stencil).
- Switching EO → IR after a vehicle drove in EO for minutes must show the warmed-up state, not a spawn-like cold vehicle: Task 3 (`Model.NoEnvironmentStillLags`) — stepping never depends on IR running.
- An entity far from the georeference origin (100 km) must place its hot spots to millimetres: Task 5 (`Builder.EntityRecordFarFromOrigin`).

---

## How to build and run tests (all tasks)

```bash
cd /opt/mac/camera-simulator2
scripts/run.sh --build-only            # editor build, ~10 s incremental; must exit 0
UE_BIN=/opt/UE/Engine/Binaries/Linux/UnrealEditor
"$UE_BIN" unreal_project/CamSimTest/CamSimTest.uproject \
  -ExecCmds="Automation RunTests CamSim.Thermal.Entity+Quit" \
  -TestExit="Automation Test Queue Empty" -ReportExportPath=.cache/automation-report \
  -unattended -nullrhi -nosound -nosplash -log -stdout -FullStdOutLogOutput 2>&1 | grep -E "Test Completed|Result=|Error" | tail -40
```

Replace the filter per task. GPU tests: `scripts/run_gpu_tests.sh CamSim.GPU.Thermal`. Read failures from `.cache/automation-report/index.json` (`encoding="utf-8-sig"`).

---

### Task 1: Entity thermal types and `thermal.entity` config

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Thermal/EntityThermalTypes.h`
- Modify: `Source/CamSimTest/Config/CamSimConfig.h` (inside `FThermalConfig`, after `FLandCoverConfig LandCover;`)
- Modify: `Source/CamSimTest/Config/CamSimConfig.cpp` (yaml parse after the `land_cover` block ~line 755; env after the land-cover env ~line 1040; validation after land-cover validation ~line 1490)
- Test: `Source/CamSimTest/Tests/ThermalEntityConfigTest.cpp`

**Interfaces:**
- Produces: `EEntityThermalPartKind`, `EEntityThermalPartShape`, `FEntityThermalPartSpec`, `FEntityThermalKindParams`, `FEntityThermalSettings` (in `EntityThermalTypes.h`); `FCamSimConfig::FThermalConfig::Entity` of type `FEntityThermalSettings`.

- [ ] **Step 1: Create the types header**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** Entity thermal state (ROADMAP 4C, docs/thermal.md): part kinds, part geometry and the model settings. */

enum class EEntityThermalPartKind : uint8 { Engine = 0, Exhaust = 1, RunningGear = 2, Count = 3 };
enum class EEntityThermalPartShape : uint8 { Box = 0, Ellipsoid = 1 };

/** One hot-spot volume of an entity type (entity_types.<id>.thermal_parts[i]). Body frame: X forward, Y right, Z down, metres. */
struct FEntityThermalPartSpec
{
	EEntityThermalPartKind  Kind  = EEntityThermalPartKind::Engine;
	EEntityThermalPartShape Shape = EEntityThermalPartShape::Box;
	FVector3f CentreM   = FVector3f::ZeroVector;
	FVector3f HalfM     = FVector3f(0.5f, 0.5f, 0.5f);   // >= MinExtentM per axis
	float     FalloffM  = 0.2f;                          // >= MinExtentM
	// Per-part override of the kind's temperature parameter (engine/running_gear: DeltaK; exhaust: TempK; running_gear: MaxK)
	TOptional<float> DeltaK;
	TOptional<float> TempK;
	TOptional<float> KPerMps;
	TOptional<float> MaxK;

	static constexpr float MinExtentM = 0.01f;
	bool operator==(const FEntityThermalPartSpec&) const = default;
};

/** A part kind's defaults (thermal.entity.<kind>). */
struct FEntityThermalKindParams
{
	float DeltaK   = 0.0f;   // engine: T_air + DeltaK while running
	float TempK    = 0.0f;   // exhaust: absolute while running
	float KPerMps  = 0.0f;   // running_gear: T_air + min(KPerMps v, MaxK)
	float MaxK     = 0.0f;
	float TauUpS   = 60.0f;
	float TauDownS = 60.0f;
	bool operator==(const FEntityThermalKindParams&) const = default;
};

/** thermal.entity (ROADMAP 4C). */
struct FEntityThermalSettings
{
	static constexpr int32 MaxParts = 4;

	bool  bEnabled         = true;
	float MovingMps        = 0.5f;
	float IdleHoldS        = 120.0f;
	float SkinRunningK     = 4.0f;
	float ConvectionV0Mps  = 10.0f;
	float SkinTauS         = 600.0f;
	float BurnK            = 700.0f;
	float BurnS            = 300.0f;
	float HullCoolTauS     = 1800.0f;
	FEntityThermalKindParams Kinds[static_cast<int32>(EEntityThermalPartKind::Count)] = {
		{ 45.0f, 0.0f,   0.0f, 0.0f,  300.0f, 900.0f },   // engine
		{ 0.0f,  450.0f, 0.0f, 0.0f,  20.0f,  60.0f  },   // exhaust
		{ 0.0f,  0.0f,   1.5f, 30.0f, 180.0f, 600.0f },   // running_gear
	};

	const FEntityThermalKindParams& Kind(EEntityThermalPartKind K) const { return Kinds[static_cast<int32>(K)]; }
	FEntityThermalKindParams&       Kind(EEntityThermalPartKind K)       { return Kinds[static_cast<int32>(K)]; }
	bool operator==(const FEntityThermalSettings& O) const
	{
		bool bKinds = true;
		for (int32 I = 0; I < static_cast<int32>(EEntityThermalPartKind::Count); ++I) bKinds &= Kinds[I] == O.Kinds[I];
		return bKinds && bEnabled == O.bEnabled && MovingMps == O.MovingMps && IdleHoldS == O.IdleHoldS && SkinRunningK == O.SkinRunningK
			&& ConvectionV0Mps == O.ConvectionV0Mps && SkinTauS == O.SkinTauS && BurnK == O.BurnK && BurnS == O.BurnS
			&& HullCoolTauS == O.HullCoolTauS;
	}
};

namespace CamSimEntityThermal
{
	/** "engine" | "exhaust" | "running_gear" (case-insensitive); false otherwise. */
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
```

- [ ] **Step 2: Add the config field**

In `CamSimConfig.h`, add `#include "Thermal/EntityThermalTypes.h"` with the other includes, and inside `FThermalConfig` after `FLandCoverConfig LandCover;`:

```cpp
		/** Entity thermal state (ROADMAP 4C, docs/thermal.md): running/parked/burning vehicles, part hot spots. */
		FEntityThermalSettings Entity;
```

- [ ] **Step 3: Write the failing test**

`Tests/ThermalEntityConfigTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"

// CamSim.Thermal.Entity.Config.*: thermal.entity yaml, env and validation (ROADMAP 4C).

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalEntityConfigDefaultsTest, "CamSim.Thermal.Entity.Config.Defaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalEntityConfigDefaultsTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	TestTrue(TEXT("load"), FCamSimConfig::LoadFromYamlString(TEXT("thermal: {}\n"), Cfg));
	const FEntityThermalSettings& E = Cfg.Thermal.Entity;
	TestTrue(TEXT("enabled"), E.bEnabled);
	TestEqual(TEXT("idle hold"), E.IdleHoldS, 120.0f);
	TestEqual(TEXT("engine delta"), E.Kind(EEntityThermalPartKind::Engine).DeltaK, 45.0f);
	TestEqual(TEXT("exhaust temp"), E.Kind(EEntityThermalPartKind::Exhaust).TempK, 450.0f);
	TestEqual(TEXT("gear k"), E.Kind(EEntityThermalPartKind::RunningGear).KPerMps, 1.5f);
	TestEqual(TEXT("no validation errors"), Cfg.Validate().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalEntityConfigYamlTest, "CamSim.Thermal.Entity.Config.YamlAndEnv",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalEntityConfigYamlTest::RunTest(const FString& Parameters)
{
	const TCHAR* Yaml =
		TEXT("thermal:\n")
		TEXT("  entity:\n")
		TEXT("    enabled: false\n")
		TEXT("    moving_mps: 1.0\n")
		TEXT("    idle_hold_s: 30\n")
		TEXT("    skin_running_k: 2\n")
		TEXT("    convection_v0_mps: 5\n")
		TEXT("    skin_tau_s: 100\n")
		TEXT("    burn_k: 650\n")
		TEXT("    burn_s: 60\n")
		TEXT("    hull_cool_tau_s: 900\n")
		TEXT("    engine: { delta_k: 30, tau_up_s: 100, tau_down_s: 200 }\n")
		TEXT("    exhaust: { temp_k: 500, tau_up_s: 10, tau_down_s: 40 }\n")
		TEXT("    running_gear: { k_per_mps: 2, max_k: 20, tau_up_s: 50, tau_down_s: 70 }\n");
	FCamSimConfig Cfg;
	TestTrue(TEXT("load"), FCamSimConfig::LoadFromYamlString(Yaml, Cfg));
	const FEntityThermalSettings& E = Cfg.Thermal.Entity;
	TestFalse(TEXT("enabled"), E.bEnabled);
	TestEqual(TEXT("moving"), E.MovingMps, 1.0f);
	TestEqual(TEXT("hold"), E.IdleHoldS, 30.0f);
	TestEqual(TEXT("skin run"), E.SkinRunningK, 2.0f);
	TestEqual(TEXT("v0"), E.ConvectionV0Mps, 5.0f);
	TestEqual(TEXT("skin tau"), E.SkinTauS, 100.0f);
	TestEqual(TEXT("burn k"), E.BurnK, 650.0f);
	TestEqual(TEXT("burn s"), E.BurnS, 60.0f);
	TestEqual(TEXT("hull"), E.HullCoolTauS, 900.0f);
	TestEqual(TEXT("engine delta"), E.Kind(EEntityThermalPartKind::Engine).DeltaK, 30.0f);
	TestEqual(TEXT("engine up"), E.Kind(EEntityThermalPartKind::Engine).TauUpS, 100.0f);
	TestEqual(TEXT("engine down"), E.Kind(EEntityThermalPartKind::Engine).TauDownS, 200.0f);
	TestEqual(TEXT("exhaust temp"), E.Kind(EEntityThermalPartKind::Exhaust).TempK, 500.0f);
	TestEqual(TEXT("gear k"), E.Kind(EEntityThermalPartKind::RunningGear).KPerMps, 2.0f);
	TestEqual(TEXT("gear max"), E.Kind(EEntityThermalPartKind::RunningGear).MaxK, 20.0f);
	TestEqual(TEXT("no unknown keys"), Cfg.UnknownYamlKeys.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalEntityConfigValidateTest, "CamSim.Thermal.Entity.Config.Validation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalEntityConfigValidateTest::RunTest(const FString& Parameters)
{
	auto ErrorsFor = [](TFunctionRef<void(FEntityThermalSettings&)> Edit)
	{
		FCamSimConfig Cfg;
		Edit(Cfg.Thermal.Entity);
		int32 N = 0;
		for (const FString& E : Cfg.Validate()) N += E.Contains(TEXT("thermal.entity")) ? 1 : 0;
		return N;
	};
	TestEqual(TEXT("defaults"), ErrorsFor([](FEntityThermalSettings&) {}), 0);
	TestEqual(TEXT("burn_k above LUT"), ErrorsFor([](FEntityThermalSettings& E) { E.BurnK = 1200.0f; }), 1);
	TestEqual(TEXT("exhaust below LUT"), ErrorsFor([](FEntityThermalSettings& E) { E.Kind(EEntityThermalPartKind::Exhaust).TempK = 100.0f; }), 1);
	TestEqual(TEXT("tau 0"), ErrorsFor([](FEntityThermalSettings& E) { E.SkinTauS = 0.0f; }), 1);
	TestEqual(TEXT("tau NaN"), ErrorsFor([](FEntityThermalSettings& E) { E.Kind(EEntityThermalPartKind::Engine).TauUpS = NAN; }), 1);
	TestEqual(TEXT("negative hold"), ErrorsFor([](FEntityThermalSettings& E) { E.IdleHoldS = -1.0f; }), 1);
	TestEqual(TEXT("v0 0"), ErrorsFor([](FEntityThermalSettings& E) { E.ConvectionV0Mps = 0.0f; }), 1);
	return true;
}
```

Also add an env check to the YamlAndEnv test only if the file already has a pattern for setting env vars in tests (search `FPlatformMisc::SetEnvironmentVar` in `Tests/`); if it does, append:

```cpp
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_THERMAL_ENTITY_ENABLED"), TEXT("1"));
	FCamSimConfig Env;
	FCamSimConfig::LoadFromYamlString(Yaml, Env);
	Env.ApplyEnvOverrides();
	TestTrue(TEXT("env overrides enabled"), Env.Thermal.Entity.bEnabled);
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_THERMAL_ENTITY_ENABLED"), TEXT(""));
```

using the exact names that file's existing env tests use (`ApplyEnvOverrides` may be named differently; copy from `Tests/ThermalLandCoverConfigTest.cpp`).

- [ ] **Step 4: Build and run — expect failure**

Build fails (no parsing yet is fine: tests compile but YamlAndEnv fails on values and unknown keys). Run filter `CamSim.Thermal.Entity.Config`. Expected: `YamlAndEnv` and `Validation` FAIL.

- [ ] **Step 5: Parse, env, validate**

In `CamSimConfig.cpp` after the `land_cover` block (still inside the `thermal` node scope, variable `T`):

```cpp
			if (YamlHas(T, "entity"))   // ROADMAP 4C
			{
				ryml::ConstNodeRef En = T["entity"];
				FEntityThermalSettings& E = Cfg.Thermal.Entity;
				YamlBool (En, "enabled",           E.bEnabled);
				YamlFloat(En, "moving_mps",        E.MovingMps);
				YamlFloat(En, "idle_hold_s",       E.IdleHoldS);
				YamlFloat(En, "skin_running_k",    E.SkinRunningK);
				YamlFloat(En, "convection_v0_mps", E.ConvectionV0Mps);
				YamlFloat(En, "skin_tau_s",        E.SkinTauS);
				YamlFloat(En, "burn_k",            E.BurnK);
				YamlFloat(En, "burn_s",            E.BurnS);
				YamlFloat(En, "hull_cool_tau_s",   E.HullCoolTauS);
				struct FKindKey { const char* Key; EEntityThermalPartKind Kind; };
				const FKindKey Kinds[] = { { "engine", EEntityThermalPartKind::Engine }, { "exhaust", EEntityThermalPartKind::Exhaust },
					{ "running_gear", EEntityThermalPartKind::RunningGear } };
				for (const FKindKey& K : Kinds)
				{
					const c4::csubstr Key = c4::to_csubstr(K.Key);
					if (!YamlHas(En, Key)) continue;
					ryml::ConstNodeRef KN = En[Key];
					FEntityThermalKindParams& P = E.Kind(K.Kind);
					YamlFloat(KN, "delta_k",    P.DeltaK);
					YamlFloat(KN, "temp_k",     P.TempK);
					YamlFloat(KN, "k_per_mps",  P.KPerMps);
					YamlFloat(KN, "max_k",      P.MaxK);
					YamlFloat(KN, "tau_up_s",   P.TauUpS);
					YamlFloat(KN, "tau_down_s", P.TauDownS);
				}
			}
```

Env (next to the land-cover env lines):

```cpp
	// Entity thermal state (ROADMAP 4C)
	Cfg.Thermal.Entity.bEnabled = GetEnvBool(TEXT("CAMSIM_THERMAL_ENTITY_ENABLED"), Cfg.Thermal.Entity.bEnabled);
```

Validation (after the land-cover validation lines, inside `Validate()`):

```cpp
	// Entity thermal state (ROADMAP 4C). Written !(x in range) so NaN is reported too.
	{
		const FEntityThermalSettings& E = Thermal.Entity;
		auto Positive = [&Errors](const TCHAR* Name, float V)
		{
			if (!(V > 0.0f && V <= 1.0e6f)) Errors.Add(FString::Printf(TEXT("thermal.entity.%s=%.3f must be in (0, 1e6]"), Name, V));
		};
		auto InLut = [&Errors](const TCHAR* Name, float V)
		{
			if (!(V >= 150.0f && V <= 1000.0f)) Errors.Add(FString::Printf(TEXT("thermal.entity.%s=%.1f out of range [150, 1000] K"), Name, V));
		};
		auto InRange = [&Errors](const TCHAR* Name, float V, float Lo, float Hi)
		{
			if (!(V >= Lo && V <= Hi)) Errors.Add(FString::Printf(TEXT("thermal.entity.%s=%.3f out of range [%.1f, %.1f]"), Name, V, Lo, Hi));
		};
		InRange(TEXT("moving_mps"), E.MovingMps, 0.0f, 50.0f);
		InRange(TEXT("idle_hold_s"), E.IdleHoldS, 0.0f, 86400.0f);
		InRange(TEXT("skin_running_k"), E.SkinRunningK, -50.0f, 100.0f);
		Positive(TEXT("convection_v0_mps"), E.ConvectionV0Mps);
		Positive(TEXT("skin_tau_s"), E.SkinTauS);
		InLut(TEXT("burn_k"), E.BurnK);
		InRange(TEXT("burn_s"), E.BurnS, 0.0f, 86400.0f);
		Positive(TEXT("hull_cool_tau_s"), E.HullCoolTauS);
		const FEntityThermalKindParams& Eng = E.Kind(EEntityThermalPartKind::Engine);
		const FEntityThermalKindParams& Exh = E.Kind(EEntityThermalPartKind::Exhaust);
		const FEntityThermalKindParams& Gear = E.Kind(EEntityThermalPartKind::RunningGear);
		InRange(TEXT("engine.delta_k"), Eng.DeltaK, -50.0f, 500.0f);
		InLut(TEXT("exhaust.temp_k"), Exh.TempK);
		InRange(TEXT("running_gear.k_per_mps"), Gear.KPerMps, 0.0f, 50.0f);
		InRange(TEXT("running_gear.max_k"), Gear.MaxK, 0.0f, 500.0f);
		Positive(TEXT("engine.tau_up_s"), Eng.TauUpS);       Positive(TEXT("engine.tau_down_s"), Eng.TauDownS);
		Positive(TEXT("exhaust.tau_up_s"), Exh.TauUpS);      Positive(TEXT("exhaust.tau_down_s"), Exh.TauDownS);
		Positive(TEXT("running_gear.tau_up_s"), Gear.TauUpS); Positive(TEXT("running_gear.tau_down_s"), Gear.TauDownS);
	}
```

If `GetEnvBool` is defined after its first use point, it is already used for land cover, so it is in scope.

- [ ] **Step 6: Build and run — expect PASS**

Filter `CamSim.Thermal.Entity.Config`: 3/3 pass. Also run `CamSim.Config` and `CamSim.Thermal` (no regressions).

- [ ] **Step 7: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Thermal/EntityThermalTypes.h unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.* unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalEntityConfigTest.cpp
git commit -m "feat(thermal): thermal.entity config and entity thermal types (4C)"
```

---

### Task 2: `entity_types.<id>.thermal_parts`

**Files:**
- Modify: `Source/CamSimTest/Entity/EntityTypeTable.h` (`FEntityTypeEntry`, after `ThermalOffsetK`)
- Modify: `Source/CamSimTest/Entity/EntityTypeTable.cpp` (after the `thermal_offset_k` block ~line 261)
- Test: `Source/CamSimTest/Tests/EntityTypeTableTest.cpp` (append)

**Interfaces:**
- Consumes: `FEntityThermalPartSpec`, `CamSimEntityThermal::ParseKind/ParseShape` (Task 1).
- Produces: `TArray<FEntityThermalPartSpec> FEntityTypeEntry::ThermalParts` (≤ 4, validated); static `bool FEntityTypeTable::ParseThermalPart(ryml::ConstNodeRef, FEntityThermalPartSpec&, FString& OutWhy)` is internal — tests go through the table's yaml loader.

- [ ] **Step 1: Write the failing test** (append to `EntityTypeTableTest.cpp`; use the same loader call the existing tests in that file use — search for `LoadFromYamlString` or `ParseYaml` there and copy its form; below it is written as `Table.LoadFromYamlString(Yaml)`)

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityTypeThermalPartsTest, "CamSim.Thermal.Entity.TypeTable.ThermalParts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEntityTypeThermalPartsTest::RunTest(const FString& Parameters)
{
	const TCHAR* Yaml =
		TEXT("entity_types:\n")
		TEXT("  \"2001\":\n")
		TEXT("    mesh: truck/ural_4320.glb\n")
		TEXT("    thermal_parts:\n")
		TEXT("      - { kind: running_gear, shape: box, centre_m: [0.47, 0, -0.62], half_m: [3.7, 1.55, 0.62], falloff_m: 0.15 }\n")
		TEXT("      - { kind: engine, shape: ellipsoid, centre_m: [3.45, 0, -1.45], half_m: [0.75, 0.75, 0.45], falloff_m: 0.3, delta_k: 30 }\n")
		TEXT("      - { kind: exhaust, shape: box, centre_m: [1.9, 1.25, -0.75], half_m: [0.5, 0.2, 0.2], falloff_m: 0.15, temp_k: 500 }\n")
		TEXT("      - { kind: plasma, shape: box, centre_m: [0, 0, 0], half_m: [1, 1, 1], falloff_m: 0.1 }\n")
		TEXT("      - { kind: engine, shape: cone, centre_m: [0, 0, 0], half_m: [1, 1, 1], falloff_m: 0.1 }\n")
		TEXT("      - { kind: engine, shape: box, centre_m: [0, 0], half_m: [1, 1, 1], falloff_m: 0.1 }\n")
		TEXT("      - { kind: engine, shape: box, centre_m: [0, 0, .nan], half_m: [1, 1, 1], falloff_m: 0.1 }\n")
		TEXT("      - { kind: engine, shape: box, centre_m: [0, 0, 0], half_m: [0.001, 1, 1], falloff_m: 0 }\n")
		TEXT("      - { kind: exhaust, shape: box, centre_m: [0, 0, 0], half_m: [1, 1, 1], falloff_m: 0.1 }\n");
	FEntityTypeTable Table;
	Table.LoadFromYamlString(Yaml);
	const FEntityTypeEntry* E = Table.FindEntry(2001);
	if (!TestNotNull(TEXT("entry"), E)) return false;
	// Valid: gear, engine, exhaust, the clamped one (4th valid). The 5th valid (last line) is dropped: max 4.
	if (!TestEqual(TEXT("parts"), E->ThermalParts.Num(), 4)) return false;
	TestEqual(TEXT("gear kind"), E->ThermalParts[0].Kind, EEntityThermalPartKind::RunningGear);
	TestEqual(TEXT("gear centre x"), E->ThermalParts[0].CentreM.X, 0.47f);
	TestEqual(TEXT("gear half y"), E->ThermalParts[0].HalfM.Y, 1.55f);
	TestEqual(TEXT("engine shape"), E->ThermalParts[1].Shape, EEntityThermalPartShape::Ellipsoid);
	TestTrue(TEXT("engine delta override"), E->ThermalParts[1].DeltaK.IsSet() && *E->ThermalParts[1].DeltaK == 30.0f);
	TestTrue(TEXT("exhaust temp override"), E->ThermalParts[2].TempK.IsSet() && *E->ThermalParts[2].TempK == 500.0f);
	TestEqual(TEXT("tiny half clamped"), E->ThermalParts[3].HalfM.X, FEntityThermalPartSpec::MinExtentM);
	TestEqual(TEXT("zero falloff clamped"), E->ThermalParts[3].FalloffM, FEntityThermalPartSpec::MinExtentM);
	return true;
}
```

- [ ] **Step 2: Run — expect compile failure (`ThermalParts` missing)**

- [ ] **Step 3: Implement**

`EntityTypeTable.h`: add `#include "Thermal/EntityThermalTypes.h"` and in `FEntityTypeEntry` after `TOptional<float> ThermalOffsetK;`:

```cpp
	// ROADMAP 4C — hot-spot volumes (body frame), at most FEntityThermalSettings::MaxParts, in priority order (later wins)
	TArray<FEntityThermalPartSpec> ThermalParts;
```

`EntityTypeTable.cpp`, after the `thermal_offset_k` block:

```cpp
		// ROADMAP 4C — thermal_parts: hot-spot volumes. Invalid parts are skipped with a warning; extras beyond 4 dropped.
		if (EntryNode.has_child("thermal_parts"))
		{
			ryml::ConstNodeRef Parts = EntryNode["thermal_parts"];
			int32 Index = 0;
			for (ryml::ConstNodeRef PN : Parts.children())
			{
				FEntityThermalPartSpec Spec;
				FString Why;
				if (!ParseThermalPart(PN, Spec, Why))
				{
					UE_LOG(LogCamSim, Warning, TEXT("EntityTypeTable: type %u thermal_parts[%d] skipped: %s"), TypeId, Index, *Why);
				}
				else if (Entry.ThermalParts.Num() >= FEntityThermalSettings::MaxParts)
				{
					UE_LOG(LogCamSim, Warning, TEXT("EntityTypeTable: type %u thermal_parts[%d] dropped: at most %d parts"),
						TypeId, Index, FEntityThermalSettings::MaxParts);
				}
				else
				{
					Entry.ThermalParts.Add(Spec);
				}
				++Index;
			}
		}
```

and a file-static helper above the loader:

```cpp
/** One thermal_parts entry (ROADMAP 4C). False with a reason for unknown kind/shape, a missing or non-finite vector. */
static bool ParseThermalPart(ryml::ConstNodeRef N, FEntityThermalPartSpec& Out, FString& OutWhy)
{
	if (!N.is_map()) { OutWhy = TEXT("not a map"); return false; }
	FString Kind, Shape;
	YamlString(N, "kind", Kind);
	YamlString(N, "shape", Shape);
	if (!CamSimEntityThermal::ParseKind(Kind, Out.Kind)) { OutWhy = FString::Printf(TEXT("unknown kind '%s'"), *Kind); return false; }
	if (!CamSimEntityThermal::ParseShape(Shape, Out.Shape)) { OutWhy = FString::Printf(TEXT("unknown shape '%s'"), *Shape); return false; }
	auto Vec3 = [&N](const char* Key, FVector3f& V) -> bool
	{
		const c4::csubstr K = c4::to_csubstr(Key);
		if (!N.has_child(K)) return false;
		ryml::ConstNodeRef A = N[K];
		if (!A.is_seq() || A.num_children() != 3) return false;
		float C[3];
		for (int32 I = 0; I < 3; ++I)
		{
			const c4::csubstr S = A[I].val();
			C[I] = FCString::Atof(*FString(static_cast<int32>(S.len), S.str));
			if (S.len >= 4 && (S.begins_with(".nan") || S.begins_with(".NaN") || S.begins_with(".inf") || S.begins_with("-.inf"))) return false;
			if (!FMath::IsFinite(C[I])) return false;
		}
		V = FVector3f(C[0], C[1], C[2]);
		return true;
	};
	if (!Vec3("centre_m", Out.CentreM)) { OutWhy = TEXT("centre_m must be 3 finite numbers"); return false; }
	if (!Vec3("half_m", Out.HalfM)) { OutWhy = TEXT("half_m must be 3 finite numbers"); return false; }
	float F = Out.FalloffM;
	YamlFloat(N, "falloff_m", F);
	if (!FMath::IsFinite(F)) { OutWhy = TEXT("falloff_m not finite"); return false; }
	const float Min = FEntityThermalPartSpec::MinExtentM;
	Out.HalfM = FVector3f(FMath::Max(FMath::Abs(Out.HalfM.X), Min), FMath::Max(FMath::Abs(Out.HalfM.Y), Min), FMath::Max(FMath::Abs(Out.HalfM.Z), Min));
	Out.FalloffM = FMath::Max(F, Min);
	float V = 0.0f;
	if (YamlFloat(N, "delta_k", V) && FMath::IsFinite(V))   Out.DeltaK = V;
	if (YamlFloat(N, "temp_k", V) && FMath::IsFinite(V))    Out.TempK = FMath::Clamp(V, 150.0f, 1000.0f);
	if (YamlFloat(N, "k_per_mps", V) && FMath::IsFinite(V)) Out.KPerMps = FMath::Max(V, 0.0f);
	if (YamlFloat(N, "max_k", V) && FMath::IsFinite(V))     Out.MaxK = FMath::Max(V, 0.0f);
	return true;
}
```

Note: `half_m: [0.001, 1, 1]` is a valid part with the axis clamped to 0.01 (test expects it kept). If the file's `YamlString`/`YamlFloat` helpers take `const char*` keys rather than `c4::csubstr`, adapt the two lambda lines (the other calls already match the file's usage). If the type table tracks read keys for an unknown-key warning, register `thermal_parts` the same way `thermal_offset_k` is.

- [ ] **Step 4: Run — expect PASS** (`CamSim.Thermal.Entity.TypeTable`, plus `CamSim.EntityTypeTable` or whatever prefix the existing tests in that file use — run the whole `CamSim` NullRHI suite once here.)

- [ ] **Step 5: Commit** — `git commit -m "feat(entity): entity_types thermal_parts (4C)"`

---

### Task 3: Pure entity thermal model and speed tracker

**Files:**
- Create: `Source/CamSimTest/Thermal/EntityThermal.h`, `Source/CamSimTest/Thermal/EntityThermal.cpp`
- Test: `Source/CamSimTest/Tests/ThermalEntityModelTest.cpp`

**Interfaces:**
- Consumes: Task 1 types.
- Produces:

```cpp
struct FEntityThermalCommanded { bool bEngineOn = false; uint8 Damage = 0; bool bFlaming = false; };
struct FEntityThermalInputs
{
	FEntityThermalCommanded Cmd;
	float SpeedMps = 0.0f;
	double SimSec = 0.0;
	bool  bHasEnv = false;   // false: D = 0
	float TairK = 288.15f;
	float BaselineK = 288.15f;   // B = T_class + offset for this entity
};
struct FEntityThermalState
{
	bool   bInitialized = false;
	double LastSimSec = 0.0;
	double LastMovingSec = -1.0e300;   // sim time it was last moving
	double BurnStartSec = -1.0;        // < 0: not burning
	bool   bBurnDone = false;
	bool   bHullCooling = false;
	float  SkinExcessK = 0.0f;
	float  PartExcessK[FEntityThermalSettings::MaxParts] = {};
};
namespace CamSimEntityThermal
{
	bool IsRunning(const FEntityThermalState&, const FEntityThermalInputs&, const FEntityThermalSettings&);
	float ConvectionFactor(float SpeedMps, float V0);
	void Targets(const FEntityThermalState&, const FEntityThermalInputs&, const FEntityThermalSettings&,
		TConstArrayView<FEntityThermalPartSpec>, float& OutSkin, float OutParts[FEntityThermalSettings::MaxParts]);   // excess K
	void Step(FEntityThermalState&, const FEntityThermalInputs&, const FEntityThermalSettings&, TConstArrayView<FEntityThermalPartSpec>);
	bool ApplyComponent(FEntityThermalCommanded&, uint16 ComponentId, uint8 State);   // 11, 12 (and 10 for damage)
}
class FEntitySpeedTracker
{
public:
	float Update(const FVector& EcefM, double SimSec);   // returns smoothed m/s
	float GetSpeedMps() const;
	void  Reset();
};
```

- [ ] **Step 1: Write the failing tests** (`ThermalEntityModelTest.cpp`)

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Thermal/EntityThermal.h"

// CamSim.Thermal.Entity.Model.* / SpeedTracker.*: the pure 4C model (spec §1-2).

namespace
{
	const FEntityThermalSettings S;   // defaults
	TArray<FEntityThermalPartSpec> TruckParts()
	{
		FEntityThermalPartSpec Gear; Gear.Kind = EEntityThermalPartKind::RunningGear;
		FEntityThermalPartSpec Eng;  Eng.Kind  = EEntityThermalPartKind::Engine;
		FEntityThermalPartSpec Exh;  Exh.Kind  = EEntityThermalPartKind::Exhaust;
		return { Gear, Eng, Exh };
	}
	FEntityThermalInputs Env(double SimSec, float Tair = 280.0f, float B = 285.0f)
	{
		FEntityThermalInputs In; In.SimSec = SimSec; In.bHasEnv = true; In.TairK = Tair; In.BaselineK = B; return In;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityThermalTargetsTest, "CamSim.Thermal.Entity.Model.Targets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEntityThermalTargetsTest::RunTest(const FString& Parameters)
{
	const TArray<FEntityThermalPartSpec> P = TruckParts();
	FEntityThermalState St;
	float Skin = 0.0f, Parts[4] = {};
	// Parked, engine off, no env: everything 0 excess (exactly 4A)
	FEntityThermalInputs Off; Off.SimSec = 10.0;
	CamSimEntityThermal::Targets(St, Off, S, P, Skin, Parts);
	TestEqual(TEXT("off skin"), Skin, 0.0f);
	for (int32 K = 0; K < 3; ++K) TestEqual(*FString::Printf(TEXT("off part %d"), K), Parts[K], 0.0f);
	// Engine on, parked, env Tair 280, B 285 (D = -5): skin = run = 4; engine = D + 45 = 40; exhaust = 450 - 285 = 165; gear = D + 0 = -5
	FEntityThermalInputs On = Env(10.0); On.Cmd.bEngineOn = true;
	CamSimEntityThermal::Targets(St, On, S, P, Skin, Parts);
	TestNearlyEqual(TEXT("running skin"), Skin, 4.0f, 1e-4f);
	TestNearlyEqual(TEXT("gear parked"), Parts[0], -5.0f, 1e-4f);
	TestNearlyEqual(TEXT("engine"), Parts[1], 40.0f, 1e-4f);
	TestNearlyEqual(TEXT("exhaust"), Parts[2], 165.0f, 1e-4f);
	// Moving at 10 m/s: c = 0.5; skin = D (1 - c) + run c = -2.5 + 2 = -0.5; gear = D + min(15, 30) = 10
	FEntityThermalInputs Mv = Env(10.0); Mv.SpeedMps = 10.0f;
	CamSimEntityThermal::Targets(St, Mv, S, P, Skin, Parts);
	TestNearlyEqual(TEXT("convection skin"), Skin, -0.5f, 1e-4f);
	TestNearlyEqual(TEXT("gear moving"), Parts[0], 10.0f, 1e-4f);
	// Gear cap: 40 m/s -> min(60, 30) = 30 -> D + 30 = 25
	Mv.SpeedMps = 40.0f;
	CamSimEntityThermal::Targets(St, Mv, S, P, Skin, Parts);
	TestNearlyEqual(TEXT("gear capped"), Parts[0], 25.0f, 1e-4f);
	// Part overrides
	TArray<FEntityThermalPartSpec> O = P; O[1].DeltaK = 25.0f; O[2].TempK = 400.0f;
	CamSimEntityThermal::Targets(St, On, S, O, Skin, Parts);
	TestNearlyEqual(TEXT("engine override"), Parts[1], 20.0f, 1e-4f);
	TestNearlyEqual(TEXT("exhaust override"), Parts[2], 115.0f, 1e-4f);
	TestNearlyEqual(TEXT("c(0) = 1"), CamSimEntityThermal::ConvectionFactor(0.0f, 10.0f), 1.0f, 1e-6f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityThermalRunningTest, "CamSim.Thermal.Entity.Model.RunningLogic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEntityThermalRunningTest::RunTest(const FString& Parameters)
{
	FEntityThermalState St;
	FEntityThermalInputs In = Env(0.0);
	TestFalse(TEXT("parked off"), CamSimEntityThermal::IsRunning(St, In, S));
	In.Cmd.bEngineOn = true;
	TestTrue(TEXT("commanded on"), CamSimEntityThermal::IsRunning(St, In, S));
	In.Cmd.bEngineOn = false; In.SpeedMps = 3.0f;
	TestTrue(TEXT("explicit off while moving still runs"), CamSimEntityThermal::IsRunning(St, In, S));
	// Moving at t=0, stops: held until idle_hold_s
	const TArray<FEntityThermalPartSpec> P = TruckParts();
	CamSimEntityThermal::Step(St, In, S, P);   // records LastMovingSec = 0
	FEntityThermalInputs Stop = Env(100.0);
	TestTrue(TEXT("idle hold at 100 s"), CamSimEntityThermal::IsRunning(St, Stop, S));
	Stop.SimSec = 121.0;
	TestFalse(TEXT("hold expired at 121 s"), CamSimEntityThermal::IsRunning(St, Stop, S));
	Stop.Cmd.bEngineOn = true; Stop.Cmd.Damage = 2;
	TestFalse(TEXT("destroyed never runs"), CamSimEntityThermal::IsRunning(St, Stop, S));
	Stop.Cmd.Damage = 0; Stop.Cmd.bFlaming = true;
	TestFalse(TEXT("flaming never runs"), CamSimEntityThermal::IsRunning(St, Stop, S));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityThermalLagTest, "CamSim.Thermal.Entity.Model.Lag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEntityThermalLagTest::RunTest(const FString& Parameters)
{
	const TArray<FEntityThermalPartSpec> P = TruckParts();
	FEntityThermalState St;
	// Spawn parked off: at target (0)
	CamSimEntityThermal::Step(St, Env(0.0), S, P);
	TestTrue(TEXT("initialised"), St.bInitialized);
	TestEqual(TEXT("spawn exhaust"), St.PartExcessK[2], 0.0f);
	// Engine on: exhaust rises with tau_up 20 s. After 20 s in 1/30 s steps: 165 (1 - e^-1)
	FEntityThermalInputs On = Env(0.0); On.Cmd.bEngineOn = true;
	for (int32 I = 1; I <= 600; ++I) { On.SimSec = I / 30.0; CamSimEntityThermal::Step(St, On, S, P); }
	TestNearlyEqual(TEXT("exhaust after 1 tau"), St.PartExcessK[2], 165.0f * (1.0f - FMath::Exp(-1.0f)), 0.05f);
	// One 20 s step gives the same (exact exponential)
	FEntityThermalState St2;
	CamSimEntityThermal::Step(St2, Env(0.0), S, P);
	On.SimSec = 20.0; CamSimEntityThermal::Step(St2, On, S, P);
	TestNearlyEqual(TEXT("step-size independent"), St2.PartExcessK[2], St.PartExcessK[2], 0.05f);
	// Engine off after warm-up: falls with tau_down 60 s (target 0, parked: hold window starts... it never moved)
	FEntityThermalState W; CamSimEntityThermal::Step(W, On, S, P);   // spawn running: at target 165
	TestNearlyEqual(TEXT("spawn running at target"), W.PartExcessK[2], 165.0f, 1e-3f);
	FEntityThermalInputs OffIn = Env(20.0 + 60.0);
	CamSimEntityThermal::Step(W, OffIn, S, P);
	TestNearlyEqual(TEXT("exhaust after 1 tau down"), W.PartExcessK[2], 165.0f * FMath::Exp(-1.0f), 0.05f);
	// dt == 0: no change
	const float Before = W.PartExcessK[2];
	CamSimEntityThermal::Step(W, OffIn, S, P);
	TestEqual(TEXT("frozen clock"), W.PartExcessK[2], Before);
	// Negative dt snaps to target (0)
	OffIn.SimSec = 10.0;
	CamSimEntityThermal::Step(W, OffIn, S, P);
	TestEqual(TEXT("rewind snaps"), W.PartExcessK[2], 0.0f);
	// Jump > 1 h snaps
	FEntityThermalInputs Far = On; Far.SimSec = 10.0 + 3601.0;
	CamSimEntityThermal::Step(W, Far, S, P);
	TestNearlyEqual(TEXT("jump snaps to running target"), W.PartExcessK[2], 165.0f, 1e-3f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityThermalBurnTest, "CamSim.Thermal.Entity.Model.Burn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEntityThermalBurnTest::RunTest(const FString& Parameters)
{
	const TArray<FEntityThermalPartSpec> P = TruckParts();
	FEntityThermalState St;
	CamSimEntityThermal::Step(St, Env(0.0), S, P);
	FEntityThermalInputs D = Env(1.0); D.Cmd.Damage = 2;
	float Skin = 0.0f, Parts[4] = {};
	CamSimEntityThermal::Step(St, D, S, P);
	CamSimEntityThermal::Targets(St, D, S, P, Skin, Parts);
	TestNearlyEqual(TEXT("burn skin target = 700 - B"), Skin, 415.0f, 1e-3f);
	TestNearlyEqual(TEXT("burn part target"), Parts[1], 415.0f, 1e-3f);
	// Skin rises toward 415 with skin_tau 600 s; after the 300 s burn, target 0 and hull cooling tau 1800 s
	for (int32 I = 2; I <= 301; ++I) { D.SimSec = I; CamSimEntityThermal::Step(St, D, S, P); }
	const float Peak = St.SkinExcessK;
	TestNearlyEqual(TEXT("skin after burn"), Peak, 415.0f * (1.0f - FMath::Exp(-300.0f / 600.0f)), 0.5f);
	TestTrue(TEXT("burn done"), St.bBurnDone && St.bHullCooling);
	D.SimSec = 301.0 + 1800.0;
	CamSimEntityThermal::Step(St, D, S, P);
	TestNearlyEqual(TEXT("hull cools with 1800 s"), St.SkinExcessK, Peak * FMath::Exp(-1.0f), 0.5f);
	// Repair: leaving destroyed ends the burn state; flaming alone burns while set
	FEntityThermalInputs Fix = Env(D.SimSec + 1.0);
	CamSimEntityThermal::Step(St, Fix, S, P);
	TestTrue(TEXT("burn reset"), St.BurnStartSec < 0.0 && !St.bBurnDone);
	FEntityThermalInputs Fl = Env(Fix.SimSec + 1.0); Fl.Cmd.bFlaming = true;
	CamSimEntityThermal::Step(St, Fl, S, P);
	Fl.SimSec += 1000.0;   // flaming: no burn_s limit
	CamSimEntityThermal::Targets(St, Fl, S, P, Skin, Parts);
	TestNearlyEqual(TEXT("flaming burns past burn_s"), Skin, 415.0f, 1e-3f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityThermalNoEnvTest, "CamSim.Thermal.Entity.Model.NoEnvironmentStillLags",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEntityThermalNoEnvTest::RunTest(const FString& Parameters)
{
	// Review focus 4: EO-only driving still warms the state (D = 0), so IR shows a warm vehicle at the switch.
	const TArray<FEntityThermalPartSpec> P = TruckParts();
	FEntityThermalState St;
	FEntityThermalInputs In; In.SimSec = 0.0; In.SpeedMps = 8.0f;   // no env
	CamSimEntityThermal::Step(St, In, S, P);
	In.SimSec = 600.0;
	CamSimEntityThermal::Step(St, In, S, P);
	TestTrue(TEXT("engine warm"), St.PartExcessK[1] > 40.0f);
	TestTrue(TEXT("exhaust hot"), St.PartExcessK[2] > 150.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityThermalComponentTest, "CamSim.Thermal.Entity.Model.ComponentCommands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEntityThermalComponentTest::RunTest(const FString& Parameters)
{
	FEntityThermalCommanded C;
	TestTrue(TEXT("11 handled"), CamSimEntityThermal::ApplyComponent(C, 11, 1));
	TestTrue(TEXT("engine on"), C.bEngineOn);
	CamSimEntityThermal::ApplyComponent(C, 11, 0);
	TestFalse(TEXT("engine off"), C.bEngineOn);
	TestTrue(TEXT("12 handled"), CamSimEntityThermal::ApplyComponent(C, 12, 1));
	TestTrue(TEXT("flaming"), C.bFlaming);
	TestTrue(TEXT("10 handled"), CamSimEntityThermal::ApplyComponent(C, 10, 2));
	TestEqual(TEXT("destroyed"), C.Damage, static_cast<uint8>(2));
	CamSimEntityThermal::ApplyComponent(C, 10, 7);
	TestEqual(TEXT("damage clamped"), C.Damage, static_cast<uint8>(2));
	TestFalse(TEXT("lights not thermal"), CamSimEntityThermal::ApplyComponent(C, 0, 1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntitySpeedTrackerTest, "CamSim.Thermal.Entity.SpeedTracker.EmaAndTeleport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEntitySpeedTrackerTest::RunTest(const FString& Parameters)
{
	FEntitySpeedTracker T;
	const FVector P0(6378137.0, 0.0, 0.0);
	TestEqual(TEXT("first sample 0"), T.Update(P0, 0.0), 0.0f);
	// 10 m/s along Y for 5 s at 30 Hz: EMA converges to ~10
	for (int32 I = 1; I <= 150; ++I) T.Update(P0 + FVector(0.0, 10.0 * I / 30.0, 0.0), I / 30.0);
	TestNearlyEqual(TEXT("converged"), T.GetSpeedMps(), 10.0f, 0.1f);
	// dt <= 0 keeps v
	T.Update(P0 + FVector(0.0, 1000.0, 0.0), 5.0);
	TestNearlyEqual(TEXT("dt 0 keeps"), T.GetSpeedMps(), 10.0f, 0.1f);
	// Teleport 5 km: reset to 0
	T.Update(P0 + FVector(0.0, 5000.0, 0.0), 5.0 + 1.0 / 30.0);
	TestEqual(TEXT("teleport resets"), T.GetSpeedMps(), 0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntitySpeedJitterTest, "CamSim.Thermal.Entity.SpeedTracker.JitterStaysParked",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEntitySpeedJitterTest::RunTest(const FString& Parameters)
{
	// Review focus 1: +-2 cm pose noise at 30 Hz (surface clamp / DR corrections) must stay below moving_mps (0.5).
	FEntitySpeedTracker T;
	const FVector P0(6378137.0, 0.0, 0.0);
	float Max = 0.0f;
	for (int32 I = 0; I <= 300; ++I)
	{
		const double J = ((I * 7919) % 5 - 2) * 0.01;   // deterministic -0.02..+0.02 m
		Max = FMath::Max(Max, T.Update(P0 + FVector(J, -J, J), I / 30.0));
	}
	TestTrue(*FString::Printf(TEXT("jitter speed %.3f < 0.5"), Max), Max < 0.5f);
	return true;
}
```

- [ ] **Step 2: Run — expect compile failure**

- [ ] **Step 3: Implement `EntityThermal.h`** with exactly the declarations from **Interfaces → Produces** above (include `Thermal/EntityThermalTypes.h`; `CAMSIMTEST_API` on the class and functions), plus in `FEntitySpeedTracker` private members:

```cpp
	FVector LastEcefM = FVector::ZeroVector;
	double  LastSec = 0.0;
	bool    bHasLast = false;
	float   SpeedMps = 0.0f;
public:
	static constexpr double EmaTauS = 1.0;
	static constexpr double TeleportMinM = 50.0;
	static constexpr double TeleportMaxMps = 400.0;
```

- [ ] **Step 4: Implement `EntityThermal.cpp`**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Thermal/EntityThermal.h"

namespace CamSimEntityThermal
{
	static constexpr double MaxStepS = 3600.0;
	static constexpr float  MinExcessK = -200.0f, MaxExcessK = 900.0f;

	static bool IsBurning(const FEntityThermalState& St, const FEntityThermalInputs& In, const FEntityThermalSettings& S)
	{
		if (In.Cmd.bFlaming) return true;
		return In.Cmd.Damage >= 2 && St.BurnStartSec >= 0.0 && (In.SimSec - St.BurnStartSec) < static_cast<double>(S.BurnS);
	}

	bool IsRunning(const FEntityThermalState& St, const FEntityThermalInputs& In, const FEntityThermalSettings& S)
	{
		if (In.Cmd.Damage >= 2 || In.Cmd.bFlaming) return false;
		if (In.Cmd.bEngineOn) return true;
		if (In.SpeedMps > S.MovingMps) return true;
		return (In.SimSec - St.LastMovingSec) <= static_cast<double>(S.IdleHoldS);
	}

	float ConvectionFactor(float SpeedMps, float V0)
	{
		const float V = FMath::Max(SpeedMps, 0.0f);
		return V0 / (V0 + V);
	}

	void Targets(const FEntityThermalState& St, const FEntityThermalInputs& In, const FEntityThermalSettings& S,
		TConstArrayView<FEntityThermalPartSpec> Parts, float& OutSkin, float OutParts[FEntityThermalSettings::MaxParts])
	{
		const float B = In.bHasEnv ? In.BaselineK : 0.0f;
		const float D = In.bHasEnv ? (In.TairK - In.BaselineK) : 0.0f;
		const int32 N = FMath::Min(Parts.Num(), FEntityThermalSettings::MaxParts);
		for (int32 K = 0; K < FEntityThermalSettings::MaxParts; ++K) OutParts[K] = 0.0f;
		// Burning: every target burn_k (absolute). Without an environment B is unknown: use 288.15 K for it.
		if (IsBurning(St, In, S))
		{
			const float Burn = S.BurnK - (In.bHasEnv ? B : 288.15f);
			OutSkin = Burn;
			for (int32 K = 0; K < N; ++K) OutParts[K] = Burn;
			return;
		}
		const float C = ConvectionFactor(In.SpeedMps, S.ConvectionV0Mps);
		const bool bRun = IsRunning(St, In, S);
		OutSkin = D * (1.0f - C) + (bRun ? S.SkinRunningK * C : 0.0f);
		for (int32 K = 0; K < N; ++K)
		{
			const FEntityThermalPartSpec& P = Parts[K];
			const FEntityThermalKindParams& Kp = S.Kind(P.Kind);
			if (!bRun) { OutParts[K] = OutSkin; continue; }
			switch (P.Kind)
			{
			case EEntityThermalPartKind::Engine:
				OutParts[K] = D + P.DeltaK.Get(Kp.DeltaK);
				break;
			case EEntityThermalPartKind::Exhaust:
				OutParts[K] = P.TempK.Get(Kp.TempK) - (In.bHasEnv ? B : 288.15f);
				break;
			case EEntityThermalPartKind::RunningGear:
				OutParts[K] = D + FMath::Min(P.KPerMps.Get(Kp.KPerMps) * FMath::Max(In.SpeedMps, 0.0f), P.MaxK.Get(Kp.MaxK));
				break;
			default:
				OutParts[K] = OutSkin;
				break;
			}
		}
	}

	static float Relax(float T, float Target, double Dt, float TauUp, float TauDown)
	{
		const double Tau = FMath::Max(static_cast<double>(Target > T ? TauUp : TauDown), 1e-3);
		const float A = static_cast<float>(1.0 - FMath::Exp(-Dt / Tau));
		return FMath::Clamp(T + (Target - T) * A, MinExcessK, MaxExcessK);
	}

	void Step(FEntityThermalState& St, const FEntityThermalInputs& In, const FEntityThermalSettings& S,
		TConstArrayView<FEntityThermalPartSpec> Parts)
	{
		// Burn bookkeeping (before targets): start on the first destroyed sim time; leaving destroyed/flaming resets it.
		const bool bDestroyed = In.Cmd.Damage >= 2;
		if (bDestroyed && St.BurnStartSec < 0.0) { St.BurnStartSec = In.SimSec; St.bBurnDone = false; }
		if (!bDestroyed && !In.Cmd.bFlaming && (St.BurnStartSec >= 0.0 || St.bHullCooling))
		{
			St.BurnStartSec = -1.0; St.bBurnDone = false; St.bHullCooling = false;
		}
		if (bDestroyed && St.BurnStartSec >= 0.0 && (In.SimSec - St.BurnStartSec) >= static_cast<double>(S.BurnS)) St.bBurnDone = true;
		if ((St.bBurnDone || In.Cmd.bFlaming) && !St.bHullCooling) St.bHullCooling = St.bBurnDone;
		if (In.SpeedMps > S.MovingMps) St.LastMovingSec = In.SimSec;

		float SkinT = 0.0f, PartT[FEntityThermalSettings::MaxParts] = {};
		Targets(St, In, S, Parts, SkinT, PartT);
		const int32 N = FMath::Min(Parts.Num(), FEntityThermalSettings::MaxParts);
		const double Dt = In.SimSec - St.LastSimSec;
		if (!St.bInitialized || Dt < 0.0 || Dt > MaxStepS)
		{
			St.SkinExcessK = FMath::Clamp(SkinT, MinExcessK, MaxExcessK);
			for (int32 K = 0; K < FEntityThermalSettings::MaxParts; ++K) St.PartExcessK[K] = K < N ? FMath::Clamp(PartT[K], MinExcessK, MaxExcessK) : 0.0f;
			St.bInitialized = true;
			St.LastSimSec = In.SimSec;
			return;
		}
		if (Dt == 0.0) return;
		const float SkinDown = St.bHullCooling ? S.HullCoolTauS : S.SkinTauS;
		St.SkinExcessK = Relax(St.SkinExcessK, SkinT, Dt, S.SkinTauS, SkinDown);
		for (int32 K = 0; K < N; ++K)
		{
			const FEntityThermalKindParams& Kp = S.Kind(Parts[K].Kind);
			St.PartExcessK[K] = Relax(St.PartExcessK[K], PartT[K], Dt, Kp.TauUpS, Kp.TauDownS);
		}
		St.LastSimSec = In.SimSec;
	}

	bool ApplyComponent(FEntityThermalCommanded& C, uint16 ComponentId, uint8 State)
	{
		switch (ComponentId)
		{
		case 10: C.Damage = FMath::Min<uint8>(State, 2); return true;
		case 11: C.bEngineOn = State != 0; return true;
		case 12: C.bFlaming = State != 0; return true;
		default: return false;
		}
	}
}

float FEntitySpeedTracker::Update(const FVector& EcefM, double SimSec)
{
	if (!bHasLast || !EcefM.ContainsNaN() == false)
	{
		if (EcefM.ContainsNaN()) return SpeedMps;
		LastEcefM = EcefM; LastSec = SimSec; bHasLast = true; SpeedMps = 0.0f;
		return SpeedMps;
	}
	const double Dt = SimSec - LastSec;
	if (Dt <= 0.0) return SpeedMps;
	const double Dist = FVector::Dist(EcefM, LastEcefM);
	LastEcefM = EcefM; LastSec = SimSec;
	if (Dist > FMath::Max(TeleportMinM, TeleportMaxMps * Dt)) { SpeedMps = 0.0f; return SpeedMps; }
	const double Raw = Dist / Dt;
	const double A = 1.0 - FMath::Exp(-Dt / EmaTauS);
	SpeedMps = static_cast<float>(SpeedMps + (Raw - SpeedMps) * A);
	return SpeedMps;
}

float FEntitySpeedTracker::GetSpeedMps() const { return SpeedMps; }
void  FEntitySpeedTracker::Reset() { bHasLast = false; SpeedMps = 0.0f; }
```

Clean the first branch of `Update` to read plainly: `if (EcefM.ContainsNaN()) return SpeedMps; if (!bHasLast) { …first sample… }`. (Written above as it must behave; simplify the condition when typing it.)

Note on the hull-cooling flag: `bHullCooling` becomes true once `bBurnDone`; flaming alone never sets it (it cools with the hull constant only after a destroyed burn — the spec's "when flaming clears, cooling follows the hull constant" is implemented by setting `bHullCooling = true` when `In.Cmd.bFlaming` goes from set to clear). Implement that by tracking `bool bWasFlaming` in `FEntityThermalState` (add the field) and, in `Step`, `if (St.bWasFlaming && !In.Cmd.bFlaming && In.Cmd.Damage < 2) St.bHullCooling = true;` **before** the repair reset — and make the repair reset condition `!bDestroyed && !In.Cmd.bFlaming && !St.bWasFlaming && St.BurnStartSec >= 0.0` so a flaming-cleared entity keeps hull cooling until its skin is within 1 K of target, then clear `bHullCooling`. Add to `FEntityThermalBurnTest` before the final return:

```cpp
	Fl.Cmd.bFlaming = false; Fl.SimSec += 1.0;
	CamSimEntityThermal::Step(St, Fl, S, P);
	TestTrue(TEXT("flaming cleared: hull cooling"), St.bHullCooling);
```

and in the repair section expect `burn reset` only for the destroyed→intact transition (as written).

- [ ] **Step 5: Run — expect PASS** (`CamSim.Thermal.Entity.Model`, `CamSim.Thermal.Entity.SpeedTracker`). Fix numeric tolerances only if the expected value is analytically wrong (show the derivation in the commit message), never to hide a model bug.

- [ ] **Step 6: Commit** — `git commit -m "feat(thermal): pure entity thermal model and speed tracker (4C)"`

---

### Task 4: DIS appearance → component commands; CIGI CompId 11/12 on the entity

**Files:**
- Modify: `Source/CamSimTest/Hosts/DisCommands.h/.cpp`
- Modify: `Source/CamSimTest/DIS/DisEntityAdapter.h/.cpp`
- Modify: `Source/CamSimTest/Entity/CamSimEntity.h/.cpp` (`ApplyComponent`, new members)
- Test: `Source/CamSimTest/Tests/DisAppearanceTest.cpp`

**Interfaces:**
- Consumes: `CamSimEntityThermal::ApplyComponent`, `FEntityThermalCommanded` (Task 3).
- Produces:

```cpp
namespace CamSim::Dis
{
	struct FPlatformAppearance { bool bPowerPlant = false; uint8 Damage = 0; bool bFlaming = false; bool operator==(const FPlatformAppearance&) const = default; };
	/** Kind 1 (platform), domains 1-3: decoded; otherwise unset. */
	TOptional<FPlatformAppearance> DecodePlatformAppearance(uint8 Kind, uint8 Domain, uint32 Appearance);
	/** Component commands for the fields that changed (Previous unset: fields that differ from the defaults). */
	void AppearanceCommands(const FEntityKey& Key, const TOptional<FPlatformAppearance>& Previous, const FPlatformAppearance& Now,
		TArray<FComponentCommand>& Out);
}
// ACamSimEntity
const FEntityThermalCommanded& GetThermalCommanded() const;
```

- [ ] **Step 1: Write the failing test** (`DisAppearanceTest.cpp`)

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Hosts/DisCommands.h"

// CamSim.Thermal.Entity.DisAppearance.*: DIS Entity State appearance -> canonical component commands (ROADMAP 4C).

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDisAppearanceDecodeTest, "CamSim.Thermal.Entity.DisAppearance.Decode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDisAppearanceDecodeTest::RunTest(const FString& Parameters)
{
	using namespace CamSim::Dis;
	const uint32 Power = 1u << 22, Flaming = 1u << 15;
	auto Dmg = [](uint32 D) { return (D & 3u) << 3; };
	TOptional<FPlatformAppearance> A = DecodePlatformAppearance(1, 1, Power | Dmg(3) | Flaming);
	if (!TestTrue(TEXT("land decoded"), A.IsSet())) return false;
	TestTrue(TEXT("power"), A->bPowerPlant);
	TestEqual(TEXT("destroyed -> 2"), A->Damage, static_cast<uint8>(2));
	TestTrue(TEXT("flaming"), A->bFlaming);
	TestEqual(TEXT("slight -> 1"), DecodePlatformAppearance(1, 3, Dmg(1))->Damage, static_cast<uint8>(1));
	TestEqual(TEXT("moderate -> 1"), DecodePlatformAppearance(1, 2, Dmg(2))->Damage, static_cast<uint8>(1));
	TestEqual(TEXT("none -> 0"), DecodePlatformAppearance(1, 1, 0u)->Damage, static_cast<uint8>(0));
	TestFalse(TEXT("munition not decoded"), DecodePlatformAppearance(2, 1, Power).IsSet());
	TestFalse(TEXT("subsurface not decoded"), DecodePlatformAppearance(1, 4, Power).IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDisAppearanceCommandsTest, "CamSim.Thermal.Entity.DisAppearance.OnChangeOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDisAppearanceCommandsTest::RunTest(const FString& Parameters)
{
	using namespace CamSim::Dis;
	FEntityKey Key; Key.Source = EHostSource::Dis; Key.Id = 42;
	TArray<FComponentCommand> Out;
	FPlatformAppearance Zero;
	AppearanceCommands(Key, {}, Zero, Out);
	TestEqual(TEXT("first PDU at defaults: nothing"), Out.Num(), 0);
	FPlatformAppearance On; On.bPowerPlant = true;
	AppearanceCommands(Key, {}, On, Out);
	if (!TestEqual(TEXT("first PDU engine on: one command"), Out.Num(), 1)) return false;
	TestEqual(TEXT("comp 11"), Out[0].ComponentId, static_cast<uint16>(11));
	TestEqual(TEXT("state 1"), Out[0].State, static_cast<uint8>(1));
	TestTrue(TEXT("key"), Out[0].Key == Key);
	Out.Reset();
	AppearanceCommands(Key, On, On, Out);
	TestEqual(TEXT("heartbeat: nothing"), Out.Num(), 0);
	FPlatformAppearance Dead = On; Dead.bPowerPlant = false; Dead.Damage = 2; Dead.bFlaming = true;
	AppearanceCommands(Key, On, Dead, Out);
	TestEqual(TEXT("three changes"), Out.Num(), 3);
	TSet<uint16> Ids; for (const FComponentCommand& C : Out) Ids.Add(C.ComponentId);
	TestTrue(TEXT("10, 11, 12"), Ids.Contains(10) && Ids.Contains(11) && Ids.Contains(12));
	return true;
}
```

- [ ] **Step 2: Run — expect compile failure**

- [ ] **Step 3: Implement in `DisCommands`** (declarations as in Interfaces; `.cpp`):

```cpp
TOptional<CamSim::Dis::FPlatformAppearance> CamSim::Dis::DecodePlatformAppearance(uint8 Kind, uint8 Domain, uint32 Appearance)
{
	// IEEE 1278.1 / SISO-REF-010 platform appearance (land, air, surface share these bits): bits 3-4 damage,
	// bit 15 flaming, bit 22 power plant on.
	if (Kind != 1 || Domain < 1 || Domain > 3) return {};
	FPlatformAppearance A;
	A.bPowerPlant = ((Appearance >> 22) & 1u) != 0u;
	A.bFlaming    = ((Appearance >> 15) & 1u) != 0u;
	const uint32 D = (Appearance >> 3) & 3u;
	A.Damage = D == 3u ? 2 : (D == 0u ? 0 : 1);
	return A;
}

void CamSim::Dis::AppearanceCommands(const FEntityKey& Key, const TOptional<FPlatformAppearance>& Previous,
	const FPlatformAppearance& Now, TArray<FComponentCommand>& Out)
{
	const FPlatformAppearance Prev = Previous.Get(FPlatformAppearance());
	auto Emit = [&](uint16 Id, uint8 State)
	{
		FComponentCommand C; C.Key = Key; C.ComponentClass = 0; C.ComponentId = Id; C.State = State;
		Out.Add(C);
	};
	if (Now.Damage != Prev.Damage)           Emit(10, Now.Damage);
	if (Now.bPowerPlant != Prev.bPowerPlant) Emit(11, Now.bPowerPlant ? 1 : 0);
	if (Now.bFlaming != Prev.bFlaming)       Emit(12, Now.bFlaming ? 1 : 0);
}
```

Adapter (`DisEntityAdapter.h` private): `TMap<FDisEntityId, CamSim::Dis::FPlatformAppearance> LastAppearance;`. In `ProcessPdu` after the entity `Submit`:

```cpp
	// ROADMAP 4C: platform appearance (power plant, damage, flaming) as component commands, only on change.
	if (const TOptional<CamSim::Dis::FPlatformAppearance> A = CamSim::Dis::DecodePlatformAppearance(
		Pdu.EntityType.EntityKind, Pdu.EntityType.Domain, Pdu.Appearance))
	{
		const CamSim::Dis::FPlatformAppearance* Prev = LastAppearance.Find(Pdu.EntityId);
		TArray<FComponentCommand> Cmds;
		CamSim::Dis::AppearanceCommands(CamSim::Dis::Key(Pdu.EntityId), Prev ? TOptional<CamSim::Dis::FPlatformAppearance>(*Prev)
			: TOptional<CamSim::Dis::FPlatformAppearance>(), *A, Cmds);
		for (const FComponentCommand& C : Cmds) Sink.Submit(C);
		LastAppearance.Add(Pdu.EntityId, *A);
	}
```

In `SweepTimeouts` next to `EntityTimestamps.Remove(DisId);` add `LastAppearance.Remove(DisId);`.

Entity: in `CamSimEntity.h` add `#include "Thermal/EntityThermal.h"`, public `const FEntityThermalCommanded& GetThermalCommanded() const { return ThermalCmd; }`, private `FEntityThermalCommanded ThermalCmd;`. In `ApplyComponent` at the top after the class check:

```cpp
	// ROADMAP 4C: power plant (11), flaming (12) and damage (10) feed the thermal state; 10 also swaps meshes below.
	const bool bThermal = CamSimEntityThermal::ApplyComponent(ThermalCmd, C.ComponentId, C.State);
```

and add cases so 11/12 don't hit a default "unhandled" log:

```cpp
	case 11:
	case 12:
		UE_LOG(LogCamSim, Log, TEXT("ACamSimEntity[%u]: %s %s"), EntityId, C.ComponentId == 11 ? TEXT("power plant") : TEXT("flaming"),
			C.State != 0 ? TEXT("ON") : TEXT("OFF"));
		break;
```

(`bThermal` is otherwise unused; drop the variable if the compiler warns.)

- [ ] **Step 4: Run — expect PASS** (`CamSim.Thermal.Entity.DisAppearance`, and the existing `DisProtocolTest` tests: run `CamSim.DIS` / `CamSim.Dis` — whichever prefix that file uses).

- [ ] **Step 5: Commit** — `git commit -m "feat(dis,entity): appearance and CompId 11/12 feed entity thermal inputs (4C)"`

---

### Task 5: Frame params records, builder, environment, render-thread finalise

**Files:**
- Modify: `Source/CamSimShaders/Public/ThermalFrameParams.h`
- Modify: `Source/CamSimTest/Thermal/ThermalFrameBuilder.h/.cpp`
- Test: `Source/CamSimTest/Tests/ThermalEntityBuilderTest.cpp`

**Interfaces:**
- Consumes: Task 1 types, Task 3 `FEntityThermalState` excess values.
- Produces:

```cpp
// ThermalFrameParams.h
static constexpr int32 MaxEntityParts = 4;
static constexpr int32 EntityRecordFloat4s = 16;
FVector4f EntityRecords[NumStencils * EntityRecordFloat4s] = {};   // layout: spec §3
FVector3d EntityOriginWorld[NumStencils] = {};                      // UE world cm; CPU only (render thread finalises row w)
/** Fill rows 0-2 .w of every valid record: w_i = -dot(M_i.xyz, float(origin + PreViewTranslation)) (doubles until the dot). */
inline void FinalizeEntityRecords(FThermalFrameParams& P, const FVector3d& PreViewTranslation);
// ThermalFrameBuilder.h
struct FThermalStencilEntity { ...existing...;
	bool    bHasThermalState = false;      // 4C fields below are valid
	FVector OriginWorld = FVector::ZeroVector;  // actor location, UE world cm
	FQuat   RotationWorld = FQuat::Identity;
	float   SkinExcessK = 0.0f;
	TArray<FEntityThermalPartSpec, TInlineAllocator<4>> Parts;
	float   PartExcessK[4] = {};
};
struct FEntityThermalEnv { bool bValid = false; float TairK = 288.15f; float BaselineK[FThermalFrameParams::NumStencils] = {}; };
// FThermalFrameBuilder
const FEntityThermalEnv& GetEntityEnv() const;   // from the last Build
```

- [ ] **Step 1: Write the failing tests** (`ThermalEntityBuilderTest.cpp`)

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Thermal/ThermalFrameBuilder.h"
#include "Thermal/ThermalMaterials.h"
#include "Time/SimClock.h"

// CamSim.Thermal.Entity.Builder.*: entity records, environment, finalise (ROADMAP 4C).

namespace
{
	FThermalFrameInputs Night()
	{
		FThermalFrameInputs In;
		In.UtcMicros = FSimClock::ToMicros(FDateTime(2026, 12, 21, 10, 10));
		In.CamLatDeg = 37.7989; In.CamLonDeg = -122.4662; In.CamAltHaeM = 342.0;
		return In;
	}
	FThermalFrameBuilder Make(bool bEntity)
	{
		FCamSimConfig::FThermalConfig Cfg;
		Cfg.Entity.bEnabled = bEntity;
		FThermalFrameBuilder B;
		B.Configure(Cfg, 3.0f, 5.0f);
		return B;
	}
	FThermalStencilEntity Truck(uint8 Stencil)
	{
		FThermalStencilEntity E;
		E.Stencil = Stencil; E.bSurfaceVehicle = true; E.bHasThermalState = true;
		E.OriginWorld = FVector(1000.0, 2000.0, 300.0);
		E.RotationWorld = FQuat(FVector::UpVector, FMath::DegreesToRadians(90.0));   // bow toward UE +Y
		E.SkinExcessK = 3.0f;
		FEntityThermalPartSpec Eng; Eng.Kind = EEntityThermalPartKind::Engine; Eng.Shape = EEntityThermalPartShape::Ellipsoid;
		Eng.CentreM = FVector3f(3.0f, 0.5f, -1.5f); Eng.HalfM = FVector3f(0.7f, 0.6f, 0.4f); Eng.FalloffM = 0.25f;
		E.Parts.Add(Eng);
		E.PartExcessK[0] = 40.0f;
		return E;
	}
	const FVector4f& Row(const FThermalFrameParams& P, int32 S, int32 R) { return P.EntityRecords[S * FThermalFrameParams::EntityRecordFloat4s + R]; }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalEntityRecordTest, "CamSim.Thermal.Entity.Builder.EntityRecord",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalEntityRecordTest::RunTest(const FString& Parameters)
{
	FThermalFrameBuilder B = Make(true);
	FThermalFrameInputs In = Night();
	In.Entities = { Truck(7) };
	FThermalFrameParams P;
	B.Build(In, P);
	const float Base = P.ClassTempK[P.StencilClass[7]] + P.StencilOffsetK[7];
	TestEqual(TEXT("4C: implicit +8 K off"), P.StencilOffsetK[7], 0.0f);
	TestEqual(TEXT("valid"), Row(P, 7, 3).W, 1.0f);
	TestNearlyEqual(TEXT("skin = B + 3"), Row(P, 7, 3).X, Base + 3.0f, 1e-3f);
	TestEqual(TEXT("class"), Row(P, 7, 3).Y, static_cast<float>(P.StencilClass[7]));
	TestEqual(TEXT("parts"), Row(P, 7, 3).Z, 1.0f);
	TestEqual(TEXT("shape"), Row(P, 7, 4).W, 1.0f);
	TestEqual(TEXT("centre x"), Row(P, 7, 4).X, 3.0f);
	TestEqual(TEXT("falloff"), Row(P, 7, 5).W, 0.25f);
	TestNearlyEqual(TEXT("part T = B + 40"), Row(P, 7, 6).X, Base + 40.0f, 1e-3f);
	TestEqual(TEXT("untagged stencil invalid"), Row(P, 8, 3).W, 0.0f);
	// Transform: the world point 3 m ahead of the bow (UE +Y), 1.5 m up -> body (3, 0, -1.5)
	FinalizeEntityRecords(P, FVector3d(-500.0, -500.0, -100.0));   // a camera at (500, 500, 100)
	const FVector3f Pw = FVector3f(FVector3d(1000.0, 2000.0 + 300.0, 300.0 + 150.0) + FVector3d(-500.0, -500.0, -100.0));
	FVector3f Pb;
	for (int32 I = 0; I < 3; ++I) { const FVector4f M = Row(P, 7, I); Pb[I] = M.X * Pw.X + M.Y * Pw.Y + M.Z * Pw.Z + M.W; }
	TestNearlyEqual(TEXT("body x"), Pb.X, 3.0f, 1e-4f);
	TestNearlyEqual(TEXT("body y"), Pb.Y, 0.0f, 1e-4f);
	TestNearlyEqual(TEXT("body z (down)"), Pb.Z, -1.5f, 1e-4f);
	// Environment
	const FEntityThermalEnv& Env = B.GetEntityEnv();
	TestTrue(TEXT("env valid"), Env.bValid);
	TestNearlyEqual(TEXT("env Tair"), Env.TairK, P.TairK, 1e-6f);
	TestNearlyEqual(TEXT("env baseline"), Env.BaselineK[7], Base, 1e-4f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalEntityFarTest, "CamSim.Thermal.Entity.Builder.EntityRecordFarFromOrigin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalEntityFarTest::RunTest(const FString& Parameters)
{
	// Review focus 5: 100 km from the origin, camera 200 m away: body coordinates to 1 mm.
	FThermalFrameBuilder B = Make(true);
	FThermalFrameInputs In = Night();
	FThermalStencilEntity E = Truck(9);
	E.OriginWorld = FVector(1.0e7, -2.0e6, 5.0e4);
	In.Entities = { E };
	FThermalFrameParams P;
	B.Build(In, P);
	const FVector3d Cam(1.0e7 + 20000.0, -2.0e6, 5.0e4 + 3000.0);
	FinalizeEntityRecords(P, -Cam);
	const FVector3d World = E.OriginWorld + FVector3d(0.0, 300.0, 150.0);   // body (3, 0, -1.5) for the 90-degree yaw
	const FVector3f Pw = FVector3f(World - Cam);
	FVector3f Pb;
	for (int32 I = 0; I < 3; ++I) { const FVector4f M = Row(P, 9, I); Pb[I] = M.X * Pw.X + M.Y * Pw.Y + M.Z * Pw.Z + M.W; }
	TestNearlyEqual(TEXT("far x"), Pb.X, 3.0f, 1e-3f);
	TestNearlyEqual(TEXT("far y"), Pb.Y, 0.0f, 1e-3f);
	TestNearlyEqual(TEXT("far z"), Pb.Z, -1.5f, 1e-3f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalEntityReuseTest, "CamSim.Thermal.Entity.Builder.EntityRecordReuse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalEntityReuseTest::RunTest(const FString& Parameters)
{
	// Review focus 3: a stencil reused by an entity without parts carries none of the previous owner's parts.
	FThermalFrameBuilder B = Make(true);
	FThermalFrameInputs In = Night();
	In.Entities = { Truck(5) };
	FThermalFrameParams P;
	B.Build(In, P);
	FThermalStencilEntity Plain; Plain.Stencil = 5; Plain.bHasThermalState = true;
	In.Entities = { Plain };
	FThermalFrameParams Q;
	B.Build(In, Q);
	TestEqual(TEXT("no parts"), Row(Q, 5, 3).Z, 0.0f);
	TestEqual(TEXT("part row cleared"), Row(Q, 5, 6).X, 0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalEntityDisabledTest, "CamSim.Thermal.Entity.Builder.DisabledIs4A",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalEntityDisabledTest::RunTest(const FString& Parameters)
{
	FThermalFrameBuilder B = Make(false);
	FThermalFrameInputs In = Night();
	In.Entities = { Truck(3) };
	FThermalFrameParams P;
	B.Build(In, P);
	TestEqual(TEXT("4A +8 K"), P.StencilOffsetK[3], 8.0f);
	for (int32 S = 0; S < FThermalFrameParams::NumStencils; ++S)
	{
		if (Row(P, S, 3).W != 0.0f) { AddError(FString::Printf(TEXT("stencil %d valid with 4C off"), S)); break; }
	}
	TestFalse(TEXT("no env with 4C off"), B.GetEntityEnv().bValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalEntityRejectTest, "CamSim.Thermal.Entity.Builder.RejectsNonFinite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalEntityRejectTest::RunTest(const FString& Parameters)
{
	FThermalFrameBuilder B = Make(true);
	FThermalFrameInputs In = Night();
	FThermalStencilEntity BadOrigin = Truck(10); BadOrigin.OriginWorld.X = NAN;
	FThermalStencilEntity BadPart = Truck(11); BadPart.PartExcessK[0] = INFINITY;
	In.Entities = { BadOrigin, BadPart };
	FThermalFrameParams P;
	TArray<FString> Warnings;
	B.Build(In, P, &Warnings);
	TestEqual(TEXT("bad origin: invalid record"), Row(P, 10, 3).W, 0.0f);
	TestEqual(TEXT("bad part: record valid"), Row(P, 11, 3).W, 1.0f);
	TestEqual(TEXT("bad part dropped"), Row(P, 11, 3).Z, 0.0f);
	TestTrue(TEXT("warned"), Warnings.Num() >= 2);
	return true;
}
```

- [ ] **Step 2: Run — expect compile failure**

- [ ] **Step 3: Params** — in `ThermalFrameParams.h` after `EntityDepthRatio`:

```cpp
	// Entity thermal records (ROADMAP 4C), indexed by stencil, EntityRecordFloat4s float4 each:
	//   0-2  world->body rows: Pb_i = dot(xyz, Pw) + w (Pw translated world cm, Pb body m: X fwd, Y right, Z down); w set by
	//        FinalizeEntityRecords on the render thread
	//   3    (skin T K, class index, part count 0..MaxEntityParts, valid 0/1); invalid -> 4A's StencilClass/StencilOffsetK
	//   4+3k (centre xyz m, shape 0 box / 1 ellipsoid), 5+3k (half xyz m, falloff m), 6+3k (T K, 0, 0, 0)
	static constexpr int32 MaxEntityParts = 4;
	static constexpr int32 EntityRecordFloat4s = 4 + 3 * MaxEntityParts;
	FVector4f EntityRecords[NumStencils * EntityRecordFloat4s] = {};
	FVector3d EntityOriginWorld[NumStencils] = {};   // UE world cm (CPU only)
```

and after the struct, before `ThermalLutRadiance`:

```cpp
/** Render thread (and tests): rows 0-2 .w of every valid record from the entity origin in translated world (doubles until the dot). */
inline void FinalizeEntityRecords(FThermalFrameParams& P, const FVector3d& PreViewTranslation)
{
	for (int32 S = 1; S < FThermalFrameParams::NumStencils; ++S)
	{
		FVector4f* R = &P.EntityRecords[S * FThermalFrameParams::EntityRecordFloat4s];
		if (R[3].W == 0.0f) continue;
		const FVector3d O = P.EntityOriginWorld[S] + PreViewTranslation;
		for (int32 I = 0; I < 3; ++I)
		{
			R[I].W = static_cast<float>(-(static_cast<double>(R[I].X) * O.X + static_cast<double>(R[I].Y) * O.Y + static_cast<double>(R[I].Z) * O.Z));
		}
	}
}
```

`static_assert(FThermalFrameParams::EntityRecordFloat4s == 16, "CamSimThermalCommon.ush ENTITY_RECORD_FLOAT4S");` goes in `ThermalPass.cpp` in Task 7.

- [ ] **Step 4: Builder** — `ThermalFrameBuilder.h`: add `#include "Thermal/EntityThermalTypes.h"`, extend `FThermalStencilEntity` and add `FEntityThermalEnv` exactly as in Interfaces; in the class add `const FEntityThermalEnv& GetEntityEnv() const { return EntityEnv; }`, private `FEntityThermalEnv EntityEnv; TSet<uint8> WarnedEntityRecords;`. Update the `ThermalOffsetK` comment: "unset = +8 K for a surface vehicle with thermal.entity off, else 0".

`ThermalFrameBuilder.cpp`: replace the stencil-table loop (lines ~105-131) with:

```cpp
	// Stencil table (4A) and entity records (4C): rebuilt from the live entities every frame (a released stencil reverts to
	// the default and an invalid record).
	const bool bEntityThermal = Config.Entity.bEnabled;
	for (int32 S = 0; S < FThermalFrameParams::NumStencils; ++S)
	{
		Out.StencilClass[S]   = static_cast<uint8>(FThermalMaterialTable::VehiclePaint);
		Out.StencilOffsetK[S] = 0.0f;
	}
	FMemory::Memzero(Out.EntityRecords, sizeof(Out.EntityRecords));
	FMemory::Memzero(Out.EntityOriginWorld, sizeof(Out.EntityOriginWorld));
	EntityEnv.bValid = bEntityThermal;
	EntityEnv.TairK  = static_cast<float>(TairK);
	for (const FThermalStencilEntity& E : In.Entities)
	{
		if (E.Stencil == 0) continue;
		// ... existing class resolution (keep the unknown-material warning code exactly as it is), producing `Class` ...
		Out.StencilClass[E.Stencil]   = static_cast<uint8>(Class);
		Out.StencilOffsetK[E.Stencil] = E.ThermalOffsetK.IsSet() ? *E.ThermalOffsetK
			: ((E.bSurfaceVehicle && !bEntityThermal) ? DefaultVehicleOffsetK : 0.0f);
		const float BaseK = Out.ClassTempK[Class] + Out.StencilOffsetK[E.Stencil];
		EntityEnv.BaselineK[E.Stencil] = BaseK;
		if (bEntityThermal && E.bHasThermalState) WriteEntityRecord(E, static_cast<int32>(Class), BaseK, Out, Warn);
	}
```

where `Warn` is the existing lambda/helper that appends to `OutWarnings` (use whatever the current code uses for the unknown-material warning; if it is inline code, wrap it in a local lambda `auto Warn = [&](const FString& W) { ... }` and reuse). Add the private member function:

```cpp
void FThermalFrameBuilder::WriteEntityRecord(const FThermalStencilEntity& E, int32 Class, float BaseK, FThermalFrameParams& Out,
	TFunctionRef<void(const FString&)> Warn)
{
	auto Finite3 = [](const FVector& V) { return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z); };
	const FQuat Q = E.RotationWorld;
	const bool bQuat = FMath::IsFinite(Q.X) && FMath::IsFinite(Q.Y) && FMath::IsFinite(Q.Z) && FMath::IsFinite(Q.W) && Q.SizeSquared() > 1e-6;
	if (!Finite3(E.OriginWorld) || !bQuat || !FMath::IsFinite(E.SkinExcessK))
	{
		if (!WarnedEntityRecords.Contains(E.Stencil))
		{
			WarnedEntityRecords.Add(E.Stencil);
			Warn(FString::Printf(TEXT("entity stencil %u: non-finite pose or skin; 4A treatment for it"), E.Stencil));
		}
		return;
	}
	FVector4f* R = &Out.EntityRecords[E.Stencil * FThermalFrameParams::EntityRecordFloat4s];
	const FQuat U = Q.GetNormalized();
	const FVector Ax = U.GetAxisX(), Ay = U.GetAxisY(), Az = U.GetAxisZ();
	// UE actor frame (X fwd, Y right, Z up, cm) -> body (X fwd, Y right, Z down, m)
	R[0] = FVector4f(FVector3f(Ax / 100.0), 0.0f);
	R[1] = FVector4f(FVector3f(Ay / 100.0), 0.0f);
	R[2] = FVector4f(FVector3f(-Az / 100.0), 0.0f);
	Out.EntityOriginWorld[E.Stencil] = E.OriginWorld;
	int32 N = 0;
	for (int32 K = 0; K < E.Parts.Num() && K < FThermalFrameParams::MaxEntityParts; ++K)
	{
		const FEntityThermalPartSpec& P = E.Parts[K];
		if (!FMath::IsFinite(E.PartExcessK[K]))
		{
			if (!WarnedEntityRecords.Contains(E.Stencil))
			{
				WarnedEntityRecords.Add(E.Stencil);
				Warn(FString::Printf(TEXT("entity stencil %u: part %d temperature not finite; part dropped"), E.Stencil, K));
			}
			continue;
		}
		const int32 Row = 4 + 3 * N;
		R[Row + 0] = FVector4f(P.CentreM, P.Shape == EEntityThermalPartShape::Ellipsoid ? 1.0f : 0.0f);
		R[Row + 1] = FVector4f(P.HalfM, P.FalloffM);
		R[Row + 2] = FVector4f(FMath::Clamp(BaseK + E.PartExcessK[K], FThermalFrameParams::LutMinK, FThermalFrameParams::LutMaxK), 0.0f, 0.0f, 0.0f);
		++N;
	}
	R[3] = FVector4f(FMath::Clamp(BaseK + E.SkinExcessK, FThermalFrameParams::LutMinK, FThermalFrameParams::LutMaxK),
		static_cast<float>(Class), static_cast<float>(N), 1.0f);
}
```

Declare it in the header's private section. Note `PartExcessK[K]` index pairs with `Parts[K]` (the entity's spec order); the record packs only the kept parts (`N`), preserving order (priority).

When `Config.Entity.bEnabled` is false, also set `EntityEnv.bValid = false` (done above).

- [ ] **Step 5: Run — expect PASS** (`CamSim.Thermal.Entity.Builder`, and all `CamSim.Thermal.Builder` — the existing `StencilTableReuseAndUnknownMaterial` test expects +8 K for the truck with default config: **update it** to set `Cfg.Entity.bEnabled = false` in its `MakeBuilder` call for that test, and add one assertion that with 4C on the truck's offset is 0. Keep `PerFrameCost` passing; if it now exceeds its limit, report the measured cost rather than raising the limit silently.)

- [ ] **Step 6: Commit** — `git commit -m "feat(thermal): per-stencil entity records, environment and finalise (4C)"`

---

### Task 6: CPU reference — entity part temperature

**Files:**
- Modify: `Source/CamSimTest/Thermal/ThermalReference.h/.cpp`
- Test: `Source/CamSimTest/Tests/ThermalEntityReferenceTest.cpp`

**Interfaces:**
- Consumes: `FThermalFrameParams::EntityRecords` (Task 5).
- Produces: `float CamSimThermalRef::EntityPartTemp(const FThermalFrameParams& P, uint32 Stencil, const FVector3f& Pw)` (skin blended with parts; requires a valid record); `EvaluatePixel` uses records for entity pixels when valid.

- [ ] **Step 1: Failing test**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Thermal/ThermalReference.h"

// CamSim.Thermal.Entity.Reference.*: CamSimThermalRef::EntityPartTemp (spec §3).

namespace
{
	/** Identity body frame at translated-world origin O (cm): Pb = (Pw - O) / 100 with Z flipped. */
	FThermalFrameParams OneRecord(int32 S, float SkinK, const FVector3d& O)
	{
		FThermalFrameParams P;
		FVector4f* R = &P.EntityRecords[S * FThermalFrameParams::EntityRecordFloat4s];
		R[0] = FVector4f(0.01f, 0.0f, 0.0f, 0.0f);
		R[1] = FVector4f(0.0f, 0.01f, 0.0f, 0.0f);
		R[2] = FVector4f(0.0f, 0.0f, -0.01f, 0.0f);
		R[3] = FVector4f(SkinK, 2.0f, 0.0f, 1.0f);
		P.EntityOriginWorld[S] = O;
		FinalizeEntityRecords(P, FVector3d::ZeroVector);
		return P;
	}
	void AddPart(FThermalFrameParams& P, int32 S, int32 Shape, FVector3f C, FVector3f H, float F, float T)
	{
		FVector4f* R = &P.EntityRecords[S * FThermalFrameParams::EntityRecordFloat4s];
		const int32 K = static_cast<int32>(R[3].Z);
		R[4 + 3 * K] = FVector4f(C, static_cast<float>(Shape));
		R[5 + 3 * K] = FVector4f(H, F);
		R[6 + 3 * K] = FVector4f(T, 0.0f, 0.0f, 0.0f);
		R[3].Z = static_cast<float>(K + 1);
	}
	FVector3f BodyToWorld(const FVector3d& O, FVector3f Pb) { return FVector3f(FVector3d(O) + FVector3d(Pb.X, Pb.Y, -Pb.Z) * 100.0); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalEntityPartTempTest, "CamSim.Thermal.Entity.Reference.PartTemp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalEntityPartTempTest::RunTest(const FString& Parameters)
{
	const FVector3d O(500.0, -200.0, -300.0);
	FThermalFrameParams P = OneRecord(4, 290.0f, O);
	TestEqual(TEXT("no parts = skin"), CamSimThermalRef::EntityPartTemp(P, 4, BodyToWorld(O, FVector3f(1, 1, 1))), 290.0f);
	AddPart(P, 4, 0, FVector3f(2, 0, -1), FVector3f(0.5f, 0.5f, 0.5f), 0.2f, 350.0f);   // box
	TestEqual(TEXT("box inside"), CamSimThermalRef::EntityPartTemp(P, 4, BodyToWorld(O, FVector3f(2.2f, 0.1f, -1.1f))), 350.0f);
	// 0.1 m outside the face: w = 1 - smoothstep(0, 0.2, 0.1) = 0.5
	TestNearlyEqual(TEXT("box shell"), CamSimThermalRef::EntityPartTemp(P, 4, BodyToWorld(O, FVector3f(2.6f, 0.0f, -1.0f))), 320.0f, 1e-2f);
	TestEqual(TEXT("box outside"), CamSimThermalRef::EntityPartTemp(P, 4, BodyToWorld(O, FVector3f(3.0f, 0.0f, -1.0f))), 290.0f);
	AddPart(P, 4, 1, FVector3f(2, 0, -1), FVector3f(0.2f, 0.2f, 0.2f), 0.1f, 280.0f);   // cooler ellipsoid, later: wins in overlap
	TestEqual(TEXT("overlap: later part wins"), CamSimThermalRef::EntityPartTemp(P, 4, BodyToWorld(O, FVector3f(2.0f, 0.0f, -1.0f))), 280.0f);
	TestEqual(TEXT("cooler part outside it: box"), CamSimThermalRef::EntityPartTemp(P, 4, BodyToWorld(O, FVector3f(2.4f, 0.0f, -1.0f))), 350.0f);
	// Ellipsoid sphere shell: centre + 0.25 m (0.05 outside) -> w = 1 - smoothstep(0, 0.1, 0.05) = 0.5 of the way 350 -> 280
	TestNearlyEqual(TEXT("ellipsoid shell"), CamSimThermalRef::EntityPartTemp(P, 4, BodyToWorld(O, FVector3f(2.25f, 0.0f, -1.0f))), 315.0f, 1e-2f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalEntityEvaluateTest, "CamSim.Thermal.Entity.Reference.EvaluatePixelUsesRecord",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalEntityEvaluateTest::RunTest(const FString& Parameters)
{
	// An entity pixel with a valid record takes its temperature from the record; with an invalid one, 4A's table.
	FThermalFrameParams P;
	P.NumClasses = 3; P.ClassTempK[2] = 295.0f; P.ClassEmissivity[2] = 0.9f;
	for (int32 S = 0; S < 256; ++S) { P.StencilClass[S] = 2; P.StencilOffsetK[S] = 8.0f; }
	P.ClipToTranslatedWorld = CamSimThermalRef::MakeClipToTranslatedWorld(FRotator(-30.0, 0.0, 0.0), 60.0f, 64, 36, 10.0f);
	P.TairK = 288.15f;
	CamSimThermalRef::FPixelSample S;
	S.U = 0.5f; S.V = 0.5f; S.DeviceZ = 0.01f; S.CustomZ = 0.01f; S.Stencil = 6;
	TestNearlyEqual(TEXT("invalid record: 4A"), CamSimThermalRef::EvaluatePixel(P, S).TempK, 303.0f, 1e-3f);
	FVector4f* R = &P.EntityRecords[6 * FThermalFrameParams::EntityRecordFloat4s];
	R[0] = FVector4f(0.01f, 0, 0, 0); R[1] = FVector4f(0, 0.01f, 0, 0); R[2] = FVector4f(0, 0, -0.01f, 0);
	R[3] = FVector4f(310.0f, 2.0f, 0.0f, 1.0f);
	TestNearlyEqual(TEXT("valid record: skin"), CamSimThermalRef::EvaluatePixel(P, S).TempK, 310.0f, 1e-3f);
	return true;
}
```

- [ ] **Step 2: Run — expect compile failure**

- [ ] **Step 3: Implement** — `ThermalReference.h`: declare

```cpp
	/** Entity pixel temperature from a valid record (ROADMAP 4C): T = skin; for each part k in order, d = outside distance
	 *  (box: |max(|Pb - c| - h, 0)|; ellipsoid: (|(Pb - c) / h| - 1) min(h)), w = 1 - smoothstep(0, falloff, d),
	 *  T += (T_k - T) w. Pb = rows 0-2 of the record applied to Pw. HLSL twin: EntityPartTemp in CamSimThermalCommon.ush. */
	float EntityPartTemp(const FThermalFrameParams& P, uint32 Stencil, const FVector3f& Pw);
```

and update the header comment's step 2 to "class/offset from the stencil table, or (valid 4C record) class and EntityPartTemp".

`ThermalReference.cpp`:

```cpp
	static float SmoothStep01(float E0, float E1, float X)
	{
		const float T = FMath::Clamp((X - E0) / (E1 - E0), 0.0f, 1.0f);
		return T * T * (3.0f - 2.0f * T);
	}

	float EntityPartTemp(const FThermalFrameParams& P, uint32 Stencil, const FVector3f& Pw)
	{
		const FVector4f* R = &P.EntityRecords[(Stencil & 0xFFu) * FThermalFrameParams::EntityRecordFloat4s];
		const FVector3f Pb(
			R[0].X * Pw.X + R[0].Y * Pw.Y + R[0].Z * Pw.Z + R[0].W,
			R[1].X * Pw.X + R[1].Y * Pw.Y + R[1].Z * Pw.Z + R[1].W,
			R[2].X * Pw.X + R[2].Y * Pw.Y + R[2].Z * Pw.Z + R[2].W);
		float T = R[3].X;
		const int32 N = FMath::Min(static_cast<int32>(R[3].Z), FThermalFrameParams::MaxEntityParts);
		for (int32 K = 0; K < N; ++K)
		{
			const FVector4f A = R[4 + 3 * K], H = R[5 + 3 * K], Tk = R[6 + 3 * K];
			const FVector3f D = Pb - FVector3f(A.X, A.Y, A.Z);
			float Dist;
			if (A.W > 0.5f)
			{
				const FVector3f Q(D.X / H.X, D.Y / H.Y, D.Z / H.Z);
				Dist = (FMath::Sqrt(Q.X * Q.X + Q.Y * Q.Y + Q.Z * Q.Z) - 1.0f) * FMath::Min3(H.X, H.Y, H.Z);
			}
			else
			{
				const FVector3f E(FMath::Max(FMath::Abs(D.X) - H.X, 0.0f), FMath::Max(FMath::Abs(D.Y) - H.Y, 0.0f), FMath::Max(FMath::Abs(D.Z) - H.Z, 0.0f));
				Dist = FMath::Sqrt(E.X * E.X + E.Y * E.Y + E.Z * E.Z);
			}
			const float W = 1.0f - SmoothStep01(0.0f, H.W, Dist);
			T = T + (Tk.X - T) * W;
		}
		return T;
	}
```

In `EvaluatePixel`, where the entity branch sets `Class`/`Offset` from `StencilClass`/`StencilOffsetK`, change to:

```cpp
		const FVector4f* Rec = &P.EntityRecords[Stencil * FThermalFrameParams::EntityRecordFloat4s];
		if (Rec[3].W != 0.0f)
		{
			Class = static_cast<uint32>(Rec[3].Y);
			bEntityRecord = true;   // T computed below from EntityPartTemp instead of Cd.X + Offset
		}
		else
		{
			Class = P.StencilClass[Stencil];
			Offset = P.StencilOffsetK[Stencil];
		}
```

declare `bool bEntityRecord = false;` with the other locals, and replace `float T = Cd.X + Offset;` with
`float T = bEntityRecord ? EntityPartTemp(P, Stencil, Pw) : Cd.X + Offset;` (Pw is the already computed translated-world position; keep the class clamp before `Cd` is read).

- [ ] **Step 4: Run — expect PASS** (`CamSim.Thermal.Entity.Reference`, and `CamSim.Thermal.Reference` unchanged).

- [ ] **Step 5: Commit** — `git commit -m "feat(thermal): CPU reference entity part temperature (4C)"`

---

### Task 7: ThermalCS records buffer, HLSL mirror, render-thread finalise, GPU test

**Files:**
- Modify: `unreal_project/CamSimTest/Shaders/Private/CamSimThermalCommon.ush`
- Modify: `Source/CamSimShaders/Private/ThermalPass.cpp`
- Modify: `Source/CamSimTest/Camera/CamSimFrameGrabExtension.cpp` (~line 182)
- Test: `Source/CamSimTest/Tests/ThermalGpuTest.cpp` (append `EntityPartsMatchesCpu`)

**Interfaces:**
- Consumes: Task 5 records, Task 6 CPU mirror.
- Produces: `StructuredBuffer<float4> EntityRecords` bound by `AddThermalPass` from `P.EntityRecords`.

- [ ] **Step 1: Write the failing GPU test** (append to `ThermalGpuTest.cpp`, reusing its `RunThermalOnGpu`, `FLayout`, `EBase`):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalGpuEntityPartsTest, "CamSim.GPU.Thermal.EntityPartsMatchesCpu",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalGpuEntityPartsTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI) { AddInfo(TEXT("skipped: NullRHI (run scripts/run_gpu_tests.sh)")); return true; }
	CamSimThermalTest::FThermalTestScene S = CamSimThermalTest::MakeScene(64, 36, 64, 36);
	// Stencil 3 (visible on terrain, U 0.60-0.70, V 0.70-0.80): a record whose origin sits at that patch's centre, rotated 30 deg,
	// with a box, an overlapping later ellipsoid and a cooler part; stencil 5 keeps 4A treatment (invalid record).
	const float U = 0.65f, V = 0.75f;
	const float Z = CamSimThermalTest::DeviceZForPlane(S.P, S.ViewRot, U, V, -700.0f);
	const FVector3f C = CamSimThermalRef::ClipToWorld(S.P, U * 2.0f - 1.0f, 1.0f - V * 2.0f, Z);
	const FQuat Q(FVector::UpVector, FMath::DegreesToRadians(30.0));
	FVector4f* R = &S.P.EntityRecords[3 * FThermalFrameParams::EntityRecordFloat4s];
	R[0] = FVector4f(FVector3f(Q.GetAxisX() / 100.0), 0.0f);
	R[1] = FVector4f(FVector3f(Q.GetAxisY() / 100.0), 0.0f);
	R[2] = FVector4f(FVector3f(-Q.GetAxisZ() / 100.0), 0.0f);
	R[3] = FVector4f(300.0f, 2.0f, 3.0f, 1.0f);
	R[4] = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);   R[5] = FVector4f(1.5f, 1.0f, 0.5f, 0.6f);   R[6] = FVector4f(360.0f, 0, 0, 0);
	R[7] = FVector4f(0.8f, 0.3f, 0.0f, 1.0f);   R[8] = FVector4f(0.7f, 0.5f, 0.4f, 0.3f);   R[9] = FVector4f(700.0f, 0, 0, 0);
	R[10] = FVector4f(-1.5f, -0.8f, 0.0f, 0.0f); R[11] = FVector4f(0.4f, 0.4f, 0.4f, 0.2f); R[12] = FVector4f(270.0f, 0, 0, 0);
	S.P.EntityOriginWorld[3] = FVector3d(C);
	FinalizeEntityRecords(S.P, FVector3d::ZeroVector);   // the test scene's translated world is its world
	const TArray<CamSimThermalRef::FPixelResult> Ref = CamSimThermalRef::Run(S.Images(true), S.P);
	FLayout L;
	L.ColorExtent = FIntPoint(S.W, S.H);
	L.DepthExtent = FIntPoint(S.DW, S.DH);
	const TArray<float> Gpu = RunThermalOnGpu(S, L, EBase::Float, {});
	if (!TestEqual(TEXT("readback"), Gpu.Num(), S.W * S.H)) return false;
	int32 Bad = 0, Hot = 0;
	float Worst = 0.0f;
	for (int32 I = 0; I < Gpu.Num(); ++I)
	{
		const float Rel = FMath::Abs(Gpu[I] - Ref[I].Radiance) / FMath::Max(FMath::Abs(Ref[I].Radiance), 1e-6f);
		Worst = FMath::Max(Worst, Rel);
		Bad += Rel <= 1e-4f ? 0 : 1;
		Hot += (Ref[I].Class == CamSimThermalRef::EPixelClass::Entity && Ref[I].TempK > 330.0f) ? 1 : 0;
	}
	AddInfo(FString::Printf(TEXT("max relative %.3g; %d hot entity pixels"), Worst, Hot));
	TestEqual(TEXT("pixels beyond 1e-4"), Bad, 0);
	TestTrue(TEXT("parts visible (hot entity pixels)"), Hot > 0);
	return true;
}
```

If `FLayout` has other required fields (`ColorMin`, `DepthMin`), leave them at their defaults (zero). If `RunThermalOnGpu`'s last parameter type differs, pass an empty value of that type.

- [ ] **Step 2: Shader** — in `CamSimThermalCommon.ush`, after `float4 StencilData[128];` add

```hlsl
#define ENTITY_RECORD_FLOAT4S 16         // FThermalFrameParams::EntityRecordFloat4s
#define ENTITY_MAX_PARTS 4
StructuredBuffer<float4> EntityRecords;  // ROADMAP 4C: per stencil, see ThermalFrameParams.h
```

and before `EvaluatePixel`:

```hlsl
/** CamSimThermalRef::EntityPartTemp (same order). */
float EntityPartTemp(uint Stencil, float3 Pw)
{
	const uint B = Stencil * ENTITY_RECORD_FLOAT4S;
	const float4 M0 = EntityRecords[B + 0u], M1 = EntityRecords[B + 1u], M2 = EntityRecords[B + 2u], R3 = EntityRecords[B + 3u];
	const float3 Pb = float3(M0.x * Pw.x + M0.y * Pw.y + M0.z * Pw.z + M0.w,
	                         M1.x * Pw.x + M1.y * Pw.y + M1.z * Pw.z + M1.w,
	                         M2.x * Pw.x + M2.y * Pw.y + M2.z * Pw.z + M2.w);
	float T = R3.x;
	const uint N = min((uint)R3.z, (uint)ENTITY_MAX_PARTS);
	for (uint K = 0u; K < N; ++K)
	{
		const float4 A = EntityRecords[B + 4u + 3u * K];
		const float4 H = EntityRecords[B + 5u + 3u * K];
		const float  Tk = EntityRecords[B + 6u + 3u * K].x;
		const float3 D = Pb - A.xyz;
		float Dist;
		if (A.w > 0.5)
		{
			const float3 Q = float3(D.x / H.x, D.y / H.y, D.z / H.z);
			Dist = (sqrt(Q.x * Q.x + Q.y * Q.y + Q.z * Q.z) - 1.0) * min(H.x, min(H.y, H.z));
		}
		else
		{
			const float3 E = float3(max(abs(D.x) - H.x, 0.0), max(abs(D.y) - H.y, 0.0), max(abs(D.z) - H.z, 0.0));
			Dist = sqrt(E.x * E.x + E.y * E.y + E.z * E.z);
		}
		const float St = saturate((Dist - 0.0) / (H.w - 0.0));
		const float W = 1.0 - St * St * (3.0 - 2.0 * St);
		T = T + (Tk - T) * W;
	}
	return T;
}
```

(`saturate((X − E0)/(E1 − E0))` then `t²(3 − 2t)` matches `SmoothStep01`; do not use HLSL `smoothstep`, its rounding may differ.) In `EvaluatePixel`'s entity branch:

```hlsl
	bool bEntityRecord = false;
	...
	if (Stencil > 0u && S.CustomZ >= S.DeviceZ * EntityDepthRatio)
	{
		const float4 R3 = EntityRecords[Stencil * ENTITY_RECORD_FLOAT4S + 3u];
		if (R3.w != 0.0)
		{
			Class = (uint)R3.y;
			bEntityRecord = true;
		}
		else
		{
			const float4 E = StencilData[Stencil >> 1u];
			const bool bOdd = (Stencil & 1u) != 0u;
			Class = (uint)(bOdd ? E.z : E.x);
			Offset = bOdd ? E.w : E.y;
		}
		bTerrain = false;
	}
```

and `float T = bEntityRecord ? EntityPartTemp(Stencil, Pw) : Cd.x + Offset;`.

- [ ] **Step 3: Pass** — `ThermalPass.cpp`: `static_assert(FThermalFrameParams::EntityRecordFloat4s == 16, "ENTITY_RECORD_FLOAT4S in CamSimThermalCommon.ush");`, add `SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, EntityRecords)` to the parameter struct (after `StencilData`), and in `AddThermalPass` before the dispatch:

```cpp
	// ROADMAP 4C: per-stencil entity records (64 KB), uploaded with this frame's parameters.
	const FRDGBufferRef Records = CreateStructuredBuffer(GraphBuilder, TEXT("CamSimThermalEntityRecords"), sizeof(FVector4f),
		FThermalFrameParams::NumStencils * FThermalFrameParams::EntityRecordFloat4s, P.EntityRecords, sizeof(P.EntityRecords));
	Pass->EntityRecords = GraphBuilder.CreateSRV(Records);
```

(`CreateStructuredBuffer` is in `RenderGraphUtils.h`, already included; it copies the data by default.)

- [ ] **Step 4: Render-thread finalise** — `CamSimFrameGrabExtension.cpp` after `TP.ClipToTranslatedWorld = ...`:

```cpp
	// ROADMAP 4C: entity record rows' w from each origin in this view's translated world (doubles until the dot).
	FinalizeEntityRecords(TP, View.ViewMatrices.GetPreViewTranslation());
```

- [ ] **Step 5: Build, run GPU tests** — `scripts/run_gpu_tests.sh CamSim.GPU.Thermal`: 7/7 pass (6 existing + new). Then the NullRHI `CamSim` suite.

- [ ] **Step 6: Commit** — `git commit -m "feat(thermal): ThermalCS entity part hot spots from per-stencil records (4C)"`

---

### Task 8: Entity integration — state on the actor, stepping, environment wiring, shipped defaults

**Files:**
- Modify: `Source/CamSimTest/Entity/CamSimEntity.h/.cpp`
- Modify: `Source/CamSimTest/Entity/CamSimEntityManager.h/.cpp`
- Modify: `Source/CamSimTest/Camera/CamSimCaptureComponent.cpp` (after `ThermalBuilder.Build`)
- Modify: `deploy/camsim_config.yaml` (`thermal.entity` block; `thermal_parts` for 2001 and 3001)
- Test: `Source/CamSimTest/Tests/ThermalEntityIntegrationTest.cpp`

**Interfaces:**
- Consumes: Tasks 1–5.
- Produces: `void FCamSimEntityManager::SetEntityThermalEnv(const FEntityThermalEnv&)`; `void FCamSimEntityManager::StepEntityThermal(double SimSec)` (public for tests); `ACamSimEntity::StepThermal(double SimSec, const FEntityThermalSettings&, bool bHasEnv, float TairK, float BaselineK)`; `ACamSimEntity::GetThermalState()`.

- [ ] **Step 1: Write the failing test** (world-free parts only; the actor path is covered by acceptance in Task 10):

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Thermal/EntityThermal.h"
#include "Geospatial/EcefFrames.h"

// CamSim.Thermal.Entity.Integration.*: geodetic poses -> ECEF speed -> model, as ACamSimEntity::StepThermal does it.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityThermalGeodeticDriveTest, "CamSim.Thermal.Entity.Integration.GeodeticDrive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEntityThermalGeodeticDriveTest::RunTest(const FString& Parameters)
{
	FEntityThermalSettings S;
	FEntityThermalPartSpec Exh; Exh.Kind = EEntityThermalPartKind::Exhaust;
	const TArray<FEntityThermalPartSpec> Parts = { Exh };
	FEntitySpeedTracker Speed;
	FEntityThermalState St;
	// 10 m/s north for 60 s at 30 Hz, from 37.795 N
	const double MPerDegLat = 111000.0;
	for (int32 I = 0; I <= 1800; ++I)
	{
		const double T = I / 30.0;
		const FVector E = CamSimFrames::GeodeticToEcef(37.795 + 10.0 * T / MPerDegLat, -122.46, 5.0);
		FEntityThermalInputs In; In.SimSec = T; In.SpeedMps = Speed.Update(E, T);
		CamSimEntityThermal::Step(St, In, S, Parts);
	}
	TestNearlyEqual(TEXT("speed ~10"), Speed.GetSpeedMps(), 10.0f, 0.2f);
	TestTrue(TEXT("exhaust hot after a minute of driving"), St.PartExcessK[0] > 150.0f);
	return true;
}
```

- [ ] **Step 2: Run — expect PASS once Task 3 exists** (this test pins the ECEF path; it should pass immediately — if it fails, fix Task 3, not the test).

- [ ] **Step 3: Actor** — `CamSimEntity.h` public:

```cpp
	/** Step the thermal state (ROADMAP 4C) at sim time SimSec: speed from this entity's ECEF position, targets from the
	 *  environment (bHasEnv false: D = 0). Called by the entity manager once per tick after poses are final. */
	void StepThermal(double SimSec, const FEntityThermalSettings& Settings, bool bHasEnv, float TairK, float BaselineK);
	const FEntityThermalState& GetThermalState() const { return ThermalState; }
```

private: `FEntityThermalState ThermalState; FEntitySpeedTracker ThermalSpeed;`

`CamSimEntity.cpp`:

```cpp
void ACamSimEntity::StepThermal(double SimSec, const FEntityThermalSettings& Settings, bool bHasEnv, float TairK, float BaselineK)
{
	CamSimFrames::FGeoPose Pose;
	float SpeedMps = ThermalSpeed.GetSpeedMps();
	if (GetGeoPose(Pose))
	{
		SpeedMps = ThermalSpeed.Update(CamSimFrames::GeodeticToEcef(Pose.Lat, Pose.Lon, Pose.Alt), SimSec);
	}
	FEntityThermalInputs In;
	In.Cmd = ThermalCmd;
	In.Cmd.Damage = DamageState;   // CompId 10 is applied to DamageState too; keep one source
	In.SpeedMps = SpeedMps;
	In.SimSec = SimSec;
	In.bHasEnv = bHasEnv;
	In.TairK = TairK;
	In.BaselineK = BaselineK;
	const FEntityTypeEntry* Entry = TypeTable ? TypeTable->FindEntry(EntityType) : nullptr;
	static const TArray<FEntityThermalPartSpec> NoParts;
	CamSimEntityThermal::Step(ThermalState, In, Settings, Entry ? TConstArrayView<FEntityThermalPartSpec>(Entry->ThermalParts) : NoParts);
}
```

Add `#include "Geospatial/EcefFrames.h"` and `#include "Entity/EntityTypeTable.h"` if not already included. A type change (`SetEntityType` to a different type) resets thermal state: at the top of `SetEntityType`, `if (Type != EntityType) { ThermalState = FEntityThermalState(); }`.

- [ ] **Step 4: Manager** — header: `#include "Thermal/ThermalFrameBuilder.h"` is heavy; instead forward-declare `struct FEntityThermalEnv;` and hold `TUniquePtr<FEntityThermalEnv> ThermalEnv;` plus the methods:

```cpp
	/** Latch the thermal environment of the last IR frame (ROADMAP 4C; capture component, game thread). */
	void SetEntityThermalEnv(const FEntityThermalEnv& Env);
	/** Step every entity's thermal state at SimSec (Tick calls it after poses are final; public for tests). */
	void StepEntityThermal(double SimSec);
```

`.cpp` (include `Thermal/ThermalFrameBuilder.h`, `Time/SimClock.h`):

```cpp
void FCamSimEntityManager::SetEntityThermalEnv(const FEntityThermalEnv& Env)
{
	if (!ThermalEnv) ThermalEnv = MakeUnique<FEntityThermalEnv>();
	*ThermalEnv = Env;
}

void FCamSimEntityManager::StepEntityThermal(double SimSec)
{
	if (!Subsystem) return;
	const FEntityThermalSettings& Settings = Subsystem->GetConfig().Thermal.Entity;
	if (!Settings.bEnabled) return;
	const bool bEnv = ThermalEnv.IsValid() && ThermalEnv->bValid;
	for (const TPair<FEntityKey, ACamSimEntity*>& KV : EntityMap)
	{
		ACamSimEntity* E = KV.Value;
		if (!IsValid(E)) continue;
		const uint8* Stencil = StencilOf.Find(KV.Key);
		const bool bTagged = bEnv && Stencil && *Stencil != 0;
		E->StepThermal(SimSec, Settings, bTagged, bTagged ? ThermalEnv->TairK : 0.0f, bTagged ? ThermalEnv->BaselineK[*Stencil] : 0.0f);
	}
}
```

(Use whatever accessor the subsystem exposes for the live config — search `GetConfig()` in `CamSimSubsystem.h`; if it returns a pointer, adapt.) Call it at the end of `Tick`, after `if (Camera) Camera->FollowAttachParent();`:

```cpp
	// ROADMAP 4C: entity thermal state on sim time, after every pose is final for this tick.
	StepEntityThermal(static_cast<double>(FSimClock::Get().NowMicros()) * 1e-6);
```

`GetThermalStencilEntities`: after `T.bSurfaceVehicle = ...`:

```cpp
		// ROADMAP 4C
		const FEntityThermalState& St = E->GetThermalState();
		T.bHasThermalState = St.bInitialized;
		T.OriginWorld   = E->GetActorLocation();
		T.RotationWorld = E->GetActorQuat();
		T.SkinExcessK   = St.SkinExcessK;
		if (const FEntityTypeEntry* Type = TypeTable ? TypeTable->FindEntry(E->EntityType) : nullptr)
		{
			for (int32 K = 0; K < Type->ThermalParts.Num() && K < FEntityThermalSettings::MaxParts; ++K)
			{
				T.Parts.Add(Type->ThermalParts[K]);
				T.PartExcessK[K] = St.PartExcessK[K];
			}
		}
```

(Merge with the existing `Type` lookup in that function rather than looking it up twice.)

- [ ] **Step 5: Capture component** — after `ThermalBuilder.Build(TIn, *ThermalParams, &Warnings);`:

```cpp
		// ROADMAP 4C: the entity thermal model steps with this frame's T_air and baselines (latched until the next IR frame).
		if (FCamSimEntityManager* EM = Subsystem ? Subsystem->GetEntityManager() : nullptr)
		{
			EM->SetEntityThermalEnv(ThermalBuilder.GetEntityEnv());
		}
```

- [ ] **Step 6: Shipped defaults** — `deploy/camsim_config.yaml`: under `thermal:` (after `land_cover:`), add

```yaml
  # Entity thermal state (ROADMAP 4C, docs/thermal.md): engine/exhaust/running-gear hot spots, speed-cooled skin,
  # burning when destroyed, first-order lag on sim time. false = 4A (one class + offset per entity).
  # Env: CAMSIM_THERMAL_ENTITY_ENABLED
  entity:
    enabled: true
    moving_mps: 0.5          # faster than this = moving (engine counts as running)
    idle_hold_s: 120         # engine counts as running this long after the vehicle stopped
    skin_running_k: 4        # skin above the paint class while running
    convection_v0_mps: 10    # skin solar excess scales by v0 / (v0 + v)
    skin_tau_s: 600
    burn_k: 700              # destroyed (CIGI CompId 10 = 2, DIS damage 3) / flaming: every surface at this for burn_s
    burn_s: 300
    hull_cool_tau_s: 1800
    engine:       { delta_k: 45, tau_up_s: 300, tau_down_s: 900 }
    exhaust:      { temp_k: 450, tau_up_s: 20, tau_down_s: 60 }
    running_gear: { k_per_mps: 1.5, max_k: 30, tau_up_s: 180, tau_down_s: 600 }
```

and the spec's `thermal_parts` for `"2001"` (replace the 4A comment line about `thermal_offset_k` +8 K with "thermal_offset_k: additive skin offset (default 0; +8 K only with thermal.entity off)") and `"3001"` exactly as in the spec §4. Check the yaml path to `thermal:` in that file and keep indentation consistent.

- [ ] **Step 7: Build, run** `CamSim` NullRHI suite (all pass), then `scripts/run.sh` briefly with `send_dis_test.py both` and `camsim.Thermal.Log 1` to confirm no warnings about entity records. Exit criteria: log shows `entities=2` and no "non-finite pose" warning.

- [ ] **Step 8: Commit** — `git commit -m "feat(entity): step entity thermal state on sim time; publish environment; Ural/Mako parts (4C)"`

---

### Task 9: DIS test sender — appearance and park/destroy scenarios

**Files:**
- Modify: `scripts/send_dis_test.py`

**Interfaces:**
- Produces: `pack_entity_state(..., appearance: int = 0)`; presets `truck-park` (drive the truck loop for `--drive-s` seconds, default 120, then stop with power plant off and keep sending), `truck-parked` (stationary at the loop start, power plant off), `truck-destroyed` (stationary, damage 3, flaming); option `--engine-on` sets bit 22 on every truck PDU of any preset.

- [ ] **Step 1: Implement** — add constants and thread `appearance` through `pack_entity_state` (offset 84, `>I`, currently packed as 0):

```python
APPEARANCE_POWER_PLANT = 1 << 22  # IEEE 1278.1 platform appearance: power plant on
APPEARANCE_FLAMING = 1 << 15
APPEARANCE_DESTROYED = 3 << 3  # damage bits 3-4 = 3


def truck_appearance(preset: str, t: float, drive_s: float, engine_on: bool) -> int:
    """Appearance bits for the truck at time t (seconds since start) in a preset."""
    a = APPEARANCE_POWER_PLANT if engine_on else 0
    if preset == "truck-destroyed":
        return APPEARANCE_DESTROYED | APPEARANCE_FLAMING
    if preset == "truck-park" and t < drive_s:
        return APPEARANCE_POWER_PLANT
    return a
```

In `pack_entity_state`, replace `pdu += struct.pack(">I", 0)  # appearance` with `pdu += struct.pack(">I", appearance & 0xFFFFFFFF)  # appearance`, adding `appearance: int = 0` as a keyword parameter. In the presets: `truck-park` uses the truck-loop path for `t < drive_s` and then holds the last position/heading with zero velocity; `truck-parked` and `truck-destroyed` hold the loop's start point with zero velocity. Register them in `PRESETS` and the argparse choices; add `ap.add_argument("--drive-s", type=float, default=120.0)` and `ap.add_argument("--engine-on", action="store_true")`. Update the module docstring's usage line. Heartbeats keep sending the same appearance (CamSim emits on change only).

- [ ] **Step 2: Verify the PDU** — add a self-check run:

```bash
python3 - <<'EOF'
import sys; sys.path.insert(0, "scripts")
import send_dis_test as sd, struct
pdu = sd.pack_entity_state(*sd._example_args(), appearance=sd.APPEARANCE_POWER_PLANT) if hasattr(sd, "_example_args") else None
print("ok" if pdu is None or struct.unpack(">I", pdu[84:88])[0] == 1 << 22 else "BAD")
print(sd.truck_appearance("truck-park", 10, 120, False) == sd.APPEARANCE_POWER_PLANT, sd.truck_appearance("truck-park", 130, 120, False) == 0)
EOF
```

Expected: `ok` and `True True`. If `pack_entity_state` needs positional arguments, build them as the `both` preset does and check bytes 84–88 directly instead of `_example_args`.

- [ ] **Step 3: Live smoke** — CamSim running with DIS: `python3 scripts/send_dis_test.py truck-park --drive-s 10 --duration 20` and confirm in the CamSim log `power plant ON` then `power plant OFF` exactly once each (emit on change).

- [ ] **Step 4: Commit** — `git commit -m "test(dis): appearance bits and truck park/parked/destroyed presets (4C)"`

---

### Task 10: Acceptance gates m–p, part tuning, shots

**Files:**
- Modify: `scripts/thermal_check.py`
- Modify: `deploy/camsim_config.yaml` (tuned part volumes only)

**Interfaces:**
- Consumes: Task 9 presets, existing `thermal_check.py` helpers (`box_mask`, `ring_mask`, `region_mean`, `percentile`, `load_view`, `capture_view`, `check_band`'s `add`).

- [ ] **Step 1: Runs** — add an `entity` run group (selected by `--runs entity` and included in the default set) with night views at the truck loop start, `oblique_on(out_m=60, up_m=40, fov=20, from_bearing=...)` framing the truck (copy the gate-b night view's pose builder), per band (MWIR, LWIR):
  1. `parked_cold`: sender `truck-parked`, capture after 10 s;
  2. `running`: sender `truck-park --drive-s 9999` with the camera tracking the loop like gate b's view (reuse gate b's capture: it is the running truck);
  3. `park`: sender `truck-park --drive-s 60`; captures at drive end + 5 s and + 180 s, camera at the stop point;
  4. `destroyed`: sender `truck-destroyed`, capture after 20 s (MWIR only).
  The sender is started by `run_once` today with `"both"`; make the preset part of `RunSpec` (`dis_preset: str = "both"`, plus `dis_args: list[str]`) and pass it through.

- [ ] **Step 2: Gates** — in `check_band`, for night views, with `tb` the truck's COCO box (as gate b finds it) and `y` the luma:

```python
def box_stats(y: np.ndarray, box: list[float]) -> tuple[float, float, float]:
    """(mean, median, p99) of luma inside a COCO box."""
    v = y[box_mask(y.shape, box)]
    return float(v.mean()), float(np.median(v)), float(np.percentile(v, 99))

# m: running - parked-cold box mean >= +5 DN (MWIR and LWIR)
add(rows, "m", run_mean - cold_mean, threshold=5.0, ok=(run_mean - cold_mean) >= 5.0)
# n: running hot spot: p99 - median >= +20 DN MWIR, +10 DN LWIR
lim_n = 20.0 if band == "mwir" else 10.0
add(rows, "n", run_p99 - run_med, threshold=lim_n, ok=(run_p99 - run_med) >= lim_n)
# o: cool-down: p99(+180 s) < p99(+5 s) and > parked-cold p99
ok_o = park180_p99 < park5_p99 and park180_p99 > cold_p99
add(rows, "o", park5_p99 - park180_p99, ok=ok_o, detail=f"p99 +5 s {park5_p99:.1f}, +180 s {park180_p99:.1f}, cold {cold_p99:.1f}")
# p: destroyed box mean - ring >= +60 DN (MWIR)
add(rows, "p", dead_mean - dead_ring, threshold=60.0, ok=(dead_mean - dead_ring) >= 60.0)
```

Register `("m", b, "night"), ("n", b, "night"), ("o", b, "night")` for both bands and `("p", "mwir", "night")` in `expected_rows` when the `entity` group runs. Write each view's overlay with the configured part volumes projected (reuse `overlay()`; project the body-frame box corners with the COCO `box3d` pose if available, otherwise skip the volume overlay and note it in the report).

- [ ] **Step 3: Run** — `python3 scripts/thermal_check.py --band both` (the full set, a–p). All gates pass. If m/n fail, **tune the part volumes in `deploy/camsim_config.yaml`** by inspecting the shots (`.cache/thermal_check/<run>/shots`) — the engine box must cover the hood, the exhaust the muffler/stack, the gear box the wheels without heating the cab or cargo bed; do not change thresholds. Record the tuned values in the spec §4 note and in the commit message.

- [ ] **Step 4: Gate f** — confirm `ThermalCS` p95 ≤ 0.5 ms at 1080p is still reported passing in the same run.

- [ ] **Step 5: Commit** — `git commit -m "test(thermal): 4C acceptance gates m-p; tuned Ural/Mako part volumes"`

---

### Task 11: Docs, roadmap, CLAUDE.md

**Files:**
- Modify: `docs/thermal.md` (new "Entity thermal state (4C)" section; update "Known limits": remove "Entities have one class plus an offset…"; config keys)
- Modify: `docs/configuration.md` (`thermal.entity.*` rows, `CAMSIM_THERMAL_ENTITY_ENABLED`, `entity_types.<id>.thermal_parts`, `thermal_offset_k` default change)
- Modify: `docs/entity-rendering.md` (CompId 11 power plant, 12 flaming; CompId 10 = 2 starts the burn)
- Modify: `docs/dis.md` (appearance bits decoded; destroyed DIS entities now swap to `mesh_destroyed`)
- Modify: `ROADMAP.md` (4C section: status, built, results table from Task 10 with gate values, performance, known issues, carried over: aircraft plumes, damaged effect, F-16 default parts, hot ground under parked vehicles, host-set initial state; **Editor / human changes: none**)
- Modify: `CLAUDE.md` (Thermal IR gotcha: one sentence on 4C — records per stencil finalised on the render thread, excess over baseline, CPU mirror `EntityPartTemp`, `CamSim.GPU.Thermal.EntityPartsMatchesCpu`; Testing count line: update the test/file counts by counting `IMPLEMENT_SIMPLE_AUTOMATION_TEST` occurrences and test files)

- [ ] **Step 1: Write the docs** (facts only from the implemented code and Task 10 results).
- [ ] **Step 2: Count tests** — `grep -rho 'IMPLEMENT_SIMPLE_AUTOMATION_TEST\|IMPLEMENT_COMPLEX_AUTOMATION_TEST\|IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST' unreal_project/CamSimTest/Source/CamSimTest/Tests | wc -l` and `ls unreal_project/CamSimTest/Source/CamSimTest/Tests/*.cpp | wc -l`; update CLAUDE.md's two places that state them (Architecture comment and Testing section), and the GPU count (`CamSim.GPU.*`).
- [ ] **Step 3: Commit** — `git commit -m "docs: entity thermal state (ROADMAP 4C)"`
