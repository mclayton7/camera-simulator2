#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""
send_cigi_test.py — CIGI 3.3 test packet sender for CamSim.

Sends Host→IG CIGI 3.3 UDP datagrams containing:
  • IG Control packet       (required first packet in every host frame)
  • Entity Control packet   (moves the camera to a geospatial position)
  • View Definition packet  (optional, sets FOV)
  • View Control packet     (optional, --gimbal: airframe-relative gimbal angles)

Each datagram represents one "host frame" at the specified rate.

CIGI 3.3 references:
  • CIGI ICD version 3.3, 15 June 2004 (CIGI_ICD_3.3.pdf)
  • Packet IDs: IG Control=1, Entity Control=2, View Control=16, View Definition=21

Usage:
    uv run scripts/send_cigi_test.py [options]

Options:
    --host HOST         CamSim CIGI listen address  (default: 127.0.0.1)
    --port PORT         CamSim CIGI listen port      (default: 8888)
    --lat  DEGREES      Initial latitude             (default: 37.7749)
    --lon  DEGREES      Initial longitude            (default: -122.4194)
    --alt  METRES       Initial altitude MSL         (default: 1000.0)
    --yaw  DEGREES      Initial heading [0,360)      (default: 0.0)
    --pitch DEGREES     Initial pitch                (default: -15.0)
    --roll  DEGREES     Initial roll                 (default: 0.0)
    --rate FPS          Packets per second           (default: 30)
    --duration SEC      Total send duration (0=∞)    (default: 0)
    --sweep             Slowly rotate heading 0→360 for a pan test
    --circle LAT LON    Orbit a point (overrides --lat/--lon during send)
    --tour              Automatically cycle through a sequence of camera poses
                        (altitude dives, banking turns, pitch sweeps, rolls).
                        Loops forever; good for hands-off video validation.
    --tour-speed SCALE  Speed multiplier for --tour (default: 1.0; 2.0 = twice
                        as fast, 0.5 = half speed)
    --entity-id ID      CIGI entity ID for the camera (default: 1)
    --sensor-id ID      Sensor waveband: 0=EO (color), 1=IR (thermal) (default: 0;
                        other values are unmapped and default to EO)
    --polarity POL      IR polarity: 0=WhiteHot, 1=BlackHot (default: 0; IR mode only)
    --fov-h DEGREES     Horizontal FOV for View Definition (default: 60)
    --gimbal YAW PITCH ROLL  View Control angles relative to the airframe, degrees
                        (yaw right +, pitch up +, roll right +; yaw sent within ±180)
    --time HHMM         Time of day (e.g. 0600 for sunrise, 2200 for night)
    --visibility METRES Visibility range (e.g. 500 for dense fog)
    --weather           Enable basic overcast weather layer
    --cloud-base METRES     Cloud layer base altitude in metres MSL (default: 2000)
    --cloud-thickness METRES Cloud layer thickness in metres (default: 500)
    --time-sweep        Cycle time-of-day 0000→2400 (use with --tour)
    --time-sweep-period SEC  Seconds for one full day/night cycle (default: 120)
    --hat-extended LAT,LON   Extended HAT/HOT request (repeatable); prints HAT, HOT and the
                        material code (docs/terrain-feedback.md) from each 103 response
    --los-extended LAT,LON[,ALT]  Extended LOS segment from the sensor to the point (ALT default
                        -500 m, below the ground; repeatable); prints the hit and its material
    --query-interval SEC     Resend period of the queries (default: 2)
    --response-port PORT     Port CamSim sends responses to (default: 8889)

Examples:
    # Hover over San Francisco airport at 500 m, looking down at -30°:
    uv run scripts/send_cigi_test.py --lat 37.6213 --lon -122.379 --alt 500 --pitch -30

    # Sweep heading slowly (good for visual validation):
    uv run scripts/send_cigi_test.py --sweep --duration 30

    # Orbit a point over 60 seconds:
    uv run scripts/send_cigi_test.py --circle 37.7749 -122.4194 --alt 2000 --duration 60

    # Automated tour (loops until Ctrl-C):
    uv run scripts/send_cigi_test.py --tour
    uv run scripts/send_cigi_test.py --tour --tour-speed 2.0 --alt 2000

    # Sunrise with fog:
    uv run scripts/send_cigi_test.py --time 0600 --visibility 2000

    # Night scene:
    uv run scripts/send_cigi_test.py --time 2200

    # Day/night cycle during a tour:
    uv run scripts/send_cigi_test.py --tour --time-sweep --time-sweep-period 60

    # Overcast weather:
    uv run scripts/send_cigi_test.py --weather

    # Gimbal sign test (HITL.md phase 0): heading 0, level, camera yaw +90 pitch -45
    # -> the frame centre should be due east, about one altitude away:
    uv run scripts/send_cigi_test.py --yaw 0 --pitch 0 --gimbal 90 -45 0

    # Material codes (REALISM R2) at a point on I-5, with an LOS from 300 m above it:
    uv run scripts/send_cigi_test.py --lat 33.2994 --lon -117.4655 --alt 300 --pitch -90 \\
        --hat-extended 33.299358,-117.465474 --los-extended 33.299358,-117.465474

    # Low stratus at 500 m base, 200 m thick:
    uv run scripts/send_cigi_test.py --weather --cloud-base 500 --cloud-thickness 200

    # High cirrus at 8000 m base, 1000 m thick:
    uv run scripts/send_cigi_test.py --weather --cloud-base 8000 --cloud-thickness 1000
"""

import argparse
import math
import socket
import struct
import time

# ---------------------------------------------------------------------------
# CIGI 3.3 packet construction
# ---------------------------------------------------------------------------


def pack_ig_control(frame_ctr: int, db_number: int = 0) -> bytes:
    """
    IG Control packet -- Packet ID 1, Size 24 bytes (CIGI 3.3).

    Byte layout (CIGI 3.3 ICD):
      0     uint8   Packet ID (0x01)
      1     uint8   Packet Size (0x18 = 24)
      2     uint8   Major Version (0x03)
      3     int8    Database Number
      4     uint8   IG Mode (bits 0-1) | Timestamp Valid (bit 2) |
                    Smoothing Enable (bit 3) | Minor Version (bits 4-7)
      5     uint8   reserved
      6-7   uint16  Byte Swap Magic (0x8000 in sender byte order)
      8-11  uint32  Host Frame Counter
      12-15 uint32  Timestamp (10-microsecond ticks since midnight UTC)
      16-19 uint32  Last Received IG Frame
      20-23         reserved (zero)
    """
    # IG Mode = 1 (Operate), timestamp valid = 1 (bit 2), smoothing = 0,
    # Minor Version = 3 (bits 4-7) identifies a CIGI 3.3 host; CCL reads 2 as 3.2
    # and 0 as 3.0.
    ig_mode_byte = 0x35  # Minor(3<<4) | TS_Valid(1<<2) | Operate(1)

    # Byte Swap Magic: 0x8000 in sender byte order.
    # We use big-endian ('>'), so on-wire bytes 6-7 = 0x80 0x00.
    # The CCL receiver compares against 0x8000 in its native order to
    # detect whether byte swapping is needed.
    BYTE_SWAP_MAGIC = 0x8000

    # 10-us ticks since midnight UTC (ICD 4.1.1: Timestamp units are 10 us)
    ts = int((time.time() % 86400) * 100_000) & 0xFFFF_FFFF

    return struct.pack(
        ">BBBbBxHIIII",
        1,  # byte  0: Packet ID
        24,  # byte  1: Packet Size (V3.3 = 24)
        3,  # byte  2: Major Version
        db_number,  # byte  3: Database Number (signed int8)
        ig_mode_byte,  # byte  4: IG Mode flags
        #               # byte  5: reserved (x)
        BYTE_SWAP_MAGIC,  # bytes 6-7: Byte Swap Magic
        frame_ctr & 0xFFFF_FFFF,  # bytes 8-11: Host Frame Counter
        ts,  # bytes 12-15: Timestamp
        0,  # bytes 16-19: Last Received IG Frame
        0,  # bytes 20-23: reserved
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
      8-9   entity_type  (always 0 for the camera entity)
      10-11 parent_id
      12-23 roll, pitch, yaw  (3× float32)
      24-47 lat, lon, alt     (3× float64)
    """
    state_byte = entity_state & 0x03

    header = struct.pack(
        ">BBHBBBxHH",
        2,  # packet_id
        48,  # size
        entity_id,
        state_byte,  # entity state / flags
        0,  # animation flags
        255,  # alpha
        0,  # entity_type (camera entity — type irrelevant)
        0,  # parent_entity_id
    )
    angles = struct.pack(">fff", roll, pitch, yaw)
    position = struct.pack(">ddd", lat, lon, alt)

    pkt = header + angles + position
    assert len(pkt) == 48, f"Entity Control packet size error: {len(pkt)}"
    return pkt


def pack_view_definition(
    view_id: int = 0,
    group_id: int = 0,
    fov_left: float = -30.0,
    fov_right: float = 30.0,
    fov_top: float = 16.875,  # ~16.875° for 16:9 ratio with 33.75° VFOV
    fov_bottom: float = -16.875,
    near_plane: float = 0.1,
    far_plane: float = 1_000_000.0,
) -> bytes:
    """
    View Definition packet — Packet ID 21, Size 32 bytes.

    Byte layout (CIGI 3.3 ICD §4.1.16):
      0     uint8   Packet ID (0x15 = 21)
      1     uint8   Packet Size (0x20 = 32)
      2-3   uint16  View ID
      4     uint8   Group ID
      5     uint8   Near (0x01) | Far (0x02) | Left (0x04) | Right (0x08) |
                    Top (0x10) | Bottom (0x20) enables | Mirror Mode (bits 6-7)
                    (CigiViewDefV3::Pack; CamSim reads only Right - Left)
      6     uint8   Pixel Replicate (0-2) | Projection (3) | Reorder (4) |
                    View Type (5-7)
      7     uint8   reserved
      8-11  float32 Near Plane (metres)
      12-15 float32 Far Plane  (metres)
      16-19 float32 FOV Left   (degrees, negative = left of boresight)
      20-23 float32 FOV Right  (degrees, positive = right of boresight)
      24-27 float32 FOV Top    (degrees, positive = above boresight)
      28-31 float32 FOV Bottom (degrees, negative = below boresight)
    """
    # Enable near/far and all four FOV fields
    flags = 0x3F

    header = struct.pack(
        ">BBHBBxx",
        21,  # packet_id
        32,  # size
        view_id,
        group_id,
        flags,
    )
    planes_fov = struct.pack(
        ">ffffff",
        near_plane,
        far_plane,
        fov_left,
        fov_right,
        fov_top,
        fov_bottom,
    )
    pkt = header + planes_fov
    assert len(pkt) == 32, f"View Definition size error: {len(pkt)}"
    return pkt


def pack_view_control(
    yaw: float,
    pitch: float,
    roll: float = 0.0,
    view_id: int = 0,
    group_id: int = 0,
    entity_id: int = 0,
) -> bytes:
    """
    View Control packet — Packet ID 16, Size 32 bytes (CIGI 3.3).

    Byte layout (CigiViewCtrlV3::Pack):
      0     uint8   Packet ID (0x10 = 16)
      1     uint8   Packet Size (0x20 = 32)
      2-3   uint16  View ID
      4     uint8   Group ID
      5     uint8   X/Y/Z Offset Enable (bits 0-2) | Roll/Pitch/Yaw Enable (bits 3-5)
      6-7   uint16  Entity ID (CamSim: 0; non-zero takes the first-person-view path)
      8-19  float32 X, Y, Z offsets (stored, never used by CamSim)
      20-31 float32 Roll, Pitch, Yaw (degrees, relative to the camera platform)

    Yaw is wrapped to [-180, 180): CamSim clamps it to +-180 without wrapping.
    """
    yaw = (yaw + 180.0) % 360.0 - 180.0
    pkt = struct.pack(
        ">BBHBBHffffff",
        16,
        32,
        view_id & 0xFFFF,
        group_id & 0xFF,
        0x38,  # roll | pitch | yaw enable; offsets disabled
        entity_id & 0xFFFF,
        0.0,
        0.0,
        0.0,
        roll,
        pitch,
        yaw,
    )
    assert len(pkt) == 32, f"View Control size error: {len(pkt)}"
    return pkt


def pack_sensor_control(
    sensor_id: int = 0,
    view_id: int = 0,
    sensor_on: bool = True,
    polarity: int = 0,
    gain: float = 0.0,
) -> bytes:
    """
    Sensor Control packet — Packet ID 17, Size 24 bytes (CIGI 3.3).

    Byte layout verified against CigiSensorCtrlV3::Unpack() disassembly:
      0     uint8   Packet ID (0x11 = 17)
      1     uint8   Packet Size (0x18 = 24)
      2-3   uint16  View ID          (NO Entity ID in V3)
      4     uint8   Sensor ID
      5     uint8   Sensor On/Off (bit 0) | Polarity (bit 1) | Line-by-line dropout (bit 2) |
                    Auto Gain (bit 3) | Track Polarity (bit 4) | Track Mode (bits 5-7)
      6     uint8   Response Type (bit 0) | reserved (bits 1-7)
      7     uint8   reserved
      8-11  float32 Gain
      12-15 float32 Level
      16-19 float32 AC Coupling
      20-23 float32 Noise
    """
    flags = 0
    if sensor_on:
        flags |= 0x01
    flags |= (polarity & 0x01) << 1

    return struct.pack(
        ">BBHBBBxffff",
        17,  # Packet ID
        24,  # Packet Size
        view_id & 0xFFFF,
        sensor_id & 0xFF,
        flags,
        0,  # ResponseType = GatePos (0)
        gain,
        0.0,  # Level
        0.0,  # AC Coupling
        0.0,  # Noise
    )


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
    pkt = struct.pack(
        ">BBHBBxxffffff",
        6,
        32,
        entity_id & 0xFFFF,
        art_part_id & 0xFF,
        flags,
        0.0,
        0.0,
        0.0,  # x/y/z offsets (not enabled)
        roll,
        pitch,
        yaw,
    )
    assert len(pkt) == 32
    return pkt


def pack_celestial_control(
    hour: int = 12,
    minute: int = 0,
    month: int = 6,
    day: int = 21,
    year: int = 2024,
    ephemeris_en: bool = True,
    sun_en: bool = True,
    moon_en: bool = True,
    star_en: bool = True,
    star_intensity: float = 100.0,
) -> bytes:
    """
    Celestial Sphere Control packet — Packet ID 9, Size 16 bytes (CIGI 3.3).

    Defaults enable everything a night test needs (ROADMAP 6A): sun, moon and the star field at 100 %. CamSim's stars
    follow the host after its first Celestial Sphere Control, so a packet with Star Field Enable 0 turns them off.

    Byte layout (CIGI 3.3 ICD §4.1.9):
      0     uint8   Packet ID (0x09)
      1     uint8   Packet Size (0x10 = 16)
      2     uint8   Hour (0-23)
      3     uint8   Minute (0-59)
      4     uint8   Ephemeris En (bit 0) | Sun En (bit 1) | Moon En (bit 2) |
                    Star Field En (bit 3) | Date Valid (bit 4)
      5-7           reserved
      8-11  uint32  Date, MMDDYYYY = month * 1000000 + day * 10000 + year
      12-15 float32 Star Field Intensity (percent)
    """
    flags = 0
    if ephemeris_en:
        flags |= 0x01
    if sun_en:
        flags |= 0x02
    if moon_en:
        flags |= 0x04
    if star_en:
        flags |= 0x08
    flags |= 0x10  # Date Valid

    return struct.pack(
        ">BBBBBxxxIf",
        9,  # Packet ID
        16,  # Packet Size
        hour & 0xFF,
        minute & 0xFF,
        flags,
        month * 1_000_000 + day * 10_000 + year,
        float(star_intensity),  # Star Field Intensity, percent
    )


def pack_atmos_control(
    visibility: float = 50000.0,
    humidity: float = 30.0,
    air_temp: float = 20.0,
    baro_press: float = 1013.25,
    horiz_wind_sp: float = 0.0,
    vert_wind_sp: float = 0.0,
    wind_dir: float = 0.0,
    atmos_en: bool = True,
) -> bytes:
    """
    Atmosphere Control packet — Packet ID 10, Size 32 bytes (CIGI 3.3).

    Byte layout (CIGI 3.3 ICD §4.1.10):
      0     uint8   Packet ID (0x0A)
      1     uint8   Packet Size (0x20 = 32)
      2     uint8   Atmospheric Model En (bit 0)
      3     uint8   Global Humidity (%)
      4-7   float32 Air Temp (°C)
      8-11  float32 Visibility Range (m)
      12-15 float32 Horiz Wind Speed (m/s)
      16-19 float32 Vert Wind Speed (m/s)
      20-23 float32 Wind Direction (°)
      24-27 float32 Barometric Pressure (mb)
      28-31         reserved
    """
    flags = 0x01 if atmos_en else 0x00

    return struct.pack(
        ">BBBBffffffxxxx",
        10,  # Packet ID
        32,  # Packet Size
        flags,
        max(0, min(100, round(humidity))),  # uint8 percent
        air_temp,
        visibility,
        horiz_wind_sp,
        vert_wind_sp,
        wind_dir,
        baro_press,
    )


def pack_weather_control(
    coverage: float = 0.0,
    base_elev: float = 2000.0,
    thickness: float = 500.0,
    cloud_type: int = 0,
    severity: int = 0,
    layer_id: int = 0,
    region_id: int = 0,
    weather_en: bool = True,
    visibility_rng: float = 50000.0,
    scope: int = 0,
    transition: float = 500.0,
) -> bytes:
    """
    Weather Control packet — Packet ID 12, Size 56 bytes (CIGI 3.3).

    Byte layout (CIGI 3.3 ICD §4.1.12):
      0     uint8   Packet ID (0x0C)
      1     uint8   Packet Size (0x38 = 56)
      2-3   uint16  Region ID
      4     uint8   Layer ID
      5     uint8   Humidity (unused, set 0)
      6     uint8   Weather Enable (bit 0) | Scud Enable (bit 1) |
                    Random Winds En (bit 2) | Random Lightning En (bit 3) |
                    Cloud Type (bits 4-7)
      7     uint8   Scope (bits 0-1) | Severity (bits 2-4)
      8-11  float32 Air Temp (°C, 0.0)
      12-15 float32 Visibility Range (m)
      16-19 float32 Scud Frequency (0.0)
      20-23 float32 Coverage (%)
      24-27 float32 Base Elevation (m MSL)
      28-31 float32 Thickness (m)
      32-35 float32 Transition Band (m)
      36-39 float32 Horiz Wind Speed (m/s)
      40-43 float32 Vert Wind Speed (m/s)
      44-47 float32 Wind Direction (°)
      48-51 float32 Barometric Pressure (mb, 1013.25)
      52-55 float32 Aerosol Concentration (0.0)
    """
    flags = 0x00
    if weather_en:
        flags |= 0x01
    flags |= (cloud_type & 0x0F) << 4

    scope_sev = (scope & 0x03) | ((severity & 0x07) << 2)

    return struct.pack(
        ">BBHBBBBffffffffffff",
        12,  # Packet ID
        56,  # Packet Size
        region_id & 0xFFFF,
        layer_id & 0xFF,
        0,  # Humidity (unused)
        flags,
        scope_sev,
        0.0,  # Air Temp
        visibility_rng,
        0.0,  # Scud Frequency
        coverage,
        base_elev,
        thickness,
        transition,
        0.0,  # Horiz Wind Speed
        0.0,  # Vert Wind Speed
        0.0,  # Wind Direction
        1013.25,  # Barometric Pressure
        0.0,  # Aerosol Concentration
    )


def build_host_frame(
    frame_ctr: int,
    entity_id: int,
    lat: float,
    lon: float,
    alt: float,
    yaw: float,
    pitch: float,
    roll: float,
    fov_h: float = 60.0,
    include_view_def: bool = True,
    sensor_id: int = 0,
    polarity: int = 0,
    celestial: dict | None = None,
    atmosphere: dict | None = None,
    weather: dict | None = None,
    gimbal: tuple[float, float, float] | None = None,
) -> bytes:
    """Assemble a complete CIGI 3.3 host frame datagram.

    gimbal = (yaw, pitch, roll) degrees adds a View Control packet."""
    ig_ctrl = pack_ig_control(frame_ctr)
    entity = pack_entity_control(entity_id, lat, lon, alt, yaw, pitch, roll)

    payload = ig_ctrl + entity

    # Send Sensor Control every frame so the IG always knows the active waveband
    payload += pack_sensor_control(
        sensor_id=sensor_id,
        view_id=0,
        sensor_on=True,
        polarity=polarity,
    )

    if gimbal is not None:
        payload += pack_view_control(*gimbal)

    if include_view_def:
        half_h = fov_h / 2.0
        half_v = half_h * (9.0 / 16.0)  # 16:9 aspect
        view = pack_view_definition(
            fov_left=-half_h,
            fov_right=half_h,
            fov_top=half_v,
            fov_bottom=-half_v,
        )
        payload += view

    if celestial is not None:
        payload += pack_celestial_control(**celestial)
    if atmosphere is not None:
        payload += pack_atmos_control(**atmosphere)
    if weather is not None:
        payload += pack_weather_control(**weather)

    return payload


# ---------------------------------------------------------------------------
# Waypoint / motion helpers
# ---------------------------------------------------------------------------

# Tour mode: a sequence of (name, yaw_deg, pitch_deg, roll_deg, alt_factor).
# alt_factor is multiplied by the base --alt value to get the target altitude.
# Segments are traversed in order, cycling back to the first after the last.
_TOUR_POSES = [
    ("bird-eye", 0, -80, 0, 2.5),  # high altitude, near-vertical
    ("glide-north", 10, -20, 0, 1.0),  # low, flat glide heading north
    ("bank-east", 90, -35, 20, 1.5),  # medium, banking turn to east
    ("dive-south", 185, -60, 0, 0.7),  # steep dive heading south
    ("pan-west", 270, -25, -20, 1.8),  # sweeping west, gentle left bank
    ("roll-ne", 45, -15, 40, 1.2),  # heavy right roll, low pitch
]
_TOUR_SEGMENT_SEC = 6.0  # seconds per segment at tour_speed=1.0


def _coslerp(a: float, b: float, t: float) -> float:
    """Cosine-smoothed interpolation from a to b with t in [0, 1]."""
    f = (1.0 - math.cos(t * math.pi)) / 2.0
    return a * (1.0 - f) + b * f


def _lerp_angle(a: float, b: float, t: float) -> float:
    """Cosine-smoothed interpolation between two angles (degrees) via shortest arc."""
    diff = ((b - a) + 180.0) % 360.0 - 180.0
    return a + _coslerp(0.0, diff, t)


def tour_pose(
    elapsed: float, base_alt: float, tour_speed: float = 1.0
) -> tuple[float, float, float, float, str]:
    """
    Compute the current camera pose for tour mode.

    Returns (yaw, pitch, roll, alt, segment_name).
    Smoothly interpolates between adjacent _TOUR_POSES entries using cosine easing,
    cycling continuously through all segments.
    """
    n = len(_TOUR_POSES)
    total_period = _TOUR_SEGMENT_SEC * n / tour_speed
    phase = (elapsed % total_period) / total_period  # 0..1 for a full cycle
    pos = phase * n
    seg = int(pos) % n
    t = pos - int(pos)  # 0..1 within this segment
    nxt = (seg + 1) % n

    _, yaw0, pitch0, roll0, alt_f0 = _TOUR_POSES[seg]
    _, yaw1, pitch1, roll1, alt_f1 = _TOUR_POSES[nxt]

    yaw = _lerp_angle(yaw0, yaw1, t)
    pitch = _coslerp(pitch0, pitch1, t)
    roll = _coslerp(roll0, roll1, t)
    alt = _coslerp(base_alt * alt_f0, base_alt * alt_f1, t)

    return yaw, pitch, roll, alt, _TOUR_POSES[seg][0]


def orbit_position(
    center_lat: float,
    center_lon: float,
    alt: float,
    radius_m: float,
    heading_deg: float,
) -> tuple[float, float, float, float]:
    """
    Compute a position on a circular orbit around (center_lat, center_lon)
    at radius_m metres, facing inward.
    Returns (lat, lon, alt, yaw_toward_center).
    """
    R = 6_371_000.0  # Earth radius in metres
    bearing_rad = math.radians(heading_deg)

    lat1 = math.radians(center_lat)
    lon1 = math.radians(center_lon)
    d = radius_m / R

    lat2 = math.asin(
        math.sin(lat1) * math.cos(d)
        + math.cos(lat1) * math.sin(d) * math.cos(bearing_rad)
    )
    lon2 = lon1 + math.atan2(
        math.sin(bearing_rad) * math.sin(d) * math.cos(lat1),
        math.cos(d) - math.sin(lat1) * math.sin(lat2),
    )

    # Yaw toward center = bearing + 180
    yaw = (heading_deg + 180.0) % 360.0

    return math.degrees(lat2), math.degrees(lon2), alt, yaw


# ---------------------------------------------------------------------------
# Main send loop
# ---------------------------------------------------------------------------


# ---------------------------------------------------------------------------
# Terrain queries (HAT/HOT, LOS) and their responses
# ---------------------------------------------------------------------------


def pack_hat_hot_request(
    request_id: int, lat: float, lon: float, alt: float = 0.0, req_type: int = 2
) -> bytes:
    """HAT/HOT Request, opcode 24, 32 bytes (CIGI 3.3 ICD 4.1.24), geodetic, one shot.

    0 ID, 1 size, 2-3 HAT/HOT ID, 4 Request Type (bits 0-1: 0 HAT, 1 HOT, 2 extended) |
    Coordinate System (bit 2, 0 = geodetic), 5 Update Period (0 = one shot), 6-7 Entity ID,
    8 Lat, 16 Lon, 24 Alt (doubles)."""
    return struct.pack(
        ">BBHBBHddd", 24, 32, request_id & 0xFFFF, req_type & 0x3, 0, 0, lat, lon, alt
    )


def pack_los_segment_request(
    los_id: int,
    src: tuple[float, float, float],
    dst: tuple[float, float, float],
    extended: bool = True,
) -> bytes:
    """LOS Segment Request, opcode 25, 64 bytes (CIGI 3.3 ICD 4.1.25), geodetic source and destination.

    0 ID, 1 size, 2-3 LOS ID, 4 Request Type (bit 0, 1 = extended) | Source / Destination / Response
    Coordinate System (bits 1-3, 0 = geodetic) | Destination Entity ID Valid (bit 4), 5 Alpha Threshold,
    6-7 Entity ID, 8/16/24 source Lat/Lon/Alt, 32/40/48 destination Lat/Lon/Alt (doubles),
    56 Material Mask (u32), 60 Update Period (0 = one shot), 61 reserved, 62-63 Destination Entity ID."""
    return struct.pack(
        ">BBHBBHddddddIBxH",
        25,
        64,
        los_id & 0xFFFF,
        1 if extended else 0,
        0,
        0,
        *src,
        *dst,
        0xFFFFFFFF,
        0,
        0,
    )


def parse_query_responses(datagram: bytes) -> list[dict]:
    """HAT/HOT Extended (103) and LOS Extended (105) responses in one IG -> host datagram.

    CamSim packs in its native order; the Start of Frame's Byte Swap Magic (bytes 6-7) says which.
      103, 40 B: 2-3 ID, 4 Valid (bit 0), 8 HAT, 16 HOT (f64), 24 Material (u32), 28 Normal Az, 32 Normal El (f32)
      105, 56 B: 2-3 ID, 4 Valid (bit 0) | Entity ID Valid (1) | Range Valid (2) | Visible (3), 6-7 Entity ID,
                 8 Range, 16 Lat, 24 Lon, 32 Alt (f64), 40-43 RGBA, 44 Material (u32), 48 Normal Az, 52 El (f32)"""
    e = ">" if datagram[:1] == b"\x65" and datagram[6:8] == b"\x80\x00" else "<"
    out: list[dict] = []
    i, n = 0, len(datagram)
    while i + 2 <= n:
        op, size = datagram[i], datagram[i + 1]
        if size < 2 or i + size > n:
            break
        p = datagram[i : i + size]
        if op == 103 and size >= 40:
            (rid,) = struct.unpack_from(e + "H", p, 2)
            hat, hot, mat, az, el = struct.unpack_from(e + "ddIff", p, 8)
            out.append(
                {
                    "op": 103,
                    "id": rid,
                    "valid": bool(p[4] & 1),
                    "hat": hat,
                    "hot": hot,
                    "material": mat,
                    "normal": (az, el),
                }
            )
        elif op == 105 and size >= 56:
            (rid,) = struct.unpack_from(e + "H", p, 2)
            rng, lat, lon, alt = struct.unpack_from(e + "dddd", p, 8)
            mat, az, el = struct.unpack_from(e + "Iff", p, 44)
            out.append(
                {
                    "op": 105,
                    "id": rid,
                    "valid": bool(p[4] & 1),
                    "visible": bool(p[4] & 8),
                    "range": rng,
                    "lat": lat,
                    "lon": lon,
                    "alt": alt,
                    "material": mat,
                    "normal": (az, el),
                }
            )
        i += size
    return out


def parse_lat_lon(text: str) -> tuple[float, ...]:
    """'LAT,LON' or 'LAT,LON,ALT' -> floats (argparse type)."""
    try:
        v = tuple(float(t) for t in text.split(","))
    except ValueError:
        v = ()
    if len(v) not in (2, 3):
        raise argparse.ArgumentTypeError(f"expected LAT,LON[,ALT], got {text!r}")
    return v


def print_query_responses(
    sock: socket.socket, stop, names: dict[tuple[int, int], str]
) -> None:
    """Thread body: print every 103 / 105 response to the queries this run sent."""
    while not stop.is_set():
        try:
            data, _ = sock.recvfrom(65535)
        except (TimeoutError, OSError):
            continue
        for r in parse_query_responses(data):
            name = names.get((r["op"], r["id"]))
            if name is None:
                continue
            if r["op"] == 103:
                print(
                    f"  HAT/HOT ext {name}: valid={r['valid']} material={r['material']} "
                    f"HAT={r['hat']:.2f} m HOT={r['hot']:.2f} m normal az/el={r['normal'][0]:.1f}/{r['normal'][1]:.1f}"
                )
            else:
                print(
                    f"  LOS ext {name}: valid={r['valid']} visible={r['visible']} material={r['material']} "
                    f"hit=({r['lat']:.6f}, {r['lon']:.6f}, {r['alt']:.2f} m) range={r['range']:.1f} m"
                )


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--host", default="127.0.0.1", help="CamSim host")
    ap.add_argument("--port", type=int, default=8888, help="CamSim CIGI UDP port")
    ap.add_argument("--lat", type=float, default=37.7749)
    ap.add_argument("--lon", type=float, default=-122.4194)
    ap.add_argument("--alt", type=float, default=1000.0, help="Altitude metres MSL")
    ap.add_argument("--yaw", type=float, default=0.0)
    ap.add_argument("--pitch", type=float, default=-15.0)
    ap.add_argument("--roll", type=float, default=0.0)
    ap.add_argument("--rate", type=float, default=30.0, help="Packets/sec")
    ap.add_argument("--duration", type=float, default=0.0, help="Seconds (0=forever)")
    ap.add_argument("--sweep", action="store_true", help="Rotate heading 0→360")
    ap.add_argument(
        "--circle", nargs=2, type=float, metavar=("LAT", "LON"), help="Orbit a point"
    )
    ap.add_argument(
        "--tour",
        action="store_true",
        help="Cycle through predefined camera poses (loops forever)",
    )
    ap.add_argument(
        "--tour-speed",
        type=float,
        default=1.0,
        metavar="SCALE",
        help="Tour speed multiplier (default: 1.0)",
    )
    ap.add_argument(
        "--entity-id",
        type=int,
        default=1,
        help="CIGI entity ID for the camera (must match camera_entity_id in config)",
    )
    ap.add_argument(
        "--sensor-id",
        type=int,
        default=0,
        choices=[0, 1, 2],
        help="Sensor waveband: 0=EO (color), 1=IR (thermal) (default: 0; "
        "other values are unmapped and default to EO)",
    )
    ap.add_argument(
        "--polarity",
        type=int,
        default=0,
        choices=[0, 1],
        help="IR polarity: 0=WhiteHot, 1=BlackHot (default: 0; IR mode only)",
    )
    ap.add_argument("--fov-h", type=float, default=60.0, help="Horizontal FOV degrees")
    ap.add_argument(
        "--gimbal",
        nargs=3,
        type=float,
        metavar=("YAW", "PITCH", "ROLL"),
        default=None,
        help="Send View Control every frame with these airframe-relative angles (deg)",
    )
    ap.add_argument(
        "--time",
        type=str,
        default=None,
        metavar="HHMM",
        help="Time of day as HHMM (e.g. 0600 for sunrise)",
    )
    ap.add_argument(
        "--stars",
        type=float,
        default=100.0,
        metavar="PCT",
        help="Star Field Intensity in percent sent with --time / --time-sweep (default 100; 0 sends Star Field "
        "Enable off). Sun and moon are always enabled",
    )
    ap.add_argument(
        "--visibility",
        type=float,
        default=None,
        metavar="METRES",
        help="Visibility range in metres (e.g. 500 for dense fog)",
    )
    ap.add_argument(
        "--weather", action="store_true", help="Enable basic overcast weather layer"
    )
    ap.add_argument(
        "--cloud-base",
        type=float,
        default=None,
        metavar="METRES",
        help="Cloud layer base altitude in metres MSL (default: 2000)",
    )
    ap.add_argument(
        "--cloud-thickness",
        type=float,
        default=None,
        metavar="METRES",
        help="Cloud layer thickness in metres (default: 500)",
    )
    ap.add_argument(
        "--time-sweep",
        action="store_true",
        help="Cycle time-of-day 0000→2400 (use with --tour)",
    )
    ap.add_argument(
        "--time-sweep-period",
        type=float,
        default=120.0,
        metavar="SEC",
        help="Seconds for one full day/night cycle (default: 120)",
    )
    ap.add_argument(
        "--hat-extended",
        type=parse_lat_lon,
        action="append",
        default=[],
        metavar="LAT,LON",
        help="Send an extended HAT/HOT request (opcode 24) here every --query-interval and print the "
        "response (103: HAT, HOT, material code); repeatable",
    )
    ap.add_argument(
        "--los-extended",
        type=parse_lat_lon,
        action="append",
        default=[],
        metavar="LAT,LON[,ALT]",
        help="Send an extended LOS segment request (opcode 25) from the sensor position to this point "
        "(ALT ellipsoid metres, default -500: below the ground) every --query-interval and print the "
        "response (105: hit point, material code); repeatable",
    )
    ap.add_argument(
        "--query-interval",
        type=float,
        default=2.0,
        metavar="SEC",
        help="Query resend period",
    )
    ap.add_argument(
        "--response-port",
        type=int,
        default=8889,
        help="UDP port CamSim sends responses to",
    )
    args = ap.parse_args()

    interval = 1.0 / args.rate
    queries: list[
        tuple[str, bytes]
    ] = []  # (label, request) sent every --query-interval
    query_names: dict[tuple[int, int], str] = {}
    for k, ll in enumerate(args.hat_extended):
        rid, label = 1000 + k, f"#{1000 + k} ({ll[0]:.6f}, {ll[1]:.6f})"
        queries.append((label, pack_hat_hot_request(rid, ll[0], ll[1])))
        query_names[(103, rid)] = label
    for k, ll in enumerate(args.los_extended):
        rid, dst = 2000 + k, (ll[0], ll[1], ll[2] if len(ll) == 3 else -500.0)
        label = f"#{rid} sensor -> ({dst[0]:.6f}, {dst[1]:.6f}, {dst[2]:.0f} m)"
        queries.append(
            (label, pack_los_segment_request(rid, (args.lat, args.lon, args.alt), dst))
        )
        query_names[(105, rid)] = label
    resp_stop = None
    if queries:
        import threading

        resp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        resp_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        resp_sock.bind(("0.0.0.0", args.response_port))
        resp_sock.settimeout(0.5)
        resp_stop = threading.Event()
        threading.Thread(
            target=print_query_responses,
            args=(resp_sock, resp_stop, query_names),
            daemon=True,
        ).start()
        print(
            f"    Queries: {', '.join(q[0] for q in queries)} every {args.query_interval:g} s "
            f"(responses on :{args.response_port})"
        )
    next_query = 0.0
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    # Parse environment options
    celestial_base: dict | None = None
    if args.time is not None:
        hh = int(args.time[:2]) if len(args.time) >= 2 else 12
        mm = int(args.time[2:4]) if len(args.time) >= 4 else 0
        celestial_base = {"hour": hh, "minute": mm}
    elif args.time_sweep:
        celestial_base = {"hour": 0, "minute": 0}
    if celestial_base is not None:
        celestial_base["star_en"] = args.stars > 0.0
        celestial_base["star_intensity"] = max(args.stars, 0.0)

    atmosphere_base: dict | None = None
    if args.visibility is not None:
        atmosphere_base = {"visibility": args.visibility}

    weather_base: dict | None = None
    if args.weather or args.cloud_base is not None or args.cloud_thickness is not None:
        weather_base = {
            "coverage": 80.0 if args.weather else 20.0,
            "base_elev": args.cloud_base if args.cloud_base is not None else 2000.0,
            "thickness": args.cloud_thickness
            if args.cloud_thickness is not None
            else 500.0,
            "cloud_type": 2,
            "severity": 2 if args.weather else 0,
            "weather_en": True,
            "visibility_rng": 5000.0 if args.weather else 50000.0,
        }

    _SENSOR_NAMES = {0: "EO (color)", 1: "IR (thermal)"}
    _POLARITY_NAMES = {0: "WhiteHot", 1: "BlackHot"}
    print(f"==> Sending CIGI 3.3 to {args.host}:{args.port} @ {args.rate:.0f} fps")
    if args.circle:
        clat, clon = args.circle
        print(f"    Mode: orbit ({clat:.4f}, {clon:.4f}), alt={args.alt}m")
    elif args.sweep:
        print(
            f"    Mode: heading sweep ({args.lat:.4f}, {args.lon:.4f}), alt={args.alt}m"
        )
    elif args.tour:
        seg_sec = _TOUR_SEGMENT_SEC / args.tour_speed
        print(
            f"    Mode: tour  ({args.lat:.4f}, {args.lon:.4f}), base alt={args.alt}m, "
            f"speed={args.tour_speed}x ({seg_sec:.1f}s/segment)"
        )
        pose_names = ", ".join(p[0] for p in _TOUR_POSES)
        print(f"    Poses: {pose_names}")
    else:
        print(
            f"    Mode: static  lat={args.lat:.4f} lon={args.lon:.4f} "
            f"alt={args.alt:.0f}m  yaw={args.yaw:.1f}° pitch={args.pitch:.1f}°"
        )
    if celestial_base:
        if args.time_sweep:
            print(f"    Time: sweep 0000→2400 over {args.time_sweep_period:.0f}s")
        else:
            print(
                f"    Time: {celestial_base['hour']:02d}:{celestial_base['minute']:02d}"
            )
    if atmosphere_base:
        print(f"    Visibility: {atmosphere_base['visibility']:.0f}m")
    if weather_base:
        print(
            f"    Weather: coverage={weather_base['coverage']:.0f}%  "
            f"base={weather_base['base_elev']:.0f}m  "
            f"thickness={weather_base['thickness']:.0f}m"
        )
    sensor_label = _SENSOR_NAMES.get(args.sensor_id, f"id={args.sensor_id}")
    pol_label = _POLARITY_NAMES.get(args.polarity, str(args.polarity))
    print(f"    Sensor: {sensor_label}  polarity: {pol_label}")
    if args.gimbal:
        gy, gp, gr = args.gimbal
        print(
            f"    Gimbal (View Control): yaw={gy:.1f}° pitch={gp:.1f}° roll={gr:.1f}°"
        )
    print("    Press Ctrl-C to stop")
    print()

    frame_ctr = 0
    start_time = time.monotonic()
    next_send = start_time
    orbit_deg = 0.0  # current orbit/sweep angle
    sent_count = 0

    try:
        while True:
            now = time.monotonic()
            elapsed = now - start_time

            if args.duration > 0 and elapsed >= args.duration:
                break

            # Compute this frame's position
            seg_name = ""
            if args.circle:
                clat, clon = args.circle
                lat, lon, alt, yaw = orbit_position(
                    clat,
                    clon,
                    args.alt,
                    radius_m=500.0,
                    heading_deg=orbit_deg,
                )
                pitch = args.pitch
                roll = 0.0
            elif args.sweep:
                lat, lon, alt = args.lat, args.lon, args.alt
                yaw = orbit_deg % 360.0
                pitch = args.pitch
                roll = 0.0
            elif args.tour:
                lat, lon = args.lat, args.lon
                yaw, pitch, roll, alt, seg_name = tour_pose(
                    elapsed, args.alt, args.tour_speed
                )
            else:
                lat, lon, alt = args.lat, args.lon, args.alt
                yaw, pitch, roll = args.yaw, args.pitch, args.roll

            # Compute time-sweep celestial override
            celestial_frame = celestial_base
            if args.time_sweep and celestial_base is not None:
                # Map elapsed time to 0-24 hour range
                day_frac = (elapsed % args.time_sweep_period) / args.time_sweep_period
                hour_dec = day_frac * 24.0
                celestial_frame = dict(celestial_base)
                celestial_frame["hour"] = int(hour_dec) % 24
                celestial_frame["minute"] = int((hour_dec % 1.0) * 60.0) % 60

            # Build and send the CIGI datagram
            pkt = build_host_frame(
                frame_ctr,
                args.entity_id,
                lat,
                lon,
                alt,
                yaw,
                pitch,
                roll,
                fov_h=args.fov_h,
                include_view_def=(frame_ctr == 0),  # send view def only on first frame
                sensor_id=args.sensor_id,
                polarity=args.polarity,
                celestial=celestial_frame,
                atmosphere=atmosphere_base,
                weather=weather_base,
                gimbal=tuple(args.gimbal) if args.gimbal else None,
            )
            if queries and elapsed >= next_query:
                pkt += b"".join(
                    q[1] for q in queries
                )  # after IG Control, in the same host frame
                next_query = elapsed + args.query_interval
            sock.sendto(pkt, (args.host, args.port))
            frame_ctr += 1
            sent_count += 1

            # Advance motion at 6°/s for orbit/sweep
            orbit_deg = (orbit_deg + (6.0 * interval)) % 360.0

            # Status update every ~5 seconds
            if sent_count % int(args.rate * 5) == 0:
                extra = f"  [{seg_name}]" if seg_name else ""
                if celestial_frame:
                    extra += f"  time={celestial_frame['hour']:02d}:{celestial_frame['minute']:02d}"
                print(
                    f"  [{elapsed:6.1f}s] frame {frame_ctr}  "
                    f"lat={lat:.4f} lon={lon:.4f} alt={alt:.0f}m  "
                    f"yaw={yaw:.1f}° pitch={pitch:.1f}° roll={roll:.1f}°{extra}"
                )

            # Rate-limit
            next_send += interval
            sleep_for = next_send - time.monotonic()
            if sleep_for > 0:
                time.sleep(sleep_for)

    except KeyboardInterrupt:
        print()

    elapsed = time.monotonic() - start_time
    actual_fps = sent_count / elapsed if elapsed > 0 else 0
    print(
        f"==> Sent {sent_count} frames in {elapsed:.1f}s ({actual_fps:.1f} fps actual)"
    )
    sock.close()
    if resp_stop is not None:
        resp_stop.set()


if __name__ == "__main__":
    main()
