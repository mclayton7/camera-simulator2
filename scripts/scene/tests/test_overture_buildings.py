import duckdb
import pytest
from overture_fixture import box_lonlat, write_overture_parquet

from camsim_scene.cache import Cache, CacheError
from camsim_scene.sources import make_source
from camsim_scene.sources.base import Area, Layer
from camsim_scene.sources.overture_buildings import snapshot_url

BBOX = (10.0, 10.0, 10.1, 10.1)


def _rows():
    return [
        {"id": "b2", "geom": box_lonlat(10.05, 10.05, 20, 10), "height": 7.5},
        {"id": "b1", "geom": box_lonlat(10.02, 10.02, 12, 12), "num_floors": 2},
        {"id": "far", "geom": box_lonlat(11.0, 11.0, 10, 10)},  # outside the bbox
    ]


def _src(tmp_path, parquet=None, release="2026-09-23.1"):
    opts = {"release": release}
    if parquet is not None:
        opts["parquet"] = str(parquet)
    return make_source("overture_buildings", None, opts)


def test_source_metadata():
    s = make_source("overture_buildings", None, {"release": "2026-09-23.1"})
    assert s.layer == Layer.BUILDINGS and s.licence == "ODbL-1.0" and s.area_kind == "bbox"
    assert s.version == "2026-09-23.1"


def test_discover_needs_no_network_and_names_release_and_bbox():
    s = make_source("overture_buildings", None, {"release": "2026-09-23.1"})
    [a] = s.discover(Area(BBOX))
    assert a.url == snapshot_url("2026-09-23.1", BBOX)
    assert "2026-09-23.1" in a.url and "10.1" in a.url
    assert a.metadata == {"release": "2026-09-23.1", "bbox": list(BBOX)}


def test_snapshot_keeps_bbox_rows_sorted_by_id(tmp_path):
    pq = write_overture_parquet(tmp_path / "src.parquet", _rows())
    s = _src(tmp_path, pq)
    [a] = s.discover(Area(BBOX))
    path, sha, size = s.fetch_blob(a, Cache(tmp_path / "cache"))
    ids = [r[0] for r in duckdb.connect().execute(f"SELECT id FROM read_parquet('{path}')").fetchall()]
    assert ids == ["b1", "b2"] and size == path.stat().st_size and len(sha) == 64


def test_fetch_uses_cached_snapshot_without_query(tmp_path):
    pq = write_overture_parquet(tmp_path / "src.parquet", _rows())
    cache = Cache(tmp_path / "cache")
    s = _src(tmp_path, pq)
    [a] = s.discover(Area(BBOX))
    p1, sha1, _ = s.fetch_blob(a, cache)
    pq.unlink()  # the "release" is gone
    a.sha256 = sha1
    p2, sha2, _ = s.fetch_blob(a, cache)
    assert (p2, sha2) == (p1, sha1)


def test_missing_release_names_it(tmp_path):
    s = _src(tmp_path, tmp_path / "missing.parquet", release="2025-01-01.0")
    [a] = s.discover(Area(BBOX))
    with pytest.raises(CacheError, match="2025-01-01.0"):
        s.fetch_blob(a, Cache(tmp_path / "cache"))


def test_put_file_moves_into_blobs_and_indexes_url(tmp_path):
    cache = Cache(tmp_path / "cache")
    f = tmp_path / "x.bin"
    f.write_bytes(b"abc")
    path, sha, size = cache.put_file("overture://x", f)
    assert path == cache.blob_path(sha) and size == 3 and not f.exists()
    assert cache.get("overture://x") == (path, sha, 3)


@pytest.mark.network
def test_live_query_tiny_bbox(tmp_path):
    """Oceanside pier area: the real S3 path, schema and geometry type."""
    s = make_source("overture_buildings", None, {"release": "2026-09-23.1"})
    [a] = s.discover(Area((-117.386, 33.192, -117.382, 33.196)))
    path, _, _ = s.fetch_blob(a, Cache(tmp_path / "cache"))
    row = duckdb.connect().execute(f"SELECT id, geometry, typeof(geometry) FROM read_parquet('{path}') LIMIT 1").fetchone()
    assert row is not None and row[2] == "BLOB"
