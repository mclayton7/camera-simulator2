"""Minimal glTF 2.0 binary (GLB) for building tiles (REALISM R3): one mesh with two primitives (roofs, walls) sharing
POSITION / NORMAL / _FEATURE_ID_0, one EXT_structural_metadata property table `building`. Deterministic bytes: JSON with
sorted keys and no whitespace, every bufferView 8-byte aligned."""

from __future__ import annotations

import json
import struct
from dataclasses import dataclass

import numpy as np

GLB_MAGIC = 0x46546C67
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942
FLOAT, UBYTE, UINT = 5126, 5121, 5125
ARRAY_BUFFER, ELEMENT_ARRAY_BUFFER = 34962, 34963
ROOF_RGBA = (0.30, 0.30, 0.31, 1.0)
WALL_RGBA = (0.42, 0.41, 0.40, 1.0)
HEIGHT_SOURCES = ("overture_height", "overture_floors", "default")
SCHEMA = {
    "id": "camsim_buildings",
    "enums": {
        "heightSource": {
            "valueType": "UINT8",
            "values": [{"name": n, "value": i} for i, n in enumerate(HEIGHT_SOURCES)],
        }
    },
    "classes": {
        "building": {
            "properties": {
                "class": {"type": "STRING"},
                "height_m": {"type": "SCALAR", "componentType": "FLOAT32"},
                "height_source": {"type": "ENUM", "enumType": "heightSource"},
                "source_id": {"type": "STRING"},
            }
        }
    },
}


@dataclass
class TileMesh:
    positions: np.ndarray  # (n, 3) float32: east, up, -north (m) from the tile's ENU origin
    normals: np.ndarray  # (n, 3) float32, same axes
    feature_ids: np.ndarray  # (n,) float32: index into the property table
    roof_indices: np.ndarray  # (k,) uint32
    wall_indices: np.ndarray  # (j,) uint32


@dataclass
class PropertyTable:
    source_ids: list[str]
    classes: list[str]
    height_m: np.ndarray  # float32
    height_source: np.ndarray  # uint8, index into HEIGHT_SOURCES


@dataclass
class DecodedTile:
    positions: np.ndarray
    normals: np.ndarray
    feature_ids: np.ndarray
    roof_indices: np.ndarray
    wall_indices: np.ndarray
    table: PropertyTable
    copyright: str


class _Bin:
    def __init__(self):
        self.data = bytearray()
        self.views: list[dict] = []

    def add(self, raw: bytes, target: int | None = None) -> int:
        self.data += b"\0" * (-len(self.data) % 8)
        view = {"buffer": 0, "byteOffset": len(self.data), "byteLength": len(raw)}
        if target is not None:
            view["target"] = target
        self.data += raw
        self.views.append(view)
        return len(self.views) - 1


def _strings(b: _Bin, values: list[str]) -> tuple[int, int]:
    enc = [v.encode("utf-8") for v in values]
    offsets = np.cumsum([0] + [len(e) for e in enc], dtype=np.uint64).astype(np.uint32)
    return b.add(b"".join(enc)), b.add(offsets.tobytes())


def write_glb(mesh: TileMesh, table: PropertyTable, copyright: str) -> bytes:
    b = _Bin()
    acc: list[dict] = []

    def accessor(arr: np.ndarray, ctype: int, atype: str, target: int, minmax: bool = False) -> int:
        a = {
            "bufferView": b.add(np.ascontiguousarray(arr).tobytes(), target),
            "componentType": ctype,
            "count": int(arr.shape[0]),
            "type": atype,
        }
        if minmax:
            a["min"] = [float(v) for v in arr.min(axis=0)]
            a["max"] = [float(v) for v in arr.max(axis=0)]
        acc.append(a)
        return len(acc) - 1

    pos = accessor(mesh.positions.astype(np.float32), FLOAT, "VEC3", ARRAY_BUFFER, minmax=True)
    nrm = accessor(mesh.normals.astype(np.float32), FLOAT, "VEC3", ARRAY_BUFFER)
    fid = accessor(mesh.feature_ids.astype(np.float32), FLOAT, "SCALAR", ARRAY_BUFFER)
    features = {
        "EXT_mesh_features": {
            "featureIds": [{"featureCount": len(table.source_ids), "attribute": 0, "propertyTable": 0}]
        }
    }
    prims = []
    for mat, idx in ((0, mesh.roof_indices), (1, mesh.wall_indices)):
        if len(idx) == 0:
            continue
        ia = accessor(idx.astype(np.uint32), UINT, "SCALAR", ELEMENT_ARRAY_BUFFER)
        prims.append(
            {
                "attributes": {"POSITION": pos, "NORMAL": nrm, "_FEATURE_ID_0": fid},
                "indices": ia,
                "material": mat,
                "mode": 4,
                "extensions": features,
            }
        )
    cls_v, cls_o = _strings(b, table.classes)
    sid_v, sid_o = _strings(b, table.source_ids)
    h_v = b.add(table.height_m.astype(np.float32).tobytes())
    hs_v = b.add(table.height_source.astype(np.uint8).tobytes())
    doc = {
        "asset": {"version": "2.0", "generator": "camsim-scene", "copyright": copyright},
        "extensionsUsed": ["EXT_mesh_features", "EXT_structural_metadata"],
        "extensions": {
            "EXT_structural_metadata": {
                "schema": SCHEMA,
                "propertyTables": [
                    {
                        "class": "building",
                        "count": len(table.source_ids),
                        "properties": {
                            "class": {"values": cls_v, "stringOffsets": cls_o, "stringOffsetType": "UINT32"},
                            "height_m": {"values": h_v},
                            "height_source": {"values": hs_v},
                            "source_id": {"values": sid_v, "stringOffsets": sid_o, "stringOffsetType": "UINT32"},
                        },
                    }
                ],
            }
        },
        "materials": [
            {"pbrMetallicRoughness": {"baseColorFactor": list(rgba), "metallicFactor": 0.0, "roughnessFactor": 0.9}}
            for rgba in (ROOF_RGBA, WALL_RGBA)
        ],
        "meshes": [{"primitives": prims}],
        "nodes": [{"mesh": 0}],
        "scenes": [{"nodes": [0]}],
        "scene": 0,
        "accessors": acc,
        "bufferViews": b.views,
        "buffers": [{"byteLength": len(b.data) + (-len(b.data) % 4)}],
    }
    js = json.dumps(doc, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    js += b" " * (-len(js) % 4)
    bin_ = bytes(b.data) + b"\0" * (-len(b.data) % 4)
    total = 12 + 8 + len(js) + 8 + len(bin_)
    return (
        struct.pack("<III", GLB_MAGIC, 2, total)
        + struct.pack("<II", len(js), CHUNK_JSON)
        + js
        + struct.pack("<II", len(bin_), CHUNK_BIN)
        + bin_
    )


def read_glb(data: bytes) -> tuple[dict, bytes]:
    magic, version, length = struct.unpack_from("<III", data, 0)
    if magic != GLB_MAGIC or version != 2 or length != len(data):
        raise ValueError("not a GLB 2.0 file")
    jlen, jtype = struct.unpack_from("<II", data, 12)
    if jtype != CHUNK_JSON:
        raise ValueError("first GLB chunk is not JSON")
    doc = json.loads(data[20 : 20 + jlen])
    off = 20 + jlen
    blen, btype = struct.unpack_from("<II", data, off)
    if btype != CHUNK_BIN:
        raise ValueError("second GLB chunk is not BIN")
    return doc, data[off + 8 : off + 8 + blen]


def _view(doc: dict, bin_: bytes, i: int) -> bytes:
    v = doc["bufferViews"][i]
    return bin_[v["byteOffset"] : v["byteOffset"] + v["byteLength"]]


def _accessor(doc: dict, bin_: bytes, i: int) -> np.ndarray:
    a = doc["accessors"][i]
    dt = {FLOAT: np.float32, UINT: np.uint32, UBYTE: np.uint8}[a["componentType"]]
    n = {"SCALAR": 1, "VEC3": 3}[a["type"]]
    arr = np.frombuffer(_view(doc, bin_, a["bufferView"]), dt, count=a["count"] * n)
    return arr.reshape(-1, 3) if n == 3 else arr


def _decode_strings(doc: dict, bin_: bytes, prop: dict) -> list[str]:
    raw = _view(doc, bin_, prop["values"])
    off = np.frombuffer(_view(doc, bin_, prop["stringOffsets"]), np.uint32)
    return [raw[off[i] : off[i + 1]].decode("utf-8") for i in range(len(off) - 1)]


def decode_tile(data: bytes) -> DecodedTile:
    doc, bin_ = read_glb(data)
    prims = doc["meshes"][0]["primitives"]
    attrs = prims[0]["attributes"]
    idx = {p["material"]: _accessor(doc, bin_, p["indices"]) for p in prims}
    empty = np.zeros(0, np.uint32)
    t = doc["extensions"]["EXT_structural_metadata"]["propertyTables"][0]["properties"]
    table = PropertyTable(
        _decode_strings(doc, bin_, t["source_id"]),
        _decode_strings(doc, bin_, t["class"]),
        np.frombuffer(_view(doc, bin_, t["height_m"]["values"]), np.float32).copy(),
        np.frombuffer(_view(doc, bin_, t["height_source"]["values"]), np.uint8).copy(),
    )
    return DecodedTile(
        _accessor(doc, bin_, attrs["POSITION"]).copy(),
        _accessor(doc, bin_, attrs["NORMAL"]).copy(),
        _accessor(doc, bin_, attrs["_FEATURE_ID_0"]).copy(),
        idx.get(0, empty).copy(),
        idx.get(1, empty).copy(),
        table,
        doc["asset"].get("copyright", ""),
    )
