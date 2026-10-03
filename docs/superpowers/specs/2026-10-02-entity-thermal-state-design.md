# Entity thermal state for thermal IR (ROADMAP 4C) — design

Status: implemented 2026-10-02 (branch `feat/entity-thermal`); this text is aligned with the as-built design (see ROADMAP 4C).
Builds on 4A (`2026-10-01-thermal-core-design.md`) and 4B (`2026-10-01-terrain-classification-design.md`).
User decisions: state comes from CIGI + DIS + speed; temperatures lag (first order, sim time);
hot parts are body-frame volumes from config (no assets); scope is engine/exhaust, running gear +
speed-dependent skin, and damage/burning. Aircraft exhaust plumes are out.

## Goal

A vehicle's IR signature follows what it is doing, not only its class:

- a running truck shows a hot engine deck and exhaust, warm running gear when it moves, and a
  slightly warm skin; a parked, long-cold truck reads as paint at the diurnal temperature;
- a truck that has just parked stays hot for minutes (exhaust cools in about a minute, the engine
  deck over a quarter of an hour): the "recently driven" cue;
- speed cools sunlit skin toward air temperature (a fast boat at noon reads cooler than a moored one);
- a destroyed vehicle burns (hull ~700 K) for a few minutes, then cools as a hulk.

Success, in the 4A acceptance views (Presidio, DIS truck and boat):

1. running truck minus parked-cold truck, box mean, MWIR and LWIR night: ≥ +5 DN;
2. running truck hot spot (box p99 minus box median) at night: ≥ +20 DN MWIR, ≥ +10 DN LWIR;
3. 600 s after the truck parks with the engine off, its box mean (over the ring) is lower than 5 s after
   parking and still above the parked-cold truck's (as built: the mean, not p99 — MWIR hot parts clip at
   the display ceiling, so p99 cannot fall; and +600 s, not +180 s: for the first minutes after parking the
   idle hold keeps the engine running and the lost convection warms the skin, so the box warms slightly);
4. a destroyed truck (DIS damage = 3) reads hot: box mean minus ring ≥ +60 DN MWIR night;
5. `ThermalCS` p95 at 1080p stays ≤ 0.5 ms (gate f) with parts on;
6. `thermal.entity.enabled: false` gives 4A output bit for bit; EO is unchanged (gate g).

## Non-goals

- Aircraft exhaust plumes (hot gas volumes; needs ray marching or a proxy mesh). Carried over.
- A thermal effect for "damaged" (slight/moderate). It stays a mesh swap only.
- Per-part emissivity, authored thermal masks, glTF node-name parts.
- Default parts for the F-16 (type 1001): its model orientation is unverified; aircraft parts work
  through config.
- Hot ground under or behind a parked vehicle, wakes, tyre tracks.
- Host-set initial temperatures ("parked 10 minutes ago" on spawn).
- Semantic class in ground truth, validation against published data (4D).

## Architecture

```
CIGI Component Control 11/10 ─┐
DIS Entity State Appearance ──┼─► FComponentCommand (canonical; DIS adapter emits on change)
                              │        │
                              │        ▼
                              │   ACamSimEntity::ApplyComponent ── commanded engine / damage / flaming
                              │        │
committed world pose ─────────┴─► FCamSimEntityManager tick (after poses, sim time)
                                       │  speed EMA, running logic
                                       ▼
                                  FEntityThermalModel::Step  (pure: Thermal/EntityThermal.{h,cpp})
                                       │  skin + ≤ 4 part temperatures, per entity
                                       ▼
                                  GetThermalStencilEntities ── stencil, type config, temps, world pose
                                       │
                                       ▼
                                  FThermalFrameBuilder ── 256 entity records (camera-relative transforms)
                                       │
                                       ▼
                                  ThermalCS (StructuredBuffer<float4> EntityRecords)
                                  CamSimThermalRef::EvaluatePixel (CPU mirror, same expressions)
```

## 1. Thermal state model

Each entity carries a skin temperature and up to **4 parts**, each of kind `engine`, `exhaust` or
`running_gear`. State lives in `FEntityThermalState`; `FEntityThermalModel::Step(State, Inputs,
Settings, Parts, SimSec)` is pure (no UE objects), so tests drive it directly.

**State is stored as excess over the parked baseline** `B = T_class + off_k` (K): absolute
`T = B + excess`. A parked, long-cold vehicle has zero excess and renders exactly as 4A, and the
diurnal drift of `B` needs no state. `T_class` and `T_air` exist only in `FThermalFrameBuilder`
(capture component, IR frames only), so the builder publishes an environment after each build
(`FEntityThermalEnv`: `T_air` and `B` per stencil) that the entity manager latches and steps with on
the next tick. With no environment yet (EO only so far) or for an untagged entity, `D = T_air − B` is
taken as 0. The targets below are written as absolute temperatures; the model subtracts `B`.

**Targets.** `T_class(t)` is the type's 4A class temperature (diurnal, closed form), `T_air` the air
temperature, `v` the smoothed speed (m/s), `off_k` the type's `thermal_offset_k` (default 0 with 4C on).

| Part | engine off | engine running |
|---|---|---|
| skin | `T_air + (T_class + off_k − T_air) · c(v)` | `T_air + (T_class + off_k + skin_running_k − T_air) · c(v)` |
| engine | skin target | `T_air + engine delta_k` (default 45 K) |
| exhaust | skin target | `exhaust temp_k` absolute (default 450 K) |
| running_gear | skin target | `T_air + min(k_per_mps · v, max_k)` (defaults 1.5 K per m/s, 30 K) |

- Convection factor `c(v) = v0 / (v0 + v)`, `v0 = convection_v0_mps` (default 10). `c(0) = 1`.
- `skin_running_k` default 4 K.
- **Destroyed** (damage state 2, or the DIS flaming bit): the burn starts at the first sim time it is
  seen. For `burn_s` (default 300 s) every target (skin and parts) is `burn_k` (default 700 K). After
  the burn, targets return to the engine-off row (a destroyed vehicle never runs), and the skin's
  cooling time constant becomes `hull_cool_tau_s` (default 1800 s). Leaving the destroyed state
  (damage back to 0/1, flaming cleared) ends the burn and restores normal targets and constants.
  The flaming bit alone (damage < destroyed) burns as long as it is set; when it clears, the cooling
  follows the hull constant.
- "Damaged" (state 1) has no thermal effect.

**Lag.** Every temperature relaxes toward its target: `T ← T + (T_target − T) · (1 − exp(−dt / τ))`,
`τ = tau_up_s` when `T_target > T`, else `tau_down_s`. Defaults:

| | tau_up_s | tau_down_s |
|---|---|---|
| exhaust | 20 | 60 |
| engine | 300 | 900 |
| running_gear | 180 | 600 |
| skin | 600 | 600 (1800 after a burn, `hull_cool_tau_s`) |

- `dt` is the sim-clock step. **Spawn**: state starts at the current targets. **Clock jump**: `dt < 0`
  or `dt > 3600 s` snaps every temperature to its target. **Frozen clock** (`dt = 0`): no change.
- Deterministic in the input history and sim time; no RNG.
- Excess is clamped to `[−200, 900] K` after each step; the builder clamps the absolute `B + excess` to
  `[150, 1000] K` (the LUT range).

## 2. Inputs

**One input path: canonical component commands** (`FComponentCommand`, entity class 0).

| CompId | meaning | states |
|---|---|---|
| 10 (existing) | damage | 0 intact, 1 damaged, 2 destroyed (existing mesh swap kept; 2 starts the burn) |
| **11 (new)** | power plant | 0 off, 1 on |
| **12 (new, DIS only)** | flaming | 0 off, 1 on |

- **CIGI**: Component Control, class 0, CompId 11 (and 10 as today). CompId 12 is accepted from CIGI
  too (documented as CamSim-specific).
- **DIS**: the DIS adapter decodes the Entity State Appearance (bits shared by land, air and surface
  platform appearance records, IEEE 1278.1 / SISO-REF-010): bit 22 power plant (1 on), bits 3–4 damage
  (0 none, 1 slight, 2 moderate, 3 destroyed), bit 15 flaming. It maps them to CompId 11 (bit), 10
  (none → 0, slight/moderate → 1, destroyed → 2) and 12 (bit). It keeps the last decoded value per
  entity and emits a command only when a value changes, and on the entity's first PDU only for values
  that differ from the defaults (0). Other kinds/domains (munitions, life forms, …) are not decoded.
  Side effect: a destroyed DIS entity now also swaps to `mesh_destroyed` when configured.

**Running** = commanded on (CompId 11 = 1) OR `v > moving_mps` (default 0.5) OR the entity was moving
within the last `idle_hold_s` (default 120 s, sim time). An explicit "off" never forces a moving
vehicle cold. Destroyed or flaming → not running.

**Speed.** From the committed world position (UE world, after surface clamping and attachment) each
entity-manager tick, over the sim-clock step: `v_raw = |ΔP| / dt`, smoothed by an EMA with τ = 1 s
(`v ← v + (v_raw − v)(1 − exp(−dt/1 s))`). A teleport (below) or the first tick after spawn sets
`v = 0` and skips the sample. Measured from positions, so it works for CIGI
(no velocity on the wire), DIS (dead-reckoned) and attached entities alike. Positions are ECEF (from
the entity's geodetic pose), not UE world, so Cesium origin shifts are not motion, and horizontal only (the
component along the geodetic up is removed: wave heave and terrain-refinement height changes are not travel). The teleport rule
is `|ΔP| > max(50 m, 400 m/s · dt)`; `dt ≤ 0` (frozen or rewound clock) keeps `v` and skips the sample.

**Ownership.** `ACamSimEntity` owns `FEntityThermalState` plus its commanded inputs (engine, damage,
flaming) and speed tracker; `ApplyComponent` sets the commanded inputs. `FCamSimEntityManager` steps
every entity's thermal state once per tick after poses are committed (its existing ordered pass), on
sim time, with `T_class` and `T_air` from the thermal model for the current sim time.
After each IR build the entity manager latches T_air and each entity's own B (through the stencil it holds at that
build) into the entity (`FEntityThermalLatch`); an entity without a latch (spawned since, or never in IR) steps with
D = 0. `GetThermalStencilEntities` returns, per tagged entity: stencil, type thermal config (as today), the
stepped skin and part temperatures, the part geometry, and the entity's world transform (double).

## 3. GPU evaluation and CPU mirror

**Entity records.** The builder fills **256 records** indexed by stencil (record 0 unused, untagged
stencils zero), each **16 `float4`**, uploaded per frame as an RDG structured buffer
`StructuredBuffer<float4> EntityRecords` (64 KB):

| float4 | contents |
|---|---|
| 0–2 | world→body affine, rows: `Pb_i = dot(M_i.xyz, Pw) + M_i.w`; `Pw` translated world (cm, camera at origin), `Pb` body metres (X forward, Y right, Z down) |
| 3 | skin T (K), class index, part count (0–4), unused |
| 4 + 3k | part k: centre xyz (m), shape (0 box, 1 ellipsoid) |
| 5 + 3k | part k: half-extents xyz (m, ≥ 0.01), falloff (m, ≥ 0.01) |
| 6 + 3k | part k: temperature (K), unused ×3 |

The transform is built on the game thread: the entity's world rotation and origin minus the camera
location, both in doubles, then converted to float. This is the same camera location the frame's
translated world is centred on (the builder already takes it for land cover's `CamOffsetM`), so
precision does not depend on distance from the georeference origin. The body frame is the entity's
UE actor frame with Z flipped (UE is left-handed X forward, Y right, Z up; body is right-handed X forward,
Y right, Z down), scaled cm → m, actor scale ignored. The game thread cannot know the view's
`PreViewTranslation`, so the builder writes the rotation rows and keeps each entity's world origin
(double) in the params; the render thread, where `ClipToTranslatedWorld` is set, fills each row's `w`
from `origin + PreViewTranslation` in doubles (`FinalizeEntityRecords`). The
mesh `rotation`/`scale` in the type table is already in the actor's component transform, so part
coordinates are in the vehicle's own frame as placed in the world (bow +X).

Record row 3's `w` is a valid flag. A valid record's skin T and class replace 4A's stencil table for
that pixel; an invalid one (4C disabled, untagged, or rejected values) falls back to
`StencilClass` / `StencilOffsetK` / the `StencilData` uniform array exactly as 4A, which the builder
keeps filling (`B` per stencil). With 4C disabled no record is valid and 4A's +8 K default for surface
vehicles applies: 4A bit for bit.

**ThermalCS, entity pixel** (after 4A's entity classification; `Pw` is already reconstructed):

1. `Pb = M · Pw` (record rows 0–2).
2. For each part k < count: outside distance
   - box: `d = length(max(abs(Pb − c) − h, 0))`;
   - ellipsoid: `d = (length((Pb − c) / h) − 1) · min(h.x, h.y, h.z)` (≤ 0 inside).
3. `w_k = 1 − smoothstep(0, falloff_k, d)` (1 inside).
4. `T = T_skin`; then for k in order: `T = T + (T_k − T) · w_k`. Later parts win in overlaps (config
   order is priority: list the exhaust last). A part cooler than the skin cools it (handled, unlike `max`).
5. Continue exactly as 4A with `T` in place of `T_class + offset`: the class's emissivity, `k_fast` and
   solar fast term, sky reflection, path extinction.

HLSL lives in `CamSimThermalCommon.ush` (`EntityPartTemp`), mirrored by
`CamSimThermalRef::EntityPartTemp` (same expressions, same order). Records are validated on the CPU:
a non-finite value drops the part (or, for the transform or skin, the whole record → invalid: the
4A stencil-table path) with a once-per-entity warning; count is clamped to 4.

## 4. Configuration

```yaml
thermal:
  entity:                        # ROADMAP 4C
    enabled: true                # false: 4A bit for bit (CAMSIM_THERMAL_ENTITY_ENABLED)
    moving_mps: 0.5
    idle_hold_s: 120
    skin_running_k: 4
    convection_v0_mps: 10
    skin_tau_s: 600
    burn_k: 700
    burn_s: 300
    hull_cool_tau_s: 1800
    engine:       { delta_k: 45,     tau_up_s: 300, tau_down_s: 900 }
    exhaust:      { temp_k: 450,     tau_up_s: 20,  tau_down_s: 60 }
    running_gear: { k_per_mps: 1.5, max_k: 30, tau_up_s: 180, tau_down_s: 600 }

entity_types:
  "2001":                        # Ural-4320
    thermal_parts:               # body frame: X forward, Y right, Z down, metres from the entity origin
      - { kind: running_gear, shape: box, centre_m: [0.47, 0, -0.62], half_m: [3.7, 1.55, 0.62], falloff_m: 0.15 }
      - { kind: engine,  shape: box, centre_m: [3.45, 0, -1.45], half_m: [0.75, 0.75, 0.45], falloff_m: 0.3 }
      - { kind: exhaust, shape: box, centre_m: [1.9, 1.25, -0.75], half_m: [0.5, 0.2, 0.2], falloff_m: 0.15 }
  "3001":                        # Mako 655 outboard
    thermal_parts:
      - { kind: engine, shape: ellipsoid, centre_m: [-3.3, 0, -0.75], half_m: [0.4, 0.35, 0.5], falloff_m: 0.2, delta_k: 25 }
```

- A part may override its kind's temperature parameter (`delta_k`, `temp_k`, `k_per_mps`, `max_k`); time
  constants are per kind only.
- Ural/Mako part positions are first estimates from the glTF bounds (Ural: origin on the ground, 3.27 m
  tall, X −3.31…+4.26 m; Mako: outboard at the stern). As built they ship unchanged: every acceptance gate
  passes with them; the visual review decides whether any volume needs moving.
- Validation (warn and skip the part): unknown kind/shape, non-finite or missing vectors, half-extent or
  falloff ≤ 0 (clamped to 0.01 m if merely tiny), more than 4 parts (extras dropped). Range checks on
  the `thermal.entity` scalars (warn, keep default): temperatures within the LUT, time constants > 0.
- `thermal_offset_k`: an additive offset on the skin target; its implicit +8 K default for surface
  vehicles applies only with `thermal.entity.enabled: false`.
- All keys documented in `docs/configuration.md`; env override only for `enabled`.

## 5. Testing

**Unit (NullRHI, `CamSim.Thermal.Entity.*`):**
- lag step against the exact exponential; rise vs fall constants; spawn at target; clock jump and
  negative `dt` snap; `dt = 0` freezes;
- targets: engine-off table, running table, convection factor, `skin_running_k`, part overrides;
- running logic: commanded on, moving, idle hold expiry, explicit off while moving, destroyed never runs;
- burn timeline: burn_k for burn_s, then hull cooling constant; flaming bit on/off; leaving destroyed;
- speed tracker: EMA, teleport reset, first tick, `dt ≤ 0`;
- config: parse, defaults, every validation path, overrides, `enabled: false`;
- DIS appearance decode → component commands (only on change, first-PDU rule, domains);
- CIGI CompId 11/12 → commanded inputs; CompId 10 = 2 starts the burn;
- builder: record packing; a known body point maps through the transform (rotated, translated, far
  from the origin: camera-relative precision ≤ 1 mm at 100 km); disabled → records equal 4A's
  stencil class/offset;
- `CamSimThermalRef::EntityPartTemp`: box/ellipsoid inside/shell/outside, overlap order, cooler part,
  zero parts = skin.

**GPU (`CamSim.GPU.Thermal.EntityPartsMatchesCpu`):** synthetic depth/stencil scene with records
covering both shapes, overlap, a cooler part and a rotated body; GPU vs CPU ≤ 1e-4 relative radiance.
The existing `MatchesCpu` / `LandCoverMatchesCpu` keep passing.

**Acceptance (`scripts/thermal_check.py`, new gates n–q; `m` was already an info row):** `send_dis_test.py` gains appearance
control and two modes: `truck-park` (drive, then stop with power plant off) and a parked-cold truck
(spawned stationary, engine off). Night MWIR and LWIR:

| Gate | check |
|---|---|
| n | running truck (box mean − ring) − parked-cold truck (box mean − ring) ≥ +5 DN (ring-referenced: the runs differ in AGC state) |
| o | running truck box p99 − box median ≥ +20 DN MWIR, +10 DN LWIR |
| p | after parking: box mean − ring at +600 s < at +5 s, and > parked-cold |
| q | destroyed truck (damage 3) box mean − ring ≥ +60 DN MWIR |

Gates a–l keep passing (b now with the running truck's parts). Visual review of the shot set
(running, parked, cooling, burning; MWIR/LWIR; part overlays from the configured volumes) closes 4C.

## Performance budget

- `ThermalCS`: ≤ 4 part tests on entity pixels only; expected < +0.02 ms at 1080p; gate f (≤ 0.5 ms) holds.
- Builder: 256 records, 32 live entities: target < 20 µs per frame (`CamSim.Thermal.Builder.PerFrameCost`).
- Entity manager: one `Step` per entity per tick, negligible.

## Risks

- **Coarse volumes heat the wrong surface** (a box intersecting the cab heats the cab). Mitigation:
  tuning against shots with part overlays; falloff shells; config order.
- **Speed noise** from dead reckoning corrections or the surface clamp making a parked truck "move".
  Mitigation: EMA, `moving_mps` threshold, teleport reset; unit test with a jittering parked pose.
- **DIS senders with odd appearance bits** (e.g. damage bits set by a different appearance layout).
  Only platform kinds are decoded; the bit layout is the shared one.
- **Records buffer per frame**: one 64 KB upload; if it shows up in profiles, upload only live records.
