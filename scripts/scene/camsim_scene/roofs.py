"""Roof texture atlas of one buildings tile (REALISM R3b, spec section 2). Each roof is sampled from the imagery
layer's own sources (a RoofSampler) on a grid of texel_m in the tile's local ENU frame, over its axis-aligned box, and
packed into one square power-of-two atlas (roofatlas.pack_roofs). A roof whose best source (the one supplying most of
its triangle centroids) is coarser than max_source_m keeps the flat roof material, as does one that does not fit at the
last texel step. Texels with no valid source inside a textured roof take the roof's mean colour. Gutters repeat the edge
texels, so mipmaps do not bleed. UV origin is the atlas's top-left (north-west); u east, v south."""

from __future__ import annotations

import io
from dataclasses import dataclass
from typing import Protocol

import numpy as np
from PIL import Image

from . import geodesy
from .roofatlas import GUTTER_PX, pack_roofs

MAX_SOURCE_M = 2.0
ROOF_JPEG_QUALITY = 85
FILL_RGB = (128, 128, 128)  # a textured roof with no valid texel at all (never expected: its centroids were valid)


class RoofSampler(Protocol):
    def sample(self, lon: np.ndarray, lat: np.ndarray, target_m: float) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
        """(rgb uint8 (3, *shape), valid bool (*shape), native resolution in m of the source of each value, inf where
        invalid)."""


@dataclass(frozen=True)
class RoofOptions:
    texel_m: float = 0.5
    max_px: int = 2048
    max_source_m: float = MAX_SOURCE_M
    jpeg_quality: int = ROOF_JPEG_QUALITY

    @staticmethod
    def from_settings(s: dict) -> RoofOptions:
        return RoofOptions(float(s["roof_texel_m"]), int(s["roof_texture_max_px"]))


@dataclass
class RoofTexture:
    texcoords: np.ndarray  # (n, 2) float32 for every vertex of the tile; (0, 0) off the atlas
    textured: frozenset  # feature indices whose roof is drawn from the atlas
    texel_m: float | None  # None: no textured roof in this tile
    rgb: np.ndarray | None  # (size, size, 3) uint8 atlas before JPEG (tests, verify)
    jpeg: bytes | None
    n_coarse: int  # roofs flat because their best source is coarser than max_source_m (or has no data)
    n_overflow: int  # roofs flat because they did not fit at the last texel step


def enu_to_lonlat(matrix: np.ndarray, east, north, up) -> tuple[np.ndarray, np.ndarray]:
    enu = np.stack([np.asarray(east, np.float64), np.asarray(north, np.float64), np.asarray(up, np.float64)], axis=-1)
    lon, lat, _ = geodesy.ecef_to_geodetic(enu @ matrix[:3, :3].T + matrix[:3, 3])
    return lon, lat


def encode_jpeg(rgb: np.ndarray, quality: int) -> bytes:
    buf = io.BytesIO()
    Image.fromarray(np.ascontiguousarray(rgb)).save(buf, "JPEG", quality=quality, subsampling=2, optimize=False,
                                                   progressive=False)
    return buf.getvalue()


def _majority_res(res: np.ndarray, ok: np.ndarray) -> float:
    if not ok.any():
        return float("inf")
    vals, counts = np.unique(res[ok], return_counts=True)
    return float(vals[int(np.argmax(counts))])  # ties: the finer source (np.unique sorts ascending)


def roof_texture(positions: np.ndarray, roof: dict, source_ids: list[str], matrix: np.ndarray, sampler: RoofSampler,
                 opts: RoofOptions) -> RoofTexture:
    p = np.asarray(positions, np.float64)
    east, up, north = p[:, 0], p[:, 1], -p[:, 2]
    uv = np.zeros((len(p), 2), np.float32)
    extents: dict[str, tuple[float, float]] = {}
    boxes: dict[int, tuple[float, float, float]] = {}
    n_coarse = 0
    for f in sorted(roof):
        idx = roof[f]
        c = p[idx].reshape(-1, 3, 3).mean(axis=1)  # triangle centroids: inside the roof
        lon, lat = enu_to_lonlat(matrix, c[:, 0], -c[:, 2], c[:, 1])
        _, ok, res = sampler.sample(lon, lat, opts.texel_m)
        if _majority_res(res, ok) > opts.max_source_m:
            n_coarse += 1
            continue
        e0, e1, n0, n1 = east[idx].min(), east[idx].max(), north[idx].min(), north[idx].max()
        boxes[f] = (float(e0), float(n1), float(up[idx][0]))
        extents[source_ids[f]] = (float(e1 - e0), float(n1 - n0))
    pk = pack_roofs(extents, opts.texel_m, opts.max_px)
    if not pk.placed:
        return RoofTexture(uv, frozenset(), None, None, None, n_coarse, len(pk.flat))
    by_id = {source_ids[f]: f for f in boxes}
    t, size, g = pk.texel_m, pk.size, GUTTER_PX
    atlas = np.zeros((size, size, 3), np.uint8)
    for sid, pl in sorted(pk.placed.items()):
        f = by_id[sid]
        e0, n1, u0 = boxes[f]
        ge, gn = np.meshgrid(e0 + (np.arange(pl.w) + 0.5) * t, n1 - (np.arange(pl.h) + 0.5) * t)
        lon, lat = enu_to_lonlat(matrix, ge, gn, np.full(ge.shape, u0))
        rgb, ok, _ = sampler.sample(lon, lat, t)
        block = np.ascontiguousarray(np.moveaxis(rgb, 0, -1), np.uint8).copy()
        if not ok.all():
            fill = np.rint(block[ok].mean(axis=0)).astype(np.uint8) if ok.any() else np.array(FILL_RGB, np.uint8)
            block[~ok] = fill
        padded = np.pad(block, ((g, g), (g, g), (0, 0)), mode="edge")  # gutters repeat the edge texels
        atlas[pl.y - g : pl.y + pl.h + g, pl.x - g : pl.x + pl.w + g] = padded
        idx = roof[f]
        uv[idx, 0] = (pl.x + (east[idx] - e0) / t) / size
        uv[idx, 1] = (pl.y + (n1 - north[idx]) / t) / size
    textured = frozenset(by_id[sid] for sid in pk.placed)
    return RoofTexture(uv, textured, t, atlas, encode_jpeg(atlas, opts.jpeg_quality), n_coarse, len(pk.flat))
