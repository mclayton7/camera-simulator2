"""MAVLink Camera Protocol component (HITL.md "Camera component").

PX4 only routes camera traffic, so this implements the whole camera side with
the real camera's identity: heartbeat, CAMERA_INFORMATION and the other
requested messages (REQUEST_MESSAGE and the superseded per-message commands),
an http:// definition file with PARAM_EXT_*, zoom (-> View Definition HFOV),
EO/IR and polarity (-> Sensor Control), image capture from CamSim's
``GET /snapshot``, video capture by recording CamSim's raw UDP, and
CAMERA_FOV_STATUS from the Sensor Extended Response frame centre.

Every command is ACKed first (QGC 5.1 times out after 1200 ms and refuses a
second command with the same ID while one is pending), then worked on.
"""

from __future__ import annotations

import bisect
import collections
import http.server
import logging
import math
import queue
import socket
import struct
import threading
import time
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Protocol

from . import frames
from .config import CameraConfig, CameraParam
from .mavlink_link import Component, encode_str, mavlink, payload_bytes, u8_array, version_u32
from .quat import Quat

log = logging.getLogger(__name__)

INT32_MAX = 2**31 - 1
INT32_MIN = -(2**31)

MAV_TYPE_CAMERA = 30
MAV_AUTOPILOT_INVALID = 8
MAV_STATE_ACTIVE = 4

MAV_RESULT_ACCEPTED = 0
MAV_RESULT_TEMPORARILY_REJECTED = 1
MAV_RESULT_DENIED = 2
MAV_RESULT_UNSUPPORTED = 3
MAV_RESULT_FAILED = 4

# MAV_CMD
CMD_DO_DIGICAM_CONTROL = 203
CMD_SET_MESSAGE_INTERVAL = 511
CMD_REQUEST_MESSAGE = 512
CMD_REQUEST_CAMERA_INFORMATION = 521
CMD_REQUEST_CAMERA_SETTINGS = 522
CMD_REQUEST_STORAGE_INFORMATION = 525
CMD_STORAGE_FORMAT = 526
CMD_REQUEST_CAMERA_CAPTURE_STATUS = 527
CMD_RESET_CAMERA_SETTINGS = 529
CMD_SET_CAMERA_MODE = 530
CMD_SET_CAMERA_ZOOM = 531
CMD_SET_CAMERA_FOCUS = 532
CMD_SET_CAMERA_SOURCE = 534
CMD_IMAGE_START_CAPTURE = 2000
CMD_IMAGE_STOP_CAPTURE = 2001
CMD_CAMERA_TRACK_POINT = 2004
CMD_CAMERA_TRACK_RECTANGLE = 2005
CMD_CAMERA_STOP_TRACKING = 2010
CMD_VIDEO_START_CAPTURE = 2500
CMD_VIDEO_STOP_CAPTURE = 2501
CMD_VIDEO_START_STREAMING = 2502
CMD_VIDEO_STOP_STREAMING = 2503
CMD_REQUEST_VIDEO_STREAM_INFORMATION = 2504
CMD_REQUEST_VIDEO_STREAM_STATUS = 2505

# Message IDs
MSG_CAMERA_INFORMATION = 259
MSG_CAMERA_SETTINGS = 260
MSG_STORAGE_INFORMATION = 261
MSG_CAMERA_CAPTURE_STATUS = 262
MSG_CAMERA_IMAGE_CAPTURED = 263
MSG_VIDEO_STREAM_INFORMATION = 269
MSG_VIDEO_STREAM_STATUS = 270
MSG_CAMERA_FOV_STATUS = 271

SUPERSEDED = {
    CMD_REQUEST_CAMERA_INFORMATION: MSG_CAMERA_INFORMATION,
    CMD_REQUEST_CAMERA_SETTINGS: MSG_CAMERA_SETTINGS,
    CMD_REQUEST_STORAGE_INFORMATION: MSG_STORAGE_INFORMATION,
    CMD_REQUEST_CAMERA_CAPTURE_STATUS: MSG_CAMERA_CAPTURE_STATUS,
    CMD_REQUEST_VIDEO_STREAM_INFORMATION: MSG_VIDEO_STREAM_INFORMATION,
    CMD_REQUEST_VIDEO_STREAM_STATUS: MSG_VIDEO_STREAM_STATUS,
}

CAMERA_COMMANDS = {
    CMD_DO_DIGICAM_CONTROL,
    CMD_STORAGE_FORMAT,
    CMD_RESET_CAMERA_SETTINGS,
    CMD_SET_CAMERA_MODE,
    CMD_SET_CAMERA_ZOOM,
    CMD_SET_CAMERA_FOCUS,
    CMD_SET_CAMERA_SOURCE,
    CMD_IMAGE_START_CAPTURE,
    CMD_IMAGE_STOP_CAPTURE,
    CMD_CAMERA_TRACK_POINT,
    CMD_CAMERA_TRACK_RECTANGLE,
    CMD_CAMERA_STOP_TRACKING,
    CMD_VIDEO_START_CAPTURE,
    CMD_VIDEO_STOP_CAPTURE,
    CMD_VIDEO_START_STREAMING,
    CMD_VIDEO_STOP_STREAMING,
}

ZOOM_TYPE_STEP = 0
ZOOM_TYPE_CONTINUOUS = 1
ZOOM_TYPE_RANGE = 2
ZOOM_TYPE_FOCAL_LENGTH = 3
ZOOM_TYPE_HORIZONTAL_FOV = 4

CAMERA_SOURCE_RGB = 1
CAMERA_SOURCE_IR = 2

PARAM_EXT_TYPES = {
    "uint8": (1, "<B"),
    "int8": (2, "<b"),
    "uint16": (3, "<H"),
    "int16": (4, "<h"),
    "uint32": (5, "<I"),
    "int32": (6, "<i"),
    "uint64": (7, "<Q"),
    "int64": (8, "<q"),
    "real32": (9, "<f"),
    "real64": (10, "<d"),
    "custom": (11, None),
}
PARAM_ACK_ACCEPTED = 0
PARAM_ACK_VALUE_UNSUPPORTED = 1
PARAM_ACK_FAILED = 2


# ---------------------------------------------------------------------------
# Pose hand-off from the CIGI host
# ---------------------------------------------------------------------------


@dataclass
class CameraPose:
    """The camera pose the CIGI host sent for the latest frame."""

    t_ns: int
    lat: float
    lon: float
    alt_msl: float
    alt_ell: float
    q_nc: Quat
    agl: float
    utc: float  # Unix seconds


@dataclass
class FrameCentre:
    lat: float
    lon: float
    alt_ell: float
    frame: int
    rx_ns: int


class HostView(Protocol):
    def latest_pose(self) -> CameraPose | None: ...
    def latest_frame_centre(self) -> FrameCentre | None: ...
    def undulation(self, lat: float, lon: float) -> float: ...


# ---------------------------------------------------------------------------
# Zoom model
# ---------------------------------------------------------------------------


class ZoomModel:
    """Zoom % (0-100) <-> focal length <-> HFOV.

    model = "focal_length": f = f_min..f_max (linear or log in zoom %), HFOV =
    2 atan(sensor_width / 2f). model = "table": [[zoom_pct, hfov_deg], ...],
    interpolated (use the real lens curve here)."""

    def __init__(self, cfg):
        self.cfg = cfg
        self.f_min, self.f_max = cfg.focal_length_mm
        self.w = cfg.sensor_width_mm
        self.table = sorted((float(a), float(b)) for a, b in cfg.hfov_table) if cfg.model == "table" else []
        if cfg.model == "table" and len(self.table) < 2:
            raise ValueError("camera.zoom.hfov_table needs at least two [zoom_pct, hfov_deg] rows")

    def focal(self, pct: float) -> float:
        u = max(0.0, min(100.0, pct)) / 100.0
        if self.table:
            return self.w / (2.0 * math.tan(math.radians(self.hfov(pct)) / 2.0))
        if self.cfg.curve == "log":
            return self.f_min * (self.f_max / self.f_min) ** u
        return self.f_min + (self.f_max - self.f_min) * u

    def hfov(self, pct: float) -> float:
        pct = max(0.0, min(100.0, pct))
        if self.table:
            xs = [r[0] for r in self.table]
            i = bisect.bisect_left(xs, pct)
            if i <= 0:
                return self.table[0][1]
            if i >= len(xs):
                return self.table[-1][1]
            (x0, y0), (x1, y1) = self.table[i - 1], self.table[i]
            return y0 + (y1 - y0) * (pct - x0) / max(1e-9, x1 - x0)
        return math.degrees(2.0 * math.atan(self.w / (2.0 * self.focal(pct))))

    def pct_for_hfov(self, hfov: float) -> float:
        lo, hi = 0.0, 100.0  # HFOV decreases with zoom
        for _ in range(50):
            mid = (lo + hi) / 2
            if self.hfov(mid) > hfov:
                lo = mid
            else:
                hi = mid
        return (lo + hi) / 2

    def pct_for_focal(self, f: float) -> float:
        return self.pct_for_hfov(math.degrees(2.0 * math.atan(self.w / (2.0 * max(1e-6, f)))))


# ---------------------------------------------------------------------------
# PARAM_EXT store
# ---------------------------------------------------------------------------


class ParamStore:
    def __init__(self, params: list[CameraParam]):
        self.params = list(params)
        for p in self.params:
            if p.type not in PARAM_EXT_TYPES:
                raise ValueError(f"camera param {p.name}: unknown type {p.type}")
            if len(p.name.encode()) > 16:
                raise ValueError(f"camera param {p.name}: id longer than 16 bytes")

    def index(self, name: str) -> int:
        for i, p in enumerate(self.params):
            if p.name == name:
                return i
        return -1

    def by_role(self, role: str) -> CameraParam | None:
        return next((p for p in self.params if p.role == role), None)

    @staticmethod
    def encode(p: CameraParam) -> bytes:
        tid, fmt = PARAM_EXT_TYPES[p.type]
        if fmt is None:
            raw = str(p.value).encode()[:128]
        else:
            v = float(p.value) if fmt in ("<f", "<d") else int(p.value)
            raw = struct.pack(fmt, v)
        return raw.ljust(128, b"\0")

    @staticmethod
    def decode(type_name: str, raw: bytes):
        tid, fmt = PARAM_EXT_TYPES[type_name]
        if fmt is None:
            return raw.rstrip(b"\0").decode(errors="replace")
        return struct.unpack_from(fmt, raw.ljust(8, b"\0"), 0)[0]

    def type_id(self, p: CameraParam) -> int:
        return PARAM_EXT_TYPES[p.type][0]


# ---------------------------------------------------------------------------
# Video recorder: CamSim's raw MPEG-TS over UDP to a file (never via ffmpeg)
# ---------------------------------------------------------------------------


class VideoRecorder(threading.Thread):
    def __init__(self, source: str, interface: str, path: Path):
        super().__init__(name="video-rec", daemon=True)
        addr = source.split("://", 1)[-1].lstrip("@")
        host, _, port = addr.rpartition(":")
        self.group, self.port = host, int(port)
        self.interface = interface
        self.path = path
        self._halt = threading.Event()
        self.bytes = 0
        self.started = time.monotonic()
        first = int(self.group.split(".")[0]) if self.group[:1].isdigit() else 0
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16 * 1024 * 1024)
        except OSError:
            pass
        if 224 <= first <= 239:
            self.sock.bind(("", self.port))
            mreq = socket.inet_aton(self.group) + socket.inet_aton(self.interface)
            self.sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
        else:
            self.sock.bind((self.group or "0.0.0.0", self.port))
        self.sock.settimeout(0.2)

    def stop(self) -> None:
        self._halt.set()
        self.join(timeout=2.0)

    def run(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self.path.open("wb") as f:
            while not self._halt.is_set():
                try:
                    data = self.sock.recv(65535)
                except (TimeoutError, socket.timeout):
                    continue
                except OSError:
                    break
                f.write(data)
                self.bytes += len(data)
        self.sock.close()
        log.info("camera: recorded %d bytes to %s", self.bytes, self.path)


# ---------------------------------------------------------------------------
# HTTP: definition file + captured images
# ---------------------------------------------------------------------------


class _Handler(http.server.SimpleHTTPRequestHandler):
    routes: dict[str, Path] = {}
    capture_dir: Path | None = None

    def log_message(self, fmt, *args):  # quiet
        log.debug("http: " + fmt, *args)

    def do_GET(self):  # noqa: N802
        path = self.path.split("?", 1)[0]
        f = self.routes.get(path)
        if f is None and path.startswith("/captures/") and self.capture_dir is not None:
            name = Path(path).name
            cand = self.capture_dir / name
            if cand.is_file():
                f = cand
        if f is None or not f.is_file():
            self.send_error(404)
            return
        data = f.read_bytes()
        ctype = "application/xml" if f.suffix == ".xml" else ("image/png" if f.suffix == ".png" else "application/octet-stream")
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


class CameraHttpServer:
    def __init__(self, port: int, definition: Path | None, capture_dir: Path):
        handler = type("CamHandler", (_Handler,), {"routes": {}, "capture_dir": capture_dir})
        if definition is not None:
            handler.routes["/" + definition.name] = definition
        self.server = http.server.ThreadingHTTPServer(("0.0.0.0", port), handler)
        self.thread = threading.Thread(target=self.server.serve_forever, name="camera-http", daemon=True)

    @property
    def port(self) -> int:
        return self.server.server_address[1]

    def start(self) -> None:
        self.thread.start()

    def stop(self) -> None:
        self.server.shutdown()
        self.server.server_close()


# ---------------------------------------------------------------------------
# The component
# ---------------------------------------------------------------------------


@dataclass
class Optics:
    hfov: float
    vfov: float
    sensor_id: int  # 0 EO, 1 IR
    polarity: int  # 0 white-hot, 1 black-hot
    version: int


class CameraComponent(Component):
    def __init__(
        self,
        cfg: CameraConfig,
        sysid: int,
        host: HostView | None = None,
        base_dir: Path = Path("."),
        fetch: Callable[[str, float], bytes] | None = None,
    ):
        super().__init__(sysid, cfg.compid)
        self.cfg = cfg
        self.host = host
        self.zoom = ZoomModel(cfg.zoom)
        self.params = ParamStore(cfg.params)
        self._lock = threading.RLock()
        self.zoom_pct = cfg.zoom.initial_pct
        self.zoom_speed = 0.0  # -1, 0, +1 (continuous zoom)
        self.focus = float("nan")
        self.mode_id = 0
        self.sensor_id = 1 if cfg.initial_sensor.lower() == "ir" else 0
        self.polarity = 1 if cfg.initial_polarity.lower() == "black_hot" else 0
        self._optics_version = 0
        self.base_dir = base_dir
        self.capture_dir = self._resolve(cfg.capture.dir)
        self.video_dir = self._resolve(cfg.video.dir)
        self.definition = self._resolve(cfg.definition_file) if cfg.definition_file else None
        self.fetch = fetch or self._http_fetch
        self.image_index = 0
        self.image_count = 0
        self.captured: dict[int, object] = {}
        self._seen_seq: collections.deque[tuple[str, int]] = collections.deque(maxlen=64)
        self._last_capture = (None, 0.0)  # (source, monotonic)
        self._capture_q: queue.Queue = queue.Queue()
        self._capturing = 0
        self._interval: tuple[float, int, float] | None = None  # (interval s, remaining, next time)
        self.recorder: VideoRecorder | None = None
        self.msg_intervals: dict[int, float] = {}
        if cfg.fov_status_rate_hz > 0:
            self.msg_intervals[MSG_CAMERA_FOV_STATUS] = 1.0 / cfg.fov_status_rate_hz
        self._next_msg: dict[int, float] = {}
        self._stop = threading.Event()
        self._threads: list[threading.Thread] = []
        self.http: CameraHttpServer | None = None
        self.acks: list[tuple[int, int]] = []  # (command, result) for tests/diagnostics
        self._sync_role_params()

    def _resolve(self, p: str) -> Path:
        pp = Path(p).expanduser()
        return pp if pp.is_absolute() else self.base_dir / pp

    # -- lifecycle ---------------------------------------------------------
    def start(self, http: bool = True) -> None:
        if http:
            self.http = CameraHttpServer(self.cfg.http_port, self.definition, self.capture_dir)
            self.http.start()
            log.info("camera: http://%s:%d/ serving %s", self.cfg.advertise_host, self.http.port, self.definition or "(no definition file)")
        for target, name in ((self._status_loop, "camera-status"), (self._capture_loop, "camera-capture")):
            t = threading.Thread(target=target, name=name, daemon=True)
            t.start()
            self._threads.append(t)

    def stop(self) -> None:
        self._stop.set()
        self._capture_q.put(None)
        for t in self._threads:
            t.join(timeout=2.0)
        if self.recorder is not None:
            self.recorder.stop()
            self.recorder = None
        if self.http is not None:
            self.http.stop()

    # -- optics hand-off to the CIGI host ------------------------------------
    def optics(self) -> Optics:
        with self._lock:
            hfov = self.zoom.hfov(self.zoom_pct)
            w, h = self.cfg.resolution
            vfov = math.degrees(2 * math.atan(math.tan(math.radians(hfov) / 2) * h / w))
            return Optics(hfov, vfov, self.sensor_id, self.polarity, self._optics_version)

    def _optics_changed(self) -> None:
        self._optics_version += 1

    # -- definition-file parameter roles -------------------------------------
    def _sync_role_params(self) -> None:
        for role, val in (("source", "ir" if self.sensor_id else "eo"), ("polarity", "black_hot" if self.polarity else "white_hot")):
            p = self.params.by_role(role)
            if p is not None and val in p.role_values:
                p.value = p.role_values[val]
        p = self.params.by_role("mode")
        if p is not None:
            p.value = self.mode_id

    def _apply_role(self, p: CameraParam) -> None:
        rev = {float(v): k for k, v in p.role_values.items()}
        key = rev.get(float(p.value)) if not isinstance(p.value, str) else None
        if p.role == "source" and key in ("eo", "ir"):
            self.sensor_id = 1 if key == "ir" else 0
            self._optics_changed()
        elif p.role == "polarity" and key in ("white_hot", "black_hot"):
            self.polarity = 1 if key == "black_hot" else 0
            self._optics_changed()
        elif p.role == "mode":
            self.mode_id = int(p.value)

    # -- messages ----------------------------------------------------------
    def send_heartbeat(self) -> None:
        self.send(mavlink.MAVLink_heartbeat_message(MAV_TYPE_CAMERA, MAV_AUTOPILOT_INVALID, 0, 0, MAV_STATE_ACTIVE, 3))

    def definition_uri(self) -> str:
        if self.definition is None:
            return ""
        port = self.http.port if self.http is not None else self.cfg.http_port
        return f"http://{self.cfg.advertise_host}:{port}/{self.definition.name}"

    def camera_information_msg(self):
        c = self.cfg
        with self._lock:
            f = self.zoom.focal(self.zoom_pct)
        return mavlink.MAVLink_camera_information_message(
            self.time_boot_ms(),
            u8_array(c.vendor_name),
            u8_array(c.model_name),
            version_u32(c.firmware_version),
            f,
            c.sensor_size_mm[0],
            c.sensor_size_mm[1],
            c.resolution[0],
            c.resolution[1],
            c.lens_id,
            c.flags,
            c.cam_definition_version,
            encode_str(self.definition_uri(), 140),
            c.gimbal_device_id,
            0,
        )

    def camera_settings_msg(self):
        with self._lock:
            return mavlink.MAVLink_camera_settings_message(
                self.time_boot_ms(), self.mode_id, float(self.zoom_pct), float(self.focus), 0
            )

    def _used_mib(self) -> float:
        tot = 0
        for d in (self.capture_dir, self.video_dir):
            if d.is_dir():
                tot += sum(f.stat().st_size for f in d.iterdir() if f.is_file())
        return tot / (1024 * 1024)

    def storage_information_msg(self):
        used = self._used_mib()
        total = self.cfg.storage_total_mib
        return mavlink.MAVLink_storage_information_message(
            self.time_boot_ms(), 1, 1, 2, total, used, max(0.0, total - used), float("nan"), float("nan"),
            0, encode_str("CamSim", 32), 1 | 2 | 4,
        )

    def capture_status_msg(self):
        with self._lock:
            interval = self._interval[0] if self._interval else 0.0
            if self._interval:
                image_status = 3 if self._capturing else 2
            else:
                image_status = 1 if self._capturing else 0
            rec = self.recorder
        video = 1 if rec is not None else 0
        rec_ms = int((time.monotonic() - rec.started) * 1000) if rec is not None else 0
        avail = max(0.0, self.cfg.storage_total_mib - self._used_mib())
        return mavlink.MAVLink_camera_capture_status_message(
            self.time_boot_ms(), image_status, video, interval, rec_ms, avail, self.image_count, 0
        )

    def video_stream_information_msg(self):
        w, h = self.cfg.resolution
        hfov = int(round(self.optics().hfov))
        return mavlink.MAVLink_video_stream_information_message(
            1, 1, 3, 1, 30.0, w, h, 0, 0, hfov, encode_str("CamSim", 32), encode_str(self.cfg.video_stream_uri, 160), 1, 0
        )

    def video_stream_status_msg(self):
        w, h = self.cfg.resolution
        return mavlink.MAVLink_video_stream_status_message(1, 1, 30.0, w, h, 0, 0, int(round(self.optics().hfov)), 0)

    def fov_status_msg(self):
        o = self.optics()
        pose = self.host.latest_pose() if self.host else None
        fc = self.host.latest_frame_centre() if self.host else None
        q = [1.0, 0.0, 0.0, 0.0]
        lat_c = lon_c = alt_c = INT32_MAX
        lat_i = lon_i = alt_i = INT32_MAX
        if pose is not None:
            lat_c, lon_c = int(round(pose.lat * 1e7)), int(round(pose.lon * 1e7))
            alt_c = int(round(pose.alt_msl * 1000))
            q = pose.q_nc.canonical().as_list()
            if frames.boresight_above_horizon(pose.q_nc):
                lat_i = lon_i = alt_i = INT32_MIN  # at infinity: no ground intersection
            elif fc is not None and math.isfinite(fc.lat) and math.isfinite(fc.lon):
                lat_i, lon_i = int(round(fc.lat * 1e7)), int(round(fc.lon * 1e7))
                n = self.host.undulation(fc.lat, fc.lon) if self.host else 0.0
                alt_i = int(round((fc.alt_ell - n) * 1000))
        return mavlink.MAVLink_camera_fov_status_message(
            self.time_boot_ms(), lat_c, lon_c, alt_c, lat_i, lon_i, alt_i, q, o.hfov, o.vfov, 0
        )

    def _message(self, mid: int, param2: float = 0.0):
        if mid == MSG_CAMERA_INFORMATION:
            return self.camera_information_msg()
        if mid == MSG_CAMERA_SETTINGS:
            return self.camera_settings_msg()
        if mid == MSG_STORAGE_INFORMATION:
            return self.storage_information_msg()
        if mid == MSG_CAMERA_CAPTURE_STATUS:
            return self.capture_status_msg()
        if mid == MSG_CAMERA_FOV_STATUS:
            return self.fov_status_msg()
        if mid == MSG_CAMERA_IMAGE_CAPTURED:
            return self.captured.get(int(param2))
        if mid == MSG_VIDEO_STREAM_INFORMATION and self.cfg.video_stream_uri:
            return self.video_stream_information_msg()
        if mid == MSG_VIDEO_STREAM_STATUS and self.cfg.video_stream_uri:
            return self.video_stream_status_msg()
        return None

    SERVED = {
        MSG_CAMERA_INFORMATION,
        MSG_CAMERA_SETTINGS,
        MSG_STORAGE_INFORMATION,
        MSG_CAMERA_CAPTURE_STATUS,
        MSG_CAMERA_IMAGE_CAPTURED,
        MSG_CAMERA_FOV_STATUS,
        MSG_VIDEO_STREAM_INFORMATION,
        MSG_VIDEO_STREAM_STATUS,
    }

    # -- command handling --------------------------------------------------
    def _ack(self, msg, command: int, result: int, progress: int = 0) -> None:
        self.acks.append((command, result))
        self.send(
            mavlink.MAVLink_command_ack_message(
                command, result, progress, 0, msg.get_srcSystem(), msg.get_srcComponent()
            )
        )

    def handle(self, msg) -> None:
        t = msg.get_type()
        if t in ("COMMAND_LONG", "COMMAND_INT"):
            self._on_command(msg, t == "COMMAND_INT")
        elif t == "PARAM_EXT_REQUEST_LIST":
            self._param_list()
        elif t == "PARAM_EXT_REQUEST_READ":
            self._param_read(msg)
        elif t == "PARAM_EXT_SET":
            self._param_set(msg)
        elif t == "CAMERA_TRIGGER" and self.cfg.capture.capture_on_camera_trigger:
            self._request_capture("trigger", int(msg.seq))

    def _params(self, msg, is_int: bool) -> list[float]:
        if is_int:
            return [msg.param1, msg.param2, msg.param3, msg.param4, float(msg.x), float(msg.y), msg.z]
        return [msg.param1, msg.param2, msg.param3, msg.param4, msg.param5, msg.param6, msg.param7]

    def _on_command(self, msg, is_int: bool) -> None:
        cmd = msg.command
        p = self._params(msg, is_int)
        targeted = getattr(msg, "target_component", 0) == self.compid
        if cmd == CMD_REQUEST_MESSAGE or cmd in SUPERSEDED:
            mid = int(p[0]) if cmd == CMD_REQUEST_MESSAGE else SUPERSEDED[cmd]
            if mid not in self.SERVED:
                if targeted:
                    self._ack(msg, cmd, MAV_RESULT_UNSUPPORTED)
                return
            out = self._message(mid, p[1])
            if out is None:
                self._ack(msg, cmd, MAV_RESULT_UNSUPPORTED if mid != MSG_CAMERA_IMAGE_CAPTURED else MAV_RESULT_FAILED)
                return
            self._ack(msg, cmd, MAV_RESULT_ACCEPTED)
            self.send(out)
            return
        if cmd == CMD_SET_MESSAGE_INTERVAL:
            mid = int(p[0])
            if mid not in self.SERVED:
                if targeted:
                    self._ack(msg, cmd, MAV_RESULT_UNSUPPORTED)
                return
            us = p[1]
            with self._lock:
                if us < 0:
                    self.msg_intervals.pop(mid, None)
                elif us == 0:
                    if mid == MSG_CAMERA_FOV_STATUS and self.cfg.fov_status_rate_hz > 0:
                        self.msg_intervals[mid] = 1.0 / self.cfg.fov_status_rate_hz
                    else:
                        self.msg_intervals.pop(mid, None)
                else:
                    self.msg_intervals[mid] = max(0.02, us / 1e6)
            self._ack(msg, cmd, MAV_RESULT_ACCEPTED)
            return
        if cmd not in CAMERA_COMMANDS:
            if targeted:
                self._ack(msg, cmd, MAV_RESULT_UNSUPPORTED)
            return
        handler = {
            CMD_SET_CAMERA_ZOOM: self._cmd_zoom,
            CMD_SET_CAMERA_MODE: self._cmd_mode,
            CMD_SET_CAMERA_FOCUS: self._cmd_focus,
            CMD_SET_CAMERA_SOURCE: self._cmd_source,
            CMD_IMAGE_START_CAPTURE: self._cmd_image_start,
            CMD_IMAGE_STOP_CAPTURE: self._cmd_image_stop,
            CMD_DO_DIGICAM_CONTROL: self._cmd_digicam,
            CMD_VIDEO_START_CAPTURE: self._cmd_video_start,
            CMD_VIDEO_STOP_CAPTURE: self._cmd_video_stop,
            CMD_VIDEO_START_STREAMING: lambda m, c, p: (MAV_RESULT_ACCEPTED, None),
            CMD_VIDEO_STOP_STREAMING: lambda m, c, p: (MAV_RESULT_ACCEPTED, None),
            CMD_STORAGE_FORMAT: lambda m, c, p: (MAV_RESULT_ACCEPTED, None),
            CMD_RESET_CAMERA_SETTINGS: self._cmd_reset,
        }.get(cmd)
        if handler is None:  # tracking: only if the real camera tracks
            self._ack(msg, cmd, MAV_RESULT_UNSUPPORTED)
            return
        result, work = handler(msg, cmd, p)
        self._ack(msg, cmd, result)  # ACK first ...
        if work is not None:
            work()  # ... then do the work

    def _settings_after(self):
        return lambda: self.send(self.camera_settings_msg())

    def _cmd_zoom(self, msg, cmd, p):
        ztype, value = int(p[0]), p[1]
        z = self.cfg.zoom
        with self._lock:
            if ztype == ZOOM_TYPE_STEP:
                self.zoom_pct = max(0.0, min(100.0, self.zoom_pct + value * z.step_pct))
                self.zoom_speed = 0.0
            elif ztype == ZOOM_TYPE_CONTINUOUS:
                self.zoom_speed = max(-1.0, min(1.0, value))
                if self.zoom_speed != 0.0:
                    return MAV_RESULT_ACCEPTED, None  # settings follow when it stops
            elif ztype == ZOOM_TYPE_RANGE:
                self.zoom_pct = max(0.0, min(100.0, value))
                self.zoom_speed = 0.0
            elif ztype == ZOOM_TYPE_FOCAL_LENGTH and z.accept_focal_length:
                self.zoom_pct = self.zoom.pct_for_focal(value)
            elif ztype == ZOOM_TYPE_HORIZONTAL_FOV and z.accept_hfov:
                self.zoom_pct = self.zoom.pct_for_hfov(value)
            else:
                return MAV_RESULT_UNSUPPORTED, None
            self._optics_changed()
        return MAV_RESULT_ACCEPTED, self._settings_after()

    def _cmd_mode(self, msg, cmd, p):
        with self._lock:
            self.mode_id = int(p[1])
            self._sync_role_params()
        return MAV_RESULT_ACCEPTED, self._settings_after()

    def _cmd_focus(self, msg, cmd, p):
        with self._lock:
            self.focus = float(p[1])
        return MAV_RESULT_ACCEPTED, self._settings_after()

    def _cmd_reset(self, msg, cmd, p):
        with self._lock:
            self.zoom_pct = self.cfg.zoom.initial_pct
            self.zoom_speed = 0.0
            self.focus = float("nan")
            self._optics_changed()
        return MAV_RESULT_ACCEPTED, self._settings_after()

    def _cmd_source(self, msg, cmd, p):
        src = int(p[1])
        with self._lock:
            if src == CAMERA_SOURCE_IR:
                self.sensor_id = 1
            elif src in (CAMERA_SOURCE_RGB, 0):
                self.sensor_id = 0
            else:
                return MAV_RESULT_UNSUPPORTED, None
            self._optics_changed()
            self._sync_role_params()
        return MAV_RESULT_ACCEPTED, self._settings_after()

    # -- capture -----------------------------------------------------------
    def _accept_capture(self, source: str, ident: int) -> bool:
        now = time.monotonic()
        with self._lock:
            if source == "interval":  # our own timer: never a duplicate
                self._last_capture = (source, now)
                return True
            if ident and (source, ident) in self._seen_seq:
                return False
            if ident:  # remembered even if the window drops it: a repeat is a duplicate
                self._seen_seq.append((source, ident))
            last_src, last_t = self._last_capture
            if last_src is not None and now - last_t < self.cfg.capture.dedup_window_s:
                if last_src != source or not ident:
                    return False
            self._last_capture = (source, now)
            return True

    def _request_capture(self, source: str, ident: int) -> bool:
        if not self._accept_capture(source, ident):
            log.info("camera: duplicate trigger (%s %d) ignored", source, ident)
            return False
        pose = self.host.latest_pose() if self.host else None
        with self._lock:
            self._capturing += 1
        self._capture_q.put(pose)
        return True

    def _cmd_image_start(self, msg, cmd, p):
        interval, count, seq = p[1], int(p[2]), int(p[3])
        if interval > 0 and count != 1:
            with self._lock:
                self._interval = (interval, count if count > 0 else -1, time.monotonic())
            return MAV_RESULT_ACCEPTED, None
        return MAV_RESULT_ACCEPTED, (lambda: self._request_capture("image", seq))

    def _cmd_image_stop(self, msg, cmd, p):
        with self._lock:
            self._interval = None
        return MAV_RESULT_ACCEPTED, None

    def _cmd_digicam(self, msg, cmd, p):
        if int(p[4]) != 1:
            return MAV_RESULT_ACCEPTED, None
        return MAV_RESULT_ACCEPTED, (lambda: self._request_capture("digicam", int(p[5])))

    def _http_fetch(self, url: str, timeout: float) -> bytes:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            if r.status != 200:
                raise OSError(f"HTTP {r.status}")
            return r.read()

    def _capture_loop(self) -> None:
        while not self._stop.is_set():
            pose = self._capture_q.get()
            if pose is None and self._stop.is_set():
                return
            self._do_capture(pose)

    def _do_capture(self, pose: CameraPose | None) -> None:
        self.image_index += 1
        idx = self.image_index
        name = f"IMG_{idx:05d}.png"
        ok = False
        try:
            data = self.fetch(self.cfg.capture.snapshot_url, self.cfg.capture.timeout_s)
            self.capture_dir.mkdir(parents=True, exist_ok=True)
            (self.capture_dir / name).write_bytes(data)
            ok = True
            self.image_count += 1
        except Exception as e:
            log.warning("camera: snapshot %s failed: %s (operational.snapshot_endpoint_enabled?)", self.cfg.capture.snapshot_url, e)
        finally:
            with self._lock:
                self._capturing = max(0, self._capturing - 1)
        port = self.http.port if self.http is not None else self.cfg.http_port
        url = f"http://{self.cfg.advertise_host}:{port}/captures/{name}" if ok else ""
        if pose is not None:
            lat, lon = int(round(pose.lat * 1e7)), int(round(pose.lon * 1e7))
            alt, rel = int(round(pose.alt_msl * 1000)), int(round(pose.agl * 1000))
            q, utc = pose.q_nc.canonical().as_list(), int(pose.utc * 1e6)
        else:
            lat = lon = alt = rel = 0
            q, utc = [1.0, 0.0, 0.0, 0.0], int(time.time() * 1e6)
        m = mavlink.MAVLink_camera_image_captured_message(
            self.time_boot_ms(), utc, 0, lat, lon, alt, rel, q, idx - 1, 1 if ok else 0, encode_str(url, 205)
        )
        self.captured[idx - 1] = m
        self.send(m)
        self.send(self.capture_status_msg())

    # -- video -------------------------------------------------------------
    def _cmd_video_start(self, msg, cmd, p):
        with self._lock:
            if self.recorder is not None:
                return MAV_RESULT_ACCEPTED, None
            path = self.video_dir / time.strftime("VID_%Y%m%d_%H%M%S.ts")
            try:
                self.recorder = VideoRecorder(self.cfg.video.source, self.cfg.video.interface, path)
            except OSError as e:
                log.error("camera: cannot record %s: %s", self.cfg.video.source, e)
                return MAV_RESULT_FAILED, None
            self.recorder.start()
            log.info("camera: recording %s -> %s", self.cfg.video.source, path)
            if p[1] > 0:
                self.msg_intervals[MSG_CAMERA_CAPTURE_STATUS] = 1.0 / p[1]
        return MAV_RESULT_ACCEPTED, (lambda: self.send(self.capture_status_msg()))

    def _cmd_video_stop(self, msg, cmd, p):
        with self._lock:
            rec, self.recorder = self.recorder, None
        if rec is not None:
            rec.stop()
        return MAV_RESULT_ACCEPTED, (lambda: self.send(self.capture_status_msg()))

    # -- PARAM_EXT ---------------------------------------------------------
    def _param_value_msg(self, i: int):
        p = self.params.params[i]
        return mavlink.MAVLink_param_ext_value_message(
            encode_str(p.name, 16), self.params.encode(p), self.params.type_id(p), len(self.params.params), i
        )

    def _param_list(self) -> None:
        for i in range(len(self.params.params)):
            self.send(self._param_value_msg(i))

    def _param_read(self, msg) -> None:
        i = msg.param_index
        if i < 0:
            i = self.params.index(msg.param_id.rstrip("\0") if isinstance(msg.param_id, str) else msg.param_id.decode())
        if 0 <= i < len(self.params.params):
            self.send(self._param_value_msg(i))

    def _param_set(self, msg) -> None:
        raw = payload_bytes(msg, 147)  # target_system, target_component, id[16], value[128], type
        pid = raw[2:18].rstrip(b"\0").decode(errors="replace")
        value_raw, ptype = raw[18:146], raw[146]
        i = self.params.index(pid)
        if i < 0:
            self.send(mavlink.MAVLink_param_ext_ack_message(encode_str(pid, 16), value_raw, ptype, PARAM_ACK_FAILED))
            return
        p = self.params.params[i]
        if ptype != self.params.type_id(p):
            self.send(mavlink.MAVLink_param_ext_ack_message(encode_str(pid, 16), self.params.encode(p), self.params.type_id(p), PARAM_ACK_VALUE_UNSUPPORTED))
            return
        with self._lock:
            old = p.value
            p.value = self.params.decode(p.type, value_raw)
            if p.role and p.role != "mode" and p.role_values and float(p.value) not in {float(v) for v in p.role_values.values()}:
                p.value = old
                result = PARAM_ACK_VALUE_UNSUPPORTED
            else:
                self._apply_role(p)
                result = PARAM_ACK_ACCEPTED
        self.send(mavlink.MAVLink_param_ext_ack_message(encode_str(pid, 16), self.params.encode(p), self.params.type_id(p), result))
        if result == PARAM_ACK_ACCEPTED and p.role:
            self.send(self.camera_settings_msg())

    # -- periodic ----------------------------------------------------------
    def _status_loop(self) -> None:
        next_hb = 0.0  # heartbeat at once: PX4 forwards only after seeing it
        last = time.monotonic()
        while not self._stop.is_set():
            now = time.monotonic()
            dt, last = now - last, now
            if now >= next_hb:
                next_hb = now + 1.0 / max(0.1, self.cfg.heartbeat_rate_hz)
                self.send_heartbeat()
            self._tick_zoom(dt)
            self._tick_interval(now)
            with self._lock:
                intervals = dict(self.msg_intervals)
            for mid, iv in intervals.items():
                if now >= self._next_msg.get(mid, 0.0):
                    self._next_msg[mid] = now + iv
                    out = self._message(mid)
                    if out is not None:
                        self.send(out)
            self._stop.wait(0.05)

    def _tick_zoom(self, dt: float) -> None:
        send = False
        with self._lock:
            if self.zoom_speed != 0.0:
                new = max(0.0, min(100.0, self.zoom_pct + self.zoom_speed * self.cfg.zoom.continuous_rate_pct_s * dt))
                if new != self.zoom_pct:
                    self.zoom_pct = new
                    self._optics_changed()
                if new in (0.0, 100.0):
                    self.zoom_speed = 0.0
                    send = True
        if send:
            self.send(self.camera_settings_msg())

    def _tick_interval(self, now: float) -> None:
        with self._lock:
            iv = self._interval
            if iv is None or now < iv[2]:
                return
            interval, remaining, _ = iv
            remaining = remaining - 1 if remaining > 0 else remaining
            self._interval = None if remaining == 0 else (interval, remaining, now + interval)
        self._request_capture("interval", 0)
