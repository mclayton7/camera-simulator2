# Vegetation, deeper (REALISM R4) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Several species and shrubs chosen per land-cover code by the scene package, tree density and height from
CHMv2 canopy rasters, and a far tier out to 4 km whose edge dissolves, all deterministic and within the R4 bench budget.

**Architecture:** Three parts on the existing R4 slice. (A) A v2 vegetation library (roles, far meshes, crown radius), a
per-code species mix (`vegetation/mix.json` from the package's `[vegetation]` table) and a pure scatter v2 that adds a
species draw, canopy-driven acceptance and a shrub grid on new hash sub-streams. (B) A `camsim-scene` canopy layer
(CHMv2 → cover % and p90 height on the land-cover grid) read at runtime through `FLandCoverTileCache`. (C) Streaming by
400 m blocks, each either near (16 full cells) or far (one low-poly, thinned, shadowless actor with four dissolve HISMs),
with a no-gap handoff.

**Tech Stack:** UE 5.8 C++ (Automation tests, HISM, Cesium for Unreal 2.29 `SampleHeightMostDetailed`, glTFRuntime),
Python 3.12 (`camsim-scene`, rasterio, numpy, Pillow, pytest; `uv`), `prepare_tree.py` (obj2gltf, gltfpack via `npx`).

**Spec:** `docs/superpowers/specs/2026-10-11-r4-vegetation-deeper-design.md`

## Global Constraints

- Copyright line on every new C++ file: `// Copyright CamSim Contributors. All Rights Reserved.`; `#include "CoreMinimal.h"` first, `.generated.h` last; UE naming (`F`/`A`/`U`/`E`), verb-first functions.
- Pure units are pure: `VegetationScatter`, `VegetationMix`, `VegetationBlocks`, `VegetationFar` never touch UObjects, files or the game thread.
- Determinism: every draw is `CamSimHash::Hash(col, row, k, seed, sub)`; never a live RNG. Vegetation owns streams 80–86, sub-streams 160–173 (160–164 R4, 165 tree species, 166–171 shrubs, 172 far keep, 173 dissolve bucket).
- **R4 bit-identity:** with no canopy layer, no package mix (built-in default, `tree_broadleaf_a` at library index 0), shrubs and far tier off, every tree cell's `HashInstances` equals R4's.
- Assets are CC0-1.0 only (`prepare_tree.py` refuses anything else); `tree_broadleaf_a` stays library index 0.
- Canopy data: Meta/WRI CHMv2, `https://dataforgood-fb-data.s3.amazonaws.com/forests/v2/global/dinov3_global_chm_v2_ml3/chm/<quadkey>.tif`, level-10 quadkeys, uint8 metres, EPSG:3857, licence `CC-BY-4.0`.
- Canopy tiles: two greyscale 8-bit 600 × 600 PNG sets on the land-cover grid (0.05°), `canopy/cover/` (%) and `canopy/height/` (m), each `index.json` with `"format": "camsim-canopy-1"`. Cover = share of CHM pixels ≥ 3 m; height = nearest-rank p90 `sorted[floor(0.9 (n − 1))]` of those pixels.
- Config keys (all startup only, `CAMSIM_VEGETATION_*` env overrides, documented in `docs/configuration.md` and `deploy/camsim_config.yaml`): `mix_file` "" ; `canopy_enabled` true; `shrubs_enabled` true; `shrub_radius_m` 500 [0, `radius_m`]; `shrub_spacing_m` 3.0 [1, 10]; `far_radius_m` 4000 (0 or [1.5 × `radius_m`, 10000]); `far_block_cells` 4 [2, 8]; `far_keep` 0.35 (0, 1]; `far_scale` 1.3 [1, 2].
- Bench budget over all-off: game p50 ≤ +1.5 ms, p95 ≤ +3.0 ms, GPU ≤ +5.0 ms, memory ≤ +1024 MiB. Defaults are starting values; the user picks the final ones from the sweep.
- After each C++ task: `scripts/check.sh` compiles, and the vegetation tests pass headlessly (command below). After each Python task: the scene tests pass.
- Keep `ROADMAP.md`, `REALISM.md`, `CLAUDE.md` (test counts too) current at the end (Task 14). No human editor work.

**Commands used throughout** (macOS; Linux: `UE_BIN=/opt/UE/Engine/Binaries/Linux/UnrealEditor`, drop `-DisablePython`):

```bash
# compile
scripts/check.sh
# vegetation automation tests (NullRHI, < 1 min); FILTER e.g. CamSim.Vegetation or CamSim.Vegetation.Scatter
UE_BIN="/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"
"$UE_BIN" "$PWD/unreal_project/CamSimTest/CamSimTest.uproject" -ExecCmds="Automation RunTests $FILTER+Quit" \
  -TestExit="Automation Test Queue Empty" -ReportExportPath="$PWD/.cache/automation-report" \
  -unattended -nullrhi -nosound -nosplash -DisablePython -log -stdout -FullStdOutLogOutput
python3 scripts/parse_automation_report.py .cache/automation-report/index.json
# scene tooling tests
uv run --project scripts/scene --with pytest pytest scripts/scene/tests -q
```

## Review Focus

1. **A package mix naming species the mounted library lacks** (a user's own `library_dir`): a warning per name, the code renormalised, trees still placed; with `scene.offline: true` CamSim exits 1. Tests in Task 3 (parse) and Task 5 (offline fatal flag).
2. **The camera at the canopy layer's edge** (canopy covers the package bbox; land cover covers the 100 km ring): outside the canopy tiles the scatter uses the R4 rule, no crash, no empty band. Test in Task 8 (`Sample` unset outside, scatter falls back).
3. **A camera cut of tens of km during a near↔far switch** (a CIGI reposition): every block and cell of the old place is dropped, nothing waits forever on a `MayDrop` that can't become true. Test in Task 12 (teleport clears everything within a few ticks).
4. **Reduced configurations:** `far_radius_m: 0`, `shrubs_enabled: false`, a library without shrubs or without far meshes, a v1 library: no errors, R4-like behaviour. Tests in Tasks 1, 3, 5 and 12.
5. **Extreme canopy texels:** cover 100 % with height 255 m, cover > 0 with height < 3 m, height ≥ 3 m with cover 0: scale clamps to [0.5, 2] × jitter, no tree below 3 m, acceptance clamps to [0, 1]. Test in Task 4.

---

## File structure

C++ (`unreal_project/CamSimTest/Source/CamSimTest/`):

| File | Responsibility |
|---|---|
| `Vegetation/VegetationLibrary.{h,cpp}` (modify) | `camsim-vegetation-1/2` parse; roles, crown radius, far mesh; sha checks |
| `Vegetation/VegetationMix.{h,cpp}` (new) | `camsim-vegmix-1` parse against a library, built-in default, `PickType` |
| `Vegetation/VegetationScatter.{h,cpp}` (modify) | `GenerateCellContent` (trees + shrubs, species, canopy); R4 wrappers |
| `Vegetation/VegetationCanopy.{h,cpp}` (new) | point samples from `canopy/cover` + `canopy/height` tile caches |
| `Vegetation/VegetationBlocks.{h,cpp}` (new) | block ids, block geometry, `BlocksWithin`, `FVegetationTierPlanner` |
| `Vegetation/VegetationFar.{h,cpp}` (new) | pure far content: `FarFilter`, `FFarHeightGrid` |
| `Vegetation/VegetationStreaming.{h,cpp}` (modify) | add `TickWanted` (caller-chosen wanted set, `MayDrop`, caps) |
| `Vegetation/VegetationFarTier.{h,cpp}` (new) | far-block pipeline (generate → grid heights → spawn), hooks |
| `Vegetation/VegetationManager.{h,cpp}` (modify) | planner + near cells (trees + shrubs) + far tier; live hooks; dumps; events |
| `Vegetation/VegetationCellActor.{h,cpp}` (modify) | one actor, N HISM batches (mesh, shadows, cull) |
| `Thermal/LandCoverTiles.{h,cpp}` (modify) | optional expected index format |
| `Config/CamSimConfig.{h,cpp}`, `Config/ScenePackage.cpp` (modify) | new keys, validation, `Scene.VegetationMixPath`, `Scene.CanopyDir` |
| `Subsystem/CamSimSubsystem.cpp` (modify) | exit 1 on an offline mix error |
| `Tests/Vegetation*Test.cpp` | one test file per unit (new: Mix, Canopy, Blocks, Far, FarTier) |

Python:

| File | Responsibility |
|---|---|
| `scripts/vegetation/prepare_tree.py` (modify) | `--role`, `--crown-radius-m`, `--far-tris` |
| `scripts/scene/camsim_scene/config.py` (modify) | `[vegetation]`, `[canopy]` tables; `canopy` layer priorities |
| `scripts/scene/camsim_scene/vegmix.py` (new) | mix table → `vegetation/mix.json` |
| `scripts/scene/camsim_scene/sources/chmv2.py` (new) | quadkeys, discovery, attribution |
| `scripts/scene/camsim_scene/layers/canopy.py` (new) | CHM → cover/height tiles, index, attribution; `build` |
| `scripts/scene/camsim_scene/pipeline.py`, `verify.py`, `sources/__init__.py`, `sources/base.py` (modify) | wire both in |
| `scripts/vegetation_check.py`, `scripts/bench/scenario.py`, `scripts/bench/sweep.py` (modify) | new gates, `long_range` phase, axes |

---

### Task 1: Library v2

**Files:**
- Modify: `Vegetation/VegetationLibrary.h`, `Vegetation/VegetationLibrary.cpp`
- Test: `Tests/VegetationLibraryTest.cpp`

**Interfaces:**
- Produces: `enum class EVegRole : uint8 { Tree, Shrub }`; `FVegetationType` gains `EVegRole Role = EVegRole::Tree; float CrownRadiusM = 0.0f; FString FarGltfPath; FString FarSha256;` (`bool HasFarMesh() const { return !FarGltfPath.IsEmpty(); }`); `ParseLibrary`/`LoadLibrary` signatures unchanged; `CamSimVegetation::FindType(const FVegetationLibrary&, const FString& Name) -> int32` (−1 if absent).

- [ ] **Step 1: Write the failing tests** (append to `Tests/VegetationLibraryTest.cpp`)

```cpp
namespace
{
	FString V2(const FString& TypesJson) { return TEXT("{\"format\":\"camsim-vegetation-2\",\"types\":[") + TypesJson + TEXT("]}"); }
	const FString ShaA = FString::ChrN(64, 'a');
	FString TypeJson(const TCHAR* Name, const TCHAR* Role, const TCHAR* Extra = TEXT(""))
	{
		return FString::Printf(TEXT("{\"name\":\"%s\",\"role\":\"%s\",\"gltf\":\"%s.glb\",\"sha256\":\"%s\",\"height_m\":10.0,")
			TEXT("\"licence\":\"CC0-1.0\",\"source_url\":\"u\"%s}"), Name, Role, Name, *ShaA, Extra);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegLibV2Test, "CamSim.Vegetation.Library.V2",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegLibV2Test::RunTest(const FString& Parameters)
{
	FVegetationLibrary L; FString Err;
	const FString Far = FString::Printf(TEXT(",\"far_gltf\":\"p_far.glb\",\"far_sha256\":\"%s\",\"crown_radius_m\":2.5"), *FString::ChrN(64, 'b'));
	TestTrue(TEXT("v2 parses"), ParseLibrary(V2(TypeJson(TEXT("p"), TEXT("tree"), *Far) + TEXT(",") + TypeJson(TEXT("s"), TEXT("shrub"))), TEXT("/lib"), L, Err));
	if (TestEqual(TEXT("two types"), L.Types.Num(), 2))
	{
		TestEqual(TEXT("tree role"), L.Types[0].Role, EVegRole::Tree);
		TestEqual(TEXT("crown given"), L.Types[0].CrownRadiusM, 2.5f);
		TestEqual(TEXT("far path"), L.Types[0].FarGltfPath, FString(TEXT("/lib/p_far.glb")));
		TestTrue(TEXT("has far"), L.Types[0].HasFarMesh());
		TestEqual(TEXT("shrub role"), L.Types[1].Role, EVegRole::Shrub);
		TestEqual(TEXT("shrub crown default 0.5 h"), L.Types[1].CrownRadiusM, 5.0f);
		TestFalse(TEXT("shrub no far"), L.Types[1].HasFarMesh());
		TestEqual(TEXT("find"), FindType(L, TEXT("s")), 1);
		TestEqual(TEXT("find missing"), FindType(L, TEXT("x")), -1);
	}
	TestFalse(TEXT("unknown role"), ParseLibrary(V2(TypeJson(TEXT("p"), TEXT("grass"))), TEXT("/lib"), L, Err));
	TestTrue(TEXT("unknown role message"), Err.Contains(TEXT("role")));
	TestFalse(TEXT("duplicate name"), ParseLibrary(V2(TypeJson(TEXT("p"), TEXT("tree")) + TEXT(",") + TypeJson(TEXT("p"), TEXT("shrub"))), TEXT("/lib"), L, Err));
	TestFalse(TEXT("shrub with far mesh"), ParseLibrary(V2(TypeJson(TEXT("s"), TEXT("shrub"), *Far)), TEXT("/lib"), L, Err));
	const FString FarNoSha = TEXT(",\"far_gltf\":\"p_far.glb\"");
	TestFalse(TEXT("far without sha"), ParseLibrary(V2(TypeJson(TEXT("p"), TEXT("tree"), *FarNoSha)), TEXT("/lib"), L, Err));
	TestFalse(TEXT("unknown format"), ParseLibrary(TEXT("{\"format\":\"camsim-vegetation-3\",\"types\":[]}"), TEXT("/lib"), L, Err));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegLibV1CompatTest, "CamSim.Vegetation.Library.V1Compat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegLibV1CompatTest::RunTest(const FString& Parameters)
{
	FVegetationLibrary L; FString Err;
	TestTrue(TEXT("v1 parses"), ParseLibrary(Entry(FString::ChrN(64, 'a')), TEXT("/lib"), L, Err));
	if (TestEqual(TEXT("one"), L.Types.Num(), 1))
	{
		TestEqual(TEXT("v1 = tree"), L.Types[0].Role, EVegRole::Tree);
		TestEqual(TEXT("v1 crown default 0.3 h"), L.Types[0].CrownRadiusM, 2.7f);
		TestFalse(TEXT("v1 no far"), L.Types[0].HasFarMesh());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegLibFarShaTest, "CamSim.Vegetation.Library.FarMeshSha",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegLibFarShaTest::RunTest(const FString& Parameters)
{
	const FString Dir = LibTempDir(TEXT("FarSha"));
	const TArray<uint8> Near = { 1, 2, 3 }, FarB = { 4, 5 };
	FFileHelper::SaveArrayToFile(Near, *FPaths::Combine(Dir, TEXT("p.glb")));
	FFileHelper::SaveArrayToFile(FarB, *FPaths::Combine(Dir, TEXT("p_far.glb")));
	const FString Good = FString::Printf(TEXT("{\"format\":\"camsim-vegetation-2\",\"types\":[{\"name\":\"p\",\"role\":\"tree\",\"gltf\":\"p.glb\",\"sha256\":\"%s\",")
		TEXT("\"far_gltf\":\"p_far.glb\",\"far_sha256\":\"%s\",\"height_m\":10,\"licence\":\"CC0-1.0\"}]}"), *Sha256Hex(Near), *Sha256Hex(FarB));
	FFileHelper::SaveStringToFile(Good, *FPaths::Combine(Dir, TEXT("library.json")));
	FVegetationLibrary L; FString Err;
	TestTrue(TEXT("both shas match"), LoadLibrary(Dir, L, Err));
	FFileHelper::SaveStringToFile(Good.Replace(*Sha256Hex(FarB), *FString::ChrN(64, 'c')), *FPaths::Combine(Dir, TEXT("library.json")));
	TestFalse(TEXT("far sha mismatch refused"), LoadLibrary(Dir, L, Err));
	TestTrue(TEXT("message names the far file"), Err.Contains(TEXT("p_far.glb")));
	return true;
}
```

- [ ] **Step 2: Run, verify they fail to compile/fail** — `scripts/check.sh` fails on `EVegRole`/`FindType`/`HasFarMesh`.

- [ ] **Step 3: Implement.** In `VegetationLibrary.h` add the enum, fields, `HasFarMesh()` and `FindType` declaration. In `VegetationLibrary.cpp`:
  - accept `camsim-vegetation-1` and `camsim-vegetation-2` (`const bool bV2 = Format == TEXT("camsim-vegetation-2")`; any other → `library.json format '%s' is not camsim-vegetation-1 or -2`);
  - per type: `role` (v2 only; missing = tree; `"tree"`/`"shrub"` else error `type '%s': role '%s' is not tree or shrub`); `crown_radius_m` (optional, must be in (0, 30]) else default `(Role == Shrub ? 0.5f : 0.3f) * HeightM`; `far_gltf` + `far_sha256` both or neither (`type '%s': far_gltf needs far_sha256`), a shrub with `far_gltf` → `type '%s': shrubs have no far mesh`; duplicate names → `duplicate type name '%s'`;
  - factor the LFS/sha check of `LoadLibrary` into `static bool CheckFile(const FString& Name, const FString& Path, const FString& Sha, FString& OutError)` and call it for `GltfPath` and, when set, `FarGltfPath`;
  - `int32 FindType(const FVegetationLibrary& L, const FString& Name) { return L.Types.IndexOfByPredicate([&](const FVegetationType& T) { return T.Name == Name; }); }`.

- [ ] **Step 4: Run tests** — `FILTER=CamSim.Vegetation.Library`: all pass (old `Parse`/`Sha256` included).
- [ ] **Step 5: Commit** — `git commit -m "feat(vegetation): library v2 (roles, crown radius, far meshes) (REALISM R4)"`

---

### Task 2: Species and shrub assets (risk task) + `prepare_tree.py` far meshes

**Files:**
- Modify: `scripts/vegetation/prepare_tree.py`
- Modify: `unreal_project/CamSimTest/Content/NonUFS/Vegetation/library.json` (+ new `.glb`s, git LFS)
- Modify: `docs/vegetation.md` ("Asset" section → one row per type, licences quoted)

**Interfaces:**
- Consumes: Task 1's v2 format.
- Produces: library types, in this order: `tree_broadleaf_a` (index 0), `tree_pine_a`, `tree_pine_b`, `shrub_chaparral`, `shrub_desert`, optionally `cactus_a`; each tree with `far_gltf`. Later tasks refer to these names.

- [ ] **Step 1: Download candidate kits into separate empty directories (untrusted data):**

```bash
mkdir -p .cache/veg/src/kenney_nature && cd .cache/veg/src/kenney_nature && \
  curl -L -o kenney_nature-kit.zip "https://kenney.nl/media/pages/assets/nature-kit/<link from the page>" && unzip -q kenney_nature-kit.zip && cd -
```
  Open the page in a browser if the zip URL is not discoverable; save the zip as above. Do the same for the free Quaternius "Stylized Nature MegaKit" into `.cache/veg/src/quaternius_megakit/`. Record, for each: the licence file's text (`License.txt`), the page wording, the date checked. Reject any kit whose licence file is not CC0 1.0.

- [ ] **Step 2: Pick models.** List candidates (`find .cache/veg/src -iname '*bush*' -o -iname '*shrub*' -o -iname '*cactus*' -o -iname '*plant*'`). Choose: a dense rounded bush (`shrub_chaparral`, 1.5–2.0 m), a sparse twiggy/low bush (`shrub_desert`, 1.0–1.5 m), and `cactus_a` only if a cactus model exists (Yuma). Pines: `.cache/veg/src/quaternius/Textured Stylized Trees - May 2020/OBJ/Pine_1.obj` and `Pine_4.obj` (or whichever two look most like longleaf/loblolly: tall bare trunk, crown in the top third; view the pack's `Preview.jpg`). If no kit has a CC0 shrub, stop and tell the user; the fallback is the procedural path (`CAMSIM_VEGETATION_PROCEDURAL`, a low-poly ellipsoid shrub) and is its own decision.

- [ ] **Step 3: Extend `prepare_tree.py`.** New arguments:

```python
    ap.add_argument("--role", choices=("tree", "shrub"), default="tree")
    ap.add_argument("--crown-radius-m", type=float, default=None, help="default 0.3 x height (tree) / 0.5 x height (shrub)")
    ap.add_argument("--far-tris", type=int, default=150, help="far-tier mesh triangle budget (trees only; 0 = none)")
```
  After writing `<name>.glb`, for trees with `--far-tris > 0`: run the same `gltfpack -si <ratio>` pass on the prepared `.glb` (ratio = `far_tris / triangle_count(doc)`, capped at 1.0; no `-cc`/`-kn` flags, as for the near mesh), then downscale every embedded image to ≤ 128 px on its long side with Pillow (`Image.LANCZOS`, keep PNG/JPEG type, re-encode deterministically: PNG `optimize=False`, JPEG `quality=85, optimize=False`), check `triangle_count <= far_tris` (fail otherwise) and write `<name>_far.glb`. Print the v2 entry:

```python
    entry = {"name": a.name, "role": a.role, "gltf": f"{a.name}.glb", "sha256": sha,
             "height_m": a.height_m, "crown_radius_m": crown, "thermal_class": thermal,
             "licence": a.licence, "source_url": a.source_url}
    if far_path:
        entry |= {"far_gltf": far_path.name, "far_sha256": sha256(far_path)}
```
  `thermal` = `tree_canopy` for trees, `shrubland` for shrubs. Running the script twice must produce byte-identical files (check with `shasum`).

- [ ] **Step 4: Prepare every type** (example for a pine; shrubs with `--role shrub --far-tris 0`):

```bash
Q=".cache/veg/src/quaternius/Textured Stylized Trees - May 2020"
python3 scripts/vegetation/prepare_tree.py "$Q/OBJ/Pine_1.obj" --name tree_pine_a --height-m 16.0 --max-tris 10000 \
  --licence CC0-1.0 --source-url https://quaternius.itch.io/textured-lowpoly-trees \
  --out unreal_project/CamSimTest/Content/NonUFS/Vegetation \
  --texture "Pine=$Q/Textures/Pine_Leaves.png" --texture "Bark=$Q/Textures/Tree_Bark.jpg"
```
  Re-prepare `tree_broadleaf_a` from `Tree_7.obj` with the same arguments as `docs/vegetation.md` "Preparing a model" plus the far mesh: its near `.glb` must keep sha256 `f49350f8…7e0c` (unchanged bytes); only the far mesh is new. Write `library.json` as `camsim-vegetation-2` with the entries in the order above. `git lfs track` already covers `*.glb` under that directory (check `.gitattributes`).

- [ ] **Step 5: Check every model live.** `scripts/run.sh` with the Pendleton package, then per type in the console: `camsim.Vegetation.Probe <abs path to .glb>` (near and far meshes). Check: masked leaves, no black/missing textures, base on the ground, height plausible beside the existing broadleaf. Save one screenshot per type to `docs/images/vegetation/probe-<name>.jpg`.

- [ ] **Step 6: Run the library tests** (`FILTER=CamSim.Vegetation.Library`) and a live start: the log shows `Vegetation: on (type 'tree_broadleaf_a'` (the manager still uses type 0 until Task 5).

- [ ] **Step 7: Docs + commit.** `docs/vegetation.md` "Asset": a table row per type (model file, source URL, licence quote, triangles near/far, height, sha256s). Commit: `feat(vegetation): pine and shrub types with far meshes (CC0) (REALISM R4)`.

---

### Task 3: Species mix (runtime)

**Files:**
- Create: `Vegetation/VegetationMix.h`, `Vegetation/VegetationMix.cpp`
- Modify: `Config/CamSimConfig.h/.cpp` (`mix_file`), `Config/ScenePackage.cpp` (`Scene.VegetationMixPath`)
- Test: `Tests/VegetationMixTest.cpp` (new), `Tests/VegetationConfigTest.cpp`, `Tests/ScenePackageTest.cpp`

**Interfaces:**
- Consumes: `FVegetationLibrary`, `FindType` (Task 1).
- Produces:

```cpp
namespace CamSimVegetation
{
	struct FMixEntry { int32 Type = 0; float CumWeight = 0.0f; };   // CumWeight ascending; the last is 1
	struct FCodeMix
	{
		TArray<FMixEntry> Trees;
		TArray<FMixEntry> Shrubs;
		float ShrubsPer100M2 = 0.0f;
	};
	struct FVegetationMix
	{
		TMap<uint8, FCodeMix> Codes;
		TArray<float> TypeHeightM;        // per library index
		TArray<float> TypeCrownRadiusM;   // per library index
		const FCodeMix* Find(uint8 Code) const { return Codes.Find(Code); }
	};
	inline constexpr float DefaultShrubsPer100M2 = 5.0f;   // a code with shrubs but no shrub_density
	/** Index into Entries (not a library index) for a uniform U in [0, 1): the first entry with U < CumWeight; -1 when empty. */
	CAMSIMTEST_API int32 PickType(const TArray<FMixEntry>& Entries, float U);
	/** {"format":"camsim-vegmix-1","mix":{"<code>":{"<type>":w,...}},"shrub_density":{"<code>":n}} against Lib: splits each
	 *  code's names into trees and shrubs by role, normalises each list. Names Lib lacks go to OutUnknown and are dropped. */
	CAMSIMTEST_API bool ParseMix(const FString& Json, const FVegetationLibrary& Lib, FVegetationMix& Out,
		TArray<FString>& OutUnknown, FString& OutError);
	/** Code 10 -> library index 0 if it is a tree (R4); 20 -> shrub_chaparral at 6 / 100 m^2 and 30 -> at 1 / 100 m^2 when
	 *  the library has it. */
	CAMSIMTEST_API FVegetationMix DefaultMix(const FVegetationLibrary& Lib);
	/** Every code's mix restricted to one type, for the R4 wrappers: Code -> Type, weight 1, no shrubs. */
	CAMSIMTEST_API FVegetationMix SingleTypeMix(uint8 Code, int32 Type, float HeightM, float CrownRadiusM);
}
```
- `FCamSimConfig::FVegetationConfig::MixFile` (`mix_file`, `CAMSIM_VEGETATION_MIX_FILE`); `FSceneConfig::VegetationMixPath` set when `<dir>/vegetation/mix.json` exists.

- [ ] **Step 1: Write the failing tests** (`Tests/VegetationMixTest.cpp`):

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Vegetation/VegetationLibrary.h"
#include "Vegetation/VegetationMix.h"

using namespace CamSimVegetation;

namespace
{
	FVegetationLibrary Lib()
	{
		FVegetationLibrary L;
		auto Add = [&L](const TCHAR* N, EVegRole R, float H) { FVegetationType T; T.Name = N; T.Role = R; T.HeightM = H; T.CrownRadiusM = 0.3f * H; L.Types.Add(T); };
		Add(TEXT("tree_broadleaf_a"), EVegRole::Tree, 9.0f);
		Add(TEXT("tree_pine_a"), EVegRole::Tree, 16.0f);
		Add(TEXT("shrub_chaparral"), EVegRole::Shrub, 1.8f);
		return L;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegMixParseTest, "CamSim.Vegetation.Mix.Parse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegMixParseTest::RunTest(const FString& Parameters)
{
	const FString J = TEXT("{\"format\":\"camsim-vegmix-1\",\"mix\":{\"10\":{\"tree_pine_a\":3,\"tree_broadleaf_a\":1,\"shrub_chaparral\":2},")
		TEXT("\"20\":{\"shrub_chaparral\":1}},\"shrub_density\":{\"20\":8}}");
	FVegetationMix M; TArray<FString> Unknown; FString Err;
	TestTrue(TEXT("parses"), ParseMix(J, Lib(), M, Unknown, Err));
	const FCodeMix* C10 = M.Find(10);
	if (TestNotNull(TEXT("code 10"), C10))
	{
		TestEqual(TEXT("two trees"), C10->Trees.Num(), 2);
		TestEqual(TEXT("names in alphabetical order: broadleaf first"), C10->Trees[0].Type, 0);
		TestEqual(TEXT("broadleaf cum 0.25"), C10->Trees[0].CumWeight, 0.25f);
		TestEqual(TEXT("pine last, cum 1"), C10->Trees[1].Type, 1);
		TestEqual(TEXT("last cum 1"), C10->Trees[1].CumWeight, 1.0f);
		TestEqual(TEXT("shrub list normalised alone"), C10->Shrubs[0].CumWeight, 1.0f);
		TestEqual(TEXT("10 has shrubs but no density: default"), C10->ShrubsPer100M2, DefaultShrubsPer100M2);
	}
	const FCodeMix* C20 = M.Find(20);
	if (TestNotNull(TEXT("code 20"), C20)) { TestEqual(TEXT("no trees"), C20->Trees.Num(), 0); TestEqual(TEXT("density"), C20->ShrubsPer100M2, 8.0f); }
	TestNull(TEXT("code 30 absent"), M.Find(30));
	TestEqual(TEXT("type heights carried"), M.TypeHeightM[1], 16.0f);
	TestEqual(TEXT("PickType low -> entry 0 (broadleaf)"), C10->Trees[PickType(C10->Trees, 0.1f)].Type, 0);
	TestEqual(TEXT("PickType high -> entry 1 (pine)"), C10->Trees[PickType(C10->Trees, 0.9f)].Type, 1);
	TestEqual(TEXT("PickType empty"), PickType({}, 0.5f), -1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegMixUnknownTest, "CamSim.Vegetation.Mix.UnknownNamesDropped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegMixUnknownTest::RunTest(const FString& Parameters)
{
	const FString J = TEXT("{\"format\":\"camsim-vegmix-1\",\"mix\":{\"10\":{\"tree_pine_b\":1,\"tree_pine_a\":1},\"30\":{\"cactus_a\":1}}}");
	FVegetationMix M; TArray<FString> Unknown; FString Err;
	TestTrue(TEXT("parses"), ParseMix(J, Lib(), M, Unknown, Err));
	TestEqual(TEXT("two unknown"), Unknown.Num(), 2);
	TestTrue(TEXT("named"), Unknown.Contains(TEXT("tree_pine_b")) && Unknown.Contains(TEXT("cactus_a")));
	const FCodeMix* C10 = M.Find(10);
	if (TestNotNull(TEXT("10 kept"), C10)) { TestEqual(TEXT("renormalised"), C10->Trees.Num(), 1); TestEqual(TEXT("cum 1"), C10->Trees[0].CumWeight, 1.0f); }
	TestNull(TEXT("30 empty -> absent"), M.Find(30));
	TestFalse(TEXT("bad format"), ParseMix(TEXT("{\"format\":\"x\",\"mix\":{}}"), Lib(), M, Unknown, Err));
	TestFalse(TEXT("bad code"), ParseMix(TEXT("{\"format\":\"camsim-vegmix-1\",\"mix\":{\"300\":{\"tree_pine_a\":1}}}"), Lib(), M, Unknown, Err));
	TestFalse(TEXT("non-positive weight"), ParseMix(TEXT("{\"format\":\"camsim-vegmix-1\",\"mix\":{\"10\":{\"tree_pine_a\":0}}}"), Lib(), M, Unknown, Err));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegMixDefaultTest, "CamSim.Vegetation.Mix.Default",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegMixDefaultTest::RunTest(const FString& Parameters)
{
	const FVegetationMix M = DefaultMix(Lib());
	TestEqual(TEXT("10 -> type 0 (R4)"), M.Find(10)->Trees[0].Type, 0);
	TestEqual(TEXT("10 only one tree"), M.Find(10)->Trees.Num(), 1);
	TestEqual(TEXT("20 shrubs 6"), M.Find(20)->ShrubsPer100M2, 6.0f);
	TestEqual(TEXT("30 shrubs 1"), M.Find(30)->ShrubsPer100M2, 1.0f);
	FVegetationLibrary NoShrubs = Lib(); NoShrubs.Types.RemoveAt(2);
	const FVegetationMix M2 = DefaultMix(NoShrubs);
	TestNull(TEXT("no shrub type: no code 20"), M2.Find(20));
	TestEqual(TEXT("still R4 trees"), M2.Find(10)->Trees[0].Type, 0);
	return true;
}
```
  In `VegetationConfigTest.cpp` add `mix_file` to the defaults (`""`) and the YAML/env test (`mix_file: "/m/mix.json"`, `CAMSIM_VEGETATION_MIX_FILE`), and a validation case: a relative `mix_file` is an error. In `ScenePackageTest.cpp`, the package-fixture test that writes `features/buildings/mask/index.json` gets a sibling: write `vegetation/mix.json` → `Cfg.Scene.VegetationMixPath == Dir / "vegetation/mix.json"`; absent → empty.

- [ ] **Step 2: Run, verify failure** (compile errors).
- [ ] **Step 3: Implement `VegetationMix.cpp`.** Parse with `FJsonSerializer`. Codes: keys parse as integers 1..255 (`FCString::Atoi` + round-trip check `FString::FromInt(C) == Key`), else `mix: code '%s' is not 1..255`. Weights: numbers > 0 and finite, else `mix.%s.%s: weight must be > 0`. Take a code's names in **alphabetical order** (`FJsonObject::Values` is a `TMap` and keeps no order; sorting makes the cumulative weights independent of JSON key order). Split by `Lib.Types[Idx].Role`; cumulative weights `Sum/Total` in float, last forced to exactly 1.0f. `shrub_density` values in [0, 100]. A code with neither trees nor shrubs after dropping is omitted. `TypeHeightM`/`TypeCrownRadiusM` filled for every library index. `PickType`: linear scan `for (i) if (U < E[i].CumWeight) return i; return E.Num() - 1;` (an index into Entries, -1 when empty; callers use `Entries[i].Type`). `DefaultMix`: as the interface comment. `SingleTypeMix`: one code, one tree entry.
- [ ] **Step 4: Config.** `FVegetationConfig::MixFile` (comment `// CAMSIM_VEGETATION_MIX_FILE; absolute; species mix when the package has none (docs/vegetation.md)`); YAML `YamlString(V, "mix_file", ...)`, env `GetEnv(TEXT("CAMSIM_VEGETATION_MIX_FILE"), ...)`, validation `if (!MixFile.IsEmpty() && FPaths::IsRelative(MixFile)) Errors.Add(...)`. `FSceneConfig::VegetationMixPath` reset in `ResolvePackage` and set when `FPaths::FileExists(Dir / TEXT("vegetation/mix.json"))`. `deploy/camsim_config.yaml` + `docs/configuration.md` rows.
- [ ] **Step 5: Run** `FILTER=CamSim.Vegetation.Mix`, `CamSim.Vegetation.Config`, `CamSim.Scene`: pass.
- [ ] **Step 6: Commit** — `feat(vegetation): species mix per land-cover code (REALISM R4)`

---

### Task 4: Scatter v2 (species, canopy, shrubs)

**Files:**
- Modify: `Vegetation/VegetationScatter.h`, `Vegetation/VegetationScatter.cpp`, `CamSimShaders/Public/SensorHash.h` (comment only)
- Test: `Tests/VegetationScatterTest.cpp`

**Interfaces:**
- Consumes: `FVegetationMix`, `PickType` (Task 3).
- Produces:

```cpp
namespace CamSimVegetation
{
	// FScatterParams gains:
	//   float ShrubSpacingM = 3.0f; bool bShrubs = true; float ShrubScaleMin = 0.7f, ShrubScaleMax = 1.3f;
	//   float MinCanopyHeightM = 3.0f;
	struct FVegPoint
	{
		uint8 Code = 0;
		bool  bHasCanopy = false;   // the canopy layer covers this point (else the R4 rule)
		float CoverFrac = 0.0f;     // 0..1
		float HeightM = 0.0f;
	};
	using FPointSampler = TFunctionRef<FVegPoint(double LatDeg, double LonDeg)>;
	struct FCellContent
	{
		TArray<FTreeInstance> Trees;
		TArray<FTreeInstance> Shrubs;
	};
	/** Trees (R4 grid + species 165 + canopy rule) and shrubs (own grid, 166-171). IsClear null = no building test. */
	CAMSIMTEST_API FCellContent GenerateCellContent(uint32 Seed, FVegCellId Cell, const FScatterParams& P,
		const FVegetationMix& Mix, FPointSampler Sample, const FClearPredicate* IsClear = nullptr);
	CAMSIMTEST_API int32 ShrubCandidatesPerAxis(const FScatterParams& P);
	/** Tree acceptance on a canopy texel; 0 below MinCanopyHeightM or at no cover. OutScaleBase = clamp(H / TypeH, 0.5, 2). */
	CAMSIMTEST_API float CanopyAccept(const FScatterParams& P, float CoverFrac, float HeightM, float TypeHeightM,
		float CrownRadiusM, float& OutScaleBase);
}
```
  The existing `GenerateCell(...)` overloads stay and become: `return GenerateCellContent(Seed, Cell, NoShrubs(P), SingleTypeMix(P.TreeCode, 0, 9.0f, 2.7f), [&](double La, double Lo) { FVegPoint V; V.Code = Sampler(La, Lo); return V; }, IsClear).Trees;` (`NoShrubs(P)` copies P with `bShrubs = false`).

- [ ] **Step 1: Write the failing tests** (append to `VegetationScatterTest.cpp`):

```cpp
#include "Vegetation/VegetationMix.h"

namespace
{
	FVegPoint TreesNoCanopy(double, double) { FVegPoint V; V.Code = 10; return V; }
	// Golden R4 values: compute them ONCE before changing the .cpp (Step 2) and paste the printed numbers here.
	constexpr uint32 GoldenSeed = 0x5EEDu;
	constexpr uint32 GoldenHash[3] = { 0u /*paste*/, 0u /*paste*/, 0u /*paste*/ };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegScatterR4IdentityTest, "CamSim.Vegetation.Scatter.R4Identity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegScatterR4IdentityTest::RunTest(const FString& Parameters)
{
	FScatterParams P; P.bShrubs = false;
	const FVegCellId C0 = CellOf(33.307, -117.354, P.CellSizeM);
	const FVegCellId Cells[3] = { C0, { C0.Row + 1, C0.Col }, { C0.Row, C0.Col + 7 } };
	FVegetationLibrary L; FVegetationType T; T.Name = TEXT("tree_broadleaf_a"); T.Role = EVegRole::Tree; T.HeightM = 9.0f; T.CrownRadiusM = 2.7f; L.Types.Add(T);
	const FVegetationMix Mix = DefaultMix(L);
	for (int32 K = 0; K < 3; ++K)
	{
		const uint32 Legacy = HashInstances(GenerateCell(VegetationSeed(GoldenSeed), Cells[K], P, AllTrees));
		const uint32 V2 = HashInstances(GenerateCellContent(VegetationSeed(GoldenSeed), Cells[K], P, Mix, TreesNoCanopy).Trees);
		TestEqual(*FString::Printf(TEXT("cell %d legacy == golden"), K), Legacy, GoldenHash[K]);
		TestEqual(*FString::Printf(TEXT("cell %d v2 == golden"), K), V2, GoldenHash[K]);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegScatterSpeciesTest, "CamSim.Vegetation.Scatter.SpeciesFollowMix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegScatterSpeciesTest::RunTest(const FString& Parameters)
{
	FScatterParams P; P.bShrubs = false;
	FVegetationMix Mix = SingleTypeMix(10, 0, 9.0f, 2.7f);
	Mix.Codes[10].Trees = { { 0, 0.25f }, { 1, 1.0f } };
	Mix.TypeHeightM = { 9.0f, 16.0f }; Mix.TypeCrownRadiusM = { 2.7f, 2.5f };
	int64 N[2] = { 0, 0 };
	const FVegCellId O = CellOf(30.5, -86.5, P.CellSizeM);
	for (int32 K = 0; K < 400; ++K)
		for (const FTreeInstance& T : GenerateCellContent(VegetationSeed(3u), { O.Row + K / 20, O.Col + K % 20 }, P, Mix, TreesNoCanopy).Trees) ++N[T.Type];
	const double Total = double(N[0] + N[1]);
	const double Chi2 = FMath::Square(N[0] - 0.25 * Total) / (0.25 * Total) + FMath::Square(N[1] - 0.75 * Total) / (0.75 * Total);
	TestTrue(*FString::Printf(TEXT("chi2 %.2f < 10.83 (1 dof, p 0.001), n=%.0f"), Chi2, Total), Chi2 < 10.83);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegScatterCanopyTest, "CamSim.Vegetation.Scatter.CanopyCoverMatches",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegScatterCanopyTest::RunTest(const FString& Parameters)
{
	FScatterParams P; P.bShrubs = false;
	const FVegetationMix Mix = SingleTypeMix(10, 0, 9.0f, 2.7f);
	// Below saturation only: one crown per 34.6 m^2 candidate caps crown cover at pi r^2 / 34.6 (0.66 at r 2.7 m).
	for (const float Cover : { 0.1f, 0.3f, 0.5f })
	{
		auto Sample = [Cover](double, double) { FVegPoint V; V.Code = 10; V.bHasCanopy = true; V.CoverFrac = Cover; V.HeightM = 9.0f; return V; };
		double CrownM2 = 0.0, AreaM2 = 0.0;
		const FVegCellId O = CellOf(38.3, -76.4, P.CellSizeM);
		for (int32 K = 0; K < 10000; ++K)
		{
			const FVegCellId C{ O.Row + K / 100, O.Col + K % 100 };
			const FVegCellBounds B = CellBounds(C, P.CellSizeM);
			const double LatC = 0.5 * (B.SouthDeg + B.NorthDeg);
			AreaM2 += GroundDistanceM(B.SouthDeg, B.WestDeg, B.NorthDeg, B.WestDeg) * GroundDistanceM(LatC, B.WestDeg, LatC, B.EastDeg);
			for (const FTreeInstance& T : GenerateCellContent(VegetationSeed(9u), C, P, Mix, Sample).Trees)
				CrownM2 += PI * FMath::Square(2.7 * T.Scale);
		}
		TestTrue(*FString::Printf(TEXT("cover %.1f: crown fraction %.3f within 0.02"), Cover, CrownM2 / AreaM2), FMath::Abs(CrownM2 / AreaM2 - Cover) <= 0.02);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegScatterCanopyEdgesTest, "CamSim.Vegetation.Scatter.CanopyEdgeCases",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegScatterCanopyEdgesTest::RunTest(const FString& Parameters)
{
	FScatterParams P; float S = 0.0f;
	TestEqual(TEXT("below 3 m: none"), CanopyAccept(P, 0.8f, 2.9f, 9.0f, 2.7f, S), 0.0f);
	TestEqual(TEXT("no cover: none"), CanopyAccept(P, 0.0f, 12.0f, 9.0f, 2.7f, S), 0.0f);
	TestEqual(TEXT("full cover, 255 m: clamps to 1"), CanopyAccept(P, 1.0f, 255.0f, 9.0f, 2.7f, S), 1.0f);
	TestEqual(TEXT("scale clamp high"), S, 2.0f);
	CanopyAccept(P, 0.5f, 3.0f, 16.0f, 2.5f, S);
	TestEqual(TEXT("scale clamp low"), S, 0.5f);
	auto Tall = [](double, double) { FVegPoint V; V.Code = 10; V.bHasCanopy = true; V.CoverFrac = 1.0f; V.HeightM = 255.0f; return V; };
	for (const FTreeInstance& T : GenerateCellContent(VegetationSeed(1u), CellOf(30.5, -86.5, 100.0), P, SingleTypeMix(10, 0, 9.0f, 2.7f), Tall).Trees)
		TestTrue(TEXT("scale in [2*0.9, 2*1.1]"), T.Scale >= 1.8f - 1e-5f && T.Scale <= 2.2f + 1e-5f);
	auto Short = [](double, double) { FVegPoint V; V.Code = 10; V.bHasCanopy = true; V.CoverFrac = 0.9f; V.HeightM = 1.0f; return V; };
	TestEqual(TEXT("short canopy: no trees"), GenerateCellContent(VegetationSeed(1u), CellOf(30.5, -86.5, 100.0), P, SingleTypeMix(10, 0, 9.0f, 2.7f), Short).Trees.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegScatterShrubsTest, "CamSim.Vegetation.Scatter.Shrubs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegScatterShrubsTest::RunTest(const FString& Parameters)
{
	FScatterParams P;
	FVegetationMix Mix;
	Mix.TypeHeightM = { 9.0f, 1.8f }; Mix.TypeCrownRadiusM = { 2.7f, 0.9f };
	FCodeMix Scrub; Scrub.Shrubs = { { 1, 1.0f } }; Scrub.ShrubsPer100M2 = 8.0f;
	Mix.Codes.Add(20, Scrub);
	auto HalfHalf = [](double, double Lon) { FVegPoint V; V.Code = FMath::Frac(Lon * 1000.0) < 0.5 ? 20 : 30; return V; };
	int64 On20 = 0, On30 = 0, Trees = 0;
	const FVegCellId O = CellOf(32.7, -114.6, P.CellSizeM);
	for (int32 K = 0; K < 100; ++K)
	{
		const FCellContent C = GenerateCellContent(VegetationSeed(5u), { O.Row + K / 10, O.Col + K % 10 }, P, Mix, HalfHalf);
		Trees += C.Trees.Num();
		for (const FTreeInstance& S : C.Shrubs)
		{
			(FMath::Frac(S.LonDeg * 1000.0) < 0.5 ? On20 : On30)++;
			TestTrue(TEXT("shrub scale range"), S.Scale >= 0.7f && S.Scale <= 1.3f);
			TestEqual(TEXT("shrub type"), int32(S.Type), 1);
		}
	}
	TestEqual(TEXT("no trees (no tree list)"), Trees, int64(0));
	TestEqual(TEXT("none on 30"), On30, int64(0));
	const double Expected = 8.0 / 100.0 * 0.5 * 100 * 100.0 * 100.0;   // per m^2 x half of 100 cells of ~1 ha
	TestTrue(*FString::Printf(TEXT("%lld shrubs vs ~%.0f (+-10%%)"), On20, Expected), FMath::Abs(On20 - Expected) <= 0.10 * Expected);
	FScatterParams Off = P; Off.bShrubs = false;
	TestEqual(TEXT("bShrubs false"), GenerateCellContent(VegetationSeed(5u), O, Off, Mix, HalfHalf).Shrubs.Num(), 0);
	int32 Calls = 0;
	FClearPredicate Never = [&Calls](double, double) { ++Calls; return false; };
	TestEqual(TEXT("mask drops shrubs"), GenerateCellContent(VegetationSeed(5u), O, P, Mix, HalfHalf, &Never).Shrubs.Num(), 0);
	return true;
}
```

- [ ] **Step 2: Capture the golden R4 hashes before touching the .cpp.** Temporarily add to the test a `AddInfo(FString::Printf(TEXT("golden %u"), HashInstances(GenerateCell(VegetationSeed(GoldenSeed), Cells[K], P, AllTrees))))` loop, run `FILTER=CamSim.Vegetation.Scatter.R4Identity` on the **unchanged** scatter (stub `GenerateCellContent` to return `{}` and `DefaultMix` etc. if needed to compile), paste the three values into `GoldenHash`, remove the info loop. Commit the test file alone: `test(vegetation): golden R4 cell hashes`.

- [ ] **Step 3: Implement.** In `VegetationScatter.cpp` add sub-stream constants (`SubSpecies = 165`, `SubShrubE = 166` … `SubShrubSpecies = 171`). `CanopyAccept`:

```cpp
	float CanopyAccept(const FScatterParams& P, float CoverFrac, float HeightM, float TypeHeightM, float CrownRadiusM, float& OutScaleBase)
	{
		OutScaleBase = FMath::Clamp(HeightM / FMath::Max(TypeHeightM, 0.1f), 0.5f, 2.0f);
		if (!(HeightM >= P.MinCanopyHeightM) || !(CoverFrac > 0.0f)) return 0.0f;
		const double N = CandidatesPerAxis(P);
		const double ACand = FMath::Square(P.CellSizeM / N);
		const double R = double(CrownRadiusM) * OutScaleBase;
		return static_cast<float>(FMath::Clamp(double(FMath::Min(CoverFrac, 1.0f)) * ACand / (PI * R * R) * P.DensityScale, 0.0, 1.0));
	}
```
  Tree loop (replaces `GenerateCellImpl`'s body; keep row-major J, I order and the R4 expressions bit for bit):

```cpp
	const float R4Accept = FMath::Clamp(P.DensityScale * P.PTree, 0.0f, 1.0f);
	for (J) for (I)
	{
		const uint32 K = J * N + I;
		const double Fx = (I + Uniform(Hash(X, Y, K, Seed, SubJitterE))) / N;
		const double Fy = (J + Uniform(Hash(X, Y, K, Seed, SubJitterN))) / N;
		FTreeInstance T; T.LatDeg = ...; T.LonDeg = ...;          // as R4
		const FVegPoint Pt = Sample(T.LatDeg, T.LonDeg);
		const FCodeMix* CM = Mix.Find(Pt.Code);
		if (!CM || CM->Trees.Num() == 0) continue;
		const int32 E = PickType(CM->Trees, Uniform(Hash(X, Y, K, Seed, SubSpecies)));
		const int32 Type = CM->Trees[E].Type;
		float ScaleBase = 1.0f;
		const float Accept = Pt.bHasCanopy ? CanopyAccept(P, Pt.CoverFrac, Pt.HeightM, Mix.TypeHeightM[Type], Mix.TypeCrownRadiusM[Type], ScaleBase) : R4Accept;
		if (Accept <= 0.0f || Uniform(Hash(X, Y, K, Seed, SubAccept)) >= Accept) continue;
		if (IsClear && !(*IsClear)(T.LatDeg, T.LonDeg)) continue;
		T.YawDeg = ...;                                            // as R4
		const float U = Uniform(Hash(X, Y, K, Seed, SubScale));
		T.Scale = Pt.bHasCanopy ? ScaleBase * (0.9f + 0.2f * U) : P.ScaleMin + (P.ScaleMax - P.ScaleMin) * U;
		T.Type = static_cast<uint8>(Type);
		Out.Trees.Add(T);
	}
```
  Then, when `P.bShrubs`, the shrub grid with `NS = ShrubCandidatesPerAxis(P)` (`max(1, round(CellSizeM / max(ShrubSpacingM, 0.5)))`), accept `Uniform(Hash(.., SubShrubAccept)) < clamp(CM->ShrubsPer100M2 / 100 × (CellSizeM / NS)² × DensityScale, 0, 1)`, species from `CM->Shrubs` (SubShrubSpecies), the building test, yaw (SubShrubYaw), scale `ShrubScaleMin + (Max − Min) × u` (SubShrubScale). Update `SensorHash.h`'s comment to "Streams 80-86 … 160 + k, k = 0..13" with the list from Global Constraints.

- [ ] **Step 4: Run** `FILTER=CamSim.Vegetation.Scatter`: all pass, including the old `OrderIndependent`, `Density`, `InsideCellAndRanges` and the building-mask tests.
- [ ] **Step 5: Commit** — `feat(vegetation): scatter v2 with species, canopy acceptance and shrubs (REALISM R4)`

---

### Task 5: Near tier with species and shrubs (actor batches, manager, live hooks)

**Files:**
- Modify: `Vegetation/VegetationCellActor.{h,cpp}`, `Vegetation/VegetationManager.{h,cpp}`, `Config/CamSimConfig.{h,cpp}`, `Subsystem/CamSimSubsystem.cpp`, `deploy/camsim_config.yaml`, `docs/configuration.md`
- Test: `Tests/VegetationManagerTest.cpp`, `Tests/VegetationConfigTest.cpp`

**Interfaces:**
- Consumes: Tasks 1, 3, 4.
- Produces:

```cpp
// VegetationCellActor.h
struct FVegHismBatch
{
	UStaticMesh* Mesh = nullptr;
	TArray<FTransform> Instances;   // actor-local ESU, cm
	bool  bCastShadows = true;
	float CullDistanceM = 0.0f;     // InstanceEndCullDistance; <= 0 = no cut
};
// ACamSimVegetationCell::Init(double CentreLatDeg, double CentreLonDeg, double BaseHeightM, const TArray<FVegHismBatch>& Batches);
// ACamSimVegetationCell::GetInstanceCount() sums every batch.

// VegetationManager.h
struct FVegCellSpawn   // gains:
{
	// ... existing (Instances/HeightsM/LocalInstances = trees) ...
	TArray<CamSimVegetation::FTreeInstance> Shrubs;
	TArray<double> ShrubHeightsM;
	TArray<FTransform> ShrubLocal;
};
// FVegetationHooks::Generate becomes:
//   TFunction<void(FVegCellId, TFunction<void(CamSimVegetation::FCellContent)>)> Generate;
// CreateLiveManager gains an out flag:
CAMSIMTEST_API TSharedPtr<FVegetationManager> CreateLiveManager(UWorld* World, ACesium3DTileset* Terrain,
	const FCamSimConfig& Cfg, FString& OutWhyNot, bool& bOutFatal);   // bOutFatal: offline + unknown mix names
```
  Config: `bShrubsEnabled` (true), `ShrubRadiusM` (500), `ShrubSpacingM` (3.0).

- [ ] **Step 1: Failing tests.** In `VegetationManagerTest.cpp`, change `FFake::Generate` to the new signature (`FCellContent C; C.Trees = …; Done(MoveTemp(C));`) and add `int32 ShrubsPerCell = 0;` producing shrubs at `La - K * 1e-5`. New test:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegManagerShrubsTest, "CamSim.Vegetation.Manager.TreesAndShrubsOneHeightBatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegManagerShrubsTest::RunTest(const FString& Parameters)
{
	FFake F; F.TreesPerCell = 3; F.ShrubsPerCell = 5;
	FVegetationStreaming::FParams SP; SP.RadiusM = 60.0;
	TSharedRef<FVegetationManager> M = MakeShared<FVegetationManager>(SP, F.Hooks());
	for (int32 I = 0; I < 6; ++I) { M->Tick(33.3, -117.4); F.Flush(); }
	TestTrue(TEXT("spawned"), F.Live.Num() > 0);
	TestEqual(TEXT("one height batch per cell"), F.HeightCalls, F.Spawns);
	for (const auto& Kv : F.Live)
	{
		TestEqual(TEXT("trees"), Kv.Value.Instances.Num(), 3);
		TestEqual(TEXT("shrubs"), Kv.Value.Shrubs.Num(), 5);
		TestEqual(TEXT("shrub heights split back"), Kv.Value.ShrubHeightsM[0], 100.0 + 3);   // fake height = 100 + index
		TestEqual(TEXT("shrub local"), Kv.Value.ShrubLocal.Num(), 5);
	}
	F.TreesPerCell = 0;   // a cell with shrubs only still spawns
	return true;
}
```
  Plus `CamSim.Vegetation.Manager.ShrubsOnlyCellSpawns` (TreesPerCell 0, ShrubsPerCell 2 → spawned, `BaseHeightM` from the first shrub). Config tests for the three keys and their validation (`shrub_radius_m` > `radius_m` → error; `shrub_spacing_m` 0.5 → error).

- [ ] **Step 2: Run, verify failure.**
- [ ] **Step 3: Implement the manager.** `OnGenerated(Id, Token, FCellContent C)`: count = trees + shrubs; `LonLatH` = trees then shrubs; store `R.Instances = C.Trees; R.Shrubs = C.Shrubs` (add `TArray<FTreeInstance> Shrubs;` to `FRequest`). `FinishHeights`: run `KeepClearOfBuildings` over the combined heights; indices `< Trees.Num()` are trees, the rest shrubs (offset). `BaseHeightM` = the first kept tree's height, else the first kept shrub's. `ToCellLocal` for both lists. A cell with zero kept trees and shrubs reports 0 (Empty).
- [ ] **Step 4: Implement the actor.** Remove the `Trees` default subobject. `Init` creates one `UHierarchicalInstancedStaticMeshComponent` per batch with a non-null mesh and instances: `NewObject<UHierarchicalInstancedStaticMeshComponent>(this)`, `SetupAttachment(Root)`, the same collision/navigation/custom-depth settings as the old `Trees` component, `SetCastShadow`, `SetCullDistances(0, CullCm)` (the old clamping code, `CullDistanceM <= 0` → `MAX_int32`), `SetStaticMesh`, `RegisterComponent()`, then `AddInstances(..., bWorldSpace=false)`. Keep them in `UPROPERTY() TArray<TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> Batches;`. The globe anchor is set before the components register.
- [ ] **Step 5: Implement the live hooks** in `CreateLiveManager`:
  - load every library type's near mesh into `TArray<TStrongObjectPtr<UStaticMesh>> NearMeshes` (index = library index; a type that fails to load logs `Vegetation: could not load type '%s'; it is not drawn` and stays null);
  - the mix: `Cfg.Scene.VegetationMixPath` → `EMix = Package`; else `V.MixFile` → `File`; else `DefaultMix` → `Default`. Unknown names: one `Vegetation: mix names unknown type '%s' (not in the library); dropped` warning each; when `Cfg.Scene.bOffline`, set `bOutFatal = true`, `OutWhyNot = "mix names types the library lacks: ..."` and return null. A mix file that does not parse: offline → fatal, online → warning and `DefaultMix`;
  - `FScatterParams`: `bShrubs = V.bShrubsEnabled && V.ShrubRadiusM > 0`, `ShrubSpacingM = V.ShrubSpacingM`; the generate task calls `GenerateCellContent(Seed, Id, P, *Mix, Sampler, Mask ? &Clear : nullptr)` with `Sampler = [&LandCover](La, Lo) { FVegPoint V; V.Code = LandCover->Code(La, Lo); return V; }` (canopy joins in Task 8);
  - Spawn: batches grouped by tree type (`NearMeshes[Type]`, `bShadows`, cull `CullM`) and by shrub type (shrubs: shadows `bShadows`, cull `V.ShrubRadiusM`). `CullM` stays `V.RadiusM` in this task (Task 12 changes it);
  - dump line: `"hash"` stays `HashInstances(trees)`; instances gain a 6th element, the type; add `"shrub_hash":%u,"shrubs":[[lat,lon,h,yaw,scale,type],...]`;
  - log line: `Vegetation: on (types %d (trees %d, shrubs %d), mix %s, shrubs %s, radius ..., ..., buildings: ...)` with `mix package|file|default` and `shrubs <radius> m|off`.
  In `CamSimSubsystem.cpp` pass `bFatal`; when set and `CamSimScene::ShouldExitOnConfigErrors(Config, GIsEditor)`: log `Vegetation: scene.offline is set — exiting on a mix error`, `GLog->Flush(); std::_Exit(1);`.
- [ ] **Step 6: Run** `FILTER=CamSim.Vegetation` (all), `scripts/check.sh`. Live check: `scripts/run.sh` on `pendleton-r4`: the log shows `mix default, shrubs 500 m`; chaparral appears on the hills (screenshot the riparian pose for the commit message notes).
- [ ] **Step 7: Commit** — `feat(vegetation): species and shrubs in the near tier (REALISM R4)`

---

### Task 6: `[vegetation]` table → `vegetation/mix.json` (scene tooling)

**Files:**
- Create: `scripts/scene/camsim_scene/vegmix.py`, `scripts/scene/tests/test_vegmix.py`
- Modify: `scripts/scene/camsim_scene/config.py` (KEYS, `ScenePlan.vegetation`, `_parse_vegetation`, `layer_settings`), `pipeline.py` (`build_scene` writes it), `verify.py` (check)

**Interfaces:**
- Produces: `vegmix.FORMAT = "camsim-vegmix-1"`, `vegmix.FILE = Path("vegetation") / "mix.json"`, `vegmix.mix_json(settings: dict) -> bytes` (canonical, sorted keys, `\n`-terminated), `vegmix.write(pkg: Path, m: Manifest) -> str | None` (sha256 or None, removes a stale file when the table is absent), `verify` check name `vegetation_mix`. Manifest `layers["vegetation"] = {"mix": {...}, "shrub_density": {...}}` only when the table is present.

- [ ] **Step 1: Failing tests** (`test_vegmix.py`):

```python
import json

import pytest

from camsim_scene import vegmix
from camsim_scene.config import ConfigError, parse_scene

BASE = {"name": "t", "bbox": [-117.4, 33.2, -117.3, 33.3]}


def test_vegetation_table_parses_and_normalises_keys():
    plan = parse_scene({**BASE, "vegetation": {"mix": {"10": {"tree_pine_a": 0.6, "tree_broadleaf_a": 0.4}},
                                               "shrub_density": {"20": 8.0}}})
    assert plan.vegetation == {"mix": {"10": {"tree_broadleaf_a": 0.4, "tree_pine_a": 0.6}}, "shrub_density": {"20": 8.0}}


@pytest.mark.parametrize("bad", [
    {"mix": {"0": {"a": 1}}},                 # code out of 1..255
    {"mix": {"10": {"a": 0}}},                # weight must be > 0
    {"mix": {"10": {}}},                      # empty code
    {"mix": {"10": {"Bad Name": 1}}},         # type names are [a-z0-9_]+
    {"shrub_density": {"20": 101}},           # [0, 100]
    {"mix": {"10": {"a": 1}}, "extra": 1},    # unknown key
])
def test_bad_vegetation_tables_are_refused(bad):
    with pytest.raises(ConfigError):
        parse_scene({**BASE, "vegetation": bad})


def test_mix_json_is_canonical():
    s = {"mix": {"10": {"tree_pine_a": 0.6}}, "shrub_density": {"20": 8.0}}
    a = vegmix.mix_json(s)
    assert a == vegmix.mix_json(json.loads(json.dumps(s)))
    assert json.loads(a) == {"format": "camsim-vegmix-1", **s}
    assert a.endswith(b"\n")


def test_no_table_no_layer_settings():
    from camsim_scene.config import layer_settings, vegetation_settings

    plan = parse_scene(BASE)
    assert vegetation_settings(plan) is None
    assert "vegetation" not in layer_settings(plan)
```
  Add a pipeline test in `tests/test_pipeline.py` using the existing synthetic-package fixture: build with `[vegetation]` → `vegetation/mix.json` exists, is listed in `hashes.txt`, and `verify` reports `vegetation_mix` ok; rebuild without the table → the file is gone.

- [ ] **Step 2: Run** `uv run --project scripts/scene --with pytest pytest scripts/scene/tests/test_vegmix.py -q`: fails.
- [ ] **Step 3: Implement.** `config.py`: add `"vegetation"` to `KEYS`; `ScenePlan.vegetation: dict = field(default_factory=dict)`; `_parse_vegetation(data)` validating as the tests (type names `^[a-z0-9_]{1,64}$`, codes `1..255` given as TOML keys, weights finite > 0, densities in [0, 100]); returned with codes and names sorted; `vegetation_settings(plan) -> dict | None`; `layer_settings` adds `out["vegetation"] = ...` when not None. `vegmix.write`: `atomic_write(pkg / FILE, mix_json(s))`. `pipeline.build_scene`: call `vegmix.write(pkg, m)` after the land cover, before `write_hashes`. `verify.py`: `_check_vegetation_mix(pkg, m)` (file present iff the manifest has the settings; parses; format; equals `mix_json(settings)`).
- [ ] **Step 4: Run** all scene tests: pass.
- [ ] **Step 5: Docs + commit.** `docs/scene-packages.md`: a "Vegetation mix" section (the table, the file, runtime split by role, unknown names). Commit `feat(scene): [vegetation] species mix -> vegetation/mix.json (REALISM R4)`.

---

### Task 7: CHMv2 canopy layer (scene tooling)

**Files:**
- Create: `scripts/scene/camsim_scene/sources/chmv2.py`, `scripts/scene/camsim_scene/layers/canopy.py`, `scripts/scene/tests/test_canopy.py`
- Modify: `sources/__init__.py` (`"chmv2": "camsim_scene.sources.chmv2:Chmv2"`), `sources/base.py` (`Layer.CANOPY = "canopy"`), `config.py` (`LAYERS += ("canopy",)`, profiles `"canopy": []`, `[canopy]` table → `prio["canopy"] = ["chmv2"]`, `canopy_settings`), `pipeline.py` (build + skip logic; exclude `Layer.CANOPY` from the datum/footprint paths like `LANDCOVER`), `verify.py`, `docs/realism/data-sources.md`

**Interfaces:**
- Produces:

```python
# sources/chmv2.py
BASE = "https://dataforgood-fb-data.s3.amazonaws.com/forests/v2/global/dinov3_global_chm_v2_ml3"
CHM_URL = BASE + "/chm/{qk}.tif"
LEVEL = 10
def quadkey(x: int, y: int, z: int = LEVEL) -> str
def quadkeys_for_bbox(w: float, s: float, e: float, n: float, z: int = LEVEL) -> list[str]   # sorted
def quadkey_bounds_3857(qk: str) -> tuple[float, float, float, float]                      # minx, miny, maxx, maxy
class Chmv2(SourceBase): id = "chmv2"; layer = Layer.CANOPY; licence = "CC-BY-4.0"; area_kind = "bbox"; datum = None
    # discover(): one Asset per quadkey that answers HEAD 200 (404 = ocean, skipped); prepare() -> None

# layers/canopy.py
FORMAT = "camsim-canopy-1"; MIN_HEIGHT_M = 3; P90 = 0.9
def texel_stats(lon: np.ndarray, lat: np.ndarray, h: np.ndarray, i: int, j: int) -> tuple[np.ndarray, np.ndarray]
    # (cover %, height m) uint8 600x600 for tile (i, j) from CHM pixel centres (lon, lat) and heights h (uint8)
def tile_from_chm(i: int, j: int, chm_paths: list[Path]) -> tuple[np.ndarray, np.ndarray]
def build(pkg: Path, m: Manifest, ctx: BuildContext) -> LayerStats      # canopy/cover, canopy/height (+ index, ATTRIBUTION)
```

- [ ] **Step 1: Check the CHMv2 attribution text and dates source** (2 minutes): fetch `https://registry.opendata.aws/dataforgood-fb-forestsv2/` and the bucket's `metadata/` listing (`curl -s "https://dataforgood-fb-data.s3.amazonaws.com/?list-type=2&prefix=forests/v2/global/dinov3_global_chm_v2_ml3/metadata/&max-keys=20"`). Use the registry's citation as `attribution`; if per-tile acquisition dates are in `metadata/` or `tiles.geojson` properties, record the min/max date of the used tiles in the asset metadata (`metadata={"acquired": [min, max]}`), else omit them and note it in `data-sources.md`.

- [ ] **Step 2: Failing tests** (`test_canopy.py`):

```python
import numpy as np
import pytest
import rasterio
from rasterio.transform import from_origin

from camsim_scene.layers import canopy
from camsim_scene.sources import chmv2

R = 6378137.0


def merc(lon, lat):
    return np.radians(lon) * R, R * np.log(np.tan(np.pi / 4 + np.radians(lat) / 2))


def test_quadkey_of_pendleton():
    assert chmv2.quadkeys_for_bbox(-117.36, 33.29, -117.34, 33.31) == ["0230132032"]


def test_quadkeys_cover_a_bbox_crossing_tiles():
    qks = chmv2.quadkeys_for_bbox(-117.62, 33.19, -117.24, 33.52)
    assert 2 <= len(qks) <= 6 and qks == sorted(qks)


def test_texel_stats_cover_and_p90():
    i, j = 666, -2348                       # tile +33.30_-117.40
    west, north = j * 0.05, (i + 1) * 0.05
    # 10 pixel centres inside texel (row 0, col 0): heights 0..9 -> cover 7/10 (>= 3), p90 of [3..9] = sorted[floor(0.9*6)] = 8
    lon = west + (np.arange(10) % 5 + 0.5) / 12000 / 5
    lat = north - (np.arange(10) // 5 + 0.5) / 12000 / 2
    cover, height = canopy.texel_stats(lon, lat, np.arange(10, dtype=np.uint8), i, j)
    assert cover.shape == (600, 600) and cover.dtype == np.uint8
    assert cover[0, 0] == 70 and height[0, 0] == 8
    assert cover[0, 1] == 0 and height[0, 1] == 0           # no pixels


def test_texel_stats_ignores_pixels_outside_the_tile():
    i, j = 666, -2348
    lon = np.array([j * 0.05 - 1e-6]); lat = np.array([(i + 0.5) * 0.05])
    cover, _ = canopy.texel_stats(lon, lat, np.array([20], np.uint8), i, j)
    assert cover.sum() == 0


def test_tile_from_synthetic_chm(tmp_path):
    # a 3857 raster over the tile, 1.2 m pixels: west half 10 m canopy, east half bare
    i, j = 666, -2348
    x0, y1 = merc(j * 0.05 - 0.001, (i + 1) * 0.05 + 0.001)
    x1, y0 = merc((j + 1) * 0.05 + 0.001, i * 0.05 - 0.001)
    res = 1.2
    w, h = int((x1 - x0) / res) + 1, int((y1 - y0) / res) + 1
    xs = x0 + (np.arange(w) + 0.5) * res
    data = np.where(np.degrees(xs / R)[None, :] < (j + 0.5) * 0.05, 10, 0).astype(np.uint8).repeat(h, axis=0)
    p = tmp_path / "0230132032.tif"
    with rasterio.open(p, "w", driver="GTiff", width=w, height=h, count=1, dtype="uint8", crs="EPSG:3857",
                       transform=from_origin(x0, y1, res, res), tiled=True, blockxsize=512, blockysize=512) as ds:
        ds.write(data, 1)
    cover, height = canopy.tile_from_chm(i, j, [p])
    assert cover[:, :290].min() == 100 and height[:, :290].min() == 10
    assert cover[:, 310:].max() == 0
    again = canopy.tile_from_chm(i, j, [p])
    assert np.array_equal(again[0], cover) and np.array_equal(again[1], height)   # deterministic


def test_texel_with_no_chm_is_zero(tmp_path):
    cover, height = canopy.tile_from_chm(666, -2348, [])
    assert cover.max() == 0 and height.max() == 0
```
  Plus `test_pipeline.py`: a synthetic package with `[canopy] enabled = true` and a fake `chmv2` adapter whose asset is a small synthetic CHM (follow the `fake_sources.py` pattern used for the other layers): `canopy/cover/index.json` and `canopy/height/index.json` exist with `format` `camsim-canopy-1`, `band` keys, the tiles listed, `ATTRIBUTION.txt` contains the CHMv2 credit, a second build skips (`stats["canopy"].skipped == tiles`), and `verify` reports `canopy` ok. Config tests: `[canopy] enabled = true` → `plan.priorities["canopy"] == ["chmv2"]`; an unknown key in `[canopy]` → `ConfigError`; no table → `[]`.

- [ ] **Step 3: Run, verify failure.**
- [ ] **Step 4: Implement `chmv2.py`.** `quadkey(x, y, z)`: digits `((x >> b) & 1) + 2 * ((y >> b) & 1)` from the top bit. Tile x/y of a lon/lat: `x = floor((lon + 180) / 360 × 2^z)`, `y = floor((1 − ln(tan φ + sec φ) / π) / 2 × 2^z)`. `quadkeys_for_bbox`: the x/y ranges of the corners, every pair, sorted quadkeys. `quadkey_bounds_3857`: from x, y: size `2π R / 2^z`, `minx = −πR + x size`, `maxy = πR − y size`. `discover`: HEAD each `CHM_URL`; 404 → skip; other non-200 → `HttpError`; `Asset(id=qk, url=..., size=size, group=self.id, rank=k)`.
- [ ] **Step 5: Implement `canopy.py`.**

```python
def texel_stats(lon, lat, h, i, j):
    col = np.floor((lon - j * TILE_DEG) * CELLS_PER_DEG).astype(np.int64)
    row = np.floor(((i + 1) * TILE_DEG - lat) * CELLS_PER_DEG).astype(np.int64)
    ok = (col >= 0) & (col < TILE_PX) & (row >= 0) & (row < TILE_PX)
    idx, hv = row[ok] * TILE_PX + col[ok], h[ok]
    n = np.bincount(idx, minlength=TILE_PX * TILE_PX)
    tall = hv >= MIN_HEIGHT_M
    nt = np.bincount(idx[tall], minlength=TILE_PX * TILE_PX)
    cover = np.zeros(TILE_PX * TILE_PX, np.uint8)
    np.divide(100 * nt, n, out=(c := np.zeros(n.shape)), where=n > 0)
    cover[:] = np.floor(c + 0.5).astype(np.uint8)
    height = np.zeros(TILE_PX * TILE_PX, np.uint8)
    ti, th = idx[tall], hv[tall]
    if ti.size:
        order = np.lexsort((th, ti))            # by texel, then height
        ti, th = ti[order], th[order]
        starts = np.flatnonzero(np.r_[True, ti[1:] != ti[:-1]])
        counts = np.diff(np.r_[starts, ti.size])
        height[ti[starts]] = th[starts + np.floor(P90 * (counts - 1)).astype(np.int64)]
    return cover.reshape(TILE_PX, TILE_PX), height.reshape(TILE_PX, TILE_PX)


def tile_from_chm(i, j, chm_paths):
    lons, lats, hs = [], [], []
    w, s, e, n = j * TILE_DEG, i * TILE_DEG, (j + 1) * TILE_DEG, (i + 1) * TILE_DEG
    for p in chm_paths:
        with rasterio.open(p) as ds:
            x0, y0 = _merc(w, s); x1, y1 = _merc(e, n)
            win = from_bounds(x0, y0, x1, y1, ds.transform).round_offsets().round_lengths()
            win = win.intersection(Window(0, 0, ds.width, ds.height))   # rasterio raises WindowError when disjoint: skip
            a = ds.read(1, window=win)
            t = ds.window_transform(win)
            cols = t.c + (np.arange(a.shape[1]) + 0.5) * t.a
            rows = t.f + (np.arange(a.shape[0]) + 0.5) * t.e
            lon = np.degrees(cols / R)
            lat = np.degrees(2 * np.arctan(np.exp(rows / R)) - np.pi / 2)
            lons.append(np.broadcast_to(lon[None, :], a.shape).ravel())
            lats.append(np.broadcast_to(lat[:, None], a.shape).ravel())
            hs.append(a.ravel())
    if not hs:
        z = np.zeros((TILE_PX, TILE_PX), np.uint8)
        return z, z.copy()
    return texel_stats(np.concatenate(lons), np.concatenate(lats), np.concatenate(hs), i, j)
```
  (Catch `rasterio.errors.WindowError` around `intersection` and `continue`.) `build(pkg, m, ctx)`: settings `m.layers.get("canopy")`; none → remove `pkg / "canopy"` and its marker, return empty stats. Inputs hash = `inputs_hash(m.layer_settings_hash("canopy"), __version__, *sorted(asset shas), credit)`; skip when the `.state/canopy.json` marker matches both index files' sha256 and every listed tile exists (mirror `_build_landcover`). Tiles = `landcover.tiles_for_bbox(*m.bbox)`; for each, the CHM files whose `quadkey_bounds_3857` intersect the tile's 3857 bounds; run tiles in a `ProcessPoolExecutor(jobs)` (each tile reads ≈ 26 M pixels; keep ≤ `jobs` in flight); write `cover/<file>` and `height/<file>` with `landcover.write_tile`; index per band = `landcover.build_index(...)` with `format`/`source`/`source_url`/`licence`/`attribution` replaced by the CHMv2 values and `"band": "cover"|"height"`, `"units": "percent"|"m"`; `ATTRIBUTION.txt` in `canopy/`. `pipeline.build_scene`: `stats["canopy"] = canopy_layer.build(pkg, m, ctx)` after `_build_landcover` (only recorded when the layer is on). `verify.py`: `_check_canopy(pkg)`: both indexes parse, same tile list, every tile a 600 × 600 mode-`L` PNG, cover ≤ 100; called when the manifest has the layer. Config: `CANOPY_KEYS = ("enabled",)`, `_parse_canopy`, `canopy_settings(plan) -> {"priorities": [...], "bounds": list(plan.bbox), "tile_deg": 0.05, "min_height_m": 3, "height_stat": "p90 nearest-rank", "format": "camsim-canopy-1"}`.
- [ ] **Step 6: Run** scene tests: pass. Then a live smoke on Pendleton: add `[canopy]\nenabled = true` to the Pendleton scene file used for `pendleton-r4` (find it with `jq -r .config_path .cache/scene-packages/pendleton-r4/manifest.json` or the build log), `uv run --project scripts/scene camsim-scene plan .cache/scene-packages/pendleton-r5veg` … `fetch` … `build`, `verify`. Record the download size and build time for `docs/scene-packages.md`. Visually check one cover tile against the imagery (`.cache/scene-packages/pendleton-r5veg/canopy/cover/+33.30_-117.40.png`: riparian woodland bright).
- [ ] **Step 7: Docs + commit.** `docs/scene-packages.md` "Canopy (R4)": table, source, format, size, licence. `docs/realism/data-sources.md`: the CHMv2 tile facts (level-10 quadkeys, 141 MB, uint8 m, 3857 1.194 m, overviews 2–64). Commit `feat(scene): CHMv2 canopy cover and height layer (REALISM R4)`.

---

### Task 8: Canopy at runtime

**Files:**
- Modify: `Thermal/LandCoverTiles.{h,cpp}` (expected format), `Config/CamSimConfig.{h,cpp}` (`canopy_enabled`), `Config/ScenePackage.cpp` (`Scene.CanopyDir`), `Vegetation/VegetationManager.cpp`
- Create: `Vegetation/VegetationCanopy.{h,cpp}`, `Tests/VegetationCanopyTest.cpp`

**Interfaces:**
- Consumes: `FVegPoint` (Task 4), the tile format (Task 7).
- Produces:

```cpp
// LandCoverTiles.h: new optional constructor argument and ParseIndex overload
explicit FLandCoverTileCache(const FString& Dir, int32 MaxTiles = 64, const TCHAR* ExpectedFormat = TEXT("camsim-landcover-1"));
static bool ParseIndex(const FString& Json, FLandCoverIndex& Out, FString& OutError, const TCHAR* ExpectedFormat);

// VegetationCanopy.h
struct FCanopySample { float CoverFrac = 0.0f; float HeightM = 0.0f; };
class CAMSIMTEST_API FVegetationCanopy
{
public:
	static constexpr int32 MaxTiles = 16;
	explicit FVegetationCanopy(const FString& CanopyDir);   // game thread; <dir>/cover, <dir>/height
	bool IsValid() const;
	const FString& GetError() const;
	/** Unset where either set has no tile for the point (outside the package bbox) or the point is not finite. Any thread. */
	TOptional<FCanopySample> Sample(double LatDeg, double LonDeg);
	TArray<FString> TakeWarnings();
};
```
  `FSceneConfig::CanopyDir` set when both `canopy/cover/index.json` and `canopy/height/index.json` exist. `FVegetationConfig::bCanopyEnabled` (true).

- [ ] **Step 1: Failing tests** (`VegetationCanopyTest.cpp`): write two tiny tile sets into `Saved/Automation/Vegetation/Canopy/{cover,height}` with one tile `+33.30_-117.40.png` each (PNG encode via `IImageWrapperModule` the way `VegetationLandCoverTest.cpp` does: copy its helper), cover tile filled with 40, texel (0, 0) = 100; height tile 12, texel (0, 0) = 255. Index JSON per set with `"format":"camsim-canopy-1"`. Assert: `Sample` at the texel (0, 0) centre → cover 1.0, height 255; inside the tile → 0.4 / 12; at a point in the neighbouring tile → unset; NaN → unset; a set whose index says `camsim-landcover-1` → `IsValid()` false with `format` in the error. In `LandCoverTiles` tests (existing file), a canopy index is refused by the default land-cover cache. Config test for `canopy_enabled`. Scene-package test: `CanopyDir` set only when both indexes exist.
- [ ] **Step 2: Run, verify failure.**
- [ ] **Step 3: Implement.** `FLandCoverTileCache`: store `ExpectedFormat` (an `FString`), use it in `ParseIndex`; the old 3-argument `ParseIndex` calls the new one with `TEXT("camsim-landcover-1")`. `FVegetationCanopy`: two caches (`MaxTiles` each) built with `TEXT("camsim-canopy-1")`; `Sample`: the land-cover `GlobalCell`/`CellToTile` pick (same as `FVegetationLandCover::Code`); both `Get` non-null else unset; values from `Codes[In.Y * TilePx + In.X]` → `CoverFrac = min(v, 100) / 100`, `HeightM = v`.
- [ ] **Step 4: Wire into the scatter sampler** in `CreateLiveManager`: when `V.bCanopyEnabled && !Cfg.Scene.CanopyDir.IsEmpty()`, make a `TSharedPtr<FVegetationCanopy, ESPMode::ThreadSafe>`; invalid → warning `Vegetation: canopy layer unusable (%s); using the land-cover rule` and reset. The generate task's sampler:

```cpp
auto Sampler = [&LandCover, &Canopy](double La, double Lo)
{
	FVegPoint V;
	V.Code = LandCover->Code(La, Lo);
	if (Canopy.IsValid())
		if (const TOptional<FCanopySample> S = Canopy->Sample(La, Lo); S.IsSet())
		{
			V.bHasCanopy = true; V.CoverFrac = S->CoverFrac; V.HeightM = S->HeightM;
		}
	return V;
};
```
  Log line gains `canopy chmv2|none|off`. The tile caches' warnings are drained into the log with the land cover's (`TakeWarnings` on the game thread each tick, as the land cover's are).
- [ ] **Step 5: Run** `FILTER=CamSim.Vegetation`, `CamSim.Thermal.LandCover`, `CamSim.Scene`: pass. Live: `pendleton-r5veg` (built in Task 7) → `canopy chmv2` in the log; trees now on the CHM's woodland outside code 10 where the default mix lists trees (only code 10 by default — so trees still only on 10; check density varies with cover in the riparian corridor).
- [ ] **Step 6: Commit** — `feat(vegetation): canopy cover and height drive tree density and size (REALISM R4)`

---

### Task 9: Blocks and the tier planner (pure)

**Files:**
- Create: `Vegetation/VegetationBlocks.{h,cpp}`, `Tests/VegetationBlocksTest.cpp`

**Interfaces:**
- Consumes: `VegetationGrid.h`.
- Produces:

```cpp
namespace CamSimVegetation
{
	/** A block = BlockCells x BlockCells cells: rows [Row*N, Row*N+N), cols [Col*N, Col*N+N) (cols past NumCols(row) don't exist). */
	using FVegBlockId = FVegCellId;
	CAMSIMTEST_API FVegBlockId BlockOf(FVegCellId Cell, int32 BlockCells);
	CAMSIMTEST_API TArray<FVegCellId> CellsOfBlock(FVegBlockId Block, int32 BlockCells, double CellM);
	CAMSIMTEST_API void BlockCentre(FVegBlockId Block, int32 BlockCells, double CellM, double& OutLatDeg, double& OutLonDeg);
	/** Blocks whose centre is within RadiusM, nearest first (ties Row, Col). */
	CAMSIMTEST_API TArray<FVegBlockId> BlocksWithin(double LatDeg, double LonDeg, double RadiusM, double CellM, int32 BlockCells);

	enum class EVegTier : uint8 { Near, Far };
}

class CAMSIMTEST_API FVegetationTierPlanner
{
public:
	struct FParams
	{
		double NearM = 1000.0;
		double FarM = 4000.0;        // 0 = no far tier
		double NearDropFactor = 1.2; // a near block goes far (or away) past NearM * this
		double FarDropFactor = 1.1;  // a far block goes away past FarM * this
		double CellSizeM = 100.0;
		int32  BlockCells = 4;
	};
	struct FChange { CamSimVegetation::FVegBlockId Block; TOptional<CamSimVegetation::EVegTier> From, To; };

	explicit FVegetationTierPlanner(const FParams& P) : Params(P) {}
	/** Recomputes the tiers; returns the blocks whose tier changed (unset = untracked). */
	TArray<FChange> Update(double CamLatDeg, double CamLonDeg);
	TOptional<CamSimVegetation::EVegTier> GetTier(CamSimVegetation::FVegBlockId B) const;
	/** Cells of the near blocks / the far blocks, each nearest first from the last Update's camera. */
	const TArray<CamSimVegetation::FVegCellId>& GetNearCells() const { return NearCells; }
	const TArray<CamSimVegetation::FVegBlockId>& GetFarBlocks() const { return FarBlocks; }
	const FParams& GetParams() const { return Params; }
private:
	FParams Params;
	TMap<CamSimVegetation::FVegBlockId, CamSimVegetation::EVegTier> Tiers;
	TArray<CamSimVegetation::FVegCellId> NearCells;
	TArray<CamSimVegetation::FVegBlockId> FarBlocks;
};
```

- [ ] **Step 1: Failing tests:**

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegBlocksGeometryTest, "CamSim.Vegetation.Blocks.Geometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegBlocksGeometryTest::RunTest(const FString& Parameters)
{
	const FVegCellId C = CellOf(33.3, -117.4, 100.0);
	const FVegBlockId B = BlockOf(C, 4);
	const TArray<FVegCellId> Cells = CellsOfBlock(B, 4, 100.0);
	TestEqual(TEXT("16 cells"), Cells.Num(), 16);
	TestTrue(TEXT("contains its cell"), Cells.Contains(C));
	for (const FVegCellId& X : Cells) TestEqual(TEXT("every cell maps back"), BlockOf(X, 4), B);
	TestEqual(TEXT("negative rows floor"), BlockOf({ -1, -1 }, 4), FVegBlockId{ -1, -1 });
	double La, Lo; BlockCentre(B, 4, 100.0, La, Lo);
	TestTrue(TEXT("centre within 300 m of the cell"), GroundDistanceM(La, Lo, 33.3, -117.4) < 300.0);
	const TArray<FVegBlockId> W = BlocksWithin(33.3, -117.4, 4000.0, 100.0, 4);
	TestTrue(TEXT("~pi*40^2/16 blocks"), W.Num() > 250 && W.Num() < 380);
	double PrevD = -1.0; bool bSorted = true;
	for (const FVegBlockId& X : W) { double A, O; BlockCentre(X, 4, 100.0, A, O); const double D = GroundDistanceM(33.3, -117.4, A, O); bSorted &= D >= PrevD; PrevD = D; }
	TestTrue(TEXT("nearest first"), bSorted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegTierHysteresisTest, "CamSim.Vegetation.Blocks.TierHysteresis",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegTierHysteresisTest::RunTest(const FString& Parameters)
{
	FVegetationTierPlanner P({});
	P.Update(33.3, -117.4);
	const FVegBlockId Home = BlockOf(CellOf(33.3, -117.4, 100.0), 4);
	TestEqual(TEXT("home near"), P.GetTier(Home), TOptional<EVegTier>(EVegTier::Near));
	double La, Lo; BlockCentre(Home, 4, 100.0, La, Lo);
	const double DLat = 1.0 / 111320.0;
	P.Update(La + 1100.0 * DLat, Lo);   // 1.1 km: past NearM, inside 1.2 NearM
	TestEqual(TEXT("still near (hysteresis)"), P.GetTier(Home), TOptional<EVegTier>(EVegTier::Near));
	P.Update(La + 1300.0 * DLat, Lo);
	TestEqual(TEXT("far past 1.2 NearM"), P.GetTier(Home), TOptional<EVegTier>(EVegTier::Far));
	P.Update(La + 4300.0 * DLat, Lo);
	TestEqual(TEXT("far inside 1.1 FarM"), P.GetTier(Home), TOptional<EVegTier>(EVegTier::Far));
	P.Update(La + 4500.0 * DLat, Lo);
	TestFalse(TEXT("dropped past 1.1 FarM"), P.GetTier(Home).IsSet());
	P.Update(La, Lo);
	TestEqual(TEXT("back: near"), P.GetTier(Home), TOptional<EVegTier>(EVegTier::Near));
	FVegetationTierPlanner::FParams NoFar; NoFar.FarM = 0.0;
	FVegetationTierPlanner Q(NoFar);
	Q.Update(La, Lo); Q.Update(La + 1300.0 * DLat, Lo);
	TestFalse(TEXT("no far tier: dropped"), Q.GetTier(Home).IsSet());
	TestEqual(TEXT("no far blocks"), Q.GetFarBlocks().Num(), 0);
	const TArray<FVegetationTierPlanner::FChange> Ch = Q.Update(La + 50000.0 * DLat, Lo);   // a 50 km cut
	TestEqual(TEXT("nothing near the old place"), Q.GetNearCells().ContainsByPredicate([&](const FVegCellId& C) { return BlockOf(C, 4) == Home; }), false);
	return true;
}
```
- [ ] **Step 2: Run, verify failure.**
- [ ] **Step 3: Implement.** `BlockOf`: floor division (`R >= 0 ? R / N : -((-R + N - 1) / N)`). `CellsOfBlock`: rows `B.Row*N .. +N-1` with `IsLatAllowed(row centre)`, cols `B.Col*N .. +N-1` kept when `< NumCols(row)` and `>= 0`. `BlockCentre`: mean of `CellCentre` of the first and last existing cell (lat) and of the row-middle cell's first/last col (lon). `BlocksWithin`: `CellsWithin(Lat, Lon, RadiusM + 1.5 × N × CellM, CellM)` mapped to unique blocks, kept when the centre distance ≤ RadiusM, sorted like `CellsWithin`. (Cost: about 6,000 cells at 4.4 km; the planner only calls it when the camera moved ≥ 25 m since the last full update, keeping the last result otherwise: add `double LastLat, LastLon` and a `bForce` on first call.) `Update`: candidates = `BlocksWithin(cam, max(NearM × NearDropFactor, FarM × FarDropFactor))`; per candidate (and per tracked block, which may now be outside that radius → distance large) apply:

```text
prev none: d < NearM -> Near; else FarM > 0 && d < FarM -> Far; else none
prev Near: d < NearM*NearDrop -> Near; else FarM > 0 && d < FarM -> Far; else none
prev Far:  d < NearM -> Near; d < FarM*FarDrop -> Far; else none
```
  Tracked blocks not in the candidate list become none. `NearCells` = cells of Near blocks sorted by distance (ties Row, Col); `FarBlocks` = Far blocks sorted by centre distance. Returns the changes.
- [ ] **Step 4: Run** `FILTER=CamSim.Vegetation.Blocks`: pass.
- [ ] **Step 5: Commit** — `feat(vegetation): 400 m blocks and near/far tier planner (REALISM R4)`

---

### Task 10: Streaming with a caller-chosen wanted set

**Files:**
- Modify: `Vegetation/VegetationStreaming.{h,cpp}`
- Test: `Tests/VegetationStreamingTest.cpp`

**Interfaces:**
- Produces:

```cpp
// FVegetationStreaming
/** Tracked ids not in Wanted are dropped when MayDrop(id) (else kept as they are); ReadyToSpawn ids in Wanted spawn in
 *  Wanted's order (at most SpawnCap); untracked Wanted ids start generating while this streaming's in-flight count is
 *  below InFlightCap. Tick(lat, lon) = TickWanted(CellsWithin(..), "beyond RadiusM*DropFactor", MaxCellsInFlight, MaxSpawnsPerTick). */
FActions TickWanted(TConstArrayView<CamSimVegetation::FVegCellId> Wanted, TFunctionRef<bool(CamSimVegetation::FVegCellId)> MayDrop,
	int32 InFlightCap, int32 SpawnCap);
/** Spawned or Empty: nothing more will happen to it. */
bool IsSettled(CamSimVegetation::FVegCellId Id) const;
```
  Note: the legacy `Tick(lat, lon)` keeps its exact drop rule (distance > RadiusM × DropFactor), so it is implemented as: wanted = `CellsWithin(RadiusM)`, `MayDrop = distance > DropM` — and a tracked id that is not wanted but within DropM stays (as today).

- [ ] **Step 1: Failing tests:** `CamSim.Vegetation.Streaming.TickWanted` — (a) a wanted list of 6 ids with caps 4/2 generates the first 4 in order; (b) after reporting them generated + sampled, spawns the first 2 by Wanted order; (c) an id removed from Wanted with `MayDrop` false stays tracked and in `ToDrop` never; with `MayDrop` true it is dropped and appears in `ToDrop` if it was Spawned; (d) `IsSettled` for Spawned/Empty only; (e) the legacy `Tick` tests still pass unchanged.
- [ ] **Step 2–4:** implement (`Tick` rewritten on top of `TickWanted`), run `FILTER=CamSim.Vegetation.Streaming` + `CamSim.Vegetation.Manager`: pass.
- [ ] **Step 5: Commit** — `refactor(vegetation): streaming takes the wanted set from the caller`

---

### Task 11: Far content (pure)

**Files:**
- Create: `Vegetation/VegetationFar.{h,cpp}`, `Tests/VegetationFarTest.cpp`

**Interfaces:**
- Produces:

```cpp
namespace CamSimVegetation
{
	struct FFarParams
	{
		float Keep = 0.35f;
		float Scale = 1.3f;
		int32 Buckets = 4;
		TArray<bool> TypeHasFar;   // per library index; false = never drawn far
	};
	struct FFarTree { FTreeInstance Tree; uint8 Bucket = 0; };
	/** The far subset of one cell's trees: Uniform(Hash(col, row, k, seed, 172)) < Keep, k = index in Trees; scale x Scale;
	 *  bucket = floor(Uniform(Hash(.., 173)) * Buckets). */
	CAMSIMTEST_API TArray<FFarTree> FarFilter(uint32 Seed, FVegCellId Cell, const TArray<FTreeInstance>& Trees, const FFarParams& P);
	CAMSIMTEST_API uint32 HashFarTrees(const TArray<FFarTree>& Trees);

	/** A regular lat/lon grid of heights over a block (bilinear). */
	struct CAMSIMTEST_API FFarHeightGrid
	{
		double SouthDeg = 0, WestDeg = 0, DLatDeg = 0, DLonDeg = 0;
		int32  N = 0;                      // points per axis
		TArray<TOptional<double>> H;       // row-major from the south-west, N*N
		static FFarHeightGrid ForBlock(FVegBlockId Block, int32 BlockCells, double CellM, double SpacingM);
		TArray<FVector> LonLatPoints() const;   // N*N (lon, lat, 0) in H's order
		/** Unset when the point is outside the grid or any of its 4 corners is unset. */
		TOptional<double> Sample(double LatDeg, double LonDeg) const;
	};
	/** Far-tier cull distance of bucket b: FarM * (0.7 + 0.3 * b / (Buckets - 1)) -> 0.7, 0.8, 0.9, 1.0 x FarM for 4
	 *  (FarM for Buckets 1). */
	CAMSIMTEST_API float BucketCullM(float FarM, int32 Bucket, int32 Buckets);
}
```

- [ ] **Step 1: Failing tests:** `CamSim.Vegetation.Far.Subset` (FarFilter output positions/types all come from the input, keep fraction over 20,000 trees within ±2 % of 0.35, scale ×1.3 exactly, types with `TypeHasFar[t] == false` absent, deterministic across calls, bucket counts each 25 % ± 2 %); `CamSim.Vegetation.Far.HeightGrid` (`ForBlock(…, 25 m)` gives N = 17 for 4 × 100 m; a grid with H = lat-linear plane reproduces the plane at random points within 1 mm; a point with an unset corner → unset; outside → unset); `CamSim.Vegetation.Far.BucketCull` (0.7/0.8/0.9/1.0 × 4000).
- [ ] **Step 2–4:** implement; `ForBlock`: bounds from `CellsOfBlock` (min south, max north, min west, max east of the cells' `CellBounds`), `N = max(2, round(max(height_m, width_m) / SpacingM) + 1)`, `DLat = (N - S) / (N - 1)`. Run `FILTER=CamSim.Vegetation.Far`: pass.
- [ ] **Step 5: Commit** — `feat(vegetation): far-tier subset, buckets and height grid (REALISM R4)`

---

### Task 12: Far tier and two-tier manager (no-gap handoff)

**Files:**
- Create: `Vegetation/VegetationFarTier.{h,cpp}`, `Tests/VegetationFarTierTest.cpp`
- Modify: `Vegetation/VegetationManager.{h,cpp}`, `Config/CamSimConfig.{h,cpp}`, `deploy/camsim_config.yaml`, `docs/configuration.md`
- Test: `Tests/VegetationManagerTest.cpp`, `Tests/VegetationConfigTest.cpp`

**Interfaces:**
- Consumes: Tasks 9, 10, 11; the near pipeline of Task 5.
- Produces:

```cpp
struct FVegFarSpawn
{
	double CentreLatDeg = 0.0, CentreLonDeg = 0.0, BaseHeightM = 0.0;
	TArray<CamSimVegetation::FFarTree> Trees;   // grounded ones
	TArray<double> HeightsM;
	TArray<FTransform> Local;                    // centre ESU, cm
};
struct FVegetationFarHooks
{
	TFunction<void(CamSimVegetation::FVegBlockId, TFunction<void(TArray<CamSimVegetation::FFarTree>)>)> Generate;
	TFunction<void(const TArray<FVector>& LonLatH, TFunction<void(TArray<TOptional<double>>)>)> SampleHeights;
	TFunction<void(CamSimVegetation::FVegBlockId, const FVegFarSpawn&)> Spawn;
	TFunction<void(CamSimVegetation::FVegBlockId)> Destroy;
};
class CAMSIMTEST_API FVegetationFarTier : public TSharedFromThis<FVegetationFarTier>
{
public:
	static constexpr double SinkM = 0.3;
	static constexpr double GridSpacingM = 25.0;
	FVegetationFarTier(int32 BlockCells, double CellSizeM, FVegetationFarHooks Hooks);
	/** Same contract as the near streaming: Wanted = far blocks nearest first; MayDrop gates drops. */
	void Tick(TConstArrayView<CamSimVegetation::FVegBlockId> Wanted, TFunctionRef<bool(CamSimVegetation::FVegBlockId)> MayDrop,
		int32 InFlightCap, int32 SpawnCap, int64 TickCount);
	bool IsSettled(CamSimVegetation::FVegBlockId B) const;
	int32 NumInFlight() const;
	const TMap<CamSimVegetation::FVegBlockId, FVegFarSpawn>& GetSpawned() const;
};
// FVegetationManager: constructor gains (const FVegetationTierPlanner::FParams& Tiers, FVegetationFarHooks FarHooks) in an
// overload; the existing (FVegetationStreaming::FParams, FVegetationHooks) constructor = far tier off, so old tests run.
// FVegetationManager::GetFarTier() -> FVegetationFarTier* (null when off); GetPlanner() -> const FVegetationTierPlanner&.
// Events: optional TFunction<void(const FString& JsonLine)> OnEvent in FVegetationHooks (tier changes, spawn, destroy, empty).
```
  Config: `FarRadiusM` (4000), `FarBlockCells` (4), `FarKeep` (0.35), `FarScale` (1.3) with validation per Global Constraints.

- [ ] **Step 1: Failing tests** (`VegetationFarTierTest.cpp`, fake hooks as in `VegetationManagerTest.cpp`, plus a far fake producing 3 far trees per block):
  - `CamSim.Vegetation.FarTier.SpawnsBeyondNear`: NearM 300, FarM 1500, BlockCells 2, CellM 100 → after enough ticks + flushes, near spawns only cells of near blocks, far spawns only far blocks; no block id has both a live near cell and a live far actor **after** settling.
  - `CamSim.Vegetation.FarTier.NoGapOnSwitch`: move the camera so one block goes near → far. Record, per tick, whether the block has ≥ 1 live actor (near cells of it in `Live` or the far fake's `Live`). Assert it never reads 0 between its first spawn and the end (the fakes always give trees). Then far → near: same.
  - `CamSim.Vegetation.FarTier.EmptyTierDoesNotBlock`: the far fake yields 0 trees for that block → the near cells still drop after the switch (Empty counts as settled).
  - `CamSim.Vegetation.FarTier.TeleportClears`: after settling, `Tick` at a point 50 km away: within 3 ticks every old near cell and far block is destroyed (Destroys == Spawns of the old place) — **Review Focus 3**.
  - `CamSim.Vegetation.FarTier.OffEqualsNear`: FarM 0 → `GetFarTier() == nullptr`, the far fake's Generate never called.
  - `CamSim.Vegetation.FarTier.BudgetsShared`: at most 4 in flight across both tiers and at most 2 spawns per tick across both, every tick.
  - `CamSim.Vegetation.FarTier.GridHeights`: one `SampleHeights` call per far block with `N*N` points; a far tree whose quad has a failed corner is dropped; heights = grid − 0.3 m.
- [ ] **Step 2: Run, verify failure.**
- [ ] **Step 3: Implement `FVegetationFarTier`.** Internally an `FVegetationStreaming` (ids = block ids) driven by `TickWanted`, requests with tokens and `StuckTicks` as the near manager (copy the pattern: `FRequest { Token, StartTick, Trees, Grid }`). Generate → `Done(TArray<FFarTree>)`; zero → `OnGenerated(B, 0)`. Then `Grid = FFarHeightGrid::ForBlock(B, BlockCells, CellSizeM, GridSpacingM)`, `SampleHeights(Grid.LonLatPoints(), …)`; on the answer: `Grid.H = …`, each tree `Grid.Sample(lat, lon)` minus `SinkM`, unset → dropped; `OnHeightsSampled(B, kept)`; Ready → spawn with `BlockCentre` as the anchor, `ToCellLocal` (works for any centre; pass `FTreeInstance`s).
- [ ] **Step 4: Implement the manager orchestration** (`FVegetationManager::Tick`):

```cpp
++TickCount;
ExpireStuckRequests();
const TArray<FVegetationTierPlanner::FChange> Changes = Planner.Update(CamLat, CamLon);
for (const auto& C : Changes) EmitEvent(TEXT("tier"), C.Block, C.To);
const int32 BlockCells = Planner.GetParams().BlockCells;
auto NearMayDrop = [this, BlockCells](FVegCellId Cell)
{
	const FVegBlockId B = BlockOf(Cell, BlockCells);
	return !(Far && Planner.GetTier(B) == TOptional<EVegTier>(EVegTier::Far)) || Far->IsSettled(B);
};
const int32 FarInFlight = Far ? Far->NumInFlight() : 0;
const FVegetationStreaming::FActions A = Streaming.TickWanted(Planner.GetNearCells(), NearMayDrop,
	MaxInFlight - FarInFlight, MaxSpawnsPerTick);
// ... existing drop / ready / spawn / generate handling of A (unchanged) ...
if (Far)
{
	auto FarMayDrop = [this, BlockCells](FVegBlockId B)
	{
		if (Planner.GetTier(B) != TOptional<EVegTier>(EVegTier::Near)) return true;
		for (const FVegCellId& C : CellsOfBlock(B, BlockCells, CellSizeM))
			if (!Streaming.IsSettled(C)) return false;
		return true;
	};
	Far->Tick(Planner.GetFarBlocks(), FarMayDrop, MaxInFlight - Streaming.NumInFlight(), MaxSpawnsPerTick - A.ToSpawn.Num(), TickCount);
}
```
  `IsSettled` for a near cell that was never requested (e.g. a block with cells outside the latitude limit) counts as settled only if `CellsOfBlock` omitted it — `CellsOfBlock` already filters those, so every listed cell gets requested. A near cell id not tracked at all (not yet requested) is **not** settled: the far actor stays until the near cells are generated.
  Live hooks in `CreateLiveManager`: far meshes (`TypeHasFar[t] = FarMeshes[t] != null`); `FarHooks.Generate` = a worker task running, for each cell of the block, `GenerateCellContent(Seed, C, PNoShrubs, *Mix, Sampler, Clear)` then `FarFilter(Seed, C, Content.Trees, FarParams)`; `FarHooks.SampleHeights` = the terrain sampler; `FarHooks.Spawn` = an `ACamSimVegetationCell` with batches per (type, bucket): far mesh, `bCastShadows = false`, `CullDistanceM = BucketCullM(FarM, b, 4)`. Near-tree cull becomes `V.RadiusM * 1.2f + BlockCells * CellSizeM * 1.4142f`. Far dump CVar `camsim.Vegetation.FarDumpPath`: `{"block_row","block_col","hash":HashFarTrees,"instances":[[lat,lon,h,yaw,scale,type,bucket],...]}`. Events CVar `camsim.Vegetation.EventsPath`: one JSON line per event, `{"tick":N,"ev":"tier|spawn|destroy|empty","tier":"near|far|none","row":R,"col":C,"block_row":BR,"block_col":BC}` (near events carry both the cell and its block). Log line gains `far <radius> m (keep %.2f, blocks %d)|off`.
- [ ] **Step 5: Config** keys, YAML, env, validation, docs rows (Global Constraints).
- [ ] **Step 6: Run** `FILTER=CamSim.Vegetation` (all), `scripts/check.sh`. Live: `pendleton-r5veg`, a 300 m HAE oblique at the riparian pose: trees reach the horizon, the far edge thins in steps, near/far swap visible only as a model change. Screenshots to `.cache/veg/far/`.
- [ ] **Step 7: Commit** — `feat(vegetation): far tier to 4 km with a dissolving edge and no-gap handoff (REALISM R4)`

---

### Task 13: Acceptance and bench tooling

**Files:**
- Modify: `scripts/vegetation_check.py`, `scripts/bench/scenario.py`, `scripts/bench/sweep.py`
- Create: `scripts/tests/test_vegetation_gates.py` (pure gate functions; run with `uv run --with pytest --with numpy pytest scripts/tests/test_vegetation_gates.py`)

**Interfaces:**
- Consumes: dump formats (Tasks 5, 12), events (Task 12), the mix file (Task 6), canopy tiles (Task 7).
- Produces (pure functions in `vegetation_check.py`):

```python
def species_gate(cells: dict, mix: dict, codes_at) -> dict          # per code: chi2 p-value of type counts vs weights; pass if every p > 0.001
def canopy_gate(cells: dict, cover_at, cell_area_m2, crown_m: dict) -> dict  # cells with mean cover in [0.1, 0.5], >= 50; median |err| <= 0.10
def far_determinism_gate(far_a: dict, far_b: dict, near: dict) -> dict      # same hashes on common blocks; every far tree in its cell's near dump
def handoff_gate(events: list[dict]) -> dict                         # no destroy before the other tier settled (spec rule)
def r4_identity_gate(cells: dict, r4_cells: dict) -> dict            # equal "hash" on every common cell
def placement_gate(cells: dict, mix: dict, code_at, canopy_at) -> dict  # trees on tree codes (+ cover > 0, h >= 3 with canopy), shrubs on shrub codes
```
  New stages: `species`, `canopy`, `far`, `handoff`, `long_range`, `r4_identity` (CLI `--r4-baseline DIR` for the R4 dumps, default `.cache/veg/check-i1-r2`). `long_range` runs `run_bench.py --site pendleton --phases long_range`.

- [ ] **Step 1: Failing pytest** for every pure gate with small synthetic inputs (a pass and a fail case each; for `handoff_gate`: events `tier far`, `destroy near` before any `spawn far` → fail; with `spawn far` first → pass; with `tier none` → destroy allowed).
- [ ] **Step 2: Implement the gates** (`scipy` is not a dependency: compute the χ² p-value with the regularised upper gamma from `math` — `p = gammaincc(k/2, x/2)`; implement `gammaincc` by the series/continued fraction in ~25 lines, or use the Wilson–Hilferty normal approximation `z = ((x/k)^(1/3) − (1 − 2/(9k))) / sqrt(2/(9k))`, `p = 0.5 erfc(z/√2)`, which is accurate enough at p = 0.001). Wire the stages into `main()` the way `footprints_gate` is wired (gates of stages not run are read back from `report.json`).
- [ ] **Step 3: Bench.** `scenario.py`: `_long_range(t)`: 5,000 m HAE, 100 m/s on `DENSE`'s bearing from the riparian pose (33.307, −117.354), gimbal −15°, FOV 30°; `Phase("long_range", 90.0, _long_range)` appended for coast sites (and visited in the warm-up: insert before `far` with 30 s). `sweep.py` `AXES`: `"far_radius": ("CAMSIM_VEGETATION_FAR_RADIUS_M", "trees")`, `"far_keep": ("CAMSIM_VEGETATION_FAR_KEEP", "trees")`, `"shrubs": ("CAMSIM_VEGETATION_SHRUBS_ENABLED", "trees")`, `"shrub_radius": ("CAMSIM_VEGETATION_SHRUB_RADIUS_M", "trees")`.
- [ ] **Step 4: Run** the pytest file and `python3 scripts/vegetation_check.py --help` / `scripts/bench/sweep.py --help`.
- [ ] **Step 5: Commit** — `test(vegetation): species, canopy, far, handoff, long-range and R4-identity gates; bench long_range phase`

---

### Task 14: Pendleton acceptance, bench sweep, docs

**Files:**
- Modify: `docs/vegetation.md`, `docs/scene-packages.md`, `docs/configuration.md`, `REALISM.md`, `ROADMAP.md`, `CLAUDE.md`, `FOR_LINUX.md`
- Data: `.cache/scene-packages/pendleton-r5veg` (not committed)

- [ ] **Step 1: Build `pendleton-r5veg`**: the `pendleton-r4` scene file + `[canopy] enabled = true` + a Pendleton `[vegetation]` table:

```toml
[vegetation]
mix.10 = { tree_broadleaf_a = 1.0 }
mix.20 = { shrub_chaparral = 0.8, shrub_desert = 0.2 }
mix.30 = { shrub_chaparral = 0.5, shrub_desert = 0.5, tree_broadleaf_a = 0.05 }
shrub_density.20 = 8.0
shrub_density.30 = 1.0
```
  `plan`, `fetch`, `build`, `verify` (all checks ok).
- [ ] **Step 2: Acceptance**: `caffeinate -ims uv run --project scripts/scene --with pycocotools python scripts/vegetation_check.py --scene "$PWD/.cache/scene-packages/pendleton-r5veg" --baseline .cache/veg/baseline --out .cache/veg/check-r5 --stages approach,off,gt,truck,oblique,footprints,species,canopy,far,handoff,long_range` and `--stages r4_identity --scene "$PWD/.cache/scene-packages/pendleton-r2"` with `CAMSIM_VEGETATION_SHRUBS_ENABLED=0 CAMSIM_VEGETATION_FAR_RADIUS_M=0`. Every gate passes; on a failure, debug (superpowers:systematic-debugging) before changing a threshold, and never change one without telling the user.
- [ ] **Step 3: Eglin-style shot** with a test mix file (`vegetation.mix_file`, 10 → 85 % pines) on the same package, for the docs (pines in the riparian pose).
- [ ] **Step 4: Bench sweep**: `uv run --with numpy --with pillow python scripts/bench/sweep.py --scene "$PKG" --out .cache/bench-r5veg --axis buildings=on --axis trees=off,on --axis far_radius=0,2000,4000,6000 --axis far_keep=0.25,0.35,0.5 --axis shrubs=off,on --axis shrub_radius=300,500,1000 --repeat 2 --phases dense_pass,long_range` (prune rows if the run would exceed ~6 h: drop `far_keep` to the 0.35 column for `far_radius` 2000/6000). Table with budget pass/fail per row in `docs/vegetation.md` "Bench".
- [ ] **Step 5: Ask the user to choose the defaults** from the table (AskUserQuestion with the 2–3 rows inside budget). Apply the choice to `CamSimConfig.h`, YAML, docs, config tests.
- [ ] **Step 6: Docs.** `docs/vegetation.md`: rewrite "How it works" for library v2, mix, scatter v2 (streams), canopy, blocks/far tier, events/dumps; "Configuration" table; site mix examples (Pendleton above; Eglin `mix.10 = { tree_pine_a = 0.5, tree_pine_b = 0.35, tree_broadleaf_a = 0.15 }`, `mix.30 = { shrub_chaparral = 1.0 }`, `shrub_density.30 = 2.0`, `mix.90 = { shrub_chaparral = 1.0 }`, `shrub_density.90 = 6.0`; Pax River `mix.10 = { tree_broadleaf_a = 0.6, tree_pine_a = 0.4 }`, `mix.90 = { shrub_chaparral = 1.0 }`, `shrub_density.90 = 8.0`; Yuma `mix.20 = { shrub_desert = 0.85, cactus_a = 0.15 }` (if `cactus_a` exists), `mix.30 = { shrub_desert = 1.0 }`, `mix.60 = { shrub_desert = 1.0 }`, `shrub_density.20 = 1.5`, `shrub_density.30 = 0.8`, `shrub_density.60 = 0.3`); "Acceptance" table with the new gates; "Measured"; "Known limits" (spec's list). `REALISM.md` R4: done vs still deferred. `ROADMAP.md`: the Realism-track line. `CLAUDE.md`: the Vegetation gotcha (streams 80–86, blocks, mix, canopy, far tier, CVars), the Commands rows for `vegetation_check.py` stages, the test count (`grep -rE 'IMPLEMENT_[A-Z_]*AUTOMATION_TEST\(' unreal_project/CamSimTest/Source/CamSimTest/Tests | wc -l`, files by `grep -rl`). `FOR_LINUX.md`: the acceptance run to repeat on Linux.
- [ ] **Step 7: Full test run** (`FILTER=CamSim`, all scene tests, the gate pytest) and commit — `docs(realism): R4 vegetation deeper measured on Pendleton`.
