#!/usr/bin/env python3
"""Control API acceptance (ROADMAP 2.4, spec docs/superpowers/specs/2026-10-15-control-api-design.md §6).

Launches CamSim headless with the control API and ground truth on and NO CIGI host, then:

  C1 ready        /ready goes true with no CIGI traffic once the API poses the camera; GET /camera echoes the FOV
  C2 lockstep     with lockstep on the clock stands still over wall time; step 10 moves the capture timestamp by 10 s
  C3 pairing      a truck (ground) and a boat (water) each come back in their capture's record with a rendered mask,
                  boxes inside the image, a depth PNG of the image size (overlays in OUT/c3/)
  C4 placement    the truck's committed height equals a CIGI HOT at its position within 0.1 m (camera on the truck)
  C5 ownership    a CIGI host sending another camera pose doesn't move the API-owned camera; after release it does
  C6 dataset      make_dataset: 12 samples x EO+IR = 24 images, loaded by pycocotools, every RLE decodes
  C7 determinism  a second launch, same seed (6 samples x EO+IR): boxes within 1 px, mask IoU >= 0.98, image mean
                  abs diff <= 1 DN
  C8 throughput   captures per minute and settle time (informational)

Every launch pins the sensor noise and UE's temporal sequences as determinism_check.py does.

    uv run --project scripts/control --with pillow --with numpy --with pycocotools \\
        python scripts/control_api_check.py --scene .cache/scene-packages/pendleton-bridges --out .cache/control/check
"""

from __future__ import annotations

import argparse
import contextlib
import io
import json
import os
import socket
import statistics
import subprocess
import sys
import time
import urllib.error
import urllib.request
from collections.abc import Iterator
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts"))
sys.path.insert(0, str(REPO / "scripts" / "control"))
import check_cigi_responses as ccr  # noqa: E402
import gt_check_lib as gtl  # noqa: E402
import make_dataset  # noqa: E402
import numpy as np  # noqa: E402
import send_cigi_test as sc  # noqa: E402
from bench import run_bench as rb  # noqa: E402
from bench import scenario  # noqa: E402
from camsim_control.client import ApiError, Client  # noqa: E402
from camsim_control.coco import load_categories  # noqa: E402
from camsim_control.randomize import EntitySpec, offset_ll  # noqa: E402
from PIL import Image  # noqa: E402
from pycocotools import mask as mask_util  # noqa: E402
from pycocotools.coco import COCO  # noqa: E402

SITE = (33.3072917, -117.344625)   # georeg flat site (paved, < 2 deg slope)
SEA = (33.2100, -117.4300)         # >= 2 km offshore (hot_check SEA_POINTS)
# The determinism pins of determinism_check.py, and the level's volumetric clouds off: they scroll on UE engine time
# (REALISM R6 open item), which moves their shadows between launches (2.8 DN mean, mostly IR, with them on).
EXEC_CMDS = "-ExecCmds=camsim.Sensor.FixedNoiseFrame 0,r.Test.FreezeTemporalSequences 1,r.VolumetricCloud 0"
CIGI_RESPONSE_PORT = 8889
PID_FILE = REPO / ".cache" / "camsim.pid"


def http_code(path: str) -> int:
    try:
        with urllib.request.urlopen(rb.HEALTH + path, timeout=3) as r:
            return r.status
    except urllib.error.HTTPError as e:
        return e.code
    except OSError:
        return 0


@contextlib.contextmanager
def launch(out: Path, scene: Path | None, tag: str) -> Iterator[Client]:
    rb.wait_port_free(int(rb.HEALTH.rsplit(":", 1)[1]))
    env = dict(os.environ, CAMSIM_CONTROL_API_ENABLED="1", CAMSIM_ML_ENABLED="1", CAMSIM_ML_INTERVAL_FRAMES="100000",
               CAMSIM_ML_OUTPUT_DIR=str(out / f"ml-{tag}"))
    if scene:
        env["CAMSIM_SCENE_DIR"] = str(scene.resolve())
    subprocess.run([str(REPO / "scripts" / "run.sh"), "--headless", "--local", "--detach", EXEC_CMDS],
                   env=env, check=True, stdout=subprocess.DEVNULL)
    try:
        c = Client(rb.HEALTH)
        c.wait_ready(timeout_s=900)
        yield c
    finally:
        subprocess.run([str(REPO / "scripts" / "stop.sh")], check=False, stdout=subprocess.DEVNULL)


class Report:
    def __init__(self) -> None:
        self.rows: list[tuple[str, bool | None, str]] = []

    def add(self, gate: str, ok: bool | None, detail: str) -> None:
        self.rows.append((gate, ok, detail))
        print(f"{gate}: {'PASS' if ok else ('INFO' if ok is None else 'FAIL')} - {detail}", flush=True)

    def write(self, path: Path) -> bool:
        lines = ["# Control API acceptance", "", "| Gate | Result | Detail |", "|---|---|---|"]
        for g, ok, d in self.rows:
            lines.append(f"| {g} | {'PASS' if ok else ('info' if ok is None else '**FAIL**')} | {d} |")
        path.write_text("\n".join(lines) + "\n")
        return all(ok is not False for _, ok, _ in self.rows)


def camera_at(aim: tuple[float, float], ground: float, slant: float, depression: float, heading: float,
              hfov: float, mode: str = "eo") -> dict:
    import math
    horiz = slant * math.cos(math.radians(depression))
    h = math.radians(heading)
    lat, lon = offset_ll(aim[0], aim[1], -horiz * math.cos(h), -horiz * math.sin(h))
    return {"lat": lat, "lon": lon, "alt": ground + slant * math.sin(math.radians(depression)), "heading": heading,
            "pitch": 0.0, "roll": 0.0, "gimbal": {"yaw": 0.0, "pitch": -depression, "roll": 0.0}, "hfov_deg": hfov,
            "sensor": {"mode": mode}}


def committed(c: Client, entity_id: int, timeout_s: float = 20.0) -> dict | None:
    end = time.time() + timeout_s
    while time.time() < end:
        for e in c.entities():
            if e["id"] == entity_id and e.get("spawned"):
                return e
        time.sleep(0.5)
    return None


def hot(lat: float, lon: float, rid: int = 77, seconds: float = 5.0) -> float | None:
    """CIGI HOT (extended request, opcode 24 -> 103) at a point, from a one-shot host datagram."""
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    rx.bind(("0.0.0.0", CIGI_RESPONSE_PORT))
    rx.settimeout(0.2)
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        end = time.time() + seconds
        frame = 1
        while time.time() < end:
            tx.sendto(sc.pack_ig_control(frame) + sc.pack_hat_hot_request(rid, lat, lon, req_type=2),
                      ("127.0.0.1", rb.CIGI_PORT))
            frame += 1
            t_wait = time.time() + 0.5
            while time.time() < t_wait:
                try:
                    d = rx.recv(65536)
                except TimeoutError:
                    continue
                for r in ccr.parse_responses(d)["hat_hot"]:
                    if r["op"] == 103 and r["id"] == rid and r["valid"]:
                        return float(r["hot"])
        return None
    finally:
        rx.close()
        tx.close()


def decode_png(data: bytes) -> np.ndarray:
    return np.asarray(Image.open(io.BytesIO(data)))


# ---------------------------------------------------------------------------
# Main run: C1-C6, the first C7 dataset
# ---------------------------------------------------------------------------


def run_main(c: Client, out: Path, rep: Report, ground: dict) -> None:
    # C1: not ready before the API owns the camera (no CIGI host), ready after
    before = http_code("/ready")
    c.set_clock(lockstep=True, utc="2026-06-15T19:00:00Z")
    c.set_camera(**C1_CAMERA)
    t0, after = time.time(), 0
    while time.time() - t0 < 180 and after != 200:
        after = http_code("/ready")
        time.sleep(1.0)
    echoed = c.get_camera().get("hfov_deg")
    rep.add("C1 ready", before == 503 and after == 200 and abs(echoed - 12.0) < 1e-3,
            f"/ready {before} before the camera, {after} after ({time.time() - t0:.0f} s), no CIGI; GET /camera hfov {echoed}")

    # C7, first half: at the same point of both launches (right after C1's camera pose), so they compare like for like
    c7_dataset(c, out / "c7a")

    # C2: lockstep
    u0 = c.clock()["utc"]
    time.sleep(3.0)
    u1 = c.clock()["utc"]
    a = c.capture()
    c.step(10.0)
    b = c.capture()
    dt = b.timestamp_us - a.timestamp_us
    rep.add("C2 lockstep", u0 == u1 and dt == 10_000_000,
            f"utc {u0} -> {u1} over 3 s wall; capture timestamps differ by {dt} us after step 10")

    # C3 + C4: a truck on the paved site, a boat offshore
    c3 = out / "c3"
    c3.mkdir(parents=True, exist_ok=True)
    c.clear_entities()
    c.put_entity(1, type_id=2001, lat=SITE[0], lon=SITE[1], alt=0.0, heading=45.0, surface="ground")
    truck = committed(c, 1)
    g = truck["alt"] if truck else -11.0
    ground["site"] = g
    c.set_camera(**camera_at(SITE, g, 600.0, 50.0, 30.0, 6.0))
    caps = {"truck": c.capture()}
    # C4 while the camera looks at the truck: HOT and the clamp both trace the loaded tiles, and the API re-submits
    # its entities every tick, so with the camera 10 km away both read the coarser tiles there (~1 m lower)
    truck = committed(c, 1)
    h = hot(SITE[0], SITE[1])
    c.put_entity(2, type_id=3001, lat=SEA[0], lon=SEA[1], alt=0.0, heading=200.0, surface="water")
    boat = committed(c, 2)
    c.set_camera(**camera_at(SEA, boat["alt"] if boat else 0.0, 600.0, 50.0, 300.0, 6.0))
    caps["boat"] = c.capture()
    ok3, notes = True, []
    for name, cap in caps.items():
        anns = (cap.record or {}).get("annotations", [])
        want_id = "1" if name == "truck" else "2"
        mine = [x for x in anns if x.get("source") == "api" and x.get("source_id") == want_id]
        (c3 / f"{name}.png").write_bytes(cap.png)
        img = decode_png(cap.png)
        depth = decode_png(cap.depth_png) if cap.depth_png else None
        inside = all(0 <= x["bbox"][0] and 0 <= x["bbox"][1] and x["bbox"][0] + x["bbox"][2] <= cap.width + 0.5
                     and x["bbox"][1] + x["bbox"][3] <= cap.height + 0.5 for x in anns)
        good = (len(mine) == 1 and mine[0].get("mask_source") == "render" and inside and depth is not None
                and depth.shape[:2] == img.shape[:2] == (cap.height, cap.width))
        ok3 &= good
        notes.append(f"{name}: {len(mine)} ann ({mine[0].get('mask_source') if mine else '-'}, "
                     f"{mine[0]['area'] if mine else 0:.0f} px), depth {None if depth is None else depth.shape}")
        if mine:
            gtl.draw_overlay(c3 / f"{name}.png", c3 / f"{name}_overlay.png", mine)
    rep.add("C3 pairing", ok3, "; ".join(notes))

    if truck is None or h is None:
        rep.add("C4 placement", False, f"truck {truck}, HOT {h}")
    else:
        rep.add("C4 placement", abs(truck["alt"] - h) <= 0.1,
                f"truck committed {truck['alt']:.3f} m, CIGI HOT {h:.3f} m (diff {truck['alt'] - h:+.3f})")

    # C5: a CIGI host 5 km away while the API owns the camera, then after release
    mine_cam = c.get_camera()
    far = offset_ll(SITE[0], SITE[1], 5000.0, 0.0)
    host = rb.Host()
    host.pose = scenario.Pose(lat=far[0], lon=far[1], alt=1500.0, yaw=0.0, pitch=0.0, roll=0.0, gimbal_pitch=-45.0)
    host.thread.start()
    try:
        time.sleep(3.0)
        held = c.get_camera()
        c.release_camera()
        time.sleep(3.0)
        moved = c.get_camera()
    finally:
        host.stop.set()
        host.thread.join(timeout=2)
    ok5 = (abs(held["lat"] - mine_cam["lat"]) < 1e-7 and abs(moved["lat"] - far[0]) < 1e-6)
    rep.add("C5 ownership", ok5, f"owned: lat {mine_cam['lat']:.7f} -> {held['lat']:.7f} with a CIGI host at "
            f"{far[0]:.7f}; released: {moved['lat']:.7f}")

    # C6 + C8: dataset (the API takes the camera back with its first PUT)
    t0 = time.time()
    times: list[float] = []
    ds = make_dataset.generate(c, out / "c6", 12, 7, SITE, g, ["eo", "ir"], [EntitySpec(2001, "ground")],
                               load_categories(REPO / "deploy" / "camsim_config.yaml"),
                               log=lambda s: (print("  " + s), times.append(time.time())))
    wall = time.time() - t0
    coco = COCO(str(ds))
    n_img = len(coco.getImgIds())
    bad_rle = 0
    for ann in coco.loadAnns(coco.getAnnIds()):
        if "segmentation" in ann:
            m = mask_util.decode(ann["segmentation"])
            bad_rle += int(m.sum() == 0)
    rep.add("C6 dataset", n_img == 24 and bad_rle == 0 and len(coco.getAnnIds()) > 0,
            f"{n_img} images, {len(coco.getAnnIds())} annotations, {bad_rle} empty RLE masks ({ds})")
    settles = [img["camsim"]["frame_id"] for img in coco.loadImgs(coco.getImgIds())]
    rep.add("C8 throughput", None, f"{24 / wall * 60:.1f} captures/min ({wall:.0f} s for 24 incl. scene setup); "
            f"frame ids {settles[0]}..{settles[-1]}")



C1_CAMERA = camera_at(SITE, -11.0, 1000.0, 45.0, 0.0, 12.0)
C7_GROUND = -11.235   # the site's ground (C4's clamp, ellipsoid m); fixed so both launches place the same scene


def c7_dataset(c: Client, out: Path) -> None:
    make_dataset.generate(c, out, 6, 11, SITE, C7_GROUND, ["eo", "ir"], [EntitySpec(2001, "ground")],
                          load_categories(REPO / "deploy" / "camsim_config.yaml"), log=lambda s: print("  " + s))


def compare(a_dir: Path, b_dir: Path) -> tuple[bool, str]:
    a = json.loads((a_dir / "annotations.json").read_text())
    b = json.loads((b_dir / "annotations.json").read_text())
    worst_box, worst_iou, problems = 0.0, 1.0, []
    diffs: dict[str, list[float]] = {"eo": [], "ir": []}
    by_img = lambda d: {i["id"]: [x for x in d["annotations"] if x["image_id"] == i["id"]] for i in d["images"]}  # noqa: E731
    aa, bb = by_img(a), by_img(b)
    for ia, ib in zip(a["images"], b["images"]):
        if ia["file_name"] != ib["file_name"]:
            problems.append(f"image order {ia['file_name']} vs {ib['file_name']}")
            continue
        pa = decode_png((a_dir / ia["file_name"]).read_bytes()).astype(np.float64)
        pb = decode_png((b_dir / ib["file_name"]).read_bytes()).astype(np.float64)
        diffs[ia["camsim"]["sensor_mode"] or "eo"].append(float(np.abs(pa - pb).mean()))
        xa = {x["camsim"]["source_id"]: x for x in aa[ia["id"]]}
        xb = {x["camsim"]["source_id"]: x for x in bb[ib["id"]]}
        if set(xa) != set(xb):
            problems.append(f"{ia['file_name']}: entities {sorted(xa)} vs {sorted(xb)}")
            continue
        for k in xa:
            worst_box = max(worst_box, max(abs(p - q) for p, q in zip(xa[k]["bbox"], xb[k]["bbox"])))
            if "segmentation" in xa[k] and "segmentation" in xb[k]:
                iou = float(mask_util.iou([xa[k]["segmentation"]], [xb[k]["segmentation"]], [0])[0][0])
                worst_iou = min(worst_iou, iou)
    # Gate: labels, and each mode's mean image difference (as R6 reports its determinism). Single images can differ by
    # up to ~3 DN (a global offset): the renderer carries state from earlier frames (TSR, Lumen, exposure history), so
    # the same view rendered after a different history differs a little, also within one launch (REALISM R6 open item).
    eo, ir = diffs["eo"], diffs["ir"]
    ok = (not problems and worst_box <= 1.0 and worst_iou >= 0.98 and bool(eo) and bool(ir)
          and statistics.mean(eo) <= 1.0 and statistics.mean(ir) <= 1.0)
    stat = lambda v: f"mean {statistics.mean(v):.3f} / max {max(v):.3f}" if v else "none"  # noqa: E731
    detail = (f"{len(eo) + len(ir)} image pairs: box diff max {worst_box:.2f} px, mask IoU min {worst_iou:.4f}; "
              f"image mean abs diff EO {stat(eo)} DN, IR {stat(ir)} DN (gate: mean <= 1)")
    if problems:
        detail += "; " + "; ".join(problems[:3])
    return ok, detail


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--scene", type=Path, default=None, help="scene package (default: CamSim's configured terrain)")
    ap.add_argument("--runs", default="main,repeat", help="main (C1-C6, C8, C7 first half), repeat (C7 second half)")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    runs = set(args.runs.split(","))
    rep = Report()
    ground: dict = {}
    if "main" in runs:
        with launch(args.out, args.scene, "main") as c:
            try:
                run_main(c, args.out, rep, ground)
            except ApiError as e:
                rep.add("main run", False, f"API error {e}")
        (args.out / "ground.json").write_text(json.dumps(ground))
    if "repeat" in runs:
        with launch(args.out, args.scene, "repeat") as c:
            c.set_clock(lockstep=True, utc="2026-06-15T19:00:00Z")
            c.set_camera(**C1_CAMERA)
            c7_dataset(c, args.out / "c7b")
        ok, detail = compare(args.out / "c7a", args.out / "c7b")
        rep.add("C7 determinism", ok, detail)
    ok = rep.write(args.out / "report.md")
    print(f"report: {args.out / 'report.md'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
