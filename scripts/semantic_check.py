#!/usr/bin/env python3
"""Semantic ground truth acceptance (ROADMAP 4D, spec docs/superpowers/specs/2026-10-16-semantic-ground-truth-design.md §5).

Through the control API on a scene package (sites: scripts/semantic/sites_pendleton.json):

  S1 entity pixels   a truck (vehicle) and a boat (vessel): >= 99 % of each visible-mask pixel has its class, and no
                     entity-class pixel lies outside the union of the visible masks
  S2 sky             a 10 deg depression view: sky pixels exist and every one is a depth-far pixel (depth PNG 65535)
  S3 terrain         nadir 300 m: flat -> road >= 90 %, roof -> building >= 50 %, sea -> water >= 99 %, tree -> tree >= 80 %
                     of the centre 20 % (by width and height)
  S4 EO = IR         the flat-site view in EO and IR: identical class images
  S5 dataset         make_dataset writes semantic/ for every image, ids in the class table, camsim_semantic_classes
  S6 cost            two launches (every frame annotated; semantic files off vs on), the same nadir view held 600
                     frames: GPU frame time p50 difference (informational unless > 0.3 ms)

    uv run --project scripts/control --with pillow --with numpy --with pycocotools \\
        python scripts/semantic_check.py --scene .cache/scene-packages/pendleton-bridges --out .cache/semantic/check
"""

from __future__ import annotations

import argparse
import io
import json
import statistics
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts"))
sys.path.insert(0, str(REPO / "scripts" / "control"))
import control_api_check as k  # noqa: E402
import make_dataset  # noqa: E402
import numpy as np  # noqa: E402
from camsim_control.coco import load_categories  # noqa: E402
from camsim_control.randomize import EntitySpec  # noqa: E402
from camsim_control.semantic import CLASSES  # noqa: E402
from PIL import Image  # noqa: E402
from pycocotools import mask as mask_util  # noqa: E402

SITES = {s["name"]: s for s in json.loads((REPO / "scripts/semantic/sites_pendleton.json").read_text())["sites"]}
NAME_TO_ID = {v: k_ for k_, v in CLASSES.items()}
PALETTE = np.zeros((256, 3), np.uint8)
for cid, rgb in {0: (0, 0, 0), 1: (120, 170, 255), 2: (20, 60, 200), 3: (90, 90, 90), 4: (140, 80, 40),
                 5: (220, 40, 40), 6: (200, 120, 120), 7: (20, 120, 20), 8: (120, 200, 80), 9: (190, 170, 120),
                 10: (255, 255, 255), 20: (255, 220, 0), 21: (255, 0, 255), 22: (0, 255, 255), 23: (255, 128, 0),
                 24: (255, 255, 128), 25: (128, 0, 255)}.items():
    PALETTE[cid] = rgb


def gray(data: bytes) -> np.ndarray:
    return np.asarray(Image.open(io.BytesIO(data)))


def overlay(out: Path, name: str, cap) -> None:
    img = np.asarray(Image.open(io.BytesIO(cap.png)).convert("RGB"))
    sem = PALETTE[gray(cap.semantic_png)]
    Image.fromarray(np.hstack([img, sem])).save(out / f"{name}.png")


def centre(a: np.ndarray, frac: float = 0.2) -> np.ndarray:
    h, w = a.shape[:2]
    dh, dw = int(h * frac / 2), int(w * frac / 2)
    return a[h // 2 - dh:h // 2 + dh, w // 2 - dw:w // 2 + dw]


def ground_at(c, lat: float, lon: float) -> float:
    """The ground's ellipsoid height under a point: a marker clamped there, then removed (it must not be in shots)."""
    c.put_entity(900, type_id=9001, lat=lat, lon=lon, alt=0.0, surface="ground")
    c.set_camera(**k.camera_at((lat, lon), 0.0, 300.0, 90.0, 0.0, 20.0, "eo"))
    c.capture(settle_frames=5, semantic=False, depth=False)   # tiles under the point loaded
    e = k.committed(c, 900)
    c.delete_entity(900)
    return e["alt"] if e else 0.0


def run_main(c, out: Path, rep: k.Report) -> None:
    shots = out / "shots"
    shots.mkdir(parents=True, exist_ok=True)
    c.set_clock(lockstep=True, utc="2026-06-15T19:00:00Z")
    c.set_camera(**k.C1_CAMERA)
    c.clear_entities()

    # S3 terrain classes, and S4 on the flat site
    flat_ground = None
    for name in ("flat", "roof", "tree", "sea"):
        s = SITES[name]
        g = 0.0 if name == "sea" else ground_at(c, s["lat"], s["lon"])
        if name == "flat":
            flat_ground = g
        c.set_camera(**k.camera_at((s["lat"], s["lon"]), g, 300.0, 90.0, 0.0, 20.0, "eo"))
        cap = c.capture()
        overlay(shots, f"s3_{name}", cap)
        sem = centre(gray(cap.semantic_png))
        want = NAME_TO_ID[s["expect"]]
        frac = float((sem == want).mean())
        need = {"flat": 0.9, "roof": 0.5, "sea": 0.99, "tree": 0.8}[name]
        top = sorted(((int((sem == v).sum()), CLASSES.get(int(v), str(v))) for v in np.unique(sem)), reverse=True)[:3]
        rep.add(f"S3 {name}", frac >= need, f"{s['expect']} {frac:.1%} of the centre (need {need:.0%}); top {top}")
        if name == "flat":
            c.set_camera(sensor={"mode": "ir"})
            ir = c.capture()
            same = np.array_equal(gray(cap.semantic_png), gray(ir.semantic_png))
            diff = int((gray(cap.semantic_png) != gray(ir.semantic_png)).sum())
            rep.add("S4 EO = IR", same, f"{diff} pixels differ between the EO and IR class images")
            c.set_camera(sensor={"mode": "eo"})

    # S1 entity pixels: a truck on the flat site, a boat offshore
    c.put_entity(1, type_id=2001, lat=SITES["flat"]["lat"], lon=SITES["flat"]["lon"], alt=0.0, heading=45.0, surface="ground")
    c.put_entity(2, type_id=3001, lat=SITES["sea"]["lat"], lon=SITES["sea"]["lon"], alt=0.0, heading=200.0, surface="water")
    ok1, notes = True, []
    for name, site, cls, sid in (("truck", "flat", 20, "1"), ("boat", "sea", 21, "2")):
        s = SITES[site]
        e = k.committed(c, int(sid))   # aim at the entity's own height (sea level is ~-35 m HAE here)
        ground = e["alt"] if e else (flat_ground or -11.0)
        c.set_camera(**k.camera_at((s["lat"], s["lon"]), ground, 400.0, 50.0, 30.0, 4.0, "eo"))
        cap = c.capture()
        overlay(shots, f"s1_{name}", cap)
        sem = gray(cap.semantic_png)
        anns = (cap.record or {}).get("annotations", [])
        union = np.zeros(sem.shape, bool)
        inside_frac = None
        for a in anns:
            if "segmentation" not in a:
                continue
            m = mask_util.decode(a["segmentation"]).astype(bool)
            union |= m
            if a.get("source_id") == sid:
                inside_frac = float((sem[m] == cls).mean()) if m.any() else 0.0
        entity_px = sem >= 20
        outside = int((entity_px & ~union).sum())
        good = inside_frac is not None and inside_frac >= 0.99 and outside == 0
        ok1 &= good
        notes.append(f"{name}: {inside_frac if inside_frac is None else f'{inside_frac:.2%}'} of its mask is "
                     f"{CLASSES[cls]}, {outside} entity-class px outside masks")
    rep.add("S1 entity pixels", ok1, "; ".join(notes))
    c.clear_entities()

    # S2 sky: a low view over the sea toward the horizon
    s = SITES["sea"]
    c.set_camera(**k.camera_at((s["lat"], s["lon"]), 0.0, 2000.0, 10.0, 270.0, 40.0, "eo"))
    cap = c.capture()
    overlay(shots, "s2_sky", cap)
    sem, depth = gray(cap.semantic_png), gray(cap.depth_png)
    sky = sem == 1
    bad = int((sky & (depth != 65535)).sum())
    rep.add("S2 sky", sky.sum() > 0 and bad == 0, f"{int(sky.sum())} sky px, {bad} of them not at the depth far plane")

    # S5 dataset
    ds = make_dataset.generate(c, out / "s5", 4, 3, (SITES["flat"]["lat"], SITES["flat"]["lon"]), flat_ground or -11.0,
                               ["eo"], [EntitySpec(2001, "ground")], load_categories(REPO / "deploy" / "camsim_config.yaml"),
                               log=lambda m: print("  " + m))
    coco = json.loads(ds.read_text())
    files = [im["camsim"].get("semantic_file") for im in coco["images"]]
    ids = set()
    for f in files:
        if f:
            ids |= set(np.unique(np.asarray(Image.open(out / "s5" / f))).tolist())
    ok5 = all(files) and ids <= set(CLASSES) and "camsim_semantic_classes" in coco
    rep.add("S5 dataset", ok5, f"{sum(bool(f) for f in files)}/{len(files)} images with semantic/; classes seen "
            f"{sorted(CLASSES.get(i, str(i)) for i in ids)}")


def run_cost(out: Path, scene: Path | None, rep: k.Report) -> None:
    p50 = {}
    for tag, on in (("cost-off", "0"), ("cost-on", "1")):
        stats = out / f"{tag}.jsonl"
        stats.unlink(missing_ok=True)
        env = {"CAMSIM_ML_INTERVAL_FRAMES": "1", "CAMSIM_ML_SEMANTIC_ENABLED": on, "CAMSIM_FRAME_STATS_PATH": str(stats)}
        with k.launch(out, scene, tag, env) as c:
            s = SITES["flat"]
            c.set_clock(lockstep=True, utc="2026-06-15T19:00:00Z")
            c.set_camera(**k.camera_at((s["lat"], s["lon"]), -11.0, 300.0, 90.0, 0.0, 20.0, "eo"))
            c.capture(settle_frames=60, semantic=False)
            start = time.time()
            time.sleep(25.0)
        rows = [json.loads(line) for line in stats.read_text().splitlines() if line.strip()]
        rows = [r for r in rows if r.get("t", 0) >= rows[-1]["t"] - 20.0] if rows else []
        p50[tag] = statistics.median(r["gpu_ms"] for r in rows) if rows else float("nan")
        print(f"  {tag}: {len(rows)} frames, gpu p50 {p50[tag]:.3f} ms ({time.time() - start:.0f} s)")
    d = p50["cost-on"] - p50["cost-off"]
    rep.add("S6 cost", None if d <= 0.3 else False, f"GPU frame p50 {p50['cost-off']:.3f} -> {p50['cost-on']:.3f} ms "
            f"(+{d:.3f} ms; every frame annotated, 720p)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--scene", type=Path, default=None)
    ap.add_argument("--runs", default="main,cost")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    rep = k.Report()
    runs = set(args.runs.split(","))
    if "main" in runs:
        with k.launch(args.out, args.scene, "main") as c:
            run_main(c, args.out, rep)
    if "cost" in runs:
        run_cost(args.out, args.scene, rep)
    ok = rep.write(args.out / "report.md", "Semantic ground truth acceptance")
    print(f"report: {args.out / 'report.md'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
