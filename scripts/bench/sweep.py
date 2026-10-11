"""Realism vs cost sweep for buildings and trees (REALISM R3/R4 bench,
docs/superpowers/specs/2026-10-10-r3r4-realism-cost-bench-design.md).

Runs run_bench.py once per row (only the dense_pass phase plus the warm-up, the dense_oblique shot, GPU memory on)
and writes sweep.csv (one line per run), sweep.md (ranges per row, deltas and budget against the all-off row) and
sweep_sheet.png (the dense_oblique shot of every row).

Usage:
  uv run --with numpy --with pillow python scripts/bench/sweep.py --scene PKG --out DIR \
      --axis buildings=off,on --axis buildings_sse=8,16,32,64 [--repeat 2] [--sensor eo] [--dry-run]
"""

from __future__ import annotations

import argparse
import csv
import itertools
import json
import statistics
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# axis -> (CamSim env var, the master axis whose "off" disables it)
AXES: dict[str, tuple[str, str | None]] = {
    "buildings": ("CAMSIM_SCENE_BUILDINGS_ENABLED", None),
    "buildings_sse": ("CAMSIM_SCENE_BUILDINGS_MAXIMUM_SCREEN_SPACE_ERROR", "buildings"),
    "buildings_physics": ("CAMSIM_SCENE_BUILDINGS_CREATE_PHYSICS_MESHES", "buildings"),
    "trees": ("CAMSIM_VEGETATION_ENABLED", None),
    "trees_radius": ("CAMSIM_VEGETATION_RADIUS_M", "trees"),
    "trees_density": ("CAMSIM_VEGETATION_DENSITY_SCALE", "trees"),
    "trees_shadows": ("CAMSIM_VEGETATION_CAST_SHADOWS", "trees"),
}
SWITCH = {"on": "1", "off": "0"}  # CamSim reads env bools as ints
PHASE = "dense_pass"
SHOT = "dense_oblique"
METRICS = ["game_ms_p50", "game_ms_p95", "gpu_ms_p50", "wall_ms_p95", "dropped", "emitted_fps", "gpu_mem_mib_max"]
# Spec section 4: increase of a row's median over the all-off row's median. Memory: 1 GiB (raised 2026-10-11 from 381 MiB).
BUDGET = {"game_ms_p50": 1.5, "game_ms_p95": 3.0, "gpu_ms_p50": 5.0, "gpu_mem_mib_max": 1024.0}
MIN_FPS = 29.5  # "30 fps": emitted_fps over a 90 s phase reads ~29.9


def parse_axis(text: str) -> tuple[str, list[str]]:
    name, _, vals = text.partition("=")
    if name not in AXES:
        sys.exit(f"unknown axis '{name}' (valid: {', '.join(AXES)})")
    values = [v for v in (x.strip() for x in vals.split(",")) if v]
    if not values:
        sys.exit(f"axis '{name}' has no values")
    return name, values


def build_rows(axes: list[tuple[str, list[str]]]) -> list[dict[str, str]]:
    """Cartesian product in axis order; a dependent axis is dropped from rows where its master is off or unnamed, and the
    duplicates that leaves are removed (first occurrence kept)."""
    rows: list[dict[str, str]] = []
    for combo in itertools.product(*[v for _, v in axes]) if axes else [()]:
        row = dict(zip([n for n, _ in axes], combo))
        row = {k: v for k, v in row.items() if not (AXES[k][1] and row.get(AXES[k][1]) != "on")}
        if row not in rows:
            rows.append(row)
    return rows


def row_label(row: dict[str, str]) -> str:
    return "_".join(f"{k}-{v}" for k, v in row.items()) or "baseline"


MASTERS = [k for k, (_, master) in AXES.items() if master is None]


def row_env(row: dict[str, str]) -> dict[str, str]:
    """Both master switches are always sent: the runtime defaults are ON, so a feature the row does not switch on
    must be forced off or the all-off baseline would silently have it."""
    env = {AXES[m][0]: SWITCH["on" if row.get(m) == "on" else "off"] for m in MASTERS}
    env.update({AXES[k][0]: SWITCH.get(v, v) for k, v in row.items()})
    return env


def run_metrics(results_path: Path) -> dict:
    try:
        ph = json.loads(results_path.read_text())["phases"][PHASE]
    except Exception:  # noqa: BLE001 -- a crashed run leaves no (or a partial) results.json
        return {"status": "failed"}
    if not ph.get("frames"):
        return {"status": "failed"}  # _pct reports 0.0 ms for an empty phase: never read that as fast
    return {"status": "ok", **{m: ph.get(m) for m in METRICS}}


def _median(runs: list[dict], m: str) -> float | None:
    vals = [r[m] for r in runs if r.get(m) is not None]
    return statistics.median(vals) if vals else None


MEM = "gpu_mem_mib_max"


def budget_verdict(row: dict, base: dict) -> dict[str, bool | None]:
    """True / False per budget item. Memory that was not measured (no footprint / nvidia-smi on this host) is None,
    reported n/a; any other unknown metric is not a pass."""
    v: dict[str, bool | None] = {}
    for m, limit in BUDGET.items():
        a, b = base.get(f"{m}_median"), row.get(f"{m}_median")
        if m == MEM and (a is None or b is None):
            v[m] = None
            continue
        v[m] = a is not None and b is not None and b - a <= limit  # unknown is not a pass
    v["dropped"] = row.get("dropped_median") == 0
    v["fps"] = (row.get("emitted_fps_median") or 0.0) >= MIN_FPS
    return v


def _is_baseline(row: dict[str, str]) -> bool:
    """The all-off row: every master switch is off or unnamed (unnamed ones are forced off by row_env)."""
    return all(row.get(m) != "on" for m in MASTERS)


def aggregate(rows: list[dict], runs: dict[str, list[dict]]) -> list[dict]:
    out = []
    for row in rows:
        label = row_label(row)
        ok = [r for r in runs.get(label, []) if r["status"] == "ok"]
        a = {"label": label, "row": row, "runs": len(runs.get(label, [])), "ok_runs": len(ok),
             "status": "ok" if ok else "failed", "baseline": _is_baseline(row)}
        for m in METRICS:
            vals = [r[m] for r in ok if r.get(m) is not None]
            a[m] = (min(vals), max(vals)) if vals else None
            a[f"{m}_median"] = _median(ok, m)
        out.append(a)
    base = next((a for a in out if a["baseline"] and a["status"] == "ok"), None)
    for a in out:
        a["budget"] = budget_verdict(a, base) if base and a["status"] == "ok" and not a["baseline"] else {}
        vals = list(a["budget"].values())
        # None = no budget (baseline, failed row) or every measured item passes but memory is n/a.
        a["within_budget"] = (False if False in vals else None if None in vals else True) if vals else None
    return out


def _rng(v) -> str:
    if v is None:
        return "n/a"
    lo, hi = v
    return f"{lo:.2f}" if lo == hi else f"{lo:.2f}-{hi:.2f}"


def render_md(agg: list[dict]) -> str:
    head = ["row", "status", *METRICS, "budget"]
    lines = ["| " + " | ".join(head) + " |", "|" + " --- |" * len(head)]
    for a in agg:
        failed = [k for k, ok in a["budget"].items() if ok is False]
        verdict = ("baseline" if a["baseline"] else
                   "FAIL: " + ", ".join(failed) if failed else
                   "pass" if a["within_budget"] else
                   "pass (memory n/a)" if a["budget"] else
                   "n/a")
        cells = [a["label"], f"{a['status']} ({a['ok_runs']}/{a['runs']})", *[_rng(a[m]) for m in METRICS], verdict]
        lines.append("| " + " | ".join(cells) + " |")
    lines.append("")
    mem_missing = any(a["status"] == "ok" and a.get(f"{MEM}_median") is None for a in agg)
    lines.append(f"Budget (spec section 4): increase of the row median over the all-off row: "
                 f"{', '.join(f'{k} <= {v}' for k, v in BUDGET.items())}; dropped 0; fps >= {MIN_FPS}. "
                 f"Memory in MiB{' (n/a: not measurable on this host)' if mem_missing else ''}. Ranges are over repeats.")
    return "\n".join(lines) + "\n"


def write_csv(path: Path, rows: list[dict], runs: dict[str, list[dict]]) -> None:
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["row", "repeat", "status", *METRICS])
        for row in rows:
            for i, r in enumerate(runs.get(row_label(row), [])):
                w.writerow([row_label(row), i, r["status"], *[r.get(m, "") for m in METRICS]])


def sheet_cols(axes: list[tuple[str, list[str]]]) -> int:
    """Contact sheet width = the last axis's cardinality, so each sheet row is one setting of the other axes."""
    return max(1, len(axes[-1][1])) if axes else 1


def contact_sheet(images: list[tuple[str, Path]], out: Path, thumb_w: int = 640, cols: int = 3) -> None:
    from PIL import Image, ImageDraw

    thumb_h = thumb_w * 9 // 16
    label_h = 24
    n = len(images)
    rows = (n + cols - 1) // cols
    sheet = Image.new("RGB", (cols * thumb_w, rows * (thumb_h + label_h)), (32, 32, 32))
    draw = ImageDraw.Draw(sheet)
    for i, (label, path) in enumerate(images):
        x, y = (i % cols) * thumb_w, (i // cols) * (thumb_h + label_h)
        if path.exists():
            im = Image.open(path).convert("RGB")
            im.thumbnail((thumb_w, thumb_h))
            sheet.paste(im, (x, y + label_h))
        else:
            draw.text((x + 8, y + label_h + thumb_h // 2), "no shot (run failed)", fill=(255, 96, 96))
        draw.text((x + 8, y + 6), label, fill=(255, 255, 255))
    sheet.save(out)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--scene", type=Path, required=True, help="scene package (absolute path)")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--axis", action="append", default=[], metavar="NAME=V1,V2")
    ap.add_argument("--repeat", type=int, default=2)
    ap.add_argument("--site", default="pendleton")
    ap.add_argument("--sensor", choices=["eo", "ir"], default="eo")
    ap.add_argument("--dry-run", action="store_true", help="print the rows and commands, run nothing")
    a = ap.parse_args(argv)
    axes = [parse_axis(t) for t in a.axis]
    rows = build_rows(axes)
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    runs: dict[str, list[dict]] = {}
    for row in rows:
        label = row_label(row)
        for k in range(a.repeat):
            run_dir = out / "rows" / label / f"r{k}"
            cmd = [sys.executable, str(REPO / "scripts" / "bench" / "run_bench.py"),
                   "--label", f"{label}-r{k}", "--site", a.site, "--out", str(run_dir),
                   "--phases", PHASE, "--shots", SHOT, "--gpu-mem", "--sensor", a.sensor,
                   "--env", f"CAMSIM_SCENE_DIR={a.scene.resolve()}"]
            for key, val in row_env(row).items():
                cmd += ["--env", f"{key}={val}"]
            print(f"[sweep] {label} r{k}: {' '.join(cmd)}", flush=True)
            if a.dry_run:
                continue
            subprocess.run(cmd, check=False)  # a failed run is recorded as failed, the sweep goes on
            runs.setdefault(label, []).append(run_metrics(run_dir / "results.json"))
    if a.dry_run:
        return 0
    agg = aggregate(rows, runs)
    write_csv(out / "sweep.csv", rows, runs)
    (out / "sweep.md").write_text(render_md(agg))
    (out / "shots").mkdir(exist_ok=True)
    sheet = []
    for row in rows:
        label = row_label(row)
        src = out / "rows" / label / "r0" / "shots" / f"{SHOT}.png"
        dst = out / "shots" / f"{label}.png"
        dst.unlink(missing_ok=True)  # a failed r0 must not show an old shot
        if src.exists():
            dst.write_bytes(src.read_bytes())
        sheet.append((label, dst))
    contact_sheet(sheet, out / "sweep_sheet.png", cols=sheet_cols(axes))
    print((out / "sweep.md").read_text())
    return 0


if __name__ == "__main__":
    sys.exit(main())
