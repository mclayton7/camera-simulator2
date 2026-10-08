"""HTTP with retries and backoff: JSON APIs, HEAD, and streamed downloads (used by cache.py)."""

from __future__ import annotations

import time

import requests

from . import __version__

RETRY_STATUS = (429, 500, 502, 503, 504)


class HttpError(Exception):
    pass


def redact(url: str) -> str:
    """Drop the query string (Planetary Computer SAS tokens live there)."""
    return url.split("?", 1)[0]


class Http:
    def __init__(self, session=None, retries: int = 5, backoff_s: float = 1.0, sleep=time.sleep):
        self.session = session or requests.Session()
        if session is None:
            self.session.headers["User-Agent"] = f"camsim-scene/{__version__}"
        self.retries, self.backoff_s, self.sleep = retries, backoff_s, sleep

    def _request(self, method: str, url: str, **kw):
        last: object = None
        for attempt in range(self.retries):
            try:
                r = self.session.request(method, url, timeout=(15, 300), **kw)
                if r.status_code not in RETRY_STATUS:
                    return r
                last = f"HTTP {r.status_code}"
                r.close()
            except requests.RequestException as e:
                last = e
            if attempt + 1 < self.retries:
                self.sleep(self.backoff_s * 2**attempt)
        raise HttpError(f"{method} {redact(url)}: {last}")

    def get_json(self, url: str, params: dict | None = None):
        r = self._request("GET", url, params=params)
        if r.status_code != 200:
            raise HttpError(f"GET {redact(url)}: HTTP {r.status_code}")
        return r.json()

    def post_json(self, url: str, body: dict):
        r = self._request("POST", url, json=body)
        if r.status_code != 200:
            raise HttpError(f"POST {redact(url)}: HTTP {r.status_code}")
        return r.json()

    def head(self, url: str) -> tuple[int, int | None]:
        r = self._request("HEAD", url, allow_redirects=True)
        size = r.headers.get("Content-Length")
        return r.status_code, int(size) if size is not None else None

    def stream(self, url: str):
        return self._request("GET", url, stream=True)
