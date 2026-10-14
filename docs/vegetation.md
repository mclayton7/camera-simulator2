# Vegetation (trees and shrubs)

REALISM R4. Instanced trees and shrubs scattered around the camera from land cover, a per-package species mix and, where
the scene package has it, CHMv2 canopy cover and height; loaded at runtime from a mounted vegetation library (glTF through
glTFRuntime) into HISMs. A near tier (full meshes, shadows) out to `radius_m` and a far tier (low-poly, thinned) out to
`far_radius_m`. Specs: the slice
[`superpowers/specs/2026-10-10-r4-trees-slice-design.md`](superpowers/specs/2026-10-10-r4-trees-slice-design.md) and
"deeper" [`superpowers/specs/2026-10-11-r4-vegetation-deeper-design.md`](superpowers/specs/2026-10-11-r4-vegetation-deeper-design.md)
(plans beside them in `superpowers/plans/`).

The same 100 m cell always holds the same trees and shrubs, whichever way the camera got there. Vegetation is scene
geometry only: visible in EO and IR (in IR the pixel takes the land cover's class: `tree_canopy` on 10, `shrubland` on
20, `grassland` on 30) and occluding entities in the ground-truth masks, but CIGI and DIS never see it.

## How it works

- **Grid** (`Vegetation/VegetationGrid.h`): a global geographic grid of `cell_size_m` cells (100 m),
  defined in angle so a cell id `(row, col)` never depends on the camera: Δlat = cell / 111,320 m, Δlon
  per latitude row = cell / (111,320 m · cos(row centre latitude)). No vegetation above 85° latitude.
- **Species mix** (`Vegetation/VegetationMix.h`): per land-cover code, the library types that stand there and their
  weights, trees and shrubs separately, plus shrubs per 100 m². From the package's `vegetation/mix.json` (its
  `[vegetation]` table, [`scene-packages.md`](scene-packages.md#vegetation-mix-r4)), else `vegetation.mix_file`, else the
  built-in default: 10 → `tree_broadleaf_a` (R4's trees bit for bit), 20 → `shrub_chaparral` at 6 / 100 m², 30 → at
  1 / 100 m². A type the library lacks is dropped with one warning (the code renormalised); with `scene.offline: true`
  CamSim exits 1. Names are taken in alphabetical order, so the JSON key order never matters.
- **Trees** (`Vegetation/VegetationScatter.h`, pure, `GenerateCellContent`): each cell is a jittered grid of
  `round(cell / 6 m)` = 17 candidates per axis (**real spacing 5.88 m**). Candidate *i* draws from
  `CamSimHash::Hash(col, row, i, seed, 160 + k)` (the PCG hash of the sensor noise, never a live RNG): jitter E/N inside
  its own sub-cell (so trees never cross cell edges), species (165, from the code's mix), acceptance, yaw, scale.
  - **With canopy** (the package's `canopy/` covers the point): no tree below 3 m canopy height or at no cover; else
    scale base s = clamp(height / type height, 0.5, 2) and p = clamp(cover × 34.6 m² / (π (r_crown s)²) × density, 0, 1),
    so the expected crown area per candidate equals the canopy cover. It **saturates** at one crown per 34.6 m²:
    π r² / 34.6 ≈ 0.66 cover for the 2.7 m broadleaf crown (≈ 290 trees/ha); closed forest above that is drawn at the
    cap. Scale s × (0.9–1.1). Trees stand on every code whose mix lists trees (oaks on grassland, street trees on 50).
  - **Without canopy** (no layer, `canopy_enabled: false`, or outside the package bbox): R4's rule, p = 0.15 × density
    (one tree per 231 m² of mix code) and scale 0.8–1.2.
- **Shrubs**: their own grid of `round(cell / shrub_spacing_m)` = 33 per axis (3.03 m), sub-streams 166–171 (jitter,
  accept, yaw, scale, species): accepted with p = shrubs per 100 m² / 100 × 9.18 m² × density on codes whose mix lists
  shrubs, scale 0.7–1.3. Shrubs ignore the canopy (CHMv2 doesn't resolve them) and may stand under trees. They come
  from the **shrub ring** (below), never from the near cells or the far tier.
- **Land cover and canopy at points** (`VegetationLandCover.h`, `VegetationCanopy.h`): the scatter reads the package's
  `landcover/` and `canopy/{cover,height}/` tiles itself, on a worker, each through its own 16-tile LRU of
  `FLandCoverTileCache` (never the CIGI lookup's game-thread budget). A bad tile reads as no data there; the caches'
  summary warnings reach the log once a second.
- **Seed:** the scene package manifest's `seed` mixed with a vegetation constant (`VegetationSeed`);
  `vegetation.seed` only when there is no package. Pendleton's package has manifest seed 0, which gives vegetation seed
  186258786 (as `vegetation.seed: 0` does).
- **Heights (near):** one `ACesium3DTileset::SampleHeightMostDetailed` batch per cell (trees then shrubs) on the terrain
  tileset (the one tagged `CamSimTerrain`, never the buildings tileset). A failed sample drops that plant only.
- **Plants stay out of buildings** (REALISM R3 / R3b; `vegetation.drop_in_buildings`, default true, gates all of it).
  Land cover alone would put ~10 % of footprint area under trees (WorldCover tags canopy over roofs as code 10: on
  Pendleton 10.4 % of the 23.0 km² of footprints, about 10,400 trees at density 1, and a 7–11 m tree would poke its
  crown through a 3–8 m flat roof). In order of preference:
  1. **Footprint mask** (a package built with R3b: `features/buildings/mask/index.json` exists):
     `FVegetationBuildingMask` decodes the 1-bit mask tiles on the scatter worker into bitsets (8-tile LRU, ~23 MB,
     separate from the land-cover sampler). `GenerateCellContent` drops a tree or shrub whose trunk, or any of 4 points
     `vegetation.building_clearance_m` (2 m) east, west, north and south of it, is masked (the 5-point rule). No
     runtime sampling; the hash draws are unchanged, so without a mask every cell is bit-identical. The probes
     are axis-aligned: a footprint corner diagonally within the clearance is not seen. It applies whether or not the
     buildings tileset is drawn (`scene.buildings.enabled`), since the roofs are in the imagery either way. A mask
     whose `index.json` is unusable logs one `Vegetation: building mask unusable (...); falling back` warning and the
     next method is used.
  2. **Runtime check** (an R3 package: buildings tileset, no mask): each cell's height stage runs a second
     `SampleHeightMostDetailed` batch on the tileset tagged `CamSimBuildings` for the same points, beside the terrain
     batch; both must answer (or the stage times out, as above) before the cell spawns. A plant whose buildings sample
     succeeds more than 0.5 m above its terrain height is dropped (`CamSimVegetation::KeepClearOfBuildings`); a failed
     sample is a miss and keeps it. Cesium loads the buildings' most detailed (z15) tiles under every point
     to answer it: in `dense_pass` it costs about +0.3 ms game p50, +2.5 ms game p95 and +34 MiB.
  3. Neither (no mask, no buildings tileset): no exclusion.
- **Bad mask tiles:** a tile that is listed but unreadable (a git LFS pointer, the wrong size, truncated, missing)
  reads as "no buildings" there and logs one `Vegetation: building mask tile ... unreadable (...); treated as no
  buildings` warning per tile. A failed tile is remembered and not retried, so restart after `git lfs pull`.
- **Blocks and tiers** (`Vegetation/VegetationBlocks.h`): streaming works in blocks of `far_block_cells` × cell size
  (400 m). A cell belongs to the block holding its centre, with block columns `far_block_cells` × the block row's
  middle-row column width (the grid's column widths differ per row, so cells drift row to row; this keeps blocks square
  anywhere): usually 4 × 4 cells, give or take a column. `FVegetationTierPlanner` makes a block **near** within
  `radius_m` of the camera (it stays near to 1.2 × `radius_m`), **far** within `far_radius_m` (it stays far to 1.1 ×
  `far_radius_m`, and turns near again only inside `radius_m`), else drops it; it replans after the camera moved 25 m
  along the ground or in height. Distances are **3D**, from the block centre to the camera at its height above the
  terrain (a nadir trace every 15 ticks, the frame centre's terrain until one hits), because HISM cull distances are
  3D: planned by ground distance, a camera 2 km up drew no near tree at all. So from 1.5 km up there are no near cells,
  and from 4.4 km up nothing is planned.
- **Near tier** (`Vegetation/VegetationCellActor.h`, `VegetationManager.h`): one actor per cell, globe-anchored at the
  cell centre in its East-South-Up frame (survives origin shifts), one HISM per type: trees with `cast_shadows`, culled
  at 1.2 × `radius_m` + a block diagonal (blocks are whole). Collision off, no custom depth or stencil. Trees take
  exact per-tree heights (one `SampleHeightMostDetailed` batch per cell).
- **Shrub ring**: the cells whose centre is within `shrub_radius_m` + half a cell diagonal (every cell overlapping
  the circle, in 3D like the tiers; kept to 1.2 × that) get a second, shrub-only actor (`GenerateCellContent` with trees off), one HISM per
  shrub type culled at `shrub_radius_m`, shadows as the trees. Heights come from a 20 m grid per cell (36 points,
  bilinear, sunk 0.15 m): Cesium answers every height point on the game thread, and ~1,000 shrubs per cell sampled one
  by one cost +8 ms game p95. Ring cells share the in-flight and spawn budgets (near cells first, then the ring, then
  far blocks). Earlier the near cells carried their shrubs out to `radius_m` and `shrub_radius_m` only culled them,
  which cost +1.9 ms game p50 in the dense pass whatever the shrub radius ("Bench").
- **Far tier** (`Vegetation/VegetationFar.h`, `VegetationFarTier.h`): one actor per far block. Its trees are the same
  `GenerateCellContent` trees of the block's cells (shrubs off), kept when `Uniform(Hash(col, row, k, seed, 172))` <
  `far_keep` (0.35) and enlarged by `far_scale` (1.3), drawn with the type's `far_gltf` (a procedural 108-triangle lathe
  crown + trunk, untextured), no shadows. Heights come from a 100 m grid per block traced on the **rendered** terrain's
  collision (`FRetryingTraceSampler`: no tile loading; misses retried every 30 ticks, 20 times; bilinear, trees sunk
  0.5 m; a tree whose grid quad has a failed point is dropped). `SampleHeightMostDetailed` there loaded the finest
  terrain tiles over the whole far disk (a 25 m grid cost several ms in `Cesium::loadTiles`, and even a 100 m grid
  slowed later passes, "Measured"); without `create_physics_meshes` the far tier still uses it. Trees are split by
  `Hash(..., 173)` into 4 HISMs culled at 0.7 / 0.8 / 0.9 / 1.0 × `far_radius_m`: the outer 30 % thins in four steps
  instead of ending in a hard line.
- **No gap at a switch:** a block switching tier keeps its old actors until the new tier has settled (its far actor
  spawned or found empty; all its near cells spawned or empty). A block that left both tiers drops at once.
- **Budgets:** at most 4 cells/blocks in their generate or height stage, and 2 actor spawns per tick, shared by both
  tiers, near first. A stage that never answers (Cesium drops the callback when a tileset is destroyed) times out after
  1800 ticks (60 s) and leaves that cell or block empty.
- **Readiness:** `/ready` does not wait for vegetation; it fills in within seconds of the terrain.
- Without land cover (no package, no `thermal.land_cover.dir`) or without a loadable library, vegetation stays
  off with one `Vegetation: off (...)` log line. `Vegetation: on (...)` logs the types (trees, shrubs), the mix source
  (`package` / `file` / `default`), `canopy chmv2|none|off`, the shrub radius, radius, the far tier (`far 4000 m (keep
  0.35, blocks 4)` or `off`), density, shadows, seed, land-cover directory and the building exclusion method.

## Configuration

`vegetation:` in `deploy/camsim_config.yaml`, every key startup only; table with env vars and ranges in
[`configuration.md`](configuration.md#vegetation-vegetation).

| Key | Env | Default |
|---|---|---|
| `enabled` | `CAMSIM_VEGETATION_ENABLED` | `true` |
| `library_dir` | `CAMSIM_VEGETATION_LIBRARY_DIR` | `""` (`Content/NonUFS/Vegetation`) |
| `radius_m` | `CAMSIM_VEGETATION_RADIUS_M` | `1000` (near tier) |
| `density_scale` | `CAMSIM_VEGETATION_DENSITY_SCALE` | `1.0` (trees and shrubs) |
| `cast_shadows` | `CAMSIM_VEGETATION_CAST_SHADOWS` | `true` (near tier) |
| `cell_size_m` | `CAMSIM_VEGETATION_CELL_SIZE_M` | `100` |
| `seed` | `CAMSIM_VEGETATION_SEED` | `0` (used only without a package) |
| `drop_in_buildings` | `CAMSIM_VEGETATION_DROP_IN_BUILDINGS` | `true` |
| `building_clearance_m` | `CAMSIM_VEGETATION_BUILDING_CLEARANCE_M` | `2.0` (`[0, 10]`) |
| `mix_file` | `CAMSIM_VEGETATION_MIX_FILE` | `""` (absolute; used only without a package mix) |
| `canopy_enabled` | `CAMSIM_VEGETATION_CANOPY_ENABLED` | `true` |
| `shrubs_enabled` | `CAMSIM_VEGETATION_SHRUBS_ENABLED` | `true` |
| `shrub_radius_m` | `CAMSIM_VEGETATION_SHRUB_RADIUS_M` | `500` (`[0, radius_m]`) |
| `shrub_spacing_m` | `CAMSIM_VEGETATION_SHRUB_SPACING_M` | `3.0` (`[1, 10]`) |
| `far_radius_m` | `CAMSIM_VEGETATION_FAR_RADIUS_M` | `4000` (0 = off, else `[1.5 × radius_m, 10000]`) |
| `far_block_cells` | `CAMSIM_VEGETATION_FAR_BLOCK_CELLS` | `4` (`[2, 8]`) |
| `far_keep` | `CAMSIM_VEGETATION_FAR_KEEP` | `0.35` (`(0, 1]`) |
| `far_scale` | `CAMSIM_VEGETATION_FAR_SCALE` | `1.3` (`[1, 2]`) |

The defaults of the R4 "deeper" keys are starting values until the bench sweep ("Bench") and the user's choice.

## Site mixes

Example `[vegetation]` tables for the operational sites (put them in the site's scene file; see
[`scene-packages.md`](scene-packages.md#vegetation-mix-r4)). Weights are relative within a code; codes are WorldCover's.

```toml
# Camp Pendleton (coastal sage scrub, chaparral, oak/sycamore riparian woodland). CHMv2 puts little >= 3 m canopy on
# much of WorldCover's tree cover here, so code 10 also gets scrub.
[vegetation]
mix.10 = { tree_broadleaf_a = 1.0, shrub_chaparral = 1.0 }
mix.20 = { shrub_chaparral = 0.8, shrub_desert = 0.2 }
mix.30 = { shrub_chaparral = 0.5, shrub_desert = 0.5, tree_broadleaf_a = 0.05 }
shrub_density.10 = 4.0
shrub_density.20 = 8.0
shrub_density.30 = 1.0

# Eglin AFB (longleaf pine sandhills, wiregrass understory)
[vegetation]
mix.10 = { tree_pine_a = 0.5, tree_pine_b = 0.35, tree_broadleaf_a = 0.15 }
mix.30 = { shrub_chaparral = 1.0 }
mix.90 = { shrub_chaparral = 1.0, tree_broadleaf_a = 0.2 }
shrub_density.30 = 2.0
shrub_density.90 = 6.0

# NAS Patuxent River (mixed hardwood / loblolly pine, tidal marsh)
[vegetation]
mix.10 = { tree_broadleaf_a = 0.6, tree_pine_a = 0.25, tree_pine_b = 0.15 }
mix.90 = { shrub_chaparral = 1.0 }
shrub_density.90 = 8.0

# Yuma Proving Ground (creosote-bursage desert scrub, washes)
[vegetation]
mix.20 = { shrub_desert = 0.85, cactus_a = 0.15 }
mix.30 = { shrub_desert = 1.0 }
mix.60 = { shrub_desert = 1.0 }
shrub_density.20 = 1.5
shrub_density.30 = 0.8
shrub_density.60 = 0.3
```

## Library format

A library is a directory with `library.json` and the glTF files it names (`vegetation.library_dir`,
absolute, to mount your own):

```json
{"format": "camsim-vegetation-2",
 "types": [{"name": "tree_pine_a", "role": "tree", "gltf": "tree_pine_a.glb", "sha256": "b710…cd49",
            "far_gltf": "tree_pine_a_far.glb", "far_sha256": "0815…e74c",
            "height_m": 22.0, "crown_radius_m": 6.78, "thermal_class": "tree_canopy",
            "licence": "CC0-1.0", "source_url": "https://quaternius.itch.io/textured-lowpoly-trees"}]}
```

`role` is `tree` (default) or `shrub`; `crown_radius_m` (default 0.3 × height for trees, 0.5 × for shrubs) turns canopy
cover into a tree density; `far_gltf` + `far_sha256` (trees only, optional: a tree without one is not drawn far).
`height_m` is the model's real height. The loader refuses any other `format` (`camsim-vegetation-1` is still read: every
type a tree, no far mesh), duplicate names, an unknown role, a shrub with a far mesh, a missing file and a sha256
mismatch (lowercase hex). Library index 0 must stay `tree_broadleaf_a` for R4's bit-identity (the default mix uses
index 0). To add or replace a type: prepare the model with `scripts/vegetation/prepare_tree.py` (below; CC0 only),
paste the entry it prints, and check it with `CamSim.Vegetation.Library.ShippedMeshesLoad` and live with
`camsim.Vegetation.Probe`.

## Console variables and commands

| Name | Use |
|---|---|
| `camsim.Vegetation.DumpPath <abs file>` | Append one JSON line per spawned near cell: `{"row", "col", "hash", "shrub_hash", "in_buildings", "instances": [[lat, lon, height_m, yaw_deg, scale, type], ...], "shrubs": [...]}`. `hash` is `HashInstances` over the trees' bits (lat, lon, yaw, scale, type; not heights), R4's value for R4's cells; `shrub_hash` the shrubs'. A cell spawned again (after a drop) gets a new line. |
| `camsim.Vegetation.FarDumpPath <abs file>` | Append one JSON line per spawned far block: `{"block_row", "block_col", "hash", "instances": [[lat, lon, height_m, yaw_deg, scale, type, bucket], ...]}`. |
| `camsim.Vegetation.EventsPath <abs file>` | Append one JSON line per tier change (`{"tick", "ev": "tier", "tier": "near"\|"far"\|"none", "block_row", "block_col"}`), spawn, destroy and empty settle (`"tier": "near"` lines also carry the cell's `row`, `col`): the handoff gate reads it. |
| `camsim.Vegetation.GroundCheck 1` | Once the view has settled (terrain gate open for 2 s) and streaming is idle, traces 200 spawned tree bases against the terrain (the DIS clamp's complex trace) and logs `Vegetation: ground check n=200 max_dz_m=… p50_dz_m=…`, then resets itself. |
| `camsim.Vegetation.Probe <abs .glb>` | Debug: load a model, log its materials' instanced-usage flags, spawn a 10 × 10 grid at the camera nadir (below). |

All work from `-ExecCmds=` at launch (as `vegetation_check.py` sets them).

## Asset

The library (`unreal_project/CamSimTest/Content/NonUFS/Vegetation/`, `library.json` `camsim-vegetation-2` + the `.glb`s,
git LFS) ships six types; `tree_broadleaf_a` stays first (index 0), so packages without a mix keep R4's trees bit for bit.

| Type | Role | Source model | Height m | Crown radius m | Triangles near / far | sha256 |
|---|---|---|---|---|---|---|
| `tree_broadleaf_a` | tree | Quaternius Textured LowPoly Trees, `OBJ/Tree_7.obj`, leaves × 0.632, 0.53, 0.947, far crown 0.339, 0.43, 0.278 (calibrated) | 9 | 2.7 | 1,332 / 108 | `1e60df6770c1…` |
| `tree_pine_a` | tree | Quaternius, `OBJ/Pine_4.obj` (crown base at 46 % of the height: the pack's only high-crowned pine) | 22 | 6.78 | 460 / 108 | `b71028685c37…` |
| `tree_pine_b` | tree | Quaternius, `OBJ/Pine_5.obj` (slender, crown to the ground) | 16 | 4.04 | 664 / 108 | `2a28cc567588…` |
| `shrub_chaparral` | shrub | procedural clump (`prepare_tree.py --procedural-shrub`, seed 11), base colour 0.316, 0.396, 0.248 (linear, calibrated) | 2 | 1 | 160 / – | `c39904dcfba2…` |
| `shrub_desert` | shrub | procedural clump (seed 12), base colour 0.276, 0.341, 0.216 (calibrated) | 1.4 | 0.7 | 160 / – | `74f51f5d28d1…` |
| `cactus_a` | shrub | Kenney, `cactus_tall.obj`, base colour 0.090, 0.141, 0.045 | 2.5 | 1.25 | 122 / – | `336b736fc6d1…` |

Crown radius is measured from the foliage (p90 horizontal extent) by `prepare_tree.py`, except `tree_broadleaf_a`, kept
at R4's 2.7 m; it turns canopy cover into a tree density. Shrubs give theirs directly (shrub density comes from the
mix, not the canopy). `scripts/vegetation/build_library.sh OUT` rebuilds every mesh but the cactus from its source
(Quaternius models, procedural shrubs) with the calibrated colours, reproducibly; `install_entries.py` merges the
printed entries into `library.json` by name.

| Source | Licence (checked 2026-10-11) |
|---|---|
| Quaternius "Textured LowPoly Trees" (May 2020), https://quaternius.itch.io/textured-lowpoly-trees | CC0 1.0. Page: "Free for everyone to use in any project, even comercially! (CC0)"; the pack's `License.txt`: "CC0 1.0 Universal (CC0 1.0) Public Domain Dedication" |
| Kenney "Nature Kit" 2.1, https://kenney.nl/assets/nature-kit | CC0 1.0. The kit's `License.txt`: "License: (Creative Commons Zero, CC0) http://creativecommons.org/publicdomain/zero/1.0/ … This content is free to use in personal, educational and commercial projects." |

- **Far meshes** (`*_far.glb`, trees only) are procedural (`prepare_tree.py` `far_mesh`): a lathe crown whose six ring
  radii follow the foliage's height profile (round for the broadleaf, conical for the pines) plus a hexagonal trunk,
  untextured, coloured with the model's mean leaf and bark colours or `--far-color` (calibrated): 108 triangles.
  Leaf-card trees don't simplify below ~600 triangles with gltfpack (each card is its own island), and at 1–4 km a tree
  is a few pixels. Until 2026-10-12 every far mesh was **inside out** (a winding flip in the builder): the camera saw
  the crowns' back faces lit from inside, 3–10× too dark, which was most of the "far trees darker than near" finding.
- **Shrubs are procedural** (`prepare_tree.py --procedural-shrub`): a central low dome and three smaller ones around it,
  ring radii jittered ±12 % by the seed, one untextured material, 160 triangles, solid from above. They replaced
  Kenney's leaf fans, whose large bush covered only 59 % of its centre seen from above: hollow hexagonal rings in a
  nadir view. Better CC0 meshes remain an option: Quaternius "Ultimate Nature Pack" (`Bush_1`, `Bush_2`; Google Drive,
  rate-limited when fetched by script on 2026-10-11 and 2026-10-12). Swap one in with `prepare_tree.py --role shrub`
  and the same `name`, then recalibrate its colour.

### Colour calibration

`scripts/vegetation/calibrate_colours.py --scene PKG --out DIR [--configs ...]` (a package without its own mix,
e.g. `pendleton-r4`) renders the riparian woodland from 120 m and 350 m once with vegetation off and once per
configuration: one library type at a time through a species mix file (`broadleaf`, `pine`, `chaparral`, `desert`),
or far trees alone (`far_broadleaf`, `far_pine`: near radius 100 m). Vegetation pixels are where the frames differ;
over them, the imagery's mean linear colour divided by the render's (the auto-exposure difference measured on the
other pixels and divided out) is the factor the type's base colour needs. The target is the **green subset**: the
imagery pixels under the plants whose green leads red and blue (by 20 %), i.e. the canopy itself. The mean over all
pixels under a plant also averages in the dry ground between plants (and WorldCover's tree cover takes in much dry
chaparral: only 11–32 % of the pixels under far trees and 5–6 % under shrubs look green), so matching it made the
plants tan. Results 2026-10-12 (M1 Pro, Pendleton imagery, NAIP 2022), sRGB:

| Type | Before (merged R4) | Calibrated | Imagery, green subset | Factor (linear) |
|---|---|---|---|---|
| broadleaf (near) | 128, 160, 79 (lit tops) | 70–73, 77–79, 53–56 | 71–76, 80–85, 55–60 | leaf texture × 0.632, 0.53, 0.947 |
| broadleaf (far) | 43–56, 51–68, 37–43 (inside out) | 70–79, 76–87, 62–69 | 74–79, 80–88, 61–69 | crown 0.339, 0.43, 0.278 |
| chaparral | 37–46, 42, 23–31 (Kenney) | 77, 81, 58 | 73, 78, 56 | 0.316, 0.396, 0.248 |
| desert bush | – | 80, 81, 61 | 80, 85, 62 | 0.276, 0.341, 0.216 |
| pine (near / far) | 60–67, 70–78, 39–45 / 29–30, 35–37, 27–29 | unchanged / 58–64, 71–79, 43 | – | no Pendleton truth (tune on Eglin); far now matches near |

Every calibrated type is within ~10 % of its target per channel. Shots: `.cache/veg/check-hue/{oblique,truck}/`;
earlier passes in `.cache/veg/check-colour/` (mean-colour target) and `.cache/veg/check-r7/` (before). Sunlit shrubs
read pale green up close (a nadir from tens of metres); at the sensor's usual 100+ m they blend with the hillside.

### Preparing a model

`scripts/vegetation/prepare_tree.py` (`uv run --with pillow`; needs `npx`) converts OBJ with `obj2gltf`
(`--checkTransparency`), simplifies to `--max-tris` with `gltfpack -si` (no quantisation or
compression, which glTFRuntime may not decode), turns alpha-blended materials into double-sided MASK,
scales the model to `--height-m`, writes `<name>.glb` (and, for `--role tree`, the far mesh `<name>_far.glb`, within
`--far-tris`, default 150) and prints the `camsim-vegetation-2` entry (`--role`, measured or `--crown-radius-m`;
`--base-color MATERIAL=R,G,B` overrides a stylised MTL colour, linear). It refuses anything but `CC0-1.0`. The output is byte-for-byte reproducible from the same inputs. The Quaternius
MTLs name no textures, so `--texture MATERIAL=PATH` binds them:

```bash
Q=".cache/veg/src/quaternius/Textured Stylized Trees - May 2020"   # the unzipped pack
uv run --with pillow python scripts/vegetation/prepare_tree.py "$Q/OBJ/Tree_7.obj" \
  --name tree_broadleaf_a --height-m 9.0 --crown-radius-m 2.7 --max-tris 10000 --licence CC0-1.0 \
  --source-url https://quaternius.itch.io/textured-lowpoly-trees \
  --out unreal_project/CamSimTest/Content/NonUFS/Vegetation \
  --texture "Bark=$Q/Textures/Tree_Bark.jpg" --texture "Tree_Leaves=$Q/Textures/Tree_Leaves.png"
```

### Runtime check (probe)

`camsim.Vegetation.Probe <abs path to .glb>` (console; `Vegetation/VegetationProbe.cpp`, kept as a
debug tool) loads the model with `CamSimMeshLoader::LoadStaticMesh`, logs each material's
`MATUSAGE_InstancedStaticMeshes` flag before the first render, and spawns a 10 x 10 grid (15 m
spacing) as one `ACamSimVegetationCell` at the camera nadir, grounded by one
`SampleHeightMostDetailed`. Absolute paths pass through `CamSimEntityPaths::Resolve` unchanged.

Results (2026-10-10, M1 Pro, Metal, Pendleton R2 package, 33.302 N -117.355 W):

| Build | Materials (base) | Instanced usage | Instances |
|---|---|---|---|
| Editor game mode | `MID_M_glTFRuntimeBase`, `MID_M_glTFRuntimeTwoSidedMasked_Inst` (both `/glTFRuntime/M_glTFRuntimeBase`) | 1, 1 | 100 |
| Cooked (Mac Development, pak) | `MID_M_glTFRuntimeBase`, `MID_M_glTFRuntimeTwoSidedMasked_Inst` (both `/glTFRuntime/M_glTFRuntimeBase`) | 1, 1 | 100 |

Trees stand on the ground, textured, with masked leaves and shadows:

![Probe grid, 300 m oblique](images/vegetation/probe-oblique-300m.jpg)

![Probe grid, 350 m nadir](images/vegetation/probe-nadir-350m.jpg)

glTFRuntime's base material already has "Used with Instanced Static Meshes" set, so the cooked build
renders the textured trees too (no human editor work needed). The cooked run loaded the `.glb` staged
inside the app (`Content/NonUFS/Vegetation/`, via `DirectoriesToAlwaysStageAsNonUFS`). It ran on the
default config (no `camsim_config.yaml` beside the binary), so the grid stands at the default pose
(Washington, DC) rather than Pendleton:

![Probe grid, cooked Mac build](images/vegetation/probe-cooked-mac.jpg)

Cooking for Mac on this M1 Pro needed two local workarounds, neither committed: `-unrealexe=<UnrealEditor.app
binary>` (the engine-installed Cesium plugin's rpaths don't resolve from `UnrealEditor-Cmd`, so the cook
commandlet fails to load `CesiumRuntime`), and `SF_METAL_SM5` added to the Mac `TargetedRHIs` (the project
cooks SM6 only, and an M1 has no SM6: the packaged game exits with "SF_METAL_SM5 was not cooked").

## Acceptance (`scripts/vegetation_check.py`)

```bash
PKG="$PWD/.cache/scene-packages/pendleton-r5veg"   # pendleton-r4 + [canopy] + the Pendleton [vegetation] mix
caffeinate -ims uv run --project scripts/scene --with pycocotools python scripts/vegetation_check.py \
    --scene "$PKG" --baseline .cache/veg/baseline --out .cache/veg/check-r5 \
    [--stages approach,off,gt,truck,oblique,footprints,r4_identity,long_range]
```

macOS, local build, one CamSim at a time (it launches about nine). The baseline is two pre-slice
`run_bench.py --smoke --site pendleton` runs (`a/`, `b/`). Writes `report.json` (exit 0 only when every
gate passes) and the shots. `--stages` reruns a subset and keeps the other gates from an existing
`report.json`.

| Gate | What |
|---|---|
| placement | every dumped tree stands on a code whose mix lists trees (and, inside the package's canopy layer, on a texel with cover > 0 and height ≥ 3 m); every shrub on a code whose mix lists shrubs (land cover read with `bench/dense_path.LandCover`) |
| species | per code and role, the dumped type counts vs the mix weights: χ² p > 0.001 (codes with ≥ 50 plants and ≥ 2 types); a type the code's mix doesn't list fails |
| canopy | (packages with `canopy/`) per 100 m cell with mean CHMv2 cover in [0.1, 0.5] (≥ 50 such cells): crown fraction Σ π (r × scale)² / cell area vs cover, median \|error\| ≤ 10 pp. Above 0.5 one crown per candidate saturates (see "How it works") |
| far_determinism | the far blocks dumped by the two approach runs have the same hashes, and every far tree whose cell was dumped near is one of that cell's near trees |
| handoff | from the events dumps: no destroy of one tier's actor while the block's current tier hasn't settled since its latest tier change |
| grounding | `camsim.Vegetation.GroundCheck`: n = 200 and max \|dz\| < 0.5 m, in both runs |
| determinism | approach the riparian pose from 3 km north and from 3 km south (30 s): every cell in both dumps has the same hash |
| origin_shift | `/snapshot` at the riparian pose, 20 s at lon + 3.41° (`far_origin`), back: SSIM ≥ the baseline pair's SSIM − 0.01, and every cell spawned again has its first hash |
| off_switch | `CAMSIM_VEGETATION_ENABLED=0` smoke shot vs the baseline: SSIM ≥ the baseline pair's SSIM − 0.005 (live frames are not bit-identical run to run), and the off run's log has no vegetation line; a trees-on smoke is measured beside it to show the margin (little: see "Measured") |
| gt | `scripts/gt_occlusion_check.py` exits 0 with vegetation on (its San Francisco default config, so trees from the committed SF land-cover sample, Presidio included) |
| footprints | (`--stages footprints`, R3b) no tree or shrub within the building clearance of the package's mask |
| r4_identity | (`--stages r4_identity`) `--r4-scene` (default `pendleton-r2`: no canopy, no mix) with shrubs and the far tier off: every cell has the `--r4-baseline` (R4) run's tree hash |
| long_range | (`--stages long_range`) the bench's `long_range` phase (5 km HAE, 15° down, 30° FOV, 9 km at 100 m/s toward the riparian woodland) with vegetation on and off: 0 dropped frames, game p95 ≤ +3.0 ms |

Informational: `surface_dz` (CIGI HOT, a trace against the drawn tiles, at 25 dumped trees per distance
band, before and after the far trip), `truck` (a DIS truck parked 2 m from the trunk of the tree with the
most neighbours, with ground truth on) and `shots` (low oblique over the woodland, trees on and off).

The riparian pose is 33.307 N, -117.354 W, 350 m HAE, heading 45°, gimbal −20°: the pose sits on code 10
and looks north-east over the densest Santa Margarita woodland (the plan's 33.302, -117.355 looks at the
MCAS runway and misses it).

## Measured

2026-10-11, M1 Pro, Metal, Pendleton R2 package (`pendleton-r2`, no buildings layer), defaults (radius 1000 m,
density 1.0, shadows on), commit `69b7e87`. All six gates pass (`.cache/veg/check/report.json`):

| Gate | Result |
|---|---|
| placement | 8,992 trees in 472 cells, 0 off code 10 |
| grounding | north n=200 max_dz 0.000 m, south n=200 max_dz 0.000 m (traces and `SampleHeightMostDetailed` agree to the mm) |
| determinism | 113 cells in both dumps, 0 hash mismatches (104 only north, 255 only south) |
| origin_shift | SSIM 0.9834 (north) / 0.9832 (south) vs floor 0.9796 − 0.01; 113 cells spawned again after the trip in each run, 0 hash changes |
| off_switch | log shows no vegetation line (disabled); `nadir_3km` SSIM 0.97957 vs baseline a, 0.97997 vs b; the baseline pair's own SSIM 0.97957 (first run: 0.97951 / 0.97955). **Weak, see below** |
| gt | exit 0: nadir truck visibility median 0.958, boat 1.000, edge truncation, crest 58.3 % < 0.9, 9,801 masks decode, frame time diff −0.01 ms |

- **The off-switch gate has little power.** It compares the one `--smoke` shot, `nadir_3km` (straight down from
  3 km), where a tree is a few pixels, against a baseline whose own run-to-run SSIM is 0.97957. A trees-**on** smoke
  run of the same shot measures SSIM 0.99801 vs baseline a and 0.97959 vs b: it would pass the gate too. Run-to-run
  noise is larger than the trees' effect, so the gate catches only gross changes (a broken frame, a
  different scene); the real evidence that `enabled: false` restores the pre-slice frame is the code path (no
  manager is created) and the log check (no `Vegetation:` line in the off run, one `Vegetation: on (...)` in the on
  run). A stronger gate needs a shot where trees fill the frame, captured before and after the slice.
- **Ground check timing:** the first live run measured max_dz 2.133 m (p50 0.355 m) in the north run: the
  check fired 0.3 s before the terrain gate opened (load progress 99.4 %), against tiles still refining.
  It now also waits for the terrain gate to have been open for 2 s, and the script holds the start pose
  until it has logged.
- **Tree base vs the drawn surface** (CIGI HOT minus the dumped height; 25 trees per band per run, before
  and after the far trip): max |dz| 0.006 m in every band, 0–300 m, 300–700 m and 700–1000 m (up to 986 m
  from the camera, i.e. cells at `radius_m`). Once the view has settled, far cells don't float; while tiles
  are still refining (the first seconds after a cut), trees can sit up to a couple of metres off the
  coarse surface, as the first ground check showed.
- **Terrain tileset after an origin shift:** after the trip to lon + 3.41° and back, the 113 cells around
  the pose spawned again with grounded heights (the after-trip surface dz above) and no vegetation warning
  was logged, so the terrain tileset is still found by its `CamSimTerrain` tag.
- **Truck under trees** (reported, not gated): `send_dis_test.py truck-parked` re-centred 2 m east of a
  tree at 33.30782 N, -117.35484 W with ground truth on. From 45° above (15° FOV): visibility 0.153, modal
  box [518, 246, 124, 177], amodal [489, 231, 294, 221] (green and orange below). Straight down: visibility
  0.002, amodal [486, 217, 296, 297] centred on the image. 373 annotations, one id; boxes where the truck is.

![DIS truck under the canopy: modal box green, amodal orange](images/vegetation/truck-under-trees.jpg)

Shots for the visual review (pending the user): `.cache/veg/check/oblique/low_oblique_trees_{on,off}.png`
(120 m HAE, heading 45°, −12°), `riparian_trees_{on,off}.png` (the riparian pose),
`.cache/veg/check/north/a.png`, `.cache/veg/check/truck/truck_trees{,_nadir}.png`, and the bench's
`dense_pass` contact sheet below. Seen so far: the 1 km hard cut is visible where woodland continues beyond
it, and the low-poly crowns are a brighter, yellower green than the NAIP woodland around them.

### Trees vs building footprints (`pendleton-r3`)

2026-10-11, after the final review's I1 fix, defaults, buildings on (SSE 64). One session each with
`vegetation.drop_in_buildings` on and off: the `dense_pass` midpoint (33.2200, −117.3103, 357.7 m HAE, heading 105°),
then Mainside (33.305, −117.314, 350 m, north, −30°), then a low oblique 120 m south of the cell with the most drops
(row 137255, col 58330, 60 m above its trees, looking north, 50° FOV). Results in `.cache/veg/i1-footprints{,-off}/`.

- Check on: 6,519 trees in 489 dumped cells; check off: 6,892 in 491. **373 trees dropped (5.4 %)**, and 2 cells
  lost all their trees. The dumped `in_buildings` counts add up to 371 (the two emptied cells are never dumped).
  The cell with the most drops kept 27 of 43.
- Shots: `close_oblique.png` in each directory. With the check off, crowns stand on the flat roofs of the Mainside
  barracks; with it on, no crown shows through a roof, and trees stand between the buildings. Trees right beside a
  wall can still lean their crowns over its edge (only the trunk point is tested).
- `pendleton-r2` (no buildings layer) is unchanged: `vegetation_check.py --stages approach` rerun after the fix
  (`.cache/veg/check-i1-r2/`) passes placement (8,992 trees in 472 cells, 0 off code 10), grounding (max |dz| 0.000 m
  both runs), determinism (113 common cells, 0 mismatches) and origin shift (SSIM 0.9834 / 0.9834), and every one of
  the 217 north and 368 south cells has the same hash as in the run above (`in_buildings` 0 everywhere; the log said
  `buildings none`, R4's wording; now `buildings: none`).

### Trees vs building footprints (R3b mask)

2026-10-11, M1 Pro / Metal, `pendleton-r4` (`pendleton-r3` rebuilt with the footprint mask, `features/buildings/mask/`),
defaults (`vegetation.building_clearance_m` 2.0). `scripts/vegetation_check.py --stages footprints,approach,off`
(`.cache/r3b/veg/report.json`):

- **Footprints gate: pass.** 4,895 trees dumped in 399 cells around the two poses (the `dense_pass` midpoint and
  Mainside); **0 probe hits** (trunk and 4 probes against the package's mask, the runtime rule), **0 trunks inside a
  footprint** (10,031 footprints in the bbox, polygons shrunk by 0.76 m); 36 trees within the 2 m buffered footprints
  (information: the 5-point rule cannot see a corner diagonally). Log line ends `buildings: mask (clearance 2.0 m))`.
  The same counts on the flat-roof package (`.cache/r3b/veg-flat/`, same mask).
- Placement 8,987 trees in 472 cells, 0 off code 10; grounding max |dz| 0.000 m both runs; determinism 113 common cells,
  0 mismatches; origin shift SSIM 0.9835 / 0.9834, 0 hash changes; off switch passes (log disabled, `nadir_3km` SSIM
  0.97848 vs a, 0.97857 vs b; the weak gate described above).
- **Without a mask nothing changed:** `--stages approach` on `pendleton-r2` (`.cache/r3b/veg-r2/`) against the R4 run
  on disk (`.cache/veg/check-i1-r2/`): 217 north and 368 south common cells, **0 hash mismatches**; the log ends
  `buildings: none`.
- **Cost** (`.cache/bench-r3b/combined/`, `dense_pass`, 2 repeats, medians): trees on with buildings on vs buildings on
  alone: game p50 +0.60 ms, p95 +0.57 ms, +53 MiB; the R3/R4 combined sweep without any footprint check had +0.60 /
  +0.60 ms / +69 MiB for the same pair. The mask costs nothing measurable, against R4's runtime check (+0.3 ms p50,
  **+2.5 ms p95**, +34 MiB). Both features on now measure +1.39 ms game p95 over all-off (R4 with the runtime check:
  +3.61 ms, over the 3.0 ms limit).
- The runtime check (`SampleHeightMostDetailed` on the buildings tileset) remains only for packages with buildings and
  no mask (R3 packages).

### Bench

`scripts/bench/sweep.py` (bench spec `superpowers/specs/2026-10-10-r3r4-realism-cost-bench-design.md`), 2026-10-11,
M1 Pro, `pendleton-r2`, EO, the `dense_pass` phase (90 s at 30 fps after the warm-up), 2 repeats per row,
buildings off in every row:

```bash
uv run --with numpy --with pillow python scripts/bench/sweep.py --scene "$PKG" --out .cache/bench-r3r4/trees \
    --axis buildings=off --axis trees=off,on --axis trees_radius=500,1000,2000 --axis trees_density=0.5,1.0 \
    --axis trees_shadows=on,off --repeat 2
```

Increase of the row median over the trees-off row (game 3.50 / 6.53 ms p50 / p95, GPU 19.45 ms, footprint 8,894 MiB).
Every row: 0 dropped frames, 29.6–29.9 fps. Budget: game p50 ≤ +1.5 ms, p95 ≤ +3.0 ms, GPU ≤ +5.0 ms, memory ≤ +1024 MiB (raised 2026-10-11 by the user from 381 MiB; every row below passes both).

| radius m | density | shadows | game p50 | game p95 | GPU p50 | memory MiB | budget |
|---|---|---|---|---|---|---|---|
| 500 | 0.5 | on | +0.20 | +0.04 | +0.82 | +62 | pass |
| 500 | 0.5 | off | +0.22 | +0.14 | +1.07 | +20 | pass |
| 500 | 1.0 | on | +0.28 | +0.35 | −0.10 | +38 | pass |
| 500 | 1.0 | off | +0.30 | +0.79 | −0.13 | +42 | pass |
| 1000 | 0.5 | on | +0.55 | +0.43 | +0.96 | +103 | pass |
| 1000 | 0.5 | off | +0.57 | +0.96 | +1.10 | +107 | pass |
| **1000** | **1.0** | **on** | **+0.63** | **+0.46** | **+1.13** | **+81** | **pass** |
| 1000 | 1.0 | off | +0.57 | +0.10 | +1.39 | +85 | pass |
| 2000 | 0.5 | on | +1.48 | +0.90 | +2.12 | +146 | pass (marginal) |
| 2000 | 0.5 | off | +1.48 | +0.78 | +2.26 | +95 | pass (marginal) |
| 2000 | 1.0 | on | +1.64 | +1.36 | +3.08 | +163 | FAIL: game p50 |
| 2000 | 1.0 | off | +1.64 | +1.01 | +2.07 | +93 | FAIL: game p50 |

- The radius drives the cost: game-thread p50 grows +0.2–0.3, +0.6, +1.5–1.6 ms at 500, 1000, 2000 m (the cell
  count grows with the area: ~80, ~315, ~1,260 cells). Density and shadows move nothing beyond the run-to-run
  noise (GPU p50 varies ±1 ms between repeats of one row).
- **Defaults chosen 2026-10-11:** keep the starting values, radius 1000 m, density 1.0, shadows on
  (bold row: inside every budget with margin). Radius 2000 m fits only at density 0.5 and with no game-thread
  margin left; 500 m saves ~0.35 ms but the hard cut moves into the middle distance of an oblique view.
- Contact sheet of the `dense_oblique` shot per row: `.cache/bench-r3r4/trees/sweep_sheet.png` (table:
  `sweep.md`, per run: `sweep.csv`). The `dense_pass` view is a residential area with few code-10 cells, so trees
  show along the drainage at the bottom of the frame only.
- The spec's combined row (buildings and trees both on vs both off, `pendleton-r3`) was run in the combined sweep
  (`scene-packages.md`, "R3/R4 bench check and combined cost"): at the chosen defaults (buildings SSE 64 with
  physics, trees 1000 m / 1.0 / shadows on) game p50 +1.05 ms, p95 +1.09 ms, GPU +2.98 ms, memory +359 MiB: inside
  the budget (a 90 s `dense_pass` figure, before the footprint check existed; with it on: +1.40 / +3.61 / +3.54 ms,
  +408 MiB: game p95 is over its 3.0 ms limit, which the user accepted on 2026-10-11 to keep the check on by default,
  with a build-time footprint exclusion as the follow-up; memory passes the raised 1024 MiB budget). Trees alone on that package: +0.61 /
  +0.58 / +1.12 ms, +52 MiB.

## Measured: species, canopy, shrubs, far tier (R4 deeper)

2026-10-12, M1 Pro, Metal, `pendleton-r5veg` (the R4 package plus `[canopy]` CHMv2 and the Pendleton mix, buildings
with the R3b mask), defaults (radius 1000 m, shrubs 500 m, far 4000 m, keep 0.35, blocks 4), commit `63d589f`. Every
gate passes (`scripts/vegetation_check.py --stages approach,off,gt,truck,oblique,footprints,r4_identity,long_range`,
`.cache/veg/check-r7/report.json`):

| Gate | Result |
|---|---|
| placement | 18,290 trees and 70,478 shrubs in 920 cells; 0 trees or shrubs off their mix's codes, 0 trees on canopy texels with no cover or < 3 m |
| species | χ² per code and role, p > 0.001: code 30 shrubs 19,962 / 20,219 (`shrub_chaparral` / `shrub_desert`, p 0.20), code 20 9,289 / 2,276 (p 0.39); single-type lists (10 and 30 trees, 10 shrubs) 0 strays |
| canopy | 104 cells with CHMv2 cover in [0.1, 0.5]: median \|drawn crown cover − CHMv2 cover\| 7.9 pp (mean −9.3 pp: the crowns' saturation) |
| far_determinism | 269 far blocks in both runs, 0 hash mismatches; 12,832 far trees checked against their near cells, 0 missing |
| handoff | 9,942 tier/spawn/destroy events, 1,587 destroys, 0 gaps |
| grounding | north and south n=200, max dz 0.000 m |
| determinism | 158 cells in both dumps, 0 hash mismatches |
| origin_shift | SSIM 0.971 / 0.985 vs floor 0.980 − 0.01; 191 / 202 cells spawned again after the trip, 0 hash changes |
| off_switch | no `Vegetation:` line in the off run; `nadir_3km` SSIM 0.9785 / 0.9786 vs the baseline pair's 0.9796 (weak gate, as in the slice) |
| gt | `gt_occlusion_check.py` exit 0 |
| footprints | 12,803 trees, 0 trunks inside a footprint, 0 probe hits |
| r4_identity | 198 cells against the R4 baseline (no mix, no canopy, shrubs and far off): 0 tree-hash mismatches |
| long_range | 650 m HAE, 12° down, 30° FOV over the riparian woodland: game p95 +1.06 ms (budget 3.0), 0 dropped; 5,549 far trees drawn in the view (in bearing and inside their bucket's 3D cull), 1,686 in the wedge past it |

Truck under trees (reported): visibility median 0.18. Shot of the long-range view: `.cache/veg/check-r7/long_range/view/long_range_view.png`.

**What the measurements found on the way** (each is fixed and covered by a test):

- **Height sampling on the game thread.** The first acceptance run failed `long_range` at game p95 +11.2 ms: Insights put
  it in `Cesium::loadTiles` (35.9 s over the run vs 0.28 s with vegetation off), because cesium-native answers every
  `SampleHeightMostDetailed` point on the game thread and the run asked one point per shrub and a 25 m grid per far
  block. Shrubs moved to a 20 m grid per cell and far blocks to a 100 m grid; trees keep exact heights.
- **Shrubs generated out to the near radius.** Near cells carried ~1,000 shrubs each out to `radius_m`, and
  `shrub_radius_m` only culled them: +1.9 ms game p50, +3.8 ms p95 in the dense pass at any shrub radius. The shrub ring
  generates them only within `shrub_radius_m`.
- **Tiers by ground distance** (final review). HISM culls are 3D, so above ~1.2 km no near tree was drawn, and the old
  5 km HAE `long_range` view had every far tree past its cull: it measured invisible trees. Tiers and the ring are now
  planned by 3D distance from the camera's height above the terrain, and `long_range` flies low enough to see the far
  tier, with a gate on far trees drawn.
- **Far heights loaded the finest terrain everywhere.** `SampleHeightMostDetailed` on every far block's grid loaded the
  finest 3DEP tiles over the whole 4.4 km far disk; after the low long-range pass the next pass ran +0.3 ms p50,
  +2.5 ms p95 and +500 MiB. Far blocks now trace the rendered terrain (`FRetryingTraceSampler`).
- **Canopy no-data** (final review). Texels no CHMv2 pixel covers were written 0 % cover (no trees); they are now
  no data (255) and get the land-cover rule.

### Bench (R4 deeper)

`scripts/bench/sweep.py`, 2026-10-12, M1 Pro, `pendleton-r5veg`, EO, commit `63d589f`, 2 repeats per row, buildings on
(SSE 64) in every row, increase of the row median over the buildings-on / trees-off row. Every row: 0 dropped,
≥ 29.6 fps. Budget: game p50 ≤ +1.5 ms, p95 ≤ +3.0 ms, GPU ≤ +5.0 ms, memory ≤ +1024 MiB.

Far tier, `long_range` phase (`--axis far_radius=0,2000,4000,6000 --phase long_range`; trees-off row 4.17 / 6.94 ms,
GPU 18.2 ms):

| far_radius_m | game p50 | game p95 | GPU p50 | memory MiB | budget |
|---|---|---|---|---|---|
| 0 (off) | +0.38 | +0.51 | +1.37 | +114 | pass |
| 2000 | +0.58 | +0.75 | −1.44 | +124 | pass |
| **4000** | **+0.91** | **+1.60** | **+2.24** | **+202** | **pass** |
| 6000 | +1.43 | +2.06 | −0.07 | +228 | pass (marginal p50) |

GPU p50 varies ±1.5 ms between repeats of one row here. `far_keep` 0.25 / 0.35 / 0.5 (measured on the earlier 5 km view)
moved game p50 by < 0.1 ms: the block count drives the cost, not the kept share.

Dense pass at the defaults (`--axis shrubs=off,on --axis shrub_radius=500 --phase dense_pass`; trees-off row 4.00 /
6.82 ms):

| shrubs | game p50 | game p95 | GPU p50 | memory MiB | budget |
|---|---|---|---|---|---|
| off | +1.21 | +1.68 | +3.73 | +189 | pass |
| **on (500 m)** | **+1.63** | **+2.22** | **+3.64** | **+201** | **FAIL: game p50 by 0.13 ms (accepted)** |

The shrub-radius sweep (300 / 500 / 1000 m, with the ring, before the last two fixes) measured shrubs on at +1.64 /
+1.75 / +2.22 ms game p50: the radius sets the ring's cell count and with it the cost.

**Defaults chosen 2026-10-12 (user):** shrubs on at 500 m, far tier 4000 m, keep 0.35, blocks 4. The dense pass is
+0.13 ms over the game p50 budget with them (p95, GPU and memory pass; `long_range` passes everything); accepted as a
known cost, like the R4 footprint check's p95 overrun. Contact sheets: `.cache/bench-r7veg/{far,dense}/sweep_sheet.png`.

## R4 on Linux (2026-10-13)

Reference box: Core Ultra 9 285K, RTX 5080 (16 GB), driver 595.99.02, Vulkan SM6, UE 5.8.3, head `44519b6`, native
editor build. `pendleton-r5veg` built in the reference container from the box's `pendleton-r4` (`--replan`, the
Pendleton `[vegetation]` table of "Site mixes" and `[canopy] enabled = true`): 72 canopy tiles (10.9 MB, 4 CHMv2 tiles
fetched), everything else skipped; `verify`: `canopy` and `vegetation_mix` ok. Assets: `CamSim.Vegetation.Library.*`
8/8, incl. `ShippedMeshesLoad`. Outputs under `.cache/linux-6/` (untracked).

Baselines, made on this box because the Mac's are not here: the off-switch pair is two `run_bench.py --smoke --site
pendleton` runs on `pendleton-r2` with `CAMSIM_VEGETATION_ENABLED=0` (current build: the gate compares an off run with
off runs, so a pre-slice build is not needed); the `r4_identity` baseline is a run of the **R4-era code**
(`947d04e`, the last commit before R4 deeper, built in a separate worktree) — its own `vegetation_check.py --stages
approach` on `pendleton-r2`, which passed its gates there (8,992 trees in 472 cells, determinism 113 common cells, 0
mismatches) and dumped 330 cells. Found on the way: `vegetation_check.py` read only the macOS editor log, so it could
not run on Linux (fixed, the same switch as `ocean_check.py`).

**Every gate passes** (`vegetation_check.py --stages approach,off,gt,truck,oblique,footprints,r4_identity,long_range`,
`.cache/linux-6/check-r5/report.json`), with the **same counts as the Mac's run** ("Measured: species, canopy, shrubs,
far tier" above), so the scatter, species draw, canopy acceptance and far-tier selection are platform-independent:

| Gate | Linux / Vulkan (Mac where it differs) |
|---|---|
| placement | 18,290 trees and 70,478 shrubs in 920 cells; 0 off their mix, 0 trees without canopy |
| species | code 30 shrubs 19,962 / 20,219 (p 0.20), code 20 9,289 / 2,276 (p 0.39); 0 strays |
| canopy | 104 cells, median \|error\| 7.9 pp (mean −9.3 pp) |
| far_determinism | 269 far blocks, 0 mismatches; 12,832 far trees, 0 missing from their near cells |
| handoff | 9,934 events (Mac 9,942), 1,586 destroys (Mac 1,587), 0 gaps |
| grounding | north and south n = 200, max dz 0.000 m |
| determinism | 158 common cells, 0 mismatches |
| origin_shift | SSIM 0.973 / 0.984 vs floor 0.980 − 0.01; 191 / 201 cells respawned (Mac 191 / 202), 0 hash changes |
| off_switch | no `Vegetation:` line; `nadir_3km` SSIM 0.9784 / 0.9784 vs the pair's 0.9797. Here a trees-on run scores 0.887 and would fail, so on this box the gate has power (on the Mac a trees-on run passed it) |
| gt | `gt_occlusion_check.py` exit 0 |
| footprints | 12,803 trees, 0 probe hits, 0 trunks inside a footprint; 82 within the 2 m clearance (info) |
| r4_identity | 198 cells against the R4-era build's dump: **0 mismatches** |
| long_range | game p95 +1.62 ms (budget 3.0; Mac +1.06), 0 dropped; 5,549 far trees drawn in the view, 1,686 past it |

Truck under trees (reported): visibility median 0.18, as on the Mac. Cell hashes were not compared line by line with
the Mac's dumps (not on this box); the identical counts in every gate, and `r4_identity` against an R4-era build run here,
are the cross-platform evidence, with `CamSim.Vegetation.Scatter.R4Identity`'s golden hashes passing in the NullRHI suite.

### Bench on Linux

`scripts/bench/sweep.py` as on the Mac ("Bench (R4 deeper)"), 2026-10-13, head `44519b6`, `pendleton-r5veg`, EO, 2
repeats per row, buildings on (SSE 64) in every row, increase of the row median over the buildings-on / trees-off row.
Every row: 0 dropped, ≥ 29.9 fps. Memory (`nvidia-smi`, whole process) moves by up to 270 MiB between repeats of one
row here, so differences under ~150 MiB are noise. Outputs: `.cache/linux-6/sweep-{far,shrubs}/`.

Far tier, `long_range` phase (trees-off row 3.92 / 6.63 ms, GPU 2.66 ms):

| far_radius_m | game p50 | game p95 | GPU p50 | memory MiB | budget | Mac game p50 |
|---|---|---|---|---|---|---|
| 0 (off) | +0.29 | +0.68 | +0.04 | −57 | pass | +0.38 |
| 2000 | +0.56 | +0.72 | +0.06 | −51 | pass | +0.58 |
| **4000** | **+0.96** | **+1.17** | **+0.16** | **+23** | **pass** | **+0.91** |
| 6000 | +1.60 | +1.93 | +0.25 | +92 | FAIL: game p50 by 0.10 ms | +1.43 |

The game-thread cost of the far tier matches the Mac's within 0.2 ms (it is CPU work: block planning, height traces,
HISM updates), so the 4000 m default holds on both. The GPU cost is ~0.2 ms here against the Mac's +2.2 ms (±1.5 ms
noise there): the RTX 5080 draws the 108-triangle far meshes for free. 6000 m tips over the p50 budget here, as it
nearly did on the Mac.

Dense pass (`--axis trees=off,on --axis shrubs=off,on --axis shrub_radius=300,500,1000 --phase dense_pass`; trees-off
row 3.67 / 6.68 ms, GPU 2.84 ms). The three shrubs-off rows differ only in the unused radius and agree within 0.03 ms
(p50), so they are shown once:

| shrubs | game p50 | game p95 | GPU p50 | memory MiB | budget | Mac game p50 / p95 |
|---|---|---|---|---|---|---|
| off | +1.33 | +3.01 | +0.43 | +96 | FAIL: game p95 by 0.01 ms | +1.21 / +1.68 |
| on, 300 m | +1.55 | +3.15 | +0.44 | +33 | FAIL: p50 by 0.05, p95 by 0.15 ms | +1.64 / – |
| **on, 500 m (default)** | **+1.83** | **+3.57** | **+0.44** | **+96** | **FAIL: p50 by 0.33, p95 by 0.57 ms** | **+1.63 / +2.22** |
| on, 1000 m | +2.51 | +3.61 | +0.58 | +116 | FAIL: p50 by 1.01, p95 by 0.61 ms | +2.22 / – |

At the defaults the dense pass is 0.33 ms over the game p50 budget here (Mac: 0.13 ms, accepted 2026-10-12), and
also over the p95 budget, which the Mac passed. The p95 excess is already in the trees-only row (+3.01 vs the Mac's
+1.68), so it comes with the near trees' streaming on the game thread, not with shrubs; not profiled further here. GPU and memory are far inside the budget. Not loosened: recorded as a Linux result for the
dense-pass follow-up in `ROADMAP.md`.

Capacity (`scale_bench.py camsim:linux-r4 OUT 1 2 4 --path dense`, the package mounted read-only, Docker image
`camsim:linux-r4` (C++ as `main` `c501693`, `camsim-vegetation-2` library); 45 s warm-up, 90 s measured per N; outputs
`.cache/linux-6/scale-*`):

| setting | N | min fps | dropped | wall p99 max ms | GPU busy | VRAM MiB | GPU ms p50 | game ms p50 |
|---|---|---|---|---|---|---|---|---|
| buildings on, vegetation on (defaults) | 1 | 30.00 | 0 | 37.1 | 11 % | 4,496 | 3.39 | 3.94 |
| | 2 | 30.00 | 0 | 37.5 | 22 % | 8,924 | 3.41 | 4.02 |
| | 4 | 30.00 | 0 | 38.3 | 74 % | 15,823 | 14.71 | 4.08 |
| buildings on, vegetation off | 1 | 30.00 | 0 | 34.9 | 10 % | 4,489 | 2.89 | 2.75 |
| | 4 | 30.00 | 0 | 35.0 | 44 % | 15,825 | 3.33 | 2.82 |
| buildings off, vegetation on | 1 | 30.00 | 0 | 37.4 | 10 % | 3,897 | 3.29 | 3.57 |
| | 4 | 30.00 | 0 | 38.3 | 45 % | 14,924 | 3.56 | 3.62 |

**Four instances hold 30 fps with 0 dropped in every setting**, vegetation included. With buildings and vegetation both on,
four instances fill the card (15.8 of 16.3 GB) and GPU time per frame rises to 14.7 ms (contention: the single-instance
cost is 3.4 ms). That is the edge: the same VRAM figure with R3b roof atlases and no vegetation dropped 21 frames in
`docs/capacity-linux-rtx5080.md`'s run of 2026-10-13, so four instances with buildings are VRAM-bound with no margin,
and run-to-run luck decides whether a frame slips. Vegetation adds ~1 ms game p50 per instance; its VRAM does not show in the whole-card figure, because each instance's
tile caches grow into whatever is free.

## Known limits

- Without a buildings layer, trees stand inside building footprints wherever WorldCover calls the canopy tree
  cover (~10 % of footprint area on Pendleton). With a mask (R3b) or the runtime check they are dropped (How it
  works). Building clearance is tested at 4 axis points; a crown can still overhang a footprint corner or, with a
  clearance below the crown radius, a wall. The runtime check tests only the trunk point.
- Species come from a per-code mix the scene file sets: there is no species map (LANDFIRE EVT was out of scope), so a
  code is one blend everywhere in the package.
- Canopy is CHMv2, a model estimate from 2016–2020 imagery: the canopy gate measures agreement with CHMv2, not with
  truth. Only inside the package bbox; outside it (and without `canopy/`) density is tree / no tree per 10 m texel.
- Crown cover saturates at ≈ 0.57–0.66 (one tree per 34.6 m² candidate); closed forest above that is drawn at the cap.
- Trees placed by canopy on non-tree codes take that code's thermal class in IR (per-instance thermal class deferred).
- The near/far switch is a model swap at block granularity (400 m), not a fade; far meshes are untextured blobs.
- Shrubs end at `shrub_radius_m` with a hard cut, and Kenney's shrubs are stylised leaf fans (see "Asset").
- Shrub heights are bilinear on a 20 m grid per cell: on rough ground a shrub can sit up to ~0.5–1 m off the surface
  (sunk 0.15 m to hide most of it). Far trees stand on the rendered terrain where their block was traced (100 m grid);
  as tiles refine on approach they can float or sink a few metres until the near tier takes over at ~1 km.
- Far blocks whose terrain isn't loaded when traced (e.g. right after a cut) retry for ~20 s; points still missing
  then drop their trees until the block is generated again. Without `create_physics_meshes` far blocks sample
  `SampleHeightMostDetailed` (finest tiles over the whole far disk: slow after low passes).
- At the defaults the dense pass is +1.63 ms game p50 over vegetation off, 0.13 ms over the budget (accepted
  2026-10-12, "Bench (R4 deeper)").
- Colours are calibrated on Pendleton imagery only (broadleaf, shrubs; "Colour calibration"); pines have no Pendleton
  truth and keep their texture colours until an Eglin package exists. The 500 m shrub edge is still a hard cut.
- Vegetation has no collision: CIGI LOS passes through it, DIS ignores it.
- `/ready` does not wait for vegetation.
- Leaf-on all year (NAIP is leaf-on too); no grass.
