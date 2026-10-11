#!/usr/bin/env python3
# Copyright CamSim Contributors. All Rights Reserved.
"""Prepare a CC0 tree model for the vegetation library (REALISM R4 slice, docs/vegetation.md).

  prepare_tree.py SRC --name tree_broadleaf_a --height-m 9.0 --max-tris 10000 \
      --licence CC0-1.0 --source-url URL --out unreal_project/CamSimTest/Content/NonUFS/Vegetation \
      [--texture MATERIAL=PATH ...]

SRC is .obj, .gltf or .glb. Steps: OBJ -> glTF (npx obj2gltf), simplify to --max-tris (npx gltfpack -si, -noq, no
compression; embedded textures stay embedded), wrap the scene's root nodes in one node scaled so the model's height (Y extent, glTF
is Y-up) is --height-m, write <out>/<name>.glb, print the library.json type entry (with sha256 and the final triangle
count). Fails if the final mesh has more than --max-tris triangles or uses non-identity node rotations/scales below the
root.

--texture binds a diffuse map to an OBJ material whose MTL names none (some packs ship MTLs without map_Kd): the OBJ
is converted from a temporary copy whose MTL gains `map_Kd`. Alpha-blended materials (obj2gltf --checkTransparency marks
any texture with transparent texels BLEND) become double-sided MASK with cutoff 0.5: leaf cards are cut-outs seen from
both sides, and a masked material sorts and casts shadows like an opaque one.
"""

from __future__ import annotations

import argparse
import hashlib
import json
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


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("src", type=Path)
    ap.add_argument("--name", required=True)
    ap.add_argument("--height-m", type=float, required=True)
    ap.add_argument("--max-tris", type=int, default=10_000)
    ap.add_argument("--licence", required=True)
    ap.add_argument("--source-url", required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--texture", action="append", default=[], metavar="MATERIAL=PATH")
    a = ap.parse_args()
    if a.licence != "CC0-1.0":
        raise SystemExit("the vegetation library ships CC0-1.0 assets only")
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
        ratio = min(1.0, a.max_tris / max(tris, 1))
        subprocess.run(
            [
                "npx",
                "-y",
                GLTFPACK,
                "-i",
                str(src),
                "-o",
                str(simplified),
                "-noq",
                "-si",
                f"{ratio:.4f}",
            ],
            check=True,
        )
        doc, binary = read_glb(simplified)
        check_node_transforms(doc)
        check_embedded_images(doc)
        mask_blended_materials(doc)
        tris = triangle_count(doc)
        if tris > a.max_tris:
            raise SystemExit(
                f"{tris} triangles after simplification > {a.max_tris}; try -sa or another model"
            )
        lo, hi = y_extent(doc)
        scale = a.height_m / (hi - lo)
        scene = doc["scenes"][doc.get("scene", 0)]
        doc["nodes"].append(
            {
                "name": "camsim_scale",
                "scale": [scale] * 3,
                "translation": [0, -lo * scale, 0],
                "children": scene["nodes"],
            }
        )
        scene["nodes"] = [len(doc["nodes"]) - 1]
        a.out.mkdir(parents=True, exist_ok=True)
        dst = a.out / f"{a.name}.glb"
        write_glb(dst, doc, binary)
    sha = hashlib.sha256(dst.read_bytes()).hexdigest()
    print(
        json.dumps(
            {
                "name": a.name,
                "gltf": dst.name,
                "height_m": a.height_m,
                "thermal_class": "tree_canopy",
                "licence": a.licence,
                "source_url": a.source_url,
                "sha256": sha,
                "triangles": tris,
            },
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
