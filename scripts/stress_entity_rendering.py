#!/usr/bin/env python3
"""
stress_entity_rendering.py — bulk entity spawn/update stress sender for CamSim.

This script sends CIGI 3.3 host frames that include one camera entity and a
large number of scene entities, useful for load-testing entity lifecycle and
transform throughput.
"""

import argparse
import math
import socket
import struct
import time


def pack_ig_control(frame_ctr: int) -> bytes:
    ts = int((time.time() % 86400) * 10_000) & 0xFFFF_FFFF
    return struct.pack(
        ">BBBbBxHIIII",
        1, 24, 3, 0, 0x05, 0x8000,
        frame_ctr & 0xFFFF_FFFF, ts, 0, 0,
    )


def pack_entity_control(
    entity_id: int,
    lat: float,
    lon: float,
    alt: float,
    yaw: float,
    pitch: float,
    roll: float,
    entity_state: int = 1,
    entity_type: int = 0,
) -> bytes:
    header = struct.pack(
        ">BBHBBBxHH",
        2, 48,
        entity_id & 0xFFFF,
        entity_state & 0x03,
        0,
        255,
        entity_type & 0xFFFF,
        0,
    )
    pkt = header + struct.pack(">fff", roll, pitch, yaw) + struct.pack(">ddd", lat, lon, alt)
    assert len(pkt) == 48
    return pkt


def camera_behind(center_lat: float, center_lon: float, center_alt: float, yaw_deg: float) -> tuple[float, float, float]:
    alt_above = 700.0
    pitch = -35.0
    dist_behind = alt_above / math.tan(math.radians(abs(pitch)))
    yaw_rad = math.radians(yaw_deg)
    dlat = -dist_behind * math.cos(yaw_rad) / 111320.0
    dlon = -dist_behind * math.sin(yaw_rad) / (111320.0 * math.cos(math.radians(center_lat)))
    return center_lat + dlat, center_lon + dlon, center_alt + alt_above


def grid_offsets(count: int, radius_m: float) -> list[tuple[float, float]]:
    side = max(1, int(math.ceil(math.sqrt(count))))
    step = (2.0 * radius_m) / max(1, side - 1)
    offsets = []
    for i in range(count):
        row = i // side
        col = i % side
        east = -radius_m + col * step
        north = -radius_m + row * step
        offsets.append((north, east))
    return offsets


def ring_offsets(count: int, radius_m: float) -> list[tuple[float, float]]:
    offsets = []
    for i in range(count):
        theta = 2.0 * math.pi * (i / max(1, count))
        ring = 1.0 + (i % 3) * 0.2
        r = radius_m * min(1.5, ring)
        north = math.cos(theta) * r
        east = math.sin(theta) * r
        offsets.append((north, east))
    return offsets


def send_chunked(
    sock: socket.socket,
    addr: tuple[str, int],
    frame_ctr: int,
    camera_pkt: bytes,
    entity_pkts: list[bytes],
    max_bytes: int,
) -> int:
    sent = 0
    base = pack_ig_control(frame_ctr) + camera_pkt
    datagram = base

    for pkt in entity_pkts:
        if len(datagram) + len(pkt) > max_bytes and len(datagram) > len(base):
            sock.sendto(datagram, addr)
            sent += 1
            datagram = base
        datagram += pkt

    if len(datagram) > len(base):
        sock.sendto(datagram, addr)
        sent += 1

    return sent


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8888)
    ap.add_argument("--camera-id", type=int, default=0)
    ap.add_argument("--start-entity-id", type=int, default=100)
    ap.add_argument("--count", type=int, default=100)
    ap.add_argument("--entity-type", type=int, default=1001)
    ap.add_argument("--center-lat", type=float, default=37.7749)
    ap.add_argument("--center-lon", type=float, default=-122.4194)
    ap.add_argument("--alt", type=float, default=900.0)
    ap.add_argument("--pattern", choices=["ring", "grid"], default="ring")
    ap.add_argument("--radius-m", type=float, default=500.0)
    ap.add_argument("--angular-rate-deg-s", type=float, default=5.0)
    ap.add_argument("--rate", type=float, default=10.0)
    ap.add_argument("--duration", type=float, default=30.0)
    ap.add_argument("--max-dgram-bytes", type=int, default=1300)
    ap.add_argument("--remove-on-exit", action="store_true")
    args = ap.parse_args()

    count = max(1, args.count)
    offsets = ring_offsets(count, args.radius_m) if args.pattern == "ring" else grid_offsets(count, args.radius_m)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    addr = (args.host, args.port)
    interval = 1.0 / max(1.0, args.rate)

    print(
        f"[stress] target={args.host}:{args.port} entities={count} type={args.entity_type} "
        f"pattern={args.pattern} rate={args.rate:.1f}Hz duration={args.duration:.1f}s"
    )

    start = time.monotonic()
    next_send = start
    frame_ctr = 0
    sent_datagrams = 0

    try:
        while True:
            elapsed = time.monotonic() - start
            if args.duration > 0 and elapsed >= args.duration:
                break

            cam_lat, cam_lon, cam_alt = camera_behind(args.center_lat, args.center_lon, args.alt, yaw_deg=0.0)
            camera_pkt = pack_entity_control(
                args.camera_id,
                cam_lat, cam_lon, cam_alt,
                0.0, -35.0, 0.0,
                entity_state=1,
                entity_type=0,
            )

            entity_pkts: list[bytes] = []
            omega = math.radians(args.angular_rate_deg_s)
            for i, (base_north, base_east) in enumerate(offsets):
                orbit_theta = omega * elapsed + (i * 2.0 * math.pi / count)
                north = base_north + math.cos(orbit_theta) * 30.0
                east = base_east + math.sin(orbit_theta) * 30.0
                lat = args.center_lat + north / 111320.0
                lon = args.center_lon + east / (111320.0 * math.cos(math.radians(args.center_lat)))
                yaw = math.degrees(orbit_theta) % 360.0
                entity_pkts.append(pack_entity_control(
                    args.start_entity_id + i,
                    lat, lon, args.alt,
                    yaw, 0.0, 0.0,
                    entity_state=1,
                    entity_type=args.entity_type,
                ))

            sent_datagrams += send_chunked(
                sock, addr, frame_ctr, camera_pkt, entity_pkts, max(512, args.max_dgram_bytes)
            )
            frame_ctr += 1

            if frame_ctr % max(1, int(args.rate)) == 0:
                print(f"  t={elapsed:6.1f}s frame={frame_ctr:5d} dgrams={sent_datagrams:6d}")

            next_send += interval
            sleep_for = next_send - time.monotonic()
            if sleep_for > 0:
                time.sleep(sleep_for)
    except KeyboardInterrupt:
        print()

    if args.remove_on_exit:
        print("[stress] sending remove packets...")
        cam_lat, cam_lon, cam_alt = camera_behind(args.center_lat, args.center_lon, args.alt, yaw_deg=0.0)
        camera_pkt = pack_entity_control(args.camera_id, cam_lat, cam_lon, cam_alt, 0.0, -35.0, 0.0, entity_state=1, entity_type=0)
        remove_pkts = [
            pack_entity_control(args.start_entity_id + i, args.center_lat, args.center_lon, args.alt,
                                0.0, 0.0, 0.0, entity_state=2, entity_type=args.entity_type)
            for i in range(count)
        ]
        for i in range(5):
            send_chunked(sock, addr, frame_ctr + i, camera_pkt, remove_pkts, max(512, args.max_dgram_bytes))
            time.sleep(0.05)

    sock.close()
    print(f"[stress] done frames={frame_ctr} datagrams={sent_datagrams}")


if __name__ == "__main__":
    main()
