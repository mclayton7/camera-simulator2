#!/usr/bin/env python3
"""
cigi_web_ui.py — Interactive CIGI 3.3 web control panel for CamSim.

HTTP UI on port 8080, WebSocket on port 8081, UDP sender to CamSim.

Usage:
    pip install websockets
    python3 scripts/cigi_web_ui.py [--ig-host HOST] [--ig-port PORT] [--rate HZ]

Then open: http://localhost:8080
"""

import argparse
import asyncio
import json
import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

try:
    import websockets
except ImportError:
    print("ERROR: 'websockets' not installed.  Run:  pip install websockets")
    raise


# ---------------------------------------------------------------------------
# CIGI 3.3 packet builders
# (adapted from send_cigi_test.py and test_entity_rendering.py)
# ---------------------------------------------------------------------------

def pack_ig_control(frame_ctr: int) -> bytes:
    """IG Control — packet ID 1, 24 bytes."""
    ts = int((time.time() % 86400) * 10_000) & 0xFFFF_FFFF
    return struct.pack(">BBBbBxHIIII",
        1, 24, 3, 0,
        0x05,         # ig_mode: Operate(1) | TS_Valid(1<<2)
        0x8000,       # byte-swap magic
        frame_ctr & 0xFFFF_FFFF, ts, 0, 0,
    )


def pack_entity_control(
    entity_id: int, lat: float, lon: float, alt: float,
    yaw: float, pitch: float, roll: float,
    entity_state: int = 1,
    entity_type: int = 0,
) -> bytes:
    """Entity Control — packet ID 2, 48 bytes (CIGI 3.3)."""
    header = struct.pack(">BBHBBBxHH",
        2, 48,
        entity_id,
        entity_state & 0x03,
        0,    # animation flags
        255,  # alpha
        entity_type & 0xFFFF,
        0,    # parent_entity_id
    )
    pkt = header + struct.pack(">fff", roll, pitch, yaw) + struct.pack(">ddd", lat, lon, alt)
    assert len(pkt) == 48
    return pkt


def pack_art_part_control(
    entity_id: int,
    art_part_id: int,
    *,
    roll: float = 0.0,
    pitch: float = 0.0,
    yaw: float = 0.0,
) -> bytes:
    """Articulated Part Control — packet ID 6, 32 bytes (CIGI 3.3)."""
    # art_part_en(0x01) | roll_en(0x10) | pitch_en(0x20) | yaw_en(0x40)
    flags = 0x01 | 0x10 | 0x20 | 0x40
    pkt = struct.pack(">BBHBBxxffffff",
        6, 32,
        entity_id & 0xFFFF,
        art_part_id & 0xFF,
        flags,
        0.0, 0.0, 0.0,  # x/y/z offsets (not enabled)
        roll, pitch, yaw,
    )
    assert len(pkt) == 32
    return pkt


def pack_component_control(
    instance_id: int,
    comp_id: int,
    comp_state: int,
    comp_class: int = 0,
) -> bytes:
    """Component Control — packet ID 4, 32 bytes (CIGI 3.3)."""
    pkt = struct.pack(">BBHHBBxxxxxxxxxxxxxxxxxxxxxxxx",
        4, 32,
        instance_id & 0xFFFF,
        comp_id & 0xFFFF,
        comp_class & 0xFF,
        comp_state & 0xFF,
    )
    assert len(pkt) == 32
    return pkt


def pack_view_definition(
    view_id: int = 0,
    fov_left: float = -30.0,
    fov_right: float = 30.0,
    fov_top: float = 16.875,
    fov_bottom: float = -16.875,
    near_plane: float = 0.1,
    far_plane: float = 1_000_000.0,
) -> bytes:
    """View Definition — packet ID 21, 32 bytes (CIGI 3.3)."""
    pkt = struct.pack(">BBHBBxx",
        21, 32, view_id, 0,
        0b00011111,  # enable near/far + all 4 FOV fields
    )
    pkt += struct.pack(">ffffff",
        near_plane, far_plane,
        fov_left, fov_right, fov_top, fov_bottom,
    )
    assert len(pkt) == 32
    return pkt


def pack_celestial_control(
    hour: int = 12,
    minute: int = 0,
    month: int = 6,
    day: int = 21,
    year: int = 2024,
) -> bytes:
    """Celestial Sphere Control — packet ID 9, 16 bytes (CIGI 3.3)."""
    flags = 0x01 | 0x02 | 0x04 | 0x10  # ephemeris | sun | moon | date_valid
    return struct.pack(">BBBBBxBBHfH",
        9, 16,
        hour & 0xFF, minute & 0xFF,
        flags,
        month & 0xFF, day & 0xFF,
        year & 0xFFFF,
        0.0,  # star field intensity
        0,    # reserved
    )


def pack_atmos_control(visibility: float = 50000.0) -> bytes:
    """Atmosphere Control — packet ID 10, 32 bytes (CIGI 3.3)."""
    return struct.pack(">BBBxfffffff",
        10, 32,
        0x01,     # atmos_en
        30.0,     # humidity (%)
        20.0,     # air temp (°C)
        visibility,
        0.0,      # horiz wind speed
        0.0,      # vert wind speed
        0.0,      # wind direction
        1013.25,  # barometric pressure (mb)
    )


def pack_weather_control(
    base_elev: float = 2000.0,
    thickness: float = 500.0,
    weather_en: bool = True,
) -> bytes:
    """Weather Control — packet ID 12, 56 bytes (CIGI 3.3)."""
    flags = (0x01 if weather_en else 0x00) | (2 << 4)  # weather_en | cloud_type=2
    scope_sev = (2 << 2) if weather_en else 0           # severity=2 when enabled
    return struct.pack(">BBHBBBBffffffffffff",
        12, 56,
        0,           # region_id
        0,           # layer_id
        0,           # humidity (unused)
        flags,
        scope_sev,
        0.0,         # air temp
        50000.0,     # visibility range
        0.0,         # scud frequency
        80.0 if weather_en else 0.0,  # coverage (%)
        base_elev,
        thickness,
        500.0,       # transition band
        0.0, 0.0, 0.0,  # wind
        1013.25,     # baro pressure
        0.0,         # aerosol
    )


def pack_sensor_control(
    view_id: int,
    sensor_id: int,
    on: bool,
    polarity: int,
    gain: float,
) -> bytes:
    """Sensor Control — packet ID 17, 24 bytes (CIGI 3.3)."""
    flags = (0x01 if on else 0x00) | ((polarity & 0x01) << 1)
    pkt = struct.pack(">BBHBBxxffff",
        17, 24,
        view_id & 0xFFFF,
        sensor_id & 0xFF,
        flags,
        gain, 0.0, 0.0, 0.0,
    )
    assert len(pkt) == 24
    return pkt


def pack_view_control(
    view_id: int,
    entity_id: int,
    roll: float = 0.0,
    pitch: float = 0.0,
    yaw: float = 0.0,
    x_off: float = 0.0,
    y_off: float = 0.0,
    z_off: float = 0.0,
) -> bytes:
    """View Control — packet ID 16, 32 bytes (CIGI 3.3)."""
    pkt = struct.pack(">BBHHBBffffff",
        16, 32,
        view_id & 0xFFFF,
        entity_id & 0xFFFF,
        0,     # group_id
        0x3F,  # dof_flags: all 6 DOF enabled
        x_off, y_off, z_off,
        roll, pitch, yaw,
    )
    assert len(pkt) == 32
    return pkt


# ---------------------------------------------------------------------------
# Shared state
# ---------------------------------------------------------------------------

class IGState:
    """All mutable IG state; owned by the asyncio main loop."""

    def __init__(self):
        # Network
        self.ig_host: str = "127.0.0.1"
        self.ig_port: int = 8888
        self.rate_hz: float = 10.0
        self.frame_ctr: int = 0
        self.udp_sock = None

        # Connected WebSocket clients
        self.ws_clients: set = set()

        # Camera entity
        self.camera_entity_id: int = 0
        self.cam_lat: float = 37.7749
        self.cam_lon: float = -122.4194
        self.cam_alt: float = 1000.0
        self.cam_yaw: float = 0.0
        self.cam_pitch: float = 0.0    # platform level — gimbal points to nadir
        self.cam_roll: float = 0.0

        # Gimbal (driven by ArtPart on camera entity, art_part_id=0)
        # -90 = nadir (straight down); 0 = boresighted to platform (forward)
        self.gimbal_yaw: float = 0.0
        self.gimbal_pitch: float = -90.0
        self.gimbal_roll: float = 0.0

        # Sensor
        self.sensor_on: bool = True
        self.sensor_id: int = 0         # 0=EO, 1=IR, 2=NVG
        self.sensor_polarity: int = 0   # 0=white hot, 1=black hot (IR only)
        self.sensor_gain: float = 0.0
        self.sensor_view_id: int = 0

        # View definition — one-shot, resent when True
        self.hfov: float = 60.0
        self.send_view_def: bool = True  # send on first frame

        # Environment — one-shot, resent when True
        self.env_hour: int = 12
        self.env_minute: int = 0
        self.env_month: int = 6
        self.env_day: int = 21
        self.env_year: int = 2024
        self.env_visibility: float = 50000.0
        self.env_weather_en: bool = False
        self.env_cloud_base: float = 2000.0
        self.env_cloud_thickness: float = 500.0
        self.send_env: bool = True  # send on first frame

        # Scene entities: entity_id → data dict
        self.entities: dict = {}

        # One-shot packets appended here; drained each frame
        self.pending_packets: list = []


# ---------------------------------------------------------------------------
# Asyncio tasks
# ---------------------------------------------------------------------------

async def frame_send_task(state: IGState):
    """Build and send one CIGI host-frame datagram per 1/rate_hz seconds."""
    while True:
        t0 = time.monotonic()
        try:
            dgram = pack_ig_control(state.frame_ctr)

            # Camera entity (always present)
            dgram += pack_entity_control(
                state.camera_entity_id,
                state.cam_lat, state.cam_lon, state.cam_alt,
                state.cam_yaw, state.cam_pitch, state.cam_roll,
            )

            # Gimbal via ArtPart on the camera entity (art_part_id=0)
            dgram += pack_art_part_control(
                state.camera_entity_id, 0,
                roll=state.gimbal_roll,
                pitch=state.gimbal_pitch,
                yaw=state.gimbal_yaw,
            )

            # Sensor control
            dgram += pack_sensor_control(
                state.sensor_view_id, state.sensor_id,
                state.sensor_on, state.sensor_polarity, state.sensor_gain,
            )

            # View definition (one-shot)
            if state.send_view_def:
                half_h = state.hfov / 2.0
                half_v = half_h * (9.0 / 16.0)
                dgram += pack_view_definition(
                    fov_left=-half_h, fov_right=half_h,
                    fov_top=half_v, fov_bottom=-half_v,
                )
                state.send_view_def = False

            # Environment (one-shot)
            if state.send_env:
                dgram += pack_celestial_control(
                    hour=state.env_hour, minute=state.env_minute,
                    month=state.env_month, day=state.env_day, year=state.env_year,
                )
                dgram += pack_atmos_control(visibility=state.env_visibility)
                if state.env_weather_en:
                    dgram += pack_weather_control(
                        base_elev=state.env_cloud_base,
                        thickness=state.env_cloud_thickness,
                    )
                state.send_env = False

            # Scene entities
            for eid, ed in list(state.entities.items()):
                dgram += pack_entity_control(
                    eid,
                    ed.get("lat", 37.7749),
                    ed.get("lon", -122.4194),
                    ed.get("alt", 1000.0),
                    ed.get("yaw", 0.0),
                    ed.get("pitch", 0.0),
                    ed.get("roll", 0.0),
                    entity_state=ed.get("state", 1),
                    entity_type=ed.get("entity_type", 0),
                )

            # Drain pending one-shot packets (atomic swap — no lock needed in asyncio)
            pending, state.pending_packets = state.pending_packets, []
            for pkt in pending:
                dgram += pkt

            if state.udp_sock:
                state.udp_sock.sendto(dgram, (state.ig_host, state.ig_port))

        except Exception as exc:
            print(f"[frame_send] {exc}")

        state.frame_ctr += 1
        elapsed = time.monotonic() - t0
        await asyncio.sleep(max(0.0, 1.0 / state.rate_hz - elapsed))


async def status_broadcast_task(state: IGState):
    """Broadcast status JSON to all WebSocket clients every second."""
    while True:
        await asyncio.sleep(1.0)
        if not state.ws_clients:
            continue
        try:
            msg = json.dumps({
                "type": "status",
                "frame": state.frame_ctr,
                "entities": [
                    {
                        "id":          eid,
                        "entity_type": ed.get("entity_type", 0),
                        "lat":         ed.get("lat", 0.0),
                        "lon":         ed.get("lon", 0.0),
                        "alt":         ed.get("alt", 0.0),
                        "yaw":         ed.get("yaw", 0.0),
                    }
                    for eid, ed in state.entities.items()
                ],
            })
            dead = set()
            for ws in list(state.ws_clients):
                try:
                    await ws.send(msg)
                except Exception:
                    dead.add(ws)
            state.ws_clients -= dead
        except Exception as exc:
            print(f"[status_broadcast] {exc}")


async def ws_handler(websocket, state: IGState):
    """Handle one WebSocket client connection."""
    state.ws_clients.add(websocket)
    print(f"[ws] connected: {websocket.remote_address}")
    try:
        async for raw in websocket:
            try:
                m = json.loads(raw)
                t = m.get("type", "")

                if t == "config":
                    state.ig_host = str(m.get("host", state.ig_host))
                    state.ig_port = int(m.get("port", state.ig_port))
                    state.rate_hz = max(1.0, float(m.get("rate", state.rate_hz)))
                    print(f"[ws] config → {state.ig_host}:{state.ig_port}  {state.rate_hz} Hz")

                elif t == "camera":
                    state.camera_entity_id = int(m.get("entity_id", state.camera_entity_id))
                    state.cam_lat   = float(m.get("lat",   state.cam_lat))
                    state.cam_lon   = float(m.get("lon",   state.cam_lon))
                    state.cam_alt   = float(m.get("alt",   state.cam_alt))
                    state.cam_yaw   = float(m.get("yaw",   state.cam_yaw))
                    state.cam_pitch = float(m.get("pitch", state.cam_pitch))
                    state.cam_roll  = float(m.get("roll",  state.cam_roll))

                elif t == "gimbal":
                    state.gimbal_yaw   = float(m.get("yaw",   state.gimbal_yaw))
                    state.gimbal_pitch = float(m.get("pitch", state.gimbal_pitch))
                    state.gimbal_roll  = float(m.get("roll",  state.gimbal_roll))

                elif t == "sensor":
                    state.sensor_on       = bool(m.get("on",        state.sensor_on))
                    state.sensor_id       = int(m.get("sensor_id",  state.sensor_id))
                    state.sensor_polarity = int(m.get("polarity",   state.sensor_polarity))
                    state.sensor_gain     = float(m.get("gain",     state.sensor_gain))
                    state.sensor_view_id  = int(m.get("view_id",    state.sensor_view_id))

                elif t == "view_def":
                    state.hfov = float(m.get("hfov", state.hfov))
                    state.send_view_def = True

                elif t == "environment":
                    state.env_hour            = int(m.get("hour",            state.env_hour))
                    state.env_minute          = int(m.get("minute",          state.env_minute))
                    state.env_month           = int(m.get("month",           state.env_month))
                    state.env_day             = int(m.get("day",             state.env_day))
                    state.env_year            = int(m.get("year",            state.env_year))
                    state.env_visibility      = float(m.get("visibility",    state.env_visibility))
                    state.env_weather_en      = bool(m.get("weather_en",     state.env_weather_en))
                    state.env_cloud_base      = float(m.get("cloud_base",    state.env_cloud_base))
                    state.env_cloud_thickness = float(m.get("cloud_thickness", state.env_cloud_thickness))
                    state.send_env = True

                elif t == "entity_spawn":
                    eid = int(m["id"])
                    state.entities[eid] = {
                        "lat":         float(m.get("lat",         37.7749)),
                        "lon":         float(m.get("lon",         -122.4194)),
                        "alt":         float(m.get("alt",         50.0)),
                        "yaw":         float(m.get("yaw",         0.0)),
                        "pitch":       float(m.get("pitch",       0.0)),
                        "roll":        float(m.get("roll",        0.0)),
                        "entity_type": int(m.get("entity_type",   0)),
                        "state":       1,
                    }
                    print(f"[ws] spawn entity {eid} type={state.entities[eid]['entity_type']}")

                elif t == "entity_remove":
                    eid = int(m["id"])
                    if eid in state.entities:
                        ed = state.entities.pop(eid)
                        # Queue a Remove state packet
                        state.pending_packets.append(pack_entity_control(
                            eid,
                            ed.get("lat", 37.7749), ed.get("lon", -122.4194),
                            ed.get("alt", 50.0), 0.0, 0.0, 0.0,
                            entity_state=2,
                        ))
                        print(f"[ws] remove entity {eid}")

                elif t == "component":
                    state.pending_packets.append(pack_component_control(
                        int(m["entity_id"]),
                        int(m["comp_id"]),
                        int(m["comp_state"]),
                    ))

            except Exception as exc:
                print(f"[ws] error: {exc}  msg={raw!r}")

    except Exception:
        pass
    finally:
        state.ws_clients.discard(websocket)
        print("[ws] disconnected")


# ---------------------------------------------------------------------------
# HTTP handler — always serves cigi_web_ui.html
# ---------------------------------------------------------------------------

_HTML_PATH = Path(__file__).parent / "cigi_web_ui.html"


class _UIHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        try:
            body = _HTML_PATH.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except FileNotFoundError:
            self.send_error(404, "cigi_web_ui.html not found next to this script")

    def log_message(self, *args):
        pass  # suppress access log noise


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

async def main(ig_host: str, ig_port: int, rate_hz: float):
    state = IGState()
    state.ig_host = ig_host
    state.ig_port = ig_port
    state.rate_hz = rate_hz
    state.udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    http = HTTPServer(("0.0.0.0", 8080), _UIHandler)
    threading.Thread(target=http.serve_forever, daemon=True).start()

    print("CIGI Web UI")
    print("  UI:  http://localhost:8080")
    print("  WS:  ws://localhost:8081")
    print(f"  UDP: {state.ig_host}:{state.ig_port}  rate={state.rate_hz} Hz")
    print("  Ctrl-C to stop\n")

    async def _handler(ws, *args):
        await ws_handler(ws, state)

    async with websockets.serve(_handler, "0.0.0.0", 8081):
        await asyncio.gather(
            frame_send_task(state),
            status_broadcast_task(state),
        )


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ig-host", default="127.0.0.1", help="CamSim CIGI host (default: 127.0.0.1)")
    ap.add_argument("--ig-port", type=int, default=8888, help="CamSim CIGI port (default: 8888)")
    ap.add_argument("--rate",    type=float, default=10.0, help="Send rate Hz (default: 10)")
    args = ap.parse_args()
    try:
        asyncio.run(main(args.ig_host, args.ig_port, args.rate))
    except KeyboardInterrupt:
        print("\nStopped.")
