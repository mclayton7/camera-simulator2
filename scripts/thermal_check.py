#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = ["numpy", "pillow"]
# ///
"""Thermal IR acceptance check (ROADMAP 4A, 4B gates i-m, 4C gates n-q, REALISM R2 gate r): MWIR / LWIR at noon and
02:00 with the DIS truck + boat.

Usage: uv run scripts/thermal_check.py [--band mwir|lwir|both] [--out DIR]
                                       [--runs bands,hd,eo,lcoff,pan,entity,roads] [--scene PKG] [--check-only]
  caffeinate -ims uv run scripts/thermal_check.py --band both
  (caffeinate: a host that sleeps mid-run freezes CamSim and spoils the frame times)

21 December 2026 at the Presidio, San Francisco: solar noon = 20:10 UTC (sun elevation
28.8 deg), 02:00 local = 10:10 UTC. Every launch is headless (macOS or Linux, local build, unicast to
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
           nadir_mixed   (ROADMAP 4B) nadir over the Presidio land-cover mix
                         (37.7935, -122.4600), 800 m up, 40 deg FOV (10)
           mixed_eo      (noon only) the nadir_mixed pose seen by the EO sensor (sensor 0,
                         3 frames): the greenness masks for (i) and (j)
  hd     mwir_cooled at 1920x1080 (CAMSIM_CAPTURE_WIDTH/HEIGHT), noon, nadir_truck for 20 s
  lcoff  one per band, CAMSIM_THERMAL_LAND_COVER_ENABLED=0 (4A thermal), night, nadir_mixed:
         the baseline for (k)
  pan    mwir_cooled, noon, CAMSIM_THERMAL_LAND_COVER_RECENTRE_FRACTION=0.02 (409.6 m), nadir
         600 m up over MIXED_CENTER panning east 1.2 km and back at 1 km/min for 220 s (no
         snapshots: frame stats only), so the land-cover window re-centres several times;
         then the same pan with CAMSIM_THERMAL_LAND_COVER_ENABLED=0 (mwir_pan_lcoff), the
         frame-time control for (l2)
  eo     two EO launches (sensor 0), noon, static nadir over the truck loop: thermal default
         (enabled) and CAMSIM_THERMAL_ENABLED=0; the first also takes the coast view as the
         EO baseline for the coastline shimmer ratio
  entity (ROADMAP 4C) per band, night, nadir 120 m over the truck, 20 deg FOV (10 frames each):
           <band>_ent_park  send_dis_test.py truck-park --drive-s 60: ent_running while it drives
                            (power plant on), then ent_park5 / ent_park600 where it stopped, 5 s and
                            600 s after it stopped with the power plant off
           <band>_ent_cold  truck-parked --hold-s 60: ent_parked, stationary since spawn on the spot where
                            the park run stops (same background), power plant off
         and mwir_ent_dead (truck-destroyed --hold-s 60: ent_dead, damage destroyed + flaming)
  roads  (REALISM R2, not in the default; needs --scene, the Camp Pendleton package, as CAMSIM_SCENE_DIR) per band,
         night then noon, no DIS vehicles: road, nadir 300 m over I-5 NB San Clemente (OSM way 487165554, code 200);
         road_t9 (info), the same over I-5 SB (way 1009956653); 20 deg FOV, heading along the road (10 frames each)
  roofs  (REALISM R3b, not in the default; needs --scene, the Pendleton package built with R3b) MWIR, night then
         noon, no DIS vehicles: roof, nadir 460 m over Camp Pendleton Mainside (ROOF_POSE), 20 deg FOV (10 frames)

Each frame is a /snapshot PNG (the NV12 frame converted to RGB); Y is recovered exactly for
IR (grey: U = V = 128) and as BT.709 luma for EO. The middle frame of each view is saved to
OUT/shots/, a region overlay to OUT/overlays/ (red / cyan = the two compared regions, green =
the COCO box), all frames to OUT/<run>/frames/<time>_<view>.npz.

Checks (spec "Testing"). Exit 0 only when every expected (check, band, time) row exists and
passes: a, b, c, d, e, h, i, j per selected band when `bands` is in --runs, k per band when
`bands` and `lcoff` are, l1 and l2 when `pan` is, f when `hd` is, g when `eo` is (so `--runs
bands` alone can exit 0; f, g, k, l1, l2 are then not expected), n, o, p per band and q (MWIR)
when `entity` is, r per band at noon and night when `roads` is, s (noon) and s_air (night) for MWIR when `roofs` is. A missing
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
  (i) noon nadir_mixed (ROADMAP 4B): non-vegetation - vegetation mean Y >= +3 DN (white-hot:
      vegetation cooler). Masks from the noon mixed_eo frame: excess green (2g - r - b) / (r + g
      + b) > 0.06 = vegetation, < 0.02 = non-vegetation ground (minus water-blue and Y < 30 DN
      shadow), central 60 %, eroded 2 px, entity boxes removed; IR temporal mean of 10 frames
  (j) night nadir_mixed: the same with the same (noon EO) masks, >= +2 DN
  (k) night nadir_mixed: visible class structure (ruling S12). Temporal mean, 3 x 3 median
      (defect pixels), band-passed to ground scales 15-150 m (box(15 m / GSD) - box(150 m /
      GSD), GSD from 800 m and 40 deg), spatial std over the central 60 % with entities masked:
      land cover on >= 3 x the lcoff launch's at the same pose. The 33 px high-pass std and its
      3 x 3-median variant are info rows (detector noise dominates them on the flat 4A frame)
  (l1) pan: per land-cover window re-centre (window id change in frame stats), max wall_ms
      within 15 frames minus the pan's median wall_ms; the median over re-centres <= 2 ms,
      with >= 3 re-centres (the raw max is an info row)
  (l2) pan: per re-centre, max wall_ms within 15 frames minus the max wall_ms of the
      land-cover-off control pan over the same view-time span; the max <= 5 ms
  (m) INFO ONLY (not gated): night nadir_mixed (ROADMAP 4B Task 13) land-cover window grid. On the
      temporal-mean frame (central 60 %, entity boxes masked), the 2D power spectrum's mean in a
      band around the grid fundamental (period 10 m / GSD px, GSD from 800 m and 40 deg; along
      each image axis, |f - f0| <= 0.15 f0) over the mean of the two neighbouring bands of equal
      width (~1 = no peak). It was gated <= 2, but cannot tell a visibly blocky frame from a fixed
      one (random block fields have sinc zeros at the fundamental), so it is reported only
  (n) entity, night: running truck (box mean - ring mean) - parked-cold truck (box - ring) >= +5 DN.
      Every entity statistic is the median over the frames with a truck COCO box; ring-referenced
      because the runs' AGC states differ
  (o) entity, night: running truck box p99 - box median >= +20 DN MWIR, +10 DN LWIR (hot parts)
  (p) entity, night: box mean - ring after parking: at +600 s below +5 s, and above the parked-cold
      truck's (cooling, still warm). The mean, not p99: in MWIR the hot parts clip at the display
      ceiling (Y 235), so p99 cannot show them cooling
  (q) entity, night, MWIR: destroyed truck box - ring >= +60 DN (burning)
  (r) roads (REALISM R2): carriageway - outer verge mean Y on the temporal-mean frame >= +6 DN at noon, >= +2 DN at
      night. Pose: I-5 northbound in San Clemente (OSM way 487165554, asphalt, dark in NAIP; selection criterion at
      ROAD_POSE). Carriageway = the 20.5 m road minus 2 m at each edge, verge = 8-28 m beyond the edge on the outer
      side (right of travel = image right), both over the central 60 % rows, GSD from 300 m and 20 deg. r_info:
      asphalt and verge-class temperatures from the camsim.Thermal.Log lines. r_t9 (info, not gated): the same metric
      at the first pose (I-5 SB, way 1009956653), where OSM says asphalt but NAIP shows light, concrete-like pavement
  (s) roofs (REALISM R3b): building roofs - grassland mean Y on the temporal-mean frame >= +3 DN at noon (night value
      info, s_night). Roof = the package's footprint mask eroded 1.5 m; grass = land-cover 30 at least 10 m from any
      footprint; central 60 %, each region >= 2 % of it. (s_air) night: the building_roof class temperature
      (camsim.Thermal.Log) <= T_air - 0.5 K
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

# UE on macOS logs to ~/Library/Logs/<Project>; elsewhere to the project Saved/Logs (scripts/run.sh)
EDITOR_LOG = (Path.home() / "Library/Logs/CamSimTest/CamSimTest.log" if sys.platform == "darwin"
              else REPO / "unreal_project/CamSimTest/Saved/Logs/CamSimTest.log")
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
GRID_PEAK_RATIO = 2.0  # (m, info only) fundamental band / neighbouring bands; the former gate

# ROADMAP 4B land-cover gates (spec "Testing", i-l). MIXED_CENTER / MIXED_UP_M / MIXED_FOV / CENTRAL above.
VEG_NOON_DN = 3.0  # (i) noon: non-vegetation - vegetation >= +3 DN (white-hot: vegetation cooler)
VEG_NIGHT_DN = 2.0  # (j) night: non-vegetation (built-up / bare) - vegetation >= +2 DN
STRUCTURE_RATIO = 3.0  # (k) night class-scale band-pass std, land cover on / off (ruling S12)
STRUCTURE_LO_M, STRUCTURE_HI_M = 15.0, 150.0  # (k) band: ground scales of land-cover regions
RECENTRE_MEDIAN_MS = 2.0  # (l1) median over re-centres of (max wall_ms near it - pan median)
RECENTRE_EXCESS_MS = 5.0  # (l2) max over re-centres of (max wall_ms near it - control max at the same view time)
MIN_RECENTRES = 3
RECENTRE_RADIUS = 15  # frames either side of a re-centre
EXG_VEG = 0.06  # EO display RGB excess green above this: vegetation
EXG_NONVEG = 0.02  # ... below this: non-vegetation ground (neutral grey has ExG = 0)
HIGHPASS_K = 33
PAN_UP_M = 600.0
PAN_LEG_M = 1200.0
PAN_SPEED_MPS = 1000.0 / 60.0  # the spec's 1 km/min
PAN_HOLD_S = 220.0  # > 1.5 triangle periods: >= 4 re-centres whatever the pan phase at the view start (150 s gave 3)
PAN_RECENTRE_FRACTION = "0.02"  # 409.6 m: re-centres every ~25 s at 1 km/min (the default 25 % needs > 5 min)

# ROADMAP 4C entity thermal state, gates (n)-(q) (spec "Testing"). Ring-referenced: box statistic minus the ring mean.
ENT_UP_M = 120.0  # nadir height above the truck ground for the entity views
ENT_FOV = 20.0
ENT_DRIVE_S = 60.0  # truck-park: seconds of driving before it stops (power plant off)
ENT_RUN_DN = 5.0  # (n) running - parked-cold contrast
ENT_HOT_DN = {"mwir": 20.0, "lwir": 10.0}  # (o) running p99 - box median
ENT_DEAD_DN = 60.0  # (q) destroyed box - ring (MWIR)

# REALISM R2 gate (r): a road against its verge on the Camp Pendleton scene package (--scene).
# Pose selection (task 10 fix round 1, reproducible from the package; numbers measured on pendleton-r2):
#   candidates = straight pieces of OSM ways (motorway..tertiary, no links) that burn to code 200 (surface=asphalt or
#   none), consecutive vertices whose bearings stay within 1 deg of the piece's end-to-end bearing, >= 120 m long,
#   width_m >= 8 m, centre inside the bbox; no other road/rail way crossing within 60 m of the centre and no bridge or
#   tunnel within 60 m; land cover over +-18 m along: carriageway strip (|d| <= w/2 - 2 m) >= 90 % code 200, verge strip
#   (w/2 + 8 .. w/2 + 28 m on one side) with no 200/201/210/220 and >= 60 % codes 10/20/30/40/60; imagery (z17 NAIP,
#   sRGB luma 0-255 sampled at 2 m x 1 m over the same strips): verge mean - carriageway mean >= 15 DN ("dark
#   pavement"). 282 pieces pass the land-cover tests, 30 of them the luma test; ranked by road class (motorway first),
#   then width, then the luma difference. The first: I-5 northbound (San Diego Freeway) in San Clemente, OSM way
#   487165554 (motorway, ref I 5, oneway, lanes 5, surface=asphalt -> code 200, width_m 20.5 m), a 139 m straight piece
#   at bearing 337.74 deg; pose at its midpoint. Carriageway strip 98.8 % code 200; verge (right of travel, d 18.25 ..
#   38.25 m: dry scrub with bushes on a rising bank) 389 tree cover (10) + 10 built-up (50) samples; luma carriageway
#   75.3, verge 97.9 (+22.6 DN). Land cover across the profile, d = -60..+60 m in 4 m steps, along 0 m:
#   50 50 50 50 50 50 50 200 200 200 200 50 50 | 200 200 200 200 | 50 50 10 10 10 10 10 10 10 10 10 10 10 10
#   (d = -32..-20: the southbound carriageway). Ground 33.422644 N -117.607739 E: 47.76 m HAE (package terrain,
#   point_heights z16); verge midpoint 56.74 m (the bank). Second-ranked was CA 76 (trunk, way 215015826, +27.5 DN).
#   Candidate list: .cache/r2/gate_r_pose_candidates_loose.json, script scripts/scene/tools/select_road_pose.py
#   (args PKG OUT 8 120 0.6; its docstring records the rule and its relaxation, 10 m / 200 m / 80 % -> 8 m / 120 m / 60 %,
#   both changes made before any gate run).
ROAD_POSE = dict(  # I-5 NB, San Clemente
    lat=33.422644,
    lon=-117.607739,
    ground_hae=47.76,
    heading=337.74,
    width_m=20.5,
    outer_side=+1,
    osm_way=487165554,
    verge_class="tree_canopy",
)
# r_t9 (info only): the first gate (r) pose (task 9). I-5 southbound, OSM way
# 1009956653 (motorway, ref I 5, oneway, lanes 4, no surface tag -> code 200; width_m 17.0 m), one 408 m segment at a
# constant bearing of 134.71 deg inside a 1318 m run of chained southbound ways whose bearings stay within 0.55 deg; the
# pose is 80 % along the way (326 m in, 82 m before its end). The whole southbound I-5 in the package's bbox is tagged
# surface=concrete (code 201) except this way. Outer side +1 (right of travel, the coast side, away from the median).
# Package land cover (pendleton-r2, 10 m codes) across the profile, d = metres right of travel, -60..+60 in 4 m steps
# (| | marks d = -4..+8):
#   along -20 m: 30 30 30 30 30 50 201 201 201 201 201 50 50 50 | 200 200 200 200 | 30 30 30 30 50 50 50 200 200 200 50 50 50
#   along   0 m: 30 30 30 60 60 50 50 201 201 201 201 50 50 200 | 200 200 50 50 | 50 30 30 30 30 30 30 30 50 200 200 200 50
#   along +20 m: 30 30 60 60 60 50 50 50 201 201 201 201 50 200 | 200 200 200 200 | 50 50 50 30 30 30 30 30 30 50 200 200 200
# (the 200 at d = +40..+56 is the next interchange's ramp diverging ahead). Over +-18 m along at 2 m x 1 m steps: road
# strip (|d| <= 6.5 m) 236 texel samples 200, 30 built-up (50, 10 m texels at the edges); verge strip (d 16.5..36.5 m)
# 364 grassland (30), 35 built-up, none 200/201/210. NAIP: travel lanes from d ~ -7.5 to +15 m, vegetated verge to the
# ramp at d ~ +40 m. Ground 33.299358 N -117.465474 E: -8.23 m HAE (package terrain, point_heights z16; outer verge
# mid -10.1 m). Picked with read_osm features (camsim_scene.pipeline._load_features) in a scratch script.
# NAIP shows this pavement light grey (concrete-like) although OSM tags it asphalt, so the per-pixel fast term reads it
# as a high albedo: the OSM/imagery disagreement case, kept as an info row.
ROAD_POSE_T9 = dict(  # I-5 SB, Pendleton
    lat=33.299358,
    lon=-117.465474,
    ground_hae=-8.23,
    heading=134.71,
    width_m=17.0,
    outer_side=+1,
    osm_way=1009956653,
    verge_class="grassland",
)
ROAD_UP_M = 300.0
ROAD_FOV = 20.0
ROAD_NOON_DN = 6.0  # (r) noon: carriageway - outer verge >= +6 DN (white-hot)
ROAD_NIGHT_DN = 2.0  # (r) night: >= +2 DN (asphalt keeps its heat)
ROAD_VERGE_M = (8.0, 28.0)  # verge strip: this far beyond the carriageway edge
ROAD_EDGE_INSET_M = 2.0  # road strip: carriageway minus this at each edge
ROAD_CLASS_SLOTS = {"asphalt": 3, "tree_canopy": 6, "grassland": 8, "bare_soil": 11}  # FThermalMaterialTable indices (r_info)

# (s) REALISM R3b: roofs vs grass at noon (image) and the building_roof class vs air at 02:00 (class model). Pose picked
# while writing the R3b plan on pendleton-r3: within a 160 m square view, 34 % of the ground under Overture footprints and
# 26 % WorldCover grassland (30), the best min(building, grass) of a 0.0025 deg scan over 33.28-33.33 N, 117.28-117.34 W.
# Ground 74.88 m HAE (package terrain, layers.buildings.heights_fn at the centre).
ROOF_POSE = {"lat": 33.3000, "lon": -117.3175, "ground_hae": 74.88}
ROOF_UP_M = 460.0  # 20 deg FOV: a 162 m wide view
ROOF_FOV = 20.0
ROOF_NOON_DN = 3.0  # (s) noon: roof - grass mean Y >= +3 DN (white-hot)
ROOF_AIR_K = 0.5  # (s_air) 02:00: T_air - T(building_roof) >= 0.5 K
ROOF_ERODE_M = 1.5  # roof pixels this far inside a footprint (mask registration, PSF)
ROOF_GRASS_CLEAR_M = 10.0  # grass pixels this far from any footprint
ROOF_MIN_FRAC = 0.02  # each region >= 2 % of the central 60 %
ROOF_CLASS_SLOTS = {"building_roof": 15, "grassland": 8}  # FThermalMaterialTable indices
ROOF_M_PER_DEG = 111_320.0

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


def road_masks(
    shape: tuple[int, int], gsd_m: float, road_w_m: float, verge_side: int
) -> tuple[np.ndarray, np.ndarray]:
    """(r) Nadir, heading along the road (the road a vertical strip through the image centre): the carriageway minus
    ROAD_EDGE_INSET_M at each edge, exactly round((road_w - 2 inset) / gsd) columns centred on the image, and the verge
    strip ROAD_VERGE_M beyond the edge on `verge_side` (+1 = right of travel = image right). Central 60 % rows only."""
    h, w = shape
    cx = (w - 1) / 2.0
    x = (np.arange(w) - cx) * gsd_m
    half = road_w_m / 2.0
    n = max(0, round((road_w_m - 2.0 * ROAD_EDGE_INSET_M) / gsd_m))
    c0 = math.floor(cx - (n - 1) / 2.0 + 0.5) if n else 0  # the n columns nearest the centre (ties to the right)
    road_cols = np.zeros(w, bool)
    road_cols[max(0, c0) : max(0, c0 + n)] = True
    d = x * verge_side
    verge_cols = (d >= half + ROAD_VERGE_M[0]) & (d <= half + ROAD_VERGE_M[1])
    rows = central_mask(shape).any(axis=1)
    return rows[:, None] & road_cols[None, :], rows[:, None] & verge_cols[None, :]


def road_row(band: str, tod: str, y: np.ndarray, masks: tuple[np.ndarray, np.ndarray], threshold: float) -> dict:
    """(r) carriageway - outer verge mean Y >= threshold (NaN, an empty region, fails)."""
    road, verge = masks
    v = region_mean(y, road) - region_mean(y, verge)
    return {
        "check": "r",
        "band": band,
        "time": tod,
        "value": round(v, 2),
        "threshold": threshold,
        "pass": bool(v >= threshold),
        "detail": f"carriageway - outer verge mean Y, {int(road.sum())}/{int(verge.sum())} px",
    }


def _thermal_log_classes(ln: str) -> tuple[list[float], str, float] | None:
    """One camsim.Thermal.Log line -> (class temperatures K by slot, "noon" if sunLux > 0 else "night", T_air K; NaN when
    the line has no Tair). None when it is not a class line or does not parse."""
    if "classes[K]" not in ln or " sunLux=" not in ln:
        return None
    try:
        vals = [float(t) for t in ln.split("classes[K]", 1)[1].split(" terrain=", 1)[0].split()]
        lux = float(ln.split(" sunLux=", 1)[1].split()[0])
        tair = float(ln.split(" Tair=", 1)[1].split()[0]) if " Tair=" in ln else float("nan")
    except ValueError:
        return None
    return vals, "noon" if lux > 0 else "night", tair


def _class_k(vals: list[float], slot: int) -> float:
    """A class temperature by slot; NaN when the line doesn't carry it (a log from before every class was printed)."""
    return vals[slot] if slot < len(vals) else float("nan")


def road_class_temps(lines: list[str], verge: str = "grassland") -> dict[str, tuple[float, float]]:
    """(r_info) asphalt and `verge` class temperatures (K) from the last camsim.Thermal.Log line of each time of day
    (sunLux > 0: noon, else night). A slot the line doesn't carry (a log from before every class was printed) is NaN."""
    out: dict[str, tuple[float, float]] = {}
    for ln in lines:
        parsed = _thermal_log_classes(ln)
        if parsed is not None:
            vals, tod, _ = parsed
            out[tod] = (_class_k(vals, ROAD_CLASS_SLOTS["asphalt"]), _class_k(vals, ROAD_CLASS_SLOTS[verge]))
    return out


def roof_air_temps(lines: list[str]) -> dict[str, tuple[float, float, float]]:
    """(s_air) (building_roof K, grassland K, T_air K) from the last camsim.Thermal.Log line of each time of day."""
    out: dict[str, tuple[float, float, float]] = {}
    for ln in lines:
        parsed = _thermal_log_classes(ln)
        if parsed is not None and not math.isnan(parsed[2]):
            vals, tod, tair = parsed
            roof, grass = (_class_k(vals, ROOF_CLASS_SLOTS[c]) for c in ("building_roof", "grassland"))
            out[tod] = (roof, grass, tair)
    return out


def _dilate(m: np.ndarray, r: int) -> np.ndarray:
    """Square binary dilation by r pixels."""
    if r <= 0:
        return m.copy()
    p = np.pad(m, r)
    rows = np.lib.stride_tricks.sliding_window_view(p, 2 * r + 1, axis=0).any(axis=-1)
    return np.lib.stride_tricks.sliding_window_view(rows, 2 * r + 1, axis=1).any(axis=-1)


def roof_masks(
    shape: tuple[int, int], gsd_m: float, lat0: float, lon0: float, masked, codes
) -> tuple[np.ndarray, np.ndarray]:
    """(s) Nadir, heading 0 (image up = north, right = east): roof = the footprint mask eroded by ROOF_ERODE_M; grass =
    land-cover 30 at least ROOF_GRASS_CLEAR_M from any footprint; both within the central 60 %. masked(lon, lat) and
    codes(lon, lat) read the package (bench.dense_path.BuildingMask / LandCover)."""
    h, w = shape
    e, n = np.meshgrid((np.arange(w) - (w - 1) / 2.0) * gsd_m, ((h - 1) / 2.0 - np.arange(h)) * gsd_m)
    lat = lat0 + n / ROOF_M_PER_DEG
    lon = lon0 + e / (ROOF_M_PER_DEG * math.cos(math.radians(lat0)))
    b = np.asarray(masked(lon, lat), bool)
    centre = central_mask(shape)
    roof = ~_dilate(~b, max(1, round(ROOF_ERODE_M / gsd_m))) & centre
    grass = (np.asarray(codes(lon, lat)) == 30) & ~_dilate(b, max(1, round(ROOF_GRASS_CLEAR_M / gsd_m))) & centre
    return roof, grass


def roof_row(band: str, tod: str, y: np.ndarray, masks: tuple[np.ndarray, np.ndarray], threshold: float) -> dict:
    """(s) roof - grass mean Y >= threshold, with each region >= ROOF_MIN_FRAC of the central 60 % (else it fails)."""
    roof, grass = masks
    n_min = ROOF_MIN_FRAC * central_mask(y.shape).sum()
    v = region_mean(y, roof) - region_mean(y, grass)
    enough = roof.sum() >= n_min and grass.sum() >= n_min
    return {
        "check": "s",
        "band": band,
        "time": tod,
        "value": round(v, 2),
        "threshold": threshold,
        "pass": bool(enough and v >= threshold),
        "detail": f"roof - grass mean Y, {int(roof.sum())}/{int(grass.sum())} px"
        + ("" if enough else f" (a region under {ROOF_MIN_FRAC:.0%} of the view: pick another ROOF_POSE)"),
    }


def nadir_gsd_m(up_m: float, fov_deg: float, width_px: int) -> float:
    """Ground sample distance (m / px) of a nadir view at the image centre."""
    return 2.0 * up_m * math.tan(math.radians(fov_deg / 2.0)) / width_px


def grid_period_px(
    up_m: float, fov_deg: float, width_px: int, texel_m: float = GRID_TEXEL_M
) -> float:
    """Image period of the land-cover window grid in a nadir view: texel / GSD."""
    return texel_m / nadir_gsd_m(up_m, fov_deg, width_px)


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


def exg(rgb: np.ndarray) -> np.ndarray:
    """Excess green of a display RGB image in chromatic coordinates: (2g - r - b) / (r + g + b)."""
    f = rgb.astype(np.float32) / 255.0
    return (2.0 * f[..., 1] - f[..., 0] - f[..., 2]) / (f.sum(axis=-1) + 1e-6)


def erode(mask: np.ndarray, r: int) -> np.ndarray:
    """Binary erosion by a (2r + 1)^2 square; pixels within r of the border are cleared."""
    if r <= 0:
        return mask.copy()
    h, w = mask.shape
    inner = np.ones((h - 2 * r, w - 2 * r), bool)
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            inner &= mask[r + dy : h - r + dy, r + dx : w - r + dx]
    out = np.zeros_like(mask)
    out[r : h - r, r : w - r] = inner
    return out


def greenness_masks(rgb: np.ndarray, erode_px: int = 2) -> tuple[np.ndarray, np.ndarray]:
    """(vegetation, non-vegetation ground) from an EO frame: ExG > EXG_VEG / ExG < EXG_NONVEG; non-vegetation excludes
    water-blue (b > g, b > r) and deep shadow (Y < 30 DN). Central region only, eroded (EO/IR distortion differ)."""
    e = exg(rgb)
    f = rgb.astype(np.float32)
    blue = (f[..., 2] > f[..., 1]) & (f[..., 2] > f[..., 0])
    c = central_mask(rgb.shape[:2])
    veg = erode((e > EXG_VEG) & c, erode_px)
    non = erode((e < EXG_NONVEG) & ~blue & (y_from_rgb(rgb) >= 30.0) & c, erode_px)
    return veg, non


def veg_contrast(y: np.ndarray, veg: np.ndarray, non: np.ndarray) -> float:
    """Non-vegetation minus vegetation mean Y (white-hot: > 0 = vegetation cooler); nan if a region is too small."""
    if veg.sum() < MIN_PIXELS or non.sum() < MIN_PIXELS:
        return float("nan")
    return region_mean(y, non) - region_mean(y, veg)


def box_blur(y: np.ndarray, k: int) -> np.ndarray:
    """k x k box mean (k odd), edges replicated, through an integral image."""
    r = k // 2
    h, w = y.shape
    p = np.pad(y.astype(np.float64), r, mode="edge")
    s = np.pad(p.cumsum(0).cumsum(1), ((1, 0), (1, 0)))
    tot = s[k : k + h, k : k + w] - s[0:h, k : k + w] - s[k : k + h, 0:w] + s[0:h, 0:w]
    return (tot / (k * k)).astype(np.float32)


def median3(y: np.ndarray) -> np.ndarray:
    """3 x 3 median (edges replicated): removes isolated defect pixels."""
    h, w = y.shape
    p = np.pad(y.astype(np.float32), 1, mode="edge")
    return np.median(np.stack([p[dy : dy + h, dx : dx + w] for dy in range(3) for dx in range(3)]), axis=0)


def highpass_std(y: np.ndarray, mask: np.ndarray, k: int = HIGHPASS_K) -> float:
    """Spatial std of Y minus its k x k box mean over mask: structure, not vignetting or AGC level."""
    if mask.sum() < MIN_PIXELS:
        return float("nan")
    hp = y.astype(np.float32) - box_blur(y, k)
    return float(hp[mask].std())


def band_px(gsd_m: float, lo_m: float = STRUCTURE_LO_M, hi_m: float = STRUCTURE_HI_M) -> tuple[int, int]:
    """(lo, hi) odd box sizes in px for a band-pass of ground scales lo_m..hi_m at gsd_m."""
    return int(round(lo_m / gsd_m)) | 1, int(round(hi_m / gsd_m)) | 1


def class_structure_std(y: np.ndarray, mask: np.ndarray, lo_px: int, hi_px: int) -> float:
    """Gate (k): spatial std over mask of a 3 x 3 median (defect pixels out) band-passed between box sizes lo_px and
    hi_px (box(lo) - box(hi)): land-cover-scale structure, without pixel noise / FPN (below lo) or vignetting (above
    hi). nan for a small mask."""
    if mask.sum() < MIN_PIXELS:
        return float("nan")
    m = median3(y)
    return float((box_blur(m, lo_px) - box_blur(m, hi_px))[mask].std())


def pan_offset_m(t: float, leg_m: float = PAN_LEG_M, speed: float = PAN_SPEED_MPS) -> float:
    """East offset of the gate (l) pan: a triangle wave 0 -> leg -> 0 at `speed` m/s."""
    period = 2.0 * leg_m / speed
    ph = (t % period) / period
    return leg_m * (2.0 * ph if ph < 0.5 else 2.0 * (1.0 - ph))


def window_events(rows: list[dict]) -> list[int]:
    """Row indices where the land-cover window id changes from one nonzero id to another (re-centres)."""
    ev, prev = [], 0
    for i, r in enumerate(rows):
        w = int(r.get("land_cover_window") or 0)
        if w and prev and w != prev:
            ev.append(i)
        if w:
            prev = w
    return ev


def recentre_spike(rows: list[dict], events: list[int], radius: int = RECENTRE_RADIUS) -> tuple[float, float]:
    """(max wall_ms within `radius` rows of a re-centre minus the median wall_ms of all rows, that median)."""
    wall = [float(r["wall_ms"]) for r in rows]
    if not events or not wall:
        return float("nan"), float("nan")
    base = float(np.median(wall))
    near = [wall[j] for i in events for j in range(max(0, i - radius), min(len(wall), i + radius + 1))]
    return max(near) - base, base


def per_recentre_spikes(rows: list[dict], events: list[int], radius: int = RECENTRE_RADIUS) -> list[float]:
    """Gate (l1): for each re-centre, max wall_ms within `radius` rows minus the median wall_ms of all rows."""
    return [recentre_spike(rows, [e], radius)[0] for e in events]


def control_excess(
    rows: list[dict],
    t0: float,
    events: list[int],
    ctrl_rows: list[dict],
    ctrl_t0: float,
    radius: int = RECENTRE_RADIUS,
) -> list[float]:
    """Gate (l2): for each re-centre, max wall_ms within `radius` rows minus the max wall_ms of the control run (land
    cover off, same pan) over the same view-time span (t - t0 vs t - ctrl_t0). nan where the control has no frames."""
    out = []
    for e in events:
        lo, hi = max(0, e - radius), min(len(rows) - 1, e + radius)
        on = max(float(r["wall_ms"]) for r in rows[lo : hi + 1])
        a, b = rows[lo]["t"] - t0, rows[hi]["t"] - t0
        ctrl = [float(r["wall_ms"]) for r in ctrl_rows if a <= r["t"] - ctrl_t0 <= b]
        out.append(on - max(ctrl) if ctrl else float("nan"))
    return out


def run_group(label: str) -> str:
    if "_ent_" in label:
        return "entity"  # ROADMAP 4C: park / cold / dead
    if label.startswith("mwir_pan"):
        return "pan"  # the pan and its land-cover-off control
    if label in BANDS:
        return "bands"
    if label == "mwir_1080p":
        return "hd"
    if label.startswith("eo_"):
        return "eo"
    if label.endswith("_lcoff"):
        return "lcoff"
    if label.endswith("_roads"):
        return "roads"  # REALISM R2 gate (r)
    if label.endswith("_roofs"):
        return "roofs"  # REALISM R3b gates (s, s_air)
    raise ValueError(label)


def run_band(label: str) -> str | None:
    if label in BANDS:
        return label
    if "_ent_" in label:
        return label.split("_ent_")[0]
    if label.endswith("_lcoff") and not label.startswith("mwir_pan"):
        return label[: -len("_lcoff")]
    if label.endswith("_roads"):
        return label[: -len("_roads")]
    if label.endswith("_roofs"):
        return label[: -len("_roofs")]
    return None


Row = tuple[str, str, str]  # (check, band, time)


def expected_rows(bands: list[str], runs: set[str]) -> list[Row]:
    """Every gate row the selected bands and runs must produce. a-e, h, i, j come from the band runs; k needs the band runs
    and their land-cover-off twins (lcoff); l the pan run; f the hd run; g the eo runs. Each is expected only when its run
    groups are selected (so `--runs bands` can pass on its own); r the roads runs. (m) is info only, never expected."""
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
                ("i", b, "noon"),
                ("j", b, "night"),
            ]
    if "bands" in runs and "lcoff" in runs:
        rows += [("k", b, "night") for b in bands]
    if "pan" in runs:
        rows += [("l1", "mwir", "noon"), ("l2", "mwir", "noon")]
    if "hd" in runs:
        rows.append(("f", "mwir", "noon"))
    if "eo" in runs:
        rows.append(("g", "eo", "noon"))
    if "entity" in runs:
        for b in bands:
            rows += [("n", b, "night"), ("o", b, "night"), ("p", b, "night")]
        if "mwir" in bands:
            rows.append(("q", "mwir", "night"))
    if "roads" in runs:
        rows += [("r", b, t) for b in bands for t in ("noon", "night")]
    if "roofs" in runs and "mwir" in bands:
        rows += [("s", "mwir", "noon"), ("s_air", "mwir", "night")]
    return rows


def box_ring_stats(y: np.ndarray, anns: list[dict | None]) -> dict:
    """Medians over the frames with an annotation of: the box mean, the ring mean, the box p99 and the box median (DN)."""
    per: dict[str, list[float]] = {"mean": [], "ring": [], "p99": [], "median": []}
    for f, ann in zip(y, anns):
        if ann is None:
            continue
        box = box_mask(f.shape, ann["bbox"])
        ring = ring_mask(f.shape, ann["bbox"])
        if box.sum() < MIN_PIXELS or ring.sum() < MIN_PIXELS:
            continue
        fy = f.astype(np.float32)
        v = fy[box]
        per["mean"].append(float(v.mean()))
        per["ring"].append(region_mean(fy, ring))
        per["p99"].append(float(np.percentile(v, 99)))
        per["median"].append(float(np.median(v)))
    out = {k: med(vs) for k, vs in per.items()}
    out["n"] = len(per["mean"])
    return out


def entity_gate_rows(
    band: str, run: dict, cold: dict, park5: dict, park600: dict, dead: dict | None
) -> list[dict]:
    """Gates (n)-(q) (ROADMAP 4C) from box_ring_stats of the running, parked-cold, parked (+5 s, +600 s) and destroyed truck."""

    def row(check, value, threshold, ok, detail):
        ok = bool(ok) and not math.isnan(value)
        return {
            "check": check,
            "band": band,
            "time": "night",
            "value": value,
            "threshold": threshold,
            "pass": ok,
            "detail": detail,
        }

    run_c, cold_c = run["mean"] - run["ring"], cold["mean"] - cold["ring"]
    n = run_c - cold_c
    rows = [
        row(
            "n",
            n,
            f">= +{ENT_RUN_DN:.0f} DN",
            n >= ENT_RUN_DN,
            f"running box - ring {run_c:.1f} vs parked-cold {cold_c:.1f} ({run['n']} / {cold['n']} frames)",
        )
    ]
    o = run["p99"] - run["median"]
    rows.append(
        row(
            "o",
            o,
            f">= +{ENT_HOT_DN[band]:.0f} DN",
            o >= ENT_HOT_DN[band],
            f"running box p99 {run['p99']:.1f} - median {run['median']:.1f}",
        )
    )
    # Box mean, not p99: in MWIR the hot parts clip at the display ceiling, so p99 cannot fall.
    h5 = park5["mean"] - park5["ring"]
    h600 = park600["mean"] - park600["ring"]
    hc = cold_c
    rows.append(
        row(
            "p",
            h5 - h600,
            "box - ring: +600 s < +5 s and > parked-cold",
            h600 < h5 and h600 > hc,
            f"+5 s {h5:.1f}, +600 s {h600:.1f}, parked-cold {hc:.1f}",
        )
    )
    if dead is not None:
        q = dead["mean"] - dead["ring"]
        rows.append(
            row(
                "q",
                q,
                f">= +{ENT_DEAD_DN:.0f} DN",
                q >= ENT_DEAD_DN,
                f"destroyed box - ring ({dead['n']} frames)",
            )
        )
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
        "# Thermal check (ROADMAP 4A/4B)",
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
    sensor_id: int | None = None  # overrides the run's sensor (EO frame in an IR run)
    at_s: float | None = None  # capture no earlier than this sender time (ROADMAP 4C cool-down views)


@dataclass
class RunSpec:
    label: str
    env: dict[str, str]
    sensor_id: int
    times: list[str]
    views: Callable[[str], list[View]]  # time label -> views
    ml: bool = True
    exec_cmds: list[str] = field(default_factory=list)
    dis_args: list[str] = field(default_factory=lambda: ["both"])  # send_dis_test.py arguments; [] = no DIS sender


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

    def ent_point(t: float) -> tuple[float, float]:
        """Where truck-park stands at sender time t (t >= ENT_DRIVE_S: where it stopped)."""
        f = sd.PathFollower(truck.waypoints_ne, truck.speed_mps)
        n, e, *_ = sd.track_kinematics("truck-park", f, t, ENT_DRIVE_S, truck.speed_mps)
        return sd.ne_to_latlon(truck.center, n, e)

    def ent_static(t: float):
        lat, lon = ent_point(t)
        return static(
            scenario.Pose(
                lat, lon, ground + ENT_UP_M, gimbal_pitch=-90.0, fov_h=ENT_FOV
            )
        )

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
        "nadir_mixed": static(  # ROADMAP 4B: land-cover mix (gates i, j, k, m)
            scenario.Pose(
                MIXED_CENTER[0],
                MIXED_CENTER[1],
                ground + MIXED_UP_M,
                gimbal_pitch=-90.0,
                fov_h=MIXED_FOV,
            )
        ),
        "pan": lambda t: scenario.Pose(  # ROADMAP 4B gate (l): 1 km/min east and back
            *sd.ne_to_latlon(MIXED_CENTER, 0.0, pan_offset_m(t)),
            ground + PAN_UP_M,
            gimbal_pitch=-90.0,
            fov_h=MIXED_FOV,
        ),
        # REALISM R2 gate (r): nadir over I-5 on the Pendleton package, heading along the road (a vertical strip);
        # road_t9 is the info-only first pose (the OSM/imagery disagreement case)
        **{
            name: static(
                scenario.Pose(
                    rp["lat"],
                    rp["lon"],
                    rp["ground_hae"] + ROAD_UP_M,
                    yaw=rp["heading"],
                    gimbal_pitch=-90.0,
                    fov_h=ROAD_FOV,
                )
            )
            for name, rp in (("road", ROAD_POSE), ("road_t9", ROAD_POSE_T9))
        },
        "roof": static(
            scenario.Pose(ROOF_POSE["lat"], ROOF_POSE["lon"], ROOF_POSE["ground_hae"] + ROOF_UP_M, yaw=0.0,
                          gimbal_pitch=-90.0, fov_h=ROOF_FOV)
        ),
        # ROADMAP 4C: the truck close up, moving (truck-park while driving) or where it stands still
        "ent_running": dvc.nadir_on(truck, ground + ENT_UP_M, ENT_FOV),
        "ent_stop": ent_static(ENT_DRIVE_S),
        "nadir_static": static(
            scenario.Pose(
                c[0], c[1], ground + NADIR_UP_M, gimbal_pitch=-90.0, fov_h=NADIR_FOV
            )
        ),
    }


def build_runs(bands: list[str], wanted: set[str], scene: str | None = None) -> list[RunSpec]:
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
        v.append(View("nadir_mixed", p["nadir_mixed"], 10, "truck"))  # (i) noon, (j, k, m) night
        if tod == "noon":
            v.append(View("mixed_eo", p["nadir_mixed"], 3, None, sensor_id=0))  # greenness masks for (i) and (j)
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
    if "lcoff" in wanted:
        for b in bands:
            runs.append(
                RunSpec(
                    f"{b}_lcoff",
                    {"CAMSIM_IR_PRESET": BANDS[b], "CAMSIM_THERMAL_LAND_COVER_ENABLED": "0"},
                    1,
                    ["night"],
                    lambda tod: [View("nadir_mixed", p["nadir_mixed"], 10, "truck")],
                )
            )
    if "pan" in wanted:
        for label, extra in (("mwir_pan", {}), ("mwir_pan_lcoff", {"CAMSIM_THERMAL_LAND_COVER_ENABLED": "0"})):
            runs.append(
                RunSpec(
                    label,
                    {
                        "CAMSIM_IR_PRESET": "mwir_cooled",
                        "CAMSIM_THERMAL_LAND_COVER_RECENTRE_FRACTION": PAN_RECENTRE_FRACTION,
                        **extra,
                    },
                    1,
                    ["noon"],
                    lambda tod: [View("pan", p["pan"], 0, None, hold_s=PAN_HOLD_S)],
                    ml=False,
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
    if "entity" in wanted:  # ROADMAP 4C gates (n)-(q)
        for b in bands:
            env = {"CAMSIM_IR_PRESET": BANDS[b]}
            runs.append(
                RunSpec(
                    f"{b}_ent_park",
                    env,
                    1,
                    ["night"],
                    lambda tod: [
                        View("ent_running", p["ent_running"], 10, "truck"),
                        View("ent_park5", p["ent_stop"], 10, "truck", at_s=ENT_DRIVE_S + 5.0),
                        View("ent_park600", p["ent_stop"], 10, "truck", at_s=ENT_DRIVE_S + 600.0),
                    ],
                    exec_cmds=["camsim.Thermal.Log 1"],
                    dis_args=["truck-park", "--drive-s", str(ENT_DRIVE_S)],
                )
            )
            runs.append(
                RunSpec(
                    f"{b}_ent_cold",
                    env,
                    1,
                    ["night"],
                    lambda tod: [View("ent_parked", p["ent_stop"], 10, "truck")],
                    dis_args=["truck-parked", "--hold-s", str(ENT_DRIVE_S)],
                )
            )
        if "mwir" in bands:
            runs.append(
                RunSpec(
                    "mwir_ent_dead",
                    {"CAMSIM_IR_PRESET": BANDS["mwir"]},
                    1,
                    ["night"],
                    lambda tod: [View("ent_dead", p["ent_stop"], 10, "truck")],
                    dis_args=["truck-destroyed", "--hold-s", str(ENT_DRIVE_S)],
                )
            )
    if "roads" in wanted:  # REALISM R2 gate (r) on the scene package (main() requires --scene to launch it)
        for b in bands:
            runs.append(
                RunSpec(
                    f"{b}_roads",
                    {"CAMSIM_IR_PRESET": BANDS[b], "CAMSIM_SCENE_DIR": scene or ""},
                    1,
                    ["night", "noon"],
                    lambda tod: [View("road", p["road"], 10, None), View("road_t9", p["road_t9"], 10, None)],
                    ml=False,
                    exec_cmds=["camsim.Thermal.Log 1"],
                    dis_args=[],
                )
            )
    if "roofs" in wanted and "mwir" in bands:  # REALISM R3b gates (s, s_air) on the scene package (main() requires --scene)
        runs.append(
            RunSpec(
                "mwir_roofs",
                {"CAMSIM_IR_PRESET": BANDS["mwir"], "CAMSIM_SCENE_DIR": scene or ""},
                1,
                ["night", "noon"],
                lambda tod: [View("roof", p["roof"], 10, None)],
                ml=False,
                exec_cmds=["camsim.Thermal.Log 1"],
                dis_args=[],
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

    def apply(pose, tod: dict, sensor: int | None = None):
        return dataclasses.replace(pose, sensor_id=spec.sensor_id if sensor is None else sensor, **tod)

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
        if spec.dis_args:  # [] (roads): no vehicles; send_dis_test.py with no arguments would send `both`
            sender = subprocess.Popen(
                [sys.executable, str(REPO / "scripts" / "send_dis_test.py"), *spec.dis_args],
                stdout=subprocess.DEVNULL,
            )
        tracker = dvc.Tracker(host, time.monotonic())
        for tod_label in spec.times:
            tod = TIMES[tod_label]
            for v in spec.views(tod_label):
                print(f"[thermal] {spec.label}: {tod_label} {v.name}", flush=True)
                tracker.pose_at = lambda t, v=v, tod=tod: apply(v.pose_at(t), tod, v.sensor_id)
                time.sleep(2.0)
                dvc.wait_tiles(stats, 60.0)
                time.sleep(SETTLE_S)
                if v.at_s is not None:  # ROADMAP 4C: a fixed sender time (cool-down after parking)
                    time.sleep(max(0.0, tracker.t0 + v.at_s - time.monotonic()))
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
        # Info only (fix round 1): the metric cannot tell the visibly blocky Task 10 build (0.68) from the fixed one; a
        # random-valued block field has sinc zeros, not a peak, at the grid fundamental.
        add(
            info,
            "m (grid peak ratio, info)",
            r,
            time_="night",
            detail=f"x {rx:.2f}, y {ry:.2f} (~1 = no peak; was gated <= {GRID_PEAK_RATIO:g}) at period {period:.1f} px (10 m at {MIXED_UP_M:g} m, {MIXED_FOV:g} deg); "
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


def check_land_cover(
    out: Path, band: str, views: dict, off_views: dict, checks: list, info: list, shots: list
) -> None:
    """(i) noon and (j) night: non-vegetation minus vegetation IR mean (masks from the noon EO frame of the same pose);
    (k) night high-pass std with land cover on / off (ROADMAP 4B)."""
    eo = views.get(("noon", "mixed_eo"))
    rgb = None
    if eo is not None and Path(eo.rec["shot"]).exists():
        rgb = np.asarray(Image.open(eo.rec["shot"]).convert("RGB"))
    for tod, check, limit in (("noon", "i", VEG_NOON_DN), ("night", "j", VEG_NIGHT_DN)):
        vd = views.get((tod, "nadir_mixed"))
        if vd is None or rgb is None or rgb.shape[:2] != vd.y.shape[1:]:
            continue  # the row stays missing: the gate fails
        veg, non = greenness_masks(rgb)
        ent = entity_mask(vd)
        veg, non = veg & ~ent, non & ~ent
        img = vd.y.astype(np.float32).mean(axis=0)
        d = veg_contrast(img, veg, non)
        checks.append(
            {
                "check": check,
                "band": band,
                "time": tod,
                "value": d,
                "threshold": f">= +{limit:g} DN (non-vegetation - vegetation)",
                "pass": bool(d >= limit),  # NaN fails
                "detail": f"vegetation {int(veg.sum())} px, mean {region_mean(img, veg):.1f}; "
                f"non-vegetation {int(non.sum())} px, mean {region_mean(img, non):.1f}",
            }
        )
        shots.append(overlay(out, vd, veg, non, None, len(vd.y) // 2))
    on = views.get(("night", "nadir_mixed"))
    off = off_views.get(("night", "nadir_mixed"))
    if on is not None and off is not None and on.y.shape[1:] == off.y.shape[1:]:
        mask = central_mask(on.y.shape[1:]) & ~entity_mask(on) & ~entity_mask(off)
        y_on, y_off = on.y.astype(np.float32).mean(axis=0), off.y.astype(np.float32).mean(axis=0)
        gsd = nadir_gsd_m(MIXED_UP_M, MIXED_FOV, on.y.shape[2])
        lo, hi = band_px(gsd)
        c_on, c_off = class_structure_std(y_on, mask, lo, hi), class_structure_std(y_off, mask, lo, hi)
        ratio = c_on / max(c_off, 1e-6)
        checks.append(
            {
                "check": "k",
                "band": band,
                "time": "night",
                "value": ratio,
                "threshold": f">= {STRUCTURE_RATIO:g}x land cover off",
                "pass": bool(ratio >= STRUCTURE_RATIO),  # NaN fails
                "detail": f"{STRUCTURE_LO_M:g}-{STRUCTURE_HI_M:g} m band-pass std (3x3 median, box {lo} - box {hi} px at "
                f"{gsd:.3f} m/px) {c_on:.2f} DN vs {c_off:.2f} DN (4A), {int(mask.sum())} px",
            }
        )
        shots.append(off.rec["shot"])
        s_on, s_off = highpass_std(y_on, mask), highpass_std(y_off, mask)
        info.append(
            {
                "check": "k high-pass (info)",
                "band": band,
                "time": "night",
                "value": s_on / max(s_off, 1e-6),
                "detail": f"{HIGHPASS_K} px high-pass std {s_on:.2f} DN vs {s_off:.2f} DN (detector noise dominates the 4A frame)",
            }
        )
        # Info: the same after a 3 x 3 median (defect pixels are detector artefacts, identical on and off).
        m_on, m_off = highpass_std(median3(y_on), mask), highpass_std(median3(y_off), mask)
        info.append(
            {
                "check": "k (defects removed, info)",
                "band": band,
                "time": "night",
                "value": m_on / max(m_off, 1e-6),
                "detail": f"3x3 median, then high-pass std {m_on:.2f} DN vs {m_off:.2f} DN",
            }
        )
        shots.append(off.rec["shot"])


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
    out: Path, results: dict[str, dict], bands: list[str], scene: str | None = None
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
        off_views = {}
        off_run = results.get(f"{band}_lcoff")
        if off_run:
            for rec in off_run["views"]:
                vd = load_view(out, off_run, rec)
                if vd is not None:
                    off_views[(rec["time"], rec["view"])] = vd
        check_land_cover(out, band, views, off_views, checks, info, shots)
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

    # (n)-(q) entity thermal state (ROADMAP 4C): running, parked-cold, cooling after parking, destroyed.
    for band in bands:
        park = results.get(f"{band}_ent_park")
        cold = results.get(f"{band}_ent_cold")
        dead = results.get(f"{band}_ent_dead")
        if not park or not cold:
            continue
        stats: dict[str, dict] = {}
        for r in (park, cold, dead):
            if not r:
                continue
            for rec in r["views"]:
                vd = load_view(out, r, rec)
                if vd is None:
                    continue
                shots.append(rec["shot"])
                st = box_ring_stats(vd.y, vd.anns)
                stats[rec["view"]] = st
                idx = next((i for i, a in enumerate(vd.anns) if a is not None), None)
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
                info.append(
                    {
                        "check": f"entity {rec['view']}",
                        "band": band,
                        "time": "night",
                        "value": st["mean"] - st["ring"],
                        "detail": f"box - ring; box p99 {st['p99']:.1f}, median {st['median']:.1f}, ring {st['ring']:.1f}, {st['n']} frames",
                    }
                )
        need = ("ent_running", "ent_parked", "ent_park5", "ent_park600")
        if all(k in stats for k in need):
            checks.extend(
                entity_gate_rows(
                    band,
                    stats["ent_running"],
                    stats["ent_parked"],
                    stats["ent_park5"],
                    stats["ent_park600"],
                    stats.get("ent_dead"),
                )
            )

    # (r) REALISM R2: road vs outer verge, nadir along I-5 on the Pendleton package, temporal mean of the view's frames.
    for band in bands:
        run = results.get(f"{band}_roads")
        if not run:
            continue
        for rec in run["views"]:
            vd = load_view(out, run, rec)
            if vd is None or rec["view"] not in ("road", "road_t9"):
                continue
            rp = ROAD_POSE if rec["view"] == "road" else ROAD_POSE_T9
            shots.append(rec["shot"])
            y_mean = vd.y.astype(np.float32).mean(axis=0)
            gsd = nadir_gsd_m(ROAD_UP_M, ROAD_FOV, y_mean.shape[1])
            masks = road_masks(y_mean.shape, gsd, rp["width_m"], rp["outer_side"])
            thr = ROAD_NOON_DN if rec["time"] == "noon" else ROAD_NIGHT_DN
            row = road_row(band, rec["time"], y_mean, masks, thr)
            row["detail"] += f", GSD {gsd:.3f} m, {len(vd.y)} frames, OSM way {rp['osm_way']}"
            if rec["view"] == "road":
                checks.append(row)
            else:  # info only: the first pose, where OSM (asphalt) and NAIP (light pavement) disagree
                row["check"] = "r_t9"
                row["detail"] = "info, OSM/imagery disagreement case (threshold not applied): " + row["detail"]
                info.append({k: row[k] for k in ("check", "band", "time", "value", "detail")})
            shots.append(overlay(out, vd, masks[0], masks[1], None, len(vd.y) // 2))
        log = thermal_log_lines(out / run["label"] / "CamSimTest.log")
        for rp, label in ((ROAD_POSE, "r_info"), (ROAD_POSE_T9, "r_t9_info")):
            vc = rp["verge_class"]
            temps = road_class_temps(log, vc)
            for tod in ("night", "noon"):
                a, g = temps.get(tod, (float("nan"), float("nan")))
                info.append(
                    {
                        "check": label,
                        "band": band,
                        "time": tod,
                        "value": a - g,
                        "detail": f"class temperatures (camsim.Thermal.Log): asphalt {a:.1f} K, {vc} {g:.1f} K"
                        + (f" ({vc} not in the log line)" if math.isnan(g) else ""),
                    }
                )

    # (s) REALISM R3b: roofs vs grass at noon (image; night as info) and the building_roof class vs air at 02:00 (log).
    run = results.get("mwir_roofs")
    if run:
        if not scene:
            raise SystemExit("gates (s, s_air) need --scene: the package's footprint mask and land cover")
        from bench import dense_path as dp

        bmask = dp.BuildingMask(Path(scene) / "features" / "buildings" / "mask")
        lcov = dp.LandCover(Path(scene) / "landcover")
        for rec in run["views"]:
            vd = load_view(out, run, rec)
            if vd is None or rec["view"] != "roof":
                continue
            shots.append(rec["shot"])
            y_mean = vd.y.astype(np.float32).mean(axis=0)
            gsd = nadir_gsd_m(ROOF_UP_M, ROOF_FOV, y_mean.shape[1])
            masks = roof_masks(y_mean.shape, gsd, ROOF_POSE["lat"], ROOF_POSE["lon"], bmask.masked, lcov.codes)
            row = roof_row("mwir", rec["time"], y_mean, masks, ROOF_NOON_DN)
            row["detail"] += f", GSD {gsd:.3f} m, {len(vd.y)} frames"
            if rec["time"] == "noon":
                checks.append(row)
            else:
                row["check"] = "s_night"
                info.append({k: row[k] for k in ("check", "band", "time", "value", "detail")})
            shots.append(overlay(out, vd, masks[0], masks[1], None, len(vd.y) // 2))
        temps = roof_air_temps(thermal_log_lines(out / run["label"] / "CamSimTest.log"))
        nan3 = (float("nan"),) * 3
        r, g, tair = temps.get("night", nan3)
        checks.append(
            {
                "check": "s_air",
                "band": "mwir",
                "time": "night",
                "value": round(tair - r, 2),
                "threshold": ROOF_AIR_K,
                "pass": bool(tair - r >= ROOF_AIR_K),
                "detail": f"class temperatures (camsim.Thermal.Log): building_roof {r:.1f} K, air {tair:.1f} K, "
                f"grassland {g:.1f} K",
            }
        )
        r, g, tair = temps.get("noon", nan3)
        info.append({"check": "s_air_noon", "band": "mwir", "time": "noon", "value": round(r - g, 2),
                     "detail": f"building_roof {r:.1f} K, grassland {g:.1f} K, air {tair:.1f} K"})

    # (l) no frame-time spike when the land-cover window re-centres during a 1 km/min pan (ruling S12): l1 the median
    # per-re-centre spike, l2 each re-centre against the land-cover-off control pan at the same view time.
    run, ctrl = results.get("mwir_pan"), results.get("mwir_pan_lcoff")

    def pan_rows(r: dict) -> tuple[list[dict], float]:
        w = r["views"][0]
        rows = [x for x in load_rows(out / r["label"] / "frames.jsonl") if w["t0"] <= x["t"] <= w["t1"] and not x.get("cut")]
        return rows, w["t0"]

    if run:
        rows, t0 = pan_rows(run)
        ev = window_events(rows)
        spike, base = recentre_spike(rows, ev)
        per = per_recentre_spikes(rows, ev)
        m = med(per)
        enough = len(ev) >= MIN_RECENTRES
        checks.append(
            {
                "check": "l1",
                "band": "mwir",
                "time": "noon",
                "value": m,
                "threshold": f"median <= {RECENTRE_MEDIAN_MS:g} ms over the pan median, >= {MIN_RECENTRES} re-centres",
                "pass": bool(enough and m <= RECENTRE_MEDIAN_MS),
                "detail": f"{len(ev)} re-centres in {len(rows)} frames; per re-centre "
                + ", ".join(f"{x:+.2f}" for x in per)
                + f" ms; median wall {base:.2f} ms",
            }
        )
        quiet = [float(r["wall_ms"]) for i, r in enumerate(rows) if all(abs(i - e) > RECENTRE_RADIUS for e in ev)]
        info.append(
            {
                "check": "l max spike (info)",
                "band": "mwir",
                "time": "noon",
                "value": spike,
                "detail": f"max wall_ms near any re-centre - median; max away from re-centres "
                f"{max(quiet, default=float('nan')) - base:+.2f} ms (background hitches)",
            }
        )
        if ctrl:
            c_rows, c_t0 = pan_rows(ctrl)
            ex = control_excess(rows, t0, ev, c_rows, c_t0)
            worst = max(ex) if ex and not any(math.isnan(x) for x in ex) else float("nan")
            checks.append(
                {
                    "check": "l2",
                    "band": "mwir",
                    "time": "noon",
                    "value": worst,
                    "threshold": f"max <= {RECENTRE_EXCESS_MS:g} ms over the land-cover-off control, >= {MIN_RECENTRES} re-centres",
                    "pass": bool(enough and worst <= RECENTRE_EXCESS_MS),  # NaN fails
                    "detail": "per re-centre (max near it - control max at the same view time) "
                    + ", ".join(f"{x:+.2f}" for x in ex)
                    + f" ms; control {len(c_rows)} frames, max window id {max((int(r.get('land_cover_window') or 0) for r in c_rows), default=0)}",
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
        "--runs",
        default="bands,hd,eo,lcoff,pan,entity",
        help="comma list of: bands, hd, eo, lcoff, pan, entity, roads, roofs (roads and roofs need --scene; not in the default)",
    )
    ap.add_argument(
        "--scene",
        default=None,
        help="absolute path of the Camp Pendleton scene package (CAMSIM_SCENE_DIR) for the roads and roofs runs, gates (r) and (s)",
    )
    ap.add_argument(
        "--check-only", action="store_true", help="re-run the checks on --out"
    )
    a = ap.parse_args()
    wanted = set(filter(None, a.runs.split(",")))
    scene = str(Path(a.scene).expanduser().resolve()) if a.scene else None
    if ({"roads", "roofs"} & wanted) and not a.check_only:  # before any launch or output directory
        if not scene:
            ap.error("--runs roads/roofs needs --scene <Pendleton scene package>")
        if not (Path(scene) / "manifest.json").is_file():
            ap.error(f"--scene {scene}: no manifest.json")
    out = (
        a.out or REPO / ".cache" / "thermal_check" / time.strftime("%Y%m%d-%H%M%S")
    ).resolve()
    for d in ("shots", "overlays"):
        (out / d).mkdir(parents=True, exist_ok=True)
    bands = ["mwir", "lwir"] if a.band == "both" else [a.band]

    results: dict[str, dict] = {}
    for spec in build_runs(bands, {"bands", "hd", "eo", "lcoff", "pan", "entity", "roads", "roofs"}, scene):
        band = run_band(spec.label)
        if band is not None and band not in bands:
            continue
        group = run_group(spec.label)
        if group in wanted and not a.check_only:
            results[spec.label] = run_once(spec, out)
        elif (out / spec.label / "run.json").exists():
            results[spec.label] = json.loads((out / spec.label / "run.json").read_text())

    checks, info, shots = check_all(out, results, bands, scene)
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
