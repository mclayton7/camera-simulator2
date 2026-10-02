"""CIGI 3.3 packets, packed by hand (big-endian, declared by IG Control's Byte
Swap Magic), never with CCL: CCL rejects View Control yaw outside 0-360 deg.

Every layout below was checked against the CIGI Class Library's V3/V3_2/V3_3
``Pack()`` (``.build_tmp/ccl/source``) and CamSim's receiver
(``CIGI/CigiReceiver.cpp``). Responses from CamSim are packed by CCL in the
IG's native byte order (little-endian on x86/ARM), so the parsers read the
order from the SOF's Byte Swap Magic.
"""

from __future__ import annotations

import math
import struct
from dataclasses import dataclass

BYTE_SWAP_MAGIC = 0x8000

# Host -> IG opcodes
IG_CONTROL = 1
ENTITY_CONTROL = 2
CELESTIAL_CONTROL = 9
ATMOSPHERE_CONTROL = 10
WEATHER_CONTROL = 12
WAVE_CONTROL = 14
VIEW_CONTROL = 16
SENSOR_CONTROL = 17
VIEW_DEFINITION = 21
HAT_HOT_REQUEST = 24
PLATFORM_KINEMATICS = 201  # user-defined, hitl/PROTOCOL.md section 2

# IG -> host opcodes
START_OF_FRAME = 101
HAT_HOT_RESPONSE = 102
HAT_HOT_EXT_RESPONSE = 103
LOS_RESPONSE = 104
LOS_EXT_RESPONSE = 105
SENSOR_RESPONSE = 106
SENSOR_EXT_RESPONSE = 107


def pack_ig_control(
    frame: int,
    timestamp_ticks: int,
    last_ig_frame: int = 0,
    db_number: int = 0,
    ig_mode: int = 1,
    timestamp_valid: bool = True,
) -> bytes:
    """IG Control, opcode 1, 24 bytes (CigiIGCtrlV3_3::Pack).

    0 id, 1 size, 2 major version 3, 3 database number (int8), 4 IG Mode (bits
    0-1) | Timestamp Valid (bit 2) | Smoothing (bit 3) | Minor Version (bits
    4-7 = 3), 5 reserved, 6-7 Byte Swap Magic, 8 Host Frame Number, 12
    Timestamp (10 us ticks), 16 Last Received IG Frame, 20 reserved."""
    flags = (3 << 4) | (0x04 if timestamp_valid else 0) | (ig_mode & 0x03)
    return struct.pack(
        ">BBBbBBHIIII",
        IG_CONTROL,
        24,
        3,
        db_number,
        flags,
        0,
        BYTE_SWAP_MAGIC,
        frame & 0xFFFFFFFF,
        timestamp_ticks & 0xFFFFFFFF,
        last_ig_frame & 0xFFFFFFFF,
        0,
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
    attach: bool = False,
    alpha: int = 255,
    entity_type: int = 0,
    parent_id: int = 0,
) -> bytes:
    """Entity Control, opcode 2, 48 bytes (CigiEntityCtrlV3_3::Pack).

    0 id, 1 size, 2-3 Entity ID, 4 Entity State (bits 0-1) | Attach (bit 2) |
    Collision (3) | Inherit Alpha (4) | Clamp (5-6), 5 animation flags, 6 Alpha,
    7 reserved, 8-9 Entity Type, 10-11 Parent ID, 12 Roll, 16 Pitch, 20 Yaw
    (float32), 24 Lat, 32 Lon, 40 Alt (float64)."""
    flags = (entity_state & 0x03) | (0x04 if attach else 0)
    return struct.pack(
        ">BBHBBBBHHfffddd",
        ENTITY_CONTROL,
        48,
        entity_id & 0xFFFF,
        flags,
        0,
        alpha & 0xFF,
        0,
        entity_type & 0xFFFF,
        parent_id & 0xFFFF,
        roll,
        pitch,
        yaw,
        lat,
        lon,
        alt,
    )


def pack_view_control(
    yaw: float,
    pitch: float,
    roll: float,
    view_id: int = 0,
    group_id: int = 0,
    entity_id: int = 0,
    x_off: float = 0.0,
    y_off: float = 0.0,
    z_off: float = 0.0,
    enable_offsets: bool = False,
    enable_angles: bool = True,
) -> bytes:
    """View Control, opcode 16, 32 bytes (CigiViewCtrlV3::Pack).

    0 id, 1 size, 2-3 View ID, 4 Group ID, 5 X/Y/Z Offset Enable (bits 0-2) |
    Roll/Pitch/Yaw Enable (bits 3-5), 6-7 Entity ID, 8 X, 12 Y, 16 Z offsets,
    20 Roll, 24 Pitch, 28 Yaw (float32 deg). CamSim: send Entity ID 0."""
    flags = (0x07 if enable_offsets else 0) | (0x38 if enable_angles else 0)
    return struct.pack(
        ">BBHBBHffffff",
        VIEW_CONTROL,
        32,
        view_id & 0xFFFF,
        group_id & 0xFF,
        flags,
        entity_id & 0xFFFF,
        x_off,
        y_off,
        z_off,
        roll,
        pitch,
        yaw,
    )


def vfov_from_hfov(hfov_deg: float, width: int, height: int) -> float:
    return math.degrees(2.0 * math.atan(math.tan(math.radians(hfov_deg) / 2.0) * height / width))


def pack_view_definition(
    hfov_deg: float,
    vfov_deg: float,
    view_id: int = 0,
    group_id: int = 0,
    near: float = 0.1,
    far: float = 1.0e6,
) -> bytes:
    """View Definition, opcode 21, 32 bytes (CigiViewDefV3::Pack).

    0 id, 1 size, 2-3 View ID, 4 Group ID, 5 Near (0x01) | Far (0x02) | Left
    (0x04) | Right (0x08) | Top (0x10) | Bottom (0x20) enables | Mirror (bits
    6-7), 6 Pixel Replicate (0-2) | Projection (bit 3, 0 perspective) | Reorder
    (4) | View Type (5-7), 7 reserved, 8 Near, 12 Far, 16 Left, 20 Right, 24
    Top, 28 Bottom (float32)."""
    return struct.pack(
        ">BBHBBBBffffff",
        VIEW_DEFINITION,
        32,
        view_id & 0xFFFF,
        group_id & 0xFF,
        0x3F,
        0,
        0,
        near,
        far,
        -hfov_deg / 2.0,
        hfov_deg / 2.0,
        vfov_deg / 2.0,
        -vfov_deg / 2.0,
    )


def pack_sensor_control(
    sensor_id: int,
    polarity: int = 0,
    sensor_on: bool = True,
    view_id: int = 0,
    gain: float = 0.0,
) -> bytes:
    """Sensor Control, opcode 17, 24 bytes (CigiSensorCtrlV3::Pack).

    0 id, 1 size, 2-3 View ID, 4 Sensor ID, 5 On (bit 0) | Polarity (bit 1, 1 =
    black-hot) | Line Drop (2) | Auto Gain (3) | Track Polarity (4) | Track Mode
    (5-7), 6 Response Type (bit 0), 7 reserved, 8 Gain, 12 Level, 16 AC
    Coupling, 20 Noise (float32)."""
    flags = (0x01 if sensor_on else 0) | ((polarity & 1) << 1)
    return struct.pack(
        ">BBHBBBBffff",
        SENSOR_CONTROL,
        24,
        view_id & 0xFFFF,
        sensor_id & 0xFF,
        flags,
        0,
        0,
        gain,
        0.0,
        0.0,
        0.0,
    )


def pack_celestial_control(
    hour: int = 0,
    minute: int = 0,
    month: int = 0,
    day: int = 0,
    year: int = 0,
    ephemeris: bool = True,
    date_valid: bool = False,
    sun: bool = True,
    moon: bool = True,
    stars: bool = True,
    star_intensity: float = 100.0,
) -> bytes:
    """Celestial Sphere Control, opcode 9, 16 bytes (CigiCelestialCtrlV3::Pack).

    0 id, 1 size, 2 Hour, 3 Minute, 4 Ephemeris (0x01) | Sun (0x02) | Moon
    (0x04) | Star Field (0x08) | Date/Time Valid (0x10), 5-7 reserved, 8 Date
    (uint32 MMDDYYYY), 12 Star Field Intensity (float32 %)."""
    flags = (
        (0x01 if ephemeris else 0)
        | (0x02 if sun else 0)
        | (0x04 if moon else 0)
        | (0x08 if stars else 0)
        | (0x10 if date_valid else 0)
    )
    date = month * 1_000_000 + day * 10_000 + year
    return struct.pack(
        ">BBBBBBHIf",
        CELESTIAL_CONTROL,
        16,
        hour & 0xFF,
        minute & 0xFF,
        flags,
        0,
        0,
        date,
        star_intensity,
    )


def pack_atmosphere_control(
    visibility_m: float,
    air_temp_c: float,
    humidity_pct: float,
    wind_speed_ms: float = 0.0,
    wind_dir_deg: float = 0.0,
    baro_mb: float = 1013.25,
    vert_wind_ms: float = 0.0,
    enable: bool = True,
) -> bytes:
    """Atmosphere Control, opcode 10, 32 bytes (CigiAtmosCtrlV3::Pack).

    0 id, 1 size, 2 Atmospheric Model Enable (bit 0), 3 Global Humidity (uint8
    %), 4 Air Temp, 8 Visibility, 12 Horiz Wind, 16 Vert Wind, 20 Wind Dir, 24
    Barometric Pressure (mb), 28 reserved."""
    return struct.pack(
        ">BBBBffffffI",
        ATMOSPHERE_CONTROL,
        32,
        1 if enable else 0,
        max(0, min(100, int(round(humidity_pct)))),
        air_temp_c,
        visibility_m,
        wind_speed_ms,
        vert_wind_ms,
        wind_dir_deg,
        baro_mb,
        0,
    )


def pack_weather_control(
    coverage_pct: float,
    base_elev_m: float,
    thickness_m: float,
    visibility_m: float,
    layer_id: int = 1,
    region_id: int = 0,
    enable: bool = True,
    cloud_type: int = 0,
    scope: int = 0,
    severity: int = 0,
    air_temp_c: float = 0.0,
    humidity_pct: int = 0,
    transition_m: float = 500.0,
    wind_speed_ms: float = 0.0,
    wind_dir_deg: float = 0.0,
    baro_mb: float = 1013.25,
) -> bytes:
    """Weather Control, opcode 12, 56 bytes (CigiWeatherCtrlV3::Pack).

    0 id, 1 size, 2-3 Region/Entity ID, 4 Layer ID, 5 Humidity (uint8 %), 6
    Weather Enable (0x01) | Scud (0x02) | Random Winds (0x04) | Random Lightning
    (0x08) | Cloud Type (bits 4-7), 7 Scope (bits 0-1, 0 global) | Severity
    (bits 2-4), 8 Air Temp, 12 Visibility, 16 Scud Frequency, 20 Coverage %, 24
    Base Elevation, 28 Thickness, 32 Transition Band, 36 Horiz Wind, 40 Vert
    Wind, 44 Wind Direction, 48 Barometric Pressure, 52 Aerosol (float32)."""
    flags = (0x01 if enable else 0) | ((cloud_type & 0x0F) << 4)
    scope_sev = (scope & 0x03) | ((severity & 0x07) << 2)
    wd = wind_dir_deg - 360.0 if wind_dir_deg > 180.0 else wind_dir_deg  # CCL convention
    return struct.pack(
        ">BBHBBBBffffffffffff",
        WEATHER_CONTROL,
        56,
        region_id & 0xFFFF,
        layer_id & 0xFF,
        max(0, min(100, int(humidity_pct))),
        flags,
        scope_sev,
        air_temp_c,
        visibility_m,
        0.0,
        coverage_pct,
        base_elev_m,
        thickness_m,
        transition_m,
        wind_speed_ms,
        0.0,
        wd,
        baro_mb,
        0.0,
    )


def pack_wave_control(
    height_m: float,
    wavelength_m: float,
    period_s: float,
    direction_deg: float,
    wave_id: int = 0,
    enable: bool = True,
    scope: int = 0,
    phase_deg: float = 0.0,
) -> bytes:
    """Wave Control, opcode 14, 32 bytes (CigiWaveCtrlV3::Pack).

    0 id, 1 size, 2-3 Entity/Region ID, 4 Wave ID, 5 Enable (bit 0) | Scope
    (bits 1-2) | Breaker Type (3-4), 6-7 reserved, 8 Height (crest to trough),
    12 Wavelength, 16 Period, 20 Direction (toward), 24 Phase Offset, 28
    Leading (float32)."""
    flags = (0x01 if enable else 0) | ((scope & 0x03) << 1)
    return struct.pack(
        ">BBHBBHffffff",
        WAVE_CONTROL,
        32,
        0,
        wave_id & 0xFF,
        flags,
        0,
        height_m,
        wavelength_m,
        period_s,
        direction_deg,
        phase_deg,
        0.0,
    )


HAT = 0
HOT = 1
HAT_HOT_EXTENDED = 2


def pack_hat_hot_request(
    request_id: int,
    lat: float,
    lon: float,
    alt: float = 0.0,
    req_type: int = HAT_HOT_EXTENDED,
    update_period: int = 0,
    entity_id: int = 0,
) -> bytes:
    """HAT/HOT Request, opcode 24, 32 bytes (CigiHatHotReqV3_2::Pack), geodetic.

    0 id, 1 size, 2-3 HAT/HOT ID, 4 Request Type (bits 0-1: 0 HAT, 1 HOT, 2
    extended) | Coordinate System (bit 2, 0 geodetic), 5 Update Period, 6-7
    Entity ID, 8 Lat, 16 Lon, 24 Alt (float64)."""
    return struct.pack(
        ">BBHBBHddd",
        HAT_HOT_REQUEST,
        32,
        request_id & 0xFFFF,
        req_type & 0x03,
        update_period & 0xFF,
        entity_id & 0xFFFF,
        lat,
        lon,
        alt,
    )


# Packet 201 flags (PROTOCOL.md section 2)
PK_AIRSPEEDS = 0x01
PK_MAG_HEADING = 0x02
PK_NED_VELOCITY = 0x04
PK_SAMPLE_TIME = 0x08


def pack_platform_kinematics(
    entity_id: int,
    true_airspeed: float | None = None,
    indicated_airspeed: float | None = None,
    magnetic_heading: float | None = None,
    vel_ned: tuple[float, float, float] | None = None,
    sample_utc: float | None = None,
) -> bytes:
    """User-defined Platform Kinematics, opcode 201, 48 bytes (PROTOCOL.md 2).

    0 id, 1 size, 2-3 Entity ID, 4 flags, 5-7 reserved, 8 TAS, 12 IAS (m/s),
    16 magnetic heading (deg 0-360), 20 vN, 24 vE, 28 vD (float32), 32
    sample_utc (float64 Unix s), 40-47 reserved. Big-endian like the rest."""
    flags = 0
    tas = ias = mh = vn = ve = vd = 0.0
    if true_airspeed is not None and indicated_airspeed is not None:
        flags |= PK_AIRSPEEDS
        tas, ias = true_airspeed, indicated_airspeed
    if magnetic_heading is not None:
        flags |= PK_MAG_HEADING
        mh = magnetic_heading % 360.0
    if vel_ned is not None:
        flags |= PK_NED_VELOCITY
        vn, ve, vd = vel_ned
    utc = 0.0
    if sample_utc is not None:
        flags |= PK_SAMPLE_TIME
        utc = sample_utc
    return struct.pack(
        ">BBHB3xffffffd8x",
        PLATFORM_KINEMATICS,
        48,
        entity_id & 0xFFFF,
        flags,
        tas,
        ias,
        mh,
        vn,
        ve,
        vd,
        utc,
    )


# ---------------------------------------------------------------------------
# IG -> host responses
# ---------------------------------------------------------------------------


@dataclass
class StartOfFrame:
    ig_frame: int
    last_host_frame: int
    ig_mode: int
    ig_status: int
    database_id: int
    timestamp: int
    timestamp_valid: bool
    minor_version: int


@dataclass
class HatHotResponse:
    request_id: int
    valid: bool
    hat: float | None
    hot: float | None
    extended: bool
    host_frame_lsn: int = 0
    material: int = 0
    normal_az: float = 0.0
    normal_el: float = 0.0


@dataclass
class SensorExtResponse:
    view_id: int
    sensor_id: int
    sensor_status: int
    entity_target: bool
    entity_id: int
    gate_x: int
    gate_y: int
    gate_x_off: float
    gate_y_off: float
    frame: int
    lat: float
    lon: float
    alt: float  # ellipsoid height (CamSim)


@dataclass
class ResponseMessage:
    big_endian: bool
    sof: StartOfFrame | None
    hat_hot: list[HatHotResponse]
    sensor_ext: list[SensorExtResponse]
    opcodes: list[int]


def iter_packets(data: bytes):
    """Yield (opcode, packet bytes) from a CIGI message; stops at a bad size."""
    i, n = 0, len(data)
    while i + 2 <= n:
        op, size = data[i], data[i + 1]
        if size < 2 or i + size > n:
            break
        yield op, data[i : i + size]
        i += size


def message_byte_order(data: bytes) -> str:
    """'>' or '<' from the leading SOF/IG Control's Byte Swap Magic (bytes 6-7)."""
    if len(data) >= 8 and data[0] in (START_OF_FRAME, IG_CONTROL):
        if data[6:8] == b"\x00\x80":
            return "<"
    return ">"


def parse_sof(p: bytes, e: str) -> StartOfFrame:
    """SOF, opcode 101, 24 bytes (CigiSOFV3_2::Pack): 2 major, 3 db (int8), 4 IG
    status, 5 IG Mode (0-1) | Timestamp Valid (2) | Earth Ref (3) | Minor (4-7),
    6-7 magic, 8 IG frame, 12 timestamp, 16 last received host frame."""
    db = struct.unpack_from("b", p, 3)[0]
    flags = p[5]
    frame, ts = struct.unpack_from(e + "II", p, 8)
    last_host = struct.unpack_from(e + "I", p, 16)[0] if len(p) >= 20 else 0
    return StartOfFrame(
        ig_frame=frame,
        last_host_frame=last_host,
        ig_mode=flags & 0x03,
        ig_status=p[4],
        database_id=db,
        timestamp=ts,
        timestamp_valid=bool(flags & 0x04),
        minor_version=flags >> 4,
    )


def parse_hat_hot_response(p: bytes, e: str) -> HatHotResponse:
    """HAT/HOT Response, opcode 102, 16 bytes (CigiHatHotRespV3_2): 2-3 ID, 4
    Valid (bit 0) | Type (bit 1, 1 = HOT) | Host Frame LSN (bits 4-7), 8 height."""
    rid = struct.unpack_from(e + "H", p, 2)[0]
    valid = bool(p[4] & 0x01)
    is_hot = bool(p[4] & 0x02)
    h = struct.unpack_from(e + "d", p, 8)[0]
    return HatHotResponse(
        request_id=rid,
        valid=valid,
        hat=None if (is_hot or not valid) else h,
        hot=h if (is_hot and valid) else None,
        extended=False,
        host_frame_lsn=p[4] >> 4,
    )


def parse_hat_hot_ext_response(p: bytes, e: str) -> HatHotResponse:
    """HAT/HOT Extended Response, opcode 103, 40 bytes (CigiHatHotXRespV3_2): 2-3
    ID, 4 Valid (bit 0) | Host Frame LSN (4-7), 8 HAT, 16 HOT (float64), 24
    Material (uint32), 28 Normal Az, 32 Normal El (float32)."""
    rid = struct.unpack_from(e + "H", p, 2)[0]
    valid = bool(p[4] & 0x01)
    hat, hot = struct.unpack_from(e + "dd", p, 8)
    mat = struct.unpack_from(e + "I", p, 24)[0]
    az, el = struct.unpack_from(e + "ff", p, 28)
    return HatHotResponse(
        request_id=rid,
        valid=valid,
        hat=hat if valid else None,
        hot=hot if valid else None,
        extended=True,
        host_frame_lsn=p[4] >> 4,
        material=mat,
        normal_az=az,
        normal_el=el,
    )


def parse_sensor_ext_response(p: bytes, e: str) -> SensorExtResponse:
    """Sensor Extended Response, opcode 107, 48 bytes (CigiSensorXRespV3): 2-3
    View ID, 4 Sensor ID, 5 Status (bits 0-1) | Entity Target (bit 2), 6-7
    Entity ID, 8-9 Gate X size, 10-11 Gate Y size, 12 Gate X off, 16 Gate Y off
    (float32), 20 Frame Counter (uint32), 24 Track Lat, 32 Lon, 40 Alt (f64)."""
    view_id, sensor_id, flags, ent, gx, gy = struct.unpack_from(e + "HBBHHH", p, 2)
    gxo, gyo, frame = struct.unpack_from(e + "ffI", p, 12)
    lat, lon, alt = struct.unpack_from(e + "ddd", p, 24)
    return SensorExtResponse(
        view_id=view_id,
        sensor_id=sensor_id,
        sensor_status=flags & 0x03,
        entity_target=bool(flags & 0x04),
        entity_id=ent,
        gate_x=gx,
        gate_y=gy,
        gate_x_off=gxo,
        gate_y_off=gyo,
        frame=frame,
        lat=lat,
        lon=lon,
        alt=alt,
    )


def parse_response_message(data: bytes) -> ResponseMessage:
    e = message_byte_order(data)
    msg = ResponseMessage(big_endian=(e == ">"), sof=None, hat_hot=[], sensor_ext=[], opcodes=[])
    for op, p in iter_packets(data):
        msg.opcodes.append(op)
        if op == START_OF_FRAME and len(p) >= 16:
            msg.sof = parse_sof(p, e)
        elif op == HAT_HOT_RESPONSE and len(p) >= 16:
            msg.hat_hot.append(parse_hat_hot_response(p, e))
        elif op == HAT_HOT_EXT_RESPONSE and len(p) >= 40:
            msg.hat_hot.append(parse_hat_hot_ext_response(p, e))
        elif op == SENSOR_EXT_RESPONSE and len(p) >= 48:
            msg.sensor_ext.append(parse_sensor_ext_response(p, e))
    return msg


# Test helpers: pack IG -> host packets the way CamSim's CCL does.


def pack_sof(frame: int, last_host_frame: int = 0, order: str = "<", ig_mode: int = 1) -> bytes:
    flags = (2 << 4) | 0x04 | (ig_mode & 0x03)
    return struct.pack(
        order + "BBBbBBHIIII", START_OF_FRAME, 24, 3, 0, 0, flags, BYTE_SWAP_MAGIC, frame, 0, last_host_frame, 0
    )


def pack_hat_hot_ext_response(rid: int, valid: bool, hat: float, hot: float, order: str = "<") -> bytes:
    return struct.pack(
        order + "BBHBBHddIffI", HAT_HOT_EXT_RESPONSE, 40, rid, 1 if valid else 0, 0, 0, hat, hot, 0, 0.0, 90.0, 0
    )


def pack_hat_hot_response(rid: int, valid: bool, value: float, is_hot: bool = True, order: str = "<") -> bytes:
    flags = (1 if valid else 0) | (0x02 if is_hot else 0)
    return struct.pack(order + "BBHBBHd", HAT_HOT_RESPONSE, 16, rid, flags, 0, 0, value)


def pack_sensor_ext_response(
    frame: int, lat: float, lon: float, alt: float, sensor_id: int = 0, status: int = 1, order: str = "<"
) -> bytes:
    return struct.pack(
        order + "BBHBBHHHffIddd",
        SENSOR_EXT_RESPONSE,
        48,
        0,
        sensor_id,
        status & 0x03,
        0,
        0,
        0,
        0.0,
        0.0,
        frame,
        lat,
        lon,
        alt,
    )
