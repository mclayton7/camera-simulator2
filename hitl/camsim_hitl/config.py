"""TOML configuration (stdlib tomllib). See hitl/hitl.example.toml for every key.

Every section and key has a default, so an empty file runs a bench setup:
truth on UDP 49300, CamSim on 127.0.0.1:8888/8889, no MAVLink links.
Relative paths are resolved against the config file's directory.
"""

from __future__ import annotations

import dataclasses
import logging
import tomllib
import typing
from dataclasses import dataclass, field
from pathlib import Path

log = logging.getLogger(__name__)


@dataclass
class LogConfig:
    level: str = "INFO"
    file: str = ""


@dataclass
class FallbackPose:
    """Pose used when no truth has arrived (bench tests without X-Plane)."""

    enabled: bool = False
    latitude: float = 37.6213
    longitude: float = -122.379
    elevation_msl: float = 300.0
    heading_deg: float = 0.0
    pitch_deg: float = 0.0
    roll_deg: float = 0.0


@dataclass
class TruthConfig:
    listen: str = "0.0.0.0:49300"
    use_sender_clock: bool = True
    max_extrapolation_s: float = 0.1
    stale_after_s: float = 0.5
    replay_policy: str = "freeze"  # freeze | follow
    sim_speed_warn_below: float = 0.98
    fallback: FallbackPose = field(default_factory=FallbackPose)


@dataclass
class GeodesyConfig:
    geoid_path: str = ""  # empty = the repo's WW15MGH.DAC
    allow_missing_geoid: bool = False
    lever_arm_m: list[float] = field(default_factory=lambda: [0.0, 0.0, 0.0])  # body FRD


@dataclass
class TerrainBlendConfig:
    enabled: bool = True
    band_low_m: float = 50.0
    band_high_m: float = 100.0
    hot_poll_hz: float = 2.0
    correction_tau_s: float = 0.5
    hot_max_age_s: float = 3.0
    hot_max_distance_m: float = 50.0
    agl_source: str = "y_agl"  # y_agl | terrain_probe
    y_agl_offset_m: float = 0.0  # added to y_agl (reference point above the wheels)


@dataclass
class CigiConfig:
    camsim_host: str = "127.0.0.1"
    camsim_port: int = 8888
    extra_destinations: list[str] = field(default_factory=list)  # "host:port", same datagram
    response_listen: str = "0.0.0.0:8889"
    camera_entity_id: int = 1
    frame_rate: float = 30.0  # initial guess; the SOF interval is measured
    send_phase: float = 0.5  # send at SOF + send_phase * frame period
    render_offset_s: float = 0.0  # t_r = SOF + frame period + render_offset_s
    sof_timeout_s: float = 1.0
    free_run_rate_hz: float = 30.0
    resend_period_s: float = 1.0  # View Definition / Sensor Control / Celestial
    send_platform_kinematics: bool = True  # user packet 201
    view_control_entity_id: int = 0
    # Used when the gimbal / camera components are disabled (phase 1 of HITL.md):
    fixed_view_deg: list[float] = field(default_factory=lambda: [0.0, -30.0, 0.0])  # roll, pitch, yaw body-relative
    default_hfov_deg: float = 60.0
    default_sensor: str = "eo"  # eo | ir
    default_polarity: str = "white_hot"  # white_hot | black_hot
    stats_period_s: float = 10.0


@dataclass
class TimeConfig:
    source: str = "system"  # system | xplane
    local_year: int = 0  # xplane: the scenario's LOCAL year; 0 = this year
    resync_after_pause: bool = True  # system: re-set CamSim's clock after a pause
    star_intensity: float = 100.0


@dataclass
class WeatherConfig:
    enabled: bool = True
    period_s: float = 1.0
    coverage_source: str = "cloud_type"  # cloud_type | coverage
    coverage_scale_max: float = 6.0  # cloud_coverage full scale (0-6 or 0-4, unverified)
    cloud_type_coverage_pct: list[float] = field(
        default_factory=lambda: [0.0, 20.0, 40.0, 75.0, 100.0, 100.0]
    )  # X-Plane cloud_type 0 Clear .. 5 Stratus -> %
    cloud_type_cigi: list[int] = field(default_factory=lambda: [0, 5, 7, 9, 8, 10])
    min_visibility_m: float = 100.0
    waves: bool = True
    wave_amplitude_is_half_height: bool = True  # height = 2 * wave_amplitude
    wave_dir_is_from: bool = False  # true: add 180 deg (CIGI wants "toward")


@dataclass
class LinkConfig:
    url: str = ""  # serial:/dev/ttyUSB0:921600 | udpin:0.0.0.0:13280 | udpout:127.0.0.1:13030
    drop_msg_ids: list[int] = field(default_factory=lambda: [93])  # HIL_ACTUATOR_CONTROLS


@dataclass
class MavlinkConfig:
    sysid: int = 1  # MAV_SYS_ID of the vehicle
    links: dict[str, LinkConfig] = field(default_factory=dict)


@dataclass
class GimbalConfig:
    enabled: bool = False
    link: str = ""
    compid: int = 154
    vendor_name: str = "CamSim"
    model_name: str = "HITL gimbal"
    custom_name: str = ""
    firmware_version: str = "1.0.0.0"  # major.minor.patch.dev
    hardware_version: str = "1.0.0.0"
    uid: int = 0
    cap_flags: int = 0  # 0 = derived from the axes below
    joint_order: list[str] = field(default_factory=lambda: ["yaw", "roll", "pitch"])  # outer -> inner
    roll_limits_deg: list[float] = field(default_factory=lambda: [-45.0, 45.0])
    pitch_limits_deg: list[float] = field(default_factory=lambda: [-120.0, 30.0])
    yaw_limits_deg: list[float] = field(default_factory=lambda: [-180.0, 180.0])
    max_rate_dps: list[float] = field(default_factory=lambda: [90.0, 120.0, 120.0])  # roll, pitch, yaw
    max_accel_dps2: list[float] = field(default_factory=lambda: [600.0, 800.0, 800.0])
    servo_tau_s: float = 0.05
    gyro_bias_dps: list[float] = field(default_factory=lambda: [0.0, 0.0, 0.0])
    plant_rate_hz: float = 250.0
    status_rate_hz: float = 10.0
    heartbeat_rate_hz: float = 1.0
    setpoint_timeout_s: float = 2.0
    neutral_deg: list[float] = field(default_factory=lambda: [0.0, 0.0, 0.0])  # roll, pitch, yaw body-relative
    retract_deg: list[float] = field(default_factory=lambda: [0.0, 0.0, 0.0])
    initial_deg: list[float] = field(default_factory=lambda: [0.0, -30.0, 0.0])
    vehicle_attitude: str = "truth"  # truth (X-Plane; the rig) | autopilot (AUTOPILOT_STATE_FOR_GIMBAL_DEVICE; SITL tests)


@dataclass
class ZoomConfig:
    model: str = "focal_length"  # focal_length | table
    focal_length_mm: list[float] = field(default_factory=lambda: [4.3, 129.0])
    sensor_width_mm: float = 6.17
    curve: str = "linear"  # linear | log (zoom % -> focal length)
    hfov_table: list[list[float]] = field(default_factory=list)  # [[zoom_pct, hfov_deg], ...]
    initial_pct: float = 0.0
    step_pct: float = 10.0
    continuous_rate_pct_s: float = 25.0
    accept_focal_length: bool = False
    accept_hfov: bool = False


@dataclass
class CameraParam:
    """One definition-file parameter served by PARAM_EXT_*."""

    name: str = ""
    type: str = "uint32"  # uint8|int8|uint16|int16|uint32|int32|uint64|int64|real32|real64|custom
    value: float | int | str = 0
    role: str = ""  # "" | source | polarity | mode
    role_values: dict[str, float | int] = field(default_factory=dict)  # e.g. {eo = 0, ir = 1}


@dataclass
class CaptureConfig:
    snapshot_url: str = "http://127.0.0.1:8080/snapshot"
    dir: str = "captures"
    timeout_s: float = 6.0
    dedup_window_s: float = 0.5
    capture_on_camera_trigger: bool = False  # treat CAMERA_TRIGGER (112) as a shot


@dataclass
class VideoConfig:
    source: str = "udp://239.1.1.1:5004"
    interface: str = "0.0.0.0"
    dir: str = "videos"


@dataclass
class CameraConfig:
    enabled: bool = False
    link: str = ""
    compid: int = 100
    vendor_name: str = "CamSim"
    model_name: str = "HITL camera"
    firmware_version: str = "1.0.0.0"
    flags: int = 1 | 2 | 4 | 64  # CAMERA_CAP_FLAGS: CAPTURE_VIDEO|CAPTURE_IMAGE|HAS_MODES|HAS_BASIC_ZOOM
    cam_definition_version: int = 1
    definition_file: str = ""  # local XML to serve; empty = no definition file
    lens_id: int = 0
    resolution: list[int] = field(default_factory=lambda: [1920, 1080])
    sensor_size_mm: list[float] = field(default_factory=lambda: [6.17, 4.55])
    gimbal_device_id: int = 154
    http_port: int = 8090  # definition file + captured images
    advertise_host: str = "127.0.0.1"
    heartbeat_rate_hz: float = 1.0
    fov_status_rate_hz: float = 1.0
    initial_sensor: str = "eo"  # eo | ir
    initial_polarity: str = "white_hot"  # white_hot | black_hot
    storage_total_mib: float = 65536.0
    video_stream_uri: str = ""  # VIDEO_STREAM_INFORMATION; empty = unsupported
    zoom: ZoomConfig = field(default_factory=ZoomConfig)
    params: list[CameraParam] = field(default_factory=list)
    capture: CaptureConfig = field(default_factory=CaptureConfig)
    video: VideoConfig = field(default_factory=VideoConfig)


@dataclass
class Config:
    log: LogConfig = field(default_factory=LogConfig)
    truth: TruthConfig = field(default_factory=TruthConfig)
    geodesy: GeodesyConfig = field(default_factory=GeodesyConfig)
    terrain_blend: TerrainBlendConfig = field(default_factory=TerrainBlendConfig)
    cigi: CigiConfig = field(default_factory=CigiConfig)
    time: TimeConfig = field(default_factory=TimeConfig)
    weather: WeatherConfig = field(default_factory=WeatherConfig)
    mavlink: MavlinkConfig = field(default_factory=MavlinkConfig)
    gimbal: GimbalConfig = field(default_factory=GimbalConfig)
    camera: CameraConfig = field(default_factory=CameraConfig)
    base_dir: str = "."


def _build(cls, data: dict, where: str):
    hints = typing.get_type_hints(cls)
    kwargs = {}
    names = {f.name for f in dataclasses.fields(cls)}
    for k, v in data.items():
        if k not in names:
            log.warning("config: unknown key %s.%s (ignored)", where, k)
            continue
        t = hints[k]
        origin = typing.get_origin(t)
        if dataclasses.is_dataclass(t):
            if not isinstance(v, dict):
                raise ValueError(f"config: {where}.{k} must be a table")
            kwargs[k] = _build(t, v, f"{where}.{k}")
        elif origin is dict and typing.get_args(t) and dataclasses.is_dataclass(typing.get_args(t)[1]):
            sub = typing.get_args(t)[1]
            kwargs[k] = {name: _build(sub, tv, f"{where}.{k}.{name}") for name, tv in v.items()}
        elif origin is list and typing.get_args(t) and dataclasses.is_dataclass(typing.get_args(t)[0]):
            sub = typing.get_args(t)[0]
            kwargs[k] = [_build(sub, tv, f"{where}.{k}[{i}]") for i, tv in enumerate(v)]
        else:
            if t is float and isinstance(v, int) and not isinstance(v, bool):
                v = float(v)
            kwargs[k] = v
    return cls(**kwargs)


def load_config(path: str | Path | None) -> Config:
    if path is None:
        return Config()
    p = Path(path)
    with p.open("rb") as f:
        data = tomllib.load(f)
    cfg = _build(Config, data, "")
    cfg.base_dir = str(p.resolve().parent)
    return cfg


def resolve_path(cfg: Config, value: str) -> Path:
    p = Path(value).expanduser()
    return p if p.is_absolute() else Path(cfg.base_dir) / p


def parse_hostport(s: str, default_host: str = "0.0.0.0") -> tuple[str, int]:
    if ":" not in s:
        return default_host, int(s)
    h, _, port = s.rpartition(":")
    return (h or default_host), int(port)
