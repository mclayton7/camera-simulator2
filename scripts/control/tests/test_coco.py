"""COCO dataset from capture records: images, annotations (masked and projected), categories."""

from __future__ import annotations

import json
from pathlib import Path

from camsim_control.client import Capture
from camsim_control.coco import DatasetWriter, load_categories

RECORD = {
    "frame_id": 42, "timestamp_us": 1234567,
    "platform": {"lat": 33.0, "lon": -117.0, "alt_m": 900.0, "yaw_deg": 0, "pitch_deg": 0, "roll_deg": 0},
    "annotations": [
        {"entity_id": 3, "source": "api", "source_id": "7", "category": {"id": 2001, "name": "truck"},
         "bbox": [10.0, 20.0, 30.0, 30.0], "area": 700.0, "iscrowd": 0, "truncated": 0, "mask_source": "render",
         "visibility": 0.875, "bbox_amodal": [9.0, 19.0, 32.0, 33.0], "obb": [25, 35, 30, 30, 12],
         "obb_amodal": [25, 35, 32, 33, 12], "segmentation": {"size": [360, 640], "counts": "abc"},
         "truncation": 0.0, "geo": {"lat": 33.3, "lon": -117.3, "alt_m": 40.25}},
        {"entity_id": 4, "source": "api", "source_id": "8", "category": {"id": 3001, "name": "boat"},
         "bbox": [100.0, 100.0, 10.0, 20.0], "area": 200.0, "iscrowd": 0, "truncated": 0, "mask_source": "projection"},
    ],
}


def _cap(record=RECORD, depth=b"depth") -> Capture:
    return Capture(frame_id=42, utc="2026-06-15T18:00:00.000Z", timestamp_us=1234567, width=640, height=360,
                   sensor_mode="eo", png=b"png", depth_png=depth, depth_far_plane_m=5000.0, record=record,
                   settle={"frames_waited": 30})


def test_writes_coco(tmp_path: Path):
    w = DatasetWriter(tmp_path, {2001: "truck", 3001: "boat"})
    img_id = w.add("000000_eo", _cap(), extra={"seed": 7})
    path = w.close()
    coco = json.loads(path.read_text())
    assert img_id == 1
    assert (tmp_path / "images" / "000000_eo.png").read_bytes() == b"png"
    assert (tmp_path / "depth" / "000000_eo.png").read_bytes() == b"depth"
    assert coco["categories"] == [{"id": 2001, "name": "truck"}, {"id": 3001, "name": "boat"}]
    img = coco["images"][0]
    assert img == {"id": 1, "file_name": "images/000000_eo.png", "width": 640, "height": 360,
                   "camsim": {"frame_id": 42, "timestamp_us": 1234567, "utc": "2026-06-15T18:00:00.000Z",
                              "sensor_mode": "eo", "depth_file": "depth/000000_eo.png", "depth_far_plane_m": 5000.0,
                              "platform": RECORD["platform"], "seed": 7}}
    a, b = coco["annotations"]
    assert (a["id"], a["image_id"], a["category_id"], a["bbox"], a["area"], a["iscrowd"]) == (1, 1, 2001, [10.0, 20.0, 30.0, 30.0], 700.0, 0)
    assert a["segmentation"] == {"size": [360, 640], "counts": "abc"}
    assert a["camsim"]["mask_source"] == "render" and a["camsim"]["visibility"] == 0.875
    assert a["camsim"]["geo"] == {"lat": 33.3, "lon": -117.3, "alt_m": 40.25}
    assert "segmentation" not in b and b["camsim"]["mask_source"] == "projection"


def test_no_record_no_depth(tmp_path: Path):
    w = DatasetWriter(tmp_path, {})
    w.add("x", _cap(record=None, depth=None))
    coco = json.loads(w.close().read_text())
    assert coco["annotations"] == []
    assert "depth_file" not in coco["images"][0]["camsim"]


def test_unknown_category_added_from_record(tmp_path: Path):
    w = DatasetWriter(tmp_path, {})
    w.add("x", _cap())
    coco = json.loads(w.close().read_text())
    assert {c["id"]: c["name"] for c in coco["categories"]} == {2001: "truck", 3001: "boat"}


def test_load_categories(tmp_path: Path):
    cfg = tmp_path / "c.yaml"
    cfg.write_text('ocean:\n  x: 1\nentity_types:\n  "1001":\n    mesh: "a.glb"\n  "2001":\n    mesh: "t.glb"\n'
                   '    class_name: truck\n  "3001":\n    class_name: "boat"\nrender:\n  class_name: nope\n')
    assert load_categories(cfg) == {1001: "type_1001", 2001: "truck", 3001: "boat"}
