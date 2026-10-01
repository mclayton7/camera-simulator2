# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy", "pillow"]
# ///
"""Thermal IR acceptance check (ROADMAP 4A, 4B gate m): MWIR / LWIR at noon and 02:00 with the DIS truck + boat.

Usage: uv run scripts/thermal_check.py [--band mwir|lwir|both] [--out DIR]
                                       [--runs bands,hd,eo] [--check-only]
  caffeinate -ims uv run scripts/thermal_check.py --band both
  (caffeinate: a host that sleeps mid-run freezes CamSim and spoils the frame times)

21 December 2026 at the Presidio, San Francisco: solar noon = 20:10 UTC (sun elevation
28.8 deg), 02:00 local = 10:10 UTC. Every launch is headless (macOS, local build, unicast to
127.0.0.1), with DIS on and `send_dis_test.py both` running; the camera is driven over CIGI
(the vehicles' paths are deterministic, so the camera predicts where they are). IR is CIGI
Sensor Control sensor 1; the IR preset comes from CAMSIM_IR_PRESET.

Launches (--runs):
  bands  one per band (mwir_cooled, lwir_uncooled), COCO ground truth on (interval 1), the
         thermal builder log on (camsim.Thermal.Log 1). At night, then at noon:
           nadir_truck   nadir over the truck, 330 m up, 30 deg FOV (30 frames)
           coast         static oblique over the Presidio: Golden Gate water + land (30)
           sky           static, over the truck loop, 90 deg FOV, looking east, pitched
                         +8 deg so the top 30 % rows are sky and the bottom 30 % terrain (10)
           nadir_boat    nadir over the boat, 330 m up, 30 deg FOV (10, info)
           oblique_truck (noon only) 45 deg down from 233 m east of the truck, 25 deg FOV:
                         its shadow (cast north at noon) is beside it, not behind it (10)
           nadir_mixed   (night only, ROADMAP 4B) nadir over the Presidio land-cover mix
                         (37.7935, -122.4600), 800 m up, 40 deg FOV (10)
  hd     mwir_cooled at 1920x1080 (CAMSIM_CAPTURE_WIDTH/HEIGHT), noon, nadir_truck for 20 s
  eo     two EO launches (sensor 0), noon, static nadir over the truck loop: thermal default
         (enabled) and CAMSIM_THERMAL_ENABLED=0; the first also takes the coast view as the
         EO baseline for the coastline shimmer ratio

Each frame is a /snapshot PNG (the NV12 frame converted to RGB); Y is recovered exactly for
IR (grey: U = V = 128) and as BT.709 luma for EO. The middle frame of each view is saved to
OUT/shots/, a region overlay to OUT/overlays/ (red / cyan = the two compared regions, green =
the COCO box), all frames to OUT/<run>/frames/<time>_<view>.npz.

Checks (spec "Testing"). Exit 0 only when every expected (check, band, time) row exists and
passes: a, b, c, d, e, h, m per selected band when `bands` is in --runs, f when `hd` is, g when
`eo` is (so `--runs bands` alone can exit 0; f and g are then not expected). A missing
row (a view with no frames, an absent run) fails:
  (a) night nadir_truck: mean Y in [60, 180] and < 5 % of pixels at Y <= 16
  (b) night nadir_truck: truck box mean Y >= ring (box dilated 2x minus the box) mean + 3 DN
  (c) coast: water - land mean Y (radius-matched regions: same distance from the image
      centre, so the IR optics' vignetting cancels) changes sign between noon and night
  (d) noon oblique_truck: shadow mean Y < sunlit ground mean Y. Shadow = the truck's ground
      footprint (COCO box3d bottom face) swept away from the sun by H / tan(sun elevation),
      minus the truck box; sunlit = the box ring minus the shadow
  (e) sky: top 30 % mean Y < bottom 30 % mean Y (both bands, night and noon)
  (f) hd: thermal_gpu_ms p95 <= 0.5 ms at 1080p (frame stats)
  (g) eo: |mean Y(thermal on) - mean Y(thermal off)| <= 1 DN
  (h) coast, night and noon: edge shimmer = temporal std of Y on edge pixels of the static
      coast view <= 2 x max(std of interior land pixels, 1/sqrt(12) DN = 8-bit rounding noise;
      the raw ratio is reported beside it) (Task 17: ThermalCS runs before TSR;
      the EO baseline is printed beside it, info). The truck box-boundary ratio is printed
      too, labelled motion-contaminated: the truck moves against its box, so it is not a
      shimmer measure
  (m) night nadir_mixed (ROADMAP 4B Task 13): no visible land-cover window grid. On the
      temporal-mean frame (central 60 %, entity boxes masked), the 2D power spectrum's mean in a
      band around the grid fundamental (period 10 m / GSD px, GSD from 800 m and 40 deg; along
      each image axis, |f - f0| <= 0.15 f0) over the mean of the two neighbouring bands of equal
      width must be <= 2 for both axes
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import io
import json
import math
import os
import shutil
import subprocess
import sys
import time
import urllib.request
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts"))

EDITOR_LOG = Path.home() / "Library/Logs/CamSimTest/CamSimTest.log"
LAT_SF, LON_SF = 37.795, -122.460  # the truck loop (send_dis_test.py truck-loop)

NIGHT = dict(utc_hour=10, utc_minute=10, month=12, day=21)  # 02:00 PST
NOON = dict(utc_hour=20, utc_minute=10, month=12, day=21)  # solar noon at -122.46
TIMES = {"night": NIGHT, "noon": NOON}
BANDS = {"mwir": "mwir_cooled", "lwir": "lwir_uncooled"}

NADIR_UP_M = 330.0
NADIR_FOV = 30.0
SETTLE_S = 10.0  # AE / AGC after a pose or time change

# Thresholds (spec "Testing").
NIGHT_MEAN = (60.0, 180.0)
BLACK_DN = 16.0
BLACK_FRAC = 0.05
WHITE_HOT_DN = 3.0
THERMAL_P95_MS = 0.5
EO_DY_DN = 1.0
SHIMMER_RATIO = 2.0
# Gate (h) denominator floor (ruling R13): the temporal std that rounding to 8-bit Y adds on its
# own, 1/sqrt(12) DN (uniform quantization error). The snapshot can't resolve interior noise
# below it, so a quieter detector (MWIR at noon: 0.19 DN) must not inflate the edge ratio.
SNAPSHOT_QUANT_STD_DN = 1.0 / math.sqrt(12.0)
MIN_PIXELS = 50

# ROADMAP 4B Task 13, gate (m): no visible land-cover window grid at night.
MIXED_CENTER = (37.7935, -122.4600)  # Presidio interior: forest, lawns, roads, buildings, no open water
MIXED_UP_M = 800.0  # above the truck ground (dvc.TRUCK_GROUND_HAE)
MIXED_FOV = 40.0
CENTRAL = 0.6  # central 60 % of the frame (IR optics: distortion and vignetting grow outwards)
GRID_TEXEL_M = 10.0  # land-cover window texel
GRID_BAND = 0.15  # band half-width as a fraction of the grid fundamental (the terrain height is not known exactly)
GRID_PEAK_RATIO = 2.0  # (m) fundamental band / neighbouring bands

# Fixed-pose regions as fractions of the image height (coast) / rows (sky).
COAST_R = (0.26, 0.40)  # radius band from the image centre
COAST_WATER_ROWS = (0.44, 0.60)
COAST_LAND_ROW0 = 0.75
SKY_ROWS = 0.30


# ---------------------------------------------------------------------------
# Pure helpers (scripts/tests/test_thermal_check.py)
# ---------------------------------------------------------------------------


def y_from_rgb(rgb: np.ndarray) -> np.ndarray:
    """Studio-range Y (DN) from the /snapshot RGB, the inverse of CamSimNv12::ToBgra's luma.
    Exact for grey (IR: U = V = 128) up to the 8-bit rounding, which this undoes."""
    rgb = rgb.astype(np.float32)
    yn = (0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]) / 255.0
    return 16.0 + 219.0 * yn


def box_mask(shape: tuple[int, int], box: list[float]) -> np.ndarray:
    """Pixels inside a COCO [x, y, w, h] box (clipped to the image)."""
    h, w = shape
    x0, y0 = max(0, math.floor(box[0])), max(0, math.floor(box[1]))
    x1 = min(w, math.ceil(box[0] + box[2]))
    y1 = min(h, math.ceil(box[1] + box[3]))
    m = np.zeros(shape, bool)
    if x1 > x0 and y1 > y0:
        m[y0:y1, x0:x1] = True
    return m


def scale_box(box: list[float], factor: float) -> list[float]:
    """The box scaled about its centre."""
    cx, cy = box[0] + box[2] / 2.0, box[1] + box[3] / 2.0
    w, h = box[2] * factor, box[3] * factor
    return [cx - w / 2.0, cy - h / 2.0, w, h]


def ring_mask(
    shape: tuple[int, int], box: list[float], factor: float = 2.0
) -> np.ndarray:
    """The box dilated `factor` x about its centre, minus the box."""
    return box_mask(shape, scale_box(box, factor)) & ~box_mask(shape, box)


def region_mean(y: np.ndarray, mask: np.ndarray) -> float:
    return float(y[mask].mean()) if mask.any() else float("nan")


def black_fraction(y: np.ndarray, dn: float = BLACK_DN) -> float:
    """Fraction of pixels at Y <= dn (Y is recovered to within 0.5 DN, so compare at +0.5)."""
    return float((y <= dn + 0.5).mean())


def percentile(xs: list[float], p: float) -> float:
    """Linear-interpolated percentile (numpy's default), nan for an empty list."""
    return float(np.percentile(np.asarray(xs, float), p)) if xs else float("nan")


def sun_position(
    local_solar_hour: float, day_of_year: int, lat_deg: float
) -> tuple[float, float]:
    """(elevation, azimuth from north) in degrees: ACamSimEnvironment::ComputeSunPosition."""
    b = math.radians((360.0 / 365.0) * (day_of_year - 81))
    decl = math.radians(23.45 * math.sin(b))
    ha = (local_solar_hour - 12.0) * 15.0
    lat = math.radians(lat_deg)
    sin_e = math.sin(lat) * math.sin(decl) + math.cos(lat) * math.cos(decl) * math.cos(
        math.radians(ha)
    )
    elev = math.degrees(math.asin(max(-1.0, min(1.0, sin_e))))
    cos_az = (math.sin(decl) - math.sin(lat) * sin_e) / max(
        math.cos(lat) * math.cos(math.radians(elev)), 0.001
    )
    az = math.degrees(math.acos(max(-1.0, min(1.0, cos_az))))
    if ha > 0.0:
        az = 360.0 - az
    return elev, az


def sun_for(
    tod: dict, lat: float = LAT_SF, lon: float = LON_SF, year: int = 2026
) -> tuple[float, float]:
    """Sun (elevation, azimuth) for a time-of-day dict, as ACamSimEnvironment::ApplySun."""
    utc_h = tod["utc_hour"] + tod["utc_minute"] / 60.0
    local = (utc_h + lon / 15.0 + 48.0) % 24.0
    doy = dt.date(year, tod["month"], tod["day"]).timetuple().tm_yday
    return sun_position(local, doy, lat)


def shadow_offset_ne(
    height_m: float, sun_elev_deg: float, sun_az_deg: float
) -> tuple[float, float]:
    """(north, east) metres from an object's foot to its shadow tip: H / tan(elevation),
    pointing away from the sun."""
    length = height_m / math.tan(math.radians(sun_elev_deg))
    az = math.radians(sun_az_deg)
    return -length * math.cos(az), -length * math.sin(az)


def ground_basis_px(box3d: dict) -> tuple[np.ndarray, np.ndarray]:
    """Image displacement (px) of one metre north and one metre east on the ground at the
    entity, from the projected bottom face of its COCO box3d (corners rear-left,
    rear-right, front-right, front-left) and its yaw (heading from north, clockwise)."""
    c = np.asarray(box3d["corners_px"][:4], float)
    length, width = box3d["size_m"][0], box3d["size_m"][1]
    fwd = ((c[3] - c[0]) + (c[2] - c[1])) / 2.0 / length
    right = ((c[1] - c[0]) + (c[2] - c[3])) / 2.0 / width
    y = math.radians(box3d["yaw_deg"])
    north = math.cos(y) * fwd - math.sin(y) * right
    east = math.sin(y) * fwd + math.cos(y) * right
    return north, east


def convex_hull(points: list[tuple[float, float]]) -> list[tuple[float, float]]:
    pts = sorted(set(points))
    if len(pts) <= 2:
        return pts

    def cross(o, a, b) -> float:
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])

    lower: list[tuple[float, float]] = []
    for p in pts:
        while len(lower) >= 2 and cross(lower[-2], lower[-1], p) <= 0:
            lower.pop()
        lower.append(p)
    upper: list[tuple[float, float]] = []
    for p in reversed(pts):
        while len(upper) >= 2 and cross(upper[-2], upper[-1], p) <= 0:
            upper.pop()
        upper.append(p)
    return lower[:-1] + upper[:-1]


def polygon_mask(shape: tuple[int, int], poly: list[tuple[float, float]]) -> np.ndarray:
    img = Image.new("L", (shape[1], shape[0]), 0)
    if len(poly) >= 3:
        ImageDraw.Draw(img).polygon([(float(x), float(y)) for x, y in poly], fill=1)
    return np.asarray(img, bool)


def shadow_masks(
    shape: tuple[int, int], ann: dict, sun_elev: float, sun_az: float
) -> tuple[np.ndarray, np.ndarray, list[float]]:
    """(shadow, sunlit, shift_px) for a truck annotation. Shadow = hull of the box3d bottom
    face and the same face moved by the shadow offset, minus the box; sunlit = the box
    ring minus the shadow hull."""
    b3 = ann["box3d"]
    north, east = ground_basis_px(b3)
    dn, de = shadow_offset_ne(b3["size_m"][2], sun_elev, sun_az)
    shift = dn * north + de * east
    foot = [tuple(map(float, p)) for p in b3["corners_px"][:4]]
    hull = convex_hull(foot + [(x + shift[0], y + shift[1]) for x, y in foot])
    hull_m = polygon_mask(shape, hull)
    x, y, w, h = ann["bbox"]
    box = box_mask(
        shape, [x - 1.0, y - 1.0, w + 2.0, h + 2.0]
    )  # 1 px margin: soft edges
    return (
        hull_m & ~box,
        ring_mask(shape, ann["bbox"]) & ~hull_m,
        [float(shift[0]), float(shift[1])],
    )


def coast_masks(shape: tuple[int, int]) -> tuple[np.ndarray, np.ndarray]:
    """(water, land) for the coast pose: both in the same radius band about the image
    centre (so cos^4 vignetting affects them equally); water in a middle row band (the
    Golden Gate), land in the bottom rows (the Presidio)."""
    h, w = shape
    yy, xx = np.mgrid[0:h, 0:w]
    r = np.hypot(xx - (w - 1) / 2.0, yy - (h - 1) / 2.0)
    band = (r >= COAST_R[0] * h) & (r <= COAST_R[1] * h)
    water = band & (yy >= COAST_WATER_ROWS[0] * h) & (yy < COAST_WATER_ROWS[1] * h)
    land = band & (yy >= COAST_LAND_ROW0 * h)
    return water, land


def sky_masks(shape: tuple[int, int]) -> tuple[np.ndarray, np.ndarray]:
    """(sky, terrain): the top and bottom 30 % of rows."""
    h, _ = shape
    rows = np.arange(h)[:, None]
    sky = np.broadcast_to(rows < SKY_ROWS * h, shape)
    ter = np.broadcast_to(rows >= (1.0 - SKY_ROWS) * h, shape)
    return sky.copy(), ter.copy()


def edge_masks(
    mean_img: np.ndarray, roi: np.ndarray, top: float = 0.02
) -> tuple[np.ndarray, np.ndarray]:
    """(edge, interior) pixels of a temporal-mean image inside roi: the top `top` fraction
    by gradient magnitude, and the half below the median gradient."""
    gy, gx = np.gradient(mean_img.astype(np.float32))
    g = np.hypot(gx, gy)
    vals = g[roi]
    if vals.size == 0:
        return np.zeros_like(roi), np.zeros_like(roi)
    hi = np.quantile(vals, 1.0 - top)
    lo = np.quantile(vals, 0.5)
    return roi & (g >= hi), roi & (g <= lo)


def aligned_crops(
    ys: np.ndarray, boxes: list[list[float]]
) -> tuple[np.ndarray, list[float]] | None:
    """Crops of 3x the median box size centred on each frame's box centre (integer shift),
    and the median box in crop coordinates. None when a crop leaves the image."""
    if not boxes:
        return None
    bw = float(np.median([b[2] for b in boxes]))
    bh = float(np.median([b[3] for b in boxes]))
    cw, ch = int(math.ceil(3 * bw)) | 1, int(math.ceil(3 * bh)) | 1
    out = []
    for y, b in zip(ys, boxes):
        cx = int(round(b[0] + b[2] / 2.0))
        cy = int(round(b[1] + b[3] / 2.0))
        x0, y0 = cx - cw // 2, cy - ch // 2
        if x0 < 0 or y0 < 0 or x0 + cw > y.shape[1] or y0 + ch > y.shape[0]:
            return None
        out.append(y[y0 : y0 + ch, x0 : x0 + cw])
    return np.stack(out), [cw / 2.0 - bw / 2.0, ch / 2.0 - bh / 2.0, bw, bh]


def central_mask(shape: tuple[int, int], frac: float = CENTRAL) -> np.ndarray:
    h, w = shape
    y0, x0 = round(h * (1.0 - frac) / 2.0), round(w * (1.0 - frac) / 2.0)
    m = np.zeros(shape, bool)
    m[y0 : h - y0, x0 : w - x0] = True
    return m


def grid_period_px(
    up_m: float, fov_deg: float, width_px: int, texel_m: float = GRID_TEXEL_M
) -> float:
    """Image period of the land-cover window grid in a nadir view: texel / GSD."""
    gsd = 2.0 * up_m * math.tan(math.radians(fov_deg / 2.0)) / width_px
    return texel_m / gsd


def grid_peak_ratio(
    y: np.ndarray, mask: np.ndarray, period_px: float, band: float = GRID_BAND
) -> tuple[float, float]:
    """Gate (m): (x, y) ratios of the mean power in a band around the grid fundamental f0 = 1 / period_px along each
    image axis (|f_axis| in f0 (1 +- band), |f_other| <= band f0) to the mean power of the two neighbouring bands of equal
    width (f0 (1 - 3 band) .. f0 (1 - band) and f0 (1 + band) .. f0 (1 + 3 band)). Pixels outside mask (entities, the
    border) are set to the masked mean; the mask's bounding box is Hann-windowed. ~1: no grid; nan: empty mask."""
    if mask.sum() < MIN_PIXELS:
        return float("nan"), float("nan")
    rows, cols = np.nonzero(mask)
    r0, r1, c0, c1 = rows.min(), rows.max() + 1, cols.min(), cols.max() + 1
    crop = y[r0:r1, c0:c1].astype(np.float64)
    m = mask[r0:r1, c0:c1]
    mean = crop[m].mean()
    crop = np.where(m, crop, mean) - mean
    h, w = crop.shape
    power = np.abs(np.fft.fft2(crop * np.outer(np.hanning(h), np.hanning(w)))) ** 2
    fy = np.abs(np.fft.fftfreq(h))[:, None] * np.ones((1, w))
    fx = np.abs(np.fft.fftfreq(w))[None, :] * np.ones((h, 1))
    f0 = 1.0 / period_px
    hw = band * f0

    def ratio(along: np.ndarray, across: np.ndarray) -> float:
        def mean_in(lo: float, hi: float) -> float:
            sel = (along >= lo) & (along < hi) & (across <= hw)
            return float(power[sel].mean()) if sel.any() else float("nan")

        centre = mean_in(f0 - hw, f0 + hw)
        sides = 0.5 * (mean_in(f0 - 3 * hw, f0 - hw) + mean_in(f0 + hw, f0 + 3 * hw))
        return centre / sides if sides > 0 else float("nan")

    return ratio(fx, fy), ratio(fy, fx)


Row = tuple[str, str, str]  # (check, band, time)


def expected_rows(bands: list[str], runs: set[str]) -> list[Row]:
    """Every gate row the selected bands and runs must produce. a, b, c, d, e, h, m come from the
    band runs (only when `bands` is selected); f from `hd` and g from `eo`, each expected
    only when that run group is selected (so `--runs bands` can pass on its own)."""
    rows: list[Row] = []
    if "bands" in runs:
        for b in bands:
            rows += [
                ("a", b, "night"),
                ("b", b, "night"),
                ("c", b, "both"),
                ("d", b, "noon"),
                ("e", b, "night"),
                ("e", b, "noon"),
                ("h", b, "night"),
                ("h", b, "noon"),
                ("m", b, "night"),
            ]
    if "hd" in runs:
        rows.append(("f", "mwir", "noon"))
    if "eo" in runs:
        rows.append(("g", "eo", "noon"))
    return rows


def missing_rows(checks: list[dict], expected: list[Row]) -> list[Row]:
    have = {(c["check"], c.get("band"), c.get("time")) for c in checks}
    return [r for r in expected if r not in have]


def gate_passed(checks: list[dict], expected: list[Row]) -> bool:
    """True when every expected (check, band, time) row is present and no check failed.
    A view that captured no frames, or a run that never produced its data, leaves its rows
    missing, and a missing row fails."""
    return (
        bool(expected)
        and not missing_rows(checks, expected)
        and all(c["pass"] for c in checks)
    )


def shimmer_ratio(edge_std: float, interior_std: float) -> tuple[float, float]:
    """(raw, floored) edge / interior temporal-std ratios; the floored one divides by
    max(interior, SNAPSHOT_QUANT_STD_DN) and is the gated value."""
    raw = edge_std / max(interior_std, 1e-6)
    return raw, edge_std / max(interior_std, SNAPSHOT_QUANT_STD_DN)


def shimmer_row(band: str, tod: str, sh: dict, limit: float = SHIMMER_RATIO) -> dict:
    """Gate (h): the static coast view's floored edge / interior temporal-std ratio <= limit."""
    v = float(sh["value"])
    return {
        "check": "h",
        "band": band,
        "time": tod,
        "value": v,
        "raw": float(sh.get("raw", v)),
        "threshold": f"<= {limit:g}",
        "pass": bool(v <= limit),  # NaN fails
        "detail": sh["detail"],
    }


def build_report(
    meta: dict,
    checks: list[dict],
    info: list[dict],
    shots: list[str],
    expected: list[Row],
) -> dict:
    return {
        "meta": meta,
        "checks": checks,
        "missing": [list(r) for r in missing_rows(checks, expected)],
        "info": info,
        "shots": shots,
        "passed": gate_passed(checks, expected),
    }


def render_markdown(report: dict) -> str:
    def fmt(v) -> str:
        return f"{v:.3f}" if isinstance(v, float) else str(v)

    lines = [
        "# Thermal check (ROADMAP 4A)",
        "",
        f"Result: **{'PASS' if report['passed'] else 'FAIL'}**  ",
        "  ".join(f"{k}: {v}" for k, v in report["meta"].items()),
        "",
        "| Check | Band | Time | Value | Threshold | Result | Detail |",
        "|---|---|---|---|---|---|---|",
    ]
    for c in report["checks"]:
        lines.append(
            f"| {c['check']} | {c.get('band', '-')} | {c.get('time', '-')} | {fmt(c['value'])} | "
            f"{c['threshold']} | {'PASS' if c['pass'] else 'FAIL'} | {c.get('detail', '')} |"
        )
    for check, band, tod in report.get("missing", []):
        lines.append(
            f"| {check} | {band} | {tod} | - | - | FAIL | missing: no data for this row |"
        )
    lines += [
        "",
        "## Info (not gated)",
        "",
        "| Item | Band | Time | Value | Detail |",
        "|---|---|---|---|---|",
    ]
    for c in report["info"]:
        lines.append(
            f"| {c['check']} | {c.get('band', '-')} | {c.get('time', '-')} | {fmt(c['value'])} | {c.get('detail', '')} |"
        )
    lines += ["", "## Shots", ""] + [f"- {s}" for s in report["shots"]]
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# Live capture
# ---------------------------------------------------------------------------


@dataclass
class View:
    name: str
    pose_at: Callable[
        [float], object
    ]  # sender time -> scenario.Pose (time / sensor applied later)
    frames: int = 10
    cls: str | None = "truck"  # COCO class to pair with each frame
    hold_s: float = 0.0  # extra time on the view after capturing (frame-stats window)


@dataclass
class RunSpec:
    label: str
    env: dict[str, str]
    sensor_id: int
    times: list[str]
    views: Callable[[str], list[View]]  # time label -> views
    ml: bool = True
    exec_cmds: list[str] = field(default_factory=list)


def _poses():
    """Pose builders (imported lazily: the pure helpers above need no CamSim modules)."""
    import dis_vehicle_check as dvc
    import send_dis_test as sd
    from bench import scenario

    truck, boat = dvc.TRUCK, dvc.BOAT
    ground = dvc.TRUCK_GROUND_HAE

    def static(pose):
        return lambda t: pose

    def oblique_on(out_m: float, up_m: float, fov: float, from_bearing: float):
        """The camera out_m from the truck along `from_bearing` (deg from north), up_m above
        the ground, looking back at it."""
        f = sd.PathFollower(truck.waypoints_ne, truck.speed_mps)
        b = math.radians(from_bearing)

        def pose(t: float):
            lat, lon, _ = dvc.vehicle(truck, f, t)
            clat, clon = sd.ne_to_latlon(
                (lat, lon), out_m * math.cos(b), out_m * math.sin(b)
            )
            return scenario.Pose(
                clat,
                clon,
                ground + up_m,
                yaw=(from_bearing + 180.0) % 360.0,
                gimbal_pitch=-math.degrees(math.atan2(up_m, out_m)),
                fov_h=fov,
            )

        return pose

    c = truck.center
    return {
        "nadir_truck": dvc.nadir_on(truck, ground + NADIR_UP_M, NADIR_FOV),
        "nadir_boat": dvc.nadir_on(boat, dvc.BOAT_SURFACE_HAE + NADIR_UP_M, NADIR_FOV),
        # Task 14's horizon pose: Golden Gate water across the middle, the Presidio below.
        "coast": static(
            scenario.Pose(c[0], c[1], 400.0, yaw=300.0, gimbal_pitch=-8.0, fov_h=40.0)
        ),
        # East over the city; +8 deg keeps the horizon at ~62 % of the height (90 deg h-FOV
        # = 58.7 deg vertical), so the top 30 % is >= 21 deg up and the bottom 30 % >= 4 deg down.
        "sky": static(
            scenario.Pose(
                c[0], c[1], ground + NADIR_UP_M, yaw=90.0, gimbal_pitch=8.0, fov_h=90.0
            )
        ),
        "oblique_truck": oblique_on(
            233.0, 233.0, 25.0, 90.0
        ),  # margin for ground-height error
        "nadir_mixed": static(  # ROADMAP 4B: land-cover mix (gate m)
            scenario.Pose(
                MIXED_CENTER[0],
                MIXED_CENTER[1],
                ground + MIXED_UP_M,
                gimbal_pitch=-90.0,
                fov_h=MIXED_FOV,
            )
        ),
        "nadir_static": static(
            scenario.Pose(
                c[0], c[1], ground + NADIR_UP_M, gimbal_pitch=-90.0, fov_h=NADIR_FOV
            )
        ),
    }


def build_runs(bands: list[str], wanted: set[str]) -> list[RunSpec]:
    p = _poses()
    runs: list[RunSpec] = []

    def band_views(tod: str) -> list[View]:
        v = [
            View("nadir_truck", p["nadir_truck"], 30, "truck"),
            View("coast", p["coast"], 30, None),
            View("sky", p["sky"], 10, None),
            View("nadir_boat", p["nadir_boat"], 10, "boat"),
        ]
        if tod == "noon":
            v.append(View("oblique_truck", p["oblique_truck"], 10, "truck"))
        else:
            v.append(View("nadir_mixed", p["nadir_mixed"], 10, "truck"))  # (m)
        return v

    if "bands" in wanted:
        for b in bands:
            runs.append(
                RunSpec(
                    b,
                    {"CAMSIM_IR_PRESET": BANDS[b]},
                    1,
                    ["night", "noon"],
                    band_views,
                    exec_cmds=["camsim.Thermal.Log 1"],
                )
            )
    if "hd" in wanted:
        runs.append(
            RunSpec(
                "mwir_1080p",
                {
                    "CAMSIM_IR_PRESET": "mwir_cooled",
                    "CAMSIM_CAPTURE_WIDTH": "1920",
                    "CAMSIM_CAPTURE_HEIGHT": "1080",
                },
                1,
                ["noon"],
                lambda tod: [
                    View("nadir_truck", p["nadir_truck"], 5, None, hold_s=20.0)
                ],
                ml=False,
            )
        )
    if "eo" in wanted:
        for label, env in (
            ("eo_thermal_on", {}),
            ("eo_thermal_off", {"CAMSIM_THERMAL_ENABLED": "0"}),
        ):
            runs.append(
                RunSpec(
                    label,
                    env,
                    0,
                    ["noon"],
                    lambda tod, label=label: [
                        View("nadir_static", p["nadir_static"], 30, None, hold_s=5.0)
                    ]
                    + (
                        [View("coast", p["coast"], 30, None)]
                        if label == "eo_thermal_on"
                        else []
                    ),
                    ml=False,
                )
            )
    return runs


def last_frame_id(coco: Path) -> int:
    """frame_id of the last complete COCO line (-1 if none yet). Reads only the tail."""
    if not coco.exists():
        return -1
    with coco.open("rb") as fh:
        fh.seek(0, os.SEEK_END)
        fh.seek(max(0, fh.tell() - (1 << 20)))
        tail = fh.read().decode("utf-8", errors="replace")
    for line in reversed(tail.split("\n")):
        if line.startswith('{"frame_id":'):
            try:
                return int(json.loads(line)["frame_id"])
            except (json.JSONDecodeError, KeyError, ValueError):
                continue
    return -1


def fetch_png() -> bytes | None:
    from bench import run_bench as rb

    try:
        with urllib.request.urlopen(rb.HEALTH + "/snapshot", timeout=10) as r:
            return r.read()
    except Exception as e:  # noqa: BLE001 - report and carry on
        print(f"[thermal] snapshot failed: {e}", flush=True)
        return None


def capture_view(
    out: Path, rdir: Path, label: str, tod: str, v: View, coco: Path | None
) -> dict:
    ys, befores = [], []
    shot = out / "shots" / f"{label}_{tod}_{v.name}.png"
    for i in range(v.frames):
        before = last_frame_id(coco) if coco else -1
        data = fetch_png()
        if data is None:
            continue
        rgb = np.asarray(Image.open(io.BytesIO(data)).convert("RGB"))
        ys.append(np.clip(np.round(y_from_rgb(rgb)), 0, 255).astype(np.uint8))
        befores.append(before)
        if i == v.frames // 2:
            shot.write_bytes(data)
    npz = rdir / "frames" / f"{tod}_{v.name}.npz"
    if ys:
        np.savez_compressed(npz, y=np.stack(ys))
    return {
        "view": v.name,
        "time": tod,
        "cls": v.cls,
        "npz": str(npz),
        "shot": str(shot),
        "befores": befores,
    }


def run_once(spec: RunSpec, out: Path) -> dict:
    import dis_vehicle_check as dvc
    from bench import run_bench as rb

    rdir = out / spec.label
    shutil.rmtree(rdir, ignore_errors=True)
    (rdir / "frames").mkdir(parents=True)
    stats = rdir / "frames.jsonl"
    coco = rdir / "ml" / "camsim_coco.jsonl"
    pid_file = REPO / ".cache" / "camsim.pid"
    if rb.camsim_alive(pid_file):
        sys.exit("A CamSim instance is already running (scripts/stop.sh to stop it)")
    env = dict(
        os.environ,
        CAMSIM_DIS_ENABLED="1",
        CAMSIM_ML_ENABLED="1" if spec.ml else "0",
        CAMSIM_ML_OUTPUT_DIR=str(rdir / "ml"),
        CAMSIM_ML_DEPTH_ENABLED="0",
        CAMSIM_ML_INTERVAL_FRAMES="1",
        CAMSIM_SNAPSHOT_ENDPOINT_ENABLED="1",
        CAMSIM_MULTICAST_ADDR="127.0.0.1",
        CAMSIM_FRAME_STATS_PATH=str(stats),
    )
    env.update(spec.env)
    first_tod = TIMES[spec.times[0]]

    def apply(pose, tod: dict):
        return dataclasses.replace(pose, sensor_id=spec.sensor_id, **tod)

    rb.wait_port_free(int(rb.HEALTH.rsplit(":", 1)[1]))
    host = rb.Host()
    host.pose = apply(spec.views(spec.times[0])[0].pose_at(0.0), first_tod)
    host.thread.start()  # /ready needs CIGI traffic
    print(f"[thermal] {spec.label}: launching ({spec.env or 'defaults'})", flush=True)
    extra = [f"-ExecCmds={','.join(spec.exec_cmds)}"] if spec.exec_cmds else []
    views: list[dict] = []
    sender = tracker = None
    try:
        # Inside the try: a failed launch still stops the host thread and runs stop.sh.
        subprocess.run(
            [
                str(REPO / "scripts" / "run.sh"),
                "--headless",
                "--local",
                "--detach",
                *extra,
            ],
            env=env,
            check=True,
            stdout=subprocess.DEVNULL,
        )
        rb.wait_ready(pid_file)
        rb.wait_terrain(120)
        dvc.wait_tiles(stats)
        time.sleep(3.0)
        sender = subprocess.Popen(
            [sys.executable, str(REPO / "scripts" / "send_dis_test.py"), "both"],
            stdout=subprocess.DEVNULL,
        )
        tracker = dvc.Tracker(host, time.monotonic())
        for tod_label in spec.times:
            tod = TIMES[tod_label]
            for v in spec.views(tod_label):
                print(f"[thermal] {spec.label}: {tod_label} {v.name}", flush=True)
                tracker.pose_at = lambda t, v=v, tod=tod: apply(v.pose_at(t), tod)
                time.sleep(2.0)
                dvc.wait_tiles(stats, 60.0)
                time.sleep(SETTLE_S)
                t0 = time.time()
                rec = capture_view(
                    out, rdir, spec.label, tod_label, v, coco if spec.ml else None
                )
                time.sleep(max(0.0, t0 + v.hold_s - time.time()))
                rec.update(t0=t0, t1=time.time())
                views.append(rec)
        time.sleep(1.5)  # the last frames' COCO lines
    finally:
        if tracker:
            tracker.stop.set()
        if sender:
            sender.terminate()
        host.stop.set()
        subprocess.run(
            [str(REPO / "scripts" / "stop.sh")], check=False, stdout=subprocess.DEVNULL
        )
    if EDITOR_LOG.exists():
        shutil.copy(EDITOR_LOG, rdir / "CamSimTest.log")
    result = {"label": spec.label, "env": spec.env, "views": views}
    (rdir / "run.json").write_text(json.dumps(result, indent=2))
    return result


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------


def load_coco(path: Path) -> list[dict]:
    frames = []
    if not path.exists():
        return frames
    for line in path.read_text().splitlines():
        if line.startswith("{"):
            try:
                frames.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    return frames


def load_rows(path: Path) -> list[dict]:
    if not path.exists():
        return []
    return [json.loads(x) for x in path.read_text().splitlines() if x.startswith("{")]


@dataclass
class ViewData:
    run: str
    time: str
    view: str
    y: np.ndarray  # (N, H, W) uint8
    anns: list[dict | None]  # the view class's annotation per frame
    rec: dict


def load_view(out: Path, run: dict, rec: dict) -> ViewData | None:
    npz = Path(rec["npz"])
    if not npz.exists():
        return None
    y = np.load(npz)["y"]
    anns: list[dict | None] = [None] * len(y)
    if rec.get("cls"):
        frames = sorted(
            load_coco(out / run["label"] / "ml" / "camsim_coco.jsonl"),
            key=lambda f: f["frame_id"],
        )
        ids = [f["frame_id"] for f in frames]
        for i, before in enumerate(rec["befores"][: len(y)]):
            j = int(np.searchsorted(ids, before, side="right"))
            if j < len(frames):
                anns[i] = next(
                    (
                        a
                        for a in frames[j]["annotations"]
                        if a.get("category", {}).get("name") == rec["cls"]
                    ),
                    None,
                )
    return ViewData(run["label"], rec["time"], rec["view"], y, anns, rec)


def med(xs: list[float]) -> float:
    xs = [x for x in xs if not math.isnan(x)]
    return float(np.median(xs)) if xs else float("nan")


def overlay(
    out: Path,
    vd: ViewData,
    a: np.ndarray | None,
    b: np.ndarray | None,
    ann: dict | None,
    idx: int,
) -> str:
    """The frame with region a tinted red, region b cyan and the COCO box in green."""
    y = vd.y[idx].astype(np.float32)
    rgb = np.repeat(y[..., None], 3, axis=2)
    for m, col in ((a, (255, 0, 0)), (b, (0, 255, 255))):
        if m is not None:
            rgb[m] = 0.6 * rgb[m] + 0.4 * np.array(col, np.float32)
    img = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8))
    if ann:
        x, yy, w, h = ann["bbox"]
        ImageDraw.Draw(img).rectangle([x, yy, x + w, yy + h], outline=(0, 255, 0))
    dst = out / "overlays" / f"{vd.run}_{vd.time}_{vd.view}.png"
    img.save(dst)
    return str(dst)


def check_band(
    out: Path,
    band: str,
    views: dict[tuple[str, str], ViewData],
    checks: list,
    info: list,
    shots: list,
) -> None:
    def add(lst, check, value, threshold=None, ok=None, time_=None, detail=""):
        row = {
            "check": check,
            "band": band,
            "time": time_,
            "value": value,
            "detail": detail,
        }
        if lst is checks:
            row.update(threshold=threshold, **{"pass": bool(ok)})
        lst.append(row)

    coast_d: dict[str, float] = {}
    for tod in ("night", "noon"):
        # (a) + (b): nadir over the truck.
        vd = views.get((tod, "nadir_truck"))
        if vd is not None:
            means = [float(f.mean()) for f in vd.y]
            blacks = [black_fraction(f.astype(np.float32)) for f in vd.y]
            m, bf = float(np.mean(means)), float(np.mean(blacks))
            ok_a = NIGHT_MEAN[0] <= m <= NIGHT_MEAN[1] and bf < BLACK_FRAC
            detail = f"mean Y {m:.1f}, black (Y<=16) {100 * bf:.2f} % over {len(vd.y)} frames"
            if tod == "night":
                add(
                    checks,
                    "a",
                    m,
                    f"mean in [60, 180], black < 5 % ({100 * bf:.2f} %)",
                    ok_a,
                    tod,
                    detail,
                )
            else:
                add(info, "a(noon)", m, time_=tod, detail=detail)
            diffs, idx = [], None
            for i, (f, ann) in enumerate(zip(vd.y, vd.anns)):
                if ann is None:
                    continue
                box = box_mask(f.shape, ann["bbox"])
                ring = ring_mask(f.shape, ann["bbox"])
                if box.sum() < MIN_PIXELS or ring.sum() < MIN_PIXELS:
                    continue
                fy = f.astype(np.float32)
                diffs.append(region_mean(fy, box) - region_mean(fy, ring))
                idx = (
                    i
                    if idx is None
                    or abs(i - len(vd.y) // 2) < abs(idx - len(vd.y) // 2)
                    else idx
                )
            d = med(diffs)
            detail = f"truck box - ring, median over {len(diffs)} frames (min {min(diffs, default=float('nan')):.1f})"
            if tod == "night":
                add(
                    checks,
                    "b",
                    d,
                    ">= +3 DN",
                    (not math.isnan(d)) and d >= WHITE_HOT_DN,
                    tod,
                    detail,
                )
            else:
                add(info, "b(noon)", d, time_=tod, detail=detail)
            if idx is not None:
                ann = vd.anns[idx]
                shots.append(
                    overlay(
                        out,
                        vd,
                        box_mask(vd.y.shape[1:], ann["bbox"]),
                        ring_mask(vd.y.shape[1:], ann["bbox"]),
                        ann,
                        idx,
                    )
                )
            # Edge shimmer on the truck box boundary (box-aligned crops).
            pairs = [(f, a["bbox"]) for f, a in zip(vd.y, vd.anns) if a is not None]
            crops = (
                aligned_crops(
                    np.stack([p[0] for p in pairs]).astype(np.float32),
                    [p[1] for p in pairs],
                )
                if len(pairs) >= 10
                else None
            )
            if crops is not None:
                stack, cbox = crops
                std = stack.std(axis=0)
                shape = std.shape
                inner = box_mask(shape, scale_box(cbox, 0.8))
                outer = box_mask(shape, scale_box(cbox, 1.2))
                boundary = outer & ~inner
                terrain = ~box_mask(shape, scale_box(cbox, 1.6))
                ratio = region_mean(std, boundary) / max(
                    region_mean(std, terrain), 1e-6
                )
                add(
                    info,
                    "truck boundary std ratio (motion-contaminated, not a shimmer metric)",
                    ratio,
                    time_=tod,
                    detail=f"box-boundary temporal std {region_mean(std, boundary):.2f} / terrain {region_mean(std, terrain):.2f} DN over {len(stack)} aligned frames (info only: truck motion against its box dominates, not held to the shimmer limit)",
                )

        # (c) coast + shimmer on the static coastline.
        vd = views.get((tod, "coast"))
        if vd is not None:
            water, land = coast_masks(vd.y.shape[1:])
            ds = [
                region_mean(f.astype(np.float32), water)
                - region_mean(f.astype(np.float32), land)
                for f in vd.y
            ]
            coast_d[tod] = med(ds)
            mean_img = vd.y.astype(np.float32).mean(axis=0)
            add(
                info,
                "c(" + tod + ")",
                coast_d[tod],
                time_=tod,
                detail=f"water {region_mean(mean_img, water):.1f} - land {region_mean(mean_img, land):.1f} (radius-matched)",
            )
            shots.append(overlay(out, vd, water, land, None, len(vd.y) // 2))
            sh = coast_shimmer(vd)
            if sh is not None:
                checks.append(shimmer_row(band, tod, sh))

        # (e) sky vs terrain.
        vd = views.get((tod, "sky"))
        if vd is not None:
            sky, ter = sky_masks(vd.y.shape[1:])
            mean_img = vd.y.astype(np.float32).mean(axis=0)
            s, t = region_mean(mean_img, sky), region_mean(mean_img, ter)
            add(
                checks,
                "e",
                s - t,
                "sky - terrain < 0",
                s < t,
                tod,
                f"sky (top 30 %) {s:.1f}, terrain (bottom 30 %) {t:.1f}",
            )
            shots.append(overlay(out, vd, sky, ter, None, len(vd.y) // 2))

        # Boat (info): boat box vs water ring.
        vd = views.get((tod, "nadir_boat"))
        if vd is not None:
            ds, idx = [], None
            for i, (f, ann) in enumerate(zip(vd.y, vd.anns)):
                if ann is None:
                    continue
                fy = f.astype(np.float32)
                ds.append(
                    region_mean(fy, box_mask(f.shape, ann["bbox"]))
                    - region_mean(fy, ring_mask(f.shape, ann["bbox"]))
                )
                idx = i
            add(
                info,
                "boat",
                med(ds),
                time_=tod,
                detail=f"boat box - water ring, median over {len(ds)} frames",
            )
            if idx is not None:
                ann = vd.anns[idx]
                shots.append(
                    overlay(
                        out,
                        vd,
                        box_mask(vd.y.shape[1:], ann["bbox"]),
                        ring_mask(vd.y.shape[1:], ann["bbox"]),
                        ann,
                        idx,
                    )
                )

    # (m) night nadir over the land-cover mix: no distinct peak at the window-grid fundamental.
    vd = views.get(("night", "nadir_mixed"))
    if vd is not None:
        period = grid_period_px(MIXED_UP_M, MIXED_FOV, vd.y.shape[2])
        ent = entity_mask(vd)
        mask = central_mask(vd.y.shape[1:]) & ~ent
        mean_img = vd.y.astype(np.float32).mean(axis=0)
        rx, ry = grid_peak_ratio(mean_img, mask, period)
        r = max(rx, ry)
        add(
            checks,
            "m",
            r,
            f"<= {GRID_PEAK_RATIO:g} (grid band / neighbours, max of x and y)",
            (not math.isnan(r)) and r <= GRID_PEAK_RATIO,
            "night",
            f"x {rx:.2f}, y {ry:.2f} at period {period:.1f} px (10 m at {MIXED_UP_M:g} m, {MIXED_FOV:g} deg); "
            f"{int(mask.sum())} px, entities masked ({int(ent.sum())} px)",
        )
        shots.append(overlay(out, vd, ~mask, None, None, len(vd.y) // 2))

    # (c) gate: sign flip.
    if "night" in coast_d and "noon" in coast_d:
        n, d = coast_d["night"], coast_d["noon"]
        ok = (n > 0 > d) or (n < 0 < d)
        add(
            checks,
            "c",
            d - n,
            "sign(noon) = -sign(night)",
            ok,
            "both",
            f"water - land: night {n:+.1f}, noon {d:+.1f} DN",
        )

    # (d) shadow at noon.
    vd = views.get(("noon", "oblique_truck"))
    if vd is not None:
        elev, az = sun_for(NOON)
        ds, idx, shift = [], None, None
        for i, (f, ann) in enumerate(zip(vd.y, vd.anns)):
            if not ann or not ann.get("box3d") or not ann["box3d"].get("corners_px"):
                continue
            sh, lit, s = shadow_masks(f.shape, ann, elev, az)
            if sh.sum() < MIN_PIXELS or lit.sum() < MIN_PIXELS:
                continue
            fy = f.astype(np.float32)
            ds.append(region_mean(fy, sh) - region_mean(fy, lit))
            idx, shift = i, s
        d = med(ds)
        add(
            checks,
            "d",
            d,
            "shadow - sunlit < 0",
            (not math.isnan(d)) and d < 0,
            "noon",
            f"median over {len(ds)} frames; sun elev {elev:.1f} az {az:.1f}; shift {shift} px",
        )
        if idx is not None:
            ann = vd.anns[idx]
            sh, lit, _ = shadow_masks(vd.y.shape[1:], ann, elev, az)
            shots.append(overlay(out, vd, sh, lit, ann, idx))


def entity_mask(vd: ViewData) -> np.ndarray:
    """Union of the view's COCO boxes (1.5x) over its frames: entities are not terrain."""
    m = np.zeros(vd.y.shape[1:], bool)
    for a in vd.anns:
        if a is not None:
            m |= box_mask(m.shape, scale_box(a["bbox"], 1.5))
    return m


def coast_shimmer(vd: ViewData) -> dict | None:
    """Temporal std on the coast view's edge pixels (top 2 % gradient below 0.4 H: the
    coastline, ridges) over the std of interior land pixels. Static pose, so only noise
    and sub-pixel edge motion (TSR jitter, class-edge shimmer) contribute."""
    if len(vd.y) < 10:
        return None
    ys = vd.y.astype(np.float32)
    std, mean_img = ys.std(axis=0), ys.mean(axis=0)
    h = vd.y.shape[1]
    roi = np.zeros(vd.y.shape[1:], bool)
    roi[int(0.40 * h) :, :] = True
    edge, interior = edge_masks(mean_img, roi)
    _, land = coast_masks(vd.y.shape[1:])
    interior &= land | (np.arange(h)[:, None] >= COAST_LAND_ROW0 * h)
    e, i = region_mean(std, edge), region_mean(std, interior)
    raw, floored = shimmer_ratio(e, i)
    return {
        "value": floored,
        "raw": raw,
        "detail": f"floored: edge temporal std {e:.2f} / max(interior land {i:.2f}, {SNAPSHOT_QUANT_STD_DN:.3f}) DN; raw ratio {raw:.2f}; over {len(vd.y)} frames, static pose",
    }


def thermal_log_lines(log: Path) -> list[str]:
    if not log.exists():
        return []
    return [
        ln.split("LogCamSim: ", 1)[-1]
        for ln in log.read_text(errors="replace").splitlines()
        if "LogCamSim: Thermal:" in ln
    ]


def check_all(
    out: Path, results: dict[str, dict], bands: list[str]
) -> tuple[list, list, list]:
    checks: list[dict] = []
    info: list[dict] = []
    shots: list[str] = []
    for band in bands:
        run = results.get(band)
        if not run:
            continue
        views = {}
        for rec in run["views"]:
            vd = load_view(out, run, rec)
            if vd is not None:
                views[(rec["time"], rec["view"])] = vd
                shots.append(rec["shot"])
        check_band(out, band, views, checks, info, shots)
        rows = load_rows(out / band / "frames.jsonl")
        th = [r["thermal_gpu_ms"] for r in rows if r.get("thermal_gpu_ms", -1) > 0]
        info.append(
            {
                "check": "thermal_gpu_ms 720p",
                "band": band,
                "time": "all",
                "value": percentile(th, 95),
                "detail": f"p95 over {len(th)} IR frames (median {percentile(th, 50):.3f}, max {max(th, default=float('nan')):.3f})",
            }
        )
        lines = thermal_log_lines(out / band / "CamSimTest.log")
        seen: list[str] = []
        for ln in lines:
            key = ln.split(" gainEv=")[0]
            if key not in seen:
                seen.append(key)
        for s in seen[-4:]:
            info.append(
                {
                    "check": "builder",
                    "band": band,
                    "time": "-",
                    "value": "-",
                    "detail": s,
                }
            )

    # (f) ThermalCS at 1080p.
    run = results.get("mwir_1080p")
    if run:
        w = run["views"][0]
        rows = [
            r
            for r in load_rows(out / "mwir_1080p" / "frames.jsonl")
            if w["t0"] - 12.0 <= r["t"] <= w["t1"]
        ]
        th = [r["thermal_gpu_ms"] for r in rows if r.get("thermal_gpu_ms", -1) > 0]
        p95 = percentile(th, 95)
        checks.append(
            {
                "check": "f",
                "band": "mwir",
                "time": "noon",
                "value": p95,
                "threshold": "<= 0.5 ms",
                "pass": bool(th) and p95 <= THERMAL_P95_MS,
                "detail": f"1920x1080, {len(th)} frames, median {percentile(th, 50):.3f}, max {max(th, default=float('nan')):.3f} ms",
            }
        )
        shots.append(w["shot"])

    # (g) EO untouched by thermal.
    eo = {}
    for label in ("eo_thermal_on", "eo_thermal_off"):
        run = results.get(label)
        if not run:
            continue
        rec = run["views"][0]
        vd = load_view(out, run, rec)
        if vd is None:
            continue
        for crec in run["views"][1:]:
            cvd = load_view(out, run, crec) if crec["view"] == "coast" else None
            sh = coast_shimmer(cvd) if cvd is not None else None
            if sh is not None:
                info.append(
                    {
                        "check": "shimmer(coast) EO baseline",
                        "band": "eo",
                        "time": "noon",
                        **sh,
                    }
                )
                shots.append(crec["shot"])
        rows = [
            r
            for r in load_rows(out / label / "frames.jsonl")
            if rec["t0"] - 12.0 <= r["t"] <= rec["t1"] and not r.get("cut")
        ]
        eo[label] = (
            float(vd.y.astype(np.float32).mean()),
            med([r["wall_ms"] for r in rows]),
            med([r["sensor_gpu_ms"] for r in rows]),
            max((r.get("thermal_gpu_ms", -1) for r in rows), default=-1),
        )
        shots.append(rec["shot"])
    if len(eo) == 2:
        (y_on, w_on, s_on, t_on), (y_off, w_off, s_off, t_off) = (
            eo["eo_thermal_on"],
            eo["eo_thermal_off"],
        )
        checks.append(
            {
                "check": "g",
                "band": "eo",
                "time": "noon",
                "value": y_on - y_off,
                "threshold": "|dY| <= 1 DN",
                "pass": abs(y_on - y_off) <= EO_DY_DN,
                "detail": f"mean Y thermal on {y_on:.2f} / off {y_off:.2f}",
            }
        )
        info.append(
            {
                "check": "g frame time",
                "band": "eo",
                "time": "noon",
                "value": w_on - w_off,
                "detail": f"wall_ms median on {w_on:.2f} / off {w_off:.2f}; sensor_gpu_ms on {s_on:.3f} / off {s_off:.3f}; max thermal_gpu_ms on {t_on} / off {t_off} (-1 = ThermalCS never ran)",
            }
        )
    return checks, info, shots


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--band", choices=["mwir", "lwir", "both"], default="both")
    ap.add_argument("--out", type=Path, default=None)
    ap.add_argument(
        "--runs", default="bands,hd,eo", help="comma list of: bands, hd, eo"
    )
    ap.add_argument(
        "--check-only", action="store_true", help="re-run the checks on --out"
    )
    a = ap.parse_args()
    out = (
        a.out or REPO / ".cache" / "thermal_check" / time.strftime("%Y%m%d-%H%M%S")
    ).resolve()
    for d in ("shots", "overlays"):
        (out / d).mkdir(parents=True, exist_ok=True)
    bands = ["mwir", "lwir"] if a.band == "both" else [a.band]
    wanted = set(filter(None, a.runs.split(",")))

    results: dict[str, dict] = {}
    for spec in build_runs(bands, {"bands", "hd", "eo"}):
        if spec.label not in bands and spec.label in BANDS:
            continue
        group = (
            "bands"
            if spec.label in BANDS
            else ("hd" if spec.label == "mwir_1080p" else "eo")
        )
        if group in wanted and not a.check_only:
            results[spec.label] = run_once(spec, out)
        elif (out / spec.label / "run.json").exists():
            results[spec.label] = json.loads(
                (out / spec.label / "run.json").read_text()
            )

    checks, info, shots = check_all(out, results, bands)
    sha = subprocess.run(
        ["git", "rev-parse", "--short", "HEAD"],
        capture_output=True,
        text=True,
        cwd=REPO,
        check=False,
    ).stdout.strip()
    meta = {
        "git": sha,
        "bands": ",".join(bands),
        "out": str(out),
        "date": "2026-12-21 (sim)",
        "sun_noon": "elev {:.1f} az {:.1f}".format(*sun_for(NOON)),
    }
    expected = expected_rows(bands, wanted)
    report = build_report(meta, checks, info, shots, expected)
    (out / "report.json").write_text(json.dumps(report, indent=2))
    (out / "report.md").write_text(render_markdown(report))
    for c in checks:
        print(
            f"[{c['check']}] {'PASS' if c['pass'] else 'FAIL'} {c['band']} {c['time']}: {c['value']:.3f} ({c['threshold']}) {c['detail']}"
        )
    for c in info:
        v = c["value"]
        print(
            f"[info {c['check']}] {c['band']} {c['time']}: {v if isinstance(v, str) else f'{v:.3f}'} {c['detail']}"
        )
    for check, band, tod in report["missing"]:
        print(f"[{check}] FAIL {band} {tod}: missing (no data for this row)")
    print(f"Report: {out / 'report.md'}; {'PASS' if report['passed'] else 'FAIL'}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
