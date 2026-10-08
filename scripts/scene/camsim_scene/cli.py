"""camsim-scene: build CamSim scene packages (docs/scene-packages.md).

    camsim-scene plan PKG --config scene.toml          # or --bbox W S E N [--name N] [--profile sim]
    camsim-scene fetch PKG                              # download + hash every asset into the cache
    camsim-scene build PKG [--config scene.toml] [-j N] # plans only when manifest.json is missing (--replan forces)
    camsim-scene verify PKG [--deep [--all]]            # writes PKG.verify.json; exit 1 when a check fails
    camsim-scene pack PKG [--out FILE]                  # PKG.sqfs + .sha256 + .build.json
    camsim-scene attribution PKG                        # print the attribution text

Cache: --cache DIR, else $CAMSIM_SCENE_CACHE, else <repo>/.cache/scene.
"""

from __future__ import annotations

import argparse
import dataclasses
import logging
import os
import sys
from pathlib import Path

from .cache import Cache, CacheError, default_cache_root
from .config import ConfigError, ScenePlan, default_scene_toml, load_scene, validate_bbox
from .datum import DatumError
from .engine import BuildLocked
from .layers.terrain import TerrainError
from .licences import LicenceError, attribution_text
from .manifest import Manifest
from .net import HttpError
from .pack import PackError, pack
from .pipeline import BuildError, PlanError, build_scene, fetch_scene, plan_scene
from .verify import verify

ERRORS = (
    ConfigError,
    LicenceError,
    CacheError,
    HttpError,
    DatumError,
    BuildLocked,
    BuildError,
    PlanError,
    PackError,
    TerrainError,
    FileNotFoundError,
)


def _scene_args(p: argparse.ArgumentParser) -> None:
    p.add_argument("--config", type=Path, help="scene.toml")
    p.add_argument(
        "--bbox", type=float, nargs=4, metavar=("W", "S", "E", "N"), help="write a default scene file for this bbox"
    )
    p.add_argument("--name", help="package name for --bbox (default: the package directory name)")
    p.add_argument("--profile", default="sim", choices=["preview", "sim"])
    p.add_argument("--allow", action="append", default=[], metavar="LICENCE", help="also accept this licence id")


def _scene(a) -> ScenePlan | None:
    if a.bbox:
        validate_bbox(a.bbox)
        name = a.name or a.pkg.name
        path = a.config or Path(f"{name}.scene.toml")
        if not path.exists():
            path.write_text(default_scene_toml(name, tuple(a.bbox), a.profile), encoding="utf-8")
            print(f"wrote {path}", file=sys.stderr)
        a.config = path
    if not a.config:
        return None
    plan = load_scene(a.config)
    return dataclasses.replace(plan, allow=tuple(sorted(set(plan.allow) | set(a.allow)))) if a.allow else plan


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="camsim-scene", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--cache", type=Path, help="fetch cache directory")
    ap.add_argument("-v", "--verbose", action="store_true")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("plan", help="discover assets and write manifest.json")
    p.add_argument("pkg", type=Path)
    _scene_args(p)
    f = sub.add_parser("fetch", help="download and hash every asset")
    f.add_argument("pkg", type=Path)
    b = sub.add_parser("build", help="build (or resume) the package")
    b.add_argument("pkg", type=Path)
    _scene_args(b)
    b.add_argument("--replan", action="store_true", help="re-plan from --config/--bbox even when manifest.json exists")
    b.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 1)
    b.add_argument("--json-progress", action="store_true", help="progress as JSON lines on stderr")
    b.add_argument("--max-worker-rss-mb", type=float, default=2048.0)
    v = sub.add_parser("verify", help="check a built package")
    v.add_argument("pkg", type=Path)
    v.add_argument("--deep", action="store_true")
    v.add_argument(
        "--all", dest="all_tiles", action="store_true", help="with --deep: every terrain tile, not a 2 %% sample"
    )
    v.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 1)
    k = sub.add_parser("pack", help="pack into one SquashFS image")
    k.add_argument("pkg", type=Path)
    k.add_argument("--out", type=Path)
    t = sub.add_parser("attribution", help="print the attribution text")
    t.add_argument("pkg", type=Path)
    a = ap.parse_args(argv)
    logging.basicConfig(
        level=logging.DEBUG if a.verbose else logging.INFO, format="%(levelname)s %(message)s", stream=sys.stderr
    )
    cache = Cache(a.cache or default_cache_root())
    try:
        if a.cmd in ("plan", "build"):
            plan = _scene(a)
            if a.cmd == "plan":
                if plan is None:
                    raise ConfigError("plan needs --config or --bbox")
                plan_scene(plan, a.pkg, cache.http)
                return 0
            has_manifest = (a.pkg / "manifest.json").exists()
            if a.replan and plan is None:
                raise ConfigError("--replan needs --config or --bbox")
            if plan is not None and (a.replan or not has_manifest):
                plan_scene(plan, a.pkg, cache.http)
            elif not has_manifest:
                raise ConfigError(f"{a.pkg}: no manifest.json; pass --config or --bbox")
            info = build_scene(a.pkg, cache, a.jobs, a.json_progress, a.max_worker_rss_mb)
            for layer, s in info["layers"].items():
                print(
                    f"{layer}: {s['tiles']} tiles ({s['built']} built, {s['skipped']} skipped), "
                    f"{s['bytes'] / 1e6:.1f} MB, {s['seconds']:.0f} s, peak RSS {s['peak_rss_mb']:.0f} MB"
                )
            return 0
        if a.cmd == "fetch":
            fetch_scene(a.pkg, cache)
            return 0
        if a.cmd == "verify":
            r = verify(a.pkg, cache, a.deep, a.all_tiles, a.jobs)
            for c in r["checks"]:
                print(f"{'ok  ' if c['ok'] else 'FAIL'} {c['name']}" + (f": {c['detail']}" if c["detail"] else ""))
            return 0 if r["ok"] else 1
        if a.cmd == "pack":
            out = pack(a.pkg, a.out)
            print(out)
            print(f"{out}.sha256")
            print(f"{out}.build.json")
            return 0
        if a.cmd == "attribution":
            print(attribution_text(Manifest.load(a.pkg / "manifest.json")))
            return 0
    except ERRORS as e:
        print(f"camsim-scene: error: {e}", file=sys.stderr)
        return 2
    return 2


if __name__ == "__main__":
    sys.exit(main())
