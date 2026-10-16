"""Client against a local fake server: JSON in/out, errors as ApiError, capture decoding."""

from __future__ import annotations

import base64
import json
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

import pytest

from camsim_control.client import ApiError, Client

PNG = b"\x89PNG\r\n\x1a\nfake"


class _Handler(BaseHTTPRequestHandler):
    calls: list[tuple[str, str, dict | None]] = []

    def log_message(self, *args):  # quiet
        pass

    def _reply(self, code: int, obj: dict) -> None:
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _handle(self, verb: str) -> None:
        n = int(self.headers.get("Content-Length") or 0)
        body = json.loads(self.rfile.read(n)) if n else None
        _Handler.calls.append((verb, self.path, body))
        if self.path == "/api/v1/clock/step":
            return self._reply(409, {"error": "the clock is not in lockstep"})
        if self.path == "/api/v1/capture":
            return self._reply(200, {
                "frame_id": 12, "timestamp_us": 5, "utc": "2026-06-15T18:00:00.000Z", "width": 4, "height": 2,
                "image_png_b64": base64.b64encode(PNG).decode(), "depth_png_b64": base64.b64encode(b"d").decode(),
                "semantic_png_b64": base64.b64encode(b"s").decode(), "sensor_mode": "ir",
                "depth_far_plane_m": 5000, "annotation": {"frame_id": 12, "annotations": []},
                "settle": {"frames_waited": 30, "wall_ms": 1000}})
        if self.path == "/api/v1/status":
            return self._reply(200, {"ready": True, "sensor_mode": "eo"})
        return self._reply(200, {"ok": True, "echo": body})

    def do_GET(self):
        self._handle("GET")

    def do_PUT(self):
        self._handle("PUT")

    def do_POST(self):
        self._handle("POST")

    def do_DELETE(self):
        self._handle("DELETE")


@pytest.fixture()
def server():
    _Handler.calls = []
    httpd = HTTPServer(("127.0.0.1", 0), _Handler)
    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()
    yield f"http://127.0.0.1:{httpd.server_address[1]}"
    httpd.shutdown()


def test_put_entity_sends_json(server):
    c = Client(server)
    c.put_entity(7, type_id=2001, lat=33.0, lon=-117.0, alt=40.0, surface="ground")
    verb, path, body = _Handler.calls[-1]
    assert (verb, path) == ("PUT", "/api/v1/entities/7")
    assert body == {"type_id": 2001, "lat": 33.0, "lon": -117.0, "alt": 40.0, "surface": "ground"}


def test_none_fields_are_left_out(server):
    Client(server).set_clock(lockstep=True)
    assert _Handler.calls[-1][2] == {"lockstep": True}


def test_error_raises_api_error(server):
    with pytest.raises(ApiError) as e:
        Client(server).step(1.0)
    assert e.value.status == 409
    assert "lockstep" in e.value.message


def test_capture_decodes(server):
    cap = Client(server).capture(settle_frames=5)
    assert _Handler.calls[-1][2] == {"settle_frames": 5, "depth": True, "semantic": True}
    assert cap.frame_id == 12
    assert cap.png == PNG
    assert cap.depth_png == b"d"
    assert cap.semantic_png == b"s"
    assert cap.sensor_mode == "ir"
    assert cap.record == {"frame_id": 12, "annotations": []}
    assert cap.settle["frames_waited"] == 30


def test_delete_and_status(server):
    c = Client(server)
    c.delete_entity(3)
    assert _Handler.calls[-1][:2] == ("DELETE", "/api/v1/entities/3")
    assert c.status()["ready"] is True
