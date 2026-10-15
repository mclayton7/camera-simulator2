"""Building footprint raster (REALISM R3b, spec docs/superpowers/specs/2026-10-11-r3b-buildings-look-design.md
section 1): the buildings layer's kept buildings rasterised once on the land-cover tile grid at MASK_FACTOR x its
resolution. It is the single source of the fine mask (features/buildings/mask/, read by the tree scatter) and of the
land-cover burn (code 230), so the two cannot disagree. A cell is set when its centre is inside a footprint (holes
excluded). Rasterised in lon/lat, where the grid is regular, so a cell belongs to exactly one tile."""

from __future__ import annotations

import io
import json
import shutil
from pathlib import Path

import numpy as np
from PIL import Image

from .fsutil import atomic_write
from .layers.landcover import tile_filename, tile_range
from .sources.worldcover import TILE_DEG, TILE_PX

MASK_FACTOR = 8
MASK_PX = TILE_PX * MASK_FACTOR  # 4800: ~1.16 m x 0.97 m per cell at 33 N
MASK_FORMAT = "camsim-buildings-mask-1"
MASK_RULES_VERSION = "1"  # bump on any change to the rasterisation or the burn rule (it is in both layers' settings)
BURN_MIN_CELLS = 32  # of the MASK_FACTOR^2 mask cells under a land-cover cell: at least half of it covered


def rasterise_tile(parts: list, i: int, j: int) -> np.ndarray:
    """(MASK_PX, MASK_PX) bool for tile (lat index i, lon index j), row 0 = north: cell centres inside `parts`."""
    from rasterio import features as rf
    from rasterio.transform import Affine

    if not parts:
        return np.zeros((MASK_PX, MASK_PX), bool)
    d = TILE_DEG / MASK_PX
    transform = Affine(d, 0.0, j * TILE_DEG, 0.0, -d, (i + 1) * TILE_DEG)
    out = rf.rasterize(
        [(p, 1) for p in parts], out_shape=(MASK_PX, MASK_PX), transform=transform, fill=0, all_touched=False,
        dtype="uint8",
    )
    return out > 0


def raster_tiles(buildings: list) -> dict[tuple[int, int], np.ndarray]:
    """Every tile with at least one set cell -> its rows packed MSB first (np.packbits(axis=1)), in sorted key order.
    Binary union, so the result does not depend on the buildings' order."""
    by_tile: dict[tuple[int, int], list] = {}
    for b in buildings:
        for p in b.parts:
            w, s, e, n = p.bounds
            for i in tile_range(s, n):
                for j in tile_range(w, e):
                    by_tile.setdefault((i, j), []).append(p)
    out = {}
    for key in sorted(by_tile):
        m = rasterise_tile(by_tile[key], *key)
        if m.any():
            out[key] = np.packbits(m, axis=1)
    return out


def unpack(packed: np.ndarray) -> np.ndarray:
    return np.unpackbits(packed, axis=1, count=MASK_PX).astype(bool)


def burn_cells(mask: np.ndarray) -> np.ndarray:
    """(TILE_PX, TILE_PX) bool: the land-cover cells with at least BURN_MIN_CELLS of their mask cells set."""
    cover = mask.reshape(TILE_PX, MASK_FACTOR, TILE_PX, MASK_FACTOR).sum(axis=(1, 3))
    return cover >= BURN_MIN_CELLS


def mask_png(packed: np.ndarray) -> bytes:
    buf = io.BytesIO()
    Image.frombytes("1", (MASK_PX, MASK_PX), np.ascontiguousarray(packed).tobytes()).save(buf, "PNG", optimize=True)
    return buf.getvalue()


def write_mask_tile(packed: np.ndarray, path: Path) -> None:
    atomic_write(Path(path), mask_png(packed))


def mask_index(written: list[tuple[int, int]]) -> dict:
    return {
        "format": MASK_FORMAT,
        "tile_deg": TILE_DEG,
        "tile_px": MASK_PX,
        "cell_factor": MASK_FACTOR,
        "rule": "cell centre inside a building footprint, holes excluded",
        "rules": MASK_RULES_VERSION,
        "tiles": [{"file": tile_filename(i, j), "lat_index": i, "lon_index": j} for i, j in written],
    }


def mask_index_bytes(written: list[tuple[int, int]]) -> bytes:
    return (json.dumps(mask_index(written), indent=2, ensure_ascii=False) + "\n").encode("utf-8")


RASTER_INDEX = "index.json"


def _tile_path(out_dir: Path, ij: tuple[int, int]) -> Path:
    return Path(out_dir) / f"{ij[0]}_{ij[1]}.npy"


def _npy_bytes(packed: np.ndarray) -> bytes:
    buf = io.BytesIO()
    np.save(buf, np.ascontiguousarray(packed, np.uint8))
    return buf.getvalue()


def or_tile(out_dir: Path, ij: tuple[int, int], packed: np.ndarray) -> None:
    """OR one tile's packed rows into out_dir (binary union: the result does not depend on the order)."""
    p = _tile_path(out_dir, ij)
    if p.exists():
        packed = np.bitwise_or(np.load(p), packed)
    atomic_write(p, _npy_bytes(packed))


def write_raster_index(out_dir: Path, key: str) -> None:
    """Written last: the directory is complete for `key` only when the index names it."""
    tiles = sorted(tuple(int(v) for v in p.stem.split("_")) for p in Path(out_dir).glob("*.npy"))
    atomic_write(Path(out_dir) / RASTER_INDEX, json.dumps({"inputs": key, "tiles": tiles}).encode())


def raster_key(out_dir: Path) -> str | None:
    p = Path(out_dir) / RASTER_INDEX
    return json.loads(p.read_text())["inputs"] if p.exists() else None


def save_raster(raster: dict[tuple[int, int], np.ndarray], out_dir: Path, key: str) -> None:
    """A whole raster (raster_tiles) as a directory: one <i>_<j>.npy of packed rows per tile, then index.json."""
    out_dir = Path(out_dir)
    shutil.rmtree(out_dir, ignore_errors=True)
    out_dir.mkdir(parents=True)
    for ij, packed in sorted(raster.items()):
        atomic_write(_tile_path(out_dir, ij), _npy_bytes(packed))
    write_raster_index(out_dir, key)


class FootprintRaster:
    """A footprint raster directory (index.json + <i>_<j>.npy packed rows), read one tile at a time."""

    def __init__(self, out_dir: Path):
        self.dir = Path(out_dir)
        self._keys = [tuple(k) for k in json.loads((self.dir / RASTER_INDEX).read_text())["tiles"]]
        self._set = set(self._keys)

    def keys(self) -> list[tuple[int, int]]:
        return list(self._keys)

    def get(self, i: int, j: int) -> np.ndarray | None:
        return np.load(_tile_path(self.dir, (i, j))) if (i, j) in self._set else None
