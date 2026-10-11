"""Acceptance gate 2 (REALISM R3 buildings): IoU of hand-traced NAIP outlines (GeoJSON, lon/lat) against the Overture
snapshot's footprints. Each outline is matched to the footprint it overlaps most. Prints one line per outline and the
median; the gate (median >= 0.6) is read by a human."""

from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path

import shapely
from shapely.geometry import shape

from camsim_scene.buildings import read_snapshot


def ious(snapshot: Path, truth: Path) -> list[dict]:
    fps = read_snapshot(snapshot)
    tree = shapely.STRtree([shapely.make_valid(f.geometry) for f in fps])
    out = []
    for feat in json.loads(Path(truth).read_text())["features"]:
        t = shapely.make_valid(shape(feat["geometry"]))
        best, match = 0.0, None
        for i in tree.query(t, predicate="intersects"):
            g = tree.geometries[i]
            iou = t.intersection(g).area / t.union(g).area
            if iou > best:
                best, match = iou, fps[i].source_id
        out.append({"name": feat.get("properties", {}).get("name", ""), "match": match, "iou": best})
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--snapshot", type=Path, required=True)
    ap.add_argument("--truth", type=Path, required=True)
    a = ap.parse_args()
    rows = ious(a.snapshot, a.truth)
    for r in rows:
        print(f"{r['name']:<24} {r['match'] or '-':<40} {r['iou']:.3f}")
    print(f"median IoU {statistics.median(r['iou'] for r in rows):.3f} over {len(rows)} outlines")


if __name__ == "__main__":
    main()
