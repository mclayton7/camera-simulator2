"""[vegetation] species mix -> vegetation/mix.json (REALISM R4)."""

import json

import pytest
from fake_sources import build_synthetic, synthetic_scene

from camsim_scene import vegmix
from camsim_scene.config import ConfigError, layer_settings, parse_scene, vegetation_settings
from camsim_scene.verify import verify

BASE = {"name": "t", "bbox": [-117.4, 33.2, -117.3, 33.3]}


def test_vegetation_table_parses_and_sorts():
    plan = parse_scene({**BASE, "vegetation": {"mix": {"10": {"tree_pine_a": 0.6, "tree_broadleaf_a": 0.4}},
                                               "shrub_density": {"20": 8.0}}})
    assert plan.vegetation == {"mix": {"10": {"tree_broadleaf_a": 0.4, "tree_pine_a": 0.6}}, "shrub_density": {"20": 8.0}}
    assert list(plan.vegetation["mix"]["10"]) == ["tree_broadleaf_a", "tree_pine_a"]
    assert layer_settings(plan)["vegetation"] == plan.vegetation


def test_codes_sort_numerically():
    plan = parse_scene({**BASE, "vegetation": {"mix": {"30": {"a": 1}, "100": {"a": 1}, "20": {"a": 1}}}})
    assert list(plan.vegetation["mix"]) == ["20", "30", "100"]


@pytest.mark.parametrize("bad", [
    {"mix": {"0": {"a": 1}}},                 # code out of 1..255
    {"mix": {"256": {"a": 1}}},
    {"mix": {"010": {"a": 1}}},               # not canonical
    {"mix": {"10": {"a": 0}}},                # weight must be > 0
    {"mix": {"10": {"a": True}}},
    {"mix": {"10": {}}},                      # empty code
    {"mix": {"10": {"Bad Name": 1}}},         # type names are [a-z0-9_]+
    {"shrub_density": {"20": 101}},           # [0, 100]
    {"mix": {"10": {"a": 1}}, "extra": 1},    # unknown key
    {"mix": []},
])
def test_bad_vegetation_tables_are_refused(bad):
    with pytest.raises(ConfigError):
        parse_scene({**BASE, "vegetation": bad})


def test_no_table_no_layer_settings():
    plan = parse_scene(BASE)
    assert vegetation_settings(plan) is None
    assert "vegetation" not in layer_settings(plan)


def test_mix_json_is_canonical():
    s = {"mix": {"10": {"tree_pine_a": 0.6}}, "shrub_density": {"20": 8.0}}
    a = vegmix.mix_json(s)
    assert a == vegmix.mix_json(json.loads(json.dumps(s)))
    assert json.loads(a) == {"format": "camsim-vegmix-1", **s}
    assert a.endswith(b"\n")


def test_build_writes_hashes_and_verifies_the_mix(tmp_path):
    scene = synthetic_scene(tmp_path / "src")
    scene["vegetation"] = {"mix": {"10": {"tree_broadleaf_a": 1.0}}, "shrub_density": {"20": 6.0}}
    pkg, _cache, _ = build_synthetic(tmp_path, scene)
    mix = pkg / "vegetation" / "mix.json"
    assert json.loads(mix.read_text())["mix"] == {"10": {"tree_broadleaf_a": 1.0}}
    assert "vegetation/mix.json" in (pkg / "hashes.txt").read_text()
    report = verify(pkg)
    assert report["ok"], report
    assert any(c["name"] == "vegetation_mix" and c["ok"] for c in report["checks"])
    mix.write_text('{"format": "camsim-vegmix-1", "mix": {}}\n')
    bad = verify(pkg)
    assert not next(c for c in bad["checks"] if c["name"] == "vegetation_mix")["ok"]


def test_build_without_the_table_removes_a_stale_mix(tmp_path):
    pkg, _cache, _ = build_synthetic(tmp_path)
    assert not (pkg / "vegetation").exists()
    assert all(c["name"] != "vegetation_mix" for c in verify(pkg)["checks"])
