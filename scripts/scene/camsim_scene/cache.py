"""Content-addressed fetch cache (.cache/scene by default, or $CAMSIM_SCENE_CACHE):

    blobs/sha256/ab/<sha256>       whole assets, named by their hash
    urls/<h[:2]>/<sha256(url)>.json {url, sha256, size, etag?}: what a URL last resolved to
    derived/<k[:2]>/<key><suffix>  files derived from blobs (tiled GeoTIFFs with overviews)
    tmp/                           downloads in progress (never referenced)

A download streams into tmp/, is hashed, and is renamed into blobs/ only when complete, so an interrupted
fetch leaves no cache entry. Builds use cached bytes; a pinned hash that doesn't match fails the build."""

from __future__ import annotations

import hashlib
import json
import os
import time
import urllib.parse
import uuid
from collections.abc import Callable
from pathlib import Path

import requests

from .fsutil import CHUNK, atomic_write
from .net import Http, HttpError, redact

Signer = Callable[[str, bool], str]
REPO = Path(__file__).resolve().parents[3]


class CacheError(Exception):
    pass


class _Forbidden(Exception):
    pass


class _Truncated(Exception):
    pass


def default_cache_root() -> Path:
    env = os.environ.get("CAMSIM_SCENE_CACHE")
    return Path(env) if env else REPO / ".cache" / "scene"


class Cache:
    def __init__(self, root: Path, http: Http | None = None, attempts: int = 4, sleep=time.sleep):
        self.root = Path(root)
        self.http = http or Http()
        self.attempts, self.sleep = attempts, sleep
        self._etag: str | None = None

    def blob_path(self, sha256: str) -> Path:
        return self.root / "blobs" / "sha256" / sha256[:2] / sha256

    def derived_path(self, key: str, suffix: str) -> Path:
        return self.root / "derived" / key[:2] / f"{key}{suffix}"

    def _index_path(self, url: str) -> Path:
        h = hashlib.sha256(url.encode()).hexdigest()
        return self.root / "urls" / h[:2] / f"{h}.json"

    def get(self, url: str, sha256: str | None = None, sign: Signer | None = None) -> tuple[Path, str, int]:
        """Local path, sha256 and size of `url`'s bytes, downloading only when needed."""
        if sha256 and self.blob_path(sha256).exists():
            p = self.blob_path(sha256)
            return p, sha256, p.stat().st_size
        idx = self._index_path(url)
        if idx.exists():
            known = json.loads(idx.read_text())
            p = self.blob_path(known["sha256"])
            if p.exists() and sha256 in (None, known["sha256"]):
                return p, known["sha256"], known["size"]
        path, got, size = self._download(url, sign)
        if sha256 and got != sha256:
            raise CacheError(f"{redact(url)}: downloaded sha256 {got} but the manifest pins {sha256}")
        return path, got, size

    def _download(self, url: str, sign: Signer | None) -> tuple[Path, str, int]:
        tmpdir = self.root / "tmp"
        tmpdir.mkdir(parents=True, exist_ok=True)
        last: object = None
        for attempt in range(self.attempts):
            href = sign(url, attempt > 0) if sign else url
            tmp = tmpdir / f"{uuid.uuid4().hex}.part"
            self._etag = None
            try:
                h = hashlib.sha256()
                size = 0
                with open(tmp, "wb") as f:
                    for chunk in self._chunks(href):
                        f.write(chunk)
                        h.update(chunk)
                        size += len(chunk)
                sha = h.hexdigest()
                dst = self.blob_path(sha)
                dst.parent.mkdir(parents=True, exist_ok=True)
                os.replace(tmp, dst)
                entry: dict = {"url": url, "sha256": sha, "size": size}
                if self._etag:
                    entry["etag"] = self._etag
                atomic_write(self._index_path(url), json.dumps(entry).encode())
                return dst, sha, size
            except (HttpError, OSError, requests.RequestException, _Forbidden, _Truncated) as e:
                last = e
                if attempt + 1 < self.attempts:
                    self.sleep(2**attempt)
            finally:
                tmp.unlink(missing_ok=True)
        raise CacheError(f"{redact(url)}: download failed after {self.attempts} attempts: {last}")

    def _chunks(self, href: str):
        if href.startswith("file://"):
            with open(urllib.parse.unquote(urllib.parse.urlparse(href).path), "rb") as f:
                yield from iter(lambda: f.read(CHUNK), b"")
            return
        r = self.http.stream(href)
        try:
            if r.status_code in (401, 403):
                raise _Forbidden(f"HTTP {r.status_code}")
            if r.status_code != 200:
                raise HttpError(f"HTTP {r.status_code}")
            self._etag = r.headers.get("ETag")
            expected = r.headers.get("Content-Length")
            n = 0
            for chunk in r.iter_content(CHUNK):
                n += len(chunk)
                yield chunk
            if expected is not None and n != int(expected):
                raise _Truncated(f"got {n} of {expected} bytes")
        finally:
            r.close()
