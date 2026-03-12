# Phase 19 Sprint 1 — Ocean Core Design Spec

**Date:** 2026-03-12
**Scope:** 19A–19D (ocean surface, vessel wakes, vessel motion, reflections)
**Status:** Approved (v3 — post-review fixes)

---

## Overview

Adds a 3D ocean surface with Gerstner wave simulation, vessel wake particle trails, sea-state-driven vessel pitch/roll/heave, and SSR + SkyLight capture reflections. Driven by YAML config at startup and CIGI opcode 14 (Wave Control) at runtime.

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
CIGI/CigiReceiver.h/.cpp                 — opcode 14 (Wave Control) handler + queue + DequeueWaveState()
Environment/CamSimEnvironment.h/.cpp     — owns FOceanManager; drains FCigiWaveState queue; OnAtmosphereChanged()
Environment/CamSimParticleManager.h/.cpp — NS_VesselWake FX type (wake spawn/update/remove)
Entity/CamSimEntity.h/.cpp               — ApplyVesselMotion(); vessel motion state
Entity/CamSimEntityManager.h/.cpp        — sea-domain dispatch; SetOceanSurface() injection point
Entity/EntityTypeTable.h/.cpp            — vessel geometry additions (HalfLengthCm, HalfBeamCm); no EntityDomain change needed
Subsystem/CamSimSubsystem.h/.cpp         — pass FPhase19Config to environment; wire IOceanSurface* to entity manager
Tests/Phase19OceanTest.cpp               — 15 unit tests (new)
deploy/camsim_config.yaml                — phase19: block
Plan.md                                  — status updates
```

### Data Flow

```
YAML phase19: block ──► FPhase19Config ──► ACamSimEnvironment::BeginPlay()
                                                │
                                         FOceanManager::Init(World, OuterActor, Cfg)
                                         FGerstnerOceanSurface constructed with UObject components
                                         registered to ACamSimEnvironment (GC-safe)
                                                │
                                         IOceanSurface* injected into FCamSimEntityManager
                                         via FCamSimEntityManager::SetOceanSurface()

CIGI opcode 14 (Wave Control) ──► FCigiWaveState queue (TSpscQueue in FCigiReceiver)
ACamSimEnvironment::Tick():
  DequeueWaveState() ──► FOceanManager::ApplyWaveState()
                          FGerstnerOceanSurface::SetWaveParams() ──► MPC write (GPU)

ACamSimEntityManager::Tick():
  for each entity where FCigiEntityState::EntityDomain == 3 (Sea):
    CamSimParticleManager: spawn/update/remove NS_VesselWake Niagara FX
    ACamSimEntity::ApplyVesselMotion(OceanSurface):
      IOceanSurface::GetSurfaceHeightAt(bow/stern/port/stbd) [UE units, cm]
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

    // WaveHtM and WaveLenM in metres — from Beaufort table (startup) or CIGI packet (runtime).
    // AmplitudeScale and FrequencyScale are YAML multipliers applied on top.
    // Wave direction is not parameterised in Sprint 1; all waves use a fixed propagation
    // direction baked into the Material. The interface can add a Direction parameter later.
    virtual void SetWaveParams(float WaveHtM, float WaveLenM,
                               float AmplitudeScale, float FrequencyScale,
                               float Choppiness) = 0;

    virtual void Tick(float DeltaTime) = 0;

    // Returns ocean surface height at world XY in UE units (cm).
    // Used by 19C vessel motion to sample bow/stern/port/starboard.
    virtual float GetSurfaceHeightAt(FVector2D WorldXY) const = 0;

    virtual void SetEnabled(bool bEnabled) = 0;
};
```

Designed for future backend swap: `FWaterPluginOceanSurface` would implement the same interface using `UWaterBodyComponent::GetWaterSurfaceInfoAtLocation()`.

### `FGerstnerOceanSurface`

Plain C++ class. UObject components are created via `NewObject<>` with `ACamSimEnvironment` as outer — making them GC-safe and properly tracked by UE. `FGerstnerOceanSurface` holds raw `TObjectPtr<>` references only; it does not own them in the GC sense.

Components owned by (registered to) `ACamSimEnvironment`:
- `UStaticMeshComponent` — large flat plane; repositioned to camera XY each tick to cover visible ocean area
- `UMaterialParameterCollectionInstance` — writes `Amplitude`, `Frequency`, `Choppiness`, `Time` each tick
- `USkyLightComponent` with `bRealTimeCaptureEnabled = false` — manual `RecaptureSky()` call on atmosphere change

**Camera-relative repositioning:** Each `Tick()`, the ocean plane's XY world position is snapped to the camera's world XY (Z held at sea level). This ensures the plane always covers the visible area regardless of how far the camera travels. The plane size (200km × 200km, ~16 subdivisions) is chosen to cover max sensor FOV at min altitude with headroom; tessellation is left to the Material's WPO displacement.

**`GetSurfaceHeightAt()`** evaluates the Gerstner sum analytically in C++ using the same parameters as the Material, keeping vessel motion visually in sync. Returns height in UE units (cm).

**Atmosphere change hook:** `ACamSimEnvironment` gains `OnAtmosphereChanged()` — called from `ApplyCelestial()` and `ApplyWeather()` — which calls `FGerstnerOceanSurface::RecaptureSky()` to refresh the SkyLight.

### Beaufort → Wave Parameter Table

Used when CIGI Wave Control is absent or when `Beaufort` is set from YAML. Maps to physical wave height and length:

| Beaufort | WaveHt (m) | WaveLen (m) | Choppiness |
|----------|-----------|------------|------------|
| 0        | 0.0       | 0.0        | 0.0        |
| 2        | 0.3       | 8.0        | 0.2        |
| 4        | 1.0       | 30.0       | 0.4        |
| 6        | 2.5       | 70.0       | 0.6        |
| 8        | 5.0       | 140.0      | 0.8        |
| 10       | 9.0       | 250.0      | 0.9        |
| 12       | 14.0      | 400.0      | 1.0        |

Intermediate states interpolated linearly. `WaveFreq` used by the Gerstner CPU analytic eval is derived as `1.0f / WaveLenM` (spatial frequency).

### `FCigiWaveState`

Populated directly from CCL `CigiWaveCtrlV3` fields (opcode **14**, not 11):

```cpp
struct FCigiWaveState
{
    uint8 WaveID    = 0;
    bool  bEnabled  = false;
    float WaveHtM   = 0.0f;  // from CigiBaseWaveCtrl::GetWaveHt()  — metres
    float WaveLenM  = 0.0f;  // from CigiBaseWaveCtrl::GetWaveLen()  — metres
    float PeriodS   = 0.0f;  // from CigiBaseWaveCtrl::GetPeriod()   — seconds (informational)
};
```

No `Beaufort` field — CIGI carries raw physical parameters. `FOceanManager::ApplyWaveState()` passes `WaveHtM` and `WaveLenM` directly to `IOceanSurface::SetWaveParams()`. Beaufort state is a YAML/config concept only.

`CigiReceiver` adds:
- `TSpscQueue<FCigiWaveState> WaveStateQueue`
- `class FWaveCtrlProcessor : public CigiBaseEventProcessor` (friend of `FCigiReceiver`)
- Public `bool DequeueWaveState(FCigiWaveState& Out)` accessor

`ACamSimEnvironment::Tick()` drains via `Receiver->DequeueWaveState()` — identical pattern to Phase 18 weather drain.

Wave Control uses the standard CCL event processor path (not the raw-parse bypass used for Celestial/Atmosphere/Weather, which was needed only because of `CigiHoldEnvCtrl` merging behaviour that does not apply to opcode 14).

### Vessel Motion (19C)

**Sea-domain detection:** `FCigiEntityState` already carries `EntityDomain` (populated from the CIGI EntityControl packet by the existing CCL parser — confirmed present at `CigiPacketTypes.h:30`). `ACamSimEntityManager` checks `State.EntityDomain == 3` to identify sea-domain entities. This is consistent with how Phase 18's `FCamSimParticleManager` already uses `State.EntityDomain == 1` for air entities. No new field is needed in `FEntityTypeEntry` for domain.

**`FEntityTypeEntry` additions** (vessel geometry only):
```cpp
float  HalfLengthCm  = 0.0f;  // YAML: half_length_m * 100 (0 = use mesh bounds)
float  HalfBeamCm    = 0.0f;  // YAML: half_beam_m   * 100 (0 = use mesh bounds)
```

If `HalfLengthCm == 0`, `ACamSimEntity::ApplyVesselMotion()` falls back to `UStaticMeshComponent::GetStaticMesh()->GetBounds().BoxExtent.X` (game thread only, after mesh load completes). Until the mesh is loaded, vessel motion is skipped for that entity.

**`ACamSimEntity::ApplyVesselMotion(IOceanSurface* Ocean)`:**

All values in UE units (cm) throughout:

```
HalfLen = HalfLengthCm  (from FEntityTypeEntry, or mesh bounds fallback)
HalfBm  = HalfBeamCm

Bow   = Location + Forward * HalfLen   (UE units, cm)
Stern = Location - Forward * HalfLen
Port  = Location - Right   * HalfBm
Stbd  = Location + Right   * HalfBm

hBow   = Ocean->GetSurfaceHeightAt(FVector2D(Bow.X,   Bow.Y))   // cm
hStern = Ocean->GetSurfaceHeightAt(FVector2D(Stern.X, Stern.Y)) // cm
hPort  = Ocean->GetSurfaceHeightAt(FVector2D(Port.X,  Port.Y))  // cm
hStbd  = Ocean->GetSurfaceHeightAt(FVector2D(Stbd.X,  Stbd.Y))  // cm

Pitch  = FMath::Atan2(hBow - hStern, HalfLen * 2.0f)   // radians, units match (cm/cm)
Roll   = FMath::Atan2(hStbd - hPort, HalfBm  * 2.0f)
Heave  = (hBow + hStern + hPort + hStbd) * 0.25f - RestHeightCm

Apply as rotation/translation delta on top of CIGI-commanded pose via ApplyPose().
```

`VesselMotionScale` from config multiplies Pitch, Roll, and Heave before application. Non-sea-domain entities skip `ApplyVesselMotion()` entirely.

**`IOceanSurface*` injection into `FCamSimEntityManager`:**
`ACamSimEnvironment::BeginPlay()` calls `FOceanManager::Init()` to construct `FGerstnerOceanSurface`. It then injects the ocean surface pointer via:
```cpp
Subsystem->GetEntityManager()->SetOceanSurface(OceanManager->GetOceanSurface());
```
`FCamSimEntityManager` gains a new `IOceanSurface* OceanSurface = nullptr` member and `SetOceanSurface(IOceanSurface*)` method. It passes the pointer per-call to `ACamSimEntity::ApplyVesselMotion(OceanSurface)`. If `OceanSurface == nullptr` (ocean disabled), `ApplyVesselMotion()` is not called.

**Startup wave params:** `FOceanManager::Init()` translates `FPhase19Config::BeaufortState` through the Beaufort table and calls `OceanSurface->SetWaveParams(...)` directly. This establishes initial wave state before any CIGI packet arrives. Asset paths (`OceanMaterialPath`, `NiagaraVesselWake`) are editor-baked references and are not env-overridable.

### Reflections (19D)

- **SSR** — enabled by finding `APostProcessVolume` via `TActorIterator<APostProcessVolume>` in `FOceanManager::Init(UWorld*)`, then setting `PostProcessVolume->Settings.ScreenSpaceReflectionIntensity = Cfg.SSRIntensity` and `bOverride_ScreenSpaceReflectionIntensity = true`. Same pattern as Phase 18's god ray Post Process access.
- **SkyLight Reflection Capture** — `USkyLightComponent` registered to `ACamSimEnvironment` with `SourceType = SLS_CapturedScene`, `bRealTimeCaptureEnabled = false`. `RecaptureSky()` called manually in `OnAtmosphereChanged()`.

No planar reflection component — cost not justified for high-altitude ISR.

---

## Configuration

### `FPhase19Config` struct

```cpp
struct FPhase19Config {
    // 19A Ocean surface
    bool    bOceanEnabled        = false;
    int32   BeaufortState        = 0;        // 0–12; used when no CIGI Wave Control
    float   WaveAmplitudeScale   = 1.0f;     // multiplier on top of Beaufort/CIGI params
    float   WaveFrequencyScale   = 1.0f;
    float   WaveChoppiness       = 0.5f;     // 0=sine, 1=sharp Gerstner peaks
    FString OceanMaterialPath    = TEXT("/Game/Materials/M_Ocean");

    // 19B Vessel wakes
    bool    bVesselWakesEnabled  = false;
    FString NiagaraVesselWake    = TEXT("/Game/Effects/NS_VesselWake");
    float   WakeFadeTime         = 8.0f;     // seconds before wake dissipates

    // 19C Vessel surface motion
    bool    bVesselMotionEnabled = false;
    float   VesselMotionScale    = 1.0f;     // amplitude multiplier

    // 19D Reflections
    bool    bOceanReflectionsEnabled = false;
    float   SSRIntensity             = 1.0f;
    float   ReflectionCaptureRadius  = 10000.0f;  // cm
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
| `CAMSIM_OCEAN_AMP_SCALE` | `WaveAmplitudeScale` |
| `CAMSIM_OCEAN_FREQ_SCALE` | `WaveFrequencyScale` |
| `CAMSIM_OCEAN_CHOPPINESS` | `WaveChoppiness` |
| `CAMSIM_OCEAN_WAKES_ENABLED` | `bVesselWakesEnabled` |
| `CAMSIM_OCEAN_WAKE_FADE` | `WakeFadeTime` |
| `CAMSIM_OCEAN_MOTION_ENABLED` | `bVesselMotionEnabled` |
| `CAMSIM_OCEAN_MOTION_SCALE` | `VesselMotionScale` |
| `CAMSIM_OCEAN_REFLECTIONS_ENABLED` | `bOceanReflectionsEnabled` |
| `CAMSIM_OCEAN_SSR_INTENSITY` | `SSRIntensity` |
| `CAMSIM_OCEAN_REFLECTION_RADIUS` | `ReflectionCaptureRadius` |

---

## Testing

15 unit tests in `Tests/Phase19OceanTest.cpp`:

| # | What's tested |
|---|------|
| 1 | `FPhase19Config` default values correct |
| 2 | YAML `phase19:` block parses all fields |
| 3 | Env var `CAMSIM_OCEAN_BEAUFORT` overrides YAML |
| 4 | Env var `CAMSIM_OCEAN_AMP_SCALE` overrides YAML |
| 5 | Beaufort 0 → WaveHt = 0.0, WaveLen = 0.0 from table |
| 6 | Beaufort 6 → correct WaveHt/WaveLen/Choppiness from table |
| 7 | Beaufort 12 → clamped to max table values |
| 8 | Beaufort 5 → linearly interpolated WaveHt/WaveLen |
| 9 | `FCigiWaveState` fields populated from opcode 14 packet (WaveHtM, WaveLenM, PeriodS) |
| 10 | CIGI WaveHtM > 0 passed through to `SetWaveParams()` directly (no Beaufort conversion) |
| 11 | `GetSurfaceHeightAt()` returns 0.0 at Beaufort 0 (WaveHt = 0) |
| 12 | `GetSurfaceHeightAt()` returns non-zero at Beaufort 6 |
| 13 | Entity with `FCigiEntityState::EntityDomain == 3` triggers `ApplyVesselMotion()`; `EntityDomain == 2` skips it |
| 14 | Wake FX spawned on sea-domain entity appearance; removed on entity remove |
| 15 | `ACamSimEnvironment::Tick()` drains `FCigiWaveState` queue and calls `FOceanManager::ApplyWaveState()` |

---

## Validation Criteria

- Ocean surface visible from altitude with wave motion responding to Beaufort state
- CIGI opcode 14 packet changes wave height/length at runtime
- Sea-domain entities (entity_domain: 3 in EntityTypeTable YAML) exhibit pitch/roll/heave in rough seas
- Wake particle trails spawn behind moving vessels and fade after `WakeFadeTime`
- Ocean reflections show sky colour; SSR shows on-screen terrain/entity reflections
- All 15 unit tests pass
- All features off by default (`ocean_enabled: false`)
