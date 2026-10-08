import json

import pytest
from fake_sources import build_synthetic

from camsim_scene import qmesh, tiling
from camsim_scene.manifest import Manifest, write_hashes
from camsim_scene.verify import verify


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


def test_deep_catches_wrong_heights_that_hash_correctly(built, tmp_path):
    pkg = copy_pkg(built[0], tmp_path / "p")
    for p in (pkg / "terrain/6").rglob("*.terrain"):
        z, x, y = 6, int(p.parent.name), int(p.stem)
        q = qmesh.decode(p.read_bytes())
        lon, lat = q.lonlat(tiling.tile_bounds(z, x, y))
        p.write_bytes(
            qmesh.gzip_tile(qmesh.encode(lon, lat, q.heights() + 10.0, q.triangles, tiling.tile_bounds(z, x, y)))
        )
    m = Manifest.load(pkg / "manifest.json")
    m.hashes_sha256 = write_hashes(pkg)
    m.write(pkg / "manifest.json")
    r = verify(pkg, built[1], deep=True, all_tiles=True)
    assert checks(r)["hashes"]["ok"] and not checks(r)["terrain_heights"]["ok"]
