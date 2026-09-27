"""Turn frame-stats JSONL rows into per-phase metrics (ROADMAP 3A)."""
from __future__ import annotations

import json
import sys
from pathlib import Path

HITCH_MS = 66.7        # two frames at 30 fps
BIG_HITCH_MS = 100.0


def _pct(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    s = sorted(values)
    k = max(0, min(len(s) - 1, round(p / 100.0 * (len(s) - 1))))
    return float(s[k])


def load_rows(path: Path) -> list[dict]:
    """Rows that parse; a crashed run can leave a partial last line."""
    rows, bad = [], 0
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            bad += 1
    if bad:
        print(f"[analyze] {path}: skipped {bad} unparseable line(s)", file=sys.stderr)
    return rows


def summarize(rows: list[dict], phases: list[dict]) -> dict:
    out: dict[str, dict] = {}
    for ph in phases:
        if not ph["measured"]:
            continue
        sel = [r for r in rows if ph["start"] <= r["t"] < ph["end"]]
        wall = [r["wall_ms"] for r in sel]
        span = (sel[-1]["t"] - sel[0]["t"]) if len(sel) > 1 else 0.0
        out[ph["name"]] = {
            "frames": len(sel),
            "wall_ms_p50": _pct(wall, 50), "wall_ms_p95": _pct(wall, 95), "wall_ms_p99": _pct(wall, 99),
            "game_ms_p50": _pct([r["game_ms"] for r in sel], 50),
            "render_ms_p50": _pct([r["render_ms"] for r in sel], 50),
            "rhi_ms_p50": _pct([r["rhi_ms"] for r in sel], 50),
            "gpu_ms_p50": _pct([r["gpu_ms"] for r in sel], 50),
            "gpu_ms_p95": _pct([r["gpu_ms"] for r in sel], 95),
            "hitches_66": sum(1 for w in wall if w > HITCH_MS),
            "hitches_100": sum(1 for w in wall if w > BIG_HITCH_MS),
            "popin_fraction": (sum(1 for r in sel if r["load_pct"] < 100.0) / len(sel)) if sel else 0.0,
            "families_mean": (sum(r["families"] for r in sel) / len(sel)) if sel else 0.0,
            "dropped": (sel[-1]["dropped"] - sel[0]["dropped"]) if sel else 0,
            # Frames handed to the sensor model/encoder per second (the stream's real rate).
            "emitted_fps": ((sel[-1]["emitted"] - sel[0]["emitted"]) / span) if span > 0 else 0.0,
        }
    return out
