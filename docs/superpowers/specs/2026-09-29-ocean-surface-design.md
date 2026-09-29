# Ocean Surface for Boats — Design

**Date:** 2026-09-29
**Status:** Approved in conversation (sections 1–4); user asked to proceed straight to execution
**Scope:** sub-project "ocean for boats" (ROADMAP 2.6): a sea-level water surface that follows the
globe, sea state (Beaufort or CIGI Wave Control), boats that ride the waves, and HAT/HOT that sees
the water. Wakes, whitecaps, spray, breaking waves, LOS against water, Environmental Conditions
Request/Response and inland below-sea-level masking are out of scope (carry-overs).

## Intent

What the user said: add an ocean for the boats; "done" is option 2 of 3 — correct water plus sea
state, with boats pitching/rolling/heaving to match (not wakes/reflections). Built-in UE rendering
features are fine; the Water plugin is not a fit (below).

Assumptions: the output is ATR/training imagery, so correctness (sea level, placement, ground
truth) comes before visual polish; editor assets should be scripted rather than hand-made where
possible.

Success: a DIS boat in SF Bay floats on a drawn water surface at EGM96 sea level (not the
bathymetric seabed ~23 m below), rides Beaufort 0/3/6 or CIGI-commanded waves with plausible
pitch/roll/heave, keeps one stable COCO `boat` ID, HOT at the boat returns the water surface, and
the stream holds the 2.5 frame-time bar.

ROADMAP rule 5 ("no new feature phases until Milestones 0–4 land") is waived for this, as for 2.5:
the ocean is the main 2.5 carry-over and the boats are wrong without it.

## Current state (verified 2026-09-29)

- `Ocean/` (Phase 19, off by default, parked): `FOceanManager` puts `/Engine/BasicShapes/Plane`
  scaled to 200 km at UE Z = 0 (the georeference origin's height, not sea level; flat, so ~785 m
  off the globe 100 km out), recentred on the camera each tick. `ocean_material_path` is loaded as
  a `UMaterialParameterCollection` and no material is ever assigned; `M_Ocean` does not exist.
- `FGerstnerOceanSurface`: one sine wave along UE +X, time from `DeltaTime`.
- `ACamSimEntity::ApplyVesselMotion` nudges sea-domain actors after `ApplyCommand`, on top of the
  2.5 surface placement.
- `CamSimSurface::ClampWater` (2.5): height from a trace against Cesium tiles (the seabed over
  bathymetry); on a miss, the last water height, else EGM96 sea level.
- CIGI: Wave Control (opcode 14) parsed into `FCigiWaveState` (ID, enable, height, length,
  period) and mapped to `FOceanWaveCommand`; Maritime Surface Conditions (13) into
  `FMaritimeSurfaceCommand`. Environmental Conditions Request is not implemented.
- `Geospatial/Geoid.h` `GetGeoidUndulation(Lat, Lon)`; `Time/SimClock.h` `FSimClock`.
- Cesium's tileset exposes `EnableWaterMask` and ships `M_CesiumOverlayWater` (not used here).

## Why not the UE Water plugin

Its water zone is a flat quadtree over world XY with +Z up and waves in world XY: in a Cesium world
"up" is +Z only at the georeference origin, so it breaks exactly as the flat plane does. It relies
on Landscape and editor-placed water-body splines for shorelines (we have neither; Cesium streams
the world). Its buoyancy is a Chaos physics sim, which fights a host-authoritative pose and is not
deterministic. We use UE's rendering built-ins instead: `UProceduralMeshComponent`, the material
system with the **Single Layer Water** shading model, lighting/shadows, Lumen/SSR reflections, sky
light.

## Architecture

```
Config ocean: / CIGI Wave Control + Maritime Surface (via Hosts/ adapters → Sim/Commands.h)
        │
        ▼
FOceanManager (Environment-owned; game thread)
  ├─ FOceanWaves (pure C++): wave set, anchor, sim time → height / displacement at (lat, lon)
  ├─ FOceanMesh: UProceduralMeshComponent rings on ellipsoid + geoid, anchored under the camera
  └─ MPC writes: per-wave k, amplitude, Q, direction, phase (ωt mod 2π), anchor, E/N/U axes
        │
        ├─► M_Ocean (Single Layer Water; WPO + analytic normals from the same Gerstner sum)
        ├─► CamSimSurface::ClampWater (boats: height/pitch/roll from FOceanWaves)
        └─► CigiQueryHandler HAT/HOT (max(Cesium hit, sea surface))
```

### Units

All new interfaces use metres, degrees and seconds (sim time). UE centimetres appear only at the
mesh/material boundary.

### FOceanWaves (`Ocean/OceanWaves.h/.cpp`)

Pure, UObject-free, unit-testable. Holds up to 4 Gerstner waves:

```cpp
struct FOceanWave
{
	double HeightM     = 0.0;  // crest-to-trough; amplitude a = HeightM / 2
	double LengthM     = 0.0;
	double PeriodS     = 0.0;  // 0 → deep-water dispersion
	double FromDeg     = 0.0;  // direction the wave comes FROM, true north (travels toward FromDeg + 180)
	double PhaseRad    = 0.0;  // host phase offset
	double Steepness   = 0.0;  // Gerstner Q_i (set by the builder so Σ Q_i k_i a_i ≤ 1)
};
```

- `static TArray<FOceanWave> FromBeaufort(double Beaufort, double FromDeg, double Choppiness)`:
  samples `FBeaufortTable` (kept); 4 waves at wavelength × {0.6, 0.85, 1.1, 1.4}, direction
  FromDeg + {−30, −10, +10, +30}°, amplitudes weighted {0.2, 0.35, 0.3, 0.15} then scaled so
  4·√(Σaᵢ²/2) equals the table's significant height; Qᵢ = Choppiness / (kᵢ aᵢ · N), N = 4.
  Beaufort 0 → no waves.
- Deep-water dispersion when PeriodS = 0: ω = √(g k), g = 9.80665. Otherwise ω = 2π / PeriodS.
- Frame: a local tangent plane at an **anchor** (lat, lon) with east/north axes.
  `SetAnchor(Lat, Lon)` advances each wave's phase by kᵢ · (dᵢ · Δ), where Δ is the anchor shift in
  the old plane (`CamSimFrames::GeodeticDeltaToNeu`), so the surface is continuous.
- `SetTime(double SimSeconds)`: stores t; `GetPhase(i)` = (φᵢ − ωᵢ t) wrapped to [0, 2π) — the
  position-independent part of θ, computed in double so large t never reaches the GPU.
- `Displacement(E, N)` → (dE, dN, dZ) in metres (Gerstner: horizontal Qᵢ aᵢ dᵢ cos θ, vertical
  aᵢ sin θ, θ = kᵢ (dᵢ·x) − ωᵢ t + φᵢ).
- `HeightAt(Lat, Lon)`: the surface height (m, relative to sea level) at the point where the
  displaced surface lands, found by 3 fixed-point iterations x₀ ← x − D_h(x₀).
- `SignificantHeight()` for tests/logging.

### Sea level

`SeaLevelM(Lat, Lon) = GetGeoidUndulation(Lat, Lon) + TideOffsetM`, where TideOffsetM is CIGI
Maritime Surface Height (Global scope; default 0). Without the geoid grid the ocean is disabled
with a warning (no silent zero).

### FOceanMesh (`Ocean/OceanMesh.h/.cpp`)

- `UProceduralMeshComponent` (enable the `ProceduralMeshComponent` plugin and module) owned by
  the environment actor.
- Geometry: concentric rings around the camera's nadir. Ring 0 is a square grid, 2 m cells,
  ±128 cells; each further ring doubles the cell size, until the radius
  R = min(3.57 km · √(max(alt_above_sea_m, 2)) + 10%, `max_radius_km`). Ring seams are stitched
  (skirt/T-junction fill) so no cracks show.
- Vertex placement: each grid (E, N) offset → `CamSimFrames::OffsetGeodetic(nadir, E, N)` →
  height `SeaLevelM(lat, lon)` → UE position through the georeference, stored relative to the
  component origin at the nadir point (float precision ~1.5 cm at 200 km). Per-vertex normals are
  the ellipsoid up; the material supplies wave normals.
- Rebuild (on the game thread; target < 10 ms, measured) when the camera nadir moves more than
  10% of R from the build centre, R changes by > 25%, or on a teleport (`IsHorizontalJump`
  threshold). Between rebuilds only the component moves, snapped to the ring-0 cell size. The wave
  anchor re-anchors to the new build centre on each rebuild.
- Collision off; cast shadows off; receives shadows.

### M_Ocean (`Content/Ocean/M_Ocean.uasset`, scripted)

Generated by `scripts/ocean/make_ocean_material.py`, run headless:
`UnrealEditor-Cmd CamSimTest.uproject -run=pythonscript -script=…` (the asset is committed; the
script is the source of truth). Parameters from an MPC `Content/Ocean/MPC_Ocean`:

- Per wave i < 4: `Wave{i}` = (k in 1/cm, amplitude cm, Q, phase rad), `WaveDir{i}` = (dE, dN).
- `ComponentToAnchor` (cm, float): the component origin's offset from the wave anchor in UE world
  axes. The anchor is the build centre, so this stays ≤ ~10% of R (float-safe); MPCs are float
  only, so no large-world position is ever passed.
- `AxisE`, `AxisN`, `AxisU` (UE world, unit; the component has identity rotation).
- `Absorption`, `Scattering` (Single Layer Water; driven by CIGI Clarity).

Material: shading model Single Layer Water; WPO = Σ Gerstner displacement along AxisE/AxisN/AxisU,
evaluated at (pre-WPO Local Position + ComponentToAnchor) projected on AxisE/AxisN; normal from the analytic
Gerstner partial derivatives; a small procedural ripple normal (visual only); low roughness. If
Single Layer Water misbehaves on Metal or blows the budget, fall back to Default Lit (opaque) and
record why in the ROADMAP.

### FOceanManager (rewritten)

- `Init(World, Owner, Subsystem, const FCamSimConfig::FOceanConfig&)`: builds FOceanWaves from
  config, creates FOceanMesh, loads `M_Ocean` and `MPC_Ocean`, publishes `const FOceanWaves*` and
  the sea-level function to the entity manager and the query handler (via the subsystem).
- `Tick(CameraGeo)`: sets `FSimClock` sim time on the waves, rebuilds/moves the mesh, writes the
  MPC. Sim time, not `DeltaTime` (closes the 2.1 "ocean waves use DeltaTime" item).
- `ApplyWave(const FOceanWaveCommand&)`, `ApplyMaritimeSurface(const FMaritimeSurfaceCommand&)`,
  `ApplyConfig(const FOceanConfig&)` (hot reload of the wave fields).
- Removed: `FGerstnerOceanSurface`, `IOceanSurface`, the SSR/sky-light code,
  `ACamSimEntity::ApplyVesselMotion`, `FCamSimEntityManager::SetOceanSurface`.

### Boat placement (`Entity/SurfaceClamp.h`, `SurfaceProbe.h`)

`ClampWater` gains an optional ocean input (`const FOceanWaves*`, sea-level function, motion
flag/scale, half length/beam). With the ocean on:

- Base height = max(Cesium centre hit, sea level) — seabed loses to the sea, a lake above sea
  level wins. The existing easing still applies to the base height only.
- Waves (if the base is the sea and `vessel_motion`): sample `HeightAt` at the four footprint
  points (`GetFootprint`); pitch = atan2(bow − stern, 2·HalfLength), roll = atan2(stbd − port,
  2·HalfBeam), heave = mean; each × `vessel_motion_scale`; never eased. Heading from the sender;
  sender pitch/roll replaced.
- Ocean off: behaviour byte-identical to today.

### HAT/HOT (`CIGI/CigiQueryHandler`)

With the ocean on, the terrain height used for HAT/HOT = max(Cesium hit, sea surface incl. waves at
that point). Extended responses report the water normal when the water wins (from the Gerstner
derivatives). Material code stays 0.

### Config (`ocean:` replaces `phase19:`)

```yaml
ocean:
  enabled: true              # CAMSIM_OCEAN_ENABLED (startup only)
  beaufort: 3                # 0–12, fractional OK; CAMSIM_OCEAN_BEAUFORT
  wave_direction_deg: 270    # direction waves come FROM, true north; CAMSIM_OCEAN_WAVE_DIR
  choppiness: 0.5            # 0 = sine, 1 = steepest without looping; CAMSIM_OCEAN_CHOPPINESS
  vessel_motion: true        # CAMSIM_OCEAN_MOTION_ENABLED
  vessel_motion_scale: 1.0   # CAMSIM_OCEAN_MOTION_SCALE
  max_radius_km: 400         # CAMSIM_OCEAN_MAX_RADIUS_KM
  material: "/Game/Ocean/M_Ocean"
```

`FPhase19Config` becomes `FOceanConfig` (`Cfg.Ocean`). Dropped keys: amplitude/frequency scales,
wakes/Niagara, SSR/reflection radius. `enabled` defaults to true if the acceptance run holds the
frame budget, otherwise false with the reason in the ROADMAP. Documented in `docs/configuration.md`.

### CIGI (through `Hosts/CigiCommands`)

- `FCigiWaveState` / `FOceanWaveCommand` gain Direction (deg), Phase Offset (deg), Scope,
  Region/Entity ID, Breaker Type. Direction semantics (from/toward) are checked against the CIGI
  3.3 ICD and converted to FromDeg.
- Wave Control: Global scope; each Wave ID < 4 sets one wave (IDs ≥ 4 log once and are ignored);
  Wave Enable = 0 removes that ID; any CIGI wave replaces the Beaufort set; removing the last CIGI
  wave restores it. Regional/Entity scopes log once and are ignored. Breaker type ignored.
- Maritime Surface Conditions (Global): Surface Height → TideOffsetM; Clarity → absorption and
  scattering; water temperature stored for Milestone 4; whitecaps ignored.

## Testing

Unit (NullRHI, CI):

- `CamSim.Ocean.Waves.*`: Beaufort significant height within 1%; Σ Qᵢkᵢaᵢ ≤ 1 at choppiness 1;
  dispersion vs host period; direction (from 270° → crests move east); re-anchor 50 km leaves the
  height at a fixed point unchanged (< 1 mm); `HeightAt` vs brute-force search ≤ 1 cm at
  choppiness 1; frozen clock freezes the surface, `time_scale` 2 doubles the rate.
- `CamSim.Ocean.Mesh.*`: vertices on ellipsoid + geoid ≤ 1 cm (centre, mid ring, edge); radius
  formula and cap; rebuild only past the thresholds or on a teleport. (Geometry is generated by a
  pure function; the component is a thin wrapper.)
- `CamSim.Surface.*` (extends `SurfaceClampTest`): seabed −23 m → sea level; lake +300 m → lake;
  wave pitch/roll/heave sign and size along/across a single wave; ocean off identical to today.
- `CamSim.Hosts.*`: Wave Control → `FOceanWaveCommand` carries every field; enable 0 removes;
  last removal restores Beaufort; Maritime Height/Clarity map through; a CCL-encoded packet
  decodes.
- HAT/HOT: stub probe returning the seabed → the sea surface.
- `Phase19OceanTest.cpp` is deleted; still-valid cases move into the new files.

GPU (`CamSim.GPU.Ocean.MatchesCpu`, real RHI): nadir render at a known sim time, scene depth at a
pixel grid vs `FOceanWaves` heights, ≤ 2 cm at Beaufort 5.

Acceptance (`scripts/ocean_check.py`, macOS/Metal, reusing the `dis_vehicle_check.py` harness):
DIS `boat-circle` in SF Bay at Beaufort 0, 3, 6 and one CIGI-Wave-Control run.

- COCO: one stable `boat` ID in every frame.
- Boat altitude within 0.5 m + wave amplitude of EGM96 sea level (today ≈ −23 m).
- HOT at the boat returns the sea surface.
- Frame times: median ≤ 33 ms, none over 66 ms once settled; ocean GPU cost recorded
  (`RDG_EVENT_SCOPE_STAT`-style GPU stat) and mesh rebuild CPU time logged.
- Shots for human review: nadir and oblique per sea state; close waterline (draft, no clipping
  through crests at Beaufort 6); Golden Gate coastline (clean occlusion, beach shimmer noted);
  shallow water showing the seabed; 10 km altitude (horizon, no visible mesh edge).

Results go into ROADMAP "2.6 Ocean surface for boats", with the rule-5 exception stated.

## Carry-overs

- Wakes, whitecaps, spray, breaking waves.
- LOS against the water surface; Environmental Conditions Request/Response.
- Inland below-sea-level land floods (Dead Sea, Death Valley, polders): clip with Cesium's water
  mask.
- Regional/Entity-scoped waves and maritime conditions.
- Physical IR of water (Milestone 4); until then IR sees the visible-light proxy.
