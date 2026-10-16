"""A COCO dataset from control API captures: images/, depth/, annotations.json (docs/control-api.md)."""

from __future__ import annotations

import json
import re
from pathlib import Path

from camsim_control.client import Capture
from camsim_control.semantic import CLASSES

_EXTRA_KEYS = ("entity_id", "source", "source_id", "mask_source", "visibility", "bbox_amodal", "obb", "obb_amodal",
               "truncated", "truncation", "box3d", "geo")


def load_categories(config: Path) -> dict[int, str]:
    """Entity type id -> class_name from a camsim_config.yaml's entity_types (type_NNNN when unnamed)."""
    cats: dict[int, str] = {}
    in_types, current = False, None
    for line in Path(config).read_text().splitlines():
        if re.match(r"^\S", line):
            in_types = line.startswith("entity_types:")
            current = None
            continue
        if not in_types:
            continue
        if m := re.match(r'^  "?(\d+)"?:\s*$', line):
            current = int(m.group(1))
            cats[current] = f"type_{current}"
        elif current is not None and (m := re.match(r'^\s+class_name:\s*"?([^"#\s]+)"?', line)):
            cats[current] = m.group(1)
    return cats


class DatasetWriter:
    def __init__(self, out_dir: Path, categories: dict[int, str]):
        self.out = Path(out_dir)
        (self.out / "images").mkdir(parents=True, exist_ok=True)
        (self.out / "depth").mkdir(parents=True, exist_ok=True)
        (self.out / "semantic").mkdir(parents=True, exist_ok=True)
        self.categories = dict(categories)
        self.images: list[dict] = []
        self.annotations: list[dict] = []

    def add(self, name: str, cap: Capture, extra: dict | None = None) -> int:
        image_id = len(self.images) + 1
        (self.out / "images" / f"{name}.png").write_bytes(cap.png)
        camsim: dict = {"frame_id": cap.frame_id, "timestamp_us": cap.timestamp_us, "utc": cap.utc,
                        "sensor_mode": cap.sensor_mode}
        if cap.depth_png:
            (self.out / "depth" / f"{name}.png").write_bytes(cap.depth_png)
            camsim["depth_file"] = f"depth/{name}.png"
            camsim["depth_far_plane_m"] = cap.depth_far_plane_m
        if cap.semantic_png:
            (self.out / "semantic" / f"{name}.png").write_bytes(cap.semantic_png)
            camsim["semantic_file"] = f"semantic/{name}.png"
        if cap.record and "platform" in cap.record:
            camsim["platform"] = cap.record["platform"]
        camsim.update(extra or {})
        self.images.append({"id": image_id, "file_name": f"images/{name}.png", "width": cap.width,
                            "height": cap.height, "camsim": camsim})
        for a in (cap.record or {}).get("annotations", []):
            cat = a["category"]
            self.categories.setdefault(cat["id"], cat["name"])
            ann = {"id": len(self.annotations) + 1, "image_id": image_id, "category_id": cat["id"],
                   "bbox": a["bbox"], "area": a["area"], "iscrowd": a.get("iscrowd", 0),
                   "camsim": {k: a[k] for k in _EXTRA_KEYS if k in a}}
            if "segmentation" in a:
                ann["segmentation"] = a["segmentation"]
            self.annotations.append(ann)
        return image_id

    def close(self) -> Path:
        path = self.out / "annotations.json"
        cats = [{"id": i, "name": n} for i, n in sorted(self.categories.items())]
        semantic = [{"id": i, "name": n} for i, n in sorted(CLASSES.items())]
        path.write_text(json.dumps({"images": self.images, "annotations": self.annotations, "categories": cats,
                                    "camsim_semantic_classes": semantic}, indent=1))
        return path
