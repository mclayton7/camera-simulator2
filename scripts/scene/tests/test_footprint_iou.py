import json
import sys
from pathlib import Path

import shapely
from overture_fixture import box_lonlat, write_overture_parquet

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import footprint_iou  # noqa: E402


def test_iou_matches_best_overlap(tmp_path):
    snap = write_overture_parquet(
        tmp_path / "s.parquet",
        [
            {"id": "a", "geom": box_lonlat(10.0, 10.0, 20, 20)},
            {"id": "b", "geom": box_lonlat(10.01, 10.0, 20, 20)},
        ],
    )
    truth = box_lonlat(10.0, 10.0, 20, 10)  # half of a
    (tmp_path / "t.geojson").write_text(
        json.dumps(
            {
                "type": "FeatureCollection",
                "features": [
                    {"type": "Feature", "properties": {"name": "t1"}, "geometry": shapely.geometry.mapping(truth)}
                ],
            }
        )
    )
    rows = footprint_iou.ious(snap, tmp_path / "t.geojson")
    assert [r["match"] for r in rows] == ["a"] and abs(rows[0]["iou"] - 0.5) < 1e-6
