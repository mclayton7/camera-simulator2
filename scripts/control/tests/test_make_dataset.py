"""make_dataset.generate against a fake client: call order per sample and the dataset it writes."""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import make_dataset  # noqa: E402
from camsim_control.client import Capture  # noqa: E402
from camsim_control.randomize import EntitySpec  # noqa: E402


class FakeClient:
    def __init__(self):
        self.calls = []
        self.frame = 0

    def __getattr__(self, name):
        def call(*args, **kwargs):
            self.calls.append((name, args, kwargs))
        return call

    def capture(self, **kwargs):
        self.calls.append(("capture", (), kwargs))
        self.frame += 1
        return Capture(frame_id=self.frame, utc="u", timestamp_us=1, width=4, height=2, sensor_mode="eo", png=b"p",
                       depth_png=None, depth_far_plane_m=0.0,
                       record={"annotations": [{"category": {"id": 2001, "name": "truck"}, "bbox": [0, 0, 1, 1],
                                                "area": 1, "iscrowd": 0}]}, settle={})


def test_generate(tmp_path: Path):
    c = FakeClient()
    path = make_dataset.generate(c, tmp_path, 2, 7, (33.0, -117.0), 20.0, ["eo", "ir"], [EntitySpec(2001, "ground")],
                                 {2001: "truck"}, log=lambda s: None)
    names = [n for n, _, _ in c.calls]
    assert names[0] == "set_clock" and c.calls[0][2] == {"lockstep": True}
    assert names.count("capture") == 4
    assert names.count("clear_entities") == 2
    cams = [kw for n, _, kw in c.calls if n == "set_camera"]
    assert [k["sensor"]["mode"] for k in cams] == ["eo", "ir", "eo", "ir"]
    coco = json.loads(path.read_text())
    assert [i["file_name"] for i in coco["images"]] == ["images/000000_eo.png", "images/000000_ir.png",
                                                        "images/000001_eo.png", "images/000001_ir.png"]
    assert len(coco["annotations"]) == 4


def test_parse_types():
    assert make_dataset.parse_types("2001:ground,3001:water,1001") == [
        EntitySpec(2001, "ground"), EntitySpec(3001, "water"), EntitySpec(1001, "ground")]
