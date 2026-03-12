# Phase 19 Sprint 1 — Ocean Core Design Spec

**Date:** 2026-03-12
**Scope:** 19A–19D (ocean surface, vessel wakes, vessel motion, reflections)
**Status:** Approved

---

## Overview

Adds a 3D ocean surface with Gerstner wave simulation, vessel wake particle trails, sea-state-driven vessel pitch/roll/heave, and SSR + SkyLight capture reflections. Driven by YAML config at startup and CIGI opcode 11 (Wave Control) at runtime.

---

## Scope

| Item | Description | Effort |
|------|-------------|--------|
| **19A** 3D Ocean Surface | GPU Gerstner waves via Material WPO + Material Parameter Collection | L |
| **19B** Vessel Wakes | Niagara NS_VesselWake FX attached to sea-domain entities | M |
| **19C** Vessel Surface Motion | Pitch/roll/heave from IOceanSurface height samples at bow/stern/port/stbd | M |
| **19D** Ocean Reflections | SSR + SkyLight Reflection Capture fallback | M |

Out of scope: 19E (bathymetry), 19F (light point system), 19G (steerable light lobes).

---

## Architecture

### Module Structure

**New files:**
```
Ocean/
  IOceanSurface.h              — pure C++ abstract interface
  FGerstnerOceanSurface.h/.cpp — GPU Gerstner implementation
  FOceanManager.h/.cpp         — lifecycle owner; CIGI queue drain
```

**Modified files:**
```
Config/CamSimConfig.h/.cpp               — FPhase19Config struct, YAML parsing, env vars
CIGI/CigiPacketTypes.h                   — FCigiWaveState
CIGI/CigiReceiver.h/.cpp                 — opcode 11 (Wave Control) handler + queue
Environment/CamSimEnvironment.h/.cpp     — owns FOceanManager; drains FCigiWaveState queue
Environment/CamSimParticleManager.h/.cpp — NS_VesselWake FX type
Entity/CamSimEntity.h/.cpp               — ApplyVesselMotion(); vessel motion state
Entity/CamSimEntityManager.cpp           — sea-domain dispatch (Domain=3)
Subsystem/CamSimSubsystem.h/.cpp         — pass FPhase19Config to environment
Tests/Phase19OceanTest.cpp               — 12 unit tests (new)
deploy/camsim_config.yaml                — phase19: block
Plan.md                                  — status updates
```

### Data Flow

```
YAML phase19: block ──► FPhase19Config ──► FOceanManager::Init()
                                                │
CIGI opcode 11 ──► FCigiWaveState queue         │
ACamSimEnvironment::Tick() drains queue ────────►│
                                          FOceanManager::ApplyWaveState()
                                          FGerstnerOceanSurface::SetBeaufortState()
                                          → Material Parameter Collection update (GPU)

ACamSimEntityManager::Tick()
  for each entity where Domain == 3 (Sea):
    CamSimParticleManager: spawn/update NS_VesselWake Niagara FX
    ACamSimEntity::ApplyVesselMotion():
      IOceanSurface::GetSurfaceHeightAt(bow/stern/port/stbd)
      → compute pitch/roll/heave
      → ApplyPose() with rotation delta
```

---

## Components

### `IOceanSurface` Interface

```cpp
class IOceanSurface
{
public:
    virtual ~IOceanSurface() = default;

    virtual void SetBeaufortState(int32 Beaufort) = 0;
    virtual void SetWaveParams(float AmplitudeScale, float FrequencyScale, float Choppiness) = 0;
    virtual void Tick(float DeltaTime) = 0;

    // Returns ocean surface height (UE units, cm) at world XY.
    // Used by 19C vessel motion to sample bow/stern/port/starboard.
    virtual float GetSurfaceHeightAt(FVector2D WorldXY) const = 0;

    virtual void SetEnabled(bool bEnabled) = 0;
};
```

Designed for future backend swap: `FWaterPluginOceanSurface` would implement the same interface using `UWaterBodyComponent::GetWaterSurfaceInfoAtLocation()`.

### `FGerstnerOceanSurface`

Owns:
- `UStaticMeshComponent` — large flat plane (~200km × 200km)
- `UMaterialParameterCollectionInstance` — writes `Beaufort`, `Amplitude`, `Frequency`, `Choppiness`, `Time` each tick
- `UReflectionCaptureComponent` (sky capture) — recaptured on atmosphere change via `ACamSimEnvironment::OnAtmosphereChanged()`

`GetSurfaceHeightAt()` evaluates the Gerstner sum analytically in C++ using the same parameters as the Material, keeping vessel motion visually in sync.

### Beaufort → Wave Parameter Table

| Beaufort | Amplitude (m) | Frequency | Choppiness |
|----------|--------------|-----------|------------|
| 0        | 0.0          | 0.0       | 0.0        |
| 2        | 0.3          | 0.8       | 0.2        |
| 4        | 1.0          | 1.2       | 0.4        |
| 6        | 2.5          | 1.8       | 0.6        |
| 8        | 5.0          | 2.5       | 0.8        |
| 10       | 9.0          | 3.5       | 0.9        |
| 12       | 14.0         | 5.0       | 1.0        |

Intermediate states interpolated linearly.

### `FCigiWaveState`

```cpp
struct FCigiWaveState
{
    uint8  WaveID     = 0;
    bool   bEnabled   = false;
    int32  Beaufort   = 0;
    float  WaveHeight = 0.0f;  // m — overrides Beaufort table amplitude if > 0
    float  WaveFreq   = 0.0f;  // overrides Beaufort table frequency if > 0
};
```

`CigiReceiver` adds a `TSpscQueue<FCigiWaveState>` and handles `CigiWaveCtrlV3` (opcode 11). `ACamSimEnvironment::Tick()` drains it — identical pattern to Phase 18 weather drain.

### Vessel Motion (19C)

`ACamSimEntity::ApplyVesselMotion(IOceanSurface*)` samples four points:

```
Bow   = Location + Forward * HalfLength
Stern = Location - Forward * HalfLength
Port  = Location - Right   * HalfBeam
Stbd  = Location + Right   * HalfBeam

Pitch = atan2(Bow.Z  - Stern.Z, HalfLength * 2)
Roll  = atan2(Stbd.Z - Port.Z,  HalfBeam   * 2)
Heave = mean(Bow.Z, Stern.Z, Port.Z, Stbd.Z) - RestHeight
```

`HalfLength` / `HalfBeam` default to mesh bounding box extents; overridable per entity type in `EntityTypeTable` YAML. Non-sea-domain entities skip this method entirely.

### Reflections (19D)

- **SSR** — enabled via Post Process Volume settings in `FOceanManager::Init()`; intensity driven by `FPhase19Config::SSRIntensity`
- **SkyLight Reflection Capture** — owned by `FGerstnerOceanSurface`; radius from `FPhase19Config::ReflectionCaptureRadius`; recaptured when atmosphere changes

No planar reflection component — cost not justified for high-altitude ISR use case.

---

## Configuration

### `FPhase19Config` struct

```cpp
struct FPhase19Config {
    // 19A Ocean surface
    bool    bOceanEnabled        = false;
    int32   BeaufortState        = 0;
    float   WaveAmplitudeScale   = 1.0f;
    float   WaveFrequencyScale   = 1.0f;
    float   WaveChoppiness       = 0.5f;
    FString OceanMaterialPath    = TEXT("/Game/Materials/M_Ocean");

    // 19B Vessel wakes
    bool    bVesselWakesEnabled  = false;
    FString NiagaraVesselWake    = TEXT("/Game/Effects/NS_VesselWake");
    float   WakeFadeTime         = 8.0f;

    // 19C Vessel surface motion
    bool    bVesselMotionEnabled = false;
    float   VesselMotionScale    = 1.0f;

    // 19D Reflections
    bool    bOceanReflectionsEnabled = false;
    float   SSRIntensity             = 1.0f;
    float   ReflectionCaptureRadius  = 10000.0f;
};
```

### YAML block (`deploy/camsim_config.yaml`)

```yaml
phase19:
  ocean_enabled: false
  beaufort_state: 0
  wave_amplitude_scale: 1.0
  wave_frequency_scale: 1.0
  wave_choppiness: 0.5
  ocean_material_path: "/Game/Materials/M_Ocean"
  vessel_wakes_enabled: false
  niagara_vessel_wake: "/Game/Effects/NS_VesselWake"
  wake_fade_time: 8.0
  vessel_motion_enabled: false
  vessel_motion_scale: 1.0
  ocean_reflections_enabled: false
  ssr_intensity: 1.0
  reflection_capture_radius: 10000.0
```

### Env var overrides (prefix `CAMSIM_OCEAN_*`)

| Env var | Field |
|---------|-------|
| `CAMSIM_OCEAN_ENABLED` | `bOceanEnabled` |
| `CAMSIM_OCEAN_BEAUFORT` | `BeaufortState` |
| `CAMSIM_OCEAN_CHOPPINESS` | `WaveChoppiness` |
| `CAMSIM_OCEAN_WAKES_ENABLED` | `bVesselWakesEnabled` |
| `CAMSIM_OCEAN_MOTION_ENABLED` | `bVesselMotionEnabled` |
| `CAMSIM_OCEAN_REFLECTIONS_ENABLED` | `bOceanReflectionsEnabled` |

---

## Testing

12 unit tests in `Tests/Phase19OceanTest.cpp`:

| # | Test |
|---|------|
| 1 | `FPhase19Config` default values correct |
| 2 | YAML `phase19:` block parses all fields |
| 3 | Env var `CAMSIM_OCEAN_BEAUFORT` overrides YAML |
| 4 | Beaufort 0 → amplitude = 0.0 |
| 5 | Beaufort 6 → correct amplitude/frequency/choppiness from table |
| 6 | Beaufort 12 → clamped to max table values |
| 7 | Beaufort 5 → linearly interpolated params |
| 8 | `FCigiWaveState` parsed correctly from opcode 11 packet |
| 9 | CIGI WaveHeight > 0 overrides Beaufort table amplitude |
| 10 | `GetSurfaceHeightAt()` returns 0.0 at Beaufort 0 |
| 11 | `GetSurfaceHeightAt()` returns non-zero at Beaufort 6 |
| 12 | Sea-domain entity (Domain=3) triggers vessel motion; land-domain (Domain=2) skips it |

---

## Validation Criteria

- Ocean surface visible from altitude with wave motion responding to Beaufort state
- CIGI opcode 11 packet changes Beaufort state at runtime
- Sea-domain entities exhibit pitch/roll/heave in rough seas
- Wake particle trails spawn behind moving vessels and fade after `WakeFadeTime`
- Ocean reflections show sky colour; SSR shows on-screen terrain reflections
- All 12 unit tests pass
- All features off by default (`bOceanEnabled: false`)
