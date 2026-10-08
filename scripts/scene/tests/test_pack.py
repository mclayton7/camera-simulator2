import shutil
import subprocess

import pytest
from fake_sources import build_synthetic

from camsim_scene import pack


def test_sort_file_puts_coarse_tiles_first(tmp_path):
    for rel in [
        "manifest.json",
        "build.json",
        "terrain/0/0/0.terrain",
        "terrain/9/1/2.terrain",
        "imagery/12/0/0.jpg",
        ".state/x",
    ]:
        (tmp_path / rel).parent.mkdir(parents=True, exist_ok=True)
        (tmp_path / rel).write_bytes(b"x")
    lines = dict(line.rsplit(" ", 1) for line in pack.sort_file_text(tmp_path).splitlines())
    assert int(lines["terrain/0/0/0.terrain"]) > int(lines["terrain/9/1/2.terrain"]) > int(lines["imagery/12/0/0.jpg"])
    assert ".state/x" not in lines and "manifest.json" in lines
    assert "build.json" not in lines


def test_pack_refuses_an_unbuilt_package(tmp_path):
    with pytest.raises(pack.PackError, match="build"):
        pack.pack(tmp_path)


@pytest.mark.skipif(shutil.which("mksquashfs") is None, reason="mksquashfs not installed")
def test_pack_is_reproducible_and_excludes_state(tmp_path):
    pkg, _, _ = build_synthetic(tmp_path)
    a = pack.pack(pkg, tmp_path / "a.sqfs")
    (pkg / "build.json").write_text('{"built_at": "2099-01-01T00:00:00Z"}\n', encoding="utf-8")
    b = pack.pack(pkg, tmp_path / "b.sqfs")
    assert a.read_bytes() == b.read_bytes()
    assert (tmp_path / "a.sqfs.sha256").read_text().endswith("  a.sqfs\n")
    assert (tmp_path / "b.sqfs.build.json").read_text() == (pkg / "build.json").read_text()
    listing = subprocess.run(["unsquashfs", "-l", str(a)], capture_output=True, text=True, check=True).stdout
    assert "terrain/layer.json" in listing and ".state" not in listing and "build.json" not in listing
