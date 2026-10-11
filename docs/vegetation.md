# Vegetation (trees)

REALISM R4 slice. Instanced trees scattered around the camera from land cover, loaded at runtime
from a mounted vegetation library (glTF through glTFRuntime) into one HISM per cell. Spec:
[`superpowers/specs/2026-10-10-r4-trees-slice-design.md`](superpowers/specs/2026-10-10-r4-trees-slice-design.md),
plan: [`superpowers/plans/2026-10-10-r4-trees-slice.md`](superpowers/plans/2026-10-10-r4-trees-slice.md).

Trees stand wherever the land cover is WorldCover tree cover (code 10) and nowhere else. The same 100 m
cell always holds the same trees, whichever way the camera got there. They are scene geometry only:
visible in EO and IR (in IR the pixel takes the land cover's `tree_canopy` class) and occluding entities
in the ground-truth masks, but CIGI and DIS never see them.

## How it works

- **Grid** (`Vegetation/VegetationGrid.h`): a global geographic grid of `cell_size_m` cells (100 m),
  defined in angle so a cell id `(row, col)` never depends on the camera: Δlat = cell / 111,320 m, Δlon
  per latitude row = cell / (111,320 m · cos(row centre latitude)). No vegetation above 85° latitude.
- **Scatter** (`Vegetation/VegetationScatter.h`, pure): each cell is a jittered grid of
  `round(cell / 6 m)` = 17 candidates per axis, so the **real candidate spacing is 5.88 m**, not the nominal
  6 m. Candidate *i* draws from `CamSimHash::Hash(col, row, i, seed, stream)` (the PCG hash of the sensor
  noise, never a live RNG; vegetation owns streams 80–82, sub-streams 160–164) for its jitter (inside its
  own sub-cell, so trees never cross cell edges), acceptance, yaw (0–360°) and scale (0.8–1.2). It is
  accepted when the land cover under it is code 10 and `u < density_scale × 0.15`. At density 1 that is
  0.15 × 17² / 100² = one tree per 231 m² of tree cover, **about 4 % above the nominal one per 240 m²**
  (0.15 / 6²) the spec quotes.
- **Land cover** (`Vegetation/VegetationLandCover.h`): the scatter reads the package's `landcover/` tiles
  (or `thermal.land_cover.dir`) itself, on a worker, with its own 16-tile LRU; it never shares the CIGI
  material-code lookup's game-thread budget. Density is binary per 10 m texel.
- **Seed:** the scene package manifest's `seed` mixed with a vegetation constant (`VegetationSeed`);
  `vegetation.seed` only when there is no package. Pendleton's package has manifest seed 0, which gives vegetation seed 186258786 (as `vegetation.seed: 0` does).
- **Heights:** one `ACesium3DTileset::SampleHeightMostDetailed` batch per cell on the terrain tileset (the
  one tagged `CamSimTerrain`, never the buildings tileset). A failed sample drops that tree only.
- **Trees inside building footprints are dropped when the package has buildings** (REALISM R3,
  `features/buildings/`). Land cover alone would put ~10 % of footprint area under trees: WorldCover tags a
  texel by its canopy, so tree cover runs over and between suburban houses (on Pendleton 10.4 % of the 23.0 km² of
  footprints is code 10, about 10,400 trees at density 1), and a 7–11 m tree would poke its crown through a
  3–8 m flat roof. When the buildings tileset (tag `CamSimBuildings`) exists, each cell's height stage runs a
  second `SampleHeightMostDetailed` batch on it for the same points, beside the terrain batch; both must answer
  (or the stage times out, as above) before the cell spawns. A tree whose buildings sample succeeds more than
  0.5 m above its terrain height is dropped (`CamSimVegetation::KeepClearOfBuildings`); a failed buildings sample
  is a miss (no building there) and keeps the tree. Without a buildings tileset (no layer, or
  `scene.buildings.enabled: false`, or `vegetation.drop_in_buildings: false`) there is no second batch and nothing
  changes. CamSim runs nothing extra on the game thread beyond the per-cell spawn, but Cesium loads the buildings'
  most detailed (z15) tiles under every tree point to answer the queries: in `dense_pass` the check costs about
  +0.3 ms game p50, +2.5 ms game p95 and +34 MiB, which puts buildings SSE 64 + trees at +3.61 ms game p95, over the 3.0 ms
  limit. The user chose on 2026-10-11 to keep the check on by default and accept that overrun; a build-time footprint
  exclusion in the scene package (no runtime sampling) is the tracked follow-up (`scene-packages.md`, "R3/R4 bench
  check and combined cost"; ROADMAP Realism track). The dump line carries `in_buildings` (trees
  dropped in that cell), and when streaming settles with new drops a `Vegetation: settled, N cells spawned, M trees
  dropped inside building footprints so far` line is logged (at most once per 30 s).
- **Cells** (`Vegetation/VegetationCellActor.h`): one actor per cell with a `UCesiumGlobeAnchorComponent` at
  the cell centre and one `UHierarchicalInstancedStaticMeshComponent`, instances in the cell's
  East-South-Up frame, so cells survive origin shifts. Collision off, `bCastShadow = cast_shadows`,
  `InstanceEndCullDistance = radius_m` (hard cut, no fade), no custom depth or stencil.
- **Streaming** (`Vegetation/VegetationStreaming.h`, `VegetationManager.h`): every tick the cells whose centre
  is within `radius_m` of the camera's ground point are wanted, nearest first; cells beyond 1.2 × `radius_m`
  are dropped. At most 4 cells are in the generate/height stages at once and at most 2 cell actors are
  spawned per tick. A stage that never answers (Cesium drops the callback when a tileset is destroyed)
  times out after 1800 ticks (60 s) and leaves that cell empty.
- **Readiness:** `/ready` does not wait for trees; they fill in within seconds of the terrain.
- Without land cover (no package, no `thermal.land_cover.dir`) or without a loadable library, vegetation stays
  off with one `Vegetation: off (...)` log line. `Vegetation: on (...)` logs the type, radius, density,
  shadows, seed, land-cover directory and whether the buildings check is on.

## Configuration

`vegetation:` in `deploy/camsim_config.yaml`, every key startup only; table with env vars and ranges in
[`configuration.md`](configuration.md#vegetation-vegetation).

| Key | Env | Default |
|---|---|---|
| `enabled` | `CAMSIM_VEGETATION_ENABLED` | `true` |
| `library_dir` | `CAMSIM_VEGETATION_LIBRARY_DIR` | `""` (`Content/NonUFS/Vegetation`) |
| `radius_m` | `CAMSIM_VEGETATION_RADIUS_M` | `1000` |
| `density_scale` | `CAMSIM_VEGETATION_DENSITY_SCALE` | `1.0` |
| `cast_shadows` | `CAMSIM_VEGETATION_CAST_SHADOWS` | `true` |
| `cell_size_m` | `CAMSIM_VEGETATION_CELL_SIZE_M` | `100` |
| `seed` | `CAMSIM_VEGETATION_SEED` | `0` (used only without a package) |

The radius / density / shadows defaults are the spec's starting values. The bench sweep ("Measured",
"Bench") proposes keeping them; the final choice is the user's.

## Library format

A library is a directory with `library.json` and the glTF files it names (`vegetation.library_dir`,
absolute, to mount your own):

```json
{"format": "camsim-vegetation-1",
 "types": [{"name": "tree_broadleaf_a", "gltf": "tree_broadleaf_a.glb", "height_m": 9.0,
            "thermal_class": "tree_canopy", "licence": "CC0-1.0",
            "source_url": "https://quaternius.itch.io/textured-lowpoly-trees", "sha256": "f493…7e0c"}]}
```

`height_m` is the model's real height. The loader refuses any other `format`, a missing file and a sha256
mismatch (lowercase hex). The slice uses the first type only. To add or replace a type: prepare the
model with `scripts/vegetation/prepare_tree.py` (below; CC0 only), paste the entry it prints, and check it
live with `camsim.Vegetation.Probe`.

## Console variables and commands

| Name | Use |
|---|---|
| `camsim.Vegetation.DumpPath <abs file>` | Append one JSON line per spawned cell: `{"row", "col", "hash", "instances": [[lat, lon, height_m, yaw_deg, scale], ...]}`. `hash` is `HashInstances` over the instances' bits (lat, lon, yaw, scale, type; not heights). A cell spawned again (after a drop) gets a new line. |
| `camsim.Vegetation.GroundCheck 1` | Once the view has settled (terrain gate open for 2 s) and streaming is idle, traces 200 spawned tree bases against the terrain (the DIS clamp's complex trace) and logs `Vegetation: ground check n=200 max_dz_m=… p50_dz_m=…`, then resets itself. |
| `camsim.Vegetation.Probe <abs .glb>` | Debug: load a model, log its materials' instanced-usage flags, spawn a 10 × 10 grid at the camera nadir (below). |

Both CVars work from `-ExecCmds=` at launch (as `vegetation_check.py` sets them).

## Asset

The library ships one tree type, `tree_broadleaf_a`, in
`unreal_project/CamSimTest/Content/NonUFS/Vegetation/` (`library.json` + `tree_broadleaf_a.glb`,
git LFS).

| | |
|---|---|
| Model | Quaternius "Textured LowPoly Trees" (May 2020 pack), `OBJ/Tree_7.obj`: round broadleaf crown, bark + leaf-card textures (`Textures/Tree_Bark.jpg`, `Textures/Tree_Leaves.png`) |
| Source | https://quaternius.itch.io/textured-lowpoly-trees |
| Licence | CC0 1.0. Source page (checked 2026-10-10): "Free for everyone to use in any project, even comercially! (CC0)", asset licence "Creative Commons Zero v1.0 Universal". The pack's `License.txt`: "CC0 1.0 Universal (CC0 1.0) Public Domain Dedication https://creativecommons.org/publicdomain/zero/1.0/" |
| Triangles | 1,332 (budget 10,000; no simplification needed) |
| Height | 9.0 m (scaled by the `camsim_scale` root node; base at y = 0) |
| Materials | `Bark` opaque; `Tree_Leaves` alpha MASK (cutoff 0.5), double-sided |
| sha256 | `f49350f8d9faff838d0520a3e1f0ac5b6841d9bb058f27301f70a50e5b9e7e0c` |

Other candidates: Poly Haven has no tree under 10k triangles (smallest `quiver_tree_02` 153,887 polys),
so it was not tried; the QGIS hub model was not needed.

### Preparing a model

`scripts/vegetation/prepare_tree.py` (stdlib; needs `npx`) converts OBJ with `obj2gltf`
(`--checkTransparency`), simplifies to `--max-tris` with `gltfpack -si` (no quantisation or
compression, which glTFRuntime may not decode), turns alpha-blended materials into double-sided MASK,
scales the model to `--height-m`, writes `<name>.glb` and prints the `library.json` entry. It refuses
anything but `CC0-1.0`. The output is byte-for-byte reproducible from the same inputs. The Quaternius
MTLs name no textures, so `--texture MATERIAL=PATH` binds them:

```bash
Q=".cache/veg/src/quaternius/Textured Stylized Trees - May 2020"   # the unzipped pack
python3 scripts/vegetation/prepare_tree.py "$Q/OBJ/Tree_7.obj" \
  --name tree_broadleaf_a --height-m 9.0 --max-tris 10000 --licence CC0-1.0 \
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
PKG="$PWD/.cache/scene-packages/pendleton-r2"   # the package the pre-slice baseline was captured with
caffeinate -ims uv run --project scripts/scene --with pycocotools python scripts/vegetation_check.py \
    --scene "$PKG" --baseline .cache/veg/baseline --out .cache/veg/check [--stages approach,off,gt,truck,oblique]
```

macOS, local build, one CamSim at a time (it launches about nine). The baseline is two pre-slice
`run_bench.py --smoke --site pendleton` runs (`a/`, `b/`). Writes `report.json` (exit 0 only when every
gate passes) and the shots. `--stages` reruns a subset and keeps the other gates from an existing
`report.json`.

| Gate | What |
|---|---|
| placement | every dumped tree's (lat, lon) is code 10 in `$PKG/landcover` (read with `bench/dense_path.LandCover`) |
| grounding | `camsim.Vegetation.GroundCheck`: n = 200 and max \|dz\| < 0.5 m, in both runs |
| determinism | approach the riparian pose from 3 km north and from 3 km south (30 s): every cell in both dumps has the same hash |
| origin_shift | `/snapshot` at the riparian pose, 20 s at lon + 3.41° (`far_origin`), back: SSIM ≥ the baseline pair's SSIM − 0.01, and every cell spawned again has its first hash |
| off_switch | `CAMSIM_VEGETATION_ENABLED=0` smoke shot vs the baseline: SSIM ≥ the baseline pair's SSIM − 0.005 (live frames are not bit-identical run to run), and the off run's log has no vegetation line; a trees-on smoke is measured beside it to show the margin (little: see "Measured") |
| gt | `scripts/gt_occlusion_check.py` exits 0 with vegetation on (its San Francisco default config, so trees from the committed SF land-cover sample, Presidio included) |

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
  the 217 north and 368 south cells has the same hash as in the run above (`in_buildings` 0 everywhere; the log says
  `buildings none`).

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

## Known limits

- Without a buildings layer, trees stand inside building footprints wherever WorldCover calls the canopy tree
  cover (~10 % of footprint area on Pendleton). With one, they are dropped (How it works); trees right beside a
  wall can still lean their crowns over a roof, since only the trunk point is tested.
- One species; density is tree / no tree per 10 m texel (no canopy %), so woodland edges follow the
  land-cover grid.
- Hard pop-in at `radius_m`; no impostors beyond it, the imagery shows distant trees.
- Trees have no collision: CIGI LOS passes through them, DIS ignores them.
- `/ready` does not wait for trees.
- Leaf-on all year (NAIP is leaf-on too).
