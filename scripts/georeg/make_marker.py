#!/usr/bin/env python3
"""Write entities/marker/plate.glb: the REALISM R6 geo-registration marker (scripts/georeg_check.py).

A 2 m x 2 m x 0.05 m box, glTF Y-up, origin at the centre of its bottom face, flat normals, one orange material.
Entity types 9001 (scale 1) and 9002 (scale 5) in deploy/camsim_config.yaml use it. Deterministic bytes; CC0.

    python3 scripts/georeg/make_marker.py [OUT]   (default entities/marker/plate.glb)
"""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

HALF, TOP = 1.0, 0.05


def _faces() -> list[tuple[tuple[float, float, float], list[tuple[float, float, float]]]]:
    """(normal, four corners counter-clockwise seen from outside) for the six faces."""
    x0, x1, y0, y1, z0, z1 = -HALF, HALF, 0.0, TOP, -HALF, HALF
    return [
        ((0, 1, 0), [(x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0)]),
        ((0, -1, 0), [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)]),
        ((1, 0, 0), [(x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1)]),
        ((-1, 0, 0), [(x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)]),
        ((0, 0, 1), [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]),
        ((0, 0, -1), [(x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0)]),
    ]


def build() -> bytes:
    pos, nrm, idx = [], [], []
    for n, quad in _faces():
        base = len(pos)
        pos += quad
        nrm += [n] * 4
        idx += [base, base + 1, base + 2, base, base + 2, base + 3]
    pbin = b"".join(struct.pack("<3f", *p) for p in pos)
    nbin = b"".join(struct.pack("<3f", *n) for n in nrm)
    ibin = b"".join(struct.pack("<H", i) for i in idx)
    ibin += b"\x00" * (-len(ibin) % 4)
    blob = pbin + nbin + ibin
    doc = {
        "asset": {"version": "2.0", "generator": "camsim georeg/make_marker.py", "copyright": "CC0-1.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "georeg_marker"}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0}]}],
        "materials": [{"name": "marker_orange", "pbrMetallicRoughness": {
            "baseColorFactor": [1.0, 0.35, 0.0, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.8}}],
        "buffers": [{"byteLength": len(blob)}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": len(pbin), "target": 34962},
            {"buffer": 0, "byteOffset": len(pbin), "byteLength": len(nbin), "target": 34962},
            {"buffer": 0, "byteOffset": len(pbin) + len(nbin), "byteLength": 2 * len(idx), "target": 34963},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(pos), "type": "VEC3",
             "min": [-HALF, 0.0, -HALF], "max": [HALF, TOP, HALF]},
            {"bufferView": 1, "componentType": 5126, "count": len(nrm), "type": "VEC3"},
            {"bufferView": 2, "componentType": 5123, "count": len(idx), "type": "SCALAR"},
        ],
    }
    js = json.dumps(doc, sort_keys=True, separators=(",", ":")).encode()
    js += b" " * (-len(js) % 4)
    total = 12 + 8 + len(js) + 8 + len(blob)
    return (struct.pack("<4sII", b"glTF", 2, total) + struct.pack("<I4s", len(js), b"JSON") + js
            + struct.pack("<I4s", len(blob), b"BIN\x00") + blob)


if __name__ == "__main__":
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2] / "entities/marker/plate.glb"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(build())
    print(f"wrote {out} ({out.stat().st_size} bytes)")
