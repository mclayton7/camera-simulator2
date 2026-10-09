import io

import numpy as np
import pytest
import shapely
from fake_sources import COAST, LAKE, coast_scene, fake_context, naip_s2_scene, raw_truth, tone_truth
from legacy_imagery import leaf_rgb as legacy_leaf_rgb
from PIL import Image

from camsim_scene import tiling
from camsim_scene.balance import Balance, Grid
from camsim_scene.context import WorkerState
from camsim_scene.layers import imagery
from camsim_scene.sources.base import M_PER_DEG, SourceRaster
from camsim_scene.water import Water

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


# 11 m pixels so the valid-data edge sits within ~6 m of the box edge. EDGE is placed so the z15 tile boundary at
# 10.00307 E falls ~118 m inside it: the 200 m ramp spans two tiles.
EDGE = 10.002
EDGE_SCENE = {"ref_box": (EDGE, 10.06, 10.05, 10.14), "s2_box": (9.98, 10.06, 10.05, 10.14), "res": 0.0001}
Z15 = 15  # tile ~0.0055 deg (~610 m), pixel ~2.4 m


def flat_balance():
    """Every Sentinel-2 value -> 50 DN: easy to tell from NAIP and from the raw decode."""
    return Balance.make(
        "naip_pc",
        "wc_s2",
        200.0,
        [np.array([0.0, 60000.0])] * 3,
        [np.array([50.0, 50.0])] * 3,
        Grid(9.0, 9.0, 0.02, 0.02, 1, 1),
        np.zeros((3, 1, 1)),
    )


def edge_state(tmp_path, **scene):
    st = WorkerState(fake_context(tmp_path, naip_s2_scene(tmp_path / "src", **EDGE_SCENE, **scene)))
    return st, flat_balance()


def tile_at(z, lon, lat):
    s = tiling.tile_size_deg(z)
    return int((lon + 180) // s), int((lat + 90) // s)


def pixel_lons(z, x, y):
    w, _, e, _ = tiling.tile_bounds(z, x, y)
    return w + (np.arange(256) + 0.5) * (e - w) / 256


def naip_entries(st, z, x, y):
    q = imagery.query_bounds(z, x, y, 200.0)
    return [e for e in st.index["imagery"].query(q) if imagery.source_of(e) == "naip_pc"]


def test_feather_weight_ramps_inward_and_is_continuous_across_tiles(tmp_path):
    st, _ = edge_state(tmp_path)
    x, y = tile_at(Z15, EDGE, 10.1)
    rows, lons = [], []
    for xx in (x, x + 1):  # the edge is in tile x; the ramp continues into x + 1
        rows.append(imagery.feather_weight(Z15, xx, y, naip_entries(st, Z15, xx, y), 200.0)[128])
        lons.append(pixel_lons(Z15, xx, y))
    row, lon = np.concatenate(rows), np.concatenate(lons)
    assert (row[lon < EDGE] == 0).all()
    assert (row[lon > EDGE + 230 / M_PER_DEG] == 1).all()
    assert (np.diff(row) >= -1e-9).all()
    mid = np.argmin(np.abs(lon - (EDGE + 100 / M_PER_DEG)))
    assert 0.4 < row[mid] < 0.6
    step = 2.4 / 200.0  # one ~2.4 m pixel of a 200 m ramp
    assert abs(rows[1][0] - rows[0][-1]) <= 2 * step


def test_leaf_blends_naip_into_the_matched_target(tmp_path):
    st, bal = edge_state(tmp_path)
    x, y = tile_at(Z15, EDGE, 10.1)
    entries = st.index["imagery"].query(imagery.query_bounds(Z15, x, y, 200.0))
    rgb = imagery.leaf_rgb(Z15, x, y, entries, balance=bal).astype(int)
    west = pixel_lons(Z15, x, y) < EDGE - 0.0002
    assert np.abs(rgb[:, :, west] - 50).max() <= 1  # Sentinel-2 through the balance
    x1 = x + 1  # ~118-728 m inside NAIP: the end of the ramp, then pure NAIP
    entries = st.index["imagery"].query(imagery.query_bounds(Z15, x1, y, 200.0))
    rgb = imagery.leaf_rgb(Z15, x1, y, entries, balance=bal).astype(int)
    naip = imagery.leaf_rgb(Z15, x1, y, naip_entries(st, Z15, x1, y)).astype(int)
    lon = pixel_lons(Z15, x1, y)
    deep = lon > EDGE + 300 / M_PER_DEG
    assert np.array_equal(rgb[:, :, deep], naip[:, :, deep])  # pure NAIP beyond the feather
    ramp = lon < EDGE + 180 / M_PER_DEG
    between = (rgb[:, :, ramp] - 50) * (naip[:, :, ramp] - rgb[:, :, ramp]) >= -1  # between 50 and NAIP (JPEG-free)
    assert between.all()


def test_naip_edge_with_nothing_behind_it_stays_naip(tmp_path):
    st, bal = edge_state(tmp_path)
    x, y = tile_at(Z15, EDGE, 10.1)
    entries = naip_entries(st, Z15, x, y)  # no lower-priority source at all
    rgb = imagery.leaf_rgb(Z15, x, y, entries, balance=bal).astype(int)
    naip = imagery.leaf_rgb(Z15, x, y, entries).astype(int)
    valid = pixel_lons(Z15, x, y) > EDGE + 0.0001  # ~10 m inside the valid-data edge, inside the ramp
    assert valid.any() and (naip[:, :, valid] > 0).all()
    assert np.array_equal(rgb[:, :, valid], naip[:, :, valid])  # not faded toward black


def test_adjacent_leaves_agree_across_their_shared_edge_in_the_feather(tmp_path):
    # NAIP red is a flat 150 (the offset cancels the synthetic imagery), Sentinel-2 a flat 50 through the balance:
    # red is then 50 + 100 x the feather weight alone, rising ~1.2 DN per ~2.4 m pixel along the ramp
    st, bal = edge_state(tmp_path, offset=lambda lon, lat: 150.0 - tone_truth(np.round(raw_truth(lon, lat, 0))))
    x, y = tile_at(Z15, EDGE, 10.1)  # the tile boundary to x + 1 is ~118 m inside NAIP: mid-ramp
    a, b = (
        imagery.leaf_rgb(
            Z15, xx, y, st.index["imagery"].query(imagery.query_bounds(Z15, xx, y, 200.0)), balance=bal
        ).astype(int)[0]
        for xx in (x, x + 1)
    )
    assert 100 < a[:, -1].min() and b[:, 0].max() < 130  # mid-ramp on both sides
    ramp = (np.diff(a[:, -9:], axis=1).sum(1) + np.diff(b[:, :9], axis=1).sum(1)) / 16  # DN per pixel near the seam
    seam = b[:, 0] - a[:, -1]
    assert (np.abs(seam - ramp) <= 1).all()  # the seam steps like any pixel of the ramp: the leaves agree


def test_interior_shortcut_gives_the_same_bytes(tmp_path):
    st, bal = edge_state(tmp_path)
    interior = imagery.ref_interior([shapely.box(*EDGE_SCENE["ref_box"])], 200.0)
    x, y = tile_at(Z15, 10.025, 10.1)
    assert imagery.inside(interior, Z15, x, y)
    entries = st.index["imagery"].query(imagery.query_bounds(Z15, x, y, 200.0))
    a = imagery.leaf_rgb(Z15, x, y, entries, balance=bal, interior=interior)
    b = imagery.leaf_rgb(Z15, x, y, entries, balance=bal, interior=None)
    assert np.array_equal(a, b)
    assert not imagery.inside(interior, Z15, *tile_at(Z15, EDGE, 10.1))


def test_feather_spacing_aligns_with_pixels():
    assert imagery.feather_spacing_deg(17) * M_PER_DEG == pytest.approx(2.4, rel=0.05)
    assert imagery.feather_spacing_deg(13) == tiling.tile_size_deg(13) / 256
    for z in (9, 13, 15, 17):
        assert (tiling.tile_size_deg(z) / imagery.feather_spacing_deg(z)) % 1 == 0


def test_query_bounds_pads_by_the_feather_margin():
    a = imagery.query_bounds(17, 100, 100)
    b = imagery.query_bounds(17, 100, 100, 200.0)
    assert b[0] < a[0] - 199 / M_PER_DEG and b[3] > a[3] + 199 / M_PER_DEG


COAST_REF = (10.06, 10.08, 10.14, 10.12)  # coast_scene's NAIP box


def coast_state(tmp_path, buffer_m=200.0):
    st = WorkerState(fake_context(tmp_path, coast_scene(tmp_path / "src")))
    classes = (SourceRaster(path=tmp_path / "src" / "worldcover.tif", datum="wgs84"),)
    return st, flat_balance(), Water(classes, buffer_m, 100.0)


def entries_at(st, z, x, y):
    return st.index["imagery"].query(imagery.query_bounds(z, x, y, 200.0))


def behind(entries):
    return [e for e in entries if imagery.source_of(e) != "naip_pc"]


# z15 tiles at lat 10.10: COAST falls ~396 m into tile x; tile x + 1 is 215-826 m offshore (degrees of latitude on
# both axes, as the feather): beyond the 200 m buffer and the 100 m fade.


def test_land_distance_is_zero_on_land_and_the_offshore_distance_at_sea(tmp_path):
    _, _, water = coast_state(tmp_path)
    x, y = tile_at(Z15, COAST, 10.10)
    dist = imagery.land_distance(Z15, x, y, water, 200.0)
    row = imagery.pixel_distance(Z15, x, y, dist, 200.0)[128]
    lon = pixel_lons(Z15, x, y)
    assert (row[lon < COAST - 0.0001] == 0).all()
    sea = lon > COAST + 0.0001
    assert np.abs(row[sea] - (lon[sea] - COAST) * M_PER_DEG).max() <= 6.0  # 2.4 m nodes, nearest class pixel
    assert imagery.land_distance(Z15, *tile_at(Z15, 10.077, 10.11), water, 200.0) is None  # no water near


def test_open_water_beyond_the_buffer_is_the_raw_decode_behind_naip(tmp_path):
    st, bal, water = coast_state(tmp_path)
    x, y = tile_at(Z15, COAST, 10.10)
    entries = entries_at(st, Z15, x + 1, y)
    rgb = imagery.leaf_rgb(Z15, x + 1, y, entries, balance=bal, water=water)
    assert np.array_equal(rgb, imagery.leaf_rgb(Z15, x + 1, y, behind(entries)))  # no NAIP, no colour match
    assert not np.array_equal(rgb, imagery.leaf_rgb(Z15, x + 1, y, entries, balance=bal))  # unmasked: NAIP


def test_naip_is_whole_on_land_and_fades_out_across_the_buffer(tmp_path):
    st, bal, water = coast_state(tmp_path)
    x, y = tile_at(Z15, COAST, 10.10)
    ref = naip_entries(st, Z15, x, y)
    allow = imagery.land_distance(Z15, x, y, water, 200.0) <= 200.0
    wt = imagery.feather_weight(Z15, x, y, ref, 200.0, allow)[128]
    lon = pixel_lons(Z15, x, y)
    land = lon < COAST - 0.0001
    assert (wt[land] == 1).all()
    mid = np.argmin(np.abs(lon - (COAST + 100 / M_PER_DEG)))
    assert 0.4 < wt[mid] < 0.6
    assert (wt[lon > COAST + 208 / M_PER_DEG] == 0).all()
    assert (np.diff(wt[lon > COAST]) <= 1e-9).all()
    rgb = imagery.leaf_rgb(Z15, x, y, entries_at(st, Z15, x, y), balance=bal, water=water)
    naip = imagery.leaf_rgb(Z15, x, y, ref)
    assert np.array_equal(rgb[:, :, land], naip[:, :, land])


def test_the_clip_ramp_is_continuous_across_leaves(tmp_path):
    st, _, water = coast_state(tmp_path)
    x, y = tile_at(Z15, COAST, 10.10)
    wts, dists = [], []
    for yy in (y, y + 1):  # y + 1 is north of y: its southern row meets y's northern row
        dist = imagery.land_distance(Z15, x, yy, water, 200.0)
        wts.append(imagery.feather_weight(Z15, x, yy, naip_entries(st, Z15, x, yy), 200.0, dist <= 200.0))
        dists.append(imagery.pixel_distance(Z15, x, yy, dist, 200.0))
    assert np.abs(wts[1][-1] - wts[0][0]).max() <= 2.4 / 200.0  # one ~2.4 m pixel of the ramp
    assert np.abs(dists[1][-1] - dists[0][0]).max() <= 2.4


def test_the_colour_match_fades_to_the_raw_decode_over_water(tmp_path):
    st, bal, water = coast_state(tmp_path)
    x, y = tile_at(Z15, COAST, 10.10)
    rest = behind(entries_at(st, Z15, x, y))  # Sentinel-2 (and the base) alone
    rgb = imagery.leaf_rgb(Z15, x, y, rest, balance=bal, water=water).astype(int)
    raw = imagery.leaf_rgb(Z15, x, y, rest).astype(int)
    lon = pixel_lons(Z15, x, y)
    land, sea = lon < COAST - 0.0001, lon > COAST + 105 / M_PER_DEG
    assert (rgb[:, :, land] == 50).all()  # balanced on land
    assert np.array_equal(rgb[:, :, sea], raw[:, :, sea])  # raw beyond the fade
    band = ~land & ~sea
    assert ((rgb[:, :, band] - 50) * (raw[:, :, band] - rgb[:, :, band]) >= -1).all()  # between the two


def test_the_interior_shortcut_is_not_taken_over_open_water(tmp_path):
    st, bal, water = coast_state(tmp_path)
    interior = imagery.ref_interior([shapely.box(*COAST_REF)], 200.0)
    x, y = tile_at(Z15, 10.105, 10.10)  # all open water, well inside NAIP's footprint
    assert imagery.inside(interior, Z15, x, y)
    entries = entries_at(st, Z15, x, y)
    a = imagery.leaf_rgb(Z15, x, y, entries, balance=bal, interior=interior, water=water)
    assert np.array_equal(a, imagery.leaf_rgb(Z15, x, y, entries, balance=bal, water=water))
    assert np.array_equal(a, imagery.leaf_rgb(Z15, x, y, behind(entries)))  # Sentinel-2, not NAIP


def test_a_lake_narrower_than_twice_the_buffer_keeps_naip(tmp_path):
    st, bal, water = coast_state(tmp_path)
    w, s, e, n = LAKE
    x, y = tile_at(Z15, (w + e) / 2, (s + n) / 2)
    assert imagery.land_distance(Z15, x, y, water, 200.0) is not None  # the lake is in the mask
    entries = entries_at(st, Z15, x, y)
    a = imagery.leaf_rgb(Z15, x, y, entries, balance=bal, water=water)
    assert np.array_equal(a, imagery.leaf_rgb(Z15, x, y, entries, balance=bal))


def test_a_leaf_with_no_water_near_it_is_unchanged(tmp_path):
    st, bal, water = coast_state(tmp_path)
    x, y = tile_at(Z15, 10.077, 10.11)
    for entries in (entries_at(st, Z15, x, y), behind(entries_at(st, Z15, x, y))):
        a = imagery.leaf_rgb(Z15, x, y, entries, balance=bal, water=water)
        assert np.array_equal(a, imagery.leaf_rgb(Z15, x, y, entries, balance=bal))


def test_clipped_naip_with_nothing_behind_it_stays_naip(tmp_path):
    st, bal, water = coast_state(tmp_path)
    x, y = tile_at(Z15, COAST, 10.10)
    ref = naip_entries(st, Z15, x + 1, y)
    rgb = imagery.leaf_rgb(Z15, x + 1, y, ref, balance=bal, water=water)
    assert np.array_equal(rgb, imagery.leaf_rgb(Z15, x + 1, y, ref))  # never faded toward black


def test_buffer_zero_keeps_naip_offshore(tmp_path):
    st, bal, water = coast_state(tmp_path, buffer_m=0.0)
    x, y = tile_at(Z15, COAST, 10.10)
    rgb = imagery.leaf_rgb(Z15, x + 1, y, entries_at(st, Z15, x + 1, y), balance=bal, water=water)
    assert np.array_equal(rgb, imagery.leaf_rgb(Z15, x + 1, y, naip_entries(st, Z15, x + 1, y)))
