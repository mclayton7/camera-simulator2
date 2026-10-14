"""REALISM R5 gates 2 and 3 (docs/superpowers/specs/2026-10-14-r5-detail-polygons-design.md, section 4).

    uv run --project scripts/scene python scripts/scene/tools/detail_check.py PKG --reference REF \
        [--shrink-km 1] [--coast-km 1]

Gate 3 (scope): no terrain tile deeper than the bbox's terrain zoom outside the detail polygons, no imagery tile
deeper than the bbox's imagery zoom outside the detail margin (a tile counts when its parent overlaps the scope,
because siblings are always built), and no scoped footprint outside its scope (verify's `detail` check).
Gate 2 (identity): leaves at the detail zooms whose tile lies inside the detail polygons shrunk by --shrink-km have
the same sha256 as REF's (a full-sim package of the same area, e.g. pendleton-r5veg). Imagery leaves with WorldCover
water (80 or 220 in PKG's land cover) within --coast-km are listed but exempt (colour-matched Sentinel-2 at the
shore depends on the balance fit's samples). Prints a JSON report; exits 1 when a gate fails."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

import numpy as np
import shapely
from PIL import Image

from camsim_scene import detail as D
from camsim_scene.config import KM_PER_DEG
from camsim_scene.layers.landcover import tile_filename, tile_range
from camsim_scene.manifest import Manifest, read_hashes
from camsim_scene.tiling import tile_bounds

TILE_RE = re.compile(r"^(terrain|imagery)/(\d+)/(\d+)/(\d+)\.(terrain|jpg)$")
WATER_CODES = (80, 220)
LC_TILE_DEG, LC_PX = 0.05, 600


def parse_tile(path: str):
    mo = TILE_RE.match(path)
    return None if mo is None else (mo.group(1), int(mo.group(2)), int(mo.group(3)), int(mo.group(4)))


def _box_zoom(regions, layer: str) -> int:
    return next(r for r in regions if r.name == "bbox").max_zoom[layer]


def scope_violations(hashes: dict, regions) -> list[str]:
    scope = {"terrain": D.scope_geometry(regions, D.DETAIL), "imagery": D.scope_geometry(regions, D.MARGIN)}
    out = []
    for path in sorted(hashes):
        t = parse_tile(path)
        if t is None:
            continue
        layer, z, x, y = t
        if z <= _box_zoom(regions, layer):
            continue
        parent = shapely.box(*tile_bounds(z - 1, x // 2, y // 2))
        if scope[layer] is None or not parent.intersects(scope[layer]):
            out.append(path)
    return out


def identity(hashes: dict, ref: dict, layer: str, z: int, inner) -> tuple[int, list[str]]:
    compared, mism = 0, []
    for path, h in sorted(hashes.items()):
        t = parse_tile(path)
        if t is None or t[0] != layer or t[1] != z:
            continue
        if not inner.contains(shapely.box(*tile_bounds(z, t[2], t[3]))):
            continue
        compared += 1
        if ref.get(path) != h:
            mism.append(path)
    return compared, mism


def water_near(lc_dir: Path, bounds) -> bool:
    """True when any land-cover cell in bounds (W S E N) is water (80 or 220); missing tiles are no data."""
    w, s, e, n = bounds
    for i in tile_range(s, n):
        for j in tile_range(w, e):
            p = Path(lc_dir) / tile_filename(i, j)
            if not p.exists():
                continue
            codes = np.asarray(Image.open(p))
            s0, w0 = i * LC_TILE_DEG, j * LC_TILE_DEG
            c0 = max(0, int(np.floor((w - w0) / LC_TILE_DEG * LC_PX)))
            c1 = min(LC_PX, int(np.ceil((e - w0) / LC_TILE_DEG * LC_PX)))
            r0 = max(0, int(np.floor((s0 + LC_TILE_DEG - n) / LC_TILE_DEG * LC_PX)))  # row 0 is the north edge
            r1 = min(LC_PX, int(np.ceil((s0 + LC_TILE_DEG - s) / LC_TILE_DEG * LC_PX)))
            if c1 > c0 and r1 > r0 and np.isin(codes[r0:r1, c0:c1], WATER_CODES).any():
                return True
    return False


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pkg", type=Path)
    ap.add_argument("--reference", type=Path, required=True)
    ap.add_argument("--shrink-km", type=float, default=1.0)
    ap.add_argument("--coast-km", type=float, default=1.0)
    a = ap.parse_args()
    m = Manifest.load(a.pkg / "manifest.json")
    if not m.detail:
        print(f"{a.pkg}: no [detail] in the manifest", file=sys.stderr)
        return 2
    regions = m.region_objs()
    hashes, ref = read_hashes(a.pkg), read_hashes(a.reference)
    rep: dict = {"package": str(a.pkg), "reference": str(a.reference)}
    viol = scope_violations(hashes, regions)
    fp_bad = [
        f"{sid}/{x.id}"
        for sid, scope in D.scopes(m).items()
        for x in m.source(sid).assets
        if x.role == "data"
        and x.metadata.get("footprint")
        and not scope.buffer(1e-6).covers(shapely.from_wkt(x.metadata["footprint"]))
    ]
    rep["scope"] = {"tiles_outside": len(viol), "examples": viol[:20], "footprints_outside": fp_bad}
    inner = D.scope_geometry(regions, D.DETAIL).buffer(-a.shrink_km / KM_PER_DEG)
    pad = a.coast_km / KM_PER_DEG
    rep["identity"] = {}
    ok = not viol and not fp_bad
    for layer in ("terrain", "imagery"):
        z = max(r.max_zoom[layer] for r in regions if r.name.startswith(D.DETAIL + ":"))
        compared, mism = identity(hashes, ref, layer, z, inner)
        coastal = []
        if layer == "imagery":
            for p in list(mism):
                _, zz, x, y = parse_tile(p)
                w, s, e, n = tile_bounds(zz, x, y)
                if water_near(a.pkg / "landcover", (w - pad, s - pad, e + pad, n + pad)):
                    coastal.append(p)
            mism = [p for p in mism if p not in coastal]
        rep["identity"][layer] = {
            "zoom": z,
            "compared": compared,
            "mismatched": len(mism),
            "mismatched_examples": mism[:20],
            "coastal_exempt": len(coastal),
            "coastal_examples": coastal[:20],
        }
        ok = ok and compared > 0 and not mism
    rep["ok"] = ok
    print(json.dumps(rep, indent=2))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
