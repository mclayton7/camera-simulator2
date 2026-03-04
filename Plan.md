# CamSim Roadmap

## Implemented (Phases 1–11)

All core features are complete. The pipeline is production-ready for EO/IR/NVG
sensor simulation with full CIGI 3.3 support.

| Phase | Feature | Status |
|-------|---------|--------|
| 1–6  | Core pipeline: CIGI input, Cesium terrain, H.264/KLV MPEG-TS output | ✅ Done |
| 7    | Environment: day/night cycle, sky atmosphere, fog, cloud/weather layers | ✅ Done |
| 8    | Entity rendering: aircraft/vehicles, dead-reckoning, articulated parts, lights | ✅ Done |
| 9    | Gimbal & sensor: 3-DOF gimbal, slew limits, FOV presets, polarity | ✅ Done |
| 10   | Terrain feedback: HAT/HOT and LOS line traces, SOF heartbeat, IG→Host UDP | ✅ Done |
| 11   | Sensor simulation: EO/IR/NVG post-process, noise, vignetting, atmospheric extinction | ✅ Done |

---

## Remaining Work

## Phase 12 — Standards Compliance & Hardening

### 12A: MISB ST 0102 (Security Metadata)
- Add classification local set to every KLV packet (even "UNCLASSIFIED")
- Tags: classification level, classifying country, object country, caveats
- Configurable via `camsim_config.json`

### 12B: STANAG 4609 Compliance
- Validate MPEG-TS PID allocation per STANAG 4609 Ed3
- Ensure KLV metadata rate ≥ 1 Hz (currently 30 Hz — compliant)
- Verify PTS synchronization between video and KLV within one frame
- Add H.265/HEVC encoder option for STANAG 4609 Ed4

### 12C: Multi-Channel Output
- Support multiple simultaneous camera/sensor views (e.g., wide-area + narrow FOV)
- Each channel gets its own MPEG-TS output stream on a separate multicast group/port
- Driven by CIGI View Definition per view ID

### 12D: Health & Monitoring
- Health file written periodically (for container orchestration liveness probes)
- Prometheus metrics endpoint (frame rate, encode latency, CIGI packet rate, tile load count)
- IG Status message (opcode 115) sent to host with frame timing stats

### 12E: Recording & Playback
- Record CIGI input stream to file for deterministic replay
- Record raw video + KLV to local .ts file for offline analysis
- Playback mode: read recorded CIGI from file instead of UDP socket

---

## CIGI 3.3 Packet Coverage

| Opcode | Packet | Status |
|--------|--------|--------|
| 1  | IG Control | ✅ Parsed (sender) |
| 2  | Entity Control | ✅ Full multi-entity |
| 4  | Component Control | ✅ Lights / damage states |
| 6  | Articulated Part Control | ✅ Skeletal mesh bones |
| 8  | Rate Control | ✅ Dead-reckoning |
| 9  | Celestial Sphere Control | ✅ Sun/moon/stars |
| 10 | Atmosphere Control | ✅ Fog / visibility |
| 12 | Weather Control | ✅ Cloud / precipitation |
| 16 | View Control | ✅ Gimbal pan/tilt |
| 17 | Sensor Control | ✅ EO/IR/NVG polarity |
| 20 | View Definition | ✅ FOV presets |
| 24 | HAT/HOT Request | ✅ Terrain line trace |
| 25–26 | LOS Request | ✅ Line-of-sight query |
| 101 | Start of Frame (response) | ✅ SOF heartbeat |
| 102 | HAT/HOT Response | ✅ Terrain height reply |
| 103 | LOS Response | ✅ LOS reply |
