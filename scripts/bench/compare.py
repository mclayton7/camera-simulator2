"""Compare two bench runs; print a markdown table and shot SSIM (ROADMAP 3A).

Usage: uv run --with numpy --with pillow python scripts/bench/compare.py BASELINE_DIR CURRENT_DIR
Each directory holds results.json and optionally shots/*.png.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np

METRICS = [
    "wall_ms_p50",
    "wall_ms_p95",
    "wall_ms_p99",
    "gpu_ms_p50",
    "gpu_ms_p95",
    "game_ms_p50",
    "render_ms_p50",
    "hitches_66",
    "hitches_100",
    "popin_fraction",
    "families_mean",
    "emitted_fps",
    "dropped",
]


def _fmt(v) -> str:
    if v is None:
        return "—"
    return f"{v:.3f}" if isinstance(v, float) and abs(v) < 1 else f"{float(v):.1f}"


def _delta(a, b) -> str:
    if a is None or b is None:
        return "—"
    if a == 0:
        return "n/a" if b != 0 else "0%"
    return f"{(b - a) / a * 100.0:+.0f}%"


def render_table(baseline: dict, current: dict) -> str:
    bp, cp = baseline.get("phases", {}), current.get("phases", {})
    lines = [
        "| phase | metric | baseline | current | change |",
        "| --- | --- | --- | --- | --- |",
    ]
    for phase in list(dict.fromkeys([*bp, *cp])):
        for m in METRICS:
            a, b = bp.get(phase, {}).get(m), cp.get(phase, {}).get(m)
            if a is None and b is None:
                continue
            lines.append(f"| {phase} | {m} | {_fmt(a)} | {_fmt(b)} | {_delta(a, b)} |")
    return "\n".join(lines)


def ssim(a: np.ndarray, b: np.ndarray) -> float:
    """Mean SSIM over 8x8 blocks of the luma channel (information only, not a gate)."""

    def luma(x):
        x = x.astype(np.float64)
        return (
            0.299 * x[..., 0] + 0.587 * x[..., 1] + 0.114 * x[..., 2]
            if x.ndim == 3
            else x
        )

    if a.shape != b.shape:
        return 0.0
    ya, yb = luma(a), luma(b)
    if np.array_equal(ya, yb):
        return 1.0
    h, w = (ya.shape[0] // 8) * 8, (ya.shape[1] // 8) * 8
    ba = ya[:h, :w].reshape(h // 8, 8, w // 8, 8).swapaxes(1, 2).reshape(-1, 64)
    bb = yb[:h, :w].reshape(h // 8, 8, w // 8, 8).swapaxes(1, 2).reshape(-1, 64)
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    ma, mb = ba.mean(1), bb.mean(1)
    va, vb = ba.var(1), bb.var(1)
    cov = ((ba - ma[:, None]) * (bb - mb[:, None])).mean(1)
    s = ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma**2 + mb**2 + c1) * (va + vb + c2))
    return float(s.mean())


def main() -> int:
    from PIL import Image

    base_dir, cur_dir = Path(sys.argv[1]), Path(sys.argv[2])
    base = json.loads((base_dir / "results.json").read_text())
    cur = json.loads((cur_dir / "results.json").read_text())
    print(
        f"Baseline: {base.get('meta', {}).get('label')}  Current: {cur.get('meta', {}).get('label')}\n"
    )
    print(render_table(base, cur))
    shots = sorted(p.name for p in (base_dir / "shots").glob("*.png"))
    if shots:
        print("\n| shot | SSIM |\n| --- | --- |")
        for name in shots:
            other = cur_dir / "shots" / name
            val = (
                ssim(
                    np.asarray(Image.open(base_dir / "shots" / name).convert("RGB")),
                    np.asarray(Image.open(other).convert("RGB")),
                )
                if other.exists()
                else None
            )
            print(f"| {name} | {_fmt(val)} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
