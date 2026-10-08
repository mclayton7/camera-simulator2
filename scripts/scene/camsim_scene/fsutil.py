"""Filesystem helpers shared by every module: atomic writes and sha256."""

from __future__ import annotations

import hashlib
import os
import stat
import tempfile
from pathlib import Path

CHUNK = 1 << 20


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(CHUNK), b""):
            h.update(chunk)
    return h.hexdigest()


def atomic_write(path: Path, data: bytes) -> None:
    """Write via a temp file in the same directory + os.replace: readers never see a partial file.
    Mode 0644 regardless of umask, so packed images don't depend on the build host."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=f".{path.name}.", suffix=".part")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.chmod(tmp, 0o644)
        os.replace(tmp, path)
    except BaseException:
        Path(tmp).unlink(missing_ok=True)
        raise


def normalise_modes(root: Path, skip: tuple[str, ...] = ()) -> None:
    """Directories 0755 and files 0644 under root (root included; top-level names in `skip` left alone), so a packed
    image doesn't depend on the build host's umask (mksquashfs keeps modes; mkdir and PIL follow the umask)."""
    root = Path(root)
    for dirpath, dirnames, filenames in os.walk(root):
        if dirpath == str(root):
            dirnames[:] = [d for d in dirnames if d not in skip]
            filenames = [f for f in filenames if f not in skip]
        _chmod(dirpath, 0o755)
        for f in filenames:
            _chmod(os.path.join(dirpath, f), 0o644)


def _chmod(path: str, mode: int) -> None:
    st = os.lstat(path)
    if not stat.S_ISLNK(st.st_mode) and stat.S_IMODE(st.st_mode) != mode:
        os.chmod(path, mode)
