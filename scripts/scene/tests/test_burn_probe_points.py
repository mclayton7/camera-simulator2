import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import burn_probe_points as bp

from camsim_scene.layers.landcover import tile_filename


def _lc(tmp_path):
    codes = np.full((600, 600), 30, np.uint8)
    codes[100:103, 200:204] = 230
    Image.fromarray(codes).save(tmp_path / tile_filename(666, -2350))
    return tmp_path


def _centre(row, col):
    return (667 * 0.05 - (row + 0.5) / 12000.0, -2350 * 0.05 + (col + 0.5) / 12000.0)


def test_nearest_burned_cell_and_a_point_outside(tmp_path):
    lc = _lc(tmp_path)
    lat, lon = _centre(110, 210)  # 8 cells south-east of the block
    inside = bp.nearest_burned(lc, lat, lon)
    assert inside is not None and bp.code_at(lc, *inside) == 230
    assert abs(inside[0] - _centre(102, 203)[0]) < 1e-9 and abs(inside[1] - _centre(102, 203)[1]) < 1e-9
    out = bp.outside_point(lc, *inside, gap_m=10.0)
    assert out is not None and out[2] == 30 and bp.code_at(lc, out[0], out[1]) == 30


def test_no_burned_cell_nearby(tmp_path):
    lc = _lc(tmp_path)
    assert bp.nearest_burned(lc, *_centre(500, 500), search_cells=20) is None
