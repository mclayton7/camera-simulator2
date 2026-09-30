# DIS Input (IEEE 1278.1)

CamSim listens for DIS Entity State PDUs as a passive "stealth viewer": every entity it
hears is rendered with a model chosen from its DIS entity type, placed on the terrain or
the water, dead-reckoned between PDUs, and labelled in the ML ground truth. CIGI stays the
primary interface (it drives the sensor platform and the view); DIS entities live in their
own ID namespace, so CIGI entity 1000 and DIS entity 1000 are different entities.

Design: [`docs/superpowers/specs/2026-09-28-dis-vehicles-design.md`](superpowers/specs/2026-09-28-dis-vehicles-design.md).

## Enabling DIS

The shipped `deploy/camsim_config.yaml` turns DIS on (`dis.enabled: true`, env
`CAMSIM_DIS_ENABLED`) and listens on UDP port 3000 (`dis.port`) for unicast and the
`239.1.2.3` multicast group (`dis.multicast_group`), exercise 1 (`dis.exercise_id`; `0`
accepts every exercise). Every `dis.*` key, its env var and default — heartbeat timeout,
default entity type, `clamp_to_surface`, the type map — is documented in one place:
[`configuration.md`](configuration.md#dis-input-ieee-12781).

Supported PDUs: Entity State (type 1) and Designator (type 24). Position is ECEF, orientation
the ECEF-referenced psi/theta/phi Euler angles, both converted to WGS-84 geodetic pose.

## Test sender

`scripts/send_dis_test.py` (Python 3.10+, standard library only) sends 144-byte Entity State
PDUs (protocol version 7, site:application 1:1, entity numbers from 1) for scripted vehicles:

| Preset | Entity type | Path | Speed |
|--------|-------------|------|-------|
| `truck-loop` | `1:1:225:7:0:0:0` (land, USA) marking `TRUCK1` | Closed 300 m (E–W) × 200 m (N–S) loop over the Presidio, centre 37.795 N, 122.460 W; driven clockwise seen from above | 15 m/s |
| `boat-circle` | `1:3:225:7:0:0:0` (surface, USA) marking `BOAT1` | 150 m radius circle in the bay, centre 37.815 N, 122.440 W; clockwise | 8 m/s |
| `both` (default) | both of the above | | |

Heading follows the path (turns are rounded at a constant rate). PDUs go out at a 5 Hz
heartbeat plus immediately when the heading has changed by more than 3°. Dead reckoning is
algorithm 4 (world velocity + body angular rate). Altitude is 0 m and pitch/roll are zero:
CamSim places the vehicles on the surface.

| Flag | Default | Meaning |
|------|---------|---------|
| `--addr` | `127.0.0.1` | Destination (unicast or multicast) |
| `--port` | `3000` | Destination port |
| `--exercise` | `1` | Exercise ID |
| `--location LAT,LON` | -- | Re-centre the selected preset(s), keeping their shape (with `both`, the second is ~220 m north of the first) |
| `--duration SEC` | `0` | Stop after SEC seconds (0 = until Ctrl-C) |
| `--rate HZ` | `5` | Heartbeat rate |
| `--verbose` | off | Print every PDU sent |

```bash
scripts/send_dis_test.py                       # truck + boat in San Francisco, until Ctrl-C
scripts/send_dis_test.py truck-loop --location 36.60,-121.90 --duration 120
scripts/send_dis_test.py both --addr 239.1.2.3  # multicast (dis.multicast_group)
```

`scripts/dis_vehicle_check.py OUTDIR` is the end-to-end check: it launches CamSim headless
(macOS, local build) with DIS and ground truth on, runs the sender, points the camera at each
vehicle over CIGI, saves nadir and oblique shots to `OUTDIR/shots/`, and passes when the COCO
output has exactly one stable `entity_id` each for `truck` and `boat`.

## Mapping DIS types to models

An incoming DIS entity type (`kind:domain:country:category:subcategory:specific:extra`) picks
a CamSim entity type in four steps:

1. **Exact** — all seven fields match a `dis.entity_type_map` key.
2. **`kind:domain:category`** — derived from the exact entries.
3. **`kind:domain`** — derived from the exact entries, so any land platform (domain 1) maps to
   the truck and any surface platform (domain 3) to the boat.
4. **`dis.default_entity_type_id`**.

At steps 2 and 3, when several entries collapse to the same key, the one with a generic (`0`)
country/subcategory wins, then the lowest CamSim type ID, so the result is deterministic.

```yaml
dis:
  entity_type_map:
    "1:2:225:2:0:0:0": 1001   # fixed-wing aircraft -> F-16
    "1:1:225:7:0:0:0": 2001   # truck
    "1:3:225:7:0:0:0": 3001   # boat
```

The values are keys of `entity_types`, which binds each CamSim type to a model:

| Field | Meaning |
|-------|---------|
| `mesh` | A glTF/GLB path under `entities/` (loaded by glTFRuntime) or a `/Game/...` asset path |
| `skeletal` | `true` for a skeletal mesh |
| `class_name` | ML ground-truth label (COCO `category.name`); `type_NNNN` if absent |
| `scale` | Uniform model scale (to bring a model to its real size) |
| `rotation` | `pitch` / `yaw` / `roll` offset (degrees) so the model's nose points along UE +X |
| `z_offset_m` | Vertical offset of the model from the entity origin (metres, + = up; default 0). The surface clamp puts the origin on the surface, so a hull needs a negative value to sit at its draft |
| `half_length_m` / `half_beam_m` | Half length / half width of the footprint after `scale`: the surface clamp's trace points (and vessel wave motion). Without them the loaded mesh's bounds are used. |

The shipped types are `2001` (Ural-4320 cargo truck, 7.57 × 3.06 m) and `3001` (Mako 655
rigid-hull inflatable, 6.54 × 2.63 m), both CC BY 4.0 (`entities/*/LICENSE.md`). glTF models
are loaded once at startup and kept resident, so the first PDU of a new vehicle does not
wait for a model load (`/Game/` assets still load asynchronously). Each preloaded model is
also drawn once, 50 km below the world, for the first 30 s after startup: the editor binary
CamSim runs compiles material shaders on first use (~130 ms on the game thread, with the
render thread waiting), which otherwise hitched the stream when the first vehicle appeared.

## Surface placement

With `dis.clamp_to_surface: true` (the default) the sender's altitude is ignored for land and
surface platforms (kind 1, domains 1 and 3): CamSim places them on the rendered surface at
every pose commit (each PDU and each dead-reckoned frame), using line traces against the
Cesium tiles (`Entity/SurfaceClamp.h`, `Entity/SurfaceProbe.h`). Other domains (air,
subsurface, space) and every other kind use the sender's pose — including munitions, whose
domain is the domain of their target, and life forms.

- **Ground (kind 1, domain 1)**: four traces straight down at bow, stern, port and starboard
  of the footprint (`half_length_m`, `half_beam_m`). Height is the mean of the hits; pitch and
  roll follow the slope. With 1–3 hits the height still follows and the tilt is held; with
  none (after the full-span retry below) the last clamp is held. The sender's heading is
  always kept.
- **Surface (kind 1, domain 3)**, with the ocean on (`ocean.enabled`, the default; ROADMAP
  2.6): the boat floats on the sea. One trace at the centre finds the Cesium surface; the base
  height is max(that hit, sea level), where sea level is the EGM96 geoid (`Geospatial/Geoid.h`)
  plus the CIGI Maritime Surface Height tide offset. Bathymetry (the water-masked seabed, ~23 m
  below sea level in San Francisco Bay) therefore loses to the sea, and a lake more than 2 m
  above the tide-free geoid wins (the boat sits on the lake, without waves; the tide is left
  out of this test because Cesium's surface doesn't move with it). On the sea, with
  `ocean.vessel_motion`, the wave surface (`FOceanWaves::HeightAt`) is sampled at bow, stern,
  port and starboard of the footprint: heave is their mean, pitch and roll their slopes (times
  `ocean.vessel_motion_scale`); the sender's pitch and roll are replaced, its heading kept.
  Before the first hit the base is sea level; a later miss holds max(last height, sea level).
  The COCO `geo.alt_m` of a boat is its waterline: sea level + wave height at the hull.
- **Surface, ocean off** (`CAMSIM_OCEAN_ENABLED=0`): as before 2.6 — the boat sits on the
  rendered Cesium surface (EGM96 sea level before the first hit, then the last water height on
  a miss), with the sender's pitch and roll. Over bathymetry that is the seabed, ~23 m below
  sea level at the `boat-circle` preset.
- The first trace spans 9 000 m to −500 m ellipsoid height; later ones start 50 m above the
  last ground height, so a vehicle under a bridge does not jump onto it. If every one of those
  traces misses (the ground rose more than 50 m, or the tiles are gone) they are retried once
  over the full 9 000 m to −500 m span, so a vehicle can't stay buried. The clamp starts over
  (full span, snap) when the entity's surface mode changes, it is attached or detached, or its
  sender position jumps more than 100 m. Height changes under 5 m ease in with a 0.2 s time
  constant (tile refinement); larger ones snap. A second commit in the same frame (a PDU after
  the dead-reckoned tick) does not bypass the ease.
- The traces need `create_physics_meshes: true` (the default). With it off, CamSim logs one
  warning at the first surface trace, ground vehicles use the sender's altitude and boats
  EGM96 sea level.

Set `dis.clamp_to_surface: false` for senders that supply true terrain heights (and
attitudes); every entity then uses its PDU pose.

CIGI HAT/HOT requests see the same sea: with the ocean on, the terrain height a HAT/HOT
answers is max(Cesium hit, sea surface including the waves at that point), so HOT at a boat
returns the water surface, not the seabed. A trace miss (tiles not loaded yet) is still
answered invalid: the water only raises a valid hit. Over land below sea level (Death Valley,
polders) HOT returns sea level (ROADMAP 2.6 carry-over). An extended response reports the
water's normal when the water wins (material code 0). LOS still ignores the water.

## Ground truth

With `ml_training.enabled: true` each COCO record (`camsim_coco.jsonl`, one line per
annotated frame) lists the visible entities. The entity snapshot is taken at capture and
travels with the frame through the readback ring, so labels match the frame even with three
frames in flight.

| Field | Meaning |
|-------|---------|
| `entity_id` | uint32, assigned at spawn from a session counter; stable for the entity's lifetime and never reused |
| `source` | `dis`, `cigi` or `scenario` |
| `source_id` | DIS: `site.application.entity` (e.g. `1.1.2`); CIGI / scenario: the entity ID |
| `category` | `{id: CamSim type ID, name: class_name}` — `truck` / `boat` for the shipped models |
| `bbox`, `area`, `truncated` | Screen-space box `[x, y, w, h]` in pixels, its area, and whether it is clipped by the frame edge |
| `geo` | `{lat, lon, alt_m}`: the entity origin's geodetic position at capture (WGS-84 degrees, ellipsoid metres); for a clamped vehicle that is its ground contact / waterline point |

VOC XML carries the same `entity_id`, `source` and `source_id` per object.

## Limitations

- No wakes, whitecaps or spray (ROADMAP 2.6 carry-overs). The boat's draft is fixed
  (`z_offset_m: -0.49`: the keel is ~0.14 m above the model origin, so the hull sits ~0.35 m
  deep) and does not change with speed or load. Inland water that Cesium terrain does not
  render flat is not handled, and harbour piers that Cesium World Terrain drapes below sea
  level are drawn under the sea (Fisherman's Wharf).
- Boat motion is exact only near the frame centre. The drawn sea fades the shorter waves
  where its mesh cells are coarse (away from the frame centre), but placement and HOT use
  the full wave sum, so a boat a few hundred metres off-centre can heave on waves the picture
  no longer shows (from 500 ft at Beaufort 6 the two shorter waves are gone 240 m out). The
  2.6 acceptance covers only a centred boat; fixes are in ROADMAP 2.6 carry-overs (share the
  mesh's centre/radius/warp with placement behind a flag, or a denser grid).
- Waves, boat motion and HOT run on the sim clock: freezing it (CIGI Celestial Sphere
  Control, Ephemeris Model Enable off) freezes the sea while dead-reckoned boats keep moving.
- Fast-moving vehicles can leave a faint TSR ghost trail behind them in close-ups.
- Boxes are loose, world-aligned projections of the model bounds, with no occlusion test
  (a vehicle behind a hill is still labelled).
- Dead reckoning uses velocity and angular rate (algorithms 2–9) but ignores acceleration, and
  PDU timestamps are unused (extrapolation runs from the time of arrival).
- DIS articulation parameters (turrets, guns) are not applied.
