import struct

import numpy as np

from camsim_scene import gltf


def _mesh():
    pos = np.array([[0, 0, 0], [10, 0, 0], [10, 0, -10], [0, 0, -10]], np.float32)
    nrm = np.tile(np.array([0, 1, 0], np.float32), (4, 1))
    fid = np.array([0, 0, 1, 1], np.float32)
    return gltf.TileMesh(pos, nrm, fid, np.array([0, 2, 1], np.uint32), np.array([0, 3, 2], np.uint32))


def _table():
    return gltf.PropertyTable(
        ["osm:a", "osm:bé"], ["house", ""], np.array([7.5, 4.0], np.float32), np.array([0, 2], np.uint8)
    )


def test_glb_header_alignment_and_extensions():
    data = gltf.write_glb(_mesh(), _table(), "© test")
    magic, version, length = struct.unpack_from("<III", data, 0)
    assert magic == gltf.GLB_MAGIC and version == 2 and length == len(data) and len(data) % 4 == 0
    doc, _ = gltf.read_glb(data)
    assert set(doc["extensionsUsed"]) == {"EXT_mesh_features", "EXT_structural_metadata"}
    assert doc["asset"]["copyright"] == "© test"
    assert [m["pbrMetallicRoughness"]["baseColorFactor"] for m in doc["materials"]] == [
        list(gltf.ROOF_RGBA), list(gltf.WALL_RGBA)]
    for bv in doc["bufferViews"]:
        assert bv["byteOffset"] % 8 == 0
    pos = doc["accessors"][doc["meshes"][0]["primitives"][0]["attributes"]["POSITION"]]
    assert pos["min"] == [0.0, 0.0, -10.0] and pos["max"] == [10.0, 0.0, 0.0]


def test_glb_round_trip():
    t = gltf.decode_tile(gltf.write_glb(_mesh(), _table(), "© test"))
    m = _mesh()
    assert np.array_equal(t.positions, m.positions) and np.array_equal(t.feature_ids, m.feature_ids)
    assert np.array_equal(t.roof_indices, m.roof_indices) and np.array_equal(t.wall_indices, m.wall_indices)
    assert t.table.source_ids == ["osm:a", "osm:bé"] and t.table.classes == ["house", ""]
    assert np.array_equal(t.table.height_m, _table().height_m) and list(t.table.height_source) == [0, 2]
    assert t.copyright == "© test"


def test_glb_is_deterministic():
    assert gltf.write_glb(_mesh(), _table(), "x") == gltf.write_glb(_mesh(), _table(), "x")


def test_all_empty_classes_keep_bufferviews_non_empty():
    table = gltf.PropertyTable(["a", "b"], ["", ""], np.array([7.5, 4.0], np.float32), np.array([0, 2], np.uint8))
    data = gltf.write_glb(_mesh(), table, "x")
    doc, _ = gltf.read_glb(data)
    assert all(bv["byteLength"] >= 1 for bv in doc["bufferViews"])
    assert gltf.decode_tile(data).table.classes == ["", ""]


def test_write_glb_rejects_bad_input():
    import pytest

    bad = gltf.PropertyTable(["a"], ["x"], np.array([1.0], np.float32), np.array([0], np.uint8))
    with pytest.raises(ValueError):
        gltf.write_glb(_mesh(), bad, "x")  # feature id 1 >= count 1
    short = gltf.PropertyTable(["a", "b"], ["x"], np.array([1.0, 2.0], np.float32), np.array([0, 0], np.uint8))
    with pytest.raises(ValueError):
        gltf.write_glb(_mesh(), short, "x")
    m = _mesh()
    m.roof_indices = m.wall_indices = np.zeros(0, np.uint32)
    with pytest.raises(ValueError):
        gltf.write_glb(m, _table(), "x")
