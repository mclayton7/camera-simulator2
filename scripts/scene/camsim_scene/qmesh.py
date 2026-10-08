"""Quantized-mesh-1.0 terrain tiles (https://github.com/CesiumGS/quantized-mesh): writer and decoder.

Our own encoder, not quantized-mesh-encoder: that one casts positions to float32 (~0.7 m at lon -117) and
truncates to int16, so edge vertices miss 0/32767 (no skirts), and it assumes high-water-mark vertex order
without reordering (docs/realism-r0-spike.md, "Pitfalls")."""

from __future__ import annotations

import gzip
import struct
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from .fsutil import atomic_write

QMAX = 32767
A = 6378137.0
B = 6356752.314245179
E2 = 1.0 - (B * B) / (A * A)
HEADER = struct.Struct("<3d2f4d3d")  # centre, min/max height, bounding sphere, horizon occlusion point (88 bytes)
EXT_OCT_NORMALS = 1


def ecef(lon, lat, h) -> np.ndarray:
    lo, la = np.radians(np.asarray(lon, np.float64)), np.radians(np.asarray(lat, np.float64))
    h = np.asarray(h, np.float64)
    n = A / np.sqrt(1.0 - E2 * np.sin(la) ** 2)
    return np.stack(
        [(n + h) * np.cos(la) * np.cos(lo), (n + h) * np.cos(la) * np.sin(lo), (n * (1.0 - E2) + h) * np.sin(la)], -1
    )


def geodetic_up(lon, lat) -> np.ndarray:
    lo, la = np.radians(np.asarray(lon, np.float64)), np.radians(np.asarray(lat, np.float64))
    return np.stack([np.cos(la) * np.cos(lo), np.cos(la) * np.sin(lo), np.sin(la)], -1)


def _sign_not_zero(v):
    return np.where(v < 0.0, -1.0, 1.0)


def oct_encode(n: np.ndarray) -> np.ndarray:
    """Cesium's 2 x 8-bit octahedral encoding."""
    p = n / np.abs(n).sum(1, keepdims=True)
    x, y, z = p[:, 0], p[:, 1], p[:, 2]
    neg = z < 0.0
    x2 = np.where(neg, (1.0 - np.abs(y)) * _sign_not_zero(x), x)
    y2 = np.where(neg, (1.0 - np.abs(x)) * _sign_not_zero(y), y)
    q = np.rint((np.clip(np.stack([x2, y2], 1), -1.0, 1.0) * 0.5 + 0.5) * 255.0)
    return q.astype(np.uint8)


def oct_decode(e: np.ndarray) -> np.ndarray:
    xy = e.astype(np.float64) / 255.0 * 2.0 - 1.0
    x, y = xy[:, 0], xy[:, 1]
    z = 1.0 - np.abs(x) - np.abs(y)
    neg = z < 0.0
    x2 = np.where(neg, (1.0 - np.abs(y)) * _sign_not_zero(x), x)
    y2 = np.where(neg, (1.0 - np.abs(x)) * _sign_not_zero(y), y)
    v = np.stack([x2, y2, z], 1)
    return v / np.linalg.norm(v, axis=1, keepdims=True)


def renumber(tris: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Vertex order by first use in the index list (high-water-mark order); unused vertices are dropped."""
    tris = np.asarray(tris, np.int64).reshape(-1, 3)
    flat = tris.ravel()
    _, first = np.unique(flat, return_index=True)
    order = flat[np.sort(first)]
    remap = np.full(int(flat.max()) + 1, -1, np.int64)
    remap[order] = np.arange(len(order))
    return order, remap[tris]


def hwm_encode(flat: np.ndarray) -> np.ndarray:
    flat = np.asarray(flat, np.int64)
    highest = np.concatenate([[0], np.maximum.accumulate(flat)[:-1] + 1])
    codes = highest - flat
    if np.any(codes < 0):
        raise ValueError("indices are not in high-water-mark order (renumber first)")
    return codes


def hwm_decode(codes: np.ndarray) -> np.ndarray:
    codes = np.asarray(codes, np.int64)
    zeros_before = np.concatenate([[0], np.cumsum(codes == 0)[:-1]])
    return zeros_before - codes


def _zigzag(d: np.ndarray) -> np.ndarray:
    d = d.astype(np.int32)
    return ((d << 1) ^ (d >> 31)).astype(np.uint16)


def _unzigzag(a: np.ndarray) -> np.ndarray:
    a = a.astype(np.int32)
    return (a >> 1) ^ -(a & 1)


def _quantize(v: np.ndarray, lo: float, hi: float) -> np.ndarray:
    q = np.rint(np.clip((v - lo) / (hi - lo), 0.0, 1.0) * QMAX)
    tol = abs(hi - lo) * 1e-12
    q[np.abs(v - lo) <= tol] = 0
    q[np.abs(v - hi) <= tol] = QMAX
    return q.astype(np.int32)


def _f32_down(x: float) -> float:
    f = np.float32(x)
    return float(f if f <= x else np.nextafter(f, np.float32(-np.inf)))


def _f32_up(x: float) -> float:
    f = np.float32(x)
    return float(f if f >= x else np.nextafter(f, np.float32(np.inf)))


def horizon_occlusion_point(points: np.ndarray, center: np.ndarray) -> np.ndarray:
    """Cesium's EllipsoidalOccluder.computeHorizonCullingPointFromPoints, in ellipsoid-scaled space. A tile that
    reaches past the horizon of its own centre (z <= 1) gets the surface point under the centre."""
    scale = np.array([1.0 / A, 1.0 / A, 1.0 / B])
    d = center * scale
    d = d / np.linalg.norm(d)
    p = points * scale
    m = np.linalg.norm(p, axis=1)
    direction = p / m[:, None]
    m2 = np.maximum(1.0, m * m)
    m = np.maximum(1.0, m)
    cos_a = direction @ d
    sin_a = np.linalg.norm(np.cross(direction, d), axis=1)
    cos_b = 1.0 / m
    sin_b = np.sqrt(m2 - 1.0) * cos_b
    denom = cos_a * cos_b - sin_a * sin_b
    if np.any(denom <= 0.0):
        return d
    return d * float((1.0 / denom).max())


def vertex_normals(pts: np.ndarray, tris: np.ndarray, lon: np.ndarray, lat: np.ndarray) -> np.ndarray:
    """Area-weighted face normals in ECEF; degenerate or non-finite -> the ellipsoid normal."""
    a, b, c = pts[tris[:, 0]], pts[tris[:, 1]], pts[tris[:, 2]]
    fn = np.cross(b - a, c - a)  # |fn| = 2 x area
    acc = np.zeros_like(pts)
    for k in range(3):
        np.add.at(acc, tris[:, k], fn)
    norm = np.linalg.norm(acc, axis=1)
    good = np.isfinite(norm) & (norm > 0.0)
    out = geodetic_up(lon, lat)
    out[good] = acc[good] / norm[good, None]
    return out


def encode(lon, lat, height, triangles, bounds) -> bytes:
    """One uncompressed quantized-mesh tile. Triangles must be CCW in (lon, lat); vertices are renumbered."""
    lon, lat, height = (np.asarray(a, np.float64) for a in (lon, lat, height))
    order, tris = renumber(triangles)
    lon, lat, height = lon[order], lat[order], height[order]
    w, s, e, n = bounds
    u, v = _quantize(lon, w, e), _quantize(lat, s, n)
    hmin, hmax = _f32_down(float(height.min())), _f32_up(float(height.max()))
    span = hmax - hmin
    hq = np.rint(np.clip((height - hmin) / span, 0.0, 1.0) * QMAX).astype(np.int32) if span > 0 else np.zeros_like(u)
    pts = ecef(lon, lat, height)
    center = (pts.min(0) + pts.max(0)) / 2.0
    radius = float(np.linalg.norm(pts - center, axis=1).max())
    hop = horizon_occlusion_point(pts, center)
    out = bytearray(HEADER.pack(*center, hmin, hmax, *center, radius, *hop))
    nv = len(u)
    out += struct.pack("<I", nv)
    for q in (u, v, hq):
        out += _zigzag(np.diff(q, prepend=0)).astype("<u2").tobytes()
    itype, align = ("<u4", 4) if nv > 65536 else ("<u2", 2)
    out += b"\0" * (-len(out) % align)
    out += struct.pack("<I", len(tris)) + hwm_encode(tris.ravel()).astype(itype).tobytes()
    for mask, along in ((u == 0, v), (v == 0, u), (u == QMAX, v), (v == QMAX, u)):
        idx = np.nonzero(mask)[0]
        idx = idx[np.argsort(along[idx], kind="stable")]
        out += struct.pack("<I", len(idx)) + idx.astype(itype).tobytes()
    enc = oct_encode(vertex_normals(pts, tris, lon, lat)).tobytes()
    out += struct.pack("<BI", EXT_OCT_NORMALS, len(enc)) + enc
    return bytes(out)


def gzip_tile(raw: bytes) -> bytes:
    return gzip.compress(raw, compresslevel=9, mtime=0)


def write_tile(path: Path, raw: bytes) -> None:
    atomic_write(path, gzip_tile(raw))


@dataclass
class QMesh:
    center: tuple
    min_height: float
    max_height: float
    sphere_center: tuple
    sphere_radius: float
    horizon_occlusion: tuple
    u: np.ndarray
    v: np.ndarray
    h: np.ndarray
    triangles: np.ndarray
    west: np.ndarray
    south: np.ndarray
    east: np.ndarray
    north: np.ndarray
    extensions: dict

    def heights(self) -> np.ndarray:
        return self.min_height + self.h.astype(np.float64) / QMAX * (self.max_height - self.min_height)

    def lonlat(self, bounds) -> tuple[np.ndarray, np.ndarray]:
        w, s, e, n = bounds
        return w + self.u / QMAX * (e - w), s + self.v / QMAX * (n - s)

    def normals(self) -> np.ndarray | None:
        data = self.extensions.get(EXT_OCT_NORMALS)
        return None if data is None else oct_decode(np.frombuffer(data, np.uint8).reshape(-1, 2))


def decode(data: bytes) -> QMesh:
    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    hd = HEADER.unpack_from(data, 0)
    off = HEADER.size
    nv = struct.unpack_from("<I", data, off)[0]
    off += 4
    arrs = []
    for _ in range(3):
        a = np.frombuffer(data, "<u2", nv, off)
        off += 2 * nv
        arrs.append(np.cumsum(_unzigzag(a)).astype(np.uint16))
    itype, size = ("<u4", 4) if nv > 65536 else ("<u2", 2)
    off += -off % size
    nt = struct.unpack_from("<I", data, off)[0]
    off += 4
    tris = hwm_decode(np.frombuffer(data, itype, nt * 3, off)).reshape(-1, 3)
    off += size * nt * 3
    edges = []
    for _ in range(4):
        k = struct.unpack_from("<I", data, off)[0]
        off += 4
        edges.append(np.frombuffer(data, itype, k, off).astype(np.int64))
        off += size * k
    ext = {}
    while off < len(data):
        eid, ln = struct.unpack_from("<BI", data, off)
        off += 5
        ext[eid] = bytes(data[off : off + ln])
        off += ln
    return QMesh(hd[0:3], hd[3], hd[4], hd[5:8], hd[8], hd[9:12], *arrs, tris, *edges, ext)


def layer_json(name: str, available: list[list[dict]], attribution: str) -> dict:
    return {
        "tilejson": "2.1.0",
        "name": name,
        "version": "1.0.0",
        "format": "quantized-mesh-1.0",
        "scheme": "tms",
        "tiles": ["{z}/{x}/{y}.terrain"],
        "projection": "EPSG:4326",
        "bounds": [-180.0, -90.0, 180.0, 90.0],
        "minzoom": 0,
        "maxzoom": len(available) - 1,
        "extensions": ["octvertexnormals"],
        "available": available,
        "attribution": attribution,
    }
