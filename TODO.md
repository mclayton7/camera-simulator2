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

## Phase 22D — Character Animation Assets (content, not C++)

Character animation infrastructure (UCamSimAnimInstance, AnimMeshComp, InitAnimatedCharacter)
is implemented in C++. The following UE editor content work is required to activate it:

### 7A — Character Skeletal Mesh

- [ ] Source or create a character skeletal mesh (Mixamo, UE Mannequin, or custom)
- [ ] Import via Content Browser → `Content/Characters/SK_Soldier`
- [ ] Ensure skeleton has standard humanoid bone hierarchy (root, pelvis, spine, etc.)

### 7B — Animation Sequences

- [ ] Import or retarget walk, run, idle animation sequences
- [ ] Place in `Content/Characters/Animations/` (e.g. `Anim_Idle`, `Anim_Walk`, `Anim_Run`)
- [ ] Optional: crouch, prone sequences for CompId=20 stance override

### 7C — Animation Blueprint `ABP_Soldier`

- [ ] Right-click → Animation → **Animation Blueprint** → select SK_Soldier skeleton
- [ ] Name it `ABP_Soldier` in `Content/Characters/`
- [ ] Set **Parent Class** to `UCamSimAnimInstance`
- [ ] Create state machine with states: Idle, Walk, Run (and optionally Crouch, Prone)
- [ ] Wire transitions using `AnimStateIndex` variable (0=Idle, 1=Walk, 2=Run, 3=Crouch, 4=Prone)
- [ ] Compile and save

### 7D — Entity Type YAML Entry

After creating the above assets, uncomment the animated entity entry in `deploy/camsim_config.yaml`:
```yaml
entity_types:
  "1010":
    mesh: /Game/Characters/SK_Soldier.SK_Soldier
    skeletal: true
    animated: true
    anim_blueprint: /Game/Characters/ABP_Soldier.ABP_Soldier_C
    entity_category: character
    scale: 1.0
```

### 7E — Verify Damage Transition FX

- [ ] Confirm existing `NS_Smoke` and `NS_Fire` Niagara systems (from Phase 18G) work as
  damage transition particle FX when triggered via CIGI CompId=10
- [ ] If gradual damage is enabled (`damage_transition.gradual: true`), verify mesh blend
  timing matches `interpolation_sec` config value

### 7F — Scorch Damage Material (optional, for gradual damage)

- [ ] Create Material Instance Dynamic with `ScorchBlend` scalar parameter (0.0 = clean, 1.0 = scorched)
- [ ] Apply to entity material during gradual damage blend
- [ ] `DamageScorchDarkening` config value controls max darkening (default 0.3)

---

## Phase 24C — Normal Maps (content, not C++)

- Entity materials need normal map textures assigned in the UE5 editor material editor.
- Each glTF/FBX entity model should have a corresponding `_Normal.png` / `.tga` texture.
- After assigning, verify the normal map intensity with `r.NormalMap.Enable=1` in the console.
- No C++ changes needed; `ShowFlags.SetMaterialNormalMap(true)` is already set (Phase 24C).

---

## Part 6 — Phase 27A GPU Sensor Post-Process Material

Required when `gpu_sensor_effects: true` in camsim_config.yaml.

**6A: Material Parameter Collection (MPC_SensorParams)**
- Path: `Content/CamSim/Materials/MPC_SensorParams`
- Scalar parameters:
  - `SensorMode` (float: 0=EO, 1=IR, 2=NVG)
  - `NoiseIntensity` (float: 0.0–1.0)
  - `AGCLevel` (float: 0.0–1.0)

**6B: Post-Process Material (M_SensorPostProcess)**
- Path: `Content/CamSim/Materials/M_SensorPostProcess`
- Material domain: Post Process
- Blendable location: Before Tonemapping
- Must sample `MPC_SensorParams` for all effect parameters
- EO mode: color pass-through + optional vignette
- IR mode: luminance-based false-color ramp + Gaussian noise
- NVG mode: green phosphor tint + grain noise

**Performance impact:** When active, eliminates 10-25ms CPU sensor processing per frame.
The CPU pipeline bypass (SensorPostProcess.cpp) only runs defects + quantization + precipitation + HUD (<1ms).

**Verification:** Set `gpu_sensor_effects: true`, confirm IR ramp visible in output video

---

## Part 7 — Engineering Punch List (Architecture Review, 2026-04-16)

Code-level items surfaced by a general architecture / performance / Cesium review.
These are C++ changes, not editor content work. Paths are relative to
`unreal_project/CamSimTest/Source/CamSimTest/` unless noted otherwise.

Each item: **what**, **where**, **why**, **fix**. Grouped by impact tier.

**Implementation status (updated 2026-04-16):**
- **Tier 1 (7A.1–7A.6):** all six items landed — see the diff on branch `main`.
- **Tier 2 (7B.1–7B.9):** all nine items landed.
- **Tier 3 (7C.*):** still open — pure polish, can wait.
- Verification suggestions in §7D are NOT yet run (no UE build environment here);
  `stat unit` / automation tests should be exercised before shipping.

### 7A — Tier 1: High-ROI Performance & Correctness

#### 7A.1 Drop `FlushRenderingCommands()` from the per-tick readback path

- **Where:** `Camera/CamSimCamera.cpp:750`
- **Why:** The readback is designed async (EnqueueCopy + IsReady poll), but the
  code then synchronously flushes the render thread every frame. At 30 fps this
  stalls the game thread 2–5 ms per tick and serializes game/render work that
  should overlap. Single highest-ROI change in the repo.
- [ ] Replace the per-tick `FlushRenderingCommands()` with an `FRenderCommandFence`
      fired on frame N; on frame N+2 check `IsReady()` from a queued render
      command and signal the game thread via `TAtomic<bool>` / `FEvent`
- [ ] Benchmark before/after with `stat unit` and `stat GPU` to confirm the win

#### 7A.2 Pool the GPU readback objects (one per render target)

- **Where:** `Camera/CamSimCamera.cpp:436, 461` (and raw-pointer fields in `.h:118, 144`)
- **Why:** Triple-buffered render targets but a single shared
  `FRHIGPUTextureReadback`. EnqueueCopy on frame N+1 can race frame N's
  in-flight copy. The raw pointers also bypass RAII, so teardown is manual.
- [ ] Replace `FRHIGPUTextureReadback* GPUReadback` (and `DepthGPUReadback`)
      with `TArray<TUniquePtr<FRHIGPUTextureReadback>>` sized to match
      `NumCaptureTargets` (triple buffer)
- [ ] Round-robin by `CaptureTargetIndex` so each readback has exclusive
      ownership until consumed

#### 7A.3 Disable physics meshes on every `ACesium3DTileset`

- **Where:** `Camera/CamSimCamera.cpp:206–232` (tileset tuning loop)
- **Why:** The camera never collides with terrain. Every loaded tile currently
  cooks collision geometry that nothing queries — wastes VRAM and CPU cook time,
  especially during camera motion. If HAT/HOT terrain queries are needed,
  keep physics on only for the terrain tileset flagged for it.
- [ ] Add `It->SetCreatePhysicsMeshes(false);` inside the tuning loop
      (one line, immediate win)
- [ ] If HAT/HOT is required for a specific tileset, make it opt-in via tag or
      config rather than default-on

#### 7A.4 Reapply Cesium tileset params on hot-reload

- **Where:** `Subsystem/CamSimSubsystem.cpp:205–222` (`HotReloadConfig`)
- **Why:** `MaxSimultaneousTileLoads`, `MaximumScreenSpaceError`,
  `MaximumCachedBytesMB`, `LoadingDescendantLimit` are applied once at
  `BeginPlay`. Hot-reload mutates the config fields but nothing pushes them
  to live tilesets — operators tuning at runtime see zero effect.
- [ ] At the end of `HotReloadConfig()`, iterate `ACesium3DTileset` actors and
      re-run the tuning loop from `CamSimCamera.cpp:206–232`
      (extract to a shared helper: `FCamSimTilesetTuner::Apply(World, Cfg)`)
- [ ] Add an automation test that flips `MaximumScreenSpaceError` via
      `HotReloadConfig` and asserts propagation to the live tileset

#### 7A.5 Fix dead-reckoning quaternion integration (Euler → gimbal lock)

- **Where:** `Entity/CamSimEntity.cpp:618–646` (`UpdateDeadReckoning`)
- **Why:** Euler rates (`YawRate`/`PitchRate`/`RollRate`) are integrated each
  tick and rebuilt into `FQuat` via `FRotator`. At pitch ±90° this passes
  through the `FRotator→FQuat` gimbal-lock singularity and produces
  discontinuous headings. Ground targets are fine; aircraft dive/climb glitch.
- [ ] Store orientation as `FQuat` in `FDRState` (replace Euler angles)
- [ ] Integrate body-frame angular velocity on the quaternion directly:
      `Q_{t+1} = (Q_t * FQuat(BodyAngVel * dt)).GetNormalized()`
- [ ] Add a unit test: 1000 frames of pure pitch-rate input should land within
      1e-3 rad of the analytic quaternion result

#### 7A.6 Bound the SPSC queues (receiver → game thread, encoder sink)

- **Where:** `CIGI/CigiReceiver.h:147–164`, `DIS/DisReceiver.h:66`,
  `Encoder/EncoderThread.cpp:51`
- **Why:** All queues are unbounded `TSpscQueue`. A GC spike or encoder stall
  lets the producer grow the heap without limit. No counters in `/metrics`.
- [ ] Wrap each queue with a bounded variant (caps: 1024 entity-state,
      256 HAT/HOT responses, 4 video frames)
- [ ] On overflow, drop-oldest + increment a counter
- [ ] Wire the counters into `FrameDropStats_` (already exists on camera) and
      expose them via the `/metrics` endpoint

### 7B — Tier 2: Important

#### 7B.1 Scale Cesium culling to actual sensor FOV

- **Where:** `Camera/CamSimCamera.cpp:222–223` (`CulledScreenSpaceError = 200.0`)
- **Why:** Hardcoded for a ~60° FOV. A 5° EO sensor can tolerate far more
  aggressive off-frustum culling and needs a wider prefetch FOV during
  gimbal slew to avoid pop-in.
- [ ] Scale `CulledScreenSpaceError` by `HFovDeg / 60.0f` (clamp ≥ 100)
- [ ] Inflate prefetch FOV by `GimbalMaxSlewRateDegPerSec * frame_budget` when
      updating the prefetch camera in `UpdateCesiumCamera()`
- [ ] Expose `CacheDir` in `FCesiumBackendConfig` so tiles persist across
      runs (reduces re-download on region revisit)

#### 7B.2 Shrink `UCamSimSubsystem::Tick()` and move file I/O off the game thread

- **Where:** `Subsystem/CamSimSubsystem.cpp:585–921` (~337 lines)
- **Why:** Does CIGI sender flush, watchdog, health JSON serialization,
  Prometheus file write, and structured logging in one method. Sync file I/O
  every 90/150 ticks on the game thread is a cheap but visible stall source.
- [ ] Extract health/metrics writer into a dedicated `FTickableGameObject`
      or run it on the task thread (snapshot game-thread state, write off-thread)
- [ ] Keep only CIGI sender/responder flush in the subsystem Tick

#### 7B.3 Reuse entity-snapshot buffers and cull before projecting

- **Where:** `Entity/CamSimEntityManager.cpp:399–407` (`GetEntitySnapshot`)
- **Why:** Allocates a fresh `TArray<FEntityAnnotationData>` every tick and
  projects every entity AABB — no frustum culling. At `MaxEntities = 500`
  that's 15k projections/sec.
- [ ] Make the result buffer a member; call `Reset(EntityMap.Num())` each tick
- [ ] Bounding-sphere vs. camera-forward dot-product cull before projection
      (skip projection for anything outside the prefetch cone)

#### 7B.4 Async entity mesh loading

- **Where:** `Entity/CamSimEntity.cpp:241–244, 270–280`
- **Why:** `LoadSkeletalMeshFromPath` / `LoadStaticMeshFromPath` run on the
  game thread from the entity-manager tick. A burst of DIS/CIGI spawns stalls
  the frame while mesh cook/stream finishes.
- [ ] Route mesh loads through `FStreamableManager::RequestAsyncLoad`
- [ ] Queue pending-spawn entities; hide them until their mesh is resident
- [ ] Move `const_cast<FEntityTypeTable*>` cache writes to `mutable` fields
      on `FEntityTypeTable` and drop the cast

#### 7B.5 Pre-allocate the BGRA chroma-range fallback buffer

- **Where:** `Encoder/VideoEncoder.cpp:547`
- **Why:** The sws color-space-details fallback path allocates a full-frame
  temporary (≈3.6 MB at 720p) per tick. Either move the 16–235 range
  compression into the readback conversion, or reuse one buffer.
- [ ] Allocate `CompressedBuf` once in `FVideoEncoder::Open()` and reuse
- [ ] Alternative: fold the range compression into
      `Camera/CamSimPixelConvert.h` so sws always gets correct-range input

#### 7B.6 Cache the `/metrics` snapshot

- **Where:** `Health/CamSimHealthServer.cpp:87–94`
- **Why:** `GetPrometheusMetrics` runs inline in the HTTP handler. If a
  scraper polls during an entity-heavy tick, metrics generation runs on the
  HTTP thread and can race game-thread state.
- [ ] Produce a metrics snapshot on the game thread (once per second) into a
      `TSharedRef<const FString>`; have the HTTP handler read the shared ref
- [ ] Apply the same pattern to `/ready` if its work grows

#### 7B.7 Make `FCamSimConfig` safe for hot-reload

- **Where:** `Config/CamSimConfig.h` (and `cpp` hot-reload path)
- **Why:** ~780-line struct with nested `TMap`s. Hot-reload mutates these
  while the game thread reads them — the POD fields are mostly benign but
  the maps race.
- [ ] Swap to copy-on-write: `TSharedRef<const FCamSimConfig>` held by the
      subsystem; hot-reload publishes a new ref atomically; readers snapshot
      the ref at tick boundary
- [ ] Splitting into per-subsystem sub-structs is a nice-to-have follow-up,
      not required to fix the race

#### 7B.8 Clean CIGI/DIS receiver shutdown via `FEvent`

- **Where:** `CIGI/CigiReceiver.cpp:610–632` (`Stop`), same pattern in
  `DIS/DisReceiver.cpp:51–67`
- **Why:** The runnable may be inside `Socket->Recv()` when `Stop()` tears
  the socket down. Relies on the non-blocking 1 ms poll to notice.
- [ ] Add an auto-reset `FEvent* ShutdownEvent` per receiver
- [ ] `Stop()` sets `bShouldRun = false` **and** `ShutdownEvent->Trigger()`
- [ ] Receiver loop waits on the event with a small timeout instead of
      `FPlatformProcess::SleepNoStats`
- [ ] Verify non-blocking mode succeeded post-build; fail `Start()` if it didn't
- [ ] Multicast join failure in `FDisReceiver::CreateSocket` should return
      false, not log-and-continue

#### 7B.9 Proper JSONL escaping and off-thread flush

- **Where:** `Logging/CamSimJsonLogger.cpp` (only `"` and `\` currently escaped)
- **Why:** A newline or control char in a message field breaks every JSONL
  reader downstream. Flush also runs synchronously on the game thread.
- [ ] Full RFC 7159 escaping (or route through `FJsonObject::Serialize`)
- [ ] Move the file-write drain into a dedicated consumer thread on the
      existing SPSC queue

### 7C — Tier 3: Polish

#### 7C.1 Replace manual angle wrap with `FMath::UnwindDegrees`

- **Where:** `Camera/CamSimGimbalComponent.cpp:91–100` (`SlewAngle` lambda)
- [ ] Swap the `while` loop for `Delta = FMath::UnwindDegrees(Delta);`

#### 7C.2 Collapse `UCamSimSubsystem` accessor boilerplate

- **Where:** `Subsystem/CamSimSubsystem.cpp` — 14 copies of
  `return Impl ? Impl->X.Get() : nullptr;`
- [ ] Add a private inline helper template:
      `template<typename T> T* ImplGet(const TUniquePtr<T>& P) const { return Impl ? P.Get() : nullptr; }`

#### 7C.3 Reuse the KLV scratch buffer on the encoder thread

- **Where:** `Metadata/KlvBuilder.cpp` (called from `Encoder/VideoEncoder.cpp:654`)
- [ ] Keep a `TArray<uint8>` member on the encoder; pass by out-param into
      `FKlvBuilder::BuildMisbST0601`; `Reset()` each frame; mutate only the
      tags that changed (timestamp, platform lat/lon/alt, gimbal)
- [ ] Reuse `AVPacket` via `av_packet_unref` instead of
      `av_packet_free` per frame (`VideoEncoder.cpp:657`)

#### 7C.4 Generation numbers for DIS→CamSim ID allocation

- **Where:** `DIS/DisEntityAdapter.cpp:244–273` (`GetOrAllocateId`)
- **Why:** Free-list reuse of IDs without a generation can alias stale
  consumer-side handles onto new entities.
- [ ] Either stop recycling and use a monotonic counter, or pair each allocated
      ID with a 16-bit generation that consumers must match

#### 7C.5 Bounds-validate CIGI env-packet and DIS PDU lengths

- **Where:** `CIGI/CigiReceiver.cpp:368–448` (`PreParseEnvPackets`),
  `DIS/DisPduTypes.cpp:90–166` (`FDisEntityStatePdu::Parse`)
- [ ] Guard against `PktSize < 2` and `Pos + PktSize > Len`; log + break on bad
      input rather than advancing
- [ ] Log DIS PDUs that miss the 144-byte floor at `Verbose` so operators can
      see silent drops

#### 7C.6 NaN/range guard on CoT telemetry

- **Where:** `Streaming/CotSender.cpp:116–142` (`BuildCotXml`)
- [ ] Reject frames where `lat`/`lon`/`alt` are non-finite or out of bounds;
      emit a warning and skip the datagram rather than shipping a NaN lat

#### 7C.7 Latency tracker wraparound math

- **Where:** `Diagnostics/PipelineLatencyTracker.cpp:65`
- [ ] Either round `BufferCapacity` up to a power of two or replace the
      `WIdx % BufferCapacity` dance with an explicit circular-index counter
      that's safe against `uint32` overflow

#### 7C.8 Bounds-check in `FHudOverlay` draw primitives

- **Where:** `Overlay/FHudOverlay.h:59–83`
- [ ] Pass frame W/H to each `Draw*` method and clamp glyph/line writes at the
      edges instead of trusting callers

### 7D — Verification Suggestions

Before merging Tier 1 items:

- [ ] `stat unit` + `stat GPU` capture on a representative scene before/after
      the `FlushRenderingCommands` removal to confirm >1 ms game-thread save
- [ ] Run `scripts/ci_validate.sh` to confirm the video/KLV pipeline is
      still compliant after readback-pool changes
- [ ] Add an automation test that spawns 500 entities in a single frame and
      confirms the frame does not exceed 2× nominal budget (catches regression
      on async mesh loading)
- [ ] Add a `HotReloadConfig` test that mutates `MaximumScreenSpaceError` and
      asserts the value reached the live `ACesium3DTileset`

### 7E — What's Working (Don't Regress)

Non-issues called out during review — worth preserving:

- Pimpl-backed `UCamSimSubsystem` ownership
- Four-thread split (CIGI receiver, game, render, encoder) with SPSC queues
- Cesium prefetch camera + `EnforceCulledScreenSpaceError` + LOD transitions
- Cesium camera de-registration in `EndPlay` (`Camera/CamSimCamera.cpp:493–507`)
- 29 UE Automation tests + CLAUDE.md gotcha documentation
- `FCigiReceiver` playback/recording (mirror into `FDisReceiver` per 7B.8)
