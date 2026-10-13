#!/usr/bin/env python3
# Copyright CamSim Contributors. All Rights Reserved.
"""Prepare a CC0 tree or shrub model for the vegetation library (REALISM R4, docs/vegetation.md).

  uv run --with pillow python scripts/vegetation/prepare_tree.py SRC --name tree_broadleaf_a --height-m 9.0 \
      --max-tris 10000 --licence CC0-1.0 --source-url URL --out unreal_project/CamSimTest/Content/NonUFS/Vegetation \
      [--role tree|shrub] [--crown-radius-m R] [--far-tris 150] [--texture MATERIAL=PATH ...] [--base-color MATERIAL=R,G,B ...]

SRC is .obj, .gltf or .glb. Steps: OBJ -> glTF (npx obj2gltf), simplify to --max-tris (npx gltfpack -si, -noq, no
compression; embedded textures stay embedded), wrap the scene's root nodes in one node scaled so the model's height (Y extent, glTF
is Y-up) is --height-m, write <out>/<name>.glb, print the library.json type entry (with sha256 and the final triangle
count). Fails if the final mesh has more than --max-tris triangles or uses non-identity node rotations/scales below the
root.

--texture binds a diffuse map to an OBJ material whose MTL names none (some packs ship MTLs without map_Kd): the OBJ
is converted from a temporary copy whose MTL gains `map_Kd`. Alpha-blended materials (obj2gltf --checkTransparency marks
any texture with transparent texels BLEND) become double-sided MASK with cutoff 0.5: leaf cards are cut-outs seen from
both sides, and a masked material sorts and casts shadows like an opaque one.

--base-color sets a material's linear base colour factor (packs whose MTL colours are stylised). Trees also get a far-tier
mesh (--far-tris > 0, the budget it must fit): a procedural lathe crown fitted to the foliage's height profile plus a
hexagonal trunk, untextured, in the model's mean colours (`far_mesh`; leaf-card trees don't simplify below ~600
triangles), written as <name>_far.glb. The printed entry is a camsim-vegetation-2 type.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import math
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

OBJ2GLTF = "obj2gltf@3.2.0"  # 3.1.6 breaks on current cesium (defaultValue removed)
GLTFPACK = "gltfpack@0.24.0"
GLB_MAGIC, JSON_CHUNK, BIN_CHUNK = 0x46546C67, 0x4E4F534A, 0x004E4942
ALPHA_CUTOFF = 0.5
THERMAL_CLASS = {"tree": "tree_canopy", "shrub": "shrubland"}
CROWN_FACTOR = {"tree": 0.3, "shrub": 0.5}  # default crown radius / height (VegetationLibrary.cpp)


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
    out = (
        struct.pack("<III", GLB_MAGIC, 2, total)
        + struct.pack("<II", len(js), JSON_CHUNK)
        + js
    )
    if binary:
        out += struct.pack("<II", len(binary), BIN_CHUNK) + binary
    path.write_bytes(out)


def triangle_count(doc: dict) -> int:
    n = 0
    for mesh in doc.get("meshes", []):
        for prim in mesh["primitives"]:
            if prim.get("mode", 4) != 4:
                raise SystemExit("only TRIANGLES primitives are supported")
            acc = (
                prim["indices"] if "indices" in prim else prim["attributes"]["POSITION"]
            )
            n += doc["accessors"][acc]["count"] // 3
    return n


def check_node_transforms(doc: dict) -> None:
    for i, node in enumerate(doc.get("nodes", [])):
        if (
            "matrix" in node
            or node.get("scale", [1, 1, 1]) != [1, 1, 1]
            or node.get("rotation", [0, 0, 0, 1]) != [0, 0, 0, 1]
        ):
            raise SystemExit(
                f"node {i} has a rotation/scale/matrix; flatten the model first (gltfpack does unless -kn)"
            )


def check_embedded_images(doc: dict) -> None:
    for i, image in enumerate(doc.get("images", [])):
        if "uri" in image:
            raise SystemExit(
                f"image {i} references an external file ({image['uri']}); the GLB must be self-contained"
            )


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


def mask_blended_materials(doc: dict) -> None:
    for mat in doc.get("materials", []):
        if mat.get("alphaMode") == "BLEND":
            mat["alphaMode"] = "MASK"
            mat["alphaCutoff"] = ALPHA_CUTOFF
            mat["doubleSided"] = True


def set_base_colors(doc: dict, specs: list[str]) -> None:
    """MATERIAL=R,G,B (linear, 0..1): the material's pbrMetallicRoughness.baseColorFactor becomes [R, G, B, 1]."""
    by_name = {m.get("name"): m for m in doc.get("materials", [])}
    for spec in specs:
        name, sep, rgb = spec.partition("=")
        try:
            vals = [float(v) for v in rgb.split(",")]
        except ValueError:
            vals = []
        if not sep or len(vals) != 3 or not all(0.0 <= v <= 1.0 for v in vals):
            raise SystemExit(f"--base-color {spec!r}: expected MATERIAL=R,G,B with each in [0, 1]")
        if name not in by_name:
            raise SystemExit(f"--base-color: no material named {name!r} (have {sorted(n for n in by_name if n)})")
        by_name[name].setdefault("pbrMetallicRoughness", {})["baseColorFactor"] = vals + [1.0]


def read_positions(doc: dict, binary: bytes, accessor: int) -> list[tuple[float, float, float]]:
    """A float32 VEC3 accessor's values (tightly packed or strided)."""
    acc = doc["accessors"][accessor]
    if acc.get("componentType") != 5126 or acc.get("type") != "VEC3":
        raise SystemExit(f"accessor {accessor}: expected float VEC3 positions")
    view = doc["bufferViews"][acc["bufferView"]]
    base = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = view.get("byteStride", 12)
    return [struct.unpack_from("<3f", binary, base + k * stride) for k in range(acc["count"])]


def _is_foliage(mat: dict) -> bool:
    return mat.get("alphaMode") in ("MASK", "BLEND")


def _mean_colour(doc: dict, binary: bytes, mat: dict) -> list[float]:
    """Linear RGB: the base colour factor times the mean of the base colour texture's opaque texels (sRGB decoded)."""
    pbr = mat.get("pbrMetallicRoughness", {})
    factor = list(pbr.get("baseColorFactor", [1.0, 1.0, 1.0, 1.0]))[:3]
    tex = pbr.get("baseColorTexture")
    if tex is None:
        return factor
    from PIL import Image

    image = doc["images"][doc["textures"][tex["index"]]["source"]]
    view = doc["bufferViews"][image["bufferView"]]
    data = binary[view.get("byteOffset", 0) : view.get("byteOffset", 0) + view["byteLength"]]
    im = Image.open(io.BytesIO(data)).convert("RGBA")
    raw = im.tobytes()
    allpx = [raw[k : k + 4] for k in range(0, len(raw), 4)]
    px = [p for p in allpx if p[3] >= 128] or allpx

    def lin(c: float) -> float:
        c /= 255.0
        return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4

    return [factor[i] * sum(lin(p[i]) for p in px) / len(px) for i in range(3)]


FAR_RINGS, FAR_SEGMENTS, FAR_TRUNK_SIDES = 6, 8, 6


def _p90(values: list[float]) -> float:
    v = sorted(values)
    return v[int(0.9 * (len(v) - 1))] if v else 0.0


def _foliage_points(doc: dict, binary: bytes) -> tuple[list, list, list, list]:
    """(foliage points, other points, foliage materials, other materials), node translations applied."""
    foliage, other, fol_mats, other_mats = [], [], [], []
    for node in doc.get("nodes", []):
        if "mesh" not in node:
            continue
        t = node.get("translation", [0.0, 0.0, 0.0])
        for prim in doc["meshes"][node["mesh"]]["primitives"]:
            mat = doc["materials"][prim["material"]] if "material" in prim else {}
            pts = [(x + t[0], y + t[1], z + t[2]) for x, y, z in read_positions(doc, binary, prim["attributes"]["POSITION"])]
            (foliage if _is_foliage(mat) else other).extend(pts)
            (fol_mats if _is_foliage(mat) else other_mats).append(mat)
    return foliage, other, fol_mats, other_mats


def crown_radius(doc: dict, binary: bytes) -> float | None:
    """The p90 horizontal distance of the foliage vertices from their centroid (model units), or None without foliage."""
    foliage = _foliage_points(doc, binary)[0]
    if not foliage:
        return None
    cx = sum(p[0] for p in foliage) / len(foliage)
    cz = sum(p[2] for p in foliage) / len(foliage)
    return _p90([math.hypot(p[0] - cx, p[2] - cz) for p in foliage])


def far_mesh(doc: dict, binary: bytes, crown_colour: tuple[float, float, float] | None = None) -> tuple[dict, bytes]:
    """A procedural far-tier mesh (~110 triangles): the crown as a lathe surface whose ring radii are the p90 horizontal
    extent of the foliage vertices around each height, closed at the bottom with an apex on top, plus a hexagonal trunk
    from the model's base into the crown. Materials are untextured, coloured with the mean colour of the foliage and of
    the rest (crown_colour, linear, replaces the foliage mean: calibrated against the imagery, docs/vegetation.md). Node
    translations are applied (gltfpack output has no rotations/scales). Deterministic."""
    foliage, other, fol_mats, other_mats = _foliage_points(doc, binary)
    if not foliage:
        raise SystemExit("far mesh: no foliage (alpha-masked) material to take the crown from")
    base_y = min(p[1] for p in foliage + other)
    c0, c1 = min(p[1] for p in foliage), max(p[1] for p in foliage)
    cx = sum(p[0] for p in foliage) / len(foliage)
    cz = sum(p[2] for p in foliage) / len(foliage)
    band = (c1 - c0) / FAR_RINGS
    radii = []
    for k in range(FAR_RINGS):  # rings 0..K-1; the apex closes the top
        y = c0 + band * k
        near = [math.hypot(p[0] - cx, p[2] - cz) for p in foliage if abs(p[1] - y) <= 0.5 * band + 1e-9]
        radii.append(_p90(near))
    for k in range(FAR_RINGS):  # an empty band takes its neighbours' radius
        if radii[k] == 0.0:
            nb = [r for r in radii[max(0, k - 1) : k + 2] if r > 0.0]
            radii[k] = sum(nb) / len(nb) if nb else 0.05 * (c1 - base_y)
    crown_v, crown_i = [], []
    for k, r in enumerate(radii):
        y = c0 + band * k
        for s in range(FAR_SEGMENTS):
            a = 2.0 * math.pi * s / FAR_SEGMENTS
            crown_v.append((cx + r * math.cos(a), y, cz + r * math.sin(a)))
    apex, bottom = len(crown_v), len(crown_v) + 1
    crown_v += [(cx, c1, cz), (cx, c0, cz)]
    S = FAR_SEGMENTS
    for k in range(FAR_RINGS - 1):
        for s in range(S):
            a0, a1 = k * S + s, k * S + (s + 1) % S
            b0, b1 = a0 + S, a1 + S
            crown_i += [a0, b0, a1, a1, b0, b1]
    top = (FAR_RINGS - 1) * S
    for s in range(S):
        crown_i += [top + s, apex, top + (s + 1) % S]
        crown_i += [s, (s + 1) % S, bottom]
    trunk_pts = [p for p in other if p[1] <= c0]
    tr = max(0.05 * (c1 - base_y) * 0.2, _p90([math.hypot(p[0] - cx, p[2] - cz) for p in trunk_pts]) if trunk_pts else 0.0)
    t_top = c0 + 0.3 * (c1 - c0)
    trunk_v, trunk_i = [], []
    for s in range(FAR_TRUNK_SIDES):
        a = 2.0 * math.pi * s / FAR_TRUNK_SIDES
        trunk_v += [(cx + tr * math.cos(a), base_y, cz + tr * math.sin(a)), (cx + tr * math.cos(a), t_top, cz + tr * math.sin(a))]
    for s in range(FAR_TRUNK_SIDES):
        a0, a1 = 2 * s, 2 * ((s + 1) % FAR_TRUNK_SIDES)
        trunk_i += [a0, a0 + 1, a1, a1, a0 + 1, a1 + 1]
    crown_col = (list(crown_colour) if crown_colour is not None
                 else [sum(c) / len(fol_mats) for c in zip(*(_mean_colour(doc, binary, m) for m in fol_mats))])
    trunk_col = ([sum(c) / len(other_mats) for c in zip(*(_mean_colour(doc, binary, m) for m in other_mats))]
                 if other_mats else [0.08, 0.06, 0.04])
    return _build_far_doc([(crown_v, crown_i, crown_col, "far_crown"), (trunk_v, trunk_i, trunk_col, "far_trunk")])


SHRUB_RINGS = 3       # per lobe: rings from the base up, then an apex
SHRUB_SEGMENTS = 8


def procedural_shrub(height_m: float, radius_m: float, colour: tuple[float, float, float], seed: int) -> tuple[dict, bytes]:
    """A solid shrub clump (REALISM R4): a central low dome and three smaller ones around it, each a lathe dome with its
    ring radii jittered by +-12 % (random.Random(seed)), one untextured material "shrub" in colour (linear), the top at
    height_m, about 2 x radius_m across, ~160 triangles. Solid from above (the Kenney leaf fans were hollow rings seen
    from the air) and deterministic."""
    import random

    rnd = random.Random(seed)
    lobes = [(0.0, 0.0, 0.62 * radius_m, height_m)]
    a0 = rnd.uniform(0.0, 2.0 * math.pi)
    for k in range(3):
        a = a0 + 2.0 * math.pi * k / 3 + rnd.uniform(-0.35, 0.35)
        d = radius_m * rnd.uniform(0.42, 0.52)
        lobes.append((d * math.cos(a), d * math.sin(a), radius_m * rnd.uniform(0.45, 0.55), height_m * rnd.uniform(0.6, 0.85)))
    v, idx = [], []
    S = SHRUB_SEGMENTS
    for cx, cz, r, h in lobes:
        base = len(v)
        for k in range(SHRUB_RINGS):
            phi = 0.5 * math.pi * k / SHRUB_RINGS
            y = h * math.sin(phi)   # base at 0 (library contract); placement sinks shrubs (ShrubSinkM)
            for s_ in range(S):
                a = 2.0 * math.pi * s_ / S
                rr = r * math.cos(phi) * rnd.uniform(0.88, 1.12)
                v.append((cx + rr * math.cos(a), y, cz + rr * math.sin(a)))
        apex = len(v)
        v.append((cx, h, cz))
        for k in range(SHRUB_RINGS - 1):
            for s_ in range(S):
                a0_, a1_ = base + k * S + s_, base + k * S + (s_ + 1) % S
                idx += [a0_, a0_ + S, a1_, a1_, a0_ + S, a1_ + S]
        top = base + (SHRUB_RINGS - 1) * S
        for s_ in range(S):
            idx += [top + s_, apex, top + (s_ + 1) % S]
    doc, binary = _build_far_doc([(v, idx, list(colour), "shrub")])
    doc["asset"]["generator"] = "camsim prepare_tree.py procedural_shrub"
    return doc, binary


def _normals(v: list, idx: list) -> list:
    acc = [[0.0, 0.0, 0.0] for _ in v]
    for t in range(0, len(idx), 3):
        a, b, c = (v[i] for i in idx[t : t + 3])
        u = [b[i] - a[i] for i in range(3)]
        w = [c[i] - a[i] for i in range(3)]
        n = [u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]]
        for i in idx[t : t + 3]:
            for j in range(3):
                acc[i][j] += n[j]
    out = []
    for n in acc:
        L = math.sqrt(sum(x * x for x in n)) or 1.0
        out.append((n[0] / L, n[1] / L, n[2] / L))
    return out


def _build_far_doc(parts: list) -> tuple[dict, bytes]:
    doc = {"asset": {"version": "2.0", "generator": "camsim prepare_tree.py far_mesh"}, "scene": 0,
           "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}], "meshes": [{"primitives": []}],
           "materials": [], "accessors": [], "bufferViews": [], "buffers": [{}]}
    binary = b""

    def add_view(data: bytes, target: int) -> int:
        nonlocal binary
        binary += b"\0" * (-len(binary) % 4)
        doc["bufferViews"].append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(data), "target": target})
        binary += data
        return len(doc["bufferViews"]) - 1

    for v, idx, col, name in parts:
        # Winding: the lathe, prism and domes are built counter-clockwise seen from outside (glTF front faces), so the
        # normals below point out. (A flip here once turned every far mesh inside out: rendered 3-10x too dark.)
        nrm = _normals(v, idx)
        pv = add_view(b"".join(struct.pack("<3f", *p) for p in v), 34962)
        doc["accessors"].append({"bufferView": pv, "componentType": 5126, "count": len(v), "type": "VEC3",
                                 "min": [min(p[i] for p in v) for i in range(3)], "max": [max(p[i] for p in v) for i in range(3)]})
        nv = add_view(b"".join(struct.pack("<3f", *n) for n in nrm), 34962)
        doc["accessors"].append({"bufferView": nv, "componentType": 5126, "count": len(nrm), "type": "VEC3"})
        iv = add_view(b"".join(struct.pack("<H", i) for i in idx), 34963)
        doc["accessors"].append({"bufferView": iv, "componentType": 5123, "count": len(idx), "type": "SCALAR"})
        doc["materials"].append({"name": name, "pbrMetallicRoughness": {"baseColorFactor": [round(c, 6) for c in col] + [1.0],
                                                                         "metallicFactor": 0.0, "roughnessFactor": 1.0}})
        n = len(doc["accessors"])
        doc["meshes"][0]["primitives"].append({"attributes": {"POSITION": n - 3, "NORMAL": n - 2}, "indices": n - 1,
                                               "material": len(doc["materials"]) - 1})
    binary += b"\0" * (-len(binary) % 4)
    doc["buffers"][0]["byteLength"] = len(binary)
    return doc, binary


def library_entry(name: str, role: str, height_m: float, crown_radius_m: float | None, licence: str, source_url: str,
                  sha: str, tris: int, far: tuple[str, str, int] | None) -> dict:
    """The camsim-vegetation-2 type entry; far = (file name, sha256, triangles) of the far mesh."""
    e = {
        "name": name,
        "role": role,
        "gltf": f"{name}.glb",
        "sha256": sha,
        "height_m": height_m,
        "crown_radius_m": crown_radius_m if crown_radius_m is not None else round(CROWN_FACTOR[role] * height_m, 3),
        "thermal_class": THERMAL_CLASS[role],
        "licence": licence,
        "source_url": source_url,
        "triangles": tris,
    }
    if far:
        e |= {"far_gltf": far[0], "far_sha256": far[1], "far_triangles": far[2]}
    return e


def scale_to_height(doc: dict, height_m: float) -> None:
    """Wrap the scene's root nodes in one node scaled so the Y extent is height_m, base at y = 0."""
    lo, hi = y_extent(doc)
    scale = height_m / (hi - lo)
    scene = doc["scenes"][doc.get("scene", 0)]
    doc["nodes"].append({"name": "camsim_scale", "scale": [scale] * 3, "translation": [0, -lo * scale, 0], "children": scene["nodes"]})
    scene["nodes"] = [len(doc["nodes"]) - 1]


def simplify(src: Path, dst: Path, ratio: float) -> None:
    subprocess.run(["npx", "-y", GLTFPACK, "-i", str(src), "-o", str(dst), "-noq", "-si", f"{ratio:.4f}"], check=True)


def bind_obj_textures(src: Path, textures: list[str], work: Path) -> Path:
    """Copy SRC's OBJ + MTL into WORK, adding `map_Kd` for each MATERIAL=PATH; return the copied OBJ."""
    bindings = {}
    for spec in textures:
        mat, sep, tex = spec.partition("=")
        if not sep or not mat or not tex:
            raise SystemExit(f"--texture {spec!r}: expected MATERIAL=PATH")
        tex_path = Path(tex)
        if not tex_path.is_file():
            raise SystemExit(f"--texture {spec!r}: {tex_path} not found")
        shutil.copy(tex_path, work / tex_path.name)
        bindings[mat] = tex_path.name
    obj_lines = src.read_text().splitlines()
    mtl_names = [
        line.split(maxsplit=1)[1] for line in obj_lines if line.startswith("mtllib ")
    ]
    if len(mtl_names) != 1:
        raise SystemExit(
            f"{src}: --texture needs exactly one mtllib line, found {len(mtl_names)}"
        )
    mtl_out, current, seen = [], None, set()
    for line in (src.parent / mtl_names[0]).read_text().splitlines():
        if line.startswith("newmtl "):
            if current in bindings:
                mtl_out.append(f"map_Kd {bindings[current]}")
            current = line.split(maxsplit=1)[1]
            seen.add(current)
        elif line.startswith("map_Kd ") and current in bindings:
            continue
        mtl_out.append(line)
    if current in bindings:
        mtl_out.append(f"map_Kd {bindings[current]}")
    missing = set(bindings) - seen
    if missing:
        raise SystemExit(
            f"--texture: no material named {sorted(missing)} in {mtl_names[0]}"
        )
    (work / mtl_names[0]).write_text("\n".join(mtl_out) + "\n")
    obj = work / src.name
    obj.write_text("\n".join(obj_lines) + "\n")
    return obj


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("src", type=Path, nargs="?", help="the model (not with --procedural-shrub)")
    ap.add_argument("--name", required=True)
    ap.add_argument("--height-m", type=float, required=True)
    ap.add_argument("--max-tris", type=int, default=10_000)
    ap.add_argument("--licence", required=True)
    ap.add_argument("--source-url", required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--texture", action="append", default=[], metavar="MATERIAL=PATH")
    ap.add_argument("--base-color", action="append", default=[], metavar="MATERIAL=R,G,B")
    ap.add_argument("--role", choices=("tree", "shrub"), default="tree")
    ap.add_argument("--crown-radius-m", type=float, default=None,
                    help="default: measured (p90 foliage extent); without foliage 0.3 x height (tree) / 0.5 x height (shrub)")
    ap.add_argument("--far-tris", type=int, default=150, help="far-tier mesh triangle budget (trees only; 0 = none)")
    ap.add_argument("--far-color", default=None, metavar="R,G,B", help="far crown colour (linear); default: the foliage mean")
    ap.add_argument("--procedural-shrub", action="store_true",
                    help="no SRC: a solid procedural shrub clump (material 'shrub'; --crown-radius-m, --seed, --base-color)")
    ap.add_argument("--seed", type=int, default=1, help="--procedural-shrub outline seed")
    a = ap.parse_args(argv)
    if a.licence != "CC0-1.0":
        raise SystemExit("the vegetation library ships CC0-1.0 assets only")
    if a.procedural_shrub:
        return _procedural_main(a)
    if a.src is None:
        raise SystemExit("SRC is required (or --procedural-shrub)")
    far_colour = _parse_rgb("--far-color", a.far_color) if a.far_color else None
    if a.texture and a.src.suffix.lower() != ".obj":
        raise SystemExit("--texture applies to OBJ sources only")
    with tempfile.TemporaryDirectory() as td:
        src = a.src.resolve()
        if src.suffix.lower() == ".obj":
            if a.texture:
                work = Path(td) / "obj"
                work.mkdir()
                src = bind_obj_textures(src, a.texture, work)
            gltf = Path(td) / "conv.glb"
            subprocess.run(
                [
                    "npx",
                    "-y",
                    OBJ2GLTF,
                    "-i",
                    str(src),
                    "-o",
                    str(gltf),
                    "--binary",
                    "--checkTransparency",
                ],
                check=True,
            )
            src = gltf
        doc, _ = (
            read_glb(src)
            if src.suffix.lower() == ".glb"
            else (json.loads(src.read_text()), b"")
        )
        tris = triangle_count(doc)
        simplified = Path(td) / "simp.glb"
        simplify(src, simplified, min(1.0, a.max_tris / max(tris, 1)))
        doc, binary = read_glb(simplified)
        check_node_transforms(doc)
        check_embedded_images(doc)
        mask_blended_materials(doc)
        set_base_colors(doc, a.base_color)
        tris = triangle_count(doc)
        if tris > a.max_tris:
            raise SystemExit(
                f"{tris} triangles after simplification > {a.max_tris}; try -sa or another model"
            )
        far_doc = far_mesh(doc, binary, far_colour) if a.role == "tree" and a.far_tris > 0 else None
        crown = a.crown_radius_m
        if crown is None and (r := crown_radius(doc, binary)) is not None:
            lo, hi = y_extent(doc)
            crown = round(r * a.height_m / (hi - lo), 2)   # measured from the foliage, in metres at --height-m
        scale_to_height(doc, a.height_m)
        a.out.mkdir(parents=True, exist_ok=True)
        dst = a.out / f"{a.name}.glb"
        write_glb(dst, doc, binary)
        far = None
        if far_doc is not None:
            fdoc, fbin = far_doc
            ftris = triangle_count(fdoc)
            if ftris > a.far_tris:
                raise SystemExit(f"far mesh: {ftris} triangles > {a.far_tris}")
            scale_to_height(fdoc, a.height_m)
            far_dst = a.out / f"{a.name}_far.glb"
            write_glb(far_dst, fdoc, fbin)
            far = (far_dst.name, hashlib.sha256(far_dst.read_bytes()).hexdigest(), ftris)
    sha = hashlib.sha256(dst.read_bytes()).hexdigest()
    print(json.dumps(library_entry(a.name, a.role, a.height_m, crown, a.licence, a.source_url, sha, tris, far), indent=2))
    return 0


def _parse_rgb(flag: str, spec: str) -> tuple[float, float, float]:
    try:
        vals = [float(x) for x in spec.split(",")]
    except ValueError:
        vals = []
    if len(vals) != 3 or not all(0.0 <= x <= 1.0 for x in vals):
        raise SystemExit(f"{flag} {spec!r}: expected R,G,B with each in [0, 1]")
    return vals[0], vals[1], vals[2]


def _procedural_main(a: argparse.Namespace) -> int:
    """--procedural-shrub: write <out>/<name>.glb (procedural_shrub) and print its library entry."""
    if a.role != "shrub":
        raise SystemExit("--procedural-shrub makes shrubs only (--role shrub)")
    radius = a.crown_radius_m if a.crown_radius_m is not None else 0.5 * a.height_m
    colour = (0.05, 0.06, 0.03)
    for spec in a.base_color:
        name, _, rgb = spec.partition("=")
        if name != "shrub":
            raise SystemExit(f"--base-color: no material named {name!r} (have ['shrub'])")
        colour = _parse_rgb("--base-color", rgb)
    doc, binary = procedural_shrub(a.height_m, radius, colour, a.seed)
    tris = triangle_count(doc)
    if tris > a.max_tris:
        raise SystemExit(f"{tris} triangles > {a.max_tris}")
    a.out.mkdir(parents=True, exist_ok=True)
    dst = a.out / f"{a.name}.glb"
    write_glb(dst, doc, binary)
    sha = hashlib.sha256(dst.read_bytes()).hexdigest()
    print(json.dumps(library_entry(a.name, "shrub", a.height_m, round(radius, 2), a.licence, a.source_url, sha, tris, None), indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
