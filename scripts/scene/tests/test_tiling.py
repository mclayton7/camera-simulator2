import math
import xml.etree.ElementTree as ET

import numpy as np
import shapely

from camsim_scene import tiling, tms
from camsim_scene.tiling import Coverage, Region, plan_tiles


def globe_coverage(limit=8):
    return Coverage([shapely.box(*tiling.GLOBE)], [limit])


def test_tile_bounds_roots_and_z1():
    assert tiling.tile_bounds(0, 0, 0) == (-180.0, -90.0, 0.0, 90.0)
    assert tiling.tile_bounds(0, 1, 0) == (0.0, -90.0, 180.0, 90.0)
    assert tiling.tile_bounds(1, 3, 1) == (90.0, 0.0, 180.0, 90.0)
    assert tiling.tile_size_deg(16) == 180.0 / 65536


def test_pendleton_point_is_inside_its_z16_tile():
    lon, lat, z = -117.38, 33.22, 16
    s = tiling.tile_size_deg(z)
    x, y = math.floor((lon + 180) / s), math.floor((lat + 90) / s)
    w, so, e, n = tiling.tile_bounds(z, x, y)
    assert w <= lon < e and so <= lat < n


def test_keys_round_trip_and_children():
    k = tiling.make_keys([5, 7], [3, 9])
    x, y = tiling.split_keys(k)
    assert x.tolist() == [5, 7] and y.tolist() == [3, 9]
    cx, cy = tiling.split_keys(tiling.children(tiling.make_keys([1], [2])))
    assert sorted(zip(cx.tolist(), cy.tolist())) == [(2, 4), (2, 5), (3, 4), (3, 5)]


def test_globe_region_is_complete_to_its_zoom():
    plan = plan_tiles([Region.from_bounds("globe", tiling.GLOBE, {"terrain": 3})], "terrain", globe_coverage())
    assert [len(plan.tiles[z]) for z in range(4)] == [2, 8, 32, 128]
    assert plan.max_zoom == 3 and len(plan.leaves[3]) == 128 and all(len(plan.leaves[z]) == 0 for z in range(3))


def test_source_limit_caps_depth():
    plan = plan_tiles([Region.from_bounds("globe", tiling.GLOBE, {"terrain": 5})], "terrain", globe_coverage(2))
    assert plan.max_zoom == 2


def test_bbox_region_deepens_only_near_bbox_with_complete_siblings():
    bbox = (10.0, 10.0, 10.5, 10.5)
    regions = [
        Region.from_bounds("globe", tiling.GLOBE, {"imagery": 2}),
        Region.from_bounds("bbox", bbox, {"imagery": 7}),
    ]
    plan = plan_tiles(regions, "imagery", globe_coverage(17))
    assert plan.max_zoom == 7
    box = shapely.box(*bbox)
    for z in range(3, 8):
        x, y = tiling.split_keys(plan.tiles[z])
        parents = set(zip((x // 2).tolist(), (y // 2).tolist()))
        for px, py in parents:  # complete siblings: every parent with children has all four
            assert all(plan.has(z, 2 * px + dx, 2 * py + dy) for dx in (0, 1) for dy in (0, 1))
            assert shapely.box(*tiling.tile_bounds(z - 1, px, py)).intersects(box)


def test_bbox_on_tile_boundary_does_not_deepen_neighbour():
    b = tiling.tile_bounds(3, 9, 5)  # exactly one z3 tile
    regions = [Region.from_bounds("globe", tiling.GLOBE, {"terrain": 2}), Region.from_bounds("bbox", b, {"terrain": 4})]
    plan = plan_tiles(regions, "terrain", globe_coverage())
    x, y = tiling.split_keys(plan.tiles[4])
    assert set(zip((x // 2).tolist(), (y // 2).tolist())) == {(9, 5)}


def test_partial_source_footprint_limits_depth_locally():
    regions = [
        Region.from_bounds("globe", tiling.GLOBE, {"terrain": 1}),
        Region.from_bounds("bbox", (0, 0, 40, 40), {"terrain": 6}),
    ]
    cov = Coverage([shapely.box(*tiling.GLOBE), shapely.box(0.0, 0.0, 5.0, 5.0)], [3, 6])
    plan = plan_tiles(regions, "terrain", cov)
    x, y = tiling.split_keys(plan.tiles[6])
    s = tiling.tile_size_deg(6)
    assert len(x) > 0 and np.all(-180 + x * s < 5.0 + s) and np.all(-90 + y * s < 5.0 + s)


def test_available_ranges_merge_runs_and_rows():
    tiles = {0: tiling.make_keys([0, 1], [0, 0]), 1: tiling.make_keys([0, 1, 0, 1, 3], [0, 0, 1, 1, 1])}
    plan = tiling.LayerPlan(tiles, {0: tiles[0][:0], 1: tiles[1]})
    av = tiling.available_ranges(plan)
    assert av[0] == [{"startX": 0, "startY": 0, "endX": 1, "endY": 0}]
    assert av[1] == [{"startX": 0, "startY": 0, "endX": 1, "endY": 1}, {"startX": 3, "startY": 1, "endX": 3, "endY": 1}]
    back = tiling.available_keys(av)
    assert np.array_equal(np.sort(back[1]), np.sort(tiles[1]))


def test_tilemapresource_xml_round_trip():
    xml = tms.tilemapresource_xml("pendleton", 2, tiling.GLOBE)
    root = ET.fromstring(xml)
    assert root.find("SRS").text == "EPSG:4326"
    assert root.find("TileSets").get("profile") == "geodetic"
    sets = root.find("TileSets").findall("TileSet")
    assert [s.get("href") for s in sets] == ["0", "1", "2"]
    assert float(sets[2].get("units-per-pixel")) == 180.0 / 256 / 4
    assert tms.parse_tilemapresource(xml) == ([0, 1, 2], tiling.GLOBE)


def test_ndvi_plan_caps_the_imagery_and_keeps_to_the_area():
    regions = [
        Region.from_bounds("globe", tiling.GLOBE, {"imagery": 2}),
        Region.from_bounds("bbox", (10.0, 10.0, 10.5, 10.5), {"imagery": 9}),
    ]
    cov = Coverage()
    cov.add(shapely.box(*tiling.GLOBE), 9)
    ip = plan_tiles(regions, "imagery", cov)
    area = shapely.box(10.0, 10.0, 10.2, 10.2)
    p = tiling.ndvi_plan(ip, 7, area)
    assert p.max_zoom == 7 and np.array_equal(p.leaves[7], p.tiles[7])
    for z, keys in p.tiles.items():
        assert shapely.intersects(tiling.tile_boxes(z, keys), area).all() and np.isin(keys, ip.tiles[z]).all()
    assert len(p.tiles[7]) < len(ip.tiles[7])
    far = tiling.ndvi_plan(ip, 7, shapely.box(50, 50, 51, 51))  # only the globe's z0-2 reach it
    assert far.max_zoom == 2 and np.array_equal(far.leaves[2], far.tiles[2])
    assert tiling.ndvi_plan(ip, 7, shapely.Polygon()).tiles == {}


def test_tilemapresource_names_png_tiles():
    xml = tms.tilemapresource_xml("t", 3, (0.0, 0.0, 1.0, 1.0), "image/png", "png")
    tf = ET.fromstring(xml).find("TileFormat")
    assert tf.get("mime-type") == "image/png" and tf.get("extension") == "png"
    assert 'mime-type="image/jpeg"' in tms.tilemapresource_xml("t", 3, (0.0, 0.0, 1.0, 1.0))
