#!/usr/bin/env python3
"""Generate a seeded COCO dataset through CamSim's control API (ROADMAP 2.4, docs/control-api.md).

CamSim must run with the control API on (CAMSIM_CONTROL_API_ENABLED=1) and ground truth on (CAMSIM_ML_ENABLED=1;
CAMSIM_ML_INTERVAL_FRAMES=100000 keeps the JSONL to the capture frames). Each sample: clear the API entities, place
new ones, set environment and time, then pose the camera and capture once per mode (EO, IR) on the same scene.

    uv run --project scripts/control python scripts/control/make_dataset.py --out OUT --count 100 --seed 7 \
        --site 33.3072917,-117.344625 --ground-alt 20 --modes eo,ir --types 2001:ground
"""

from __future__ import annotations

import argparse
import sys
import time
from collections.abc import Callable
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from camsim_control.client import Client  # noqa: E402
from camsim_control.coco import DatasetWriter, load_categories  # noqa: E402
from camsim_control.randomize import EntitySpec, sample  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
DEFAULT_CONFIG = REPO / "deploy" / "camsim_config.yaml"


def parse_types(text: str) -> list[EntitySpec]:
    specs = []
    for item in text.split(","):
        type_id, _, surface = item.partition(":")
        specs.append(EntitySpec(int(type_id), surface or "ground"))
    return specs


def generate(client: Client, out: Path, count: int, seed: int, site: tuple[float, float], ground_alt: float,
             modes: list[str], types: list[EntitySpec], categories: dict[int, str], settle_frames: int | None = None,
             warmup_frames: int = 0, log: Callable[[str], None] = print) -> Path:
    """warmup_frames > 0: one discarded capture of sample 0's first view before the first sample."""
    writer = DatasetWriter(out, categories)
    client.set_clock(lockstep=True)
    for i in range(count):
        s = sample(seed, i, site, ground_alt, types)
        t0 = time.monotonic()
        client.clear_entities()
        for n, e in enumerate(s.entities, start=1):
            client.put_entity(n, **e)
        client.set_environment(**s.environment)
        client.set_clock(utc=s.utc)
        if i == 0 and warmup_frames > 0 and modes:
            client.set_camera(**s.camera, sensor={"mode": modes[0]})
            client.capture(settle_frames=warmup_frames)
        for mode in modes:
            client.set_camera(**s.camera, sensor={"mode": mode})
            cap = client.capture(settle_frames=settle_frames)
            n_ann = len((cap.record or {}).get("annotations", []))
            writer.add(f"{i:06d}_{mode}", cap, extra={"seed": seed, "sample": i, "utc_requested": s.utc})
            log(f"sample {i:4d} {mode}: frame {cap.frame_id}, {n_ann} annotation(s), "
                f"settled in {cap.settle.get('frames_waited')} frames / {cap.settle.get('wall_ms')} ms")
        log(f"sample {i:4d}: {time.monotonic() - t0:.1f} s")
    return writer.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--count", type=int, default=10)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--site", required=True, help="LAT,LON of the aim area")
    ap.add_argument("--ground-alt", type=float, required=True, help="ground height (WGS-84 ellipsoid m) at the site")
    ap.add_argument("--modes", default="eo", help="eo, ir or eo,ir")
    ap.add_argument("--types", default="2001:ground", help="TYPE[:SURFACE],... (surface none|ground|water)")
    ap.add_argument("--config", type=Path, default=DEFAULT_CONFIG, help="camsim_config.yaml for category names")
    ap.add_argument("--settle-frames", type=int, default=None)
    ap.add_argument("--warmup-frames", type=int, default=0, help="one discarded capture of this many settle frames first")
    ap.add_argument("--base", default="http://127.0.0.1:8080")
    args = ap.parse_args()

    lat, lon = (float(v) for v in args.site.split(","))
    client = Client(args.base)
    client.wait_ready()
    path = generate(client, args.out, args.count, args.seed, (lat, lon), args.ground_alt,
                    [m.strip() for m in args.modes.split(",")], parse_types(args.types),
                    load_categories(args.config), args.settle_frames, args.warmup_frames)
    print(f"dataset: {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
