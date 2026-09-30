# Copyright CamSim Contributors. All Rights Reserved.
"""Generate /Game/Ocean/MPC_Ocean and /Game/Ocean/M_Ocean (UE editor Python).

This script is the source of truth for both assets; the committed .uasset files are its output.
Run it with scripts/ocean/make_ocean_material.sh, which uses the pythonscript commandlet:

    UnrealEditor CamSimTest.uproject -run=pythonscript -script=<this file> -unattended ...

(That form works on macOS with UE 5.8; the -ExecutePythonScript fallback was not needed.
-AllowCommandletRendering gives the commandlet a real Metal RHI, so the material's shaders are
actually compiled and get_statistics() below can prove it; under the default NullRHI they are not.)

M_Ocean's Gerstner displacement and normal come from Shaders/Private/CamSimOcean.ush (virtual
path /CamSim/Private/CamSimOcean.ush), which mirrors FOceanWaves (Ocean/OceanWaves.cpp). The WPO
is faded by the vertex cell size (UV1.x); the normal and roughness are per pixel, faded by the pixel
footprint, with a small procedural ripple (visual only, scaled by MPC Water.z).
MPC parameter names are read by FOceanManager; don't rename them.
"""

import unreal

PKG = "/Game/Ocean"
MPC_NAME = "MPC_Ocean"
MAT_NAME = "M_Ocean"
INCLUDE = "/CamSim/Private/CamSimOcean.ush"

# name -> default (r, g, b, a)
MPC_VECTORS = [
    ("Wave0", (0, 0, 0, 0)), ("Wave1", (0, 0, 0, 0)), ("Wave2", (0, 0, 0, 0)), ("Wave3", (0, 0, 0, 0)),
    ("Dir0", (0, 0, 0, 0)), ("Dir1", (0, 0, 0, 0)), ("Dir2", (0, 0, 0, 0)), ("Dir3", (0, 0, 0, 0)),
    ("ComponentToAnchor", (0, 0, 0, 0)),
    ("AxisN", (0, 0, 0, 0)), ("AxisE", (0, 0, 0, 0)), ("AxisU", (0, 0, 1, 0)),
    ("Water", (1, 1, 0.3, 0)),   # (absorption scale, scattering scale, ripple strength, 0)
]

COMMON_PRELUDE = (
    "float4 W[4] = { W0, W1, W2, W3 }; float4 D[4] = { D0, D1, D2, D3 };\n"
    "float3 Rel = (LocalPos + ToAnchor) * 0.01;\n"
    "float2 P = float2(dot(Rel, AxisN), dot(Rel, AxisE));\n"
)
WPO_CODE = COMMON_PRELUDE + (
    "float3 Disp = CamSimOceanDisplacement(P, CellM, W, D);\n"
    "return (AxisN * Disp.x + AxisE * Disp.y + AxisU * Disp.z) * 100.0;\n"
)
# Per pixel: the normal and roughness fade each wave (and the ripples) by the pixel footprint,
# not by the vertex cell size, so waves the grid can't displace still shade the surface.
NORMAL_CODE = COMMON_PRELUDE + (
    "float3 Nn = CamSimOceanPixelNormal(P, TimeS, Water.z, W, D);\n"
    "return normalize(AxisN * Nn.x + AxisE * Nn.y + AxisU * Nn.z);\n"
)
ROUGHNESS_CODE = COMMON_PRELUDE + (
    "return CamSimOceanRoughness(P, TimeS, Water.z, W, D);\n"
)
CUSTOM_INPUTS = ["LocalPos", "CellM", "ToAnchor", "AxisN", "AxisE", "AxisU",
                 "W0", "W1", "W2", "W3", "D0", "D1", "D2", "D3"]
PIXEL_INPUTS = [n for n in CUSTOM_INPUTS if n != "CellM"] + ["TimeS", "Water"]

MEL = unreal.MaterialEditingLibrary
EAL = unreal.EditorAssetLibrary


def log(msg):
    unreal.log("OCEAN: " + str(msg))


def fresh_asset(name, asset_class, factory):
    """Create PKG/name. make_ocean_material.sh deletes the old .uasset files before launching:
    deleting in-process leaves the object loaded, and create_asset then refuses (unattended)."""
    path = PKG + "/" + name
    unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous([PKG], True)
    if EAL.does_asset_exist(path):
        raise RuntimeError(path + " already exists; run scripts/ocean/make_ocean_material.sh, which removes it first")
    asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, PKG, asset_class, factory)
    if asset is None:
        raise RuntimeError("could not create " + path)
    return asset


def make_mpc():
    mpc = fresh_asset(MPC_NAME, unreal.MaterialParameterCollection, unreal.MaterialParameterCollectionFactoryNew())
    params = []
    for name, v in MPC_VECTORS:
        p = unreal.CollectionVectorParameter()
        p.set_editor_property("parameter_name", name)
        p.set_editor_property("default_value", unreal.LinearColor(*v))
        params.append(p)
    mpc.set_editor_property("vector_parameters", params)
    names = [str(n) for n in mpc.get_vector_parameter_names()]
    if names != [n for n, _ in MPC_VECTORS]:
        raise RuntimeError("MPC parameters not as expected: %s" % names)
    log("MPC vector parameters: %s" % names)
    return mpc


class Graph:
    def __init__(self, mat):
        self.mat = mat
        self.y = 0

    def node(self, cls, x=-1200, **props):
        e = MEL.create_material_expression(self.mat, cls, x, self.y)
        if e is None:
            raise RuntimeError("could not create " + cls.__name__)
        self.y += 90
        for k, v in props.items():
            e.set_editor_property(k, v)
        return e

    def connect(self, src, dst, dst_input="", src_output=""):
        if not MEL.connect_material_expressions(src, src_output, dst, dst_input):
            raise RuntimeError("connect %s -> %s.%s failed" % (src.get_name(), dst.get_name(), dst_input))

    def mask(self, src, r=False, g=False, b=False, a=False):
        m = self.node(unreal.MaterialExpressionComponentMask, x=-1000, r=r, g=g, b=b, a=a)
        self.connect(src, m)
        return m

    def to_property(self, src, prop):
        if not MEL.connect_material_property(src, "", prop):
            raise RuntimeError("connect %s -> %s failed" % (src.get_name(), prop))


def input_named(expr, wanted):
    """The exact pin name on expr that starts with wanted (SLW pins carry suffixes like '(Albedo)')."""
    names = [str(n) for n in MEL.get_material_expression_input_names(expr)]
    for n in names:
        if n.replace(" ", "").lower().startswith(wanted.replace(" ", "").lower()):
            return n
    raise RuntimeError("no input %r on %s (has %s)" % (wanted, expr.get_name(), names))


def make_material(mpc):
    mat = fresh_asset(MAT_NAME, unreal.Material, unreal.MaterialFactoryNew())
    # Shading model is set last (below): every graph edit recompiles, and an SLW material without its
    # output node logs "Failed to compile Material", which would hide real errors in the log.
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    mat.set_editor_property("tangent_space_normal", False)
    mat.set_editor_property("two_sided", False)

    g = Graph(mat)

    # MPC reads; the xyz ones go through an RGB mask.
    mpc_nodes = {}
    for name, _ in MPC_VECTORS:
        n = g.node(unreal.MaterialExpressionCollectionParameter, x=-1400, collection=mpc, parameter_name=name)
        mpc_nodes[name] = n
    rgb = {n: g.mask(mpc_nodes[n], r=True, g=True, b=True) for n in ("ComponentToAnchor", "AxisN", "AxisE", "AxisU")}

    local_pos = g.node(unreal.MaterialExpressionLocalPosition,
                       included_offsets=unreal.PositionIncludedOffsets.EXCLUDE_OFFSETS)
    uv1 = g.node(unreal.MaterialExpressionTextureCoordinate, coordinate_index=1)
    cell_m = g.mask(uv1, r=True)

    sources = {"LocalPos": local_pos, "CellM": cell_m, "ToAnchor": rgb["ComponentToAnchor"],
               "AxisN": rgb["AxisN"], "AxisE": rgb["AxisE"], "AxisU": rgb["AxisU"]}
    for i in range(4):
        sources["W%d" % i] = mpc_nodes["Wave%d" % i]
        sources["D%d" % i] = mpc_nodes["Dir%d" % i]
    # Ripple clock (visual only): engine time, not the sim clock the waves use.
    sources["TimeS"] = g.node(unreal.MaterialExpressionTime)
    sources["Water"] = mpc_nodes["Water"]

    def custom(desc, code, pin_names=CUSTOM_INPUTS, out=unreal.CustomMaterialOutputType.CMOT_FLOAT3):
        inputs = []
        for name in pin_names:
            ci = unreal.CustomInput()
            ci.set_editor_property("input_name", name)
            inputs.append(ci)
        c = g.node(unreal.MaterialExpressionCustom, x=-600, description=desc, code=code,
                   output_type=out,
                   include_file_paths=[INCLUDE])
        c.set_editor_property("inputs", inputs)
        pins = [str(n) for n in MEL.get_material_expression_input_names(c)]
        if pins != pin_names:
            raise RuntimeError("%s pins %s != %s" % (desc, pins, pin_names))
        for name in pin_names:
            g.connect(sources[name], c, name)
        return c

    g.to_property(custom("OceanWPO", WPO_CODE), unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
    g.to_property(custom("OceanNormal", NORMAL_CODE, PIXEL_INPUTS), unreal.MaterialProperty.MP_NORMAL)
    g.to_property(custom("OceanRoughness", ROUGHNESS_CODE, PIXEL_INPUTS, unreal.CustomMaterialOutputType.CMOT_FLOAT1),
                  unreal.MaterialProperty.MP_ROUGHNESS)

    g.to_property(g.node(unreal.MaterialExpressionConstant3Vector, x=-300,
                         constant=unreal.LinearColor(0.02, 0.05, 0.07, 1.0)), unreal.MaterialProperty.MP_BASE_COLOR)
    g.to_property(g.node(unreal.MaterialExpressionConstant, x=-300, r=0.5), unreal.MaterialProperty.MP_SPECULAR)

    # Single Layer Water volume: scattering x Water.y, absorption x Water.x, phase G 0.1.
    slw = g.node(unreal.MaterialExpressionSingleLayerWaterMaterialOutput, x=0)
    log("SLW output pins: %s" % [str(n) for n in MEL.get_material_expression_input_names(slw)])
    water = mpc_nodes["Water"]
    for coeff, chan, pin in (((0.02, 0.06, 0.07), "g", "Scattering"), ((0.35, 0.09, 0.03), "r", "Absorption")):
        k = g.node(unreal.MaterialExpressionConstant3Vector, x=-500, constant=unreal.LinearColor(*coeff, 1.0))
        s = g.mask(water, **{chan: True})
        mul = g.node(unreal.MaterialExpressionMultiply, x=-300)
        g.connect(k, mul, "A")
        g.connect(s, mul, "B")
        g.connect(mul, slw, input_named(slw, pin))
    g.connect(g.node(unreal.MaterialExpressionConstant, x=-300, r=0.1), slw, input_named(slw, "PhaseG"))

    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_SINGLE_LAYER_WATER)
    MEL.layout_material_expressions(mat)
    MEL.recompile_material(mat)
    return mat


def main():
    mpc = make_mpc()
    mat = make_material(mpc)

    if mat.get_editor_property("shading_model") != unreal.MaterialShadingModel.MSM_SINGLE_LAYER_WATER:
        raise RuntimeError("shading model did not stick")
    log("shading model: %s, blend: %s, tangent_space_normal: %s" % (
        mat.get_editor_property("shading_model"), mat.get_editor_property("blend_mode"),
        mat.get_editor_property("tangent_space_normal")))
    log("expression count: %d" % MEL.get_num_material_expressions(mat))
    stats = MEL.get_statistics(mat)
    log("statistics: %s" % stats)
    if stats.num_pixel_shader_instructions <= 0 or stats.num_vertex_shader_instructions <= 0:
        raise RuntimeError("material did not compile (no shader instructions): %s" % stats)

    for a in (mpc, mat):
        if not EAL.save_loaded_asset(a, only_if_is_dirty=False):
            raise RuntimeError("could not save " + a.get_path_name())
    log("saved %s and %s" % (mpc.get_path_name(), mat.get_path_name()))
    unreal.log("OCEAN_MATERIAL_OK")


main()
