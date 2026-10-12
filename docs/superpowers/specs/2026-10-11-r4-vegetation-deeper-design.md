# Vegetation, deeper (REALISM R4) — design

2026-10-11. Follows the R4 trees slice (`2026-10-10-r4-trees-slice-design.md`, `docs/vegetation.md`) and R3b's footprint
mask. One spec for three parts, implemented in order: **A** species and shrubs, **B** canopy rasters, **C** a far tier.
Runtime glTF only: no shader change, no editor work.

## Goal

The real sites are vegetation-dominated (Eglin: pine forest; Pax River: forest and wetland; Yuma: desert scrub;
`operational-sites`), and today all of them would get one broadleaf tree on WorldCover code 10 only, out to a 1 km hard
cut. ATR training needs vehicles in realistic tree lines, under realistic canopy and in scrub. This work adds:

- shrubs and scrub on shrubland and grassland (codes 20, 30) and wherever a package's mix asks for them;
- several species (pine, broadleaf, desert and chaparral shrubs), chosen per land-cover code by the scene package;
- tree density and height from canopy cover and canopy height (Meta/WRI CHMv2), instead of one uniform density;
- a far tier out to 4 km whose edge dissolves instead of cutting.

Decisions (user, 2026-10-11): one spec for all three parts; the species come from a **weighted mix per land-cover code
in the scene package** (no LANDFIRE EVT layer); **CHMv2 for both** canopy cover and height; reach by a **far LOD tier**
(no octahedral impostors, no editor work).

Success, on Camp Pendleton (M1 Pro / Metal), package `pendleton-r5veg` (= `pendleton-r4` + canopy + mix):

- Species follow the package mix; shrubs stand only on codes whose mix lists shrubs; trees only where the canopy layer
  has cover (or on mix codes with the R4 rule, without a canopy layer).
- Tree-cover fraction per 100 m cell matches CHMv2 within the tolerance below.
- Every cell and far block is deterministic from two camera paths and after an origin shift.
- Trees are visible to 4 km with no hard edge, and the bench sweep fits the existing budget at the chosen defaults.
- A package with no mix and no canopy layer, with shrubs and the far tier off, gives R4's tree cells bit for bit.

## Findings this rests on

- **Assets.** The cached Quaternius "Textured LowPoly Trees" pack (CC0, already the source of `tree_broadleaf_a`;
  `.cache/veg/src/quaternius`) holds 10 `Pine`, 20 broadleaf `Tree`, 20 `Birch` and 20 `DeadTree` models. It has no
  shrubs. Two CC0 candidates for shrubs: Kenney "Nature Kit" (kenney.nl/assets/nature-kit, CC0 on the page, 330 files
  tagged tree/foliage; contents not yet listed) and Quaternius "Stylized Nature MegaKit" (CC0, OBJ/FBX/glTF, "grass,
  bushes and more"; only part of it is free). Neither is downloaded yet: the first plan task checks them (below).
- **CHMv2** (`s3://dataforgood-fb-data/forests/v2/global/dinov3_global_chm_v2_ml3/`, public, HTTPS at
  `https://dataforgood-fb-data.s3.amazonaws.com/...`, CC BY 4.0): one GeoTIFF per level-10 quadkey
  (`chm/<quadkey>.tif`, e.g. Pendleton `0230132032`) plus `tiles.geojson` (59 MB index) and `metadata/`. Checked on
  `0230132032`: 32768² **uint8 metres**, EPSG:3857, 1.194 m pixels (≈ 1.0 m on the ground at 33° N), 512² tiles,
  deflate, overviews 2–64, no nodata value; **141 MB**. Downsampled percentiles over the tile: p50 0 m, p90 3 m, p99 10 m.
- **Download size.** Canopy is built over the package `bbox` (scene.toml), not the land-cover ring: Pendleton's bbox
  [-117.62, 33.19, -117.24, 33.52] (0.38° × 0.33°, ≈ 35 × 37 km) touches 2–4 level-10 quadkeys ≈ 0.3–0.6 GB, downloaded
  once into the fetch cache. Cover needs the full resolution (the share of 1 m pixels over 3 m); the overviews'
  resampling isn't documented. Outside the bbox the runtime uses the no-canopy rule (below).
- **Land-cover grid** (`camsim-landcover-1`): 0.05° tiles, 600 × 600 PNG, row 0 north, named by the south-west corner;
  9.3 m × 7.8 m texels at 33° N. The canopy layer reuses the grid exactly, so a texel of canopy and of land cover are
  the same ground.
- **Hash streams.** Vegetation owns streams 80–82 (sub-streams 160–165); 160–164 are used (jitter E/N, accept, yaw,
  scale). Stream rule: stream s owns 2s and 2s + 1 (`SensorHash.h`).
- **Cost drivers** (R4 bench, `docs/vegetation.md` "Bench"): the cell count (the radius) drives game-thread cost; density
  and shadows were within noise. 2000 m at 100 m cells (~1,260 cells) failed game p50. So the far tier must not add
  100 m cell actors: it uses 400 m blocks.
- **HISM culling.** `InstanceStartCullDistance` is where a per-instance dithered fade *begins* (it needs
  `PerInstanceFadeAmount` in the material, which glTFRuntime's material lacks); it is not a near cull. Only
  `InstanceEndCullDistance` (per component) is usable as a hard cut. So the near/far handoff is decided per block, not by
  cull distances. (The in-chat design said StartCullDistance; corrected here.)
- **Thermal.** Pixels take their class from the land cover under them. Shrubs on 20/30 are already `shrubland` /
  `grassland`; trees on code 10 `tree_canopy`. Trees placed by canopy on other codes (street trees on 50) take that
  code's class: a known limit (per-instance thermal class stays deferred).

## Design

### A1. Library v2 (`Vegetation/VegetationLibrary`, `library.json`)

`"format": "camsim-vegetation-2"`; `camsim-vegetation-1` is still read (every type becomes role `tree`, no far mesh,
crown radius from the default below).

```json
{"format": "camsim-vegetation-2",
 "types": [{"name": "tree_pine_a", "role": "tree", "gltf": "tree_pine_a.glb", "sha256": "...",
            "far_gltf": "tree_pine_a_far.glb", "far_sha256": "...",
            "height_m": 14.0, "crown_radius_m": 2.5,
            "thermal_class": "tree_canopy", "licence": "CC0-1.0", "source_url": "..."}]}
```

- `role`: `tree` | `shrub` (default `tree`). `crown_radius_m` (default 0.3 × `height_m` for trees, 0.5 × for shrubs):
  the radius used to turn canopy cover into a density (B). `far_gltf` (trees only, optional): the far-tier mesh; a tree
  without one is not drawn in the far tier (logged once). Every file is sha256-checked as today.
- Type names are unique; the loader refuses duplicates, an unknown role and a shrub with a `far_gltf`.
- **`prepare_tree.py`** gains `--role`, `--crown-radius-m` and `--far-tris N` (default 150): a second gltfpack pass to
  ≤ N triangles with textures downscaled to 128 px, written as `<name>_far.glb`; the printed entry includes it. Still
  CC0 only, still byte-reproducible.
- **Starting library** (`Content/NonUFS/Vegetation`, git LFS): `tree_broadleaf_a` (existing, gets a far mesh),
  `tree_pine_a`, `tree_pine_b` (two Quaternius `Pine_*`; `tree_broadleaf_a` stays first, index 0, for the R4
  bit-identity below), and 2–3 shrubs: `shrub_chaparral` (dense rounded bush,
  coastal sage / chaparral), `shrub_desert` (sparse, creosote-like), and `cactus_a` if a CC0 kit has a usable one.
- **Plan task 1 (risk):** download Kenney Nature Kit and the free Quaternius MegaKit into separate empty directories;
  record each licence file's text and the page's wording (as `docs/vegetation.md` does for the tree); pick shrubs; check
  them live with `camsim.Vegetation.Probe` (materials, masking, scale) before any scatter work. If neither kit has a
  usable CC0 shrub, the fallback is a procedural shrub (the slice's `procedural` path, `CAMSIM_VEGETATION_PROCEDURAL`,
  a low-poly ellipsoid), so A is never blocked; the user is told.

### A2. Species mix (scene package → runtime)

- Package config (`camsim-scene`, TOML), optional:

  ```toml
  [vegetation]
  mix.10 = { tree_pine_a = 0.6, tree_pine_b = 0.25, tree_broadleaf_a = 0.15 }
  mix.20 = { shrub_chaparral = 1.0 }
  mix.30 = { shrub_chaparral = 1.0 }
  shrub_density.20 = 8.0     # shrubs per 100 m^2 of that code
  shrub_density.30 = 1.5
  ```

  Keys are land-cover codes (WorldCover and the R2/R3b burned codes); weights are positive and normalised per code
  and role. A code may list trees and shrubs. `build` writes `vegetation/mix.json` (`"format": "camsim-vegmix-1"`,
  the parsed table), lists it in the manifest and `hashes.txt`; `verify` checks it parses. The tooling does not know
  the library, so it doesn't check names.
- **Runtime** (`Vegetation/VegetationMix.h`, pure): `FVegetationMix` = per code, a tree list and a shrub list of
  (library type index, cumulative weight) + shrub density. Built at startup from `<scene.dir>/vegetation/mix.json` if
  present, else `vegetation.mix_file` (absolute, for package-less runs), else the **built-in default**:
  `10 → tree_broadleaf_a` (R4), `20 → shrub_chaparral` at 6 / 100 m², `30 → shrub_chaparral` at 1 / 100 m² (both shrub
  densities are starting values, set by the bench sweep). Missing default types are dropped from the default silently
  (a library without shrubs gives R4).
- A name the library lacks: one warning per name, the entry is dropped and the code's weights renormalised; a code left
  empty is no vegetation. With `scene.offline: true` it is a config error (exit 1, as other config errors).
- **Site mixes** are documented, not shipped as code: `docs/vegetation.md` gets example `[vegetation]` blocks for
  Pendleton (10 broadleaf; 20 chaparral dense; 30 sparse), Eglin (10 ≈ 85 % pine; 30/20 sparse low shrubs; 90 shrubs),
  Pax River (10 mixed pine/broadleaf; 90 wetland shrubs) and Yuma (20/30/60 sparse desert scrub, cactus where present).

### A3. Scatter v2 (`Vegetation/VegetationScatter`, pure)

`GenerateCell` returns trees and shrubs (`FCellContent { TArray<FTreeInstance> Trees, Shrubs; }`); `FTreeInstance.Type`
is the library index. The candidate draws are stateless hashes, so adding draws never changes existing ones.

- **Trees** (R4 grid, 17 × 17 at 100 m, sub-streams 160–164 unchanged): per candidate, after the jitter, look up code
  c = land cover and canopy (cover, height) at the point. The code's tree list empty → no tree. Species: sub-stream
  **165**, cumulative weights. Acceptance u (sub-stream 162, as R4) against:
  - no canopy layer: p = 0.15 × `density_scale` (R4);
  - canopy layer: if height < 3 m or cover = 0, no tree; else s = clamp(height / type.height_m, 0.5, 2.0) and
    p = clamp(cover × A_cand / (π (r_crown s)²) × `density_scale`, 0, 1), A_cand = (cell / N)². The expected crown
    area per candidate then equals the cover until p saturates: one crown per 34.6 m² candidate caps crown cover at
    π r² / 34.6 m², ≈ 0.57 for a 2.5 m pine crown and 0.66 for the 2.7 m broadleaf (≈ 290 trees/ha, a dense stand).
    Denser canopy is drawn at that cap (a known limit).
  Scale: canopy layer: s × (0.9 + 0.2 u_scale); none: R4's 0.8–1.2. Then the building mask (R3b, unchanged).
  **With no canopy, a mix of `10 → [one type]` and that type at index 0, the trees are R4's bit for bit** (the species
  draw doesn't change the others; type 0 hashes as R4's).
- **Shrubs** (new grid, spacing `shrub_spacing_m` 3 m → 33 × 33 per 100 m cell; streams 83–85, sub-streams
  166 jitter E, 167 jitter N, 168 accept, 169 yaw, 170 scale, 171 species): accepted when the code's shrub list is
  non-empty and u < shrub_density(c) / 100 × A_cand × `density_scale`; scale 0.7–1.3; the building mask applies.
  Shrubs ignore canopy (CHMv2 doesn't resolve sub-3 m shrubs reliably); they may stand under tree crowns (understory).
- `HashInstances` covers trees then shrubs (type included); the dump line gets `"shrubs": [...]` beside `"instances"`.
- `SensorHash.h`'s stream comment is updated: vegetation owns streams 80–86 (sub-streams 160–173; 172–173 in C).

### B1. Canopy layer (`camsim-scene`)

- **Source** `sources/chmv2.py`: the quadkeys at level 10 covering the package bbox; each `chm/<qk>.tif` downloaded whole into the fetch cache (resumable, size + ETag checked, as other
  sources). A key the bucket doesn't have (ocean) is "missing" → no canopy there. The acquisition dates come from
  `metadata/` (or `tiles.geojson`) for the tiles used, recorded in the manifest. Licence `CC-BY-4.0` (already on the
  default allow-list); attribution: "Canopy height: Meta and World Resources Institute (WRI), CHMv2 (2025), CC BY 4.0".
  The exact attribution text is taken from the registry page in the plan task.
- **Layer** `layers/canopy.py`, format `camsim-canopy-1`: two tile sets on the land-cover grid (0.05° tiles,
  600 × 600 8-bit greyscale PNG, row 0 north, same names), `canopy/cover/` (cover in %, 0–100) and `canopy/height/`
  (canopy height in m, 0–255), each with its own `index.json` (land cover's keys, `"format": "camsim-canopy-1"`,
  `"band": "cover"|"height"`), so the runtime reuses `FLandCoverTileCache`. Tiles cover the package bbox. Per texel,
  from every CHM pixel whose centre falls in it (pixel centres reprojected 3857 → lat/lon): cover = share of pixels
  ≥ 3 m; height = the nearest-rank p90 (sorted[floor(0.9 (n − 1))]) of those pixels (0 if none). A texel no CHM tile
  covers (a quadkey the bucket lacks, i.e. ocean) is cover 0, height 0. `[canopy] enabled = true` turns it on (default
  off, so existing package configs are unchanged).
- **verify:** index parses, every listed tile exists with the right size and mode, cover ≤ 100, hashes match.
  **Tests** (no network): a synthetic 3857 CHM fixture → known cover/height per texel, edge texels, a missing quadkey,
  determinism (same bytes twice).
- The R3b footprint mask is not burned into canopy (the runtime mask already drops trees in buildings).

### B2. Canopy at runtime (`Vegetation/VegetationCanopy`)

`FVegetationCanopy`, like `FVegetationLandCover`: two `FLandCoverTileCache`s (cover, height; 16 tiles each, ~11.5 MB
together; the cache takes the expected index format), `Sample(Lat, Lon) → TOptional<{CoverFrac, HeightM}>` thread-safe,
misses decode on the caller (the scatter worker); unset where the canopy layer has no tile (outside the package bbox),
and there the scatter uses the no-canopy rule. Found at `<scene.dir>/canopy/cover/index.json` +
`canopy/height/index.json`; `vegetation.canopy_enabled: false` ignores it. An unusable index logs one warning and
falls back to the no-canopy rule; a bad tile reads as no canopy there (one warning per tile, not retried), as the mask.
The `Vegetation: on (...)` line gains `canopy: chmv2|none|off`, `mix: package|file|default`, `shrubs`, `far`.

### C. Far tier (`Vegetation/VegetationStreaming`, `VegetationManager`, `VegetationCellActor`)

- **Blocks.** Streaming moves from cells to **blocks** of `far_block_cells` × `far_block_cells` cells (default 4 → 400 m
  at 100 m cells; a block is a fixed group of cell ids, so cell contents are unchanged). Each wanted block is in one
  state:
  - **near**: its 16 cell actors, full meshes, shadows per `cast_shadows`; shrubs in a second HISM per cell with
    `InstanceEndCullDistance` = `shrub_radius_m` (default 500).
  - **far**: one block actor, globe-anchored at the block centre, trees only, `far_gltf` meshes, no shadows.
- **Which state.** By the block centre's ground distance d: near when d < `radius_m`; far when d < `far_radius_m`
  (default 4000; 0 = no far tier, blocks beyond `radius_m` are dropped as today). Hysteresis as today's drop rule: a near
  block goes far only past 1.2 × `radius_m`, a far block is dropped past 1.1 × `far_radius_m`. **No gaps:** a block
  switching state keeps its old actors until the new ones have spawned. Near-tier cull distance becomes
  `radius_m` × 1.2 + block diagonal, so near blocks are never cut mid-block.
- **Far content.** The same `GenerateCell` for the block's cells (same trees, same mask), then kept when
  Uniform(Hash(col, row, K, seed, **172**)) < `far_keep` (default 0.35), scale × `far_scale` (default 1.3; full canopy
  mass would need 1/√0.35 = 1.69, which over-sizes crowns; the bench shots decide). Heights: one
  `SampleHeightMostDetailed` batch on a 17 × 17 grid (25 m) per block, bilinear, trees sunk 0.3 m; a failed grid point
  drops the trees in its quad.
- **Dissolve.** Each far actor splits its trees by Uniform(Hash(..., **173**)) into 4 HISMs with
  `InstanceEndCullDistance` = 0.7 / 0.8 / 0.9 / 1.0 × `far_radius_m`: the outer 30 % thins in four steps instead of a
  hard edge. No material work, no per-frame cost.
- **Budgets.** At most 4 blocks/cells in flight and 2 actor spawns per tick, as today (near cells count individually,
  a far block as one). Near blocks are processed nearest first, then far blocks nearest first.
- **Readiness** unchanged: `/ready` doesn't wait for vegetation.

### Config (`vegetation:`, all startup only, each with a `CAMSIM_VEGETATION_*` env override; `docs/configuration.md`)

| Key | Default | Range |
|---|---|---|
| `mix_file` | `""` | absolute path; used only without a package mix |
| `canopy_enabled` | `true` | acts only when the package has `canopy/` |
| `shrubs_enabled` | `true` | |
| `shrub_radius_m` | `500` | [0, `radius_m`] |
| `shrub_spacing_m` | `3.0` | [1, 10] |
| `far_radius_m` | `4000` | 0 or [`radius_m` × 1.5, 10000] |
| `far_block_cells` | `4` | [2, 8] |
| `far_keep` | `0.35` | (0, 1] |
| `far_scale` | `1.3` | [1, 2] |

Defaults are starting values; the bench sweep (below) proposes the final ones and the user chooses, as in R4.

## Testing (automation, `CamSim.Vegetation.*`, NullRHI)

- Library v2: parse, v1 compatibility, roles, far mesh sha, duplicate/unknown-role/shrub-far refusals.
- Mix: parse `camsim-vegmix-1`, normalisation, unknown names dropped + renormalised, offline error, the built-in default,
  default with a library lacking shrubs = R4.
- Scatter: **R4 bit-identity** (no canopy, default mix, shrubs off: same instances and hash as the slice's golden cells);
  species frequencies vs weights (χ² over many cells); canopy acceptance (expected crown fraction vs cover for cover
  0.1–0.5, within 2 pp over 10⁴ cells); height → scale and clamps; no tree below 3 m; shrubs only on codes with shrubs;
  shrub density; the mask applies to both; cell edges respected.
- Far tier: the far set is a subset of the near set (positions and types), keep fraction, dissolve buckets, the 25 m grid
  interpolation, block ↔ cell ids.
- Streaming: block states with hysteresis, no gap during a switch (old actors kept until the new ones spawn), far off =
  only near blocks, budgets.
- Canopy runtime: both tile sets, sampling at tile edges, unset outside the layer, bad tile / bad index fallbacks.
- Scene tooling (`scripts/scene/tests`): the canopy layer fixtures (B1), the mix TOML → `mix.json`, verify.

## Acceptance (`scripts/vegetation_check.py`, Pendleton `pendleton-r5veg`, macOS/Metal; Linux joins `FOR_LINUX.md`)

Existing gates stay (placement, grounding, determinism, origin shift, off switch, gt, footprints). Placement becomes:
every tree is on a code whose mix lists trees and (with canopy) on a texel with cover > 0 and height ≥ 3 m; every shrub
on a code whose mix lists shrubs. New gates:

| Gate | What |
|---|---|
| species | per code, dumped type counts vs the mix: χ² p > 0.001 |
| canopy | per 100 m cell with mean CHMv2 cover in [0.1, 0.5] (≥ 50 such cells; above that the candidate grid saturates): crown fraction Σ π (r s)² / cell area vs mean cover; median abs error ≤ 10 pp |
| far_determinism | far blocks dumped from the two approach paths: same hashes; every far tree is in the near dump of its cell |
| handoff | a 60 s straight run at 120 m HAE: no block is ever without actors between its first spawn and its drop (log check) |
| long_range | a 5 km-altitude oblique pass (EO): 0 dropped frames and game p95 within the budget below |
| r4_identity | `pendleton-r2` (no canopy, no mix) with shrubs and far off: every cell hash equals the R4 run on disk (`.cache/veg/check-i1-r2/`) |

Informational: shots (near oblique over chaparral, a far oblique showing the dissolve, Eglin-style pine mix via a test
mix file), a DIS truck in a tree line and in chaparral (visibility, boxes).

**Bench** (`scripts/bench/sweep.py`, `dense_pass` and the new long-range phase): axes `far_radius=0,2000,4000,6000`,
`far_keep=0.25,0.35,0.5`, `shrub_radius=0,300,500,1000`, shrubs on/off; budget as R4 after the 2026-10-11 raise: game
p50 ≤ +1.5 ms, p95 ≤ +3.0 ms, GPU ≤ +5.0 ms, memory ≤ +1024 MiB over all-off. The user picks the defaults from the table.

## Docs and bookkeeping

`docs/vegetation.md` (library v2, mix, canopy, far tier, site mixes, new gates, measured results, known limits),
`docs/scene-packages.md` (`[vegetation]`, `[canopy]`, the CHMv2 source and its download size), `docs/configuration.md`,
`deploy/camsim_config.yaml`, `REALISM.md` R4 (what's done, what's still deferred), `ROADMAP.md`, `CLAUDE.md` (the
vegetation gotcha: streams 80–86, blocks, mix/canopy), `docs/realism/data-sources.md` (CHMv2 tile facts above).
No human editor work.

## Known limits

- Trees placed by canopy on non-tree codes take that code's thermal class (per-instance thermal class deferred).
- CHMv2 is a model estimate from imagery of uneven date; the canopy gate measures agreement with CHMv2, not with truth.
- Crown cover saturates at ≈ 0.57–0.66 (one tree per 34.6 m² candidate); closed forest above that is drawn at the cap.
- The near/far switch is an LOD pop at block granularity (400 m), not a fade.
- Shrubs end at `shrub_radius_m` with a hard cut (they are small at that range; the bench shots judge it).
- Still no collision (CIGI LOS and DIS ignore vegetation), no grass, leaf-on all year.

## Out of scope

LANDFIRE EVT species raster; octahedral impostors; grass; mapped trees (OSM `natural=tree`); per-instance thermal class;
tree collision; utility poles, fences, clutter; seasonal leaf state.
