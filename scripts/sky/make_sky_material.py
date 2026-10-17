# Copyright CamSim Contributors. All Rights Reserved.
"""Generate /Game/Sky/MPC_Sky and /Game/Sky/M_SkyDome (UE editor Python, ROADMAP 6A night sky).

This script is the source of truth for both assets; the committed .uasset files are its output (never hand-edit).
Run it with scripts/sky/make_sky_material.sh, which uses the pythonscript commandlet with a real RHI
(-AllowCommandletRendering) so the material's shaders compile and get_statistics() below can prove it.

M_SkyDome: Surface / Unlit / Opaque / Is Sky / two-sided, for a camera-following inward-facing sphere.
Emissive, in cd/m² (V = view direction = -CameraVectorWS, T = transmittance along V, CamSimSky.ush):
    SkyAtmosphereViewLuminance
  + SkyAtmosphereLightDiskLuminance(0)                       (sun disc)
  + T * ( CamSimMoonDisc(V, MoonDir, SunDir, r, scale) * enabled * (1 - RenderingReflectionCaptureMask)
          + StarTex(equatorial UV of SkyRot * V) * Floor.y    (point sampled: a star stays one texel)
          + Floor.x )                                         (night floor, white)
One Custom node ("NightSky") computes T once for the three night terms. The moon disc is left out of the real-time sky
capture (RenderingReflectionCaptureMask, as UE's own SkyAtmosphereLightDiskLuminance does): the moon directional light
already lights the scene, so a captured disc would add its light a second time through the sky light, by an amount that
changed as the 0.5 deg disc drifted across the capture's ~0.7 deg texels. The floor stays in the capture: it is meant to
light the ground.
The moon disc and air mass come from Shaders/Private/CamSimSky.ush (virtual path /CamSim/Private/CamSimSky.ush),
which mirrors FNightIllumination / FMoonDisc (Environment/NightIllumination.cpp).

MPC parameter names and the StarTex texture parameter are read by FSkyDomeDriver; don't rename them.
"""

import unreal

PKG = "/Game/Sky"
MPC_NAME = "MPC_Sky"
MAT_NAME = "M_SkyDome"
INCLUDE = "/CamSim/Private/CamSimSky.ush"
STAR_TEX_PARAM = "StarTex"
# A linear (non-sRGB) engine texture, required as the default of a LinearColor texture parameter. The driver
# replaces it with the star field; until then Floor.y = 0 keeps the star term at 0.
STAR_TEX_DEFAULT = "/Engine/EngineMaterials/BaseFlattenLinearColor"

# name -> default (r, g, b, a)
MPC_VECTORS = [
    ("MoonDir", (0, 0, 1, 0)),        # xyz unit, UE world
    ("SunDir", (0, 0, -1, 0)),        # xyz unit, UE world
    ("MoonParams", (0, 0, 0, 0)),     # (angular radius rad, disc luminance scale cd/m², phase angle rad, enabled 0/1)
    ("SkyRot0", (1, 0, 0, 0)),        # rows of the UE-world -> equatorial rotation
    ("SkyRot1", (0, 1, 0, 0)),
    ("SkyRot2", (0, 0, 1, 0)),
    ("Floor", (0, 0, 0.172, 0)),      # (floor radiance cd/m², star scale, k_V, 0)
    ("UpWorld", (0, 0, 1, 0)),        # local up at the camera
]

# Equirectangular (RA, Dec) lookup matching FStarField: x = RA from 0 at the left, y = Dec +90 at the top.
# Load (not Sample): point sampling with no mips whatever the texture's filter, and no derivative seam at RA 0.
NIGHT_CODE = (
    "float SinEl = dot(V, UpWorld); float T = CamSimSkyTransmittance(SinEl, Floor.z);\n"
    "float NotInCapture = 1.0 - ResolvedView.RenderingReflectionCaptureMask;\n"
    "float Moon = CamSimMoonDisc(V, MoonDir, SunDir, MoonParams.x, MoonParams.y) * MoonParams.w * NotInCapture;\n"
    "float3 Q = float3(dot(SkyRot0.xyz, V), dot(SkyRot1.xyz, V), dot(SkyRot2.xyz, V));\n"
    "float Ra = atan2(Q.y, Q.x); float Dec = asin(clamp(Q.z, -1, 1));\n"
    "float2 UV = float2(frac(Ra / (2 * PI)), 0.5 - Dec / PI);\n"
    "uint W, H; StarTex.GetDimensions(W, H);\n"
    "int2 Texel = min(int2(UV * float2(W, H)), int2(W, H) - 1);\n"
    "float3 Stars = StarTex.Load(int3(Texel, 0)).rgb * Floor.y;\n"
    "return (Moon + Stars + Floor.x) * T;\n"
)

MEL = unreal.MaterialEditingLibrary
EAL = unreal.EditorAssetLibrary


def log(msg):
    unreal.log("SKY: " + str(msg))


def fresh_asset(name, asset_class, factory):
    """Create PKG/name. make_sky_material.sh deletes the old .uasset files before launching:
    deleting in-process leaves the object loaded, and create_asset then refuses (unattended)."""
    path = PKG + "/" + name
    unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous([PKG], True)
    if EAL.does_asset_exist(path):
        raise RuntimeError(path + " already exists; run scripts/sky/make_sky_material.sh, which removes it first")
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

    def add(self, a, b):
        n = self.node(unreal.MaterialExpressionAdd, x=-200)
        self.connect(a, n, "A")
        self.connect(b, n, "B")
        return n

    def to_property(self, src, prop):
        if not MEL.connect_material_property(src, "", prop):
            raise RuntimeError("connect %s -> %s failed" % (src.get_name(), prop))


def make_material(mpc):
    mat = fresh_asset(MAT_NAME, unreal.Material, unreal.MaterialFactoryNew())
    mat.set_editor_property("material_domain", unreal.MaterialDomain.MD_SURFACE)
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("is_sky", True)
    mat.set_editor_property("two_sided", True)

    g = Graph(mat)

    # MPC reads; the direction ones go through an RGB mask.
    mpc_nodes = {}
    for name, _ in MPC_VECTORS:
        mpc_nodes[name] = g.node(unreal.MaterialExpressionCollectionParameter, x=-1400,
                                 collection=mpc, parameter_name=name)
    sources = {n: g.mask(mpc_nodes[n], r=True, g=True, b=True) for n in ("MoonDir", "SunDir", "UpWorld")}
    for n in ("MoonParams", "SkyRot0", "SkyRot1", "SkyRot2", "Floor"):
        sources[n] = mpc_nodes[n]

    # V: the view direction (CameraVectorWS points from the pixel to the camera).
    cam = g.node(unreal.MaterialExpressionCameraVectorWS)
    sources["V"] = g.node(unreal.MaterialExpressionMultiply, x=-1000, const_b=-1.0)
    g.connect(cam, sources["V"], "A")

    star_tex = g.node(unreal.MaterialExpressionTextureObjectParameter, x=-1000,
                      parameter_name=STAR_TEX_PARAM,
                      sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    default_tex = unreal.load_asset(STAR_TEX_DEFAULT)
    if default_tex is None or default_tex.get_editor_property("srgb"):
        raise RuntimeError("%s missing or sRGB (a LinearColor parameter needs a linear default)" % STAR_TEX_DEFAULT)
    star_tex.set_editor_property("texture", default_tex)
    sources[STAR_TEX_PARAM] = star_tex

    def custom(desc, code, pin_names, out=unreal.CustomMaterialOutputType.CMOT_FLOAT3):
        inputs = []
        for name in pin_names:
            ci = unreal.CustomInput()
            ci.set_editor_property("input_name", name)
            inputs.append(ci)
        c = g.node(unreal.MaterialExpressionCustom, x=-600, description=desc, code=code,
                   output_type=out, include_file_paths=[INCLUDE])
        c.set_editor_property("inputs", inputs)
        pins = [str(n) for n in MEL.get_material_expression_input_names(c)]
        if pins != pin_names:
            raise RuntimeError("%s pins %s != %s" % (desc, pins, pin_names))
        for name in pin_names:
            g.connect(sources[name], c, name)
        return c

    common = ["V", "UpWorld", "Floor"]
    atmosphere = g.node(unreal.MaterialExpressionSkyAtmosphereViewLuminance, x=-600)
    sun_disc = g.node(unreal.MaterialExpressionSkyAtmosphereLightDiskLuminance, x=-600, light_index=0)
    night = custom("NightSky", NIGHT_CODE,
                   common + ["MoonDir", "SunDir", "MoonParams", "SkyRot0", "SkyRot1", "SkyRot2", STAR_TEX_PARAM])

    emissive = g.add(g.add(atmosphere, sun_disc), night)
    g.to_property(emissive, unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    MEL.layout_material_expressions(mat)
    MEL.recompile_material(mat)
    return mat


def main():
    mpc = make_mpc()
    mat = make_material(mpc)

    expect = {"material_domain": unreal.MaterialDomain.MD_SURFACE,
              "shading_model": unreal.MaterialShadingModel.MSM_UNLIT,
              "blend_mode": unreal.BlendMode.BLEND_OPAQUE, "is_sky": True, "two_sided": True}
    for k, v in expect.items():
        got = mat.get_editor_property(k)
        if got != v:
            raise RuntimeError("%s did not stick: %s != %s" % (k, got, v))
    log("settings: %s" % {k: str(mat.get_editor_property(k)) for k in expect})
    log("expression count: %d" % MEL.get_num_material_expressions(mat))
    tex_params = [str(n) for n in MEL.get_texture_parameter_names(mat)]
    if tex_params != [STAR_TEX_PARAM]:
        raise RuntimeError("texture parameters %s != [%s]" % (tex_params, STAR_TEX_PARAM))
    log("texture parameters: %s" % tex_params)
    stats = MEL.get_statistics(mat)
    log("statistics: %s" % stats)
    if stats.num_pixel_shader_instructions <= 0 or stats.num_vertex_shader_instructions <= 0:
        raise RuntimeError("material did not compile (no shader instructions): %s" % stats)

    for a in (mpc, mat):
        if not EAL.save_loaded_asset(a, only_if_is_dirty=False):
            raise RuntimeError("could not save " + a.get_path_name())
    log("saved %s and %s" % (mpc.get_path_name(), mat.get_path_name()))
    unreal.log("SKY_MATERIAL_OK")


main()
