#!/usr/bin/env python3
"""Minimal ground station for SITL tests of the IG host's gimbal and camera.

Sends gimbal-manager and camera commands the way QGC does, then prints what
comes back (ACKs, CAMERA_*, GIMBAL_*, PARAM_EXT_*). Connect it to PX4's GCS
link (SITL: udpin:0.0.0.0:14550).

    fake_gcs.py watch                              # print gimbal/camera traffic
    fake_gcs.py pitchyaw -45 30                    # DO_GIMBAL_MANAGER_PITCHYAW
    fake_gcs.py pitchyaw -45 30 --earth-yaw        # yaw locked to north
    fake_gcs.py roi 37.62 -122.38 0                # DO_SET_ROI_LOCATION
    fake_gcs.py roi-none
    fake_gcs.py zoom range 50 | zoom step 1 | zoom continuous 1
    fake_gcs.py source ir                          # SET_CAMERA_SOURCE
    fake_gcs.py capture                            # IMAGE_START_CAPTURE
    fake_gcs.py video start | video stop
    fake_gcs.py info                               # CAMERA_INFORMATION, settings, storage
    fake_gcs.py params                             # PARAM_EXT_REQUEST_LIST
    fake_gcs.py setparam CAM_SOURCE 1 --type uint8
"""

from __future__ import annotations

import argparse
import math
import os
import struct
import time

os.environ.setdefault("MAVLINK20", "1")
from pymavlink import mavutil  # noqa: E402

INTERESTING = (
    "COMMAND_ACK",
    "CAMERA_INFORMATION",
    "CAMERA_SETTINGS",
    "CAMERA_CAPTURE_STATUS",
    "CAMERA_IMAGE_CAPTURED",
    "CAMERA_FOV_STATUS",
    "STORAGE_INFORMATION",
    "GIMBAL_MANAGER_INFORMATION",
    "GIMBAL_MANAGER_STATUS",
    "GIMBAL_DEVICE_INFORMATION",
    "GIMBAL_DEVICE_ATTITUDE_STATUS",
    "PARAM_EXT_VALUE",
    "PARAM_EXT_ACK",
)

PARAM_TYPES = {"uint8": (1, "<B"), "int8": (2, "<b"), "uint16": (3, "<H"), "int16": (4, "<h"),
               "uint32": (5, "<I"), "int32": (6, "<i"), "real32": (9, "<f")}


def q_to_euler_deg(q):
    w, x, y, z = q
    yaw = math.atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z))
    pitch = math.asin(max(-1, min(1, 2 * (w * y - z * x))))
    roll = math.atan2(2 * (w * x + y * z), 1 - 2 * (x * x + y * y))
    return math.degrees(roll), math.degrees(pitch), math.degrees(yaw)


def describe(m) -> str:
    t = m.get_type()
    src = f"{m.get_srcSystem()}/{m.get_srcComponent()}"
    if t in ("GIMBAL_DEVICE_ATTITUDE_STATUS", "GIMBAL_MANAGER_STATUS"):
        if t == "GIMBAL_DEVICE_ATTITUDE_STATUS":
            r, p, y = q_to_euler_deg(m.q)
            return f"{src} {t} flags={m.flags} rpy=({r:.1f}, {p:.1f}, {y:.1f}) fail={m.failure_flags} delta_yaw={math.degrees(m.delta_yaw):.1f}"
        return f"{src} {t} flags={m.flags} primary={m.primary_control_sysid}/{m.primary_control_compid}"
    if t == "PARAM_EXT_VALUE":
        return f"{src} {t} {m.param_id}={bytes(m.param_value, 'latin-1')[:8].hex() if isinstance(m.param_value, str) else m.param_value} type={m.param_type} {m.param_index + 1}/{m.param_count}"
    return f"{src} {m}"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", default="udpin:0.0.0.0:14550", help="PX4 GCS link (default udpin:0.0.0.0:14550)")
    ap.add_argument("--sysid", type=int, default=1, help="vehicle system ID")
    ap.add_argument("--camera", type=int, default=100, help="camera component ID")
    ap.add_argument("--listen", type=float, default=3.0, help="seconds to print replies (watch: forever)")
    ap.add_argument("--all", action="store_true", help="print status streams too (FOV/attitude status)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("watch")
    p = sub.add_parser("pitchyaw")
    p.add_argument("pitch", type=float)
    p.add_argument("yaw", type=float)
    p.add_argument("--earth-yaw", action="store_true", help="add GIMBAL_MANAGER_FLAGS_YAW_LOCK (yaw relative to north)")
    p.add_argument("--body", action="store_true", help="no ROLL_LOCK/PITCH_LOCK: angles follow the airframe")
    p = sub.add_parser("roi")
    p.add_argument("lat", type=float)
    p.add_argument("lon", type=float)
    p.add_argument("alt", type=float, help="m, relative to home")
    sub.add_parser("roi-none")
    p = sub.add_parser("zoom")
    p.add_argument("kind", choices=["step", "continuous", "range", "focal", "hfov"])
    p.add_argument("value", type=float)
    p = sub.add_parser("source")
    p.add_argument("which", choices=["eo", "ir"])
    sub.add_parser("capture")
    p = sub.add_parser("video")
    p.add_argument("action", choices=["start", "stop"])
    sub.add_parser("info")
    sub.add_parser("params")
    p = sub.add_parser("setparam")
    p.add_argument("name")
    p.add_argument("value", type=float)
    p.add_argument("--type", default="uint8", choices=list(PARAM_TYPES))
    args = ap.parse_args(argv)

    mav = mavutil.mavlink_connection(args.url, source_system=255, source_component=190, dialect="common")
    m = mav.mav
    print(f"fake_gcs: waiting for the vehicle on {args.url} ...")
    mav.wait_heartbeat(timeout=15)
    print(f"fake_gcs: vehicle {mav.target_system}/{mav.target_component}")
    sysid, cam = args.sysid, args.camera

    def cmd_long(target_comp, command, *params):
        ps = list(params) + [0.0] * (7 - len(params))
        m.command_long_send(sysid, target_comp, command, 0, *ps)

    c = args.cmd
    if c in ("pitchyaw", "roi", "roi-none"):
        # Take primary control first, as QGC does; PX4 denies the command otherwise.
        cmd_long(1, mavutil.mavlink.MAV_CMD_DO_GIMBAL_MANAGER_CONFIGURE, 255, 190, -1, -1, 0, 0, 0)
        time.sleep(0.2)
    if c == "pitchyaw":
        # Like QGC: roll and pitch horizon-locked (ROLL_LOCK 4 | PITCH_LOCK 8), yaw lock optional (16).
        flags = (0 if args.body else 4 | 8) | (16 if args.earth_yaw else 0)
        cmd_long(1, mavutil.mavlink.MAV_CMD_DO_GIMBAL_MANAGER_PITCHYAW, args.pitch, args.yaw, math.nan, math.nan, flags, 0, 0)
    elif c == "roi":
        m.command_int_send(sysid, 1, mavutil.mavlink.MAV_FRAME_GLOBAL_RELATIVE_ALT_INT,
                           mavutil.mavlink.MAV_CMD_DO_SET_ROI_LOCATION, 0, 0, 0, 0, 0, 0,
                           int(args.lat * 1e7), int(args.lon * 1e7), args.alt)
    elif c == "roi-none":
        cmd_long(1, mavutil.mavlink.MAV_CMD_DO_SET_ROI_NONE)
    elif c == "zoom":
        kind = {"step": 0, "continuous": 1, "range": 2, "focal": 3, "hfov": 4}[args.kind]
        cmd_long(cam, mavutil.mavlink.MAV_CMD_SET_CAMERA_ZOOM, kind, args.value)
    elif c == "source":
        cmd_long(cam, 534, 0, 2 if args.which == "ir" else 1, 0)
    elif c == "capture":
        cmd_long(cam, mavutil.mavlink.MAV_CMD_IMAGE_START_CAPTURE, 0, 0, 1, int(time.time()) % 100000)
    elif c == "video":
        cmd_long(cam, 2500 if args.action == "start" else 2501, 0, 1 if args.action == "start" else 0)
    elif c == "info":
        for mid in (259, 260, 261, 262, 271):
            cmd_long(cam, mavutil.mavlink.MAV_CMD_REQUEST_MESSAGE, mid)
        cmd_long(1, mavutil.mavlink.MAV_CMD_REQUEST_MESSAGE, 280)  # GIMBAL_MANAGER_INFORMATION
    elif c == "params":
        m.param_ext_request_list_send(sysid, cam)
    elif c == "setparam":
        tid, fmt = PARAM_TYPES[args.type]
        v = struct.pack(fmt, args.value if fmt == "<f" else int(args.value)).ljust(128, b"\0")
        m.param_ext_set_send(sysid, cam, args.name.encode(), v, tid)

    end = math.inf if c == "watch" else time.monotonic() + args.listen
    quiet = {"CAMERA_FOV_STATUS", "GIMBAL_DEVICE_ATTITUDE_STATUS", "GIMBAL_MANAGER_STATUS"}
    seen: set[str] = set()
    next_hb = 0.0
    while time.monotonic() < end:
        if time.monotonic() >= next_hb:
            next_hb = time.monotonic() + 1.0
            m.heartbeat_send(mavutil.mavlink.MAV_TYPE_GCS, mavutil.mavlink.MAV_AUTOPILOT_INVALID, 0, 0, 0)
        msg = mav.recv_match(type=list(INTERESTING), blocking=True, timeout=0.2)
        if msg is None:
            continue
        t = msg.get_type()
        if t in quiet and not args.all and t in seen:
            continue
        seen.add(t)
        print(describe(msg))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
