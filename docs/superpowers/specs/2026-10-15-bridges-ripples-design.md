# REALISM R3 bridges and ocean ripples on the sim clock (design)

Date: 2026-10-15. Status: the user picked both items ("1 and 2") from the no-dataset, no-editor list and delegated
every design choice ("I trust your recommendations and how you choose to execute"), so this spec is self-approved.

Two independent pieces:

1. **Bridges** (REALISM R3, deferred from the thin slice). The terrain is bare earth (3DEP removes bridge decks), so
   today a road or railway over a river or valley drops to the valley floor, and the Golden Gate draws as a slab at
   the waterline. Generate bridge decks from OpenStreetMap into the scene package. CIGI and DIS treat them as solid.
2. **Ocean ripples on the sim clock** (ROADMAP R6 follow-up, recommended and accepted). `M_Ocean`'s ripple normal
   reads UE engine time, so it keeps moving with the sim clock frozen, and runs differ.

---

## 1. Bridges

### 1.1 Source and selection

- **Ways:** OSM `road` and `rail` features already read for R2 (`[features]`, `osm_geofabrik`, the cached derived
  extract `_load_features`) whose tag `bridge` is anything but `no`, and which are not tunnels, `covered=yes`, or
  `layer < 0`. Road classes and rail kinds are R2's (`road_classes`, `rail`/`light_rail`). Pendleton: 203 ways (56
  motorway, 19 rail; length median 61 m, max 425 m).
- **Config:** a new optional `[bridges]` table in the scene file: `enabled` (default true when the table is present).
  It needs `[features]` with `roads` or `rail` (else a `ConfigError`: the bridges come from the same OSM extract and
  carry its ODbL notice). No other user settings: the constants below are rules (`BRIDGES_RULES_VERSION`).

### 1.2 Deck heights (the core of the design)

3DEP is bare earth, but approach embankments are ground, so the terrain at a bridge's ends is the road's height. A
deck's height comes from its ends, not from what is under it.

- **Network:** merge the bridge ways of one kind (road / rail) into a graph whose nodes are way end points
  (coordinates equal to 1e-9°: OSM shared nodes give identical coordinates). A node of degree 1 in the bridge graph is
  **grounded**: its height is the bare-earth terrain there (the same exact-offset sampler the buildings use,
  `terrain.point_heights`, WGS-84 ellipsoid). A node joined to two or more bridge ways (a ramp merging on a viaduct)
  is **free**.
- **Free nodes** get the arc-length-weighted harmonic solution: for every free node, Σ over incident ways of
  (h_node − h_other) / length = 0 (a small dense solve per connected component, `numpy.linalg.solve` on at most a few
  hundred unknowns; components without a grounded node are dropped and counted `dropped_ungrounded`).
- **Along a way:** height is linear in arc length between its two end nodes.
- **Dropped low:** a way whose deck is never more than `MIN_CLEARANCE_M` = 2.0 m above the terrain (sampled every 5 m
  along it) is not built (`dropped_low`): the terrain already carries the road (culverts, small creek bridges). Its
  graph role (grounding) is unchanged.

### 1.3 Geometry

Each kept way is a **sweep of one cross-section along its densified centreline** (stations every ≤ 10 m, plus every
OSM vertex), in the tile's ENU frame like the buildings:

- **Width:** `features.width_m` (R2's road / rail widths; the `width` tag wins).
- **Cross-section** (across × up, relative to the deck top at the centreline): a deck slab `DECK_THICKNESS_M` = 1.5 m
  deep, with a parapet `PARAPET_W_M` = 0.3 m wide and `PARAPET_H_M` = 1.0 m high on each side. The section polygon is
  swept: one quad strip per section edge, flat-shaded; the two end faces triangulated (constrained Delaunay of the
  section).
- **Joins:** the offset direction at an interior station is the bisector of the two segment normals, scaled by
  1 / cos(half-angle) and capped at 2× (no spikes at sharp bends).
- **Piers:** for every station that is at least 10 m from both ends and at least `PIER_SPACING_M` = 30 m along the deck
  from the previous pier, where the clearance (deck bottom − terrain) is at least 3 m: a box 1.2 m along the deck × 0.6
  × deck width across, from the deck bottom to 0.5 m below the terrain under it.
- **Ends:** the deck top at a grounded node equals the terrain there, so the road runs onto the deck with no step.

### 1.4 Package layout

- `features/bridges/tileset.json` + `features/bridges/<z>/<x>/<y>.glb`: 3D Tiles 1.1, one z13 tile per deck by its
  centroid, ADD refinement, root content-less; geometric error 60 (the buildings' z13 tier: bridges are long and
  visible from far). Same ENU-per-tile convention and glTF writer as the buildings (`gltf.write_glb`) with two
  materials: the deck top (driving surface) dark asphalt grey, everything else concrete grey; rail decks the ballast
  colour. Flat colours: no imagery texture (NAIP is orthorectified to bare earth, so a deck's image is displaced by up
  to several metres; sampling it would paint water on the deck).
- **Metadata:** an `EXT_structural_metadata` table `camsim_bridges` with `source_id` (OSM way id), `kind`
  (`road` / `rail`) and `max_clearance_m` per feature.
- **Layer build:** in the main process (hundreds of ways, not millions: no chunking), skipped when its inputs hash
  (features cache key, terrain inputs, rules version) is unchanged; `.state/bridges.json` marker with stats
  (`input`, `kept`, `dropped_low`, `dropped_ungrounded`, `dropped_no_height`).
- **No `[bridges]`:** no `features/bridges/`, and every other layer byte-identical to today. The buildings layer's
  "off" path currently deletes the whole `features/` directory: it must delete only `features/buildings/` (and
  bridges only `features/bridges/`).
- **`verify`:** the tileset's GLBs exist (as for buildings); the 3d-tiles-validator test covers a small bridges
  tileset; `verify --deep` rebuilds the decks' end heights from the terrain and compares them within 0.01 m.

### 1.5 Land cover under bridges

R2 skips bridges when burning roads into land cover ("no deck geometry before R3"). With `[bridges]`, bridge ways
are burned like other roads and rail (200 / 201 / 210), kept or dropped low. Effects:

- **Thermal IR:** `ThermalCS` classifies a pixel by the land cover at its x, y, whatever its height, so the deck reads
  asphalt / concrete / rail. Without the burn a deck over a river would read as water: the water/land crossover is one
  of the strongest IR cues, so the burn is required.
- **CIGI material codes:** a hit on the deck reports the cell's code (200 / 201 / 210), as a terrain hit does.
- **Vegetation:** no trees under or through the deck (the burned cells are road codes).
- **Known limit:** the strip *under* the deck also reads as road in IR (seen only obliquely, under the deck).
  Documented.

The land-cover settings record `bridges: true`, so its inputs hash changes only for packages with `[bridges]`.

### 1.6 Runtime

- **Config:** `scene.bridges.enabled` (true), `scene.bridges.maximum_screen_space_error` (64),
  `scene.bridges.maximum_cached_bytes_mb` (128), `scene.bridges.create_physics_meshes` (true; CIGI and the DIS clamp
  need it), each with a `CAMSIM_SCENE_BRIDGES_*` env override and validation like the buildings'. `ScenePackage`
  fills `Cfg.Scene.BridgesUrl` from `features/bridges/tileset.json`; the offline check covers it.
- **Spawn:** the buildings spawn becomes one helper used twice (`SpawnFeatureTileset(World, Cfg, Tag, Url, Sse,
  CacheMB, Physics)`); bridges tag `CamSimBridges`, spawned in `SetUpCesiumWorld` right after the buildings.
- **"Not terrain":** `IsFeatureTilesetActor` (buildings or bridges) replaces `IsBuildingsActor` where the question is
  "is this the terrain tileset" (`FindTerrainTileset`'s fallback, the camera's SSE stat, the streaming controller's
  culled SSE rule). `IsBuildingsActor` stays where buildings specifically matter (material 230, the vegetation
  exclusion, the DIS probe's ignore list).
- **`/ready`** waits for the bridges tileset as it does for the buildings one (it waits on every cached tileset; a test pins it).
- **CIGI HAT/HOT and LOS:** the bridges tileset is hit like terrain. HOT over a deck is the deck top.
- **DIS clamp:**
  - **Ground vehicles** may drive onto a deck but are never captured by one above them. `ISurfaceProbe::TraceHeight`
    takes a deck ceiling: deck hits count only at or below it. `PlaceOnSurface` passes the last surface height +
    `DeckStepUpM` = 3.0 m once the entity has a surface (US minimum vertical clearance under a bridge is 4.9 m, plus
    the 1.5 m slab), and no ceiling for the first placement (the first trace takes the topmost surface, as today).
    `FCesiumSurfaceProbe` traces twice: terrain only (buildings and bridges ignored) over the span, and everything but
    the buildings over the span cut at the ceiling; the higher hit wins. With no bridges tileset it traces once, as
    today.
  - **Surface vehicles** (boats) never take a deck (ceiling = −∞): a boat under a bridge stays on the water.

### 1.7 Acceptance (`scripts/bridges_check.py`, Camp Pendleton package rebuilt with `[bridges]`)

| Gate | What | Pass |
|---|---|---|
| B1 build | Layer builds; validator clean; rebuild byte-identical; a package without `[bridges]` byte-identical to today | all |
| B2 deck | CIGI HOT (extended) at mid-span of the 10 highest-clearance decks | within 0.10 m of the built deck top; material 200 / 201 / 210 |
| B3 ends | CIGI HOT at each grounded end of those decks vs HOT 5 m beyond it on the approach | within 0.5 m |
| B4 drive over | DIS truck driven along a motorway bridge (both directions), COCO `geo.alt_m` every frame | on the deck mid-span (within 0.3 m of B2's HOT); largest frame-to-frame change ≤ 0.3 m |
| B5 drive under | DIS truck on a road passing under a bridge | stays on the terrain (≥ 3 m below the deck) |
| B6 boat | DIS boat on water under a bridge, if Pendleton has one over water reachable from the sea | stays at sea level (else recorded as not testable) |
| B7 bench | `dense_pass` and `coast_pass` bridges on vs off | game thread p50 Δ ≤ 0.3 ms; 30 fps; 0 dropped |
| B8 shots | EO + MWIR oblique and nadir of 3 bridges | written for the user's visual review (not a pass/fail) |

Plus the C++ unit tests (probe ceiling rules, config, spawn/tag helpers), the Python tests (graph heights, sweep
geometry, tileset, config, land-cover burn rule, the `features/` delete fix) and the full suites.

## 2. Ocean ripples on the sim clock

- **Phase on the CPU:** the three ripples' temporal phases φ_i = −ω_i t mod 2π (ω_i = √(g k_i), k_i = 2π / λ_i,
  λ = 0.6 / 1.3 / 2.7 m) are computed in double from the sim clock (`FOceanWaves`, the clock the Gerstner waves use)
  and written to a new MPC vector `RipplePhase` (x, y, z). Wrapping on the CPU keeps float precision at any sim time
  and makes the ripple a pure function of sim time: frozen with the clock, paused when the sim pauses, and identical
  across runs.
- **Shader:** `CamSimOceanRippleGradient` takes the phase vector instead of `TimeS`
  (`Th = k (d · P) + Phase[i]`); `CamSimOceanPixelNormal` / `CamSimOceanRoughness` likewise (roughness does not use
  time today; its parameter goes). The ripple constants stay in `CamSimOcean.ush`; the C++ copy is held equal to it by
  a test that parses the `.ush`.
- **Assets:** `make_ocean_material.py` drops the `Time` node and reads `RipplePhase` from the MPC;
  `make_ocean_material.sh` finds the Linux editor like `run.sh` and runs under Xvfb on Linux (it was macOS-only); the
  regenerated `M_Ocean` / `MPC_Ocean` are committed. No hand editing.
- **Tests:** `CamSim.Ocean.Waves.RipplePhase` (wrap, ω from λ, phase at t and t + 2π/ω equal, frozen clock constant),
  `CamSim.Ocean.RippleConstantsMatchShader`, and `CamSim.GPU.Ocean.*` still passing.
- **Acceptance:** `determinism_check.py` before / after on `coast_waves`: the cross-run difference drops, and with the
  clock frozen two frames a second apart are identical in the sea region (mask: water pixels). With the clock running
  the ripples still move (frame-to-frame difference in the sea region > 0).

## 3. Documentation

`docs/scene-packages.md` ("Bridges"), `docs/configuration.md` (`scene.bridges.*`), `docs/dis.md` (deck ceiling),
`docs/ocean` notes in ROADMAP, CLAUDE.md (commands table and a Bridges gotcha), REALISM R3 status, ROADMAP (bridges
done / ripples done, the determinism follow-up updated), `docs/testing.md` rows and counts.

## 4. Out of scope

Bridge types (suspension, truss, arch) and their structure; bridges not in OSM; tunnels; buildings-style imagery
textures on decks; the Golden Gate's towers. The deck ceiling's first placement (a vehicle spawned under a bridge
lands on the deck) is accepted.
