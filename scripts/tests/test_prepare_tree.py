"""prepare_tree.py helpers (REALISM R4: base colours, the procedural far mesh, the v2 library entry)."""

import math

import pytest
from vegetation import prepare_tree as P


def test_base_color_sets_the_named_material():
    doc = {"materials": [{"name": "grass", "pbrMetallicRoughness": {"baseColorFactor": [0.1, 0.8, 0.7, 1.0]}}, {"name": "x"}]}
    P.set_base_colors(doc, ["grass=0.05,0.07,0.02"])
    assert doc["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"] == [0.05, 0.07, 0.02, 1.0]
    assert "pbrMetallicRoughness" not in doc["materials"][1]


@pytest.mark.parametrize("spec", ["nomat=0.1,0.1,0.1", "grass=0.1,0.1", "grass=2,0,0", "grass"])
def test_bad_base_colors_are_refused(spec):
    with pytest.raises(SystemExit):
        P.set_base_colors({"materials": [{"name": "grass"}]}, [spec])


def test_library_entry_v2():
    e = P.library_entry("tree_pine_a", "tree", 16.0, None, "CC0-1.0", "u", "a" * 64, 900, ("tree_pine_a_far.glb", "b" * 64, 140))
    assert e["role"] == "tree" and e["crown_radius_m"] == pytest.approx(4.8) and e["thermal_class"] == "tree_canopy"
    assert e["far_gltf"] == "tree_pine_a_far.glb" and e["far_sha256"] == "b" * 64 and e["far_triangles"] == 140
    s = P.library_entry("shrub_desert", "shrub", 1.2, 0.5, "CC0-1.0", "u", "c" * 64, 60, None)
    assert s["thermal_class"] == "shrubland" and s["crown_radius_m"] == 0.5 and "far_gltf" not in s


def _mesh_doc(points_by_mat: list[tuple[str, list[tuple[float, float, float]], bool]]) -> tuple[dict, bytes]:
    """A glTF doc with one primitive per material (MASK = foliage); vertices only (no indices needed by far_mesh)."""
    import struct

    doc = {"asset": {"version": "2.0"}, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0, "translation": [0.0, 0.5, 0.0]}],
           "meshes": [{"primitives": []}], "materials": [], "accessors": [], "bufferViews": [], "buffers": [{}]}
    binary = b""
    for name, pts, foliage in points_by_mat:
        data = b"".join(struct.pack("<3f", *p) for p in pts)
        doc["bufferViews"].append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(data)})
        binary += data
        doc["accessors"].append({"bufferView": len(doc["bufferViews"]) - 1, "componentType": 5126, "count": len(pts), "type": "VEC3",
                                 "min": [min(p[i] for p in pts) for i in range(3)], "max": [max(p[i] for p in pts) for i in range(3)]})
        mat = {"name": name, "pbrMetallicRoughness": {"baseColorFactor": [0.2, 0.4, 0.1, 1.0] if foliage else [0.3, 0.2, 0.1, 1.0]}}
        if foliage:
            mat |= {"alphaMode": "MASK", "alphaCutoff": 0.5}
        doc["materials"].append(mat)
        doc["meshes"][0]["primitives"].append({"attributes": {"POSITION": len(doc["accessors"]) - 1}, "material": len(doc["materials"]) - 1})
    doc["buffers"][0]["byteLength"] = len(binary)
    return doc, binary


def _cone_points(n: int = 400):
    import math

    pts = []
    for k in range(n):
        y = 3.0 + 7.0 * k / n                     # crown from y 3 to 10 (+0.5 node offset)
        r = 2.0 * (10.0 - y) / 7.0                # cone: radius 2 at the bottom, 0 at the top
        a = 2.3999 * k
        pts.append((r * math.cos(a), y, r * math.sin(a)))
    return pts


def test_far_mesh_follows_the_crown_profile():
    trunk = [(x, y, z) for y in (0.0, 3.0) for x in (-0.2, 0.2) for z in (-0.2, 0.2)]
    doc, binary = _mesh_doc([("Leaves", _cone_points(), True), ("Bark", trunk, False)])
    fdoc, fbin = P.far_mesh(doc, binary)
    assert P.triangle_count(fdoc) <= 150
    lo, hi = P.y_extent(fdoc)
    assert lo == pytest.approx(0.5, abs=1e-6) and hi == pytest.approx(10.5, abs=0.05)   # node translation applied
    assert len(fdoc["materials"]) == 2 and all("alphaMode" not in m for m in fdoc["materials"])
    crown = P.read_positions(fdoc, fbin, fdoc["meshes"][0]["primitives"][0]["attributes"]["POSITION"])

    def rmax(y0, y1):
        return max(math.hypot(x, z) for x, y, z in crown if y0 <= y <= y1)

    assert rmax(3.4, 4.6) > rmax(6.5, 7.5) > rmax(9.0, 10.6)  # tapers like the cone
    assert fdoc["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"][:3] == pytest.approx([0.2, 0.4, 0.1])
    assert P.far_mesh(doc, binary)[1] == fbin            # deterministic


def test_far_mesh_needs_foliage():
    doc, binary = _mesh_doc([("Bark", [(0, 0, 0), (0, 1, 0), (1, 0, 0)], False)])
    with pytest.raises(SystemExit):
        P.far_mesh(doc, binary)


def test_crown_radius_is_the_p90_foliage_extent():
    doc, binary = _mesh_doc([("Leaves", _cone_points(), True), ("Bark", [(0, 0, 0), (0, 3, 0), (0.1, 0, 0)], False)])
    r = P.crown_radius(doc, binary)
    assert 1.4 < r < 2.0                                  # cone of base radius 2: p90 of the radii
    assert P.crown_radius(*_mesh_doc([("Bark", [(0, 0, 0), (0, 1, 0), (1, 0, 0)], False)])) is None
