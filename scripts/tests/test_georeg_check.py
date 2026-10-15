"""Unit tests for scripts/georeg_check.py (REALISM R6 geo-registration runner): view geometry, evaluation, gates."""

import math

import georeg_check as gc
import georeg_lib as g
import numpy as np
import pytest
from pycocotools import mask as cocomask

W, H = 1280, 720
SITE = {"name": "flat", "kind": "flat", "lat": 33.3073, "lon": -117.3446, "heading_deg": 30.0}


def test_ten_views_with_the_spec_geometry():
    sites = [SITE, {**SITE, "name": "slope", "kind": "slope", "heading_deg": 120.0},
             {**SITE, "name": "roof", "kind": "roof", "heading_deg": 250.0}]
    views = gc.views(sites)
    assert [v.name for v in views] == [
        "flat/nadir", "flat/oblique", "flat/slant", "slope/nadir", "slope/oblique", "slope/slant",
        "roof/nadir", "roof/oblique", "roof/slant", "flat/corner"]
    slant = next(v for v in views if v.name == "roof/slant")
    assert (slant.depression, slant.slant_m, slant.fov_h, slant.marker_type) == (10.0, 5000.0, 5.0, 9002)


@pytest.mark.parametrize("name", ["flat/nadir", "flat/oblique", "flat/slant", "flat/corner"])
def test_camera_pose_puts_the_marker_where_the_view_asks(name):
    view = next(v for v in gc.views([SITE]) if v.name == name)
    pose = gc.camera_pose(view, marker_h=20.0)
    cam = g.Camera.from_klv(gc.pose_tags(pose, W, H), W, H)
    x, y = cam.project(SITE["lat"], SITE["lon"], 20.0)
    tx, ty = view.target
    # The 5 km view ignores the Earth's curvature in its aim (~2 m at 5 km): a few pixels at 5 deg FOV.
    tol = 8.0 if name == "flat/slant" else 0.5
    assert x == pytest.approx(tx * W, abs=tol) and y == pytest.approx(ty * H, abs=tol)
    slant = np.linalg.norm(g.ecef(pose.lat, pose.lon, pose.alt) - g.ecef(SITE["lat"], SITE["lon"], 20.0))
    assert slant == pytest.approx(view.slant_m, rel=1e-3)


def _rle(mask):
    r = cocomask.encode(np.asfortranarray(mask.astype(np.uint8)))
    return {"size": [H, W], "counts": r["counts"].decode()}


def _synthetic(view, k1=0.0):
    """COCO frames and KLV packets a perfect CamSim would produce for the view (a rasterised plate)."""
    pose = gc.camera_pose(view, marker_h=20.0)
    tags = gc.pose_tags(pose, W, H)
    cam = g.Camera.from_klv(tags, W, H)
    for k, (px, py) in zip((82, 84, 86, 88), ((0, 0), (W, 0), (W, H), (0, H))):
        tags[k], tags[k + 1] = cam.unproject(px, py, 20.0)
    tags[23], tags[24] = cam.unproject(W / 2, H / 2, 20.0)
    place = {"lat": SITE["lat"], "lon": SITE["lon"], "h": 20.0, "normal_az": 0.0, "normal_el": 90.0,
             "heading": view.site["heading_deg"]}
    corners = gc.marker_corners(view, place)
    quad = [cam.project_distorted(*c, k1, 0.0) for c in corners]
    # Point-sample the projected (convex) quad at each pixel centre plus a per-frame sub-pixel jitter (8 x 8 offsets),
    # as CamSim's stencil is sampled under TSR's jitter: the mean centroid over the frames is the area centroid.
    r0, c0 = int(min(q[1] for q in quad)) - 2, int(min(q[0] for q in quad)) - 2
    r1, c1 = int(max(q[1] for q in quad)) + 3, int(max(q[0] for q in quad)) + 3
    gy, gx = np.mgrid[r0:r1, c0:c1]
    sign = np.sign(sum((quad[i][0] - quad[i - 1][0]) * (quad[(i + 1) % 4][1] - quad[i][1])
                       - (quad[i][1] - quad[i - 1][1]) * (quad[(i + 1) % 4][0] - quad[i][0]) for i in range(4)))
    frames = []
    for i, (ox, oy) in enumerate((ox, oy) for ox in np.arange(8) / 8 + 1 / 16 for oy in np.arange(8) / 8 + 1 / 16):
        px, py = gx + ox, gy + oy
        inside = np.ones_like(px, bool)
        for (x0, y0), (x1, y1) in zip(quad, quad[1:] + quad[:1]):
            inside &= sign * ((x1 - x0) * (py - y0) - (y1 - y0) * (px - x0)) >= 0
        mask = np.zeros((H, W), bool)
        mask[r0:r1, c0:c1] = inside
        ann = {"category": {"id": view.marker_type, "name": "georeg_marker"}, "visibility": 1.0,
               "segmentation": _rle(mask)}
        frames.append({"frame_id": i, "timestamp_us": i, "annotations": [ann]})
    return frames, [{"tags": dict(tags)} for _ in range(10)], place


@pytest.mark.parametrize("name", ["flat/nadir", "flat/oblique", "flat/corner"])
def test_evaluate_on_a_perfect_render_is_within_a_tenth_of_a_pixel(name):
    view = next(v for v in gc.views([SITE]) if v.name == name)
    frames, klv, place = _synthetic(view)
    r = gc.evaluate(view, place, frames, klv, k1=0.0, k2=0.0)
    assert r["error_px"]["a"] < 0.1 and r["error_px"]["c"] < 0.1, r
    assert r["frames"] == 64


def test_evaluate_reports_a_marker_not_seen():
    view = gc.views([SITE])[0]
    r = gc.evaluate(view, {"lat": 0, "lon": 0, "h": 0, "normal_az": 0, "normal_el": 90, "heading": 0},
                    [{"frame_id": 1, "annotations": []}], [], k1=0.0, k2=0.0)
    assert r["error"] == "marker not seen (0 clean frames, need 30)"


def test_evaluate_rejects_klv_that_moves():
    view = gc.views([SITE])[0]
    frames, klv, place = _synthetic(view)
    klv[3]["tags"][5] += 1.0
    assert gc.evaluate(view, place, frames, klv, 0.0, 0.0)["error"].startswith("KLV not static: tag 5")


def test_gates_apply_g2_to_flat_and_corner_only():
    res = [{"view": "flat/nadir", "error_px": {"a": 0.4, "b": 0.4, "c": 0.5}},
           {"view": "roof/oblique", "error_px": {"a": 0.6, "b": 0.6, "c": 4.0}},
           {"view": "flat/corner", "error_px": {"a": 0.9, "b": 0.9, "c": 1.2}},
           {"view": "slope/slant", "error": "marker not seen (3 clean frames, need 30)"}]
    gates = gc.gates(res)
    assert gates["G1"] == {"pass": False, "failed": ["slope/slant"]}
    assert gates["G2"] == {"pass": False, "failed": ["flat/corner"]}


def test_pose_tags_match_the_klv_meaning():
    pose = gc.scenario.Pose(33.0, -117.0, 500.0, yaw=45.0, gimbal_pitch=-30.0, fov_h=10.0)
    t = gc.pose_tags(pose, W, H)
    assert (t[13], t[14], t[75], t[5], t[19], t[16]) == (33.0, -117.0, 500.0, 45.0, -30.0, 10.0)
    assert t[17] == pytest.approx(math.degrees(2 * math.atan(math.tan(math.radians(5.0)) * H / W)))


def test_with_lens_distortion_b_matches_and_a_misses_at_the_corner():
    view = next(v for v in gc.views([SITE]) if v.name == "flat/corner")
    frames, klv, place = _synthetic(view, k1=-0.2)
    r = gc.evaluate(view, place, frames, klv, k1=-0.2, k2=0.0)
    assert r["error_px"]["b"] < 0.1 and r["error_px"]["a"] > 2.0, r["error_px"]
