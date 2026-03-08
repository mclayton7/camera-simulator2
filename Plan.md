# CamSim Roadmap

## Implemented (Phases 1–12)

All features are complete. The pipeline is production-ready for EO/IR/NVG
sensor simulation with full CIGI 3.3 support, STANAG 4609 compliance,
security metadata, health monitoring, and recording/playback.

| Phase | Feature                                                                              | Status |
| ----- | ------------------------------------------------------------------------------------ | ------ |
| 1–6   | Core pipeline: CIGI input, Cesium terrain, H.264/KLV MPEG-TS output                  | ✅ Done |
| 7     | Environment: day/night cycle, sky atmosphere, fog, cloud/weather layers              | ✅ Done |
| 8     | Entity rendering: aircraft/vehicles, dead-reckoning, articulated parts, lights       | ✅ Done |
| 9     | Gimbal & sensor: 3-DOF gimbal, slew limits, FOV presets, polarity                    | ✅ Done |
| 10    | Terrain feedback: HAT/HOT and LOS line traces, SOF heartbeat, IG→Host UDP            | ✅ Done |
| 11    | Sensor simulation: EO/IR/NVG post-process, noise, vignetting, atmospheric extinction | ✅ Done |
| 12A   | MISB ST 0102: security classification local set in every KLV packet                  | ✅ Done |
| 12B   | STANAG 4609: PID allocation, KLV rate, PTS sync, H.265/HEVC encoder                 | ✅ Done |
| 12C   | Multi-channel: multiple simultaneous output streams with digital zoom                | ✅ Done |
| 12D   | Health & monitoring: health file, Prometheus metrics, IG mode in SOF                 | ✅ Done |
| 12E   | Recording & playback: CIGI recording/replay, local .ts video recording               | ✅ Done |

---

## CIGI 3.3 Packet Coverage

| Opcode | Packet                    | Status                   |
| ------ | ------------------------- | ------------------------ |
| 1      | IG Control                | ✅ Parsed (sender)        |
| 2      | Entity Control            | ✅ Full multi-entity      |
| 4      | Component Control         | ✅ Lights / damage states |
| 6      | Articulated Part Control  | ✅ Skeletal mesh bones    |
| 8      | Rate Control              | ✅ Dead-reckoning         |
| 9      | Celestial Sphere Control  | ✅ Sun/moon/stars         |
| 10     | Atmosphere Control        | ✅ Fog / visibility       |
| 12     | Weather Control           | ✅ Cloud / precipitation  |
| 16     | View Control              | ✅ Gimbal pan/tilt        |
| 17     | Sensor Control            | ✅ EO/IR/NVG polarity     |
| 20     | View Definition           | ✅ FOV presets            |
| 24     | HAT/HOT Request           | ✅ Terrain line trace     |
| 25–26  | LOS Request               | ✅ Line-of-sight query    |
| 101    | Start of Frame (response) | ✅ SOF + IG mode          |
| 102    | HAT/HOT Response          | ✅ Terrain height reply   |
| 103    | LOS Response              | ✅ LOS reply              |
