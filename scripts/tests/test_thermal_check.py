"""Unit tests for the pure helpers of scripts/thermal_check.py (ROADMAP 4A acceptance)."""

import json
import math

import numpy as np
import thermal_check as tc


def test_ring_mask_is_dilated_box_minus_box():
    shape = (40, 60)
    box = [20.0, 10.0, 10.0, 6.0]  # x 20..30, y 10..16
    ring = tc.ring_mask(shape, box)
    inner = tc.box_mask(shape, box)
    outer = tc.box_mask(shape, [15.0, 7.0, 20.0, 12.0])  # 2x about the centre (25, 13)
    assert not (ring & inner).any()
    assert np.array_equal(ring, outer & ~inner)
    assert ring.sum() == 20 * 12 - 10 * 6


def test_ring_mask_clips_at_the_image_edge():
    ring = tc.ring_mask((20, 20), [0.0, 0.0, 6.0, 6.0])
    assert ring.sum() == 9 * 9 - 6 * 6  # outer box -3..9 clipped to 0..9


def test_shadow_offset_at_noon_elevation_points_north():
    n, e = tc.shadow_offset_ne(3.275, 28.8, 180.0)
    assert math.isclose(n, 3.275 / math.tan(math.radians(28.8)), rel_tol=1e-9)
    assert math.isclose(n, 5.96, abs_tol=0.01)
    assert abs(e) < 1e-9
    n, e = tc.shadow_offset_ne(1.0, 45.0, 90.0)  # sun in the east -> shadow west
    assert abs(n) < 1e-9 and math.isclose(e, -1.0)


def test_sun_at_solar_noon_on_21_december_in_san_francisco():
    elev, az = tc.sun_for(tc.NOON)
    assert math.isclose(elev, 28.8, abs_tol=0.1)
    assert math.isclose(az, 180.0, abs_tol=2.0)
    elev, _ = tc.sun_for(tc.NIGHT)
    assert elev < -30.0


def _box3d(yaw_deg: float, px_per_m: float = 10.0) -> dict:
    """A nadir-viewed 8 m x 3 m x 3.275 m box at the origin, image +x = east, +y = south."""
    y = math.radians(yaw_deg)
    fwd = (
        np.array([math.sin(y), -math.cos(y)]) * px_per_m
    )  # (x east, y south) of 1 m forward
    right = np.array([math.cos(y), math.sin(y)]) * px_per_m
    c0 = np.array([100.0, 100.0]) - 4.0 * fwd - 1.5 * right
    corners = [c0, c0 + 3.0 * right, c0 + 3.0 * right + 8.0 * fwd, c0 + 8.0 * fwd]
    return {
        "size_m": [8.0, 3.0, 3.275],
        "yaw_deg": yaw_deg,
        "corners_px": [list(map(float, c)) for c in corners] * 2,
    }


def test_ground_basis_recovers_north_and_east_for_any_heading():
    for yaw in (0.0, 37.0, 90.0, 200.0):
        north, east = tc.ground_basis_px(_box3d(yaw))
        assert np.allclose(north, [0.0, -10.0], atol=1e-9), yaw
        assert np.allclose(east, [10.0, 0.0], atol=1e-9), yaw


def test_shadow_masks_put_the_shadow_north_of_the_box_at_noon():
    shape = (200, 200)
    b3 = _box3d(90.0)  # heading east: footprint x 60..140, y 85..115 at 10 px/m
    ann = {"bbox": [60.0, 85.0, 80.0, 30.0], "box3d": b3}
    shadow, sunlit, shift = tc.shadow_masks(shape, ann, 28.8, 180.0)
    assert math.isclose(shift[1], -59.6, abs_tol=0.2) and abs(shift[0]) < 1e-6
    ys, xs = np.nonzero(shadow)
    assert (
        ys.max() < 85 and ys.min() >= 85 - 60 and 59 <= xs.min() and xs.max() <= 141
    )  # rasteriser rounding
    assert not (shadow & sunlit).any()
    assert not (sunlit & tc.box_mask(shape, ann["bbox"])).any()


def test_statistics_on_a_synthetic_y_image():
    y = np.full((100, 100), 100.0, np.float32)
    y[:10, :] = 16.0  # 10 % black
    y[40:60, 40:60] = 200.0
    box = [40.0, 40.0, 20.0, 20.0]
    assert math.isclose(tc.black_fraction(y), 0.10)
    assert tc.region_mean(y, tc.box_mask(y.shape, box)) == 200.0
    assert tc.region_mean(y, tc.ring_mask(y.shape, box)) == 100.0
    assert math.isclose(tc.percentile([1.0, 2.0, 3.0, 4.0, 5.0], 95), 4.8)
    assert math.isnan(tc.percentile([], 95))


def test_y_from_rgb_inverts_the_grey_snapshot():
    yv = np.arange(16, 236, dtype=np.float32)
    g = np.clip(np.round((yv - 16.0) / 219.0 * 255.0), 0, 255)
    rgb = np.stack([g, g, g], axis=-1)
    assert np.array_equal(np.round(tc.y_from_rgb(rgb)), yv)


def test_coast_masks_are_radius_matched_and_disjoint():
    water, land = tc.coast_masks((720, 1280))
    assert water.sum() > 5000 and land.sum() > 5000
    assert not (water & land).any()
    yy, xx = np.mgrid[0:720, 0:1280]
    r = np.hypot(xx - 639.5, yy - 359.5)
    for m in (water, land):
        assert r[m].min() >= 0.26 * 720 - 1e-6 and r[m].max() <= 0.40 * 720 + 1e-6


def test_report_json_shape_and_gate():
    checks = [
        {
            "check": g,
            "band": "mwir",
            "time": "night",
            "value": 1.0,
            "threshold": "t",
            "pass": True,
            "detail": "",
        }
        for g in "abcdefg"
    ]
    rep = tc.build_report(
        {"git": "abc"}, checks, [{"check": "boat", "value": 2.0}], ["/x.png"]
    )
    assert set(rep) == {"meta", "checks", "info", "shots", "passed"}
    assert rep["passed"] is True
    json.dumps(rep)  # serialisable
    assert "| a | mwir | night |" in tc.render_markdown(rep)
    checks[3]["pass"] = False
    assert tc.build_report({}, checks, [], [])["passed"] is False
    assert tc.build_report({}, checks[:6], [], [])["passed"] is False  # gate g missing
