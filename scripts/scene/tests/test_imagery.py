import io

import numpy as np
from fake_sources import fake_context
from legacy_imagery import leaf_rgb as legacy_leaf_rgb
from PIL import Image

from camsim_scene import tiling
from camsim_scene.context import WorkerState
from camsim_scene.layers import imagery

Z = 9


def decode(data):
    return np.asarray(Image.open(io.BytesIO(data)).convert("RGB")).astype(int)


def leaf(st, z, lon, lat):
    s = tiling.tile_size_deg(z)
    x, y = int((lon + 180) // s), int((lat + 90) // s)
    return decode(imagery.leaf_tile(z, x, y, st.index["imagery"].query(imagery.query_bounds(z, x, y)), 85))


def test_leaf_is_north_up_and_from_the_high_priority_source(tmp_path):
    st = WorkerState(fake_context(tmp_path))  # hi_rgb: red 250 in its north half, 200 in the south half
    img = leaf(st, 11, 10.25, 10.25)  # z11 tile inside hi_rgb, straddling its middle (10.25 N)
    assert abs(img[5, :, 0].mean() - 250) < 8 and abs(img[-5, :, 0].mean() - 200) < 8
    assert abs(img[:, :, 2].mean() - 50) < 8


def test_nodata_falls_through_to_the_next_source(tmp_path):
    st = WorkerState(fake_context(tmp_path))
    # a z11 tile straddling hi_rgb's west edge (9.9 E): hi_rgb has no data west of it, so those pixels must
    # come from base_rgb (40, 80, 160) while the eastern ones keep hi_rgb's red (>= 200)
    s = tiling.tile_size_deg(11)
    x, y = int((9.9 + 180) // s), int((10.25 + 90) // s)
    w, _, e, _ = tiling.tile_bounds(11, x, y)
    assert w < 9.9 < e
    img = decode(imagery.leaf_tile(11, x, y, st.index["imagery"].query(imagery.query_bounds(11, x, y)), 95))
    edge = int((9.9 - w) / (e - w) * 256)
    assert 8 < edge < 248
    assert np.abs(img[:, : edge - 6].reshape(-1, 3).mean(0) - [40, 80, 160]).max() < 8
    assert img[:, edge + 6 :, 0].min() >= 190


def test_parent_places_children_by_quadrant():
    colours = {(0, 1): (255, 0, 0), (1, 1): (0, 255, 0), (0, 0): (0, 0, 255), (1, 0): (255, 255, 0)}
    kids = {
        k: imagery.encode_jpeg(np.broadcast_to(np.array(c, np.uint8)[:, None, None], (3, 256, 256)).copy(), 95)
        for k, c in colours.items()
    }
    p = decode(imagery.parent_tile(kids, 95))
    assert p.shape == (256, 256, 3)
    for (dx, dy), c in colours.items():
        r0, c0 = (1 - dy) * 128, dx * 128
        assert np.abs(p[r0 + 32 : r0 + 96, c0 + 32 : c0 + 96].reshape(-1, 3).mean(0) - c).max() < 10


def test_jpeg_bytes_are_deterministic(tmp_path):
    st = WorkerState(fake_context(tmp_path))
    s = tiling.tile_size_deg(Z)
    x, y = int((10.2 + 180) // s), int((10.2 + 90) // s)
    entries = st.index["imagery"].query(imagery.query_bounds(Z, x, y))
    assert imagery.leaf_tile(Z, x, y, entries, 85) == imagery.leaf_tile(Z, x, y, entries, 85)


def test_leaf_without_balance_matches_the_legacy_algorithm(tmp_path):
    st = WorkerState(fake_context(tmp_path))
    s = tiling.tile_size_deg(11)
    for lon in (9.9, 10.25, 10.6):  # across hi_rgb's west edge, inside, across its east edge
        x, y = int((lon + 180) // s), int((10.25 + 90) // s)
        entries = st.index["imagery"].query(imagery.query_bounds(11, x, y))
        assert np.array_equal(imagery.leaf_rgb(11, x, y, entries), legacy_leaf_rgb(11, x, y, entries))


def test_sample_entries_reports_which_entry_won(tmp_path):
    st = WorkerState(fake_context(tmp_path))
    lon, lat = np.array([[9.5, 10.25]]), np.array([[10.25, 10.25]])  # outside hi_rgb, inside
    entries = st.index["imagery"].query((9.4, 10.2, 10.3, 10.3))
    vals, ok, which = imagery.sample_entries(entries, lon, lat, 100.0)
    assert ok.all() and [imagery.source_of(entries[k]) for k in which[0]] == ["base_rgb", "hi_rgb"]
    assert vals[0, 0, 1] >= 200
