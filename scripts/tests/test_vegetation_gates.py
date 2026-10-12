"""Unit tests for the REALISM R4 'vegetation deeper' gates in scripts/vegetation_check.py (pure functions)."""

import math

import pytest
import vegetation_check as vc

LIB = {"format": "camsim-vegetation-2", "types": [
    {"name": "tree_broadleaf_a", "role": "tree", "height_m": 9.0, "crown_radius_m": 2.7},
    {"name": "tree_pine_a", "role": "tree", "height_m": 22.0, "crown_radius_m": 6.78},
    {"name": "shrub_chaparral", "role": "shrub", "height_m": 2.0},
]}
MIX = {"format": "camsim-vegmix-1", "mix": {"10": {"tree_pine_a": 3.0, "tree_broadleaf_a": 1.0, "shrub_chaparral": 1.0},
                                            "20": {"shrub_chaparral": 1.0}}, "shrub_density": {"20": 8.0}}


def test_mix_roles_split_by_library_role():
    roles = vc.mix_roles(MIX, LIB)
    assert roles[10]["trees"] == {0: 0.25, 1: 0.75}
    assert roles[10]["shrubs"] == {2: 1.0}
    assert roles[20] == {"trees": {}, "shrubs": {2: 1.0}}


def test_mix_roles_default_without_a_package_mix():
    roles = vc.mix_roles(None, LIB)
    assert roles[10] == {"trees": {0: 1.0}, "shrubs": {}}
    assert roles[20]["shrubs"] == {2: 1.0} and roles[30]["shrubs"] == {2: 1.0}


def test_chi2_p_value():
    assert vc.chi2_p(0.0, 1) == pytest.approx(1.0)
    assert vc.chi2_p(10.83, 1) == pytest.approx(0.001, rel=0.25)
    assert vc.chi2_p(3.84, 1) == pytest.approx(0.05, rel=0.2)
    assert vc.chi2_p(100.0, 3) < 1e-6


def _cell(instances=(), shrubs=(), h=1, sh=2):
    return {"hash": h, "shrub_hash": sh, "instances": [list(i) for i in instances], "shrubs": [list(s) for s in shrubs]}


def test_placement_gate():
    code_at = lambda lon, lat: [10 if la < 33.31 else 20 for la in lat]
    cells = {(1, 1): _cell([(33.30, -117.3, 100, 0, 1, 0)], [(33.32, -117.3, 100, 0, 1, 2)])}
    assert vc.placement_gate(cells, vc.mix_roles(MIX, LIB), code_at)["pass"]
    bad = {(1, 1): _cell([(33.32, -117.3, 100, 0, 1, 0)])}               # a tree on 20 (shrubs only)
    r = vc.placement_gate(bad, vc.mix_roles(MIX, LIB), code_at)
    assert not r["pass"] and r["trees_off_mix"] == 1


def test_placement_gate_with_canopy():
    code_at = lambda lon, lat: [10] * len(lat)
    canopy_at = lambda lon, lat: [(0.0, 0.0, True) if la > 33.31 else (0.5, 12.0, True) for la in lat]
    ok = {(1, 1): _cell([(33.30, -117.3, 100, 0, 1, 0)])}
    assert vc.placement_gate(ok, vc.mix_roles(MIX, LIB), code_at, canopy_at)["pass"]
    bare = {(1, 1): _cell([(33.32, -117.3, 100, 0, 1, 0)])}
    r = vc.placement_gate(bare, vc.mix_roles(MIX, LIB), code_at, canopy_at)
    assert not r["pass"] and r["trees_without_canopy"] == 1


def test_species_gate():
    code_at = lambda lon, lat: [10] * len(lat)
    roles = vc.mix_roles(MIX, LIB)
    good = {(0, k): _cell([(33.3, -117.3, 0, 0, 1, 1)] * 3 + [(33.3, -117.3, 0, 0, 1, 0)]) for k in range(100)}
    assert vc.species_gate(good, roles, code_at)["pass"]
    bad = {(0, k): _cell([(33.3, -117.3, 0, 0, 1, 0)] * 3 + [(33.3, -117.3, 0, 0, 1, 1)]) for k in range(100)}
    r = vc.species_gate(bad, roles, code_at)
    assert not r["pass"] and r["codes"]["10"]["trees"]["p"] < 0.001


def test_cell_bounds_match_the_cpp_grid():
    s, n, w, e = vc.cell_bounds(*vc.cell_of(33.3, -117.4))
    assert s <= 33.3 < n and w <= -117.4 < e
    assert (n - s) * 111_320.0 == pytest.approx(100.0)
    assert (e - w) * 111_320.0 * math.cos(math.radians(0.5 * (s + n))) == pytest.approx(100.0, rel=1e-6)


def test_canopy_gate():
    row, col = vc.cell_of(33.3, -117.4)
    s, n, w, e = vc.cell_bounds(row, col)
    area = vc.cell_area_m2(row, col)
    crowns = {0: 2.7}
    # crown area pi 2.7^2 = 22.9 m^2; 131 trees ~ 0.30 of a hectare-sized cell
    trees = [((s + n) / 2, (w + e) / 2, 0, 0, 1.0, 0)] * round(0.30 * area / (math.pi * 2.7 ** 2))
    cells = {(row, col + k): _cell(trees) for k in range(60)}
    cover = lambda r, c: 0.30
    g = vc.canopy_gate(cells, cover, crowns)
    assert g["pass"] and g["cells"] == 60 and g["median_abs_err"] < 0.01
    assert not vc.canopy_gate(cells, lambda r, c: 0.45, crowns)["pass"]  # 15 pp off
    few = dict(list(cells.items())[:10])
    assert not vc.canopy_gate(few, cover, crowns)["pass"]                # < 50 cells in range


def test_far_determinism_gate():
    a = {(1, 1): {"hash": 5, "instances": [[33.3, -117.3, 0, 0, 1.3, 0, 2]]}}
    b = {(1, 1): {"hash": 5, "instances": [[33.3, -117.3, 0, 0, 1.3, 0, 2]]}}
    near = {vc.cell_of(33.3, -117.3): _cell([(33.3, -117.3, 0, 0, 1.0, 0)])}
    g = vc.far_determinism_gate(a, b, near, min_common=1)
    assert g["pass"] and g["far_trees_checked"] == 2 and g["not_in_near"] == 0   # one far tree per run
    wrong_type = {k: {"hash": 5, "instances": [[33.3, -117.3, 0, 0, 1.3, 1, 2]]} for k in a}
    assert vc.far_determinism_gate(wrong_type, wrong_type, near, min_common=1)["not_in_near"] == 2
    b[(1, 1)]["hash"] = 6
    assert not vc.far_determinism_gate(a, b, near, min_common=1)["pass"]
    lone = {(1, 1): {"hash": 5, "instances": [[33.4, -117.3, 0, 0, 1.3, 0, 2]]}}
    assert vc.far_determinism_gate(lone, lone, near, min_common=1)["not_in_near"] == 0  # its cell was never near: unchecked


def _ev(tick, ev, tier, br=1, bc=1, row=None):
    e = {"tick": tick, "ev": ev, "tier": tier, "block_row": br, "block_col": bc}
    if row is not None:
        e |= {"row": row, "col": 0}
    return e


def test_handoff_gate():
    ok = [_ev(1, "tier", "near"), _ev(2, "spawn", "near", row=4), _ev(10, "tier", "far"), _ev(14, "spawn", "far"),
          _ev(15, "destroy", "near", row=4)]
    assert vc.handoff_gate(ok)["pass"]
    gap = [_ev(1, "tier", "near"), _ev(2, "spawn", "near", row=4), _ev(10, "tier", "far"), _ev(11, "destroy", "near", row=4),
           _ev(14, "spawn", "far")]
    r = vc.handoff_gate(gap)
    assert not r["pass"] and r["gaps"] == 1
    gone = [_ev(1, "tier", "near"), _ev(2, "spawn", "near", row=4), _ev(10, "tier", "none"), _ev(11, "destroy", "near", row=4)]
    assert vc.handoff_gate(gone)["pass"]
    empty_far = [_ev(1, "tier", "near"), _ev(2, "spawn", "near", row=4), _ev(10, "tier", "far"), _ev(12, "empty", "far"),
                 _ev(13, "destroy", "near", row=4)]
    assert vc.handoff_gate(empty_far)["pass"]
    back = [_ev(1, "tier", "far"), _ev(2, "spawn", "far"), _ev(10, "tier", "near"), _ev(11, "destroy", "far")]
    assert not vc.handoff_gate(back)["pass"]                              # far dropped before any near settle


def test_r4_identity_gate():
    r4 = {(k, 0): {"hash": k} for k in range(30)}
    now = {(k, 0): {"hash": k} for k in range(25)}
    assert vc.r4_identity_gate(now, r4)["pass"]
    now[(3, 0)] = {"hash": 99}
    g = vc.r4_identity_gate(now, r4)
    assert not g["pass"] and g["mismatched"] == 1
    assert not vc.r4_identity_gate({}, r4)["pass"]
