#!/usr/bin/env python3
"""Pick the REALISM R6 geo-registration marker sites from a scene package (spec section 1.2) and print candidates:

  flat   a paved block (land cover 200/201: no vegetation, no buildings) with a DEM plane fit < 2 deg, relief < 0.3 m
  slope  grass or bare ground (30/60), no tree cells (10) within ~45 m, plane fit 10-20 deg, relief < 1.5 m
  roof   an Overture building >= 6 m tall whose footprint is a near-rectangle (area / min-rotated-rect >= 0.9) with
         both sides >= 25 m; the marker at its centroid

    uv run --project scripts/scene python scripts/georeg/pick_sites.py PKG [--write scripts/georeg/sites_pendleton.json]

The DEM is the package's 3DEP 1/3" source (10 m; the plane fit uses the samples within 25 m), read from the fetch
cache. The runner re-checks each site on the rendered terrain (CIGI HOT normal), so this only has to find good
candidates. Sites stay 0.06 deg inside the bbox so the 5 km views look at package terrain."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[2]
BLOBS = REPO / ".cache" / "scene" / "blobs" / "sha256"
MARGIN_DEG = 0.06
HEADINGS = {"flat": 30.0, "slope": 120.0, "roof": 250.0}


def plane_fit(x: np.ndarray, y: np.ndarray, z: np.ndarray) -> tuple[float, float, float]:
    """Least-squares plane through (x east, y north, z up) metres: slope (deg), aspect (deg from north, the downhill
    direction) and relief (RMS misfit, m)."""
    a = np.column_stack([x, y, np.ones_like(x)])
    coef = np.linalg.lstsq(a, z, rcond=None)[0]
    gx, gy = float(coef[0]), float(coef[1])
    resid = z - a @ coef
    slope = math.degrees(math.atan(math.hypot(gx, gy)))
    aspect = math.degrees(math.atan2(-gx, -gy)) % 360.0
    return slope, aspect, float(np.sqrt(np.mean(resid**2)))


def _blob(manifest: dict, source: str, pred) -> list[tuple[dict, Path]]:
    s = next(s for s in manifest["sources"] if s["id"] == source)
    return [(a, BLOBS / a["sha256"][:2] / a["sha256"]) for a in s["assets"] if pred(a)]


class Dem:
    def __init__(self, manifest: dict, lat: float, lon: float):
        import rasterio

        hits = _blob(manifest, "dep3_13", lambda a: a["metadata"]["bbox"][0] <= lon <= a["metadata"]["bbox"][2]
                     and a["metadata"]["bbox"][1] <= lat <= a["metadata"]["bbox"][3])
        self.ds = rasterio.open(hits[0][1])

    def fit(self, lat: float, lon: float, radius_m: float = 25.0) -> tuple[float, float, float]:
        m_lat, m_lon = 111_320.0, 111_320.0 * math.cos(math.radians(lat))
        r, c = self.ds.index(lon, lat)
        n = int(math.ceil(radius_m / 9.0)) + 1
        from rasterio.windows import Window

        z = self.ds.read(1, window=Window(c - n, r - n, 2 * n + 1, 2 * n + 1)).astype(np.float64)
        rows, cols = np.mgrid[r - n : r + n + 1, c - n : c + n + 1]
        lons, lats = self.ds.xy(rows.ravel(), cols.ravel())
        x = (np.asarray(lons) - lon) * m_lon
        y = (np.asarray(lats) - lat) * m_lat
        keep = np.hypot(x, y) <= radius_m
        return plane_fit(x[keep], y[keep], z.ravel()[keep])


def landcover(pkg: Path, w: float, s: float, e: float, n: float) -> tuple[np.ndarray, float, float, float]:
    """Code mosaic over [w, e] x [s, n] (row 0 north), its NW corner and the pixel size (deg)."""
    from PIL import Image

    idx = json.loads((pkg / "landcover" / "index.json").read_text())
    td, tp = idx["tile_deg"], idx["tile_px"]
    i0, i1 = math.floor(s / td), math.floor(n / td)
    j0, j1 = math.floor(w / td), math.floor(e / td)
    out = np.zeros(((i1 - i0 + 1) * tp, (j1 - j0 + 1) * tp), np.uint8)
    for i in range(i0, i1 + 1):
        for j in range(j0, j1 + 1):
            f = pkg / "landcover" / f"{i * td:+.2f}_{j * td:+.2f}.png"
            if f.exists():
                out[(i1 - i) * tp : (i1 - i + 1) * tp, (j - j0) * tp : (j - j0 + 1) * tp] = np.asarray(Image.open(f))
    return out, (i1 + 1) * td, j0 * td, td / tp


def windows_all(codes: np.ndarray, allowed: set[int], k: int) -> np.ndarray:
    """True where the k x k window centred on the pixel holds only `allowed` codes."""
    ok = np.isin(codes, list(allowed)).astype(np.int32)
    s = np.pad(ok, ((1, 0), (1, 0))).cumsum(0).cumsum(1)
    h = k // 2
    out = np.zeros_like(ok, bool)
    win = s[k:, k:] - s[:-k, k:] - s[k:, :-k] + s[:-k, :-k]
    out[h : h + win.shape[0], h : h + win.shape[1]] = win == k * k
    return out


def roofs(manifest: dict, w: float, s: float, e: float, n: float) -> list[dict]:
    import duckdb
    import shapely

    path = _blob(manifest, "overture_buildings", lambda a: True)[0][1]
    con = duckdb.connect()
    rows = con.execute(
        f"SELECT id, geometry, height FROM read_parquet('{path}') WHERE height >= 6 AND geometry IS NOT NULL"
    ).fetchall()
    out = []
    for sid, wkb, h in rows:
        g = shapely.from_wkb(bytes(wkb))
        c = g.centroid
        if not (w < c.x < e and s < c.y < n):
            continue
        m_lon = 111_320.0 * math.cos(math.radians(c.y))
        gm = shapely.affinity.scale(shapely.affinity.translate(g, -c.x, -c.y), m_lon, 111_320.0, origin=(0, 0))
        rect = gm.minimum_rotated_rectangle
        xs, ys = rect.exterior.coords.xy
        sides = sorted(math.hypot(xs[i + 1] - xs[i], ys[i + 1] - ys[i]) for i in range(2))
        fill = gm.area / rect.area
        if sides[0] >= 25 and fill >= 0.9 and gm.contains(shapely.Point(0, 0).buffer(10)):
            out.append({"lat": c.y, "lon": c.x, "id": sid, "height_m": h, "sides_m": [round(v, 1) for v in sides],
                        "fill": round(fill, 3), "area_m2": round(gm.area)})
    return sorted(out, key=lambda r: -r["area_m2"])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pkg", type=Path)
    ap.add_argument("--write", type=Path)
    a = ap.parse_args()
    manifest = json.loads((a.pkg / "manifest.json").read_text())
    w, s, e, n = manifest["bbox"]
    w, s, e, n = w + MARGIN_DEG, s + MARGIN_DEG, e - MARGIN_DEG, n - MARGIN_DEG
    codes, lat0, lon0, px = landcover(a.pkg, w, s, e, n)
    dem = Dem(manifest, (s + n) / 2, (w + e) / 2)

    def at(r: int, c: int) -> tuple[float, float]:
        return lat0 - (r + 0.5) * px, lon0 + (c + 0.5) * px

    def inside(lat: float, lon: float) -> bool:
        return s <= lat <= n and w <= lon <= e

    picks: dict[str, list[dict]] = {"flat": [], "slope": [], "roof": []}
    paved = windows_all(codes, {200, 201}, 9)
    big = windows_all(codes, {200, 201}, 15)
    for r, c in zip(*np.nonzero(big)):
        lat, lon = at(r, c)
        if not inside(lat, lon):
            continue
        sl, asp, rel = dem.fit(lat, lon)
        if sl < 2.0 and rel < 0.3:
            picks["flat"].append({"lat": lat, "lon": lon, "slope_deg": round(sl, 2), "relief_m": round(rel, 3),
                                  "why": "15 x 15 land-cover cells (~135 m) all paved (200/201)"})
    if not picks["flat"]:
        for r, c in zip(*np.nonzero(paved)):
            lat, lon = at(r, c)
            if inside(lat, lon):
                sl, asp, rel = dem.fit(lat, lon)
                if sl < 2.0 and rel < 0.3:
                    picks["flat"].append({"lat": lat, "lon": lon, "slope_deg": round(sl, 2),
                                          "relief_m": round(rel, 3), "why": "9 x 9 cells (~80 m) all paved"})
    open_ground = windows_all(codes, {30, 60}, 7) & windows_all(codes, set(range(256)) - {10}, 11)
    ys, xs = np.nonzero(open_ground)
    for r, c in list(zip(ys, xs))[:: max(1, len(ys) // 4000)]:
        lat, lon = at(r, c)
        if not inside(lat, lon):
            continue
        sl, asp, rel = dem.fit(lat, lon)
        if 10.0 <= sl <= 20.0 and rel < 1.5:
            picks["slope"].append({"lat": lat, "lon": lon, "slope_deg": round(sl, 2), "aspect_deg": round(asp, 1),
                                   "relief_m": round(rel, 3),
                                   "why": "7 x 7 cells grass/bare (30/60), no tree cell (10) in 11 x 11"})
    picks["slope"].sort(key=lambda p: (p["relief_m"] + abs(p["slope_deg"] - 15.0) / 10.0))
    picks["roof"] = roofs(manifest, w, s, e, n)
    for kind, cands in picks.items():
        print(f"{kind}: {len(cands)} candidates")
        for p in cands[:5]:
            print("  ", json.dumps(p))
    if a.write:
        sites = []
        for kind in ("flat", "slope", "roof"):
            if not picks[kind]:
                raise SystemExit(f"no {kind} candidate")
            p = dict(picks[kind][0])
            sites.append({"name": kind, "kind": kind, "lat": round(p.pop("lat"), 7), "lon": round(p.pop("lon"), 7),
                          "heading_deg": HEADINGS[kind], **p})
        a.write.write_text(json.dumps({"package": a.pkg.name, "sites": sites}, indent=1) + "\n")
        print(f"wrote {a.write}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
