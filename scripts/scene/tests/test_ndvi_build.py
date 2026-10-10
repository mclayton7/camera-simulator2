import json

import numpy as np
import pytest
from fake_sources import COAST, NDVI_ZOOM, build_synthetic, coast_scene, fast_fits, ndvi_scene, s2_ndvi_truth

from camsim_scene import tiling
from camsim_scene.layers import imagery, ndvi
from camsim_scene.pipeline import BuildError, build_scene


@pytest.fixture(autouse=True)
def fast(monkeypatch):
    fast_fits(monkeypatch)


def hashes(pkg):
    return (pkg / "hashes.txt").read_text()


def tile(pkg, z, lon, lat):
    s = tiling.tile_size_deg(z)
    x, y = int((lon + 180) // s), int((lat + 90) // s)
    (glon, glat), _ = imagery.pixel_grid(z, x, y)
    return ndvi.decode(ndvi.read_png((pkg / f"ndvi/{z}/{x}/{y}.png").read_bytes())), glon, glat


def test_ndvi_layer_is_written_hashed_and_fitted(tmp_path):
    pkg, _, info = build_synthetic(tmp_path, ndvi_scene(tmp_path / "src"))
    names = hashes(pkg)
    assert "ndvi/fit.json" in names and "ndvi/tilemapresource.xml" in names and "ndvi/10/" in names
    assert info["ndvi"]["fitted"] is True and info["ndvi"]["gain"] == pytest.approx(0.9, abs=0.03)
    fit = json.loads((pkg / "ndvi/fit.json").read_text())
    assert fit["format"] == 2 and len(fit["cell_offsets_e6"]) == fit["grid"]["ny"] == info["ndvi"]["grid"]["ny"]
    assert info["ndvi"]["cells"] > 0 and "cell_offset_max" in info["ndvi"]
    assert info["layers"]["ndvi"]["files"] > 0
    built = json.loads((pkg / "build.json").read_text())
    assert built["ndvi"]["samples_fit"] > 0
    per_zoom = built["layers"]["ndvi"]["per_zoom"]
    assert sorted(int(z) for z in per_zoom) == list(range(11))
    for k in ("tiles", "files", "bytes"):
        assert sum(v[k] for v in per_zoom.values()) == built["layers"]["ndvi"][k]
    pngs = list((pkg / "ndvi/10").rglob("*.png"))
    assert per_zoom["10"]["files"] == len(pngs) and per_zoom["10"]["bytes"] == sum(p.stat().st_size for p in pngs)
    v, lon, lat = tile(pkg, 10, 10.1, 10.1)
    inside = (lon > 10.01) & (lon < 10.19) & (lat > 10.01) & (lat < 10.19)
    assert np.abs(v[inside] - s2_ndvi_truth(lon, lat)[inside]).max() < 0.03  # NAIP fitted onto Sentinel-2
    assert sorted(int(p.name) for p in (pkg / "ndvi").iterdir() if p.is_dir()) == list(range(11))
    assert 'mime-type="image/png"' in (pkg / "ndvi/tilemapresource.xml").read_text()
    for p in (pkg / "ndvi").rglob("*.png"):
        assert ndvi.read_png(p.read_bytes()).any()


def test_rebuild_skips_every_ndvi_tile_and_the_fit(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path, ndvi_scene(tmp_path / "src"))
    before = hashes(pkg)
    info = build_scene(pkg, cache, jobs=1)
    assert info["ndvi"]["skipped"] is True and info["layers"]["ndvi"]["built"] == 0
    assert hashes(pkg) == before


def test_ndvi_build_is_independent_of_worker_count(tmp_path):
    s = ndvi_scene(tmp_path / "src")
    a, cache, _ = build_synthetic(tmp_path, s, jobs=1, name="a")
    b, _, _ = build_synthetic(tmp_path, s, jobs=2, name="b", cache=cache)
    assert hashes(a) == hashes(b)


def without_ndvi(text):
    return [line for line in text.splitlines() if not line.split("  ", 1)[1].startswith("ndvi/")]


def test_ndvi_off_gives_the_same_package_without_the_layer(tmp_path):
    s = ndvi_scene(tmp_path / "src")
    a, cache, _ = build_synthetic(tmp_path, s, name="on")
    b, _, info = build_synthetic(tmp_path, {**s, "ndvi": False}, name="off", cache=cache)
    assert not (b / "ndvi").exists() and info["ndvi"] is None and "ndvi" not in info["layers"]
    assert hashes(b).splitlines() == without_ndvi(hashes(a))


def test_adding_ndvi_rebuilds_no_other_tile(tmp_path):
    s = ndvi_scene(tmp_path / "src")
    pkg, cache, _ = build_synthetic(tmp_path, {**s, "ndvi": False})
    on, _, _ = build_synthetic(tmp_path, s, name="on", cache=cache)
    (pkg / "manifest.json").write_bytes((on / "manifest.json").read_bytes())
    info = build_scene(pkg, cache, jobs=1)
    assert info["layers"]["terrain"]["built"] == 0 and info["layers"]["imagery"]["built"] == 0
    assert info["layers"]["ndvi"]["built"] > 0 and hashes(pkg) == hashes(on)


def test_turning_ndvi_off_removes_the_layer(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path, ndvi_scene(tmp_path / "src"))
    m = json.loads((pkg / "manifest.json").read_text())
    del m["layers"]["ndvi"]
    (pkg / "manifest.json").write_text(json.dumps(m))
    build_scene(pkg, cache, jobs=1)
    assert not (pkg / "ndvi").exists() and not (pkg / ".state/ndvi").exists()
    assert not (pkg / ".state/ndvi_fit.json").exists() and "ndvi/" not in hashes(pkg)


def test_sentinel2_only_ndvi_builds_without_a_fit(tmp_path):
    s = ndvi_scene(tmp_path / "src")
    s["priorities"] = {**s["priorities"], "imagery": ["wc_s2", "base_rgb"]}
    del s["sources"]["naip_pc"]
    pkg, _, info = build_synthetic(tmp_path, s)
    assert info["ndvi"] is None and not (pkg / "ndvi/fit.json").exists()
    v, lon, lat = tile(pkg, 10, 10.1, 10.1)
    ok = np.isfinite(v)
    assert ok.mean() > 0.5 and np.abs(v[ok] - s2_ndvi_truth(lon, lat)[ok]).max() < 0.03


def test_coast_ndvi_is_sentinel2_offshore(tmp_path):
    zoom = {**NDVI_ZOOM, "bbox": {"terrain": 3, "imagery": 13}}
    scn = {**coast_scene(tmp_path / "src", ndvi=True), "zoom": zoom, "ndvi_max_zoom": 13}
    pkg, _, info = build_synthetic(tmp_path, scn)
    assert info["ndvi"]["fitted"] is True
    v, lon, lat = tile(pkg, 13, 10.11, 10.10)
    off = (lon > COAST + 0.004) & (lon < 10.139) & (lat > 10.081) & (lat < 10.119)
    assert np.abs(v[off] - s2_ndvi_truth(lon, lat)[off]).max() < 0.02


def test_a_format_1_fit_is_refit(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path, ndvi_scene(tmp_path / "src"))
    before = hashes(pkg)
    marker = pkg / ".state/ndvi_fit.json"
    old = b'{"format": 1, "gain_e6": 900000, "offset_e6": 50000, "reference": "naip_pc", "target": "wc_s2"}'
    (pkg / "ndvi/fit.json").write_bytes(old)  # a Task 4 package: its marker's inputs predate the grid settings
    rec = json.loads(marker.read_text())
    marker.write_text(json.dumps({**rec, "inputs": "pre-grid"}))
    info = build_scene(pkg, cache, jobs=1)
    assert info["ndvi"]["skipped"] is False and json.loads((pkg / "ndvi/fit.json").read_text())["format"] == 2
    assert hashes(pkg) == before


def test_a_manifest_planned_before_the_grid_asks_for_a_replan(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path, ndvi_scene(tmp_path / "src"))
    m = json.loads((pkg / "manifest.json").read_text())
    del m["layers"]["ndvi"]["cell_km"]
    (pkg / "manifest.json").write_text(json.dumps(m))
    with pytest.raises(BuildError, match="replan"):
        build_scene(pkg, cache, jobs=1)
