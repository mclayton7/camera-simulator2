# DIS-Driven Trucks and Boats — Design

**Date:** 2026-09-28
**Status:** Approved in conversation; awaiting written-spec review
**Scope:** sub-project 1 of "boats and trucks for ATR". Sub-project 2 (tight oriented boxes,
occlusion) and the later items (ocean surface and wakes, DR acceleration, PDU timestamps, DIS
articulated parts) are out of scope.

## Intent

CamSim provides synthetic video to an ATR; it does not build the ATR. The ATR will be used both
for live tracking of the stream and for training/scoring from the ground-truth sidecar files.

Success: a scripted truck and boat, driven by DIS Entity State PDUs, appear in the video at the
right place and orientation, sitting on the terrain / water, and the COCO ground truth labels them
`truck` / `boat` with IDs that are stable and unique for each vehicle's lifetime.

What the user said: open-licensed models are fine; there is no DIS sender yet; one truck and one
boat model (two classes) is enough for now; the test sender should be a scripted-path script.

Assumptions: trucks drive on Cesium terrain anywhere; boats are on open water or lakes; a visible
ocean surface and wakes can wait (they need editor-authored assets).

## Current state (verified 2026-09-28)

- `FDisReceiver` parses Entity State (type 1) and Designator (24) PDUs; `FDisEntityAdapter` maps
  DIS types (exact 7-field key, then fuzzy `kind:domain:category`, then
  `dis.default_entity_type_id` = the F-16) and converts ECEF / Euler / DR 2–9
  (`Hosts/DisCommands.cpp`).
- Models load from `entities/` via glTFRuntime, synchronously on first spawn;
  `FEntityTypeTable`'s mesh cache holds weak pointers. The only model is the F-16.
- `FEntityCommand::bClampToTerrain` is set by the CIGI conformal clamp and read by nothing. DIS
  altitude is used as sent.
- Ground truth: `SnapshotGroundTruthEntities()` overwrites the collector's single
  `PendingEntities` at capture; the background task for that frame reads it later, unsynchronised,
  while up to three frames are in flight. `EntityId = Key.Id & 0xFFFF` collides across sources.
- No script sends DIS.

## Architecture

```
send_dis_test.py* ──ESPDU/UDP 3000──▶ FDisReceiver ─▶ FDisEntityAdapter ─▶ FEntityCommand (+ SurfaceMode*)
                                                                   │
ACamSimEntity::Tick: dead reckoning ─▶ surface clamp* (terrain traces) ─▶ GlobeAnchor
                                                                   │
Capture: per-slot entity snapshot* ─▶ readback ring ─▶ COCO / VOC (stable IDs*)
```
(\* = new)

## 1. DIS test sender — `scripts/send_dis_test.py`

- Standalone Python 3.10+, standard library only, style of `send_cigi_test.py`.
- Sends IEEE 1278.1 Entity State PDUs: protocol version 7, 144 bytes (no articulation
  parameters), exercise 1, site:application 1:1, entity numbers from 1. Unicast or multicast
  (`--addr`, default `127.0.0.1`; `--port`, default 3000; `--exercise`, default 1).
- Presets, each a list of waypoints (lat, lon), a speed, and a DIS entity type:
  - `truck-loop`: truck, 15 m/s, a closed loop over the Presidio hills (slopes to exercise the
    tilt), centre ≈ 37.795 N, −122.460 E.
  - `boat-circle`: boat, 8 m/s, circle of ≈ 400 m radius in the bay, centre ≈ 37.815 N,
    −122.440 E.
  - `both` (default): the two together.
  - `--location LAT,LON` re-centres the selected preset(s), keeping their shape.
- Motion follows the waypoints (great-circle legs on a sphere is fine at these scales). Heading is
  the leg bearing; turns at waypoints are rounded with a constant-rate turn so heading changes
  smoothly.
- Heartbeat 5 Hz, plus an immediate PDU when heading has changed by more than 3° since the last
  one sent. DR algorithm 4 (world velocity + body angular rate); velocity in ECEF, yaw rate in the
  angular-velocity field.
- Altitude: 0 m ellipsoid height (CamSim places the vehicle). Orientation: psi/theta/phi from the
  local heading with zero pitch/roll (CamSim's clamp supplies terrain pitch/roll).
- Entity types (SISO-REF-010; confirm the surface category value while implementing):
  - Truck: `1:1:225:7:0:0:0` (land platform, USA, large wheeled utility vehicle).
  - Boat: `1:3:225:7:0:0:0` (surface platform, USA, light/patrol craft).
- PDU encoding is one function with no socket side effects, so a pytest test can check it.
- `--duration`, `--rate` and `--verbose` flags; Ctrl-C exits cleanly.

## 2. Surface placement

### Mode

`FEntityCommand` gets `ESurfaceMode SurfaceMode { None, Ground, Water }`, replacing the unused
`bClampToTerrain`.

- DIS: domain 1 (land) → `Ground`, domain 3 (surface) → `Water`, anything else → `None`.
  Config `dis.clamp_to_surface` (bool, default `true`, env `CAMSIM_DIS_CLAMP_TO_SURFACE`); `false`
  forces `None` for DIS senders that supply true terrain heights.
- CIGI: the conformal clamp that set `bClampToTerrain` now sets `Ground`.
- The entity keeps its current mode; each command updates it.

### Where

In `ACamSimEntity::Tick`, after `UpdateDeadReckoning` (and after a command's `ApplyGeoPose`), so it
covers both PDU updates and DR frames. The camera ticks later (`TG_PostUpdateWork`), so the capture
sees the clamped pose. Attached entities are not clamped (they follow their parent).

The clamp math is a pure function (inputs: mode, sender pose, hit heights, previous clamp state,
Dt; output: pose + new state) so it can be unit-tested without a world.

### Ground (trucks)

- Four traces straight down along local up at bow, stern, port and starboard of the footprint:
  ± `half_length_m` along the heading, ± `half_beam_m` across it, from the entity type (fallback:
  the loaded mesh's bounds, as `ApplyVesselMotion` does).
- Traces hit Cesium tiles only (hits on non-tileset actors are ignored; entities never clamp onto
  each other).
- Height = mean of the hits' heights (ellipsoid heights via the georeference). Pitch =
  `atan((bow − stern) / (2·half_length))`, roll = `atan((stbd − port) / (2·half_beam))`. The
  sender's heading is kept.
- Trace span: the first clamp traces from 9 000 m to −500 m ellipsoid height. Afterwards from
  last ground height + 50 m down to last ground height − 500 m, so a truck under an overpass does
  not jump onto the bridge.
- 1–3 hits: height from the mean of the hits, pitch/roll held at their last values. 0 hits: hold
  the last clamped height and tilt. Before the first hit ever: the sender's pose.

### Water (boats)

- One trace at the centre with the same span rules; Cesium terrain renders sea and lakes as a
  surface, so the boat sits on the rendered water at its height.
- No hit: EGM96 sea level at the position (`Geospatial/Geoid.h`, ellipsoid height = undulation).
- Pitch/roll: the sender's (no waves). Vessel wave motion (parked ocean) is unchanged and remains
  off by default.

### Smoothing

A height change under 5 m eases with a 0.2 s time constant (tile refinement shifts the surface);
5 m or more snaps (first hit, teleport). Pitch/roll ease with the same constant.

### Guards and cost

- `create_physics_meshes: false` → traces cannot hit; log one warning, then Ground falls back to
  the sender's pose and Water to EGM96 sea level.
- Cost: 4 traces per Ground entity, 1 per Water entity, per frame.

## 3. Models, mapping, preload

### Models

- One truck and one boat, CC0 or CC-BY, `.glb`, realistic look (not stylised low-poly), ≤ ~100 k
  triangles and ≤ ~20 MB each.
- `entities/truck/<name>.glb` and `entities/boat/<name>.glb`, each with a `LICENSE.md` (source
  URL, author, licence, attribution text). Git LFS, as the F-16.
- Sourcing order: direct-download CC0/CC-BY sources first; if none is good enough, give the user a
  shortlist of Sketchfab links to download; last resort the Khronos `CesiumMilkTruck` (CC-BY 4.0).

### Config

- `entity_types`: `"2001"` truck and `"3001"` boat, each with `mesh`, `scale`, `rotation`,
  `class_name` (`truck` / `boat`), `half_length_m`, `half_beam_m` (measured from the model).
- `dis.entity_type_map`: the sender's two exact types → 2001 / 3001.
- `FDisEntityAdapter` fuzzy matching gains a `kind:domain` level after `kind:domain:category`,
  derived from the exact mappings the same way (generic entries win), so any land platform maps to
  the truck and any surface platform to the boat. Order: exact → `kind:domain:category` →
  `kind:domain` → default.

### Orientation and scale

glTFRuntime converts glTF's Y-up metres; the per-model `rotation` offset fixes the remaining facing.
Verified by the GPU test in §5.

### Preload

At startup (subsystem init, before the stream starts), load every `entity_types` entry whose mesh
is glTF and keep a strong reference for the session (the table's cache stays as is but is filled
from the preload). Log each model's load time. `/Game/` entries keep loading asynchronously. A
missing file is still skipped with a warning at config load.

## 4. Ground-truth fixes

### Per-frame snapshot

- Each readback-ring slot (`FSlot`) carries `TArray<FEntityAnnotationData> Entities` alongside its
  `Telemetry`. `Capture()` fills the slot's array; the background task moves it into its lambda.
- `FGroundTruthCollector::WriteAnnotationFrame(const TArray<FEntityAnnotationData>& Entities,
  const FCamSimTelemetry&, uint64 FrameIdx)`. `SetPendingEntitySnapshot`, `PendingEntities` and
  the pending width/height are deleted.

### IDs

- `FEntityAnnotationData::EntityId` becomes `uint32`, assigned by `FCamSimEntityManager` at spawn
  from a session counter starting at 1 that is never reused. It stays constant for the entity's
  lifetime.
- New fields `Source` (`dis` / `cigi` / `scenario`) and `SourceId` (`site:app:entity` for DIS, the
  entity ID for CIGI and scenario entities), written to COCO and VOC.
- `entity_id` in COCO widens to 32 bits; the new fields are additive. Update the readers found in
  `scripts/` and `docs/` (`entity-rendering.md`, `configuration.md`, and any COCO consumer).

### Unchanged

World-aligned boxes, no occlusion, pinhole projection (sub-project 2 and ROADMAP 3B.3).

## 5. Testing and acceptance

### Automation (NullRHI, `CamSim.*`)

- `Dis.SurfaceMode`: land → Ground, surface → Water, air → None; `clamp_to_surface: false` →
  None; CIGI conformal clamp → Ground.
- `Entity.SurfaceClamp` (pure function): four hits → height, pitch, roll; partial hits keep tilt;
  no hits hold; before first hit → sender pose; < 5 m eases, ≥ 5 m snaps; Water no hit → EGM96.
- `Dis.TypeMapFallback`: an unmapped land / surface type → truck / boat via `kind:domain`; exact
  and `kind:domain:category` still take precedence.
- `EntityTypes.Preload`: glTF entries are loaded at startup and survive a garbage collection; a
  missing file is skipped.
- `GroundTruth.PerFrameSnapshot`: two frames in flight written out of order each get their own
  entities; IDs are unique across a DIS and a CIGI key with the same low 16 bits and are not
  reused after removal; COCO output contains `source` / `source_id`.
- `Config`: `dis.clamp_to_surface` default, YAML and env override.

### GPU (Metal, `CamSim.GPU.*`)

Spawn the truck and the boat at known headings; the projected box is longest along the travel
direction and its size matches `half_length_m` / `half_beam_m` within 10%.

### Python (pytest)

`send_dis_test.py`'s PDU encoding matches the C++ parser's offsets for known values (ECEF position,
psi/theta/phi, velocity, entity type, DR algorithm, length 144); presets produce positions within
the expected radius of their centre and headings along the path.

### End-to-end acceptance (manual, recorded in ROADMAP)

Launch headless with `ml_training.enabled: true`; run `send_dis_test.py both`; point the camera at
each vehicle via CIGI; capture shots. Pass when both models are visible and face their direction of
travel, the truck sits on the terrain (no visible float or sink; pitch follows the slope), the boat
sits on the water, COCO has `truck` / `boat` boxes with stable IDs across frames, and the stream
holds 30 fps with no hitch when the vehicles appear.

## Documentation

- `docs/configuration.md`: `dis.clamp_to_surface`, the `kind:domain` fuzzy level, the new
  `entity_types`.
- A DIS section in the docs: the sender, presets, and how to map DIS types to models.
- `ROADMAP.md`: this work as its own item; carry-overs for tight/oriented boxes and occlusion, the
  ocean surface and wakes, DR acceleration and PDU timestamps, DIS articulated parts.
- `CLAUDE.md`: `scripts/send_dis_test.py` in the command table.

## Risks

- Cesium World Terrain may not render inland water as a flat surface everywhere; the EGM96
  fallback covers the sea but not lakes. Acceptable for this scope.
- The model search may not find a realistic free boat with a direct download; the user may need to
  download one.
- The surface category value for the boat's DIS type needs checking against SISO-REF-010.
