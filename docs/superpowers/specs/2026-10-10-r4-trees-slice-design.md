# Trees thin slice (REALISM R4-lite) — design

2026-10-10. Runtime only (a deterministic scatter around the camera) plus one CC0 tree in a vegetation library folder.
No scene-package change, no shader change, no editor work. Realism/cost measurement:
`2026-10-10-r3r4-realism-cost-bench-design.md`. Sibling slice: `2026-10-10-r3-buildings-slice-design.md` (implemented
interleaved).

## Goal

Put trees where the land cover says tree cover, deterministically, at a cost we have measured. This is a thin slice of
REALISM R4: one species, density from the land-cover tiles already in every package, so the realism/performance trade can
be measured before investing in canopy rasters, species, shrubs and impostors.

Scope (agreed 2026-10-10): visible in EO, deterministic, grounded, within the bench budget. Trees get no collision (CIGI
and the DIS clamp ignore them). Deferred: canopy % / height rasters (USFS TCC, CHMv2), several species and shrubs,
distance fade and impostors, mapped individual trees (OSM `natural=tree`), ground-truth occlusion checks.

Success, on the Camp Pendleton `sim` package (M1 Pro / Metal):

- Trees stand on the terrain wherever WorldCover is tree cover (code 10), and nowhere else.
- The same 100 m cell always holds the same trees, whatever path the camera took to get there.
- The `dense_pass` sweep fits the budget at the chosen defaults; `vegetation.enabled: false` is R2 bit for bit.

## Findings this rests on

- **Land cover is already in every package** (`landcover/`, 600×600 uint8 PNG tiles of 0.05°, 9.28 m × 7.77 m texels at
  33° N; `Thermal/LandCoverTiles.h` decodes them). R2 burns roads, rail and water over WorldCover, so code 10 is never
  under a road.
- **Code 10 is already `tree_canopy`** in thermal (`Thermal/LandCoverClasses.cpp`, `Vegetation` family), so trees in IR
  get the right class with no thermal work: the pixel's class comes from the land cover under it, which is tree cover.
- **`FLandCoverCodeLookup` decodes tiles on the game thread (2–4 ms per tile)**, budgeted for CIGI queries; the scatter
  must not share that budget. The thermal `FLandCoverWindow` exists only in IR. The scatter reads tiles itself, on a
  worker.
- **Cesium 2.29 has `ACesium3DTileset::SampleHeightMostDetailed`** (async, batched, loads the finest tiles for the
  queried points), so tree bases need no game-thread line traces and don't float when coarse tiles refine.
- **Entities already load glTF at runtime** (`Entity/EntityMeshLoader.h`, glTFRuntime), and survive origin shifts with
  `UCesiumGlobeAnchorComponent`.
- Scale check: radius 1 km = 3.1 km². At the default density (one tree per 240 m², below) a view that is all tree cover
  holds ~13k instances; the candidate grid caps it at one per 36 m² (~87k). Pendleton's riparian corridors are a small
  fraction of a view, but density and radius are the main cost knobs.

## Design

### 1. Vegetation library

- `unreal_project/CamSimTest/Content/NonUFS/Vegetation/` (git LFS, staged like `NonUFS/LandCover`; add to the staging
  list if not already covered), overridable with `vegetation.library_dir` (absolute) so a user can mount their own.
- `library.json`:

  ```json
  {"format": "camsim-vegetation-1",
   "types": [{"name": "tree_broadleaf_a", "gltf": "tree_broadleaf_a.glb", "height_m": 9.0,
              "thermal_class": "tree_canopy", "licence": "CC0-1.0", "source_url": "...", "sha256": "..."}]}
  ```

  `height_m` is the model's real height after any rescale; the loader checks the sha256 and refuses a mismatch.
- **The slice's tree:** one CC0 model (Poly Haven or another CC0 source; licence checked on the source page and recorded),
  broadleaf to suit coastal live oak / riparian woodland, ≤ 10k triangles (decimated offline if needed, the script
  committed), leaves solid or alpha-masked cards. Attribution is not required for CC0 but the source is listed in
  `ATTRIBUTION`/docs anyway.
- **First plan task (risk):** load the candidate through `CamSimMeshLoader::LoadStaticMesh` into a HISM in a live run and
  check the masked-leaf material and shadows look right. If no CC0 model qualifies, fall back to a procedural trunk +
  ellipsoid crown generated in C++ (same `library.json` entry with `"procedural": true`), so the slice is never blocked.

### 2. Cells and placement (pure)

- **Grid:** global geographic cells of `cell_size_m` (default 100 m), defined in angle so the id never depends on the
  camera: Δlat = cell_size_m / 111,320; Δlon fixed per latitude row = cell_size_m / (111,320 · cos(row centre lat)).
  `FVegetationCellId {int32 Row; int32 Col}`.
- **`GenerateCell(Seed, CellId, Params, Sampler) → TArray<FTreeInstance>`** (`Vegetation/VegetationScatter.h`, pure, no
  UObjects): a jittered grid of candidates at spacing `s` = 6 m (one candidate per s²); candidate *i* gets
  `h = PCG(Seed, Row, Col, i)` (the `SensorHash.h` hash) for jitter (±s/2 in each axis), acceptance, yaw (0–360°) and
  scale (0.8–1.2). Accepted if `Sampler(lat, lon) == 10` and `u(h) < density_scale × p_tree`, with p_tree = 0.15 (≈ one
  tree per 240 m² at full density, open-canopy oak woodland; tuned by the sweep). Output: lat, lon, yaw, scale, type 0.
- **Sampler:** `FVegetationLandCover` reads land-cover tiles with the existing decoder on a worker, a small LRU of decoded
  tiles (16 tiles ≈ 5.8 MB) owned by the scatter.
- **Seed:** the package manifest's `seed` mixed with a constant for vegetation; `vegetation.seed` when there is no
  package.

### 3. Runtime streaming (`Vegetation/VegetationManager`)

- Owned by `UCamSimSubsystem`, ticked on the game thread after the camera pose update.
- Each tick: the set of cells whose centre is within `radius_m` of the camera's ground point is wanted; cells beyond
  `radius_m × 1.2` are dropped (hysteresis). New cells are queued nearest first.
- **Pipeline per cell:** worker task `GenerateCell` → `SampleHeightMostDetailed` on the terrain tileset for the
  instances' lon/lat (one batch per cell) → game thread: spawn a cell actor with a `UCesiumGlobeAnchorComponent` at the
  cell centre and one `UHierarchicalInstancedStaticMeshComponent`, `AddInstances` with transforms in the cell's
  East-South-Up frame (`CamSimFrames`), so cells survive origin shifts. Failed height samples drop that instance.
- **Budget:** at most `MaxCellsSpawnedPerTick` = 2 cell actors created per tick, and at most 4 cells in flight in the
  worker/height stage (constants, in the style of `FLandCoverCodeLookup::MaxLoadsPerTick`), so game-thread p95 stays
  flat while flying.
- **Render settings:** collision off, `bCastShadow = cast_shadows`, `InstanceEndCullDistance = radius_m` (hard cut, no
  fade in the slice), no custom depth or stencil (stencils stay for entities). Trees are opaque scene geometry, so they
  occlude entities in the ground-truth instance mask automatically.
- **Not hit by queries:** collision is off, so CIGI LOS/HAT/HOT and the DIS clamp never see trees.
- **Readiness:** `/ready` does not wait for trees (the gate is terrain + buildings); trees fill in within the first
  seconds. Noted as a known limit; revisit if ATR runs need it.
- No land cover configured (no package, no `thermal.land_cover.dir`) → vegetation stays off, one log line.

### 4. Config (`vegetation`, `docs/configuration.md`, each with an env override)

| Key | Env | Default |
|---|---|---|
| `enabled` | `CAMSIM_VEGETATION_ENABLED` | `true` |
| `library_dir` | `CAMSIM_VEGETATION_LIBRARY_DIR` | `""` (built-in `NonUFS/Vegetation`) |
| `radius_m` | `CAMSIM_VEGETATION_RADIUS_M` | from the sweep (start 1000) |
| `density_scale` | `CAMSIM_VEGETATION_DENSITY_SCALE` | from the sweep (start 1.0) |
| `cast_shadows` | `CAMSIM_VEGETATION_CAST_SHADOWS` | from the sweep (start `true`) |
| `cell_size_m` | `CAMSIM_VEGETATION_CELL_SIZE_M` | 100 |
| `seed` | `CAMSIM_VEGETATION_SEED` | 0 (used only without a package) |

## Testing

- **UE automation (NullRHI, pure code):**
  - Determinism: `GenerateCell` for a 5×5 block of cells is byte-identical generated in row order, reverse order and a
    random order, and across two "camera paths" driving the manager's wanted-set logic.
  - Density: on a synthetic sampler (half code 10, half code 30), accepted instances on code 10 are within ±10 % of
    `density_scale × p_tree / s²` per area over 100 cells; zero on code 30.
  - Cell grid: id round-trip lat/lon → cell → bounds; neighbours share edges; no gaps at row changes.
  - Wanted/drop sets with hysteresis; per-tick spawn budget respected.
  - Config + env parsing; library.json parsing (sha mismatch refused, procedural entry accepted).
- **Live:** acceptance below.

## Acceptance (Pendleton `sim`, macOS/Metal; Linux joins `FOR_LINUX.md`)

1. **Placement:** over the Santa Margarita riparian corridor, every instance sits on a code-10 texel (checked by a debug
   dump of instance lat/lon against the tiles); a reference shot shows trees following the WorldCover tree areas.
2. **Grounding:** 200 instance bases vs a complex terrain trace after the view settles: |dz| < 0.5 m for all.
3. **Determinism live:** the instance dump for one cell is identical after reaching it from the north and from the
   south.
4. **Origin shift:** `far_origin` and a return to Pendleton: trees stay put (shot diff at a fixed pose before/after).
5. **Ground truth:** `gt_occlusion_check.py` still passes with vegetation on; a DIS truck parked under trees has
   visibility < 1 and a correct box (reported, not gated, in the slice).
6. **Bench:** sweep per the bench spec; budget met at the chosen defaults; existing phases within noise.
7. **Off switch:** `vegetation.enabled: false` → shots identical to R2.
8. **Visual review** by the user: `dense_pass` contact sheet and a low oblique over the riparian corridor.

## Known limits (go into `docs/scene-packages.md` or a new `docs/vegetation.md`)

- One species; density is tree / no tree per 10 m texel (no canopy %), so woodland edges follow the land-cover grid.
- Hard pop-in at `radius_m`; no impostors beyond it, the imagery shows distant trees.
- Trees have no collision: CIGI LOS passes through them, DIS ignores them.
- `/ready` does not wait for trees.
- Leaf-on all year (NAIP is leaf-on too).

## Out of scope

Canopy % and height rasters (USFS TCC, CHMv2, LiDAR CHM), species by region, shrubs and grass, impostors and distance
fade, mapped trees and tree rows from OSM, poles/fences/clutter, PCG graphs, per-instance thermal state, tree collision.
