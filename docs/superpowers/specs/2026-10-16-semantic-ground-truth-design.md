# ROADMAP 4D (part): per-pixel semantic classes in ground truth (design)

Date: 2026-10-16. Status: the user chose this as the next item and delegated the design ("Yes please" to spec → plan →
build → acceptance as for 2.4). The choices below are the recommended ones. The rest of 4D (validation against
published data, thermal shadow lag) stays open.

## 1. Goal

Every annotated frame can carry a **semantic class image**: one class per output pixel, registered with the image,
the instance masks and the depth map. Datasets then have dense labels (segmentation, scene understanding), not only
boxes and masks for the entities CamSim placed.

Success:

- A control API capture returns the class image (8-bit PNG) next to image, depth and annotations; `make_dataset.py`
  writes it; `ml_training.semantic` writes it per annotated frame like depth.
- Entity pixels carry their type's class exactly (same pixels as the visible instance mask); sky and water are
  classified geometrically; terrain pixels carry the land-cover class of the cell under them.
- The GPU pass matches a CPU reference (`CamSim.GPU.GroundTruth.Semantic.MatchesCpu`), and EO and IR captures of a view
  give the same class image.
- GPU cost ≤ 0.3 ms per annotated frame at 720p.

## 2. Decisions

| # | Decision | Why |
|---|---|---|
| D1 | A **`WRITE_SEMANTIC` permutation of `InstanceIdCS`**, not a new pass. | The class must sit on exactly the source texel the instance ID and depth use (`SourceTexel`, distortion via `UndistortScale`): one pass keeps them registered by construction, and it already reads scene depth, custom depth and stencil. |
| D2 | **Classes from what exists:** sky (scene depth at far), entity (visible stencil → class table), water (land-cover water codes, or the sea band), else the land-cover code of the cell under the pixel's world position. | Trees, buildings and bridges have no per-pixel tag, and giving them custom depth would make them occlude entities in the custom-depth buffer (breaking visible/amodal masks and ThermalCS's entity test). The land-cover window (10 m, with R2 roads/rail/water and R3b building footprints burned in) is the one per-pixel source for terrain-like classes. |
| D3 | **Nearest land-cover texel, no warp.** | ThermalCS warps the lookup to hide square cells in the radiance; labels must follow the data, not an art effect. |
| D4 | The **land-cover window is built in EO too** when semantic output is possible (`ml_training.semantic`, or the control API enabled), and its parameters travel with the frame-grab request, not only inside `FThermalFrameParams`. | Today it is built only while thermal IR runs. A window re-centre is ~0.5 ms once per 5 km of travel. |
| D5 | **Entity classes are configured per type**: `entity_types.<id>.semantic_class` ∈ {`vehicle`, `vessel`, `aircraft`, `person`, `marker`, `other`}; default from `entity_category` (`character` → person, else `other`). The canonical config sets 1001 aircraft, 2001 vehicle, 3001 vessel, 9001/9002 marker. A 256-entry stencil → class table is built from the frame's entity snapshot on the game thread. | Exact, explicit, no guessing from DIS domains (API entities carry no classification). |
| D6 | **Stable class ids** (below), documented in `docs/ground-truth.md`, written into `annotations.json` (`camsim_semantic_classes`). | Datasets from different runs must mean the same thing. |
| D7 | **Files off by default** (`ml_training.semantic: false`); the control API returns it by default (`capture {"semantic": true}`). | Existing `ml_training` users see no new files or cost. |
| D8 | One **uint8 class per pixel**, two pixels per uint32 word in the same layout as the instance IDs (low 8 bits of each 16-bit half). | Reuses the pair-per-thread dispatch and `FInstanceIdImage`-style unpacking. |

### Class ids

| Id | Name | From |
|---|---|---|
| 0 | `unlabeled` | no land-cover data (outside the window, land cover off, code 0) and not sky/water/entity |
| 1 | `sky` | scene depth at the far plane |
| 2 | `water` | land-cover 80 or 220, or inside the sea band (below) |
| 3 | `road` | 200, 201 |
| 4 | `rail` | 210 |
| 5 | `building` | 230 |
| 6 | `built_up` | 50 (WorldCover built-up, other than burned roads/buildings) |
| 7 | `tree` | 10 |
| 8 | `low_vegetation` | 20, 30, 40, 90, 95, 100 |
| 9 | `bare` | 60 |
| 10 | `snow_ice` | 70 |
| 20 | `vehicle` | entity, `semantic_class: vehicle` |
| 21 | `vessel` | entity |
| 22 | `aircraft` | entity |
| 23 | `person` | entity |
| 24 | `marker` | entity |
| 25 | `other_entity` | entity, `other` |

Any other land-cover code (a user override, a future code) → `unlabeled`. The code → class LUT is a 256-entry table
built on the CPU (`CamSimSemantic::BuildCodeLut`) and uploaded with the pass, so the shader has no code list.

### Sea band

Water beyond the land-cover window (or where the window says 0 offshore) is found geometrically, exactly as ThermalCS
does: with the camera's height above sea level `CamHeightCm` (EGM96 + package offset + tide, `FOceanSurface`), the
Gaussian Earth radius `SeaRadiusCm` and `WaterBandCm` = `WaterBandBaseM` + the maximum wave amplitude, a pixel is in the
sea band when `CamHeightCm + Vert + D²/(2·SeaRadiusCm) < WaterBandCm` (`Vert`, `D` from the pixel's camera-relative world
position). The three values come from one helper shared with `FThermalFrameBuilder` (`CamSimThermal::ComputeSeaBand`),
so IR and labels agree. A sea-band pixel is `water` when its land-cover code is 0 or 80 (a pier or beach cell with
another code keeps that code's class). Without an ocean (`ocean.enabled: false`) there is no sea band.

## 3. Data flow

```
Game thread, annotated frame (or capture job):
  entity snapshot ─► stencil→class table (256 B)
  land-cover window (EO or IR) ─► FLandCoverLabelParams {East, North, CamOffsetM, TexelM, Texels, WindowId} + GPU window
  ocean ─► sea params {CamHeightAboveSeaM, EarthRadiusM, BandM}
  └─► FFrameGrabRequest {bSemantic, StencilClass[256], LandCover, Sea}
Render thread: AddGroundTruthReadback_RenderThread ─► InstanceIdCS (WRITE_SEMANTIC) ─► OutSemantic (W*H/2 uint)
  ─► third AddReadbackBufferPass (W*H*2 bytes)
Game thread Poll ─► FSlot.Semantic ─► task thread: collector WriteSemanticFrame (PNG file, own task, ≤ in-flight cap)
  and the capture job's SemanticPng
```

`ClipToTranslatedWorld` is always set when semantic output runs (today only with water planes).

## 4. Units

| Unit | Kind | Purpose |
|---|---|---|
| `GroundTruth/SemanticClasses.h/.cpp` | pure | Class ids and names, `BuildCodeLut() -> TStaticArray<uint8, 256>`, `ParseSemanticClass(name)`, `ClassForEntity(const FEntityTypeEntry*)`, `BuildStencilClassTable(entities)`. |
| `GroundTruth/SemanticReference.h/.cpp` | pure | CPU reference `CamSimSemanticRef::ClassifyPixel(...)` mirroring the shader (sky, entity, land cover nearest texel, sea band) — the GPU test holds the shader to it. |
| `Shaders/Private/CamSimInstanceId.usf` | shader | `WRITE_SEMANTIC` permutation: `OutSemantic`, `StencilClass` buffer, land-cover window + LUT, sea params. |
| `CamSimShaders/InstanceIdPass` | changed | Inputs for the semantic output; returns `FRDGBufferRef* OutSemantic`. |
| `Thermal/LandCoverWindow` + `UCamSimCaptureComponent` | changed | Window updated in EO when semantic is possible; params + window carried by the request. |
| `FrameGrab*` / capture component | changed | Third readback (pool, generation, slot array, poll, failure resets). |
| `FGroundTruthCollector` | changed | `WriteSemanticFrame(...)` (`<out>/semantic/semantic_%08llu.png`, 8-bit grey) and `EncodeSemanticPng` reused by the capture job. |
| `Entity/EntityTypeTable` | changed | `semantic_class` key. |
| Config | changed | `ml_training.semantic` (`CAMSIM_ML_SEMANTIC_ENABLED`). |
| Control API | changed | `capture {"semantic": true}` → `semantic_png_b64`; status `ground_truth.semantic`. |
| Python | changed | `Capture.semantic_png`, `DatasetWriter` writes `semantic/NAME.png` + `camsim_semantic_classes`. |

## 5. Acceptance: `scripts/semantic_check.py --scene PKG --out DIR` (control API, Pendleton)

| Gate | Check |
|---|---|
| S1 entity pixels | A truck (vehicle) and a boat (vessel) in view: ≥ 99 % of each visible-mask pixel has the entity's class, and no entity-class pixel lies outside the union of visible masks. |
| S2 sky | A 10° depression view to the horizon: sky pixels exist, and every sky pixel is a depth-far pixel (depth PNG 65535). |
| S3 terrain registration | Nadir 300 m views: the georeg flat site (paved) → `road` ≥ 90 % of the centre 20 %; the georeg roof site → `building` at the centre; a sea point 2 km offshore → `water` ≥ 99 %; a WorldCover tree cell area → `tree` ≥ 80 % of the centre. |
| S4 EO = IR | Same view in EO and IR: identical class images. |
| S5 dataset | `make_dataset.py` writes `semantic/` for every image, ids within the table, `camsim_semantic_classes` present. |
| S6 cost | Frame GPU time with semantic on vs off over 300 annotated frames: Δ p50 ≤ 0.3 ms (informational if the bench noise exceeds it). |

C++ tests: class LUT and names, entity class parsing/defaults, stencil table, the CPU reference cases (sky, entity
visible/hidden, each code, sea band in/out, outside window), `CamSim.GPU.GroundTruth.Semantic.MatchesCpu` (synthetic
depth/stencil/land-cover window, distortion on), PNG round trip, config key.

## 6. Limits (documented)

- Terrain classes are at the land-cover resolution (10 m): a 6 m road burned into 10 m cells labels its cells, not
  its paint edges. Tree crowns and building walls in oblique views take the class of the cell under them.
- Vegetation instances and the buildings/bridges tilesets are not tagged individually (no instance ids for them).
- Labels are nearest-texel at the source resolution; under TSR, like the instance masks, up to ~0.5 render texel off.
