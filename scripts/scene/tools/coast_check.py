"""R1 coast gate (spec docs/superpowers/specs/2026-10-10-r1-coast-and-gates-design.md, Part C gate 3): package
terrain along transects across the Camp Pendleton coast vs the sea CamSim draws (EGM96 + sea_level.json).

    uv run --project scripts/scene python scripts/scene/tools/coast_check.py PKG [--before OLDPKG] [--out DIR]

Gates: (a) over WorldCover water >= 100 m from land, terrain <= sea - 0.5 m for >= 99 % of samples; (b) no step
> 3 m between adjacent 30 m samples within 1 km of the old 3DEP coverage edge; (c) median distance between the
terrain / local-MSL crossing and the WorldCover water edge <= 30 m; (d) sea_level.json offset_m equals an
independent pyproj computation within 1 cm. --before prints the same numbers for an older package."""

from __future__ import annotations

import argparse
import functools
import json
import sys
from pathlib import Path

import numpy as np

from camsim_scene import qmesh, sea_level, tiling
from camsim_scene.cache import Cache
from camsim_scene.context import class_rasters, reference_footprints
from camsim_scene.manifest import Manifest
from camsim_scene.pipeline import make_context
from camsim_scene.water import WATER, class_codes

REPO = Path(__file__).resolve().parents[3]
DAC = REPO / "unreal_project/CamSimTest/Content/NonUFS/Geoid/WW15MGH.DAC"
M_PER_DEG = 111_320.0
STEP_M = 10.0
LENGTH_M = 6000.0
BEARING = 235.0  # roughly normal to the coast
INLAND_M = 300.0
MIN_OFFSHORE_M = 100.0  # water further than this from land is "open sea"
SEA_MARGIN_M = 0.5  # (a) terrain at least this far below the sea
BAD_FRACTION = 0.01
STEP_MAX_M = 3.0
EDGE_REACH_M = 1000.0
WATERLINE_MEDIAN_M = 30.0
OFFSET_TOL_M = 0.01
COAST_LATS = tuple(np.linspace(33.20, 33.40, 12))
RING_LATS = (33.47, 33.54, 33.12, 33.04)  # Dana Point, Laguna, Carlsbad, Encinitas (latitudes only)
OFFSHORE_LON = -117.95  # marches start here, well out to sea
LEVEL_PROBE_ZOOM = 17


# ---------------------------------------------------------------- EGM96 (as CamSim's Geospatial/Geoid.cpp)


@functools.lru_cache(maxsize=1)
def _geoid() -> np.ndarray:
    """WW15MGH.DAC: 721 x 1440 big-endian int16 centimetres, rows 90N -> 90S, columns 0E -> 359.75E."""
    return np.frombuffer(DAC.read_bytes(), ">i2").reshape(721, 1440).astype(np.float64) / 100.0


def egm96(lat: float, lon: float) -> float:
    """Geoid undulation N (m), bilinear on the 15' grid."""
    g = _geoid()
    r = (90.0 - lat) * 4.0
    c = (lon % 360.0) * 4.0
    r0, c0 = int(r), int(c)
    fr, fc = r - r0, c - c0
    c1 = (c0 + 1) % 1440
    return float(
        g[r0, c0] * (1 - fr) * (1 - fc)
        + g[r0, c1] * (1 - fr) * fc
        + g[r0 + 1, c0] * fr * (1 - fc)
        + g[r0 + 1, c1] * fr * fc
    )


# ---------------------------------------------------------------- package terrain


@functools.lru_cache(maxsize=256)
def _tile(pkg: str, z: int, x: int, y: int):
    p = Path(pkg) / f"terrain/{z}/{x}/{y}.terrain"
    if not p.exists():
        return None
    m = qmesh.decode(p.read_bytes())
    lo, la = m.lonlat(tiling.tile_bounds(z, x, y))
    return lo, la, m.heights(), m.triangles


def terrain_height(pkg: Path, lon: float, lat: float) -> tuple[int, float]:
    """(zoom of the deepest tile there, ellipsoid height) at a point: barycentric in the containing triangle.
    KeyError when no tile covers it."""
    for z in range(LEVEL_PROBE_ZOOM, -1, -1):
        s = tiling.tile_size_deg(z)
        t = _tile(str(pkg), z, int((lon + 180) // s), int((lat + 90) // s))
        if t is None:
            continue
        lo, la, h, tris = t
        a, b, c = (np.stack([lo[tris[:, i]], la[tris[:, i]]], 1) for i in range(3))
        v0, v1, v2 = b - a, c - a, np.array([lon, lat]) - a
        d00, d01, d11 = (v0 * v0).sum(1), (v0 * v1).sum(1), (v1 * v1).sum(1)
        d20, d21 = (v2 * v0).sum(1), (v2 * v1).sum(1)
        den = d00 * d11 - d01 * d01
        with np.errstate(divide="ignore", invalid="ignore"):
            v = (d11 * d20 - d01 * d21) / den
            w = (d00 * d21 - d01 * d20) / den
        u = 1 - v - w
        hit = np.flatnonzero((u >= -1e-9) & (v >= -1e-9) & (w >= -1e-9))
        if hit.size == 0:
            continue
        i, tri = hit[0], tris[hit[0]]
        return z, float(u[i] * h[tri[0]] + v[i] * h[tri[1]] + w[i] * h[tri[2]])
    raise KeyError((lon, lat))


# ---------------------------------------------------------------- transects


def transect(lat0: float, lon0: float, bearing_deg: float, step_m: float, length_m: float):
    """(lon[], lat[]) of points every step_m from the start along a compass bearing, length_m long (flat earth)."""
    d = np.arange(int(round(length_m / step_m)) + 1) * step_m
    b = np.radians(bearing_deg)
    lat = lat0 + d * np.cos(b) / M_PER_DEG
    lon = lon0 + d * np.sin(b) / (M_PER_DEG * np.cos(np.radians(lat0)))
    return lon, lat


def coast_starts(classes) -> tuple[dict[str, dict], list[str]]:
    """One start per latitude: march east at 10 m from well offshore along the latitude to the first WorldCover land
    sample (not water, not no-data), then step INLAND_M back inland along the transect axis. Returns ({name: {lat, lon,
    start_class}}, names of latitudes that found no land)."""
    out, dropped = {}, []
    lats = [(f"coast_{i:02d}", la) for i, la in enumerate(COAST_LATS)]
    lats += [(f"ring_{i}", la) for i, la in enumerate(RING_LATS)]
    for name, lat0 in lats:
        lon, lat = transect(lat0, OFFSHORE_LON, 90.0, STEP_M, 100_000.0)
        codes = class_codes(classes, lon, lat)
        land = np.flatnonzero((codes != WATER) & (codes != 0))  # 0: outside the package's WorldCover, not land
        if not land.size:
            dropped.append(name)
            continue
        lo, la = transect(float(lat[land[0]]), float(lon[land[0]]), (BEARING + 180.0) % 360.0, STEP_M, INLAND_M)
        code = class_codes(classes, lo[-1:], la[-1:])
        out[name] = {"lat": float(la[-1]), "lon": float(lo[-1]), "start_class": int(code[0])}
    return out, dropped


# ---------------------------------------------------------------- gates (pure)


def land_distance(wet: np.ndarray, step_m: float) -> np.ndarray:
    """Distance (m) along the transect from each sample to the nearest non-water sample (0 on land; inf when the
    whole transect is water)."""
    idx = np.arange(len(wet))
    land = np.flatnonzero(~wet)
    if land.size == 0:
        return np.full(len(wet), np.inf)
    k = np.clip(np.searchsorted(land, idx), 1, land.size) if land.size > 1 else np.zeros(len(idx), int)
    if land.size > 1:
        nearest = np.minimum(np.abs(idx - land[k - 1]), np.abs(idx - land[np.minimum(k, land.size - 1)]))
    else:
        nearest = np.abs(idx - land[0])
    return nearest * step_m


def gate_offshore(rel: np.ndarray, dist_land_m: np.ndarray) -> dict:
    """(a) rel = terrain - sea. Over water >= 100 m from land, >= 99 % of samples at least 0.5 m below the sea."""
    sel = dist_land_m >= MIN_OFFSHORE_M
    n = int(sel.sum())
    if n == 0:
        return {"pass": False, "n": 0}
    r = rel[sel]
    frac = float(np.mean(r > -SEA_MARGIN_M))
    return {
        "pass": frac <= BAD_FRACTION,
        "n": n,
        "frac_bad": frac,
        "min": float(r.min()),
        "median": float(np.median(r)),
        "max": float(r.max()),
    }


def gate_steps(h: np.ndarray, dist_edge_m: np.ndarray) -> dict:
    """(b) resampled to every 3rd sample (30 m), no step > 3 m between neighbours within 1 km of the old edge."""
    h, e = h[::3], dist_edge_m[::3]
    d = np.abs(np.diff(h))
    near = (e[:-1] <= EDGE_REACH_M) & (e[1:] <= EDGE_REACH_M)
    mx = float(d[near].max()) if near.any() else 0.0
    return {"pass": mx <= STEP_MAX_M, "n": int(near.sum()), "max": mx}


def waterline(rel: np.ndarray, wet: np.ndarray, step_m: float) -> dict | None:
    """(c) for one transect. The edge is the start of the water run connected to open sea (the one that reaches
    100 m from land); the crossing is the terrain-below-MSL crossing nearest it, either side. None when the
    transect has no open sea or no crossing."""
    open_sea = np.flatnonzero(wet & (land_distance(wet, step_m) >= MIN_OFFSHORE_M))
    cross = np.flatnonzero((rel[1:] < 0) & (rel[:-1] >= 0)) + 1
    if not open_sea.size or not cross.size:
        return None
    edge = int(open_sea[0])
    while edge > 0 and wet[edge - 1]:
        edge -= 1
    k = int(cross[np.argmin(np.abs(cross - edge))])
    return {"edge_index": edge, "crossing_index": k, "offset_m": abs(k - edge) * step_m}


def waterline_offsets(rels: list, wets: list, step_m: float) -> list[float]:
    """Waterline offsets in metres over transects (those without an edge or crossing are skipped)."""
    found = (waterline(r, w, step_m) for r, w in zip(rels, wets))
    return [f["offset_m"] for f in found if f]


def gate_waterline(offsets: list[float]) -> dict:
    if not offsets:
        return {"pass": False, "n": 0}
    med = float(np.median(offsets))
    return {"pass": med <= WATERLINE_MEDIAN_M, "n": len(offsets), "median": med, "max": float(max(offsets))}


# ---------------------------------------------------------------- evaluation


def edge_distance(m: Manifest, lon: np.ndarray, lat: np.ndarray) -> np.ndarray:
    """Metres from each point to the boundary of the union of the dep3_13 footprints (inf if the package has none)."""
    import shapely
    from shapely import affinity

    fps = reference_footprints(m, "dep3_13") if any(s.id == "dep3_13" for s in m.sources) else []
    if not fps:
        return np.full(len(lon), np.inf)
    k = np.cos(np.radians(float(np.mean(lat))))
    edge = affinity.scale(shapely.union_all(fps), k, 1.0, origin=(0, 0)).boundary
    return shapely.distance(shapely.points(lon * k, lat), edge) * M_PER_DEG


def offset_of(pkg: Path) -> float | None:
    p = pkg / sea_level.FILE
    return float(json.loads(p.read_text())["offset_m"]) if p.exists() else None


def sample(pkg: Path, lon: np.ndarray, lat: np.ndarray) -> np.ndarray:
    h = np.full(len(lon), np.nan)
    for i in range(len(lon)):
        try:
            h[i] = terrain_height(pkg, float(lon[i]), float(lat[i]))[1]
        except KeyError:
            pass
    return h


def evaluate(pkg: Path, classes, starts: dict) -> dict:
    """Gates a-c for one package over the given transect starts."""
    m = Manifest.load(pkg / "manifest.json")
    off = offset_of(pkg) or 0.0
    rels, wets, rows = [], [], []
    all_rel, all_dist, steps = [], [], []
    for name, st0 in starts.items():
        lon, lat = transect(st0["lat"], st0["lon"], BEARING, STEP_M, LENGTH_M)
        h = sample(pkg, lon, lat)
        ok = np.isfinite(h)
        if not ok.any():
            rows.append({"name": name, **st0, "covered": 0})
            continue
        sea = np.array([egm96(a, b) for a, b in zip(lat, lon)]) + off
        rel = h - sea
        wet = class_codes(classes, lon, lat) == WATER
        dist = land_distance(wet, STEP_M)
        dist[~ok] = 0.0  # no terrain: not counted
        edge = edge_distance(m, lon, lat)
        st = gate_steps(np.where(ok, h, 0.0), np.where(ok, edge, np.inf))
        rels.append(np.where(ok, rel, np.nan))
        wets.append(wet)
        all_rel.append(np.where(ok, rel, 0.0))
        all_dist.append(dist)
        steps.append(st)
        sel = dist >= MIN_OFFSHORE_M
        row = {"name": name, **st0, "covered": int(ok.sum()), "max_step": st["max"]}
        if sel.any():
            row.update(off_min=float(rel[sel].min()), off_median=float(np.median(rel[sel])), off_max=float(rel[sel].max()))
        wl = waterline(rels[-1], wet, STEP_M)
        row.update(wl or {"edge_index": None, "crossing_index": None, "offset_m": None})
        rows.append(row)
    a = gate_offshore(np.concatenate(all_rel), np.concatenate(all_dist)) if all_rel else {"pass": False, "n": 0}
    b = {"pass": all(s["pass"] for s in steps), "n": sum(s["n"] for s in steps), "max": max((s["max"] for s in steps), default=0.0)}
    c = gate_waterline(waterline_offsets(rels, wets, STEP_M))
    return {"offset_m": off, "transects": rows, "a": a, "b": b, "c": c}


def navd88_ellipsoid_height(lon: float, lat: float) -> float:
    """ITRF2014 ellipsoid height (m, epoch 2010.0) of NAVD88 zero by pyproj. RuntimeError unless PROJ can apply
    the GEOID18 grid (otherwise it silently falls back to a null vertical shift)."""
    import pyproj

    pyproj.network.set_network_enabled(True)  # a tool, not a build: PROJ may fetch GEOID18
    group = pyproj.transformer.TransformerGroup("EPSG:6318+5703", "EPSG:7912", always_xy=True)
    if not group.best_available:
        raise RuntimeError("GEOID18 unavailable: PROJ cannot apply the NAVD88 grid (network or PROJ_DATA)")
    return float(group.transformers[0].transform(lon, lat, 0.0, 2010.0)[2])


def gate_offset(pkg: Path) -> dict:
    """(d) sea_level.json offset_m vs an independent pyproj computation at the manifest's station."""
    m = Manifest.load(pkg / "manifest.json")
    have = offset_of(pkg)
    if have is None:
        return {"pass": False, "reason": "no sea_level.json"}
    if not m.sea_level:
        return {"pass": False, "reason": "sea_level.json without a manifest sea_level section"}
    s = m.sea_level
    try:
        h0 = navd88_ellipsoid_height(s["lon"], s["lat"])
    except RuntimeError as e:
        return {"pass": False, "reason": str(e)}
    want = float(h0) + s["msl_above_navd88_m"] - egm96(s["lat"], s["lon"])
    return {"pass": abs(want - have) <= OFFSET_TOL_M, "file_m": have, "independent_m": want, "diff_m": have - want}


# ---------------------------------------------------------------- CLI


def _fmt(g: dict) -> str:
    return " ".join(f"{k}={v:.3f}" if isinstance(v, float) else f"{k}={v}" for k, v in g.items() if k != "pass")


def print_report(title: str, r: dict) -> None:
    print(f"\n== {title}  (sea offset {r['offset_m']:+.3f} m)")
    print(f"{'transect':<10}{'lat':>9}{'lon':>11}{'cls':>4}{'cover':>6}{'off min':>9}{'med':>8}{'max':>8}{'step':>6}"
          f"{'edge':>6}{'xing':>6}{'dev m':>7}")
    for t in r["transects"]:
        head = f"{t['name']:<10}{t['lat']:9.4f}{t['lon']:11.4f}{t['start_class']:4d}"
        if not t.get("covered"):
            print(head + "  no terrain")
            continue
        o = [f"{t[k]:+8.2f}" if k in t else f"{'-':>8}" for k in ("off_min", "off_median", "off_max")]
        e, c, d = (("-" if t[k] is None else f"{t[k]:.0f}") for k in ("edge_index", "crossing_index", "offset_m"))
        print(f"{head}{t['covered']:>6}{o[0]:>9}{o[1]}{o[2]}{t['max_step']:6.1f}{e:>6}{c:>6}{d:>7}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pkg", type=Path)
    ap.add_argument("--before", type=Path, help="an older package: print gates a-c for it alongside")
    ap.add_argument("--out", type=Path, default=REPO / ".cache/coast_check")
    ap.add_argument("--cache", type=Path, default=REPO / ".cache/scene")
    a = ap.parse_args()
    cache = Cache(a.cache)
    m = Manifest.load(a.pkg / "manifest.json")
    classes, _ = class_rasters(m, make_context(a.pkg, m, cache).asset_paths)
    starts, dropped = coast_starts(classes)
    if dropped:
        print(f"no land found for: {', '.join(dropped)} (dropped)")
    report = {"package": str(a.pkg), "dropped_starts": dropped, **evaluate(a.pkg, classes, starts)}
    report["d"] = gate_offset(a.pkg)
    print_report(str(a.pkg), report)
    if a.before:
        mb = Manifest.load(a.before / "manifest.json")
        cb, _ = class_rasters(mb, make_context(a.before, mb, cache).asset_paths)
        report["before"] = {"package": str(a.before), **evaluate(a.before, cb, starts)}
        print_report(f"{a.before} (before)", report["before"])
    names = {"a": "offshore terrain below sea", "b": "no step at the old 3DEP edge", "c": "waterline vs WorldCover", "d": "sea_level.json offset"}
    print()
    for k, label in names.items():
        g = report[k]
        extra = g.get("reason") or _fmt(g)
        print(f"({k}) {'PASS' if g['pass'] else 'FAIL'}  {label}: {extra}")
        if a.before and k in "abc":
            print(f"      before: {'PASS' if report['before'][k]['pass'] else 'FAIL'}  {_fmt(report['before'][k])}")
    a.out.mkdir(parents=True, exist_ok=True)
    (a.out / "coast_check.json").write_text(json.dumps(report, indent=2, default=float))
    return 0 if all(report[k]["pass"] for k in names) else 1


if __name__ == "__main__":
    sys.exit(main())
