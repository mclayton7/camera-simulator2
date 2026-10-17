# CamSim Roadmap

Started fresh on 2026-09-26 after a full architecture review. **Trimmed 2026-10-16:** finished work is now one line per
item under "Done". The full write-ups (acceptance tables, findings, decisions) are in the guides under `docs/` and in
git history: `git show ae0a390:ROADMAP.md`. Item IDs (0.3, 2.4, 3B.2, 4A, R3b, …) are unchanged, because code comments
and docs cite them.

## Direction

CamSim stays on **UE5 + Cesium for Unreal + FFmpeg + CIGI (CCL)**. That stack is still the right foundation for an
open-source alternative to VRSG. The work is about **depth, not breadth**: be correct against the standards claimed, make
the sensor physically meaningful, then add surface area.

Guiding rules:

1. **Standards correctness beats feature count.** A host integrator or MISB decoder tests exactly the edge cases
   (entity-relative coordinates, frame counters, timestamps, checksums). One wrong field breaks interoperability.
2. **Keep pixels on the GPU.** CPU-side image processing is a reference for tests, not a product path.
3. **One source of truth** for time, coordinate frames, and entity state.
4. **Nothing is "done" until a test or acceptance check proves it.** No `|| true` on validation steps.
5. **No new feature phases until Milestones 0–4 land.** Waived for the realism track (2026-10-09). Milestones 0–4 have
   landed except the rest of 4D (below). CI tasks were dropped on 2026-10-16 (3D, the CI runners and jobs) and 3C was
   deferred to 11.6. Waived for Milestone 6 (2026-10-16); Milestones 7–10 need the same waiver (or 4D closed).

---

## Next: planned milestones (from the 2026-10-16 capability review)

What professional IGs (VRSG, VBS Blue IG, CAE/Presagis, Aechelon) do that CamSim doesn't yet, plus engine and Cesium
features worth adopting. Checked against the UE 5.8 release notes on 2026-10-16. Recommended order: **6 → 7 → 8**, then
9 and 10 as demand appears. The small adoption spikes (11.x) can run at any time.

### Milestone 6: Night EO and night vision

Night is the largest gap for an EO/IR trainer. Today the night sky is "very dim blue light (moonlight approximation)"
(`Environment/CamSimEnvironment.cpp`), and NVG was removed in 3B.2.

| ID  | Item |
| --- | ---- |
| 6.1 | **6A done (2026-10-16)** (`docs/night.md`): `FEphemeris` sun/moon/sidereal time, moon as atmosphere light 1 (K&S), physical sun (k = 1, 127.5 klx TOA), generated IsSky dome `M_SkyDome` (atmosphere, discs, BSC5 stars, night floor), Lumen cached-lighting band, radiometric EO exposure (9.4), CIGI celestial enables; `night_check.py` N1–N6 and N8 pass on the RTX 5080, N7 open (moonlit clouds +0.55 ms GPU). |
| 6.2 | **Light points / cultural lighting:** streetlights, airfield lighting, building windows (from R3 buildings and R2 roads in the scene package), vehicle headlights and nav lights. Built on **MegaLights** (production-ready in UE 5.8: hundreds of shadow-casting lights at near-constant cost). CIGI: Component Control for entity lights. |
| 6.3 | **Image-intensifier (NVG / I²) preset** on the photon detector (3B.2): photocathode spectral response, gain, halo/blooming around point sources, scintillation; green or white phosphor display. Optional SWIR preset. Watch **Fog Screen Space Scattering** (experimental in 5.8: light scattering in volumetric fog and local fog volumes) for halos in fog. |
| 6.4 | **Acceptance:** night scene under full moon vs new moon (NVG mean level and noise follow illumination); lit airfield and street grid visible in EO and NVG; light count vs frame budget on the RTX 5080 and the M1 Pro. |

Editor/assets: light-point fixture types (data, not meshes) and a vehicle-light material slot convention.

### Milestone 7: Model pipeline and entity library

`entities/` ships four types (truck, boat, F-16, marker). For ATR training, breadth of targets (people, civilian
vehicles, armour classes, damage states) is the dataset.

| ID  | Item |
| --- | ---- |
| 7.1 | **Import pipeline:** a script that takes a CC0/CC-BY glTF and writes its `entity_types` entry (scale, `z_offset_m`, `semantic_class`, `thermal_material`, `thermal_parts` volumes, `mesh_destroyed`), LODs and a licence file; checked like the vegetation library (sha256). |
| 7.2 | **Starter library:** civilian cars, pickups, a tracked and a wheeled armoured vehicle, small boats, a helicopter; each with a destroyed state and thermal parts. |
| 7.3 | **Human figures (dismounts):** animated walking/standing/prone figures with `person` semantic class and a skin/clothing thermal model. Low-poly CC0 characters first. **MetaHuman Crowd** (experimental in 5.8) is an option to evaluate, not the default: heavy for ATR scale. |
| 7.4 | **Many entities:** UE 5.8's Mass overhaul (Mass Signals moved into the engine) lowers the cost of the "Mass/ECS entities" item under "Later"; take it up when scenes need more than a few hundred entities. |
| 7.5 | **DIS articulation parameters** (turrets, guns; 2.5 carry-over) and per-entity motion commands over the control API (2.4). |

Human: model selection and licence review for the starter library.

### Milestone 8: Obscurants, weather and battle effects

| ID  | Item |
| --- | ---- |
| 8.1 | **Smoke, fire, dust and exhaust plumes** (Heterogeneous Volumes or Niagara) **with a thermal signature**: today particles are composited by TSR in visible colour over the radiance (4A). Burning vehicles (4C) get a plume; aircraft exhaust plumes (4C carry-over). |
| 8.2 | **Rendered precipitation:** rain and snow in EO and IR (today only a telemetry flag from CIGI Weather Control; the CPU overlay was removed in 3B.2), with in-band attenuation. HITL gap 13. |
| 8.3 | **Surface state:** wet, snow-covered and icy surfaces from CIGI Terrestrial Surface Conditions (EO reflectance, thermal emissivity and cooling). |
| 8.4 | **Battle damage:** craters and scorch marks as decals over Cesium terrain (Runtime Virtual Textures or deferred decals); persistent per scene. |
| 8.5 | **Maritime:** vessel wakes, whitecaps, spray, breaking waves (2.6 carry-over; Breaker Type is parsed, unused), wakes in IR. |
| 8.6 | **Local fog and haze:** Local Fog Volumes for valley fog and smoke banks; per-band atmospheric transmission and path radiance beyond 4A's terms (MODTRAN-style tables). |
| 8.7 | **Thermal cloud term from the rendered clouds** (4A known issue: only CIGI weather cools the scene). |

Human: `/Game/Effects` Niagara/volume assets (then add `/Game/Effects` to `DirectoriesToAlwaysCook` in
`DefaultGame.ini`, or the package won't carry them).

### Milestone 9: Sensor behaviours

| ID  | Item |
| --- | ---- |
| 9.1 | **Simulated video tracker:** point/area track on a selected entity or ground point, driving the gimbal and the KLV track gate (Tags 43/44, already conformant). |
| 9.2 | **Laser designator / rangefinder:** spot visible in the sensor band, range from LOS; consumes `DisEntityAdapter::GetDesignatorSpot` (state exists, no consumer since 3B.2). |
| 9.3 | **Motion blur and rolling shutter from the integration time**, inside `SensorCS` and its CPU reference (not UE's motion blur, which stays off). Real platform imagery at 30 fps has both; detectors trained without them suffer for it. |
| 9.5 | **Distortion-aware KLV corners** (3B.3): Tags 82–89 are pinhole when `optics.k1`/`k2` ≠ 0. |

### Milestone 10: Interoperability

| ID   | Item |
| ---- | ---- |
| 10.1 | **One World Terrain (OWT)** import: the US Army's synthetic-training terrain is 3D Tiles based, so Cesium loads most of it; map its feature attribution onto land-cover codes, thermal classes and semantic classes. |
| 10.2 | **OGC CDB** ingest into scene packages (terrain, imagery, vectors, models): military users already own CDB databases. |
| 10.3 | **CIGI coverage:** Position Request/Response, Environmental Conditions Request + Weather Conditions Response, Terrestrial Surface Conditions (8.3), Environmental Region Control and regional Weather/Wave/Maritime (logged and ignored today), Collision Detection responses, Event Notification, Short Component/Art Part Control; optional Symbol Surface/Text packets (a host-drawn overlay layer, off by default). |
| 10.4 | **CIGI 4.0** adapter (was 5.4). |
| 10.5 | **HLA** through an external HLA↔DIS gateway (2.3 decision), documented and tested. |

### 11: Engine and library adoption (spikes, any time)

| ID   | Item |
| ---- | ---- |
| 11.1 | **Hardware ray tracing + path-tracer reference frames** for R6: `r.RayTracing=False` today. A path-traced still of a check pose measures Lumen's error; HW Lumen is a possible quality switch. Check Metal and Vulkan support on both reference machines. |
| 11.2 | **Lumen Lite** (Beta in 5.8, irradiance fields with probe occlusion): bench with `scripts/bench/sweep.py` against Lumen, and judge with R6. Capacity is VRAM-bound (`docs/capacity-linux-rtx5080.md`), so a GPU-time saving alone won't add instances. |
| 11.3 | **Cesium clipping polygons** (`CesiumCartographicPolygon` + `CesiumPolygonRasterOverlay`, or `CesiumTileExcluder`): fix piers and below-sea-level land that flood under the ocean (2.6 carry-over; HAT/HOT there returns sea level). |
| 11.4 | **`CesiumFeaturesMetadataComponent`** on the buildings tileset: per-building class and ID from its `EXT_structural_metadata` (`camsim_buildings`) for per-instance building labels (4D) and per-building thermal materials (a 4B non-goal until now). |
| 11.5 | **Nanite for entity meshes**; **Nanite Foliage** stays a watch item (experimental since 5.7), not a replacement for the HISM vegetation. |
| 11.6 | **3C GPU-texture encode** (deferred 2026-10-16): UE `AVCodecs` (experimental in 5.8) NVENC/VideoToolbox from GPU textures, FFmpeg for TS/KLV muxing only; a second encode path beside the readback (libx264 and ground truth keep it). Spike first: does `AVCodecs` work on Vulkan + NVENC and Metal + VideoToolbox? Take it up only if readback or encode shows up as a cost. |

Considered and not reopened: land cover as a Cesium raster overlay (4B chose the camera-centred window sampled in
`ThermalCS`); Mesh Terrain (5.8, experimental; terrain comes from Cesium); Movie Render Graph (offline rendering).

---

## Open items carried from finished milestones

### Standards, protocols, KLV

- **HAT/HOT and LOS hit only loaded tiles** (1.9, HITL gap 14): points outside the view miss. HAT/HOT could use
  `ACesium3DTileset::SampleHeightMostDetailed`. Periodic requests (Update Period > 0) and Host Frame Number LSN aren't
  implemented. LOS doesn't test the water surface.
- **LOS to a destination on the surface** can hit the surface at the destination and report Occluded (1.8): treat a hit
  within a small tolerance of the destination as Visible.
- An entity attached to a camera platform that is itself attached lags one frame (1.13); not measured live.
- **Keyframe bursts** (1.14, deferred): only receivers that never enlarge their socket buffer are affected. If one turns
  up, the agreed fix is intra refresh behind `streaming.intra_refresh`.
- **DIS:** CIGI/DIS precedence and ID ranges (2.3); DR acceleration and PDU timestamps (extrapolation runs from arrival
  time); not verified against a live federate.
- **Control API** (2.4): render-history dependence (the same view after a different frame history differs by up to
  ~3 DN, a global offset; TSR, Lumen and exposure history suspected); HAT/HOT and LOS over the API; moving the CIGI
  view/sensor/environment path onto the canonical consumers (2.3).
- KLV attitude near nadir: `Pose.Neu.Rotator()` (`CamSimPlatformRig.cpp`) snaps the heading/roll split within 0.08° of
  local nadir (pointing unaffected; R6).
- VideoToolbox adds ~10 frames of latency (no `MaxFrameDelayCount` in FFmpeg); its max-rate test is flaky.

### GPU pipeline and performance (Milestone 3 remainder)

- Mode-switch AE transients (3B.3): skip histograms with `Serial < SnapAfterSerial` while a snap is pending; seed a
  mode's first entry.
- GPU tests for a NaN bloom texel and for non-same-size + bloom + blur partial thread groups.
- Config NaN checks: `highlight_percentile`, the AGC percentiles and `max_photon_gain_ev` use range checks a NaN passes.
- `scripts/check_cigi_responses.py` / `capture_cigi_stream.py` need Python ≥ 3.10 (`TimeoutError`): declare it.
- **Yosemite snap coarseness** since Cesium's LOD crossfade went off (re-enabling costs 8–12 ms game thread): a user
  trade-off. `ForbidHoles` under frustum culling is untested.
- **Cesium per-tile collision cost:** 2.29.1 re-applies the collision profile to every rendered tile every frame; worth
  an upstream issue.
- Static entities tick their hidden skeletal/anim components (~0.5–0.9 ms game thread at 500 entities).
- A periodic ~30 s engine hitch (+7 ms), source unknown; the close-up tile-streaming hitch; a TSR ghost trail behind
  moving vehicles in close-ups.
- One intermittent renderer crash (`FMeshDrawShaderBindings::SetOnCommandList`, after a cut to ground level with
  buildings and trees on), not reproduced since; watch for it.

### Ocean (2.6)

- Off-centre boats heave on waves the drawn sea has faded (mesh cell-size fade vs unfaded placement); fix by publishing
  the mesh's fade to placement or a denser grid.
- Ripples hatch visibly at close zoom (three fixed directions); ripple precision degrades > 200 km from the anchor.
- Inland water where Cesium terrain has no flat surface; lake boats get no submerged-hull cut.
- No wave anchor before the camera's first tick (boats heave without pitch/roll); one-frame phase mismatch at a re-anchor.
- Deferred review minors: see `git show ae0a390:ROADMAP.md`, 2.6 "Deferred minors".

### Ground truth (2.7, 4D)

- **4D remainder:** validation against published thermal data (4.6), thermal shadow lag, per-instance labels for trees
  and buildings (see 11.4), semantic Metal run.
- Vehicle-on-vehicle amodal masks; submerged-hull cut is one horizontal plane per entity.
- Per-frame instance PNG (only RLE per annotation today); TSR jitter misregisters mask edges by up to ~0.5 texel.
- Semantic label offset of about h·d/R toward the window centre; without a window the sea band alone decides water.
- Deferred review minors: see `git show ae0a390:ROADMAP.md`, 2.7 "Deferred minors".

### Thermal IR (4A–4C, R2)

- **R2 gate (r) fails** on the data-picked I-5 pose (dry verge refined to `bare_soil`): give vector road codes their
  class albedo in the solar term and stop dry verges refining to `bare_soil`; NDVI in `ThermalCS` (own spec). Large dark
  IR patches over I-5 and runway grass, absent in EO (cause unverified). Needs a decision.
- ThermalCS is +0.27 ms over 4A's plan: skip the vegetation taps for the None family and non-terrain pixels.
- 10 m class-edge staircase between same-colour classes; WorldCover is a single epoch (seasonal land cover is a non-goal).
- IR TSR alpha costs +1.6 ms and the first EO→IR switch has a PSO hitch; no TSR history reset on a switch when the user
  already set `r.TSR.AlphaChannel=1`.
- A burning vehicle drives the thermal AE to its floor and crushes the background (a lower `min_gain_ev` trades noise).
- Hot-spot volumes are unreviewed first estimates; F-16 has no parts; a "damaged" thermal effect, hot ground under
  parked vehicles, tyre tracks and host-set thermal history are missing; one unexplained bright-bar frame in MWIR.
- Metal runs: `thermal_check.py --runs entity`, `CamSim.GPU.Thermal.EntityPartsMatchesCpu`.
- Cleanup: move `CamSim.GPU.Thermal.*` onto the `WRITE_SCENE_COLOR` path and drop the R32F `OutRadiance`. Deferred
  review minors: see `git show ae0a390:ROADMAP.md`, 4A/4B "Carried over".

### Realism track (`REALISM.md`)

- **R1:** thin land features (piers) leave a dark ~200 m NAIP disc; a turquoise shallow shelf with a hard outline
  (seabed under the ocean); the sea-level offset is one station constant, but local MSL varies +0.08 to +0.66 m along the
  Pendleton coast (add a per-package correction grid read by `FOceanSurface`); `-j 1` builds report a false RSS fail.
- **R2:** land cover's feature load at 200 km peaks at 2,494 MB: read and burn per land-cover tile or chunk.
- **R3:** footprint IoU (gate 2, needs ~30 human-traced NAIP outlines); the deepening pass (gabled/hipped roofs,
  facade/roof materials); bridge towers and cables. **Bridges B3** fails at 4 of 18 ends (at-grade junctions keep part
  of the bare-earth dip): follow the approach way, or ramp the deck end.
- **R4:** dense-pass game p50 over budget (+0.13 ms Mac, +0.33 ms p50 / +0.57 ms p95 Linux); packages for the
  operational sites (Eglin, Pax River, Yuma); review minors (far-block in-flight starvation, no far tier without far
  meshes, all-unknown mix codes go bare instead of default, far trees ignore R3 buildings, cut-during-switch tests).
- **R5:** corridors from routes or recordings, the `high` profile and cache warming (deferred by the user 2026-10-14).
- **R6:** appearance and sim-to-real wait on a real UAV EO/IR dataset (`REALISM.md` open question 3). Determinism:
  volumetric clouds scroll on engine time (recommended: a generated cloud material driven by `FSimClock`, like
  `M_Ocean`); Lumen's surface-cache refresh has no deterministic freeze; set the cross-GPU SSIM limit with
  `determinism_check.py --against`.

### Night sky (6A)

- **N7 (night cost) fails: +0.55 ms GPU p50** at the bench `night_slant` pose (2.975 → 3.528 ms). The moon, as atmosphere
  light 1 with cloud scattering on (spec), lights the level's volumetric clouds (UE's "expensive" second cloud light,
  ~0.4 ms) and casts VSM shadows (~0.1 ms). Decision needed: moon `CloudScatteredLuminanceScale` black (≈ +0.12 ms, dark
  clouds under the moon) or accept the cost (`docs/night.md` "Acceptance").
- Thermal IR still takes its sun from `ACamSimEnvironment::ComputeSunPosition`; move `FThermalModel` onto `FEphemeris`
  (changes the thermal calibration; rerun `thermal_check.py`).
- `Main.umap` has no `ExponentialHeightFog`: CIGI visibility does nothing (with 8.6).
- 6C light points: MegaLights needs hardware ray tracing (`r.RayTracing=False`; the M1 Pro has none): find another
  approach (or HW RT, 11.1).
- Metal: `M_SkyDome` not compiled or run on the M1 Pro; the M1 Pro run of `night_check.py` is open.
- Memory: the star texture keeps a 64 MB CPU copy and 64 MB of VRAM, and `FStarField::Bin` allocates a ~200 MB
  transient accumulator at startup (capacity is VRAM-bound).
- After an IR → EO switch the cached-lighting band can read a stale median for a frame or two.
- `r.Test.FreezeTemporalSequences 1` (the R6 determinism pin) makes TSR hold stale light indefinitely after a large
  lighting jump in lockstep (full → new moon kept the median 5 EV high; night → noon left a mottled sky):
  `determinism_check.py` / `control_api_check.py` captures after such jumps are suspect.
- The sky dome's stars are one texel (≈ 5′); no earthshine, planets or Milky Way.

### Operations

- macOS: `run.sh` copies the config into a staged `.app` (not checked).
- HITL: CamSimTruth mac/Windows builds unverified; the on-rig checks in `hitl/README.md`.

---

## Human and editor work

- Done 2026-10-16 (headless editor Python, no human needed): `Main.umap` lost the OSM Buildings actor (ion 96188)
  and the terrain's Bing overlay (ion 2); the terrain tileset itself stays, because `ApplyCesiumBackendConfig`
  configures the level's tileset and never spawns one. `Main.umap` and `CesiumIonSaaS` resaved in 5.8 (the last
  5.7-saved assets). Epic's Unreal MCP plugin (`ModelContextProtocol`) is enabled for the editor target only.
- **Visual reviews pending:** R3 buildings EO shots; R4 (`.cache/veg/check-hue/`; pines wait for Eglin imagery; the
  500 m shrub edge is a hard cut); R5 renders (`.cache/linux-r5/render/`, `render-r0/`, `edges/`, including a faint
  horizontal break in `outside_low`); bridges (`.cache/r3/bridges/shots/`).
- R3: trace ~30 NAIP building outlines for footprint IoU; facade/roof materials in the deepening pass.
- Milestones 6–8: light-point fixture data, the starter model library (licence review), `/Game/Effects` assets.

---

## Done

One line each; details in the named guide or `git show ae0a390:ROADMAP.md`.

| ID | Done | What |
| -- | ---- | ---- |
| 0.1–0.8 | 2026-09-26 | Correctness: CIGI heading in East-South-Up (`CigiFrames.h`), KLV Tag 2 UTC, misb.js conformance (0.3: running-sum checksum over `01 02`, gate Tags 43/44, Tag 47 bits, Tag 56, ST 0102 Tag 13; `check.js` replaces `validate_klv.py`), entity-relative CIGI coordinates, 32-bit SOF frame counter, ground speed from host time, terrain readiness gate, libx264 VBV cap |
| Upgrade | 2026-09-26 | UE 5.8.3, Cesium for Unreal 2.29.1 installed into the engine, glTFRuntime 20260826, FFmpeg 8.1.3 |
| 1.1–1.13 | 2026-09-26 | Strict CI validation, tests through the real loader, unknown-YAML-key warnings, `ACamSimCamera` split, gimbal slew between packets, extended HAT/HOT and LOS responses (material codes from R2), versioned CCL casts and Rate Control frames, KLV Tags 15/25 as MSL via EGM96 (`Geoid.h`), one ordered entity/camera pass; merged phase branches deleted |
| 1.15 | 2026-10-01 | Docker image with the host's NVIDIA GPU (`docs/docker.md`); Shipping package fixed 2026-10-10; lavapipe can't run UE 5.8 |
| 2.1 | 2026-09-26 | Sim clock (`FSimClock`): CIGI Celestial date/time, rate, lockstep; drives KLV, sun, ground truth |
| 2.2 | 2026-09-26 | Coordinate frames (`EcefFrames.h`, `CigiFrames.h`): DIS ECEF orientation/velocity, IEEE 1278.1 dead reckoning 2–9 |
| 2.3 | 2026-09-26 | Host adapter layer: canonical commands, entities keyed per source (CIGI, DIS, control API) |
| 2.4 | 2026-10-15 | Control API (`docs/control-api.md`): `/api/v1`, lockstep, camera ownership, settled captures with masks/depth, `make_dataset.py` |
| 2.5 | 2026-09-28 | DIS trucks and boats on the rendered surface (`docs/dis.md`) |
| 2.6 | 2026-09-30 | Ocean: Gerstner waves on sim time, globe-conforming sea at EGM96 + tide, boat placement, HAT/HOT on water, `M_Ocean` generated |
| 2.7 | 2026-09-30 | ATR ground truth (`docs/ground-truth.md`): rendered masks, visibility, truncation, OBB, box3d, RLE; depth from the primary view (2026-10-01) |
| HITL | 2026-10-01 | Hooter rig (`HITL.md`, `hitl/`): gimbal unwinding, one read point per datagram, packet 201 Platform Kinematics, KLV corners 82–89, IG host and X-Plane truth plugin |
| 3A, 3A.1 | 2026-09-27 | Sensor as the primary view (TSR, one render per frame), readback ring, 30 fps output |
| 3B.1, 3B.2 | 2026-10-02 | GPU sensor graph replacing the tonemapper; physical sensor model in one fused `SensorCS` with a CPU reference; presets `eo_hd_cmos` / `mwir_cooled` / `lwir_uncooled`; legacy CPU path, NVG, overlays removed. Exit criterion met on the RTX 5080 (GPU p99 17% of budget at 1080p) |
| 4A | 2026-10-02 | Thermal core (`docs/thermal.md`): in-band radiance from class temperatures, sky, Planck LUT; `ThermalCS` pre-TSR |
| 4B | 2026-10-02 | Land cover for thermal: WorldCover window, imagery refinement |
| 4C | 2026-10-07 | Entity thermal state: skin and parts on sim time from power, damage, flaming, motion |
| 4D (part) | 2026-10-16 | Semantic class image in ground truth, datasets and control API captures |
| 6A (6.1) | 2026-10-16 | Night sky (`docs/night.md`): ephemeris, moon light, IsSky dome with stars and night floor, absolute light levels (k = 1), Lumen cached-lighting band; `night_check.py` N1–N6, N8 (N7 open) |
| 9.4 | 2026-10-16 | Radiometric EO photon gain (folded into 6A): integration time from f-number, pitch, transmission, QE; IR stays calibrated (Milestone 9) |
| R0 | 2026-10-09 | Offline scene packages (`docs/scene-packages.md`): 3DEP + NAIP over `file://`, byte-identical builds |
| R1 | 2026-10-10 | Imagery edge (Sentinel-2 → NAIP), NDVI layer, seabed under tidal water, local MSL; Linux/Docker offline |
| R2 | 2026-10-10 | OSM roads/rail and NHD water burned into land cover; CIGI material codes |
| R3, R3b | 2026-10-11 | Overture buildings as a 3D Tiles tileset; footprint mask, 230 burn, NAIP roof atlases; Linux 2026-10-13 |
| R3 bridges | 2026-10-15 | OSM bridge decks as a second feature tileset; CIGI and DIS solid |
| R4 | 2026-10-12 | Vegetation (`docs/vegetation.md`): species mix, CHMv2 canopy, shrubs, far tier; Linux 2026-10-13 |
| R5 | 2026-10-14 | Detail polygons at `sim` inside a `preview` box; buildings built by chunk, ~2 km draw outside detail (2026-10-15) |
| R6 (part) | 2026-10-15 | Geo-registration (`georeg_check.py`, G1/G3 pass) and datum checks; near-nadir view tilt fixed; determinism check built (`docs/realism-validation.md`) |

---

## Milestone 5: Drone-community interfaces

| ID  | Item |
| --- | ---- |
| 5.1 | **MAVLink adapter:** PX4/ArduPilot software-in-the-loop sims drive vehicle pose and gimbal (Gimbal Protocol v2). The HITL IG host (`hitl/`) covers this outside CamSim for the Hooter rig. |
| 5.2 | **RTSP/RTP (or SRT) output** alongside MPEG-TS multicast, for the autonomy/QGroundControl toolchain. |
| 5.3 | **ROS 2 bridge** (images + camera info + TF) via the control API. |
| 5.4 | CIGI 4.0: moved to 10.4. |

## Later / only when there's demand

- Mass/ECS entities with a timestamped interpolation buffer, for pattern-of-life scenes with thousands of entities
  (one actor per entity is fine up to a few hundred; see 7.4).
- Multiple sensors per platform and multiple platforms per instance, with sensors as components; frame-synchronised
  multi-channel output.
- SAR/radar, LiDAR output, VR/dome output, scenario editor GUI.

## Removed (2026-10-01 feature trim)

The features parked here ("stay in the tree, not extended") were reviewed and the unused, dead or duplicated ones were
removed rather than carried. Removed config keys now produce the standard unknown-key warning; the full list of keys and
env vars is in `docs/configuration.md` ("Removed in the 2026-10 feature trim").

- **TAK / ROVER**: the CoT position sender (`Streaming/CotSender`), the second ATAK FMV encode, and the ROVER PID /
  Constrained-Baseline remap. A TAK feed is the host's job; receivers find the streams through the PMT.
- **Scenario engine**: scripted entities, waypoints, triggers, pattern-of-life, formation flying and the randomizer
  (`Scenario/`), the `Scenario` host source, and `scripts/batch_run.py`. Driving entities is the CIGI/DIS host's job;
  dataset generation moved to the control API (2.4).
- **Effects with no assets**: Niagara rotor wash / smoke / fire / contrails, crater decals and damage-transition FX
  (`Environment/CamSimParticleManager`); the `/Game/Effects` assets were never in the repository. The "gradual" damage
  mode (a delayed mesh swap) went with them: Component Control 10 swaps meshes immediately. (Milestone 8 brings
  effects back with assets and a thermal signature.)
- **Non-physical optics**: `optical_realism` (UE motion blur, bloom, depth of field, lens flare) on top of the physical
  sensor model (3B.2). Motion blur is always off (9.3 adds it physically, in the sensor model).
- **Weather extras**: god rays, sky-scattering multipliers and regional weather zones. CIGI Weather Control with Region
  ID > 0 is ignored.
- **Duplicated outputs**: the telemetry JSONL sidecar (`ground_truth:`; KLV carries it), Pascal VOC export (COCO is a
  superset), the `camsim_health.json` file and the Prometheus textfile (`/ready` and `/metrics` cover both). Frame drops
  are always counted, by reason, on `/metrics` (`camsim_frame_drops_by_reason_total`).
- **Runtime knobs**: config hot reload (and `UCamSimSubsystem::GetConfigSnapshot`, which only existed for it),
  gimbal-slew tile prefetch, adaptive SSE, and the separate render/output frame rates (`frame_rate` is the one rate).
- **Dead config**: `rendering_quality.rt_reflections`, `use_instanced_rendering`,
  `scenario.entities[].activity_profile` (parsed, never read).

Kept: CIGI, DIS, KLV (ST 0601/0102), the sensor model, ocean, ground-truth masks + COCO + depth, the HTTP health
server, terrain gate, Cesium backend config, CIGI record/playback, and `output_views` (multi-stream fan-out).
