# Semantic Ground Truth (ROADMAP 4D part) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A per-pixel semantic class image in ground truth (sky, water, land-cover classes, entity classes), registered
with the instance masks and depth, returned by control API captures and written by `ml_training.semantic`.

**Architecture:** A `WRITE_SEMANTIC` permutation of `InstanceIdCS` classifies each output pixel's source texel: sky
from scene depth, entities from the visible stencil through a 256-entry class table, water from land-cover water codes
or ThermalCS's sea band, terrain from the nearest land-cover texel under the pixel's world position through a 256-entry
code LUT. The game thread builds `FSemanticFrameParams` (land-cover window geometry, sea band, LUTs) per annotated
frame; the land-cover window is kept up to date in EO too when semantic output is possible. A third readback carries
the classes to the task thread (file writer, capture job).

**Tech Stack:** UE 5.8 C++ / HLSL (SM5 compute, Vulkan + Metal), RDG; UE Automation (NullRHI + `CamSim.GPU.*` via
`scripts/run_gpu_tests.sh`); Python 3.11 client.

**Spec:** `docs/superpowers/specs/2026-10-16-semantic-ground-truth-design.md`

## Global Constraints

- Class ids exactly as the spec's table (0 unlabeled, 1 sky, 2 water, 3 road, 4 rail, 5 building, 6 built_up, 7 tree,
  8 low_vegetation, 9 bare, 10 snow_ice, 20 vehicle, 21 vessel, 22 aircraft, 23 person, 24 marker, 25 other_entity).
- Shader integer ops / bit-tested non-finite guards only (CLAUDE.md: no float atomics, no wave intrinsics; NaN checks
  bit-test a loaded value); the CPU reference mirrors the shader expression for expression.
- `ml_training.semantic` default false (`CAMSIM_ML_SEMANTIC_ENABLED`); control API capture `semantic` default true.
- Nearest land-cover texel, no warp. Sea band = ThermalCS's (`WaterBandBaseM` 0.5 m + max wave amplitude, Gaussian radius).
- Layout: two pixels per uint32, class in the low 8 bits of each 16-bit half (pixel 2k in the low half).
- GPU cost ≤ 0.3 ms per annotated frame at 720p (S6).

## Review Focus

1. **No land-cover window yet** (EO start, first frames, window still building, land cover off, no georeference):
   terrain pixels must be `unlabeled`, never a stale or garbage class — CPU ref test `NoWindowUnlabeled`, GPU test case.
2. **Entity hidden behind terrain** (amodal only): its pixels must take the occluder's class, not the entity's — CPU ref
   `HiddenEntityTakesOccluder`.
3. **A stencil reused within the 4-frame delay / stencil with no entity in the snapshot**: class table entry 0 → the
   pixel falls through to terrain classification, not `other_entity` — `BuildStencilClassTable` test.
4. **Window re-centre between capture and render** (window id in params ≠ bound GPU window id): bind nothing and
   label terrain `unlabeled` for that frame rather than mislabel — `ShouldBindWindow` reused; test in Task 4.
5. **Sea band at a cliff/pier** (land-cover code ≠ 0/80 inside the band) keeps the land class — CPU ref `SeaBandKeepsLandCode`.

---

### Task 1: Class ids, code LUT, entity `semantic_class`

**Files:** Create `GroundTruth/SemanticClasses.h/.cpp`; Modify `Entity/EntityTypeTable.h/.cpp` (`ESemanticEntityClass SemanticClass`, key `semantic_class`), `deploy/camsim_config.yaml` (1001 aircraft, 2001 vehicle, 3001 vessel, 9001/9002 marker); Test `Tests/SemanticClassesTest.cpp`.

**Produces:**

```cpp
namespace CamSimSemantic
{
	enum EClass : uint8 { Unlabeled = 0, Sky = 1, Water = 2, Road = 3, Rail = 4, Building = 5, BuiltUp = 6, Tree = 7,
		LowVegetation = 8, Bare = 9, SnowIce = 10, Vehicle = 20, Vessel = 21, Aircraft = 22, Person = 23, Marker = 24,
		OtherEntity = 25 };
	CAMSIMTEST_API const TCHAR* Name(uint8 Class);                 // "road"; "" for an unused id
	CAMSIMTEST_API TArray<uint8> AllClasses();                     // the 17 ids in order
	CAMSIMTEST_API TStaticArray<uint8, 256> BuildCodeLut();        // land-cover code -> class
	CAMSIMTEST_API bool ParseEntityClass(const FString& Name, uint8& OutClass);   // vehicle..other -> 20..25
	CAMSIMTEST_API uint8 DefaultEntityClass(const FString& EntityCategory);       // "character" -> Person, else OtherEntity
	/** Entity snapshot -> per-stencil class (0 = no entity: falls through to terrain). */
	CAMSIMTEST_API TStaticArray<uint8, 256> BuildStencilClassTable(TConstArrayView<FEntityAnnotationData> Entities,
		TFunctionRef<uint8(uint16 EntityType)> ClassOfType);
}
```

`FEntityTypeEntry::SemanticClass` (uint8, 0 = unset → `DefaultEntityClass(EntityCategory)` at lookup:
`FEntityTypeTable::SemanticClassOf(uint16 TypeId)`).

- [ ] Tests (`CamSim.GroundTruth.Semantic.Classes.*`): every code of the spec table maps right, 0 and unknown codes →
  0; names round-trip; `ParseEntityClass` accepts the six names, rejects others; config `semantic_class: vessel`
  parsed, unset → default by category, an unknown name is a load warning and falls back; `BuildStencilClassTable`:
  stencils 3 and 7 from two entities, stencil 0 ignored, an unset stencil 0 in the table (Review Focus 3).
- [ ] RED (compile) → implement → GREEN; `CamSim.Config` + `CamSim.Entity` still pass. Commit `feat(gt): semantic class ids, land-cover LUT, entity semantic_class (4D)`.

### Task 2: Frame params, builder and CPU reference

**Files:** Create `Source/CamSimShaders/Public/SemanticFrameParams.h`; `GroundTruth/SemanticFrame.h/.cpp` (builder);
`GroundTruth/SemanticReference.h/.cpp`; Test `Tests/SemanticReferenceTest.cpp`.

**Produces:**

```cpp
// CamSimShaders/Public/SemanticFrameParams.h
struct FSemanticFrameParams
{
	uint32    bLandCover = 0;        // a window of LandCoverTexels^2 is bound for WindowId
	uint32    LandCoverWindowId = 0;
	FVector3f LandCoverEast = FVector3f(1, 0, 0), LandCoverNorth = FVector3f(0, 1, 0);   // unit, world
	FVector2f LandCoverCamOffsetM = FVector2f::ZeroVector;   // camera (E, N) from the window centre
	float     LandCoverTexelM = 10.0f;
	uint32    LandCoverTexels = 0;
	uint32    bSea = 0;
	FVector3f Up = FVector3f(0, 0, 1);
	float     CamHeightCm = 0.0f, SeaRadiusCm = 6.371e8f, WaterBandCm = 50.0f;
	uint8     CodeClass[256] = {};   // land-cover code -> class
	uint8     StencilClass[256] = {};// stencil -> entity class (0: none)
};

// GroundTruth/SemanticFrame.h
struct FSemanticBuildInputs
{
	const FLandCoverWindowData* Window = nullptr;   // null: no land cover
	FVector EastWorld, NorthWorld, UpWorld;          // from CamSimThermal::LandCoverAxesWorld + the camera's up
	double CamLatDeg = 0, CamLonDeg = 0, CamAltHaeM = 0;
	TOptional<double> SeaLevelHaeM; double MaxWaveAmplitudeM = 0;
	TStaticArray<uint8, 256> StencilClass;
};
namespace CamSimSemantic { CAMSIMTEST_API FSemanticFrameParams BuildFrameParams(const FSemanticBuildInputs& In); }

// GroundTruth/SemanticReference.h
namespace CamSimSemanticRef
{
	/** Mirrors ClassifyTexel in CamSimInstanceId.usf. Pw: translated-world (camera-relative) cm of the scene surface. */
	CAMSIMTEST_API uint8 ClassifyTexel(const FSemanticFrameParams& P, float SceneZ, float CustomZ, uint32 Stencil,
		const FVector3f& Pw, TFunctionRef<uint8(int32 X, int32 Y)> LandCoverCode);
	CAMSIMTEST_API bool NearestLandCoverTexel(const FSemanticFrameParams& P, const FVector3f& Pw, int32& OutX, int32& OutY);
}
```

Classification order (shader and reference): `SceneZ <= 0` → Sky; visible entity (`Stencil > 0 && CustomZ >=
SceneZ*(1-1e-4) && StencilClass[Stencil] != 0`) → that class; land-cover code `c` at the nearest texel (0 without a
window or outside it); sea band (`bSea && CamHeightCm + dot(Pw,Up) + (|Pw|²-dot²)/(2 SeaRadiusCm) < WaterBandCm`) and
`c ∈ {0, 80}` → Water; else `CodeClass[c]`. Nearest texel: `X = E/TexelM + Half - 0.5`, `Y = Half - N/TexelM - 0.5`,
`round = floor(x + 0.5)`, inside `[0, Texels-1]` else none (bit-test NaN first).

- [ ] Tests (`CamSim.GroundTruth.Semantic.Ref.*`): `Sky`, `VisibleEntity`, `HiddenEntityTakesOccluder`,
  `EntityStencilWithoutClassFallsThrough`, `EachCode` (window with known codes at known E/N), `NearestTexelBoundaries`
  (±0.5 texel), `OutsideWindowUnlabeled`, `NoWindowUnlabeled`, `SeaBandWater`, `SeaBandKeepsLandCode`, `AboveBandNotWater`;
  builder: offsets equal `GeodeticToWindowEN`, sea values equal `FThermalFrameBuilder`'s for the same inputs, no window
  → `bLandCover` 0.
- [ ] RED → implement → GREEN. Commit `feat(gt): semantic frame params, builder, CPU reference (4D)`.

### Task 3: Shader permutation + pass + GPU test

**Files:** `Shaders/Private/CamSimInstanceId.usf` (`WRITE_SEMANTIC`), `Source/CamSimShaders/Public/InstanceIdPass.h` +
`Private/InstanceIdPass.cpp`; Test `Tests/InstanceIdGpuTest.cpp` (`CamSim.GPU.GroundTruth.Semantic.MatchesCpu`).

**Produces:** `FInstanceIdInputs` gains `const FSemanticFrameParams* Semantic = nullptr; FRDGTextureRef LandCover = nullptr;`
(`Texture2D<uint>` R8_UINT; null → a 1×1 zero dummy, `bLandCover` forced 0); `AddInstanceIdPass(..., FRDGBufferRef* OutDepth, FRDGBufferRef* OutSemantic = nullptr)`
(W*H/2 uint32, pair layout). Shader: `CodeClassPacked[16]` / `StencilClassPacked[16]` `uint4` (as ThermalCS's
`LandCoverClassPacked`), the land-cover and sea params, `RWStructuredBuffer<uint> OutSemantic`; `ClassifyTexel` mirrors the
reference; ClipToTranslatedWorld always used (texel centre → NDC, scene device Z). Permutation domain grows to view rect
× depth × semantic (8); `IsInstanceIdPassSupported` checks all.

- [ ] GPU test: 64×32 synthetic scene depth (sky band, terrain), custom depth/stencil (one visible entity, one hidden),
  an identity-ish ClipToTranslatedWorld giving known Pw, a 16×16 land-cover texture with every code, sea band on, and
  distortion on (K1 ≠ 0); compare every output pixel with a CPU loop (source texel as the existing tests compute it,
  then `ClassifyTexel`) — exact equality. Run `scripts/run_gpu_tests.sh CamSim.GPU.GroundTruth` → RED (missing
  permutation) → GREEN; the existing InstanceId/Depth GPU tests stay green.
- [ ] Commit `feat(gt): InstanceIdCS WRITE_SEMANTIC (4D)`.

### Task 4: Capture plumbing, files, config

**Files:** `Camera/CamSimCaptureComponent.h/.cpp` (land-cover window updated when `bSemanticPossible`; params built at
capture; `SemanticReadbackPool`, `SemanticGrabbedGeneration`, `FSlot::Semantic`, `bWantSemantic`; poll copy; failure
resets; `SubmitFrameToEncoder` gets the array); `Camera/FrameGrabRequestQueue.h` (`bSemantic`, `TSharedPtr<const
FSemanticFrameParams>`, the GPU window); `Camera/CamSimFrameGrabExtension.h/.cpp` (third readback; bind the window only
when `CamSimLandCover::ShouldBindWindow(...)`); `GroundTruth/FGroundTruthCollector.*` (`WriteSemanticFrame`,
`static TArray64<uint8> EncodeSemanticPng(const TArray<uint32>& Pairs, int32 W, int32 H)`); `Control/ControlCaptureJob.h`
(`bWantSemantic`, `SemanticPng`); `Config/CamSimConfig.*` (`MLTraining.bSemantic`); `Subsystem/CamSimSubsystem.*`
(`IsGroundTruthSemanticAvailable()` = `IsGroundTruthMaskAvailable()`: every InstanceIdCS permutation, semantic included, is checked at startup; without land cover the terrain is `unlabeled` but sky/water/entities still work);
docs/configuration.md row. Test `Tests/SemanticCaptureTest.cpp`.

- [ ] Tests (NullRHI): `EncodeSemanticPngRoundTrip` (pairs → 8-bit grey PNG values), `ConfigKey` (yaml + env),
  `WindowBindRule` (params id ≠ GPU id → not bound; Review Focus 4, via `CamSimLandCover::ShouldBindWindow`),
  `CollectorForceWritesSemantic` (file written for a forced frame with interval 1000).
- [ ] RED → implement → GREEN; full NullRHI suite; GPU suite (`run_gpu_tests.sh`) green. Commit `feat(gt): semantic readback, files, capture job (4D)`.

### Task 5: Control API

**Files:** `Control/ControlApiJson.*` (`FCaptureRequest::bSemantic` default true), `Control/ControlApiService.cpp`
(job `bWantSemantic`; response `semantic_png_b64` when non-empty; status `ground_truth.semantic` via hook),
`Subsystem/CamSimSubsystem.cpp` hooks; tests in `ControlApiJsonTest.cpp` (`Capture` case) and `ControlApiRoutesTest.cpp`
(`CaptureRoundTrip` asserts `semantic_png_b64`).

- [ ] RED → GREEN; `CamSim.ControlApi` green. Commit `feat(control): semantic image in captures (4D)`.

### Task 6: Python client and dataset

**Files:** `scripts/control/camsim_control/client.py` (`Capture.semantic_png`, `capture(semantic=True)`),
`coco.py` (`semantic/NAME.png`, `camsim.semantic_file` per image, top-level `camsim_semantic_classes` [{id, name}]),
`camsim_control/semantic.py` (the class table mirrored, `CLASSES: dict[int, str]`); tests.

- [ ] Tests: client decodes `semantic_png_b64`; writer writes the file and the class list; class table equals the C++
  one (parse `GroundTruth/SemanticClasses.cpp`'s name table with a regex, so the two can't drift).
- [ ] RED → GREEN. Commit `feat(control): semantic PNGs in datasets (4D)`.

### Task 7: Acceptance `scripts/semantic_check.py`

S1–S6 of spec §5 on Pendleton (`pendleton-bridges`), reusing `control_api_check.launch`/`camera_at`/`committed`.
Sites: flat (paved) and roof from `scripts/georeg/sites_pendleton.json`; sea (33.21, −117.43); a tree site chosen
once from the package's land cover (a code-10 cell with a 5×5 code-10 neighbourhood, read with the scene tooling's
land-cover tile reader) and committed as `scripts/semantic/sites_pendleton.json` with how it was picked.
Output `report.md` + overlays (class image colourised beside the EO image).

- [ ] Run live; fix what fails with a failing test first for C++ logic. Commit `test(gt): semantic acceptance S1–S6 (4D)`.

### Task 8: Docs

`docs/ground-truth.md` (class table, sources, limits), `docs/control-api.md` (`semantic`), `docs/configuration.md`,
`docs/testing.md`, `ROADMAP.md` (4D semantic part done with measured results; rest of 4D open), `CLAUDE.md` (counts,
architecture line, gotcha: semantic classes ride InstanceIdCS's WRITE_SEMANTIC and its CPU reference must change
together). Commit `docs: semantic ground truth (4D)`.
