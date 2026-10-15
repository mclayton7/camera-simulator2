"""Fetch the NGS reference data for datum_check.py (REALISM R6 datum gates) into data/ngs_pendleton.json.

    uv run --project scripts/scene python scripts/scene/tools/fetch_ngs.py [--bbox W S E N] [--out PATH]

- Survey marks: the NGS datasheet API (`/api/nde/bounds`, 0.25 deg tiles, split further where a tile returns the
  API's 500-record cap). Kept: a NAD83(2011) ellipsoid height, an ADJUSTED first- or second-order NAVD88 height and the
  published GEOID18 height. Those test our GEOID18 grid step (D1) and give GEOID18's own fit (informational).
- CORS: every station whose `corsId` the marks carry, from its NGS coordinate file (`coord_14/<id>_14.coord.txt`):
  the antenna reference point's ITRF2014 and NAD83(2011) positions at epoch 2010.0 (D2: our NAD83(2011) -> ITRF2014
  Helmert). Public domain (US Government work)."""

from __future__ import annotations

import argparse
import json
import re
import time
import urllib.request
from pathlib import Path

NDE_URL = "https://geodesy.noaa.gov/api/nde/bounds?minlat={s}&maxlat={n}&minlon={w}&maxlon={e}"
CORS_URL = "https://geodesy.noaa.gov/corsdata/coord/coord_14/{id}_14.coord.txt"
CAP = 500
BBOX = (-118.0, 32.8, -116.9, 33.9)  # ~1 deg around Camp Pendleton
OUT = Path(__file__).resolve().parent / "data" / "ngs_pendleton.json"


def _get(url: str) -> str:
    with urllib.request.urlopen(url, timeout=60) as r:
        return r.read().decode()


def nde(w: float, s: float, e: float, n: float, step: float = 0.25) -> list[dict]:
    out: dict[str, dict] = {}

    def tile(w, s, e, n):
        recs = json.loads(_get(NDE_URL.format(w=w, s=s, e=e, n=n)))
        if len(recs) >= CAP:
            mx, my = (w + e) / 2, (s + n) / 2
            for box in ((w, s, mx, my), (mx, s, e, my), (w, my, mx, n), (mx, my, e, n)):
                tile(*box)
            return
        for r in recs:
            out[r["pid"]] = r

    lat = s
    while lat < n - 1e-9:
        lon = w
        while lon < e - 1e-9:
            tile(round(lon, 4), round(lat, 4), round(min(lon + step, e), 4), round(min(lat + step, n), 4))
            lon += step
        lat += step
    return list(out.values())


def _num(v: str) -> float | None:
    v = (v or "").strip()
    return float(v) if v else None


def usable_marks(records: list[dict]) -> list[dict]:
    out = []
    for r in records:
        eh, oh, n = _num(r.get("ellipHeight")), _num(r.get("orthoHt")), _num(r.get("geoidHt"))
        if (eh is None or oh is None or n is None or "2011" not in r.get("posDatum", "")
                or not r.get("vertSource", "").strip().upper().startswith("ADJ")
                or r.get("vertOrder", "").strip() not in ("1", "2") or r.get("geoidModel", "").strip() != "GEOID18"):
            continue
        out.append({"pid": r["pid"], "lat": float(r["lat"]), "lon": float(r["lon"]), "ellip_h": eh, "navd88_h": oh,
                    "geoid18_n": n, "vert_order": r["vertOrder"].strip()})
    return sorted(out, key=lambda m: m["pid"])


def cors_ids(records: list[dict]) -> list[str]:
    return sorted({r["corsId"].strip() for r in records if r.get("corsId", "").strip()})


def _dms(d: str, m: str, s: str, hemi: str) -> float:
    v = int(d) + int(m) / 60 + float(s) / 3600
    return -v if hemi in "SW" else v


_POS = re.compile(
    r"latitude\s*=\s*(\d+) (\d+) ([\d.]+) ([NS]).*?longitude\s*=\s*(\d+) (\d+) ([\d.]+) ([EW])"
    r".*?ellipsoid height\s*=\s*(-?[\d.]+)", re.S)


def parse_cors(sid: str, text: str) -> dict:
    """The antenna reference point's ITRF2014 and NAD83(2011) positions (the first block of each in the file)."""

    def block(title: str) -> dict:
        i = text.index(title)
        m = _POS.search(text, i)
        return {"lat": _dms(*m.group(1, 2, 3, 4)), "lon": _dms(*m.group(5, 6, 7, 8)), "h": float(m.group(9))}

    epoch = float(re.search(r"ITRF2014 POSITION \(EPOCH ([\d.]+)\)", text).group(1))
    return {"id": sid, "epoch": epoch, "itrf2014": block("ITRF2014 POSITION"),
            "nad83_2011": block("NAD_83 (2011) POSITION")}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bbox", type=float, nargs=4, default=BBOX, metavar=("W", "S", "E", "N"))
    ap.add_argument("--out", type=Path, default=OUT)
    a = ap.parse_args()
    records = nde(*a.bbox)
    marks = usable_marks(records)
    cors = []
    for sid in cors_ids(records):
        try:
            cors.append(parse_cors(sid, _get(CORS_URL.format(id=sid.lower()))))
        except (OSError, ValueError, AttributeError) as e:
            print(f"CORS {sid}: skipped ({e})")
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps({
        "retrieved": time.strftime("%Y-%m-%d"), "bbox": list(a.bbox), "nde_url": NDE_URL, "cors_url": CORS_URL,
        "licence": "Public domain (US Government work, NOAA National Geodetic Survey)",
        "marks": marks, "cors": cors}, indent=1) + "\n")
    print(f"{len(records)} records, {len(marks)} usable marks, {len(cors)} CORS -> {a.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
