import hashlib
import os

import pytest

from camsim_scene import fsutil


def test_atomic_write_creates_parents_and_hashes(tmp_path):
    p = tmp_path / "a" / "b" / "c.bin"
    fsutil.atomic_write(p, b"hello")
    assert p.read_bytes() == b"hello"
    assert fsutil.sha256_file(p) == hashlib.sha256(b"hello").hexdigest() == fsutil.sha256_bytes(b"hello")
    assert oct(p.stat().st_mode & 0o777) == "0o644"


def test_interrupted_atomic_write_leaves_nothing(tmp_path, monkeypatch):
    p = tmp_path / "t.bin"

    def boom(src, dst):
        raise KeyboardInterrupt

    monkeypatch.setattr(os, "replace", boom)
    with pytest.raises(KeyboardInterrupt):
        fsutil.atomic_write(p, b"x" * 1000)
    assert list(tmp_path.iterdir()) == []
