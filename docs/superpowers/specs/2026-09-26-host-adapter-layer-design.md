# Host Adapter Layer — Design Spec (ROADMAP 2.3)

**Date:** 2026-09-26
**Status:** Approved 2026-09-26 (open questions resolved with the proposals below)
**Scope:** How protocol input (CIGI, DIS, scenario files, and later the control API and MAVLink)
reaches the simulation, and how responses go back out.

## Problem

CamSim was built CIGI-first, and CIGI's packet structs became the simulation's internal model.

- **Protocol types everywhere.** `FCigiEntityState`, `FCigiRateControl`, `FCigiArtPartControl`,
  `FCigiComponentControl`, `FCigiViewControl`, `FCigiSensorControl`, the environment structs,
  and `FCigiReceiver` itself are used directly by the camera components, the entity manager and
  actors, the environment, the ocean, the particle manager and the scenario engine (about ten
  modules outside `CIGI/`). DIS and the scenario engine *emit* `FCigiEntityState`, so the DIS
  adapter and the scenario engine are coupled to CIGI 3.3's layout too. CIGI 4.0 or MAVLink
  would have to be squeezed through CIGI 3.3 structs.
- **One shared entity ID space, no precedence rules.** CIGI, DIS and scenario states are merged
  into one `TMap<uint16, FCigiEntityState>` per tick. For the same ID, DIS silently overwrites
  CIGI and the scenario overwrites both. DIS IDs start at `dis.id_base_offset` (1000), but
  nothing stops a CIGI host or a scenario from using 1000+. The rate limiter is keyed by ID and
  shared across sources, so one source can suppress another's updates.
- **Only CIGI can drive the camera.** The camera platform route (`CameraEntityQueue`) exists only
  inside the CIGI receiver. A DIS federate (e.g. the MAVLink→DIS bridge in the config) can put
  an ownship entity in the scene but cannot fly the sensor.
- **Queries and responses are CIGI-only plumbing.** HAT/HOT and LOS requests are CIGI structs
  answered by a CIGI-specific handler writing CIGI packets; nothing else can ask "where does
  this ray hit the terrain" without speaking CIGI.
- **DIS dead reckoning is incomplete.** Algorithms 2–9 now use the right frames (2.2), but
  acceleration (4, 5, 8, 9) is ignored, and extrapolation starts from PDU arrival, not the PDU
  time.

## Goals

- One canonical command model. Adapters translate protocols into it; nothing outside an
  adapter includes a protocol header. Enforced by a check in CI.
- Entity identity scoped per source, with explicit, configurable precedence rules — no silent
  overwrites.
- Any source can own the camera platform (ownship), chosen in config.
- Terrain queries as a service with canonical requests and results; each adapter serializes
  its own responses.
- Full DIS dead reckoning (algorithms 1–9 including acceleration).
- No change to CIGI or KLV wire behaviour; each step verified by the automation suite and the
  live `ci_validate.sh --native` run.

## Non-Goals

- The CIGI 4.0, MAVLink and Python/gRPC adapters themselves (2.4, 5.1, 5.4) — this spec only
  makes them straightforward.
- Native HLA (use an HLA↔DIS gateway).
- Changing threading: protocol receivers keep their own threads and SPSC queues.
- DIS output (CamSim stays a passive DIS listener).

## Design

### Canonical commands (`Sim/Commands.h`)

Plain structs, protocol-neutral, in CamSim's conventions: WGS-84 geodetic positions (ellipsoid
height), orientation as a local-NEU quaternion (`CamSimFrames::FGeoPose`), SI units, sim time
from `FSimClock`.

| Command | Carries | Replaces |
|---|---|---|
| `FEntityCommand` | key, lifecycle (Active / Hidden / Remove), CamSim type id, classification, **pose** (geodetic, or attached: parent key + body-frame offset + relative rotation), optional **motion model**, source time | `FCigiEntityState`, `FCigiConfClampEntityState` |
| `FMotionModel` | linear velocity and acceleration with frame (world NED / body), angular velocity with frame, `bRotating` | `FCigiRateControl` (entity rates) |
| `FArticulationCommand` | entity key, part id, enabled DOFs, offsets/angles, optional rates | `FCigiArtPartControl`, art-part rate control |
| `FComponentCommand` | entity key, component class/id, state | `FCigiComponentControl` |
| `FViewCommand` | FOV, gimbal target (snap or slew), eye offset, first-person entity key | `FCigiViewDefinition`, `FCigiViewControl`, camera art part |
| `FSensorCommand` | on/off, waveband, polarity, gain / FOV preset | `FCigiSensorControl` |
| `FTimeCommand` | set UTC and/or rate / static | Celestial Sphere Control date/time |
| `FAtmosphereCommand`, `FWeatherCommand`, `FOceanCommand` | global atmosphere; weather layer or region; waves and maritime surface | the environment and ocean structs |

`FSimCommandSink` is the game-thread interface adapters write into (`Submit(FEntityCommand)` and
so on). The entity manager, camera, environment and clock consume from it — they never see
`FCigiReceiver`.

### Entity identity and precedence

- `FEntityKey { EHostSource Source; uint32 SourceId; }`. Each source has its own ID space:
  CIGI entity 1000 and DIS entity 1000 are different entities. `FEntityRegistry` maps keys to
  CamSim actors, so collisions are impossible by construction and `dis.id_base_offset` goes
  away.
- An entity belongs to the source that created it. Commands for an existing key from another
  source can't happen (different key), so there is no precedence rule to get wrong for
  entities.
- **Rate limiting** is per key, not per ID.
- **Cross-source references.** Attach parents and first-person targets are resolved within the
  requesting source's namespace (a CIGI child attaches to a CIGI parent). Query responses report
  a hit entity's ID only if it belongs to the asking source; otherwise "not an entity".
- **Singletons** (camera platform, view, sensor, environment, clock) have one owner each,
  configured under `hosts:` (below). Commands for a singleton from a non-owner are dropped with
  a once-per-source warning. Defaults reproduce today's behaviour: CIGI owns everything, and the
  scenario may set the clock at start.
- **Ownship.** The camera platform is "the ownship entity of the ownship source":
  `hosts.ownship: { source: cigi, id: 1 }` (today's `camera_entity_id`), or
  `{ source: dis, id: "site:app:entity" }` so a DIS federate — e.g. the MAVLink→DIS bridge —
  flies the sensor. The ownship entity is not also spawned as a visible actor unless
  `render_ownship: true`.

```yaml
hosts:
  cigi:     { enabled: true }
  dis:      { enabled: true }
  scenario: { enabled: false }
  ownship:  { source: cigi, id: 1 }
  view_owner:        cigi      # view, gimbal, sensor
  environment_owner: cigi      # atmosphere, weather, ocean
  clock_owner:       cigi      # CIGI Celestial; scenario start_hour still applies at start
```

### Adapters (`Hosts/`)

```cpp
class IHostAdapter
{
public:
    virtual EHostSource GetSource() const = 0;
    virtual void Start(const FCamSimConfig&) = 0;           // sockets, threads
    virtual void Stop() = 0;
    virtual void Poll(FSimCommandSink& Sink) = 0;           // game thread, once per frame
    virtual void EndFrame(const FFrameReport& Report) {}    // outbound: SOF, CoT, …
};
```

- **`FCigiHostAdapter`** owns `FCigiReceiver` and `FCigiSender`. `Poll` drains the existing SPSC
  queues and converts (pure functions `CigiToCommands.h`, unit-tested). `EndFrame` sends SOF
  plus query responses. CIGI 3.3 details (CCL classes, byte order, packet layouts) stay inside
  `CIGI/`.
- **`FDisHostAdapter`** owns `FDisReceiver`; converts Entity State PDUs to `FEntityCommand` with
  a full `FMotionModel`; designator PDUs to a `FDesignatorCommand`.
- **`FScenarioHostAdapter`** wraps `FScenarioEngine`, which emits `FEntityCommand` instead of
  `FCigiEntityState`.
- Later: `FControlApiAdapter` (2.4), `FMavlinkAdapter` (5.1), `FCigi4Adapter` (5.4).

`FHostAdapterSet` (owned by the subsystem) polls adapters in a fixed order inside the entity
manager's ordered pass (2.1/1.13): poll all adapters → apply commands (entities, then ownship,
then attachments) → queries → camera capture.

### Terrain queries (`Sim/TerrainQueries.h`)

`FTerrainQueryService` takes canonical requests — `FHeightQuery` (HAT/HOT at a point),
`FSegmentQuery`, `FRayQuery` (with min/max range) — each with a requester (adapter + opaque
request id + options such as "extended" or "entity coordinates"). It runs the line traces
(today's `FCigiQueryHandler` logic) and returns `FTerrainQueryResult` (hit, range, point,
surface normal, hit entity key) to the requesting adapter, which serializes it (CIGI 103/104/105
today). Future: `SampleHeightMostDetailed` for points outside loaded tiles (ROADMAP 1.9 note).

### DIS dead reckoning

`CamSimFrames::IntegrateMotion(FGeoPose&, const FMotionModel&, Dt)` replaces `IntegrateRates`:
second-order position (`p += v·dt + ½·a·dt²`, `v += a·dt`) in the model's frame, rotation from
body angular velocity for rotating models, CIGI world rates unchanged. DIS algorithm → model:

| DRM | Linear | Acceleration | Rotation |
|---|---|---|---|
| 1 Static | — | — | — |
| 2 FPW / 3 RPW | world | — | 3 only |
| 4 RVW / 5 FVW | world | world | 4 only |
| 6 FPB / 7 RPB | body | — | 7 only |
| 8 RVB / 9 FVB | body | body | 8 only |

Extrapolation starts from the PDU's own timestamp when the federate's clock is usable (DIS
absolute timestamps, relative to the sim clock), else from arrival time.

### Enforcement

`scripts/check_layering.sh` (run in CI and by the pre-commit hook): no file outside `CIGI/`,
`DIS/` and `Hosts/` includes `CIGI/*.h`, `DIS/*.h` or `cigicl/*`, or names an `FCigi*` / `FDis*`
type.

## Phases

Each phase is independently revertible and must pass the automation suite and
`ci_validate.sh --native`.

1. **Types and conversions.** `Sim/Commands.h`, `FEntityKey`, `FMotionModel`; pure CIGI→command
   and DIS→command converters with tests (every CIGI field that is consumed today must survive).
2. **Entities.** `FEntityRegistry` with per-source keys; `FCamSimEntityManager`, `ACamSimEntity`
   and the particle manager consume `FEntityCommand` / `FMotionModel` / `FArticulationCommand` /
   `FComponentCommand`. CIGI, DIS and scenario become adapters feeding the sink. Fixes the ID
   collisions and the shared rate limiter.
3. **Camera and ownship.** Platform rig, gimbal and sensor components consume `FEntityCommand`
   (ownship) / `FViewCommand` / `FSensorCommand`; `hosts.ownship` config, including DIS ownship.
4. **Environment and clock.** Environment, ocean and `FSimClock` driven by commands with
   single owners.
5. **Queries and outbound.** `FTerrainQueryService`; CIGI responses and SOF via
   `FCigiHostAdapter::EndFrame`.
6. **DIS dead reckoning.** `IntegrateMotion` with acceleration, all nine algorithms, PDU-time
   extrapolation.
7. **Enforcement and cleanup.** Layering check in CI; remove `FCigi*` from non-protocol code,
   `dis.id_base_offset`, dead code (`FCamSimEntityManager::BuildScenarioState`).

## Decisions

1. **Entities across sources in query responses:** a host's query response reports an entity
   it hits only if the entity belongs to that host's source; anything else is "not an entity".
2. **DIS ownship:** supported from phase 3 (`hosts.ownship.source: dis`); the MAVLink→DIS
   bridge is the way to fly the sensor from PX4/ArduPilot until the MAVLink adapter (5.1).
3. **Same vehicle from two sources:** no de-duplication; configure one source to stay silent.
4. **Scenario and a live host at once:** allowed; scenario entities have their own namespace,
   and the scenario sets the clock only at start.
