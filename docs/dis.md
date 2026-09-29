# DIS Input (IEEE 1278.1)

CamSim listens for DIS Entity State PDUs as a passive "stealth viewer": every entity it
hears is rendered with a model chosen from its DIS entity type, placed on the terrain or
the water, dead-reckoned between PDUs, and labelled in the ML ground truth. CIGI stays the
primary interface (it drives the sensor platform and the view); DIS entities live in their
own ID namespace, so CIGI entity 1000 and DIS entity 1000 are different entities.

Design: [`docs/superpowers/specs/2026-09-28-dis-vehicles-design.md`](superpowers/specs/2026-09-28-dis-vehicles-design.md).

## Enabling DIS

| Key | Env var | Default (code) | Notes |
|-----|---------|----------------|-------|
| `dis.enabled` | `CAMSIM_DIS_ENABLED` | `false` | The shipped `deploy/camsim_config.yaml` turns it on. |
| `dis.bind_addr` | `CAMSIM_DIS_BIND_ADDR` | `0.0.0.0` | UDP bind address. |
| `dis.port` | `CAMSIM_DIS_PORT` | `3000` | The IEEE 1278.1 default port. |
| `dis.multicast_group` | `CAMSIM_DIS_MULTICAST_GROUP` | empty | Group to join; empty = unicast only. The shipped config uses `239.1.2.3`. Unicast to the port is received either way. |
| `dis.exercise_id` | `CAMSIM_DIS_EXERCISE_ID` | `1` | PDUs from other exercises are dropped; `0` accepts all. |
| `dis.site_id` / `dis.application_id` | `CAMSIM_DIS_SITE_ID` / `CAMSIM_DIS_APP_ID` | `1` / `1` | This IG's own DIS identifiers. |
| `dis.heartbeat_timeout_sec` | `CAMSIM_DIS_HEARTBEAT_TIMEOUT` | `12.0` | DIS has no "remove entity": an entity silent this long is removed. |
| `dis.default_entity_type_id` | `CAMSIM_DIS_DEFAULT_ENTITY_TYPE` | `1001` | Model for DIS types nothing else matches (the F-16). |
| `dis.clamp_to_surface` | `CAMSIM_DIS_CLAMP_TO_SURFACE` | `true` | See [Surface placement](#surface-placement). |
| `dis.entity_type_map` | -- | `{}` | See [Mapping DIS types to models](#mapping-dis-types-to-models). |

The full reference is in [`configuration.md`](configuration.md#dis-input-ieee-12781).

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
| `half_length_m` / `half_beam_m` | Half length / half width of the footprint after `scale`: the surface clamp's trace points (and vessel wave motion). Without them the loaded mesh's bounds are used. |

The shipped types are `2001` (Ural-4320 cargo truck, 7.57 × 3.06 m) and `3001` (Mako 655
rigid-hull inflatable, 6.54 × 2.63 m), both CC BY 4.0 (`entities/*/LICENSE.md`). glTF models
are loaded once at startup and kept resident, so the first PDU of a new vehicle does not
wait for a model load (`/Game/` assets still load asynchronously). One ~100 ms frame remains
when a vehicle first appears (ROADMAP 2.5).

## Surface placement

With `dis.clamp_to_surface: true` (the default) the sender's altitude is ignored for land and
surface platforms: CamSim places them on the rendered surface at every pose commit (each PDU
and each dead-reckoned frame), using line traces against the Cesium tiles
(`Entity/SurfaceClamp.h`, `Entity/SurfaceProbe.h`). Other domains (air, subsurface, space)
use the sender's pose.

- **Ground (domain 1)**: four traces straight down at bow, stern, port and starboard of the
  footprint (`half_length_m`, `half_beam_m`). Height is the mean of the hits; pitch and roll
  follow the slope. With 1–3 hits the height still follows and the tilt is held; with none the
  last clamp is held. The sender's heading is always kept.
- **Surface (domain 3)**: one trace at the centre: the boat sits on the rendered water surface.
  No hit: EGM96 sea level at the position (`Geospatial/Geoid.h`). Pitch and roll are the
  sender's. Where Cesium World Terrain carries bathymetry the rendered "water" is the
  water-masked seabed: in San Francisco Bay at the `boat-circle` preset it is about 23 m below
  sea level (KLV Tag 25), and the boat sits there, which looks right but puts its altitude
  below sea level.
- The first trace spans 9 000 m to −500 m ellipsoid height; later ones start 50 m above the
  last ground height, so a vehicle under a bridge does not jump onto it. Height changes under
  5 m ease in with a 0.2 s time constant (tile refinement); larger ones snap.
- The traces need `create_physics_meshes: true` (the default). With it off, CamSim logs one
  warning, ground vehicles use the sender's altitude and boats EGM96 sea level.

Set `dis.clamp_to_surface: false` for senders that supply true terrain heights (and
attitudes); every entity then uses its PDU pose.

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

VOC XML carries the same `entity_id`, `source` and `source_id` per object.

## Limitations

- No visible ocean surface, waves or wakes: boats ride on Cesium's water-masked terrain (needs
  editor assets; see ROADMAP), which can be the seabed (above). The hull has no draft: the
  Mako 655's keel is about 0.14 m above the model origin, so close up (tens of metres) the boat
  reads as sitting on the water rather than in it. Inland water that Cesium terrain does not
  render flat is not handled.
- Fast-moving vehicles can leave a faint TSR ghost trail behind them in close-ups.
- Boxes are loose, world-aligned projections of the model bounds, with no occlusion test
  (a vehicle behind a hill is still labelled).
- Dead reckoning uses velocity and angular rate (algorithms 2–9) but ignores acceleration, and
  PDU timestamps are unused (extrapolation runs from the time of arrival).
- DIS articulation parameters (turrets, guns) are not applied.
