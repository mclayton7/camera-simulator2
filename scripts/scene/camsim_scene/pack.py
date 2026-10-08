"""pack: the package directory -> one SquashFS image, uncompressed (JPEG and gzip tiles don't compress further),
reproducible (owner root, all timestamps 0, coarse tiles first), plus <image>.sha256. Pinned by the build
container's squashfs-tools (>= 4.6, reproducible by default). Mounting: docs/realism/offline-hosting.md.

build.json holds build timestamps, so it is excluded from the image (like .state) to keep two builds of the
same manifest byte-identical; it is copied beside the image as <image>.build.json instead."""

from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path

from .fsutil import sha256_file
from .manifest import STATE_DIR, package_files

FLAGS = [
    "-noappend",
    "-noI",
    "-noD",
    "-noF",
    "-noX",
    "-all-root",
    "-all-time",
    "0",
    "-mkfs-time",
    "0",
    "-no-progress",
    "-quiet",
]
BUILD_JSON = "build.json"
IMAGE_METADATA = ("manifest.json", "hashes.txt")
TILE_RE = re.compile(r"^(terrain|imagery)/(\d+)/")


class PackError(Exception):
    pass


def sort_file_text(pkg: Path) -> str:
    """mksquashfs -sort list: higher priority is stored first. Metadata first, then tiles by zoom (coarse first).
    build.json and .state are not in the image, so they are not listed."""
    lines = [f"{name} 1000" for name in IMAGE_METADATA if (Path(pkg) / name).exists()]
    for rel in package_files(pkg):
        m = TILE_RE.match(rel)
        lines.append(f"{rel} {500 - int(m.group(2)) if m else 1000}")
    return "\n".join(lines) + "\n"


def pack(pkg: Path, out: Path | None = None, exe: str = "mksquashfs") -> Path:
    pkg = Path(pkg)
    if not (pkg / "hashes.txt").exists():
        raise PackError(f"{pkg}: no hashes.txt; build the package first")
    exe_path = shutil.which(exe)
    if exe_path is None:
        raise PackError("mksquashfs not found (apt install squashfs-tools, or brew install squashfs)")
    out = Path(out) if out else pkg.parent / f"{pkg.name}.sqfs"
    sort = pkg.parent / f".{pkg.name}.sort"
    sort.write_text(sort_file_text(pkg), encoding="utf-8")
    try:
        subprocess.run(
            [exe_path, str(pkg), str(out), *FLAGS, "-sort", str(sort), "-e", STATE_DIR, BUILD_JSON], check=True
        )
    finally:
        sort.unlink(missing_ok=True)
    (out.parent / f"{out.name}.sha256").write_text(f"{sha256_file(out)}  {out.name}\n", encoding="utf-8")
    if (pkg / BUILD_JSON).exists():
        shutil.copyfile(pkg / BUILD_JSON, out.parent / f"{out.name}.build.json")
    return out
