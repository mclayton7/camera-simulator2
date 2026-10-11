# Buildings thin slice (REALISM R3-lite) — design

2026-10-10. Scene tooling (`scripts/scene`, a new `buildings` layer) plus a runtime change (one extra Cesium tileset from
the scene package, config, a CIGI material code, the DIS clamp's ignore list). No shader change, no editor work.
Realism/cost measurement: `2026-10-10-r3r4-realism-cost-bench-design.md`. Sibling slice:
`2026-10-10-r4-trees-slice-design.md` (the two are implemented interleaved).

## Goal

Put buildings into scene packages and into the rendered world at a cost we have measured, with the physics that CIGI
hosts depend on. This is a thin slice of REALISM R3: extruded footprints with flat roofs and one neutral material, so the
realism/performance trade can be measured before investing in roofs, facades, bridges and thermal classes.

Scope (agreed 2026-10-10): visible in EO; CIGI LOS and HAT/HOT hit buildings; the DIS clamp behaves; within the bench
budget. Deferred to the deepening pass: thermal class per building, ground-truth occlusion checks, bridges, gabled roofs,
LiDAR heights, facade/roof materials.

Success, on the Camp Pendleton `sim` package (M1 Pro / Metal):

- The package carries `features/buildings/` (3D Tiles 1.1) built from an Overture snapshot; rebuilds are byte-identical.
- Buildings render on the package terrain without floating or sinking visibly, in a neutral grey.
- CIGI LOS through a building hits its wall; HAT on a roof returns the roof; the material code says "building".
- A DIS truck on a road beside buildings stays on the road.
- The `dense_pass` sweep fits the budget at the chosen defaults, and `scene.buildings.enabled: false` is R2 bit for bit.

## Findings this rests on

- **Overture at Pendleton** (`docs/realism/data-sources.md` §4): 81,825 buildings in the bbox, read with DuckDB over S3 in
  ~80 s; 73 % have a height (inside the base OSM heights are dense). Overture's buildings theme is ODbL as a whole and
  releases are deleted after 60 days, so the source must be snapshotted and hashed (principle 3).
- **Cesium handles feature tilesets** (`docs/realism/cesium-findings.md` §1, §4): `file:///` 3D Tiles load like the
  terrain; `CreatePhysicsMeshes` gives collision; CamSim's complex `ECC_Visibility` traces (CIGI query handler, surface
  probe) hit any tileset with physics meshes, with no code change. Metadata from a hit is available through
  `UCesiumMetadataPickingBlueprintLibrary` (not used in this slice).
- **The tileset cache drives readiness and LOD.** `ApplyCesiumBackendConfig` destroys every tileset but the terrain at
  world init. `UCamSimSubsystem::RefreshCachedTilesets()` feeds `FCamSimStreamingController::UpdateTerrainGate` (the
  `/ready` gate, from load progress) and `UpdateLevelOfDetail` (FOV-scaled culled SSE). A tileset added to the cache is
  covered by both.
- **The DIS clamp's narrow span is last height +50 m / −500 m** (`Entity/SurfaceProbe.cpp`), with four footprint traces
  (bow, stern, port, starboard). Overture footprints are misregistered by a few metres in places, so a truck on a road
  beside a building could put one footprint point inside a footprint and tilt or jump onto the roof.
- **Material codes** (R2, `CamSimLandCover::QueryMaterialCode`): 0 no hit or an entity, 80 sea surface, else the
  land-cover code under the hit. Codes 200–220 are taken; 230 is free.

## Design

### 1. Source `overture_buildings` (scene tooling)

- New source in `scripts/scene/camsim_scene/sources/overture_buildings.py`. Reads the buildings theme of a pinned Overture
  release (`release = "2026-09-23.1"` in config) over the package bbox with DuckDB (MIT, new dependency; macOS universal2
  and manylinux wheels), selecting `id`, `geometry`, `height`, `num_floors`, `class`, `subtype`, `sources`.
- Snapshot: the query result is written as GeoParquet into the fetch cache, keyed by (release, bbox), with its sha256 in
  the manifest's `sources[]`. A rebuild reads the snapshot, never S3; a missing snapshot with a deleted release is a clear
  error naming the release.
- Licence: `ODbL-1.0` (attribution "© OpenStreetMap contributors, Overture Maps Foundation" plus Overture's listed
  co-sources). Needs `allow = ["ODbL-1.0"]`, as R2's OSM features do.

### 2. Config (package toml)

```toml
[buildings]                 # REALISM R3 slice
enabled = true
release = "2026-09-23.1"    # Overture release; snapshotted on first fetch
min_area_m2 = 10            # drop slivers
```

Absent section = no buildings layer (packages without it are unchanged byte for byte).

### 3. Footprints to buildings

Per building, in a local metric frame:

- **Clean:** `shapely.make_valid`, keep polygons ≥ `min_area_m2`, drop duplicate Overture ids. Multipolygons become one
  building with several parts.
- **Height:** Overture `height` → `num_floors × 3.2 m` → 4.0 m default; clamped to [2.5, 150] m. `height_source` ∈
  {`overture_height`, `overture_floors`, `default`}.
- **Base:** min over the footprint's vertices (and centroid) of the package's bare-earth DEM, as WGS-84 ellipsoid height,
  sampled through the same height path the terrain layer uses (`point_heights`), minus 0.5 m so walls reach the ground on
  slopes. Roof = base + 0.5 m + height.
- **Mesh:** roof by `shapely.constrained_delaunay_triangles` (holes kept); walls as one quad per ring edge, outward
  normals (CCW exterior / CW holes enforced), flat shading (duplicated vertices per face). No floor (never seen; traces
  from below are not a use case). Roof colour linear (0.30, 0.30, 0.31), walls (0.42, 0.41, 0.40) as glTF
  `baseColorFactor`, metallic 0, roughness 0.9.

### 4. Tiles

- **Layout:** `features/buildings/tileset.json` (3D Tiles 1.1, explicit tree) + `features/buildings/<z>/<x>/<y>.glb`, on
  the geographic tiling scheme of the package's terrain layer (EPSG:4326, two root tiles; a z13 tile is 0.022°, ~2.4 km
  N-S, a z15 tile ~0.6 km).
- **Size tiers with additive refinement:** a building lives in exactly one tile, chosen by footprint area: ≥ 2,000 m² at
  z13, ≥ 300 m² at z14, the rest at z15; the tile is the one containing the footprint centroid. Parents are `ADD` refined,
  so large buildings stay visible from far away and small ones load close in. Empty tiles are omitted; the root covers the
  bbox. Each tile's bounding volume is a `region` from its buildings' real extents (min base, max roof).
- **Geometric error** per level: z13 = 60 m, z14 = 20 m, z15 = 6 m, leaves 0; root = 200 m. These are starting values the
  SSE sweep evaluates; changing them is a package rebuild, changing `maximum_screen_space_error` is not.
- **GLB per tile:** positions in a local ENU frame at the tile centre (Y-up glTF, the 3D Tiles y-up → z-up convention),
  the tile's `transform` = inv(parent's world transform) × ENU→ECEF at that centre (3D Tiles transforms compose down
  the tree; tiles without content have none). Positions float32 relative to the centre (sub-mm at 1 km).
- **Metadata:** `EXT_mesh_features` (one feature id per building, per vertex) + `EXT_structural_metadata` property table
  `building` {`class` string, `height_m` float32, `height_source` enum, `source_id` string}. Written now so the thermal
  deepening pass needs no rebuild; not read at runtime in this slice.
- **Writer:** in-house (`camsim_scene/gltf.py`, numpy + struct), no new native dependency. Deterministic: buildings sorted
  by Overture id, fixed float formatting in JSON, no timestamps.

### 5. Verify

- `verify` (default): every `.glb` listed in `tileset.json` exists and hashes match the manifest.
- `verify --deep`: decode every tile; building count and ids match a recomputation from the snapshot exactly, roof heights within
  0.01 m (float32 positions relative to the tile centre); bounding regions contain their content.
- Tests run CesiumGS `3d-tiles-validator` (pinned npm package, Node is already used for `check.js`) on a small fixture
  tileset; skipped if Node is missing.

### 6. Runtime

- **Package resolution:** `CamSimScene::ResolvePackage` sets `Cfg.Scene.BuildingsUrl` (file:/// URL) when
  `features/buildings/tileset.json` exists.
- **Config** (`scene.buildings`, `docs/configuration.md`, each with an env override):

  | Key | Env | Default |
  |---|---|---|
  | `enabled` | `CAMSIM_SCENE_BUILDINGS_ENABLED` | `true` (only acts when the package has buildings) |
  | `maximum_screen_space_error` | `CAMSIM_SCENE_BUILDINGS_MAXIMUM_SCREEN_SPACE_ERROR` | from the sweep (start 16) |
  | `create_physics_meshes` | `CAMSIM_SCENE_BUILDINGS_CREATE_PHYSICS_MESHES` | `true` |

- **Spawn:** at world init (`PrepareCesiumWorld` → `SetUpCesiumWorld`), right after `ApplyCesiumBackendConfig`'s
  destroy loop, one `ACesium3DTileset` tagged `CamSimBuildings` is spawned: source `FromUrl`, the buildings URL, the level's georeference, the
  config's SSE and physics, the same tuning (`ApplyCesiumTilesetTuning`) and mobility as the terrain, no raster overlays,
  then `RefreshCachedTilesets()`. So `/ready` waits for buildings too (ATR frames never ship without them) and the
  culled-SSE scaling applies. `ValidateSources` accepts it under `scene.offline` (local `file:///` that exists).
  `SelectTerrainTileset` is unaffected: the buildings tileset is spawned after selection and is not an ion asset.
- **Lookup:** by actor tag (`CamSim::Geospatial::BuildingsTag`), since the surface probe holds only a `UWorld` and the CIGI
  handler only needs to classify a hit actor. The frame-stats `Sse` keeps reporting the terrain tileset.
- **CIGI material code:** in `QueryMaterialCode`'s caller, a hit whose actor is the buildings tileset reports **230 =
  building** instead of the land-cover code under the roof. `docs/terrain-feedback.md` code table gains the row.
- **DIS clamp:** `FCesiumSurfaceProbe::TraceHeight` adds the buildings tileset to `FCollisionQueryParams` ignored actors.
  Ground vehicles clamp to terrain; bridges (deferred) will be a separate tileset the clamp does hit.
- **Thermal:** unchanged. Building pixels take the land-cover class under them (WorldCover built-up 50 → the `BuiltUp`
  family), a known limit.

## Testing

- **Scene tooling (pytest, no network):** footprint cleaning (invalid, sliver, duplicate id, multipolygon); height
  fallbacks and clamps; base from a synthetic DEM on a slope; roof triangulation with a hole (area preserved, outward
  normals); tier assignment and centroid tile; GLB round-trip (positions back to lon/lat/height within 1 mm, feature ids,
  property table); byte-identical rebuild; ODbL gate (build fails without the allow entry); `verify --deep` catches an
  edited height and a missing tile; validator on the fixture (Node-gated). `--network` adds a live DuckDB query on a tiny
  bbox.
- **UE automation:** `ResolvePackage` finds/omits the buildings URL; config + env parsing; `ValidateSources` offline
  acceptance/rejection; material-code mapping (hit actor = buildings → 230, else unchanged); surface probe ignore list
  contains the buildings tileset when present.
- **GPU / live:** covered by acceptance below.

## Acceptance (Pendleton `sim`, macOS/Metal; Linux joins `FOR_LINUX.md`)

1. **Package:** `camsim-scene build` with `[buildings]` → `verify --deep` all ok; validator clean; rebuild identical;
   without `[buildings]` the package is byte for byte R2. Report building count, tiles per level, layer size and build time.
2. **Footprints vs imagery:** IoU of ~30 hand-checked buildings (Mainside, housing, a hangar) against NAIP outlines;
   report it, target median ≥ 0.6 (Overture + NAIP misregistration both count).
3. **Heights:** decode-and-compare exact (verify --deep); report the height-source mix.
4. **CIGI** (`send_cigi_test.py --los-extended / --hat-extended` at chosen buildings): LOS through a building hits the
   near wall (range within 1 m of the footprint edge) with code 230; HAT on a roof = roof within 0.5 m; HAT 5 m outside
   the footprint = ground.
5. **DIS:** `send_dis_test.py truck-loop` relocated to a Mainside road beside buildings: the truck stays on the road (no
   jump > 0.5 m between ticks); `dis_vehicle_check.py` and `gt_occlusion_check.py` still pass.
6. **Bench:** sweep per the bench spec; budget met at the chosen defaults; existing phases within noise.
7. **Off switch:** `scene.buildings.enabled: false` → shots and CIGI responses identical to R2.
8. **Visual review** by the user: EO shots from `dense_pass` and a low oblique over Mainside.

## Known limits (go into `docs/scene-packages.md`)

- Flat roofs, one neutral colour, no facades, no bridges.
- Thermal IR: buildings take the land-cover class under them.
- Footprints and heights only as good as Overture (OSM-mapped heights inside the base; 27 % default 4 m).
- Imagery shows roofs at their NAIP position; where Overture is offset, the roof texture and the building disagree.

## Out of scope

Bridges, gabled/hipped roofs, LiDAR nDSM heights, facade/roof materials and per-feature material assignment, thermal
class from metadata, building instance stencils, glTFRuntime actor loading (fallback only if the bench rules out Cesium).
