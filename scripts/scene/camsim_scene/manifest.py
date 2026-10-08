"""manifest.json: the deterministic description of a package (inputs, settings, datum, sources, hashes).
Nothing in it depends on time or host; build-time facts go to build.json."""

from __future__ import annotations

import json
import os
from collections.abc import Callable, Iterator
from dataclasses import asdict, dataclass, field
from pathlib import Path

from .fsutil import atomic_write, sha256_bytes, sha256_file
from .tiling import Region

SCHEMA_VERSION = 1
STATE_DIR = ".state"
NOT_HASHED = ("manifest.json", "build.json", "hashes.txt")


def canonical_json(obj) -> str:
    """Sorted keys, 2-space indent, Python's shortest round-trip float repr, no NaN."""
    return json.dumps(obj, sort_keys=True, indent=2, ensure_ascii=False, allow_nan=False) + "\n"


@dataclass
class AssetRecord:
    id: str
    url: str  # never signed
    sha256: str | None = None
    size: int | None = None
    group: str = ""  # assets of one group are merged first-valid-wins; groups are feathered against each other
    rank: int = 0  # order inside the source (0 = preferred)
    role: str = "data"  # "data" or an auxiliary role ("geoid")
    metadata: dict = field(default_factory=dict)


@dataclass
class SourceRecord:
    id: str
    adapter: str
    dataset: str
    version: str
    licence: str
    attribution: str
    options: dict
    assets: list[AssetRecord]


@dataclass
class Manifest:
    name: str
    bbox: list[float]
    seed: int
    regions: list[dict]
    tiling: dict
    layers: dict
    datum: dict
    sources: list[SourceRecord]
    licence_allow: list[str]
    tool: dict
    schema_version: int = SCHEMA_VERSION
    hashes_sha256: str | None = None

    def to_dict(self) -> dict:
        return asdict(self)

    @classmethod
    def from_dict(cls, d: dict) -> Manifest:
        if d.get("schema_version") != SCHEMA_VERSION:
            raise ValueError(f"manifest schema_version {d.get('schema_version')} != {SCHEMA_VERSION}")
        d = dict(d)
        d["sources"] = [SourceRecord(**{**s, "assets": [AssetRecord(**a) for a in s["assets"]]}) for s in d["sources"]]
        return cls(**d)

    def dumps(self) -> str:
        return canonical_json(self.to_dict())

    @classmethod
    def load(cls, path: Path) -> Manifest:
        return cls.from_dict(json.loads(Path(path).read_text(encoding="utf-8")))

    def write(self, path: Path) -> None:
        atomic_write(Path(path), self.dumps().encode("utf-8"))

    def source(self, source_id: str) -> SourceRecord:
        for s in self.sources:
            if s.id == source_id:
                return s
        raise KeyError(f"no source {source_id!r} in the manifest")

    def region_objs(self) -> list[Region]:
        return [Region.from_dict(r) for r in self.regions]

    def is_fetched(self) -> bool:
        assets_ok = all(a.sha256 for s in self.sources for a in s.assets)
        grids_ok = all(g.get("sha256") for g in self.datum.get("grids", {}).values())
        return assets_ok and grids_ok

    def layer_settings_hash(self, layer: str) -> str:
        """Everything (except asset bytes) that a tile of `layer` depends on."""
        payload = {
            "layer": self.layers[layer],
            "regions": self.regions,
            "tiling": self.tiling,
            "datum": self.datum,
            "tool": self.tool,
        }
        return sha256_bytes(canonical_json(payload).encode())


def _walk(root: Path, rel: str) -> Iterator[str]:
    # Sorting entries by name + "/" for directories yields global lexicographic order of full paths.
    entries = sorted(os.scandir(root / rel if rel else root), key=lambda e: e.name + ("/" if e.is_dir() else ""))
    for e in entries:
        path = f"{rel}/{e.name}" if rel else e.name
        if e.is_dir():
            if not rel and e.name == STATE_DIR:
                continue
            yield from _walk(root, path)
        elif not (not rel and e.name in NOT_HASHED):
            yield path


def package_files(pkg: Path) -> Iterator[str]:
    """Package-relative POSIX paths of every hashed file, in sorted order (streaming; no full list in memory)."""
    yield from _walk(Path(pkg), "")


def write_hashes(pkg: Path, known: Callable[[str], str | None] | None = None) -> str:
    """Write hashes.txt ("sha256  path" per line); `known` may supply already-verified hashes (tile markers)."""
    pkg = Path(pkg)
    lines = []
    for rel in package_files(pkg):
        h = known(rel) if known else None
        lines.append(f"{h or sha256_file(pkg / rel)}  {rel}\n")
    data = "".join(lines).encode("utf-8")
    atomic_write(pkg / "hashes.txt", data)
    return sha256_bytes(data)


def read_hashes(pkg: Path) -> dict[str, str]:
    out = {}
    for line in (Path(pkg) / "hashes.txt").read_text(encoding="utf-8").splitlines():
        h, rel = line.split("  ", 1)
        out[rel] = h
    return out
