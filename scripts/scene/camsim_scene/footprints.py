"""Building footprint raster (REALISM R3b, spec docs/superpowers/specs/2026-10-11-r3b-buildings-look-design.md
section 1): the buildings layer's kept buildings rasterised once on the land-cover tile grid at MASK_FACTOR x its
resolution. It is the single source of the fine mask (features/buildings/mask/, read by the tree scatter) and of the
land-cover burn (code 230), so the two cannot disagree. A cell is set when its centre is inside a footprint (holes
excluded). Rasterised in lon/lat, where the grid is regular, so a cell belongs to exactly one tile."""

from __future__ import annotations

import io
import json
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


def save_raster(raster: dict[tuple[int, int], np.ndarray], path: Path) -> None:
    buf = io.BytesIO()
    np.savez(buf, **{f"{i}_{j}": a for (i, j), a in sorted(raster.items())})
    atomic_write(Path(path), buf.getvalue())


def load_raster(path: Path) -> dict[tuple[int, int], np.ndarray]:
    with np.load(Path(path)) as z:
        return {tuple(int(v) for v in k.split("_")): z[k] for k in sorted(z.files, key=lambda k: tuple(map(int, k.split("_"))))}
