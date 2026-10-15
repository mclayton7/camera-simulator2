"""REALISM R3 bridges acceptance (spec docs/superpowers/specs/2026-10-15-bridges-ripples-design.md section 1.7) on a
scene package with features/bridges/:

    uv run --project scripts/scene python scripts/scene/tools/bridges_check.py --scene PKG --out DIR [--runs ...]

Runs (one CamSim each): `hot` (B2 mid-span CIGI HOT = the deck top, material 200/201/210; B3 grounded end vs 5 m
beyond on the approach), `drive` (B4 a DIS truck along a motorway deck both ways; B5 a truck on a road under a deck),
`shots` (B8 EO + MWIR oblique and nadir of the three highest decks). B6 (boat) and B7 (bench) are run separately
(`ocean_check.py`, `bench/run_bench.py`); their results go in the docs. Writes DIR/report.json and prints a table.
The decks are re-planned from the package's features cache and terrain (the same plan verify --deep uses)."""

from __future__ import annotations

import argparse
import dataclasses
import json
import math
import sys
import threading
import time
from pathlib import Path

import numpy as np
from shapely.geometry import LineString, Point

from camsim_scene.bridges import Deck, _scale, deck_height, metric, select

B2_TOL_M, B3_TOL_M, B4_TOL_M, B4_STEP_M, B5_BELOW_M = 0.10, 0.5, 0.3, 0.3, 3.0
ROAD_CODES = {200, 201, 210}
NODE_DIGITS = 9
TRUCK_ID = 7
UNDERPASS_END_M = 15.0  # a road crossing a deck closer than this to its end is its approach, not an underpass


def node_key(pt) -> tuple:
    return (round(pt[0], NODE_DIGITS), round(pt[1], NODE_DIGITS))


def grounded(features) -> dict:
    """(source_id, end 0 | 1) -> whether that OSM way end is grounded (degree 1 in its kind's bridge graph, as
    plan_decks). Keyed by way, not by point: a deck's line is extended past a grounded end (to the embankment)."""
    deg: dict = {}
    sel = select(features)
    for f in sel:
        for p in (f.geom.coords[0], f.geom.coords[-1]):
            k = (f.kind, node_key(p))
            deg[k] = deg.get(k, 0) + 1
    return {(f.source_id, e): deg[(f.kind, node_key(f.geom.coords[-1 if e else 0]))] == 1 for f in sel for e in (0, 1)}


def pick(decks: list[Deck], n: int) -> list[Deck]:
    return sorted(decks, key=lambda d: (-d.max_clearance_m, d.source_id))[:n]


def _at(line: LineString, s_m: float) -> tuple[float, float]:
    """(lat, lon) at arc length s_m (m) along a lon/lat line; beyond the ends it extends the end segments."""
    c0 = line.coords[0]
    p = metric(line, c0[1], c0[0])
    seg = np.linalg.norm(np.diff(p, axis=0), axis=1)
    cum = np.concatenate([[0.0], np.cumsum(seg)])
    if s_m <= 0:
        u = (p[1] - p[0]) / seg[0]
        q = p[0] + u * s_m
    elif s_m >= cum[-1]:
        u = (p[-1] - p[-2]) / seg[-1]
        q = p[-1] + u * (s_m - cum[-1])
    else:
        q = np.array([np.interp(s_m, cum, p[:, 0]), np.interp(s_m, cum, p[:, 1])])
    sx, sy = _scale(c0[1])
    return c0[1] + q[1] / sy, c0[0] + q[0] / sx


def midspan(d: Deck) -> tuple[float, float, float]:
    lat, lon = _at(d.line, d.length_m / 2.0)
    return lat, lon, deck_height(d, d.length_m / 2.0)


def grounded_ends(d: Deck, ground: dict, beyond_m: float = 5.0) -> list[dict]:
    out = []
    for e, end, s, out_s, h in ((0, d.line.coords[0], 0.0, -beyond_m, d.h0),
                                (1, d.line.coords[-1], d.length_m, d.length_m + beyond_m, d.h1)):
        if ground.get((d.source_id, e)):
            out.append({"end": (end[1], end[0]), "beyond": _at(d.line, out_s), "h": h, "s": s})
    return out


def evaluate_b2(hot: dict, deck_top: float) -> dict:
    err = float(hot["hot"]) - deck_top
    ok = abs(err) <= B2_TOL_M and hot.get("material") in ROAD_CODES
    return {"pass": bool(ok), "error_m": round(err, 3), "material": hot.get("material")}


def evaluate_b3(hot_end: float, hot_beyond: float) -> dict:
    d = hot_beyond - hot_end
    return {"pass": abs(d) <= B3_TOL_M, "step_m": round(d, 3)}


def along(line: LineString, lat: float, lon: float) -> float:
    """Arc length (m) of the projection of (lat, lon) on the line, negative / past the end beyond it."""
    c0 = line.coords[0]
    p = metric(line, c0[1], c0[0])
    sx, sy = _scale(c0[1])
    q = Point((lon - c0[0]) * sx, (lat - c0[1]) * sy)
    lm = LineString(p)
    s = lm.project(q)
    if 0 < s < lm.length:
        return s
    a, b = (p[0], p[1]) if s <= 0 else (p[-2], p[-1])
    u = (b - a) / np.linalg.norm(b - a)
    base = 0.0 if s <= 0 else lm.length
    return base + float(np.dot(np.array([q.x, q.y]) - (a if s <= 0 else b), u))


def evaluate_over(track: list[tuple[float, float]], d: Deck) -> dict:
    """track: (arc length along the deck, COCO alt) in time order. Mid-span samples (20-80 % of the deck) on the deck
    top within B4_TOL_M; the largest frame-to-frame height change within B4_STEP_M."""
    mid = [(s, a) for s, a in track if 0.2 * d.length_m <= s <= 0.8 * d.length_m]
    errs = [a - deck_height(d, s) for s, a in mid]
    alts = [a for _, a in track]
    step = max((abs(b - a) for a, b in zip(alts, alts[1:])), default=math.inf)
    ok = bool(mid) and max(abs(e) for e in errs) <= B4_TOL_M and step <= B4_STEP_M
    return {"pass": ok, "mid_samples": len(mid), "max_deck_error_m": round(max((abs(e) for e in errs), default=math.nan), 3),
            "max_step_m": round(step, 3), "samples": len(track)}


def evaluate_under(alts: list[float], deck_top: float) -> dict:
    hi = max(alts) if alts else math.inf
    return {"pass": bool(alts) and hi <= deck_top - B5_BELOW_M, "highest_m": round(hi, 3), "deck_top_m": round(deck_top, 3),
            "samples": len(alts)}


def find_underpass(decks: list[Deck], features) -> dict | None:
    """A non-bridge road crossing a kept deck at least UNDERPASS_END_M inside it, on a deck with >= 6 m clearance
    (highest clearance first)."""
    roads = [f for f in features if f.kind == "road" and f.tags.get("bridge", "no") == "no"
             and f.tags.get("tunnel", "no") == "no" and f.geom.geom_type == "LineString"]
    osm = {f.source_id: f.geom for f in features}
    for d in pick(decks, len(decks)):
        if d.max_clearance_m < 6.0:
            break
        way = osm.get(d.source_id, d.line)  # the OSM way: the deck's line may extend past its ends
        ends = {node_key(way.coords[0]), node_key(way.coords[-1])}
        for r in sorted(roads, key=lambda r: r.source_id):
            if node_key(r.geom.coords[0]) in ends or node_key(r.geom.coords[-1]) in ends:
                continue  # the bridge's own approach
            x = way.intersection(r.geom)
            if x.is_empty or x.geom_type != "Point":
                continue
            s = along(way, x.y, x.x)
            if s < UNDERPASS_END_M or s > _length_m(way) - UNDERPASS_END_M:  # at the abutment, not under the span
                continue
            return {"deck": d.source_id, "road": r.source_id, "crossing": (x.y, x.x),
                    "deck_top": deck_height(d, along(d.line, x.y, x.x)), "road_line": list(r.geom.coords)}
    return None


def track_along(line: LineString, beyond_m: float, step_m: float) -> list[tuple[float, float, float]]:
    """(lat, lon, heading deg) every step_m from beyond_m before the start to beyond_m past the end."""
    total = _length_m(line)
    out = []
    s = -beyond_m
    while s <= total + beyond_m + 1e-6:
        a, b = _at(line, s), _at(line, s + 0.5)
        sx, sy = _scale(a[0])
        h = math.degrees(math.atan2((b[1] - a[1]) * sx, (b[0] - a[0]) * sy)) % 360.0
        out.append((a[0], a[1], h))
        s += step_m
    return out


def _length_m(line: LineString) -> float:  # metres along a lon/lat line
    c0 = line.coords[0]
    p = metric(line, c0[1], c0[0])
    return float(np.linalg.norm(np.diff(p, axis=0), axis=1).sum())


# --- the runs (one CamSim each) ----------------------------------------------------------------------------------------
def load_plan(pkg: Path):
    """(decks, features) re-planned from the package's features cache and terrain."""
    from camsim_scene.cache import Cache, default_cache_root
    from camsim_scene.layers import bridges as layer
    from camsim_scene.manifest import Manifest
    from camsim_scene.pipeline import _load_features, make_context

    m = Manifest.load(pkg / "manifest.json")
    if not m.layers.get("bridges"):
        sys.exit(f"{pkg}: no bridges layer (build the package with [bridges])")
    ctx = make_context(pkg, m, Cache(default_cache_root()))
    feats = _load_features(m, ctx)
    decks, stats, _ = layer.plan(m, ctx, feats)
    return decks, feats, stats


def _modules():
    import camsim_session as cs  # puts scripts/ on sys.path
    import ocean_check as oc  # noqa: I001
    import send_dis_test as sd

    return cs, oc, sd


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
    return {"valid": False}


def run_hot(pkg: Path, decks: list[Deck], feats, out: Path, n: int = 10) -> dict:
    cs, oc, _ = _modules()
    deg = grounded(feats)
    chosen = pick(decks, n)
    responses = oc.Responses()
    res = []
    rid = 300
    env = {**cs.env_for("package", pkg), "CAMSIM_DIS_ENABLED": "0", "CAMSIM_MULTICAST_ADDR": "127.0.0.1"}
    lat0, lon0, h0 = midspan(chosen[0])
    try:
        with cs.camsim(env, cs.Pose(lat0, lon0, h0 + 400.0, gimbal_pitch=-90.0, fov_h=30.0)) as host:
            for d in chosen:
                lat, lon, top = midspan(d)
                host.pose = cs.Pose(lat, lon, top + 400.0, gimbal_pitch=-90.0, fov_h=30.0)
                time.sleep(1.0)
                cs.rb.wait_terrain(180)
                rid += 1
                hot = _hot(responses, oc, host, rid, lat, lon)
                r = {"deck": d.source_id, "kind": d.kind, "clearance_m": round(d.max_clearance_m, 2),
                     "midspan": [lat, lon], "deck_top": round(top, 3)}
                r["b2"] = evaluate_b2(hot, top) if hot.get("valid") else {"pass": False, "error": "no HOT"}
                r["b3"] = []
                for e in grounded_ends(d, deg):
                    rid += 2
                    he = _hot(responses, oc, host, rid - 1, *e["end"])
                    hb = _hot(responses, oc, host, rid, *e["beyond"])
                    if he.get("valid") and hb.get("valid"):
                        r["b3"].append({**evaluate_b3(he["hot"], hb["hot"]), "end": e["end"], "hot_end": he["hot"],
                                        "deck_end": round(e["h"], 3)})
                    else:
                        r["b3"].append({"pass": False, "end": e["end"], "error": "no HOT"})
                res.append(r)
                print(f"[bridges] {d.source_id}: B2 {r['b2']}  B3 {[x.get('step_m') for x in r['b3']]}", flush=True)
    finally:
        responses.stop.set()
    return {"decks": res, "b2_pass": all(r["b2"]["pass"] for r in res),
            "b3_pass": all(x["pass"] for r in res for x in r["b3"]) and any(r["b3"] for r in res)}


class _Truck:
    """Sends the DIS truck's Entity State at 10 Hz along a list of (lat, lon, heading) from a function of time."""

    def __init__(self, sd, where):
        import socket

        self.sd, self.where = sd, where
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.stop = threading.Event()
        self.t0 = time.monotonic()
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while not self.stop.is_set():
            lat, lon, h, v = self.where(time.monotonic() - self.t0)
            self.sock.sendto(self.sd.pack_entity_state(TRUCK_ID, self.sd.TRUCK_TYPE, lat, lon, 0.0, h, v, 0.0, 1,
                                                       "BRIDGE1", time.time()), ("127.0.0.1", 3000))
            time.sleep(0.1)


def _coco_alts(path: Path, start: int) -> tuple[list[dict], int]:
    if not path.exists():
        return [], start
    with path.open("rb") as f:
        f.seek(start)
        data = f.read()
    end = start + data.rfind(b"\n") + 1 if b"\n" in data else start
    out = []
    for line in data[: end - start].decode().splitlines():
        if line.strip():
            rec = json.loads(line)
            for a in rec.get("annotations", []):
                if a.get("source") == "dis" and "geo" in a:
                    out.append({"frame": rec["frame_id"], **a["geo"]})
    return out, end


def _drive(cs, sd, host, coco: Path, line: LineString, top_of, beyond_m: float, speed: float) -> list[dict]:
    """Drive the truck along `line` from beyond_m before it to beyond_m past it, the camera nadir above the truck."""
    total = _length_m(line)
    dur = (total + 2 * beyond_m) / speed

    def where(t):
        s = min(-beyond_m + speed * t, total + beyond_m)
        a, b = _at(line, s), _at(line, s + 0.5)
        sx, sy = _scale(a[0])
        return a[0], a[1], math.degrees(math.atan2((b[1] - a[1]) * sx, (b[0] - a[0]) * sy)) % 360.0, speed

    _, start = _coco_alts(coco, coco.stat().st_size if coco.exists() else 0)
    truck = _Truck(sd, where)
    try:
        t_end = time.monotonic() + dur + 3.0
        while time.monotonic() < t_end:
            lat, lon, _, _ = where(time.monotonic() - truck.t0)
            host.pose = cs.Pose(lat, lon, top_of(lat, lon) + 250.0, gimbal_pitch=-90.0, fov_h=30.0)
            time.sleep(0.1)
    finally:
        truck.stop.set()
    time.sleep(1.0)
    alts, _ = _coco_alts(coco, start)
    return alts


def run_drive(pkg: Path, decks: list[Deck], feats, out: Path, speed: float = 10.0) -> dict:
    cs, _, sd = _modules()
    coco = out / "drive" / "ml" / "camsim_coco.jsonl"
    env = {**cs.env_for("package", pkg), "CAMSIM_DIS_ENABLED": "1", "CAMSIM_ML_ENABLED": "1",
           "CAMSIM_ML_OUTPUT_DIR": str(coco.parent), "CAMSIM_ML_DEPTH_ENABLED": "0", "CAMSIM_ML_INTERVAL_FRAMES": "1",
           "CAMSIM_MULTICAST_ADDR": "127.0.0.1"}
    roads = [d for d in pick(decks, len(decks)) if d.kind == "road" and d.length_m >= 80.0]
    over = roads[0]
    under = find_underpass(decks, feats)
    res: dict = {"over": {"deck": over.source_id, "length_m": round(over.length_m, 1)}, "under": under and {
        k: under[k] for k in ("deck", "road", "crossing", "deck_top")}}
    lat0, lon0, h0 = midspan(over)
    with cs.camsim(env, cs.Pose(lat0, lon0, h0 + 250.0, gimbal_pitch=-90.0, fov_h=30.0)) as host:
        cs.rb.wait_terrain(180)
        for name, line in (("forward", over.line), ("reverse", LineString(list(over.line.coords)[::-1]))):
            alts = _drive(cs, sd, host, coco, line, lambda la, lo: h0, 60.0, speed)
            track = [(along(over.line, a["lat"], a["lon"]), a["alt_m"]) for a in alts]
            res["over"][name] = evaluate_over(track, over)
            (out / "drive" / f"over_{name}.json").write_text(json.dumps(alts) + "\n")
            print(f"[bridges] over {name}: {res['over'][name]}", flush=True)
        if under:
            road = LineString(under["road_line"])
            cx = along(road, *under["crossing"])
            sub = LineString([_at(road, cx - 80.0)[::-1], _at(road, cx + 80.0)[::-1]])
            alts = _drive(cs, sd, host, coco, sub, lambda la, lo: under["deck_top"], 0.0, speed)
            near = [a["alt_m"] for a in alts if abs(along(sub, a["lat"], a["lon"]) - 80.0) <= 10.0]
            res["under"]["result"] = evaluate_under(near, under["deck_top"])
            (out / "drive" / "under.json").write_text(json.dumps(alts) + "\n")
            print(f"[bridges] under: {res['under']['result']}", flush=True)
    res["b4_pass"] = all(res["over"][k]["pass"] for k in ("forward", "reverse"))
    res["b5_pass"] = bool(under) and res["under"]["result"]["pass"]
    return res


def run_shots(pkg: Path, decks: list[Deck], out: Path) -> dict:
    cs, _, _ = _modules()
    env = {**cs.env_for("package", pkg), "CAMSIM_DIS_ENABLED": "0", "CAMSIM_MULTICAST_ADDR": "127.0.0.1",
           "CAMSIM_SNAPSHOT_ENDPOINT_ENABLED": "1"}
    shots = []
    chosen = pick(decks, 3)
    lat0, lon0, h0 = midspan(chosen[0])
    (out / "shots").mkdir(parents=True, exist_ok=True)
    with cs.camsim(env, cs.Pose(lat0, lon0, h0 + 500.0, gimbal_pitch=-90.0, fov_h=30.0)) as host:
        for d in chosen:
            lat, lon, top = midspan(d)
            a, b = _at(d.line, d.length_m / 2 - 1), _at(d.line, d.length_m / 2 + 1)
            sx, sy = _scale(lat)
            hd = math.degrees(math.atan2((b[1] - a[1]) * sx, (b[0] - a[0]) * sy))
            look = (hd + 90.0) % 360.0  # across the deck
            back = math.radians(look + 180.0)
            olat, olon = lat + 566.0 * math.cos(back) / sy, lon + 566.0 * math.sin(back) / sx
            for view, pose in (("oblique", cs.Pose(olat, olon, top + 566.0, yaw=look, gimbal_pitch=-45.0, fov_h=20.0)),
                               ("nadir", cs.Pose(lat, lon, top + 500.0, gimbal_pitch=-90.0, fov_h=30.0))):
                for band, sensor in (("eo", 0), ("mwir", 1)):
                    host.pose = dataclasses.replace(pose, sensor_id=sensor)
                    time.sleep(2.0)
                    cs.rb.wait_terrain(180)
                    time.sleep(3.0)
                    name = f"{d.source_id.replace(':', '_')}_{view}_{band}.png"
                    cs.rb.fetch_snapshot(out / "shots" / name)
                    shots.append(name)
    return {"shots": shots}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scene", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--runs", default="hot,drive,shots")
    a = ap.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    pkg = a.scene.resolve()
    decks, feats, stats = load_plan(pkg)
    report_path = out / "report.json"
    report = json.loads(report_path.read_text()) if report_path.exists() else {}
    report["plan"] = {"package": str(pkg), **stats}
    for run in a.runs.split(","):
        report[run] = {"hot": lambda: run_hot(pkg, decks, feats, out), "drive": lambda: run_drive(pkg, decks, feats, out),
                       "shots": lambda: run_shots(pkg, decks, out)}[run]()
        report_path.write_text(json.dumps(report, indent=1) + "\n")
    gates = {k: report[r][k] for r, k in (("hot", "b2_pass"), ("hot", "b3_pass"), ("drive", "b4_pass"),
                                         ("drive", "b5_pass")) if r in report}
    print(json.dumps(gates))
    return 0 if all(gates.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
