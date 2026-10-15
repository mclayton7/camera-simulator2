"""HTTP/JSON client for CamSim's /api/v1 (docs/control-api.md). Standard library only."""

from __future__ import annotations

import base64
import json
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any


class ApiError(RuntimeError):
    """A non-2xx answer: HTTP status and CamSim's error message."""

    def __init__(self, status: int, message: str):
        super().__init__(f"HTTP {status}: {message}")
        self.status = status
        self.message = message


@dataclass(frozen=True)
class Capture:
    frame_id: int
    utc: str
    timestamp_us: int
    width: int
    height: int
    sensor_mode: str
    png: bytes
    depth_png: bytes | None
    depth_far_plane_m: float
    record: dict | None  # the frame's camsim_coco.jsonl record (None without ground truth)
    settle: dict


def _clean(fields: dict[str, Any]) -> dict[str, Any]:
    return {k: v for k, v in fields.items() if v is not None}


class Client:
    def __init__(self, base: str = "http://127.0.0.1:8080", timeout: float = 30.0):
        self.base = base.rstrip("/")
        self.timeout = timeout

    def _call(self, verb: str, path: str, body: dict | None = None, timeout: float | None = None) -> dict:
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(self.base + path, data=data, method=verb,
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=timeout or self.timeout) as resp:
                raw = resp.read()
        except urllib.error.HTTPError as e:
            raw = e.read()
            try:
                message = json.loads(raw).get("error", raw.decode(errors="replace"))
            except (ValueError, AttributeError):
                message = raw.decode(errors="replace")
            raise ApiError(e.code, message) from None
        return json.loads(raw) if raw else {}

    # --- status and clock ---
    def status(self) -> dict:
        return self._call("GET", "/api/v1/status")

    def clock(self) -> dict:
        return self._call("GET", "/api/v1/clock")

    def set_clock(self, *, lockstep: bool | None = None, utc: str | None = None, rate: float | None = None) -> dict:
        return self._call("PUT", "/api/v1/clock", _clean({"lockstep": lockstep, "utc": utc, "rate": rate}))

    def step(self, seconds: float) -> dict:
        return self._call("POST", "/api/v1/clock/step", {"seconds": seconds})

    # --- camera ---
    def set_camera(self, **fields: Any) -> dict:
        """lat/lon/alt (together), heading/pitch/roll, gimbal={yaw,pitch,roll}, hfov_deg, sensor={mode,polarity,on}."""
        return self._call("PUT", "/api/v1/camera", _clean(fields))

    def get_camera(self) -> dict:
        return self._call("GET", "/api/v1/camera")

    def release_camera(self) -> None:
        self._call("DELETE", "/api/v1/camera")

    # --- entities ---
    def put_entity(self, entity_id: int, **fields: Any) -> dict:
        """type_id, lat/lon/alt, heading/pitch/roll, surface (none|ground|water), hidden, components=[{class,id,state}]."""
        return self._call("PUT", f"/api/v1/entities/{entity_id}", _clean(fields))

    def delete_entity(self, entity_id: int) -> None:
        self._call("DELETE", f"/api/v1/entities/{entity_id}")

    def clear_entities(self) -> None:
        self._call("DELETE", "/api/v1/entities")

    def entities(self) -> list[dict]:
        return self._call("GET", "/api/v1/entities")["entities"]

    # --- environment ---
    def set_environment(self, **fields: Any) -> dict:
        """atmosphere={visibility_m,...}, clouds={coverage_pct,...}, sea={beaufort,direction_deg}."""
        return self._call("PUT", "/api/v1/environment", _clean(fields))

    # --- capture ---
    def capture(self, *, settle_frames: int | None = None, timeout_s: float | None = None, depth: bool = True) -> Capture:
        body = _clean({"settle_frames": settle_frames, "timeout_s": timeout_s, "depth": depth})
        r = self._call("POST", "/api/v1/capture", body, timeout=(timeout_s or 120.0) + 30.0)
        depth_b64 = r.get("depth_png_b64")
        return Capture(
            frame_id=r["frame_id"], utc=r["utc"], timestamp_us=r["timestamp_us"], width=r["width"],
            height=r["height"], sensor_mode=r.get("sensor_mode", ""), png=base64.b64decode(r["image_png_b64"]),
            depth_png=base64.b64decode(depth_b64) if depth_b64 else None,
            depth_far_plane_m=float(r.get("depth_far_plane_m", 0.0)), record=r.get("annotation"),
            settle=r.get("settle", {}))

    def wait_ready(self, timeout_s: float = 900.0, poll_s: float = 2.0) -> None:
        """Until the API answers and the camera exists (GET /camera stops answering 503)."""
        deadline = time.monotonic() + timeout_s
        last: Exception | None = None
        while time.monotonic() < deadline:
            try:
                self.get_camera()
                return
            except (ApiError, OSError) as e:
                last = e
            time.sleep(poll_s)
        raise TimeoutError(f"CamSim control API not ready after {timeout_s:.0f} s: {last}")
