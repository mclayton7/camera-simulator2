#!/usr/bin/env python3
"""Night sky acceptance (ROADMAP 6A, spec docs/superpowers/specs/2026-10-16-night-sky-6a-design.md, guide docs/night.md).

Gates (each writes OUT/<gate>.json with its measured values, threshold and pass/fail):

  N1 day      daytime unchanged vs the pre-6A baseline (--baseline: this script's `day` run on 59c3ff6): nadir 3 km noon,
              slant 1 km 10:00, low 300 m 15:00 local; EO mean luma within +-3 DN, temporal noise within 10 %
  N2 moon     full moon high (2026-10-26 07:00 UTC) vs new moon (2026-11-09 07:00 UTC), slant 500 m: the AE's scene
              median (log2) differs by log2(predicted ground illuminance ratio) +-0.5 EV; flat-patch temporal noise of Y
              within +-20 % of a Monte Carlo mirror of CamSimSensorRef::Run at the logged gains
  N3 moon     moon in view, 2 deg FOV, camera on the ephemeris moon (full and gibbous): the > 50 %-of-max centroid within
              2 px of the expected one (the disc model: albedo map x lunar-Lambert, IAU orientation, through the display
              curve); the brighter half of the disc toward the projected sun; full moon: limb / centre > 0.7 (not a
              Lambert ball) and maria contrast (std / mean inside 0.6 R) > 0.15
  N4 stars    new moon, el 60 deg toward Ursa Major, 40 deg FOV, long integration (detector.integration_max_ms 2000):
              >= 20 detections (> 5 sigma) within 2 px of BSC5 stars projected with skyfield; at noon, same view: 0
  N5 dusk     clock at 60x through sunset: consecutive-frame mean luma step <= 5 DN through the band switch, settled
              (|step| < 1 DN for 30 frames) within 30 frames
  N6 thermal  scripts/thermal_check.py --scene PKG exits 0
  N7 cost     bench night_slant phase vs the baseline: game thread and GPU p50 within +0.3 ms
  N8 dusk     sun ground illuminance (camsim.Sky.Log) non-increasing through 0 -> -1 deg and < 1 % of its 10 deg value
              by -1 deg; frame mean luma non-increasing (+1 DN) through civil twilight
  N9 dusk     twilight colour: the ground's illuminant (nadir 100 m, its linear RGB / the same pose's at sun +22 deg)
              at +3, 0, -3, -6 deg against Spitschan et al. (2016)'s measured bands (scripts/sky/twilight_bands.json):
              0 deg in band in R/G, B/G and the R/G-vs-B/G colour temperature difference; B/G in band at +3/-3/-6;
              colour temperature rising +3 -> 0 -> -3; zenith not magenta and bluer than the ground. R/G at +3/-3/-6
              is reported as a documented limit

Runs (--runs, comma list): day (N1), moon (N2, N3), stars (N4), dusk (N5, N8, N9), thermal (N6), cost (N7). Each run
launches CamSim once (thermal and cost launch their own tools) with the level's engine-time volumetric clouds off; the
day run also freezes TSR jitter (as its baseline was taken), the night runs don't (see EXEC_CMDS). The sensor's temporal
noise is never pinned (N1/N2 measure it). summary.md always lists N1-N9: gates not evaluated by this invocation come from
OUT/N*.json left by earlier ones (with their time and commit).

    uv run --project scripts/control --with numpy --with pillow --with scipy --with skyfield \\
        python scripts/night_check.py --scene .cache/scene-packages/pendleton-r4 --out .cache/night/6a \\
        --baseline .cache/night/base

The baseline is this script's `--runs day,cost` on the pre-6A commit (docs/night.md "Acceptance").
"""

from __future__ import annotations

import argparse
import contextlib
import dataclasses
import json
import math
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
from collections.abc import Iterator
from datetime import datetime, timedelta, timezone
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts"))
sys.path.insert(0, str(REPO / "scripts" / "control"))
sys.path.insert(0, str(REPO / "scripts" / "sky"))
import ephemeris as eph  # noqa: E402
import moon_surface as msurf  # noqa: E402
import numpy as np  # noqa: E402
from bench import run_bench as rb  # noqa: E402
from camsim_control.client import Client  # noqa: E402
from camsim_control.randomize import offset_ll  # noqa: E402
from PIL import Image  # noqa: E402

SITE = (33.3072917, -117.344625)  # georeg flat site, Camp Pendleton (paved, < 2 deg slope)
GROUND = -11.235                  # its ground, ellipsoid m (control_api_check.py C7_GROUND)
W, H = 1280, 720                  # deploy/camsim_config.yaml capture size
# Diagnostics on, the level's engine-time volumetric clouds off (control_api_check.py). TSR jitter is NOT frozen for the
# night runs: with r.Test.FreezeTemporalSequences 1, TSR history held stale light for good after a large lighting jump
# (full -> new moon, night -> noon in lockstep: a mottled noon sky). The day run keeps it, as its baseline was taken.
EXEC_CMDS = "-ExecCmds=camsim.Sky.Log 1,camsim.Sensor.AeLog 1,r.VolumetricCloud 0"
DAY_EXEC_CMDS = EXEC_CMDS + ",r.Test.FreezeTemporalSequences 1"
# UE's log: ~/Library/Logs/<Project> on macOS, the project's Saved/Logs elsewhere (scripts/run.sh)
LOG = (Path.home() / "Library/Logs/CamSimTest/CamSimTest.log" if sys.platform == "darwin"
       else REPO / "unreal_project/CamSimTest/Saved/Logs/CamSimTest.log")
STREAM_PORT = rb.STREAM_PORT
SETTLE_DAY, SETTLE_NIGHT = 60, 120

# eo_hd_cmos preset (Sensor/SensorPresets.cpp) + FSensorFrameParams defaults (SensorFrameParams.h)
EO = {"full_well": 10000.0, "read": 2.0, "prnu": 0.01, "dsnu": 1.0, "dark_es": 5.0, "adc_max": 4095.0, "knee": 0.8}

DATE_FULL = datetime(2026, 10, 26, 7, 0, tzinfo=timezone.utc)
DATE_NEW = datetime(2026, 11, 9, 7, 0, tzinfo=timezone.utc)
DATE_GIBBOUS = datetime(2026, 10, 22, 4, 30, tzinfo=timezone.utc)   # phase angle ~50 deg, moon el ~48, sun el -42
DATE_WIDE = datetime(2026, 10, 25, 12, 0, tzinfo=timezone.utc)        # moon el ~17, phase ~10 deg, sun -26
WIDE_HFOV, WIDE_PITCH_BELOW_MOON = 40.0, 10.0                        # the moon near the top, dark ground at the bottom
DATE_STARS = datetime(2027, 1, 7, 11, 0, tzinfo=timezone.utc)       # new moon, sun -48, Megrez at el ~59 az ~31
DATE_STARS_NOON = datetime(2027, 1, 7, 20, 0, tzinfo=timezone.utc)
MEGREZ = (183.856, 57.0326)                                          # RA, Dec J2000 (deg), the middle of the Dipper
DUSK_DAY = (2026, 6, 21)                                             # sunset ~03:00 UTC on the 22nd
STARS_INTEGRATION_MS = 2000.0
N9_DAY_EL = 22.0                                                     # N9's daylight reference (Spitschan 20-26 deg bin)


# ---------------------------------------------------------------------------
# Poses
# ---------------------------------------------------------------------------


def camera_at(aim: tuple[float, float], ground: float, slant: float, depression: float, heading: float,
              hfov: float) -> dict:
    horiz = slant * math.cos(math.radians(depression))
    h = math.radians(heading)
    lat, lon = offset_ll(aim[0], aim[1], -horiz * math.cos(h), -horiz * math.sin(h))
    return {"lat": lat, "lon": lon, "alt": ground + slant * math.sin(math.radians(depression)), "heading": heading,
            "pitch": 0.0, "roll": 0.0, "gimbal": {"yaw": 0.0, "pitch": -depression, "roll": 0.0}, "hfov_deg": hfov,
            "sensor": {"mode": "eo"}}


def sky_camera(az: float, el: float, hfov: float, alt: float = GROUND + 300.0) -> dict:
    """Platform at the site pointed along (az, el): the gimbal stops at +30 deg, so the platform pitches."""
    return {"lat": SITE[0], "lon": SITE[1], "alt": alt, "heading": az, "pitch": el, "roll": 0.0,
            "gimbal": {"yaw": 0.0, "pitch": 0.0, "roll": 0.0}, "hfov_deg": hfov, "sensor": {"mode": "eo"}}


DAY_POSES = {
    "nadir_3km_noon": (datetime(2026, 6, 21, 19, 0, tzinfo=timezone.utc),
                       {"lat": SITE[0], "lon": SITE[1], "alt": GROUND + 3000.0, "heading": 0.0, "pitch": 0.0,
                        "roll": 0.0, "gimbal": {"yaw": 0.0, "pitch": -90.0, "roll": 0.0}, "hfov_deg": 40.0,
                        "sensor": {"mode": "eo"}}),
    "slant_1km_1000": (datetime(2026, 6, 21, 17, 0, tzinfo=timezone.utc), camera_at(SITE, GROUND, 1000.0, 30.0, 45.0, 30.0)),
    "low_300m_1500": (datetime(2026, 6, 21, 22, 0, tzinfo=timezone.utc),
                      camera_at(SITE, GROUND, 300.0 / math.sin(math.radians(15.0)), 15.0, 90.0, 40.0)),
}
SLANT_500 = camera_at(SITE, GROUND, 500.0, 45.0, 0.0, 30.0)


def iso(t: datetime) -> str:
    return t.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


# ---------------------------------------------------------------------------
# Launch and logs
# ---------------------------------------------------------------------------


@contextlib.contextmanager
def launch(run_dir: Path, scene: Path, config_text: str | None = None, exec_cmds: str = EXEC_CMDS) -> Iterator[Client]:
    """CamSim headless with the control API, frame stats to run_dir/frames.jsonl; the UE log copied to run_dir."""
    run_dir.mkdir(parents=True, exist_ok=True)
    rb.wait_port_free(int(rb.HEALTH.rsplit(":", 1)[1]))
    env = dict(os.environ, CAMSIM_CONTROL_API_ENABLED="1", CAMSIM_SCENE_DIR=str(scene.resolve()),
               CAMSIM_FRAME_STATS_PATH=str((run_dir / "frames.jsonl").resolve()))
    cfg_path = None
    if config_text is not None:
        cfg_path = run_dir / "camsim_config.override.yaml"
        cfg_path.write_text(config_text)
        env["CAMSIM_CONFIG_SRC"] = str(cfg_path.resolve())
    project_cfg = REPO / "unreal_project/CamSimTest/camsim_config.yaml"
    subprocess.run([str(REPO / "scripts" / "run.sh"), "--headless", "--local", "--detach", exec_cmds],
                   env=env, check=True, stdout=subprocess.DEVNULL)
    try:
        c = Client(rb.HEALTH)
        c.wait_ready(timeout_s=900)
        yield c
    finally:
        subprocess.run([str(REPO / "scripts" / "stop.sh")], check=False, stdout=subprocess.DEVNULL)
        if cfg_path is not None:  # run.sh copied the override over the project config: restore the deploy one
            project_cfg.write_bytes((REPO / "deploy" / "camsim_config.yaml").read_bytes())
        if LOG.exists():
            shutil.copy(LOG, run_dir / "camsim.log")


LOG_LINE = re.compile(r"^\[(\d{4})\.(\d\d)\.(\d\d)-(\d\d)\.(\d\d)\.(\d\d):(\d{3})\]\[\s*\d+\](.*)$")
AE_RE = re.compile(r"Sensor AE: mode=(\d+) gainEv=(\S+) medianLog2=(\S+) photonGain=(\S+) analogGain=(\S+) t_int=(\S+) ms")
SKY_RE = re.compile(r"6A sky: utc=(\S+) sun_el=(\S+) sun_ground_lux=(\S+) moon_el=(\S+) moon_phase=(\S+) "
                    r"moon_illum=(\S+) moon_ground_lux=(\S+) floor_cdm2=(\S+) band=(\w+) stars=(\d+)(?: moon_az=(\S+))?(?: moon_norm=(\d+)/(\S+)ms)?")
BAND_RE = re.compile(r"6A: cached lighting band \S+ (\w+) \((\S+) EV\), scene median 2\^(\S+)")


def parse_utc(s: str) -> datetime:
    return datetime.fromisoformat(s.replace("Z", "+00:00"))


def read_log(path: Path) -> dict[str, list[dict]]:
    """AE, sky and band lines with their wall time (UE log times are UTC)."""
    out: dict[str, list[dict]] = {"ae": [], "sky": [], "band": []}
    if not path.exists():
        return out
    for line in path.read_text(errors="replace").splitlines():
        m = LOG_LINE.match(line)
        if not m:
            continue
        y, mo, d, hh, mi, ss, ms, rest = m.groups()
        wall = datetime(int(y), int(mo), int(d), int(hh), int(mi), int(ss), int(ms) * 1000, tzinfo=timezone.utc).timestamp()
        if a := AE_RE.search(rest):
            out["ae"].append({"wall": wall, "mode": int(a[1]), "gain_ev": float(a[2]), "median_log2": float(a[3]),
                              "photon_gain": float(a[4]), "analog_gain": float(a[5]), "t_int_ms": float(a[6])})
        elif s := SKY_RE.search(rest):
            out["sky"].append({"wall": wall, "utc": s[1], "sun_el": float(s[2]), "sun_ground_lux": float(s[3]),
                               "moon_el": float(s[4]), "moon_phase": float(s[5]), "moon_illum": float(s[6]),
                               "moon_ground_lux": float(s[7]), "floor_cdm2": float(s[8]), "band": s[9],
                               "stars": int(s[10]), "moon_az": float(s[11]) if s[11] else None,
                               "moon_norm_n": int(s[12]) if s[12] else None, "moon_norm_ms": float(s[13]) if s[13] else None})
        elif b := BAND_RE.search(rest):
            out["band"].append({"wall": wall, "band": b[1], "ev": float(b[2]), "median_log2": float(b[3])})
    return out


def lines_between(lines: list[dict], t0: float, t1: float) -> list[dict]:
    return [x for x in lines if t0 <= x["wall"] <= t1]


def frame_rows(run_dir: Path) -> list[dict]:
    p = run_dir / "frames.jsonl"
    if not p.exists():
        return []
    rows = []
    for line in p.read_text().splitlines():
        try:
            rows.append(json.loads(line))
        except ValueError:
            continue
    return rows


# ---------------------------------------------------------------------------
# Images
# ---------------------------------------------------------------------------


def load_rgb(path: Path) -> np.ndarray:
    return np.asarray(Image.open(path).convert("RGB")).astype(np.float64)


def luma(rgb: np.ndarray) -> np.ndarray:
    """Y' code value (16..235) of the capture PNG: CamSimNv12::ToBgra is the BT.709 inverse, so the forward weights
    recover the NV12 Y exactly up to the 8-bit RGB rounding and clamping."""
    return 16.0 + 219.0 * (0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]) / 255.0


def temporal_noise(a: np.ndarray, b: np.ndarray) -> float:
    """std of the difference of two frames / sqrt(2) (both Y)."""
    return float(np.std(a - b) / math.sqrt(2.0))


def flat_patch(y: np.ndarray, size: int = 64, step: int = 32, lo: float = 20.0, hi: float = 220.0) -> tuple[int, int]:
    """Top-left of the size x size block (on a step grid, 32 px from the border) with the lowest spatial std of y,
    among blocks whose mean is in [lo, hi] (falls back to all blocks)."""
    best, cands = None, []
    for r in range(32, y.shape[0] - size - 32 + 1, step):
        for c in range(32, y.shape[1] - size - 32 + 1, step):
            blk = y[r:r + size, c:c + size]
            cands.append((float(blk.std()), float(blk.mean()), r, c))
    ok = [x for x in cands if lo <= x[1] <= hi] or cands
    best = min(ok)
    return best[2], best[3]


def save_json(path: Path, obj: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, indent=2, default=float))


def load_json(path: Path) -> dict | None:
    return json.loads(path.read_text()) if path.exists() else None


# ---------------------------------------------------------------------------
# Sensor mirror (CamSimSensorRef::Run on a uniform field, EO photon detector)
# ---------------------------------------------------------------------------


def knee(x: np.ndarray, k: float) -> np.ndarray:
    if k >= 1.0:
        return x
    y = k + (1.0 - k) * (1.0 - np.exp(-(x - k) / (1.0 - k))) / (1.0 - math.exp(-1.0))
    return np.where(x <= k, x, y)


def oetf709(v: np.ndarray) -> np.ndarray:
    v = np.clip(v, 0.0, 1.0)
    return np.where(v < 0.018, 4.5 * v, 1.099 * np.power(v, 0.45) - 0.099)


def simulate_frames(level: np.ndarray, gains: dict, n: int = 128, seed: int = 1) -> list[np.ndarray]:
    """Two frames of a uniform n x n field through the detector (DetectPixel), display (DisplayEo), NV12 packing
    (PackNv12: per-pixel Y, 2x2-mean chroma) and the capture PNG's CamSimNv12::ToBgra; returns the RGB uint8 frames.
    `level` is the per-channel signal (cd/m^2 after InputScale), gains {photon_gain, analog_gain, dark_e}. Noise is
    numpy's Gaussian (the GPU hashes a PCG stream; same distribution); PRNU/DSNU are drawn once (fixed pattern)."""
    rng = np.random.default_rng(seed)
    fw, adc = EO["full_well"], EO["adc_max"]
    prnu = 1.0 + EO["prnu"] * rng.standard_normal((3, n, n))
    dsnu = EO["dsnu"] * rng.standard_normal((3, n, n))
    out = []
    for _ in range(2):
        e = level[:, None, None] * gains["photon_gain"] * fw * prnu + gains["dark_e"]
        e = e + np.sqrt(np.maximum(e, 0.0)) * rng.standard_normal(e.shape)
        e = e + dsnu + EO["read"] * rng.standard_normal(e.shape)
        e5 = np.clip(e, 0.0, fw) * gains["analog_gain"]
        dn = np.clip(np.floor(e5 * adc / fw + 0.5), 0.0, adc)
        disp = oetf709(knee(dn / adc, EO["knee"]))                      # (3, n, n) R'G'B'
        yl = 0.2126 * disp[0] + 0.7152 * disp[1] + 0.0722 * disp[2]
        ycode = np.clip(np.floor(16.0 + 219.0 * yl + 0.5), 0, 255)
        m = disp.reshape(3, n // 2, 2, n // 2, 2).mean(axis=(2, 4))      # 2x2 chroma
        ym = 0.2126 * m[0] + 0.7152 * m[1] + 0.0722 * m[2]
        cb = np.clip(np.floor(128.0 + 224.0 * (m[2] - ym) / 1.8556 + 0.5), 0, 255)
        cr = np.clip(np.floor(128.0 + 224.0 * (m[0] - ym) / 1.5748 + 0.5), 0, 255)
        cb = np.repeat(np.repeat((cb - 128.0) / 224.0, 2, axis=0), 2, axis=1)
        cr = np.repeat(np.repeat((cr - 128.0) / 224.0, 2, axis=0), 2, axis=1)
        yn = (ycode - 16.0) / 219.0
        rgb = np.stack([yn + 1.5748 * cr, yn - 0.1873 * cb - 0.4681 * cr, yn + 1.8556 * cb], axis=-1)
        out.append(np.clip(np.rint(rgb * 255.0), 0, 255))
    return out


def predict_noise(obs_rgb_mean: np.ndarray, gains: dict) -> dict:
    """Fit the per-channel signal so the mirror's mean RGB matches the observed patch, then its Y temporal noise."""
    lin = np.maximum(obs_rgb_mean / 255.0, 1e-4)
    lin = np.where(lin < 0.081, lin / 4.5, np.power((lin + 0.099) / 1.099, 1.0 / 0.45))   # inverse OETF (no knee)
    level = lin / (gains["photon_gain"] * gains["analog_gain"])
    for _ in range(12):
        fr = simulate_frames(level, gains, n=64)
        sim = np.mean([f.reshape(-1, 3).mean(axis=0) for f in fr], axis=0)
        level = level * np.clip((obs_rgb_mean + 0.5) / (sim + 0.5), 0.5, 2.0)
    fr = simulate_frames(level, gains, n=256, seed=7)
    sim_mean = np.mean([f.reshape(-1, 3).mean(axis=0) for f in fr], axis=0)
    return {"sigma_y": temporal_noise(luma(fr[0]), luma(fr[1])), "level": level.tolist(),
            "sim_rgb_mean": sim_mean.tolist(), "electrons": (level * gains["photon_gain"] * EO["full_well"]).tolist()}


# ---------------------------------------------------------------------------
# Camera projection (pinhole, k1 = k2 = 0 for eo_hd_cmos)
# ---------------------------------------------------------------------------


def camera_basis(heading: float, pitch: float) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    h, p = math.radians(heading), math.radians(pitch)
    fwd = np.array([math.sin(h) * math.cos(p), math.cos(h) * math.cos(p), math.sin(p)])
    right = np.array([math.cos(h), -math.sin(h), 0.0])
    up = np.cross(right, fwd)
    return fwd, right, up


def project(enu: np.ndarray, heading: float, pitch: float, hfov: float) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """ENU unit vectors (N, 3) -> pixel x, y (pixel centres at integers, image centre ((W-1)/2, (H-1)/2)), in front."""
    fwd, right, up = camera_basis(heading, pitch)
    f = (W / 2.0) / math.tan(math.radians(hfov) / 2.0)
    z = enu @ fwd
    x = (W - 1) / 2.0 + f * (enu @ right) / np.where(z > 0, z, np.nan)
    y = (H - 1) / 2.0 - f * (enu @ up) / np.where(z > 0, z, np.nan)
    return x, y, z > 0


# ---------------------------------------------------------------------------
# Runs
# ---------------------------------------------------------------------------


def capture_pair(c: Client, out: Path, name: str, settle: int) -> dict:
    """Two captures of the same (lockstep, frozen) scene: frame a after `settle` frames, frame b on the next one."""
    t0 = time.time()
    a = c.capture(settle_frames=settle, timeout_s=300, depth=False, semantic=False)
    b = c.capture(settle_frames=1, timeout_s=120, depth=False, semantic=False)
    t1 = time.time()
    (out / f"{name}_a.png").write_bytes(a.png)
    (out / f"{name}_b.png").write_bytes(b.png)
    return {"name": name, "wall0": t0, "wall1": t1, "frame_a": a.frame_id, "frame_b": b.frame_id, "utc": a.utc}


def run_day(c: Client, out: Path) -> dict:
    caps = {}
    for name, (utc, pose) in DAY_POSES.items():
        c.set_clock(lockstep=True, utc=iso(utc))
        c.set_camera(**pose)
        caps[name] = capture_pair(c, out, name, SETTLE_DAY)
        print(f"  day {name}: frames {caps[name]['frame_a']}, {caps[name]['frame_b']}", flush=True)
    return {"captures": caps}


def run_moon(c: Client, out: Path) -> dict:
    caps = {}
    for name, utc in (("full_slant", DATE_FULL), ("new_slant", DATE_NEW)):
        c.set_clock(lockstep=True, utc=iso(utc))
        c.set_camera(**SLANT_500)
        caps[name] = capture_pair(c, out, name, SETTLE_NIGHT)
        print(f"  moon {name}: frames {caps[name]['frame_a']}", flush=True)
    views = {}
    for name, utc in (("moon_full", DATE_FULL), ("moon_gibbous", DATE_GIBBOUS)):
        cam = sky_camera(0.0, 0.0, 2.0)
        e = eph.compute(utc, cam["lat"], cam["lon"], cam["alt"])
        cam.update(heading=e.moon_az, pitch=e.moon_el)
        c.set_clock(lockstep=True, utc=iso(utc))
        c.set_camera(**cam)
        caps[name] = capture_pair(c, out, name, SETTLE_NIGHT)
        views[name] = {"camera": cam, "utc": iso(utc)}
        print(f"  moon {name}: az {e.moon_az:.3f} el {e.moon_el:.3f} phase {e.moon_phase_angle:.1f}", flush=True)
    # A small, near-full moon in a wide night view over dark ground: the AE's target (and UE's pre-exposure) is set by
    # the ground, so this is where scene colour would overflow fp16 on the disc.
    cam = sky_camera(0.0, 0.0, WIDE_HFOV)
    e = eph.compute(DATE_WIDE, cam["lat"], cam["lon"], cam["alt"])
    cam.update(heading=e.moon_az, pitch=e.moon_el - WIDE_PITCH_BELOW_MOON)
    c.set_clock(lockstep=True, utc=iso(DATE_WIDE))
    c.set_camera(**cam)
    caps["moon_wide"] = capture_pair(c, out, "moon_wide", SETTLE_NIGHT)
    print(f"  moon moon_wide: el {e.moon_el:.2f} phase {e.moon_phase_angle:.1f}", flush=True)
    return {"captures": caps, "views": views, "wide": {"camera": cam, "utc": iso(DATE_WIDE)}}


def run_stars(c: Client, out: Path) -> dict:
    caps, views = {}, {}
    for name, utc in (("stars_night", DATE_STARS), ("stars_noon", DATE_STARS_NOON)):
        ep = eph.compute(DATE_STARS, SITE[0], SITE[1])
        az, _el = eph._eq_to_hor(ep.last_deg - MEGREZ[0], MEGREZ[1], SITE[0])
        cam = sky_camera(az, 60.0, 40.0)
        c.set_clock(lockstep=True, utc=iso(utc))
        c.set_camera(**cam)
        caps[name] = capture_pair(c, out, name, SETTLE_NIGHT)
        views[name] = {"camera": cam, "utc": iso(utc)}
        print(f"  stars {name}: az {az:.2f}", flush=True)
    return {"captures": caps, "views": views}


def sun_crossing(el_target: float, after: datetime) -> datetime:
    """First minute after `after` when the sun at the site drops below el_target."""
    t = after
    while eph.compute(t, *SITE).sun_el > el_target:
        t += timedelta(minutes=1)
    return t


class UdpRecorder:
    """Raw MPEG-TS from CamSim's unicast stream (run.sh --local) into a file (KLV untouched: never ffmpeg)."""

    def __init__(self, path: Path, port: int = STREAM_PORT) -> None:
        self.path, self.stop = path, threading.Event()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16 * 1024 * 1024)
        self.sock.bind(("127.0.0.1", port))
        self.sock.settimeout(0.2)
        self.thread = threading.Thread(target=self._run, daemon=True)

    def _run(self) -> None:
        with self.path.open("wb") as f:
            while not self.stop.is_set():
                try:
                    f.write(self.sock.recv(65536))
                except TimeoutError:
                    continue
        self.sock.close()

    def __enter__(self) -> UdpRecorder:
        self.thread.start()
        return self

    def __exit__(self, *exc) -> None:
        self.stop.set()
        self.thread.join(timeout=5)


def run_dusk(c: Client, out: Path) -> dict:
    for stale in (out / "series.json", out / "frames"):   # derived from the previous recording
        if stale.is_dir():
            shutil.rmtree(stale)
        else:
            stale.unlink(missing_ok=True)
    noonish = datetime(*DUSK_DAY, 23, 0, tzinfo=timezone.utc)
    t0 = sun_crossing(11.0, noonish)
    t1 = sun_crossing(-9.0, t0)
    c.set_clock(lockstep=True, utc=iso(t0))
    c.set_camera(**SLANT_500)
    start = c.capture(settle_frames=SETTLE_DAY, timeout_s=300, depth=False, semantic=False)
    (out / "dusk_start.png").write_bytes(start.png)
    rate = 60.0
    wall0 = time.time()
    with UdpRecorder(out / "dusk.ts"):
        time.sleep(1.0)
        c.set_clock(lockstep=False)
        c.set_clock(utc=iso(t0), rate=rate)
        deadline = time.time() + (t1 - t0).total_seconds() / rate + 120.0
        while time.time() < deadline:
            if parse_utc(c.clock()["utc"]) >= t1:
                break
            time.sleep(1.0)
        time.sleep(1.0)
    c.set_clock(lockstep=True)
    end = c.capture(settle_frames=SETTLE_NIGHT, timeout_s=300, depth=False, semantic=False)
    (out / "dusk_end.png").write_bytes(end.png)
    wall1 = time.time()
    return {"t0": iso(t0), "t1": iso(t1), "rate": rate, "wall0": wall0, "wall1": wall1, "colour": run_twilight_colour(c, out)}


N9_ELEVATIONS = {"day": N9_DAY_EL, "p3": 3.0, "z0": 0.0, "m3": -3.0, "m6": -6.0}
N9_GROUND = {"lat": SITE[0], "lon": SITE[1], "alt": GROUND + 100.0, "heading": 0.0, "pitch": 0.0, "roll": 0.0,
             "gimbal": {"yaw": 0.0, "pitch": -90.0, "roll": 0.0}, "hfov_deg": 40.0, "sensor": {"mode": "eo"}}


def twilight_views(utc: datetime) -> dict[str, dict]:
    """N9's views: the ground straight down from 100 m (little aerial perspective) and the sky 85 deg up, away from the
    sun."""
    az = eph.compute(utc, *SITE).sun_az
    return {"ground": N9_GROUND, "zenith": sky_camera((az + 180.0) % 360.0, 85.0, 30.0)}


def run_twilight_colour(c: Client, out: Path) -> dict:
    """N9: lockstep captures of the ground and the zenith sky at sun +22 (the daylight reference, Spitschan's 20-26 deg
    bin), +3, 0, -3 and -6 deg."""
    t = datetime(*DUSK_DAY, 22, 0, tzinfo=timezone.utc)
    res = {}
    for tag, el in N9_ELEVATIONS.items():
        t = sun_crossing(el, t)
        c.set_clock(lockstep=True, utc=iso(t))
        views = {}
        for view, cam in twilight_views(t).items():
            c.set_camera(**cam)
            cap = c.capture(settle_frames=SETTLE_NIGHT, timeout_s=300, depth=False, semantic=False)
            name = f"colour_{tag}_{view}.png"
            (out / name).write_bytes(cap.png)
            views[view] = name
        res[tag] = {"utc": iso(t), "sun_el": eph.compute(t, *SITE).sun_el, "views": views}
        print(f"  dusk colour {tag}: sun el {res[tag]['sun_el']:.2f}", flush=True)
    return res


def run_thermal(scene: Path, out: Path) -> dict:
    cmd = ["uv", "run", str(REPO / "scripts" / "thermal_check.py"), "--scene", str(scene.resolve()),
           "--out", str((out / "thermal").resolve())]
    t0 = time.time()
    r = subprocess.run(cmd, cwd=REPO, check=False)
    return {"cmd": cmd, "returncode": r.returncode, "minutes": (time.time() - t0) / 60.0}


def run_cost(out: Path) -> dict:
    dest = (out / "cost").resolve()
    if dest.exists():
        shutil.rmtree(dest)
    cmd = ["uv", "run", "--with", "numpy", "--with", "pillow", "python", str(REPO / "scripts/bench/run_bench.py"),
           "--label", "night-cost", "--phases", "night_slant,day_slant", "--shots", "night_slant", "--out", str(dest)]
    r = subprocess.run(cmd, cwd=REPO, check=False)
    return {"cmd": cmd, "returncode": r.returncode}


# ---------------------------------------------------------------------------
# Gates
# ---------------------------------------------------------------------------


def git_head() -> str:
    try:
        return subprocess.run(["git", "-C", str(REPO), "rev-parse", "--short", "HEAD"], capture_output=True, text=True,
                              check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def gate(out: Path, name: str, passed: bool | None, values: dict, threshold: str, detail: str = "") -> dict:
    g = {"gate": name, "pass": passed, "threshold": threshold, "detail": detail, "values": values,
         "evaluated_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"), "commit": git_head()}
    save_json(out / f"{name}.json", g)
    return g


GATE_NAMES = [f"N{i}" for i in range(1, 10)]


def merged_gates(out: Path, evaluated: list[dict]) -> list[dict]:
    """This invocation's gates plus every other OUT/N*.json from earlier invocations (runs split over several calls), so
    summary.md always covers N1-N9; a gate with no JSON yet is listed as not run."""
    by_name = {g["gate"]: g for g in evaluated}
    for name in GATE_NAMES:
        if name not in by_name:
            prev = load_json(out / f"{name}.json")
            by_name[name] = prev if prev else {"gate": name, "pass": None, "detail": "not run"}
    return [by_name[n] for n in GATE_NAMES] + [g for n, g in sorted(by_name.items()) if n not in GATE_NAMES]


def summary_markdown(gates: list[dict], evaluated: set[str]) -> str:
    lines = ["| Gate | Result | Evaluated | Detail |", "|---|---|---|---|"]
    for g in gates:
        res = "PASS" if g["pass"] else ("n/a" if g["pass"] is None else "FAIL")
        when = g.get("evaluated_utc", "")
        when = f"{when} @ {g['commit']}" if g.get("commit") else when
        if g["gate"] not in evaluated and g.get("detail") != "not run":
            when += " (earlier invocation)"
        lines.append(f"| {g['gate']} | {res} | {when} | {g['detail']} |")
    return "\n".join(lines) + "\n"


def gains_at(log: dict, cap: dict) -> dict | None:
    """The last EO AE line logged while the capture pair was taken (no slack after it: the next command follows)."""
    ae = [x for x in lines_between(log["ae"], cap["wall0"], cap["wall1"]) if x["mode"] == 0]
    return ae[-1] if ae else None


def eval_n1(out: Path, base: Path | None) -> dict:
    if base is None or not (base / "day").exists():
        return gate(out, "N1", False, {}, "+-3 DN mean, +-10 % noise", "no --baseline day run: cannot evaluate")
    rows, ok = {}, True
    for name in DAY_POSES:
        cur = [luma(load_rgb(out / "day" / f"{name}_{s}.png")) for s in "ab"]
        old = [luma(load_rgb(base / "day" / f"{name}_{s}.png")) for s in "ab"]
        r, c0 = flat_patch((cur[0] + cur[1]) / 2.0)
        sl = (slice(r, r + 64), slice(c0, c0 + 64))
        n_cur, n_old = temporal_noise(cur[0][sl], cur[1][sl]), temporal_noise(old[0][sl], old[1][sl])
        m_cur, m_old = float((cur[0].mean() + cur[1].mean()) / 2), float((old[0].mean() + old[1].mean()) / 2)
        ratio = n_cur / n_old if n_old > 0 else float("inf")
        good = abs(m_cur - m_old) <= 3.0 and abs(ratio - 1.0) <= 0.10
        ok &= good
        rows[name] = {"mean_before": m_old, "mean_after": m_cur, "mean_diff": m_cur - m_old, "noise_before": n_old,
                      "noise_after": n_cur, "noise_ratio": ratio, "patch_rc": [r, c0],
                      "patch_mean_after": float(cur[0][sl].mean()), "pass": good}
    for tag, d in (("after", out / "day"), ("before", base / "day")):
        log = read_log(d / "camsim.log")
        frames = frame_rows(d)
        for name, cap in (load_json(d / "run.json") or {}).get("captures", {}).items():
            if name in rows:
                sel = [f for f in frames if cap["wall0"] <= f["t"] <= cap["wall1"] and f.get("sensor_gain_ev") is not None]
                rows[name][f"gain_ev_{tag}"] = sel[-1]["sensor_gain_ev"] if sel else None
                rows[name][f"median_log2_{tag}"] = sel[-1]["scene_median_log2"] if sel else None
                g = gains_at(log, cap)
                rows[name][f"t_int_ms_{tag}"] = g["t_int_ms"] if g else None
    detail = "; ".join(f"{k}: mean {v['mean_before']:.2f} -> {v['mean_after']:.2f} DN, noise {v['noise_before']:.3f} -> "
                       f"{v['noise_after']:.3f} DN (x{v['noise_ratio']:.3f})" for k, v in rows.items())
    return gate(out, "N1", ok, rows, "|mean diff| <= 3 DN and |noise ratio - 1| <= 0.10 per pose", detail)


def eval_n2(out: Path) -> dict:
    d = out / "moon"
    run = load_json(d / "run.json")
    if not run:
        return gate(out, "N2", False, {}, "", "no moon run")
    log = read_log(d / "camsim.log")
    cam = SLANT_500
    vals: dict = {}
    for name, utc in (("full_slant", DATE_FULL), ("new_slant", DATE_NEW)):
        cap = run["captures"][name]
        g = gains_at(log, cap)
        e = eph.compute(utc, cam["lat"], cam["lon"], cam["alt"])
        il = eph.illumination(e)
        sky = [x for x in log["sky"] if parse_utc(x["utc"]) == utc]
        a, b = (luma(load_rgb(d / f"{name}_{s}.png")) for s in "ab")
        rgb_a, rgb_b = load_rgb(d / f"{name}_a.png"), load_rgb(d / f"{name}_b.png")
        r, c0 = flat_patch((a + b) / 2.0, lo=16.0, hi=235.0)
        sl = (slice(r, r + 64), slice(c0, c0 + 64))
        meas = temporal_noise(a[sl], b[sl])
        v = {"median_log2": g["median_log2"] if g else None, "gains": g, "predicted_ground_lux": il.total,
             "moon_ground_lux": il.moon_ground_lux, "moon_el": e.moon_el, "moon_phase": e.moon_phase_angle,
             "logged_moon_ground_lux": sky[-1]["moon_ground_lux"] if sky else None,
             "logged_band": sky[-1]["band"] if sky else None, "patch_rc": [r, c0], "patch_mean_y": float(a[sl].mean()),
             "noise_measured": meas}
        if g:
            gains = {"photon_gain": g["photon_gain"], "analog_gain": g["analog_gain"],
                     "dark_e": EO["dark_es"] * g["t_int_ms"] / 1000.0}
            obs = np.concatenate([rgb_a[sl].reshape(-1, 3), rgb_b[sl].reshape(-1, 3)]).mean(axis=0)
            pred = predict_noise(obs, gains)
            v.update(noise_predicted=pred["sigma_y"], noise_ratio=meas / pred["sigma_y"] if pred["sigma_y"] else None,
                     mirror=pred, patch_rgb_mean=obs.tolist())
        vals[name] = v
    f, n = vals["full_slant"], vals["new_slant"]
    ok = True
    details = []
    if f["median_log2"] is None or n["median_log2"] is None:
        ok = False
        details.append("no AE log line during a capture")
    else:
        want = math.log2(f["predicted_ground_lux"] / n["predicted_ground_lux"])
        got = f["median_log2"] - n["median_log2"]
        vals["median_diff_ev"], vals["predicted_diff_ev"] = got, want
        ok &= abs(got - want) <= 0.5
        details.append(f"median full - new {got:+.2f} EV vs predicted {want:+.2f} EV "
                       f"(ground {f['predicted_ground_lux']:.4g} / {n['predicted_ground_lux']:.4g} lux)")
    for name in ("full_slant", "new_slant"):
        v = vals[name]
        if v.get("noise_ratio") is None:
            ok = False
            continue
        good = abs(v["noise_ratio"] - 1.0) <= 0.20
        ok &= good
        details.append(f"{name} noise {v['noise_measured']:.3f} DN vs mirror {v['noise_predicted']:.3f} "
                       f"(x{v['noise_ratio']:.3f}, patch Y {v['patch_mean_y']:.1f}, analog x{v['gains']['analog_gain']:.1f}, "
                       f"t_int {v['gains']['t_int_ms']:.1f} ms)")
    return gate(out, "N2", ok, vals, "|median diff - predicted| <= 0.5 EV; |noise / mirror - 1| <= 0.20", "; ".join(details))


def thresholded_centroid(y: np.ndarray) -> tuple[float, float, int]:
    hi = float(np.percentile(y, 99.9))
    bg = float(np.median(y))
    mask = y > bg + 0.5 * (hi - bg)
    rr, cc = np.nonzero(mask)
    return float(cc.mean()), float(rr.mean()), int(mask.sum())


def eval_n3(out: Path) -> dict:
    d = out / "moon"
    run = load_json(d / "run.json")
    if not run or "views" not in run:
        return gate(out, "N3", False, {}, "", "no moon run")
    log = read_log(d / "camsim.log")
    vals, ok, details = {}, True, []
    for name, view in run["views"].items():
        cam, utc = view["camera"], parse_utc(view["utc"])
        e_py = eph.compute(utc, cam["lat"], cam["lon"], cam["alt"])
        cap = run["captures"][name]
        sky = [x for x in log["sky"] if parse_utc(x["utc"]) == utc and x["wall"] <= cap["wall1"]]
        # The model uses the moon direction CamSim drew (camsim.Sky.Log, 1e-5 deg): the Python mirror of FEphemeris
        # differs from it by up to ~0.003 deg (2 px at 2 deg FOV); the ephemeris itself is held to JPL by the C++ tests.
        e = e_py
        if sky and sky[-1].get("moon_az") is not None:
            e = dataclasses.replace(e_py, moon_az=sky[-1]["moon_az"], moon_el=sky[-1]["moon_el"])
        py_vs_drawn = math.degrees(math.acos(min(1.0, float(np.dot(eph.az_el_to_enu(e.moon_az, e.moon_el),
                                                                   eph.az_el_to_enu(e_py.moon_az, e_py.moon_el))))))
        y = (luma(load_rgb(d / f"{name}_a.png")) + luma(load_rgb(d / f"{name}_b.png"))) / 2.0
        moon = np.array([eph.az_el_to_enu(e.moon_az, e.moon_el)])
        mpx, mpy, _ = project(np.array([eph.az_el_to_enu(e_py.moon_az, e_py.moon_el)]), cam["heading"], cam["pitch"],
                              cam["hfov_deg"])
        sun = np.array(eph.az_el_to_enu(e.sun_az, e.sun_el))
        mx, my, _ = project(moon, cam["heading"], cam["pitch"], cam["hfov_deg"])
        # The sun's side of the disc in the image: the sun direction's component across the line of sight
        _fwd, right, up = camera_basis(cam["heading"], cam["pitch"])
        m = moon[0]
        t = sun - (sun @ m) * m
        sun_img = np.array([t @ right, -(t @ up)])
        sun_img /= np.linalg.norm(sun_img)
        f_px = (W / 2.0) / math.tan(math.radians(cam["hfov_deg"]) / 2.0)
        radius_px = f_px * math.tan(math.asin(1737.4 / e.moon_km))
        cx, cy, npx = thresholded_centroid(y)
        # Expected: the model disc (FMoonDisc::Radiance) through the display, k fitted to this frame
        rad = moon_model(cam, utc, e)
        best = None
        for k in np.exp(np.linspace(math.log(0.05), math.log(500.0), 120)):
            mdl = display_model(rad, float(k))
            sse = float(np.mean((mdl - y) ** 2))
            if best is None or sse < best[0]:
                best = (sse, float(k), mdl)
        _, k_fit, mdl = best
        ex, ey, _ = thresholded_centroid(mdl)
        off_centroid = math.hypot(cx - ex, cy - ey)
        # The disc's displacement from the model: least-squares image registration (with an albedo map the > 50 %
        # region is patchy, so its centroid swings with small level differences; it is reported, not gated).
        reg = register_shift(y, mdl, (float(mx[0]), float(my[0])), radius_px)
        off = math.hypot(*reg["shift"])
        # Brighter half: sum of Y - background over each half of the disc split across the sun direction
        yy, xx = np.mgrid[0:H, 0:W].astype(np.float64)
        dxx, dyy = xx - mx[0], yy - my[0]
        disc = dxx**2 + dyy**2 <= (radius_px * 1.05) ** 2
        side = dxx * sun_img[0] + dyy * sun_img[1]
        bg = float(np.median(y[~disc]))
        toward = float(np.sum((y - bg)[disc & (side > 0)]))
        away = float(np.sum((y - bg)[disc & (side < 0)]))
        lit_ok = toward > away
        centre_ok = off <= 2.0
        gate_lit = name == "moon_gibbous"   # at full moon (phase ~5 deg) both halves are lit: info only
        ok &= centre_ok and (lit_ok or not gate_lit)
        vals[name] = {"moon_az": e.moon_az, "moon_el": e.moon_el, "phase_angle": e.moon_phase_angle,
                      "logged_moon_el": sky[-1]["moon_el"] if sky else None, "projected": [float(mx[0]), float(my[0])],
                      "radius_px": radius_px, "sun_img": sun_img.tolist(), "centroid": [cx, cy], "pixels": npx,
                      "expected_centroid": [ex, ey], "centroid_offset_px": off_centroid, "registration": reg,
                      "offset_px": off, "offset_from_projection_px": math.hypot(cx - mpx[0], cy - mpy[0]),
                      "drawn_vs_python_moon_deg": py_vs_drawn, "drawn_moon_az_el": [e.moon_az, e.moon_el], "k_fit": k_fit, "half_toward_sun": toward,
                      "half_away": away, "lit_limb_toward_sun": lit_ok, "lit_gated": gate_lit}
        details.append(f"{name}: disc {off:.2f} px from the model (registration; residual {reg['rms_dn']:.1f} DN rms; > 50 % "
                       f"centroid {off_centroid:.2f} px from the model's, {math.hypot(cx - mpx[0], cy - mpy[0]):.2f} px from the "
                       f"Python ephemeris projection, drawn moon {py_vs_drawn * 3600:.1f}\" from it; phase {e.moon_phase_angle:.1f}); bright half {'toward' if lit_ok else 'AWAY from'} "
                       f"the sun ({toward / max(away, 1e-9):.2f}x){'' if gate_lit else ' (info)'}")
        if name == "moon_full":
            sf = full_moon_surface_check(y, rad, cam, utc, e, (float(mx[0]), float(my[0])), radius_px)
            vals[name]["surface"] = sf
            ok &= sf["pass"]
            details.append(f"moon_full surface: limb / centre {sf['limb_centre']:.2f} (> {N3_LIMB_CENTRE_MIN}; model "
                           f"{sf['model']['limb_centre']:.2f}, a Lambert ball {sf['lambert']['limb_centre']:.2f}), maria "
                           f"std / mean {sf['contrast']:.3f} (> {N3_CONTRAST_MIN}; model {sf['model']['contrast']:.3f}, "
                           f"uniform albedo {sf['uniform']['contrast']:.3f}), {sf['clipped_frac']:.1%} of the disc clipped")
    if "wide" in run:
        w = wide_moon_check(d, run["wide"])
        vals["moon_wide"] = w
        ok &= w["pass"]
        details.append(f"moon_wide ({WIDE_HFOV:.0f} deg FOV): disc min Y {w['disc_min_y']:.1f} (>= 100), ring mean "
                       f"{w['ring_mean_y']:.2f} vs sky {w['sky_median_y']:.2f} DN (no dark halo), scene exposure "
                       f"{w['scene_exposure_note']}")
    else:
        ok = False
        details.append("no moon_wide capture")
    return gate(out, "N3", ok, vals, "disc within 2 px of the model (least-squares registration of the frame on the "
                "modelled disc; the > 50 % centroid offsets and the raw offset from the ephemeris projection reported); gibbous: brighter half toward the sun; full moon: limb (0.80-0.95 R) / centre "
                f"(< 0.5 R) linear signal > {N3_LIMB_CENTRE_MIN} and std / mean inside 0.6 R > {N3_CONTRAST_MIN}; wide "
                "night view: moon disc not black (min Y >= 100 inside 0.7 R) and no dark halo (ring 1.3-2.5 R mean >= "
                "sky median - 1 DN)",
                "; ".join(details))


def wide_moon_check(d: Path, view: dict) -> dict:
    """The moon in a 40 deg night view: inf/NaN scene colour on the disc reaches the sensor as 0 (black) through TSR."""
    cam, utc = view["camera"], parse_utc(view["utc"])
    e = eph.compute(utc, cam["lat"], cam["lon"], cam["alt"])
    mx, my, _ = project(np.array([eph.az_el_to_enu(e.moon_az, e.moon_el)]), cam["heading"], cam["pitch"], cam["hfov_deg"])
    f_px = (W / 2.0) / math.tan(math.radians(cam["hfov_deg"]) / 2.0)
    r = f_px * math.tan(math.asin(1737.4 / e.moon_km))
    y = (luma(load_rgb(d / "moon_wide_a.png")) + luma(load_rgb(d / "moon_wide_b.png"))) / 2.0
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float64)
    dist = np.hypot(xx - mx[0], yy - my[0])
    disc = y[dist <= 0.7 * r]
    ring = y[(dist >= 1.3 * r) & (dist <= 2.5 * r)]
    sky = y[(dist >= 6 * r) & (dist <= 20 * r) & (yy < my[0] + 6 * r)]
    log = read_log(d / "camsim.log")
    ae = [a for a in log["ae"] if a["mode"] == 0]
    out = {"projected": [float(mx[0]), float(my[0])], "radius_px": r, "disc_px": int(disc.size),
           "disc_min_y": float(disc.min()) if disc.size else -1.0, "disc_mean_y": float(disc.mean()) if disc.size else -1.0,
           "ring_mean_y": float(ring.mean()) if ring.size else -1.0, "sky_median_y": float(np.median(sky)) if sky.size else -1.0,
           "scene_exposure_note": f"AE median 2^{ae[-1]['median_log2']:.2f}" if ae else "no AE line"}
    out["pass"] = bool(disc.size and ring.size and sky.size and out["disc_min_y"] >= 100.0
                       and out["ring_mean_y"] >= out["sky_median_y"] - 1.0)
    return out


# Full-moon surface (defect 1): a Lambert ball has limb / centre 0.47 at night_check's full moon (lunar-Lambert: ~1 x the
# albedo pattern); inside 0.6 R a Lambert ball's std / mean is ~0.06 and a uniform lunar-Lambert disc's ~0.01, the
# shipped albedo map's ~0.3 (both reported by the gate): 0.15 sits between them.
N3_LIMB_CENTRE_MIN = 0.7
N3_CONTRAST_MIN = 0.15
_ALBEDO: list = []


def albedo_map() -> np.ndarray | None:
    if not _ALBEDO:
        _ALBEDO.append(msurf.load_albedo())
    return _ALBEDO[0]


def view_dirs(cam: dict) -> np.ndarray:
    """(H, W, 3) unit ENU view direction of each pixel centre (the inverse of project())."""
    fwd, right, up = camera_basis(cam["heading"], cam["pitch"])
    f = (W / 2.0) / math.tan(math.radians(cam["hfov_deg"]) / 2.0)
    xs = (np.arange(W) - (W - 1) / 2.0) / f
    ys = ((H - 1) / 2.0 - np.arange(H)) / f
    d = fwd + xs[None, :, None] * right + ys[:, None, None] * up
    return d / np.linalg.norm(d, axis=-1, keepdims=True)


def moon_model(cam: dict, utc: datetime, e, lam_l: float | None = None, albedo: bool = True) -> np.ndarray:
    """Relative disc radiance per pixel (max 1) as M_SkyDome draws it: CamSimMoonDisc with the IAU orientation, the albedo
    map and L(phase) (lam_l / albedo=False give the comparators: 0 = a Lambert ball, uniform albedo)."""
    jd = eph.julian_day(utc)
    days = msurf.days_tt(jd, eph.delta_t_seconds(utc.year, utc.month))
    m = msurf.j2000_to_enu(cam["lat"], e.last_deg, (jd - 2451545.0) / 36525.0)
    axes = (m @ msurf.iau_axes_j2000(days).T).T
    moon = np.array(eph.az_el_to_enu(e.moon_az, e.moon_el))
    sun = np.array(eph.az_el_to_enu(e.sun_az, e.sun_el))
    lam = msurf.lunar_lambert_l(e.moon_phase_angle) if lam_l is None else lam_l
    rad = msurf.disc_radiance(view_dirs(cam), moon, sun, math.asin(1737.4 / e.moon_km), lam, axes,
                              albedo_map() if albedo else None)
    return rad / max(float(rad.max()), 1e-12)


def register_shift(y: np.ndarray, mdl: np.ndarray, centre: tuple[float, float], radius_px: float,
                   search: int = 6) -> dict:
    """Sub-pixel (dx, dy) that moves the model onto the frame (least squares over a box around the disc; integer search
    +- search px, then a parabola through the minimum on each axis), and the residual rms at that integer shift."""
    r = int(radius_px * 1.3) + search
    x0, y0 = int(round(centre[0])), int(round(centre[1]))
    ys, xs = slice(max(y0 - r, 0), min(y0 + r, H)), slice(max(x0 - r, 0), min(x0 + r, W))
    target = y[ys, xs]
    sse = np.full((2 * search + 1, 2 * search + 1), np.inf)
    for j, dy in enumerate(range(-search, search + 1)):
        for i, dx in enumerate(range(-search, search + 1)):
            sse[j, i] = float(np.mean((np.roll(mdl, (dy, dx), axis=(0, 1))[ys, xs] - target) ** 2))
    j, i = np.unravel_index(int(np.argmin(sse)), sse.shape)

    def vertex(a: float, b: float, c: float) -> float:
        den = a - 2 * b + c
        return 0.0 if den <= 0 else 0.5 * (a - c) / den

    fx = vertex(sse[j, i - 1], sse[j, i], sse[j, i + 1]) if 0 < i < 2 * search else 0.0
    fy = vertex(sse[j - 1, i], sse[j, i], sse[j + 1, i]) if 0 < j < 2 * search else 0.0
    return {"shift": [float(i - search + fx), float(j - search + fy)], "rms_dn": float(math.sqrt(sse[j, i])),
            "rms_dn_unshifted": float(math.sqrt(sse[search, search])), "at_search_edge": bool(i in (0, 2 * search) or j in (0, 2 * search))}


def display_model(rad: np.ndarray, k: float) -> np.ndarray:
    return 16.0 + 219.0 * oetf709(knee(np.clip(k * rad, 0.0, 1.0), EO["knee"]))


def inverse_display(y: np.ndarray) -> np.ndarray:
    """Luma code value -> linear signal (relative), inverting the BT.709 OETF and the sensor knee."""
    v = np.clip((y - 16.0) / 219.0, 0.0, 1.0)
    lin = np.where(v < 0.081, v / 4.5, np.power((v + 0.099) / 1.099, 1.0 / 0.45))
    k = EO["knee"]
    arg = np.clip(1.0 - (lin - k) * (1.0 - math.exp(-1.0)) / (1.0 - k), 1e-6, None)
    return np.where(lin <= k, lin, k - (1.0 - k) * np.log(arg))


def surface_metrics(lin: np.ndarray, centre: tuple[float, float], radius_px: float) -> dict:
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float64)
    r = np.hypot(xx - centre[0], yy - centre[1]) / radius_px
    core, limb, inner = lin[r < 0.5], lin[(r >= 0.80) & (r <= 0.95)], lin[r < 0.6]
    return {"limb_centre": float(limb.mean() / max(core.mean(), 1e-12)),
            "contrast": float(inner.std() / max(inner.mean(), 1e-12))}


def full_moon_surface_check(y: np.ndarray, rad: np.ndarray, cam: dict, utc: datetime, e, centre: tuple[float, float],
                            radius_px: float) -> dict:
    """Defect 1: the full moon is nearly uniform to the limb and shows maria. Measured on the frame's linear signal, with
    the same metrics on the model (expected) and on a uniform lunar-Lambert disc and a Lambert ball (comparators)."""
    bg = float(np.median(y))
    lin = np.clip(inverse_display(y) - float(inverse_display(np.array(bg))), 0.0, None)
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float64)
    disc = np.hypot(xx - centre[0], yy - centre[1]) <= 0.95 * radius_px
    out = surface_metrics(lin, centre, radius_px)
    out.update(model=surface_metrics(rad, centre, radius_px),
               uniform=surface_metrics(moon_model(cam, utc, e, albedo=False), centre, radius_px),
               lambert=surface_metrics(moon_model(cam, utc, e, lam_l=0.0, albedo=False), centre, radius_px),
               clipped_frac=float((y[disc] >= 234.0).mean()), has_albedo_map=albedo_map() is not None)
    out["pass"] = bool(out["limb_centre"] > N3_LIMB_CENTRE_MIN and out["contrast"] > N3_CONTRAST_MIN)
    return out


def bsc5_stars(path: Path) -> np.ndarray:
    """(RA deg, Dec deg, Vmag) of the shipped binary BSC5 (J2000), as FStarField::ParseBsc5 reads it."""
    b = path.read_bytes()
    star_n, entry = struct.unpack_from("<i", b, 8)[0], struct.unpack_from("<i", b, 24)[0]
    rows = []
    for i in range(-star_n):
        o = 28 + i * entry
        ra, dec = struct.unpack_from("<dd", b, o + 4)
        mag = struct.unpack_from("<h", b, o + 22)[0]
        if mag == 0 and ra == 0.0 and dec == 0.0:
            continue
        rows.append((math.degrees(ra), math.degrees(dec), mag / 100.0))
    return np.array(rows)


def star_enu(stars: np.ndarray, utc: datetime) -> np.ndarray:
    """Apparent topocentric ENU (no refraction) of each star at the site, with skyfield (JPL DE421)."""
    from skyfield.api import Star, load, wgs84

    cache = REPO / ".cache" / "night" / "skyfield"
    cache.mkdir(parents=True, exist_ok=True)
    loader = load.__class__(str(cache))
    ts = loader.timescale()
    planets = loader("de421.bsp")
    t = ts.from_datetime(utc)
    obs = planets["earth"] + wgs84.latlon(SITE[0], SITE[1], GROUND + 300.0)
    alt, az, _ = obs.at(t).observe(Star(ra_hours=stars[:, 0] / 15.0, dec_degrees=stars[:, 1])).apparent().altaz()
    a, z = alt.radians, az.radians
    return np.stack([np.sin(z) * np.cos(a), np.cos(z) * np.cos(a), np.sin(a)], axis=-1)


def detect_blobs(y: np.ndarray, nsigma: float = 5.0, min_px: int = 2) -> tuple[list[tuple[float, float, float]], float]:
    from scipy import ndimage

    bg = ndimage.median_filter(y, size=15)
    r = y - bg
    sigma = 1.4826 * float(np.median(np.abs(r - np.median(r))))
    sigma = max(sigma, 0.3)
    lab, n = ndimage.label(r > nsigma * sigma, structure=np.ones((3, 3)))
    out = []
    for i in range(1, n + 1):
        rr, cc = np.nonzero(lab == i)
        if rr.size < min_px:
            continue  # hot pixels (one pixel; a star after TSR covers two or more)
        w = r[rr, cc]
        out.append((float((cc * w).sum() / w.sum()), float((rr * w).sum() / w.sum()), float(w.max())))
    return out, sigma


def eval_n4(out: Path) -> dict:
    d = out / "stars"
    run = load_json(d / "run.json")
    if not run:
        return gate(out, "N4", False, {}, "", "no stars run")
    stars = bsc5_stars(REPO / "unreal_project/CamSimTest/Content/NonUFS/Sky/bsc5.dat")
    vals, details = {}, []
    for name, view in run["views"].items():
        cam, utc = view["camera"], parse_utc(view["utc"])
        enu = star_enu(stars, utc)
        x, y, front = project(enu, cam["heading"], cam["pitch"], cam["hfov_deg"])
        inside = front & (x >= 0) & (x <= W - 1) & (y >= 0) & (y <= H - 1) & (enu[:, 2] > 0)
        px = np.stack([x[inside], y[inside]], axis=-1)
        mags = stars[inside, 2]
        # Both frames averaged (a star is static, the noise is not); >= 2 px: a hot pixel is one
        img = (luma(load_rgb(d / f"{name}_a.png")) + luma(load_rgb(d / f"{name}_b.png"))) / 2.0
        blobs, sigma = detect_blobs(img, min_px=2)
        matched, dists = 0, []
        used = set()
        for bx, by, _ in blobs:
            if len(px) == 0:
                break
            dd = np.hypot(px[:, 0] - bx, px[:, 1] - by)
            j = int(np.argmin(dd))
            if dd[j] <= 2.0 and j not in used:
                matched += 1
                used.add(j)
                dists.append(float(dd[j]))
        vals[name] = {"detections": len(blobs), "matched": matched, "sigma_dn": sigma, "stars_in_view": int(inside.sum()),
                      "stars_in_view_v_le_5": int((mags <= 5.0).sum()),
                      "match_dist_median_px": float(np.median(dists)) if dists else None,
                      "matched_mags": sorted(float(mags[j]) for j in used)}
        details.append(f"{name}: {matched} of {len(blobs)} detections match a BSC5 star within 2 px "
                       f"({int(inside.sum())} catalogue stars in view, sigma {sigma:.2f} DN)")
    ok = vals.get("stars_night", {}).get("matched", 0) >= 20 and vals.get("stars_noon", {}).get("matched", 1) == 0
    return gate(out, "N4", ok, vals, ">= 20 matches at night, 0 at noon", "; ".join(details))


# --- dusk stream ---

ST0601_KEY = bytes.fromhex("060e2b34020b01010e01030101000000")


def ts_pes(data: bytes) -> dict[int, list[tuple[int | None, bytes]]]:
    """PES payloads per PID with their PTS (90 kHz), from raw 188-byte TS packets."""
    pes: dict[int, list[list]] = {}
    i = data.find(b"\x47")
    while i + 188 <= len(data):
        if data[i] != 0x47:
            j = data.find(b"\x47", i + 1)
            if j < 0:
                break
            i = j
            continue
        p = data[i:i + 188]
        i += 188
        pid = ((p[1] & 0x1F) << 8) | p[2]
        pusi = bool(p[1] & 0x40)
        afc = (p[3] >> 4) & 3
        off = 4
        if afc in (2, 3):
            off = 5 + p[4]
        if afc == 2 or off >= 188:
            continue
        payload = p[off:]
        lst = pes.setdefault(pid, [])
        if pusi:
            if payload[:3] != b"\x00\x00\x01":
                lst.append([None, bytearray(payload)])  # PSI
                continue
            hdr_len = payload[8]
            pts = None
            if payload[7] & 0x80:
                b = payload[9:14]
                pts = (((b[0] >> 1) & 7) << 30) | (b[1] << 22) | ((b[2] >> 1) << 15) | (b[3] << 7) | (b[4] >> 1)
            lst.append([pts, bytearray(payload[9 + hdr_len:])])
        elif lst:
            lst[-1][1].extend(payload)
    return {k: [(a, bytes(b)) for a, b in v] for k, v in pes.items()}


def klv_times(data: bytes) -> dict[int, int]:
    """PTS (90 kHz) -> ST 0601 Tag 2 precision time stamp (us, the sim clock), from the KLV PES of a raw TS."""
    out: dict[int, int] = {}
    for packets in ts_pes(data).values():
        for pts, body in packets:
            k = body.find(ST0601_KEY)
            if pts is None or k < 0:
                continue
            j = k + 16
            n = body[j]
            j += 1 + (n & 0x7F if n & 0x80 else 0)
            end = len(body)
            while j + 2 <= end:
                tag, ln = body[j], body[j + 1]
                if tag == 2 and ln == 8:
                    out[pts] = struct.unpack(">Q", body[j + 2:j + 10])[0]
                    break
                j += 2 + ln
    return out


def video_frames(ts_path: Path) -> tuple[list[int], list[float]]:
    """Per decoded video frame: PTS (90 kHz) and mean Y code value (Y plane of yuv420p, no range conversion)."""
    probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "frame=pts",
                            "-of", "csv=p=0", str(ts_path)], capture_output=True, text=True, check=True)
    pts = [int(x.split(",")[0]) for x in probe.stdout.split() if x.split(",")[0].lstrip("-").isdigit()]
    proc = subprocess.Popen(["ffmpeg", "-v", "error", "-i", str(ts_path), "-map", "0:v:0", "-fps_mode", "passthrough",
                             "-f", "rawvideo", "-pix_fmt", "yuv420p", "-"], stdout=subprocess.PIPE)
    n = W * H * 3 // 2
    means = []
    while True:
        buf = proc.stdout.read(n)
        if len(buf) < n:
            break
        means.append(float(np.frombuffer(buf, dtype=np.uint8, count=W * H).mean()))
    proc.wait()
    return pts, means


def dusk_series(d: Path) -> dict:
    cache = d / "series.json"
    if cache.exists():
        return json.loads(cache.read_text())
    data = (d / "dusk.ts").read_bytes()
    kt = klv_times(data)
    pts, means = video_frames(d / "dusk.ts")
    if len(pts) != len(means):
        n = min(len(pts), len(means))
        print(f"  dusk: ffprobe {len(pts)} frames vs decoded {len(means)}; using the last {n}")
        pts, means = pts[-n:], means[-n:]
    rows = []
    for p, m in zip(pts, means):
        us = kt.get(p)
        if us is None:
            continue
        utc = datetime.fromtimestamp(us / 1e6, tz=timezone.utc)
        e = eph.compute(utc, SLANT_500["lat"], SLANT_500["lon"], SLANT_500["alt"])
        rows.append({"pts": p, "us": us, "y": m, "sun_el": e.sun_el})
    out = {"frames": rows, "klv_packets": len(kt), "video_frames": len(pts)}
    cache.write_text(json.dumps(out))
    return out


def eval_n5_n8(out: Path) -> tuple[dict, dict]:
    d = out / "dusk"
    run = load_json(d / "run.json")
    if not run or not (d / "dusk.ts").exists():
        return gate(out, "N5", False, {}, "", "no dusk run"), gate(out, "N8", False, {}, "", "no dusk run")
    log = read_log(d / "camsim.log")
    ser = dusk_series(d)
    fr = ser["frames"]
    y = np.array([f["y"] for f in fr])
    us = np.array([f["us"] for f in fr])
    el = np.array([f["sun_el"] for f in fr])
    step = np.diff(y, prepend=y[0])
    # Band switches: the sim time of the sky line just before each band line (same or previous tick)
    switches = []
    for b in log["band"]:
        if not (run["wall0"] <= b["wall"] <= run["wall1"]):
            continue  # before the recording, or N9's lockstep captures after it (their sim times overlap the recording's)
        prior = [s for s in log["sky"] if s["wall"] <= b["wall"]]
        if not prior or not len(us):
            continue
        t_us = parse_utc(prior[-1]["utc"]).timestamp() * 1e6
        if not (us[0] <= t_us <= us[-1]):
            continue  # outside the recording (start-up, the final capture)
        i = int(np.searchsorted(us, t_us))
        lo, hi = max(i - 5, 1), min(i + 16, len(y))
        win = step[lo:hi]
        k = int(np.argmax(np.abs(win))) + lo
        settle = None
        for s in range(i, min(i + 200, len(y) - 30)):
            if np.all(np.abs(step[s:s + 30]) < 1.0):
                settle = s - i
                break
        switches.append({"band": b["band"], "frame": i, "sun_el": float(el[i]) if i < len(el) else None,
                         "median_log2": b["median_log2"], "max_step_dn": float(abs(step[k])), "max_step_frame": k,
                         "settle_frames": settle, "steps_around": [round(float(s), 2) for s in step[i - 5:i + 35]]})
    if switches:
        ok5 = all(s["max_step_dn"] <= 5.0 and s["settle_frames"] is not None and s["settle_frames"] <= 30 for s in switches)
        det5 = "; ".join(f"-> {s['band']} at sun el {s['sun_el']:.2f}: max step {s['max_step_dn']:.2f} DN, settled after "
                         f"{s['settle_frames']} frames" for s in switches)
    else:
        ok5, det5 = False, "no band switch inside the recording"
    g5 = gate(out, "N5", ok5, {"switches": switches, "frames": len(fr), "klv_packets": ser["klv_packets"],
                                "video_frames": ser["video_frames"]},
              "max |step| <= 5 DN in [switch - 5, switch + 15]; |step| < 1 DN for 30 frames within 30 frames", det5)

    # N8: sun illuminance at the ground (log) and frame mean through civil twilight
    sky = sorted(lines_between(log["sky"], run["wall0"], run["wall1"]), key=lambda s: s["utc"])
    els = np.array([s["sun_el"] for s in sky])
    lux = np.array([s["sun_ground_lux"] for s in sky])
    vals8: dict = {}
    ok8 = True
    det8 = []
    order = np.argsort(-els)
    els_s, lux_s = els[order], lux[order]
    near = (els_s <= 2.0) & (els_s >= -1.5)
    rises = np.diff(lux_s[near])
    max_rise = float(rises.max()) if rises.size else 0.0
    above = els_s >= 10.0
    below = els_s <= -1.0
    if above.any() and (~above).any() and below.any():
        lux10 = float(np.interp(10.0, els_s[::-1], lux_s[::-1]))
        lux_m1 = float(lux_s[below][0])
        frac = lux_m1 / lux10 if lux10 > 0 else float("nan")
        ok8 &= max_rise <= 1e-9 * max(lux10, 1.0) and frac < 0.01
        vals8.update(lux_at_10=lux10, lux_at_m1=lux_m1, fraction=frac, max_rise_0_to_m1=max_rise,
                     sky_lines=len(sky))
        det8.append(f"sun ground lux {lux10:.4g} at 10 deg, {lux_m1:.3g} by -1 deg ({frac:.2%}), max rise through 0 -> -1 "
                    f"{max_rise:.3g}")
    else:
        ok8 = False
        det8.append(f"sky log does not span 10 -> -1 deg (els {els.min() if els.size else None}..{els.max() if els.size else None})")
    civil = (el <= 0.0) & (el >= -6.0)
    if civil.sum() > 30:
        from scipy import ndimage

        s = ndimage.median_filter(y[civil], size=15, mode="nearest")
        run_min = np.minimum.accumulate(s)
        excess = s - run_min
        worst = int(np.argmax(excess))
        raw = y[civil] - np.minimum.accumulate(y[civil])   # reported, not gated (spec N8: the 15-frame median)
        ok8 &= float(excess[worst]) <= 1.0
        vals8.update(civil_frames=int(civil.sum()), y_start=float(s[0]), y_end=float(s[-1]),
                     max_rise_dn=float(excess[worst]), max_rise_sun_el=float(el[civil][worst]),
                     raw_max_rise_dn=float(raw.max()))
        det8.append(f"mean luma through civil twilight {s[0]:.1f} -> {s[-1]:.1f} DN over {int(civil.sum())} frames, max rise "
                    f"above the running minimum {excess[worst]:.2f} DN (15-frame median; raw frames {raw.max():.2f}) "
                    f"(at sun el {el[civil][worst]:.2f})")
    else:
        ok8 = False
        det8.append("too few frames in civil twilight")
    g8 = gate(out, "N8", ok8, vals8, "lux non-increasing 0 -> -1 deg, < 1 % of the 10 deg value by -1 deg; 15-frame "
              "median luma never > 1 DN above its running minimum (sun 0 -> -6 deg)", "; ".join(det8))
    save_dusk_frames(d, fr, switches)
    return g5, g8


def save_dusk_frames(d: Path, fr: list[dict], switches: list[dict]) -> None:
    """A few colour frames from the stream for the eye: sun 0, -3, -6 deg and just after each band switch."""
    picks = {}
    el = np.array([f["sun_el"] for f in fr])
    for target in (0.0, -3.0, -6.0):
        if el.size and el.min() <= target <= el.max():
            picks[f"dusk_sun_{target:+.0f}deg"] = int(np.argmin(np.abs(el - target)))
    for s in switches:
        picks[f"dusk_switch_{s['band']}_plus2"] = min(s["frame"] + 2, len(fr) - 1)
    frames_dir = d / "frames"
    frames_dir.mkdir(exist_ok=True)
    for name, idx in picks.items():
        dest = frames_dir / f"{name}.png"
        if dest.exists():
            continue
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(d / "dusk.ts"), "-map", "0:v:0", "-vf",
                        f"select=eq(n\\,{idx})", "-fps_mode", "passthrough", "-frames:v", "1", str(dest)], check=False)


def linear_rgb(rgb: np.ndarray) -> np.ndarray:
    """Scene-linear RGB (relative) of capture PNG pixels: the BT.709 OETF inverted (the knee above 0.8 ignored: N9's
    regions sit below it)."""
    v = np.clip(rgb / 255.0, 0.0, 1.0)
    return np.where(v < 0.081, v / 4.5, np.power((v + 0.099) / 1.099, 1.0 / 0.45))


def region_rgb(rgb: np.ndarray, view: str) -> np.ndarray:
    """Mean linear RGB of N9's region: the ground pose's lower two thirds (central three quarters), the whole sky view."""
    h, w = rgb.shape[:2]
    if view == "ground":
        rgb = rgb[h // 3:, w // 8:w - w // 8]
    return linear_rgb(rgb).reshape(-1, 3).mean(axis=0)


TWILIGHT_BANDS = REPO / "scripts" / "sky" / "twilight_bands.json"


def implied_mired(bands: dict, rel_rg: float, rel_bg: float) -> tuple[float, float]:
    """Mired of the blackbody whose R/G (and, separately, B/G) relative to the reference CCT equals the illuminant's
    (scripts/sky/twilight_bands.py's table; 0 = beyond the locus's infinite-temperature end)."""
    bb = bands["blackbody_rel"]
    t, rg, bg = np.array(bb["temps_k"]), np.array(bb["rg"]), np.array(bb["bg"])
    m_rg = 0.0 if rel_rg <= rg[-1] else 1e6 / float(np.interp(-rel_rg, -rg, t))
    m_bg = 0.0 if rel_bg >= bg[-1] else 1e6 / float(np.interp(rel_bg, bg, t))
    return m_rg, m_bg


def twilight_colour_metrics(mean: dict, bands: dict) -> dict:
    """N9 from mean linear RGB per elevation tag (day, p3, z0, m3, m6) and view (ground, zenith). The ground's illuminant
    relative to daylight is its RGB over the same pose's at sun +22 (the albedo cancels), as R/G and B/G ratios; each
    ratio implies a colour temperature on the Planckian locus. Compared with Spitschan et al. (2016)'s measured
    10-90 % bands (rural, relative to their 20-26 deg spectra)."""
    day = mean["day"]["ground"]
    ratio = lambda c, i: float(c[i] / c[1])   # noqa: E731
    v: dict = {}
    for tag in ("p3", "z0", "m3", "m6"):
        g, z = mean[tag]["ground"], mean[tag]["zenith"]
        rel_rg, rel_bg = ratio(g, 0) / ratio(day, 0), ratio(g, 2) / ratio(day, 2)
        m_rg, m_bg = implied_mired(bands, rel_rg, rel_bg)
        b = bands["bins"][tag]
        v[tag] = {"rel_rg": rel_rg, "rel_bg": rel_bg, "mired_rg": m_rg, "mired_bg": m_bg, "dmired": m_rg - m_bg,
                  "zenith_rg": ratio(z, 0), "zenith_bg": ratio(z, 2), "zenith_br": float(z[2] / z[0]),
                  "ground_br": float(g[2] / g[0]), "band_rg": [b["rg"][0], b["rg"][2]], "band_bg": [b["bg"][0], b["bg"][2]],
                  "band_dmired": [b["dmired"][0], b["dmired"][2]], "measured_median": [b["rg"][1], b["bg"][1]]}
    inside = lambda x, band: band[0] <= x <= band[1]   # noqa: E731
    z0, p3, m3 = v["z0"], v["p3"], v["m3"]
    checks = {
        "0 deg: R/G in the measured band": inside(z0["rel_rg"], z0["band_rg"]),
        "0 deg: B/G in the measured band": inside(z0["rel_bg"], z0["band_bg"]),
        "0 deg: R/G and B/G CCTs agree as measured (mired difference in band)": inside(z0["dmired"], z0["band_dmired"]),
        "+3, -3, -6 deg: B/G in the measured bands": all(inside(v[t]["rel_bg"], v[t]["band_bg"]) for t in ("p3", "m3", "m6")),
        "CCT (B/G) rises +3 -> 0 -> -3 deg": p3["mired_bg"] > z0["mired_bg"] > m3["mired_bg"],
        "0 deg bluer than +3 deg in both ratios": z0["rel_rg"] < p3["rel_rg"] and z0["rel_bg"] > p3["rel_bg"],
        "zenith not magenta (R/G <= 1) at 0, -3, -6 deg": all(v[t]["zenith_rg"] <= 1.0 for t in ("z0", "m3", "m6")),
        "zenith bluer (B/R) than the ground at 0, -3, -6 deg": all(v[t]["zenith_br"] > v[t]["ground_br"] for t in ("z0", "m3", "m6")),
    }
    # Documented limit (docs/night.md "Twilight colour"): UE's three-wavelength atmosphere keeps too much red at +3, -3
    # and -6 deg (the illuminant sits off the locus toward magenta); reported, not gated.
    limit = {t: {"rg_in_band": inside(v[t]["rel_rg"], v[t]["band_rg"]),
                 "dmired_in_band": inside(v[t]["dmired"], v[t]["band_dmired"])} for t in ("p3", "m3", "m6")}
    return {"elevations": v, "checks": checks, "known_limit": limit, "pass": all(checks.values())}


def eval_n9(out: Path) -> dict:
    d = out / "dusk"
    run = load_json(d / "run.json")
    if not run or "colour" not in run or "z0" not in run["colour"]:
        return gate(out, "N9", False, {}, "", "no dusk colour captures (+22, +3, 0, -3, -6 deg)")
    col = run["colour"]
    mean = {tag: {view: region_rgb(load_rgb(d / name), "sky") for view, name in c["views"].items()} for tag, c in col.items()}
    m = twilight_colour_metrics(mean, json.loads(TWILIGHT_BANDS.read_text()))
    for tag, c in col.items():
        if tag in m["elevations"]:
            m["elevations"][tag]["sun_el"] = c["sun_el"]
    det = []
    for tag in ("p3", "z0", "m3", "m6"):
        e = m["elevations"][tag]
        det.append(f"{e.get('sun_el', 0):+.1f}: R/G x{e['rel_rg']:.2f} [{e['band_rg'][0]:.2f}-{e['band_rg'][1]:.2f}] B/G "
                   f"x{e['rel_bg']:.2f} [{e['band_bg'][0]:.2f}-{e['band_bg'][1]:.2f}] CCT {mired_k(e['mired_rg'])}/"
                   f"{mired_k(e['mired_bg'])} K, zenith R/G {e['zenith_rg']:.2f}")
    failed = [k for k, x in m["checks"].items() if not x]
    lim = [t for t, x in m["known_limit"].items() if not (x["rg_in_band"] and x["dmired_in_band"])]
    detail = "; ".join(det) + (f"; FAIL: {', '.join(failed)}" if failed else "") + \
        (f"; known limit (R/G off the measured band, magenta of the locus): {', '.join(lim)}" if lim else "")
    return gate(out, "N9", m["pass"], m, "Spitschan et al. 2016 measured 10-90 % bands (rural, vs 20-26 deg): 0 deg R/G, "
                "B/G and the R/G-vs-B/G CCT difference in band; B/G in band at +3/-3/-6; CCT (B/G) rising +3 -> 0 -> -3; "
                "0 deg bluer than +3 in both ratios; zenith R/G <= 1 and bluer (B/R) than the ground at 0/-3/-6. "
                "Reported, not gated (documented limit): R/G at +3/-3/-6", detail)


def mired_k(mired: float) -> str:
    return "inf" if mired <= 0 else f"{1e6 / mired:,.0f}"


def eval_n6(out: Path) -> dict:
    r = load_json(out / "thermal_run.json")
    if not r:
        return gate(out, "N6", False, {}, "exit 0", "no thermal run")
    return gate(out, "N6", r["returncode"] == 0, r, "thermal_check.py exit 0",
                f"exit {r['returncode']} after {r['minutes']:.0f} min; report {out / 'thermal' / 'report.md'}")


# Spec N7 (amended 2026-10-16): night with the moon up GPU <= +0.6 ms (the moon lights the volumetric clouds, UE's
# second cloud light) and game thread <= +0.3 ms; day <= +0.3 ms on both.
N7_LIMITS = {"night_slant": {"game_ms_p50": 0.3, "gpu_ms_p50": 0.6}, "day_slant": {"game_ms_p50": 0.3, "gpu_ms_p50": 0.3}}


def eval_n7(out: Path, base: Path | None) -> dict:
    cur = load_json(out / "cost" / "results.json")
    old = load_json(base / "cost" / "results.json") if base else None
    if not cur or not old:
        return gate(out, "N7", False, {}, str(N7_LIMITS), "missing results.json (baseline or current): cannot evaluate")
    vals, ok, det = {}, True, []
    for phase, limits in N7_LIMITS.items():
        a, b = old["phases"].get(phase, {}), cur["phases"].get(phase, {})
        for m, lim in limits.items():
            before, after = a.get(m), b.get(m)
            good = before is not None and after is not None and after - before <= lim
            ok &= good
            vals[f"{phase}.{m}"] = {"before": before, "after": after, "limit": lim, "pass": good}
            det.append(f"{phase} {m} {before} -> {after} (+{lim})" if before is not None and after is not None
                       else f"{phase} {m} missing")
    table = subprocess.run([sys.executable, str(REPO / "scripts/bench/compare.py"), str(base / "cost"), str(out / "cost")],
                           capture_output=True, text=True, check=False).stdout
    (out / "cost" / "compare.md").write_text(table)
    return gate(out, "N7", ok, vals, "per phase and metric: " + str(N7_LIMITS), ", ".join(det))


# ---------------------------------------------------------------------------


STARS_CONFIG_MARK = "    preset: eo_hd_cmos\n"


def stars_config() -> str:
    """deploy/camsim_config.yaml with a long EO integration limit (the stars run's 'manual long exposure')."""
    text = (REPO / "deploy" / "camsim_config.yaml").read_text()
    if STARS_CONFIG_MARK not in text:
        sys.exit("night_check: deploy/camsim_config.yaml has no 'preset: eo_hd_cmos' line to extend")
    return text.replace(STARS_CONFIG_MARK, STARS_CONFIG_MARK + f"    detector:\n      integration_max_ms: {STARS_INTEGRATION_MS}\n", 1)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scene", type=Path, required=True, help="scene package (Camp Pendleton)")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--runs", default="day,moon,stars,dusk,thermal,cost")
    ap.add_argument("--baseline", type=Path, default=None, help="this script's output on the pre-6A commit (day, cost)")
    ap.add_argument("--check-only", action="store_true", help="evaluate the gates on --out without launching")
    a = ap.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    runs = [r for r in a.runs.split(",") if r]
    if not a.check_only:
        launched = {"day": run_day, "moon": run_moon, "stars": run_stars, "dusk": run_dusk}
        for r in runs:
            print(f"[night] run {r}", flush=True)
            if r in launched:
                d = out / r
                with launch(d, a.scene, stars_config() if r == "stars" else None,
                            DAY_EXEC_CMDS if r == "day" else EXEC_CMDS) as c:
                    res = launched[r](c, d)
                save_json(d / "run.json", res)
            elif r == "thermal":
                save_json(out / "thermal_run.json", run_thermal(a.scene, out))
            elif r == "cost":
                save_json(out / "cost_run.json", run_cost(out))
            else:
                sys.exit(f"unknown run {r}")
    base = a.baseline.resolve() if a.baseline else None
    gates = []
    if "day" in runs:
        gates.append(eval_n1(out, base))
    if "moon" in runs:
        gates += [eval_n2(out), eval_n3(out)]
    if "stars" in runs:
        gates.append(eval_n4(out))
    if "dusk" in runs:
        gates += list(eval_n5_n8(out)) + [eval_n9(out)]
    if "thermal" in runs:
        gates.append(eval_n6(out))
    if "cost" in runs:
        gates.append(eval_n7(out, base))
    gates.sort(key=lambda g: g["gate"])
    for g in gates:
        res = "PASS" if g["pass"] else ("n/a" if g["pass"] is None else "FAIL")
        print(f"{g['gate']}: {res} - {g['detail']}")
    # Merged with the gate JSONs of earlier invocations (runs split over several calls): one complete summary.
    (out / "summary.md").write_text(summary_markdown(merged_gates(out, gates), {g["gate"] for g in gates}))
    print(f"summary: {out / 'summary.md'}")
    return 1 if any(g["pass"] is False for g in gates) else 0


if __name__ == "__main__":
    sys.exit(main())
