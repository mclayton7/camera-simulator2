"""Seeded sampler: deterministic per (seed, index), independent of other indices, values in range."""

from __future__ import annotations

import math

from camsim_control.randomize import EntitySpec, offset_ll, sample

SITE = (33.3072917, -117.344625)
TYPES = [EntitySpec(2001, "ground"), EntitySpec(3001, "water")]


def test_deterministic_and_independent():
    a = sample(7, 3, SITE, 20.0, TYPES)
    sample(7, 2, SITE, 20.0, TYPES)
    assert sample(7, 3, SITE, 20.0, TYPES) == a
    assert sample(8, 3, SITE, 20.0, TYPES) != a


def test_ranges():
    for i in range(50):
        s = sample(1, i, SITE, 20.0, TYPES)
        cam = s.camera
        assert 5.0 <= cam["hfov_deg"] <= 40.0
        assert -90.0 <= cam["gimbal"]["pitch"] <= -20.0
        assert cam["alt"] > 20.0
        assert 1 <= len(s.entities) <= 4
        for e in s.entities:
            assert e["type_id"] in (2001, 3001)
            assert e["surface"] == ("ground" if e["type_id"] == 2001 else "water")
        env = s.environment
        assert 5000 <= env["atmosphere"]["visibility_m"] <= 50000
        assert 0 <= env["clouds"]["coverage_pct"] <= 60
        assert 0 <= env["sea"]["beaufort"] <= 4
        assert s.utc.startswith("2026-06-15T")


def test_camera_looks_at_aim_point():
    s = sample(3, 0, SITE, 20.0, TYPES)
    cam, aim = s.camera, s.aim
    dn = (aim[0] - cam["lat"]) * math.pi / 180 * 6378137.0
    de = (aim[1] - cam["lon"]) * math.pi / 180 * 6378137.0 * math.cos(math.radians(cam["lat"]))
    bearing = math.degrees(math.atan2(de, dn)) % 360
    assert abs((bearing - cam["heading"] + 180) % 360 - 180) < 0.5
    horiz = math.hypot(dn, de)
    depression = math.degrees(math.atan2(cam["alt"] - 20.0, horiz))
    assert abs(depression + cam["gimbal"]["pitch"]) < 0.5


def test_offset_ll_round_trip():
    lat, lon = offset_ll(33.0, -117.0, 1000.0, -500.0)
    assert abs((lat - 33.0) * math.pi / 180 * 6378137.0 - 1000.0) < 1e-6
    assert abs((lon + 117.0) * math.pi / 180 * 6378137.0 * math.cos(math.radians(33.0)) + 500.0) < 1e-6
