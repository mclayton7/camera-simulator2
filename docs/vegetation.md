# Vegetation (trees)

REALISM R4 slice. Instanced trees scattered around the camera from land cover, loaded at runtime
from a mounted vegetation library (glTF through glTFRuntime) into one HISM per cell. Plan:
[`superpowers/plans/2026-10-10-r4-trees-slice.md`](superpowers/plans/2026-10-10-r4-trees-slice.md).

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
