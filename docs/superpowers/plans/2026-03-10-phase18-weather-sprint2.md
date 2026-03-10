# Phase 18 Sprint 2 — Weather, Atmosphere & Particle Effects Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement Phase 18 items 18A (volumetric clouds), 18B (cloud shadows), 18F–18H (Niagara particle FX), 18I (decal cratering), and 18L (regional weather zones) in CamSim.

**Architecture:** The existing `CloudActor` in `ACamSimEnvironment` is extended with coverage and shadow parameters wired to `ApplyWeather()`. A new `FCamSimParticleManager` (owned by `FSubsystemImpl` Pimpl) attaches `UNiagaraComponent`s to entity actors based on entity type and CIGI Component Control events. `ACamSimEnvironment` gains distance-based weather zone blending using pre-configured YAML zones updated by CIGI `RegionId`.

**Tech Stack:** Unreal Engine 5.7, Niagara particle system, existing `AVolumetricCloud`/`UVolumetricCloudComponent`, CIGI Class Library (CCL v3), rapidyaml, UE Automation test framework.

---

## File Map

**Create:**
- `Source/CamSimTest/Environment/CamSimParticleManager.h` — `FCamSimParticleManager` class + `FEntityParticleState` struct
- `Source/CamSimTest/Environment/CamSimParticleManager.cpp` — implementation
- `Source/CamSimTest/Tests/Phase18WeatherTest.cpp` — 15 new automation tests

**Modify:**
- `Source/CamSimTest/CIGI/CigiPacketTypes.h` — add `EntityKind`, `EntityDomain`, `EntityCategory` to `FCigiEntityState`
- `Source/CamSimTest/CIGI/CigiReceiver.cpp` — populate new entity state fields from CCL
- `Source/CamSimTest/Config/CamSimConfig.h` — append new fields to `FPhase18Config`; add `FWeatherZoneConfig` struct for YAML-configured zone positions
- `Source/CamSimTest/Config/CamSimConfig.cpp` — YAML load + env var overrides for new fields
- `Source/CamSimTest/Environment/CamSimEnvironment.h` — add weather zone structs, camera position cache, new method declarations (all new includes go **before** `CamSimEnvironment.generated.h`)
- `Source/CamSimTest/Environment/CamSimEnvironment.cpp` — extend `ApplyWeather()` for 18A/18B; implement zone blending for 18L; add `SetCameraPosition()`
- `Source/CamSimTest/Camera/CamSimCamera.cpp` — call `SetCameraPosition()` from `Tick()`
- `Source/CamSimTest/Subsystem/CamSimSubsystem.h` — add `FCamSimParticleManager* GetParticleManager()` getter
- `Source/CamSimTest/Subsystem/CamSimSubsystem.cpp` — add `FCamSimParticleManager` to `FSubsystemImpl`
- `Source/CamSimTest/Entity/CamSimEntityManager.cpp` — call particle manager on spawn/update/remove/component-control
- `Source/CamSimTest/CamSimTest.Build.cs` — add `"Niagara"`, `"NiagaraCore"`
- `deploy/camsim_config.yaml` — append new `phase18:` fields including `weather_zones:` array
- `README.md` — Niagara/decal editor asset requirements
- `Plan.md` — mark 18A/18B/18F/18G/18H/18I/18L done

**Key architecture notes:**
- `CloudActor` (type `AVolumetricCloud`) is found via `TActorIterator` in `BeginPlay()` and already drives altitude/thickness in `ApplyWeather()`. This plan extends `ApplyWeather()` — no second cloud component is created.
- Weather zones are **position-configured in YAML** (`phase18.weather_zones[]` array with lat/lon/radius). CIGI `FCigiWeatherState.RegionId` (uint16) matches YAML zone IDs to update their weather parameters.
- Camera lat/lon for zone blending is cached in `ACamSimEnvironment` via `SetCameraPosition(double, double)` called from `ACamSimCamera::Tick()`.
- Entity IDs are `uint16` throughout — `FCamSimParticleManager` uses `TMap<uint16, FEntityParticleState>` to match the rest of the codebase.
- Contrail speed threshold uses altitude-only gating (speed is not in `FCigiEntityState`); noted as a known limitation.

---

## Chunk 1: Config, Build.cs, YAML, and README

### Task 1: Extend FPhase18Config with new fields

**Files:**
- Modify: `Source/CamSimTest/Config/CamSimConfig.h`
- Modify: `Source/CamSimTest/Config/CamSimConfig.cpp`
- Modify: `Source/CamSimTest/CamSimTest.Build.cs`
- Modify: `deploy/camsim_config.yaml`
- Create: `Source/CamSimTest/Tests/Phase18WeatherTest.cpp`

- [ ] **Step 1: Create Phase18WeatherTest.cpp with failing config tests**

These tests reference struct fields that don't exist yet — they must fail before Step 3.

Create `Source/CamSimTest/Tests/Phase18WeatherTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"

// ─── 18A/18B: Cloud config defaults ─────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18CloudConfigDefaultsTest,
    "CamSim.Phase18.CloudConfigDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18CloudConfigDefaultsTest::RunTest(const FString& Parameters)
{
    FCamSimConfig Cfg;
    TestFalse(TEXT("bVolumetricClouds off by default"),       Cfg.Phase18.bVolumetricClouds);
    TestNearlyEqual(TEXT("CloudShadowStrength default 0.6"),  Cfg.Phase18.CloudShadowStrength, 0.6f, 0.001f);
    return true;
}

// ─── 18L: Weather zone config defaults ──────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18WeatherZoneConfigDefaultsTest,
    "CamSim.Phase18.WeatherZoneConfigDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18WeatherZoneConfigDefaultsTest::RunTest(const FString& Parameters)
{
    FCamSimConfig Cfg;
    TestFalse(TEXT("bWeatherZones off by default"), Cfg.Phase18.bWeatherZones);
    return true;
}

// ─── 18F/G/H/I: Particle / crater config defaults ────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18ParticleConfigDefaultsTest,
    "CamSim.Phase18.ParticleConfigDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18ParticleConfigDefaultsTest::RunTest(const FString& Parameters)
{
    FCamSimConfig Cfg;
    TestNearlyEqual(TEXT("ContrailAltM default 8000"),   Cfg.Phase18.ContrailAltM,    8000.0f, 1.0f);
    TestNearlyEqual(TEXT("ContrailSpeedMs default 100"), Cfg.Phase18.ContrailSpeedMs, 100.0f,  1.0f);
    TestEqual(TEXT("SmokeComponentID default 1"),  Cfg.Phase18.SmokeComponentID,          1);
    TestEqual(TEXT("FireComponentID default 2"),   Cfg.Phase18.FireComponentID,            2);
    TestEqual(TEXT("CraterComponentID default 10"),Cfg.Phase18.CraterImpactComponentID,   10);
    TestEqual(TEXT("MaxCraters default 32"),        Cfg.Phase18.MaxCraters,               32);
    return true;
}
```

- [ ] **Step 2: Run to confirm FAIL** — `Cfg.Phase18.bVolumetricClouds` does not compile

- [ ] **Step 3: Append new fields to FPhase18Config in CamSimConfig.h**

Add a `FWeatherZoneConfig` struct **before** `FCamSimConfig` in the file:

```cpp
/** YAML-configured position for a regional weather zone (18L). */
struct FWeatherZoneConfig
{
    int32  ZoneID   = 0;
    double LatDeg   = 0.0;
    double LonDeg   = 0.0;
    float  RadiusM  = 10000.0f;
};
```

Then inside `struct FPhase18Config`, append after `VisibilityRangeM`:

```cpp
    // 18A/18B Volumetric cloud shadow strength (cloud actor exists in scene; Phase18 enables shadow)
    bool  bVolumetricClouds   = false;  // enable/disable cloud coverage wiring
    float CloudShadowStrength = 0.6f;   // [0,1] shadow intensity on terrain

    // 18L Regional weather zones — positions from YAML, parameters from CIGI RegionId
    bool                        bWeatherZones = false;
    TArray<FWeatherZoneConfig>  WeatherZoneConfigs;  // up to 16, populated from YAML array

    // 18F/18G/18H Niagara particle FX — soft asset paths, authored in UE editor
    FString NiagaraRotorWash  = TEXT("/Game/Effects/NS_RotorWash");
    FString NiagaraSmoke      = TEXT("/Game/Effects/NS_Smoke");
    FString NiagaraFire       = TEXT("/Game/Effects/NS_Fire");
    FString NiagaraContrail   = TEXT("/Game/Effects/NS_Contrail");
    float   ContrailAltM      = 8000.0f;  // altitude threshold (metres)
    // NOTE: speed threshold not enforced — FCigiEntityState has no velocity field.
    // ContrailSpeedMs is reserved for future use when speed data is available.
    float   ContrailSpeedMs   = 100.0f;
    int32   SmokeComponentID  = 1;   // CIGI Component Control CompId → smoke
    int32   FireComponentID   = 2;   // CIGI Component Control CompId → fire

    // 18I Decal cratering
    // NOTE: FCigiComponentControl has no CompData field — per-impact radius from CIGI is not
    // available. CraterDefaultRadiusM is used for all craters (configurable via YAML/env var).
    FString CraterDecalMaterial     = TEXT("/Game/Effects/M_Crater");
    int32   CraterImpactComponentID = 10;
    int32   MaxCraters              = 32;
    float   CraterDefaultRadiusM    = 5.0f;
```

- [ ] **Step 4: Add YAML loading in CamSimConfig.cpp**

In the `if (Root.has_child("phase18"))` block, append after the last existing `YamlFloat(P18, ...)` line:

```cpp
        // 18A/18B
        YamlBool (P18, "volumetric_clouds",          Cfg.Phase18.bVolumetricClouds);
        YamlFloat(P18, "cloud_shadow_strength",       Cfg.Phase18.CloudShadowStrength);
        // 18L Weather zones — array of {id, lat, lon, radius_m}
        YamlBool (P18, "weather_zones",               Cfg.Phase18.bWeatherZones);
        if (P18.has_child("zone_positions") && P18["zone_positions"].is_seq())
        {
            for (const ryml::ConstNodeRef& ZNode : P18["zone_positions"])
            {
                FWeatherZoneConfig ZCfg;
                YamlInt   (ZNode, "id",       ZCfg.ZoneID);
                YamlDouble(ZNode, "lat",      ZCfg.LatDeg);
                YamlDouble(ZNode, "lon",      ZCfg.LonDeg);
                YamlFloat (ZNode, "radius_m", ZCfg.RadiusM);
                if (Cfg.Phase18.WeatherZoneConfigs.Num() < 16)
                {
                    Cfg.Phase18.WeatherZoneConfigs.Add(ZCfg);
                }
            }
        }
        // 18F/G/H Niagara
        YamlString(P18, "niagara_rotor_wash",         Cfg.Phase18.NiagaraRotorWash);
        YamlString(P18, "niagara_smoke",               Cfg.Phase18.NiagaraSmoke);
        YamlString(P18, "niagara_fire",                Cfg.Phase18.NiagaraFire);
        YamlString(P18, "niagara_contrail",            Cfg.Phase18.NiagaraContrail);
        YamlFloat (P18, "contrail_alt_m",              Cfg.Phase18.ContrailAltM);
        YamlFloat (P18, "contrail_speed_ms",           Cfg.Phase18.ContrailSpeedMs);
        YamlInt   (P18, "smoke_component_id",          Cfg.Phase18.SmokeComponentID);
        YamlInt   (P18, "fire_component_id",           Cfg.Phase18.FireComponentID);
        // 18I
        YamlString(P18, "crater_decal_material",       Cfg.Phase18.CraterDecalMaterial);
        YamlInt   (P18, "crater_impact_component_id",  Cfg.Phase18.CraterImpactComponentID);
        YamlInt   (P18, "max_craters",                 Cfg.Phase18.MaxCraters);
        YamlFloat (P18, "crater_default_radius_m",     Cfg.Phase18.CraterDefaultRadiusM);
```

- [ ] **Step 5: Add env var overrides in CamSimConfig.cpp**

After the last existing Phase 18 env override:

```cpp
    Cfg.Phase18.bVolumetricClouds   = GetEnvInt(TEXT("CAMSIM_VOLUMETRIC_CLOUDS"),      Cfg.Phase18.bVolumetricClouds   ? 1 : 0) != 0;
    Cfg.Phase18.CloudShadowStrength = GetEnvFloat(TEXT("CAMSIM_CLOUD_SHADOW_STRENGTH"),Cfg.Phase18.CloudShadowStrength);
    Cfg.Phase18.bWeatherZones       = GetEnvInt(TEXT("CAMSIM_WEATHER_ZONES"),          Cfg.Phase18.bWeatherZones       ? 1 : 0) != 0;
    Cfg.Phase18.ContrailAltM        = GetEnvFloat(TEXT("CAMSIM_CONTRAIL_ALT_M"),       Cfg.Phase18.ContrailAltM);
    Cfg.Phase18.MaxCraters          = GetEnvInt(TEXT("CAMSIM_MAX_CRATERS"),            Cfg.Phase18.MaxCraters);
```

- [ ] **Step 6: Update deploy/camsim_config.yaml**

Append to the `phase18:` block:

```yaml
  # 18A/18B Volumetric cloud shadow (cloud actor must exist in the level)
  volumetric_clouds: false
  cloud_shadow_strength: 0.6   # [0,1] intensity of cloud shadow projected onto terrain

  # 18L Regional weather zones — CIGI RegionId > 0 updates zone parameters
  weather_zones: false
  zone_positions:              # static lat/lon positions; parameters updated by CIGI
    - { id: 1, lat: 37.0, lon: -122.0, radius_m: 10000.0 }
    # add more zones here (max 16)

  # 18F/18G/18H Niagara particle FX — asset paths authored in UE editor
  niagara_rotor_wash: "/Game/Effects/NS_RotorWash"
  niagara_smoke:      "/Game/Effects/NS_Smoke"
  niagara_fire:       "/Game/Effects/NS_Fire"
  niagara_contrail:   "/Game/Effects/NS_Contrail"
  contrail_alt_m: 8000.0    # metres — altitude above which contrails form
  contrail_speed_ms: 100.0  # reserved for future use (speed data not in CIGI entity state)
  smoke_component_id: 1     # CIGI Component Control CompId → smoke
  fire_component_id: 2      # CIGI Component Control CompId → fire

  # 18I Decal cratering
  crater_decal_material: "/Game/Effects/M_Crater"
  crater_impact_component_id: 10
  max_craters: 32
  crater_default_radius_m: 5.0
```

- [ ] **Step 7: Add Niagara to Build.cs**

In `Source/CamSimTest/CamSimTest.Build.cs`, add to `PublicDependencyModuleNames`:

```csharp
    "Niagara",
    "NiagaraCore",
```

- [ ] **Step 8: Run config tests — expect PASS**

Expected: 3 tests pass (`CloudConfigDefaults`, `WeatherZoneConfigDefaults`, `ParticleConfigDefaults`).

- [ ] **Step 9: Commit**

```bash
git add Source/CamSimTest/Config/CamSimConfig.h \
        Source/CamSimTest/Config/CamSimConfig.cpp \
        Source/CamSimTest/Tests/Phase18WeatherTest.cpp \
        Source/CamSimTest/CamSimTest.Build.cs \
        deploy/camsim_config.yaml
git commit -m "feat: Phase 18 sprint 2 — config fields, Niagara module, YAML defaults"
```

---

### Task 2: README — Editor Asset Requirements

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Add editor asset note to Gotchas section**

In `README.md`, find `## Gotchas` and add before the last bullet:

```markdown
- **Phase 18 Niagara assets**: Particle effects (18F rotor wash, 18G smoke/fire, 18H contrails) and decal cratering (18I) require assets authored in the UE editor. Create the following and save to `/Game/Effects/`:
  | Asset | Type | Key exposed parameters |
  |---|---|---|
  | `NS_RotorWash` | Niagara System | `Velocity` (float), `Density` (float), `Radius` (float) |
  | `NS_Smoke` | Niagara System | `EmitRate` (float), `Color` (LinearColor) |
  | `NS_Fire` | Niagara System | `EmitRate` (float), `Scale` (float) |
  | `NS_Contrail` | Niagara System | `Width` (float), `Opacity` (float) |
  | `M_Crater` | Decal Material | Normal map input, burn ring albedo, opacity mask |
  Asset paths are configurable via `phase18.niagara_*` and `phase18.crater_decal_material` in `camsim_config.yaml`. Missing assets log a warning and skip that effect — other effects still work.
```

- [ ] **Step 2: Commit**

```bash
git add README.md
git commit -m "docs: Phase 18 — add Niagara editor asset requirements to README"
```

---

## Chunk 2: Volumetric Clouds, Cloud Shadows, and Weather Zones

### Task 3: Extend FCigiEntityState with Kind/Domain/Category

**Files:**
- Modify: `Source/CamSimTest/CIGI/CigiPacketTypes.h`
- Modify: `Source/CamSimTest/CIGI/CigiReceiver.cpp`

- [ ] **Step 1: Add fields to FCigiEntityState in CigiPacketTypes.h**

Find `struct FCigiEntityState`. After the `Roll` field, append:

```cpp
    // Entity classification from CIGI EntityCtrlV3 — used by FCamSimParticleManager
    // Kind=1 (Platform), Domain=1 (Air), Category=2=fixed-wing, Category=3=rotary-wing
    uint8 EntityKind     = 0;
    uint8 EntityDomain   = 0;
    uint8 EntityCategory = 0;
```

- [ ] **Step 2: Populate from CCL in CigiReceiver.cpp**

In `FEntityCtrlProcessor::OnPacketReceived()`, after the existing field assignments, add:

```cpp
    State.EntityKind     = static_cast<uint8>(Pkt->GetEntityKind());
    State.EntityDomain   = static_cast<uint8>(Pkt->GetEntityDomain());
    State.EntityCategory = static_cast<uint8>(Pkt->GetEntityCategory());
```

CCL methods on `CigiEntityCtrlV3*`: `GetEntityKind()`, `GetEntityDomain()`, `GetEntityCategory()`.

- [ ] **Step 3: Commit**

```bash
git add Source/CamSimTest/CIGI/CigiPacketTypes.h \
        Source/CamSimTest/CIGI/CigiReceiver.cpp
git commit -m "feat: extend FCigiEntityState with Kind/Domain/Category from CCL"
```

---

### Task 4: ACamSimEnvironment — Volumetric Clouds (18A) + Cloud Shadows (18B)

The existing `CloudActor` (`TObjectPtr<AVolumetricCloud>`) is already found in `BeginPlay()` and its altitude/thickness are set in `ApplyWeather()`. This task extends `ApplyWeather()` to also drive cloud shadow and wires the coverage threshold.

**Files:**
- Modify: `Source/CamSimTest/Environment/CamSimEnvironment.h`
- Modify: `Source/CamSimTest/Environment/CamSimEnvironment.cpp`
- Modify: `Source/CamSimTest/Tests/Phase18WeatherTest.cpp`

- [ ] **Step 1: Write failing test**

Append to `Tests/Phase18WeatherTest.cpp`:

```cpp
// ─── 18A/18B: Coverage-to-shadow mapping ────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18CoverageToShadowTest,
    "CamSim.Phase18.CoverageToShadowMapping",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18CoverageToShadowTest::RunTest(const FString& Parameters)
{
    // CIGI Coverage field is percent 0-100; normalise to [0,1] and apply threshold
    const float CoveragePercent = 75.0f;
    const float Coverage01      = FMath::Clamp(CoveragePercent / 100.0f, 0.0f, 1.0f);
    const bool  bShadowOn       = (Coverage01 > 0.1f);
    TestNearlyEqual(TEXT("Coverage01 from 75%"), Coverage01, 0.75f, 0.001f);
    TestTrue(TEXT("Shadow enabled at 75% coverage"), bShadowOn);

    const float LowCoverage01 = FMath::Clamp(5.0f / 100.0f, 0.0f, 1.0f);
    TestFalse(TEXT("Shadow disabled at 5% coverage"), LowCoverage01 > 0.1f);
    return true;
}
```

- [ ] **Step 2: Run — expect PASS** (pure math, no UE types referenced)

- [ ] **Step 3: Add SetCloudShadowsEnabled declaration to CamSimEnvironment.h**

All new includes must go **before** `#include "CamSimEnvironment.generated.h"` (currently the last line of the includes block). Add one new private method declaration alongside the existing `ApplySecondFogLayer`, etc.:

```cpp
    void SetCloudShadowsEnabled(bool bEnabled, float Strength);
```

No new includes are needed — `UDirectionalLightComponent` is already available via existing includes.

- [ ] **Step 4: Implement SetCloudShadowsEnabled in CamSimEnvironment.cpp**

```cpp
void ACamSimEnvironment::SetCloudShadowsEnabled(bool bEnabled, float Strength)
{
    if (!SunLight) return;
    UDirectionalLightComponent* LightComp =
        Cast<UDirectionalLightComponent>(SunLight->GetLightComponent());
    if (!LightComp) return;
    LightComp->bCastCloudShadows   = bEnabled;
    LightComp->CloudShadowStrength = FMath::Clamp(Strength, 0.0f, 1.0f);
    LightComp->MarkRenderStateDirty();
}
```

- [ ] **Step 5: Extend the cloud block inside ApplyWeather()**

Find `ApplyWeather()` in `CamSimEnvironment.cpp`. It already has:

```cpp
if (CloudActor)
{
    UVolumetricCloudComponent* CloudComp = CloudActor->FindComponentByClass<UVolumetricCloudComponent>();
    if (CloudComp)
    {
        CloudComp->SetLayerBottomAltitude(FMath::Max(CurrentWeather.BaseElev / 1000.0f, 0.1f));
        CloudComp->SetLayerHeight(FMath::Max(CurrentWeather.Thickness / 1000.0f, 0.1f));
    }
}
```

Extend it to:

```cpp
if (CloudActor)
{
    UVolumetricCloudComponent* CloudComp = CloudActor->FindComponentByClass<UVolumetricCloudComponent>();
    if (CloudComp)
    {
        CloudComp->SetLayerBottomAltitude(FMath::Max(CurrentWeather.BaseElev / 1000.0f, 0.1f));
        CloudComp->SetLayerHeight(FMath::Max(CurrentWeather.Thickness / 1000.0f, 0.1f));

        // 18A: Show/hide cloud component based on coverage and enable flag
        const float Coverage01 = FMath::Clamp(CurrentWeather.Coverage / 100.0f, 0.0f, 1.0f);
        if (Phase18Cfg.bVolumetricClouds)
        {
            CloudComp->SetVisibility(Coverage01 > 0.01f);
        }

        // 18B: Drive cloud shadow from coverage threshold
        SetCloudShadowsEnabled(
            Phase18Cfg.bVolumetricClouds && Coverage01 > 0.1f,
            Phase18Cfg.CloudShadowStrength);
    }
}
```

- [ ] **Step 6: Commit**

```bash
git add Source/CamSimTest/Environment/CamSimEnvironment.h \
        Source/CamSimTest/Environment/CamSimEnvironment.cpp \
        Source/CamSimTest/Tests/Phase18WeatherTest.cpp
git commit -m "feat: Phase 18A/18B — cloud visibility and shadow wired to CIGI coverage"
```

---

### Task 5: ACamSimEnvironment — Weather Zone Blending (18L)

Zone positions come from YAML (`FWeatherZoneConfig` array). CIGI `FCigiWeatherState.RegionId > 0` updates zone parameters. Camera lat/lon for blending is cached in the environment actor and updated each game tick from `ACamSimCamera`.

**Files:**
- Modify: `Source/CamSimTest/Environment/CamSimEnvironment.h`
- Modify: `Source/CamSimTest/Environment/CamSimEnvironment.cpp`
- Modify: `Source/CamSimTest/Camera/CamSimCamera.cpp`
- Modify: `Source/CamSimTest/Tests/Phase18WeatherTest.cpp`

- [ ] **Step 1: Write failing tests**

Append to `Tests/Phase18WeatherTest.cpp`:

```cpp
// ─── 18L: Weather zone blending math ─────────────────────────────────────────

#include "Environment/CamSimEnvironment.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18WeatherZoneAddTest,
    "CamSim.Phase18.WeatherZone_Add",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18WeatherZoneAddTest::RunTest(const FString& Parameters)
{
    TArray<FWeatherZone> Zones;
    FWeatherZone Z; Z.ZoneID = 1; Z.LatDeg = 37.0; Z.LonDeg = -122.0; Z.RadiusM = 5000.0f;
    Z.Params.FogDensity = 0.8f;
    Zones.Add(Z);
    TestEqual(TEXT("One zone stored"), Zones.Num(), 1);
    TestNearlyEqual(TEXT("Zone fog density"), Zones[0].Params.FogDensity, 0.8f, 0.001f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18WeatherZoneRemoveTest,
    "CamSim.Phase18.WeatherZone_Remove",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18WeatherZoneRemoveTest::RunTest(const FString& Parameters)
{
    TArray<FWeatherZone> Zones;
    FWeatherZone Z; Z.ZoneID = 5;
    Zones.Add(Z);
    Zones.RemoveAll([](const FWeatherZone& W){ return W.ZoneID == 5; });
    TestEqual(TEXT("Zone removed"), Zones.Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18WeatherZoneBlendInsideTest,
    "CamSim.Phase18.WeatherZoneBlend_Inside",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18WeatherZoneBlendInsideTest::RunTest(const FString& Parameters)
{
    const float Dist = 0.0f, Radius = 5000.0f;
    const float Alpha = FMath::Clamp(1.0f - (Dist / Radius), 0.0f, 1.0f);
    TestNearlyEqual(TEXT("Alpha at center = 1"), Alpha, 1.0f, 0.001f);
    const float Result = FMath::Lerp(0.02f, 0.8f, Alpha);
    TestNearlyEqual(TEXT("Fog blended to zone value"), Result, 0.8f, 0.001f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18WeatherZoneBlendOutsideTest,
    "CamSim.Phase18.WeatherZoneBlend_Outside",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18WeatherZoneBlendOutsideTest::RunTest(const FString& Parameters)
{
    const float Dist = 10000.0f, Radius = 5000.0f;
    const float Alpha = FMath::Clamp(1.0f - (Dist / Radius), 0.0f, 1.0f);
    TestNearlyEqual(TEXT("Alpha outside = 0"), Alpha, 0.0f, 0.001f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18WeatherZoneCapTest,
    "CamSim.Phase18.WeatherZone_Cap16",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18WeatherZoneCapTest::RunTest(const FString& Parameters)
{
    TArray<FWeatherZone> Zones;
    for (int32 i = 0; i < 17; ++i)
    {
        if (Zones.Num() >= 16) { Zones.RemoveAt(0); }
        FWeatherZone Z; Z.ZoneID = i;
        Zones.Add(Z);
    }
    TestEqual(TEXT("Max 16 zones"), Zones.Num(), 16);
    TestEqual(TEXT("ID=0 evicted"), Zones[0].ZoneID, 1);
    return true;
}
```

- [ ] **Step 2: Run — expect FAIL** on `FWeatherZone` not found

- [ ] **Step 3: Add structs and members to CamSimEnvironment.h**

Add the following **before** `#include "CamSimEnvironment.generated.h"`:

```cpp
// No new includes needed — all types below are POD structs.
```

Add **before** the `ACamSimEnvironment` class declaration:

```cpp
/** Weather parameters for a regional zone (blended when camera is inside the radius). */
struct FWeatherZoneParams
{
    float FogDensity    = 0.0f;
    float VisibilityM   = 10000.0f;
    float CloudCoverage = 0.0f;
};

/** Runtime state of a weather zone — position from YAML config, params from CIGI. */
struct FWeatherZone
{
    int32  ZoneID  = 0;
    double LatDeg  = 0.0;
    double LonDeg  = 0.0;
    float  RadiusM = 10000.0f;
    FWeatherZoneParams Params;
};
```

Add private members to `ACamSimEnvironment` (alongside existing private member section):

```cpp
    TArray<FWeatherZone>  ActiveWeatherZones;    // runtime zones (up to 16)
    FWeatherZoneParams    GlobalWeatherBaseline; // fog density from last global weather update
    double                CameraLatDeg = 0.0;   // updated each tick via SetCameraPosition()
    double                CameraLonDeg = 0.0;
```

Add public and private method declarations:

```cpp
    // Public: called from ACamSimCamera::Tick() to update camera position for zone blending
    void SetCameraPosition(double LatDeg, double LonDeg);

private:
    void  UpdateWeatherZone(uint16 RegionId, const FWeatherZoneParams& Params);
    void  BlendWeatherZones();
    static float GreatCircleApproxM(double Lat1, double Lon1, double Lat2, double Lon2);
```

- [ ] **Step 4: Implement zone methods in CamSimEnvironment.cpp**

```cpp
void ACamSimEnvironment::SetCameraPosition(double LatDeg, double LonDeg)
{
    CameraLatDeg = LatDeg;
    CameraLonDeg = LonDeg;
}

void ACamSimEnvironment::UpdateWeatherZone(uint16 RegionId, const FWeatherZoneParams& Params)
{
    // Find existing zone with this ID and update its parameters
    for (FWeatherZone& Z : ActiveWeatherZones)
    {
        if (Z.ZoneID == static_cast<int32>(RegionId))
        {
            Z.Params = Params;
            return;
        }
    }
    // Zone not in runtime list — it may have a position configured in YAML.
    // Ignore if not pre-configured (position-less zones cannot be blended).
    UE_LOG(LogTemp, Verbose,
        TEXT("ACamSimEnvironment: RegionId %d has no YAML position config — ignoring zone update."),
        static_cast<int32>(RegionId));
}

float ACamSimEnvironment::GreatCircleApproxM(double Lat1, double Lon1, double Lat2, double Lon2)
{
    // Flat-earth approximation, valid for radii < ~100 km
    const double dLat = FMath::DegreesToRadians(Lat2 - Lat1);
    const double dLon = FMath::DegreesToRadians(Lon2 - Lon1)
                      * FMath::Cos(FMath::DegreesToRadians((Lat1 + Lat2) * 0.5));
    return static_cast<float>(FMath::Sqrt(dLat*dLat + dLon*dLon) * 6371000.0);
}

void ACamSimEnvironment::BlendWeatherZones()
{
    if (!Phase18Cfg.bWeatherZones || ActiveWeatherZones.IsEmpty()) return;

    float                     BestAlpha  = 0.0f;
    const FWeatherZoneParams* BestParams = nullptr;

    for (const FWeatherZone& Z : ActiveWeatherZones)
    {
        const float DistM = GreatCircleApproxM(CameraLatDeg, CameraLonDeg, Z.LatDeg, Z.LonDeg);
        const float Alpha = FMath::Clamp(1.0f - (DistM / Z.RadiusM), 0.0f, 1.0f);
        if (Alpha > BestAlpha) { BestAlpha = Alpha; BestParams = &Z.Params; }
    }

    if (BestAlpha <= 0.0f || !BestParams) return;

    // Blend fog
    if (HeightFog)
    {
        UExponentialHeightFogComponent* FogComp = HeightFog->GetComponent();
        if (FogComp)
        {
            FogComp->SetFogDensity(
                FMath::Lerp(GlobalWeatherBaseline.FogDensity, BestParams->FogDensity, BestAlpha));
        }
    }
}
```

- [ ] **Step 5: Initialize ActiveWeatherZones from config in BeginPlay()**

In `ACamSimEnvironment::BeginPlay()`, after `Phase18Cfg = ...`:

```cpp
    // 18L: Populate runtime zone list from YAML-configured positions
    for (const FWeatherZoneConfig& ZCfg : Phase18Cfg.WeatherZoneConfigs)
    {
        FWeatherZone Z;
        Z.ZoneID  = ZCfg.ZoneID;
        Z.LatDeg  = ZCfg.LatDeg;
        Z.LonDeg  = ZCfg.LonDeg;
        Z.RadiusM = ZCfg.RadiusM;
        ActiveWeatherZones.Add(Z);
    }
```

- [ ] **Step 6: Call BlendWeatherZones from Tick()**

In `ACamSimEnvironment::Tick()`, after the existing weather state drain loop, add:

```cpp
    // 18L: Blend zone weather toward camera
    BlendWeatherZones();
```

Also update `GlobalWeatherBaseline` when a global weather update arrives (in the `if (bGotWeather)` block, before `ApplyWeather()`):

```cpp
    GlobalWeatherBaseline.FogDensity = HeightFog
        ? HeightFog->GetComponent()->FogDensity
        : 0.0f;
```

- [ ] **Step 7: Wire CIGI RegionId > 0 → UpdateWeatherZone**

In `ACamSimEnvironment::Tick()`, inside the `while (Receiver->DequeueWeatherState(WxState))` loop, add:

```cpp
        if (WxState.RegionId > 0)
        {
            FWeatherZoneParams ZParams;
            ZParams.FogDensity   = FMath::Lerp(0.0f, 0.1f, WxState.Coverage / 100.0f);
            ZParams.VisibilityM  = WxState.VisibilityRng;
            ZParams.CloudCoverage = WxState.Coverage / 100.0f;
            UpdateWeatherZone(WxState.RegionId, ZParams);
        }
        else
        {
            bGotWeather = true;
        }
```

- [ ] **Step 8: Call SetCameraPosition from ACamSimCamera::Tick()**

In `Camera/CamSimCamera.cpp`, find where the camera's lat/lon is read from `GlobeAnchor` (near the telemetry building). After extracting `LatDeg` and `LonDeg`, add:

```cpp
    if (ACamSimEnvironment* Env = Subsystem->GetEnvironment())
    {
        Env->SetCameraPosition(Telemetry.Latitude, Telemetry.Longitude);
    }
```

`FCamSimTelemetry` fields are `Latitude` and `Longitude` (not `SensorLatDeg`).

- [ ] **Step 9: Run tests — expect PASS**

Expected: 5 zone tests pass (`Add`, `Remove`, `Blend_Inside`, `Blend_Outside`, `Cap16`).

- [ ] **Step 10: Commit**

```bash
git add Source/CamSimTest/Environment/CamSimEnvironment.h \
        Source/CamSimTest/Environment/CamSimEnvironment.cpp \
        Source/CamSimTest/Camera/CamSimCamera.cpp \
        Source/CamSimTest/Tests/Phase18WeatherTest.cpp
git commit -m "feat: Phase 18L — regional weather zone distance-based blending"
```

---

## Chunk 3: Particle Manager

### Task 6: FCamSimParticleManager — Header

**Files:**
- Create: `Source/CamSimTest/Environment/CamSimParticleManager.h`
- Modify: `Source/CamSimTest/Tests/Phase18WeatherTest.cpp`

- [ ] **Step 1: Write failing test**

Append to `Tests/Phase18WeatherTest.cpp`:

```cpp
// ─── 18F/G/H/I: Particle state defaults ──────────────────────────────────────

#include "Environment/CamSimParticleManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18ParticleStateDefaultsTest,
    "CamSim.Phase18.ParticleState_Defaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18ParticleStateDefaultsTest::RunTest(const FString& Parameters)
{
    FEntityParticleState State;
    TestNull(TEXT("RotorWashComp null"),   State.RotorWashComp);
    TestNull(TEXT("SmokeComp null"),       State.SmokeComp);
    TestNull(TEXT("FireComp null"),        State.FireComp);
    TestNull(TEXT("ContrailComp null"),    State.ContrailComp);
    TestFalse(TEXT("Smoke inactive"),      State.bSmokeActive);
    TestFalse(TEXT("Fire inactive"),       State.bFireActive);
    TestFalse(TEXT("Contrail inactive"),   State.bContrailActive);
    return true;
}
```

- [ ] **Step 2: Run — expect FAIL** (`FEntityParticleState` not found)

- [ ] **Step 3: Create CamSimParticleManager.h**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UCamSimSubsystem;
struct FCamSimConfig;
struct FCigiComponentControl;
struct FCigiEntityState;
class UNiagaraComponent;
class UNiagaraSystem;
class UDecalComponent;
class AActor;

/**
 * Per-entity particle effect state. All pointers are game-thread only.
 * bContrailActive tracks whether contrail is currently emitting (altitude threshold).
 */
struct FEntityParticleState
{
    UNiagaraComponent* RotorWashComp  = nullptr;
    UNiagaraComponent* SmokeComp      = nullptr;
    UNiagaraComponent* FireComp       = nullptr;
    UNiagaraComponent* ContrailComp   = nullptr;
    bool               bSmokeActive   = false;
    bool               bFireActive    = false;
    bool               bContrailActive = false;
};

/**
 * Manages entity-attached Niagara particle effects and decal craters.
 * Owned by UCamSimSubsystem::FSubsystemImpl. Game-thread only.
 *
 * Entity IDs use uint16 throughout to match the rest of the entity system.
 *
 * Phase 18: 18F (rotor wash), 18G (smoke/fire), 18H (contrails), 18I (craters).
 * Note: contrail speed gate is not enforced — FCigiEntityState has no velocity field.
 * Contrails activate on altitude threshold only.
 */
class FCamSimParticleManager
{
public:
    explicit FCamSimParticleManager(UCamSimSubsystem* InSubsystem);
    ~FCamSimParticleManager() = default;

    void Initialize(const FCamSimConfig& Config);

    void OnEntitySpawned(uint16 EntityID, AActor* Actor, const FCigiEntityState& State);
    void OnEntityUpdated(uint16 EntityID, AActor* Actor, const FCigiEntityState& State);
    void OnEntityRemoved(uint16 EntityID);
    void OnComponentControl(uint16 EntityID, AActor* Actor, const FCigiComponentControl& Pkt);
    void Tick(float DeltaTime);

private:
    void SpawnRotorWash(uint16 EntityID, AActor* Actor);
    void UpdateContrail(uint16 EntityID, AActor* Actor, float AltitudeM);
    void ActivateSmoke(uint16 EntityID, AActor* Actor);
    void ActivateFire(uint16 EntityID, AActor* Actor);
    void SpawnCraterDecal(AActor* Actor);
    void DetachAll(FEntityParticleState& State);

    static constexpr uint8 KindPlatform  = 1;
    static constexpr uint8 DomainAir     = 1;
    static constexpr uint8 CatFixedWing  = 2;
    static constexpr uint8 CatRotaryWing = 3;

    UCamSimSubsystem*   Subsystem      = nullptr;
    UNiagaraSystem*     RotorWashAsset = nullptr;
    UNiagaraSystem*     SmokeAsset     = nullptr;
    UNiagaraSystem*     FireAsset      = nullptr;
    UNiagaraSystem*     ContrailAsset  = nullptr;
    UMaterialInterface* CraterMaterial = nullptr;

    float ContrailAltM      = 8000.0f;
    int32 SmokeComponentID  = 1;
    int32 FireComponentID   = 2;
    int32 CraterComponentID = 10;
    int32 MaxCraters        = 32;
    float CraterRadiusM     = 5.0f;

    TMap<uint16, FEntityParticleState> EntityParticles;
    TArray<UDecalComponent*>           ActiveCraters;  // ring buffer
};
```

- [ ] **Step 4: Run test — expect PASS**

- [ ] **Step 5: Commit**

```bash
git add Source/CamSimTest/Environment/CamSimParticleManager.h \
        Source/CamSimTest/Tests/Phase18WeatherTest.cpp
git commit -m "feat: Phase 18 — FCamSimParticleManager header + FEntityParticleState"
```

---

### Task 7: FCamSimParticleManager — Implementation

**Files:**
- Create: `Source/CamSimTest/Environment/CamSimParticleManager.cpp`
- Modify: `Source/CamSimTest/Tests/Phase18WeatherTest.cpp`

- [ ] **Step 1: Write failing tests**

Append to `Tests/Phase18WeatherTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18ParticleMapRemoveTest,
    "CamSim.Phase18.ParticleMap_EntityRemoved",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18ParticleMapRemoveTest::RunTest(const FString& Parameters)
{
    TMap<uint16, FEntityParticleState> EntityParticles;
    EntityParticles.Add(42u, FEntityParticleState{});
    TestEqual(TEXT("Entity registered"), EntityParticles.Num(), 1);
    EntityParticles.Remove(42u);
    TestEqual(TEXT("Entity removed"),    EntityParticles.Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18CraterRingBufferTest,
    "CamSim.Phase18.Crater_RingBuffer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18CraterRingBufferTest::RunTest(const FString& Parameters)
{
    TArray<int32> FakeCraters;
    const int32 MaxCraters = 32;
    for (int32 i = 0; i < 33; ++i)
    {
        if (FakeCraters.Num() >= MaxCraters) { FakeCraters.RemoveAt(0); }
        FakeCraters.Add(i);
    }
    TestEqual(TEXT("Ring buffer at max"),  FakeCraters.Num(), MaxCraters);
    TestEqual(TEXT("Oldest (0) evicted"),  FakeCraters[0], 1);
    return true;
}
```

- [ ] **Step 2: Run — expect PASS** (pure data structure logic)

- [ ] **Step 3: Create CamSimParticleManager.cpp**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.
#include "Environment/CamSimParticleManager.h"
#include "Config/CamSimConfig.h"
#include "Subsystem/CamSimSubsystem.h"
#include "CIGI/CigiPacketTypes.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "NiagaraFunctionLibrary.h"
#include "Components/DecalComponent.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"

FCamSimParticleManager::FCamSimParticleManager(UCamSimSubsystem* InSubsystem)
    : Subsystem(InSubsystem)
{}

void FCamSimParticleManager::Initialize(const FCamSimConfig& Config)
{
    ContrailAltM      = Config.Phase18.ContrailAltM;
    SmokeComponentID  = Config.Phase18.SmokeComponentID;
    FireComponentID   = Config.Phase18.FireComponentID;
    CraterComponentID = Config.Phase18.CraterImpactComponentID;
    MaxCraters        = Config.Phase18.MaxCraters;
    CraterRadiusM     = Config.Phase18.CraterDefaultRadiusM;

    auto LoadNiagara = [](const FString& Path) -> UNiagaraSystem*
    {
        UNiagaraSystem* A = LoadObject<UNiagaraSystem>(nullptr, *Path);
        if (!A)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("CamSimParticleManager: Niagara asset not found: '%s'. "
                     "Author it in the UE editor — see README Gotchas."), *Path);
        }
        return A;
    };
    RotorWashAsset = LoadNiagara(Config.Phase18.NiagaraRotorWash);
    SmokeAsset     = LoadNiagara(Config.Phase18.NiagaraSmoke);
    FireAsset      = LoadNiagara(Config.Phase18.NiagaraFire);
    ContrailAsset  = LoadNiagara(Config.Phase18.NiagaraContrail);

    CraterMaterial = LoadObject<UMaterialInterface>(nullptr, *Config.Phase18.CraterDecalMaterial);
    if (!CraterMaterial)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("CamSimParticleManager: Crater material not found: '%s'."),
            *Config.Phase18.CraterDecalMaterial);
    }
}

void FCamSimParticleManager::OnEntitySpawned(uint16 EntityID, AActor* Actor,
                                              const FCigiEntityState& State)
{
    if (!Actor) return;
    EntityParticles.FindOrAdd(EntityID);

    // 18F: Rotary-wing entities get rotor wash immediately on spawn
    if (State.EntityKind == KindPlatform && State.EntityDomain == DomainAir
        && State.EntityCategory == CatRotaryWing)
    {
        SpawnRotorWash(EntityID, Actor);
    }
}

void FCamSimParticleManager::OnEntityUpdated(uint16 EntityID, AActor* Actor,
                                              const FCigiEntityState& State)
{
    if (!Actor) return;

    // 18H: Fixed-wing contrails — altitude threshold only (speed not in FCigiEntityState)
    if (State.EntityKind == KindPlatform && State.EntityDomain == DomainAir
        && State.EntityCategory == CatFixedWing)
    {
        UpdateContrail(EntityID, Actor, State.Altitude);
    }
}

void FCamSimParticleManager::OnEntityRemoved(uint16 EntityID)
{
    if (FEntityParticleState* PS = EntityParticles.Find(EntityID))
    {
        DetachAll(*PS);
        EntityParticles.Remove(EntityID);
    }
}

void FCamSimParticleManager::OnComponentControl(uint16 EntityID, AActor* Actor,
                                                 const FCigiComponentControl& Pkt)
{
    if (!Actor) return;
    if (Pkt.CompId == static_cast<uint16>(SmokeComponentID))  { ActivateSmoke(EntityID, Actor); return; }
    if (Pkt.CompId == static_cast<uint16>(FireComponentID))   { ActivateFire(EntityID, Actor);  return; }
    if (Pkt.CompId == static_cast<uint16>(CraterComponentID)) { SpawnCraterDecal(Actor);         return; }
}

void FCamSimParticleManager::Tick(float DeltaTime)
{
    // Reserved for future per-frame parameter updates
}

// ─── Private ──────────────────────────────────────────────────────────────────

void FCamSimParticleManager::SpawnRotorWash(uint16 EntityID, AActor* Actor)
{
    if (!RotorWashAsset) return;
    FEntityParticleState& PS = EntityParticles.FindOrAdd(EntityID);
    if (PS.RotorWashComp) return;
    PS.RotorWashComp = UNiagaraFunctionLibrary::SpawnSystemAttached(
        RotorWashAsset, Actor->GetRootComponent(),
        NAME_None, FVector::ZeroVector, FRotator::ZeroRotator,
        EAttachLocation::KeepRelativeOffset, /*bAutoDestroy=*/false);
}

void FCamSimParticleManager::UpdateContrail(uint16 EntityID, AActor* Actor, float AltitudeM)
{
    FEntityParticleState& PS = EntityParticles.FindOrAdd(EntityID);
    const bool bShouldBeActive = (AltitudeM >= ContrailAltM);

    if (bShouldBeActive && !PS.bContrailActive)
    {
        if (!PS.ContrailComp && ContrailAsset)
        {
            PS.ContrailComp = UNiagaraFunctionLibrary::SpawnSystemAttached(
                ContrailAsset, Actor->GetRootComponent(),
                NAME_None, FVector::ZeroVector, FRotator::ZeroRotator,
                EAttachLocation::KeepRelativeOffset, /*bAutoDestroy=*/false);
        }
        if (PS.ContrailComp) { PS.ContrailComp->Activate(true); }
        PS.bContrailActive = true;
    }
    else if (!bShouldBeActive && PS.bContrailActive)
    {
        if (PS.ContrailComp) { PS.ContrailComp->Deactivate(); }
        PS.bContrailActive = false;
    }
}

void FCamSimParticleManager::ActivateSmoke(uint16 EntityID, AActor* Actor)
{
    if (!SmokeAsset) return;
    FEntityParticleState& PS = EntityParticles.FindOrAdd(EntityID);
    if (PS.bSmokeActive) return;
    if (!PS.SmokeComp)
    {
        PS.SmokeComp = UNiagaraFunctionLibrary::SpawnSystemAttached(
            SmokeAsset, Actor->GetRootComponent(),
            NAME_None, FVector::ZeroVector, FRotator::ZeroRotator,
            EAttachLocation::KeepRelativeOffset, /*bAutoDestroy=*/false);
    }
    if (PS.SmokeComp) { PS.SmokeComp->Activate(true); }
    PS.bSmokeActive = true;
}

void FCamSimParticleManager::ActivateFire(uint16 EntityID, AActor* Actor)
{
    if (!FireAsset) return;
    FEntityParticleState& PS = EntityParticles.FindOrAdd(EntityID);
    if (PS.bFireActive) return;
    if (!PS.FireComp)
    {
        PS.FireComp = UNiagaraFunctionLibrary::SpawnSystemAttached(
            FireAsset, Actor->GetRootComponent(),
            NAME_None, FVector::ZeroVector, FRotator::ZeroRotator,
            EAttachLocation::KeepRelativeOffset, /*bAutoDestroy=*/false);
    }
    if (PS.FireComp) { PS.FireComp->Activate(true); }
    PS.bFireActive = true;
}

void FCamSimParticleManager::SpawnCraterDecal(AActor* Actor)
{
    if (!CraterMaterial || !Actor || !Actor->GetWorld()) return;

    const FVector Start = Actor->GetActorLocation();
    const FVector End   = Start - FVector(0.0f, 0.0f, 100000.0f);   // 1 km down
    FHitResult Hit;
    if (!Actor->GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility)) return;

    const float RadiusCm = CraterRadiusM * 100.0f;
    UDecalComponent* Decal = UGameplayStatics::SpawnDecalAtLocation(
        Actor->GetWorld(), CraterMaterial,
        FVector(RadiusCm * 2.0f, RadiusCm * 2.0f, RadiusCm),
        Hit.Location, Hit.Normal.Rotation(), /*LifeSpan=*/0.0f);
    if (!Decal) return;
    Decal->SetFadeScreenSize(0.0001f);

    if (ActiveCraters.Num() >= MaxCraters)
    {
        if (UDecalComponent* Oldest = ActiveCraters[0]) { Oldest->DestroyComponent(); }
        ActiveCraters.RemoveAt(0);
    }
    ActiveCraters.Add(Decal);
}

void FCamSimParticleManager::DetachAll(FEntityParticleState& PS)
{
    auto Destroy = [](UNiagaraComponent*& Comp)
    {
        if (Comp) { Comp->DeactivateImmediate(); Comp->DestroyComponent(); Comp = nullptr; }
    };
    Destroy(PS.RotorWashComp);
    Destroy(PS.SmokeComp);
    Destroy(PS.FireComp);
    Destroy(PS.ContrailComp);
    PS.bSmokeActive    = false;
    PS.bFireActive     = false;
    PS.bContrailActive = false;
}
```

- [ ] **Step 4: Run tests — expect PASS** (`ParticleMap_EntityRemoved`, `Crater_RingBuffer`)

- [ ] **Step 5: Commit**

```bash
git add Source/CamSimTest/Environment/CamSimParticleManager.cpp \
        Source/CamSimTest/Tests/Phase18WeatherTest.cpp
git commit -m "feat: Phase 18F/18G/18H/18I — FCamSimParticleManager implementation"
```

---

## Chunk 4: Subsystem Integration and Finalization

### Task 8: Wire FCamSimParticleManager into FSubsystemImpl

**Files:**
- Modify: `Source/CamSimTest/Subsystem/CamSimSubsystem.h`
- Modify: `Source/CamSimTest/Subsystem/CamSimSubsystem.cpp`
- Modify: `Source/CamSimTest/Entity/CamSimEntityManager.cpp`

- [ ] **Step 1: Add to FSubsystemImpl in CamSimSubsystem.cpp**

Add include at top of `CamSimSubsystem.cpp`:

```cpp
#include "Environment/CamSimParticleManager.h"
```

In `struct UCamSimSubsystem::FSubsystemImpl`, after `GroundTruthCollector`:

```cpp
    TUniquePtr<FCamSimParticleManager> ParticleManager;
```

In `~FSubsystemImpl()`, before `EntityManager.Reset()`:

```cpp
    ParticleManager.Reset();
```

- [ ] **Step 2: Instantiate in Initialize**

After `Impl->GroundTruthCollector` is set up, add:

```cpp
    Impl->ParticleManager = MakeUnique<FCamSimParticleManager>(this);
    Impl->ParticleManager->Initialize(Config);
```

- [ ] **Step 3: Add getter**

In `CamSimSubsystem.h`, public accessors:

```cpp
    FCamSimParticleManager* GetParticleManager() const;
```

In `CamSimSubsystem.cpp`:

```cpp
FCamSimParticleManager* UCamSimSubsystem::GetParticleManager() const
{
    return Impl ? Impl->ParticleManager.Get() : nullptr;
}
```

- [ ] **Step 4: Wire entity spawn/update/remove in CamSimEntityManager.cpp**

Add at top:

```cpp
#include "Environment/CamSimParticleManager.h"
```

In `SpawnEntity()`, after `Entity->ApplyPose(S)`:

```cpp
    if (FCamSimParticleManager* PM = Subsystem->GetParticleManager())
    {
        PM->OnEntitySpawned(S.EntityId, Entity, S);
    }
```

In `ApplyEntityState()`, in the Active-state update path where `Entity->ApplyPose(S)` is called:

```cpp
    if (FCamSimParticleManager* PM = Subsystem->GetParticleManager())
    {
        PM->OnEntityUpdated(S.EntityId, Entity, S);
    }
```

In the Remove path:

```cpp
    if (FCamSimParticleManager* PM = Subsystem->GetParticleManager())
    {
        PM->OnEntityRemoved(S.EntityId);
    }
```

- [ ] **Step 5: Wire Component Control in ProcessComponentControls()**

The existing loop uses a pointer-pattern lookup: `ACamSimEntity** EntityPtr = EntityMap.Find(Comp.EntityId)`. After the existing `(*EntityPtr)->ApplyComponentControl(Comp)` call, add:

```cpp
    if (FCamSimParticleManager* PM = Subsystem->GetParticleManager())
    {
        // EntityPtr is already in scope from the existing Find() above
        PM->OnComponentControl(Comp.EntityId,
            EntityPtr ? static_cast<AActor*>(*EntityPtr) : nullptr,
            Comp);
    }
```

- [ ] **Step 6: Commit**

```bash
git add Source/CamSimTest/Subsystem/CamSimSubsystem.h \
        Source/CamSimTest/Subsystem/CamSimSubsystem.cpp \
        Source/CamSimTest/Entity/CamSimEntityManager.cpp
git commit -m "feat: Phase 18 — wire FCamSimParticleManager into subsystem and entity manager"
```

---

### Task 9: Update Plan.md

**Files:**
- Modify: `Plan.md`

- [ ] **Step 1: Mark Phase 18 items done**

Update the Phase 18 table rows and the summary table at the top to show all Phase 18 items complete.

- [ ] **Step 2: Commit**

```bash
git add Plan.md
git commit -m "docs: mark Phase 18 sprint 2 complete in Plan.md"
```

---

## Validation Checklist

After all tasks complete:

- [ ] All 15 `CamSim.Phase18.Cloud*/WeatherZone*/Particle*/Crater*` tests pass
- [ ] No regressions in prior tests (29 Phase 13 + 5 Phase 15 + 19 Phase 16 + 7 Phase 17 + 6 Phase 18 Sprint 1)
- [ ] UE project builds without errors (`scripts/run.sh --build`)
- [ ] With `volumetric_clouds: true` and coverage CIGI packet, `CloudActor` component visibility toggles
- [ ] With `weather_zones: true`, CIGI RegionId=1 packet updates zone fog parameters
- [ ] Spawning a rotary-wing entity (CIGI kind=1/domain=1/category=3) attaches rotor wash if asset exists
- [ ] CIGI Component Control CompId=10 triggers `SpawnCraterDecal` without crash (asset optional)
- [ ] `generated.h` remains the last include in all modified header files
