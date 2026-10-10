"""R0 gate 5: CIGI frame-centre heights (opcode 107) at nadir points vs 3DEP truth (NAVD88 -> ITRF2014 ellipsoid via
GEOID18). Pass: |error| <= 0.25 m where the 1 m DEM exists, <= 1.0 m on 1/3 arc-second data.

    uv run --project scripts/scene python scripts/scene/tools/hot_check.py package PKG --third SRC --onem SRC
    uv run --project scripts/scene python scripts/scene/tools/hot_check.py cwt --third SRC --onem SRC

SRC is a URL or a local path (e.g. the cached blob of the asset named in PKG/manifest.json).
"""

from __future__ import annotations

import argparse
import json
import socket
import sys
import time
from pathlib import Path

import pyproj
import rasterio
from camsim_session import Pose, camsim, env_for, rb  # also puts scripts/ on sys.path

# isort: split
import check_cigi_responses as ccr
import ocean_check
from coast_check import egm96

POINTS = [  # five near Oceanside (1 m D24), two in the base interior (1/3" only)
    (33.2100, -117.3700),
    (33.2300, -117.3600),
    # D24 stops at the base boundary (x46y368 is 57 % nodata): these three are inside it, on flat ground
    # (< 1.1 m relief within 50 m), so a frame-centre offset of a few metres doesn't read as height error
    (33.2179, -117.3541),
    (33.1998, -117.3755),
    (33.2360, -117.3327),
    (33.3800, -117.4200),
    (33.3600, -117.3950),  # was 33.42 -117.35: ~20 m relief within 10 m, where 1.5 m of offset is 3 m of height
]
# >= 2 km offshore, inside the bbox
SEA_POINTS = [(33.2100, -117.4300), (33.3000, -117.5000), (33.3700, -117.5900)]
SEA_LIMIT_M = 0.05
LIMIT_M = {"1m": 0.25, '1/3"': 1.0}
CIGI_RESPONSE_PORT = 8889


def open_src(src: str):
    return rasterio.open("/vsicurl/" + src) if src.startswith("http") else rasterio.open(src)


def truth(lat: float, lon: float, onem: str, third: str) -> tuple[float, str]:
    pyproj.network.set_network_enabled(True)  # a tool, not a build: PROJ may fetch GEOID18
    to_itrf = pyproj.Transformer.from_crs("EPSG:6318+5703", "EPSG:7912", always_xy=True)
    lo, la = pyproj.Transformer.from_crs("EPSG:7912", "EPSG:6318", always_xy=True).transform(lon, lat)
    for src, label in ((onem, "1m"), (third, '1/3"')):
        with rasterio.Env(GDAL_DISABLE_READDIR_ON_OPEN="EMPTY_DIR"), open_src(src) as ds:
            x, y = pyproj.Transformer.from_crs(ds.crs.geodetic_crs, ds.crs, always_xy=True).transform(lo, la)
            inside = ds.bounds.left <= x <= ds.bounds.right and ds.bounds.bottom <= y <= ds.bounds.top
            v = float(next(ds.sample([(x, y)]))[0]) if inside else None
            if v is not None and v != ds.nodata and v > -1000:
                return float(to_itrf.transform(lo, la, v, 2010.0)[2]), label
    raise SystemExit(f"no 3DEP truth at {lat}, {lon}")


def frame_centres(sock: socket.socket, seconds: float) -> list[dict]:
    got, t_end = [], time.time() + seconds
    while time.time() < t_end:
        try:
            d = sock.recv(65536)
        except TimeoutError:
            continue
        got += [s for s in ccr.parse_responses(d)["sensor_ext"] if s["status"] != 0]
    return got


def hat_hot(host, sock: socket.socket, rid: int, lat: float, lon: float, seconds: float = 3.0) -> dict | None:
    """One-shot HAT/HOT request (opcode 24, extended) and its response (opcode 103) with the same ID."""
    ocean_check.send_packets(host, ocean_check.pack_hat_hot_request(rid, lat, lon, req_type=2))
    t_end = time.time() + seconds
    while time.time() < t_end:
        try:
            d = sock.recv(65536)
        except TimeoutError:
            continue
        for r in ccr.parse_responses(d)["hat_hot"]:
            if r["op"] == 103 and r["id"] == rid and r["valid"]:
                return r
    return None


def sea_offset(pkg: Path | None) -> float:
    """sea_level.json offset_m (local MSL - EGM96) of the package; 0 on ion or when absent."""
    p = pkg and pkg / "sea_level.json"
    return float(json.loads(p.read_text())["offset_m"]) if p and p.exists() else 0.0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["package", "cwt"])
    ap.add_argument("pkg", type=Path, nargs="?")
    ap.add_argument("--third", required=True)
    ap.add_argument("--onem", required=True)
    a = ap.parse_args()
    if a.mode == "package" and not a.pkg:
        ap.error("package mode needs PKG")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", CIGI_RESPONSE_PORT))
    sock.settimeout(0.5)
    out, hots, sea = [], [], []
    offset = sea_offset(a.pkg)
    rid = 100
    start = Pose(lat=POINTS[0][0], lon=POINTS[0][1], alt=1500, gimbal_pitch=-90, fov_h=20)
    with camsim({**env_for(a.mode, a.pkg and a.pkg.resolve()), "CAMSIM_OCEAN_BEAUFORT": "0"}, start) as host:
        for lat, lon in POINTS:
            host.pose = Pose(lat=lat, lon=lon, alt=1500, gimbal_pitch=-90, fov_h=20)
            time.sleep(1)
            rb.wait_terrain(90)
            time.sleep(3)
            got = frame_centres(sock, 3.0)
            if not got:
                out.append({"pt": [lat, lon], "error": "no frame-centre response", "pass": False})
                print(f"{lat:.4f} {lon:.4f} no frame-centre response FAIL", flush=True)
                continue
            fc = got[-1]
            t, src = truth(fc["lat"], fc["lon"], a.onem, a.third)
            err = fc["alt"] - t
            rec = {
                "pt": [lat, lon],
                "fc": [fc["lat"], fc["lon"], round(fc["alt"], 2)],
                "truth": round(t, 2),
                "src": src,
                "err_m": round(err, 2),
                "pass": abs(err) <= LIMIT_M[src],
            }
            print(f"{lat:.4f} {lon:.4f} {src:5s} err {err:+.2f} m {'PASS' if rec['pass'] else 'FAIL'}", flush=True)
            out.append(rec)
            rid += 1
            r = hat_hot(host, sock, rid, lat, lon)
            if r is None:
                hots.append({"pt": [lat, lon], "error": "no HAT/HOT response", "pass": False})
                print(f"{lat:.4f} {lon:.4f} HOT no response FAIL", flush=True)
                continue
            t, src = truth(lat, lon, a.onem, a.third)
            err = r["hot"] - t
            hots.append(
                {
                    "pt": [lat, lon],
                    "hot": round(r["hot"], 2),
                    "truth": round(t, 2),
                    "src": src,
                    "err_m": round(err, 2),
                    "pass": abs(err) <= LIMIT_M[src],
                }
            )
            print(f"{lat:.4f} {lon:.4f} HOT {src:5s} err {err:+.2f} m {'PASS' if hots[-1]['pass'] else 'FAIL'}", flush=True)
        for lat, lon in SEA_POINTS:
            host.pose = Pose(lat=lat, lon=lon, alt=1500, gimbal_pitch=-90, fov_h=20)
            time.sleep(1)
            rb.wait_terrain(90)
            time.sleep(1)
            rid += 1
            r = hat_hot(host, sock, rid, lat, lon)
            expected = egm96(lat, lon) + offset
            if r is None:
                sea.append({"pt": [lat, lon], "expected": round(expected, 3), "error": "no HAT/HOT response", "pass": False})
                print(f"{lat:.4f} {lon:.4f} sea no response FAIL", flush=True)
                continue
            err = r["hot"] - expected
            sea.append(
                {
                    "pt": [lat, lon],
                    "hot": round(r["hot"], 3),
                    "expected": round(expected, 3),
                    "offset_m": offset,
                    "err_m": round(err, 3),
                    "pass": abs(err) <= SEA_LIMIT_M,
                }
            )
            print(f"{lat:.4f} {lon:.4f} sea HOT {r['hot']:.3f} expected {expected:.3f} {'PASS' if sea[-1]['pass'] else 'FAIL'}", flush=True)
    ok = bool(out) and all(r["pass"] for r in out + hots + sea) and len(sea) == len(SEA_POINTS)
    Path(f"hot_{a.mode}.json").write_text(json.dumps({"points": out, "hot": hots, "sea": sea, "pass": ok}, indent=1))
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
