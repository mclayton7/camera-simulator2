# Phase 18 (Sprint 2) — Weather, Atmosphere & Particle Effects Design

**Date:** 2026-03-10
**Scope:** All remaining Phase 18 items: 18A, 18B, 18F, 18G, 18H, 18I, 18L

---

## Decisions

| Item | Decision |
|---|---|
| 18A Volumetric Clouds | UE5 built-in `UVolumetricCloudComponent` (not custom ray-march) |
| 18B Cloud Shadows | `UDirectionalLightComponent::bCastCloudShadows` — no extra actors |
| 18F/18G/18H Particles | Option A: C++ spawns `UNiagaraComponent`; Niagara `.uasset` files authored in editor |
| 18I Cratering | Decal-based (not mesh deformation) — Cesium-compatible |
| 18L Weather Zones | Distance-based blending (not boundary volumes) |

---

## Architecture

```
FSubsystemImpl
├── ACamSimEnvironment   ← extended: 18A clouds, 18B shadows, 18L zones
└── FCamSimParticleManager  ← new: 18F/G/H Niagara, 18I craters
```

### Data Flow Additions

```
CIGI opcode 12 → FCigiReceiver → FWeatherZoneUpdate queue → ACamSimEnvironment::Tick
CIGI opcode 13 → FCigiReceiver → FWeatherZoneUpdate queue → ACamSimEnvironment::Tick
CIGI opcode 4 (Component Control) → FCigiReceiver → FCamSimParticleManager
Entity spawn/update/remove → ACamSimEntityManager → FCamSimParticleManager
```

---

## 18A — Volumetric Clouds

- `UVolumetricCloudComponent` spawned on `ACamSimEnvironment` at `BeginPlay`
- Default material: UE5 built-in `M_VolumetricCloud`
- New method: `ApplyVolumetricClouds(const FCigiWeatherControlV3&)`

**CIGI → UE5 mapping:**

| CIGI field | UE5 property |
|---|---|
| `Coverage` (0–1) | material `CloudCoverage` scalar + `LayerBottomAltitude` offset |
| `BaseAlt` | `LayerBottomAltitude` (km) |
| `Thickness` | `LayerHeight` (km) |
| `WeatherType` | cloud material density scalar preset |

**New config fields:**
```yaml
phase18:
  volumetric_clouds_enabled: false
  cloud_coverage: 0.5
  cloud_altitude_km: 2.0
  cloud_thickness_km: 3.0
  cloud_shadow_strength: 0.6
```
Env vars: `CAMSIM_CLOUD_COVERAGE`, `CAMSIM_CLOUD_ALTITUDE_KM`, `CAMSIM_CLOUD_THICKNESS_KM`, `CAMSIM_CLOUD_SHADOW_STRENGTH`.

---

## 18B — Dynamic Cloud Shadows

- `SunLight->bCastCloudShadows = true` + `CloudShadowStrength` set when `CloudCoverage > 0.1f`
- Disabled automatically on clear weather CIGI updates
- No additional actors or render passes — UE5 projects shadows onto terrain natively

---

## 18L — Regional Weather Zones

**Structures:**
```cpp
struct FWeatherZoneParams {
    float FogDensity;
    float PrecipRate;
    float VisibilityM;
    float CloudCoverage;
};

struct FWeatherZone {
    int32  ZoneID;
    double LatDeg, LonDeg;
    float  RadiusM;
    FWeatherZoneParams Params;
};
```

**Blending logic** (`ACamSimEnvironment::Tick`):
1. Compute camera lat/lon from `FCamSimTelemetry`
2. Find nearest active zone within its radius
3. Alpha = `1 - (dist / radius)`, clamped 0–1
4. Lerp global fog/precip/cloud params toward zone params using alpha
5. If no zone in range, lerp back to global baseline

**New methods:**
- `ApplyWeatherZoneUpdate(const FWeatherZoneUpdate&)` — add/modify/remove from `TArray<FWeatherZone>`
- `BlendWeatherZones(const FVector2D& CameraLatLon)` — called from `Tick`

**Limit:** 16 simultaneous zones (CIGI spec limit). O(N) scan per tick.

---

## 18F/18G/18H — Particle Manager

```cpp
struct FEntityParticleState {
    UNiagaraComponent* RotorWashComp = nullptr;
    UNiagaraComponent* SmokeComp     = nullptr;
    UNiagaraComponent* FireComp      = nullptr;
    UNiagaraComponent* ContrailComp  = nullptr;
    bool bSmokeActive = false;
    bool bFireActive  = false;
};

class FCamSimParticleManager {
    TMap<int32, FEntityParticleState> EntityParticles;
    UNiagaraSystem* RotorWashAsset;
    UNiagaraSystem* SmokeAsset;
    UNiagaraSystem* FireAsset;
    UNiagaraSystem* ContrailAsset;
public:
    void Initialize(const FCamSimConfig&);
    void OnEntitySpawned(int32 EntityID, AActor*, const FCigiEntityState&);
    void OnEntityUpdated(int32 EntityID, AActor*, const FCigiEntityState&);
    void OnEntityRemoved(int32 EntityID);
    void OnComponentControl(int32 EntityID, const FCigiCompCtrlV3&);
    void Tick(float DeltaTime);
};
```

**Trigger rules:**

| Effect | Trigger | Condition |
|---|---|---|
| Rotor wash (18F) | `OnEntitySpawned` | Entity kind = rotary wing |
| Contrail (18H) | `OnEntityUpdated` | Fixed wing + alt > `ContrailAltM` + speed > `ContrailSpeedMs` |
| Smoke (18G) | `OnComponentControl` | Component ID = `SmokeComponentID` (default 1) |
| Fire (18G) | `OnComponentControl` | Component ID = `FireComponentID` (default 2) |

**New config fields:**
```yaml
phase18:
  niagara_rotor_wash: "/Game/Effects/NS_RotorWash"
  niagara_smoke:      "/Game/Effects/NS_Smoke"
  niagara_fire:       "/Game/Effects/NS_Fire"
  niagara_contrail:   "/Game/Effects/NS_Contrail"
  contrail_alt_m:     8000.0
  contrail_speed_ms:  100.0
  smoke_component_id: 1
  fire_component_id:  2
```

---

## 18I — Dynamic Cratering

- Triggered by CIGI Component Control with `ComponentID = crater_impact_component_id` (default 10)
- Line trace down from entity actor position to find terrain surface hit point
- Spawn `UDecalComponent` at hit location using `M_Crater` decal material
- Decal size: `CompData[0]` (0–255) mapped to 1–20 m radius
- Ring buffer: max 32 craters; oldest removed when exceeded

**New config fields:**
```yaml
phase18:
  crater_decal_material: "/Game/Effects/M_Crater"
  max_craters: 32
  crater_impact_component_id: 10
```

---

## Testing

New file: `Tests/Phase18WeatherTest.cpp` (15 tests)

| Test | Verifies |
|---|---|
| `VolumetricCloud_EnabledByConfig` | Cloud component non-null when enabled |
| `VolumetricCloud_CigiWeatherControl_SetsCoverage` | Mock CIGI opcode 12 → cloud param updated |
| `VolumetricCloud_ClearWeather_DisablesShadow` | Coverage < 0.1 → `bCastCloudShadows = false` |
| `WeatherZone_AddZone_StoresEntry` | Add update → zone in active list |
| `WeatherZone_RemoveZone_ClearsEntry` | Remove update → zone gone |
| `WeatherZone_CameraInside_BlendsParams` | Camera at center → alpha=1, params = zone params |
| `WeatherZone_CameraOutside_UsesGlobal` | Camera outside all zones → global unchanged |
| `WeatherZone_MaxZones_CapAt16` | 17 zones added → 16 stored |
| `ParticleManager_RotorWash_SpawnedOnRotaryWing` | Rotary-wing spawn → RotorWashComp non-null |
| `ParticleManager_Contrail_AboveAltitude` | Fixed-wing above threshold → ContrailComp active |
| `ParticleManager_Contrail_BelowAltitude` | Fixed-wing below threshold → ContrailComp null |
| `ParticleManager_Smoke_OnComponentControl` | Component ID=1 → SmokeComp activated |
| `ParticleManager_EntityRemoved_CleansUp` | Entity remove → all components detached |
| `Crater_ComponentControl_SpawnsDecal` | Component ID=10 → ActiveCraters increments |
| `Crater_MaxCraters_RingBuffer` | 33 impacts → 32 decals, oldest removed |

---

## Editor Assets Required (README note)

The following Niagara systems and decal material must be authored in the UE editor:

| Asset path | Type | Required parameters |
|---|---|---|
| `/Game/Effects/NS_RotorWash` | `UNiagaraSystem` | `Velocity` (float), `Density` (float), `Radius` (float) |
| `/Game/Effects/NS_Smoke` | `UNiagaraSystem` | `EmitRate` (float), `Color` (LinearColor) |
| `/Game/Effects/NS_Fire` | `UNiagaraSystem` | `EmitRate` (float), `Scale` (float) |
| `/Game/Effects/NS_Contrail` | `UNiagaraSystem` | `Width` (float), `Opacity` (float) |
| `/Game/Effects/M_Crater` | `UMaterialInterface` (decal) | Normal map input, burn ring albedo, opacity mask |
