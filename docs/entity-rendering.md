# Entity Rendering (Phase 8)

CamSim can render arbitrary 3D scene objects — aircraft, vehicles, ground
equipment — driven by CIGI 3.3 packets. The system handles the full entity
lifecycle: spawn, update, dead-reckoning, articulated part animation, component
control (lights, damage), and removal.

## Contents

- [CIGI packets handled](#cigi-packets-handled)
- [Entity routing](#entity-routing)
- [Entity lifecycle](#entity-lifecycle)
- [Configuration](#configuration)
- [Phase C scale and scenario controls](#phase-c-scale-and-scenario-controls)
- [Dead-reckoning](#dead-reckoning)
- [Articulated parts](#articulated-parts)
- [Component control](#component-control)
- [Testing](#testing)
- [Log messages](#log-messages)

---

## CIGI Packets Handled

| Opcode | CCL class | Size | Purpose |
|--------|-----------|------|---------|
| 2 | `CigiEntityCtrlV3` | 48 B | Spawn / position / remove an entity |
| 8 | `CigiRateCtrlV3` | 32 B | Set linear and angular velocity for dead-reckoning |
| 6 | `CigiArtPartCtrlV3` | 32 B | Move a bone on a skeletal mesh |
| 4 | `CigiCompCtrlV3` | 32 B | Toggle lights or change damage state |

---

## Entity Routing

A single CIGI host datagram may contain Entity Control packets for both the
camera and scene entities. These must go to different consumers on the game
thread. CamSim routes them at the producer (receiver thread) side using two
SPSC queues:

```
FEntityCtrlProcessor::OnPacketReceived()
        │
        ├─ EntityId == camera_entity_id ──► CameraEntityQueue ──► ACamSimCamera
        │
        └─ all other EntityIds          ──► EntityStateQueue  ──► FCamSimEntityManager
```

`camera_entity_id` is read from `camsim_config.json` (default `0`). Set it to
match the entity ID your host uses for the camera:

```json
"camera_entity_id": 0
```

The three other queues (Rate Control, Art Part, Component Control) are consumed
exclusively by `FCamSimEntityManager` — the camera does not receive them.

---

## Entity Lifecycle

The CIGI `EntityState` field in the Entity Control packet (byte 5, bits 0–1)
drives the state machine. CCL enum values:

| Value | Name | Action |
|-------|------|--------|
| `0` | Standby | Hide actor (`SetActorHiddenInGame(true)`) — actor stays in memory |
| `1` | Active | Spawn if new, update pose if existing |
| `2` | Remove | Destroy actor and remove from entity map |

On every tick `FCamSimEntityManager`:

1. **Drains** the entity state queue into a per-frame map (last packet per entity
   ID wins — the host may send multiple updates in one datagram).
2. **Processes** each entry: spawn, update pose, hide, or destroy.
3. **Purges** stale entries where the actor has been externally invalidated.
4. **Drains** rate control, art part, and component control queues, forwarding
   each packet to the matching entity by ID.
5. **Applies** optional scenario-orchestrated entity states from `scenario.entities`.

### Spawn

When an Active packet arrives for an entity ID not in the map, CamSim:

1. Calls `World->SpawnActor<ACamSimEntity>()`.
2. Sets `EntityId` and calls `SetEntityType()` to load the mesh.
3. Calls `ApplyPose()` to position the actor.
4. Inserts the actor into `EntityMap`.

Mesh loading is synchronous (`FSoftObjectPath::TryLoad()`). If the asset path is
incorrect or the asset is not in the cooked content, a warning is logged and the
entity remains in the map but invisible.

### Update

When an Active packet arrives for an existing entity:

- If `EntityType` changed, `SetEntityType()` is called to swap the mesh.
- `ApplyPose()` snaps position and orientation from the packet and resets the
  dead-reckoning base state.

### Remove

The actor is destroyed with `Destroy()` and the entry is removed from the map.
Send a Remove packet for any entity that is no longer needed — leaking actors
wastes GPU memory and draw calls.

---

## Configuration

Entity types are configured in `camsim_config.json`:

```json
"entity_types": {
  "1001": {
    "mesh":           "/Game/Models/F16/F16.F16",
    "mesh_damaged":   "/Game/Models/F16/F16_Damaged.F16_Damaged",
    "mesh_destroyed": "/Game/Models/F16/F16_Destroyed.F16_Destroyed",
    "skeletal":       true
  },
  "2001": {
    "mesh":     "/Game/Models/Truck/Truck.Truck",
    "skeletal": false
  }
}
```

Keys are CIGI EntityType values as strings. The type table is loaded once at
startup inside `FCamSimConfig::Load()`. Adding a type without a corresponding
cooked asset now fails preflight validation and that specific type is skipped.

**Asset paths** follow UE's content browser path format:
`"/Game/Path/To/Asset.AssetName"`. The asset must be included in the packaged
build's cooked content.

**`skeletal: true`** loads a `USkeletalMesh` and places it in
`UPoseableMeshComponent`, enabling per-bone transforms for articulated part
control. `skeletal: false` (default) loads a `UStaticMesh`.

Both components are always created; only one is visible at a time based on the
type's `skeletal` flag.

### Startup preflight validation

On startup, `EntityTypeTable` validates each `entity_types` entry:

- Type key range (`1..65535`)
- Primary `mesh` path format and existence
- `/Game/...` mesh type compatibility (`skeletal` vs `static`)
- Optional damage variants (`mesh_damaged`, `mesh_destroyed`)

Invalid primary entries are skipped; invalid damage variants are cleared with
warnings.

---

## Phase C Scale and Scenario Controls

### Runtime scale controls (`entity_scale`)

`entity_scale` config reduces cost in dense scenes:

- `max_draw_distance_m`: applies distance culling to mesh and light components
- `tick_rate_hz`: sets per-entity actor tick interval
- `default_max_update_rate_hz`: global pose apply-rate cap
- `max_update_rate_hz_overrides`: per-entity pose-rate caps

These controls are applied when entities spawn, and pose throttling is enforced
in `FCamSimEntityManager::ApplyEntityState()`.

### Scenario orchestration (`scenario`)

When `scenario.enabled=true`, `FCamSimEntityManager` synthesizes entity states
from `scenario.entities` using deterministic monotonic time
(`FPlatformTime::Seconds`) with `scenario.time_scale`.

Per scripted entity:

- spawn at `spawn_time_sec`
- update at `update_rate_hz` (or every tick if `0`)
- move/rotate using configured linear and angular rates
- despawn after `despawn_time_sec` (if greater than spawn time)

Scenario entities coexist with live CIGI traffic and are processed each manager
tick.

---

## Dead-Reckoning

When a Rate Control packet is received for an entity, `ACamSimEntity` stores the
velocity and applies it each game tick to advance the entity's position between
CIGI updates.

### Algorithm

```
NewYaw   = Yaw   + YawRate   × dt
NewPitch = Pitch + PitchRate × dt
NewRoll  = Roll  + RollRate  × dt

// Body-frame → world (NED): X=forward(North), Y=right(East), Z=down
Q = quaternion(Pitch, Yaw, Roll)
WorldVel = Q.RotateVector(XRate, YRate, ZRate)

NewLat = Lat + (WorldVel.X × dt) / 111320
NewLon = Lon + (WorldVel.Y × dt) / (111320 × cos(Lat × π/180))
NewAlt = Alt - WorldVel.Z × dt          // CIGI Z=down; Alt increases upward
```

`ApplyPose()` (called on every incoming Entity Control packet) always resets
the dead-reckoning base to the packet's values. This prevents accumulation of
linearisation error.

### Rate Control Packet (opcode 8)

| Field | CIGI field | Units |
|-------|------------|-------|
| `XRate` | X Rate | m/s body-frame forward |
| `YRate` | Y Rate | m/s body-frame right |
| `ZRate` | Z Rate | m/s body-frame down |
| `YawRate` | Yaw Rate | deg/s |
| `PitchRate` | Pitch Rate | deg/s |
| `RollRate` | Roll Rate | deg/s |
| `ArtPartId` | Art Part ID | — (only used when `ApplyToArtPart` is set) |
| `bApplyToArtPart` | Apply to Art Part | When true, rates apply to an art part, not the entity body — currently a no-op in CamSim |

Dead-reckoning is active as long as `bHasRate` is true (set by the first Rate
Control packet and never cleared). To stop DR, send a Rate Control packet with
all rates = 0.

---

## Articulated Parts

Skeletal mesh bones are controlled by CIGI Articulated Part Control packets
(opcode 6).

### Bone naming convention

Bones must be named `ArtPart_XX` where `XX` is the zero-padded decimal CIGI
Art Part ID:

| CIGI Art Part ID | Bone name | Suggested use |
|------------------|-----------|---------------|
| 0 | `ArtPart_00` | Landing gear (extend/retract via XOff) |
| 1 | `ArtPart_01` | Left aileron (Roll) |
| 2 | `ArtPart_02` | Right aileron (Roll) |
| 3 | `ArtPart_03` | Elevator (Pitch) |
| 4 | `ArtPart_04` | Rudder (Yaw) |

Any other Art Part ID can be used — CamSim performs a bone lookup by name and
silently ignores IDs with no matching bone.

### Enable flags

Each DOF has an independent enable flag. Only enabled DOFs are written; the
others retain their current bone transform:

| Flag | DOF |
|------|-----|
| `bArtPartEn` | Master enable — packet is ignored if false |
| `bXOffEn` | X offset (metres) |
| `bYOffEn` | Y offset (metres) |
| `bZOffEn` | Z offset (metres) |
| `bRollEn` | Roll (degrees) |
| `bPitchEn` | Pitch (degrees) |
| `bYawEn` | Yaw (degrees) |

Transforms are applied in `UPoseableMeshComponent` component space.

---

## Component Control

Component Control packets (opcode 4) address subsystems of an entity.
`CigiCompCtrlV3` identifies the target entity via `InstanceID` (the entity ID).

CamSim handles `CompClass = 0` (entity class) only. `CompId` selects the
subsystem:

| CompId | Subsystem | CompState values |
|--------|-----------|-----------------|
| 0 | Navigation lights (red/green/white point lights) | `0` = off, `1` = on |
| 1 | Anti-collision strobe | `0` = off, `1` = on (1 Hz, 50% duty cycle) |
| 2 | Landing light | `0` = off, `1` = on |
| 10 | Damage state | `0` = intact, `1` = damaged, `2` = destroyed |

### Navigation lights

Three `UPointLightComponent` objects (red, green, white) are created on every
`ACamSimEntity` and hidden by default. CompId=0, CompState=1 makes all three
visible simultaneously.

### Strobe

CompId=1, CompState=1 enables a 1 Hz anti-collision strobe with 50% duty cycle
driven in `ACamSimEntity::Tick()` using an accumulator:

```
StrobeAccum += DeltaTime
if StrobeAccum >= 1.0: StrobeAccum -= 1.0
StrobeLight.SetVisible(StrobeAccum < 0.5)
```

### Landing light

CompId=2 toggles a dedicated white landing light component. This is an immediate
on/off control (no blink pattern) and is independent of nav/strobe states.

### Damage state

CompId=10 triggers an asset swap on the mesh component. The `mesh_damaged` and
`mesh_destroyed` paths from `entity_types` are used for states 1 and 2
respectively. State 0 reloads the primary `mesh`. If the damage-variant path is
empty in config, the swap is silently skipped.

---

## Testing

The `scripts/test_entity_rendering.py` script covers all Phase 8 scenarios.
Phase C adds stress and deterministic replay tooling:

- `scripts/stress_entity_rendering.py`
- `scripts/capture_cigi_stream.py`
- `scripts/replay_cigi_stream.py`

### Prerequisites

- CamSim is running with `camera_entity_id: 0` in config.
- `entity_types` block maps type `1001` to an asset (or any type you use).
- For art-part tests: the skeletal mesh has bones named `ArtPart_00`, etc.

### 8A — Spawn and remove

```bash
# Spawn entity 1 (type 1001) for 20 seconds
python3 scripts/test_entity_rendering.py spawn \
    --entity-id 1 --entity-type 1001 \
    --lat 37.6213 --lon -122.379 --alt 1000 \
    --duration 20

# Verify log: "EntityManager: spawned entity 1 (type 1001)"

# Remove it
python3 scripts/test_entity_rendering.py remove --entity-id 1
# Verify log: "EntityManager: removed entity 1"
```

### 8B — Mesh loading

```bash
python3 scripts/test_entity_rendering.py spawn \
    --entity-type 1001 --lat 37.6213 --lon -122.379 --alt 800
```

The entity should appear in the video at the commanded position. If the asset
path is wrong, the entity is invisible and the log shows:
`ACamSimEntity[1]: failed to load skeletal mesh '/Game/...'`

### 8C — Dead-reckoning

```bash
python3 scripts/test_entity_rendering.py deadreckon \
    --entity-type 1001 --duration 30
```

The script sends entity state + rate control for 2 seconds, then stops sending
entity updates. The entity should continue moving north at ~200 m/s. The script
prints the expected latitude offset every 5 seconds for comparison with the
video.

### 8D — Articulated parts

```bash
python3 scripts/test_entity_rendering.py artpart \
    --entity-type 1001 --duration 30
```

`ArtPart_00` (landing gear) cycles: extended (XOff = −2.0) for 5 s, retracted
(XOff = 0.0) for 5 s. No-op if the mesh has no `ArtPart_00` bone.

### 8E — Lights and strobe

```bash
python3 scripts/test_entity_rendering.py lights \
    --entity-type 1001 --duration 20
```

Cycles through four phases every 3 seconds:
1. Both off
2. Nav lights on
3. Strobe on
4. Both on

### 8F — Damage state

```bash
python3 scripts/test_entity_rendering.py damage \
    --entity-type 1001 --duration 30
```

Cycles intact → damaged → destroyed every 5 seconds. Requires `mesh_damaged`
and `mesh_destroyed` paths in `entity_types` config. Check log for the state
transitions.

### Phase C1/C2/C3 — Stress and deterministic replay

```bash
# 1) Stress entity throughput (spawn+update many entities)
python3 scripts/stress_entity_rendering.py \
    --host 127.0.0.1 --port 8888 \
    --count 200 --entity-type 1001 \
    --pattern ring --rate 10 --duration 60 --remove-on-exit

# 2) Capture a real CIGI stream for deterministic replay
python3 scripts/capture_cigi_stream.py \
    --bind-host 0.0.0.0 --bind-port 8888 \
    --output /tmp/cigi_capture.jsonl --duration 30

# 3) Replay the capture at 1x (or adjust --speed, --loops)
python3 scripts/replay_cigi_stream.py \
    --input /tmp/cigi_capture.jsonl --host 127.0.0.1 --port 8888 --speed 1.0 --loops 1
```

### Regression — camera unaffected

Run the camera test simultaneously to confirm entity routing does not interfere
with the camera:

```bash
# Terminal 1 — camera tour (entity id=0, the camera)
python3 scripts/send_cigi_test.py --entity-id 0 --tour &

# Terminal 2 — scene entity (entity id=1)
python3 scripts/test_entity_rendering.py spawn --entity-id 1 --duration 60
```

KLV metadata should continue updating correctly (`validate_klv.py`) and there
should be no frame drops.

---

## Log Messages

| Message | Meaning |
|---------|---------|
| `FCigiReceiver: listening on ... (camera entity id=N)` | Routing configured correctly |
| `EntityTypeTable: loaded N entries (M skipped by preflight)` | `entity_types` config parsed with startup validation |
| `EntityTypeTable: type 1001 -> /Game/... (skeletal)` | One type entry loaded |
| `EntityManager: spawned entity N (type T)` | Active packet, new entity |
| `EntityManager: removed entity N` | Remove packet received |
| `ACamSimEntity[N]: no asset for type T` | Type not in `entity_types` config |
| `ACamSimEntity[N]: failed to load skeletal mesh '...'` | Asset path wrong or not cooked |
| `ACamSimEntity[N]: failed to load static mesh '...'` | As above, static variant |
