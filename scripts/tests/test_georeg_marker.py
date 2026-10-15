"""Unit tests for scripts/georeg/make_marker.py (REALISM R6 marker plate)."""

import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "georeg"))
import make_marker  # noqa: E402


def test_build_is_deterministic():
    assert make_marker.build() == make_marker.build()


def test_glb_header_and_bounds():
    b = make_marker.build()
    magic, version, length = struct.unpack_from("<4sII", b, 0)
    assert (magic, version, length) == (b"glTF", 2, len(b))
    jlen, jtype = struct.unpack_from("<I4s", b, 12)
    assert jtype == b"JSON"
    doc = json.loads(b[20 : 20 + jlen])
    pos = doc["accessors"][doc["meshes"][0]["primitives"][0]["attributes"]["POSITION"]]
    # glTF is Y-up: a 2 m x 2 m plate in x / z, 5 cm thick, origin at the centre of its bottom face.
    assert pos["min"] == [-1.0, 0.0, -1.0] and pos["max"] == [1.0, 0.05, 1.0]
    blen, btype = struct.unpack_from("<I4s", b, 20 + jlen)
    assert btype == b"BIN\x00" and 20 + jlen + 8 + blen == len(b)


def test_committed_glb_matches_the_writer():
    glb = Path(__file__).resolve().parents[2] / "entities" / "marker" / "plate.glb"
    assert glb.read_bytes() == make_marker.build()
