"""REALISM R6 datum gates (spec docs/superpowers/specs/2026-10-15-r6-georeg-determinism-design.md section 1.6): the
package's stored PROJ pipeline for NAD83(2011) + NAVD88 (GEOID18) -> ITRF2014 against NGS-published values. No
rendering.

    uv run --project scripts/scene python scripts/scene/tools/datum_check.py PKG [--data data/ngs_pendleton.json]
        [--cache .cache/scene] [--json out.json]

D1 geoid  the manifest's `vgridshift` step (the vendored, hash-pinned GEOID18 grid) at each survey mark vs the mark's
          published GEOID18 height: <= 5 mm at every mark.
D2 frame  the rest of the manifest's `to_itrf_3d` (NAD83(2011) -> ITRF2014 Helmert at epoch 2010.0) applied to each
          CORS antenna reference point's published NAD83(2011) position vs its published ITRF2014 position (NGS
          computed both, with HTDP): <= 1 cm horizontally and <= 1 cm vertically at every station.
Informational: GEOID18's own fit at the marks (published ellipsoid height - NAVD88 height - our N); the package's
vertical-offset lattice (datum.offset_lattice, zooms 12 / 14 / 16) vs the exact offset at the marks inside its bbox.
The reference data comes from fetch_ngs.py (data/ngs_pendleton.json). Exit 0 when D1 and D2 pass, 1 otherwise."""

from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
from pathlib import Path

import numpy as np
import pyproj

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from camsim_scene import datum as D  # noqa: E402

REPO = Path(__file__).resolve().parents[3]
DATA = Path(__file__).resolve().parent / "data" / "ngs_pendleton.json"
CACHE = REPO / ".cache" / "scene"
ENTRY = "nad83_2011_navd88_geoid18"
GRID = "us_noaa_g2018u0.tif"
D1_LIMIT_M = 0.005
D2_LIMIT_M = 0.01


def grid_path(manifest: dict, cache: Path = CACHE) -> Path:
    sha = manifest["datum"]["grids"][GRID]["sha256"]
    return cache / "blobs" / "sha256" / sha[:2] / sha


def _steps(pipeline: str) -> list[str]:
    head, *steps = pipeline.split(" +step ")
    assert head.strip() == "+proj=pipeline", pipeline
    return [s.strip() for s in steps]


def _pipeline(steps: list[str], grid: Path) -> pyproj.Transformer:
    pyproj.network.set_network_enabled(False)
    text = "+proj=pipeline " + " ".join(f"+step {s}" for s in steps)
    return pyproj.Transformer.from_pipeline(D.resolve_grids(text, {GRID: grid}))


def geoid_transform(manifest: dict, grid: Path) -> pyproj.Transformer:
    """NAD83(2011) lon/lat + NAVD88 height -> NAD83(2011) ellipsoid height: the manifest's vgridshift step alone."""
    steps = _steps(manifest["datum"]["datums"][ENTRY]["to_itrf_3d"])
    vg = [s for s in steps if s.startswith("+proj=vgridshift")]
    if len(vg) != 1:
        raise ValueError(f"expected one vgridshift step in {ENTRY}.to_itrf_3d, found {len(vg)}")
    return _pipeline(["+proj=unitconvert +xy_in=deg +xy_out=rad", vg[0],
                      "+proj=unitconvert +xy_in=rad +xy_out=deg"], grid)


def frame_transform(manifest: dict, grid: Path) -> pyproj.Transformer:
    """NAD83(2011) lon/lat/ellipsoid height -> ITRF2014: the manifest's to_itrf_3d without its vgridshift step."""
    steps = [s for s in _steps(manifest["datum"]["datums"][ENTRY]["to_itrf_3d"]) if not s.startswith("+proj=vgridshift")]
    return _pipeline(steps, grid)


def check_geoid(manifest: dict, grid: Path, marks: list[dict]) -> dict:
    t = geoid_transform(manifest, grid)
    epoch = manifest["datum"]["epoch"]
    rows = []
    for m in marks:
        n_ours = float(t.transform(m["lon"], m["lat"], 0.0, epoch)[2])
        rows.append({"pid": m["pid"], "n_ours": n_ours, "n_ngs": m["geoid18_n"], "diff_m": n_ours - m["geoid18_n"],
                     "fit_m": m["ellip_h"] - m["navd88_h"] - n_ours})
    worst = max(abs(r["diff_m"]) for r in rows)
    fits = sorted(abs(r["fit_m"]) for r in rows)
    return {"pass": worst <= D1_LIMIT_M, "n": len(rows), "max_abs_m": worst, "limit_m": D1_LIMIT_M,
            "geoid18_fit": {"median_abs_m": statistics.median(fits), "max_abs_m": fits[-1]}, "marks": rows}


def check_frame(manifest: dict, grid: Path, cors: list[dict]) -> dict:
    t = frame_transform(manifest, grid)
    rows = []
    for c in cors:
        n83, itrf = c["nad83_2011"], c["itrf2014"]
        lon, lat, h, _ = t.transform(n83["lon"], n83["lat"], n83["h"], c["epoch"])
        dn = (lat - itrf["lat"]) * 111_132.0
        de = (lon - itrf["lon"]) * 111_320.0 * math.cos(math.radians(lat))
        rows.append({"id": c["id"], "horiz_m": math.hypot(de, dn), "vert_m": h - itrf["h"], "de_m": de, "dn_m": dn})
    hmax = max(r["horiz_m"] for r in rows)
    vmax = max(abs(r["vert_m"]) for r in rows)
    return {"pass": hmax <= D2_LIMIT_M and vmax <= D2_LIMIT_M, "n": len(rows), "max_horiz_m": hmax,
            "max_vert_m": vmax, "limit_m": D2_LIMIT_M, "stations": rows}


def lattice_error(manifest: dict, grid: Path, marks: list[dict]) -> dict:
    w, s, e, n = manifest["bbox"]
    inside = [m for m in marks if w <= m["lon"] <= e and s <= m["lat"] <= n]
    if not inside:
        return {"n": 0}
    dt = D.DatumTransform(manifest["datum"]["datums"][ENTRY], {GRID: grid})
    lon = np.array([m["lon"] for m in inside])
    lat = np.array([m["lat"] for m in inside])
    # ITRF lon/lat of the marks, as offset_lattice takes them (the package's lattice is in ITRF coordinates)
    slon, slat = lon, lat
    exact = dt.vertical_offset(slon, slat)
    out = {"n": len(inside)}
    for z in (12, 14, 16):
        lat_off = D.offset_lattice(dt, z, lon, lat)
        out[f"z{z}_max_abs_m"] = float(np.max(np.abs(lat_off - exact)))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pkg", type=Path)
    ap.add_argument("--data", type=Path, default=DATA)
    ap.add_argument("--cache", type=Path, default=CACHE)
    ap.add_argument("--json", type=Path)
    a = ap.parse_args()
    m = json.loads((a.pkg / "manifest.json").read_text())
    data = json.loads(a.data.read_text())
    grid = grid_path(m, a.cache)
    if not grid.exists():
        print(f"GEOID18 grid not in the fetch cache: {grid} (run `camsim-scene fetch`)", file=sys.stderr)
        return 2
    d1 = check_geoid(m, grid, data["marks"])
    d2 = check_frame(m, grid, data["cors"])
    lat = lattice_error(m, grid, data["marks"])
    print(f"D1 geoid: {d1['n']} marks, max |our N - NGS GEOID18| {d1['max_abs_m'] * 1000:.2f} mm "
          f"(limit {D1_LIMIT_M * 1000:.0f} mm) -> {'PASS' if d1['pass'] else 'FAIL'}")
    print(f"D2 frame: {d2['n']} CORS, max horizontal {d2['max_horiz_m'] * 1000:.2f} mm, max vertical "
          f"{d2['max_vert_m'] * 1000:.2f} mm (limit {D2_LIMIT_M * 1000:.0f} mm) -> {'PASS' if d2['pass'] else 'FAIL'}")
    f = d1["geoid18_fit"]
    print(f"info: GEOID18 fit at the marks: median |h - H - N| {f['median_abs_m'] * 100:.2f} cm, "
          f"max {f['max_abs_m'] * 100:.2f} cm")
    print(f"info: vertical-offset lattice vs exact at {lat['n']} marks in the bbox: "
          + ", ".join(f"{k} {v * 1000:.2f} mm" for k, v in lat.items() if k != "n"))
    if a.json:
        a.json.write_text(json.dumps({"D1": d1, "D2": d2, "lattice": lat, "data_retrieved": data.get("retrieved")},
                                     indent=1) + "\n")
    return 0 if d1["pass"] and d2["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
