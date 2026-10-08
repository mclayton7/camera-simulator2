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


def test_normalise_modes_fixes_files_and_directories(tmp_path):
    import stat

    from camsim_scene.fsutil import normalise_modes

    (tmp_path / "a/b").mkdir(parents=True)
    (tmp_path / ".state").mkdir()
    for p, mode in ((tmp_path / "a/b/f.png", 0o600), (tmp_path / "top.txt", 0o666), (tmp_path / ".state/m", 0o600)):
        p.write_bytes(b"x")
        p.chmod(mode)
    for d in (tmp_path / "a", tmp_path / "a/b", tmp_path):
        d.chmod(0o700)
    normalise_modes(tmp_path, skip=(".state",))
    mode = lambda p: stat.S_IMODE(p.stat().st_mode)
    assert [mode(d) for d in (tmp_path, tmp_path / "a", tmp_path / "a/b")] == [0o755] * 3
    assert mode(tmp_path / "a/b/f.png") == mode(tmp_path / "top.txt") == 0o644
    assert mode(tmp_path / ".state/m") == 0o600  # skipped: never packed
