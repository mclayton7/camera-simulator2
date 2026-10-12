"""Species mix per land-cover code (REALISM R4, docs/scene-packages.md "Vegetation mix"): the scene file's [vegetation]
table, written to vegetation/mix.json (camsim-vegmix-1). CamSim splits each code's types into trees and shrubs by the
vegetation library's roles and normalises the weights; the tooling doesn't know the library, so it checks syntax only."""

from __future__ import annotations

import json
from pathlib import Path

from .fsutil import atomic_write
from .manifest import Manifest

FORMAT = "camsim-vegmix-1"
FILE = Path("vegetation") / "mix.json"


def mix_json(settings: dict) -> bytes:
    """The file's bytes: format + the settings, sorted keys, two-space indent, newline-terminated."""
    return (json.dumps({"format": FORMAT, **settings}, indent=2, sort_keys=True) + "\n").encode()


def write(pkg: Path, m: Manifest) -> bytes | None:
    """Writes vegetation/mix.json from the manifest's layer settings, or removes a stale one; returns the bytes."""
    path = Path(pkg) / FILE
    s = m.layers.get("vegetation")
    if not s:
        path.unlink(missing_ok=True)
        if path.parent.exists() and not any(path.parent.iterdir()):
            path.parent.rmdir()
        return None
    data = mix_json(s)
    path.parent.mkdir(parents=True, exist_ok=True)
    atomic_write(path, data)
    return data
