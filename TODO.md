# Unreal Editor Asset Checklist

C++ implementations are complete across all phases. The tasks below require work inside the
Unreal Editor before various features activate at runtime. All features degrade gracefully
(log warning + skip) unless noted as **hard requirement**.

---

## Part 1 — Level Actors (all phases)

These actors are discovered at runtime via `TActorIterator`. They do not need to exist for
the project to run, but each controls a significant visual or environmental system. Place them
once and they persist across sessions.

### 1A — Directional Light (sun)

`ACamSimEnvironment` searches for a `ADirectionalLight` to drive day/night rotation and god-ray
effects (Phase 18E). Without it, sun azimuth/elevation CIGI commands are silently ignored.

- [ ] Place → Lights → **Directional Light**
- [ ] In Details → set **Mobility** to Movable (required for runtime rotation)
- [ ] Enable **Cast Ray Traced Shadows** if ray tracing is active
- [ ] Enable **Light Shaft Bloom** and **Light Shaft Occlusion** (required for Phase 18E god rays)

### 1B — Sky Light

`ACamSimEnvironment` searches for `ASkyLight` to provide ambient/reflection lighting.
Without it the scene has no sky-based ambient — looks flat in shadow.

- [ ] Place → Lights → **Sky Light**
- [ ] Set **Mobility** to Movable
- [ ] Enable **Real Time Capture** (auto-updates sky cube each frame)

### 1C — Sky Atmosphere

`ACamSimEnvironment::ApplySkyAtmosphericScattering()` (Phase 18J) sets Rayleigh and Mie
scattering scales on `USkyAtmosphereComponent`. Without this actor Phase 18J is skipped.

- [ ] Place → Visual Effects → **Sky Atmosphere**
- [ ] Leave default parameters — C++ overrides `RayleighScatteringScale` and `MieScatteringScale`
  at runtime based on weather config

### 1D — Volumetric Cloud

`ACamSimEnvironment` searches for `AVolumetricCloud` to enable cloud shadows (Phase 18B).
Without it cloud shadow effects are skipped.

- [ ] Place → Visual Effects → **Volumetric Cloud**
- [ ] Set **Layer Bottom Altitude** and **Layer Height** to taste (default 2 km / 8 km works)

### 1E — Exponential Height Fog

`ACamSimEnvironment` searches for `AExponentialHeightFog` to apply second fog layer (Phase 18C)
and dynamic IR extinction (Phase 18K). Without it those effects are skipped.

- [ ] Place → Visual Effects → **Exponential Height Fog**
- [ ] Set **Mobility** to Movable
- [ ] Leave base fog density low — C++ overrides both primary and secondary fog layers at runtime

### 1F — Post Process Volume

`FOceanManager::EnableSSR()` (Phase 19) finds the first `APostProcessVolume` in the level to
set `ScreenSpaceReflectionIntensity`. Without it ocean SSR is silently skipped.

- [ ] Place → Volumes → **Post Process Volume**
- [ ] In Details, enable **Infinite Extent (Unbound)**
- [ ] Leave all overrides disabled — C++ writes only the properties it needs

### 1G — Cesium Sun Sky (optional, preferred over manual sun)

`ACamSimEnvironment` prefers `ACesiumSunSky` over a plain `ADirectionalLight` when both exist.
If present, CesiumSunSky drives geospatially accurate sun position automatically.

- [ ] Place → Cesium → **Cesium Sun Sky** (from Cesium plugin)
- [ ] Configure latitude/longitude origin to match your primary operating area
- [ ] If using CesiumSunSky, the plain Directional Light in 1A is still needed as a fallback
  for non-Cesium builds

### 1H — Cesium 3D Tileset (terrain)

`ACamSimCamera` tunes all `ACesium3DTileset` actors found in the level for performance.
At least one must exist for terrain to appear. **Hard requirement for terrain-based simulation.**

- [ ] Place → Cesium → **Cesium 3D Tileset**
- [ ] Set the Cesium ion asset ID (e.g. asset `1` = Cesium World Terrain)
- [ ] Add a **Bing Maps Aerial** imagery layer overlay (Cesium ion asset `2`)
- [ ] Enable **Create Physics Mesh** if HAT/HOT terrain queries are needed (CIGI terrain feedback)

---

## Part 2 — Phase 18 Particle Effects & Decals

These assets are loaded by path string in `FCamSimParticleManager`. Missing assets log a warning
and the effect is silently skipped — no crash. All paths are overridable in `camsim_config.yaml`.

### 2A — Rotor Wash `/Game/Effects/NS_RotorWash`

Spawned on air-domain entities (CIGI `EntityDomain == 1`) with rotary-wing entity types.

- [ ] Create folder `Content/Effects/` if absent
- [ ] Right-click → FX → **Niagara System** → pick a **GPU Sprites** or cone spray template
- [ ] Name it `NS_RotorWash`
- [ ] Configure: downward velocity, dust/debris sprites, lifetime ~2–4 s, radius ~200–500 cm
- [ ] Save

### 2B — Smoke `/Game/Effects/NS_Smoke`

Spawned on damaged/destroyed entities (CIGI entity state changes).

- [ ] Right-click → FX → **Niagara System** → pick a smoke/volumetric template
- [ ] Name it `NS_Smoke`
- [ ] Configure: rising velocity, grey–black colour gradient, lifetime ~5–10 s, billowing size
- [ ] Save

### 2C — Fire `/Game/Effects/NS_Fire`

Spawned alongside smoke on destroyed entities.

- [ ] Right-click → FX → **Niagara System** → pick a fire/flame template
- [ ] Name it `NS_Fire`
- [ ] Configure: orange–yellow, upward velocity, lifetime ~3–6 s, emissive intensity
- [ ] Save

### 2D — Contrails `/Game/Effects/NS_Contrail`

Spawned on air-domain fixed-wing entities at altitude (Phase 18H).

- [ ] Right-click → FX → **Niagara System** → pick a **Ribbon** template
- [ ] Name it `NS_Contrail`
- [ ] Configure:
  - Emitter type: Ribbon (continuous trail, not burst)
  - Ribbon width: ~50–150 cm (expands over lifetime)
  - Color: white, alpha fades to 0 over ~20–60 s
  - Velocity: inherit from parent with minimal spread
- [ ] Save

### 2E — Crater Decal `/Game/Effects/M_Crater`

Applied as a `UDecalComponent` on terrain when Phase 18I (cratering) is triggered via CIGI.

- [ ] Right-click → Material → **Material**
- [ ] Name it `M_Crater`
- [ ] In Material Editor:
  - **Material Domain**: Deferred Decal
  - **Blend Mode**: Translucent
  - **Decal Blend Mode**: DBuffer Translucent Color, Normal, Roughness
- [ ] Author a scorched-earth look: dark charred center, debris ring, cracked soil normal map
- [ ] Save

---

## Part 3 — Phase 19 Ocean

### 3A — Material Parameter Collection `/Game/Materials/MPC_Ocean`

`FGerstnerOceanSurface::WriteMPC()` calls `UKismetMaterialLibrary::SetScalarParameterValue()`
with these 4 names every frame. If the MPC doesn't exist, the ocean silently disables.

- [ ] Navigate to `Content/Materials/` (create folder if absent)
- [ ] Right-click → Material → **Material Parameter Collection**
- [ ] Name it `MPC_Ocean`
- [ ] Add **4 Scalar Parameters** (all default value `0.0`):
  - `Amplitude` — wave height in UE units (cm); range ~0–2000 cm depending on Beaufort
  - `Frequency` — wave number k = 2π/wavelength; range ~0.002–0.1 rad/cm
  - `Choppiness` — Gerstner peak sharpness; range 0.0 (sine) to 1.0 (sharp peaks)
  - `Time` — elapsed seconds; incremented each frame by C++
- [ ] Save

### 3B — Ocean Material `/Game/Materials/M_Ocean`

`FOceanManager::Init()` loads `OceanMaterialPath` (default `/Game/Materials/M_Ocean`) and
applies it to the 200 km × 200 km ocean plane mesh.

- [ ] In `Content/Materials/`, right-click → **Material** → name it `M_Ocean`
- [ ] Open the Material Editor. Set:
  - **Blend Mode**: Translucent (or Opaque for performance)
  - **Two Sided**: enabled (prevents back-face clipping on waves)
- [ ] Build the Material graph:
  - Add a **Material Parameter Collection** node referencing `MPC_Ocean`
  - Break out all 4 scalar params: `Amplitude`, `Frequency`, `Choppiness`, `Time`
  - Compute angular frequency: `AngularFreq = sqrt(981.0 × Frequency)` (g = 981 cm/s²)
  - Compute phase: `Phase = Frequency × WorldPosition.X − AngularFreq × Time`
  - World Position Offset Z: `Amplitude × cos(Phase)`
  - Gerstner horizontal shift: `WPO.X += Choppiness × Amplitude × sin(Phase)`
  - Connect to **World Position Offset** pin
- [ ] Aesthetics (minimum viable):
  - **Base Color**: deep blue-green (e.g. `0.01, 0.05, 0.12`)
  - **Normal**: tiling normal map — UE built-in `T_Water_N` works; scale UV to taste
  - **Roughness**: `0.05`, **Specular**: `0.5`, **Metallic**: `0.0`
- [ ] Save

### 3C — Niagara Vessel Wake `/Game/Effects/NS_VesselWake`

`FCamSimParticleManager` spawns this on every sea-domain entity (CIGI `EntityDomain == 3`)
and calls `WakeComp->SetFloatParameter(FName("FadeTime"), ...)`. The asset **must** expose
`FadeTime` as a User Parameter or the fade duration won't apply.

- [ ] In `Content/Effects/`, right-click → FX → **Niagara System** → Ribbon template
- [ ] Name it `NS_VesselWake`
- [ ] Configure emitter:
  - **Emitter type**: Ribbon
  - **Spawn rate**: ~50–100 particles/sec
  - **Color**: white/light-blue foam, alpha fades to 0 over lifetime
  - **Size**: start ~50–200 cm width, shrink over lifetime
  - **Velocity**: inherit from attachment with slight spreading
- [ ] Expose `FadeTime` as a **User Exposed Float Parameter**:
  - System Parameters panel → add float parameter named exactly `FadeTime`
  - Wire into Particle Lifetime binding
  - Default value: `8.0`
- [ ] Save

---

## Part 4 — Entity Meshes

`FCamSimEntityManager` loads meshes referenced in the `entity_types:` block of
`deploy/camsim_config.yaml`. Missing meshes log a warning and the entity renders invisible
(actor exists but has no geometry). Paths can be UE content paths (`/Game/...`) or `.gltf`/`.glb`
file paths relative to the repo root `entities/` folder.

- [ ] For each entity type you intend to spawn via CIGI, ensure a mesh exists at the configured path
- [ ] Static meshes: import via Content Browser → Import → `.fbx` or `.gltf`
- [ ] Skeletal meshes: same import flow; set `skeletal: true` in the YAML entry
- [ ] Optional damaged/destroyed variants: import and set `mesh_damaged:` / `mesh_destroyed:` YAML keys
- [ ] Verify paths in `deploy/camsim_config.yaml` under `entity_types:` match Content Browser asset paths

---

## Part 5 — Enable Features in Config

After all required assets exist, enable features in `deploy/camsim_config.yaml`.
All are disabled by default.

```yaml
# Phase 18 particle/weather features
phase18:
  rotor_wash_enabled: true
  smoke_enabled: true
  fire_enabled: true
  contrail_enabled: true
  cratering_enabled: true        # requires M_Crater
  god_rays_enabled: true         # requires Directional Light with light shafts
  volumetric_clouds_enabled: true  # requires Volumetric Cloud actor
  second_fog_enabled: true       # requires Exponential Height Fog actor

# Phase 19 ocean features
phase19:
  ocean_enabled: true
  beaufort_state: 3              # light waves — good for initial testing
  vessel_wakes_enabled: true
  vessel_motion_enabled: true
  ocean_reflections_enabled: true
  ssr_intensity: 1.0
```

Rebuild + launch: `scripts/run.sh --build`

---

## Verification

- [ ] Ocean plane visible from altitude with animated wave deformation
- [ ] Sea-domain entities show pitch/roll/heave (`beaufort_state: 6` for visible motion)
- [ ] Wake trails spawn behind moving vessels and fade after ~8 s
- [ ] Sky colour reflected in ocean surface (SSR)
- [ ] Rotor wash dust visible beneath hovering air entities
- [ ] Smoke + fire spawn on destroyed entities
- [ ] Contrail ribbons trail behind fixed-wing entities at altitude
- [ ] Crater decals appear on terrain after Phase 18I trigger
- [ ] God rays visible when sun near horizon (Phase 18E)
- [ ] Cloud shadows cast on terrain (Phase 18B)

---

## Quick Reference: Asset Paths

All paths are overridable in `camsim_config.yaml` or via `CAMSIM_*` env vars.

| Asset | Default Path | Type | Phase | Missing Behaviour |
|-------|-------------|------|-------|-------------------|
| `MPC_Ocean` | `/Game/Materials/MPC_Ocean` | MaterialParameterCollection | 19 | Ocean disables silently |
| `M_Ocean` | `/Game/Materials/M_Ocean` | Material | 19 | Ocean plane has no material |
| `NS_VesselWake` | `/Game/Effects/NS_VesselWake` | NiagaraSystem | 19 | No wake FX, warning logged |
| `NS_RotorWash` | `/Game/Effects/NS_RotorWash` | NiagaraSystem | 18F | No rotor wash, warning logged |
| `NS_Smoke` | `/Game/Effects/NS_Smoke` | NiagaraSystem | 18G | No smoke, warning logged |
| `NS_Fire` | `/Game/Effects/NS_Fire` | NiagaraSystem | 18G | No fire, warning logged |
| `NS_Contrail` | `/Game/Effects/NS_Contrail` | NiagaraSystem | 18H | No contrails, warning logged |
| `M_Crater` | `/Game/Effects/M_Crater` | MaterialInterface | 18I | No crater decals, warning logged |
| Entity meshes | Per `entity_types:` YAML | Static/SkeletalMesh | 8 | Entity invisible, warning logged |
| DirectionalLight | (level actor) | Actor | all | Sun control disabled |
| SkyLight | (level actor) | Actor | all | No ambient/sky reflection |
| SkyAtmosphere | (level actor) | Actor | 18J | Atmospheric scattering skipped |
| VolumetricCloud | (level actor) | Actor | 18B | Cloud shadows skipped |
| ExponentialHeightFog | (level actor) | Actor | 18C/18K | Fog effects skipped |
| PostProcessVolume | (level actor) | Actor | 19 | SSR intensity not applied |
| Cesium3DTileset | (level actor) | Actor | all | **No terrain** |

---

## Reference Source Files

| File | Relevant to |
|------|-------------|
| `Ocean/FGerstnerOceanSurface.cpp` | MPC parameter names, wave math |
| `Ocean/FOceanManager.cpp` | Material loading, SSR PPV lookup |
| `Environment/CamSimParticleManager.cpp` | All Niagara spawn paths + `FadeTime` param |
| `Environment/CamSimEnvironment.cpp` | Level actor discovery (TActorIterator calls) |
| `Entity/CamSimEntity.cpp` | Entity mesh loading from YAML paths |
| `Config/CamSimConfig.h` | All `FPhase18Config` / `FPhase19Config` path fields |
| `deploy/camsim_config.yaml` | Canonical runtime config — enable features here |

---

## Phase 24C — Normal Maps (content, not C++)

- Entity materials need normal map textures assigned in the UE5 editor material editor.
- Each glTF/FBX entity model should have a corresponding `_Normal.png` / `.tga` texture.
- After assigning, verify the normal map intensity with `r.NormalMap.Enable=1` in the console.
- No C++ changes needed; `ShowFlags.SetMaterialNormalMap(true)` is already set (Phase 24C).
