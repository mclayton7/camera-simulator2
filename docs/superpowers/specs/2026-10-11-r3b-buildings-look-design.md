# Buildings that look and behave right (REALISM R3b) — design

2026-10-11. Builds on the R3 buildings and R4 trees thin slices (`2026-10-10-r3-buildings-slice-design.md`,
`2026-10-10-r4-trees-slice-design.md`) and their bench (`2026-10-10-r3r4-realism-cost-bench-design.md`), branch
`realism-r3r4-slices` (PR #12).

## Goal

Two follow-ups the R3/R4 acceptance left open, done together because both start from the building footprints at
package build time:

1. **Trees stay out of buildings without a runtime cost.** The R3/R4 fix samples the buildings tileset under every
   tree (`vegetation.drop_in_buildings`). It costs about +2.5 ms on the slowest 5 % of game-thread frames and
   +34 MiB, so the both-on game p95 is +3.61 ms, over the 3.0 ms budget. The user accepted the overrun on
   2026-10-11, provided it is moved to build time.
2. **Roofs look like the roofs in the imagery.** R3 roofs are one neutral grey, which hides the real red-tile, white
   and dark roofs that the imagery shows. The target views are low-altitude obliques (≤ 500 m, typical UAS / ATR
   training; user, 2026-10-11), where roof detail such as vents, panels and tile pattern is visible.

A side effect that is wanted: infrared roofs stop taking the class of the land cover under them. Today a roof over a
WorldCover tree-cover cell renders as tree canopy in IR.

Decisions agreed with the user (2026-10-11):

- **Footprints:** both a fine building mask, read by trees, and a land-cover burn, which gives infrared and CIGI a
  building code.
- **Roofs:** a packed roof texture atlas per building tile, cut from the imagery sources at build time (approach A).
  Not a runtime raster overlay, and not one flat colour per building.

Success, on Camp Pendleton (M1 Pro / Metal), with both features on at their defaults:

- No tree inside a footprint plus clearance, and no runtime buildings sampling for packages that carry the mask.
- Game-thread p95 back within +3.0 ms of all-off, and memory within +1,024 MiB over a whole run.
- Low-rise roofs in close obliques match the ground imagery within 1–2 m.
- In MWIR, a roof is hotter than nearby grass at noon and cooler than air at 02:00.

## Findings this rests on

- **Land cover** is 0.05° tiles of 600 × 600 uint8 PNG (9.28 m × 7.77 m texels at 33° N).
  - R2 burns OSM/NHD features into them in `features.burn_tile` (`scripts/scene/camsim_scene/features.py`), with
    `PRIORITY = {water:0, paved_area:1, rail:2, road:3}`.
  - Burns use the centre-sample rule, and later burns win.
  - `RULES_VERSION` is part of the hash of the derived-features cache and of the landcover layer.
  - `index.json` gains `codes`/`credits` only when features are configured, so packages without features stay
    byte-identical.
- **Buildings** (`buildings.py`, `gltf.py`, `layers/buildings.py`) are size-tiered tiles, not levels of detail:
  - z13 holds buildings ≥ 2000 m², z14 ≥ 300 m², z15 the rest; each building is in exactly one tile.
  - Each tile has its own ENU origin.
  - Tiles are non-indexed triangle soup, with `POSITION`, `NORMAL` and `_FEATURE_ID_0` only.
  - Each tile has two flat-colour materials: roof `(0.30, 0.30, 0.31)` and wall `(0.42, 0.41, 0.40)`.
  - Roofs are shapely constrained Delaunay triangulations lifted to `roof_m`.
  - The layer depends only on terrain and the Overture snapshot.
- **Imagery** (`sim` profile):
  - Sources: NAIP 2022 via Planetary Computer, then Sentinel-2 balanced to NAIP, then BMNG.
  - Geographic TMS of 256 px JPEG tiles, max zoom 17, which is about 0.50 m/px east-west and 0.60 m/px north-south
    at Pendleton.
  - `imagery.sample_entries(entries, lon, lat, target_m)` samples the source rasters at points. Nothing reads built
    JPEGs back.
- **Cesium for Unreal 2.29's default material** (`M_CesiumBaseMaterial`) binds a glTF `baseColorTexture` and its
  texcoord index (`CesiumGltfComponent.cpp`). It probably ignores `COLOR_0`, so per-vertex colours would need a
  custom editor material.
  - `SceneBuildings.cpp` never sets a material on the buildings tileset.
- **Thermal** (`Thermal/LandCoverClasses.cpp`):
  - 15 built-in classes; at most 32.
  - Code 230 is not in the code table today (it maps to `terrain_default`).
  - `ThermalCS` treats building pixels as terrain and classifies them by the 10 m land-cover cell under the roof or
    wall hit point. Building geometry carries no stencil.
- **CIGI** (`Thermal/LandCoverLookup.cpp`, `QueryMaterialCode`): a hit on the buildings tileset reports 230. A terrain
  hit reports the stored land-cover byte at the hit or request point, so a burned 230 is reported verbatim.
- **Trees** (`Vegetation/VegetationScatter.cpp`) accept land-cover code 10 only.
  - The runtime footprint check is `CamSimVegetation::KeepClearOfBuildings`, wired in
    `VegetationManager.cpp` (`SampleBuildingHeights`) and gated by `vegetation.drop_in_buildings`.

## Design

### 1. Footprint mask and land-cover burn (build time)

**One raster, two outputs.** A shared, cached build step takes exactly the buildings the buildings layer keeps (same
filter, same `source_id` order) and rasterises their footprints once. The mask and the burn both derive from this
raster, so they cannot disagree.

1. **Fine mask:** `features/buildings/mask/`.
   - Tiles share the land-cover tile grid and naming: 0.05° tiles named by SW corner, row 0 = north.
   - Each tile is 4800 × 4800 (8 × the land-cover resolution, about 1.16 m × 0.97 m per cell at 33° N).
   - Cells are 1-bit PNG (mode `1`), set where the cell centre is inside a footprint, holes excluded.
   - A tile with no set cell is not written.
   - `index.json` lists the written tiles, the resolution and the rules version.
2. **Land-cover burn:** a land-cover cell becomes code **230** when at least half of it is covered, i.e. at least 32 of
   its 8 × 8 mask cells.
   - 230 is the code CIGI already reports for a building hit, so terrain hits inside footprints say "building" too.
   - Priority (later wins): WorldCover, then water, then paved areas, then **buildings**, then rail, then roads.
     Buildings overwrite paved areas (parking structures); roads and rail overwrite buildings, because a road is
     usually right where a footprint is misregistered.
   - `index.json` `codes` gains 230 when it is present.

**Determinism and hashing.**

- Packages without `[buildings]` stay byte-identical to R3. The landcover layer reads buildings only when the
  buildings layer is configured.
- `BUILDINGS_RULES_VERSION` and the landcover `RULES_VERSION` are bumped.
- The landcover inputs hash gains the buildings snapshot sha, the buildings layer settings hash and the buildings
  rules version.
- The mask and burn are byte-identical across rebuilds: deterministic rasterisation, PNG `optimize=True`.

### 2. Roof texture atlas (build time)

**Scope.** Every buildings tile, at every tier, gets one roof atlas. Walls keep the flat wall colour.

**Pixel source.**

- Roofs are sampled from the same source rasters and colour balance as the imagery layer, through
  `imagery.sample_entries`, not from the built JPEGs. That keeps roofs matched to the ground imagery and avoids
  compressing twice.
- The default texel is `buildings.roof_texel_m` = 0.5 m.
- A roof whose best source is coarser than 2 m (no NAIP, only Sentinel-2 or BMNG) keeps the flat grey roof material.

**Packing.**

- Each roof's axis-aligned bounding box, in the tile's local ENU metres, becomes one rectangle with a 2 px gutter on
  each side. The gutter is filled by edge extension, so mipmaps do not bleed.
- A deterministic shelf packer places the rectangles. Order: rectangle height in pixels descending, then `source_id` ascending; ties
  cannot occur because `source_id` is unique.
- The atlas is square and a power of two, at most `buildings.roof_texture_max_px` (default 2048).
- If the rectangles do not fit, the tile's texel size steps up (0.5, 0.75, 1.0, 1.5, 2.0 m) until they fit. A roof
  that still does not fit at 2.0 m keeps the flat roof material.
- The chosen texel size is recorded per tile.

**Tile output.**

- Roof vertices gain `TEXCOORD_0` into the atlas; wall vertices get (0, 0). The arrays stay shared and non-indexed.
- The roof material becomes `pbrMetallicRoughness.baseColorTexture`: one embedded JPEG (`image/jpeg`, quality 85,
  4:2:0) with sampler mag LINEAR / min LINEAR_MIPMAP_LINEAR and wrap CLAMP_TO_EDGE. `baseColorFactor` is 1.
- Roofs that fell back keep a separate flat roof primitive and material, so a tile can have three primitives: textured
  roof, flat roof and wall.
- The tile's `extras` (or tileset metadata) gain `roof_texel_m`.
- The tiles pass 3d-tiles-validator 0.6.1.

**New config** (`[buildings]` in the package TOML; part of the buildings settings hash):

| Key | Default |
|---|---|
| `roof_textures` | `true` |
| `roof_texel_m` | `0.5` |
| `roof_texture_max_px` | `2048` |

### 3. Runtime and infrared

**Scene package.** `ScenePackage` exposes `BuildingsMaskDir` when `features/buildings/mask/index.json` exists.

**Trees.**

- `FVegetationBuildingMask` reads mask tiles on the scatter worker thread.
  - Decoded tiles are bitsets of about 2.9 MB each, held in an 8-tile LRU (about 23 MB), separate from the land-cover
    sampler.
  - A missing tile means no buildings.
- `GenerateCell` takes an optional `IsClear(lat, lon)` predicate. A candidate is rejected if its trunk, or any of 4
  points at `vegetation.building_clearance_m` east, west, north and south of it, is masked.
  - The hash streams per candidate do not change.
  - Without a mask, every cell hash is bit-identical to the R3/R4 build.
- Order of methods:
  - mask present: the mask is used, and no buildings tileset sampling runs;
  - buildings tileset present without a mask (older R3 packages): the runtime `KeepClearOfBuildings` check, as today;
  - neither: no exclusion.

  `vegetation.drop_in_buildings` (default true) gates both methods. The `Vegetation: on (...)` log line names the method.
- New config key:

  | Key | Env | Default | Range |
  |---|---|---|---|
  | `vegetation.building_clearance_m` | `CAMSIM_VEGETATION_BUILDING_CLEARANCE_M` | 2.0 | [0, 10] |

**Infrared.**

- New built-in thermal class `building_roof`, the 16th, leaving 16 of 32 free.
  - `DefaultMaterialName[230] = "building_roof"`.
  - `FamilyForCode(230)` = Bare. BuiltUp would replace the class with the asphalt/concrete refinement and hide `building_roof`; Bare keeps the roof class and only blends toward vegetation on green pixels (corrected while planning).
  - Parameters: a light, low-mass roof, hot at midday and below air temperature at night. The values are chosen in
    the plan with cited sources.
- It is class-table data only, with no shader change. `CamSim.GPU.Thermal.LandCoverMatchesCpu` and `MatchesCpu`
  still hold.
- Code 50 (WorldCover built-up) keeps `built_up` for the ground between buildings.
- `thermal.land_cover.enabled: false` leaves IR unchanged.

**CIGI.** No code change. Terrain hits inside burned cells now report 230. This is documented in
`docs/terrain-feedback.md` next to the building-hit rule.

**Textures.** No runtime change: Cesium's default material samples the embedded texture. The IR refinement already
reads GBuffer base colour, so roofs refine on their real colour.

### 4. Docs

The following are updated:

- `docs/scene-packages.md` "Buildings": mask, burn, atlas, config, known limits.
- `docs/vegetation.md`: mask method, clearance, fallback.
- `docs/thermal.md`: the `building_roof` class.
- `docs/configuration.md` and `deploy/camsim_config.yaml`: the new keys.
- `docs/terrain-feedback.md`: 230 on terrain.
- `CLAUDE.md`: gotchas and test counts.
- `ROADMAP.md` / `REALISM.md`: the R3b status. The ROADMAP item "build-time building-footprint exclusion" is closed.

## Testing

- **Scene tooling** (pytest, no network):
  - Mask: a known polygon (with a hole) sets exactly the expected cells, and a tile with no buildings is not written.
  - Burn: the half-cover rule at 31 vs 32 of 64; roads and rail over buildings; buildings over paved areas.
  - Hashing: a rebuild gives identical `hashes.txt`, and a package without `[buildings]` is byte-identical to R3.
  - Atlas:
    - deterministic packing;
    - overflow steps the texel size and records it;
    - a roof that cannot fit falls back to flat;
    - a coarse source falls back to flat.
  - Texcoords: against a synthetic imagery entry with a known pattern, each roof triangle's texels sample the expected
    source pixels within half a texel.
  - Validation: 3d-tiles-validator accepts textured tiles (no errors), and `verify --deep` checks mask ↔ burn
    consistency and the presence of `roof_texel_m`.
- **UE automation** (NullRHI):
  - mask decode at tile edges and corners;
  - the 5-point clearance rule;
  - no mask means unchanged cell hashes;
  - config parsing and validation;
  - 230 → `building_roof` / Bare;
  - class count 16;
  - existing thermal and vegetation tests pass.
- **Live acceptance** on a rebuilt `pendleton-r4`:
  1. **Trees:** the vegetation dump against the footprints shows 0 trees whose trunk or any of its 4 clearance points is masked, and 0 trunks inside a footprint shrunk by 0.76 m (half a mask cell diagonal); the count inside the clearance-buffered footprint is reported, not gated (5 probes cannot see footprint corners).
     `vegetation_check.py` placement, determinism and off-switch gates pass.
  2. **Roofs:** close obliques at Mainside and on the Oceanside dense path, with textures on and with
     `roof_textures = false`. Low-rise roof texture is aligned with the ground imagery within 1–2 m, judged from the
     shots.
  3. **Infrared:** in MWIR, a roof is hotter than nearby grass at noon and cooler than air at 02:00. The other
     `thermal_check.py` gates pass.
  4. **CIGI:** HAT/HOT on terrain inside a burned cell returns 230, and 10 m outside returns that cell's land-cover
     code.
  5. **Bench:**
     - a combined sweep (buildings × trees, off/on, against all-off);
     - a full Pendleton `run_bench.py --gpu-mem`.

     Gates: game p95 ≤ +3.0 ms, memory ≤ +1,024 MiB over the whole run, 30 fps and 0 dropped frames. If memory is
     over, rebuild once with `roof_texel_m = 1.0` and report both.
  6. **Visual review** by the user: roof close-ups, trees near buildings before and after, MWIR day and night.

## Known limits

- NAIP is not a true orthophoto, so tall buildings' roofs are offset by their lean. This is fine for low-rise; not
  corrected.
- Roof texture is only as good as NAIP: 0.6 m, one acquisition date, shadows baked in.
- The land-cover burn marks a 9 m cell as building only when at least half of it is covered, so very small buildings
  keep their WorldCover class in IR.
- Walls stay flat coloured; roofs stay flat (no pitch).

## Out of scope

Lean correction; wall/facade textures; pitched roofs and bridges; GPU-compressed textures (KTX2/Basis, a contingency
only if memory fails); re-tracing footprints for the IoU gate.
