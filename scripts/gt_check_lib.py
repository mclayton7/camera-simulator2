"""Helpers for the ground-truth acceptance check (ROADMAP 2.7, scripts/gt_occlusion_check.py).

Pure functions over CamSim's COCO JSONL (one JSON object per annotated frame) plus an
overlay renderer. Field conventions are documented in docs/ground-truth.md.
"""

from __future__ import annotations

import json
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from pycocotools import mask as cocomask

GREEN = (0, 255, 0)
YELLOW = (255, 230, 0)
CYAN = (0, 230, 255)
ORANGE = (255, 140, 0)
MAGENTA = np.array([255, 0, 255], np.float32)

# box3d.corners_px: bottom face then top, each rear-left, rear-right, front-right, front-left.
BOX3D_EDGES = [(0, 1), (1, 2), (2, 3), (3, 0), (4, 5), (5, 6), (6, 7), (7, 4)] + [
    (i, i + 4) for i in range(4)
]


def load_coco(path: Path) -> list[dict]:
    """One dict per frame line; a trailing partial line (file still being written) is skipped."""
    frames = []
    for line in Path(path).read_text().splitlines():
        if not line.startswith("{"):
            continue
        try:
            frames.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return frames


def ann_for(frames: list[dict], cls: str) -> list[tuple[dict, dict]]:
    """(frame, annotation) pairs for every annotation of category name `cls`."""
    return [
        (f, a)
        for f in frames
        for a in f.get("annotations", [])
        if a.get("category", {}).get("name") == cls
    ]


def _rle_obj(seg: dict) -> dict:
    counts = seg["counts"]
    if isinstance(counts, str):
        counts = counts.encode()
    elif not isinstance(counts, bytes):
        raise TypeError(f"counts is {type(counts).__name__}, not a string")
    return {"size": [int(seg["size"][0]), int(seg["size"][1])], "counts": counts}


def rle_area(seg: dict) -> int:
    """Pixel count of a COCO compressed RLE (pycocotools.mask.area)."""
    return int(cocomask.area(_rle_obj(seg)))


def decode_rle(seg: dict) -> np.ndarray:
    """H x W uint8 mask."""
    return cocomask.decode(_rle_obj(seg))


def check_rle(frames: list[dict]) -> list[str]:
    """Problems: a `segmentation` that doesn't decode, or whose area != the annotation's `area`."""
    problems = []
    for f in frames:
        for a in f.get("annotations", []):
            seg = a.get("segmentation")
            if seg is None:
                continue
            where = f"frame {f.get('frame_id')} entity {a.get('entity_id')}"
            try:
                m = decode_rle(seg)
                n = int(m.sum())
                if n != rle_area(seg):
                    problems.append(f"{where}: decoded sum {n} != RLE area")
                    continue
            except Exception as e:  # noqa: BLE001 - any decode failure is the finding
                problems.append(f"{where}: undecodable segmentation ({e})")
                continue
            if n != round(float(a.get("area", -1))):
                problems.append(f"{where}: RLE area {n} != area {a.get('area')}")
    return problems


def obb_heading_error_deg(ann: dict | list, expected_image_angle_deg: float) -> float:
    """|OBB long-axis angle - expected| folded mod 180 into [0, 90]. `ann` is an annotation
    (its `obb` is used) or the `[cx, cy, w, h, angle_deg]` list itself."""
    obb = ann["obb"] if isinstance(ann, dict) else ann
    d = (float(obb[4]) - expected_image_angle_deg) % 180.0
    return round(min(d, 180.0 - d), 9)


def nadir_image_angle_deg(vehicle_heading_deg: float, camera_yaw_deg: float) -> float:
    """Image angle (from +x toward +y, the OBB convention) of a vehicle's long axis seen
    straight down by a camera with the given yaw: at yaw 0 image +x is east and +y south,
    so heading h (from north, clockwise) points along (sin h, -cos h) -> angle h - 90."""
    return (vehicle_heading_deg - camera_yaw_deg) - 90.0


def obb_corners(obb: list[float]) -> list[tuple[float, float]]:
    """The 4 corners of `[cx, cy, w, h, angle_deg]`: c +- (w/2)(cos, sin) +- (h/2)(-sin, cos)."""
    cx, cy, w, h, a = (float(v) for v in obb)
    c, s = math.cos(math.radians(a)), math.sin(math.radians(a))
    ux, uy = 0.5 * w * c, 0.5 * w * s
    vx, vy = -0.5 * h * s, 0.5 * h * c
    return [
        (cx - ux - vx, cy - uy - vy),
        (cx + ux - vx, cy + uy - vy),
        (cx + ux + vx, cy + uy + vy),
        (cx - ux + vx, cy - uy + vy),
    ]


def draw_overlay(png_in: Path, png_out: Path, anns: list[dict]) -> None:
    """Modal mask tinted magenta, modal bbox green, amodal bbox orange (when it differs),
    OBB yellow, projected 3D box cyan. Boxes are pixel-edge [x, y, w, h]: drawn on the
    outermost covered pixels."""
    im = Image.open(png_in).convert("RGB")
    rgb = np.asarray(im).astype(np.float32)
    for a in anns:
        seg = a.get("segmentation")
        if seg:
            m = decode_rle(seg).astype(bool)
            if m.shape == rgb.shape[:2]:
                rgb[m] = 0.5 * rgb[m] + 0.5 * MAGENTA
    im = Image.fromarray(rgb.clip(0, 255).astype(np.uint8))
    d = ImageDraw.Draw(im)

    def rect(b: list[float], color: tuple[int, int, int]) -> None:
        x, y, w, h = (float(v) for v in b)
        d.rectangle([x, y, x + w - 1, y + h - 1], outline=color)

    for a in anns:
        box = a.get("box3d") or {}
        pts = box.get("corners_px")
        if pts and all(p is not None for p in pts):
            for i, j in BOX3D_EDGES:
                d.line([tuple(pts[i]), tuple(pts[j])], fill=CYAN, width=1)
        if a.get("obb"):
            c = obb_corners(a["obb"])
            d.line(c + [c[0]], fill=YELLOW, width=1)
        if a.get("bbox_amodal") and a["bbox_amodal"] != a.get("bbox"):
            rect(a["bbox_amodal"], ORANGE)
        if a.get("bbox"):
            rect(a["bbox"], GREEN)
    Path(png_out).parent.mkdir(parents=True, exist_ok=True)
    im.save(png_out)
