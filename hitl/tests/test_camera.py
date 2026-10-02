"""Camera Protocol component: request handling, ACK-first, zoom/source, capture
dedup, PARAM_EXT, CAMERA_FOV_STATUS, the HTTP server and video recording."""

import math
import socket
import struct
import threading
import time
import urllib.request
from pathlib import Path

import pytest

from camsim_hitl.camera import (
    INT32_MAX,
    INT32_MIN,
    CameraComponent,
    CameraPose,
    FrameCentre,
    ZoomModel,
)
from camsim_hitl.config import CameraConfig, CameraParam, ZoomConfig
from camsim_hitl.mavlink_link import mavlink
from camsim_hitl.quat import from_euler_zyx

D = math.radians
GCS = mavlink.MAVLink(None, srcSystem=255, srcComponent=190)
PARSER = mavlink.MAVLink(None)
HITL = Path(__file__).resolve().parents[1]


def wire(msg):
    """Pack as the GCS and decode, so the message has a source like a received one."""
    out = PARSER.parse_buffer(msg.pack(GCS))
    GCS.seq = (GCS.seq + 1) % 256
    return out[0]


def cmd(command, *params, target=100):
    ps = list(params) + [0.0] * (7 - len(params))
    return wire(mavlink.MAVLink_command_long_message(1, target, command, 0, *ps))


class FakeLink:
    def __init__(self):
        self.sent = []
        self.lock = threading.Lock()

    def send(self, comp, msg):
        with self.lock:
            self.sent.append((time.monotonic(), msg))

    def types(self):
        with self.lock:
            return [m.get_type() for _, m in self.sent]

    def of(self, t):
        with self.lock:
            return [m for _, m in self.sent if m.get_type() == t]

    def clear(self):
        with self.lock:
            self.sent.clear()


class FakeHost:
    def __init__(self):
        self.pose = None
        self.fc = None

    def latest_pose(self):
        return self.pose

    def latest_frame_centre(self):
        return self.fc

    def undulation(self, lat, lon):
        return -32.0


def camera_cfg(tmp_path, **kw) -> CameraConfig:
    c = CameraConfig(enabled=True, link="test", vendor_name="Acme", model_name="EOIR-1", firmware_version="2.3.4.5",
                     definition_file=str(HITL / "camera_definition.example.xml"), advertise_host="127.0.0.1",
                     http_port=0, fov_status_rate_hz=0.0)
    c.capture.dir = str(tmp_path / "captures")
    c.video.dir = str(tmp_path / "videos")
    c.capture.dedup_window_s = 0.3
    c.params = [
        CameraParam(name="CAM_SOURCE", type="uint8", value=0, role="source", role_values={"eo": 0, "ir": 1}),
        CameraParam(name="CAM_POLARITY", type="uint8", value=0, role="polarity", role_values={"white_hot": 0, "black_hot": 1}),
        CameraParam(name="CAM_EV", type="real32", value=0.5),
    ]
    for k, v in kw.items():
        setattr(c, k, v)
    return c


@pytest.fixture
def cam(tmp_path):
    host = FakeHost()
    fetched = []

    def fetch(url, timeout):
        fetched.append(url)
        time.sleep(0.2)  # a slow snapshot must not delay the ACK
        return b"\x89PNG fake"

    c = CameraComponent(camera_cfg(tmp_path), 1, host=host, base_dir=tmp_path, fetch=fetch)
    c.link = FakeLink()
    c.start(http=True)
    c.fetched = fetched
    yield c, host
    c.stop()


def wait_until(pred, timeout=2.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return True
        time.sleep(0.01)
    return False


def test_heartbeat_sent_at_startup(cam):
    c, _ = cam
    assert wait_until(lambda: "HEARTBEAT" in c.link.types(), 0.5)
    hb = c.link.of("HEARTBEAT")[0]
    assert hb.type == 30 and hb.autopilot == 8


def test_camera_information_ack_first(cam):
    c, _ = cam
    c.link.clear()
    c.handle(cmd(512, 259))
    t = [x for x in c.link.types() if x != "HEARTBEAT"]
    assert t[:2] == ["COMMAND_ACK", "CAMERA_INFORMATION"]
    ack = c.link.of("COMMAND_ACK")[0]
    assert ack.result == 0 and ack.target_system == 255 and ack.target_component == 190
    info = c.link.of("CAMERA_INFORMATION")[0]
    assert bytes(info.vendor_name).rstrip(b"\0") == b"Acme"
    assert bytes(info.model_name).rstrip(b"\0") == b"EOIR-1"
    assert info.firmware_version == (5 << 24) | (4 << 16) | (3 << 8) | 2
    assert info.cam_definition_uri == f"http://127.0.0.1:{c.http.port}/camera_definition.example.xml"
    assert info.gimbal_device_id == 154
    assert (info.resolution_h, info.resolution_v) == (1920, 1080)
    # the definition file is served over http://
    body = urllib.request.urlopen(info.cam_definition_uri, timeout=2).read()
    assert b"<mavlinkcamera>" in body


@pytest.mark.parametrize("command,mtype", [(521, "CAMERA_INFORMATION"), (522, "CAMERA_SETTINGS"),
                                           (525, "STORAGE_INFORMATION"), (527, "CAMERA_CAPTURE_STATUS")])
def test_superseded_requests(cam, command, mtype):
    c, _ = cam
    c.link.clear()
    c.handle(cmd(command, 1))
    assert c.link.of("COMMAND_ACK")[0].result == 0
    assert c.link.of(mtype)
    if mtype == "STORAGE_INFORMATION":
        assert c.link.of(mtype)[0].status == 2  # READY


def test_broadcast_gimbal_request_not_answered(cam):
    c, _ = cam
    c.link.clear()
    c.handle(cmd(512, 283, target=0))  # GIMBAL_DEVICE_INFORMATION is the gimbal's
    assert not [t for t in c.link.types() if t != "HEARTBEAT"]
    c.handle(cmd(512, 283, target=100))
    assert c.link.of("COMMAND_ACK")[0].result == 3


def test_zoom_range_step_and_view_definition(cam):
    c, _ = cam
    o0 = c.optics()
    c.link.clear()
    c.handle(cmd(531, 2, 50.0))  # RANGE 50 %
    assert c.link.types()[0] == "COMMAND_ACK"
    s = c.link.of("CAMERA_SETTINGS")[-1]
    assert s.zoomLevel == pytest.approx(50.0)
    o1 = c.optics()
    assert o1.hfov < o0.hfov and o1.version > o0.version
    assert o1.vfov == pytest.approx(math.degrees(2 * math.atan(math.tan(D(o1.hfov) / 2) * 1080 / 1920)))
    c.handle(cmd(531, 0, 1.0))  # STEP +1 = +10 %
    assert c.zoom_pct == pytest.approx(60.0)
    # zoom commands from a mission arrive at component 0 and are handled too
    c.handle(cmd(531, 2, 10.0, target=0))
    assert c.zoom_pct == pytest.approx(10.0)
    # FOCAL_LENGTH not accepted unless the real camera has it
    c.link.clear()
    c.handle(cmd(531, 3, 20.0))
    assert c.link.of("COMMAND_ACK")[0].result == 3


def test_continuous_zoom(cam):
    c, _ = cam
    c.handle(cmd(531, 1, 1.0))
    time.sleep(0.4)
    assert 5.0 < c.zoom_pct < 15.0  # 25 %/s
    c.link.clear()
    c.handle(cmd(531, 1, 0.0))  # stop
    z = c.zoom_pct
    time.sleep(0.2)
    assert c.zoom_pct == z
    assert c.link.of("CAMERA_SETTINGS")


def test_source_and_params(cam):
    c, _ = cam
    c.handle(cmd(534, 0, 2))  # SET_CAMERA_SOURCE primary IR
    assert c.optics().sensor_id == 1
    assert c.params.params[0].value == 1  # role-synced definition-file parameter
    c.handle(cmd(534, 0, 1))
    assert c.optics().sensor_id == 0
    c.link.clear()
    c.handle(wire(mavlink.MAVLink_param_ext_request_list_message(1, 100)))
    vals = c.link.of("PARAM_EXT_VALUE")
    assert [v.param_id for v in vals] == ["CAM_SOURCE", "CAM_POLARITY", "CAM_EV"]
    assert vals[0].param_type == 1 and vals[0].param_count == 3
    # PARAM_EXT_SET with binary values (MAVLink 2 trims the trailing zero bytes)
    c.link.clear()
    c.handle(wire(mavlink.MAVLink_param_ext_set_message(1, 100, b"CAM_SOURCE", struct.pack("<B", 1).ljust(128, b"\0"), 1)))
    ack = c.link.of("PARAM_EXT_ACK")[0]
    assert ack.param_result == 0 and c.optics().sensor_id == 1
    c.handle(wire(mavlink.MAVLink_param_ext_set_message(1, 100, b"CAM_POLARITY", b"\x01".ljust(128, b"\0"), 1)))
    assert c.optics().polarity == 1
    c.handle(wire(mavlink.MAVLink_param_ext_set_message(1, 100, b"CAM_EV", struct.pack("<f", -1.5).ljust(128, b"\0"), 9)))
    assert c.params.params[2].value == pytest.approx(-1.5)
    c.link.clear()
    c.handle(wire(mavlink.MAVLink_param_ext_set_message(1, 100, b"CAM_SOURCE", b"\x07".ljust(128, b"\0"), 1)))
    assert c.link.of("PARAM_EXT_ACK")[0].param_result == 1  # value unsupported
    c.handle(wire(mavlink.MAVLink_param_ext_request_read_message(1, 100, b"CAM_EV", -1)))
    v = c.link.of("PARAM_EXT_VALUE")[-1]
    assert v.param_id == "CAM_EV"


def test_capture_ack_first_and_dedup(cam, tmp_path):
    c, host = cam
    host.pose = CameraPose(t_ns=0, lat=37.5, lon=-122.25, alt_msl=150.0, alt_ell=118.0,
                           q_nc=from_euler_zyx(0, D(-45), 0), agl=140.0, utc=1.79e9)
    c.link.clear()
    t0 = time.monotonic()
    c.handle(cmd(2000, 0, 0, 1, 7))  # single shot, sequence 7
    ack_t, ack = c.link.sent[0]
    assert ack.get_type() == "COMMAND_ACK" and ack.result == 0
    assert ack_t - t0 < 0.05  # ACK before the (0.2 s) snapshot
    c.handle(cmd(2000, 0, 0, 1, 7))  # the same sequence number again: one photo
    for ident in (5, 5, 5):  # TRIG_INTERFACE=3 may deliver one photo up to three times
        c.handle(cmd(203, 0, 0, 0, 0, 1, ident))
    assert wait_until(lambda: len(c.link.of("CAMERA_IMAGE_CAPTURED")) >= 1, 2.0)
    time.sleep(0.6)
    caps = c.link.of("CAMERA_IMAGE_CAPTURED")
    assert len(caps) == 1  # digicam within the window of the other source: dropped
    m = caps[0]
    assert m.capture_result == 1 and m.image_index == 0
    assert (m.lat, m.lon, m.alt, m.relative_alt) == (375000000, -1222500000, 150000, 140000)
    assert m.file_url.endswith("/captures/IMG_00001.png")
    assert (tmp_path / "captures/IMG_00001.png").read_bytes() == b"\x89PNG fake"
    assert urllib.request.urlopen(m.file_url, timeout=2).read() == b"\x89PNG fake"
    # outside the window, a new identity is a new photo; the old one stays a duplicate
    c.handle(cmd(203, 0, 0, 0, 0, 1, 6))
    c.handle(cmd(203, 0, 0, 0, 0, 1, 5))
    assert wait_until(lambda: len(c.link.of("CAMERA_IMAGE_CAPTURED")) == 2, 2.0)
    time.sleep(0.4)
    assert len(c.link.of("CAMERA_IMAGE_CAPTURED")) == 2
    assert c.fetched[0] == "http://127.0.0.1:8080/snapshot"
    # request the captured-image message again by index
    c.link.clear()
    c.handle(cmd(512, 263, 0))
    assert c.link.of("CAMERA_IMAGE_CAPTURED")[0].image_index == 0


def test_capture_failure_reports_result_0(tmp_path):
    def fetch(url, timeout):
        raise OSError("503")

    c = CameraComponent(camera_cfg(tmp_path), 1, host=FakeHost(), base_dir=tmp_path, fetch=fetch)
    c.link = FakeLink()
    c.start(http=False)
    try:
        c.handle(cmd(2000, 0, 0, 1, 1))
        assert wait_until(lambda: c.link.of("CAMERA_IMAGE_CAPTURED"), 2.0)
        assert c.link.of("CAMERA_IMAGE_CAPTURED")[0].capture_result == 0
    finally:
        c.stop()


def test_fov_status(cam):
    c, host = cam
    m = c.fov_status_msg()
    assert m.lat_image == INT32_MAX  # no pose yet: unknown
    host.pose = CameraPose(t_ns=0, lat=37.5, lon=-122.25, alt_msl=150.0, alt_ell=118.0,
                           q_nc=from_euler_zyx(0, D(-45), 0), agl=140.0, utc=1.79e9)
    host.fc = FrameCentre(lat=37.5013, lon=-122.25, alt_ell=-30.0, frame=10, rx_ns=0)
    m = c.fov_status_msg()
    assert (m.lat_image, m.lon_image) == (375013000, -1222500000)
    assert m.alt_image == 2000  # -30 m ellipsoid - (-32 m geoid) = 2 m MSL
    assert m.alt_camera == 150000
    assert m.hfov == pytest.approx(c.optics().hfov)
    # boresight above the horizon: INT32_MIN even though CamSim keeps the last hit
    host.pose.q_nc = from_euler_zyx(0, D(5), 0)
    m = c.fov_status_msg()
    assert m.lat_image == INT32_MIN and m.lon_image == INT32_MIN


def test_video_record_and_streaming_ack(cam, tmp_path):
    c, _ = cam
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    c.cfg.video.source = f"udp://127.0.0.1:{port}"
    c.link.clear()
    c.handle(cmd(2500, 0, 0))
    assert c.link.of("COMMAND_ACK")[0].result == 0
    assert c.capture_status_msg().video_status == 1
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for i in range(5):
        tx.sendto(bytes([0x47]) + bytes(187) * 1, ("127.0.0.1", port))
    time.sleep(0.3)
    c.handle(cmd(2501))
    files = list((tmp_path / "videos").glob("VID_*.ts"))
    assert len(files) == 1 and files[0].stat().st_size == 5 * 188
    c.link.clear()
    c.handle(cmd(2502))
    c.handle(cmd(2503))
    assert [a.result for a in c.link.of("COMMAND_ACK")] == [0, 0]
    c.link.clear()
    c.handle(cmd(2004, 0.5, 0.5))  # tracking: not supported by this camera
    assert c.link.of("COMMAND_ACK")[0].result == 3


def test_set_message_interval_fov_status(cam):
    c, host = cam
    c.link.clear()
    c.handle(cmd(511, 271, 100000))  # 10 Hz
    assert c.link.of("COMMAND_ACK")[0].result == 0
    time.sleep(0.5)
    assert len(c.link.of("CAMERA_FOV_STATUS")) >= 3


def test_zoom_models():
    z = ZoomModel(ZoomConfig(focal_length_mm=[4.3, 129.0], sensor_width_mm=6.17))
    assert z.hfov(0) == pytest.approx(math.degrees(2 * math.atan(6.17 / 8.6)))
    assert z.hfov(100) == pytest.approx(math.degrees(2 * math.atan(6.17 / 258.0)))
    assert z.pct_for_hfov(z.hfov(37.0)) == pytest.approx(37.0, abs=1e-6)
    assert z.pct_for_focal(z.focal(63.0)) == pytest.approx(63.0, abs=1e-6)
    t = ZoomModel(ZoomConfig(model="table", hfov_table=[[0, 60.0], [50, 20.0], [100, 2.0]]))
    assert t.hfov(25) == pytest.approx(40.0)
    assert t.hfov(75) == pytest.approx(11.0)
    lg = ZoomModel(ZoomConfig(curve="log", focal_length_mm=[10.0, 1000.0]))
    assert lg.focal(50) == pytest.approx(100.0)
