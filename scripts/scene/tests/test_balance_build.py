import json

import numpy as np
import pytest
from fake_sources import COAST, POND, build_synthetic, coast_scene, naip_s2_scene, synthetic_scene
from legacy_imagery import leaf_rgb as legacy_leaf_rgb
from PIL import Image

from camsim_scene import config, tiling
from camsim_scene.context import WorkerState, coverage
from camsim_scene.layers import imagery
from camsim_scene.manifest import Manifest
from camsim_scene.pipeline import build_scene, make_context
from camsim_scene.tiling import plan_tiles, split_keys


@pytest.fixture(autouse=True)
def fast_fit(monkeypatch):
    monkeypatch.setitem(config.BALANCE, "fit_step_m", 200.0)
    monkeypatch.setitem(config.BALANCE, "min_cell_samples", 20)


def hashes(pkg):
    return (pkg / "hashes.txt").read_bytes()


def test_balance_json_is_written_hashed_and_reported(tmp_path):
    pkg, _, info = build_synthetic(tmp_path, naip_s2_scene(tmp_path / "src"))
    assert (pkg / "imagery/balance.json").is_file()
    assert "imagery/balance.json" in hashes(pkg).decode()
    assert info["balance"]["fitted"] is True
    after = info["balance"]["heldout"]["after"]["cell_bias_median_dn"]
    assert after is not None and max(after) <= 2.0
    built = json.loads((pkg / "build.json").read_text())
    assert built["balance"]["samples_fit"] > 0


def test_rebuild_skips_the_fit_and_is_identical(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path, naip_s2_scene(tmp_path / "src"))
    before = hashes(pkg)
    info = build_scene(pkg, cache, jobs=1)
    assert info["balance"]["skipped"] is True and info["layers"]["imagery"]["built"] == 0
    assert hashes(pkg) == before


def test_rebuild_from_the_manifest_alone_is_identical(tmp_path):
    a, cache, _ = build_synthetic(tmp_path, naip_s2_scene(tmp_path / "src"), name="a")
    b = tmp_path / "b"
    b.mkdir()
    (b / "manifest.json").write_bytes((a / "manifest.json").read_bytes())
    build_scene(b, cache, jobs=1)
    assert hashes(a) == hashes(b)


def test_balance_build_is_independent_of_worker_count(tmp_path):
    scene = naip_s2_scene(tmp_path / "src")
    a, cache, _ = build_synthetic(tmp_path, scene, jobs=1, name="a")
    b, _, _ = build_synthetic(tmp_path, scene, jobs=2, name="b", cache=cache)
    assert hashes(a) == hashes(b)


def _leaves_match_legacy(pkg, cache):
    m = Manifest.load(pkg / "manifest.json")
    st = WorkerState(make_context(pkg, m, cache))
    plan = plan_tiles(m.region_objs(), "imagery", coverage(m, "imagery"))
    n = 0
    for z in range(plan.max_zoom + 1):
        x, y = split_keys(plan.leaves[z])
        for a, b in zip(x.tolist(), y.tolist()):
            entries = st.index["imagery"].query(imagery.query_bounds(z, a, b))
            want = imagery.encode_jpeg(legacy_leaf_rgb(z, a, b, entries), m.layers["imagery"]["jpeg_quality"])
            assert (pkg / f"imagery/{z}/{a}/{b}.jpg").read_bytes() == want, (z, a, b)
            n += 1
    assert n > 0


def test_balance_off_and_no_margin_reproduce_legacy_tiles(tmp_path):
    scene = {**naip_s2_scene(tmp_path / "src"), "balance": False, "imagery_margin_km": 0}
    pkg, cache, info = build_synthetic(tmp_path, scene)
    assert not (pkg / "imagery/balance.json").exists() and info["balance"] is None
    _leaves_match_legacy(pkg, cache)


def test_turning_balance_off_removes_the_file_and_restores_legacy_tiles(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path, naip_s2_scene(tmp_path / "src"))
    m = json.loads((pkg / "manifest.json").read_text())
    m["layers"]["imagery"]["balance"] = None
    (pkg / "manifest.json").write_text(json.dumps(m))
    build_scene(pkg, cache, jobs=1)
    assert not (pkg / "imagery/balance.json").exists()
    _leaves_match_legacy(pkg, cache)


def test_turning_balance_on_adds_the_file_and_matches_a_fresh_build(tmp_path):
    scene = naip_s2_scene(tmp_path / "src")
    a, cache, _ = build_synthetic(tmp_path, scene, name="a")
    b, _, _ = build_synthetic(tmp_path, {**scene, "balance": False}, name="b", cache=cache)
    assert not (b / "imagery/balance.json").exists()
    m = json.loads((b / "manifest.json").read_text())
    m["layers"]["imagery"]["balance"] = json.loads((a / "manifest.json").read_text())["layers"]["imagery"]["balance"]
    (b / "manifest.json").write_text(json.dumps(m))
    info = build_scene(b, cache, jobs=1)
    assert info["balance"]["fitted"] is True and info["balance"]["skipped"] is False
    assert info["layers"]["imagery"]["built"] > 0
    assert hashes(a) == hashes(b)


def test_unreadable_old_balance_json_does_not_block_the_refit(tmp_path):
    scene = naip_s2_scene(tmp_path / "src")
    a, cache, _ = build_synthetic(tmp_path, scene, name="a")
    b, _, _ = build_synthetic(tmp_path, scene, name="b", cache=cache)
    (b / "imagery/balance.json").write_text("not json, or an old format")
    marker = b / ".state/balance.json"
    marker.write_text(json.dumps({**json.loads(marker.read_text()), "inputs": "changed"}))  # the fit must run
    info = build_scene(b, cache, jobs=1)
    assert info["balance"]["fitted"] is True and info["balance"]["skipped"] is False
    assert hashes(a) == hashes(b)


def test_no_overlap_writes_no_balance_file(tmp_path):
    scene = naip_s2_scene(tmp_path / "src", s2_box=(11.0, 11.0, 11.2, 11.2))
    pkg, cache, info = build_synthetic(tmp_path, scene)
    assert info["balance"]["fitted"] is False and not (pkg / "imagery/balance.json").exists()
    _leaves_match_legacy(pkg, cache)


def test_old_manifest_builds_without_balance(tmp_path):
    a, cache, _ = build_synthetic(tmp_path, synthetic_scene(tmp_path / "src"), name="a")
    m = json.loads((a / "manifest.json").read_text())
    for k in ("balance", "margin_km"):
        m["layers"]["imagery"].pop(k)
    m["regions"] = [r for r in m["regions"] if r["name"] != "margin"]
    b = tmp_path / "b"
    b.mkdir()
    (b / "manifest.json").write_text(json.dumps(m))
    info = build_scene(b, cache, jobs=1)
    assert info["balance"] is None and info["layers"]["imagery"]["tiles"] > 0
    _leaves_match_legacy(b, cache)


COAST_ZOOM = {
    "globe": {"terrain": 2, "imagery": 2},
    "ring": {"terrain": 3, "imagery": 3},
    "bbox": {"terrain": 3, "imagery": 13},  # z13 leaves: ~9.5 m pixels, ~2.4 km tiles
}


def coast(root, **kw):
    return {**coast_scene(root, **kw), "zoom": COAST_ZOOM}


def sea(pkg, cache, z=13, lon0=10.11, lat0=10.10):
    """The package's WorkerState, and at the leaf holding (lon0, lat0), over pixels more than ~450 m offshore inside
    NAIP's box: the built RGB, the raw decode of the sources behind NAIP, and NAIP's own decode."""
    m = Manifest.load(pkg / "manifest.json")
    st = WorkerState(make_context(pkg, m, cache))
    s = tiling.tile_size_deg(z)
    x, y = int((lon0 + 180) // s), int((lat0 + 90) // s)
    built = np.moveaxis(np.asarray(Image.open(pkg / f"imagery/{z}/{x}/{y}.jpg").convert("RGB")).astype(int), -1, 0)
    entries = st.index["imagery"].query(imagery.query_bounds(z, x, y))
    raw = imagery.leaf_rgb(z, x, y, [e for e in entries if imagery.source_of(e) != "naip_pc"]).astype(int)
    naip = imagery.leaf_rgb(z, x, y, [e for e in entries if imagery.source_of(e) == "naip_pc"]).astype(int)
    (lon, lat), _ = imagery.pixel_grid(z, x, y)
    off = (lon > COAST + 0.004) & (lon < 10.139) & (lat > 10.081) & (lat < 10.119)
    return st, built[:, off], raw[:, off], naip[:, off]


def test_open_sea_is_the_raw_decode_and_the_mask_is_reported(tmp_path):
    pkg, cache, info = build_synthetic(tmp_path, coast(tmp_path / "src"))
    assert info["balance"]["fitted"] is True and info["balance"]["water_mask"] is True
    st, built, raw, naip = sea(pkg, cache)
    assert st.water is not None and st.water.buffer_m == 200.0 and st.water.fade_m == 100.0 and st.water_shas
    assert np.abs(built - raw).mean() <= 2.0  # JPEG noise only: no NAIP, no colour match
    assert np.abs(built - naip).mean() > 20.0


def test_coast_build_is_independent_of_worker_count(tmp_path):
    scene = coast(tmp_path / "src")
    a, cache, _ = build_synthetic(tmp_path, scene, jobs=1, name="a")
    b, _, _ = build_synthetic(tmp_path, scene, jobs=2, name="b", cache=cache)
    assert hashes(a) == hashes(b)


def test_a_worldcover_change_rebuilds_the_leaves(tmp_path):
    a, cache, _ = build_synthetic(tmp_path, coast(tmp_path / "src_a"), name="a")
    b, _, _ = build_synthetic(tmp_path, coast(tmp_path / "src_b", extra_water=POND), name="b", cache=cache)
    assert hashes(a) != hashes(b)  # the pond fades the colour match out in a z13 leaf west of NAIP
    bal = (a / "imagery/balance.json").read_bytes()
    (a / "manifest.json").write_bytes((b / "manifest.json").read_bytes())
    info = build_scene(a, cache, jobs=1)
    assert (a / "imagery/balance.json").read_bytes() == bal  # the fit (NAIP's footprint only) is unchanged...
    assert info["layers"]["imagery"]["built"] > 0  # ...so only the WorldCover hashes in the leaf inputs rebuild
    assert hashes(a) == hashes(b)


def test_balance_off_ignores_the_water_mask(tmp_path):
    scene = {**coast(tmp_path / "src"), "balance": False, "imagery_margin_km": 0}
    pkg, cache, info = build_synthetic(tmp_path, scene)
    assert info["balance"] is None
    assert WorkerState(make_context(pkg, Manifest.load(pkg / "manifest.json"), cache)).water is None
    _leaves_match_legacy(pkg, cache)


def test_buffer_zero_keeps_naip_offshore(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path, {**coast(tmp_path / "src"), "naip_water_buffer_m": 0})
    _, built, _, naip = sea(pkg, cache)
    assert np.abs(built - naip).mean() <= 2.0


def test_a_balance_without_water_settings_builds_as_before(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path, coast(tmp_path / "src"))
    m = json.loads((pkg / "manifest.json").read_text())
    for k in ("naip_water_buffer_m", "water_fade_m"):
        m["layers"]["imagery"]["balance"].pop(k)
    (pkg / "manifest.json").write_text(json.dumps(m))
    build_scene(pkg, cache, jobs=1)
    st, built, _, naip = sea(pkg, cache)
    assert st.water is None
    assert np.abs(built - naip).mean() <= 2.0  # NAIP over the sea again: the Task 6 behaviour
