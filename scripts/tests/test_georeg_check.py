"""Unit tests for scripts/georeg_check.py (REALISM R6 geo-registration runner): view geometry, evaluation, gates."""

import dataclasses
import json
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
             "heading": view.site["heading_deg"], "camera": dataclasses.asdict(pose),
             "corner_hot": {k: 20.0 for k in ("82", "84", "86", "88", "23")}}
    # The mask is the silhouette of the whole box (top face and the visible side faces), as CamSim's stencil is
    quad = g.convex_hull([cam.project_distorted(*c, k1, 0.0) for c in gc.marker_box(view, place)])
    # Point-sample the projected (convex) quad at each pixel centre plus a per-frame sub-pixel jitter (8 x 8 offsets),
    # as CamSim's stencil is sampled under TSR's jitter: the mean centroid over the frames is the area centroid.
    r0, c0 = int(min(q[1] for q in quad)) - 2, int(min(q[0] for q in quad)) - 2
    r1, c1 = int(max(q[1] for q in quad)) + 3, int(max(q[0] for q in quad)) + 3
    gy, gx = np.mgrid[r0:r1, c0:c1]
    k = len(quad)
    sign = np.sign(sum((quad[i][0] - quad[i - 1][0]) * (quad[(i + 1) % k][1] - quad[i][1])
                       - (quad[i][1] - quad[i - 1][1]) * (quad[(i + 1) % k][0] - quad[i][0]) for i in range(k)))
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


@pytest.mark.parametrize("name", ["flat/nadir", "flat/oblique", "flat/slant", "flat/corner"])
def test_evaluate_on_a_perfect_render_is_within_a_tenth_of_a_pixel(name):
    view = next(v for v in gc.views([SITE]) if v.name == name)
    frames, klv, place = _synthetic(view)
    r = gc.evaluate(view, place, frames, klv, k1=0.0, k2=0.0)
    # (c) over the 5 km footprint: the homography is a plane, the ellipsoid is not (~0.12 px of curvature)
    assert r["error_px"]["a"] < 0.1 and r["error_px"]["c"] < (0.2 if name == "flat/slant" else 0.1), r
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
    # results without corner_ray_px have no G3 data: they fail it
    assert gates["G3"] == {"pass": False, "failed": ["flat/nadir", "roof/oblique", "flat/corner", "slope/slant"]}
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


def test_commanded_pose_prediction_and_corner_rays():
    view = next(v for v in gc.views([SITE]) if v.name == "flat/oblique")
    frames, klv, place = _synthetic(view)
    r = gc.evaluate(view, place, frames, klv, k1=0.0, k2=0.0)
    assert r["error_px"]["cmd"] < 0.1
    assert max(r["corner_ray_px"].values()) < 0.05, r["corner_ray_px"]


def test_a_corner_off_its_ray_fails_g3():
    view = next(v for v in gc.views([SITE]) if v.name == "flat/oblique")
    frames, klv, place = _synthetic(view)
    for p in klv:
        p["tags"][83] += 0.0001  # ~9 m east: off the upper-left ray
    r = gc.evaluate(view, place, frames, klv, k1=0.0, k2=0.0)
    assert r["corner_ray_px"]["82"] > 5
    assert gc.gates([r])["G3"] == {"pass": False, "failed": ["flat/oblique"]}


def test_pan_poses_sweep_the_gimbal_about_the_oblique_view():
    base = gc.camera_pose(next(v for v in gc.views([SITE]) if v.name == "flat/oblique"), 20.0)
    assert gc.pan_pose(SITE, 20.0, 0.0) == base
    p = gc.pan_pose(SITE, 20.0, gc.PAN_PERIOD_S / 4)
    assert p.gimbal_yaw == pytest.approx(gc.PAN_AMP_DEG) and (p.lat, p.lon, p.alt) == (base.lat, base.lon, base.alt)


def test_evaluate_pan_finds_a_one_frame_klv_lag(tmp_path):
    view = next(v for v in gc.views([SITE]) if v.name == "flat/oblique")
    place = {"lat": SITE["lat"], "lon": SITE["lon"], "h": 20.0, "normal_az": 0.0, "normal_el": 90.0,
             "heading": SITE["heading_deg"]}
    box = gc.marker_box(view, place)
    frames, klv = [], []
    poses = [gc.pan_pose(SITE, 20.0, i / 30.0) for i in range(40)]
    for i, pose in enumerate(poses):
        cam = g.Camera.from_klv(gc.pose_tags(pose, W, H), W, H)
        cx, cy = g.silhouette_centroid(cam.project, box)
        m = np.zeros((H, W), bool)
        m[round(cy) - 3 : round(cy) + 3, round(cx) - 3 : round(cx) + 3] = True  # a 6 x 6 blob at the true centroid
        ann = {"category": {"name": "georeg_marker"}, "visibility": 1.0, "segmentation": _rle(m)}
        frames.append({"frame_id": i, "timestamp_us": 1000 + i, "annotations": [ann]})
        # The KLV stamped with frame i carries frame i - 1's pose (one frame late)
        klv.append({"tags": {**{str(k): v for k, v in gc.pose_tags(poses[max(0, i - 1)], W, H).items()},
                             "2": 1000 + i}})
    (tmp_path / "placement.json").write_text(json.dumps(place))
    (tmp_path / "coco.jsonl").write_text("".join(json.dumps(f) + "\n" for f in frames))
    (tmp_path / "klv.jsonl").write_text("".join(json.dumps(p) + "\n" for p in klv))
    r = gc.evaluate_pan(tmp_path)
    assert r["best_lag"] == 1
    assert r["lags"][1]["median_px"] < 1.0 < r["lags"][0]["median_px"]


def test_g3_catches_a_corner_at_the_wrong_range_along_its_own_ray():
    # The KLV corner on the right bearing but where the ray meets a surface 150 m too low: a bearing-only check
    # passes it; the surface height at the corner (HOT) must not
    view = next(v for v in gc.views([SITE]) if v.name == "flat/oblique")
    frames, klv, place = _synthetic(view)
    cam = g.Camera.from_klv(gc.pose_tags(gc.scenario.Pose(**place["camera"]), W, H), W, H)
    lat, lon = cam.unproject(0, 0, 20.0 - 150.0)
    for p in klv:
        p["tags"][82], p["tags"][83] = lat, lon
    r = gc.evaluate(view, place, frames, klv, k1=0.0, k2=0.0)
    assert r["corner_ray_px"]["82"] > 5
    assert gc.gates([r])["G3"]["failed"] == ["flat/oblique"]


def test_g3_needs_the_corner_heights():
    view = next(v for v in gc.views([SITE]) if v.name == "flat/oblique")
    frames, klv, place = _synthetic(view)
    del place["corner_hot"]
    r = gc.evaluate(view, place, frames, klv, k1=0.0, k2=0.0)
    assert gc.gates([r])["G3"]["failed"] == ["flat/oblique"]
