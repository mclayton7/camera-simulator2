import hashlib
import json

import pytest
from fakes import FakeResponse, FakeSession

from camsim_scene.cache import Cache, CacheError
from camsim_scene.net import Http, HttpError

BODY = b"x" * 3_000_000
SHA = hashlib.sha256(BODY).hexdigest()
URL = "https://example.org/data.tif"


def make_cache(tmp_path, responses):
    session = FakeSession(responses)
    http = Http(session=session, retries=3, sleep=lambda s: None)
    return Cache(tmp_path / "cache", http=http, sleep=lambda s: None), session


def test_download_stores_blob_by_hash_and_indexes_url(tmp_path):
    cache, session = make_cache(tmp_path, [FakeResponse(body=BODY)])
    path, sha, size = cache.get(URL)
    assert sha == SHA and size == len(BODY) and path == cache.blob_path(SHA) and path.read_bytes() == BODY
    assert cache.get(URL) == (path, SHA, len(BODY))  # served from the index: no second request
    assert len(session.calls) == 1


def test_index_records_etag_when_sent(tmp_path):
    cache, _ = make_cache(tmp_path, [FakeResponse(body=BODY, headers={"ETag": '"abc123"'})])
    cache.get(URL)
    entry = json.loads(next((tmp_path / "cache" / "urls").rglob("*.json")).read_text())
    assert entry == {"url": URL, "sha256": SHA, "size": len(BODY), "etag": '"abc123"'}
    cache2, _ = make_cache(tmp_path / "b", [FakeResponse(body=BODY)])
    cache2.get(URL)
    entry2 = json.loads(next((tmp_path / "b" / "cache" / "urls").rglob("*.json")).read_text())
    assert entry2.get("etag") is None


def test_pinned_hash_already_cached_needs_no_request(tmp_path):
    cache, session = make_cache(tmp_path, [FakeResponse(body=BODY)])
    cache.get(URL)
    assert cache.get("https://mirror.example.org/other-name.tif", sha256=SHA)[1] == SHA
    assert len(session.calls) == 1


def test_hash_mismatch_fails(tmp_path):
    cache, _ = make_cache(tmp_path, [FakeResponse(body=BODY)])
    with pytest.raises(CacheError, match="manifest pins"):
        cache.get(URL, sha256="0" * 64)


def test_interrupted_download_retries_and_leaves_no_partial(tmp_path):
    cache, session = make_cache(tmp_path, [FakeResponse(body=BODY, fail_after=1 << 20), FakeResponse(body=BODY)])
    _, sha, _ = cache.get(URL)
    assert sha == SHA and len(session.calls) == 2
    assert list((tmp_path / "cache" / "tmp").iterdir()) == []


def test_truncated_body_is_a_failure(tmp_path):
    short = FakeResponse(body=BODY[:1000], headers={"Content-Length": str(len(BODY))})
    cache, _ = make_cache(tmp_path, [short, FakeResponse(body=BODY)])
    assert cache.get(URL)[1] == SHA


def test_forbidden_resigns_and_redacts(tmp_path):
    signed = []

    def sign(url, force):
        signed.append(force)
        return url + ("?sig=NEW" if force else "?sig=OLD")

    cache, session = make_cache(tmp_path, [FakeResponse(status=403), FakeResponse(body=BODY)])
    assert cache.get(URL, sign=sign)[1] == SHA
    assert signed == [False, True] and session.calls[1][1].endswith("?sig=NEW")

    cache2, _ = make_cache(tmp_path / "b", lambda m, u, kw: FakeResponse(status=403))
    with pytest.raises(CacheError) as err:
        cache2.get(URL, sign=sign)
    assert URL in str(err.value) and "sig=" not in str(err.value)


def test_file_urls_are_copied(tmp_path):
    src = tmp_path / "src.bin"
    src.write_bytes(BODY)
    cache, session = make_cache(tmp_path, [])
    path, sha, _ = cache.get(src.as_uri())
    assert sha == SHA and path.read_bytes() == BODY and session.calls == []


def test_http_retries_server_errors_then_reports(tmp_path):
    session = FakeSession([FakeResponse(status=503), FakeResponse(status=200, json_data={"ok": 1})])
    assert Http(session=session, retries=3, sleep=lambda s: None).get_json("https://api") == {"ok": 1}
    session = FakeSession(lambda m, u, kw: FakeResponse(status=503))
    with pytest.raises(HttpError, match="503"):
        Http(session=session, retries=2, sleep=lambda s: None).get_json("https://api")
