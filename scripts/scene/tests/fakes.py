"""Test doubles for HTTP: no test talks to the network."""

from __future__ import annotations

import json
from pathlib import Path

import requests

FIXTURES = Path(__file__).parent / "fixtures"


def fixture_json(name: str):
    return json.loads((FIXTURES / "http" / name).read_text(encoding="utf-8"))


class FakeResponse:
    def __init__(self, status=200, body=b"", headers=None, json_data=None, fail_after=None):
        self.status_code = status
        self.body = body
        self.headers = dict(headers or {})
        if body and "Content-Length" not in self.headers:
            self.headers["Content-Length"] = str(len(body))
        self._json = json_data
        self.fail_after = fail_after

    def iter_content(self, n):
        for i in range(0, len(self.body), max(1, n)):
            if self.fail_after is not None and i >= self.fail_after:
                raise requests.ConnectionError("connection reset")
            yield self.body[i : i + n]

    def json(self):
        return self._json

    def close(self):
        pass


class FakeSession:
    """Returns queued responses (or calls a function) and records (method, url, kwargs)."""

    def __init__(self, responses):
        self.responses = responses
        self.calls = []

    def request(self, method, url, **kw):
        self.calls.append((method, url, kw))
        if callable(self.responses):
            return self.responses(method, url, kw)
        return self.responses.pop(0)


class FakeHttp:
    """Duck-types net.Http for adapters: routes keyed by URL (GET/POST JSON) or ("HEAD", url)."""

    def __init__(self, routes: dict):
        self.routes = routes
        self.calls = []

    def _route(self, key, *args):
        self.calls.append((key, *args))
        r = self.routes[key]
        return r(*args) if callable(r) else r

    def get_json(self, url, params=None):
        return self._route(url, params)

    def post_json(self, url, body):
        return self._route(url, body)

    def head(self, url):
        return self.routes.get(("HEAD", url), (404, None))
