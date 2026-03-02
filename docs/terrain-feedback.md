# Terrain Feedback: HAT/HOT & LOS (Phase 10)

CamSim implements the CIGI IG→Host response channel, allowing the host
simulation to query terrain height and line-of-sight conditions. Every game
tick the IG also sends a mandatory Start-of-Frame (SOF) heartbeat to the host.

## Contents

- [Overview](#overview)
- [CIGI packets handled](#cigi-packets-handled)
- [SOF heartbeat](#sof-heartbeat)
- [HAT/HOT queries (opcode 24)](#hathot-queries-opcode-24)
- [LOS segment queries (opcode 25)](#los-segment-queries-opcode-25)
- [LOS vector queries (opcode 26)](#los-vector-queries-opcode-26)
- [Entity ID in LOS responses](#entity-id-in-los-responses)
- [Configuration](#configuration)
- [Architecture](#architecture)
- [Verification](#verification)

---

## Overview

```
Host → IG  (opcode 24/25/26 requests)
           ↓ FCigiReceiver → SPSC queue
           ↓ FCigiQueryHandler::Tick()
           ↓ World->LineTraceSingleByChannel()
           ↓ FCigiSender::Enqueue*Response()
IG → Host  (opcode 101 SOF + 102 HAT/HOT resp + 104 LOS resp)
           one UDP datagram per frame
```

Requests arrive on the receiver thread and are placed into SPSC queues. Each
game tick, `FCigiQueryHandler` drains those queues, executes UE5 line traces,
and stages responses. `FCigiSender::FlushFrame()` then packs the SOF plus all
staged responses into a single UDP datagram and sends it to the host.

---

## CIGI Packets Handled

### Received (Host → IG)

| Opcode | Packet | CCL class |
|--------|--------|-----------|
| 24 | HAT/HOT Request | `CigiHatHotReqV3` |
| 25 | LOS Segment Request | `CigiLosSegReqV3` |
| 26 | LOS Vector Request | `CigiLosVectReqV3` |

### Sent (IG → Host)

| Opcode | Packet | CCL class |
|--------|--------|-----------|
| 101 | Start of Frame | `CigiSOFV3` |
| 102 | HAT/HOT Response | `CigiHatHotRespV3` |
| 104 | LOS Response | `CigiLosRespV3` |

All outgoing packets are packed into one datagram per frame by `FCigiSender`.

---

## SOF Heartbeat

A conformant CIGI IG must send a Start-of-Frame packet to the host every frame.
`FCigiSender::FlushFrame()` is called once per game tick (from
`UCamSimSubsystem::Tick()`, which is driven by `FCamSimEntityManager::Tick()`).

The SOF packet carries the IG frame counter, which is incremented each tick.
The host uses this to detect IG frame drops.

---

## HAT/HOT Queries (opcode 24)

A HAT/HOT Request specifies a geodetic point (lat, lon, alt). CamSim:

1. Converts (lat, lon, 50 000 m) → UE world position (`TopPt`).
2. Converts (lat, lon, −500 m) → UE world position (`BotPt`).
3. Runs `LineTraceSingleByChannel(TopPt, BotPt, ECC_Visibility)`.
4. Converts the hit world position back to geodetic altitude via
   `ACesiumGeoreference::TransformUnrealPositionToLongitudeLatitudeHeight`.
5. Returns:
   - **HOT** = terrain altitude above WGS-84 ellipsoid at the hit point (metres)
   - **HAT** = `Req.Alt − HOT` (height above terrain at the query altitude)

If the trace finds no hit, `Valid = false` is returned.

### ReqType

| Value | Meaning | CamSim behaviour |
|-------|---------|-----------------|
| 0 | HAT query | Returns both HAT and HOT; response `ReqType = 0` |
| 1 | HOT query | Returns both HAT and HOT; response `ReqType = 1` |
| 2 | Extended | Treated as HOT; extended response not implemented |

---

## LOS Segment Queries (opcode 25)

A LOS Segment Request specifies source and destination geodetic points. CamSim:

1. Converts both endpoints to UE world positions.
2. Runs `LineTraceSingleByChannel(SrcWorld, DstWorld, ECC_Visibility)`.
3. Returns:
   - `Valid = true`, `Visible = false` — an obstruction was found (hit)
   - `Valid = false`, `Visible = true` — no obstruction (line of sight clear)
   - `Range` — distance from source to hit point in metres
   - `HitLat/Lon/Alt` — geodetic coordinates of the hit point
   - `EntityId` — CIGI entity ID if the hit actor is a `ACamSimEntity`

> **Note on DestEntityId:** The CIGI 3.3 (v3) LOS Segment packet does not
> include a DestEntityId field — that field was added in CIGI 3.2. CamSim
> always performs a full scene trace regardless.

---

## LOS Vector Queries (opcode 26)

A LOS Vector Request specifies a source geodetic point, an azimuth (true north,
degrees), an elevation angle (degrees, positive up), and a maximum range
(metres). CamSim:

1. Computes the end point using a flat-earth approximation:
   ```
   HorizM  = MaxRange × cos(ElRad)
   UpM     = MaxRange × sin(ElRad)
   NorthM  = HorizM  × cos(AzRad)
   EastM   = HorizM  × sin(AzRad)

   DeltaLat = NorthM / 111320
   DeltaLon = EastM  / (111320 × cos(SrcLat))

   EndLat = SrcLat + DeltaLat
   EndLon = SrcLon + DeltaLon
   EndAlt = SrcAlt + UpM
   ```
2. If `MinRange > 0`, the trace starts at the interpolated point along the
   vector (skipping the near-field exclusion zone).
3. Runs `LineTraceSingleByChannel(TraceStart, EndWorld, ECC_Visibility)`.
4. Returns the same fields as a LOS Segment response. `Range` is always
   measured from the original source point, not the trace start.

The flat-earth approximation is consistent with `ComputeGeometricLOS()` used
in the camera's KLV metadata computation. Error is negligible at typical sensor
slant ranges (< 50 km).

---

## Entity ID in LOS Responses

If the ray hits an actor that is an `ACamSimEntity`, the response includes the
entity's CIGI Entity ID (`EntityIDValid = true`, `EntityID = N`). If the hit
actor is terrain or any other non-entity actor, `EntityIDValid = false`.

---

## Configuration

| JSON field | Default | Env var | Description |
|------------|---------|---------|-------------|
| `cigi_response_addr` | `"127.0.0.1"` | `CAMSIM_CIGI_RESPONSE_ADDR` | Destination IP for IG→Host packets. Set to the host simulation's IP address. |
| `cigi_response_port` | `8889` | `CAMSIM_CIGI_RESPONSE_PORT` | Destination UDP port for IG→Host packets. |

See [configuration.md](configuration.md) for the full field reference.

---

## Architecture

### Class responsibilities

| Class | Responsibility |
|-------|---------------|
| `FCigiReceiver` | Receives opcodes 24/25/26 on the receiver thread; enqueues `FCigiHatHotRequest`, `FCigiLosSegRequest`, `FCigiLosVectRequest` into SPSC queues |
| `FCigiQueryHandler` | Game-thread object; drains the three queues each tick; runs UE line traces; calls `FCigiSender::Enqueue*Response()` |
| `FCigiSender` | Packs SOF + staged responses into one UDP datagram per tick via `FlushFrame()` |
| `UCamSimSubsystem` | Owns `FCigiSender` and `FCigiQueryHandler`; provides `Tick()` called from `FCamSimEntityManager::Tick()` |

### Ticking

`UCamSimSubsystem` does not implement `FTickableGameObject` directly. Instead,
`FCamSimEntityManager::Tick()` calls `UCamSimSubsystem::Tick()` at the end of
its own tick, after all entity state has been processed. This guarantees that
responses are sent within the same game tick as the entity positions are
updated.

### CCL session

`FCigiSender` uses a separate `CigiIGSession(0, 0, 2, 4096)` — zero incoming
buffers, two outgoing buffers of 4096 bytes. This keeps the sender fully
independent from the receiver's CCL session.

---

## Verification

### SOF heartbeat

```bash
# Run CamSim, then capture on the response port
sudo tcpdump -i lo0 udp port 8889 -c 300
```

Expect ~30 packets/second. Each packet's first two bytes are the CIGI SOF
packet ID (`0x65` = 101) and packet size.

### HAT/HOT

Extend `scripts/send_cigi_test.py` to send an opcode 24 request at a known
lat/lon (e.g. a mountain peak visible in the Cesium scene). Capture the
response and check that `HOT` matches the terrain elevation at that point.

### LOS segment

Send a segment from high altitude straight down toward terrain. Expect:
- `Visible = false` (terrain blocks it)
- `Range` ≈ `SrcAlt − HOT` at that point

Send a segment between two points above the ocean with no intervening terrain.
Expect:
- `Visible = true`

### LOS vector

Send a vector from the camera position along the current gimbal boresight.
`Range` in the response should closely match `SlantRangeM` reported in the KLV
stream (within flat-earth approximation error).
