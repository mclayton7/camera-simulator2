"""Summarise scripts/bench/load_scale.sh output: per point, thread times, fps, drops, latency, COCO count.

Usage: python3 scripts/bench/load_report.py [OUT]   (default .cache/bench/load)
"""

import json
import re
import statistics
import sys
from pathlib import Path


def pct(values: list[float], q: float) -> float:
    v = sorted(values)
    return v[int(q * (len(v) - 1))] if v else float("nan")


def read_jsonl(path: Path) -> list[dict]:
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


def p50_p99(rows: list[dict], key: str) -> str:
    vals = [r[key] for r in rows]
    return f"{pct(vals, 0.5):5.2f}/{pct(vals, 0.99):5.2f}"


def summarise(d: Path) -> str:
    w = json.loads((d / "window.json").read_text())
    sel = [
        r for r in read_jsonl(d / "frames.jsonl") if w["start"] <= r["t"] <= w["end"]
    ]
    if not sel:
        return f"{d.name:16} no frames in window"
    span = sel[-1]["t"] - sel[0]["t"]
    # emitted / dropped are cumulative counters
    fps = (sel[-1]["emitted"] - sel[0]["emitted"]) / span if span > 0 else 0.0
    drops = sel[-1]["dropped"] - sel[0]["dropped"]
    hitches = sum(r["wall_ms"] > 66.7 for r in sel)
    m = (d / "metrics.txt").read_text()
    ents = re.search(r"^camsim_entity_count (\d+)", m, re.MULTILINE)
    lat = dict(
        re.findall(
            r'^camsim_frame_latency_ms\{quantile="([0-9.]+)"\} ([0-9.]+)',
            m,
            re.MULTILINE,
        )
    )
    coco = d / "ml" / "camsim_coco.jsonl"
    annotated = (
        round(statistics.median(len(r["annotations"]) for r in read_jsonl(coco)))
        if coco.exists()
        else "-"
    )
    return (
        f"{d.name:16} {ents.group(1) if ents else '?':>5} {len(sel):6} {fps:6.2f} {drops:4} {hitches:5} "
        f"{p50_p99(sel, 'game_ms'):>13} {p50_p99(sel, 'render_ms'):>15} {p50_p99(sel, 'gpu_ms'):>12} "
        f"{float(lat.get('0.5', 'nan')):5.1f}/{float(lat.get('0.99', 'nan')):5.1f} {annotated}"
    )


def main() -> None:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".cache/bench/load")
    print(
        f"{'point':16} {'ents':>5} {'frames':>6} {'fps':>6} {'drop':>4} {'>66ms':>5} "
        f"{'game p50/p99':>13} {'render p50/p99':>15} {'gpu p50/p99':>12} {'lat p50/p99':>12} annotated/frame"
    )
    for d in sorted(root.iterdir(), key=lambda p: (len(p.name), p.name)):
        if (d / "window.json").exists():
            print(summarise(d))


if __name__ == "__main__":
    main()
