#!/usr/bin/env python3
# Copyright CamSim Contributors. All Rights Reserved.
"""Calibrate vegetation colours against the imagery under them (REALISM R4, docs/vegetation.md "Colour calibration").

  caffeinate -ims uv run --project scripts/scene --with pillow --with numpy python scripts/vegetation/calibrate_colours.py \
      --scene "$PWD/.cache/scene-packages/pendleton-r4" --out .cache/veg/colours [--configs broadleaf,chaparral,...]

For each configuration (one library type at a time, through a species mix file on a package without its own mix; far
trees alone by shrinking the near radius to 100 m), CamSim renders the riparian woodland from 120 m and 350 m with
vegetation on, and once with it off. Vegetation pixels are where the two frames differ; over them, the imagery's mean
linear colour divided by the render's (after dividing out the auto-exposure difference, measured on the other pixels)
is the factor that type's base colour needs. Prints one line per configuration and writes report.json; the new factors
go to prepare_tree.py (--base-color / --far-color). The ratio is linear in the base colour (fixed sun, fixed sensor),
so one pass gets close and a second confirms.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

MIN_PIXELS = 300
DIFF_THRESHOLD = 60  # summed |on - off| over RGB (8-bit) for a pixel to count as vegetation

# name -> (mix {code: {type: weight}}, shrub_density {code: n}, extra env)
CONFIGS: dict[str, tuple[dict, dict, dict]] = {
    "broadleaf": ({"10": {"tree_broadleaf_a": 1.0}}, {}, {"CAMSIM_VEGETATION_SHRUBS_ENABLED": "0", "CAMSIM_VEGETATION_FAR_RADIUS_M": "0"}),
    "pine": ({"10": {"tree_pine_a": 1.0}}, {}, {"CAMSIM_VEGETATION_SHRUBS_ENABLED": "0", "CAMSIM_VEGETATION_FAR_RADIUS_M": "0"}),
    "chaparral": ({"20": {"shrub_chaparral": 1.0}, "30": {"shrub_chaparral": 1.0}}, {"20": 6.0, "30": 6.0},
                  {"CAMSIM_VEGETATION_FAR_RADIUS_M": "0"}),
    "desert": ({"20": {"shrub_desert": 1.0}, "30": {"shrub_desert": 1.0}}, {"20": 6.0, "30": 6.0},
               {"CAMSIM_VEGETATION_FAR_RADIUS_M": "0"}),
    "far_broadleaf": ({"10": {"tree_broadleaf_a": 1.0}}, {}, {"CAMSIM_VEGETATION_SHRUBS_ENABLED": "0", "CAMSIM_VEGETATION_RADIUS_M": "100"}),
    "far_pine": ({"10": {"tree_pine_a": 1.0}}, {}, {"CAMSIM_VEGETATION_SHRUBS_ENABLED": "0", "CAMSIM_VEGETATION_RADIUS_M": "100"}),
}


def _lin(srgb8: np.ndarray) -> np.ndarray:
    a = srgb8.astype(np.float64) / 255.0
    return np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)


def _srgb8(lin: np.ndarray) -> list[int]:
    lin = np.clip(np.asarray(lin, float), 0.0, 1.0)
    return (np.where(lin <= 0.0031308, lin * 12.92, 1.055 * lin ** (1 / 2.4) - 0.055) * 255).round().astype(int).tolist()


def colour_ratio(on: np.ndarray, off: np.ndarray, threshold: int = DIFF_THRESHOLD) -> dict:
    """Vegetation pixels (|on - off| summed over RGB > threshold) of two HxWx3 uint8 frames of one pose: per channel,
    the imagery's mean linear colour there (off) over the render's (on / gain), gain = the on frame's exposure relative
    to off, measured on the other pixels. ratio None with fewer than MIN_PIXELS vegetation pixels."""
    veg = np.abs(on.astype(int) - off.astype(int)).sum(-1) > threshold
    n = int(veg.sum())
    lon, loff = _lin(on), _lin(off)
    rest = ~veg
    gain = (lon[rest].mean(0) / np.maximum(loff[rest].mean(0), 1e-6)) if rest.any() else np.ones(3)
    if n < MIN_PIXELS:
        return {"n": n, "gain": gain.tolist(), "ratio": None}
    render = lon[veg].mean(0) / gain
    imagery = loff[veg].mean(0)
    return {"n": n, "gain": gain.tolist(), "ratio": (imagery / np.maximum(render, 1e-6)).tolist(),
            "render_srgb": _srgb8(render), "imagery_srgb": _srgb8(imagery)}


def new_factor(old: list[float], ratio: list[float]) -> list[float]:
    """The base colour factor that moves the render onto the imagery: old x ratio, clamped to [0, 1]."""
    return [float(min(1.0, max(0.0, o * r))) for o, r in zip(old, ratio)]


def _combine(results: list[dict]) -> dict:
    """Pixel-weighted mean ratio over the poses that had an answer."""
    ok = [r for r in results if r["ratio"] is not None]
    if not ok:
        return {"n": 0, "ratio": None}
    w = np.array([r["n"] for r in ok], float)
    ratio = (np.array([r["ratio"] for r in ok]) * w[:, None]).sum(0) / w.sum()
    return {"n": int(w.sum()), "ratio": ratio.tolist(), "poses": ok}


def run(scene: Path, out: Path, names: list[str]) -> dict:
    import vegetation_check as vc
    from bench import run_bench as rb
    from bench import scenario

    poses = {"low": vc.pose(vc.RIPARIAN[0], vc.RIPARIAN[1], 120.0, yaw=vc.YAW, pitch=-12.0), "riparian": vc.pose(*vc.RIPARIAN)}

    def shoot(label: str, env: dict) -> dict[str, Path]:
        host = rb.Host(scenario.SITES["pendleton"])
        host.pose = poses["low"]
        shots = {}
        try:
            vc.launch({"CAMSIM_SCENE_DIR": str(scene), **env}, [], host)
            for k, (pname, p) in enumerate(poses.items()):
                host.pose = p
                time.sleep(25.0 if k == 0 else 12.0)
                rb.wait_terrain(60)
                shots[pname] = out / label / f"{pname}.png"
                vc.snapshot(shots[pname])
        finally:
            vc.stop(host, out / label / "CamSimTest.log")
        return shots

    off = shoot("off", {"CAMSIM_VEGETATION_ENABLED": "0"})
    report = {}
    for name in names:
        mix, density, env = CONFIGS[name]
        mix_path = out / name / "mix.json"
        mix_path.parent.mkdir(parents=True, exist_ok=True)
        mix_path.write_text(json.dumps({"format": "camsim-vegmix-1", "mix": mix, "shrub_density": density}))
        on = shoot(name, {"CAMSIM_VEGETATION_ENABLED": "1", "CAMSIM_VEGETATION_MIX_FILE": str(mix_path), **env})
        per = [colour_ratio(np.asarray(Image.open(on[p]).convert("RGB")), np.asarray(Image.open(off[p]).convert("RGB")))
               | {"pose": p} for p in poses]
        report[name] = _combine(per)
        r = report[name]
        print(f"{name:14} n={r['n']:7}  ratio (imagery/render, linear) "
              f"{'n/a' if r['ratio'] is None else [round(x, 3) for x in r['ratio']]}", flush=True)
    (out / "report.json").write_text(json.dumps(report, indent=2))
    return report


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scene", type=Path, required=True, help="absolute path of a package WITHOUT vegetation/mix.json")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--configs", default=",".join(CONFIGS), help=f"comma list of {','.join(CONFIGS)}")
    a = ap.parse_args()
    if (a.scene / "vegetation" / "mix.json").exists():
        raise SystemExit("the package has its own species mix, which overrides --configs' mix files: use one without")
    names = [n for n in a.configs.split(",") if n]
    unknown = [n for n in names if n not in CONFIGS]
    if unknown:
        raise SystemExit(f"unknown configs {unknown}")
    run(a.scene.resolve(), a.out.resolve(), names)
    return 0


if __name__ == "__main__":
    sys.exit(main())
