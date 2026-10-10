import json
import shutil

import numpy as np
import pytest
from fake_sources import build_synthetic, fast_fits, ndvi_scene

from camsim_scene import qmesh, tiling
from camsim_scene.layers import ndvi
from camsim_scene.manifest import Manifest, write_hashes
from camsim_scene.verify import base_p99_limit, verify


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    return build_synthetic(tmp_path_factory.mktemp("verify"))


def checks(report):
    return {c["name"]: c for c in report["checks"]}


def copy_pkg(src, dst):
    import shutil

    shutil.copytree(src, dst)
    return dst


def test_clean_package_passes_quick_and_deep(built, tmp_path):
    pkg, cache, _ = built
    r = verify(pkg, cache, deep=True, all_tiles=True)
    assert r["ok"], [c for c in r["checks"] if not c["ok"]]
    assert r["terrain_error_m"]["p99"] <= 0.5 and r["terrain_error_m"]["n"] > 0
    assert json.loads((pkg.parent / f"{pkg.name}.verify.json").read_text())["ok"]


def test_tampered_tile_and_extra_file_fail_hashes(built, tmp_path):
    pkg = copy_pkg(built[0], tmp_path / "p")
    tile = next((pkg / "terrain/2").rglob("*.terrain"))
    tile.write_bytes(tile.read_bytes() + b"x")
    (pkg / "imagery/stray.txt").write_text("x")
    c = checks(verify(pkg))["hashes"]
    assert not c["ok"] and "modified terrain/2/" in c["detail"] and "unlisted imagery/stray.txt" in c["detail"]


def test_missing_attribution_line_fails(built, tmp_path):
    pkg = copy_pkg(built[0], tmp_path / "p")
    text = (pkg / "ATTRIBUTION.txt").read_text().splitlines()
    (pkg / "ATTRIBUTION.txt").write_text("\n".join(text[:-3]) + "\n")
    r = verify(pkg)
    assert not checks(r)["attribution"]["ok"] and not r["ok"]


def test_missing_tile_fails_availability(built, tmp_path):
    pkg = copy_pkg(built[0], tmp_path / "p")
    next((pkg / "terrain/1").rglob("*.terrain")).unlink()
    assert not checks(verify(pkg))["terrain_available"]["ok"]


def shift_zoom(pkg, z, dh):
    """Raise every terrain tile at zoom z by dh metres and rehash, so only the deep height check can notice."""
    for p in (pkg / f"terrain/{z}").rglob("*.terrain"):
        x, y = int(p.parent.name), int(p.stem)
        q = qmesh.decode(p.read_bytes())
        lon, lat = q.lonlat(tiling.tile_bounds(z, x, y))
        p.write_bytes(
            qmesh.gzip_tile(qmesh.encode(lon, lat, q.heights() + dh, q.triangles, tiling.tile_bounds(z, x, y)))
        )
    m = Manifest.load(pkg / "manifest.json")
    m.hashes_sha256 = write_hashes(pkg)
    m.write(pkg / "manifest.json")


def test_deep_catches_wrong_heights_that_hash_correctly(built, tmp_path):
    pkg = copy_pkg(built[0], tmp_path / "p")
    shift_zoom(pkg, 6, 10.0)  # a scene zoom (above the synthetic globe's z2)
    r = verify(pkg, built[1], deep=True, all_tiles=True)
    assert checks(r)["hashes"]["ok"] and not checks(r)["terrain_heights"]["ok"]


def test_base_zooms_are_held_to_a_fraction_of_the_tin_tolerance(built, tmp_path):
    assert base_p99_limit(16) == 0.5 and base_p99_limit(8) == pytest.approx(0.7526, abs=1e-4)
    pkg = copy_pkg(built[0], tmp_path / "p")
    shift_zoom(pkg, 2, 2.0)  # well inside z2's 48 m: the datum-offset lattice is allowed metres there
    r = verify(pkg, built[1], deep=True, all_tiles=True)
    assert checks(r)["terrain_heights"]["ok"], checks(r)["terrain_heights"]["detail"]
    assert r["terrain_error_m"]["base_max_zoom"] == 2 and r["terrain_error_m"]["by_zoom"]["2"][
        "p99_limit"
    ] == pytest.approx(48.17, abs=0.01)
    assert "z3" not in [k for k, v in r["terrain_error_m"]["by_zoom"].items() if "p99_limit" in v]
    shift_zoom(pkg, 2, 500.0)  # a wrong tile, not lattice error
    c = checks(verify(pkg, built[1], deep=True, all_tiles=True))["terrain_heights"]
    assert not c["ok"] and "z2 p99" in c["detail"]


def test_malformed_inputs_fail_their_check_instead_of_crashing(built, tmp_path):
    pkg = copy_pkg(built[0], tmp_path / "p")
    (pkg / "terrain/layer.json").unlink()
    with open(pkg / "hashes.txt", "a", encoding="utf-8") as f:
        f.write("not-a-hash-line\n")
    for p in (pkg / "imagery").iterdir():
        if p.is_dir():
            import shutil

            shutil.rmtree(p)
    r = verify(pkg)
    c = checks(r)
    assert not r["ok"]
    assert not c["terrain_available"]["ok"] and "layer.json" in c["terrain_available"]["detail"]
    assert not c["hashes"]["ok"] and not c["tilemapresource"]["ok"]
    assert c["licences"]["ok"] and c["attribution"]["ok"]


def test_deep_reports_height_errors_per_zoom(built):
    pkg, cache, _ = built
    r = verify(pkg, cache, deep=True)
    by_zoom = r["terrain_error_m"]["by_zoom"]
    present = sorted(int(p.name) for p in (pkg / "terrain").iterdir() if p.is_dir())
    assert sorted(int(z) for z in by_zoom) == present
    assert all({"n", "p50", "p99", "max"} <= set(v) and v["n"] > 0 for v in by_zoom.values())
    assert sum(v["n"] for v in by_zoom.values()) == r["terrain_error_m"]["n"]
    assert "z" in checks(r)["terrain_heights"]["detail"]


def test_deep_failure_on_a_corrupt_tile_is_a_failed_check(built, tmp_path):
    pkg = copy_pkg(built[0], tmp_path / "p")
    for p in (pkg / "terrain/2").rglob("*.terrain"):
        p.write_bytes(b"\x1f\x8b garbage")
    r = verify(pkg, built[1], deep=True, all_tiles=True)
    assert not r["ok"] and not checks(r)["deep"]["ok"]


@pytest.fixture(scope="module")
def built_ndvi(tmp_path_factory):
    root = tmp_path_factory.mktemp("verify_ndvi")
    with pytest.MonkeyPatch.context() as mp:
        fast_fits(mp)
        return build_synthetic(root, ndvi_scene(root / "src"))


def test_ndvi_package_passes_deep_verify(built_ndvi):
    pkg, cache, _ = built_ndvi
    r = verify(pkg, cache, deep=True, all_tiles=True)
    assert r["ok"], [c for c in r["checks"] if not c["ok"]]
    c = checks(r)
    assert c["ndvi_tilemapresource"]["ok"] and c["ndvi_values"]["ok"]
    e = r["ndvi_error_steps"]
    assert e["n"] > 0 and e["p99"] <= 1
    assert e["naip"]["n"] + e["s2"]["n"] == e["n"] and e["naip"]["p99"] <= 1 and e["s2"]["p99"] <= 1


def test_deep_catches_wrong_ndvi_values_that_hash_correctly(built_ndvi, tmp_path):
    pkg = copy_pkg(built_ndvi[0], tmp_path / "p")
    for p in (pkg / "ndvi/10").rglob("*.png"):
        code = ndvi.read_png(p.read_bytes()).astype(int)
        p.write_bytes(ndvi.png_bytes(np.where(code > 0, np.clip(code + 10, 1, 255), 0).astype(np.uint8)))
    m = Manifest.load(pkg / "manifest.json")
    m.hashes_sha256 = write_hashes(pkg)
    m.write(pkg / "manifest.json")
    r = verify(pkg, built_ndvi[1], deep=True, all_tiles=True)
    assert checks(r)["hashes"]["ok"] and not checks(r)["ndvi_values"]["ok"]


def test_a_missing_ndvi_level_fails_its_tilemapresource(built_ndvi, tmp_path):
    pkg = copy_pkg(built_ndvi[0], tmp_path / "p")
    shutil.rmtree(pkg / "ndvi/10")
    assert not checks(verify(pkg))["ndvi_tilemapresource"]["ok"]


def test_a_package_without_ndvi_has_no_ndvi_checks(built):
    r = verify(built[0], built[1], deep=True)
    assert "ndvi_values" not in checks(r) and "ndvi_tilemapresource" not in checks(r) and "ndvi_error_steps" not in r
