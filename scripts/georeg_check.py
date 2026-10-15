#!/usr/bin/env python3
"""REALISM R6 check 1, geo-registration (spec docs/superpowers/specs/2026-10-15-r6-georeg-determinism-design.md):
what CamSim draws is where its KLV says it is.

    TMPDIR=/opt/mac/tmp uv run --project scripts/scene --with pycocotools python scripts/georeg_check.py \\
        --scene .cache/scene-packages/pendleton-r5veg --out OUT [--runs main,distorted,pan] [--check-only]

A 2 m marker plate (entity type 9001; 9002 is the 10 m plate for the 5 km views) is placed over CIGI at each site of
scripts/georeg/sites_pendleton.json, on the rendered surface (CIGI HOT, tilted to its normal), and seen from ten views
(nadir 300 m, 45 deg at 1 km, 10 deg at 5 km per site, plus a nadir view with the marker near the frame corner). Per
view, 3 s of KLV (check.js capture + decode) and COCO masks: the measured pixel is the mask centroid, the mean over the
clean frames (visibility >= 0.95, not touching the frame edge, >= 30 needed): one binary mask is quantised to its
pixels, but the stencil is rendered under TSR's per-frame sub-pixel jitter, so the mean converges on the area centroid; the predictions come from the KLV alone
(scripts/georeg_lib.py):
  (a) the ST 0601 pinhole camera (Tags 13/14/75, 5/6/7 or 90/91, 18/19/20, 16/17);
  (b) (a) with the lens model (k1, k2 of the run; equal to (a) with the shipped presets);
  (c) the four corner points (Tags 82-89) as a homography, as a video player draws the footprint.
Also (a') the camera of the commanded CIGI pose (no KLV in the loop: separates KLV quantisation from render error).
Gates: G1 (a) <= 1 px on every view; G2 (c) <= 1 px on the flat views and the corner view (a four-corner homography
assumes a plane: relief and the 5 km footprint's curvature move points by more by construction, so it is reported
there only); G3 every KLV corner point (82-89) and the frame centre (23/24), at the rendered surface height there (CIGI
HOT after the capture), projects through the commanded camera onto its image corner / centre within 1 px, on every view
(bearing and range along the ray). The report gives the error in metres on the ground and the KLV quantisation budget (px per tag LSB).
Runs: main (the ten views), distorted (flat/nadir and flat/corner with k1 = -0.2, informational: KLV corners are
pinhole, ROADMAP 3B.3), pan (the flat/oblique camera with the gimbal yaw swept +-3 deg over 6 s for 20 s, the marker
crossing the frame at up to ~13 px per frame; per-frame error of (a) with KLV matched to COCO by Tag 2 = timestamp_us,
at lags -3..3 frames: the best lag says whether metadata and pixels are in step; informational).
OUT/<run>/<view>/ keeps placement.json, coco.jsonl, klv.jsonl and shot.png, so --check-only re-evaluates without
CamSim. Writes OUT/report.json and OUT/overlays/. Exit 0 when G1 and G2 pass, 1 on a gate failure, 2 on an error."""

from __future__ import annotations

import argparse
import dataclasses
import json
import math
import os
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts"))
sys.path.insert(0, str(REPO / "scripts" / "bench"))
import georeg_lib as g  # noqa: E402
from bench import scenario  # noqa: E402

SITES = REPO / "scripts" / "georeg" / "sites_pendleton.json"
CHECK_JS = REPO / "scripts" / "klv_conformance" / "check.js"
MARKER_ENTITY_ID = 50
PLATE = {9001: (1.0, 0.05), 9002: (5.0, 0.25)}  # half size, thickness (m)
LIFT_M = 0.01
MIN_FRAMES = 30
MIN_VISIBILITY = 0.95
GATE_PX = 1.0
CAPTURE_S = 3.0
SETTLE_S = 10.0
DISTORTED_K1 = -0.2
PAN_AMP_DEG = 3.0
PAN_PERIOD_S = 6.0
PAN_CAPTURE_S = 20.0
ASPECT = 9 / 16  # capture height / width of the shipped config (1920 x 1080; 1280 x 720 alike)


@dataclass(frozen=True)
class View:
    name: str
    site: dict = field(hash=False, compare=False)
    depression: float
    slant_m: float
    fov_h: float
    marker_type: int
    target: tuple[float, float] = (0.5, 0.5)  # where the marker should land, as a fraction of W, H


def views(sites: list[dict]) -> list[View]:
    out = []
    for s in sites:
        out += [View(f"{s['name']}/nadir", s, 90.0, 300.0, 20.0, 9001),
                View(f"{s['name']}/oblique", s, 45.0, 1000.0, 10.0, 9001),
                View(f"{s['name']}/slant", s, 10.0, 5000.0, 5.0, 9002)]
    flat = next((s for s in sites if s["kind"] == "flat"), None)
    if flat:
        out.append(View(f"{flat['name']}/corner", flat, 90.0, 300.0, 20.0, 9001, (0.85, 0.85)))
    return out


def vfov(fov_h: float, aspect: float = ASPECT) -> float:
    return math.degrees(2 * math.atan(math.tan(math.radians(fov_h / 2)) * aspect))


def camera_pose(view: View, marker_h: float, aspect: float = ASPECT) -> scenario.Pose:
    """The CIGI pose that puts the marker (site lat/lon at marker_h) at view.target, view.slant_m away, the camera
    heading view.site['heading_deg'] and looking view.depression down (the gimbal pitch; platform pitch/roll 0)."""
    psi = view.site["heading_deg"]
    tx = (view.target[0] - 0.5) * 2 * math.tan(math.radians(view.fov_h / 2))
    ty = (view.target[1] - 0.5) * 2 * math.tan(math.radians(vfov(view.fov_h, aspect) / 2))
    d = g.zyx(psi, 0, 0) @ g.zyx(0, -view.depression, 0) @ g.np.array([1.0, tx, ty])
    n, e, down = view.slant_m * d / g.np.linalg.norm(d)
    lat, lon, alt = g.offset_m(view.site["lat"], view.site["lon"], marker_h, east=-e, north=-n, up=down)
    return scenario.Pose(lat, lon, alt, yaw=psi, pitch=0.0, roll=0.0, gimbal_yaw=0.0, gimbal_pitch=-view.depression,
                         fov_h=view.fov_h)


def pose_tags(pose: scenario.Pose, width: int, height: int) -> dict:
    """The KLV tags a pose would produce (tests, and the commanded camera (a'))."""
    return {13: pose.lat, 14: pose.lon, 75: pose.alt, 5: pose.yaw % 360.0, 6: pose.pitch, 7: pose.roll,
            18: pose.gimbal_yaw % 360.0, 19: pose.gimbal_pitch, 20: 0.0, 16: pose.fov_h,
            17: vfov(pose.fov_h, height / width)}


def marker_corners(view: View, place: dict, top: bool = True) -> list[tuple[float, float, float]]:
    """The plate's top-face corners (what the mask shows from above); top=False: its footprint on the surface."""
    half, thick = PLATE[view.marker_type]
    return g.plate_corners(place["lat"], place["lon"], place["h"] + (LIFT_M if top else 0.0), half,
                           thick if top else 0.0, place["heading"], place["normal_az"], place["normal_el"])


def marker_box(view: View, place: dict) -> list[tuple[float, float, float]]:
    """The plate's eight corners (bottom face at the lifted placement height, top face `thick` above it)."""
    half, thick = PLATE[view.marker_type]
    args = (half, place["heading"], place["normal_az"], place["normal_el"])
    bottom = g.plate_corners(place["lat"], place["lon"], place["h"] + LIFT_M, args[0], 0.0, *args[1:])
    top = g.plate_corners(place["lat"], place["lon"], place["h"] + LIFT_M, args[0], thick, *args[1:])
    return bottom + top


def _static_tags(klv: list[dict]) -> dict:
    tags: dict[int, list[float]] = {}
    for p in klv:
        for k, v in p["tags"].items():
            if isinstance(v, (int, float)):
                tags.setdefault(int(k), []).append(float(v))
    for k, vs in tags.items():
        if k in g.LSB and max(vs) - min(vs) > 1.5 * g.LSB[k] + 1e-12:
            raise ValueError(f"KLV not static: tag {k} spans {max(vs) - min(vs):.3g}")
    return {k: statistics.median(vs) for k, vs in tags.items()}


def evaluate(view: View, place: dict, frames: list[dict], klv: list[dict], k1: float, k2: float) -> dict:
    out: dict = {"view": view.name}
    cents, size = [], None
    for f in frames:
        for a in f.get("annotations", []):
            if a.get("category", {}).get("name") != "georeg_marker" or "segmentation" not in a:
                continue
            seg = a["segmentation"]
            if a.get("visibility", 0.0) < MIN_VISIBILITY or g.mask_touches_edge(seg):
                continue
            cents.append(g.mask_centroid(seg))
            size = seg["size"]
    out["frames"] = len(cents)
    if len(cents) < MIN_FRAMES:
        out["error"] = f"marker not seen ({len(cents)} clean frames, need {MIN_FRAMES})"
        return out
    try:
        tags = _static_tags(klv)
    except ValueError as e:
        out["error"] = str(e)
        return out
    height, width = size
    mx, my = statistics.fmean(c[0] for c in cents), statistics.fmean(c[1] for c in cents)
    cam = g.Camera.from_klv(tags, width, height)
    # The mask is the plate's silhouette (its top and its visible side faces): predict the centroid of the projected
    # box, not of the top face (0.37 px apart at 10 deg depression for the 10 m plate)
    box = marker_box(view, place)
    corners = marker_corners(view, place)
    pred = {"a": g.silhouette_centroid(cam.project, box),
            "b": g.silhouette_centroid(lambda la, lo, h: cam.project_distorted(la, lo, h, k1, k2), box)}
    if all(k in tags for k in (23, 24, 82, 83, 84, 85, 86, 87, 88, 89)):
        # The corner homography maps the ground: map the plate's footprint, then add the parallax of its silhouette
        # (box above the footprint) as camera (a) sees it, so (c) measures the corner tags and not the plate's height
        hom = g.corner_homography(tags, width, height)
        foot = marker_corners(view, place, top=False)
        hx, hy = g.projected_centroid(lambda la, lo, h: hom(la, lo), foot)
        fx, fy = g.projected_centroid(cam.project, foot)
        pred["c"] = (hx + pred["a"][0] - fx, hy + pred["a"][1] - fy)
    if place.get("camera"):
        # (a') the commanded CIGI pose, no KLV in the loop: separates KLV quantisation from render error; and G3:
        # each KLV corner point and the frame centre must lie on that camera's ray through its image point
        cmd = g.Camera.from_klv(pose_tags(scenario.Pose(**place["camera"]), width, height), width, height)
        pred["cmd"] = g.silhouette_centroid(cmd.project, box)
        if place.get("corner_hot"):
            # G3: each KLV corner (and the frame centre) at the rendered surface height there (CIGI HOT) must project
            # through the commanded camera onto its image corner: bearing and range along the ray both count
            rays = {}
            for k, (px, py) in zip((82, 84, 86, 88, 23),
                                   ((0, 0), (width, 0), (width, height), (0, height), (width / 2, height / 2))):
                h = place["corner_hot"].get(str(k))
                if k in tags and k + 1 in tags and h is not None:
                    x, y = cmd.project(tags[k], tags[k + 1], h)
                    rays[str(k)] = math.hypot(x - px, y - py)
            out["corner_ray_px"] = rays
    centre = tuple(sum(c[i] for c in corners) / 4 for i in range(3))
    gx, gy = cam.ground_pixel_size(*centre)
    out.update(measured=[mx, my], predicted={k: list(v) for k, v in pred.items()},
               error_px={k: math.hypot(v[0] - mx, v[1] - my) for k, v in pred.items()},
               error_m={k: math.hypot((v[0] - mx) * gx, (v[1] - my) * gy) for k, v in pred.items()},
               ground_px_m=[gx, gy], budget_px=g.quantisation_budget(tags, width, height, centre),
               size=[width, height])
    return out


def gates(results: list[dict]) -> dict:
    def gate(views_in, key):
        failed = [r["view"] for r in results if views_in(r["view"])
                  and ("error" in r or r["error_px"].get(key, math.inf) > GATE_PX)]
        return {"pass": not failed, "failed": failed}

    g3 = [r["view"] for r in results if "error" in r or not r.get("corner_ray_px")
          or max(r["corner_ray_px"].values()) > GATE_PX]
    return {"G1": gate(lambda v: True, "a"),
            "G2": gate(lambda v: v.startswith("flat/"), "c"),
            "G3": {"pass": not g3, "failed": g3}}


# --- CamSim session -------------------------------------------------------------------------------------------------


def _session_modules():
    sys.path.insert(0, str(REPO / "scripts" / "scene" / "tools"))
    import camsim_session as cs
    import ocean_check
    import test_entity_rendering as ter

    return cs, ocean_check, ter


def _hot(responses, oc, host, rid: int, lat: float, lon: float) -> dict:
    responses.hot.pop(rid, None)
    for _ in range(10):
        oc.send_packets(host, oc.pack_hat_hot_request(rid, lat, lon, req_type=2))
        t_end = time.time() + 1.0
        while time.time() < t_end:
            r = responses.hot.get(rid)
            if r and r.get("valid") and "hot" in r:
                return r
            time.sleep(0.05)
    raise RuntimeError(f"no HOT response at {lat:.6f}, {lon:.6f}")


class Marker:
    """Sends the marker's Entity Control once a second (CIGI entity: no clamp)."""

    def __init__(self, host, oc, ter):
        self.host, self.oc, self.ter = host, oc, ter
        self.packet: bytes | None = None
        self.stop = threading.Event()
        threading.Thread(target=self._run, daemon=True).start()

    def place(self, entity_type: int, place: dict) -> None:
        yaw, pitch, roll = g.plate_attitude(place["heading"], place["normal_az"], place["normal_el"])
        place.update(yaw=yaw, pitch=pitch, roll=roll)
        self.packet = self.ter.pack_entity_control(MARKER_ENTITY_ID, place["lat"], place["lon"], place["h"] + LIFT_M,
                                                   yaw, pitch, roll, entity_type=entity_type)
        self.oc.send_packets(self.host, self.packet)

    def _run(self) -> None:
        while not self.stop.is_set():
            if self.packet:
                self.oc.send_packets(self.host, self.packet)
            time.sleep(1.0)


def _coco_lines(path: Path, start: int) -> tuple[list[dict], int]:
    if not path.exists():
        return [], start
    with path.open("rb") as f:
        f.seek(start)
        data = f.read()
    end = start + data.rfind(b"\n") + 1 if b"\n" in data else start
    lines = data[: end - start].decode().splitlines()
    return [json.loads(line) for line in lines if line.strip()], end


def _capture(rdir: Path, coco: Path, rb) -> None:
    _, start = _coco_lines(coco, coco.stat().st_size if coco.exists() else 0)
    ts = rdir / "klv.ts"
    subprocess.run(["node", str(CHECK_JS), "capture", "udp://127.0.0.1:5004", str(ts), "--duration-sec",
                    str(CAPTURE_S)], check=True, stdout=subprocess.DEVNULL)
    rb.fetch_snapshot(rdir / "shot.png")
    time.sleep(0.5)
    frames, _ = _coco_lines(coco, start)
    (rdir / "coco.jsonl").write_text("".join(json.dumps(f) + "\n" for f in frames))
    subprocess.run(["node", str(CHECK_JS), "decode", str(ts), str(rdir / "klv.jsonl")], check=True,
                   stdout=subprocess.DEVNULL)
    ts.unlink()


def run_views(label: str, pkg: Path, vs: list[View], out: Path, k1: float, config_src: Path | None) -> None:
    cs, oc, ter = _session_modules()
    rb = cs.rb
    run_dir = out / label
    coco = run_dir / "ml" / "camsim_coco.jsonl"
    env = {**cs.env_for("package", pkg), "CAMSIM_ML_ENABLED": "1", "CAMSIM_ML_OUTPUT_DIR": str(run_dir / "ml"),
           "CAMSIM_ML_DEPTH_ENABLED": "0", "CAMSIM_ML_INTERVAL_FRAMES": "1", "CAMSIM_DIS_ENABLED": "0",
           "CAMSIM_MULTICAST_ADDR": "127.0.0.1"}
    if config_src:
        env["CAMSIM_CONFIG_SRC"] = str(config_src)
    responses = oc.Responses()
    rid = 200
    try:
        with cs.camsim(env, camera_pose(vs[0], 0.0)) as host:
            marker = Marker(host, oc, ter)
            try:
                for v in vs:
                    vdir = run_dir / v.name.replace("/", "_")
                    vdir.mkdir(parents=True, exist_ok=True)
                    print(f"[georeg] {label} {v.name}", flush=True)
                    host.pose = camera_pose(v, 0.0)
                    time.sleep(1.0)
                    rb.wait_terrain(180)
                    rid += 1
                    h0 = _hot(responses, oc, host, rid, v.site["lat"], v.site["lon"])["hot"]
                    pose = camera_pose(v, h0)
                    host.pose = pose
                    time.sleep(1.0)
                    rb.wait_terrain(180)
                    rid += 1
                    hot = _hot(responses, oc, host, rid, v.site["lat"], v.site["lon"])
                    place = {"lat": v.site["lat"], "lon": v.site["lon"], "h": hot["hot"],
                             "normal_az": hot.get("normal_az", 0.0), "normal_el": hot.get("normal_el", 90.0),
                             "material": hot.get("material"), "heading": v.site["heading_deg"], "k1": k1, "h0": h0,
                             "camera": dataclasses.asdict(pose)}
                    marker.place(v.marker_type, place)
                    (vdir / "placement.json").write_text(json.dumps(place, indent=1) + "\n")
                    time.sleep(SETTLE_S)
                    _capture(vdir, coco, rb)
                    place["corner_hot"] = {}
                    try:
                        tags = _static_tags([json.loads(x) for x in (vdir / "klv.jsonl").read_text().splitlines()
                                             if x.strip()])
                    except ValueError:
                        tags = {}  # KLV not static: evaluate() reports it; G3 has no heights and fails
                    for k in (82, 84, 86, 88, 23):
                        if k in tags and k + 1 in tags:
                            rid += 1
                            place["corner_hot"][str(k)] = _hot(responses, oc, host, rid, tags[k], tags[k + 1])["hot"]
                    (vdir / "placement.json").write_text(json.dumps(place, indent=1) + "\n")
            finally:
                marker.stop.set()
    finally:
        responses.stop.set()


def pan_pose(site: dict, marker_h: float, t: float) -> scenario.Pose:
    """The pan run's pose at time t: the flat/oblique camera (45 deg, 1 km) with the gimbal yaw swept
    PAN_AMP_DEG sin(2 pi t / PAN_PERIOD_S), so the marker crosses the frame at up to ~13 px per frame."""
    base = camera_pose(View("flat/pan", site, 45.0, 1000.0, 10.0, 9001), marker_h)
    return dataclasses.replace(base, gimbal_yaw=PAN_AMP_DEG * math.sin(2 * math.pi * t / PAN_PERIOD_S))


def run_pan(pkg: Path, site: dict, out: Path) -> None:
    cs, oc, ter = _session_modules()
    rb = cs.rb
    run_dir = out / "pan"
    vdir = run_dir / "flat_pan"
    vdir.mkdir(parents=True, exist_ok=True)
    coco = run_dir / "ml" / "camsim_coco.jsonl"
    env = {**cs.env_for("package", pkg), "CAMSIM_ML_ENABLED": "1", "CAMSIM_ML_OUTPUT_DIR": str(run_dir / "ml"),
           "CAMSIM_ML_DEPTH_ENABLED": "0", "CAMSIM_ML_INTERVAL_FRAMES": "1", "CAMSIM_DIS_ENABLED": "0",
           "CAMSIM_MULTICAST_ADDR": "127.0.0.1"}
    responses = oc.Responses()
    try:
        with cs.camsim(env, pan_pose(site, 0.0, 0.0)) as host:
            marker = Marker(host, oc, ter)
            try:
                rb.wait_terrain(180)
                h0 = _hot(responses, oc, host, 900, site["lat"], site["lon"])["hot"]
                host.pose = pan_pose(site, h0, 0.0)
                time.sleep(1.0)
                rb.wait_terrain(180)
                hot = _hot(responses, oc, host, 901, site["lat"], site["lon"])
                place = {"lat": site["lat"], "lon": site["lon"], "h": hot["hot"],
                         "normal_az": hot.get("normal_az", 0.0), "normal_el": hot.get("normal_el", 90.0),
                         "heading": site["heading_deg"], "k1": 0.0, "h0": h0}
                marker.place(9001, place)
                (vdir / "placement.json").write_text(json.dumps(place, indent=1) + "\n")
                time.sleep(SETTLE_S)
                stop = threading.Event()

                def fly() -> None:
                    t0 = time.monotonic()
                    while not stop.is_set():
                        host.pose = pan_pose(site, h0, time.monotonic() - t0)
                        time.sleep(1 / 120)

                threading.Thread(target=fly, daemon=True).start()
                start = coco.stat().st_size if coco.exists() else 0
                ts = vdir / "klv.ts"
                subprocess.run(["node", str(CHECK_JS), "capture", "udp://127.0.0.1:5004", str(ts), "--duration-sec",
                                str(PAN_CAPTURE_S)], check=True, stdout=subprocess.DEVNULL)
                stop.set()
                time.sleep(0.5)
                frames, _ = _coco_lines(coco, start)
                (vdir / "coco.jsonl").write_text("".join(json.dumps(f) + "\n" for f in frames))
                subprocess.run(["node", str(CHECK_JS), "decode", str(ts), str(vdir / "klv.jsonl")], check=True,
                               stdout=subprocess.DEVNULL)
                ts.unlink()
            finally:
                marker.stop.set()
    finally:
        responses.stop.set()


def evaluate_pan(vdir: Path) -> dict:
    """Per-frame error of (a) with the KLV packet stamped with frame i + lag (Tag 2 = timestamp_us), lag -3..3: the
    lag with the smallest median is where metadata and pixels line up (0 = in step)."""
    place = json.loads((vdir / "placement.json").read_text())
    frames = [json.loads(line) for line in (vdir / "coco.jsonl").read_text().splitlines() if line.strip()]
    klv = sorted((json.loads(line)["tags"] for line in (vdir / "klv.jsonl").read_text().splitlines() if line.strip()),
                 key=lambda t: t["2"])
    by_ts = {int(t["2"]): i for i, t in enumerate(klv)}
    corners = marker_corners(View("flat/pan", {"heading_deg": place["heading"]}, 45.0, 1000.0, 10.0, 9001), place)
    out: dict = {"frames": len(frames), "klv_packets": len(klv), "lags": {}}
    meas, speeds = [], []
    for f in frames:
        anns = [a for a in f["annotations"] if a.get("category", {}).get("name") == "georeg_marker"
                and "segmentation" in a and a.get("visibility", 0) >= MIN_VISIBILITY
                and not g.mask_touches_edge(a["segmentation"])]
        i = by_ts.get(int(f["timestamp_us"]))
        if anns and i is not None:
            meas.append((i, g.mask_centroid(anns[0]["segmentation"]), anns[0]["segmentation"]["size"]))
    for (_, a, _), (_, b, _) in zip(meas, meas[1:]):
        speeds.append(math.hypot(b[0] - a[0], b[1] - a[1]))
    out["matched"] = len(meas)
    out["max_speed_px_per_frame"] = max(speeds) if speeds else 0.0
    for lag in range(-3, 4):
        errs = []
        for i, (mx, my), (h, w) in meas:
            if 0 <= i + lag < len(klv):
                cam = g.Camera.from_klv({int(k): v for k, v in klv[i + lag].items()}, w, h)
                px, py = g.projected_centroid(cam.project, corners)
                errs.append(math.hypot(px - mx, py - my))
        if errs:
            errs.sort()
            out["lags"][lag] = {"median_px": statistics.median(errs), "p95_px": errs[int(0.95 * (len(errs) - 1))],
                                "n": len(errs)}
    if out["lags"]:
        out["best_lag"] = min(out["lags"], key=lambda k: out["lags"][k]["median_px"])
    return out


def overlay(vdir: Path, r: dict, dest: Path) -> None:
    from PIL import Image, ImageDraw

    shot = vdir / "shot.png"
    if not shot.exists() or "measured" not in r:
        return
    im = Image.open(shot).convert("RGB")
    sx, sy = im.width / r["size"][0], im.height / r["size"][1]
    mx, my = r["measured"][0] * sx, r["measured"][1] * sy
    half = 40
    crop = im.crop((int(mx) - half, int(my) - half, int(mx) + half, int(my) + half)).resize((8 * half, 8 * half),
                                                                                              Image.NEAREST)
    d = ImageDraw.Draw(crop)

    def dot(x, y, colour):
        cx, cy = (x * sx - (int(mx) - half)) * 4, (y * sy - (int(my) - half)) * 4
        d.ellipse((cx - 5, cy - 5, cx + 5, cy + 5), outline=colour, width=2)

    dot(*r["measured"], (255, 0, 0))
    for k, colour in (("a", (0, 255, 0)), ("b", (0, 128, 255)), ("c", (255, 255, 0))):
        if k in r["predicted"]:
            dot(*r["predicted"][k], colour)
    dest.parent.mkdir(parents=True, exist_ok=True)
    crop.save(dest)


def distorted_config(k1: float) -> Path:
    """deploy/camsim_config.yaml with the EO mode's optics.k1 set (the first commented optics block)."""
    src = (REPO / "deploy" / "camsim_config.yaml").read_text()
    anchor = "    # optics:                      # 1080p industrial CMOS (eo_hd_cmos preset values)\n"
    if anchor not in src:
        raise SystemExit("deploy/camsim_config.yaml: the EO optics block moved; update distorted_config()")
    f = Path(tempfile.mkstemp(prefix="georeg-config-", suffix=".yaml")[1])
    f.write_text(src.replace(anchor, f"    optics:\n      k1: {k1}\n", 1))
    return f


def evaluate_run(out: Path, label: str, vs: list[View]) -> list[dict]:
    res = []
    for v in vs:
        vdir = out / label / v.name.replace("/", "_")
        if not (vdir / "placement.json").exists():
            res.append({"view": v.name, "error": "not run"})
            continue
        place = json.loads((vdir / "placement.json").read_text())
        frames = [json.loads(x) for x in (vdir / "coco.jsonl").read_text().splitlines() if x.strip()]
        klv = [json.loads(x) for x in (vdir / "klv.jsonl").read_text().splitlines() if x.strip()]
        r = evaluate(v, place, frames, klv, place.get("k1", 0.0), 0.0)
        r["normal_el"], r["material"] = place.get("normal_el"), place.get("material")
        res.append(r)
        overlay(vdir, r, out / "overlays" / label / f"{v.name.replace('/', '_')}.png")
    return res


def print_table(label: str, res: list[dict]) -> None:
    print(f"\n{label}: view | frames | (a) px / m | (a') cmd px | (b) px | (c) px / m | corner rays max px | "
          "ground px m | max budget px")
    for r in res:
        if "error" in r:
            print(f"  {r['view']:<14} ERROR {r['error']}")
            continue
        e, em = r["error_px"], r["error_m"]
        c = f"{e['c']:.2f} / {em['c']:.2f}" if "c" in e else "--"
        cmd = f"{e['cmd']:.2f}" if "cmd" in e else "--"
        rays = f"{max(r['corner_ray_px'].values()):.2f}" if r.get("corner_ray_px") else "--"
        print(f"  {r['view']:<14} {r['frames']:>4} | {e['a']:.2f} / {em['a']:.2f} | {cmd} | {e['b']:.2f} | {c} | "
              f"{rays} | {r['ground_px_m'][0]:.3f} x {r['ground_px_m'][1]:.3f} | {max(r['budget_px'].values()):.2f}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scene", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--sites", type=Path, default=SITES)
    ap.add_argument("--runs", default="main,distorted,pan")
    ap.add_argument("--views", help="comma-separated view names (main run), e.g. flat/nadir,flat/corner")
    ap.add_argument("--check-only", action="store_true", help="re-evaluate OUT without launching CamSim")
    a = ap.parse_args()
    runs = a.runs.split(",")
    sites = json.loads(a.sites.read_text())["sites"]
    all_views = views(sites)
    main_views = [v for v in all_views if not a.views or v.name in a.views.split(",")]
    dist_views = [v for v in all_views if v.name in ("flat/nadir", "flat/corner")]
    pkg = a.scene.resolve()
    a.out = a.out.resolve()  # CAMSIM_ML_OUTPUT_DIR: CamSim resolves a relative path against its binary directory
    try:
        if not a.check_only:
            if "main" in runs:
                run_views("main", pkg, main_views, a.out, 0.0, None)
            if "distorted" in runs:
                cfg = distorted_config(DISTORTED_K1)
                try:
                    run_views("distorted", pkg, dist_views, a.out, DISTORTED_K1, cfg)
                finally:
                    cfg.unlink()
            if "pan" in runs:
                run_pan(pkg, next(s for s in sites if s["kind"] == "flat"), a.out)
    except (RuntimeError, subprocess.CalledProcessError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    report: dict = {"scene": pkg.name, "date": time.strftime("%Y-%m-%d"), "runs": {}}
    if "main" in runs:
        res = evaluate_run(a.out, "main", main_views)
        report["runs"]["main"] = res
        report["gates"] = gates(res)
        print_table("main", res)
    if "distorted" in runs:
        res = evaluate_run(a.out, "distorted", dist_views)
        report["runs"]["distorted"] = res
        print_table(f"distorted (k1 = {DISTORTED_K1}, informational)", res)
    if "pan" in runs and (a.out / "pan" / "flat_pan" / "klv.jsonl").exists():
        p = report["runs"]["pan"] = evaluate_pan(a.out / "pan" / "flat_pan")
        print(f"\npan (informational): {p['matched']} frames matched, marker up to {p['max_speed_px_per_frame']:.1f} "
              f"px/frame; best KLV lag {p.get('best_lag')} frames; median / p95 px by lag: "
              + ", ".join(f"{k}: {v['median_px']:.2f} / {v['p95_px']:.2f}" for k, v in p["lags"].items()))
    (a.out / "report.json").write_text(json.dumps(report, indent=1, default=str) + "\n")
    if "gates" in report:
        for k, v in report["gates"].items():
            print(f"{k}: {'PASS' if v['pass'] else 'FAIL ' + ', '.join(v['failed'])}")
        return 0 if all(v["pass"] for v in report["gates"].values()) else 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
