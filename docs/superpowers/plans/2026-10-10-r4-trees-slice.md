# Trees Thin Slice (REALISM R4-lite) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Scatter one CC0 tree species, deterministically, wherever the scene's land cover is tree cover (code 10), around the camera, at a measured cost.

**Architecture:** Pure, NullRHI-testable pieces (global cell grid, `GenerateCell` hash scatter, land-cover sampler, `library.json` parser, a streaming planner state machine) plus a thin game-thread manager that runs generation on UE tasks, grounds instances with Cesium's `SampleHeightMostDetailed` on the terrain tileset, and spawns one globe-anchored actor with one HISM per 100 m cell. No scene-package change, no shader change, no editor work (unless Task 1 finds the glTFRuntime material can't instance in a cooked build; then the contingency in Task 1 applies).

**Tech Stack:** UE 5.8 C++ (CamSimTest module), Cesium for Unreal 2.29 (`ACesium3DTileset::SampleHeightMostDetailed`, `UCesiumGlobeAnchorComponent`), glTFRuntime (`CamSimMeshLoader`), `UHierarchicalInstancedStaticMeshComponent`, UE Automation tests (NullRHI), FFmpeg libavutil SHA-256 (already linked), Python 3 stdlib + `npx gltfpack`/`obj2gltf` for asset prep.

**Spec:** `docs/superpowers/specs/2026-10-10-r4-trees-slice-design.md` (realism/cost bench: `docs/superpowers/specs/2026-10-10-r3r4-realism-cost-bench-design.md`, owned by the separate bench plan).

## Global Constraints

- Copyright header on every new source file: `// Copyright CamSim Contributors. All Rights Reserved.`; `#include "CoreMinimal.h"` first, `.generated.h` last; UE naming (`A`/`U`/`F`/`E`), verb-first functions.
- Tree asset: CC0-1.0, licence checked on the source page and recorded in `library.json` (`licence`, `source_url`, `sha256`); ≤ 10,000 triangles; broadleaf.
- `library.json` format string: `camsim-vegetation-1`. The loader refuses a sha256 mismatch.
- Cells: global geographic grid, `cell_size_m` default 100; id never depends on the camera.
- Candidate spacing nominal 6 m; `p_tree` = 0.15; yaw 0–360°; scale 0.8–1.2; accepted only on land-cover code 10.
- Hash: `CamSimHash::Hash` (PCG, `CamSimShaders/Public/SensorHash.h`), never a live RNG. Vegetation owns hash stream 80–82 (sub-streams 160–164).
- Seed: package manifest `seed` mixed with a vegetation constant; `vegetation.seed` only without a package.
- Heights: `SampleHeightMostDetailed` on the terrain tileset; failed samples drop that instance. No game-thread line traces for placement.
- Budgets (constants): at most `MaxSpawnsPerTick` = 2 cell actors per tick; at most `MaxCellsInFlight` = 4 cells in generate/height stages. Drop cells beyond `radius_m × 1.2`.
- Render: collision off, `bCastShadow = cast_shadows`, `InstanceEndCullDistance = radius_m` (hard cut), no custom depth/stencil.
- Trees are invisible to CIGI LOS/HAT/HOT and the DIS clamp (no collision). `/ready` does not wait for trees.
- Config keys (`vegetation.*`) and env vars exactly: `enabled`/`CAMSIM_VEGETATION_ENABLED` (true), `library_dir`/`CAMSIM_VEGETATION_LIBRARY_DIR` (""), `radius_m`/`CAMSIM_VEGETATION_RADIUS_M` (1000), `density_scale`/`CAMSIM_VEGETATION_DENSITY_SCALE` (1.0), `cast_shadows`/`CAMSIM_VEGETATION_CAST_SHADOWS` (true), `cell_size_m`/`CAMSIM_VEGETATION_CELL_SIZE_M` (100), `seed`/`CAMSIM_VEGETATION_SEED` (0).
- No land cover configured → vegetation off with one log line.
- `vegetation.enabled: false` → no manager is created; the frame matches the pre-slice build (gated by SSIM against a baseline, see Task 11).
- New docs: `docs/vegetation.md`; keep `docs/configuration.md`, `deploy/camsim_config.yaml`, `ROADMAP.md`, `REALISM.md`, `CLAUDE.md` (test counts, gotchas) current.

## Review Focus

1. **Teleport** (a CIGI pose jumps tens of km): every old cell must drop in one tick, results of in-flight work for dropped cells must not spawn actors at the old place, and no actor may leak. Pinned by `CamSim.Vegetation.Streaming.TeleportDropsAll` (Task 8) and `CamSim.Vegetation.Manager.StaleResultsIgnored` (Task 10).
2. **No land cover under the camera / missing tiles / LFS pointer tiles:** cells generate empty, nothing spawns, at most one warning line per summary, no per-tick spam. Pinned by `CamSim.Vegetation.LandCover.MissingTilesGiveZero` (Task 5) and `CamSim.Vegetation.Manager.EmptyCellsCostNothing` (Task 10).
3. **Library problems** (missing `.glb`, a git LFS pointer instead of the file, sha mismatch, malformed JSON): vegetation turns off with one warning; CamSim keeps running. Pinned by `CamSim.Vegetation.Library.*` (Task 6) and the manager's configure path (Task 10, `ConfigureFailsClosed`).
4. **Antimeridian and high latitude:** the grid must wrap at ±180° without gaps or duplicate cells, and refuse |lat| > 85° (vegetation off there, no division blow-ups). Pinned by `CamSim.Vegetation.Grid.Antimeridian` and `CamSim.Vegetation.Grid.PolarCutoff` (Task 3).
5. **All height samples fail** (terrain not loaded yet, sample errors): the cell must become empty instead of spawning trees at height 0 or staying in flight forever and starving the 4-slot budget. Pinned by `CamSim.Vegetation.Manager.FailedHeightsMakeEmpty` (Task 10).

---

## File Structure

New, under `unreal_project/CamSimTest/Source/CamSimTest/Vegetation/`:

| File | Responsibility |
|---|---|
| `VegetationGrid.h/.cpp` | `FVegCellId`, global cell grid math (cell of a point, bounds, centre, cells within a radius, ground distance). Pure. |
| `VegetationScatter.h/.cpp` | `FScatterParams`, `FTreeInstance`, `GenerateCell` (hash scatter), expected density. Pure. |
| `VegetationLandCover.h/.cpp` | `FVegetationLandCover`: thread-safe point code lookup over its own 16-tile `FLandCoverTileCache`. |
| `VegetationLibrary.h/.cpp` | `library.json` parse, dir resolution, sha256 verify (libavutil). |
| `ProceduralTree.h/.cpp` | Fallback trunk + crown geometry (pure) and `UStaticMesh` builder. Created only if Task 1 selects the fallback. |
| `VegetationStreaming.h/.cpp` | `FVegetationStreaming`: per-cell state machine, wanted/drop sets, budgets. Pure. |
| `VegetationCellActor.h/.cpp` | `ACamSimVegetationCell`: root + globe anchor + HISM. |
| `VegetationManager.h/.cpp` | `FVegetationManager`: glue (hooks for generate / heights / spawn / destroy), dump + ground-check CVars, probe command. |

Modified: `Config/CamSimConfig.h/.cpp` (`FVegetationConfig`, `Scene.Seed`), `Config/ScenePackage.cpp` (manifest seed), `Geospatial/CamSimGeospatialProvider.h/.cpp` (terrain tag), `Subsystem/CamSimSubsystem.h/.cpp` (own + tick manager, `GetTerrainTileset`), `CamSimShaders/Public/SensorHash.h` (stream id comment + constant), `CamSimTest.Build.cs` (`MeshDescription`, `StaticMeshDescription` only if fallback), `Config/DefaultGame.ini` (`/Engine/BasicShapes` only if fallback), `deploy/camsim_config.yaml`, `docs/configuration.md`, `docs/vegetation.md` (new), `ROADMAP.md`, `REALISM.md`, `CLAUDE.md`.

Assets: `unreal_project/CamSimTest/Content/NonUFS/Vegetation/{library.json,<tree>.glb}` (`.glb` is already git LFS; `NonUFS` is already staged by `+DirectoriesToAlwaysStageAsNonUFS=(Path="NonUFS")`).

Scripts: `scripts/vegetation/prepare_tree.py` (asset prep), `scripts/vegetation_check.py` (live acceptance).

Tests (new): `Tests/VegetationGridTest.cpp`, `Tests/VegetationScatterTest.cpp`, `Tests/VegetationLandCoverTest.cpp`, `Tests/VegetationLibraryTest.cpp`, `Tests/VegetationConfigTest.cpp`, `Tests/VegetationStreamingTest.cpp`, `Tests/VegetationManagerTest.cpp`, `Tests/ProceduralTreeTest.cpp` (fallback only). Modified: `Tests/ScenePackageTest.cpp`.

## Commands used throughout

Build (macOS, editor target):

```bash
scripts/run.sh --build-only
```

Headless automation (macOS needs `-DisablePython`); replace the filter as each task says:

```bash
UE_BIN="/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"
"$UE_BIN" "$PWD/unreal_project/CamSimTest/CamSimTest.uproject" \
  -ExecCmds="Automation RunTests CamSim.Vegetation+Quit" \
  -TestExit="Automation Test Queue Empty" \
  -ReportExportPath="$PWD/.cache/automation-report" \
  -unattended -nullrhi -nosound -nosplash -DisablePython -log -stdout -FullStdOutLogOutput > .cache/automation-veg.log 2>&1
python3 scripts/parse_automation_report.py .cache/automation-report/index.json
```

(On Linux: `UE_BIN=/opt/UE/Engine/Binaries/Linux/UnrealEditor`, drop `-DisablePython`.) `parse_automation_report.py` exits 0 when every test passed, 1 on any failure, 2 if the report is missing.

The Pendleton package used by live steps: `PKG="$PWD/.cache/scene-packages/pendleton-r2"` (the R2 package; absolute path).

---

### Task 1: Tree asset risk check, library folder, cell actor and probe

The spec's first-task risk: does a CC0 tree load through glTFRuntime into an HISM and render correctly, including in a cooked build? Research done while writing this plan (2026-10-10): **Poly Haven has no tree under 10k triangles** (API `https://api.polyhaven.com/assets?type=models&categories=nature`: the smallest trees are `quiver_tree_02` 153,887 and `jacaranda_tree` 312,356 polys; `fir_tree_01` 7.8 M). Candidates, in order:

| # | Candidate | Licence page | Notes |
|---|---|---|---|
| A | Quaternius "Textured LowPoly Trees" (45 trees) | https://quaternius.itch.io/textured-lowpoly-trees (CC0 1.0 stated) | OBJ/FBX/Blend → convert OBJ with `obj2gltf`; stylised but low-poly and textured; pick a round broadleaf one |
| B | QGIS 3D model hub "Tree Leaf Big" (Klas Karlsson) | https://hub.qgis.org/wavefronts/10/ (CC-0) | low-poly, small texture, possibly oak; format to check on download |
| C | Poly Haven `jacaranda_tree` | https://polyhaven.com/a/jacaranda_tree, licence https://polyhaven.com/license (CC0) | 312k → ≤ 10k with `gltfpack -si`; leaf cards degrade heavily — last resort before procedural |
| D | Procedural trunk + crown (Task 2) | n/a (generated) | always available |

Pass criteria for a candidate (all must hold): CC0 on the source page; ≤ 10,000 triangles after prep; reads as a broadleaf tree at 300 m oblique; loads through `CamSimMeshLoader::LoadStaticMesh` into an HISM in a live run; every material on the mesh reports `GetUsageByFlag(MATUSAGE_InstancedStaticMeshes) == true` **before** first render (in the editor a missing usage is silently added and recompiled, but a cooked build falls back to the default material, so the flag must already be set).

**Files:**
- Create: `scripts/vegetation/prepare_tree.py`
- Create: `unreal_project/CamSimTest/Content/NonUFS/Vegetation/library.json`, `.../Vegetation/<name>.glb`
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Vegetation/VegetationCellActor.h/.cpp`
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Vegetation/VegetationProbe.cpp` (console command; kept as a debug tool)
- Create: `docs/vegetation.md` (Asset section)

**Interfaces:**
- Produces: `ACamSimVegetationCell::Init(double CentreLatDeg, double CentreLonDeg, double BaseHeightM, UStaticMesh* Mesh, const TArray<FTransform>& LocalInstances, bool bCastShadows, float CullDistanceM)`, `int32 ACamSimVegetationCell::GetInstanceCount() const`; the committed `library.json` with one type; console command `camsim.Vegetation.Probe <abs path to .glb>`.

- [ ] **Step 0: Capture the pre-slice baseline (needed by Task 11's off-switch gate)**

Before any code change, with the current build:

```bash
scripts/run.sh --build-only
mkdir -p .cache/veg/baseline
for RUN in a b; do
  CAMSIM_SCENE_DIR="$PKG" python3 scripts/bench/run_bench.py --label veg-baseline-$RUN --site pendleton --smoke \
    --out .cache/veg/baseline/$RUN
done
```

Expected: two runs, each with `shots/*.png`. Their pairwise SSIM is the run-to-run floor Task 11 compares against. (`--smoke` runs a 20 s orbit plus the smoke shot list.)

- [ ] **Step 1: Write `scripts/vegetation/prepare_tree.py`**

Stdlib only. Converts (OBJ via obj2gltf), simplifies (gltfpack, no quantisation or compression, which glTFRuntime may not decode), rescales to a target height by wrapping the scene roots in a scale node, and prints the `library.json` entry.

```python
#!/usr/bin/env python3
# Copyright CamSim Contributors. All Rights Reserved.
"""Prepare a CC0 tree model for the vegetation library (REALISM R4 slice, docs/vegetation.md).

  prepare_tree.py SRC --name tree_broadleaf_a --height-m 9.0 --max-tris 10000 \
      --licence CC0-1.0 --source-url URL --out unreal_project/CamSimTest/Content/NonUFS/Vegetation

SRC is .obj, .gltf or .glb. Steps: OBJ -> glTF (npx obj2gltf), simplify to --max-tris (npx gltfpack -si, -noq, no
compression), wrap the scene's root nodes in one node scaled so the model's height (Y extent, glTF is Y-up) is
--height-m, write <out>/<name>.glb, print the library.json type entry (with sha256 and the final triangle count).
Fails if the final mesh has more than --max-tris triangles or uses non-identity node rotations/scales below the root.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

OBJ2GLTF = "obj2gltf@3.1.6"
GLTFPACK = "gltfpack@0.24.0"
GLB_MAGIC, JSON_CHUNK, BIN_CHUNK = 0x46546C67, 0x4E4F534A, 0x004E4942


def read_glb(path: Path) -> tuple[dict, bytes]:
    data = path.read_bytes()
    magic, _version, _length = struct.unpack_from("<III", data, 0)
    if magic != GLB_MAGIC:
        raise SystemExit(f"{path}: not a GLB")
    off, doc, binary = 12, None, b""
    while off < len(data):
        clen, ctype = struct.unpack_from("<II", data, off)
        chunk = data[off + 8 : off + 8 + clen]
        if ctype == JSON_CHUNK:
            doc = json.loads(chunk)
        elif ctype == BIN_CHUNK:
            binary = chunk
        off += 8 + clen
    if doc is None:
        raise SystemExit(f"{path}: GLB without a JSON chunk")
    return doc, binary


def write_glb(path: Path, doc: dict, binary: bytes) -> None:
    js = json.dumps(doc, separators=(",", ":"), sort_keys=True).encode()
    js += b" " * (-len(js) % 4)
    binary += b"\0" * (-len(binary) % 4)
    total = 12 + 8 + len(js) + (8 + len(binary) if binary else 0)
    out = struct.pack("<III", GLB_MAGIC, 2, total) + struct.pack("<II", len(js), JSON_CHUNK) + js
    if binary:
        out += struct.pack("<II", len(binary), BIN_CHUNK) + binary
    path.write_bytes(out)


def triangle_count(doc: dict) -> int:
    n = 0
    for mesh in doc.get("meshes", []):
        for prim in mesh["primitives"]:
            if prim.get("mode", 4) != 4:
                raise SystemExit("only TRIANGLES primitives are supported")
            acc = prim["indices"] if "indices" in prim else prim["attributes"]["POSITION"]
            n += doc["accessors"][acc]["count"] // 3
    return n


def check_node_transforms(doc: dict) -> None:
    for i, node in enumerate(doc.get("nodes", [])):
        if "matrix" in node or node.get("scale", [1, 1, 1]) != [1, 1, 1] or node.get("rotation", [0, 0, 0, 1]) != [0, 0, 0, 1]:
            raise SystemExit(f"node {i} has a rotation/scale/matrix; flatten the model first (gltfpack does unless -kn)")


def y_extent(doc: dict) -> tuple[float, float]:
    lo, hi = float("inf"), float("-inf")
    for node in doc.get("nodes", []):
        if "mesh" not in node:
            continue
        ty = node.get("translation", [0, 0, 0])[1]
        for prim in doc["meshes"][node["mesh"]]["primitives"]:
            acc = doc["accessors"][prim["attributes"]["POSITION"]]
            lo, hi = min(lo, acc["min"][1] + ty), max(hi, acc["max"][1] + ty)
    if lo == float("inf"):
        raise SystemExit("no mesh nodes")
    return lo, hi


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("src", type=Path)
    ap.add_argument("--name", required=True)
    ap.add_argument("--height-m", type=float, required=True)
    ap.add_argument("--max-tris", type=int, default=10_000)
    ap.add_argument("--licence", required=True)
    ap.add_argument("--source-url", required=True)
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args()
    if a.licence != "CC0-1.0":
        raise SystemExit("the vegetation library ships CC0-1.0 assets only")
    with tempfile.TemporaryDirectory() as td:
        src = a.src
        if src.suffix.lower() == ".obj":
            gltf = Path(td) / "conv.glb"
            subprocess.run(["npx", "-y", OBJ2GLTF, "-i", str(src), "-o", str(gltf), "--binary"], check=True)
            src = gltf
        doc, _ = read_glb(src) if src.suffix.lower() == ".glb" else (json.loads(src.read_text()), b"")
        tris = triangle_count(doc)
        simplified = Path(td) / "simp.glb"
        ratio = min(1.0, a.max_tris / max(tris, 1))
        subprocess.run(["npx", "-y", GLTFPACK, "-i", str(src), "-o", str(simplified), "-noq", "-si", f"{ratio:.4f}"], check=True)
        doc, binary = read_glb(simplified)
        check_node_transforms(doc)
        tris = triangle_count(doc)
        if tris > a.max_tris:
            raise SystemExit(f"{tris} triangles after simplification > {a.max_tris}; try -sa or another model")
        lo, hi = y_extent(doc)
        scale = a.height_m / (hi - lo)
        scene = doc["scenes"][doc.get("scene", 0)]
        doc["nodes"].append({"name": "camsim_scale", "scale": [scale] * 3, "translation": [0, -lo * scale, 0], "children": scene["nodes"]})
        scene["nodes"] = [len(doc["nodes"]) - 1]
        a.out.mkdir(parents=True, exist_ok=True)
        dst = a.out / f"{a.name}.glb"
        write_glb(dst, doc, binary)
    sha = hashlib.sha256(dst.read_bytes()).hexdigest()
    print(json.dumps({"name": a.name, "gltf": dst.name, "height_m": a.height_m, "thermal_class": "tree_canopy",
                      "licence": a.licence, "source_url": a.source_url, "sha256": sha, "triangles": tris}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

(`triangles` is informational; the runtime parser ignores unknown keys.)

- [ ] **Step 2: Download candidate A, verify its licence, prepare it**

Open https://quaternius.itch.io/textured-lowpoly-trees, confirm the page states CC0 (screenshot or quote the line into `docs/vegetation.md`). Download into a fresh, empty directory (untrusted data rule), e.g. `.cache/veg/src/quaternius/`, pick one round broadleaf tree (OBJ + its MTL/texture), then:

```bash
python3 scripts/vegetation/prepare_tree.py .cache/veg/src/quaternius/<Tree>.obj \
  --name tree_broadleaf_a --height-m 9.0 --max-tris 10000 --licence CC0-1.0 \
  --source-url https://quaternius.itch.io/textured-lowpoly-trees \
  --out unreal_project/CamSimTest/Content/NonUFS/Vegetation
```

Expected: a JSON entry printed with `triangles` ≤ 10000. If it fails or the model isn't usable, repeat with candidate B, then C.

- [ ] **Step 3: Write `library.json`**

```json
{
  "format": "camsim-vegetation-1",
  "types": [
    {
      "name": "tree_broadleaf_a",
      "gltf": "tree_broadleaf_a.glb",
      "height_m": 9.0,
      "thermal_class": "tree_canopy",
      "licence": "CC0-1.0",
      "source_url": "https://quaternius.itch.io/textured-lowpoly-trees",
      "sha256": "<the sha256 printed by Step 2>"
    }
  ]
}
```

(The sha256 value is the one `prepare_tree.py` just printed; paste it verbatim.)

- [ ] **Step 4: Write the cell actor**

`Vegetation/VegetationCellActor.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VegetationCellActor.generated.h"

class UCesiumGlobeAnchorComponent;
class UHierarchicalInstancedStaticMeshComponent;
class UStaticMesh;

/**
 * One vegetation cell (REALISM R4 slice, docs/vegetation.md): a globe anchor at the cell centre, oriented East-South-Up,
 * so the actor's local frame is X = East, Y = South, Z = Up in cm and survives origin shifts; one HISM of trees in that
 * frame. No collision (CIGI and the DIS clamp never see trees), no custom depth.
 */
UCLASS(NotPlaceable, Transient)
class CAMSIMTEST_API ACamSimVegetationCell : public AActor
{
	GENERATED_BODY()

public:
	ACamSimVegetationCell();

	/** Game thread, once, right after SpawnActor. LocalInstances are in the cell's ESU frame (cm). */
	void Init(double CentreLatDeg, double CentreLonDeg, double BaseHeightM, UStaticMesh* Mesh,
		const TArray<FTransform>& LocalInstances, bool bCastShadows, float CullDistanceM);

	int32 GetInstanceCount() const;

private:
	UPROPERTY()
	TObjectPtr<USceneComponent> Root;

	UPROPERTY()
	TObjectPtr<UCesiumGlobeAnchorComponent> GlobeAnchor;

	UPROPERTY()
	TObjectPtr<UHierarchicalInstancedStaticMeshComponent> Trees;
};
```

`Vegetation/VegetationCellActor.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Vegetation/VegetationCellActor.h"

#include "CesiumGlobeAnchorComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"

ACamSimVegetationCell::ACamSimVegetationCell()
{
	PrimaryActorTick.bCanEverTick = false;
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);   // the globe anchor moves it on origin shifts
	SetRootComponent(Root);
	GlobeAnchor = CreateDefaultSubobject<UCesiumGlobeAnchorComponent>(TEXT("GlobeAnchor"));
	Trees = CreateDefaultSubobject<UHierarchicalInstancedStaticMeshComponent>(TEXT("Trees"));
	Trees->SetupAttachment(Root);
	Trees->SetMobility(EComponentMobility::Movable);
	Trees->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Trees->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	Trees->SetCanEverAffectNavigation(false);
	Trees->SetRenderCustomDepth(false);
}

void ACamSimVegetationCell::Init(double CentreLatDeg, double CentreLonDeg, double BaseHeightM, UStaticMesh* Mesh,
	const TArray<FTransform>& LocalInstances, bool bCastShadows, float CullDistanceM)
{
	GlobeAnchor->MoveToLongitudeLatitudeHeight(FVector(CentreLonDeg, CentreLatDeg, BaseHeightM));
	GlobeAnchor->SetEastSouthUpRotation(FQuat::Identity);
	Trees->SetCastShadow(bCastShadows);
	const int32 CullCm = FMath::Max(1, FMath::RoundToInt32(CullDistanceM * 100.0f));
	Trees->SetCullDistances(0, CullCm);
	Trees->SetStaticMesh(Mesh);
	Trees->AddInstances(LocalInstances, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/false, /*bUpdateNavigation=*/false);
}

int32 ACamSimVegetationCell::GetInstanceCount() const
{
	return Trees ? Trees->GetInstanceCount() : 0;
}
```

- [ ] **Step 5: Write the probe command**

`Vegetation/VegetationProbe.cpp` — spawns a 10 × 10 grid (15 m spacing) of the given mesh at the camera's nadir, grounded by one `SampleHeightMostDetailed`, and logs each material's instanced-mesh usage **before** the first render.

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Cesium3DTileset.h"
#include "CesiumSampleHeightResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Entity/EntityMeshLoader.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Camera/CamSimCamera.h"
#include "Subsystem/CamSimSubsystem.h"
#include "Vegetation/VegetationCellActor.h"
#include "CamSimTest.h"

namespace
{
	void LogInstancedUsage(UStaticMesh* Mesh)
	{
		for (const FStaticMaterial& M : Mesh->GetStaticMaterials())
		{
			const UMaterial* Base = M.MaterialInterface ? M.MaterialInterface->GetMaterial() : nullptr;
			UE_LOG(LogCamSim, Log, TEXT("Vegetation probe: material '%s' (base '%s') instanced usage = %d"),
				M.MaterialInterface ? *M.MaterialInterface->GetPathName() : TEXT("<none>"),
				Base ? *Base->GetPathName() : TEXT("<none>"),
				Base ? static_cast<int32>(Base->GetUsageByFlag(MATUSAGE_InstancedStaticMeshes)) : -1);
		}
	}

	void Probe(const TArray<FString>& Args, UWorld* World)
	{
		if (Args.Num() != 1 || !World) { UE_LOG(LogCamSim, Warning, TEXT("usage: camsim.Vegetation.Probe <abs .glb>")); return; }
		UStaticMesh* Mesh = CamSimMeshLoader::LoadStaticMesh(Args[0]);
		if (!Mesh) { UE_LOG(LogCamSim, Error, TEXT("Vegetation probe: could not load '%s'"), *Args[0]); return; }
		UE_LOG(LogCamSim, Log, TEXT("Vegetation probe: '%s' %d LODs, bounds %s"), *Args[0], Mesh->GetNumLODs(),
			*Mesh->GetBounds().BoxExtent.ToString());
		LogInstancedUsage(Mesh);
		UCamSimSubsystem* Sub = World->GetGameInstance() ? World->GetGameInstance()->GetSubsystem<UCamSimSubsystem>() : nullptr;
		ACamSimCamera* Cam = Sub ? Sub->GetCamera() : nullptr;
		ACesium3DTileset* Terrain = (Sub && Sub->GetCachedTilesets().Num() > 0) ? Sub->GetCachedTilesets()[0].Get() : nullptr; // Task 9 replaces this with Sub->GetTerrainTileset()
		if (!Cam || !Terrain) { UE_LOG(LogCamSim, Error, TEXT("Vegetation probe: no camera or terrain tileset")); return; }
		const double Lat = Cam->GetTelemetry().Latitude, Lon = Cam->GetTelemetry().Longitude;
		TWeakObjectPtr<UWorld> WeakWorld(World);
		TStrongObjectPtr<UStaticMesh> Keep(Mesh);
		Terrain->SampleHeightMostDetailed({ FVector(Lon, Lat, 0.0) }, FCesiumSampleHeightMostDetailedCallback::CreateLambda(
			[WeakWorld, Keep, Lat, Lon](ACesium3DTileset*, const TArray<FCesiumSampleHeightResult>& R, const TArray<FString>& W)
			{
				UWorld* Wd = WeakWorld.Get();
				if (!Wd || R.Num() != 1 || !R[0].SampleSuccess) { UE_LOG(LogCamSim, Error, TEXT("Vegetation probe: height sample failed")); return; }
				TArray<FTransform> Xf;
				for (int32 J = 0; J < 10; ++J)
					for (int32 I = 0; I < 10; ++I)
						Xf.Add(FTransform(FRotator(0.0, 37.0 * (I + 10 * J), 0.0), FVector((I - 4.5) * 1500.0, (J - 4.5) * 1500.0, 0.0)));
				ACamSimVegetationCell* Cell = Wd->SpawnActor<ACamSimVegetationCell>();
				Cell->Init(Lat, Lon, R[0].LongitudeLatitudeHeight.Z, Keep.Get(), Xf, true, 5000.0f);
				UE_LOG(LogCamSim, Log, TEXT("Vegetation probe: spawned %d instances at (%.6f, %.6f, %.1f m)"),
					Cell->GetInstanceCount(), Lat, Lon, R[0].LongitudeLatitudeHeight.Z);
			}));
	}

	FAutoConsoleCommandWithWorldAndArgs GVegetationProbe(TEXT("camsim.Vegetation.Probe"),
		TEXT("Spawn a 10x10 grid of a .glb at the camera nadir and log its materials' instanced usage (REALISM R4)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) { Probe(Args, World); }));
}
```

- [ ] **Step 6: Build**

Run: `scripts/run.sh --build-only`
Expected: build succeeds (exit 0).

- [ ] **Step 7: Live probe in the editor game mode**

Start CamSim on the Pendleton package with the camera over riparian woodland (Santa Margarita River near MCAS Camp Pendleton, 33.302 N, -117.355 W), 300 m above ground, 20° down, using the bench host to hold the pose:

```bash
GLB="$PWD/unreal_project/CamSimTest/Content/NonUFS/Vegetation/tree_broadleaf_a.glb"
CAMSIM_SCENE_DIR="$PKG" CAMSIM_MULTICAST_ADDR=127.0.0.1 CAMSIM_SNAPSHOT_ENDPOINT_ENABLED=1 \
  scripts/run.sh --headless --local --detach "-ExecCmds=camsim.Vegetation.Probe $GLB"
scripts/send_cigi_test.py --lat 33.302 --lon -117.355 --alt 350 --pitch -20 --duration 60 &
sleep 45; curl -s -o .cache/veg/probe.png http://127.0.0.1:8080/snapshot; scripts/stop.sh
grep "Vegetation probe" ~/Library/Logs/CamSimTest/CamSimTest.log
```

(`send_cigi_test.py` is a `uv run` script: `--lat/--lon/--alt/--pitch/--duration` hold one pose; altitudes are WGS-84 ellipsoid heights, ground there is ≈ 0 m HAE. The probe needs the camera when the command runs: if the log says "no camera", run the same command from the editor console (`scripts/run.sh` without `--headless`) once the view is up.)

Expected log: `instanced usage = 1` for every material, `spawned 100 instances`. Expected `probe.png`: trees standing on the ground, textured (not the grey-checker default material).

- [ ] **Step 8: Cooked check of the material usage**

The editor may hide a missing usage flag. Check in a packaged build:

```bash
scripts/package_for_docker.sh   # stages under .cache/staging; Linux target
```

On macOS, if packaging the Linux target isn't available, run a Mac cook instead: `"$UE_ROOT/Engine/Build/BatchFiles/RunUAT.sh" BuildCookRun -project="$PWD/unreal_project/CamSimTest/CamSimTest.uproject" -platform=Mac -clientconfig=Development -cook -stage -stagingdirectory="$PWD/.cache/staging-mac" -build -unattended` and launch the staged app with the same `-ExecCmds`. Expected: same log line (`instanced usage = 1`) and textured trees in the snapshot.

- [ ] **Step 9: Decide and record**

Write the "Asset" section of `docs/vegetation.md`: candidate chosen, licence quote + URL, triangle count, sha256, probe screenshot path, the usage-flag results (editor and cooked). Outcomes:
- **Pass:** keep the `.glb` and `library.json`; skip Task 2.
- **Usage flag false in the cooked build** (glTFRuntime base material lacks instanced usage): record it in `ROADMAP.md` under "Editor changes needed (human)" as human work (`ROADMAP.md` has no such section yet: create it as a new `## Editor changes needed (human)` section directly before `## Later / only when there's demand`, per `CLAUDE.md`, with one bullet per item, dated): duplicate glTFRuntime's opaque and masked base materials into `/Game/Vegetation/` with "Used with Instanced Static Meshes" ticked, add `+DirectoriesToAlwaysCook=(Path="/Game/Vegetation")`; then do Task 2 now so the slice isn't blocked, with the procedural tree as the shipped type until the human work lands.
- **No candidate passes otherwise:** do Task 2 and ship the procedural type.

- [ ] **Step 10: Commit**

```bash
git add scripts/vegetation/prepare_tree.py unreal_project/CamSimTest/Content/NonUFS/Vegetation \
  unreal_project/CamSimTest/Source/CamSimTest/Vegetation/VegetationCellActor.* \
  unreal_project/CamSimTest/Source/CamSimTest/Vegetation/VegetationProbe.cpp docs/vegetation.md ROADMAP.md
git commit -m "feat(vegetation): CC0 tree asset, cell actor and probe command (REALISM R4 slice)"
```

---

### Task 2 (only if Task 1 selected the fallback): Procedural tree

**Files:**
- Create: `Vegetation/ProceduralTree.h/.cpp`
- Modify: `CamSimTest.Build.cs` (add `"MeshDescription", "StaticMeshDescription"` to `PublicDependencyModuleNames`)
- Modify: `unreal_project/CamSimTest/Config/DefaultGame.ini` (add `+DirectoriesToAlwaysCook=(Path="/Engine/BasicShapes")`)
- Modify: `unreal_project/CamSimTest/Content/NonUFS/Vegetation/library.json` (procedural entry)
- Test: `Tests/ProceduralTreeTest.cpp`

**Interfaces:**
- Produces: `struct FProceduralTreeGeometry { TArray<FVector3f> Positions; TArray<FVector3f> Normals; TArray<uint32> Indices; TArray<int32> TriangleSection; }` (section 0 trunk, 1 crown), `FProceduralTreeGeometry CamSimVegetation::BuildProceduralTreeGeometry(float HeightM)`, `UStaticMesh* CamSimVegetation::CreateProceduralTreeMesh(float HeightM)`.

- [ ] **Step 1: Write the failing test**

`Tests/ProceduralTreeTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Vegetation/ProceduralTree.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProceduralTreeGeometryTest, "CamSim.Vegetation.ProceduralTree.Geometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FProceduralTreeGeometryTest::RunTest(const FString& Parameters)
{
	const FProceduralTreeGeometry G = CamSimVegetation::BuildProceduralTreeGeometry(9.0f);
	const int32 Tris = G.Indices.Num() / 3;
	TestTrue(TEXT("triangles <= 10k"), Tris > 0 && Tris <= 10000);
	TestEqual(TEXT("index count multiple of 3"), G.Indices.Num() % 3, 0);
	TestEqual(TEXT("one section per triangle"), G.TriangleSection.Num(), Tris);
	TestEqual(TEXT("normals per vertex"), G.Normals.Num(), G.Positions.Num());
	float MinZ = TNumericLimits<float>::Max(), MaxZ = -MinZ;
	for (const FVector3f& P : G.Positions) { MinZ = FMath::Min(MinZ, P.Z); MaxZ = FMath::Max(MaxZ, P.Z); }
	TestEqual(TEXT("base at 0 cm"), MinZ, 0.0f, 0.5f);
	TestEqual(TEXT("top at 900 cm"), MaxZ, 900.0f, 0.5f);
	for (uint32 I : G.Indices) { if (I >= static_cast<uint32>(G.Positions.Num())) { AddError(TEXT("index out of range")); break; } }
	bool bHasTrunk = false, bHasCrown = false;
	for (int32 S : G.TriangleSection) { bHasTrunk |= S == 0; bHasCrown |= S == 1; }
	TestTrue(TEXT("trunk and crown sections"), bHasTrunk && bHasCrown);
	return true;
}
```

- [ ] **Step 2: Run it to see it fail**

Run the automation command with filter `CamSim.Vegetation.ProceduralTree` after `scripts/run.sh --build-only`.
Expected: build FAILS (`Vegetation/ProceduralTree.h` not found).

- [ ] **Step 3: Implement**

`Vegetation/ProceduralTree.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UStaticMesh;

/** Fallback tree (REALISM R4 slice): an 8-sided trunk to 40 % of the height and an ellipsoid crown, cm, base at z = 0. */
struct FProceduralTreeGeometry
{
	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;
	TArray<uint32> Indices;
	TArray<int32> TriangleSection;   // 0 trunk, 1 crown
};

namespace CamSimVegetation
{
	CAMSIMTEST_API FProceduralTreeGeometry BuildProceduralTreeGeometry(float HeightM);
	/** Game thread. Two material slots: trunk brown, crown green (BasicShapeMaterial MIDs). Null on failure. */
	CAMSIMTEST_API UStaticMesh* CreateProceduralTreeMesh(float HeightM);
}
```

`Vegetation/ProceduralTree.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Vegetation/ProceduralTree.h"

#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"

namespace CamSimVegetation
{
	FProceduralTreeGeometry BuildProceduralTreeGeometry(float HeightM)
	{
		FProceduralTreeGeometry G;
		const float H = HeightM * 100.0f;
		auto AddTri = [&G](uint32 A, uint32 B, uint32 C, int32 Section) { G.Indices.Append({ A, B, C }); G.TriangleSection.Add(Section); };

		// Trunk: 8 sides, radius 25 cm, z 0 .. 0.4 H (flat-shaded quads: 4 vertices each).
		constexpr int32 Sides = 8;
		const float R = 25.0f, Top = 0.4f * H;
		for (int32 S = 0; S < Sides; ++S)
		{
			const float A0 = 2.0f * PI * S / Sides, A1 = 2.0f * PI * (S + 1) / Sides;
			const FVector3f N(FMath::Cos(0.5f * (A0 + A1)), FMath::Sin(0.5f * (A0 + A1)), 0.0f);
			const uint32 B = G.Positions.Num();
			G.Positions.Append({ FVector3f(R * FMath::Cos(A0), R * FMath::Sin(A0), 0.0f), FVector3f(R * FMath::Cos(A1), R * FMath::Sin(A1), 0.0f),
			                     FVector3f(R * FMath::Cos(A1), R * FMath::Sin(A1), Top),  FVector3f(R * FMath::Cos(A0), R * FMath::Sin(A0), Top) });
			G.Normals.Append({ N, N, N, N });
			AddTri(B, B + 2, B + 1, 0);
			AddTri(B, B + 3, B + 2, 0);
		}

		// Crown: UV ellipsoid, 12 longitudes x 8 latitudes, centre 0.62 H, radii 0.3 H (horizontal), 0.38 H (vertical),
		// so its top is at exactly H.
		constexpr int32 Lon = 12, Lat = 8;
		const float Rx = 0.3f * H, Rz = 0.38f * H, Cz = H - Rz;
		const uint32 C0 = G.Positions.Num();
		for (int32 J = 0; J <= Lat; ++J)
		{
			const float Phi = PI * J / Lat;   // 0 = top
			for (int32 I = 0; I <= Lon; ++I)
			{
				const float Th = 2.0f * PI * I / Lon;
				const FVector3f U(FMath::Sin(Phi) * FMath::Cos(Th), FMath::Sin(Phi) * FMath::Sin(Th), FMath::Cos(Phi));
				G.Positions.Add(FVector3f(Rx * U.X, Rx * U.Y, Cz + Rz * U.Z));
				G.Normals.Add(FVector3f(U.X / Rx, U.Y / Rx, U.Z / Rz).GetSafeNormal());
			}
		}
		for (int32 J = 0; J < Lat; ++J)
			for (int32 I = 0; I < Lon; ++I)
			{
				const uint32 A = C0 + J * (Lon + 1) + I, B = A + Lon + 1;
				if (J > 0)       AddTri(A, A + 1, B, 1);
				if (J < Lat - 1) AddTri(A + 1, B + 1, B, 1);
			}
		return G;
	}

	UStaticMesh* CreateProceduralTreeMesh(float HeightM)
	{
		const FProceduralTreeGeometry G = BuildProceduralTreeGeometry(HeightM);
		FMeshDescription MD;
		FStaticMeshAttributes Attr(MD);
		Attr.Register();
		const FPolygonGroupID Groups[2] = { MD.CreatePolygonGroup(), MD.CreatePolygonGroup() };
		Attr.GetPolygonGroupMaterialSlotNames()[Groups[0]] = TEXT("Trunk");
		Attr.GetPolygonGroupMaterialSlotNames()[Groups[1]] = TEXT("Crown");
		TArray<FVertexID> V;
		for (const FVector3f& P : G.Positions) { const FVertexID Id = MD.CreateVertex(); Attr.GetVertexPositions()[Id] = P; V.Add(Id); }
		for (int32 T = 0; T < G.TriangleSection.Num(); ++T)
		{
			FVertexInstanceID Inst[3];
			for (int32 K = 0; K < 3; ++K)
			{
				const uint32 Idx = G.Indices[3 * T + K];
				Inst[K] = MD.CreateVertexInstance(V[Idx]);
				Attr.GetVertexInstanceNormals()[Inst[K]] = G.Normals[Idx];
			}
			MD.CreateTriangle(Groups[G.TriangleSection[T]], Inst);
		}
		UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
		UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
		UMaterialInstanceDynamic* Trunk = Base ? UMaterialInstanceDynamic::Create(Base, Mesh) : nullptr;
		UMaterialInstanceDynamic* Crown = Base ? UMaterialInstanceDynamic::Create(Base, Mesh) : nullptr;
		if (Trunk) Trunk->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.10f, 0.07f, 0.04f));
		if (Crown) Crown->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.06f, 0.12f, 0.04f));
		Mesh->GetStaticMaterials().Add(FStaticMaterial(Trunk, TEXT("Trunk")));
		Mesh->GetStaticMaterials().Add(FStaticMaterial(Crown, TEXT("Crown")));
		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bBuildSimpleCollision = false;
		Params.bFastBuild = true;
		Mesh->BuildFromMeshDescriptions({ &MD }, Params);
		return Mesh;
	}
}
```

Add `"MeshDescription", "StaticMeshDescription",` to `PublicDependencyModuleNames` in `CamSimTest.Build.cs` (after `"ProceduralMeshComponent",`), and `+DirectoriesToAlwaysCook=(Path="/Engine/BasicShapes")` under the other `DirectoriesToAlwaysCook` lines in `Config/DefaultGame.ini`.

- [ ] **Step 4: Run the test**

Build, run filter `CamSim.Vegetation.ProceduralTree`. Expected: PASS.

- [ ] **Step 5: Probe it live**

Temporarily extend the probe: if the argument is the literal `procedural`, use `CamSimVegetation::CreateProceduralTreeMesh(9.0f)` instead of `LoadStaticMesh` (keep this branch; it's the fallback's debug path). Repeat Task 1 Steps 7–8 with `camsim.Vegetation.Probe procedural`. Expected: `instanced usage = 1` (BasicShapeMaterial), brown/green trees in the snapshot, also in the cooked build.

- [ ] **Step 6: Library entry**

Replace (or, if the glTF tree only lacks the cooked usage flag, add before it so it is type 0 until the editor work lands) the type with:

```json
{ "name": "tree_procedural", "procedural": true, "height_m": 9.0, "thermal_class": "tree_canopy", "licence": "CC0-1.0", "source_url": "generated" }
```

- [ ] **Step 7: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Vegetation/ProceduralTree.* \
  unreal_project/CamSimTest/Source/CamSimTest/Vegetation/VegetationProbe.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/ProceduralTreeTest.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/CamSimTest.Build.cs unreal_project/CamSimTest/Config/DefaultGame.ini \
  unreal_project/CamSimTest/Content/NonUFS/Vegetation/library.json
git commit -m "feat(vegetation): procedural fallback tree (REALISM R4 slice)"
```

---

### Task 3: Global cell grid

**Files:**
- Create: `Vegetation/VegetationGrid.h/.cpp`
- Test: `Tests/VegetationGridTest.cpp`

**Interfaces:**
- Produces (namespace `CamSimVegetation`): `struct FVegCellId { int32 Row = 0; int32 Col = 0; }` with `==` and `GetTypeHash`; `struct FVegCellBounds { double SouthDeg, NorthDeg, WestDeg, EastDeg; }`; constants `MetresPerDegLat = 111320.0`, `MaxLatDeg = 85.0`; `bool IsLatAllowed(double LatDeg)`; `double CellDLatDeg(double CellM)`; `double CellDLonDeg(int32 Row, double CellM)`; `int32 NumCols(int32 Row, double CellM)`; `FVegCellId CellOf(double LatDeg, double LonDeg, double CellM)`; `FVegCellBounds CellBounds(FVegCellId Id, double CellM)`; `void CellCentre(FVegCellId Id, double CellM, double& OutLatDeg, double& OutLonDeg)`; `double GroundDistanceM(double Lat1, double Lon1, double Lat2, double Lon2)`; `TArray<FVegCellId> CellsWithin(double LatDeg, double LonDeg, double RadiusM, double CellM)` (sorted nearest first, ties by Row then Col; empty when `!IsLatAllowed(LatDeg)`).

- [ ] **Step 1: Write the failing tests**

`Tests/VegetationGridTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Vegetation/VegetationGrid.h"

using namespace CamSimVegetation;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegGridRoundTripTest, "CamSim.Vegetation.Grid.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegGridRoundTripTest::RunTest(const FString& Parameters)
{
	const double Pts[][2] = { { 33.302, -117.355 }, { 0.0, 0.0 }, { -45.5, 170.25 }, { 84.9, -10.0 }, { 37.7749, -122.4194 } };
	for (const auto& P : Pts)
	{
		const FVegCellId C = CellOf(P[0], P[1], 100.0);
		const FVegCellBounds B = CellBounds(C, 100.0);
		TestTrue(*FString::Printf(TEXT("(%f,%f) inside its cell"), P[0], P[1]),
			P[0] >= B.SouthDeg && P[0] < B.NorthDeg && P[1] >= B.WestDeg && P[1] < B.EastDeg);
		double CLat, CLon; CellCentre(C, 100.0, CLat, CLon);
		TestTrue(TEXT("centre maps to the same cell"), CellOf(CLat, CLon, 100.0) == C);
		TestTrue(TEXT("cell is ~100 m N-S"), FMath::IsNearlyEqual((B.NorthDeg - B.SouthDeg) * MetresPerDegLat, 100.0, 1e-6));
		const double EwM = GroundDistanceM(CLat, B.WestDeg, CLat, B.EastDeg);
		TestTrue(*FString::Printf(TEXT("cell is ~100 m E-W (%f)"), EwM), FMath::Abs(EwM - 100.0) < 0.5);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegGridNeighboursTest, "CamSim.Vegetation.Grid.NoGapsAcrossRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegGridNeighboursTest::RunTest(const FString& Parameters)
{
	// Sample a 2 km square every 7 m: every sample belongs to exactly one cell whose bounds contain it (no gaps, no overlap).
	int32 Bad = 0;
	for (double N = -1000.0; N <= 1000.0; N += 7.0)
		for (double E = -1000.0; E <= 1000.0; E += 7.0)
		{
			const double Lat = 33.3 + N / MetresPerDegLat, Lon = -117.4 + E / (MetresPerDegLat * FMath::Cos(FMath::DegreesToRadians(33.3)));
			const FVegCellBounds B = CellBounds(CellOf(Lat, Lon, 100.0), 100.0);
			Bad += !(Lat >= B.SouthDeg && Lat < B.NorthDeg && Lon >= B.WestDeg && Lon < B.EastDeg);
		}
	TestEqual(TEXT("samples outside their cell"), Bad, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegGridAntimeridianTest, "CamSim.Vegetation.Grid.Antimeridian",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegGridAntimeridianTest::RunTest(const FString& Parameters)
{
	const FVegCellId East = CellOf(10.0, 179.9999, 100.0), West = CellOf(10.0, -179.9999, 100.0), Wrapped = CellOf(10.0, 180.0001, 100.0);
	TestEqual(TEXT("last column"), East.Col, NumCols(East.Row, 100.0) - 1);
	TestEqual(TEXT("first column"), West.Col, 0);
	TestTrue(TEXT("180.0001 wraps to -179.9999's cell"), Wrapped == West);
	const TArray<FVegCellId> Cells = CellsWithin(10.0, 179.9995, 500.0, 100.0);
	TSet<FVegCellId> Unique(Cells);
	TestEqual(TEXT("no duplicates across the seam"), Unique.Num(), Cells.Num());
	bool bBoth = false, bLow = false, bHigh = false;
	for (const FVegCellId& C : Cells) { bLow |= C.Col == 0; bHigh |= C.Col == NumCols(C.Row, 100.0) - 1; }
	bBoth = bLow && bHigh;
	TestTrue(TEXT("cells on both sides of the seam"), bBoth);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegGridPolarTest, "CamSim.Vegetation.Grid.PolarCutoff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegGridPolarTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("85.1 refused"), IsLatAllowed(85.1));
	TestTrue(TEXT("85.0 allowed"), IsLatAllowed(85.0));
	TestEqual(TEXT("no cells near the pole"), CellsWithin(89.0, 0.0, 1000.0, 100.0).Num(), 0);
	TestFalse(TEXT("NaN refused"), IsLatAllowed(TNumericLimits<double>::Quiet_NaN()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegGridWithinTest, "CamSim.Vegetation.Grid.CellsWithin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegGridWithinTest::RunTest(const FString& Parameters)
{
	const TArray<FVegCellId> Cells = CellsWithin(33.302, -117.355, 1000.0, 100.0);
	// pi * 10^2 ~ 314 cells of 100 m within 1 km (centre test).
	TestTrue(*FString::Printf(TEXT("about 314 cells (%d)"), Cells.Num()), Cells.Num() > 290 && Cells.Num() < 340);
	double Prev = -1.0;
	bool bSorted = true, bInside = true;
	for (const FVegCellId& C : Cells)
	{
		double Lat, Lon; CellCentre(C, 100.0, Lat, Lon);
		const double D = GroundDistanceM(33.302, -117.355, Lat, Lon);
		bSorted &= D >= Prev; Prev = D; bInside &= D <= 1000.0;
	}
	TestTrue(TEXT("nearest first"), bSorted);
	TestTrue(TEXT("all centres within the radius"), bInside);
	TestTrue(TEXT("first is the camera's cell"), Cells.Num() > 0 && Cells[0] == CellOf(33.302, -117.355, 100.0));
	TestTrue(TEXT("same list twice (deterministic)"), Cells == CellsWithin(33.302, -117.355, 1000.0, 100.0));
	return true;
}
```

- [ ] **Step 2: Run to see it fail**

Build: Expected FAIL — `Vegetation/VegetationGrid.h` not found.

- [ ] **Step 3: Implement**

`Vegetation/VegetationGrid.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Global vegetation cell grid (REALISM R4 slice, docs/vegetation.md). Rows of fixed angular height
 * CellM / 111,320 deg from 90 S; each row has columns of CellM / (111,320 cos(row centre)) deg from 180 W (the last
 * column of a row is narrower). A cell's id depends only on the position, never on the camera. Pure; any thread.
 */
namespace CamSimVegetation
{
	inline constexpr double MetresPerDegLat = 111320.0;
	inline constexpr double MaxLatDeg = 85.0;   // no vegetation closer to the poles

	struct FVegCellId
	{
		int32 Row = 0;
		int32 Col = 0;
		bool operator==(const FVegCellId&) const = default;
		friend uint32 GetTypeHash(const FVegCellId& C) { return HashCombine(::GetTypeHash(C.Row), ::GetTypeHash(C.Col)); }
	};

	struct FVegCellBounds
	{
		double SouthDeg = 0.0, NorthDeg = 0.0, WestDeg = 0.0, EastDeg = 0.0;
	};

	CAMSIMTEST_API bool IsLatAllowed(double LatDeg);
	CAMSIMTEST_API double CellDLatDeg(double CellM);
	CAMSIMTEST_API double CellDLonDeg(int32 Row, double CellM);
	CAMSIMTEST_API int32 NumCols(int32 Row, double CellM);
	CAMSIMTEST_API FVegCellId CellOf(double LatDeg, double LonDeg, double CellM);
	CAMSIMTEST_API FVegCellBounds CellBounds(FVegCellId Id, double CellM);
	CAMSIMTEST_API void CellCentre(FVegCellId Id, double CellM, double& OutLatDeg, double& OutLonDeg);
	/** Equirectangular ground distance (metres); the longitude difference wraps to (-180, 180]. Fine below ~10 km. */
	CAMSIMTEST_API double GroundDistanceM(double Lat1, double Lon1, double Lat2, double Lon2);
	/** Cells whose centre is within RadiusM of the point, nearest first (ties: Row, then Col). Empty past MaxLatDeg. */
	CAMSIMTEST_API TArray<FVegCellId> CellsWithin(double LatDeg, double LonDeg, double RadiusM, double CellM);
}
```

`Vegetation/VegetationGrid.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Vegetation/VegetationGrid.h"

namespace CamSimVegetation
{
	namespace
	{
		double WrapLon360(double LonDeg)   // [0, 360) measured from 180 W
		{
			double X = FMath::Fmod(LonDeg + 180.0, 360.0);
			return X < 0.0 ? X + 360.0 : X;
		}
		double RowCentreLatDeg(int32 Row, double CellM) { return -90.0 + (Row + 0.5) * CellDLatDeg(CellM); }
	}

	bool IsLatAllowed(double LatDeg) { return FMath::IsFinite(LatDeg) && FMath::Abs(LatDeg) <= MaxLatDeg; }

	double CellDLatDeg(double CellM) { return CellM / MetresPerDegLat; }

	double CellDLonDeg(int32 Row, double CellM)
	{
		const double C = FMath::Max(FMath::Cos(FMath::DegreesToRadians(RowCentreLatDeg(Row, CellM))), 0.01);
		return CellM / (MetresPerDegLat * C);
	}

	int32 NumCols(int32 Row, double CellM) { return FMath::CeilToInt32(360.0 / CellDLonDeg(Row, CellM)); }

	FVegCellId CellOf(double LatDeg, double LonDeg, double CellM)
	{
		FVegCellId Id;
		Id.Row = FMath::FloorToInt32((LatDeg + 90.0) / CellDLatDeg(CellM));
		Id.Col = FMath::Min(FMath::FloorToInt32(WrapLon360(LonDeg) / CellDLonDeg(Id.Row, CellM)), NumCols(Id.Row, CellM) - 1);
		return Id;
	}

	FVegCellBounds CellBounds(FVegCellId Id, double CellM)
	{
		const double DLat = CellDLatDeg(CellM), DLon = CellDLonDeg(Id.Row, CellM);
		FVegCellBounds B;
		B.SouthDeg = -90.0 + Id.Row * DLat;
		B.NorthDeg = B.SouthDeg + DLat;
		B.WestDeg = -180.0 + Id.Col * DLon;
		B.EastDeg = FMath::Min(B.WestDeg + DLon, 180.0);
		return B;
	}

	void CellCentre(FVegCellId Id, double CellM, double& OutLatDeg, double& OutLonDeg)
	{
		const FVegCellBounds B = CellBounds(Id, CellM);
		OutLatDeg = 0.5 * (B.SouthDeg + B.NorthDeg);
		OutLonDeg = 0.5 * (B.WestDeg + B.EastDeg);
	}

	double GroundDistanceM(double Lat1, double Lon1, double Lat2, double Lon2)
	{
		double DLon = FMath::Fmod(Lon2 - Lon1, 360.0);
		if (DLon > 180.0) DLon -= 360.0;
		if (DLon <= -180.0) DLon += 360.0;
		const double N = (Lat2 - Lat1) * MetresPerDegLat;
		const double E = DLon * MetresPerDegLat * FMath::Cos(FMath::DegreesToRadians(0.5 * (Lat1 + Lat2)));
		return FMath::Sqrt(N * N + E * E);
	}

	TArray<FVegCellId> CellsWithin(double LatDeg, double LonDeg, double RadiusM, double CellM)
	{
		TArray<FVegCellId> Out;
		if (!IsLatAllowed(LatDeg) || !(RadiusM > 0.0) || !(CellM > 0.0)) return Out;
		const double DLatR = RadiusM / MetresPerDegLat;
		const int32 Row0 = CellOf(LatDeg - DLatR, LonDeg, CellM).Row, Row1 = CellOf(LatDeg + DLatR, LonDeg, CellM).Row;
		TSet<FVegCellId> Seen;
		TArray<TPair<double, FVegCellId>> Hits;
		for (int32 Row = Row0; Row <= Row1; ++Row)
		{
			const double RowLat = RowCentreLatDeg(Row, CellM);
			if (!IsLatAllowed(RowLat)) continue;
			const double DLon = CellDLonDeg(Row, CellM);
			const int32 Cols = NumCols(Row, CellM);
			const double SpanDeg = RadiusM / (MetresPerDegLat * FMath::Max(FMath::Cos(FMath::DegreesToRadians(RowLat)), 0.01)) + DLon;
			const int32 C0 = FMath::FloorToInt32(WrapLon360(LonDeg - SpanDeg) / DLon);
			const int32 Count = FMath::Min(FMath::CeilToInt32(2.0 * SpanDeg / DLon) + 2, Cols);
			for (int32 K = 0; K < Count; ++K)
			{
				const FVegCellId Id{ Row, ((C0 + K) % Cols + Cols) % Cols };
				if (Seen.Contains(Id)) continue;
				Seen.Add(Id);
				double CLat, CLon; CellCentre(Id, CellM, CLat, CLon);
				const double D = GroundDistanceM(LatDeg, LonDeg, CLat, CLon);
				if (D <= RadiusM) Hits.Emplace(D, Id);
			}
		}
		Hits.Sort([](const TPair<double, FVegCellId>& A, const TPair<double, FVegCellId>& B)
		{
			if (A.Key != B.Key) return A.Key < B.Key;
			return A.Value.Row != B.Value.Row ? A.Value.Row < B.Value.Row : A.Value.Col < B.Value.Col;
		});
		for (const auto& H : Hits) Out.Add(H.Value);
		return Out;
	}
}
```

- [ ] **Step 4: Run tests**

Build; filter `CamSim.Vegetation.Grid`. Expected: 5 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Vegetation/VegetationGrid.* unreal_project/CamSimTest/Source/CamSimTest/Tests/VegetationGridTest.cpp
git commit -m "feat(vegetation): global cell grid (REALISM R4 slice)"
```

---

### Task 4: Deterministic scatter (`GenerateCell`)

**Files:**
- Create: `Vegetation/VegetationScatter.h/.cpp`
- Modify: `unreal_project/CamSimTest/Source/CamSimShaders/Public/SensorHash.h` (add the vegetation stream id; CPU only, no HLSL mirror needed)
- Test: `Tests/VegetationScatterTest.cpp`

**Interfaces:**
- Consumes: `FVegCellId`, `CellBounds` (Task 3); `CamSimHash::Hash`, `CamSimHash::Uniform`, `CamSimHash::Pcg`.
- Produces (namespace `CamSimVegetation`): `struct FScatterParams { double CellSizeM = 100.0; double SpacingM = 6.0; float PTree = 0.15f; float DensityScale = 1.0f; float ScaleMin = 0.8f; float ScaleMax = 1.2f; uint8 TreeCode = 10; }`; `struct FTreeInstance { double LatDeg; double LonDeg; float YawDeg; float Scale; uint8 Type; }` with `==`; `using FCodeSampler = TFunctionRef<uint8(double LatDeg, double LonDeg)>`; `int32 CandidatesPerAxis(const FScatterParams&)`; `double ExpectedTreesPerM2(const FScatterParams&)`; `uint32 VegetationSeed(uint32 BaseSeed)`; `TArray<FTreeInstance> GenerateCell(uint32 Seed, FVegCellId Cell, const FScatterParams& P, FCodeSampler Sampler)`; `uint32 HashInstances(const TArray<FTreeInstance>&)` (for dumps / determinism checks).

- [ ] **Step 1: Write the failing tests**

`Tests/VegetationScatterTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Vegetation/VegetationGrid.h"
#include "Vegetation/VegetationScatter.h"

using namespace CamSimVegetation;

namespace
{
	uint8 AllTrees(double, double) { return 10; }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegScatterDeterminismTest, "CamSim.Vegetation.Scatter.OrderIndependent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegScatterDeterminismTest::RunTest(const FString& Parameters)
{
	const FScatterParams P;
	const uint32 Seed = VegetationSeed(1234u);
	const FVegCellId Origin = CellOf(33.302, -117.355, P.CellSizeM);
	TArray<FVegCellId> Cells;
	for (int32 R = -2; R <= 2; ++R) for (int32 C = -2; C <= 2; ++C) Cells.Add({ Origin.Row + R, Origin.Col + C });

	TMap<FVegCellId, TArray<FTreeInstance>> Forward, Reverse, Shuffled;
	for (const FVegCellId& C : Cells) Forward.Add(C, GenerateCell(Seed, C, P, AllTrees));
	for (int32 I = Cells.Num() - 1; I >= 0; --I) Reverse.Add(Cells[I], GenerateCell(Seed, Cells[I], P, AllTrees));
	const int32 Order[] = { 7, 19, 3, 24, 0, 11, 15, 2, 22, 9, 4, 17, 13, 1, 20, 6, 23, 10, 16, 5, 21, 8, 14, 18, 12 };
	for (int32 I : Order) Shuffled.Add(Cells[I], GenerateCell(Seed, Cells[I], P, AllTrees));

	for (const FVegCellId& C : Cells)
	{
		TestTrue(TEXT("forward == reverse"), Forward[C] == Reverse[C]);
		TestTrue(TEXT("forward == shuffled"), Forward[C] == Shuffled[C]);
		TestEqual(TEXT("hash stable"), HashInstances(Forward[C]), HashInstances(Shuffled[C]));
	}
	TestFalse(TEXT("different seed, different trees"), GenerateCell(Seed + 1, Origin, P, AllTrees) == Forward[Origin]);
	TestFalse(TEXT("neighbour cells differ"), Forward[Cells[0]] == Forward[Cells[1]]);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegScatterDensityTest, "CamSim.Vegetation.Scatter.Density",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegScatterDensityTest::RunTest(const FString& Parameters)
{
	for (const float Density : { 0.25f, 0.5f, 1.0f })
	{
		FScatterParams P;
		P.DensityScale = Density;
		const uint32 Seed = VegetationSeed(7u);
		const FVegCellId Origin = CellOf(33.302, -117.355, P.CellSizeM);
		int64 OnTrees = 0, OnGrass = 0;
		double TreeAreaM2 = 0.0;
		for (int32 K = 0; K < 100; ++K)
		{
			const FVegCellId C{ Origin.Row + K / 10, Origin.Col + K % 10 };
			const FVegCellBounds B = CellBounds(C, P.CellSizeM);
			const double MidLon = 0.5 * (B.WestDeg + B.EastDeg);
			// West half tree cover (10), east half grassland (30).
			auto HalfHalf = [MidLon](double, double Lon) -> uint8 { return Lon < MidLon ? 10 : 30; };
			for (const FTreeInstance& T : GenerateCell(Seed, C, P, HalfHalf)) { (T.LonDeg < MidLon ? OnTrees : OnGrass)++; }
			const double LatC = 0.5 * (B.SouthDeg + B.NorthDeg);
			TreeAreaM2 += GroundDistanceM(B.SouthDeg, B.WestDeg, B.NorthDeg, B.WestDeg) * GroundDistanceM(LatC, B.WestDeg, LatC, MidLon);
		}
		const double Expected = ExpectedTreesPerM2(P) * TreeAreaM2;
		TestEqual(*FString::Printf(TEXT("density %.2f: none on grassland"), Density), OnGrass, int64(0));
		TestTrue(*FString::Printf(TEXT("density %.2f: %lld trees vs expected %.0f (+-10%%)"), Density, OnTrees, Expected),
			FMath::Abs(OnTrees - Expected) <= 0.10 * Expected);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegScatterRangesTest, "CamSim.Vegetation.Scatter.InsideCellAndRanges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegScatterRangesTest::RunTest(const FString& Parameters)
{
	const FScatterParams P;
	const FVegCellId C = CellOf(33.302, -117.355, P.CellSizeM);
	const FVegCellBounds B = CellBounds(C, P.CellSizeM);
	const TArray<FTreeInstance> T = GenerateCell(VegetationSeed(0u), C, P, AllTrees);
	TestTrue(TEXT("some trees"), T.Num() > 0);
	TestTrue(TEXT("at most one per candidate"), T.Num() <= CandidatesPerAxis(P) * CandidatesPerAxis(P));
	for (const FTreeInstance& I : T)
	{
		if (!(I.LatDeg > B.SouthDeg && I.LatDeg < B.NorthDeg && I.LonDeg > B.WestDeg && I.LonDeg < B.EastDeg)) { AddError(TEXT("outside its cell")); break; }
		if (!(I.YawDeg >= 0.0f && I.YawDeg < 360.0f)) { AddError(TEXT("yaw range")); break; }
		if (!(I.Scale >= 0.8f && I.Scale <= 1.2f)) { AddError(TEXT("scale range")); break; }
		if (I.Type != 0) { AddError(TEXT("type 0 only")); break; }
	}
	FScatterParams Zero = P; Zero.DensityScale = 0.0f;
	TestEqual(TEXT("density 0 gives nothing"), GenerateCell(VegetationSeed(0u), C, Zero, AllTrees).Num(), 0);
	return true;
}
```

- [ ] **Step 2: Run to see it fail**

Build: Expected FAIL — `Vegetation/VegetationScatter.h` not found.

- [ ] **Step 3: Add the stream id to `SensorHash.h`**

In the doc comment's stream list, after the `Stream 64 (LandCoverWarpStream, ...)` sentence, add: `Streams 80-82 (VegetationStream, REALISM R4): the vegetation scatter, Hash(col, row, candidate, seed, 160 + k), k = 0 east jitter, 1 north jitter, 2 accept, 3 yaw, 4 scale (CPU only, no HLSL mirror).` and below `LandCoverWarpStream`:

```cpp
	constexpr uint32 VegetationStream = 80u;      // streams 80-82, sub-streams 160..164 (Vegetation/VegetationScatter.cpp)
```

- [ ] **Step 4: Implement**

`Vegetation/VegetationScatter.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Vegetation/VegetationGrid.h"

/**
 * Deterministic tree scatter in one cell (REALISM R4 slice, docs/vegetation.md). A CandidatesPerAxis^2 jittered grid;
 * candidate i of cell (row, col) draws from CamSimHash::Hash(col, row, i, seed, 160 + k), so a cell's trees depend only
 * on (seed, cell, params, land cover). Each jitter stays inside its sub-cell, so trees never cross cell edges. Pure;
 * any thread (the sampler must be thread-safe if called off the game thread).
 */
namespace CamSimVegetation
{
	struct FScatterParams
	{
		double CellSizeM = 100.0;
		double SpacingM = 6.0;      // nominal; the real spacing is CellSizeM / CandidatesPerAxis
		float  PTree = 0.15f;       // acceptance at density 1 (~1 tree per 240 m^2: open-canopy oak woodland)
		float  DensityScale = 1.0f;
		float  ScaleMin = 0.8f;
		float  ScaleMax = 1.2f;
		uint8  TreeCode = 10;       // WorldCover tree cover
	};

	struct FTreeInstance
	{
		double LatDeg = 0.0;
		double LonDeg = 0.0;
		float  YawDeg = 0.0f;
		float  Scale = 1.0f;
		uint8  Type = 0;
		bool operator==(const FTreeInstance&) const = default;
	};

	using FCodeSampler = TFunctionRef<uint8(double LatDeg, double LonDeg)>;

	CAMSIMTEST_API int32 CandidatesPerAxis(const FScatterParams& P);
	/** Accepted trees per m^2 on all-tree land cover. */
	CAMSIMTEST_API double ExpectedTreesPerM2(const FScatterParams& P);
	/** The vegetation seed from a package / config seed (mixed with a constant so it differs from other users of the seed). */
	CAMSIMTEST_API uint32 VegetationSeed(uint32 BaseSeed);
	CAMSIMTEST_API TArray<FTreeInstance> GenerateCell(uint32 Seed, FVegCellId Cell, const FScatterParams& P, FCodeSampler Sampler);
	/** Order-sensitive hash of the instances' bits (dumps, determinism checks). */
	CAMSIMTEST_API uint32 HashInstances(const TArray<FTreeInstance>& Instances);
}
```

`Vegetation/VegetationScatter.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Vegetation/VegetationScatter.h"

#include "SensorHash.h"

namespace CamSimVegetation
{
	namespace
	{
		constexpr uint32 SubJitterE = 2u * CamSimHash::VegetationStream + 0u;   // 160
		constexpr uint32 SubJitterN = 2u * CamSimHash::VegetationStream + 1u;
		constexpr uint32 SubAccept  = 2u * CamSimHash::VegetationStream + 2u;
		constexpr uint32 SubYaw     = 2u * CamSimHash::VegetationStream + 3u;
		constexpr uint32 SubScale   = 2u * CamSimHash::VegetationStream + 4u;   // 164
		constexpr uint32 SeedMix    = 0x56454745u;   // "VEGE"
	}

	int32 CandidatesPerAxis(const FScatterParams& P)
	{
		return FMath::Max(1, FMath::RoundToInt32(P.CellSizeM / FMath::Max(P.SpacingM, 0.5)));
	}

	double ExpectedTreesPerM2(const FScatterParams& P)
	{
		const double N = CandidatesPerAxis(P);
		return FMath::Clamp(double(P.DensityScale) * P.PTree, 0.0, 1.0) * N * N / (P.CellSizeM * P.CellSizeM);
	}

	uint32 VegetationSeed(uint32 BaseSeed) { return CamSimHash::Pcg(BaseSeed ^ SeedMix); }

	TArray<FTreeInstance> GenerateCell(uint32 Seed, FVegCellId Cell, const FScatterParams& P, FCodeSampler Sampler)
	{
		TArray<FTreeInstance> Out;
		const float Accept = FMath::Clamp(P.DensityScale * P.PTree, 0.0f, 1.0f);
		if (Accept <= 0.0f) return Out;
		const FVegCellBounds B = CellBounds(Cell, P.CellSizeM);
		const int32 N = CandidatesPerAxis(P);
		const uint32 X = static_cast<uint32>(Cell.Col), Y = static_cast<uint32>(Cell.Row);
		for (int32 J = 0; J < N; ++J)
			for (int32 I = 0; I < N; ++I)
			{
				const uint32 K = static_cast<uint32>(J * N + I);
				if (CamSimHash::Uniform(CamSimHash::Hash(X, Y, K, Seed, SubAccept)) >= Accept) continue;
				const double Fx = (I + CamSimHash::Uniform(CamSimHash::Hash(X, Y, K, Seed, SubJitterE))) / N;
				const double Fy = (J + CamSimHash::Uniform(CamSimHash::Hash(X, Y, K, Seed, SubJitterN))) / N;
				FTreeInstance T;
				T.LatDeg = B.SouthDeg + Fy * (B.NorthDeg - B.SouthDeg);
				T.LonDeg = B.WestDeg + Fx * (B.EastDeg - B.WestDeg);
				if (Sampler(T.LatDeg, T.LonDeg) != P.TreeCode) continue;
				T.YawDeg = 360.0f * CamSimHash::Uniform(CamSimHash::Hash(X, Y, K, Seed, SubYaw));
				if (T.YawDeg >= 360.0f) T.YawDeg = 0.0f;
				T.Scale = P.ScaleMin + (P.ScaleMax - P.ScaleMin) * CamSimHash::Uniform(CamSimHash::Hash(X, Y, K, Seed, SubScale));
				Out.Add(T);
			}
		return Out;
	}

	uint32 HashInstances(const TArray<FTreeInstance>& Instances)
	{
		uint32 H = CamSimHash::Pcg(static_cast<uint32>(Instances.Num()));
		for (const FTreeInstance& T : Instances)
		{
			uint64 Lat, Lon; uint32 Yaw, Scale;
			FMemory::Memcpy(&Lat, &T.LatDeg, 8); FMemory::Memcpy(&Lon, &T.LonDeg, 8);
			FMemory::Memcpy(&Yaw, &T.YawDeg, 4); FMemory::Memcpy(&Scale, &T.Scale, 4);
			for (uint32 W : { uint32(Lat), uint32(Lat >> 32), uint32(Lon), uint32(Lon >> 32), Yaw, Scale, uint32(T.Type) })
				H = CamSimHash::Pcg(H ^ W);
		}
		return H;
	}
}
```

- [ ] **Step 5: Run tests**

Build; filter `CamSim.Vegetation.Scatter`. Expected: 3 PASS. Also run `CamSim.Sensor` (the header edit must not change sensor results): Expected PASS.

- [ ] **Step 6: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Vegetation/VegetationScatter.* \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/VegetationScatterTest.cpp \
  unreal_project/CamSimTest/Source/CamSimShaders/Public/SensorHash.h
git commit -m "feat(vegetation): deterministic per-cell tree scatter (REALISM R4 slice)"
```

---

### Task 5: Land-cover sampler

**Files:**
- Create: `Vegetation/VegetationLandCover.h/.cpp`
- Test: `Tests/VegetationLandCoverTest.cpp`

**Interfaces:**
- Consumes: `FLandCoverTileCache` (`Thermal/LandCoverTiles.h`: ctor `(const FString& Dir, int32 MaxTiles)`, `IsValid()`, `GetError()`, `Get(LatIndex, LonIndex)`, `TakeWarnings()`, `ResolveDir`), `CamSimLandCover::GlobalCell`, `CellToTile` (`Thermal/LandCoverGeometry.h`).
- Produces: `class FVegetationLandCover { static constexpr int32 MaxTiles = 16; explicit FVegetationLandCover(const FString& Dir) /* game thread */; bool IsValid() const; const FString& GetError() const; uint8 Code(double LatDeg, double LonDeg) /* any thread */; TArray<FString> TakeWarnings(); }`.

- [ ] **Step 1: Write the failing tests**

`Tests/VegetationLandCoverTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Async/ParallelFor.h"
#include "Tests/LandCoverTestTiles.h"
#include "Thermal/LandCoverLookup.h"
#include "Vegetation/VegetationLandCover.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegLandCoverMatchesLookupTest, "CamSim.Vegetation.LandCover.MatchesCodeLookup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegLandCoverMatchesLookupTest::RunTest(const FString& Parameters)
{
	using namespace CamSimLandCoverTest;
	const FString Dir = TempDir(TEXT("VegSampler"));
	WriteTileDir(Dir, { FTestTile{ 666, -2350, PatternCodes() } });
	FVegetationLandCover V(Dir);
	FLandCoverCodeLookup L;
	L.Configure(Dir);
	TestTrue(TEXT("valid"), V.IsValid());
	int32 Mismatch = 0;
	for (int32 K = 0; K < 500; ++K)
	{
		L.BeginTick();
		const double Lat = 33.30 + 0.05 * ((K * 37) % 500) / 500.0, Lon = -117.50 + 0.05 * ((K * 91) % 500) / 500.0;
		Mismatch += V.Code(Lat, Lon) != L.Code(Lat, Lon);
	}
	TestEqual(TEXT("same code as the CIGI lookup (same cell pick)"), Mismatch, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegLandCoverMissingTest, "CamSim.Vegetation.LandCover.MissingTilesGiveZero",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegLandCoverMissingTest::RunTest(const FString& Parameters)
{
	using namespace CamSimLandCoverTest;
	const FString Dir = TempDir(TEXT("VegSamplerMissing"));
	WriteTileDir(Dir, { FTestTile{ 666, -2350, PatternCodes() } });
	// An index entry whose file is an LFS pointer.
	FFileHelper::SaveArrayToFile(LfsPointerBytes(), *FPaths::Combine(Dir, TileFile(666, -2350)));
	FVegetationLandCover V(Dir);
	TestEqual(TEXT("LFS pointer tile -> 0"), static_cast<int32>(V.Code(33.32, -117.47)), 0);
	TestEqual(TEXT("no tile -> 0"), static_cast<int32>(V.Code(10.0, 10.0)), 0);
	TestEqual(TEXT("NaN -> 0"), static_cast<int32>(V.Code(TNumericLimits<double>::Quiet_NaN(), 0.0)), 0);
	for (int32 K = 0; K < 50; ++K) V.Code(33.32, -117.47);
	const TArray<FString> W = V.TakeWarnings();
	TestTrue(TEXT("at most one summary warning"), W.Num() <= 1);
	TestEqual(TEXT("no repeat warning"), V.TakeWarnings().Num(), 0);
	FVegetationLandCover Bad(TempDir(TEXT("VegSamplerNoIndex")));
	TestFalse(TEXT("dir without index.json is invalid"), Bad.IsValid());
	TestEqual(TEXT("invalid -> 0"), static_cast<int32>(Bad.Code(33.32, -117.47)), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegLandCoverThreadsTest, "CamSim.Vegetation.LandCover.AnyThread",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegLandCoverThreadsTest::RunTest(const FString& Parameters)
{
	using namespace CamSimLandCoverTest;
	const FString Dir = TempDir(TEXT("VegSamplerThreads"));
	WriteTileDir(Dir, { FTestTile{ 666, -2350, PatternCodes() }, FTestTile{ 666, -2349, PatternCodes() } });
	FVegetationLandCover V(Dir);
	TArray<uint8> Codes; Codes.SetNumZeroed(4000);
	ParallelFor(Codes.Num(), [&](int32 K) { Codes[K] = V.Code(33.30 + 0.0001 * (K % 400), -117.50 + 0.0001 * K * 0.025); });
	int32 Mismatch = 0;
	for (int32 K = 0; K < Codes.Num(); ++K) Mismatch += Codes[K] != V.Code(33.30 + 0.0001 * (K % 400), -117.50 + 0.0001 * K * 0.025);
	TestEqual(TEXT("parallel == serial"), Mismatch, 0);
	return true;
}
```

(`TileFile` and `LfsPointerBytes` are existing helpers in `Tests/LandCoverTestTiles.h`; `LfsPointerBytes` returns `TArray<uint8>`, `SaveArrayToFile` accepts it.)

- [ ] **Step 2: Run to see it fail**

Build: Expected FAIL — header not found.

- [ ] **Step 3: Implement**

`Vegetation/VegetationLandCover.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"

class FLandCoverTileCache;

/**
 * Point land-cover codes for the vegetation scatter (REALISM R4 slice): its own MaxTiles LRU (~5.8 MB), so it never
 * spends FLandCoverCodeLookup's per-tick game-thread budget. Same GlobalCell + CellToTile pick as the thermal window and
 * the CIGI lookup. Code() is thread-safe (the scatter calls it on worker tasks); a cache miss decodes on the caller.
 */
class CAMSIMTEST_API FVegetationLandCover
{
public:
	static constexpr int32 MaxTiles = 16;

	/** Game thread (the tile cache loads the ImageWrapper module). Dir as thermal.land_cover.dir (relative = project dir). */
	explicit FVegetationLandCover(const FString& Dir);
	~FVegetationLandCover();

	bool IsValid() const;
	const FString& GetError() const;
	/** The code of the cell holding (Lat, Lon), or 0 (no data, invalid, non-finite). Any thread. */
	uint8 Code(double LatDeg, double LonDeg);
	/** The tile cache's summary warnings since the last call. Any thread. */
	TArray<FString> TakeWarnings();

private:
	TUniquePtr<FLandCoverTileCache> Cache;
	FString Error;
};
```

`Vegetation/VegetationLandCover.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Vegetation/VegetationLandCover.h"

#include "Thermal/LandCoverGeometry.h"
#include "Thermal/LandCoverTiles.h"

FVegetationLandCover::FVegetationLandCover(const FString& Dir)
{
	if (Dir.IsEmpty()) { Error = TEXT("no land-cover directory"); return; }
	Cache = MakeUnique<FLandCoverTileCache>(FLandCoverTileCache::ResolveDir(Dir), MaxTiles);
	if (!Cache->IsValid()) { Error = Cache->GetError(); }
}

FVegetationLandCover::~FVegetationLandCover() = default;

bool FVegetationLandCover::IsValid() const { return Cache.IsValid() && Cache->IsValid(); }

const FString& FVegetationLandCover::GetError() const { return Error; }

uint8 FVegetationLandCover::Code(double LatDeg, double LonDeg)
{
	if (!IsValid() || !FMath::IsFinite(LatDeg) || !FMath::IsFinite(LonDeg)) return 0;
	FIntPoint Tile, In;   // Tile = (LatIndex, LonIndex); In = (column, row), row 0 = north
	CamSimLandCover::CellToTile(CamSimLandCover::GlobalCell(LatDeg, LonDeg), Tile, In);
	const TSharedPtr<const FLandCoverTile, ESPMode::ThreadSafe> T = Cache->Get(Tile.X, Tile.Y);
	return T.IsValid() ? T->Codes[In.Y * FLandCoverTileCache::TilePx + In.X] : 0;
}

TArray<FString> FVegetationLandCover::TakeWarnings()
{
	return Cache.IsValid() ? Cache->TakeWarnings() : TArray<FString>();
}
```

- [ ] **Step 4: Run tests**

Build; filter `CamSim.Vegetation.LandCover`. Expected: 3 PASS.

- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Vegetation/VegetationLandCover.* \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/VegetationLandCoverTest.cpp
git commit -m "feat(vegetation): thread-safe land-cover sampler (REALISM R4 slice)"
```

---

### Task 6: Vegetation library (`library.json`)

**Files:**
- Create: `Vegetation/VegetationLibrary.h/.cpp`
- Test: `Tests/VegetationLibraryTest.cpp`

**Interfaces:**
- Produces: `struct FVegetationType { FString Name; FString GltfPath /*absolute, empty if procedural*/; float HeightM = 0; FString ThermalClass; FString Licence; FString SourceUrl; FString Sha256 /*lowercase hex*/; bool bProcedural = false; }`; `struct FVegetationLibrary { TArray<FVegetationType> Types; }`; namespace `CamSimVegetation`: `bool ParseLibrary(const FString& Json, const FString& Dir, FVegetationLibrary& Out, FString& OutError)`, `FString ResolveLibraryDir(const FString& ConfigDir)` ("" → `Content/NonUFS/Vegetation`, relative → project dir, via `FLandCoverTileCache::ResolveDir`), `bool LoadLibrary(const FString& Dir, FVegetationLibrary& Out, FString& OutError)` (reads `<Dir>/library.json`, parses, verifies every non-procedural file's sha256), `FString Sha256Hex(TConstArrayView<uint8> Bytes)`.

- [ ] **Step 1: Write the failing tests**

`Tests/VegetationLibraryTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Vegetation/VegetationLibrary.h"

using namespace CamSimVegetation;

namespace
{
	FString LibTempDir(const TCHAR* Name)
	{
		const FString D = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("Vegetation"), Name));
		IFileManager::Get().DeleteDirectory(*D, false, true);
		IFileManager::Get().MakeDirectory(*D, true);
		return D;
	}
	FString Entry(const FString& Sha) 
	{
		return FString::Printf(TEXT("{\"format\":\"camsim-vegetation-1\",\"types\":[{\"name\":\"t\",\"gltf\":\"t.glb\",\"height_m\":9.0,")
			TEXT("\"thermal_class\":\"tree_canopy\",\"licence\":\"CC0-1.0\",\"source_url\":\"u\",\"sha256\":\"%s\"}]}"), *Sha);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegLibSha256Test, "CamSim.Vegetation.Library.Sha256",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegLibSha256Test::RunTest(const FString& Parameters)
{
	const FTCHARToUTF8 Abc(TEXT("abc"));
	TestEqual(TEXT("FIPS 180-2 'abc'"), Sha256Hex(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Abc.Get()), Abc.Length())),
		FString(TEXT("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")));
	TestEqual(TEXT("empty"), Sha256Hex({}), FString(TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegLibParseTest, "CamSim.Vegetation.Library.Parse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegLibParseTest::RunTest(const FString& Parameters)
{
	FVegetationLibrary L; FString Err;
	TestTrue(TEXT("valid"), ParseLibrary(Entry(FString::ChrN(64, 'a')), TEXT("/lib"), L, Err));
	TestEqual(TEXT("one type"), L.Types.Num(), 1);
	if (L.Types.Num() == 1)
	{
		TestEqual(TEXT("absolute gltf path"), L.Types[0].GltfPath, FString(TEXT("/lib/t.glb")));
		TestEqual(TEXT("height"), L.Types[0].HeightM, 9.0f);
		TestFalse(TEXT("not procedural"), L.Types[0].bProcedural);
	}
	TestTrue(TEXT("procedural without gltf/sha"), ParseLibrary(TEXT("{\"format\":\"camsim-vegetation-1\",\"types\":[{\"name\":\"p\",")
		TEXT("\"procedural\":true,\"height_m\":9,\"licence\":\"CC0-1.0\",\"source_url\":\"generated\"}]}"), TEXT("/lib"), L, Err));
	TestTrue(TEXT("procedural flag"), L.Types.Num() == 1 && L.Types[0].bProcedural);
	TestEqual(TEXT("thermal class default"), L.Types.Num() == 1 ? L.Types[0].ThermalClass : FString(), FString(TEXT("tree_canopy")));

	const TCHAR* Bad[] = {
		TEXT("not json"),
		TEXT("{\"format\":\"camsim-vegetation-2\",\"types\":[]}"),
		TEXT("{\"format\":\"camsim-vegetation-1\",\"types\":[]}"),
		TEXT("{\"format\":\"camsim-vegetation-1\",\"types\":[{\"name\":\"t\",\"gltf\":\"t.glb\",\"height_m\":9,\"licence\":\"CC0-1.0\",\"source_url\":\"u\"}]}"),
		TEXT("{\"format\":\"camsim-vegetation-1\",\"types\":[{\"name\":\"t\",\"gltf\":\"t.glb\",\"height_m\":0,\"licence\":\"CC0-1.0\",\"source_url\":\"u\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}]}"),
		TEXT("{\"format\":\"camsim-vegetation-1\",\"types\":[{\"name\":\"t\",\"gltf\":\"t.glb\",\"height_m\":9,\"source_url\":\"u\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}]}"),
		TEXT("{\"format\":\"camsim-vegetation-1\",\"types\":[{\"name\":\"t\",\"gltf\":\"t.glb\",\"height_m\":9,\"licence\":\"CC0-1.0\",\"source_url\":\"u\",\"sha256\":\"xyz\"}]}"),
	};
	for (const TCHAR* J : Bad)
	{
		FString E;
		TestFalse(FString::Printf(TEXT("rejects %s"), J), ParseLibrary(J, TEXT("/lib"), L, E));
		TestFalse(TEXT("with a reason"), E.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegLibLoadTest, "CamSim.Vegetation.Library.LoadVerifiesSha",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegLibLoadTest::RunTest(const FString& Parameters)
{
	const FString D = LibTempDir(TEXT("Load"));
	const FTCHARToUTF8 Body(TEXT("glb bytes"));
	TArray<uint8> Bytes(reinterpret_cast<const uint8*>(Body.Get()), Body.Length());
	FFileHelper::SaveArrayToFile(Bytes, *(D / TEXT("t.glb")));
	FVegetationLibrary L; FString Err;

	FFileHelper::SaveStringToFile(Entry(Sha256Hex(Bytes)), *(D / TEXT("library.json")));
	TestTrue(TEXT("matching sha loads"), LoadLibrary(D, L, Err));

	FFileHelper::SaveStringToFile(Entry(FString::ChrN(64, 'b')), *(D / TEXT("library.json")));
	TestFalse(TEXT("sha mismatch refused"), LoadLibrary(D, L, Err));
	TestTrue(TEXT("names the file"), Err.Contains(TEXT("t.glb")));

	const FTCHARToUTF8 Ptr(TEXT("version https://git-lfs.github.com/spec/v1\noid sha256:00\nsize 1\n"));
	FFileHelper::SaveArrayToFile(TArray<uint8>(reinterpret_cast<const uint8*>(Ptr.Get()), Ptr.Length()), *(D / TEXT("t.glb")));
	FFileHelper::SaveStringToFile(Entry(Sha256Hex(Bytes)), *(D / TEXT("library.json")));
	TestFalse(TEXT("LFS pointer refused"), LoadLibrary(D, L, Err));
	TestTrue(TEXT("hints git lfs pull"), Err.Contains(TEXT("git lfs pull")));

	IFileManager::Get().Delete(*(D / TEXT("t.glb")));
	TestFalse(TEXT("missing file refused"), LoadLibrary(D, L, Err));
	TestFalse(TEXT("missing library.json refused"), LoadLibrary(LibTempDir(TEXT("Empty")), L, Err));

	TestTrue(TEXT("built-in dir default"), ResolveLibraryDir(TEXT("")).EndsWith(TEXT("Content/NonUFS/Vegetation")));
	TestEqual(TEXT("absolute kept"), ResolveLibraryDir(TEXT("/mnt/veg")), FString(TEXT("/mnt/veg")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegLibShippedTest, "CamSim.Vegetation.Library.ShippedLibraryLoads",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegLibShippedTest::RunTest(const FString& Parameters)
{
	FVegetationLibrary L; FString Err;
	const bool bOk = LoadLibrary(ResolveLibraryDir(TEXT("")), L, Err);
	TestTrue(FString::Printf(TEXT("Content/NonUFS/Vegetation loads (%s; run git lfs pull if it is a pointer)"), *Err), bOk);
	TestTrue(TEXT("at least one type"), L.Types.Num() >= 1);
	return true;
}
```

- [ ] **Step 2: Run to see it fail**

Build: Expected FAIL — header not found.

- [ ] **Step 3: Implement**

`Vegetation/VegetationLibrary.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** One tree type of the vegetation library (REALISM R4 slice, docs/vegetation.md). */
struct FVegetationType
{
	FString Name;
	FString GltfPath;       // absolute; empty when procedural
	float   HeightM = 0.0f; // the model's real height
	FString ThermalClass;   // informational in the slice (the pixel's class comes from the land cover)
	FString Licence;
	FString SourceUrl;
	FString Sha256;         // lowercase hex; empty when procedural
	bool    bProcedural = false;
};

struct FVegetationLibrary
{
	TArray<FVegetationType> Types;
};

namespace CamSimVegetation
{
	/** library.json ("camsim-vegetation-1"). Dir makes gltf paths absolute. Unknown keys are ignored. */
	CAMSIMTEST_API bool ParseLibrary(const FString& Json, const FString& Dir, FVegetationLibrary& Out, FString& OutError);
	/** "" -> Content/NonUFS/Vegetation; relative -> project dir; absolute kept. */
	CAMSIMTEST_API FString ResolveLibraryDir(const FString& ConfigDir);
	/** Reads <Dir>/library.json and checks every non-procedural file's sha256 (an LFS pointer gets the git lfs pull hint). */
	CAMSIMTEST_API bool LoadLibrary(const FString& Dir, FVegetationLibrary& Out, FString& OutError);
	CAMSIMTEST_API FString Sha256Hex(TConstArrayView<uint8> Bytes);
}
```

`Vegetation/VegetationLibrary.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Vegetation/VegetationLibrary.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Thermal/LandCoverTiles.h"

extern "C"
{
#include "libavutil/mem.h"
#include "libavutil/sha.h"
}

namespace CamSimVegetation
{
	namespace
	{
		bool IsHex64(const FString& S)
		{
			if (S.Len() != 64) return false;
			for (TCHAR C : S) if (!FChar::IsHexDigit(C) || FChar::IsUpper(C)) return false;
			return true;
		}
	}

	FString Sha256Hex(TConstArrayView<uint8> Bytes)
	{
		AVSHA* Ctx = av_sha_alloc();
		av_sha_init(Ctx, 256);
		if (Bytes.Num() > 0) av_sha_update(Ctx, Bytes.GetData(), static_cast<size_t>(Bytes.Num()));
		uint8 Digest[32];
		av_sha_final(Ctx, Digest);
		av_free(Ctx);
		return BytesToHex(Digest, 32).ToLower();
	}

	bool ParseLibrary(const FString& Json, const FString& Dir, FVegetationLibrary& Out, FString& OutError)
	{
		Out = FVegetationLibrary();
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
		{
			OutError = TEXT("library.json is not valid JSON"); return false;
		}
		FString Format;
		if (!Root->TryGetStringField(TEXT("format"), Format) || Format != TEXT("camsim-vegetation-1"))
		{
			OutError = FString::Printf(TEXT("library.json format '%s' is not camsim-vegetation-1"), *Format); return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Types = nullptr;
		if (!Root->TryGetArrayField(TEXT("types"), Types) || Types->Num() == 0)
		{
			OutError = TEXT("library.json has no types"); return false;
		}
		for (int32 I = 0; I < Types->Num(); ++I)
		{
			const TSharedPtr<FJsonObject>* O = nullptr;
			if (!(*Types)[I]->TryGetObject(O)) { OutError = FString::Printf(TEXT("types[%d] is not an object"), I); return false; }
			FVegetationType T;
			double H = 0.0;
			(*O)->TryGetStringField(TEXT("name"), T.Name);
			(*O)->TryGetNumberField(TEXT("height_m"), H);
			(*O)->TryGetStringField(TEXT("licence"), T.Licence);
			(*O)->TryGetStringField(TEXT("source_url"), T.SourceUrl);
			(*O)->TryGetBoolField(TEXT("procedural"), T.bProcedural);
			if (!(*O)->TryGetStringField(TEXT("thermal_class"), T.ThermalClass)) T.ThermalClass = TEXT("tree_canopy");
			T.HeightM = static_cast<float>(H);
			if (T.Name.IsEmpty()) { OutError = FString::Printf(TEXT("types[%d]: name missing"), I); return false; }
			if (!(H > 0.5 && H <= 80.0)) { OutError = FString::Printf(TEXT("type '%s': height_m %g not in (0.5, 80]"), *T.Name, H); return false; }
			if (T.Licence.IsEmpty()) { OutError = FString::Printf(TEXT("type '%s': licence missing"), *T.Name); return false; }
			if (!T.bProcedural)
			{
				FString Gltf;
				if (!(*O)->TryGetStringField(TEXT("gltf"), Gltf) || Gltf.IsEmpty()) { OutError = FString::Printf(TEXT("type '%s': gltf missing"), *T.Name); return false; }
				if (!(*O)->TryGetStringField(TEXT("sha256"), T.Sha256) || !IsHex64(T.Sha256))
				{
					OutError = FString::Printf(TEXT("type '%s': sha256 must be 64 lowercase hex digits"), *T.Name); return false;
				}
				T.GltfPath = FPaths::IsRelative(Gltf) ? FPaths::Combine(Dir, Gltf) : Gltf;
			}
			Out.Types.Add(MoveTemp(T));
		}
		return true;
	}

	FString ResolveLibraryDir(const FString& ConfigDir)
	{
		return FLandCoverTileCache::ResolveDir(ConfigDir.IsEmpty() ? FString(TEXT("Content/NonUFS/Vegetation")) : ConfigDir);
	}

	bool LoadLibrary(const FString& Dir, FVegetationLibrary& Out, FString& OutError)
	{
		FString Json;
		if (!FFileHelper::LoadFileToString(Json, *FPaths::Combine(Dir, TEXT("library.json"))))
		{
			OutError = FString::Printf(TEXT("no library.json in '%s'"), *Dir); return false;
		}
		if (!ParseLibrary(Json, Dir, Out, OutError)) return false;
		for (const FVegetationType& T : Out.Types)
		{
			if (T.bProcedural) continue;
			TArray<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, *T.GltfPath))
			{
				OutError = FString::Printf(TEXT("type '%s': cannot read %s"), *T.Name, *T.GltfPath); return false;
			}
			// A git LFS pointer is a short text file; compare the prefix within the bytes actually loaded (the buffer is not NUL-terminated).
			static const ANSICHAR LfsPrefix[] = "version https://git-lfs";
			constexpr int32 LfsPrefixLen = static_cast<int32>(sizeof(LfsPrefix) - 1);
			if (Bytes.Num() < 200 && Bytes.Num() >= LfsPrefixLen && FMemory::Memcmp(Bytes.GetData(), LfsPrefix, LfsPrefixLen) == 0)
			{
				OutError = FString::Printf(TEXT("type '%s': %s is a git LFS pointer (run git lfs pull)"), *T.Name, *T.GltfPath); return false;
			}
			const FString Got = Sha256Hex(Bytes);
			if (Got != T.Sha256)
			{
				OutError = FString::Printf(TEXT("type '%s': %s sha256 %s != library.json %s"), *T.Name, *T.GltfPath, *Got, *T.Sha256); return false;
			}
		}
		return true;
	}
}
```

(The bundled `libavutil/sha.h` declares `av_sha_update(struct AVSHA*, const uint8_t*, size_t)`, checked while writing the plan.)

- [ ] **Step 4: Run tests**

Build; filter `CamSim.Vegetation.Library`. Expected: 4 PASS (`ShippedLibraryLoads` needs Task 1's committed library).

- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Vegetation/VegetationLibrary.* \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/VegetationLibraryTest.cpp
git commit -m "feat(vegetation): library.json parser with sha256 check (REALISM R4 slice)"
```

---

### Task 7: Config (`vegetation.*`) and the package seed

**Files:**
- Modify: `Config/CamSimConfig.h` (new `FVegetationConfig Vegetation;` after `FThermalConfig Thermal;`; `Scene.Seed` + `Scene.bHasSeed`)
- Modify: `Config/CamSimConfig.cpp` (YAML block next to `ocean`, env block next to the ocean env lines, `Validate()` checks)
- Modify: `Config/ScenePackage.cpp` (read manifest `seed`)
- Modify: `deploy/camsim_config.yaml`, `docs/configuration.md`
- Test: `Tests/VegetationConfigTest.cpp`, `Tests/ScenePackageTest.cpp`

**Interfaces:**
- Produces: `FCamSimConfig::FVegetationConfig { bool bEnabled = true; FString LibraryDir; float RadiusM = 1000.0f; float DensityScale = 1.0f; bool bCastShadows = true; float CellSizeM = 100.0f; int32 Seed = 0; }` as `Cfg.Vegetation`; `Cfg.Scene.Seed` (`int64`, 0 when absent) and `Cfg.Scene.bHasSeed`.

- [ ] **Step 1: Write the failing tests**

`Tests/VegetationConfigTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"

#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegConfigDefaultsTest, "CamSim.Vegetation.Config.Defaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegConfigDefaultsTest::RunTest(const FString& Parameters)
{
	const FCamSimConfig::FVegetationConfig V;
	TestTrue (TEXT("enabled"), V.bEnabled);
	TestEqual(TEXT("library_dir"), V.LibraryDir, FString());
	TestEqual(TEXT("radius_m"), V.RadiusM, 1000.0f);
	TestEqual(TEXT("density_scale"), V.DensityScale, 1.0f);
	TestTrue (TEXT("cast_shadows"), V.bCastShadows);
	TestEqual(TEXT("cell_size_m"), V.CellSizeM, 100.0f);
	TestEqual(TEXT("seed"), V.Seed, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegConfigYamlEnvTest, "CamSim.Vegetation.Config.YamlAndEnv",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegConfigYamlEnvTest::RunTest(const FString& Parameters)
{
	const FString Yaml = TEXT(
		"vegetation:\n"
		"  enabled: false\n"
		"  library_dir: \"/mnt/veg\"\n"
		"  radius_m: 1500\n"
		"  density_scale: 0.5\n"
		"  cast_shadows: false\n"
		"  cell_size_m: 50\n"
		"  seed: 42\n");
	FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(Yaml);
	TestTrue (TEXT("parsed"), Cfg.bLoadedSuccessfully);
	TestFalse(TEXT("enabled"), Cfg.Vegetation.bEnabled);
	TestEqual(TEXT("library_dir"), Cfg.Vegetation.LibraryDir, FString(TEXT("/mnt/veg")));
	TestEqual(TEXT("radius"), Cfg.Vegetation.RadiusM, 1500.0f);
	TestEqual(TEXT("density"), Cfg.Vegetation.DensityScale, 0.5f);
	TestFalse(TEXT("shadows"), Cfg.Vegetation.bCastShadows);
	TestEqual(TEXT("cell"), Cfg.Vegetation.CellSizeM, 50.0f);
	TestEqual(TEXT("seed"), Cfg.Vegetation.Seed, 42);

	const TCHAR* Vars[][2] = {
		{ TEXT("CAMSIM_VEGETATION_ENABLED"), TEXT("true") }, { TEXT("CAMSIM_VEGETATION_LIBRARY_DIR"), TEXT("/opt/veg") },
		{ TEXT("CAMSIM_VEGETATION_RADIUS_M"), TEXT("500") }, { TEXT("CAMSIM_VEGETATION_DENSITY_SCALE"), TEXT("0.25") },
		{ TEXT("CAMSIM_VEGETATION_CAST_SHADOWS"), TEXT("1") }, { TEXT("CAMSIM_VEGETATION_CELL_SIZE_M"), TEXT("200") },
		{ TEXT("CAMSIM_VEGETATION_SEED"), TEXT("7") } };
	for (const auto& V : Vars) FPlatformMisc::SetEnvironmentVar(V[0], V[1]);
	Cfg = FCamSimConfig::LoadFromYamlString(Yaml);
	for (const auto& V : Vars) FPlatformMisc::SetEnvironmentVar(V[0], TEXT(""));
	TestTrue (TEXT("env enabled"), Cfg.Vegetation.bEnabled);
	TestEqual(TEXT("env library_dir"), Cfg.Vegetation.LibraryDir, FString(TEXT("/opt/veg")));
	TestEqual(TEXT("env radius"), Cfg.Vegetation.RadiusM, 500.0f);
	TestEqual(TEXT("env density"), Cfg.Vegetation.DensityScale, 0.25f);
	TestTrue (TEXT("env shadows"), Cfg.Vegetation.bCastShadows);
	TestEqual(TEXT("env cell"), Cfg.Vegetation.CellSizeM, 200.0f);
	TestEqual(TEXT("env seed"), Cfg.Vegetation.Seed, 7);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegConfigValidateTest, "CamSim.Vegetation.Config.Validate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegConfigValidateTest::RunTest(const FString& Parameters)
{
	auto Has = [](const TArray<FString>& E, const TCHAR* S) { return E.ContainsByPredicate([S](const FString& X) { return X.Contains(S); }); };
	auto With = [](TFunction<void(FCamSimConfig::FVegetationConfig&)> Set) { FCamSimConfig C; Set(C.Vegetation); return C.Validate(); };
	const float NaN = std::numeric_limits<float>::quiet_NaN();
	TestFalse(TEXT("defaults valid"), Has(FCamSimConfig().Validate(), TEXT("vegetation.")));
	TestTrue (TEXT("radius 0"),     Has(With([](auto& V) { V.RadiusM = 0.0f; }), TEXT("vegetation.radius_m")));
	TestTrue (TEXT("radius 6000"),  Has(With([](auto& V) { V.RadiusM = 6000.0f; }), TEXT("vegetation.radius_m")));
	TestTrue (TEXT("radius NaN"),   Has(With([NaN](auto& V) { V.RadiusM = NaN; }), TEXT("vegetation.radius_m")));
	TestFalse(TEXT("radius 5000"),  Has(With([](auto& V) { V.RadiusM = 5000.0f; }), TEXT("vegetation.radius_m")));
	TestTrue (TEXT("density -0.1"), Has(With([](auto& V) { V.DensityScale = -0.1f; }), TEXT("vegetation.density_scale")));
	TestTrue (TEXT("density 4.5"),  Has(With([](auto& V) { V.DensityScale = 4.5f; }), TEXT("vegetation.density_scale")));
	TestFalse(TEXT("density 0"),    Has(With([](auto& V) { V.DensityScale = 0.0f; }), TEXT("vegetation.density_scale")));
	TestTrue (TEXT("cell 10"),      Has(With([](auto& V) { V.CellSizeM = 10.0f; }), TEXT("vegetation.cell_size_m")));
	TestTrue (TEXT("cell 600"),     Has(With([](auto& V) { V.CellSizeM = 600.0f; }), TEXT("vegetation.cell_size_m")));
	TestTrue (TEXT("seed -1"),      Has(With([](auto& V) { V.Seed = -1; }), TEXT("vegetation.seed")));
	return true;
}
```

Append to `Tests/ScenePackageTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScenePackageSeedTest,
	"CamSim.Scene.Package.Seed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FScenePackageSeedTest::RunTest(const FString& Parameters)
{
	{
		const FCamSimConfig Cfg = ResolvedConfigFor(MakePackage(TEXT("SeedPresent"), true, false, false,
			TEXT("{\"schema_version\": 1, \"name\": \"p\", \"seed\": 1234}")));
		TestTrue(TEXT("has seed"), Cfg.Scene.bHasSeed);
		TestEqual(TEXT("seed"), Cfg.Scene.Seed, int64(1234));
	}
	{
		const FCamSimConfig Cfg = ResolvedConfigFor(MakePackage(TEXT("SeedAbsent"), true, false, false));
		TestFalse(TEXT("no seed"), Cfg.Scene.bHasSeed);
		TestEqual(TEXT("no errors"), Cfg.Scene.ResolveErrors.Num(), 0);
	}
	return true;
}
```

- [ ] **Step 2: Run to see it fail**

Build: Expected FAIL — `FVegetationConfig` / `Scene.Seed` undefined.

- [ ] **Step 3: Implement**

In `Config/CamSimConfig.h`, inside `struct FSceneConfig` after `FString PackageName;`:

```cpp
		// manifest.json "seed" (REALISM R4: the vegetation scatter's seed). bHasSeed false for manifests without one.
		int64 Seed = 0;
		bool  bHasSeed = false;
```

After `FThermalConfig Thermal;`:

```cpp
	/** Vegetation scatter (REALISM R4 slice, docs/vegetation.md). */
	struct FVegetationConfig
	{
		bool    bEnabled     = true;     // CAMSIM_VEGETATION_ENABLED; acts only with land cover and a loadable library
		FString LibraryDir;              // CAMSIM_VEGETATION_LIBRARY_DIR; "" = Content/NonUFS/Vegetation
		float   RadiusM      = 1000.0f;  // CAMSIM_VEGETATION_RADIUS_M; cells within this ground radius of the camera nadir
		float   DensityScale = 1.0f;     // CAMSIM_VEGETATION_DENSITY_SCALE; x p_tree (0.15)
		bool    bCastShadows = true;     // CAMSIM_VEGETATION_CAST_SHADOWS
		float   CellSizeM    = 100.0f;   // CAMSIM_VEGETATION_CELL_SIZE_M
		int32   Seed         = 0;        // CAMSIM_VEGETATION_SEED; used only without a scene package
	};
	FVegetationConfig Vegetation;
```

In `Config/CamSimConfig.cpp`, after the `ocean` YAML block (`YamlString(O, "material", ...)` closing brace):

```cpp
		if (YamlHas(Root, "vegetation"))
		{
			ryml::ConstNodeRef V = Root["vegetation"];
			YamlBool  (V, "enabled",       Cfg.Vegetation.bEnabled);
			YamlString(V, "library_dir",   Cfg.Vegetation.LibraryDir);
			YamlFloat (V, "radius_m",      Cfg.Vegetation.RadiusM);
			YamlFloat (V, "density_scale", Cfg.Vegetation.DensityScale);
			YamlBool  (V, "cast_shadows",  Cfg.Vegetation.bCastShadows);
			YamlFloat (V, "cell_size_m",   Cfg.Vegetation.CellSizeM);
			YamlInt   (V, "seed",          Cfg.Vegetation.Seed);
		}
```

In `ApplyEnvOverrides`, after the `CAMSIM_OCEAN_WATER_TEMPERATURE_C` line:

```cpp
	Cfg.Vegetation.bEnabled     = GetEnvBool (TEXT("CAMSIM_VEGETATION_ENABLED"),       Cfg.Vegetation.bEnabled);
	Cfg.Vegetation.LibraryDir   = GetEnv     (TEXT("CAMSIM_VEGETATION_LIBRARY_DIR"),   Cfg.Vegetation.LibraryDir);
	Cfg.Vegetation.RadiusM      = GetEnvFloat(TEXT("CAMSIM_VEGETATION_RADIUS_M"),      Cfg.Vegetation.RadiusM);
	Cfg.Vegetation.DensityScale = GetEnvFloat(TEXT("CAMSIM_VEGETATION_DENSITY_SCALE"), Cfg.Vegetation.DensityScale);
	Cfg.Vegetation.bCastShadows = GetEnvBool (TEXT("CAMSIM_VEGETATION_CAST_SHADOWS"),  Cfg.Vegetation.bCastShadows);
	Cfg.Vegetation.CellSizeM    = GetEnvFloat(TEXT("CAMSIM_VEGETATION_CELL_SIZE_M"),   Cfg.Vegetation.CellSizeM);
	Cfg.Vegetation.Seed         = GetEnvInt  (TEXT("CAMSIM_VEGETATION_SEED"),          Cfg.Vegetation.Seed);
```

(Check `GetEnv` returns the default for an empty value as the other string overrides rely on; if not, mirror how `CAMSIM_SCENE_DIR` is read.)

In `FCamSimConfig::Validate()`, after the ocean checks:

```cpp
	if (!(Vegetation.RadiusM > 0.0f && Vegetation.RadiusM <= 5000.0f))
		Errors.Add(FString::Printf(TEXT("vegetation.radius_m=%.1f must be in (0, 5000]"), Vegetation.RadiusM));
	if (!(Vegetation.DensityScale >= 0.0f && Vegetation.DensityScale <= 4.0f))
		Errors.Add(FString::Printf(TEXT("vegetation.density_scale=%.2f out of range [0, 4]"), Vegetation.DensityScale));
	if (!(Vegetation.CellSizeM >= 25.0f && Vegetation.CellSizeM <= 500.0f))
		Errors.Add(FString::Printf(TEXT("vegetation.cell_size_m=%.1f out of range [25, 500]"), Vegetation.CellSizeM));
	if (Vegetation.Seed < 0)
		Errors.Add(FString::Printf(TEXT("vegetation.seed=%d must be >= 0"), Vegetation.Seed));
```

In `Config/ScenePackage.cpp`, after `Manifest->TryGetStringField(TEXT("name"), Scene.PackageName);`:

```cpp
	double SeedNum = 0.0;
	Scene.bHasSeed = Manifest->TryGetNumberField(TEXT("seed"), SeedNum) && FMath::IsFinite(SeedNum) && SeedNum >= 0.0;
	Scene.Seed = Scene.bHasSeed ? static_cast<int64>(SeedNum) : 0;
```

(and reset them where `Scene.PackageName.Reset();` is: `Scene.Seed = 0; Scene.bHasSeed = false;`).

`deploy/camsim_config.yaml`, after the `ocean:` block:

```yaml
# --- Vegetation scatter (REALISM R4 slice, docs/vegetation.md) ---
# One CC0 tree type scattered where the land cover is tree cover (code 10), around the camera, deterministic per 100 m
# cell. Needs land cover (thermal.land_cover.dir or a scene package) and Content/NonUFS/Vegetation (git lfs pull).
vegetation:
  enabled: true                          # CAMSIM_VEGETATION_ENABLED — startup only
  library_dir: ""                        # CAMSIM_VEGETATION_LIBRARY_DIR — "" = Content/NonUFS/Vegetation
  radius_m: 1000.0                       # CAMSIM_VEGETATION_RADIUS_M — (0, 5000]; hard cull at this distance
  density_scale: 1.0                     # CAMSIM_VEGETATION_DENSITY_SCALE — [0, 4] x p_tree 0.15
  cast_shadows: true                     # CAMSIM_VEGETATION_CAST_SHADOWS
  cell_size_m: 100.0                     # CAMSIM_VEGETATION_CELL_SIZE_M — [25, 500]
  seed: 0                                # CAMSIM_VEGETATION_SEED — only without a scene package (else manifest seed)
```

`docs/configuration.md`: add a `## Vegetation (\`vegetation:\`)` section after `## Ocean (\`ocean:\`)` with the table from the spec (key / env / default / meaning, the ranges above, "startup only"), and the seven env vars in "Environment Variable Quick Reference".

- [ ] **Step 4: Run tests**

Build; filters `CamSim.Vegetation.Config`, `CamSim.Scene.Package`, `CamSim.Config` (the canonical-keys test reads `deploy/camsim_config.yaml`). Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Config unreal_project/CamSimTest/Source/CamSimTest/Tests/VegetationConfigTest.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/ScenePackageTest.cpp deploy/camsim_config.yaml docs/configuration.md
git commit -m "feat(vegetation): vegetation config and scene package seed (REALISM R4 slice)"
```

---

### Task 8: Streaming planner (pure state machine)

**Files:**
- Create: `Vegetation/VegetationStreaming.h/.cpp`
- Test: `Tests/VegetationStreamingTest.cpp`

**Interfaces:**
- Consumes: `CellsWithin`, `CellCentre`, `GroundDistanceM`, `FVegCellId` (Task 3).
- Produces: `enum class EVegCellState : uint8 { Generating, Sampling, ReadyToSpawn, Spawned, Empty }`; `class FVegetationStreaming` with `struct FParams { double RadiusM = 1000; double CellSizeM = 100; int32 MaxCellsInFlight = 4; int32 MaxSpawnsPerTick = 2; double DropFactor = 1.2; }`, `struct FActions { TArray<FVegCellId> ToGenerate, ToSpawn, ToDrop; }` (ToDrop lists only cells that had an actor), `explicit FVegetationStreaming(const FParams&)`, `FActions Tick(double CamLatDeg, double CamLonDeg)`, `void OnGenerated(FVegCellId, int32 NumInstances)`, `void OnHeightsSampled(FVegCellId, int32 NumGrounded)`, `TOptional<EVegCellState> GetState(FVegCellId) const`, `int32 NumInFlight() const`, `int32 NumTracked() const`.

Rules: `Tick` (1) drops every tracked cell whose centre is farther than `RadiusM × DropFactor` (removes it; `Spawned` ones go to `ToDrop`), (2) moves up to `MaxSpawnsPerTick` `ReadyToSpawn` cells, nearest first, to `Spawned` (`ToSpawn`), (3) starts wanted untracked cells (`CellsWithin(RadiusM)`, nearest first) as `Generating` while `NumInFlight() < MaxCellsInFlight` (`ToGenerate`). `OnGenerated` on a `Generating` cell: 0 → `Empty`, else → `Sampling`; on any other state or untracked id: ignored. `OnHeightsSampled` on a `Sampling` cell: 0 → `Empty`, else → `ReadyToSpawn`; otherwise ignored. In flight = `Generating` + `Sampling`. Because `GenerateCell` is deterministic, a stale result for a cell that was dropped and re-requested is identical to the fresh one, so accepting it is harmless.

- [ ] **Step 1: Write the failing tests**

`Tests/VegetationStreamingTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Vegetation/VegetationGrid.h"
#include "Vegetation/VegetationStreaming.h"

using namespace CamSimVegetation;

namespace
{
	/** Drives a planner with instant generation/heights: every cell gets N trees; returns the spawned set. */
	TSet<FVegCellId> RunToSteadyState(FVegetationStreaming& S, double Lat, double Lon, int32 Ticks, int32 N = 5)
	{
		TSet<FVegCellId> Spawned;
		for (int32 T = 0; T < Ticks; ++T)
		{
			const FVegetationStreaming::FActions A = S.Tick(Lat, Lon);
			for (const FVegCellId& C : A.ToDrop) Spawned.Remove(C);
			for (const FVegCellId& C : A.ToSpawn) Spawned.Add(C);
			for (const FVegCellId& C : A.ToGenerate) { S.OnGenerated(C, N); S.OnHeightsSampled(C, N); }
		}
		return Spawned;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegStreamBudgetsTest, "CamSim.Vegetation.Streaming.Budgets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegStreamBudgetsTest::RunTest(const FString& Parameters)
{
	FVegetationStreaming S({});
	const FVegetationStreaming::FActions A = S.Tick(33.302, -117.355);
	TestEqual(TEXT("first tick starts MaxCellsInFlight"), A.ToGenerate.Num(), 4);
	TestTrue(TEXT("nearest first"), A.ToGenerate.Num() > 0 && A.ToGenerate[0] == CellOf(33.302, -117.355, 100.0));
	TestEqual(TEXT("in flight"), S.NumInFlight(), 4);
	TestEqual(TEXT("nothing more while full"), S.Tick(33.302, -117.355).ToGenerate.Num(), 0);
	for (const FVegCellId& C : A.ToGenerate) { S.OnGenerated(C, 10); S.OnHeightsSampled(C, 10); }
	const FVegetationStreaming::FActions B = S.Tick(33.302, -117.355);
	TestEqual(TEXT("at most 2 spawns per tick"), B.ToSpawn.Num(), 2);
	TestEqual(TEXT("slots refilled"), B.ToGenerate.Num(), 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegStreamEmptyTest, "CamSim.Vegetation.Streaming.EmptyCellsFreeSlots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegStreamEmptyTest::RunTest(const FString& Parameters)
{
	FVegetationStreaming S({});
	int32 Ticks = 0, Spawns = 0;
	while (Ticks < 200)
	{
		const FVegetationStreaming::FActions A = S.Tick(33.302, -117.355);
		Spawns += A.ToSpawn.Num();
		for (const FVegCellId& C : A.ToGenerate) S.OnGenerated(C, 0);
		++Ticks;
		if (A.ToGenerate.Num() == 0 && S.NumInFlight() == 0) break;
	}
	TestEqual(TEXT("no spawns for empty cells"), Spawns, 0);
	TestTrue(*FString::Printf(TEXT("all ~314 cells resolved in %d ticks"), Ticks), Ticks < 100);
	TestEqual(TEXT("all tracked as empty"), S.NumTracked(), CellsWithin(33.302, -117.355, 1000.0, 100.0).Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegStreamPathIndependentTest, "CamSim.Vegetation.Streaming.PathIndependent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegStreamPathIndependentTest::RunTest(const FString& Parameters)
{
	// Arrive at the same point from the north and from the south: the same cells end up spawned.
	FVegetationStreaming North({}), South({});
	for (int32 K = 0; K <= 20; ++K) RunToSteadyState(North, 33.302 + 0.02 * (20 - K) / 20.0, -117.355, 5);
	for (int32 K = 0; K <= 20; ++K) RunToSteadyState(South, 33.302 - 0.02 * (20 - K) / 20.0, -117.355, 5);
	const TSet<FVegCellId> A = RunToSteadyState(North, 33.302, -117.355, 400);
	const TSet<FVegCellId> B = RunToSteadyState(South, 33.302, -117.355, 400);
	TSet<FVegCellId> Want(CellsWithin(33.302, -117.355, 1000.0, 100.0));
	TestTrue(TEXT("north path covers the wanted set"), Want.Difference(A).Num() == 0);
	TestTrue(TEXT("south path covers the wanted set"), Want.Difference(B).Num() == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegStreamHysteresisTest, "CamSim.Vegetation.Streaming.Hysteresis",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegStreamHysteresisTest::RunTest(const FString& Parameters)
{
	FVegetationStreaming S({});
	RunToSteadyState(S, 33.302, -117.355, 400);
	const int32 Before = S.NumTracked();
	// Move 150 m north: cells 1000-1200 m behind stay (hysteresis); none is dropped.
	const FVegetationStreaming::FActions A = S.Tick(33.302 + 150.0 / MetresPerDegLat, -117.355);
	TestEqual(TEXT("nothing dropped inside 1.2 R"), A.ToDrop.Num(), 0);
	TestTrue(TEXT("still tracked"), S.NumTracked() >= Before);
	// Move 400 m north: the southern edge (now > 1200 m away) drops.
	const FVegetationStreaming::FActions B = S.Tick(33.302 + 400.0 / MetresPerDegLat, -117.355);
	TestTrue(TEXT("far cells dropped"), B.ToDrop.Num() > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegStreamTeleportTest, "CamSim.Vegetation.Streaming.TeleportDropsAll",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegStreamTeleportTest::RunTest(const FString& Parameters)
{
	FVegetationStreaming S({});
	TSet<FVegCellId> Spawned = RunToSteadyState(S, 33.302, -117.355, 400);
	TestTrue(TEXT("steady state spawned something"), Spawned.Num() > 100);
	TestEqual(TEXT("steady state has nothing in flight"), S.NumInFlight(), 0);

	// Leave work genuinely in flight at the old place: move 300 m north (new cells enter the ring), tick once, and do NOT deliver
	// their results. One of them gets its generation result (so it sits in Sampling), the others stay Generating.
	const double NorthLat = 33.302 + 300.0 / MetresPerDegLat;
	const FVegetationStreaming::FActions Move = S.Tick(NorthLat, -117.355);
	for (const FVegCellId& C : Move.ToDrop) Spawned.Remove(C);
	for (const FVegCellId& C : Move.ToSpawn) Spawned.Add(C);
	TestTrue(TEXT("new cells started and not flushed"), Move.ToGenerate.Num() > 0 && S.NumInFlight() == Move.ToGenerate.Num());
	if (Move.ToGenerate.Num() == 0) return false;
	S.OnGenerated(Move.ToGenerate[0], 5);
	TestEqual(TEXT("one cell is Sampling"), static_cast<int32>(*S.GetState(Move.ToGenerate[0])), static_cast<int32>(EVegCellState::Sampling));
	const TArray<FVegCellId> InFlightOld = Move.ToGenerate;

	// Teleport 600 km away.
	const FVegetationStreaming::FActions A = S.Tick(37.7749, -122.4194);
	TestEqual(TEXT("every spawned cell dropped in one tick"), A.ToDrop.Num(), Spawned.Num());
	for (const FVegCellId& C : InFlightOld) TestFalse(TEXT("in-flight old cell is untracked after the teleport"), S.GetState(C).IsSet());
	for (const FVegCellId& C : Spawned) TestFalse(TEXT("spawned old cell is untracked after the teleport"), S.GetState(C).IsSet());

	// Results of the dropped work arrive late: they must change nothing.
	for (const FVegCellId& C : InFlightOld) { S.OnGenerated(C, 5); S.OnHeightsSampled(C, 5); }
	int32 OldSpawned = 0, OldTracked = 0;
	TSet<FVegCellId> Old(InFlightOld);
	Old.Append(Spawned);
	for (int32 T = 0; T < 5; ++T)
	{
		const FVegetationStreaming::FActions B = S.Tick(37.7749, -122.4194);
		for (const FVegCellId& C : B.ToSpawn) OldSpawned += Old.Contains(C);
		for (const FVegCellId& C : B.ToGenerate) OldSpawned += Old.Contains(C);
	}
	for (const FVegCellId& C : Old) OldTracked += S.GetState(C).IsSet();
	TestEqual(TEXT("no stale result spawns or restarts an old cell"), OldSpawned, 0);
	TestEqual(TEXT("no old cell tracked"), OldTracked, 0);
	return true;
}
```

- [ ] **Step 2: Run to see it fail**

Build: Expected FAIL — header not found.

- [ ] **Step 3: Implement**

`Vegetation/VegetationStreaming.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"
#include "Vegetation/VegetationGrid.h"

enum class EVegCellState : uint8
{
	Generating,     // GenerateCell running on a worker
	Sampling,       // heights requested from the terrain tileset
	ReadyToSpawn,   // grounded, waiting for a spawn slot
	Spawned,        // actor exists
	Empty,          // no grounded trees: nothing to spawn, never retried while tracked
};

/**
 * Which vegetation cells to generate, spawn and drop around the camera (REALISM R4 slice, docs/vegetation.md). Pure
 * state machine; the manager does the work and reports back. In flight (Generating + Sampling) is capped at
 * MaxCellsInFlight, spawns at MaxSpawnsPerTick, both nearest first; tracked cells beyond RadiusM * DropFactor drop.
 */
class CAMSIMTEST_API FVegetationStreaming
{
public:
	struct FParams
	{
		double RadiusM = 1000.0;
		double CellSizeM = 100.0;
		int32  MaxCellsInFlight = 4;
		int32  MaxSpawnsPerTick = 2;
		double DropFactor = 1.2;
	};

	struct FActions
	{
		TArray<CamSimVegetation::FVegCellId> ToGenerate;
		TArray<CamSimVegetation::FVegCellId> ToSpawn;
		TArray<CamSimVegetation::FVegCellId> ToDrop;   // cells that had an actor
	};

	explicit FVegetationStreaming(const FParams& InParams) : Params(InParams) {}

	FActions Tick(double CamLatDeg, double CamLonDeg);
	void OnGenerated(CamSimVegetation::FVegCellId Id, int32 NumInstances);
	void OnHeightsSampled(CamSimVegetation::FVegCellId Id, int32 NumGrounded);
	TOptional<EVegCellState> GetState(CamSimVegetation::FVegCellId Id) const;
	int32 NumInFlight() const;
	int32 NumTracked() const { return Cells.Num(); }

private:
	FParams Params;
	TMap<CamSimVegetation::FVegCellId, EVegCellState> Cells;
};
```

`Vegetation/VegetationStreaming.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Vegetation/VegetationStreaming.h"

using namespace CamSimVegetation;

FVegetationStreaming::FActions FVegetationStreaming::Tick(double CamLatDeg, double CamLonDeg)
{
	FActions A;
	auto DistTo = [&](FVegCellId Id) { double La, Lo; CellCentre(Id, Params.CellSizeM, La, Lo); return GroundDistanceM(CamLatDeg, CamLonDeg, La, Lo); };

	const double DropM = Params.RadiusM * Params.DropFactor;
	for (auto It = Cells.CreateIterator(); It; ++It)
	{
		if (DistTo(It.Key()) > DropM)
		{
			if (It.Value() == EVegCellState::Spawned) A.ToDrop.Add(It.Key());
			It.RemoveCurrent();
		}
	}

	TArray<TPair<double, FVegCellId>> Ready;
	for (const auto& Kv : Cells) if (Kv.Value == EVegCellState::ReadyToSpawn) Ready.Emplace(DistTo(Kv.Key), Kv.Key);
	Ready.Sort([](const auto& X, const auto& Y)
	{
		if (X.Key != Y.Key) return X.Key < Y.Key;
		return X.Value.Row != Y.Value.Row ? X.Value.Row < Y.Value.Row : X.Value.Col < Y.Value.Col;
	});
	for (int32 I = 0; I < Ready.Num() && I < Params.MaxSpawnsPerTick; ++I)
	{
		Cells[Ready[I].Value] = EVegCellState::Spawned;
		A.ToSpawn.Add(Ready[I].Value);
	}

	int32 InFlight = NumInFlight();
	if (InFlight < Params.MaxCellsInFlight)
	{
		for (const FVegCellId& Id : CellsWithin(CamLatDeg, CamLonDeg, Params.RadiusM, Params.CellSizeM))
		{
			if (InFlight >= Params.MaxCellsInFlight) break;
			if (Cells.Contains(Id)) continue;
			Cells.Add(Id, EVegCellState::Generating);
			A.ToGenerate.Add(Id);
			++InFlight;
		}
	}
	return A;
}

void FVegetationStreaming::OnGenerated(FVegCellId Id, int32 NumInstances)
{
	EVegCellState* S = Cells.Find(Id);
	if (S && *S == EVegCellState::Generating) *S = NumInstances > 0 ? EVegCellState::Sampling : EVegCellState::Empty;
}

void FVegetationStreaming::OnHeightsSampled(FVegCellId Id, int32 NumGrounded)
{
	EVegCellState* S = Cells.Find(Id);
	if (S && *S == EVegCellState::Sampling) *S = NumGrounded > 0 ? EVegCellState::ReadyToSpawn : EVegCellState::Empty;
}

TOptional<EVegCellState> FVegetationStreaming::GetState(FVegCellId Id) const
{
	const EVegCellState* S = Cells.Find(Id);
	return S ? TOptional<EVegCellState>(*S) : TOptional<EVegCellState>();
}

int32 FVegetationStreaming::NumInFlight() const
{
	int32 N = 0;
	for (const auto& Kv : Cells) N += Kv.Value == EVegCellState::Generating || Kv.Value == EVegCellState::Sampling;
	return N;
}
```

- [ ] **Step 4: Run tests**

Build; filter `CamSim.Vegetation.Streaming`. Expected: 5 PASS.

- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Vegetation/VegetationStreaming.* \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/VegetationStreamingTest.cpp
git commit -m "feat(vegetation): cell streaming planner with budgets and hysteresis (REALISM R4 slice)"
```

---

### Task 9: Terrain tileset tag and accessor

The scatter grounds trees on the **terrain** tileset. Once the buildings slice adds a second tileset, "the first cached tileset" is ambiguous, so the terrain gets an actor tag where it is chosen.

**Files:**
- Modify: `Geospatial/CamSimGeospatialProvider.h` (constant), `Geospatial/CamSimGeospatialProvider.cpp` (tag in `ApplyCesiumBackendConfig`)
- Modify: `Subsystem/CamSimSubsystem.h/.cpp` (`GetTerrainTileset`)
- Modify: `Vegetation/VegetationProbe.cpp` (Task 1's probe picks the terrain with `GetTerrainTileset()`)
- Test: `Tests/CesiumBackendConfigTest.cpp` (append)

**Interfaces:**
- Produces: `CamSim::Geospatial::TerrainTilesetTag` (`extern CAMSIMTEST_API const FName TerrainTilesetTag;` in the header, defined `const FName TerrainTilesetTag(TEXT("CamSimTerrain"));` in the .cpp, the same pattern as the buildings plan's `BuildingsTag`), `ACesium3DTileset* CamSim::Geospatial::PickTerrainTileset(TConstArrayView<ACesium3DTileset*> Tilesets)` (the one tagged; else the single tileset when there is exactly one; else null), `ACesium3DTileset* UCamSimSubsystem::GetTerrainTileset() const`.

- [ ] **Step 1: Write the failing test**

Append to `Tests/CesiumBackendConfigTest.cpp` (it already includes the provider header; add `#include "Cesium3DTileset.h"` and `#include "UObject/Package.h"` if missing):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPickTerrainTilesetTest, "CamSim.CesiumBackend.PickTerrainTileset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPickTerrainTilesetTest::RunTest(const FString& Parameters)
{
	using namespace CamSim::Geospatial;
	ACesium3DTileset* A = NewObject<ACesium3DTileset>(GetTransientPackage());
	ACesium3DTileset* B = NewObject<ACesium3DTileset>(GetTransientPackage());
	TestNull(TEXT("none"), PickTerrainTileset({}));
	TestTrue(TEXT("single untagged"), PickTerrainTileset({ A }) == A);
	TestNull(TEXT("two untagged: ambiguous"), PickTerrainTileset({ A, B }));
	B->Tags.Add(TerrainTilesetTag);
	TestTrue(TEXT("tagged wins"), PickTerrainTileset({ A, B }) == B);
	return true;
}
```

- [ ] **Step 2: Run to see it fail**

Build: Expected FAIL — `PickTerrainTileset` undefined.

- [ ] **Step 3: Implement**

In `Geospatial/CamSimGeospatialProvider.h`, inside `namespace CamSim::Geospatial` next to `SelectTerrainTileset`:

```cpp
	/** Actor tag ApplyCesiumBackendConfig puts on the terrain tileset (REALISM R4: vegetation grounds on it). */
	extern CAMSIMTEST_API const FName TerrainTilesetTag;
	/** The tileset tagged TerrainTilesetTag; else the only one; else nullptr. */
	ACesium3DTileset* PickTerrainTileset(TConstArrayView<ACesium3DTileset*> Tilesets);
```

(forward-declare `class ACesium3DTileset;` at the top if not already).

In `Geospatial/CamSimGeospatialProvider.cpp`, inside `if (ACesium3DTileset* Tileset = Tilesets.IsValidIndex(TerrainIndex) ? ... )`, first line of the block:

```cpp
		Tileset->Tags.AddUnique(CamSim::Geospatial::TerrainTilesetTag);
```

and the definitions in the `CamSim::Geospatial` namespace (next to the buildings plan's `BuildingsTag` if that has landed):

```cpp
	const FName TerrainTilesetTag(TEXT("CamSimTerrain"));

	ACesium3DTileset* PickTerrainTileset(TConstArrayView<ACesium3DTileset*> Tilesets)
	{
		for (ACesium3DTileset* T : Tilesets) if (T && T->Tags.Contains(TerrainTilesetTag)) return T;
		return Tilesets.Num() == 1 ? Tilesets[0] : nullptr;
	}
```

In `Subsystem/CamSimSubsystem.h` after `RefreshCachedTilesets()`:

```cpp
	/** The terrain tileset (tagged by ApplyCesiumBackendConfig), or nullptr. Game thread. */
	ACesium3DTileset* GetTerrainTileset() const;
```

In `Subsystem/CamSimSubsystem.cpp`:

```cpp
ACesium3DTileset* UCamSimSubsystem::GetTerrainTileset() const
{
	TArray<ACesium3DTileset*> Live;
	for (const TWeakObjectPtr<ACesium3DTileset>& W : GetCachedTilesets()) if (ACesium3DTileset* T = W.Get()) Live.Add(T);
	return CamSim::Geospatial::PickTerrainTileset(Live);
}
```

(include `Geospatial/CamSimGeospatialProvider.h` if not already.)

In `Vegetation/VegetationProbe.cpp` (Task 1, and Task 2 Step 5's edit of it), replace the terrain pick `Sub->GetCachedTilesets()[0].Get()` with the accessor, since the buildings slice (R3 Task 8) caches a second tileset by now and index 0 may be the buildings:

```cpp
		ACesium3DTileset* Terrain = Sub ? Sub->GetTerrainTileset() : nullptr;
```

Then `grep -rn "GetCachedTilesets()\[0\]" unreal_project/CamSimTest/Source/CamSimTest/Vegetation` must print nothing: no vegetation code may take a terrain from the cache by index.

- [ ] **Step 4: Run tests**

Build; filter `CamSim.CesiumBackend`. Expected: PASS (existing tests too).

- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Geospatial/CamSimGeospatialProvider.* \
  unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.* \
  unreal_project/CamSimTest/Source/CamSimTest/Vegetation/VegetationProbe.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/CesiumBackendConfigTest.cpp
git commit -m "feat(geospatial): tag the terrain tileset; UCamSimSubsystem::GetTerrainTileset (REALISM R4 slice)"
```

---

### Task 10: Vegetation manager and subsystem wiring

**Files:**
- Create: `Vegetation/VegetationManager.h/.cpp`
- Modify: `Subsystem/CamSimSubsystem.cpp` (own, configure lazily, tick, tear down)
- Test: `Tests/VegetationManagerTest.cpp`

**Interfaces:**
- Consumes: everything above; `UCamSimSubsystem::GetTerrainTileset()`, `GetCamera()`, `GetConfig()`; `CamSimMeshLoader::LoadStaticMesh`; (fallback) `CamSimVegetation::CreateProceduralTreeMesh`.
- Produces:
  - `struct FVegetationHooks` — the four side effects, injectable for tests:
    - `TFunction<void(FVegCellId, TFunction<void(TArray<FTreeInstance>)> Done)> Generate`
    - `TFunction<void(const TArray<FVector>& LonLatH, TFunction<void(TArray<TOptional<double>>)> Done)> SampleHeights`
    - `TFunction<void(FVegCellId, const FVegCellSpawn&)> Spawn`
    - `TFunction<void(FVegCellId)> Destroy`
  - `struct FVegCellSpawn { double CentreLatDeg, CentreLonDeg, BaseHeightM; TArray<FTransform> LocalInstances; TArray<FTreeInstance> Instances; TArray<double> HeightsM; }`
  - `class FVegetationManager : public TSharedFromThis<FVegetationManager>`: `FVegetationManager(const FVegetationStreaming::FParams&, FVegetationHooks)`, `void Tick(double CamLatDeg, double CamLonDeg)`, `int32 NumSpawned() const`, `FVegetationStreaming& GetStreaming()`.
  - Free function `TArray<FTransform> CamSimVegetation::ToCellLocal(double CentreLatDeg, double CentreLonDeg, double BaseHeightM, const TArray<FTreeInstance>&, const TArray<double>& HeightsM)` (ESU, cm).
  - `TSharedPtr<FVegetationManager> CamSimVegetation::CreateLiveManager(UWorld*, ACesium3DTileset* Terrain, const FCamSimConfig&, FString& OutWhyNot)` — builds real hooks (UE tasks + `FVegetationLandCover`, `SampleHeightMostDetailed`, `ACamSimVegetationCell`), or null with a reason.
  - CVars: `camsim.Vegetation.DumpPath` (string; when set, each spawn appends one JSON line), `camsim.Vegetation.GroundCheck` (int; when 1, once ≥ 200 instances are spawned, traces 200 of them against the terrain and logs `Vegetation: ground check n=… max_dz_m=… p50_dz_m=…`, then resets to 0).

- [ ] **Step 1: Write the failing tests**

`Tests/VegetationManagerTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Vegetation/VegetationGrid.h"
#include "Vegetation/VegetationManager.h"
#include "Vegetation/VegetationScatter.h"

using namespace CamSimVegetation;

namespace
{
	/** Fake hooks: generation and heights complete when Flush() is called (simulates async), spawns are recorded. */
	struct FFake
	{
		TArray<TFunction<void()>> Pending;
		TMap<FVegCellId, FVegCellSpawn> Live;
		TArray<FVegCellId> SpawnLog;   // every Spawn call, in order
		int32 Spawns = 0, Destroys = 0;
		int32 TreesPerCell = 6;
		bool bHeightsFail = false;

		FVegetationHooks Hooks()
		{
			FVegetationHooks H;
			H.Generate = [this](FVegCellId Id, TFunction<void(TArray<FTreeInstance>)> Done)
			{
				Pending.Add([this, Id, Done]()
				{
					TArray<FTreeInstance> T;
					double La, Lo; CellCentre(Id, 100.0, La, Lo);
					for (int32 K = 0; K < TreesPerCell; ++K) T.Add({ La + K * 1e-5, Lo, 10.0f * K, 1.0f, 0 });
					Done(MoveTemp(T));
				});
			};
			H.SampleHeights = [this](const TArray<FVector>& P, TFunction<void(TArray<TOptional<double>>)> Done)
			{
				const int32 N = P.Num();
				Pending.Add([this, N, Done]()
				{
					TArray<TOptional<double>> R;
					for (int32 K = 0; K < N; ++K) R.Add(bHeightsFail ? TOptional<double>() : TOptional<double>(100.0 + K));
					Done(MoveTemp(R));
				});
			};
			H.Spawn = [this](FVegCellId Id, const FVegCellSpawn& S) { ++Spawns; SpawnLog.Add(Id); Live.Add(Id, S); };
			H.Destroy = [this](FVegCellId Id) { ++Destroys; Live.Remove(Id); };
			return H;
		}
		void Flush() { TArray<TFunction<void()>> P = MoveTemp(Pending); for (auto& F : P) F(); }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegManagerPipelineTest, "CamSim.Vegetation.Manager.Pipeline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegManagerPipelineTest::RunTest(const FString& Parameters)
{
	FFake F;
	TSharedRef<FVegetationManager> M = MakeShared<FVegetationManager>(FVegetationStreaming::FParams{}, F.Hooks());
	for (int32 T = 0; T < 600; ++T) { const int32 Before = F.Spawns; M->Tick(33.302, -117.355); TestTrue(TEXT("<= 2 spawns per tick"), F.Spawns - Before <= 2); F.Flush(); }
	const int32 Want = CellsWithin(33.302, -117.355, 1000.0, 100.0).Num();
	TestEqual(TEXT("every wanted cell spawned"), F.Live.Num(), Want);
	const FVegCellSpawn* S = F.Live.Find(CellOf(33.302, -117.355, 100.0));
	TestNotNull(TEXT("camera cell spawned"), S);
	if (S)
	{
		TestEqual(TEXT("instances grounded"), S->LocalInstances.Num(), 6);
		TestEqual(TEXT("base = first height"), S->BaseHeightM, 100.0);
		TestEqual(TEXT("relative height of instance 3 (cm)"), S->LocalInstances[3].GetTranslation().Z, 300.0, 1e-6);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegManagerStaleTest, "CamSim.Vegetation.Manager.StaleResultsIgnored",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegManagerStaleTest::RunTest(const FString& Parameters)
{
	FFake F;
	TSharedRef<FVegetationManager> M = MakeShared<FVegetationManager>(FVegetationStreaming::FParams{}, F.Hooks());
	for (int32 T = 0; T < 600; ++T) { M->Tick(33.302, -117.355); F.Flush(); }
	TestTrue(TEXT("old place populated"), F.Live.Num() > 100);

	// Move 300 m north and tick WITHOUT flushing: new cells start generating and their completions stay pending.
	M->Tick(33.302 + 300.0 / MetresPerDegLat, -117.355);
	const int32 PendingAtOldPlace = F.Pending.Num();
	TestTrue(TEXT("generations are really pending at the old place"), PendingAtOldPlace > 0);
	const int32 SpawnsBefore = F.Spawns;

	// Teleport 600 km away: every actor goes, and the pending generations now belong to dropped cells.
	M->Tick(37.7749, -122.4194);
	TestEqual(TEXT("all old actors destroyed"), F.Live.Num(), 0);
	TestTrue(TEXT("the stale completions are still queued"), F.Pending.Num() >= PendingAtOldPlace);

	// Deliver the stale completions (generation, and any height request a wrong implementation would have made).
	for (int32 K = 0; K < 3; ++K) F.Flush();
	TestEqual(TEXT("a stale result spawned nothing"), F.Spawns, SpawnsBefore);

	// Then let the new place fill in: only cells near the new place may ever spawn.
	for (int32 T = 0; T < 600; ++T) { M->Tick(37.7749, -122.4194); F.Flush(); }
	TestTrue(TEXT("the new place populated"), F.Live.Num() > 100);
	for (int32 K = SpawnsBefore; K < F.SpawnLog.Num(); ++K)
	{
		double La, Lo; CellCentre(F.SpawnLog[K], 100.0, La, Lo);
		if (GroundDistanceM(37.7749, -122.4194, La, Lo) > 1200.0) { AddError(TEXT("a cell spawned at the old place after the teleport")); break; }
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegManagerFailedHeightsTest, "CamSim.Vegetation.Manager.FailedHeightsMakeEmpty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegManagerFailedHeightsTest::RunTest(const FString& Parameters)
{
	FFake F;
	F.bHeightsFail = true;
	TSharedRef<FVegetationManager> M = MakeShared<FVegetationManager>(FVegetationStreaming::FParams{}, F.Hooks());
	for (int32 T = 0; T < 600; ++T) { M->Tick(33.302, -117.355); F.Flush(); }
	TestEqual(TEXT("nothing spawned"), F.Spawns, 0);
	TestEqual(TEXT("no cell stuck in flight"), M->GetStreaming().NumInFlight(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegManagerEmptyTest, "CamSim.Vegetation.Manager.EmptyCellsCostNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegManagerEmptyTest::RunTest(const FString& Parameters)
{
	FFake F;
	F.TreesPerCell = 0;
	int32 HeightCalls = 0;
	FVegetationHooks H = F.Hooks();
	TFunction<void(const TArray<FVector>&, TFunction<void(TArray<TOptional<double>>)>)> Inner = H.SampleHeights;
	H.SampleHeights = [&HeightCalls, Inner](const TArray<FVector>& P, TFunction<void(TArray<TOptional<double>>)> D) { ++HeightCalls; Inner(P, D); };
	TSharedRef<FVegetationManager> M = MakeShared<FVegetationManager>(FVegetationStreaming::FParams{}, H);
	for (int32 T = 0; T < 200; ++T) { M->Tick(33.302, -117.355); F.Flush(); }
	TestEqual(TEXT("no spawns"), F.Spawns, 0);
	TestEqual(TEXT("no height requests for empty cells"), HeightCalls, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegManagerLocalFrameTest, "CamSim.Vegetation.Manager.ToCellLocal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegManagerLocalFrameTest::RunTest(const FString& Parameters)
{
	const double Lat0 = 33.3, Lon0 = -117.4;
	TArray<FTreeInstance> T = { { Lat0 + 10.0 / MetresPerDegLat, Lon0, 90.0f, 1.1f, 0 },   // 10 m north
	                            { Lat0, Lon0 + 20.0 / (MetresPerDegLat * FMath::Cos(FMath::DegreesToRadians(Lat0))), 0.0f, 1.0f, 0 } }; // 20 m east
	const TArray<FTransform> X = ToCellLocal(Lat0, Lon0, 50.0, T, { 52.0, 49.0 });
	TestEqual(TEXT("north -> -Y (ESU)"), X[0].GetTranslation().Y, -1000.0, 5.0);
	TestEqual(TEXT("north: no east"), X[0].GetTranslation().X, 0.0, 5.0);
	TestEqual(TEXT("north: +2 m up"), X[0].GetTranslation().Z, 200.0, 1e-6);
	TestEqual(TEXT("east -> +X"), X[1].GetTranslation().X, 2000.0, 5.0);
	TestEqual(TEXT("east: -1 m up"), X[1].GetTranslation().Z, -100.0, 1e-6);
	TestEqual(TEXT("scale"), X[0].GetScale3D().X, 1.1, 1e-6);
	return true;
}
```

(Tolerances of 5 cm absorb the small-area mapping vs the 111,320 m/deg approximation; both use the WGS-84 radii through `GeodeticToWindowEN`.)

- [ ] **Step 2: Run to see it fail**

Build: Expected FAIL — header not found.

- [ ] **Step 3: Implement the manager**

`Vegetation/VegetationManager.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"
#include "Templates/SharedPointer.h"
#include "Vegetation/VegetationGrid.h"
#include "Vegetation/VegetationScatter.h"
#include "Vegetation/VegetationStreaming.h"

class ACesium3DTileset;
class FCesiumSurfaceProbe;
class UWorld;
struct FCamSimConfig;

/** Everything a cell actor needs (REALISM R4 slice). LocalInstances are in the cell's ESU frame, cm. */
struct FVegCellSpawn
{
	double CentreLatDeg = 0.0;
	double CentreLonDeg = 0.0;
	double BaseHeightM = 0.0;
	TArray<FTransform> LocalInstances;
	TArray<CamSimVegetation::FTreeInstance> Instances;   // the grounded ones, same order
	TArray<double> HeightsM;                              // ellipsoid heights, same order
};

/** The manager's side effects; real ones from CreateLiveManager, fakes in tests. Done callbacks run on the game thread. */
struct FVegetationHooks
{
	TFunction<void(CamSimVegetation::FVegCellId, TFunction<void(TArray<CamSimVegetation::FTreeInstance>)>)> Generate;
	TFunction<void(const TArray<FVector>& LonLatH, TFunction<void(TArray<TOptional<double>>)>)> SampleHeights;
	TFunction<void(CamSimVegetation::FVegCellId, const FVegCellSpawn&)> Spawn;
	TFunction<void(CamSimVegetation::FVegCellId)> Destroy;
};

/**
 * Vegetation streaming around the camera (REALISM R4 slice, docs/vegetation.md): FVegetationStreaming decides, the hooks
 * act. Game thread. Results for a cell the planner no longer tracks are dropped by the planner's state checks.
 */
class CAMSIMTEST_API FVegetationManager : public TSharedFromThis<FVegetationManager>
{
public:
	FVegetationManager(const FVegetationStreaming::FParams& Params, FVegetationHooks InHooks);
	~FVegetationManager();

	void Tick(double CamLatDeg, double CamLonDeg);
	int32 NumSpawned() const { return Spawned.Num(); }
	FVegetationStreaming& GetStreaming() { return Streaming; }
	/** Spawned cells (for the ground check and dumps). */
	const TMap<CamSimVegetation::FVegCellId, FVegCellSpawn>& GetSpawned() const { return Spawned; }

private:
	void OnGenerated(CamSimVegetation::FVegCellId Id, TArray<CamSimVegetation::FTreeInstance> Instances);
	void OnHeights(CamSimVegetation::FVegCellId Id, TArray<CamSimVegetation::FTreeInstance> Instances, TArray<TOptional<double>> Heights);

	FVegetationStreaming Streaming;
	FVegetationHooks Hooks;
	double CellSizeM = 100.0;
	TMap<CamSimVegetation::FVegCellId, FVegCellSpawn> Ready;     // grounded, waiting for a spawn slot
	TMap<CamSimVegetation::FVegCellId, FVegCellSpawn> Spawned;
};

namespace CamSimVegetation
{
	CAMSIMTEST_API TArray<FTransform> ToCellLocal(double CentreLatDeg, double CentreLonDeg, double BaseHeightM,
		const TArray<FTreeInstance>& Instances, const TArray<double>& HeightsM);

	/** Real hooks: UE tasks + FVegetationLandCover, SampleHeightMostDetailed on Terrain, ACamSimVegetationCell. Game thread.
	 *  Null (with OutWhyNot) when vegetation is off, land cover is missing or the library doesn't load. */
	CAMSIMTEST_API TSharedPtr<FVegetationManager> CreateLiveManager(UWorld* World, ACesium3DTileset* Terrain,
		const FCamSimConfig& Cfg, FString& OutWhyNot);

	/** Game thread, once per tick after Tick(): camsim.Vegetation.GroundCheck. Waits until 200 instances are spawned,
	 *  traces their bases with Probe (the DIS clamp's complex ECC_Visibility trace), logs, resets the CVar. */
	CAMSIMTEST_API void RunDebugHooks(const FCesiumSurfaceProbe& Probe, FVegetationManager& Manager);
}
```

`Vegetation/VegetationManager.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Vegetation/VegetationManager.h"

#include "Cesium3DTileset.h"
#include "CesiumSampleHeightResult.h"
#include "Config/CamSimConfig.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Entity/EntityMeshLoader.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Tasks/Task.h"
#include "Thermal/LandCoverGeometry.h"
#include "Vegetation/VegetationCellActor.h"
#include "Vegetation/VegetationLandCover.h"
#include "Vegetation/VegetationLibrary.h"
#include "CamSimTest.h"

using namespace CamSimVegetation;

static TAutoConsoleVariable<FString> CVarVegDumpPath(TEXT("camsim.Vegetation.DumpPath"), TEXT(""),
	TEXT("When set, append one JSON line per spawned vegetation cell (row, col, hash, instances) to this file (REALISM R4)."));
static TAutoConsoleVariable<int32> CVarVegGroundCheck(TEXT("camsim.Vegetation.GroundCheck"), 0,
	TEXT("1: trace 200 spawned tree bases against the terrain once and log the height error (REALISM R4)."));

FVegetationManager::FVegetationManager(const FVegetationStreaming::FParams& Params, FVegetationHooks InHooks)
	: Streaming(Params), Hooks(MoveTemp(InHooks)), CellSizeM(Params.CellSizeM)
{
}

FVegetationManager::~FVegetationManager()
{
	for (const auto& Kv : Spawned) if (Hooks.Destroy) Hooks.Destroy(Kv.Key);
}

void FVegetationManager::Tick(double CamLatDeg, double CamLonDeg)
{
	const FVegetationStreaming::FActions A = Streaming.Tick(CamLatDeg, CamLonDeg);
	for (const FVegCellId& Id : A.ToDrop)
	{
		Spawned.Remove(Id);
		Hooks.Destroy(Id);
	}
	// Ready entries of dropped cells: the planner forgot them; forget their payload too.
	for (auto It = Ready.CreateIterator(); It; ++It) if (!Streaming.GetState(It.Key()).IsSet()) It.RemoveCurrent();
	for (const FVegCellId& Id : A.ToSpawn)
	{
		FVegCellSpawn S;
		if (Ready.RemoveAndCopyValue(Id, S))
		{
			Hooks.Spawn(Id, S);
			Spawned.Add(Id, MoveTemp(S));
		}
	}
	for (const FVegCellId& Id : A.ToGenerate)
	{
		TWeakPtr<FVegetationManager> Weak = AsWeak();
		Hooks.Generate(Id, [Weak, Id](TArray<FTreeInstance> T) { if (TSharedPtr<FVegetationManager> M = Weak.Pin()) M->OnGenerated(Id, MoveTemp(T)); });
	}
}

void FVegetationManager::OnGenerated(FVegCellId Id, TArray<FTreeInstance> Instances)
{
	if (Streaming.GetState(Id) != TOptional<EVegCellState>(EVegCellState::Generating)) return;
	Streaming.OnGenerated(Id, Instances.Num());
	if (Instances.Num() == 0) return;
	TArray<FVector> LonLatH;
	LonLatH.Reserve(Instances.Num());
	for (const FTreeInstance& T : Instances) LonLatH.Add(FVector(T.LonDeg, T.LatDeg, 0.0));
	TWeakPtr<FVegetationManager> Weak = AsWeak();
	Hooks.SampleHeights(LonLatH, [Weak, Id, Instances = MoveTemp(Instances)](TArray<TOptional<double>> H) mutable
	{
		if (TSharedPtr<FVegetationManager> M = Weak.Pin()) M->OnHeights(Id, MoveTemp(Instances), MoveTemp(H));
	});
}

void FVegetationManager::OnHeights(FVegCellId Id, TArray<FTreeInstance> Instances, TArray<TOptional<double>> Heights)
{
	if (Streaming.GetState(Id) != TOptional<EVegCellState>(EVegCellState::Sampling)) return;
	FVegCellSpawn S;
	CellCentre(Id, CellSizeM, S.CentreLatDeg, S.CentreLonDeg);
	for (int32 K = 0; K < Instances.Num() && K < Heights.Num(); ++K)
	{
		if (!Heights[K].IsSet() || !FMath::IsFinite(Heights[K].GetValue())) continue;
		S.Instances.Add(Instances[K]);
		S.HeightsM.Add(Heights[K].GetValue());
	}
	Streaming.OnHeightsSampled(Id, S.Instances.Num());
	if (S.Instances.Num() == 0) return;
	S.BaseHeightM = S.HeightsM[0];
	S.LocalInstances = ToCellLocal(S.CentreLatDeg, S.CentreLonDeg, S.BaseHeightM, S.Instances, S.HeightsM);
	Ready.Add(Id, MoveTemp(S));
}

namespace CamSimVegetation
{
	TArray<FTransform> ToCellLocal(double CentreLatDeg, double CentreLonDeg, double BaseHeightM,
		const TArray<FTreeInstance>& Instances, const TArray<double>& HeightsM)
	{
		CamSimLandCover::FWindowSpec W;
		W.CentreLatDeg = CentreLatDeg;
		W.CentreLonDeg = CentreLonDeg;
		TArray<FTransform> Out;
		Out.Reserve(Instances.Num());
		for (int32 K = 0; K < Instances.Num(); ++K)
		{
			const FVector2D EN = CamSimLandCover::GeodeticToWindowEN(W, Instances[K].LatDeg, Instances[K].LonDeg);
			// ESU: X = East, Y = South, Z = Up; cm. Yaw about Up (heading convention doesn't matter for a random yaw).
			Out.Add(FTransform(FRotator(0.0, Instances[K].YawDeg, 0.0),
				FVector(EN.X * 100.0, -EN.Y * 100.0, (HeightsM[K] - BaseHeightM) * 100.0), FVector(Instances[K].Scale)));
		}
		return Out;
	}

	TSharedPtr<FVegetationManager> CreateLiveManager(UWorld* World, ACesium3DTileset* Terrain, const FCamSimConfig& Cfg, FString& OutWhyNot)
	{
		const FCamSimConfig::FVegetationConfig& V = Cfg.Vegetation;
		if (!V.bEnabled) { OutWhyNot = TEXT("vegetation.enabled is false"); return nullptr; }
		if (!World || !Terrain) { OutWhyNot = TEXT("no world or terrain tileset"); return nullptr; }
		TSharedPtr<FVegetationLandCover, ESPMode::ThreadSafe> LandCover = MakeShared<FVegetationLandCover, ESPMode::ThreadSafe>(Cfg.Thermal.LandCover.Dir);
		if (!LandCover->IsValid()) { OutWhyNot = FString::Printf(TEXT("no land cover (%s)"), *LandCover->GetError()); return nullptr; }
		FVegetationLibrary Library; FString LibError;
		if (!LoadLibrary(ResolveLibraryDir(V.LibraryDir), Library, LibError)) { OutWhyNot = LibError; return nullptr; }
		const FVegetationType& Type = Library.Types[0];
		UStaticMesh* Mesh = nullptr;
#if CAMSIM_VEGETATION_PROCEDURAL
		if (Type.bProcedural) Mesh = CreateProceduralTreeMesh(Type.HeightM);
#endif
		if (!Type.bProcedural) Mesh = CamSimMeshLoader::LoadStaticMesh(Type.GltfPath);
		if (!Mesh) { OutWhyNot = FString::Printf(TEXT("could not load tree type '%s'"), *Type.Name); return nullptr; }

		FScatterParams P;
		P.CellSizeM = V.CellSizeM;
		P.DensityScale = V.DensityScale;
		const uint32 Seed = VegetationSeed(Cfg.Scene.bHasSeed ? static_cast<uint32>(Cfg.Scene.Seed) : static_cast<uint32>(V.Seed));
		FVegetationStreaming::FParams SP;
		SP.RadiusM = V.RadiusM;
		SP.CellSizeM = V.CellSizeM;

		TStrongObjectPtr<UStaticMesh> KeepMesh(Mesh);
		TWeakObjectPtr<UWorld> WeakWorld(World);
		TWeakObjectPtr<ACesium3DTileset> WeakTerrain(Terrain);
		TSharedRef<TMap<FVegCellId, TWeakObjectPtr<ACamSimVegetationCell>>> Actors = MakeShared<TMap<FVegCellId, TWeakObjectPtr<ACamSimVegetationCell>>>();
		const bool bShadows = V.bCastShadows;
		const float CullM = V.RadiusM;

		FVegetationHooks H;
		H.Generate = [LandCover, P, Seed](FVegCellId Id, TFunction<void(TArray<FTreeInstance>)> Done)
		{
			UE::Tasks::Launch(UE_SOURCE_LOCATION, [LandCover, P, Seed, Id, Done = MoveTemp(Done)]() mutable
			{
				TArray<FTreeInstance> T = GenerateCell(Seed, Id, P, [&LandCover](double La, double Lo) { return LandCover->Code(La, Lo); });
				AsyncTask(ENamedThreads::GameThread, [Done = MoveTemp(Done), T = MoveTemp(T)]() mutable { Done(MoveTemp(T)); });
			});
		};
		H.SampleHeights = [WeakTerrain](const TArray<FVector>& LonLatH, TFunction<void(TArray<TOptional<double>>)> Done)
		{
			ACesium3DTileset* T = WeakTerrain.Get();
			if (!T) { TArray<TOptional<double>> None; None.SetNum(LonLatH.Num()); Done(MoveTemp(None)); return; }
			T->SampleHeightMostDetailed(LonLatH, FCesiumSampleHeightMostDetailedCallback::CreateLambda(
				[Done = MoveTemp(Done)](ACesium3DTileset*, const TArray<FCesiumSampleHeightResult>& R, const TArray<FString>&)
				{
					TArray<TOptional<double>> Out;
					for (const FCesiumSampleHeightResult& X : R) Out.Add(X.SampleSuccess ? TOptional<double>(X.LongitudeLatitudeHeight.Z) : TOptional<double>());
					Done(MoveTemp(Out));
				}));
		};
		H.Spawn = [WeakWorld, KeepMesh, Actors, bShadows, CullM](FVegCellId Id, const FVegCellSpawn& S)
		{
			UWorld* W = WeakWorld.Get();
			if (!W) return;
			ACamSimVegetationCell* Cell = W->SpawnActor<ACamSimVegetationCell>();
			if (!Cell) return;
			Cell->Init(S.CentreLatDeg, S.CentreLonDeg, S.BaseHeightM, KeepMesh.Get(), S.LocalInstances, bShadows, CullM);
			Actors->Add(Id, Cell);
			const FString Dump = CVarVegDumpPath.GetValueOnGameThread();
			if (!Dump.IsEmpty())
			{
				FString Line = FString::Printf(TEXT("{\"row\":%d,\"col\":%d,\"hash\":%u,\"instances\":["), Id.Row, Id.Col, HashInstances(S.Instances));
				for (int32 K = 0; K < S.Instances.Num(); ++K)
					Line += FString::Printf(TEXT("%s[%.9f,%.9f,%.3f,%.4f,%.4f]"), K ? TEXT(",") : TEXT(""), S.Instances[K].LatDeg,
						S.Instances[K].LonDeg, S.HeightsM[K], S.Instances[K].YawDeg, S.Instances[K].Scale);
				Line += TEXT("]}\n");
				FFileHelper::SaveStringToFile(Line, *Dump, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
			}
		};
		H.Destroy = [Actors](FVegCellId Id)
		{
			TWeakObjectPtr<ACamSimVegetationCell> A;
			if (Actors->RemoveAndCopyValue(Id, A) && A.IsValid()) A->Destroy();
		};
		UE_LOG(LogCamSim, Log, TEXT("Vegetation: on (type '%s', radius %.0f m, density %.2f, shadows %d, seed %u, land cover %s)"),
			*Type.Name, V.RadiusM, V.DensityScale, bShadows ? 1 : 0, Seed, *Cfg.Thermal.LandCover.Dir);
		return MakeShared<FVegetationManager>(SP, MoveTemp(H));
	}

	void RunDebugHooks(const FCesiumSurfaceProbe& Probe, FVegetationManager& Manager)
	{
		if (CVarVegGroundCheck.GetValueOnGameThread() == 0) return;
		int32 Available = 0;
		for (const auto& Kv : Manager.GetSpawned()) Available += Kv.Value.Instances.Num();
		if (Available < 200) return;   // wait for enough trees; the CVar stays set
		CVarVegGroundCheck->Set(0);
		TArray<double> Dz;
		for (const auto& Kv : Manager.GetSpawned())
		{
			for (int32 K = 0; K < Kv.Value.Instances.Num() && Dz.Num() < 200; ++K)
			{
				const FTreeInstance& T = Kv.Value.Instances[K];
				const double H = Kv.Value.HeightsM[K];
				const TOptional<double> G = Probe.TraceHeight(T.LatDeg, T.LonDeg, H + 50.0, H - 50.0);
				if (G.IsSet()) Dz.Add(FMath::Abs(G.GetValue() - H));
			}
			if (Dz.Num() >= 200) break;
		}
		Dz.Sort();
		UE_LOG(LogCamSim, Log, TEXT("Vegetation: ground check n=%d max_dz_m=%.3f p50_dz_m=%.3f"), Dz.Num(),
			Dz.Num() ? Dz.Last() : -1.0, Dz.Num() ? Dz[Dz.Num() / 2] : -1.0);
	}
}
```

`#include "Entity/SurfaceProbe.h"` in `VegetationManager.cpp`. Trees have no collision, so the trace hits the terrain. Once the buildings slice lands, `FCesiumSurfaceProbe` ignores the buildings tileset (buildings plan Task 10, `CamSimSurface::MakeProbeQueryParams`), so the ground check still measures terrain; a hit far above H is reported, not hidden.

Procedural switch: define `CAMSIM_VEGETATION_PROCEDURAL` as `1` at the top of `VegetationManager.cpp` (with `#include "Vegetation/ProceduralTree.h"`) only if Task 2 was done; otherwise `0` and keep the `#if`.

- [ ] **Step 4: Wire into the subsystem**

In `Subsystem/CamSimSubsystem.cpp`: `#include "Vegetation/VegetationManager.h"` and `#include "Entity/SurfaceProbe.h"`; add to `FSubsystemImpl`:

```cpp
	// Vegetation scatter (REALISM R4 slice); created lazily once the world's terrain tileset exists, null when off.
	TSharedPtr<FVegetationManager> Vegetation;
	bool bVegetationTried = false;
	TUniquePtr<FCesiumSurfaceProbe> VegetationProbe;   // ground check only
```

In `~FSubsystemImpl()` before `CesiumIonServerOverride.Reset();` add `Vegetation.Reset();` (destroys cell actors). In `Deinitialize()` inside `if (Impl)` add `Impl->Vegetation.Reset();`.

At the end of `UCamSimSubsystem::Tick` (after the watchdog block):

```cpp
	// Vegetation (REALISM R4 slice): around the camera's nadir; configured once the terrain tileset exists.
	if (Config.Vegetation.bEnabled)
	{
		UWorld* World = GetWorld();
		ACesium3DTileset* Terrain = GetTerrainTileset();
		if (!Impl->bVegetationTried && World && Terrain && Camera_.IsValid())
		{
			Impl->bVegetationTried = true;
			FString WhyNot;
			Impl->Vegetation = CamSimVegetation::CreateLiveManager(World, Terrain, Config, WhyNot);
			if (!Impl->Vegetation) UE_LOG(LogCamSim, Warning, TEXT("Vegetation: off (%s)"), *WhyNot);
		}
		if (Impl->Vegetation)
		{
			const FCamSimTelemetry& T = Camera_->GetTelemetry();
			if (CamSimVegetation::IsLatAllowed(T.Latitude)) Impl->Vegetation->Tick(T.Latitude, T.Longitude);
			if (!Impl->VegetationProbe)
				Impl->VegetationProbe = MakeUnique<FCesiumSurfaceProbe>(World, GetGeospatialProvider(), Config.bCreatePhysicsMeshes);
			CamSimVegetation::RunDebugHooks(*Impl->VegetationProbe, *Impl->Vegetation);
		}
	}
```

(These are the same constructor arguments `FCamSimEntityManager` passes, `Entity/CamSimEntityManager.cpp:316`.)

- [ ] **Step 5: Run unit tests**

Build; filter `CamSim.Vegetation`. Expected: all Vegetation tests PASS (Grid 5, Scatter 3, LandCover 3, Library 4, Config 3, Streaming 5, Manager 5, + ProceduralTree 1 if Task 2). Then the full suite, filter `CamSim`: Expected: no regressions (NullRHI: the manager never starts, since there is no camera/terrain).

- [ ] **Step 6: Live smoke**

```bash
mkdir -p .cache/veg/smoke
CAMSIM_SCENE_DIR="$PKG" CAMSIM_MULTICAST_ADDR=127.0.0.1 CAMSIM_SNAPSHOT_ENDPOINT_ENABLED=1 \
  scripts/run.sh --headless --local --detach "-ExecCmds=camsim.Vegetation.DumpPath $PWD/.cache/veg/smoke/cells.jsonl"
scripts/send_cigi_test.py --lat 33.302 --lon -117.355 --alt 350 --pitch -20 --duration 90 &
sleep 75; curl -s -o .cache/veg/smoke/snap.png http://127.0.0.1:8080/snapshot; scripts/stop.sh
grep "Vegetation" ~/Library/Logs/CamSimTest/CamSimTest.log | head; wc -l .cache/veg/smoke/cells.jsonl
```

Expected: `Vegetation: on (type 'tree_broadleaf_a', radius 1000 m, …)`, a non-empty `cells.jsonl`, trees along the river in `snap.png`, no `Vegetation: off`.

- [ ] **Step 7: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Vegetation/VegetationManager.* \
  unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/VegetationManagerTest.cpp
git commit -m "feat(vegetation): streaming manager, live hooks and subsystem wiring (REALISM R4 slice)"
```

---

### Task 11: Live acceptance (`scripts/vegetation_check.py`) and docs

**Files:**
- Create: `scripts/vegetation_check.py`
- Modify: `docs/vegetation.md` (complete), `docs/configuration.md` (link), `ROADMAP.md` (Realism track: R4 slice status; the `Editor changes needed (human)` section that Task 1 Step 9 creates, if Task 1 needed it), `REALISM.md` (R4 status line), `CLAUDE.md` (command row, Architecture `Vegetation/`, test counts, a Gotchas entry)

**Interfaces:**
- Consumes: `scripts/bench/run_bench.py` (`Host`, `wait_ready`, `wait_terrain`, `wait_port_free`, `HEALTH`), `scripts/bench/scenario.Pose`, `scripts/bench/compare.ssim` (8x8-block mean SSIM, the bench's metric), `scripts/bench/dense_path.LandCover` (bench plan Task 3: `LandCover(root).codes(lon, lat) -> uint8 array`, 0 where the tile is missing; `root` is the package's `landcover/` directory), CVars `camsim.Vegetation.DumpPath`, `camsim.Vegetation.GroundCheck`, the land-cover tiles of `$PKG/landcover/`.
- Produces: `.cache/veg/check/<run>/report.json` with gates `placement`, `grounding`, `determinism`, `origin_shift`, `off_switch`, `gt` (pass/fail + numbers) and shots.

Gates (spec "Acceptance"):
1. **placement:** every dumped instance's (lat, lon) maps to code 10 in `$PKG/landcover`, read with the bench's `dense_path.LandCover` (one decoder for tile naming and cell math; no second implementation here).
2. **grounding:** `-ExecCmds=camsim.Vegetation.GroundCheck 1` at launch; `RunDebugHooks` waits until 200 instances are spawned, then traces them once → log line `max_dz_m < 0.5`, `n == 200`.
3. **determinism:** two runs: approach the riparian pose from 3 km north and from 3 km south (host poses interpolated over 30 s), dump; for every cell present in both dumps, `hash` equal.
4. **origin_shift:** at the riparian pose take `/snapshot` A, fly to the `far_origin` site (`lon + 3.41`) for 20 s, fly back, take `/snapshot` B; SSIM(A, B) ≥ the baseline run-to-run SSIM from Task 1 Step 0 minus 0.01, and the dump shows the same cell hashes.
5. **off_switch:** `CAMSIM_VEGETATION_ENABLED=0` run of the bench smoke shots vs Task 1's baseline shots: SSIM ≥ the baseline's own pairwise SSIM minus 0.005 for every shot (live frames are not bit-identical run to run; this is the R2 practice, see spec gaps).
6. **gt:** run `scripts/gt_occlusion_check.py OUTDIR` with vegetation on (default) → exit 0. It needs a required `OUTDIR` argument and `pycocotools`, so it runs as `uv run --with numpy --with pillow --with pycocotools python scripts/gt_occlusion_check.py <outdir>` (its documented command), not under `--project scripts/scene`; it launches its own CamSim runs, so none of this script's CamSims may be running.

- [ ] **Step 1: Write `scripts/vegetation_check.py`**

```python
#!/usr/bin/env python3
# Copyright CamSim Contributors. All Rights Reserved.
"""Vegetation acceptance (REALISM R4 slice, docs/vegetation.md): placement on code 10, grounding, determinism from two
approach paths, origin-shift stability, off switch vs the pre-slice baseline, ground-truth check still passing.

  uv run --project scripts/scene python scripts/vegetation_check.py --scene "$PKG" --baseline .cache/veg/baseline \
      --out .cache/veg/check
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

import numpy as np
from PIL import Image

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts"))
from bench import dense_path as dp  # noqa: E402
from bench import run_bench as rb  # noqa: E402
from bench import scenario  # noqa: E402
from bench.compare import ssim  # noqa: E402

SHOT = "nadir_3km.png"  # the one --smoke shot; named explicitly, never by glob order (each shot is saved twice, with and without _sensor)
EDITOR_LOG = Path.home() / "Library/Logs/CamSimTest/CamSimTest.log"
RIPARIAN = (33.302, -117.355)  # Santa Margarita River riparian woodland (MCAS Camp Pendleton)
ALT_M = 350.0
PITCH = -20.0


def load(path: Path) -> np.ndarray:
    """A shot as an RGB array for compare.ssim (which takes the luma and averages 8x8 blocks itself)."""
    return np.asarray(Image.open(path).convert("RGB"))


def pose(lat: float, lon: float) -> scenario.Pose:
    return scenario.Pose(lat, lon, ALT_M, yaw=0.0, gimbal_pitch=PITCH)


def launch(env_extra: dict, exec_cmds: list[str], host: rb.Host) -> None:
    env = dict(os.environ, CAMSIM_MULTICAST_ADDR="127.0.0.1", CAMSIM_SNAPSHOT_ENDPOINT_ENABLED="1", **env_extra)
    rb.wait_port_free(int(rb.HEALTH.rsplit(":", 1)[1]))
    host.thread.start()
    extra = [f"-ExecCmds={','.join(exec_cmds)}"] if exec_cmds else []
    subprocess.run([str(REPO / "scripts/run.sh"), "--headless", "--local", "--detach", *extra], env=env, check=True,
                   stdout=subprocess.DEVNULL)
    rb.wait_ready(REPO / ".cache" / "camsim.pid")
    rb.wait_terrain(120)


def stop(host: rb.Host) -> None:
    host.stop.set()
    subprocess.run([str(REPO / "scripts/stop.sh")], check=False, stdout=subprocess.DEVNULL)


def snapshot(dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen("http://127.0.0.1:8080/snapshot", timeout=30) as r:
        dest.write_bytes(r.read())


def fly(host: rb.Host, start: tuple[float, float], end: tuple[float, float], seconds: float) -> None:
    t0 = time.monotonic()
    while (t := time.monotonic() - t0) < seconds:
        f = t / seconds
        host.pose = pose(start[0] + f * (end[0] - start[0]), start[1] + f * (end[1] - start[1]))
        time.sleep(0.1)
    host.pose = pose(*end)


def read_dump(path: Path) -> dict[tuple[int, int], dict]:
    cells = {}
    for line in path.read_text().splitlines():
        d = json.loads(line)
        cells[(d["row"], d["col"])] = d
    return cells


def run_approach(label: str, scene: Path, out: Path, d_lat: float) -> dict:
    dump = out / label / "cells.jsonl"
    dump.parent.mkdir(parents=True, exist_ok=True)
    dump.unlink(missing_ok=True)
    host = rb.Host(scenario.SITES["pendleton"])
    host.pose = pose(RIPARIAN[0] + d_lat, RIPARIAN[1])
    try:
        launch({"CAMSIM_SCENE_DIR": str(scene)}, [f"camsim.Vegetation.DumpPath {dump}", "camsim.Vegetation.GroundCheck 1"], host)
        fly(host, (RIPARIAN[0] + d_lat, RIPARIAN[1]), RIPARIAN, 30.0)
        time.sleep(20.0)
        snapshot(out / label / "a.png")
        far = (RIPARIAN[0], RIPARIAN[1] + 3.41)
        host.pose = pose(*far); time.sleep(20.0)
        host.pose = pose(*RIPARIAN); time.sleep(25.0)
        snapshot(out / label / "b.png")
    finally:
        stop(host)
    log = EDITOR_LOG.read_text(errors="replace")
    ground = [l for l in log.splitlines() if "Vegetation: ground check" in l]
    return {"dump": str(dump), "ground": ground[-1] if ground else None}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--scene", type=Path, required=True)
    ap.add_argument("--baseline", type=Path, required=True, help="Task 1 Step 0 runs (a/, b/ with shots/)")
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    report: dict = {}

    north = run_approach("north", a.scene, a.out, +3000.0 / 111_320.0)
    south = run_approach("south", a.scene, a.out, -3000.0 / 111_320.0)
    dn, ds = read_dump(Path(north["dump"])), read_dump(Path(south["dump"]))

    lc = dp.LandCover(a.scene / "landcover")
    inst = [i for d in list(dn.values()) + list(ds.values()) for i in d["instances"]]  # each i = [lat, lon, ...]
    codes = lc.codes(np.array([i[1] for i in inst], float), np.array([i[0] for i in inst], float)) if inst else np.zeros(0)
    wrong, total = int((codes != 10).sum()), len(inst)
    report["placement"] = {"pass": total > 0 and wrong == 0, "instances": total, "not_on_code_10": wrong}

    def parse_ground(line: str | None) -> dict:
        if not line:
            return {"pass": False, "why": "no ground check line"}
        kv = dict(p.split("=") for p in line.split("ground check ")[1].split())
        return {"pass": int(kv["n"]) == 200 and float(kv["max_dz_m"]) < 0.5, **kv}
    report["grounding"] = {"north": parse_ground(north["ground"]), "south": parse_ground(south["ground"])}
    report["grounding"]["pass"] = report["grounding"]["north"]["pass"] and report["grounding"]["south"]["pass"]

    common = set(dn) & set(ds)
    mismatched = [k for k in common if dn[k]["hash"] != ds[k]["hash"]]
    report["determinism"] = {"pass": len(common) > 20 and not mismatched, "common_cells": len(common), "mismatched": len(mismatched)}

    floor = ssim(load(a.baseline / "a" / "shots" / SHOT), load(a.baseline / "b" / "shots" / SHOT))
    s_n = ssim(load(a.out / "north/a.png"), load(a.out / "north/b.png"))
    report["origin_shift"] = {"pass": s_n >= floor - 0.01, "ssim": s_n, "baseline_floor": floor}

    off = a.out / "off"
    subprocess.run([sys.executable, str(REPO / "scripts/bench/run_bench.py"), "--label", "veg-off", "--site", "pendleton",
                    "--smoke", "--out", str(off)], env={**os.environ, "CAMSIM_SCENE_DIR": str(a.scene),
                                                        "CAMSIM_VEGETATION_ENABLED": "0"}, check=True)
    rows = []
    for shot in sorted(p for p in (a.baseline / "a" / "shots").glob("*.png") if not p.stem.endswith("_sensor")):
        b_floor = ssim(load(shot), load(a.baseline / "b" / "shots" / shot.name))
        s_off = ssim(load(shot), load(off / "shots" / shot.name))
        rows.append({"shot": shot.name, "ssim_off": s_off, "baseline": b_floor, "pass": s_off >= b_floor - 0.005})
    report["off_switch"] = {"pass": bool(rows) and all(r["pass"] for r in rows), "shots": rows}

    gt = subprocess.run(["uv", "run", "--with", "numpy", "--with", "pillow", "--with", "pycocotools", "python",
                         str(REPO / "scripts/gt_occlusion_check.py"), str(a.out / "gt")], check=False)
    report["gt"] = {"pass": gt.returncode == 0, "exit": gt.returncode}

    report["pass"] = all(v["pass"] for v in report.values() if isinstance(v, dict) and "pass" in v)
    (a.out / "report.json").write_text(json.dumps(report, indent=2))
    print(json.dumps({k: v["pass"] if isinstance(v, dict) else v for k, v in report.items()}, indent=2))
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
```

(Checked while writing the plan: `run_bench.py` uses `REPO/.cache/camsim.pid`, `scenario.Pose` has `yaw` and `gimbal_pitch`, and `--smoke` still captures shots via `build_shots(smoke=True)`.)

- [ ] **Step 2: Run it**

```bash
uv run --project scripts/scene python scripts/vegetation_check.py --scene "$PKG" --baseline .cache/veg/baseline --out .cache/veg/check
```

Expected: every gate `true`, exit 0. On a failure: read `report.json`, fix the cause (not the threshold), rerun.

- [ ] **Step 3: Truck under trees (reported, not gated)**

Run `send_dis_test.py truck-parked` relocated under trees at the riparian pose (edit only its start lat/lon via its CLI flags if it has them; otherwise pass the pose through env as `dis_vehicle_check.py` does) with ML on; from the COCO output record the truck's `visibility` (< 1 expected) and box, plus a crop, in `docs/vegetation.md` "Measured".

- [ ] **Step 4: Bench sweep**

Owned by the bench plan (`docs/superpowers/specs/2026-10-10-r3r4-realism-cost-bench-design.md`): run its `sweep.py` with the `trees*` axes, plus `--axis buildings=off` so the baseline is all-off even on a package with a buildings layer (R3 has landed and buildings default on): `uv run --with numpy --with pillow python scripts/bench/sweep.py --scene "$PKG" --out .cache/bench-r3r4/trees --axis buildings=off --axis trees=off,on --axis trees_radius=500,1000,2000 --axis trees_density=0.5,1.0 --axis trees_shadows=on,off --repeat 2`. Record the chosen defaults here. If they differ from the starting values (radius 1000, density 1.0, shadows on), change all of these together in this step: the `FVegetationConfig` defaults in `Config/CamSimConfig.h`, `deploy/camsim_config.yaml`, `docs/configuration.md`, and the `CamSim.Vegetation.Config.Defaults` assertions in `Tests/VegetationConfigTest.cpp` (Task 7); then rebuild and rerun `CamSim.Vegetation.Config` + `CamSim.Config` until green.

- [ ] **Step 5: Docs**

- `docs/vegetation.md`: purpose, how it works (grid, scatter, seed, heights, cells, budgets), config table, the library format and how to add a type (`prepare_tree.py`), CVars (`DumpPath`, `GroundCheck`, `Probe`), "Measured" (gates from `report.json`, truck-under-trees result, bench rows), "Known limits" exactly as the spec lists them (one species, binary density per 10 m texel, hard pop-in, no collision, `/ready` doesn't wait, leaf-on).
- `ROADMAP.md` "Realism track": a dated R4-slice status line linking `docs/vegetation.md`; if Task 1 found the cooked usage-flag problem, check that its human editor task is in the `## Editor changes needed (human)` section of `ROADMAP.md` (Task 1 Step 9 creates the section; if it is missing, create it now, per `CLAUDE.md`).
- `REALISM.md` R4: "Slice implemented 2026-…" with the gate summary and what stays deferred.
- `CLAUDE.md`: `scripts/vegetation_check.py` row in Commands; `Vegetation/` line in Architecture (`Vegetation/  # REALISM R4 slice: global cell grid, hash scatter on land-cover code 10, HISM cells, library.json`); test count (`N tests across M files`, from the automation report); a Gotchas bullet: "**Vegetation** (REALISM R4 slice, `docs/vegetation.md`): trees are scattered per 100 m global cell by `CamSimHash` (streams 80–82) on land-cover code 10, grounded with `SampleHeightMostDetailed` on the terrain tileset (tagged `CamSimTerrain`), one globe-anchored HISM actor per cell, ≤ 2 spawns/tick, ≤ 4 cells in flight; no collision (CIGI/DIS never see trees), `/ready` doesn't wait for them. The library (`Content/NonUFS/Vegetation`, git LFS) is sha256-checked; CC0 only."

- [ ] **Step 6: Final checks and commit**

Run the full automation suite (filter `CamSim`), `scripts/run_gpu_tests.sh` (no vegetation GPU test, but confirm no regression), and the scene tooling tests are unaffected (no change there). Expected: all pass.

```bash
git add scripts/vegetation_check.py docs/vegetation.md docs/configuration.md ROADMAP.md REALISM.md CLAUDE.md
# only if Step 4 changed the defaults (nothing else in this task touches C++):
git add deploy/camsim_config.yaml unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.h \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/VegetationConfigTest.cpp
git commit -m "docs(vegetation): acceptance on Pendleton, vegetation guide (REALISM R4 slice)"
```

---

## Self-review (done while writing)

- **Spec coverage:** library + CC0 tree + risk check + procedural fallback (Tasks 1–2, 6); global grid (3); pure `GenerateCell`, jitter/accept/yaw/scale, p_tree, spacing (4); worker-side sampler with own 16-tile LRU (5); seed from manifest / config (4, 7); heights via `SampleHeightMostDetailed`, failed samples dropped (10); cells: globe anchor + HISM, ESU frame, survive origin shifts (1, 10, 11 gate 4); budgets 2 spawns / 4 in flight, hysteresis 1.2 R (8); collision off, shadows config, hard cull, no stencil (1); not hit by queries (collision off, 1); `/ready` not waiting (not wired into the gate); no land cover → off with a log line (10); config keys + env (7); determinism / density / grid / wanted-drop / budget / config / library tests (3–8, 10); acceptance 1–8 (11; 6 bench via the bench plan; 8 visual review is the user's, using Task 11's shots); known limits into `docs/vegetation.md` (11).
- **Placeholder scan:** the only values filled at execution time are measured ones (the asset's sha256 printed by `prepare_tree.py`; bench-chosen defaults); each step says where they come from. External APIs used (send_cigi_test flags, bench pid file, `scenario.Pose`, `av_sha_update`, `FCesiumSurfaceProbe` ctor, `SampleHeightMostDetailed`) were checked against the code while writing the plan.
- **Type consistency:** `FVegCellId`, `FTreeInstance`, `FScatterParams`, `FVegetationStreaming::FParams/FActions`, `FVegCellSpawn`, `FVegetationHooks`, `EVegCellState` are used with the same names and members throughout.
