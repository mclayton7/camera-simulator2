import json

import pytest
from fake_sources import build_synthetic, naip_s2_scene, synthetic_scene
from legacy_imagery import leaf_rgb as legacy_leaf_rgb

from camsim_scene import config
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
