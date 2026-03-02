#!/usr/bin/env python3
"""
test_entity_rendering.py — Phase 8 entity rendering test sender for CamSim.

Sends CIGI 3.3 UDP datagrams to exercise entity spawning, dead-reckoning,
articulated parts, and component control (lights, strobe, damage).

Every frame includes:
  • IG Control        (required header)
  • Entity Control    (camera entity, id=0 by default, keeps camera stationary)
  • <test packets>    (entity state + rate/artpart/comp ctrl as required by mode)

CIGI 3.3 packet IDs used here:
  1  IG Control
  2  Entity Control
  4  Component Control
  6  Articulated Part Control
  8  Rate Control

Usage:
    python3 scripts/test_entity_rendering.py <mode> [options]

Modes:
    spawn       Spawn a scene entity and hold it stationary.
    remove      Send Remove state to destroy a previously spawned entity.
    deadreckon  Spawn entity with velocity → watch it glide without further packets.
    artpart     Animate landing gear (ArtPart_00) open/close over 5 seconds.
    lights      Toggle nav lights and strobe on/off.
    damage      Cycle through intact → damaged → destroyed states.

Options:
    --host HOST         CamSim address (default: 127.0.0.1)
    --port PORT         CamSim CIGI port (default: 8888)
    --camera-id ID      CIGI entity ID for the camera (default: 0)
    --entity-id ID      CIGI entity ID for the scene entity (default: 1)
    --entity-type TYPE  CIGI entity type (maps to mesh in config) (default: 1001)
    --lat DEGREES       Entity latitude  (default: 37.7749)
    --lon DEGREES       Entity longitude (default: -122.4194)
    --alt METRES        Entity altitude  (default: 1000.0)
    --yaw DEGREES       Entity heading   (default: 0.0)
    --rate FPS          Send rate        (default: 30)
    --duration SEC      Duration 0=forever (default: 10)

Examples:
    # Spawn F-16 (type 1001) and hold for 30 s:
    python3 scripts/test_entity_rendering.py spawn --entity-type 1001 --duration 30

    # Spawn F-16 moving north at 200 m/s; watch dead-reckoning:
    python3 scripts/test_entity_rendering.py deadreckon --entity-type 1001 --duration 20

    # Toggle nav lights and strobe:
    python3 scripts/test_entity_rendering.py lights --duration 10

    # Cycle damage states every 5 seconds:
    python3 scripts/test_entity_rendering.py damage --duration 30

    # Remove entity from a previous session:
    python3 scripts/test_entity_rendering.py remove --entity-id 1
"""

import argparse
import math
import socket
import struct
import sys
import time


# ---------------------------------------------------------------------------
# Camera positioning helper
# ---------------------------------------------------------------------------

def camera_behind_entity(
    entity_lat: float, entity_lon: float, entity_alt: float,
    yaw_deg: float,
    alt_above: float = 500.0,
    pitch_deg: float = -30.0,
) -> tuple[float, float, float]:
    """
    Return (cam_lat, cam_lon, cam_alt) such that the camera is above and
    behind the entity along the given heading, with the entity centred in
    the camera's boresight at pitch_deg.

    Geometry: camera is alt_above metres above the entity and
    alt_above/tan(|pitch|) metres behind it, so the look-vector points
    exactly at the entity.
    """
    dist_behind = alt_above / math.tan(math.radians(abs(pitch_deg)))
    yaw_rad = math.radians(yaw_deg)
    # Offset in the opposite direction of yaw (behind entity)
    dlat = -dist_behind * math.cos(yaw_rad) / 111320.0
    dlon = -dist_behind * math.sin(yaw_rad) / (
        111320.0 * math.cos(math.radians(entity_lat))
    )
    return entity_lat + dlat, entity_lon + dlon, entity_alt + alt_above

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

CIGI_ENTITY_STANDBY = 0
CIGI_ENTITY_ACTIVE  = 1
CIGI_ENTITY_REMOVE  = 2


# ---------------------------------------------------------------------------
# Packet builders
# ---------------------------------------------------------------------------

def pack_ig_control(frame_ctr: int) -> bytes:
    """IG Control — packet ID 1, 24 bytes."""
    ig_mode = 0x05  # Operate(1) | TS_Valid(1<<2)
    ts = int((time.time() % 86400) * 10_000) & 0xFFFF_FFFF
    return struct.pack(">BBBbBxHIIII",
        1, 24, 3, 0, ig_mode, 0x8000,
        frame_ctr & 0xFFFF_FFFF, ts, 0, 0,
    )


def pack_entity_control(
    entity_id: int, lat: float, lon: float, alt: float,
    yaw: float, pitch: float, roll: float,
    entity_state: int = CIGI_ENTITY_ACTIVE,
    entity_type: int = 0,
) -> bytes:
    """Entity Control — packet ID 2, 48 bytes (CIGI 3.3).

    CCL Unpack byte layout (verified against CigiEntityCtrlV3::Unpack):
      0     packet_id
      1     size
      2-3   entity_id
      4     entity state / attach / clamp flags
      5     animation flags
      6     alpha
      7     reserved
      8-9   entity_type
      10-11 parent_id
      12-23 roll, pitch, yaw  (3× float32)
      24-47 lat, lon, alt     (3× float64)
    """
    state_byte = entity_state & 0x03
    header = struct.pack(">BBHBBBxHH",
        2, 48, entity_id, state_byte, 0,  # anim flags
        255,                               # alpha
        entity_type & 0xFFFF, 0,          # entity_type, parent
    )
    angles   = struct.pack(">fff", roll, pitch, yaw)
    position = struct.pack(">ddd", lat, lon, alt)
    pkt = header + angles + position
    assert len(pkt) == 48
    return pkt


def pack_rate_control(
    entity_id: int,
    x_rate: float = 0.0,   # m/s body-frame forward (North when yaw=0)
    y_rate: float = 0.0,   # m/s body-frame right
    z_rate: float = 0.0,   # m/s body-frame down
    roll_rate:  float = 0.0,  # deg/s
    pitch_rate: float = 0.0,  # deg/s
    yaw_rate:   float = 0.0,  # deg/s
    art_part_id: int = 0,
    apply_to_art_part: bool = False,
) -> bytes:
    """
    Rate Control — packet ID 8, 32 bytes (CIGI 3.3).

    Byte layout:
      0     uint8   Packet ID (8)
      1     uint8   Packet Size (32)
      2-3   uint16  Entity ID
      4     uint8   Art Part ID
      5     uint8   Apply to Art Part (bit 0)
      6-7   reserved
      8-11  float32 X Rate (m/s)
      12-15 float32 Y Rate (m/s)
      16-19 float32 Z Rate (m/s)
      20-23 float32 Roll Rate (deg/s)
      24-27 float32 Pitch Rate (deg/s)
      28-31 float32 Yaw Rate (deg/s)
    """
    flags = 0x01 if apply_to_art_part else 0x00
    pkt = struct.pack(">BBHBBxxffffff",
        8, 32, entity_id & 0xFFFF,
        art_part_id & 0xFF, flags,
        x_rate, y_rate, z_rate,
        roll_rate, pitch_rate, yaw_rate,
    )
    assert len(pkt) == 32
    return pkt


def pack_art_part_control(
    entity_id: int,
    art_part_id: int,
    art_part_en: bool = True,
    x_off: float | None = None,
    y_off: float | None = None,
    z_off: float | None = None,
    roll:  float | None = None,
    pitch: float | None = None,
    yaw:   float | None = None,
) -> bytes:
    """
    Articulated Part Control — packet ID 6, 32 bytes (CIGI 3.3).

    Byte layout:
      0     uint8   Packet ID (6)
      1     uint8   Packet Size (32)
      2-3   uint16  Entity ID
      4     uint8   Art Part ID
      5     uint8   Art Part En (bit 0) | X Off En (bit 1) | Y Off En (bit 2) |
                    Z Off En (bit 3) | Roll En (bit 4) | Pitch En (bit 5) |
                    Yaw En (bit 6)
      6-7   reserved
      8-11  float32 X Offset (m)
      12-15 float32 Y Offset (m)
      16-19 float32 Z Offset (m)
      20-23 float32 Roll (deg)
      24-27 float32 Pitch (deg)
      28-31 float32 Yaw (deg)
    """
    flags = 0x01 if art_part_en else 0x00
    if x_off  is not None: flags |= 0x02
    if y_off  is not None: flags |= 0x04
    if z_off  is not None: flags |= 0x08
    if roll   is not None: flags |= 0x10
    if pitch  is not None: flags |= 0x20
    if yaw    is not None: flags |= 0x40

    pkt = struct.pack(">BBHBBxxffffff",
        6, 32, entity_id & 0xFFFF,
        art_part_id & 0xFF, flags,
        x_off  or 0.0, y_off  or 0.0, z_off  or 0.0,
        roll   or 0.0, pitch  or 0.0, yaw    or 0.0,
    )
    assert len(pkt) == 32
    return pkt


def pack_component_control(
    instance_id: int,   # entity ID for CompClass=0
    comp_id: int,
    comp_state: int,
    comp_class: int = 0,  # 0 = entity
) -> bytes:
    """
    Component Control — packet ID 4, 32 bytes (CIGI 3.3).

    Byte layout:
      0     uint8   Packet ID (4)
      1     uint8   Packet Size (32)
      2-3   uint16  Instance ID
      4-5   uint16  Component ID
      6     uint8   Component Class (0=entity)
      7     uint8   Component State
      8-31  24 bytes Component Data (unused, zeroed)
    """
    pkt = struct.pack(">BBHHBBxxxxxxxxxxxxxxxxxxxxxxxx",
        4, 32,
        instance_id & 0xFFFF,
        comp_id & 0xFFFF,
        comp_class & 0xFF,
        comp_state & 0xFF,
    )
    assert len(pkt) == 32
    return pkt


# ---------------------------------------------------------------------------
# Send loop helpers
# ---------------------------------------------------------------------------

def send_frame(sock, addr, frame_ctr, packets: list[bytes]):
    """Assemble a CIGI datagram (IG Control + supplied packets) and send it."""
    datagram = pack_ig_control(frame_ctr)
    for p in packets:
        datagram += p
    sock.sendto(datagram, addr)


def run_loop(sock, addr, args, frame_builder):
    """
    Generic send loop.  frame_builder(elapsed, frame_ctr) returns a list of
    bytes objects to include after the camera entity control.
    """
    interval   = 1.0 / args.rate
    start      = time.monotonic()
    next_send  = start
    frame_ctr  = 0

    _CAM_PITCH = -90.0
    cam_lat, cam_lon, cam_alt = camera_behind_entity(
        args.lat, args.lon, args.alt, args.yaw,
        alt_above=500.0, pitch_deg=_CAM_PITCH,
    )
    camera_entity = pack_entity_control(
        args.camera_id,
        cam_lat, cam_lon, cam_alt,
        args.yaw, _CAM_PITCH, 0.0,
    )

    try:
        while True:
            elapsed = time.monotonic() - start
            if args.duration > 0 and elapsed >= args.duration:
                break

            extra_packets = frame_builder(elapsed, frame_ctr)
            send_frame(sock, addr, frame_ctr, [camera_entity] + extra_packets)

            frame_ctr += 1
            next_send += interval
            sleep_for  = next_send - time.monotonic()
            if sleep_for > 0:
                time.sleep(sleep_for)

    except KeyboardInterrupt:
        print()


# ---------------------------------------------------------------------------
# Modes
# ---------------------------------------------------------------------------

def mode_spawn(sock, addr, args):
    """Spawn entity at configured lat/lon/alt and hold stationary."""
    print(f"[spawn] entity_id={args.entity_id} type={args.entity_type} "
          f"lat={args.lat:.4f} lon={args.lon:.4f} alt={args.alt:.0f}m")
    print("        Watching for entity to appear in scene...")

    entity_pkt = pack_entity_control(
        args.entity_id, args.lat, args.lon, args.alt,
        args.yaw, 0.0, 0.0,
        entity_state=CIGI_ENTITY_ACTIVE,
        entity_type=args.entity_type,
    )

    def builder(elapsed, frame_ctr):
        return [entity_pkt]

    run_loop(sock, addr, args, builder)
    print("[spawn] done — entity remains in scene until Remove is sent")


def mode_remove(sock, addr, args):
    """Send Remove state once then exit."""
    print(f"[remove] entity_id={args.entity_id}")

    remove_pkt = pack_entity_control(
        args.entity_id, args.lat, args.lon, args.alt,
        0.0, 0.0, 0.0,
        entity_state=CIGI_ENTITY_REMOVE,
    )

    cam_lat, cam_lon, cam_alt = camera_behind_entity(
        args.lat, args.lon, args.alt, args.yaw)
    camera_entity = pack_entity_control(
        args.camera_id,
        cam_lat, cam_lon, cam_alt,
        args.yaw, -30.0, 0.0,
    )

    # Send remove packet for 1 second to ensure it's received
    start     = time.monotonic()
    frame_ctr = 0
    interval  = 1.0 / args.rate
    next_send = start

    try:
        while time.monotonic() - start < 1.0:
            send_frame(sock, addr, frame_ctr, [camera_entity, remove_pkt])
            frame_ctr += 1
            next_send += interval
            sleep_for  = next_send - time.monotonic()
            if sleep_for > 0:
                time.sleep(sleep_for)
    except KeyboardInterrupt:
        pass

    print("[remove] Remove packet sent — entity should be destroyed")


def mode_deadreckon(sock, addr, args):
    """
    8C test: Spawn entity at lat/lon, send one Rate Control burst,
    then stop sending entity updates and watch dead-reckoning glide it north.

    Phase 1 (0-2s):   Send entity state + rate control at full rate.
    Phase 2 (2-end):  Stop sending entity updates — DR takes over in-engine.
    """
    print(f"[deadreckon] entity_id={args.entity_id} — 200 m/s forward (north at yaw=0)")
    print("             Phase 1 (0-2s): updates + rate ctrl")
    print("             Phase 2 (2s+):  no updates — watch entity glide north")

    rate_pkt = pack_rate_control(
        args.entity_id,
        x_rate=200.0,   # 200 m/s body-forward → north when yaw=0
        y_rate=0.0,
        z_rate=0.0,
        yaw_rate=0.0,
    )

    def builder(elapsed, frame_ctr):
        if elapsed < 2.0:
            entity_pkt = pack_entity_control(
                args.entity_id, args.lat, args.lon, args.alt,
                args.yaw, 0.0, 0.0,
                entity_state=CIGI_ENTITY_ACTIVE,
                entity_type=args.entity_type,
            )
            return [entity_pkt, rate_pkt]
        else:
            # Stop sending entity updates — engine dead-reckons
            if frame_ctr % int(args.rate * 5) == 0 and elapsed >= 2.0:
                dt = elapsed - 2.0
                approx_north_m = 200.0 * dt
                deg_north = approx_north_m / 111320.0
                print(f"  [{elapsed:5.1f}s] DR active — ~{approx_north_m:.0f}m north "
                      f"(~{deg_north:.4f}° lat offset expected)")
            return []

    run_loop(sock, addr, args, builder)


def mode_artpart(sock, addr, args):
    """
    8D test: Animate ArtPart_00 (landing gear) — extend (XOff=-2) then retract (XOff=0).
    Cycles every 5 seconds.
    """
    print(f"[artpart] entity_id={args.entity_id} — ArtPart_00 (landing gear) oscillate")
    print("          Extended XOff=-2.0 for 5s, retracted XOff=0.0 for 5s")

    entity_pkt = pack_entity_control(
        args.entity_id, args.lat, args.lon, args.alt,
        args.yaw, 0.0, 0.0,
        entity_state=CIGI_ENTITY_ACTIVE,
        entity_type=args.entity_type,
    )

    def builder(elapsed, frame_ctr):
        gear_extended = (elapsed % 10.0) < 5.0
        x_off = -2.0 if gear_extended else 0.0
        art_pkt = pack_art_part_control(
            args.entity_id,
            art_part_id=0,   # ArtPart_00 = landing gear
            art_part_en=True,
            x_off=x_off,
        )
        if frame_ctr % int(args.rate * 2) == 0:
            state = "EXTENDED (XOff=-2.0)" if gear_extended else "RETRACTED (XOff=0.0)"
            print(f"  [{elapsed:5.1f}s] landing gear: {state}")
        return [entity_pkt, art_pkt]

    run_loop(sock, addr, args, builder)


def mode_lights(sock, addr, args):
    """
    8E test: Toggle nav lights (CompId=0) and strobe (CompId=1) every 3 seconds.
    """
    print(f"[lights] entity_id={args.entity_id} — nav lights + strobe toggle every 3s")

    entity_pkt = pack_entity_control(
        args.entity_id, args.lat, args.lon, args.alt,
        args.yaw, 0.0, 0.0,
        entity_state=CIGI_ENTITY_ACTIVE,
        entity_type=args.entity_type,
    )

    prev_phase = -1

    def builder(elapsed, frame_ctr):
        nonlocal prev_phase
        # Toggle every 3 seconds
        phase = int(elapsed / 3.0) % 4  # 4 phases: lights off/on, strobe off/on
        lights_on = (phase in (1, 3))
        strobe_on = (phase in (2, 3))

        if phase != prev_phase:
            prev_phase = phase
            print(f"  [{elapsed:5.1f}s] nav lights={'ON ' if lights_on else 'OFF'}  "
                  f"strobe={'ON' if strobe_on else 'OFF'}")

        comp_nav = pack_component_control(
            args.entity_id, comp_id=0, comp_state=1 if lights_on else 0)
        comp_strobe = pack_component_control(
            args.entity_id, comp_id=1, comp_state=1 if strobe_on else 0)

        return [entity_pkt, comp_nav, comp_strobe]

    run_loop(sock, addr, args, builder)


def mode_damage(sock, addr, args):
    """
    8E test: Cycle entity through intact → damaged → destroyed every 5 seconds.
    Uses CompId=10 (damage state).
    """
    print(f"[damage] entity_id={args.entity_id} — cycle damage states every 5s")
    print("         Ensure entity_types config has mesh_damaged/mesh_destroyed paths")

    entity_pkt = pack_entity_control(
        args.entity_id, args.lat, args.lon, args.alt,
        args.yaw, 0.0, 0.0,
        entity_state=CIGI_ENTITY_ACTIVE,
        entity_type=args.entity_type,
    )

    _STATE_NAMES = {0: "INTACT", 1: "DAMAGED", 2: "DESTROYED"}
    prev_state = -1

    def builder(elapsed, frame_ctr):
        nonlocal prev_state
        damage_state = int(elapsed / 5.0) % 3  # 0→1→2→0…

        if damage_state != prev_state:
            prev_state = damage_state
            print(f"  [{elapsed:5.1f}s] damage state → {_STATE_NAMES[damage_state]} ({damage_state})")

        comp_pkt = pack_component_control(
            args.entity_id, comp_id=10, comp_state=damage_state)

        return [entity_pkt, comp_pkt]

    run_loop(sock, addr, args, builder)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

_MODES = {
    "spawn":      mode_spawn,
    "remove":     mode_remove,
    "deadreckon": mode_deadreckon,
    "artpart":    mode_artpart,
    "lights":     mode_lights,
    "damage":     mode_damage,
}


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("mode", choices=list(_MODES),
                    help="Test scenario to run")
    ap.add_argument("--host",        default="127.0.0.1")
    ap.add_argument("--port",        type=int, default=8888)
    ap.add_argument("--camera-id",   type=int, default=0,    dest="camera_id",
                    help="CIGI entity ID for the camera (must match camera_entity_id in config)")
    ap.add_argument("--entity-id",   type=int, default=1,    dest="entity_id",
                    help="CIGI entity ID for the scene entity")
    ap.add_argument("--entity-type", type=int, default=1001, dest="entity_type",
                    help="CIGI entity type (must be in entity_types config)")
    ap.add_argument("--lat",         type=float, default=37.7749)
    ap.add_argument("--lon",         type=float, default=-122.4194)
    ap.add_argument("--alt",         type=float, default=1000.0)
    ap.add_argument("--yaw",         type=float, default=0.0)
    ap.add_argument("--rate",        type=float, default=30.0)
    ap.add_argument("--duration",    type=float, default=10.0,
                    help="Duration in seconds (0=forever)")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    addr = (args.host, args.port)

    print(f"==> Phase 8 entity test: mode={args.mode}  target={args.host}:{args.port}")
    print(f"    camera_id={args.camera_id}  entity_id={args.entity_id}  "
          f"entity_type={args.entity_type}  duration={args.duration:.0f}s")
    print()

    _MODES[args.mode](sock, addr, args)

    sock.close()


if __name__ == "__main__":
    main()
