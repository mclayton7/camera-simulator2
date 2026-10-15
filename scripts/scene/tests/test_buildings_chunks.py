import duckdb
import shapely
from overture_fixture import box_lonlat, write_overture_parquet

from camsim_scene import buildings as B
from camsim_scene import buildings_chunks as C
from camsim_scene.tiling import tile_size_deg


def _snapshot(tmp_path, rows):
    src = write_overture_parquet(tmp_path / "src.parquet", rows)
    out = tmp_path / "snap.parquet"
    duckdb.connect().execute(
        f"COPY (SELECT id, geometry, height, num_floors, class, subtype, to_json(sources) AS sources "
        f"FROM read_parquet('{src}') ORDER BY id) TO '{out}' (FORMAT parquet)"
    )
    return out


def _edge_lon():
    cx, _ = B.chunk_of(10.2, 10.2)
    return (cx + 1) * tile_size_deg(B.CHUNK_ZOOM) - 180.0  # east edge of 10.2's chunk


def _rows():
    e = _edge_lon()
    return [
        {"id": "a1", "geom": box_lonlat(10.2, 10.2, 40, 30), "height": 9.0},
        {"id": "a2", "geom": box_lonlat(e - 3e-5, 10.2, 30, 20)},  # straddles the chunk edge, centroid west
        {"id": "a3", "geom": box_lonlat(e + 3e-5, 10.2, 30, 20)},  # straddles it, centroid east
        {"id": "a4", "geom": box_lonlat(10.3, 10.3, 2, 2)},  # small
        {"id": "a5", "geom": shapely.LineString([(10.2, 10.2), (10.201, 10.2)])},  # invalid
        {"id": "a6", "geom": box_lonlat(10.25, 10.25, 20, 20), "num_floors": 3},
        {"id": "a6", "geom": box_lonlat(10.26, 10.26, 20, 20)},  # duplicate id: the first row wins
    ]


def _reference(snap, min_area):
    """The whole-box path: read_snapshot + stage, chunk by centroid."""
    want, counts = {}, {"input": 0, "dropped_invalid": 0, "dropped_small": 0}
    for fp in B.read_snapshot(snap):
        counts["input"] += 1
        s = B.stage(fp, min_area)
        if isinstance(s, str):
            counts[f"dropped_{s}"] += 1
        else:
            want.setdefault(B.chunk_of(s[1], s[2]), []).append(fp.source_id)
    return want, counts


def test_partition_matches_whole_box_staging(tmp_path):
    snap = _snapshot(tmp_path, _rows())
    p = C.partition(snap, 10.0, tmp_path / "part", "k", jobs=1, range_rows=3)
    want, counts = _reference(snap, 10.0)
    assert p["counts"] == counts and p["inputs"] == "k"
    assert [tuple(c) for c in p["chunks"]] == sorted(want) and len(want) >= 2
    for (cx, cy), ids in want.items():
        assert [f.source_id for f in C.read_chunk(tmp_path / "part", cx, cy)] == ids
    [a6] = [f for c in want for f in C.read_chunk(tmp_path / "part", *c) if f.source_id == "a6"]
    assert a6.num_floors == 3  # first row of the duplicate id


def test_partition_dedups_across_range_boundary(tmp_path):
    snap = _snapshot(tmp_path, _rows())
    for n in (1, 2, 5, 6):  # n = 6 splits the a6 pair (rows 5 | 6); the others keep it together or split elsewhere
        p = C.partition(snap, 10.0, tmp_path / f"p{n}", "k", jobs=1, range_rows=n)
        ids = sorted(f.source_id for c in p["chunks"] for f in C.read_chunk(tmp_path / f"p{n}", *c))
        assert ids == ["a1", "a2", "a3", "a6"] and p["counts"]["input"] == 6


def test_chunk_follows_cleaned_centroid(tmp_path):
    e = _edge_lon()
    # a block just west of the chunk edge with a long thin spike east: the bbox centre is east of the edge, the
    # centroid (by area) west of it
    shape = shapely.Polygon([(e - 6e-4, 10.2), (e + 2e-3, 10.2), (e + 2e-3, 10.200001), (e - 1e-5, 10.200001),
                             (e - 1e-5, 10.2003), (e - 6e-4, 10.2003)])
    snap = _snapshot(tmp_path, [{"id": "spike", "geom": shape}])
    [s] = [B.stage(fp, 10.0) for fp in B.read_snapshot(snap)]
    w, so, ea, n = shape.bounds
    assert B.chunk_of((w + ea) / 2, (so + n) / 2) != B.chunk_of(s[1], s[2])
    p = C.partition(snap, 10.0, tmp_path / "part", "k", jobs=1)
    assert [tuple(c) for c in p["chunks"]] == [B.chunk_of(s[1], s[2])]


def test_partition_reuses_a_complete_partition_and_redoes_a_partial_one(tmp_path):
    snap = _snapshot(tmp_path, _rows())
    first = C.partition(snap, 10.0, tmp_path / "part", "k", jobs=1)
    marker = tmp_path / "part" / C.PARTITION_MARKER
    stamp = marker.stat().st_mtime_ns
    assert C.partition(snap, 10.0, tmp_path / "part", "k", jobs=1) == first and marker.stat().st_mtime_ns == stamp
    marker.unlink()  # crashed before the marker
    assert C.partition(snap, 10.0, tmp_path / "part", "k", jobs=1)["chunks"] == first["chunks"]
    assert C.partition(snap, 10.0, tmp_path / "part", "k2", jobs=1)["inputs"] == "k2"  # new key: redone


def test_partition_in_the_pool_equals_inline(tmp_path):
    snap = _snapshot(tmp_path, _rows())
    one = C.partition(snap, 10.0, tmp_path / "one", "k", jobs=1, range_rows=2)
    two = C.partition(snap, 10.0, tmp_path / "two", "k", jobs=2, range_rows=2)
    assert {k: v for k, v in one.items() if k != "peak_rss_mb"} == {k: v for k, v in two.items() if k != "peak_rss_mb"}
    for c in one["chunks"]:
        a = [(f.source_id, f.geometry.wkb) for f in C.read_chunk(tmp_path / "one", *c)]
        assert a == [(f.source_id, f.geometry.wkb) for f in C.read_chunk(tmp_path / "two", *c)]
