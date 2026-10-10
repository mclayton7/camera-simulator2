"""REALISM R2 gate (r) pose selection on a scene package: finds the dark-asphalt road piece the gate is posed on.

    uv run --project scripts/scene python scripts/scene/tools/select_road_pose.py PKG OUT.json MIN_W_M MIN_LEN_M MIN_VEG

The pose actually used, I-5 NB in San Clemente (OSM way 487165554), came from `... pendleton-r2 OUT.json 8 120 0.6`
(`ROAD_POSE` in `scripts/thermal_check.py`). The rule, fixed before any gate run:
 - OSM way, highway motorway..tertiary (links excluded), code_for == 200 (asphalt or no surface), LineString;
 - width_m(f) >= MIN_W_M (the carriageway mask, width - 4 m, must be wide enough at the gate's 0.083 m GSD);
 - a straight piece: consecutive vertices of the way whose segment bearings all stay within 1 deg of the piece's
   end-to-end bearing, >= MIN_LEN_M long; pose at its midpoint;
 - no other road/rail feature (any code, incl. bridges/tunnels/links) crosses the way within 60 m of the pose, and no
   bridge/tunnel feature lies within 60 m;
 - land cover over along +-18 m: road strip |d| <= w/2 - 2 m >= 90 % code 200; verge strip d in [w/2+8, w/2+28] on the
   chosen side: no 200/201/210/220 and >= MIN_VEG in {10,20,30,40,60};
 - imagery (z17 NAIP, sRGB luma 0-255, 1 m samples over the same strips): verge mean - road mean >= 15 DN.
Ranking among qualifiers: road class (motorway, trunk, primary, secondary, tertiary), then verge - road luma.

Relaxation history (the arguments, both changes made before any gate run): the first rule was MIN_W_M 10, MIN_LEN_M 200,
MIN_VEG 0.8; it was relaxed to 8 m / 120 m / 60 % (the arguments above). The luma threshold and
the other clauses never changed. OUT.json holds every land-cover candidate ("all") and the qualifiers ("qualifying").
"""

import math
import sys
import json
from pathlib import Path
import numpy as np
import shapely
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from road_registration import TmsSampler
from camsim_scene.cache import Cache, default_cache_root
from camsim_scene.manifest import Manifest
from camsim_scene.pipeline import _load_features, make_context
from camsim_scene.features import code_for, width_m

pkg = Path(sys.argv[1])
MIN_W, MIN_L, MIN_VEG = float(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5])
m = Manifest.load(pkg / "manifest.json")
ctx = make_context(pkg, m, Cache(default_cache_root()))
feats = _load_features(m, ctx)
CLS = ["motorway", "trunk", "primary", "secondary", "tertiary"]
W, S, E, N = m.bbox
M = 111320.0
lcdir = pkg / "landcover"
_lc = {}


def lc(lat, lon):
    li, lo = math.floor(lat / 0.05), math.floor(lon / 0.05)
    k = (li, lo)
    if k not in _lc:
        p = lcdir / f"{'+' if li >= 0 else '-'}{abs(li*0.05):05.2f}_{lo*0.05:+.2f}.png"
        _lc[k] = np.array(Image.open(p)) if p.exists() else None
    a = _lc[k]
    if a is None:
        return 0
    col = min(int((lon - lo * 0.05) / 0.05 * 600), 599)
    row = min(int(((li + 1) * 0.05 - lat) / 0.05 * 600), 599)
    return int(a[row, col])


samp = TmsSampler(pkg / "imagery", 17)
allroad = [f for f in feats if f.kind in ("road", "rail")]
geoms = [f.geom for f in allroad]
tree = shapely.STRtree(geoms)
cands = []
for f in feats:
    if f.kind != "road" or f.geom.geom_type != "LineString":
        continue
    hw = f.tags.get("highway", "")
    if hw not in CLS or code_for(f) != 200:
        continue
    w = width_m(f)
    if w < MIN_W:
        continue
    c = list(f.geom.coords)
    kx0 = M * math.cos(math.radians(c[0][1]))
    xy = [((lo - c[0][0]) * kx0, (la - c[0][1]) * M) for lo, la in c]
    pieces = []
    i = 0
    while i < len(xy) - 1:
        j = i + 1
        best_j = None
        while j < len(xy):
            ex, ey = xy[j][0] - xy[i][0], xy[j][1] - xy[i][1]
            b = math.atan2(ex, ey)
            ok = all(
                abs(
                    (math.atan2(xy[k + 1][0] - xy[k][0], xy[k + 1][1] - xy[k][1]) - b + math.pi) % (2 * math.pi)
                    - math.pi
                )
                <= math.radians(1.0)
                for k in range(i, j)
                if math.hypot(xy[k + 1][0] - xy[k][0], xy[k + 1][1] - xy[k][1]) > 2.0
            )
            if not ok:
                break
            best_j = j
            j += 1
        L = math.hypot(xy[best_j][0] - xy[i][0], xy[best_j][1] - xy[i][1])
        if L >= MIN_L:
            pieces.append((i, best_j, L))
        i = best_j if best_j > i else i + 1
    for i0, i1, L in pieces:
        lo0, la0 = c[i0]
        lo1, la1 = c[i1]
        kx = M * math.cos(math.radians(la0))
        de, dn = (lo1 - lo0) * kx, (la1 - la0) * M
        lon, lat = (lo0 + lo1) / 2, (la0 + la1) / 2
        if not (W < lon < E and S < lat < N):
            continue
        pose_pt = shapely.Point(lon, lat)
        seg = shapely.LineString(
            [(lon - de / L * 60 / kx, lat - dn / L * 60 / M), (lon + de / L * 60 / kx, lat + dn / L * 60 / M)]
        )
        near = tree.query(pose_pt.buffer(80 / M))
        bad = False
        for k in near:
            g = allroad[k]
            if g is f:
                continue
            if g.geom.intersects(seg.buffer(1.0 / M)) and not g.geom.equals(f.geom):
                bad = True
                break
            if (
                code_for(g) is None
                and pose_pt.distance(g.geom) * M < 60
                and g.kind in ("road", "rail")
                and (g.tags.get("bridge", "no") != "no" or g.tags.get("tunnel", "no") != "no")
            ):
                bad = True
                break
        if bad:
            continue
        hdg = math.degrees(math.atan2(de, dn)) % 360
        ue, un = math.sin(math.radians(hdg)), math.cos(math.radians(hdg))  # along
        re_, rn = un, -ue  # right of travel

        def pts(d0, d1, step=1.0):
            out = []
            for a in np.arange(-18, 18.01, 2.0):
                for d in np.arange(d0, d1 + 1e-6, step):
                    e_, n_ = a * ue + d * re_, a * un + d * rn
                    out.append((lat + n_ / M, lon + e_ / kx))
            return np.array(out)

        hw_ = w / 2
        road = pts(-(hw_ - 2), hw_ - 2)
        rc = np.array([lc(a, b) for a, b in road])
        if (rc == 200).mean() < 0.9:
            continue
        rl = samp.luma(road[:, 1], road[:, 0])
        best = None
        for side in (+1, -1):
            v = pts(hw_ + 8, hw_ + 28) if side > 0 else pts(-(hw_ + 28), -(hw_ + 8))
            vc = np.array([lc(a, b) for a, b in v])
            if np.isin(vc, [200, 201, 210, 220]).any():
                continue
            if np.isin(vc, [10, 20, 30, 40, 60]).mean() < MIN_VEG:
                continue
            vl = samp.luma(v[:, 1], v[:, 0])
            if np.isnan(vl).any() or np.isnan(rl).any():
                continue
            diff = float(vl.mean() - rl.mean())
            rec = dict(
                osm=f.source_id,
                hw=hw,
                ref=f.tags.get("ref"),
                name=f.tags.get("name"),
                surface=f.tags.get("surface"),
                lanes=f.tags.get("lanes"),
                oneway=f.tags.get("oneway"),
                width_m=w,
                seg_m=round(L),
                lat=round(lat, 6),
                lon=round(lon, 6),
                heading=round(hdg, 2),
                side=side,
                road_luma=round(float(rl.mean()), 1),
                verge_luma=round(float(vl.mean()), 1),
                diff=round(diff, 1),
                road200=round(float((rc == 200).mean()), 3),
                verge_codes={int(k): int(n) for k, n in zip(*np.unique(vc, return_counts=True))},
            )
            if best is None or rec["diff"] > best["diff"]:
                best = rec
        if best:
            cands.append(best)
samp._tiles.clear()
print("segments examined -> qualifying land-cover candidates:", len(cands))
q = [c for c in cands if c["diff"] >= 15]
q.sort(key=lambda c: (CLS.index(c["hw"]), -c["diff"]))
for c in sorted(cands, key=lambda c: (CLS.index(c["hw"]), -c["diff"]))[:40]:
    print(("Q " if c["diff"] >= 15 else "  ") + json.dumps(c))
json.dump({"all": cands, "qualifying": q}, open(sys.argv[2], "w"), indent=1)
