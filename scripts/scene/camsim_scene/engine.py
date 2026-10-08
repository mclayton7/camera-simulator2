"""Tile scheduler: resume markers, atomic writes, a spawn-based process pool, a per-package build lock, removal
of tiles that left the plan, and per-layer stats (tiles, bytes, wall time, peak RSS per worker)."""

from __future__ import annotations

import fcntl
import json
import multiprocessing
import os
import re
import resource
import shutil
import sys
import time
from collections.abc import Callable, Iterable
from concurrent.futures import FIRST_COMPLETED, ProcessPoolExecutor, wait
from dataclasses import asdict, dataclass
from pathlib import Path

import numpy as np

from .fsutil import atomic_write, sha256_bytes, sha256_file
from .manifest import STATE_DIR
from .tiling import LayerPlan, make_keys

TILE_RE = re.compile(r"^(?P<layer>terrain|imagery)/(?P<z>\d+)/(?P<x>\d+)/(?P<y>\d+)\.(terrain|jpg)$")


@dataclass
class TileResult:
    layer: str
    z: int
    x: int
    y: int
    skipped: bool
    size: int
    sha256: str
    seconds: float
    pid: int
    rss_mb: float


@dataclass
class LayerStats:
    tiles: int = 0
    files: int = 0  # files written to the package for this layer (= tiles for terrain and imagery)
    built: int = 0
    skipped: int = 0
    bytes: int = 0
    seconds: float = 0.0
    peak_rss_mb: float = 0.0

    def add(self, r: TileResult) -> None:
        self.tiles += 1
        self.files += 1
        self.built += not r.skipped
        self.skipped += r.skipped
        self.bytes += r.size
        self.peak_rss_mb = max(self.peak_rss_mb, r.rss_mb)

    def to_dict(self) -> dict:
        return asdict(self)


def peak_rss_mb() -> float:
    r = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    return r / 1024.0 if sys.platform.startswith("linux") else r / (1024.0 * 1024.0)


def inputs_hash(*parts: str) -> str:
    return sha256_bytes("\n".join(parts).encode())


def tile_path(pkg: Path, layer: str, z: int, x: int, y: int, ext: str) -> Path:
    return Path(pkg) / layer / str(z) / str(x) / f"{y}.{ext}"


class Markers:
    def __init__(self, pkg: Path):
        self.root = Path(pkg) / STATE_DIR

    def path(self, layer: str, z: int, x: int, y: int) -> Path:
        return self.root / layer / str(z) / str(x) / str(y)

    def read(self, layer: str, z: int, x: int, y: int) -> dict | None:
        try:
            return json.loads(self.path(layer, z, x, y).read_text())
        except (FileNotFoundError, json.JSONDecodeError):
            return None

    def write(self, layer: str, z: int, x: int, y: int, inputs: str, output: str) -> None:
        atomic_write(
            self.path(layer, z, x, y), json.dumps({"inputs": inputs, "output": output}, sort_keys=True).encode()
        )

    def output_for_file(self, relpath: str) -> str | None:
        m = TILE_RE.match(relpath)
        if not m:
            return None
        rec = self.read(m["layer"], int(m["z"]), int(m["x"]), int(m["y"]))
        return rec["output"] if rec else None


def run_tile(
    pkg: Path, markers: Markers, layer: str, ext: str, z: int, x: int, y: int, inputs: str, produce: Callable[[], bytes]
) -> TileResult:
    t0 = time.perf_counter()
    path = tile_path(pkg, layer, z, x, y, ext)
    rec = markers.read(layer, z, x, y)
    if rec and rec["inputs"] == inputs and path.exists():
        sha = sha256_file(path)
        if sha == rec["output"]:
            return TileResult(
                layer, z, x, y, True, path.stat().st_size, sha, time.perf_counter() - t0, os.getpid(), peak_rss_mb()
            )
    data = produce()
    atomic_write(path, data)
    sha = sha256_bytes(data)
    markers.write(layer, z, x, y, inputs, sha)
    return TileResult(layer, z, x, y, False, len(data), sha, time.perf_counter() - t0, os.getpid(), peak_rss_mb())


class BuildLocked(Exception):
    pass


class BuildLock:
    """Exclusive flock on <pkg>/.state/lock for the duration of a build."""

    def __init__(self, pkg: Path):
        self.path = Path(pkg) / STATE_DIR / "lock"
        self.fd: int | None = None

    def __enter__(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.fd = os.open(self.path, os.O_RDWR | os.O_CREAT, 0o644)
        try:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            os.close(self.fd)
            self.fd = None
            raise BuildLocked(f"another build is running on {self.path.parents[1]}") from None
        return self

    def __exit__(self, *exc):
        fcntl.flock(self.fd, fcntl.LOCK_UN)
        os.close(self.fd)
        self.fd = None


def run_pool(fn, phases: Iterable[Iterable], jobs: int, initializer, initargs, on_result) -> None:
    """Run fn(chunk) -> list[TileResult] over every chunk; each phase finishes before the next one starts."""
    if jobs <= 1:
        initializer(*initargs)
        for phase in phases:
            for chunk in phase:
                for r in fn(chunk):
                    on_result(r)
        return
    ctx = multiprocessing.get_context("spawn")
    with ProcessPoolExecutor(max_workers=jobs, mp_context=ctx, initializer=initializer, initargs=initargs) as ex:
        try:
            for phase in phases:
                pending = set()
                for chunk in phase:
                    pending.add(ex.submit(fn, chunk))
                    if len(pending) >= 4 * jobs:
                        done, pending = wait(pending, return_when=FIRST_COMPLETED)
                        for f in done:
                            for r in f.result():
                                on_result(r)
                for f in wait(pending).done:
                    for r in f.result():
                        on_result(r)
        except BaseException:
            ex.shutdown(wait=False, cancel_futures=True)
            raise


def _stale_in(root: Path, plan: LayerPlan, ext: str | None) -> int:
    """Remove z/x/y files (ext) or markers (ext None) under root that aren't in plan. Returns the count."""
    removed = 0
    if not root.exists():
        return 0
    suffix = f".{ext}" if ext else ""
    for zdir in list(root.iterdir()):
        if not zdir.is_dir():
            continue
        if not zdir.name.isdigit() or int(zdir.name) not in plan.tiles:
            removed += sum(1 for p in zdir.rglob("*") if p.is_file())
            shutil.rmtree(zdir)
            continue
        planned = plan.tiles[int(zdir.name)]
        for xdir in list(zdir.iterdir()):
            if not xdir.is_dir():
                xdir.unlink()
                removed += 1
                continue
            files = [p for p in xdir.iterdir() if p.is_file()]
            ys, keep = [], []
            for p in files:
                stem = (
                    p.name[: -len(suffix)] if suffix and p.name.endswith(suffix) else (p.name if not suffix else None)
                )
                ok = xdir.name.isdigit() and stem is not None and stem.isdigit()
                ys.append(int(stem) if ok else -1)
                keep.append(ok)
            if files:
                k = make_keys(np.full(len(ys), int(xdir.name) if xdir.name.isdigit() else -1), np.asarray(ys))
                keep = np.asarray(keep) & np.isin(k, planned)
                for p, kp in zip(files, keep):
                    if not kp:
                        p.unlink()
                        removed += 1
            if not any(xdir.iterdir()):
                xdir.rmdir()
    return removed


def remove_stale(pkg: Path, layer: str, ext: str, plan: LayerPlan) -> int:
    """Delete tiles (and their markers) that the current plan doesn't contain, and leftover temp files."""
    n = _stale_in(Path(pkg) / layer, plan, ext)
    _stale_in(Path(pkg) / STATE_DIR / layer, plan, None)
    return n


class Progress:
    def __init__(self, layer: str, total: int, json_lines: bool = False, stream=sys.stderr, every_s: float = 2.0):
        self.layer, self.total, self.json_lines, self.stream, self.every_s = layer, total, json_lines, stream, every_s
        self.done_n = self.skipped = 0
        self.t0 = self.last = time.monotonic()

    def update(self, r: TileResult) -> None:
        self.done_n += 1
        self.skipped += r.skipped
        now = time.monotonic()
        if now - self.last >= self.every_s:
            self.last = now
            self._emit(now)

    def done(self) -> None:
        self._emit(time.monotonic())

    def _emit(self, now: float) -> None:
        rate = self.done_n / max(now - self.t0, 1e-9)
        if self.json_lines:
            rec = {
                "layer": self.layer,
                "done": self.done_n,
                "total": self.total,
                "skipped": self.skipped,
                "tiles_per_s": round(rate, 1),
            }
            print(json.dumps(rec), file=self.stream, flush=True)
        else:
            print(
                f"{self.layer}: {self.done_n}/{self.total} ({self.skipped} skipped) {rate:.0f} tiles/s",
                file=self.stream,
                flush=True,
            )
