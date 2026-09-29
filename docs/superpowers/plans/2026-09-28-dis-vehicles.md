# DIS-Driven Trucks and Boats Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A scripted DIS truck and boat appear in CamSim's video sitting on the terrain / water, and the COCO ground truth labels them with stable, unique IDs.

**Architecture:** A Python DIS sender drives two entities. CamSim maps DIS domain → a surface mode on the entity command; the entity clamps every pose it commits (PDU or dead reckoning) with downward traces against Cesium tiles (pure clamp math + a trace probe). glTF models are preloaded at startup. Ground-truth entity snapshots ride in each readback-ring slot and carry a session-unique annotation ID plus the source key.

**Tech Stack:** UE 5.8 C++ (Cesium for Unreal, glTFRuntime), UE Automation tests (NullRHI + Metal GPU runner), Python 3.10+ stdlib, pytest.

**Spec:** `docs/superpowers/specs/2026-09-28-dis-vehicles-design.md`

## Global Constraints

- Branch: `feat/dis-vehicles`. Copyright header `// Copyright CamSim Contributors. All Rights Reserved.` on new C++ files; UE naming (`F`/`A`/`U`/`E`/`I`), verb-first functions, `#include "CoreMinimal.h"` first.
- Altitudes are WGS-84 ellipsoid heights everywhere; only EGM96 (`CamSim::Geospatial::GetGeoidUndulation`) converts to sea level.
- Never pass CIGI angles to `SetActorRotation`; poses go through `GlobeAnchor->MoveToLongitudeLatitudeHeight` + `SetEastSouthUpRotation(CamSimFrames::NeuToEastSouthUp(...))`.
- DIS sender: Python 3.10+, standard library only; ESPDU protocol version 7, 144 bytes, exercise 1, site:app 1:1, default `127.0.0.1:3000`; heartbeat 5 Hz plus a PDU when heading changes > 3°; DR algorithm 4; altitude 0 m.
- Truck DIS type `1:1:225:7:0:0:0` → CamSim type `2001` (`class_name: truck`); boat `1:3:225:7:0:0:0` → `3001` (`class_name: boat`).
- Clamp constants: first trace 9 000 m → −500 m; later last height + 50 m → last height − 500 m; ease time constant 0.2 s; snap at ≥ 5 m.
- Pitch = `atan((bow − stern) / (2·half_length))` nose-up positive; roll = `atan((port − stbd) / (2·half_beam))` right-side-down positive.
- Config key `dis.clamp_to_surface` (bool, default `true`, env `CAMSIM_DIS_CLAMP_TO_SURFACE`).
- Models: CC0 or CC-BY `.glb`, realistic, ≤ ~100 k triangles, ≤ ~20 MB, in `entities/truck/` and `entities/boat/` with `LICENSE.md`; git LFS (`.gitattributes` already covers `*.glb`).
- COCO `entity_id` is a `uint32` session-unique annotation ID; new fields `source`, `source_id` (from `FEntityKey::ToString()`: `dis:1.1.3` → `dis`, `1.1.3`).
- Test commands (from repo root):
  - NullRHI: `cp deploy/camsim_config.yaml unreal_project/CamSimTest/camsim_config.yaml` then `bash "$S/uetest.sh" <filter>` where `S=/private/tmp/claude-501/-Users-mclayton-developer-simulation-apps-camsim/53dfdfbd-0d12-4391-a124-e91cdf0f4e80/scratchpad` (the script runs `UnrealEditor ... -ExecCmds="Automation RunTests <filter>+Quit" -nullrhi -DisablePython` and prints `succeeded N warn N failed N` plus failures). Build first: `scripts/run.sh --build-only > .cache/build.log 2>&1; echo $?` (0 = OK; on failure `grep -n "error" .cache/build.log`).
  - GPU (Metal): `scripts/run_gpu_tests.sh <filter>`.
  - Python: `uv run -q --with pytest --with numpy --with pillow pytest -q scripts/tests`.

## Review Focus

1. Sender heading crossing 0°/360° → yaw rate must be the small real turn rate, not ±360°/Δt (DR would spin the vehicle). Test: Task 1 `test_yaw_rate_wraps`.
2. Entity with no footprint (no `half_length_m`/`half_beam_m`, mesh not loaded yet) → no NaN pose; height from the hits, tilt held. Test: Task 4 `ZeroFootprint`.
3. Probe returns a non-finite height (a `WorldToGeo` failure) → treated as a miss, never written to the pose. Test: Task 4 `NonFiniteHitIsMiss`.
4. A DIS entity removed and re-created with the same DIS ID → a new annotation ID; IDs never repeat within a session. Test: Task 2 `AnnotationIdsNeverRepeat` (allocator) — the manager assigns only at spawn (reviewer checks).
5. Config hot reload → preload runs again and meshes in use survive. Test: Task 7 `PreloadTwiceKeepsMeshes`.

---

### Task 1: DIS test sender

**Files:**
- Create: `scripts/send_dis_test.py`
- Test: `scripts/tests/test_send_dis.py`

**Interfaces:**
- Produces: `pack_entity_state(entity_id: int, entity_type: tuple[int,...7], lat: float, lon: float, alt: float, heading_deg: float, speed_mps: float, yaw_rate_dps: float, exercise: int = 1, marking: str = "", t: float = 0.0) -> bytes` (144 bytes); `geodetic_to_ecef(lat, lon, alt) -> tuple[float,float,float]`; `heading_to_dis_euler(heading_deg, lat, lon) -> tuple[psi, theta, phi]`; `class PathFollower(waypoints_ne: list[tuple[float,float]], speed_mps: float)` with `.state(t: float) -> tuple[north_m, east_m, heading_deg, yaw_rate_dps]`; `PRESETS: dict[str, Preset]`; constants `TRUCK_TYPE = (1,1,225,7,0,0,0)`, `BOAT_TYPE = (1,3,225,7,0,0,0)`.

- [ ] **Step 1: Verify the boat's DIS category.** Look up SISO-REF-010 surface-platform categories (web search "SISO-REF-010 surface platform category light patrol craft"). If category 7 is not "Light/Patrol Craft" (or similar small craft), use the correct small-craft value for `BOAT_TYPE` everywhere in this plan and in the spec, and note it in the report.

- [ ] **Step 2: Write the failing tests** — `scripts/tests/test_send_dis.py`:

```python
"""Unit tests for scripts/send_dis_test.py (PDU layout mirrors DIS/DisPduTypes.cpp)."""

import math
import struct

import send_dis_test as sd


def _pdu(**kw):
    args = dict(entity_id=3, entity_type=sd.TRUCK_TYPE, lat=37.795, lon=-122.46, alt=0.0,
                heading_deg=90.0, speed_mps=15.0, yaw_rate_dps=2.0, exercise=1, marking="TRUCK1", t=12.5)
    args.update(kw)
    return sd.pack_entity_state(**args)


def test_header_and_length():
    p = _pdu()
    assert len(p) == 144
    assert p[0] == 7 and p[1] == 1 and p[2] == 1 and p[3] == 1  # version, exercise, ESPDU, family
    assert struct.unpack(">H", p[8:10])[0] == 144


def test_entity_id_type_and_dr():
    p = _pdu()
    assert struct.unpack(">HHH", p[12:18]) == (1, 1, 3)
    assert p[19] == 0  # no articulation parameters
    kind, domain, country, cat, sub, spec, extra = struct.unpack(">BBHBBBB", p[20:28])
    assert (kind, domain, country, cat, sub, spec, extra) == sd.TRUCK_TYPE
    assert p[88] == 4  # DR algorithm 4
    assert p[129:135] == b"TRUCK1"


def test_location_is_ecef_of_lat_lon():
    p = _pdu()
    x, y, z = struct.unpack(">ddd", p[48:72])
    ex, ey, ez = sd.geodetic_to_ecef(37.795, -122.46, 0.0)
    assert (x, y, z) == (ex, ey, ez)
    assert abs(math.sqrt(x * x + y * y + z * z) - 6_370_000) < 20_000


def test_velocity_is_ecef_heading_east_at_equator():
    p = sd.pack_entity_state(entity_id=1, entity_type=sd.BOAT_TYPE, lat=0.0, lon=0.0, alt=0.0,
                             heading_deg=90.0, speed_mps=10.0, yaw_rate_dps=0.0)
    vx, vy, vz = struct.unpack(">fff", p[36:48])
    # At (0, 0) East is ECEF +Y.
    assert abs(vx) < 1e-5 and abs(vy - 10.0) < 1e-5 and abs(vz) < 1e-5


def test_euler_heading_east_at_equator():
    # BodyToEcef = NedToEcef(0,0) * Rz(90 deg) -> psi = +90 deg, theta = 0, phi = -90 deg.
    psi, theta, phi = sd.heading_to_dis_euler(90.0, 0.0, 0.0)
    assert abs(psi - math.pi / 2) < 1e-9 and abs(theta) < 1e-9 and abs(phi + math.pi / 2) < 1e-9


def test_angular_velocity_is_yaw_rate_in_body_z():
    p = _pdu(yaw_rate_dps=6.0)
    wx, wy, wz = struct.unpack(">fff", p[116:128])
    assert wx == 0.0 and wy == 0.0 and abs(wz - math.radians(6.0)) < 1e-6


def test_path_follower_stays_on_circle():
    boat = sd.PRESETS["boat-circle"]
    f = sd.PathFollower(boat.waypoints_ne, boat.speed_mps)
    for t in range(0, 300, 7):
        n, e, _, _ = f.state(float(t))
        assert abs(math.hypot(n, e) - boat.radius_m) < 5.0


def test_yaw_rate_wraps():
    # The boat circle's heading crosses 0/360 every lap; the yaw rate must stay the real
    # turn rate v/r (~3.06 deg/s), never a +-360 deg jump divided by the time step.
    boat = sd.PRESETS["boat-circle"]
    f = sd.PathFollower(boat.waypoints_ne, boat.speed_mps)
    expected = abs(boat.speed_mps / boat.radius_m * 180.0 / math.pi)
    headings, rates = [], []
    for t in range(0, 1300):  # 130 s > one lap (~118 s)
        _, _, h, r = f.state(t / 10.0)
        headings.append(h)
        rates.append(abs(r))
    assert min(headings) < 10.0 and max(headings) > 350.0  # the wrap happened
    assert max(rates) < expected * 1.5
```

- [ ] **Step 3: Run to verify it fails**

Run: `uv run -q --with pytest pytest -q scripts/tests/test_send_dis.py`
Expected: FAIL — `ModuleNotFoundError: No module named 'send_dis_test'`.

- [ ] **Step 4: Implement `scripts/send_dis_test.py`**

```python
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
BOAT_TYPE = (1, 3, 225, 7, 0, 0, 0)   # surface, USA, light/patrol craft

HEARTBEAT_S = 0.2        # 5 Hz
HEADING_THRESHOLD = 3.0  # degrees
TICK_HZ = 30.0


def geodetic_to_ecef(lat: float, lon: float, alt: float) -> tuple[float, float, float]:
    la, lo = math.radians(lat), math.radians(lon)
    n = WGS84_A / math.sqrt(1.0 - WGS84_E2 * math.sin(la) ** 2)
    return ((n + alt) * math.cos(la) * math.cos(lo),
            (n + alt) * math.cos(la) * math.sin(lo),
            (n * (1.0 - WGS84_E2) + alt) * math.sin(la))


def _ned_to_ecef(lat: float, lon: float) -> list[list[float]]:
    """Columns North, East, Down in ECEF (matches CamSimFrames::NedToEcef)."""
    sl, cl = math.sin(math.radians(lat)), math.cos(math.radians(lat))
    so, co = math.sin(math.radians(lon)), math.cos(math.radians(lon))
    return [[-sl * co, -so, -cl * co],
            [-sl * so, co, -cl * so],
            [cl, 0.0, -sl]]


def heading_to_dis_euler(heading_deg: float, lat: float, lon: float) -> tuple[float, float, float]:
    """Level body at a heading -> DIS psi/theta/phi (radians), as CamSimFrames::CigiToDisEuler."""
    h = math.radians(heading_deg)
    rz = [[math.cos(h), -math.sin(h), 0.0], [math.sin(h), math.cos(h), 0.0], [0.0, 0.0, 1.0]]
    m = _ned_to_ecef(lat, lon)
    r = [[sum(m[i][k] * rz[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
    psi = math.atan2(r[1][0], r[0][0])
    theta = -math.asin(max(-1.0, min(1.0, r[2][0])))
    phi = math.atan2(r[2][1], r[2][2])
    return psi, theta, phi


def _dis_timestamp(t: float) -> int:
    """Relative timestamp: units of 3600/2^31 s past the hour, low bit 0."""
    return (int((t % 3600.0) / 3600.0 * (1 << 31)) & 0x7FFFFFFF) << 1


def pack_entity_state(entity_id: int, entity_type: tuple, lat: float, lon: float, alt: float,
                      heading_deg: float, speed_mps: float, yaw_rate_dps: float,
                      exercise: int = 1, marking: str = "", t: float = 0.0) -> bytes:
    """One 144-byte IEEE 1278.1 Entity State PDU (layout: DIS/DisPduTypes.cpp)."""
    m = _ned_to_ecef(lat, lon)
    vn, ve = speed_mps * math.cos(math.radians(heading_deg)), speed_mps * math.sin(math.radians(heading_deg))
    vel = [m[i][0] * vn + m[i][1] * ve for i in range(3)]
    psi, theta, phi = heading_to_dis_euler(heading_deg, lat, lon)
    x, y, z = geodetic_to_ecef(lat, lon, alt)
    mark = marking.encode("ascii", "replace")[:11].ljust(11, b"\0")
    pdu = struct.pack(">BBBBIHH", 7, exercise, 1, 1, _dis_timestamp(t), 144, 0)
    pdu += struct.pack(">HHHBB", 1, 1, entity_id, 1, 0)       # site, app, entity, force, #art
    pdu += struct.pack(">BBHBBBB", *entity_type)
    pdu += bytes(8)                                            # alternative entity type
    pdu += struct.pack(">fff", *vel)
    pdu += struct.pack(">ddd", x, y, z)
    pdu += struct.pack(">fff", psi, theta, phi)
    pdu += struct.pack(">I", 0)                                # appearance
    pdu += struct.pack(">B", 4) + bytes(15)                    # DR algorithm 4 + other params
    pdu += struct.pack(">fff", 0.0, 0.0, 0.0)                  # linear acceleration
    pdu += struct.pack(">fff", 0.0, 0.0, math.radians(yaw_rate_dps))  # body angular velocity
    pdu += struct.pack(">B", 1) + mark                         # marking (ASCII)
    pdu += struct.pack(">I", 0)                                # capabilities
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
    return [(radius * math.cos(2 * math.pi * i / n), radius * math.sin(2 * math.pi * i / n)) for i in range(n)]


PRESETS: dict[str, Preset] = {
    "truck-loop": Preset("truck-loop", TRUCK_TYPE, (37.795, -122.460), 15.0,
                         [(-100.0, -150.0), (100.0, -150.0), (100.0, 150.0), (-100.0, 150.0)], marking="TRUCK1"),
    "boat-circle": Preset("boat-circle", BOAT_TYPE, (37.815, -122.440), 8.0, _circle(150.0), 150.0, "BOAT1"),
}


def ne_to_latlon(center: tuple[float, float], n: float, e: float) -> tuple[float, float]:
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
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("preset", nargs="?", default="both", choices=["both", *PRESETS])
    ap.add_argument("--addr", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=3000)
    ap.add_argument("--exercise", type=int, default=1)
    ap.add_argument("--location", help="LAT,LON: re-centre the selected preset(s)")
    ap.add_argument("--duration", type=float, default=0.0, help="seconds (0 = until Ctrl-C)")
    ap.add_argument("--rate", type=float, default=1.0 / HEARTBEAT_S, help="heartbeat Hz")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args(argv)

    names = list(PRESETS) if a.preset == "both" else [a.preset]
    tracks = []
    for i, name in enumerate(names, start=1):
        p = PRESETS[name]
        c = p.center
        if a.location:
            lat, lon = (float(v) for v in a.location.split(","))
            c = (lat + (0.002 * (i - 1)), lon)  # keep two re-centred presets apart (~220 m)
        tracks.append(_Track(i, p, c))

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
    heartbeat = 1.0 / max(a.rate, 0.1)
    t0 = time.monotonic()
    print(f"sending {', '.join(names)} to {a.addr}:{a.port} (exercise {a.exercise}); Ctrl-C to stop")
    try:
        while a.duration <= 0 or time.monotonic() - t0 < a.duration:
            t = time.monotonic() - t0
            for tr in tracks:
                n, e, h, rate = tr.follower.state(t)
                turned = abs((h - tr.last_heading + 180.0) % 360.0 - 180.0) > HEADING_THRESHOLD
                if t - tr.last_sent < heartbeat and not turned:
                    continue
                lat, lon = ne_to_latlon(tr.center, n, e)
                sock.sendto(pack_entity_state(tr.entity_id, tr.preset.entity_type, lat, lon, 0.0, h,
                                              tr.preset.speed_mps, rate, a.exercise, tr.preset.marking,
                                              time.time()), (a.addr, a.port))
                tr.last_sent, tr.last_heading = t, h
                if a.verbose:
                    print(f"{t:7.2f} {tr.preset.name:12s} {lat:.6f} {lon:.6f} hdg {h:6.1f} rate {rate:6.2f}")
            time.sleep(1.0 / TICK_HZ)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 5: Run the tests**

Run: `uv run -q --with pytest pytest -q scripts/tests/test_send_dis.py`
Expected: 8 passed. If `test_euler_heading_east_at_equator` fails, compare with `CamSimFrames::CigiToDisEuler` (`Geospatial/EcefFrames.h:139`) and `FRot3::ToEulerZYX`; the Python must match the C++ convention, not the other way round.

- [ ] **Step 6: Lint and commit**

```bash
chmod +x scripts/send_dis_test.py
uvx ruff check scripts/send_dis_test.py scripts/tests/test_send_dis.py
git add scripts/send_dis_test.py scripts/tests/test_send_dis.py
git commit -m "feat(dis): scripted DIS Entity State sender for a truck and a boat"
```

---

### Task 2: Ground truth — per-frame snapshots and stable IDs

**Files:**
- Modify: `Source/CamSimTest/GroundTruth/AnnotationTypes.h`, `GroundTruth/FGroundTruthCollector.{h,cpp}`, `GroundTruth/FCocoAnnotationWriter.cpp`, `GroundTruth/FVocAnnotationWriter.cpp`, `Camera/CamSimCaptureComponent.{h,cpp}`, `Entity/CamSimEntity.h`, `Entity/CamSimEntityManager.{h,cpp}`, docs that describe COCO/VOC fields (`grep -rn "entity_id" docs scripts`).
- Test: `Source/CamSimTest/Tests/GroundTruthTest.cpp` (append).

(All `Source/...` paths are under `unreal_project/CamSimTest/`.)

**Interfaces:**
- Produces: `FEntityAnnotationData { uint32 EntityId; uint16 EntityType; FString ClassName; FString Source; FString SourceId; FBox2D ScreenBBox; bool bVisible; bool bTruncated; }`; `struct FAnnotationIdAllocator { uint32 Allocate(); }` (starts at 1, never repeats); `void CamSimGroundTruth::SplitSourceKey(const FString& KeyString, FString& OutSource, FString& OutSourceId)`; `FGroundTruthCollector::WriteAnnotationFrame(const TArray<FEntityAnnotationData>& Entities, const FCamSimTelemetry& Telemetry, uint64 FrameIdx)`; `ACamSimEntity::AnnotationId` (`uint32`).
- Removes: `FGroundTruthCollector::SetPendingEntitySnapshot`, `PendingEntities`, `PendingImageWidth/Height`.

- [ ] **Step 1: Write the failing tests** — append to `Tests/GroundTruthTest.cpp` (add includes `GroundTruth/FGroundTruthCollector.h`, `Camera/CamSimTelemetry.h` or wherever `FCamSimTelemetry` is declared — `grep -rn "struct FCamSimTelemetry" Source`, `HAL/FileManager.h`, `Misc/FileHelper.h`, `Misc/Paths.h`):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthAnnotationIdsTest,
	"CamSim.GroundTruth.AnnotationIdsNeverRepeat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundTruthAnnotationIdsTest::RunTest(const FString& Parameters)
{
	FAnnotationIdAllocator Ids;
	TSet<uint32> Seen;
	for (int32 i = 0; i < 1000; ++i)
	{
		const uint32 Id = Ids.Allocate();
		TestTrue(TEXT("non-zero"), Id != 0);
		TestFalse(TEXT("never repeats"), Seen.Contains(Id));
		Seen.Add(Id);
	}

	// A DIS key and a CIGI key with the same low 16 bits stay distinguishable.
	FString Src, SrcId;
	CamSimGroundTruth::SplitSourceKey(FEntityKey(EHostSource::Dis, (1ull << 32) | (1ull << 16) | 7).ToString(), Src, SrcId);
	TestEqual(TEXT("dis source"), Src, FString(TEXT("dis")));
	TestEqual(TEXT("dis source id"), SrcId, FString(TEXT("1.1.7")));
	CamSimGroundTruth::SplitSourceKey(FEntityKey(EHostSource::Cigi, 7).ToString(), Src, SrcId);
	TestEqual(TEXT("cigi source"), Src, FString(TEXT("cigi")));
	TestEqual(TEXT("cigi source id"), SrcId, FString(TEXT("7")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthPerFrameSnapshotTest,
	"CamSim.GroundTruth.PerFrameSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGroundTruthPerFrameSnapshotTest::RunTest(const FString& Parameters)
{
	const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("gt_per_frame_test"));
	IFileManager::Get().DeleteDirectory(*Dir, false, true);

	FCamSimConfig Cfg;
	Cfg.MLTraining.bEnabled = true;
	Cfg.MLTraining.OutputDir = Dir;
	Cfg.MLTraining.bBoundingBoxes = true;
	Cfg.MLTraining.bCocoExport = true;
	Cfg.MLTraining.bVocExport = false;
	Cfg.MLTraining.bDepthMap = false;
	Cfg.MLTraining.AnnotationIntervalFrames = 1;

	auto Make = [](uint32 Id, const TCHAR* Cls, const TCHAR* SrcId)
	{
		FEntityAnnotationData E;
		E.EntityId = Id; E.EntityType = 2001; E.ClassName = Cls;
		E.Source = TEXT("dis"); E.SourceId = SrcId;
		E.ScreenBBox = FBox2D(FVector2D(10, 20), FVector2D(50, 60));
		E.bVisible = true;
		return E;
	};
	{
		FGroundTruthCollector Collector(Cfg);
		TestTrue(TEXT("opened"), Collector.Open());
		FCamSimTelemetry Tel;
		// Two frames in flight, delivered out of order: each writes its own entities.
		const TArray<FEntityAnnotationData> Frame1 = { Make(70000, TEXT("truck"), TEXT("1.1.1")) };
		const TArray<FEntityAnnotationData> Frame2 = { Make(70001, TEXT("boat"), TEXT("1.1.2")) };
		Collector.WriteAnnotationFrame(Frame2, Tel, 2);
		Collector.WriteAnnotationFrame(Frame1, Tel, 1);
		Collector.Close();
	}

	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *Dir, TEXT("*.jsonl"), true, false);
	if (!TestEqual(TEXT("one COCO file"), Files.Num(), 1)) return false;
	TArray<FString> Lines;
	FFileHelper::LoadFileToStringArray(Lines, *Files[0]);
	if (!TestEqual(TEXT("two lines"), Lines.Num(), 2)) return false;
	TestTrue(TEXT("frame 2 has the boat"), Lines[0].Contains(TEXT("\"frame_id\":2")) && Lines[0].Contains(TEXT("\"entity_id\":70001")) && Lines[0].Contains(TEXT("\"name\":\"boat\"")));
	TestTrue(TEXT("frame 1 has the truck"), Lines[1].Contains(TEXT("\"frame_id\":1")) && Lines[1].Contains(TEXT("\"entity_id\":70000")) && Lines[1].Contains(TEXT("\"name\":\"truck\"")));
	TestTrue(TEXT("source fields"), Lines[1].Contains(TEXT("\"source\":\"dis\",\"source_id\":\"1.1.1\"")));
	return true;
}
```

Check the actual `FCamSimConfig::MLTraining` field names and the COCO file name/location in `FCocoAnnotationWriter::Open` and `FGroundTruthCollector::Open`; adjust the test to the real names (the test's intent is fixed: two frames, out of order, each with its own entities, IDs above 65535, source fields present).

- [ ] **Step 2: Build and run to verify it fails**

Run: `scripts/run.sh --build-only > .cache/build.log 2>&1; echo $?`
Expected: non-zero — `FAnnotationIdAllocator`, `SplitSourceKey`, the new `WriteAnnotationFrame` overload and the new fields don't exist.

- [ ] **Step 3: Implement**

`AnnotationTypes.h`:

```cpp
struct FEntityAnnotationData
{
	uint32  EntityId   = 0;      // session-unique annotation ID (FAnnotationIdAllocator), stable for the entity's life
	uint16  EntityType = 0;
	FString ClassName;           // from FEntityTypeEntry::ClassName; falls back to "type_NNNN"
	FString Source;              // host source: "dis", "cigi", "scenario", ...
	FString SourceId;            // the source's own ID: "1.1.3" (DIS site.app.entity), "7" (CIGI)
	FBox2D  ScreenBBox = FBox2D(ForceInit);
	bool    bVisible   = false;
	bool    bTruncated = false;
};

/** Hands out annotation IDs: from 1, never repeated within a session (game thread). */
struct FAnnotationIdAllocator
{
	uint32 Allocate() { return Next++; }
private:
	uint32 Next = 1;
};

namespace CamSimGroundTruth
{
	/** "dis:1.1.3" (FEntityKey::ToString) → "dis", "1.1.3". */
	inline void SplitSourceKey(const FString& KeyString, FString& OutSource, FString& OutSourceId)
	{
		if (!KeyString.Split(TEXT(":"), &OutSource, &OutSourceId))
		{
			OutSource = KeyString;
			OutSourceId.Reset();
		}
	}
}
```

`FGroundTruthCollector`: delete `SetPendingEntitySnapshot` and the pending members; change `WriteAnnotationFrame` to take `const TArray<FEntityAnnotationData>& Entities` and filter `Entities` (instead of `PendingEntities`) for `bVisible`. Update the header's thread-model comment: "The game thread builds each frame's entity snapshot into that frame's readback-ring slot; the background task passes it here. No state is shared between frames."

`FCocoAnnotationWriter::WriteFrame`: the per-annotation printf becomes

```cpp
		FString SafeSource = E.Source.Replace(TEXT("\""), TEXT("\\\""));
		FString SafeSourceId = E.SourceId.Replace(TEXT("\""), TEXT("\\\""));
		Line += FString::Printf(
			TEXT("{\"entity_id\":%u,\"source\":\"%s\",\"source_id\":\"%s\",\"category\":{\"id\":%u,\"name\":\"%s\"},")
			TEXT("\"bbox\":[%.1f,%.1f,%.1f,%.1f],\"area\":%.1f,")
			TEXT("\"iscrowd\":0,\"truncated\":%d}"),
			E.EntityId, *SafeSource, *SafeSourceId,
			E.EntityType, *SafeName,
			X, Y, W, H, W * H,
			E.bTruncated ? 1 : 0);
```

`FVocAnnotationWriter`: inside each `<object>` after `<name>`, add `<entity_id>%u</entity_id>`, `<source>%s</source>`, `<source_id>%s</source_id>` (XML-escape the two strings the same way `<name>` is escaped).

`ACamSimEntity` (`CamSimEntity.h`, next to `EntityId`): `uint32 AnnotationId = 0;  // session-unique ground-truth ID, set at spawn`.

`FCamSimEntityManager`: add member `FAnnotationIdAllocator AnnotationIds;` (include `GroundTruth/AnnotationTypes.h`); in the spawn function after `Entity->EntityId = ...` add `Entity->AnnotationId = AnnotationIds.Allocate();`. In `GetEntitySnapshot`: `Data.EntityId = Entity->AnnotationId;` and `CamSimGroundTruth::SplitSourceKey(Entity->Key.ToString(), Data.Source, Data.SourceId);`.

`UCamSimCaptureComponent`:
- `FSlot` gains `TArray<FEntityAnnotationData> Entities;  // game thread fills at capture; moved to the background task`.
- `SnapshotGroundTruthEntities()` becomes `TArray<FEntityAnnotationData> BuildGroundTruthSnapshot() const` returning the snapshot (empty when the collector is off) instead of calling `SetPendingEntitySnapshot`.
- In `Capture()`: replace `if (Subsystem) SnapshotGroundTruthEntities();` with `S.Entities = Subsystem ? BuildGroundTruthSnapshot() : TArray<FEntityAnnotationData>();` after `FSlot& S = Slots[Slot];`.
- `SubmitFrameToEncoder(...)` gains `TArray<FEntityAnnotationData> Entities` as last parameter; every caller passes `MoveTemp(S.Entities)` (check both call sites — `grep -n "SubmitFrameToEncoder" Camera/CamSimCaptureComponent.cpp`); the lambda captures `Entities = MoveTemp(Entities)` and calls `Collector->WriteAnnotationFrame(Entities, Telemetry, FrameIdx);`.
- Any path that fails/recycles a slot without submitting calls `S.Entities.Reset()`.

- [ ] **Step 4: Update COCO/VOC docs and readers.** `grep -rn "entity_id\|camsim_coco" docs scripts` — document `entity_id` (32-bit, session-unique), `source`, `source_id` wherever the COCO/VOC record is described; update any script that parses these files.

- [ ] **Step 5: Build and run the tests**

Run: `scripts/run.sh --build-only > .cache/build.log 2>&1; echo $?` → `0`; then `bash "$S/uetest.sh" CamSim.GroundTruth` → all pass; then `bash "$S/uetest.sh" CamSim` → 0 failed.

- [ ] **Step 6: Commit**

```bash
git add -A unreal_project/CamSimTest/Source docs scripts
git commit -m "fix(ground-truth): per-frame entity snapshots in the readback ring; unique annotation IDs with source keys"
```

---

### Task 3: Surface mode on entity commands

**Files:**
- Modify: `Source/CamSimTest/Sim/Commands.h`, `Hosts/CigiCommands.cpp`, `Hosts/DisCommands.{h,cpp}`, `DIS/DisEntityAdapter.cpp`, `Config/CamSimConfig.{h,cpp}`, `deploy/camsim_config.yaml`, `docs/configuration.md`
- Test: `Tests/HostCommandsTest.cpp` (update the clamp assertion; append), `Tests/DisProtocolTest.cpp` (append config test)

**Interfaces:**
- Produces: `enum class ESurfaceMode : uint8 { None, Ground, Water };` in `Sim/Commands.h`; `FEntityCommand::SurfaceMode` (replaces `bClampToTerrain`); `ESurfaceMode CamSim::Dis::SurfaceModeForDomain(uint8 Domain, bool bClampToSurface)`; `FEntityCommand CamSim::Dis::ToEntityCommand(const FDisEntityStatePdu& Pdu, uint16 TypeId, bool bClampToSurface = true)`; `FCamSimConfig::FDisConfig::bClampToSurface` (default `true`).

- [ ] **Step 1: Write the failing tests** — in `Tests/HostCommandsTest.cpp` replace the conformal-clamp assertion on `bClampToTerrain` with `TestTrue(TEXT("conformal clamp follows the ground"), Out.SurfaceMode == ESurfaceMode::Ground);` (use the test's actual variable name), then append:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDisSurfaceModeTest,
	"CamSim.Dis.SurfaceMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDisSurfaceModeTest::RunTest(const FString& Parameters)
{
	using CamSim::Dis::SurfaceModeForDomain;
	TestTrue(TEXT("land → ground"),   SurfaceModeForDomain(1, true) == ESurfaceMode::Ground);
	TestTrue(TEXT("air → none"),      SurfaceModeForDomain(2, true) == ESurfaceMode::None);
	TestTrue(TEXT("surface → water"), SurfaceModeForDomain(3, true) == ESurfaceMode::Water);
	TestTrue(TEXT("subsurface → none"), SurfaceModeForDomain(4, true) == ESurfaceMode::None);
	TestTrue(TEXT("clamp off → none"), SurfaceModeForDomain(1, false) == ESurfaceMode::None);

	FDisEntityStatePdu Pdu;
	Pdu.EntityType.EntityKind = 1; Pdu.EntityType.Domain = 1;
	Pdu.LocationX = 6378137.0;
	TestTrue(TEXT("command carries ground"), CamSim::Dis::ToEntityCommand(Pdu, 2001).SurfaceMode == ESurfaceMode::Ground);
	TestTrue(TEXT("command honours clamp off"), CamSim::Dis::ToEntityCommand(Pdu, 2001, false).SurfaceMode == ESurfaceMode::None);
	return true;
}
```

And in `Tests/DisProtocolTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDisClampConfigTest,
	"CamSim.Dis.ClampToSurfaceConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDisClampConfigTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("on by default"), FCamSimConfig().DIS.bClampToSurface);
	const FCamSimConfig Yaml = FCamSimConfig::LoadFromYamlString(TEXT("dis:\n  clamp_to_surface: false\n"));
	TestFalse(TEXT("yaml value used"), Yaml.DIS.bClampToSurface);
	TestEqual(TEXT("no unknown keys"), Yaml.UnknownYamlKeys.Num(), 0);
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_DIS_CLAMP_TO_SURFACE"), TEXT("1"));
	const FCamSimConfig Env = FCamSimConfig::LoadFromYamlString(TEXT("dis:\n  clamp_to_surface: false\n"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_DIS_CLAMP_TO_SURFACE"), TEXT(""));
	TestTrue(TEXT("env overrides yaml"), Env.DIS.bClampToSurface);
	return true;
}
```

- [ ] **Step 2: Build to verify it fails** — `scripts/run.sh --build-only > .cache/build.log 2>&1; echo $?` → non-zero (`ESurfaceMode` undefined).

- [ ] **Step 3: Implement**

`Sim/Commands.h` (before `FEntityCommand`):

```cpp
/** Where an entity sits: as sent, on the terrain, or on the water surface. */
enum class ESurfaceMode : uint8
{
	None,    // pose as sent (aircraft, entities with true heights)
	Ground,  // height, pitch and roll from the terrain under the footprint
	Water,   // height from the rendered water surface (EGM96 sea level if none)
};
```

In `FEntityCommand` replace `bool bClampToTerrain = false;` and its comment with:

```cpp
	/** Surface placement, applied to every pose the entity commits (DIS domain, CIGI conformal clamp). */
	ESurfaceMode SurfaceMode = ESurfaceMode::None;
```

`Hosts/CigiCommands.cpp`: `Out.bClampToTerrain = true;` → `Out.SurfaceMode = ESurfaceMode::Ground;`.

`Hosts/DisCommands.h/.cpp`:

```cpp
	/** DIS domain → surface placement: land on the ground, surface platforms on the water. */
	ESurfaceMode SurfaceModeForDomain(uint8 Domain, bool bClampToSurface)
	{
		if (!bClampToSurface) return ESurfaceMode::None;
		switch (Domain)
		{
		case 1:  return ESurfaceMode::Ground;
		case 3:  return ESurfaceMode::Water;
		default: return ESurfaceMode::None;
		}
	}
```

`ToEntityCommand` gains `bool bClampToSurface` (header default `= true`) and sets `Out.SurfaceMode = SurfaceModeForDomain(Pdu.EntityType.Domain, bClampToSurface);`.

`DIS/DisEntityAdapter.cpp:50`: `Sink.Submit(CamSim::Dis::ToEntityCommand(Pdu, MapEntityType(Pdu.EntityType), Config.DIS.bClampToSurface));`

`Config/CamSimConfig.h` in `FDisConfig`: 

```cpp
		// Place land entities on the terrain and surface entities on the water
		// (senders without terrain). false = use the sender's altitude/attitude.
		// Env: CAMSIM_DIS_CLAMP_TO_SURFACE
		bool    bClampToSurface       = true;
```

`CamSimConfig.cpp`: YAML `YamlBool  (D, "clamp_to_surface",       Cfg.DIS.bClampToSurface);` next to the other `dis` keys; env `D.bClampToSurface = GetEnvInt(TEXT("CAMSIM_DIS_CLAMP_TO_SURFACE"), D.bClampToSurface ? 1 : 0) != 0;` next to the other DIS env reads.

`deploy/camsim_config.yaml` under `dis:` after `default_entity_type_id`:

```yaml
  # Place land platforms (domain 1) on the terrain and surface platforms (domain 3)
  # on the water; heights and attitudes are otherwise used as sent. Set false for a
  # sender that supplies true terrain heights.
  # Env: CAMSIM_DIS_CLAMP_TO_SURFACE
  clamp_to_surface: true
```

`docs/configuration.md`: add a row to the DIS table: `| \`dis.clamp_to_surface\` | bool | \`true\` | \`CAMSIM_DIS_CLAMP_TO_SURFACE\` | Place land platforms on the terrain (height, pitch, roll) and surface platforms on the water. \`false\` = the sender's altitude and attitude. |` (match the table's column layout).

`grep -rn "bClampToTerrain" unreal_project/CamSimTest/Source` must return nothing afterwards.

- [ ] **Step 4: Build and run** — build → `0`; `bash "$S/uetest.sh" CamSim.Dis` and `bash "$S/uetest.sh" CamSim.Host` → all pass.

- [ ] **Step 5: Commit**

```bash
git add -A unreal_project/CamSimTest/Source deploy/camsim_config.yaml docs/configuration.md
git commit -m "feat(dis): surface mode on entity commands (land → ground, surface → water; CIGI conformal clamp → ground)"
```

---

### Task 4: Surface clamp math

**Files:**
- Create: `Source/CamSimTest/Entity/SurfaceClamp.h`, `Source/CamSimTest/Entity/SurfaceClamp.cpp`
- Test: `Source/CamSimTest/Tests/SurfaceClampTest.cpp`

**Interfaces:**
- Consumes: `CamSimFrames::FGeoPose`, `CamSimFrames::CigiToNeu`, `CamSimFrames::OffsetGeodetic` (`Geospatial/CigiFrames.h`).
- Produces (namespace `CamSimSurface`):
  - constants `FirstTraceTopM = 9000.0`, `FirstTraceBottomM = -500.0`, `TraceAboveM = 50.0`, `TraceBelowM = 500.0`, `EaseTimeConstantSec = 0.2`, `SnapThresholdM = 5.0`;
  - `struct FClampState { bool bHasSurface = false; double Height = 0.0; double PitchDeg = 0.0; double RollDeg = 0.0; };`
  - `struct FTraceSpan { double TopM; double BottomM; };` `FTraceSpan GetTraceSpan(const FClampState& State);`
  - `struct FFootprint { double Lat[4]; double Lon[4]; };` (order: bow, stern, port, stbd) `FFootprint GetFootprint(double LatDeg, double LonDeg, double HeadingDeg, double HalfLengthM, double HalfBeamM);`
  - `struct FGroundHits { TOptional<double> Bow, Stern, Port, Stbd; };`
  - `CamSimFrames::FGeoPose ClampGround(const CamSimFrames::FGeoPose& Sender, const FGroundHits& Hits, double HalfLengthM, double HalfBeamM, double DtSec, FClampState& State);`
  - `CamSimFrames::FGeoPose ClampWater(const CamSimFrames::FGeoPose& Sender, TOptional<double> CentreHit, TOptional<double> SeaLevelM, double DtSec, FClampState& State);`

- [ ] **Step 1: Write the failing tests** — `Tests/SurfaceClampTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Entity/SurfaceClamp.h"
#include "Geospatial/CigiFrames.h"

#include <limits>

using namespace CamSimSurface;

namespace
{
	CamSimFrames::FGeoPose Sender(double HeadingDeg = 30.0, double Alt = 0.0)
	{
		CamSimFrames::FGeoPose P;
		P.Lat = 37.795; P.Lon = -122.46; P.Alt = Alt;
		P.Neu = CamSimFrames::CigiToNeu(HeadingDeg, 0.0, 0.0);
		return P;
	}
	FGroundHits Hits(double Bow, double Stern, double Port, double Stbd)
	{
		FGroundHits H; H.Bow = Bow; H.Stern = Stern; H.Port = Port; H.Stbd = Stbd; return H;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfaceClampGroundTest, "CamSim.Entity.SurfaceClamp.Ground",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSurfaceClampGroundTest::RunTest(const FString& Parameters)
{
	FClampState S;
	// Bow 1 m above stern over 10 m; starboard 1 m below port over 2 m.
	const CamSimFrames::FGeoPose P = ClampGround(Sender(), Hits(101.0, 99.0, 100.5, 99.5), 5.0, 1.0, 0.0, S);
	const FRotator R = P.Neu.Rotator();
	TestEqual(TEXT("height = mean of hits"), P.Alt, 100.0, 1e-9);
	TestEqual(TEXT("heading kept"), R.Yaw, 30.0, 1e-6);
	TestEqual(TEXT("nose up"), R.Pitch, FMath::RadiansToDegrees(FMath::Atan(2.0 / 10.0)), 1e-6);
	TestEqual(TEXT("right side down = +roll"), R.Roll, FMath::RadiansToDegrees(FMath::Atan(1.0 / 2.0)), 1e-6);
	TestEqual(TEXT("lat/lon kept"), P.Lat, 37.795, 1e-12);
	TestTrue(TEXT("state has surface"), S.bHasSurface);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfaceClampMissTest, "CamSim.Entity.SurfaceClamp.Misses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSurfaceClampMissTest::RunTest(const FString& Parameters)
{
	FClampState S;
	// Before any hit: the sender's pose.
	CamSimFrames::FGeoPose P = ClampGround(Sender(30.0, 12.0), FGroundHits(), 5.0, 1.0, 0.1, S);
	TestEqual(TEXT("no hit yet → sender alt"), P.Alt, 12.0, 1e-9);
	TestFalse(TEXT("no surface yet"), S.bHasSurface);

	ClampGround(Sender(), Hits(101.0, 99.0, 100.0, 100.0), 5.0, 1.0, 0.0, S);
	const double Pitch = S.PitchDeg;

	// Partial hits: height from the hits (small step eases), tilt held.
	FGroundHits Partial; Partial.Bow = 100.5; Partial.Port = 100.5;
	P = ClampGround(Sender(), Partial, 5.0, 1.0, 10.0, S);  // Dt >> tau → fully eased
	TestEqual(TEXT("partial: mean of hits"), P.Alt, 100.5, 1e-3);
	TestEqual(TEXT("partial: pitch held"), S.PitchDeg, Pitch, 1e-9);

	// No hits: hold.
	P = ClampGround(Sender(), FGroundHits(), 5.0, 1.0, 0.1, S);
	TestEqual(TEXT("miss: height held"), P.Alt, 100.5, 1e-3);
	TestEqual(TEXT("miss: pitch held"), P.Neu.Rotator().Pitch, Pitch, 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfaceClampEaseTest, "CamSim.Entity.SurfaceClamp.EaseAndSnap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSurfaceClampEaseTest::RunTest(const FString& Parameters)
{
	FClampState S;
	ClampGround(Sender(), Hits(100, 100, 100, 100), 5.0, 1.0, 0.0, S);
	// 2 m step over one 30 fps frame: eases by 1 - exp(-dt/tau).
	const double Dt = 1.0 / 30.0;
	CamSimFrames::FGeoPose P = ClampGround(Sender(), Hits(102, 102, 102, 102), 5.0, 1.0, Dt, S);
	TestEqual(TEXT("small step eases"), P.Alt, 100.0 + 2.0 * (1.0 - FMath::Exp(-Dt / EaseTimeConstantSec)), 1e-9);
	// 50 m step snaps.
	P = ClampGround(Sender(), Hits(150, 150, 150, 150), 5.0, 1.0, Dt, S);
	TestEqual(TEXT("big step snaps"), P.Alt, 150.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfaceClampEdgeTest, "CamSim.Entity.SurfaceClamp.EdgeCases",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSurfaceClampEdgeTest::RunTest(const FString& Parameters)
{
	// ZeroFootprint: no half sizes → no NaN, height from hits, tilt held (zero).
	FClampState S;
	CamSimFrames::FGeoPose P = ClampGround(Sender(), Hits(100, 100, 100, 100), 0.0, 0.0, 0.0, S);
	TestEqual(TEXT("zero footprint height"), P.Alt, 100.0, 1e-9);
	TestFalse(TEXT("zero footprint: finite pose"), P.Neu.ContainsNaN());
	TestEqual(TEXT("zero footprint: level"), P.Neu.Rotator().Pitch, 0.0, 1e-6);

	// NonFiniteHitIsMiss: NaN/inf heights are ignored.
	FClampState S2;
	const double Nan = std::numeric_limits<double>::quiet_NaN();
	const double Inf = std::numeric_limits<double>::infinity();
	P = ClampGround(Sender(30.0, 7.0), Hits(Nan, Inf, Nan, Nan), 5.0, 1.0, 0.0, S2);
	TestEqual(TEXT("non-finite → miss"), P.Alt, 7.0, 1e-9);
	TestFalse(TEXT("non-finite → no surface"), S2.bHasSurface);

	// Water: hit, then no hit → EGM96 sea level, then neither → sender.
	FClampState W;
	P = ClampWater(Sender(10.0, 0.0), -31.5, -32.0, 0.0, W);
	TestEqual(TEXT("water hit"), P.Alt, -31.5, 1e-9);
	P = ClampWater(Sender(10.0, 0.0), {}, -32.0, 10.0, W);
	TestEqual(TEXT("water miss → sea level"), P.Alt, -32.0, 1e-3);
	TestEqual(TEXT("water keeps heading"), P.Neu.Rotator().Yaw, 10.0, 1e-6);
	FClampState W2;
	P = ClampWater(Sender(10.0, 3.0), {}, {}, 0.0, W2);
	TestEqual(TEXT("no water, no geoid → sender"), P.Alt, 3.0, 1e-9);

	// Trace span: first from 9 km, then around the last height.
	FClampState Fresh;
	TestEqual(TEXT("first top"), GetTraceSpan(Fresh).TopM, FirstTraceTopM);
	TestEqual(TEXT("first bottom"), GetTraceSpan(Fresh).BottomM, FirstTraceBottomM);
	TestEqual(TEXT("later top"), GetTraceSpan(S).TopM, 100.0 + TraceAboveM, 1e-9);
	TestEqual(TEXT("later bottom"), GetTraceSpan(S).BottomM, 100.0 - TraceBelowM, 1e-9);

	// Footprint: heading north, bow is north of stern, starboard east of port.
	const FFootprint F = GetFootprint(37.795, -122.46, 0.0, 5.0, 1.0);
	TestTrue(TEXT("bow north"), F.Lat[0] > F.Lat[1]);
	TestTrue(TEXT("stbd east"), F.Lon[3] > F.Lon[2]);
	return true;
}
```

- [ ] **Step 2: Build to verify it fails** — build → non-zero (`Entity/SurfaceClamp.h` not found).

- [ ] **Step 3: Implement** — `Entity/SurfaceClamp.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Geospatial/CigiFrames.h"

/**
 * Surface placement math (pure; the traces are done by ISurfaceProbe).
 * Heights are WGS-84 ellipsoid metres. Angles follow CIGI: pitch nose-up
 * positive, roll right-side-down positive.
 */
namespace CamSimSurface
{
	constexpr double FirstTraceTopM      = 9000.0;
	constexpr double FirstTraceBottomM   = -500.0;
	constexpr double TraceAboveM         = 50.0;   // above the last surface: an overpass stays above
	constexpr double TraceBelowM         = 500.0;
	constexpr double EaseTimeConstantSec = 0.2;    // hides tile refinement shifts
	constexpr double SnapThresholdM      = 5.0;    // first hit, teleports

	struct FClampState
	{
		bool   bHasSurface = false;
		double Height      = 0.0;
		double PitchDeg    = 0.0;
		double RollDeg     = 0.0;
	};

	struct FTraceSpan { double TopM = 0.0; double BottomM = 0.0; };
	FTraceSpan GetTraceSpan(const FClampState& State);

	/** Probe points, in order bow, stern, port, starboard. */
	struct FFootprint { double Lat[4] = {}; double Lon[4] = {}; };
	FFootprint GetFootprint(double LatDeg, double LonDeg, double HeadingDeg, double HalfLengthM, double HalfBeamM);

	struct FGroundHits { TOptional<double> Bow, Stern, Port, Stbd; };

	/** Ground vehicles: height from the hits, pitch/roll from the footprint, heading from the sender. */
	CamSimFrames::FGeoPose ClampGround(const CamSimFrames::FGeoPose& Sender, const FGroundHits& Hits,
		double HalfLengthM, double HalfBeamM, double DtSec, FClampState& State);

	/** Surface vessels: height from the water hit, else EGM96 sea level, else the sender; attitude from the sender. */
	CamSimFrames::FGeoPose ClampWater(const CamSimFrames::FGeoPose& Sender, TOptional<double> CentreHit,
		TOptional<double> SeaLevelM, double DtSec, FClampState& State);
}
```

`Entity/SurfaceClamp.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Entity/SurfaceClamp.h"

namespace CamSimSurface
{
	namespace
	{
		TOptional<double> Finite(const TOptional<double>& V)
		{
			return (V.IsSet() && FMath::IsFinite(*V)) ? V : TOptional<double>();
		}

		double Ease(double Prev, double Target, double DtSec)
		{
			if (FMath::Abs(Target - Prev) >= SnapThresholdM || DtSec <= 0.0) return Target;
			return Prev + (Target - Prev) * (1.0 - FMath::Exp(-DtSec / EaseTimeConstantSec));
		}

		double EaseAngle(double Prev, double Target, double DtSec)
		{
			return DtSec <= 0.0 ? Target : Prev + (Target - Prev) * (1.0 - FMath::Exp(-DtSec / EaseTimeConstantSec));
		}

		CamSimFrames::FGeoPose WithSurface(const CamSimFrames::FGeoPose& Sender, double Height, double PitchDeg, double RollDeg)
		{
			CamSimFrames::FGeoPose Out = Sender;
			Out.Alt = Height;
			Out.Neu = CamSimFrames::CigiToNeu(Sender.Neu.Rotator().Yaw, PitchDeg, RollDeg);
			return Out;
		}
	}

	FTraceSpan GetTraceSpan(const FClampState& State)
	{
		if (!State.bHasSurface) return { FirstTraceTopM, FirstTraceBottomM };
		return { State.Height + TraceAboveM, State.Height - TraceBelowM };
	}

	FFootprint GetFootprint(double LatDeg, double LonDeg, double HeadingDeg, double HalfLengthM, double HalfBeamM)
	{
		const double H = FMath::DegreesToRadians(HeadingDeg);
		const FVector Fwd(FMath::Cos(H), FMath::Sin(H), 0.0);     // North, East, Up
		const FVector Right(-FMath::Sin(H), FMath::Cos(H), 0.0);
		const FVector Offsets[4] = { Fwd * HalfLengthM, -Fwd * HalfLengthM, -Right * HalfBeamM, Right * HalfBeamM };
		FFootprint F;
		for (int32 i = 0; i < 4; ++i)
		{
			double Alt;
			CamSimFrames::OffsetGeodetic(LatDeg, LonDeg, 0.0, Offsets[i], F.Lat[i], F.Lon[i], Alt);
		}
		return F;
	}

	CamSimFrames::FGeoPose ClampGround(const CamSimFrames::FGeoPose& Sender, const FGroundHits& InHits,
		double HalfLengthM, double HalfBeamM, double DtSec, FClampState& State)
	{
		const TOptional<double> Bow = Finite(InHits.Bow), Stern = Finite(InHits.Stern);
		const TOptional<double> Port = Finite(InHits.Port), Stbd = Finite(InHits.Stbd);

		double Sum = 0.0;
		int32  Count = 0;
		for (const TOptional<double>* H : { &Bow, &Stern, &Port, &Stbd })
		{
			if (H->IsSet()) { Sum += **H; ++Count; }
		}
		if (Count == 0)
		{
			return State.bHasSurface ? WithSurface(Sender, State.Height, State.PitchDeg, State.RollDeg) : Sender;
		}

		const double Target = Sum / Count;
		const bool bFirst = !State.bHasSurface;
		State.Height = bFirst ? Target : Ease(State.Height, Target, DtSec);

		if (Count == 4 && HalfLengthM > 0.0 && HalfBeamM > 0.0)
		{
			const double Pitch = FMath::RadiansToDegrees(FMath::Atan((*Bow - *Stern) / (2.0 * HalfLengthM)));
			const double Roll  = FMath::RadiansToDegrees(FMath::Atan((*Port - *Stbd) / (2.0 * HalfBeamM)));
			State.PitchDeg = bFirst ? Pitch : EaseAngle(State.PitchDeg, Pitch, DtSec);
			State.RollDeg  = bFirst ? Roll  : EaseAngle(State.RollDeg,  Roll,  DtSec);
		}
		State.bHasSurface = true;
		return WithSurface(Sender, State.Height, State.PitchDeg, State.RollDeg);
	}

	CamSimFrames::FGeoPose ClampWater(const CamSimFrames::FGeoPose& Sender, TOptional<double> CentreHit,
		TOptional<double> SeaLevelM, double DtSec, FClampState& State)
	{
		const TOptional<double> Hit = Finite(CentreHit), Sea = Finite(SeaLevelM);
		if (!Hit.IsSet() && !Sea.IsSet()) return Sender;

		const double Target = Hit.IsSet() ? *Hit : *Sea;
		State.Height = State.bHasSurface ? Ease(State.Height, Target, DtSec) : Target;
		State.bHasSurface = true;
		CamSimFrames::FGeoPose Out = Sender;
		Out.Alt = State.Height;
		return Out;
	}
}
```

If `OffsetGeodetic`'s NEU axis order differs from (North, East, Up) (check `Geospatial/CigiFrames.h:74`), fix the offsets, not the test.

- [ ] **Step 4: Build and run** — build → `0`; `bash "$S/uetest.sh" CamSim.Entity.SurfaceClamp` → 4 passed.

- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Entity/SurfaceClamp.* unreal_project/CamSimTest/Source/CamSimTest/Tests/SurfaceClampTest.cpp
git commit -m "feat(entity): surface clamp math (ground footprint tilt, water, easing)"
```

---

### Task 5: Terrain probe and entity integration

**Files:**
- Create: `Source/CamSimTest/Entity/SurfaceProbe.h`, `Source/CamSimTest/Entity/SurfaceProbe.cpp`
- Modify: `Entity/CamSimEntity.{h,cpp}`, `Entity/CamSimEntityManager.{h,cpp}`
- Test: `Tests/SurfaceClampTest.cpp` (append a probe-driven entity-free test)

**Interfaces:**
- Consumes: Task 3 `ESurfaceMode`, `FEntityCommand::SurfaceMode`; Task 4 `CamSimSurface::*`; `FCamSimGeospatialProvider::GeoToWorld/WorldToGeo`; `CamSim::Geospatial::GetGeoidUndulation`.
- Produces:
  - `class ISurfaceProbe { public: virtual ~ISurfaceProbe() = default; virtual TOptional<double> TraceHeight(double LatDeg, double LonDeg, double TopAltM, double BottomAltM) const = 0; };`
  - `class FCesiumSurfaceProbe final : public ISurfaceProbe { public: FCesiumSurfaceProbe(UWorld* World, const FCamSimGeospatialProvider* Geo); ... };` — hits count only on `ACesium3DTileset` actors.
  - `CamSimFrames::FGeoPose CamSimSurface::PlaceOnSurface(ESurfaceMode Mode, const CamSimFrames::FGeoPose& Sender, double HalfLengthM, double HalfBeamM, double DtSec, const ISurfaceProbe& Probe, FClampState& State)` — runs the traces and calls `ClampGround`/`ClampWater`.
  - `ACamSimEntity::SetSurfaceProbe(const ISurfaceProbe* Probe)`.

- [ ] **Step 1: Write the failing test** — append to `Tests/SurfaceClampTest.cpp` (include `Entity/SurfaceProbe.h`):

```cpp
namespace
{
	/** Flat ground at a fixed height inside the trace span; records the spans it saw. */
	class FFakeProbe final : public ISurfaceProbe
	{
	public:
		double Ground = 100.0;
		mutable TArray<TPair<double, double>> Spans;
		virtual TOptional<double> TraceHeight(double, double, double Top, double Bottom) const override
		{
			Spans.Add({ Top, Bottom });
			return (Ground <= Top && Ground >= Bottom) ? TOptional<double>(Ground) : TOptional<double>();
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfacePlaceTest, "CamSim.Entity.SurfaceClamp.PlaceOnSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSurfacePlaceTest::RunTest(const FString& Parameters)
{
	FFakeProbe Probe;
	FClampState S;
	CamSimFrames::FGeoPose P = PlaceOnSurface(ESurfaceMode::Ground, Sender(30.0, 0.0), 5.0, 1.0, 0.0, Probe, S);
	TestEqual(TEXT("ground height"), P.Alt, 100.0, 1e-9);
	TestEqual(TEXT("four traces"), Probe.Spans.Num(), 4);
	TestEqual(TEXT("first span top"), Probe.Spans[0].Key, FirstTraceTopM);

	// An overpass 60 m up doesn't capture the truck: the next span tops out at +50 m.
	Probe.Spans.Reset();
	Probe.Ground = 160.0;
	P = PlaceOnSurface(ESurfaceMode::Ground, Sender(30.0, 0.0), 5.0, 1.0, 0.1, Probe, S);
	TestEqual(TEXT("overpass ignored → held"), P.Alt, 100.0, 1e-9);
	TestEqual(TEXT("later span top"), Probe.Spans[0].Key, 150.0, 1e-9);

	// None: untouched, no traces.
	Probe.Spans.Reset();
	FClampState N;
	P = PlaceOnSurface(ESurfaceMode::None, Sender(30.0, 42.0), 5.0, 1.0, 0.0, Probe, N);
	TestEqual(TEXT("none keeps alt"), P.Alt, 42.0, 1e-9);
	TestEqual(TEXT("none: no traces"), Probe.Spans.Num(), 0);

	// Water: one trace.
	Probe.Spans.Reset();
	Probe.Ground = -30.0;
	FClampState W;
	P = PlaceOnSurface(ESurfaceMode::Water, Sender(30.0, 0.0), 5.0, 1.0, 0.0, Probe, W);
	TestEqual(TEXT("water height"), P.Alt, -30.0, 1e-9);
	TestEqual(TEXT("one water trace"), Probe.Spans.Num(), 1);
	return true;
}
```

- [ ] **Step 2: Build to verify it fails** — build → non-zero (`Entity/SurfaceProbe.h` missing).

- [ ] **Step 3: Implement the probe** — `Entity/SurfaceProbe.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Entity/SurfaceClamp.h"
#include "Sim/Commands.h"

class UWorld;
class FCamSimGeospatialProvider;

/** Downward surface trace: ellipsoid height of the first surface between Top and Bottom, if any. */
class ISurfaceProbe
{
public:
	virtual ~ISurfaceProbe() = default;
	virtual TOptional<double> TraceHeight(double LatDeg, double LonDeg, double TopAltM, double BottomAltM) const = 0;
};

/** Traces the rendered Cesium tiles (needs create_physics_meshes); other actors are ignored. */
class FCesiumSurfaceProbe final : public ISurfaceProbe
{
public:
	FCesiumSurfaceProbe(UWorld* InWorld, const FCamSimGeospatialProvider* InGeo);
	virtual TOptional<double> TraceHeight(double LatDeg, double LonDeg, double TopAltM, double BottomAltM) const override;

private:
	TWeakObjectPtr<UWorld>           World;
	const FCamSimGeospatialProvider* Geo = nullptr;
};

namespace CamSimSurface
{
	/** Trace the surface for Mode and clamp the sender's pose (None returns it unchanged). */
	CamSimFrames::FGeoPose PlaceOnSurface(ESurfaceMode Mode, const CamSimFrames::FGeoPose& Sender,
		double HalfLengthM, double HalfBeamM, double DtSec, const ISurfaceProbe& Probe, FClampState& State);
}
```

`Entity/SurfaceProbe.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Entity/SurfaceProbe.h"
#include "Cesium3DTileset.h"
#include "Engine/World.h"
#include "Geospatial/CamSimGeospatialProvider.h"
#include "Geospatial/Geoid.h"

FCesiumSurfaceProbe::FCesiumSurfaceProbe(UWorld* InWorld, const FCamSimGeospatialProvider* InGeo)
	: World(InWorld), Geo(InGeo)
{
}

TOptional<double> FCesiumSurfaceProbe::TraceHeight(double LatDeg, double LonDeg, double TopAltM, double BottomAltM) const
{
	UWorld* W = World.Get();
	if (!W || !Geo) return {};

	FVector Top, Bottom;
	if (!Geo->GeoToWorld(W, LatDeg, LonDeg, TopAltM, Top) || !Geo->GeoToWorld(W, LatDeg, LonDeg, BottomAltM, Bottom))
	{
		return {};
	}

	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(CamSimSurfaceProbe), /*bTraceComplex=*/true);
	if (!W->LineTraceSingleByChannel(Hit, Top, Bottom, ECC_Visibility, Params)) return {};
	if (!Hit.GetActor() || !Hit.GetActor()->IsA<ACesium3DTileset>()) return {};

	double HitLat, HitLon, HitAlt;
	if (!Geo->WorldToGeo(W, Hit.Location, HitLat, HitLon, HitAlt) || !FMath::IsFinite(HitAlt)) return {};
	return HitAlt;
}

namespace CamSimSurface
{
	CamSimFrames::FGeoPose PlaceOnSurface(ESurfaceMode Mode, const CamSimFrames::FGeoPose& Sender,
		double HalfLengthM, double HalfBeamM, double DtSec, const ISurfaceProbe& Probe, FClampState& State)
	{
		if (Mode == ESurfaceMode::None) return Sender;

		const FTraceSpan Span = GetTraceSpan(State);
		if (Mode == ESurfaceMode::Water)
		{
			const TOptional<double> Hit = Probe.TraceHeight(Sender.Lat, Sender.Lon, Span.TopM, Span.BottomM);
			const TOptional<double> Sea = Hit.IsSet() ? TOptional<double>() : CamSim::Geospatial::GetGeoidUndulation(Sender.Lat, Sender.Lon);
			return ClampWater(Sender, Hit, Sea, DtSec, State);
		}

		const FFootprint F = GetFootprint(Sender.Lat, Sender.Lon, Sender.Neu.Rotator().Yaw, HalfLengthM, HalfBeamM);
		FGroundHits Hits;
		Hits.Bow   = Probe.TraceHeight(F.Lat[0], F.Lon[0], Span.TopM, Span.BottomM);
		Hits.Stern = Probe.TraceHeight(F.Lat[1], F.Lon[1], Span.TopM, Span.BottomM);
		Hits.Port  = Probe.TraceHeight(F.Lat[2], F.Lon[2], Span.TopM, Span.BottomM);
		Hits.Stbd  = Probe.TraceHeight(F.Lat[3], F.Lon[3], Span.TopM, Span.BottomM);
		return ClampGround(Sender, Hits, HalfLengthM, HalfBeamM, DtSec, State);
	}
}
```

If `CamSimTest.Build.cs` doesn't already list `CesiumRuntime` in the module dependencies (it should — `Camera/CamSimStreamingController.cpp` uses `ACesium3DTileset`), add it. If `ECC_Visibility` traces don't hit the tiles, use the channel the CIGI HAT handler uses (`CIGI/CigiQueryHandler.cpp:106`).

- [ ] **Step 4: Integrate into the entity**

`CamSimEntity.h` (include `Entity/SurfaceClamp.h`, forward-declare `class ISurfaceProbe;`):

```cpp
public:
	void SetSurfaceProbe(const ISurfaceProbe* InProbe) { SurfaceProbe = InProbe; }
private:
	/** Write a pose to the globe anchor, placed on the surface for SurfaceMode. */
	void CommitPose(const CamSimFrames::FGeoPose& SenderPose);
	void GetFootprintHalfSizesM(double& OutHalfLengthM, double& OutHalfBeamM) const;

	const ISurfaceProbe*        SurfaceProbe = nullptr;
	ESurfaceMode                SurfaceMode  = ESurfaceMode::None;
	CamSimSurface::FClampState  SurfaceState;
	double                      LastCommitTimeSec = -1.0;
```

`CamSimEntity.cpp` (include `Entity/SurfaceProbe.h`):
- `ApplyCommand`: before the attachment branch, `SurfaceMode = Command.SurfaceMode;`.
- `ApplyGeoPose`: keep the DR base update, then replace the two `GlobeAnchor->...` lines with `CommitPose(Pose);`.
- `UpdateDeadReckoning`: replace its two `GlobeAnchor->...` lines with `CommitPose(Pose);` (the `Pose` it just integrated).
- New:

```cpp
void ACamSimEntity::GetFootprintHalfSizesM(double& OutHalfLengthM, double& OutHalfBeamM) const
{
	OutHalfLengthM = OutHalfBeamM = 0.0;
	if (const FEntityTypeEntry* Entry = TypeTable ? TypeTable->FindEntry(EntityType) : nullptr)
	{
		OutHalfLengthM = Entry->HalfLengthCm / 100.0;
		OutHalfBeamM   = Entry->HalfBeamCm / 100.0;
	}
	if ((OutHalfLengthM <= 0.0 || OutHalfBeamM <= 0.0) && StaticMeshComp && StaticMeshComp->GetStaticMesh())
	{
		// Mesh bounds in model space, times the entry's scale (same fallback as ApplyVesselMotion).
		const FVector Ext = StaticMeshComp->GetStaticMesh()->GetBounds().BoxExtent * StaticMeshComp->GetRelativeScale3D();
		if (OutHalfLengthM <= 0.0) OutHalfLengthM = Ext.X / 100.0;
		if (OutHalfBeamM   <= 0.0) OutHalfBeamM   = Ext.Y / 100.0;
	}
}

void ACamSimEntity::CommitPose(const CamSimFrames::FGeoPose& SenderPose)
{
	CamSimFrames::FGeoPose Pose = SenderPose;
	if (SurfaceMode != ESurfaceMode::None && SurfaceProbe && !bAttached)
	{
		const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
		const double Dt  = LastCommitTimeSec < 0.0 ? 0.0 : FMath::Max(0.0, Now - LastCommitTimeSec);
		LastCommitTimeSec = Now;
		double HalfLength, HalfBeam;
		GetFootprintHalfSizesM(HalfLength, HalfBeam);
		Pose = CamSimSurface::PlaceOnSurface(SurfaceMode, SenderPose, HalfLength, HalfBeam, Dt, *SurfaceProbe, SurfaceState);
	}
	GlobeAnchor->MoveToLongitudeLatitudeHeight(FVector(Pose.Lon, Pose.Lat, Pose.Alt));
	GlobeAnchor->SetEastSouthUpRotation(CamSimFrames::NeuToEastSouthUp(Pose.Neu));
}
```

The DR base (`DR.Lat/Lon/Alt/Orientation`) stays the sender's pose: the clamp is applied on top at every commit.

`FCamSimEntityManager` (include `Entity/SurfaceProbe.h`): member `TUniquePtr<FCesiumSurfaceProbe> SurfaceProbe;`. In the spawn function after `SetEntityTypeTable`:

```cpp
	if (!SurfaceProbe && Subsystem)
	{
		SurfaceProbe = MakeUnique<FCesiumSurfaceProbe>(World, Subsystem->GetGeospatialProvider());
		if (!Subsystem->GetConfig().bCreatePhysicsMeshes)
		{
			UE_LOG(LogCamSim, Warning, TEXT("EntityManager: create_physics_meshes is off — surface traces can't hit the terrain; ground entities use the sender's height, surface entities EGM96 sea level"));
		}
	}
	Entity->SetSurfaceProbe(SurfaceProbe.Get());
```

(`World` is the `UWorld*` the spawn function already has.) Reset `SurfaceProbe` wherever the manager tears down entities for a world change, if it has such a path.

- [ ] **Step 5: Build and run** — build → `0`; `bash "$S/uetest.sh" CamSim.Entity` → all pass; `bash "$S/uetest.sh" CamSim` → 0 failed.

- [ ] **Step 6: Commit**

```bash
git add -A unreal_project/CamSimTest/Source
git commit -m "feat(entity): clamp ground and surface entities to the rendered terrain / water at every pose commit"
```

---

### Task 6: DIS type map — `kind:domain` fallback

**Files:**
- Modify: `Source/CamSimTest/DIS/DisEntityAdapter.{h,cpp}`
- Test: `Tests/DisProtocolTest.cpp` (append)

**Interfaces:**
- Produces: `FDisEntityAdapter::MapEntityType` order exact → `kind:domain:category` → `kind:domain` → default; private `TMap<FString, uint16> DomainTypeMap;`.

- [ ] **Step 1: Write the failing test**

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDisTypeMapFallbackTest,
	"CamSim.Dis.TypeMapFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDisTypeMapFallbackTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Config;
	Config.DIS.DefaultEntityTypeId = 1001;
	Config.DIS.EntityTypeMappings.Add(TEXT("1:1:225:7:0:0:0"), 2001);   // truck
	Config.DIS.EntityTypeMappings.Add(TEXT("1:3:225:7:0:0:0"), 3001);   // boat
	Config.DIS.EntityTypeMappings.Add(TEXT("1:1:225:1:1:0:0"), 2500);   // a specific tank
	FDisEntityAdapter Adapter(Config, nullptr);

	auto Type = [](uint8 Kind, uint8 Domain, uint16 Country, uint8 Cat, uint8 Sub)
	{
		FDisEntityType T; T.EntityKind = Kind; T.Domain = Domain; T.Country = Country; T.Category = Cat; T.Subcategory = Sub; return T;
	};
	TestEqual(TEXT("exact"), (int32)Adapter.MapEntityType(Type(1, 1, 225, 7, 0)), 2001);
	TestEqual(TEXT("category fuzzy beats domain"), (int32)Adapter.MapEntityType(Type(1, 1, 222, 1, 9)), 2500);
	TestEqual(TEXT("any other land platform → truck"), (int32)Adapter.MapEntityType(Type(1, 1, 222, 3, 0)), 2001);
	TestEqual(TEXT("any other surface platform → boat"), (int32)Adapter.MapEntityType(Type(1, 3, 13, 61, 2)), 3001);
	TestEqual(TEXT("air platform unmapped → default"), (int32)Adapter.MapEntityType(Type(1, 2, 225, 1, 0)), 1001);
	return true;
}
```

Note the "any other land platform" expectation: both 2001 and 2500 are land mappings, so the `kind:domain` key `1:1` needs a deterministic winner — the most generic mapping (country 0 or subcategory 0 counts as generic, same rule as the category level: prefer entries whose subcategory is 0; among equals prefer the lower CamSim type ID). With this rule `1:1` → 2001 (subcategory 0) over 2500 (subcategory 1).

- [ ] **Step 2: Build and run to verify it fails** — build → `0`; `bash "$S/uetest.sh" CamSim.Dis.TypeMapFallback` → FAIL on the "any other land/surface platform" assertions (1001 returned).

- [ ] **Step 3: Implement** — in `BuildTypeMaps`, next to the category fuzzy key:

```cpp
		if (Parts.Num() >= 2)
		{
			// Domain fallback (kind:domain): any other platform of the domain. Prefer
			// a subcategory-0 mapping, then the lower type ID, so the pick is deterministic.
			const FString DomainKey = FString::Printf(TEXT("%s:%s"), *Parts[0], *Parts[1]);
			const bool bGenericSub = Parts.Num() < 5 || Parts[4] == TEXT("0");
			if (const uint16* Existing = DomainTypeMap.Find(DomainKey))
			{
				const bool bExistingGeneric = DomainGeneric.Contains(DomainKey);
				if ((bGenericSub && !bExistingGeneric) || (bGenericSub == bExistingGeneric && Mapping.Value < *Existing))
				{
					DomainTypeMap.Add(DomainKey, Mapping.Value);
					if (bGenericSub) DomainGeneric.Add(DomainKey);
				}
			}
			else
			{
				DomainTypeMap.Add(DomainKey, Mapping.Value);
				if (bGenericSub) DomainGeneric.Add(DomainKey);
			}
		}
```

with private members `TMap<FString, uint16> DomainTypeMap;` and `TSet<FString> DomainGeneric;`. In `MapEntityType`, after the category fuzzy lookup:

```cpp
	// 3. Domain fallback (kind:domain)
	const FString DomainKey = FString::Printf(TEXT("%u:%u"), DisType.EntityKind, DisType.Domain);
	if (const uint16* Found = DomainTypeMap.Find(DomainKey))
	{
		return *Found;
	}
```

and renumber the default step's comment. Add the domain map's size to the "type maps loaded" log line. Update the header comment of `MapEntityType` to list the four steps.

Check the existing `CamSim.Phase21.EntityTypeMapping` test still passes: its "unknown" type is 9:9:9, so the new level doesn't change it.

- [ ] **Step 4: Run** — `bash "$S/uetest.sh" CamSim.Dis` and `bash "$S/uetest.sh" CamSim.Phase21` → all pass.

- [ ] **Step 5: Document and commit** — in `docs/configuration.md` where `dis.entity_type_map` is described, state the lookup order (exact → kind:domain:category → kind:domain → `default_entity_type_id`).

```bash
git add -A unreal_project/CamSimTest/Source docs/configuration.md
git commit -m "feat(dis): kind:domain fallback in the entity type map"
```

---

### Task 7: Model preload

**Files:**
- Create: `Source/CamSimTest/Entity/EntityMeshLoader.h`, `Source/CamSimTest/Entity/EntityMeshLoader.cpp`
- Modify: `Entity/CamSimEntity.cpp` (use the loader), `Entity/EntityTypeTable.{h,cpp}`, `Subsystem/CamSimSubsystem.cpp`
- Test: `Source/CamSimTest/Tests/EntityPreloadGpuTest.cpp` (GPU runner: glTFRuntime mesh builds need a real RHI)

**Interfaces:**
- Produces: namespace `CamSimMeshLoader { bool IsGltfPath(const FString&); UStaticMesh* LoadStaticMesh(const FString& Path); USkeletalMesh* LoadSkeletalMesh(const FString& Path); }` (moved from `CamSimEntity.cpp`'s anonymous namespace; same behaviour); `void FEntityTypeTable::LoadFromYamlString(const FString& Yaml)`; `int32 FEntityTypeTable::PreloadGltfMeshes()` (returns the number of glTF entries resident afterwards; holds strong references until the next preload or destruction).

- [ ] **Step 1: Write the failing test** — `Tests/EntityPreloadGpuTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Entity/EntityTypeTable.h"
#include "Engine/StaticMesh.h"
#include "UObject/UObjectGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityPreloadGpuTest, "CamSim.GPU.Entity.PreloadTwiceKeepsMeshes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEntityPreloadGpuTest::RunTest(const FString& Parameters)
{
	FEntityTypeTable Table;
	Table.LoadFromYamlString(TEXT(
		"entity_types:\n"
		"  \"1001\":\n"
		"    mesh: f16/f16-c_falcon.glb\n"
		"  \"1002\":\n"
		"    mesh: missing/not_there.glb\n"));
	TestNotNull(TEXT("valid entry kept"), Table.FindEntry(1001));
	TestNull(TEXT("missing file skipped"), Table.FindEntry(1002));

	TestEqual(TEXT("one glTF preloaded"), Table.PreloadGltfMeshes(), 1);
	CollectGarbage(RF_NoFlags, true);
	UStaticMesh* First = Table.GetCachedStaticMesh(1001);
	TestNotNull(TEXT("survives GC"), First);

	// Hot reload path: preloading again keeps a resident mesh (no reload, no drop).
	TestEqual(TEXT("preload again"), Table.PreloadGltfMeshes(), 1);
	CollectGarbage(RF_NoFlags, true);
	TestTrue(TEXT("same mesh after second preload"), Table.GetCachedStaticMesh(1001) == First);
	return true;
}
```

This needs the F-16 `.glb` to be a real file, not an LFS pointer (`git lfs pull` if `head -c 100 entities/f16/f16-c_falcon.glb` shows `version https://git-lfs`).

- [ ] **Step 2: Build to verify it fails** — build → non-zero (`LoadFromYamlString`/`PreloadGltfMeshes` missing).

- [ ] **Step 3: Implement**
- Move `IsGltfPath`, `ResolveGltfPath`, `LoadStaticMeshFromPath`, `LoadSkeletalMeshFromPath` from `CamSimEntity.cpp`'s anonymous namespace into `Entity/EntityMeshLoader.{h,cpp}` as `CamSimMeshLoader::IsGltfPath`, `LoadStaticMesh`, `LoadSkeletalMesh` (the glTFRuntime includes move with them). `CamSimEntity.cpp` calls the new names.
- `FEntityTypeTable::LoadFromConfig()` reads the file and calls `LoadFromYamlString(YamlContent)`; everything after the file read moves into `LoadFromYamlString`.
- `EntityTypeTable.h`: include `UObject/StrongObjectPtr.h`; add

```cpp
	/** Parse an entity_types YAML document (LoadFromConfig reads the config file into this). */
	void LoadFromYamlString(const FString& YamlContent);

	/**
	 * Load every glTF entry now (game thread, at startup / hot reload) and keep it
	 * resident for the session, so the first spawn of a type doesn't hitch and the
	 * mesh isn't garbage-collected when its last entity goes. Returns the number of
	 * glTF entries resident.
	 */
	int32 PreloadGltfMeshes();

private:
	TMap<uint16, TStrongObjectPtr<UObject>> PreloadedMeshes;
```

- `PreloadGltfMeshes()`:

```cpp
int32 FEntityTypeTable::PreloadGltfMeshes()
{
	TMap<uint16, TStrongObjectPtr<UObject>> Kept;
	for (const TPair<uint16, FEntityTypeEntry>& Pair : TypeMap)
	{
		const FEntityTypeEntry& E = Pair.Value;
		if (E.bAnimated || !CamSimMeshLoader::IsGltfPath(E.AssetPath)) continue;

		UObject* Mesh = E.bSkeletal ? static_cast<UObject*>(GetCachedSkeletalMesh(Pair.Key))
		                            : static_cast<UObject*>(GetCachedStaticMesh(Pair.Key));
		if (!Mesh)
		{
			const double T0 = FPlatformTime::Seconds();
			if (E.bSkeletal)
			{
				USkeletalMesh* S = CamSimMeshLoader::LoadSkeletalMesh(E.AssetPath);
				if (S) SetCachedSkeletalMesh(Pair.Key, S);
				Mesh = S;
			}
			else
			{
				UStaticMesh* S = CamSimMeshLoader::LoadStaticMesh(E.AssetPath);
				if (S) SetCachedStaticMesh(Pair.Key, S);
				Mesh = S;
			}
			UE_LOG(LogCamSim, Log, TEXT("EntityTypeTable: preloaded type %u '%s' in %.0f ms%s"),
				Pair.Key, *E.AssetPath, (FPlatformTime::Seconds() - T0) * 1000.0, Mesh ? TEXT("") : TEXT(" — FAILED"));
		}
		if (Mesh) Kept.Add(Pair.Key, TStrongObjectPtr<UObject>(Mesh));
	}
	PreloadedMeshes = MoveTemp(Kept);
	return PreloadedMeshes.Num();
}
```

(Use the table's real member name for the type map; `TypeMap` is what `LoadFromConfig` uses.) `HotReload()` keeps preserving cached meshes for unchanged paths as today; the subsystem calls `PreloadGltfMeshes()` after it.
- `Subsystem/CamSimSubsystem.cpp`: after `EntityTypeTable.LoadFromConfig();` (≈ line 399) and after `EntityTypeTable.HotReload();` (≈ line 268) call `EntityTypeTable.PreloadGltfMeshes();`.

- [ ] **Step 4: Build and run** — build → `0`; `scripts/run_gpu_tests.sh CamSim.GPU.Entity` → `succeeded 1 failed 0`; `bash "$S/uetest.sh" CamSim` → 0 failed (NullRHI must not regress; the preload runs in subsystem init only where a world exists — if NullRHI runs log glTF failures, that's acceptable, but they must not fail tests).

- [ ] **Step 5: Commit**

```bash
git add -A unreal_project/CamSimTest/Source
git commit -m "feat(entity): preload glTF entity models at startup and keep them resident"
```

---

### Task 8: Truck and boat models

**Files:**
- Create: `entities/truck/<model>.glb`, `entities/truck/LICENSE.md`, `entities/boat/<model>.glb`, `entities/boat/LICENSE.md`
- Modify: `deploy/camsim_config.yaml` (`entity_types`, `dis.entity_type_map`), `docs/configuration.md` (if it lists shipped entity types)
- Test: `Source/CamSimTest/Tests/EntityPreloadGpuTest.cpp` (append `CamSim.GPU.Entity.ModelFacing`)

**Interfaces:**
- Consumes: Task 7 `FEntityTypeTable::LoadFromYamlString`, `PreloadGltfMeshes`, `GetCachedStaticMesh`.
- Produces: config types `2001` (truck) and `3001` (boat) with `class_name`, `scale`, `rotation`, `half_length_m`, `half_beam_m`.

- [ ] **Step 1: Find the models.** Search for a realistic truck (cargo / utility / pickup) and a realistic small boat (patrol boat, RHIB or fishing boat): CC0 or CC-BY only; `.glb` or convertible glTF; ≤ ~100 k triangles; ≤ ~20 MB; downloadable without a login. Candidates to check: Poly Haven models, Kenney (stylised — last resort), Khronos glTF-Sample-Assets (`CesiumMilkTruck`, CC-BY 4.0, cartoonish — fallback for the truck), Smithsonian 3D (CC0), NASA 3D resources, Sketchfab (downloads need a login). If no acceptable model can be downloaded without a login, stop and report NEEDS_CONTEXT with a shortlist of up to three Sketchfab (or other) links per vehicle, with licence, triangle count and a one-line look description, so the controller can ask the user to download one.

- [ ] **Step 2: Measure each model.** Convert to `.glb` if needed (`scripts/gltf_to_glb.py`). Measure bounds and triangles:

```bash
uv run -q --with trimesh python -c "
import sys, trimesh
m = trimesh.load(sys.argv[1], force='scene')
print('extents (glTF x,y,z m):', m.extents, 'triangles:', sum(len(g.faces) for g in m.geometry.values()))
" entities/truck/<model>.glb
```

glTF is Y-up with +Z forward by convention (models vary). Record length (along the model's forward axis) and beam; set `half_length_m`/`half_beam_m` to half of those in metres (after `scale`). Choose `scale` so the truck is 6–10 m long and the boat 7–12 m.

- [ ] **Step 3: Write `LICENSE.md` for each** — source URL, author, licence name + URL, and the attribution text CC-BY requires, e.g.:

```markdown
# <Model name>

- Source: <URL>
- Author: <name>
- Licence: CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/)
- Changes: converted to .glb; no geometry changes.

Attribution: "<Model name>" by <author>, licensed under CC BY 4.0.
```

- [ ] **Step 4: Add the config** — `deploy/camsim_config.yaml`, under `entity_types:` after `"1001"`:

```yaml
  # DIS land platforms (dis.entity_type_map below). Footprint feeds the terrain clamp.
  "2001":
    mesh: truck/<model>.glb
    skeletal: false
    class_name: truck
    scale: <measured>
    rotation: { pitch: 0.0, yaw: <measured>, roll: 0.0 }
    half_length_m: <measured>
    half_beam_m: <measured>
  # DIS surface platforms.
  "3001":
    mesh: boat/<model>.glb
    skeletal: false
    class_name: boat
    scale: <measured>
    rotation: { pitch: 0.0, yaw: <measured>, roll: 0.0 }
    half_length_m: <measured>
    half_beam_m: <measured>
```

(Replace every `<measured>` with the numbers from Step 2 and Step 6; use the block-style `rotation:` map if the YAML loader doesn't accept flow maps — check `1001`'s style.) Under `dis.entity_type_map`:

```yaml
    "1:1:225:7:0:0:0": 2001   # truck (scripts/send_dis_test.py); kind:domain fallback: any land platform
    "1:3:225:7:0:0:0": 3001   # boat; kind:domain fallback: any surface platform
```

- [ ] **Step 5: Write the facing/scale test** — append to `Tests/EntityPreloadGpuTest.cpp` (include `Config/CamSimConfig.h`, `Misc/FileHelper.h`):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityModelFacingGpuTest, "CamSim.GPU.Entity.ModelFacing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEntityModelFacingGpuTest::RunTest(const FString& Parameters)
{
	// The shipped config: each vehicle, after its rotation and scale, is longest along
	// UE +X (the entity's forward) and matches its configured footprint within 10%.
	FString Yaml;
	const FString Path = FPaths::Combine(FPaths::ProjectDir(), TEXT("../../deploy/camsim_config.yaml"));
	if (!TestTrue(TEXT("config read"), FFileHelper::LoadFileToString(Yaml, *Path))) return false;
	FEntityTypeTable Table;
	Table.LoadFromYamlString(Yaml);
	Table.PreloadGltfMeshes();

	for (const uint16 Type : { (uint16)2001, (uint16)3001 })
	{
		const FEntityTypeEntry* E = Table.FindEntry(Type);
		if (!TestNotNull(*FString::Printf(TEXT("type %u configured"), Type), E)) continue;
		UStaticMesh* Mesh = Table.GetCachedStaticMesh(Type);
		if (!TestNotNull(*FString::Printf(TEXT("type %u loaded"), Type), Mesh)) continue;

		const FBox Local = Mesh->GetBoundingBox();
		const FTransform Xf(E->ModelRotation, FVector::ZeroVector, FVector(E->ModelScale));
		const FVector Ext = Local.TransformBy(Xf).GetExtent();  // cm, entity space
		TestTrue(*FString::Printf(TEXT("type %u longest along +X"), Type), Ext.X > Ext.Y);
		TestEqual(*FString::Printf(TEXT("type %u half length"), Type), Ext.X, (double)E->HalfLengthCm, 0.1 * E->HalfLengthCm);
		TestEqual(*FString::Printf(TEXT("type %u half beam"), Type), Ext.Y, (double)E->HalfBeamCm, 0.1 * E->HalfBeamCm);
	}
	return true;
}
```

- [ ] **Step 6: Run and tune** — build → `0`; `scripts/run_gpu_tests.sh CamSim.GPU.Entity` → `succeeded 2 failed 0`. If "longest along +X" fails, set `rotation.yaw` (usually ±90) and re-run; if the sizes fail, re-measure. The test can't tell forward from backward: Task 9's shots check that.

- [ ] **Step 7: Commit (LFS)**

```bash
git lfs track "*.glb"   # already in .gitattributes; no-op
git add entities/truck entities/boat deploy/camsim_config.yaml unreal_project/CamSimTest/Source docs/configuration.md
git lfs ls-files | grep -E "truck|boat"   # both .glb must be listed
git commit -m "feat(entities): truck and boat models with DIS type mappings"
```

---

### Task 9: End-to-end check and documentation

**Files:**
- Create: `scripts/dis_vehicle_check.py`, `docs/dis.md`
- Modify: `ROADMAP.md`, `CLAUDE.md`, `docs/configuration.md` (links), `README.md` (docs index, if it lists docs)

**Interfaces:**
- Consumes: `scripts/send_dis_test.py` (Task 1), `scripts/bench/run_bench.py` (`Host`, `wait_ready`, `wait_terrain`, `wait_port_free`, `fetch_snapshot`), `scripts/bench/scenario.py` (`Pose`).

- [ ] **Step 1: Write `scripts/dis_vehicle_check.py`** — launches CamSim headless with DIS and ground truth on, runs the sender, frames each vehicle, captures shots and checks the COCO output:

```python
#!/usr/bin/env python3
"""End-to-end check: DIS truck + boat → video + COCO ground truth (macOS, local build).

Usage: scripts/dis_vehicle_check.py OUTDIR
Writes OUTDIR/shots/*.png, OUTDIR/ml/ (COCO), OUTDIR/frames.jsonl and prints a summary.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from dataclasses import replace
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts"))
sys.path.insert(0, str(REPO / "scripts" / "bench"))
from bench import run_bench as rb  # noqa: E402
from bench import scenario  # noqa: E402
import send_dis_test as sd  # noqa: E402

TRUCK = sd.PRESETS["truck-loop"].center
BOAT = sd.PRESETS["boat-circle"].center


def main() -> int:
    out = Path(sys.argv[1]).resolve()
    (out / "shots").mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, CAMSIM_DIS_ENABLED="1", CAMSIM_ML_ENABLED="1", CAMSIM_ML_OUTPUT_DIR=str(out / "ml"),
               CAMSIM_ML_DEPTH_ENABLED="0", CAMSIM_ML_INTERVAL_FRAMES="3",
               CAMSIM_SNAPSHOT_ENDPOINT_ENABLED="1", CAMSIM_MULTICAST_ADDR="127.0.0.1",
               CAMSIM_FRAME_STATS_PATH=str(out / "frames.jsonl"))
    rb.wait_port_free(8080)
    host = rb.Host()
    # Nadir over the truck loop, 30 deg FOV: ~430 m of ground across at 800 m above it.
    host.pose = scenario.Pose(TRUCK[0], TRUCK[1], 900.0, gimbal_pitch=-90.0, fov_h=30.0)
    host.thread.start()
    subprocess.run([str(REPO / "scripts" / "run.sh"), "--headless", "--local", "--detach"],
                   env=env, check=True, stdout=subprocess.DEVNULL)
    sender = None
    try:
        rb.wait_ready(REPO / ".cache" / "camsim.pid")
        rb.wait_terrain(120)
        sender = subprocess.Popen([sys.executable, str(REPO / "scripts" / "send_dis_test.py"), "both"],
                                  stdout=subprocess.DEVNULL)
        time.sleep(15.0)
        for i in range(3):
            rb.fetch_snapshot(out / "shots" / f"truck_nadir_{i}.png"); time.sleep(4.0)
        # Oblique from the south-west to see the truck's tilt on the slopes.
        host.pose = scenario.Pose(TRUCK[0] - 0.004, TRUCK[1] - 0.004, 400.0, yaw=45.0, gimbal_pitch=-25.0, fov_h=20.0)
        time.sleep(6.0)
        rb.fetch_snapshot(out / "shots" / "truck_oblique.png")
        host.pose = scenario.Pose(BOAT[0], BOAT[1], 800.0, gimbal_pitch=-90.0, fov_h=30.0)
        time.sleep(8.0)
        for i in range(3):
            rb.fetch_snapshot(out / "shots" / f"boat_nadir_{i}.png"); time.sleep(4.0)
        host.pose = scenario.Pose(BOAT[0] - 0.004, BOAT[1], 150.0, yaw=0.0, gimbal_pitch=-10.0, fov_h=20.0)
        time.sleep(6.0)
        rb.fetch_snapshot(out / "shots" / "boat_oblique.png")
    finally:
        if sender:
            sender.terminate()
        host.stop.set()
        subprocess.run([str(REPO / "scripts" / "stop.sh")], check=False, stdout=subprocess.DEVNULL)

    return summarize(out)


def summarize(out: Path) -> int:
    ids: dict[str, set[int]] = {}
    frames = 0
    for f in (out / "ml").rglob("*.jsonl"):
        for line in f.read_text().splitlines():
            rec = json.loads(line)
            frames += 1
            for a in rec.get("annotations", []):
                ids.setdefault(a["category"]["name"], set()).add(a["entity_id"])
    rows = [json.loads(line) for line in (out / "frames.jsonl").read_text().splitlines() if line.startswith("{")]
    walls = sorted(r["wall_ms"] for r in rows) or [0.0]
    hitches = sum(1 for r in rows if r["wall_ms"] > 66)
    print(f"COCO frames {frames}; ids per class {ids}")
    print(f"wall p95 {walls[int(0.95 * (len(walls) - 1))]:.1f} ms; frames > 66 ms: {hitches}")
    ok = ids.get("truck") and ids.get("boat") and all(len(v) == 1 for v in ids.values() if v)
    print("PASS (labels): one stable id each for truck and boat" if ok else "FAIL (labels)")
    print(f"Review the shots in {out / 'shots'}: on the ground / water, facing the travel direction.")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
```

Adjust to the real `run_bench` / `scenario` names if any differ (`grep -n "def \|class " scripts/bench/run_bench.py`), and the frame-stats field names (`wall_ms`) to what `frames.jsonl` contains.

- [ ] **Step 2: Run it** — `scripts/run.sh --build-only` (→ 0), then `uv run -q --with numpy --with pillow python scripts/dis_vehicle_check.py "$S/dis_check"`. Expected: `PASS (labels)`; frames > 66 ms small (compare with the SF bench's orbit phase: a few). Look at every shot (Read the PNGs): both vehicles visible, facing along their path (the truck's loop is driven clockwise viewed from above: north along the east side), the truck on the ground with no float/sink and tilted on slopes, the boat on the water. If a model faces backwards, fix `rotation.yaw` by 180 in `deploy/camsim_config.yaml` and re-run. Copy the shots to `docs/images/dis-vehicles/` (only the four best, ≤ 500 KB each) for the ROADMAP entry.

- [ ] **Step 3: Write `docs/dis.md`** — sections: *Enabling DIS* (config keys, env vars, port/exercise), *Test sender* (`scripts/send_dis_test.py` presets, flags, examples), *Mapping DIS types to models* (the four-step lookup, `entity_types` fields including `class_name` and the footprint), *Surface placement* (`clamp_to_surface`, ground/water behaviour, needs `create_physics_meshes`, EGM96 fallback), *Ground truth* (COCO fields `entity_id`/`source`/`source_id`), *Limitations* (no waves/wakes, loose world-aligned boxes and no occlusion, DR ignores acceleration and PDU timestamps, no DIS articulated parts).

- [ ] **Step 4: Update ROADMAP.md** — add an item for this work (done, dated 2026-09-28, link the spec, plan and `docs/dis.md`, the acceptance numbers and shots). Update the DIS items it supersedes (2.3's DR note: algorithms 2–9 are handled, acceleration is not). Carry-overs: tight oriented boxes + occlusion (sub-project 2); ocean surface + wakes (needs editor assets `M_Ocean`, `NS_VesselWake`); DR acceleration and PDU timestamps; DIS articulated parts; inland water where Cesium terrain has no flat surface.

- [ ] **Step 5: Update CLAUDE.md** — command table rows `| \`scripts/send_dis_test.py\` | Send DIS Entity State PDUs: scripted truck + boat (\`both\`, \`truck-loop\`, \`boat-circle\`) |` and `| \`scripts/dis_vehicle_check.py\` | End-to-end DIS vehicle check (shots + COCO labels) |`; a Gotcha: "**DIS vehicles sit on the rendered surface**: land (domain 1) and surface (domain 3) entities are clamped at every pose commit by traces against Cesium tiles (`Entity/SurfaceClamp.h`, `SurfaceProbe.h`); needs `create_physics_meshes`. The sender's altitude is ignored unless `dis.clamp_to_surface: false`."

- [ ] **Step 6: Full verification** — build → `0`; `bash "$S/uetest.sh" CamSim` → 0 failed; `scripts/run_gpu_tests.sh` → 0 failed; `uv run -q --with pytest --with numpy --with pillow pytest -q scripts/tests` → all pass; `uvx ruff check scripts` clean for the new files.

- [ ] **Step 7: Commit**

```bash
git add scripts/dis_vehicle_check.py docs/dis.md docs/images/dis-vehicles ROADMAP.md CLAUDE.md docs/configuration.md README.md
git commit -m "docs(dis): DIS vehicles guide, end-to-end check, ROADMAP entry"
```
