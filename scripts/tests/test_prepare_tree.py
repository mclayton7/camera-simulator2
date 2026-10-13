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


def _top_coverage(doc, binary, size=200):
    """Fraction of the inner 60 % (radius) of a top-down silhouette that geometry covers, and the triangles."""
    import struct

    from PIL import Image, ImageDraw
    tris = []
    for m in doc["meshes"]:
        for p in m["primitives"]:
            v = P.read_positions(doc, binary, p["attributes"]["POSITION"])
            acc = doc["accessors"][p["indices"]]
            bv = doc["bufferViews"][acc["bufferView"]]
            idx = struct.unpack_from(f"<{acc['count']}H", binary, bv.get("byteOffset", 0) + acc.get("byteOffset", 0))
            tris += [[v[i] for i in idx[t:t + 3]] for t in range(0, len(idx), 3)]
    xs = [p[0] for t in tris for p in t]
    zs = [p[2] for t in tris for p in t]
    x0, z0, span = min(xs), min(zs), max(max(xs) - min(xs), max(zs) - min(zs))
    im = Image.new("L", (size, size), 0)
    d = ImageDraw.Draw(im)
    sc = (size - 1) / span
    for t in tris:
        d.polygon([((p[0] - x0) * sc, (p[2] - z0) * sc) for p in t], fill=255)
    cx, cz, r = (-x0) * sc, (-z0) * sc, 0.6 * (size / 2) * min(max(xs) - min(xs), max(zs) - min(zs)) / span
    inner = [(i, j) for i in range(size) for j in range(size) if (i - cx) ** 2 + (j - cz) ** 2 < r * r]
    return sum(im.getpixel(q) > 0 for q in inner) / len(inner), tris


def test_procedural_shrub_is_a_solid_clump():
    doc, binary = P.procedural_shrub(height_m=2.0, radius_m=1.0, colour=(0.05, 0.06, 0.03), seed=7)
    assert P.triangle_count(doc) <= 200
    lo, hi = P.y_extent(doc)
    assert lo == pytest.approx(0.0, abs=1e-6) and hi == pytest.approx(2.0, abs=1e-6)    # library contract: base at 0, --height-m tall (placement sinks shrubs)
    cover, tris = _top_coverage(doc, binary)
    assert cover >= 0.97                                     # no hole from above (the Kenney fan covered 59 %)
    xs = [p[0] for t in tris for p in t]
    assert 1.5 <= max(xs) - min(xs) <= 2.6                   # about 2 x radius across
    m = doc["materials"]
    assert len(m) == 1 and m[0]["name"] == "shrub" and "alphaMode" not in m[0]
    assert m[0]["pbrMetallicRoughness"]["baseColorFactor"][:3] == pytest.approx([0.05, 0.06, 0.03])
    assert P.procedural_shrub(2.0, 1.0, (0.05, 0.06, 0.03), seed=7)[1] == binary   # deterministic
    assert P.procedural_shrub(2.0, 1.0, (0.05, 0.06, 0.03), seed=8)[1] != binary   # the seed varies the outline


def test_far_colour_overrides_the_mean():
    trunk = [(x, y, z) for y in (0.0, 3.0) for x in (-0.2, 0.2) for z in (-0.2, 0.2)]
    doc, binary = _mesh_doc([("Leaves", _cone_points(), True), ("Bark", trunk, False)])
    fdoc, _ = P.far_mesh(doc, binary, crown_colour=(0.03, 0.04, 0.02))
    assert fdoc["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"][:3] == pytest.approx([0.03, 0.04, 0.02])


def test_cli_procedural_shrub(tmp_path, capsys):
    rc = P.main(["--procedural-shrub", "--name", "shrub_x", "--height-m", "2.0", "--crown-radius-m", "1.1", "--seed", "3",
                 "--base-color", "shrub=0.05,0.06,0.03", "--licence", "CC0-1.0", "--source-url", "procedural",
                 "--role", "shrub", "--out", str(tmp_path)])
    assert rc == 0
    e = __import__("json").loads(capsys.readouterr().out)
    assert e["name"] == "shrub_x" and e["role"] == "shrub" and e["crown_radius_m"] == 1.1 and "far_gltf" not in e
    doc, _ = P.read_glb(tmp_path / "shrub_x.glb")
    assert P.y_extent(doc)[1] == pytest.approx(2.0, abs=1e-6)
    assert doc["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"][:3] == pytest.approx([0.05, 0.06, 0.03])
    with pytest.raises(SystemExit):   # a tree can't be procedural
        P.main(["--procedural-shrub", "--name", "t", "--height-m", "2", "--licence", "CC0-1.0", "--source-url", "p",
                "--out", str(tmp_path)])


def _faces(doc, binary):
    """(centroid, CCW geometric normal, vertex normal mean) per triangle of the first primitive."""
    import struct
    p = doc["meshes"][0]["primitives"][0]
    v = P.read_positions(doc, binary, p["attributes"]["POSITION"])
    nm = P.read_positions(doc, binary, p["attributes"]["NORMAL"])
    acc = doc["accessors"][p["indices"]]
    bv = doc["bufferViews"][acc["bufferView"]]
    idx = struct.unpack_from(f"<{acc['count']}H", binary, bv.get("byteOffset", 0) + acc.get("byteOffset", 0))
    out = []
    for t in range(0, len(idx), 3):
        a, b, c = (v[i] for i in idx[t:t + 3])
        u = [b[i] - a[i] for i in range(3)]
        w = [c[i] - a[i] for i in range(3)]
        n = (u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0])
        vn = [sum(nm[i][k] for i in idx[t:t + 3]) / 3 for k in range(3)]
        out.append(([(a[k] + b[k] + c[k]) / 3 for k in range(3)], n, vn))
    return out


def test_far_crown_faces_and_normals_point_outward():
    # glTF front faces are CCW; an inside-out crown shows its back faces lit from inside (rendered ~5-10x too dark).
    doc, binary = _mesh_doc([("Leaves", _cone_points(), True), ("Bark", [(0, 0, 0), (0, 3, 0), (0.1, 0, 0)], False)])
    fdoc, fbin = P.far_mesh(doc, binary)
    faces = _faces(fdoc, fbin)
    lo, hi = min(m[1] for m, _, _ in faces), max(m[1] for m, _, _ in faces)
    centre = (0.0, 0.5 * (lo + hi), 0.0)                               # the cone is centred on the Y axis
    for m, n, vn in faces:
        assert sum(n[k] * (m[k] - centre[k]) for k in range(3)) > 0      # away from the crown's centre (cap included)
        assert vn[0] * n[0] + vn[1] * n[1] + vn[2] * n[2] > 0           # vertex normals agree with the winding


def test_procedural_shrub_faces_up_and_out():
    doc, binary = P.procedural_shrub(2.0, 1.0, (0.05, 0.06, 0.03), seed=7)
    faces = _faces(doc, binary)
    tilt = [n[1] / math.sqrt(sum(x * x for x in n)) for _, n, _ in faces]
    assert sum(t >= -0.3 for t in tilt) >= 0.95 * len(faces)            # domes look up or sideways (jitter overhangs a little)
    assert sum(tilt) / len(tilt) > 0.15                                 # ~0.3 here; inside out they look down (~-0.3)
    assert all(vn[0] * n[0] + vn[1] * n[1] + vn[2] * n[2] > 0 for _, n, vn in faces)
