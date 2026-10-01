"""Unit tests for scripts/gt_check_lib.py (ROADMAP 2.7 acceptance helpers)."""

import json

import gt_check_lib as g
import numpy as np
from PIL import Image
from pycocotools import mask as cocomask


def _rle(m: np.ndarray) -> dict:
    """COCO JSON form of a mask: counts as str, size [H, W] (what CamSim writes)."""
    r = cocomask.encode(np.asfortranarray(m.astype(np.uint8)))
    return {
        "size": [int(r["size"][0]), int(r["size"][1])],
        "counts": r["counts"].decode(),
    }


def _mask_4x5() -> np.ndarray:
    m = np.zeros((4, 5), np.uint8)
    m[1:3, 1:4] = 1  # 2 x 3 = 6 pixels
    return m


def test_rle_area_of_4x5_mask_is_6():
    assert g.rle_area(_rle(_mask_4x5())) == 6


def test_check_rle_flags_area_mismatch_only():
    seg = _rle(_mask_4x5())
    frames = [
        {
            "frame_id": 7,
            "annotations": [
                {"entity_id": 1, "area": 6.0, "segmentation": seg},
                {"entity_id": 2, "area": 5.0, "segmentation": seg},
                {"entity_id": 3, "area": 9.0},  # projection fallback: no mask
            ],
        }
    ]
    problems = g.check_rle(frames)
    assert len(problems) == 1
    assert "frame 7" in problems[0] and "entity 2" in problems[0]


def test_check_rle_flags_undecodable():
    frames = [
        {
            "frame_id": 1,
            "annotations": [
                {
                    "entity_id": 4,
                    "area": 1.0,
                    "segmentation": {"size": [4, 5], "counts": 42},
                }
            ],
        }
    ]
    problems = g.check_rle(frames)
    assert len(problems) == 1 and "entity 4" in problems[0]


def test_obb_heading_error_wraps_mod_180():
    assert g.obb_heading_error_deg([0, 0, 10, 2, 89], -89) == 2.0
    assert g.obb_heading_error_deg([0, 0, 10, 2, 30], 30 + 180) == 0.0
    assert g.obb_heading_error_deg([0, 0, 10, 2, 0], 90) == 90.0


def test_nadir_image_angle():
    # Camera yaw 0 at nadir: image +x = east, +y = south. A vehicle heading east
    # lies along +x (0 deg); heading north points to -y (-90 == 90 mod 180).
    assert g.nadir_image_angle_deg(90.0, 0.0) % 180.0 == 0.0
    assert g.nadir_image_angle_deg(0.0, 0.0) % 180.0 == 90.0
    assert g.nadir_image_angle_deg(90.0, 90.0) % 180.0 == 90.0


def test_load_coco_two_lines(tmp_path):
    p = tmp_path / "camsim_coco.jsonl"
    p.write_text(
        json.dumps({"frame_id": 1, "annotations": []})
        + "\n"
        + json.dumps({"frame_id": 2, "annotations": [{"category": {"name": "truck"}}]})
        + "\n"
        + '{"frame_id": 3, "annot'  # a line still being written
    )
    frames = g.load_coco(p)
    assert [f["frame_id"] for f in frames] == [1, 2]
    pairs = g.ann_for(frames, "truck")
    assert len(pairs) == 1 and pairs[0][0]["frame_id"] == 2


def test_obb_corners_axis_aligned():
    pts = g.obb_corners([5.0, 3.0, 4.0, 2.0, 0.0])
    xs = sorted(p[0] for p in pts)
    ys = sorted(p[1] for p in pts)
    assert xs == [3.0, 3.0, 7.0, 7.0] and ys == [2.0, 2.0, 4.0, 4.0]


def test_draw_overlay_tints_mask_and_draws_box(tmp_path):
    src = tmp_path / "in.png"
    Image.new("RGB", (5, 4), (0, 0, 0)).save(src)
    ann = {
        "bbox": [1.0, 1.0, 3.0, 2.0],
        "segmentation": _rle(_mask_4x5()),
        "obb": [2.5, 2.0, 3.0, 2.0, 0.0],
        "box3d": {"corners_px": [[1, 1], [3, 1], [3, 3], [1, 3]] * 2},
    }
    out = tmp_path / "out.png"
    g.draw_overlay(src, out, [ann])
    im = np.asarray(Image.open(out).convert("RGB"))
    assert im.shape == (4, 5, 3)
    assert im[0, 0].tolist() == [0, 0, 0]  # outside everything
    assert im.sum() > 0
