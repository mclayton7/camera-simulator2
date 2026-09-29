#!/usr/bin/env python3
"""Send DIS Entity State PDUs for a scripted truck and boat (CamSim DIS test sender).

Usage:
  send_dis_test.py [both|truck-loop|boat-circle] [--addr 127.0.0.1] [--port 3000]
                   [--exercise 1] [--location LAT,LON] [--duration SEC] [--rate HZ] [--verbose]

CamSim needs `dis.enabled: true` (or CAMSIM_DIS_ENABLED=1). Altitude is sent as 0 m: CamSim
places the vehicles on the terrain / water (dis.clamp_to_surface).
"""

from __future__ import annotations

import argparse
import math
import socket
import struct
import sys
import time
from dataclasses import dataclass, field

WGS84_A = 6378137.0
WGS84_E2 = 6.69437999014e-3

TRUCK_TYPE = (1, 1, 225, 7, 0, 0, 0)  # land, USA, large wheeled utility vehicle
BOAT_TYPE = (1, 3, 225, 7, 0, 0, 0)  # surface, USA, light/patrol craft

HEARTBEAT_S = 0.2  # 5 Hz
HEADING_THRESHOLD = 3.0  # degrees
TICK_HZ = 30.0


def geodetic_to_ecef(lat: float, lon: float, alt: float) -> tuple[float, float, float]:
    la, lo = math.radians(lat), math.radians(lon)
    n = WGS84_A / math.sqrt(1.0 - WGS84_E2 * math.sin(la) ** 2)
    return (
        (n + alt) * math.cos(la) * math.cos(lo),
        (n + alt) * math.cos(la) * math.sin(lo),
        (n * (1.0 - WGS84_E2) + alt) * math.sin(la),
    )


def _ned_to_ecef(lat: float, lon: float) -> list[list[float]]:
    """Columns North, East, Down in ECEF (matches CamSimFrames::NedToEcef)."""
    sl, cl = math.sin(math.radians(lat)), math.cos(math.radians(lat))
    so, co = math.sin(math.radians(lon)), math.cos(math.radians(lon))
    return [[-sl * co, -so, -cl * co], [-sl * so, co, -cl * so], [cl, 0.0, -sl]]


def heading_to_dis_euler(
    heading_deg: float, lat: float, lon: float
) -> tuple[float, float, float]:
    """Level body at a heading -> DIS psi/theta/phi (radians), as CamSimFrames::CigiToDisEuler."""
    h = math.radians(heading_deg)
    rz = [
        [math.cos(h), -math.sin(h), 0.0],
        [math.sin(h), math.cos(h), 0.0],
        [0.0, 0.0, 1.0],
    ]
    m = _ned_to_ecef(lat, lon)
    r = [
        [sum(m[i][k] * rz[k][j] for k in range(3)) for j in range(3)] for i in range(3)
    ]
    psi = math.atan2(r[1][0], r[0][0])
    theta = -math.asin(max(-1.0, min(1.0, r[2][0])))
    phi = math.atan2(r[2][1], r[2][2])
    return psi, theta, phi


def _dis_timestamp(t: float) -> int:
    """Relative timestamp: units of 3600/2^31 s past the hour, low bit 0."""
    return (int((t % 3600.0) / 3600.0 * (1 << 31)) & 0x7FFFFFFF) << 1


def pack_entity_state(
    entity_id: int,
    entity_type: tuple,
    lat: float,
    lon: float,
    alt: float,
    heading_deg: float,
    speed_mps: float,
    yaw_rate_dps: float,
    exercise: int = 1,
    marking: str = "",
    t: float = 0.0,
) -> bytes:
    """One 144-byte IEEE 1278.1 Entity State PDU (layout: DIS/DisPduTypes.cpp)."""
    m = _ned_to_ecef(lat, lon)
    vn, ve = (
        speed_mps * math.cos(math.radians(heading_deg)),
        speed_mps * math.sin(math.radians(heading_deg)),
    )
    vel = [m[i][0] * vn + m[i][1] * ve for i in range(3)]
    psi, theta, phi = heading_to_dis_euler(heading_deg, lat, lon)
    x, y, z = geodetic_to_ecef(lat, lon, alt)
    mark = marking.encode("ascii", "replace")[:11].ljust(11, b"\0")
    pdu = struct.pack(">BBBBIHH", 7, exercise, 1, 1, _dis_timestamp(t), 144, 0)
    pdu += struct.pack(
        ">HHHBB", 1, 1, entity_id, 1, 0
    )  # site, app, entity, force, #art
    pdu += struct.pack(">BBHBBBB", *entity_type)
    pdu += bytes(8)  # alternative entity type
    pdu += struct.pack(">fff", *vel)
    pdu += struct.pack(">ddd", x, y, z)
    pdu += struct.pack(">fff", psi, theta, phi)
    pdu += struct.pack(">I", 0)  # appearance
    pdu += struct.pack(">B", 4) + bytes(15)  # DR algorithm 4 + other params
    pdu += struct.pack(">fff", 0.0, 0.0, 0.0)  # linear acceleration
    pdu += struct.pack(
        ">fff", 0.0, 0.0, math.radians(yaw_rate_dps)
    )  # body angular velocity
    pdu += struct.pack(">B", 1) + mark  # marking (ASCII)
    pdu += struct.pack(">I", 0)  # capabilities
    assert len(pdu) == 144
    return pdu


class PathFollower:
    """Constant speed around a closed polyline of (north_m, east_m) points."""

    def __init__(self, waypoints_ne: list[tuple[float, float]], speed_mps: float):
        self.pts = list(waypoints_ne)
        self.speed = speed_mps
        self.cum = [0.0]
        for i in range(len(self.pts)):
            a, b = self.pts[i], self.pts[(i + 1) % len(self.pts)]
            self.cum.append(self.cum[-1] + math.hypot(b[0] - a[0], b[1] - a[1]))
        self.length = self.cum[-1]

    def _point(self, s: float) -> tuple[float, float]:
        s %= self.length
        for i in range(len(self.pts)):
            if s <= self.cum[i + 1]:
                a, b = self.pts[i], self.pts[(i + 1) % len(self.pts)]
                f = (s - self.cum[i]) / max(self.cum[i + 1] - self.cum[i], 1e-9)
                return a[0] + f * (b[0] - a[0]), a[1] + f * (b[1] - a[1])
        return self.pts[0]

    def _heading(self, s: float) -> float:
        # Look ahead one second of travel: rounds the corners into smooth turns.
        ahead = max(self.speed, 1.0)
        a, b = self._point(s - ahead / 2), self._point(s + ahead / 2)
        return math.degrees(math.atan2(b[1] - a[1], b[0] - a[0])) % 360.0

    def state(self, t: float) -> tuple[float, float, float, float]:
        s = self.speed * t
        n, e = self._point(s)
        h = self._heading(s)
        dt = 0.1
        dh = (self._heading(s + self.speed * dt) - h + 180.0) % 360.0 - 180.0
        return n, e, h, dh / dt


@dataclass
class Preset:
    name: str
    entity_type: tuple
    center: tuple[float, float]
    speed_mps: float
    waypoints_ne: list[tuple[float, float]]
    radius_m: float = 0.0
    marking: str = ""


def _circle(radius: float, n: int = 36) -> list[tuple[float, float]]:
    return [
        (radius * math.cos(2 * math.pi * i / n), radius * math.sin(2 * math.pi * i / n))
        for i in range(n)
    ]


PRESETS: dict[str, Preset] = {
    "truck-loop": Preset(
        "truck-loop",
        TRUCK_TYPE,
        (37.795, -122.460),
        15.0,
        [(-100.0, -150.0), (100.0, -150.0), (100.0, 150.0), (-100.0, 150.0)],
        marking="TRUCK1",
    ),
    "boat-circle": Preset(
        "boat-circle",
        BOAT_TYPE,
        (37.815, -122.440),
        8.0,
        _circle(150.0, 180),
        150.0,
        "BOAT1",
    ),
}


def ne_to_latlon(
    center: tuple[float, float], n: float, e: float
) -> tuple[float, float]:
    lat0 = math.radians(center[0])
    m_per_deg_lat = 111_132.954 - 559.822 * math.cos(2 * lat0)
    m_per_deg_lon = 111_412.84 * math.cos(lat0)
    return center[0] + n / m_per_deg_lat, center[1] + e / m_per_deg_lon


@dataclass
class _Track:
    entity_id: int
    preset: Preset
    center: tuple[float, float]
    follower: PathFollower = field(init=False)
    last_sent: float = -1e9
    last_heading: float = 0.0

    def __post_init__(self):
        self.follower = PathFollower(self.preset.waypoints_ne, self.preset.speed_mps)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("preset", nargs="?", default="both", choices=["both", *PRESETS])
    ap.add_argument("--addr", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=3000)
    ap.add_argument("--exercise", type=int, default=1)
    ap.add_argument("--location", help="LAT,LON: re-centre the selected preset(s)")
    ap.add_argument(
        "--duration", type=float, default=0.0, help="seconds (0 = until Ctrl-C)"
    )
    ap.add_argument(
        "--rate", type=float, default=1.0 / HEARTBEAT_S, help="heartbeat Hz"
    )
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args(argv)

    names = list(PRESETS) if a.preset == "both" else [a.preset]
    tracks = []
    for i, name in enumerate(names, start=1):
        p = PRESETS[name]
        c = p.center
        if a.location:
            lat, lon = (float(v) for v in a.location.split(","))
            c = (
                lat + (0.002 * (i - 1)),
                lon,
            )  # keep two re-centred presets apart (~220 m)
        tracks.append(_Track(i, p, c))

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
    heartbeat = 1.0 / max(a.rate, 0.1)
    t0 = time.monotonic()
    print(
        f"sending {', '.join(names)} to {a.addr}:{a.port} (exercise {a.exercise}); Ctrl-C to stop"
    )
    try:
        while a.duration <= 0 or time.monotonic() - t0 < a.duration:
            t = time.monotonic() - t0
            for tr in tracks:
                n, e, h, rate = tr.follower.state(t)
                turned = (
                    abs((h - tr.last_heading + 180.0) % 360.0 - 180.0)
                    > HEADING_THRESHOLD
                )
                if t - tr.last_sent < heartbeat and not turned:
                    continue
                lat, lon = ne_to_latlon(tr.center, n, e)
                sock.sendto(
                    pack_entity_state(
                        tr.entity_id,
                        tr.preset.entity_type,
                        lat,
                        lon,
                        0.0,
                        h,
                        tr.preset.speed_mps,
                        rate,
                        a.exercise,
                        tr.preset.marking,
                        time.time(),
                    ),
                    (a.addr, a.port),
                )
                tr.last_sent, tr.last_heading = t, h
                if a.verbose:
                    print(
                        f"{t:7.2f} {tr.preset.name:12s} {lat:.6f} {lon:.6f} hdg {h:6.1f} rate {rate:6.2f}"
                    )
            time.sleep(1.0 / TICK_HZ)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
