# CamSim Realism Roadmap

Started 2026-10-07. This plans the scene-content side of realism: what the world contains, where it
comes from, and how it gets into a running CamSim. Sensor physics (Milestone 3) and thermal IR
(Milestone 4) are in `ROADMAP.md`; this track builds on them and feeds them.

It began as a review of an externally generated proposal ("bounding box → generate the world",
written for a hypothetical app called SpectraForge). The review is in the next section. What held
up is kept below, adapted to CamSim's architecture; what didn't is corrected.

**Scheduling.** `ROADMAP.md` rule 5 says no new feature phases until Milestones 0–4 land. 4C's
visual review was signed off on 2026-10-07; 4D (semantic class in ground truth, validation, thermal
shadow lag) is still to do. R0 went ahead as offline tooling with no runtime change. **Rule 5 waived for this
track (2026-10-09):** R1 onward proceed before 4D; 4D stays open and runs alongside.

**Status.** R0 spike done 2026-10-07 (`docs/realism-r0-spike.md`; research in `docs/realism/`): a
Camp Pendleton package (3DEP terrain + NAIP imagery) rendered from local files with the internet
blocked, registered to < 1 px against Cesium World Terrain + Bing, with CIGI frame-centre heights
within 0.9 m of 3DEP truth. R0 done 2026-10-09 (`camsim-scene`, `docs/scene-packages.md`): gates 1–6 pass,
including the offline profile. Left from R0: the offline run on Linux/Vulkan (`docker run --network none`) and
a SquashFS vs plain-directory cold-start timing. **R1 chunk 1 (NAIP edge) accepted on Pendleton 2026-10-09**
(`docs/scene-packages.md`, "Measured"): colour bias median 2/2/2 DN, open water within 1.10 DN of raw Sentinel-2,
identical rebuild; imagery build +20.9 % per tile (over the +10 % spec gate, accepted). Known limits: a dark ~200 m
NAIP disc around piers; the turquoise shelf outline (ocean, not imagery). Next: the rest of R1.

**Scope decisions (2026-10-07).**

- **US only.** Scene packages cover the United States (CONUS first; Alaska, Hawaii and territories
  where 3DEP and NAIP cover them). US federal data (3DEP, NAIP, NHD) is public domain, high
  resolution and consistent, so it is the default. Outside a scene package CamSim still runs
  anywhere, exactly as today.
- **No photogrammetry.** It is out of scope for now (reasons in section 1).
- **Offline.** Packages must eventually work air-gapped: no Cesium ion, no internet (principle 8).
- **First test area: Camp Pendleton** (bbox W -117.62 S 33.19 E -117.24 N 33.52, ~1,300 km²): land,
  water and terrain. Its interior has no public LiDAR or 1 m DEM (only ~10 m), which makes it the
  fallback case too; the fringes (Oceanside, San Clemente, Fallbrook) have QL1 LiDAR.

---

## 1. Review of the source proposal

### What holds up

- **Automating world construction from a bounding box.** Open data is now good enough for this.
  CamSim already does it in a small way: `scripts/landcover/fetch_worldcover.py --bbox` cuts ESA
  WorldCover tiles for thermal IR.
- **Keeping data acquisition out of Unreal.** Agreed, and it matches existing practice: fetch
  offline, commit or mount the result, and need no network at runtime.
- **An intermediate scene description between the data sources and the renderer.** Agreed. Here
  it becomes a scene package with a manifest (R0).
- **Streamed vs generated.** Global terrain and imagery stay as Cesium tiles. Discrete features
  (buildings, roads, vegetation, infrastructure) are generated. This is the right split.
- **Fidelity levels** and **mission-aware fidelity.** Worth having, with the caveat below.

### What is wrong or needs correcting

| Claim in the proposal | Problem | Correction for CamSim |
|---|---|---|
| Automation percentages ("~100 %", "~95 %") | These are invented: no source, and no definition of what "automated" means. | Drop them. Each phase has measurable exit gates instead. |
| USGS DEM/3DEP, NAIP, NLCD as the core sources | All three are US-only. That is fine now that the scope is US-only (2026-10-07). But NLCD's 30 m land cover is coarser than the 10 m WorldCover data already integrated. | 3DEP and NAIP are the primary sources. WorldCover stays the land-cover base; NLCD is used only for what WorldCover lacks (impervious % and tree canopy %), not as the class map. |
| "DEM/LiDAR → terrain" | Doesn't distinguish a surface model (DSM) from a bare-earth model (DTM). If terrain included buildings and trees (a DSM, e.g. LiDAR first returns), placing generated buildings on it would count them twice. 3DEP DEMs are bare earth, which is what we need. | Generated features always sit on bare earth (3DEP DEM; outside it, the package's global base). LiDAR first returns minus ground (nDSM) give building and canopy heights. They are never used as terrain. |
| "Build Unreal world", "a giant Unreal map", "Nanite" | Imported UE assets need the editor and a cook. CamSim runs as a packaged, headless Docker image that loads its content at runtime (`docs/docker.md`). A new bbox can't require an editor pass. | Everything generated is in formats that load at runtime: 3D Tiles/glTF (Cesium, glTFRuntime), PNG rasters, JSON. Only generic asset libraries (materials, tree meshes) are cooked, once. |
| LiDAR "~95 %" | 3DEP LiDAR covers most of the US now, but quality level (QL1/QL2), collection year and point classification vary by project, and some areas are a decade old. | LiDAR is an upgrade per area (R1, R3, R4), never a dependency. The manifest records each project's quality level and year; the fallback is the 3DEP 1/3 arc-second (~10 m) DEM. |
| Photogrammetry as the top fidelity tier | Photogrammetric meshes have the capture day's lighting and shadows baked in. That fights the simulated sun, night, and thermal IR (a shadow baked into the texture is not a cool surface). Google Photorealistic 3D Tiles also come with terms of service that restrict caching and may restrict ML use. | Out of scope (decided 2026-10-07). |
| "Dynamically increase fidelity around the field of view" | Cesium already does this at runtime (screen-space error LOD). Doing it twice adds nothing. | Mission awareness belongs offline: decide which area to generate in detail and pre-warm the tile cache along the flight footprint (R5). |
| The pipeline ends at "Unreal world" | For a sensor simulator, realism means *every* channel is consistent: EO, thermal class, ground-truth labels, CIGI HAT/HOT/LOS and material codes, KLV frame centre. Nothing in the proposal mentions this. | Every generated feature carries a class that drives the thermal material, the ground-truth semantic class and the CIGI material code (principle 2). |
| Licensing isn't mentioned | Training datasets get redistributed. OSM, Overture and Microsoft's US footprints are ODbL (Microsoft's Global ML footprints became CDLA Permissive 2.0 in 2026-03); s2cloudless after 2016 is CC BY-NC-SA; Google tiles are proprietary. | A licence register in every scene package (R0). The pipeline refuses non-redistributable sources unless explicitly allowed. |
| No reproducibility | ATR training and regression testing need the same bbox to give the same scene next month. Live APIs (Overpass, imagery services) drift. | Pinned dataset versions and release dates, content hashes, and seeded placement (principle 3). |
| No performance budget | Cesium's game-thread cost follows the rendered tile count (ROADMAP 3B). Capacity is VRAM-bound (4 instances on the RTX 5080, 1 on the M1 Pro). Dense vegetation and extra tilesets aren't free. | Every runtime phase has a `scripts/bench/run_bench.py` budget gate. |

---

## 2. Principles

1. **Offline generation, runtime loading.** Python tools under `scripts/scene/` (uv project, same
   pattern as `hitl/`) produce a scene package. CamSim loads it from a directory: no network, no
   editor, and a cooked package works as is.
2. **One class per feature, used by every channel.** A road polygon is `asphalt`. The land-cover
   window, `ThermalCS`, the COCO semantic class (4D) and CIGI HAT/HOT/LOS material codes (today 0,
   ROADMAP 1.9) all read the same class. Classes extend the thermal class table (18 of 32 slots are
   free after 4B).
3. **Reproducible.** The manifest pins each source (dataset, release or snapshot date, URL, hash)
   and a seed. The same package, seed and CIGI recording give the same frames (checked in R6).
4. **Geometry on bare earth, in WGS-84 ellipsoid heights.** This matches CamSim's altitude rule.
   Heights from sources in orthometric heights (NAVD88, EGM2008) are converted at build time, never
   at runtime.
5. **Degrade gracefully.** Without a package CamSim behaves as today (Cesium World Terrain + ion
   imagery + WorldCover). A package never requires ion; within it, a missing optional layer falls
   back to a coarser one (1 m → 1/3" DEM → global base).
6. **US federal data first.** Public-domain US sources (3DEP, NAIP, 3DHP) come before
   ODbL/CC BY ones. `camsim-scene build` refuses a bbox outside their coverage.
7. **Same datum as the sources, converted once, with a pinned pipeline.** US data is NAD83(2011)
   horizontally and NAVD88 vertically. At Pendleton, NAVD88 → ellipsoid is −35.06 m (GEOID18) and
   NAD83(2011) → ITRF2014 is 1.35 m horizontally. PROJ's defaults get this wrong (`EPSG:4979` picks
   a GEOID03 chain, 0.78 m off; `EPSG:9755` drops the geoid, 34 m off), so the manifest stores an
   explicit PROJ pipeline (GEOID18 `vgridshift` + NAD83(2011)→ITRF2014 Helmert, with its coordinate
   epoch) and the GEOID18 grid is vendored with a hash. R6 checks it.
8. **Offline-capable, self-contained packages.** A package covers the whole globe (a coarse global
   base under the detailed area), and CamSim makes no network requests when it runs from one.
   Fetching happens only at build time, on a connected machine; the package then moves to the
   air-gapped host as data.

---

## 3. Architecture

```
scripts/scene/  (offline, Python)                    CamSim runtime
──────────────────────────────────                   ──────────────────────────────────────────
camsim-scene build --bbox W S E N --profile sim
  │ fetch (cached, pinned)                            scene.dir: <package>
  │   DEM/DTM, LiDAR, imagery, WorldCover,              ├─ terrain/ ──► Cesium terrain (URL, the one terrain tileset)
  │   OSM/Overture, canopy height                       ├─ imagery/ ──► Cesium raster overlay
  │ fuse (common grid / CRS, bare earth, nDSM)          ├─ landcover/ ─► FLandCoverWindow (4B, + R2 classes)
  │ generate                                            ├─ features/ ──► feature 3D Tiles (buildings, bridges)
  ▼                                                     ├─ placement/ ─► instanced vegetation / clutter (R4)
<package>/manifest.json  ─────────────────────────────► └─ manifest.json: classes, sources, licences, seed
```

**Scene package layout** (the scene description the proposal asked for, kept as files rather than a
service):

```
<package>/
  manifest.json      # bbox, profile, seed, schema version, sources[] (id, version, date, url, sha256, licence)
  ATTRIBUTION.txt    # generated from sources[]
  terrain/           # quantized-mesh layer.json + tiles (optional)
  imagery/           # TMS/XYZ tiles (optional)
  landcover/         # same format as Content/NonUFS/LandCover (index.json + PNG), extended codes
  features/          # tileset.json + glTF/GLB, per-feature class in EXT_mesh_features / metadata
  placement/         # per-tile instance lists (type, position, yaw, scale, class), seeded
```

**Fidelity profiles** (`--profile`): `preview` (3DEP 1/3 arc-second terrain, the global base
imagery, WorldCover + roads/water rasters), `sim` (+ NAIP imagery, footprint buildings,
vegetation), `high` (+ 3DEP 1 m terrain, LiDAR building and canopy heights, dense
vegetation and clutter). Same bbox, same pipeline, different manifest. One package can mix profiles
per tile (R5).

### Large areas

UAV operating areas are big: a Group 3 aircraft's mission can range over tens to hundreds of
kilometres. **Design target (assumption, to confirm): a package up to 200 km × 200 km
(40,000 km²)**, `sim` everywhere and `high` along mission corridors. That size changes the design in
five ways.

1. **Volume.** Per 1,000 km² (terrain and imagery measured in the R0 spike, the rest estimated):

   | Layer | Per 1,000 km² | 40,000 km² | Consequence |
   |---|---|---|---|
   | NAIP 0.6 m to z17, JPEG q85 (measured) | 1.2 GB, ~75 k tiles | ~48 GB, ~3 M tiles | Bytes fine; the **file count** needs an archive format (item 3). |
   | 3DEP 1 m DEM as quantized-mesh to z16 (measured) | 0.15 GB, ~20 k tiles | ~6 GB, ~0.8 M tiles | Fine; 1/3 arc-second outside corridors is far smaller. |
   | 3DEP LiDAR point clouds (LAZ, QL2 and denser) | ~5–20 GB | hundreds of GB | Never stored: streamed per tile, reduced to rasters (canopy height, building heights), discarded. |
   | Buildings (footprint extrusions) | 10⁴–10⁶ buildings | 10⁶–10⁷ | Fine as 3D Tiles. |
   | Trees, if listed one by one | ~10⁷ | ~10⁹ | Impossible as explicit lists: runtime scatter instead (R4). |

   Measured by the R0 gates (2026-10-09, 16 workers on a 24-core Core Ultra 9 285K; totals include the 100 km
   base ring and the global z0–z8 base, so they overstate the per-km² cost of a larger box):

   | Build | Area | Terrain | Imagery | Land cover | Package | Build time | Peak RSS / worker |
   |---|---|---|---|---|---|---|---|
   | `sim` (3DEP 1 m + NAIP), Pendleton | 1,295 km² | 182,602 tiles, 0.98 GB | 253,766 tiles, 1.96 GB | 43 MB | 2.9 GB (`.sqfs` 3.0 GB) | 12.5 min | 812 MB |
   | `preview` (1/3″ + WC S2), Pendleton | 9,946 km² | 183,378 tiles, 0.96 GB | 177,350 tiles, 0.81 GB | 62 MB | 1.8 GB | 11.2 min | 647 MB |

   Files on disk need 4 KB blocks: 5.5 GB and 4.0 GB, which the SquashFS image avoids. The fetch cache for the
   `sim` build was 36 GB, mostly NAIP and the global ETOPO/Blue Marble sources.

2. **Everything is tiled, out of core and resumable.** The build works one output tile at a time
   (never a whole-area raster in memory), in parallel, with a per-tile done marker so a failed
   40,000 km² build resumes, and each layer can be rebuilt alone. Tiles use one global scheme
   (geographic or Web Mercator quadtree), not UTM: a 200 km box can cross UTM zones and NAIP states
   (different flight years and colour balance), and seams must be handled as data, not avoided.
3. **Packages are volumes, not repository content, and travel as one file.** Tens of GB and
   millions of tiles per area. Cesium for Unreal reads no tile archive (no 3TZ, MBTiles, PMTiles or
   zip; the accessor chain is fixed), so: CamSim reads plain files over `file:///`, and the package
   travels as **one read-only filesystem image (SquashFS or EROFS)**, loop-mounted on the host and
   bind-mounted `:ro` into every container. One file on the transfer media (millions of small files
   would take hours to copy and ~500 GB on exFAT), one hash, no extraction, no sidecar. Fallback for
   hosts that can't mount: a zip/3TZ served on localhost by a small server, sending no cache headers
   (or Cesium copies every tile into its SQLite cache). Never bake tiles into the Docker image. Only
   small test samples stay in git LFS. Details: `docs/realism/offline-hosting.md`.
4. **Runtime cost follows the view, not the area.** Cesium streams terrain, imagery and feature
   tilesets by screen-space error, so a bigger package costs disk, not frame time. What does grow
   with area is what a high, oblique camera sees at once:
   - the land-cover window is 2048² × 10 m ≈ 20 km across; a camera at a few km altitude looking
     toward the horizon sees beyond it. Outside the window `ThermalCS` uses `terrain_default`
     (`docs/thermal.md`), so distant terrain loses its land-cover classes at a visible edge. A
     coarse outer level (e.g. 2048² at 80 m) is needed for high oblique views;
   - vegetation and clutter have to fade to the imagery and land cover with distance (sub-pixel trees
     are pure cost);
   - feature tileset SSE settings need a long-range bench (R3).
5. **Detail follows the mission.** `high` across 40,000 km² is neither needed nor affordable.
   Corridor-tiered profiles (R5) are what keep a large package buildable, so R5's corridor logic
   moves up into R0's tooling.

### What needs the editor, and what doesn't

**A new area never needs the editor, a compile or a cook.** You run `camsim-scene build`, point
`scene.dir` at the package and start the existing CamSim image. Every per-area layer is runtime data:

| Per area (no editor) | Loaded by |
|---|---|
| Terrain (quantized-mesh), imagery (TMS) | Cesium, by URL / `file://` |
| Land cover + road/water classes, NDVI | `FLandCoverWindow` (PNG) |
| Buildings, bridges (3D Tiles / glTF) | Cesium feature tilesets |
| Vegetation density rasters, mapped instances | runtime scatter (R4) |
| Thermal classes for new codes | config (`thermal.land_cover.classes`), no shader change |

**Once per CamSim release** (a normal UBT build and `package_for_docker.sh`, the same as any code
change; not per area):

- the C++ and shader code each phase adds (scene loader, feature tilesets, scatter, any new
  `ThermalCS` input such as the NDVI window or the outer land-cover level);
- a small generic asset library cooked into the image: facade/roof materials (R3) and, if they are
  cooked, tree/shrub meshes (R4). New asset directories go in `DirectoriesToAlwaysCook`.

**Design rules that keep it that way:**

- Generated content references library assets **by name** (material `facade_brick`, tree type
  `oak_medium`), never by UE asset path baked into the area data.
- R4 should prefer plain HISM driven from C++ over a PCG graph. A PCG graph is an editor asset; it
  runs in a cooked build, but changing its rules means an editor pass.
- Tree meshes could also load at runtime through glTFRuntime (already used for entities), making the
  vegetation library a mounted folder like the scene package. The trade-off: Nanite data is built
  in the editor, so runtime-loaded trees use glTF LODs and no Nanite. R4's spec decides with a
  bench.

### Why runtime loading rather than building areas into the project

The alternative is to bake each area into UE content at editor/build time: World Partition levels,
Landscape terrain, imported Nanite meshes, a PCG pass, then cook. It has real advantages: Nanite on
buildings and trees, HLODs, editor-quality materials, fast loading from a pak. It is still the wrong
default for CamSim:

- **The globe.** UE Landscape and World Partition are flat. Over a large area the Earth curves away
  from a flat plane by d²/2R: about 8 cm at 1 km, 7.8 m at 10 km and 785 m at 100 km. CamSim's WGS-84
  correctness (CIGI positions, HAT/HOT, LOS, KLV frame centre, DIS) rests on Cesium's globe. Baked
  flat levels would break it beyond a few km from their origin.
- **Scale.** Cooking a 40,000 km² World Partition world is hours of build time and a very large pak,
  per area, on a machine with the editor.
- **Turnaround and deployment.** Every new area or data update becomes a cook and a new image (or a
  pak chunk) instead of mounting a folder. Areas get tied to the engine version they were cooked
  with.
- **Fit with what exists.** Terrain, imagery, land cover and entities already load at runtime.

**When build time is right:** generic assets that every area shares (materials, Nanite trees) are
built once, as above. If the R3/R4 benches show runtime glTF buildings or trees are too slow, the
fallback is an **automated** cook: a headless `UnrealEditor-Cmd` commandlet that imports one area's
meshes with Nanite and cooks them into a pak the image mounts. No human opens the editor, but the
build machine needs the editor installed. That is an option to measure, not the plan.

---

## 4. Phases

Each phase lists what it builds, its exit gates, and editor work a human has to do (to be copied into
`ROADMAP.md` when the phase starts, per `CLAUDE.md`).

### R0 Scene package and tooling (offline only)

Spike done (`docs/realism-r0-spike.md`, reference code `scripts/scene/spike/`); its "Pitfalls" table
is a checklist for this phase.

- `scripts/scene/` uv project: `camsim-scene build | fetch | verify | attribution`. Python 3.12–3.13
  (the rasterio/pyproj/pydelatin wheel overlap; rasterio's arm64 wheel needs macOS 15+). No system
  GDAL or PDAL.
- Our own quantized-mesh writer (header, vertices, high-water-mark indices, edge lists, oct normals,
  `layer.json` with `available`), not `quantized-mesh-encoder`: it casts to float32 and truncates,
  and doesn't reorder for high-water marks. Every build ends with a decode-and-compare verify
  against the source (the check that found the spike's mirrored tiles).
- Snapshots: sources that drift or vanish (Overture keeps releases 60 days, OSM extracts change
  daily) are stored in the fetch cache with a hash; the manifest points at the snapshot.
- The global base layer is part of every package (principle 8), merged into the package's single
  terrain pyramid and its imagery pyramid: ETOPO 2022 30″ (with bathymetry; its geoid grid converts
  to ellipsoid heights) and NASA Blue Marble NG 500 m to z8 globally, a z9–10 ring from the ESA
  WorldCover 2021 Sentinel-2 composite (CC BY 4.0, to confirm), which also fills where NAIP stops
  offshore. All public domain or CC BY.
- Packaging: the package directory → one SquashFS/EROFS image (section 3, "Large areas"). Prototype
  it and time a cold start against a plain directory.
- ~~Offline config profile: no ion source anywhere; with a package and `offline: true`, fail fast
  instead of falling back to ion.~~ **Done 2026-10-09**: `scene.dir` (package layers → terrain, TMS imagery,
  land cover) and `scene.offline` (local `file:///` sources only, exit 1 on any config error), with Cesium set
  up at world init so `Main.umap`'s ion actors never load (`docs/configuration.md`, "Scene Package").
- Manifest schema (versioned), on-disk fetch cache under `.cache/scene/`, licence register with an
  allow-list (`redistributable`, `attribution`, `non-commercial`, `proprietary`). Building a package
  with a source outside the allow-list fails.
- Move `fetch_worldcover.py` in as the first layer. Its output format stays the same, so
  `Content/NonUFS/LandCover` is unchanged.
- Large-area build engine (section 3, "Large areas"): global tile scheme, per-tile jobs in
  parallel, resumable, per-layer rebuild, and per-tile profile assignment from a bbox plus optional
  corridors.
- **Gates:** an egress-blocked run (`docker run --network none`, unicast to loopback, CIGI host in
  the container) renders the package and logs no outbound attempt. The Camp Pendleton package
  rebuilds byte-identical from the manifest. `verify` catches a tampered
  tile (hash) and a missing attribution. A killed build resumes without redoing finished tiles. A
  ~1,000 km² `preview` build reports wall time, peak memory and size per layer, which replace the
  estimates in section 3. pytest suite under `scripts/scene/tests`.

### R1 Terrain and imagery

- Bare-earth terrain: the 3DEP 1 m DEM where it exists, otherwise 1/3 arc-second, converted to
  WGS-84 ellipsoid heights (principle 7), over the package's global base. TIN by `pydelatin`,
  encoded by R0's writer, loaded through the existing `terrain.source: url` with a `file:///` URL
  (works today, no code change; three slashes, absolute path). The "one terrain tileset" rule
  stays. Generate full low-zoom coverage rather than relying on Cesium's upsampling (done in R0: whole-globe z0-8 and
  complete siblings) (the spike's
  remaining Chaos NaN-bounds ensure came from an upsampled tile). The ocean's sea level (EGM96)
  must still meet the coastline: NAVD88 zero is 0.38 m off EGM96 at Pendleton, and NOAA's
  shoreline is probably MHW; check at the Pendleton coast.
- Imagery: NAIP as local TMS tiles from the package (public domain, 0.6 m, some states 0.3 m;
  flown every 2–3 years per state, leaf-on, near midday). CA 2024 is only on the USDA image
  service or EarthExplorer (Planetary Computer stops at 2022, AWS is requester-pays). New config:
  `imagery.source: tms` + `imagery.url` (landed with R0's offline profile, 2026-10-09); the
  URL names `tilemapresource.xml`. Pyramid down to zoom 0. Record the acquisition date and sun angle
  per tile in the manifest: imagery shadows are baked in and only look right near that sun position
  (a known limit, documented, not fixed). NAIP edge: margin + Sentinel-2 colour match + feather (`docs/superpowers/specs/2026-10-09-naip-edge-design.md`;
  NAIP quarter-quads are bit-identical in their overlaps, so no per-file balancing; open water is one source: NAIP clipped to land plus 200 m, the colour match land-only).
- NAIP also has a near-infrared band. Package it as an NDVI layer next to the RGB tiles: it is a far
  better vegetation signal than 4B's GBuffer base-colour index (R2 decides whether `ThermalCS` uses
  it).
- ~~Offline: `Main.umap`'s own ion actors request ion endpoints at frame 0.~~ Solved without the editor
  (2026-10-09): CamSim configures or destroys them at world init, before they load. Stripping them from
  `Main.umap` is optional cleanup.
- Docker/Linux: confirm `file://` from a mounted volume (the spike ran on macOS/Metal).
- **Gates:** decoded vertices vs source DEM (spike: median 2–3 cm, p99 0.31 m). CIGI frame-centre
  and HAT/HOT heights vs 3DEP truth at surveyed points (spike: ≤ 0.22 m on 1 m data, ≤ 0.9 m on
  10 m). Geo-registration vs CWT + Bing < 1 px at 0.84 m GSD. A run with outbound network blocked
  makes no request and passes. Terrain readiness gate (0.7) still holds. Bench: no game-thread
  regression beyond noise from the deeper local pyramid.

### R2 Vector features into the land-cover window (no new geometry)

The cheapest big realism win, because it improves thermal IR and ground truth without drawing
anything new. Roads are one of the most visible features in LWIR and are 4B's main gap. This lifts
4B's "roads layer is a non-goal".

- Rasterise OSM roads (buffered by lane count / road class), paved areas and rail, and 3DHP water
  bodies and flowlines (public domain; NHD retired in 2023, so static NHD HR is the fallback and
  OSM water a gap filler) into the
  package's land-cover tiles as new codes above WorldCover's.
- Evaluate NAIP NDVI (R1) in place of the GBuffer vegetation index in `ThermalCS`. It needs its own
  camera-centred window at imagery resolution, so it is a measured trade (VRAM and ThermalCS time
  vs gate (i)/(j) contrast), not a given.
- New thermal classes: `road_asphalt`, `road_concrete`, `rail_ballast`, `inland_water` (and
  `roof_*` if R3 needs them), from the free class slots.
- Fill CIGI HAT/HOT and LOS extended-response material codes from the class under the hit (closes
  the "material code is 0" note in ROADMAP 1.9). Document the code table in `docs/terrain-feedback.md`.
- **Gates:** road centrelines vs imagery roads: median offset ≤ 1 window texel (10 m) on a sample
  set (if roads need to be sharper than that, a finer road raster is a follow-up). `thermal_check.py` gains a road-vs-verge contrast gate (noon
  LWIR/MWIR, sign and magnitude from published data, 4D). `land_cover.enabled: false` is still bit
  for bit. ThermalCS cost is unchanged (same lookup).

### R3 Buildings and bridges (generated 3D Tiles)

- Footprints: Overture (OSM + Microsoft footprints merged, ODbL; snapshot it, releases are deleted
  after 60 days), or Microsoft's Global ML footprints directly (CDLA Permissive 2.0, some with
  heights). Heights: in order of preference, 3DEP LiDAR nDSM, Overture `height`/`num_floors`, then
  a land-cover/density prior. Inside Camp Pendleton there is no LiDAR, so Overture/OSM heights are
  primary there (73 % of the bbox's 81,825 buildings have one). Roof
  shape: flat by default, gabled for small residential footprints.
- Geometry: extruded footprints → glTF → 3D Tiles with per-feature metadata (class, height source,
  source id). Bridges: OSM `bridge=yes` ways with the deck at the road's elevation (fixes the
  Golden Gate slab, ROADMAP 2.6).
- Runtime: relax "one terrain tileset" to **one terrain tileset + N feature tilesets from the scene
  package**. `ApplyCesiumBackendConfig` still destroys any other tileset (the `Main.umap` OSM
  Buildings lesson stands): spawn package tilesets after the destroy loop, then apply tuning,
  mobility and `RefreshCachedTilesets()` to them, and decide whether the terrain gate waits on
  them (`docs/realism/cesium-findings.md` §3). Feature tilesets get physics meshes (HAT/HOT, LOS,
  DIS surface clamp); C++ reads per-feature metadata from a complex trace hit
  (`GetPropertyTableValuesFromHit`).
- Ground truth: buildings get a semantic class (4D) but no instance stencils (those stay for
  entities). Decide in the spec whether occlusion by buildings counts toward entity visibility (it
  should; the instance mask handles it already, since buildings are opaque scene geometry).
- **Gates:** footprints vs imagery (IoU on a hand-checked sample). Heights vs LiDAR nDSM (median
  error). LOS through a building reports a hit. A DIS truck on a bridge clamps to the deck. Bench:
  game-thread and VRAM cost at 1, 2 and 4 instances on the 5080, within a budget agreed before
  implementing.
- **Editor work (human):** a small facade/roof material set (concrete, brick, glass, metal roof)
  with runtime-assignable parameters, cooked into the project. glTF materials from the generator
  reference these by name. Getting a per-feature class into a material (for `ThermalCS`) goes
  through `UCesiumFeaturesMetadataComponent`, whose "Generate Material" is editor-only: one-time
  human work on the building material, not per area.

### R4 Vegetation and clutter

- Placement inputs, offline: density and type rasters per tile from land cover + canopy height
  (a 3DEP LiDAR canopy height model where LiDAR exists, otherwise Meta/WRI CHMv2, CC BY 4.0) +
  USFS Tree Canopy Cover v2025-6 % + Annual NLCD fractional impervious + NAIP NDVI. Explicit instances only for mapped
  features (OSM `natural=tree`/rows, poles).
- Placement at runtime: a large area holds ~10⁹ trees, too many to list, so instances are scattered
  around the camera from the density rasters with a deterministic hash (seed = hash(package seed,
  cell id), the same PCG-hash idea as the sensor noise), never a live RNG. The same cell always gets
  the same trees, whichever direction the camera arrives from.
- Rendering: hierarchical instanced static meshes, generated and dropped by distance around the
  camera, globe-anchored (survives origin shifts), faded out where they would be sub-pixel. Evaluate UE PCG's runtime
  generation mode against plain HISM. The default is HISM unless PCG wins on cost.
- Utility poles and lines (OSM `power=*`; HIFLD Open went offline 2025-08-26), fences, street furniture: same mechanism, lower priority.
- Thermal and ground truth: instances carry the class of their asset (`tree_canopy`, `shrubland`);
  the sun-shadow term already comes from the GBuffer. Trees occluding entities show in the instance
  mask, as buildings do.
- **Gates:** tree-cover fraction per 100 m cell vs canopy data (error within a set tolerance).
  Deterministic: the same cell gives the same instance hash from two different camera paths.
  Long-range: a 5 km altitude oblique pass stays within the frame budget. Bench budget as R3. 4C/2.7 gates still
  pass with vegetation on (a truck under a tree: visibility < 1, box correct).
- **Editor work (human):** a tree/shrub/grass asset set with Nanite or LODs and an impostor for
  distance, cooked into the project. The repo is open source, so assets must be redistributable
  (Megascans/Fab licences generally aren't). Either ship a permissive set or make the asset pack a
  user-supplied mount with a documented manifest.

### R5 Mission-aware generation (and cache warming, online mode only)

- Input: a CIGI recording (`scripts/capture_cigi_stream.py`), a waypoint file, or a corridor.
  Output: the footprint corridor (camera FOV over terrain over time, plus a margin) fed to R0's
  per-tile profiles: `high` inside the corridor, `sim` near it, `preview`/stream-only beyond. Several
  missions can share one package (corridors are unioned).
- Offline runs need no warming: the package is local and covers the globe (principle 8). And
  warming can't be the offline answer anyway: Cesium ion's terms (§2.2.2, 2025-08) forbid storing
  Cesium World Terrain or Bing for offline use, and Bing can't be licensed offline at all. Offline
  CWT would need Cesium ion Self-Hosted plus a data licence (a sales conversation). For online runs
  without a package, warming needs `MaxCacheItems` raised (4,096 items today). Low priority.
- **Gates:** a replayed mission over a corridor-tiered package runs with networking blocked and
  identical output to the same package fully at `high` inside the corridor. Package size for a
  corridor vs the full bbox (report it; no target yet).

### R6 Realism validation

- **Geo-registration:** features rendered at known coordinates (road junctions, building corners)
  vs KLV frame centre/corner coordinates. Error within the sensor's ground sample distance.
- **Determinism:** same package + seed + CIGI recording → identical frame hashes on the same
  GPU/driver (pixel tolerance across GPUs).
- **Appearance:** compare rendered EO/IR against real UAV footage of the same place and time (where
  available) on image statistics (spectrum, edge density, local contrast). A distribution metric
  (e.g. KID on a feature extractor) as a trend, not a gate.
- **Sim-to-real (the metric that matters):** train a detector on CamSim data from profiles
  `preview`, `sim` and `high`, test on a fixed real dataset. Each realism phase should move this
  number. If it doesn't, the phase's effort goes elsewhere.

---

## 5. Not covered here

Photogrammetry (Google Photorealistic 3D Tiles, user captures) is out of scope (2026-10-07): baked
lighting is wrong at any other time of day and has no thermal meaning, and third-party terms restrict
caching and possibly ML use. Revisit only for EO-only work, with a licence review first.

Other realism axes, each better as its own `ROADMAP.md` item when it comes up: atmospheric radiative
transfer (path radiance and transmission beyond 4A's terms), wet/snow-covered surfaces from CIGI
weather, seasonal land cover and leaf-off, BRDF/specular for EO, rolling shutter and motion blur
(removed in the 2026-10 trim), and dynamic traffic / pattern of life (a host's job, or the Mass/ECS
item under "Later").

## 6. Open questions

1. Should feature tilesets be served by Cesium (3D Tiles, LOD and streaming for free, but tile-count
   cost on the game thread) or spawned as glTFRuntime actors per area (more control, and we own the
   streaming)? R3's spec should measure both on the SF sample.
2. ~~Can Cesium for Unreal 2.29 load quantized-mesh terrain and TMS imagery over `file://`?~~ Yes
   (R0 spike).
3. Which real UAV EO/IR dataset do we use as the R6 sim-to-real yardstick, and is its licence
   compatible? It should be over the US, so a scene package can match it.
4. What is the real size of the operating areas (the 200 km × 200 km design target is an
   assumption)? And are missions flown inside one area, or do some transit between areas?
5. ~~Which first test area?~~ Camp Pendleton (2026-10-07). Its interior has only 10 m terrain:
   add a second area with full QL1 LiDAR to exercise `high` (1 m terrain, LiDAR heights), or test
   `high` on Pendleton's fringes only?
6. Offline Cesium World Terrain: is open data (3DEP + ETOPO) enough, or is a Cesium ion
   Self-Hosted + CWT licence worth asking about?
