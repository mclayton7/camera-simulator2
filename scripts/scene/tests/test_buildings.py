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
