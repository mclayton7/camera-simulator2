"""Unit tests for the pure helpers of scripts/thermal_check.py (ROADMAP 4A acceptance)."""

import json
import math

import numpy as np
import pytest
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


ALL_RUNS = {"bands", "hd", "eo", "lcoff", "pan"}


def _rows(bands=("mwir", "lwir"), runs=ALL_RUNS) -> list[dict]:
    return [
        {
            "check": c,
            "band": b,
            "time": t,
            "value": 1.0,
            "threshold": "t",
            "pass": True,
            "detail": "",
        }
        for c, b, t in tc.expected_rows(list(bands), set(runs))
    ]


def test_expected_rows_cover_every_band_time_and_selected_run():
    rows = tc.expected_rows(["mwir", "lwir"], ALL_RUNS)
    assert len(rows) == 2 * 11 + 3
    assert ("e", "lwir", "noon") in rows and ("e", "lwir", "night") in rows
    assert not any(r[0] == "m" for r in rows)  # (m) is info only (Task 13 fix round 1)
    for b in ("mwir", "lwir"):  # (h) static coast shimmer, both times; 4B land-cover gates
        assert ("h", b, "night") in rows and ("h", b, "noon") in rows
        assert ("i", b, "noon") in rows and ("j", b, "night") in rows and ("k", b, "night") in rows
    assert ("f", "mwir", "noon") in rows and ("g", "eo", "noon") in rows and ("l", "mwir", "noon") in rows
    assert rows[-1] == ("g", "eo", "noon")
    only_bands = tc.expected_rows(["mwir"], {"bands"})
    assert {r[0] for r in only_bands} == set("abcdehij")  # f, g, k, l not expected
    assert tc.expected_rows(["mwir"], {"eo"}) == [("g", "eo", "noon")]
    assert tc.expected_rows(["mwir"], {"lcoff"}) == []  # (k) needs the land-cover-on band run too
    assert tc.expected_rows(["mwir"], {"pan"}) == [("l", "mwir", "noon")]


def test_complete_rows_pass():
    checks = _rows()
    exp = tc.expected_rows(["mwir", "lwir"], ALL_RUNS)
    assert tc.gate_passed(checks, exp) is True
    assert tc.missing_rows(checks, exp) == []


def test_a_missing_row_fails():
    exp = tc.expected_rows(["mwir", "lwir"], ALL_RUNS)
    for drop in range(len(exp)):
        checks = _rows()
        del checks[drop]
        assert tc.gate_passed(checks, exp) is False, exp[drop]
        assert tc.missing_rows(checks, exp) == [exp[drop]]
    # A whole band's run absent: its rows are missing even though every letter is present.
    checks = _rows(bands=("mwir",))
    assert {c["check"] for c in checks} == set("abcdefghijkl")
    assert tc.gate_passed(checks, exp) is False
    assert tc.gate_passed([], []) is False  # nothing expected -> not a pass


def test_report_json_shape_and_gate():
    exp = tc.expected_rows(["mwir", "lwir"], ALL_RUNS)
    checks = _rows()
    info = [
        {"check": "boat", "value": 2.0},
        {"check": "shimmer(coast) EO baseline", "band": "eo", "time": "noon", "value": 1.0},
    ]
    rep = tc.build_report({"git": "abc"}, checks, info, ["/x.png"], exp)
    assert set(rep) == {
        "meta",
        "checks",
        "missing",
        "info",
        "shots",
        "passed",
    }
    assert rep["passed"] is True and rep["missing"] == []
    json.dumps(rep)  # serialisable
    md = tc.render_markdown(rep)
    assert "| a | mwir | night |" in md and "| h | lwir | noon |" in md
    checks[3]["pass"] = False
    assert tc.build_report({}, checks, [], [], exp)["passed"] is False
    rep = tc.build_report({}, _rows()[:-1], [], [], exp)  # g missing
    assert rep["passed"] is False and rep["missing"] == [["g", "eo", "noon"]]
    assert "| g | eo | noon | - | - | FAIL | missing" in tc.render_markdown(rep)


def test_shimmer_gate_row():
    sh = {"value": 2.5, "detail": "d"}
    row = tc.shimmer_row("mwir", "noon", sh)
    assert row["check"] == "h" and row["band"] == "mwir" and row["time"] == "noon"
    assert row["pass"] is False and row["threshold"] == "<= 2"
    assert tc.shimmer_row("lwir", "night", {"value": 2.0, "detail": ""})["pass"] is True
    assert tc.shimmer_row("lwir", "night", {"value": float("nan"), "detail": ""})["pass"] is False


def test_coast_shimmer_ratio_on_synthetic_frames():
    rng = np.random.default_rng(1)
    h, w = 120, 160
    base = np.full((h, w), 100.0, np.float32)
    base[int(0.6 * h) :, :] = 140.0  # a horizontal "coastline" in the edge ROI
    frames = base + rng.normal(0, 1.0, (30, h, w)).astype(np.float32)
    vd = tc.ViewData("r", "noon", "coast", frames, [None] * 30, {})
    calm = tc.coast_shimmer(vd)["value"]
    assert 0.5 < calm < 1.5  # noise only: edge std ~ interior std
    flick = frames.copy()
    edge_row = int(0.6 * h)
    flick[::2, edge_row - 1 : edge_row + 1, :] = 140.0  # the edge jumps a row every other frame
    assert tc.coast_shimmer(tc.ViewData("r", "noon", "coast", flick, [None] * 30, {}))["value"] > 2.0


def test_shimmer_floor_is_8bit_quantization_noise():
    assert tc.SNAPSHOT_QUANT_STD_DN == pytest.approx(1.0 / math.sqrt(12.0))
    assert tc.SNAPSHOT_QUANT_STD_DN == pytest.approx(0.2887, abs=1e-4)


def test_shimmer_ratio_floors_an_interior_below_quantization():
    # Task 17 t17c MWIR noon: edge 0.41 over interior 0.19 DN (below 8-bit rounding).
    raw, floored = tc.shimmer_ratio(0.41, 0.19)
    assert raw == pytest.approx(0.41 / 0.19)
    assert floored == pytest.approx(0.41 / tc.SNAPSHOT_QUANT_STD_DN)  # ~1.42
    assert floored <= tc.SHIMMER_RATIO < raw
    row = tc.shimmer_row("mwir", "noon", {"value": floored, "raw": raw, "detail": ""})
    assert row["pass"] is True and row["raw"] == pytest.approx(raw)


def test_shimmer_ratio_still_fails_real_flicker_above_the_floor():
    raw, floored = tc.shimmer_ratio(2.2 * 0.5, 0.5)  # interior above the floor: no change
    assert raw == floored == pytest.approx(2.2)
    assert tc.shimmer_row("mwir", "noon", {"value": floored, "raw": raw, "detail": ""})["pass"] is False
    raw, floored = tc.shimmer_ratio(0.7, 0.1)  # 2.4x the floor: still fails
    assert floored == pytest.approx(0.7 / tc.SNAPSHOT_QUANT_STD_DN) and floored > tc.SHIMMER_RATIO


def test_coast_shimmer_reports_raw_and_floored():
    rng = np.random.default_rng(2)
    h, w = 120, 160
    base = np.full((h, w), 100.0, np.float32)
    base[int(0.6 * h) :, :] = 140.0
    frames = np.round(base + rng.normal(0, 0.1, (30, h, w))).astype(np.float32)  # sub-quantization noise
    sh = tc.coast_shimmer(tc.ViewData("r", "noon", "coast", frames, [None] * 30, {}))
    assert sh["value"] <= sh["raw"] + 1e-9
    assert "floored" in sh["detail"] and "raw" in sh["detail"]


# ---- Gate (m): land-cover grid visibility (ROADMAP 4B Task 13) ----


def test_grid_period_px_is_texel_over_gsd():
    gsd = 2.0 * 800.0 * math.tan(math.radians(20.0)) / 1280.0
    assert tc.grid_period_px(800.0, 40.0, 1280) == pytest.approx(10.0 / gsd)
    assert tc.grid_period_px(800.0, 40.0, 1280) == pytest.approx(21.98, abs=0.01)


def test_central_mask_keeps_the_middle_60_percent():
    m = tc.central_mask((100, 200))
    assert m.sum() == 60 * 120
    assert m[50, 100] and not m[10, 100] and not m[50, 30]


def _grid(h: int, w: int, period: int, amp: float = 10.0) -> np.ndarray:
    """A 10-px checker grid: square waves along both image axes (every row and column has an edge every period / 2 px)."""
    yy, xx = np.mgrid[0:h, 0:w]
    def sq(v):
        return np.where((v // (period // 2)) % 2 == 0, 1.0, -1.0)

    return 100.0 + amp * 0.5 * (sq(xx) + sq(yy))


def test_grid_peak_ratio_trips_on_a_grid_at_the_window_period():
    rng = np.random.default_rng(1)
    y = _grid(400, 600, 10) + rng.normal(0.0, 3.0, (400, 600))
    mask = tc.central_mask(y.shape)
    rx, ry = tc.grid_peak_ratio(y, mask, 10.0)
    assert rx > 2.0 * tc.GRID_PEAK_RATIO and ry > 2.0 * tc.GRID_PEAK_RATIO
    # a weak grid buried in noise still shows (AGC-stretched night frames are low contrast)
    weak = _grid(400, 600, 10, amp=1.0) + rng.normal(0.0, 3.0, (400, 600))
    assert max(tc.grid_peak_ratio(weak, mask, 10.0)) > tc.GRID_PEAK_RATIO


def test_grid_peak_ratio_passes_noise_ramps_and_other_periods():
    rng = np.random.default_rng(2)
    mask = tc.central_mask((400, 600))
    noise = 100.0 + rng.normal(0.0, 5.0, (400, 600))
    assert max(tc.grid_peak_ratio(noise, mask, 10.0)) <= tc.GRID_PEAK_RATIO
    xx = np.mgrid[0:400, 0:600][1]
    ramp = noise + 0.2 * xx  # vignetting-like gradient
    assert max(tc.grid_peak_ratio(ramp, mask, 22.0)) <= tc.GRID_PEAK_RATIO
    # a 10-px grid is not a 22-px window grid (the band sits between its harmonics)
    assert max(tc.grid_peak_ratio(_grid(400, 600, 10) + rng.normal(0, 3.0, (400, 600)), mask, 26.0)) <= tc.GRID_PEAK_RATIO


def test_grid_peak_ratio_ignores_masked_pixels():
    rng = np.random.default_rng(3)
    y = 100.0 + rng.normal(0.0, 5.0, (400, 600))
    y[150:250, 200:400] = _grid(100, 200, 10, amp=60.0)  # an entity-like patch with a strong grid
    mask = tc.central_mask(y.shape)
    assert max(tc.grid_peak_ratio(y, mask, 10.0)) > tc.GRID_PEAK_RATIO
    mask[140:260, 190:410] = False  # masked out (entity boxes): filled with the masked mean
    assert max(tc.grid_peak_ratio(y, mask, 10.0)) <= tc.GRID_PEAK_RATIO
    assert all(math.isnan(r) for r in tc.grid_peak_ratio(y, np.zeros_like(mask), 10.0))


# ---- Gates (i)-(l): land cover (ROADMAP 4B Task 11) ----


def test_run_groups_and_bands():
    assert tc.run_group("mwir") == "bands" and tc.run_band("mwir") == "mwir"
    assert tc.run_group("lwir_lcoff") == "lcoff" and tc.run_band("lwir_lcoff") == "lwir"
    assert tc.run_group("mwir_pan") == "pan" and tc.run_band("mwir_pan") is None
    assert tc.run_group("mwir_1080p") == "hd" and tc.run_group("eo_thermal_off") == "eo"


def test_exg_and_greenness_masks():
    rgb = np.full((100, 200, 3), 128, np.uint8)
    rgb[:, :100] = (60, 140, 50)  # vegetation on the left
    rgb[40:60, 140:160] = (40, 60, 160)  # water-blue patch on the right
    rgb[70:80, 120:180] = (10, 10, 10)  # deep shadow
    assert tc.exg(rgb)[50, 50] > 0.3 and abs(tc.exg(rgb)[30, 150]) < 1e-6  # neutral grey
    veg, non = tc.greenness_masks(rgb)
    assert not (veg & non).any()
    assert veg[50, 60] and not veg[50, 150]
    assert non[30, 130] and not non[50, 150] and not non[75, 150]  # blue and shadow excluded
    assert not veg[5, 60] and not non[5, 130]  # outside the central 60 %
    assert not veg[50, 99] and not non[50, 101]  # eroded at the boundary


def test_veg_contrast_sign_and_small_regions():
    y = np.full((50, 50), 100.0, np.float32)
    veg = np.zeros((50, 50), bool)
    non = np.zeros((50, 50), bool)
    veg[:, :25], non[:, 25:] = True, True
    y[:, :25] = 90.0
    assert tc.veg_contrast(y, veg, non) == pytest.approx(10.0)
    tiny = np.zeros((50, 50), bool)
    tiny[0, 0] = True
    assert math.isnan(tc.veg_contrast(y, tiny, non))


def test_box_blur_and_highpass_std():
    flat = np.full((80, 80), 50.0, np.float32)
    assert np.allclose(tc.box_blur(flat, 33), 50.0)
    mask = tc.central_mask((80, 80))
    assert tc.highpass_std(flat, mask) == pytest.approx(0.0, abs=1e-5)
    yy, xx = np.mgrid[0:80, 0:80]
    checker = flat + 10.0 * np.where(((yy // 4) + (xx // 4)) % 2 == 0, 1.0, -1.0)
    assert tc.highpass_std(checker, mask) == pytest.approx(10.0, rel=0.15)
    ramp = flat + 0.5 * xx  # a smooth gradient (vignetting-like) is not structure
    assert tc.highpass_std(ramp.astype(np.float32), mask) < 0.5


def test_pan_offset_is_a_triangle_at_the_pan_speed():
    period = 2.0 * tc.PAN_LEG_M / tc.PAN_SPEED_MPS
    assert tc.pan_offset_m(0.0) == pytest.approx(0.0)
    assert tc.pan_offset_m(period / 4) == pytest.approx(tc.PAN_LEG_M / 2)
    assert tc.pan_offset_m(period / 2) == pytest.approx(tc.PAN_LEG_M)
    assert tc.pan_offset_m(period) == pytest.approx(0.0, abs=1e-6)
    assert (tc.pan_offset_m(10.0) - tc.pan_offset_m(9.0)) == pytest.approx(tc.PAN_SPEED_MPS)


def test_window_events_and_recentre_spike():
    ids = [0, 0, 1, 1, 1, 2, 2, 3]
    rows = [{"land_cover_window": w, "wall_ms": 33.3} for w in ids]
    rows[5]["wall_ms"] = 45.3
    ev = tc.window_events(rows)
    assert ev == [5, 7]  # the first window is not a re-centre
    spike, base = tc.recentre_spike(rows, ev, radius=1)
    assert base == pytest.approx(33.3) and spike == pytest.approx(12.0)
    assert math.isnan(tc.recentre_spike(rows, [], radius=1)[0])
    assert tc.window_events([{"land_cover_window": 1}, {"land_cover_window": 0}, {"land_cover_window": 1}]) == []


def test_median3_removes_isolated_defects():
    y = np.full((20, 20), 100.0, np.float32)
    y[5, 5], y[10, 12] = 16.0, 235.0
    assert np.allclose(tc.median3(y), 100.0)
    edge = np.zeros((20, 20), np.float32)
    edge[:, 10:] = 50.0  # a step edge survives
    assert np.array_equal(tc.median3(edge), edge)
