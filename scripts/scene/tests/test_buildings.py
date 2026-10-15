import numpy as np
import pytest
import shapely
from overture_fixture import box_lonlat, write_overture_parquet

from camsim_scene import buildings as B
from camsim_scene import geodesy


def flat(h):
    return lambda lon, lat: np.full(np.shape(lon), float(h))


def slope(lon, lat):
    """Rises 1 m per metre eastward from 100 m at lon 10.0 (lat ~10)."""
    return 100.0 + (np.asarray(lon) - 10.0) * 111320.0 * np.cos(np.radians(10.0))


def test_height_fallbacks_and_clamps():
    assert B.building_height(7.5, 3) == (7.5, 0)
    assert B.building_height(None, 2) == (6.4, 1)
    assert B.building_height(0.0, None) == (4.0, 2)
    assert B.building_height(-3.0, 0) == (4.0, 2)
    assert B.building_height(1.0, None) == (2.5, 0)
    assert B.building_height(400.0, None) == (150.0, 0)
    assert B.building_height(None, 100) == (150.0, 1)


def test_tiers():
    assert B.tier_zoom(2000.0) == 13 and B.tier_zoom(1999.9) == 14 and B.tier_zoom(300.0) == 14
    assert B.tier_zoom(299.9) == 15


def test_tile_of_matches_tile_bounds():
    from camsim_scene.tiling import tile_bounds

    x, y = B.tile_of(-117.45, 33.30, 15)
    w, s, e, n = tile_bounds(15, x, y)
    assert w <= -117.45 < e and s <= 33.30 < n


def test_clean_repairs_bowtie_and_drops_lines():
    bowtie = shapely.Polygon([(10.0, 10.0), (10.001, 10.001), (10.001, 10.0), (10.0, 10.001)])
    parts = B.clean(bowtie, 10.0)
    assert len(parts) == 2 and all(p.is_valid and p.exterior.is_ccw for p in parts)
    assert B.clean(shapely.LineString([(10, 10), (10.001, 10)]), 10.0) == []


def test_clean_drops_slivers_and_orients_holes():
    tiny = box_lonlat(10.0, 10.0, 2, 2)  # 4 m2
    assert B.clean(tiny, 10.0) == []
    ring = box_lonlat(10.0, 10.0, 30, 30).difference(box_lonlat(10.0, 10.0, 10, 10))
    [p] = B.clean(ring, 10.0)
    assert p.exterior.is_ccw and not p.interiors[0].is_ccw


def test_plan_base_is_min_ground_minus_half_metre_on_a_slope():
    fp = B.Footprint("osm:w1", box_lonlat(10.0005, 10.0, 20, 10), 6.0, None, "house")
    [b], stats = B.plan_buildings([fp], slope, 10.0)
    west_edge = 10.0005 - 10.0 / (111320.0 * np.cos(np.radians(10.0)))
    assert b.base_m == pytest.approx(slope(west_edge, 10.0) - 0.5, abs=1e-6)
    assert b.roof_m == pytest.approx(b.base_m + 0.5 + 6.0)
    assert stats == {"input": 1, "kept": 1, "dropped_small": 0, "dropped_invalid": 0, "dropped_no_height": 0}


def test_plan_drops_buildings_without_ground_height():
    fp = B.Footprint("osm:w1", box_lonlat(10.0, 10.0, 20, 20), 6.0, None, "")
    kept, stats = B.plan_buildings([fp], lambda lon, lat: np.full(np.shape(lon), np.nan), 10.0)
    assert kept == [] and stats["dropped_no_height"] == 1


def test_plan_tier_and_tile_follow_area_and_centroid():
    big = B.Footprint("osm:big", box_lonlat(10.0, 10.0, 50, 50), None, None, "")
    small = B.Footprint("osm:small", box_lonlat(10.01, 10.01, 10, 10), None, None, "")
    got = {b.source_id: b for b in B.plan_buildings([big, small], flat(0.0), 10.0)[0]}
    assert got["osm:big"].tile[0] == 13 and got["osm:small"].tile[0] == 15
    assert got["osm:small"].tile[1:] == B.tile_of(10.01, 10.01, 15)


def _decode_up(mesh, origin):
    """glTF (e, u, -n) -> ellipsoid heights of every vertex."""
    enu = np.stack([mesh.positions[:, 0], -mesh.positions[:, 2], mesh.positions[:, 1]], axis=1).astype(np.float64)
    m = geodesy.enu_to_ecef_matrix(*origin)
    ecef = (m[:3, :3] @ enu.T).T + m[:3, 3]
    return geodesy.ecef_to_geodetic(ecef)


def test_mesh_round_trips_heights_and_roof_area_with_a_hole():
    ring = box_lonlat(10.0, 10.0, 30, 30).difference(box_lonlat(10.0, 10.0, 10, 10))
    fp = B.Footprint("osm:r1", ring, 9.0, None, "industrial")
    [b], _ = B.plan_buildings([fp], flat(50.0), 10.0)
    origin = (10.0, 10.0, 55.0)
    mesh, table = B.tile_mesh([b], origin)
    _, _, h = _decode_up(mesh, origin)
    assert h.min() == pytest.approx(49.5, abs=1e-3) and h.max() == pytest.approx(59.0, abs=1e-3)
    tri = mesh.positions[mesh.roof_indices].reshape(-1, 3, 3).astype(np.float64)
    cross = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    assert (cross[:, 1] > 0).all()  # every roof triangle faces up (glTF +Y)
    assert 0.5 * np.linalg.norm(cross, axis=1).sum() == pytest.approx(
        800.0, rel=1e-2
    )  # 900 - 100 m2; box_lonlat uses 111320 m/deg of lat, WGS-84 is 0.6% less at lat 10
    assert table.source_ids == ["osm:r1"] and table.classes == ["industrial"]
    assert list(table.height_source) == [0] and table.height_m[0] == pytest.approx(9.0)


def test_mesh_walls_face_outward():
    fp = B.Footprint("osm:w1", box_lonlat(10.0, 10.0, 20, 20), 5.0, None, "")
    [b], _ = B.plan_buildings([fp], flat(0.0), 10.0)
    mesh, _ = B.tile_mesh([b], (10.0, 10.0, 0.0))
    tri = mesh.positions[mesh.wall_indices].reshape(-1, 3, 3).astype(np.float64)
    centre = tri.mean(axis=1)
    normal = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    horiz = centre[:, [0, 2]]  # (e, -n) from the building centre at the origin
    assert (np.einsum("ij,ij->i", normal[:, [0, 2]], horiz) > 0).all()
    assert np.allclose(np.linalg.norm(mesh.normals, axis=1), 1.0, atol=1e-5)


def test_mesh_skips_zero_length_edges():
    p = shapely.Polygon([(10.0, 10.0), (10.0002, 10.0), (10.0002, 10.0), (10.0002, 10.0002), (10.0, 10.0002)])
    fp = B.Footprint("osm:d", p, 5.0, None, "")
    [b], _ = B.plan_buildings([fp], flat(0.0), 10.0)
    mesh, _ = B.tile_mesh([b], (10.0, 10.0, 0.0))
    assert len(mesh.wall_indices) == 4 * 6 and np.isfinite(mesh.normals).all()


def test_feature_ids_follow_source_id_order():
    a = B.Footprint("osm:b", box_lonlat(10.0, 10.0, 10, 10), 5.0, None, "")
    c = B.Footprint("osm:a", box_lonlat(10.0003, 10.0, 10, 10), 5.0, None, "")
    blds, _ = B.plan_buildings([a, c], flat(0.0), 10.0)
    mesh, table = B.tile_mesh(blds, (10.0, 10.0, 0.0))
    assert table.source_ids == ["osm:a", "osm:b"] and set(np.unique(mesh.feature_ids)) == {0.0, 1.0}


def test_read_snapshot_orders_and_dedups(tmp_path):
    rows = [
        {"id": "b", "geom": box_lonlat(10.0, 10.0, 10, 10), "height": 5.0, "class": "house"},
        {"id": "a", "geom": box_lonlat(10.001, 10.0, 10, 10), "num_floors": 2},
        {"id": "a", "geom": box_lonlat(10.002, 10.0, 10, 10)},
    ]
    src = write_overture_parquet(tmp_path / "src.parquet", rows)
    import duckdb

    out = tmp_path / "snap.parquet"
    duckdb.connect().execute(
        f"COPY (SELECT id, geometry, height, num_floors, class, subtype, to_json(sources) AS sources "
        f"FROM read_parquet('{src}') ORDER BY id) TO '{out}' (FORMAT parquet)"
    )
    fps = B.read_snapshot(out)
    assert [f.source_id for f in fps] == ["a", "b"]
    assert fps[0].num_floors == 2 and fps[1].height == 5.0 and fps[1].cls == "house" and fps[0].cls == ""


def _tiles(blds):
    by = {}
    for b in blds:
        by.setdefault(b.tile, []).append(b)
    return {k: B.tile_summary(k, v) for k, v in by.items()}


def test_tileset_tree_tiers_errors_and_transforms():
    big = B.Footprint("osm:big", box_lonlat(10.0, 10.0, 50, 50), 20.0, None, "")
    small = B.Footprint("osm:small", box_lonlat(10.001, 10.001, 10, 10), 5.0, None, "")
    blds, _ = B.plan_buildings([big, small], flat(0.0), 10.0)
    doc = B.tileset_json(_tiles(blds))
    assert doc["asset"]["version"] == "1.1" and doc["geometricError"] == 200.0
    root = doc["root"]
    assert root["refine"] == "ADD" and "content" not in root and root["geometricError"] == 200.0
    [z13] = root["children"]
    assert z13["geometricError"] == 60.0 and z13["content"]["uri"].startswith("13/")
    [z14] = z13["children"]
    assert z14["geometricError"] == 20.0 and "content" not in z14 and "transform" not in z14
    [z15] = z14["children"]
    assert z15["geometricError"] == 6.0 and z15["content"]["uri"].startswith("15/")
    walked = dict(B.walk_tileset(doc))
    for (z, x, y), (region, origin) in _tiles(blds).items():
        assert np.allclose(walked[f"{z}/{x}/{y}.glb"], geodesy.enu_to_ecef_matrix(*origin), atol=1e-3, rtol=0)


def test_tile_region_contains_straddling_building():
    from camsim_scene.tiling import tile_bounds

    x, y = B.tile_of(10.0, 10.0, 15)
    w, s, e, n = tile_bounds(15, x, y)
    # centroid just inside the east edge, footprint 20 m across the edge
    fp = B.Footprint("osm:edge", box_lonlat(e - 2e-5, (s + n) / 2, 20, 10), 5.0, None, "")
    [b], _ = B.plan_buildings([fp], flat(0.0), 10.0)
    assert b.tile == (15, x, y)
    doc = B.tileset_json(_tiles([b]))
    node = doc["root"]["children"][0]["children"][0]["children"][0]
    rw, rs, re, rn, lo, hi = node["boundingVolume"]["region"]
    bw, bs, be, bn = b.parts[0].bounds
    assert rw <= np.radians(bw) and re >= np.radians(be) and lo <= b.base_m and hi >= b.roof_m
    for parent in (doc["root"], doc["root"]["children"][0]):
        pr = parent["boundingVolume"]["region"]
        assert pr[0] <= rw and pr[2] >= re and pr[4] <= lo and pr[5] >= hi


def test_stage_matches_plan_and_reports_drops():
    good = B.Footprint("g", box_lonlat(10.0, 10.0, 20, 20), None, None, "")
    small = B.Footprint("s", box_lonlat(10.0, 10.0, 2, 2), None, None, "")
    line = B.Footprint("l", shapely.LineString([(10, 10), (10.001, 10)]), None, None, "")
    parts, cx, cy, area = B.stage(good, 10.0)
    assert len(parts) == 1 and abs(cx - 10.0) < 1e-9 and abs(cy - 10.0) < 1e-9 and 390 < area < 410
    assert B.stage(small, 10.0) == "small" and B.stage(line, 10.0) == "invalid"
    assert B.chunk_of(cx, cy) == B.tile_of(cx, cy, B.CHUNK_ZOOM)


def test_building_tiles_nest_in_their_chunk():
    for lon, lat in ((10.0, 10.0), (-117.4, 33.3), (0.0219, -0.0001)):
        cx, cy = B.chunk_of(lon, lat)
        for z in (13, 14, 15):
            x, y = B.tile_of(lon, lat, z)
            assert (x >> (z - B.CHUNK_ZOOM), y >> (z - B.CHUNK_ZOOM)) == (cx, cy)


def _two_far_tiles():
    a = B.Footprint("osm:a", box_lonlat(10.0, 10.0, 50, 50), 20.0, None, "")  # z13
    b = B.Footprint("osm:b", box_lonlat(10.5, 10.5, 50, 50), 20.0, None, "")  # z13, ~70 km away
    small = B.Footprint("osm:c", box_lonlat(10.5005, 10.5005, 10, 10), 5.0, None, "")  # z15 under b's z13
    blds, _ = B.plan_buildings([a, b, small], flat(0.0), 10.0)
    return blds


def test_tileset_without_near_only_is_unchanged_by_the_argument():
    tiles = _tiles(_two_far_tiles())
    assert B.tileset_json(tiles) == B.tileset_json(tiles, near_only=None)
    assert all(c["geometricError"] == 60.0 for c in B.tileset_json(tiles)["root"]["children"])


def test_tileset_outside_nodes_sit_under_contentless_ancestors():
    tiles = _tiles(_two_far_tiles())
    lon_a = np.radians(10.25)  # a (lon 10.0) is inside, b (lon 10.5) outside
    doc = B.tileset_json(tiles, near_only=lambda r: r[0] > lon_a)
    root = doc["root"]
    assert root["geometricError"] == 200.0 and doc["geometricError"] == 200.0
    inside = [c for c in root["children"] if c["geometricError"] == 60.0]
    outer = [c for c in root["children"] if c["geometricError"] == B.OUTSIDE_GEOMETRIC_ERROR]
    assert len(inside) == 1 and inside[0]["content"]["uri"].startswith("13/") and len(outer) == 1
    node, depth = outer[0], 0
    while node["geometricError"] == B.OUTSIDE_GEOMETRIC_ERROR:
        assert "content" not in node and "transform" not in node and len(node["children"]) == 1
        node, depth = node["children"][0], depth + 1
    assert depth == 4 and node["content"]["uri"].startswith("13/")  # z9, z10, z11, z12 ancestors
    plain = sorted(B.walk_tileset(B.tileset_json(tiles)), key=lambda t: t[0])
    grouped = sorted(B.walk_tileset(doc), key=lambda t: t[0])
    assert [u for u, _ in plain] == [u for u, _ in grouped]
    assert all(np.array_equal(m1, m2) for (_, m1), (_, m2) in zip(plain, grouped))
    r = outer[0]["boundingVolume"]["region"]
    child = node["boundingVolume"]["region"]
    assert r[0] <= child[0] and r[2] >= child[2] and r[4] <= child[4] and r[5] >= child[5]
