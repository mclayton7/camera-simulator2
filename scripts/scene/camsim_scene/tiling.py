"""Cesium's geographic TMS grid (EPSG:4326, 2 x 1 root tiles, tile size 180 deg / 2^z, y from the south)
and pyramid planning: which tiles a layer has at each zoom, from regions and source footprints."""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np
import shapely

Bounds = tuple[float, float, float, float]  # W, S, E, N in degrees
GLOBE: Bounds = (-180.0, -90.0, 180.0, 90.0)
MAX_PLAN_ZOOM = 22


def tile_size_deg(z: int) -> float:
    return 180.0 / (1 << z)


def tile_bounds(z: int, x: int, y: int) -> Bounds:
    s = tile_size_deg(z)
    return (-180.0 + x * s, -90.0 + y * s, -180.0 + (x + 1) * s, -90.0 + (y + 1) * s)


def make_keys(x, y) -> np.ndarray:
    return (np.asarray(x, np.int64) << 32) | np.asarray(y, np.int64)


def split_keys(keys) -> tuple[np.ndarray, np.ndarray]:
    k = np.asarray(keys, np.int64)
    return k >> 32, k & 0xFFFFFFFF


def children(keys: np.ndarray) -> np.ndarray:
    x, y = split_keys(keys)
    cx = np.concatenate([2 * x, 2 * x + 1, 2 * x, 2 * x + 1])
    cy = np.concatenate([2 * y, 2 * y, 2 * y + 1, 2 * y + 1])
    return np.unique(make_keys(cx, cy))


def tile_boxes(z: int, keys: np.ndarray) -> np.ndarray:
    x, y = split_keys(keys)
    s = tile_size_deg(z)
    return shapely.box(-180.0 + x * s, -90.0 + y * s, -180.0 + (x + 1) * s, -90.0 + (y + 1) * s)


@dataclass
class Region:
    """A lon/lat polygon with a maximum zoom per layer. A tile takes the highest limit of the regions it overlaps."""

    name: str
    polygon: list[tuple[float, float]]
    max_zoom: dict[str, int]

    @classmethod
    def from_bounds(cls, name: str, b: Bounds, max_zoom: dict[str, int]) -> Region:
        w, s, e, n = (float(v) for v in b)
        return cls(name, [(w, s), (e, s), (e, n), (w, n), (w, s)], dict(max_zoom))

    def geometry(self) -> shapely.Polygon:
        return shapely.Polygon(self.polygon)

    def to_dict(self) -> dict:
        return {
            "name": self.name,
            "polygon": [[float(a), float(b)] for a, b in self.polygon],
            "max_zoom": {k: int(v) for k, v in sorted(self.max_zoom.items())},
        }

    @classmethod
    def from_dict(cls, d: dict) -> Region:
        return cls(
            d["name"], [(float(a), float(b)) for a, b in d["polygon"]], {k: int(v) for k, v in d["max_zoom"].items()}
        )


@dataclass
class Coverage:
    """Where a layer's sources have data: footprints (lon/lat) with each source's zoom limit."""

    geoms: list = field(default_factory=list)
    limits: list[int] = field(default_factory=list)

    def add(self, geom, limit: int) -> None:
        self.geoms.append(geom)
        self.limits.append(int(limit))


@dataclass
class LayerPlan:
    tiles: dict[int, np.ndarray]  # z -> sorted keys
    leaves: dict[int, np.ndarray]  # z -> sorted keys of tiles without children

    @property
    def max_zoom(self) -> int:
        return max(self.tiles)

    def count(self) -> int:
        return sum(len(k) for k in self.tiles.values())

    @staticmethod
    def _member(keys: np.ndarray | None, x: int, y: int) -> bool:
        if keys is None or len(keys) == 0:
            return False
        k = (int(x) << 32) | int(y)
        i = int(np.searchsorted(keys, k))
        return i < len(keys) and int(keys[i]) == k

    def has(self, z: int, x: int, y: int) -> bool:
        return self._member(self.tiles.get(z), x, y)

    def is_leaf(self, z: int, x: int, y: int) -> bool:
        return self._member(self.leaves.get(z), x, y)


def _overlaps(boxes: np.ndarray, geom) -> np.ndarray:
    """Interior overlap: a tile that only touches a region along an edge or at a corner doesn't count."""
    return shapely.intersects(boxes, geom) & ~shapely.touches(boxes, geom)


def plan_tiles(regions: list[Region], layer: str, coverage: Coverage) -> LayerPlan:
    """Expand the two root tiles level by level. A tile gets its four children when
    min(highest overlapping region limit, highest intersecting source limit) > z (complete siblings)."""
    tree = shapely.STRtree(coverage.geoms) if coverage.geoms else None
    limits = np.asarray(coverage.limits, np.int64)
    region_geoms = [(r.geometry(), r.max_zoom.get(layer, -1)) for r in regions]
    tiles = {0: make_keys([0, 1], [0, 0])}
    leaves: dict[int, np.ndarray] = {}
    z = 0
    while True:
        keys = tiles[z]
        boxes = tile_boxes(z, keys)
        rlim = np.full(len(keys), -1, np.int64)
        for geom, mz in region_geoms:
            hit = _overlaps(boxes, geom)
            rlim[hit] = np.maximum(rlim[hit], mz)
        slim = np.full(len(keys), -1, np.int64)
        if tree is not None:
            ti, gi = tree.query(boxes, predicate="intersects")
            np.maximum.at(slim, ti, limits[gi])
        expand = (np.minimum(rlim, slim) > z) & (z < MAX_PLAN_ZOOM)
        leaves[z] = keys[~expand]
        if not expand.any():
            return LayerPlan(tiles, leaves)
        tiles[z + 1] = children(keys[expand])
        z += 1


def available_ranges(plan: LayerPlan) -> list[list[dict]]:
    """layer.json `available`: per zoom, rectangles of tiles (runs along x, merged across consecutive rows)."""
    out = []
    for z in range(plan.max_zoom + 1):
        x, y = split_keys(plan.tiles[z])
        order = np.lexsort((x, y))
        x, y = x[order].tolist(), y[order].tolist()
        runs, start = [], 0
        for i in range(1, len(x) + 1):
            if i == len(x) or y[i] != y[start] or x[i] != x[i - 1] + 1:
                runs.append((y[start], x[start], x[i - 1]))
                start = i
        rects, open_rects = [], {}
        for yy, x0, x1 in runs:
            r = open_rects.get((x0, x1))
            if r is not None and r["endY"] == yy - 1:
                r["endY"] = yy
            else:
                r = {"startX": x0, "startY": yy, "endX": x1, "endY": yy}
                rects.append(r)
                open_rects[(x0, x1)] = r
        rects.sort(key=lambda r: (r["startY"], r["startX"]))
        out.append(rects)
    return out


def available_keys(available: list[list[dict]]) -> dict[int, np.ndarray]:
    out = {}
    for z, rects in enumerate(available):
        parts = []
        for r in rects:
            xs, ys = np.meshgrid(np.arange(r["startX"], r["endX"] + 1), np.arange(r["startY"], r["endY"] + 1))
            parts.append(make_keys(xs.ravel(), ys.ravel()))
        out[z] = np.unique(np.concatenate(parts)) if parts else np.zeros(0, np.int64)
    return out
