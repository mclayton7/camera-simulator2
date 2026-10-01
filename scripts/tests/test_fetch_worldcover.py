"""Unit tests for scripts/landcover/fetch_worldcover.py (ROADMAP 4B): tiling maths, naming, code passthrough."""

import json

import numpy as np
import pytest
from PIL import Image

from landcover import fetch_worldcover as fw


def test_san_francisco_bbox_is_20_tiles():
    tiles = fw.tiles_for_bbox(-122.56, 37.69, -122.35, 37.84)
    assert len(tiles) == 20
    assert sorted({i for i, _ in tiles}) == [753, 754, 755, 756]
    assert sorted({j for _, j in tiles}) == [-2452, -2451, -2450, -2449, -2448]


def test_a_bbox_edge_on_a_tile_boundary_does_not_add_a_tile():
    assert list(fw.tile_range(37.70, 37.80)) == [754, 755]  # 37.80 starts tile 756
    assert list(fw.tile_range(-122.50, -122.45)) == [-2450]
    assert list(fw.tile_range(37.71, 37.72)) == [754]  # inside one tile


def test_invalid_bbox_raises():
    with pytest.raises(ValueError):
        fw.tiles_for_bbox(-122.0, 37.0, -123.0, 38.0)  # W > E
    with pytest.raises(ValueError):
        fw.tiles_for_bbox(-122.0, 37.0, -121.0, 91.0)  # N > 90


def test_tile_filename_is_the_signed_south_west_corner():
    assert fw.tile_filename(755, -2449) == "+37.75_-122.45.png"
    assert fw.tile_filename(0, -1) == "+0.00_-0.05.png"
    assert fw.tile_filename(-1, 3599) == "-0.05_+179.95.png"


def test_cog_name_and_offset():
    assert fw.cog_name(755, -2450) == "N36W123"
    assert fw.cog_offset(755, -2450) == (14400, 6000)  # verified against the live COG (rasterio window)
    assert fw.cog_name(-1, -1) == "S03W003"
    assert fw.cog_offset(-1, -1) == (0, 59 * 600)
    assert fw.cog_name(1799, 3599) == "N87E177"
    assert fw.cog_offset(1799, 3599) == (0, 59 * 600)
    assert fw.cog_name(-1800, -3600) == "S90W180"
    assert fw.cog_offset(-1800, -3600) == (59 * 600, 0)


def test_cog_offset_matches_the_cell_geometry():
    for i, j in [(755, -2450), (-17, 42), (1234, -3600), (-1800, 3599)]:
        row, col = fw.cog_offset(i, j)
        cog_top = (i // 60) * 3 + 3
        cog_left = (j // 60) * 3
        assert abs((cog_top - row / fw.CELLS_PER_DEG) - (i + 1) * fw.TILE_DEG) < 1e-9
        assert abs((cog_left + col / fw.CELLS_PER_DEG) - j * fw.TILE_DEG) < 1e-9


def test_fetch_passes_codes_through_losslessly(tmp_path):
    calls = []
    yy, xx = np.mgrid[0:600, 0:600]
    pattern = ((yy + 3 * xx) % 101).astype(np.uint8)

    def reader(url, row_off, col_off):
        calls.append((url, row_off, col_off))
        if "W123" in url and col_off == 6600:  # tile (755, -2449) "missing" (as a 404 would be)
            return None
        return pattern

    index = fw.fetch((-122.50, 37.75, -122.40, 37.80), tmp_path, reader=reader)
    assert [c[1:] for c in calls] == [(14400, 6000), (14400, 6600)]
    assert calls[0][0] == fw.COG_URL.format(name="N36W123")
    png = tmp_path / "+37.75_-122.50.png"
    assert png.exists() and not (tmp_path / "+37.75_-122.45.png").exists()
    img = Image.open(png)
    assert img.mode == "L" and img.size == (600, 600)
    assert np.array_equal(np.asarray(img), pattern)
    on_disk = json.loads((tmp_path / "index.json").read_text())
    assert on_disk == index
    assert index["format"] == "camsim-landcover-1"
    assert index["tile_deg"] == 0.05 and index["tile_px"] == 600
    assert index["licence"] == "CC BY 4.0" and "ESA WorldCover" in index["attribution"]
    assert index["tiles"] == [{"file": "+37.75_-122.50.png", "lat_index": 755, "lon_index": -2450}]
    assert index["missing"] == [[755, -2449]]
    assert "CC BY 4.0" in (tmp_path / "ATTRIBUTION.txt").read_text()


def test_write_tile_rejects_the_wrong_shape(tmp_path):
    with pytest.raises(ValueError):
        fw.write_tile(np.zeros((10, 10), np.uint8), tmp_path / "x.png")
