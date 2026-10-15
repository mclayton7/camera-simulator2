"""Unit tests for scripts/georeg_lib.py (REALISM R6 geo-registration geometry)."""

import math

import georeg_lib as g
import numpy as np
import pytest
from pycocotools import mask as cocomask

W, H = 1280, 720
LAT, LON, GROUND = 33.30, -117.45, 50.0


def tags(lat=LAT, lon=LON, hae=GROUND + 300.0, heading=0.0, pitch=0.0, roll=0.0, az=0.0, el=-90.0, sroll=0.0,
         hfov=20.0, vfov=None):
    if vfov is None:
        vfov = math.degrees(2 * math.atan(math.tan(math.radians(hfov / 2)) * H / W))
    return {13: lat, 14: lon, 75: hae, 5: heading, 6: pitch, 7: roll, 18: az, 19: el, 20: sroll, 16: hfov, 17: vfov}


def test_nadir_projects_the_point_below_to_the_image_centre():
    cam = g.Camera.from_klv(tags(), W, H)
    x, y = cam.project(LAT, LON, GROUND)
    assert x == pytest.approx(W / 2, abs=1e-6) and y == pytest.approx(H / 2, abs=1e-6)


def test_nadir_heading_zero_puts_north_up_and_east_right():
    cam = g.Camera.from_klv(tags(), W, H)
    n = g.offset_m(LAT, LON, GROUND, east=0.0, north=10.0)
    e = g.offset_m(LAT, LON, GROUND, east=10.0, north=0.0)
    xn, yn = cam.project(*n)
    xe, ye = cam.project(*e)
    f = (W / 2) / math.tan(math.radians(10.0))
    assert xn == pytest.approx(W / 2, abs=1e-3) and yn == pytest.approx(H / 2 - f * 10 / 300, rel=1e-4)
    assert ye == pytest.approx(H / 2, abs=1e-3) and xe == pytest.approx(W / 2 + f * 10 / 300, rel=1e-4)


def test_oblique_heading_90_depression_45_hand_worked():
    # Camera 100 m up, looking east and 45 deg down: the ground point 100 m east is the boresight; a point 10 m north
    # of it is to the camera's left (north is left when facing east): x < W/2 by f * 10 / (100 sqrt 2).
    t = tags(hae=GROUND + 100.0, heading=90.0, el=-45.0)
    cam = g.Camera.from_klv(t, W, H)
    aim = g.offset_m(LAT, LON, GROUND, east=100.0, north=0.0)
    x, y = cam.project(*aim)
    assert x == pytest.approx(W / 2, abs=0.05) and y == pytest.approx(H / 2, abs=0.05)
    left = g.offset_m(LAT, LON, GROUND, east=100.0, north=10.0)
    xl, yl = cam.project(*left)
    f = (W / 2) / math.tan(math.radians(10.0))
    assert xl == pytest.approx(W / 2 - f * 10 / (100 * math.sqrt(2)), abs=0.05)
    assert yl == pytest.approx(H / 2, abs=0.05)


def test_platform_heading_and_sensor_azimuth_compose():
    # Platform heading 30 + sensor azimuth 60 looks where heading 90 + azimuth 0 does.
    a = g.Camera.from_klv(tags(hae=GROUND + 100, heading=30.0, az=60.0, el=-45.0), W, H)
    b = g.Camera.from_klv(tags(hae=GROUND + 100, heading=90.0, az=0.0, el=-45.0), W, H)
    p = g.offset_m(LAT, LON, GROUND, east=90.0, north=20.0)
    assert a.project(*p) == pytest.approx(b.project(*p), abs=1e-6)


def test_unproject_round_trip():
    cam = g.Camera.from_klv(tags(hae=GROUND + 500, heading=250.0, el=-30.0), W, H)
    p = g.offset_m(LAT, LON, GROUND, east=-700.0, north=-300.0)
    x, y = cam.project(*p)
    lat, lon = cam.unproject(x, y, GROUND)
    assert lat == pytest.approx(p[0], abs=1e-8) and lon == pytest.approx(p[1], abs=1e-8)


def test_offset_m_is_metric():
    lat, lon, h = g.offset_m(LAT, LON, GROUND, east=1000.0, north=-2000.0)
    e, n, u = g.enu(LAT, LON, GROUND, lat, lon, h)
    assert (e, n) == pytest.approx((1000.0, -2000.0), abs=1e-3)


def test_distortion_zero_is_pinhole_and_negative_k1_pulls_in():
    cam = g.Camera.from_klv(tags(), W, H)
    p = g.offset_m(LAT, LON, GROUND, east=40.0, north=-20.0)
    assert cam.project_distorted(*p, 0.0, 0.0) == pytest.approx(cam.project(*p), abs=1e-9)
    x0, y0 = cam.project(*p)
    xd, yd = cam.project_distorted(*p, -0.05, 0.0)
    assert abs(xd - W / 2) < abs(x0 - W / 2) and abs(yd - H / 2) < abs(y0 - H / 2)


def test_corner_homography_is_exact_on_a_plane():
    t = tags(hae=GROUND + 300, heading=40.0, el=-60.0)
    cam = g.Camera.from_klv(t, W, H)
    for k, (px, py) in zip((82, 84, 86, 88), ((0, 0), (W, 0), (W, H), (0, H))):
        t[k], t[k + 1] = cam.unproject(px, py, GROUND)
    t[23], t[24] = cam.unproject(W / 2, H / 2, GROUND)
    hom = g.corner_homography(t, W, H)
    p = g.offset_m(t[23], t[24], GROUND, east=30.0, north=12.0)
    assert hom(p[0], p[1]) == pytest.approx(cam.project(*p), abs=0.02)


def test_mask_centroid_uses_pixel_edge_coordinates():
    m = np.zeros((H, W), np.uint8)
    m[20:22, 10:12] = 1  # rows 20-21, columns 10-11
    r = cocomask.encode(np.asfortranarray(m))
    rle = {"size": [H, W], "counts": r["counts"].decode()}
    assert g.mask_centroid(rle) == pytest.approx((11.0, 21.0))
    assert g.mask_touches_edge(rle) is False


def test_quarter_plate_projection_centroid():
    cam = g.Camera.from_klv(tags(), W, H)
    corners = g.plate_corners(LAT, LON, GROUND, half_m=1.0, top_m=0.05, heading=0.0, normal_az=0.0, normal_el=90.0)
    x, y = g.projected_centroid(cam.project, corners)
    assert (x, y) == pytest.approx((W / 2, H / 2), abs=1e-6)


def test_quantisation_budget_heading_matches_the_analytic_shift():
    t = tags(hae=GROUND + 100, heading=90.0, el=-45.0)
    aim = g.offset_m(LAT, LON, GROUND, east=100.0, north=0.0)
    b = g.quantisation_budget(t, W, H, aim)
    f = (W / 2) / math.tan(math.radians(10.0))
    # One LSB of heading turns the camera about the local vertical: at 45 deg depression the image centre moves by
    # f * LSB * cos(45 deg) px.
    assert b[5] == pytest.approx(f * math.radians(g.LSB[5]) * math.cos(math.radians(45)), rel=0.05)
    assert set(b) >= {13, 14, 75, 5, 6, 7, 18, 19, 20, 16, 17}


def test_plate_on_a_slope_lies_in_the_tilted_plane():
    az, el = 90.0, 75.0  # normal leans east: the ground falls away to the east
    n = np.array([math.cos(math.radians(el)) * math.sin(math.radians(az)),
                  math.cos(math.radians(el)) * math.cos(math.radians(az)), math.sin(math.radians(el))])  # ENU
    for c in g.plate_corners(LAT, LON, GROUND, half_m=1.0, top_m=0.05, heading=30.0, normal_az=az, normal_el=el):
        v = np.array(g.enu(LAT, LON, GROUND, *c))
        assert v @ n == pytest.approx(0.05, abs=1e-4)
        assert np.linalg.norm(v - (v @ n) * n) == pytest.approx(math.sqrt(2), abs=1e-4)


def test_nadir_ground_pixel_size():
    cam = g.Camera.from_klv(tags(), W, H)
    gsd = 300 * 2 * math.tan(math.radians(10)) / W
    assert cam.ground_pixel_size(LAT, LON, GROUND) == pytest.approx((gsd, gsd), rel=1e-3)
