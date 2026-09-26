# CamSim Roadmap

Started fresh on 2026-09-26 after a full architecture review. It replaces `Plan.md` and the
per-phase plans that used to live in `docs/superpowers/plans/`.

## Direction

CamSim stays on **UE5 + Cesium for Unreal + FFmpeg + CIGI (CCL)**. That stack is still the
right foundation for an open-source alternative to VRSG. The work ahead is about **depth, not
breadth**. First, make what exists correct against the standards it claims. Then make the
sensor physically meaningful. Only then add surface area.

Guiding rules:

1. **Standards correctness beats feature count.** A host integrator or MISB decoder tests
   exactly the edge cases (entity-relative coordinates, frame counters, timestamps,
   checksums). One wrong field breaks interoperability.
2. **Keep pixels on the GPU.** CPU-side image processing is a fallback, not the main path.
3. **One source of truth** for time, coordinate frames, and entity state.
4. **Nothing is "done" until CI proves it.** No `|| true` on validation steps.
5. **No new feature phases** until Milestones 0–4 land.

---

## Milestone 0: Correctness fixes (standards and geometry)

Small, independent fixes. Each gets its own commit and a regression test.

**Status (2026-09-26): done.** All eight are fixed (see "Milestone 0 results" below), and a live
stream driven by a scripted CIGI host passes the misb.js check on macOS
(`scripts/ci_validate.sh --native`).

| ID  | Issue                                                                                                                                                                                                                                                             | Where                                                                                       | Fix                                                                                                                                                                                                        |
| --- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 0.1 | **Heading ~90° off; frame tilts away from the origin.** Rotations are set in UE world space, but Cesium world space is +X=East. A CIGI heading of 0° (north) renders facing east, and the local up/north frame drifts with distance from the georeference origin. | `Camera/CamSimCamera.cpp:163,936,1279`, `Entity/CamSimEntity.cpp` (`SetActorRotation`)      | Use `GlobeAnchor->SetEastSouthUpRotation()` with yaw = heading − 90°. Route every conversion through `Geospatial/` (see 2.2). Test: heading 0 looks north at lat 0 and lat 60, and 500 km from the origin. |
| 0.2 | **KLV Tag 2 is not UTC.** It uses `FPlatformTime::Seconds()` (a monotonic clock since boot), so decoders show 1970 dates.                                                                                                                                         | `Camera/CamSimCamera.cpp:1455`                                                              | Short term: capture the UTC↔monotonic offset once at startup. Long term: use the sim clock (2.1).                                                                                                          |
| 0.3 | **KLV fails the downstream decoder.** See the misb.js conformance section below.                                                                                                                                                                                  | `Metadata/KlvBuilder.cpp`, `scripts/validate_klv.py`, `deploy/camsim_config.yaml:130-135`   | Fix all items in the section below. Add the misb.js conformance test to CI.                                                                                                                                |
| 0.4 | **Entity-relative CIGI coordinates ignored.** Offsets in metres are read as lat/lon degrees for attached Entity Control, HAT/HOT requests, and LOS segment/vector requests.                                                                                       | `CIGI/CigiReceiver.cpp` (entity ctrl, HAT/HOT, LOS processors), `CIGI/CigiQueryHandler.cpp` | Carry `AttachState`/`ParentID` and the source/destination coordinate-system flags through. Resolve the offset in the parent entity's frame.                                                                |
| 0.5 | **SOF echoes only 8 bits of the host frame counter** (it's 32-bit on the wire), so the value wraps every 256 frames.                                                                                                                                              | `Subsystem/CamSimSubsystem.cpp:744`, `CIGI/CigiSender.h:50`                                 | Pass `uint32` end to end.                                                                                                                                                                                  |
| 0.6 | **Ground speed (Tag 8) wrong.** It's zeroed on any tick without a host update, and divided by the IG's frame time rather than the host's update interval.                                                                                                         | `Camera/CamSimCamera.cpp:961,971`                                                           | Derive speed from host timestamps or Rate Control. Hold the last value between updates.                                                                                                                    |
| 0.7 | **Frames stream before terrain is loaded.** Startup and teleports emit low-detail terrain with no signal, which is bad data for ATR training.                                                                                                                     | `Camera/CamSimCamera.cpp:607` (only logged)                                                 | Gate capture/encode on tileset load progress around the camera. Report `terrain_ready` in `/ready` and the health JSON.                                                                                    |
| 0.8 | **libx264 has no VBV cap**, so bitrate spikes burst over UDP (bad on ROVER/ATAK links).                                                                                                                                                                           | `Encoder/VideoEncoder.cpp`                                                                  | Set `nal-hrd=cbr`, `vbv-maxrate`, and `vbv-bufsize` (~1–1.5× target).                                                                                                                                      |

### 0.3: KLV conformance with misb.js

**Reference:** [vidterra/misb.js](https://github.com/vidterra/misb.js) (npm
`@vidterra/misb.js` 0.1.30, commit `52c3837`). This is the decoder on the receiving side, so
it defines "correct" for this project. It replaces the old spec PDF.

These problems were found on 2026-09-26 by running a port of `FKlvBuilder` through
`st0601.parse()`:

| #   | Problem                                                                                                                                                     | What misb.js does with it                                                                                | Fix                                                                                                                                        |
| --- | ----------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------ |
| a   | **Wrong checksum algorithm.** It defaults to CRC-16/CCITT; misb.js expects a 16-bit running sum.                                                            | Marks every packet `checksum.valid = false`.                                                             | Use the running sum only and remove the `crc16` option and config key.                                                                     |
| b   | **Wrong checksum range.** Even `bcc16` mode sums before appending the checksum's own `01 02` bytes. misb.js sums everything except the final 2 value bytes. | Still `valid = false`.                                                                                   | Append `01 02`, *then* sum.                                                                                                                |
| c   | **Target track gate written as tags 40/41.** In ST 0601 those are Target Location Lat/Lon (4 bytes each); gate width/height are **43/44**.                  | **Parsing throws `RangeError` and the whole packet is lost** whenever `target_track_gate_*` is non-zero. | Move the gate values to tags 43/44. misb.js decodes them as `2 × value` pixels, so encode `width / 2`.                                     |
| d   | **Tag 47 flag bits wrong.** The spec numbers bits from 1 at the least-significant end. CamSim sets `0x20` for black-hot and `0x08` for slant range.         | Black-hot reads as **"Image Invalid"** (bit 6); slant range reads as **"Icing Detected"** (bit 4).       | IR black-hot = `0x04` (bit 3). The slant-range bit (`0x10`, bit 5) means "measured"; CamSim's slant range is calculated, so leave it at 0. |
| e   | **Ground speed sent as Tag 8.** Tag 8 is Platform *True Airspeed*; ground speed is **Tag 56**.                                                              | Decoded as airspeed.                                                                                     | Emit Tag 56. Emit Tag 8 only with a real airspeed. Combines with 0.6.                                                                      |
| f   | **ST 0102 country codes in the wrong tag.** The country-code string goes in tag 12, which is *Object Country Coding Method* (a 1-byte code).                | Decodes as "No reference for 85".                                                                        | Tag 12 = `1` (ISO-3166 two-letter); country string in **tag 13**. Check tag 13's string encoding against misb.js `st0102.js`.              |
| g   | **Tag 2 is not UTC.** (Same as 0.2.)                                                                                                                        | Shows a 1970 date.                                                                                       | See 0.2 / 2.1.                                                                                                                             |
| h   | **`validate_klv.py` passes broken output.** It shares CamSim's checksum-range bug, accepts either algorithm, and doesn't check tag semantics.               | n/a                                                                                                      | Replace the Python validator with a Node check that uses misb.js.                                                                          |

**Conformance test (CI gate):**

1. Take KLV from both the unit-test builder output and a live stream
   (`ffmpeg -i out.ts -map 0:d:0 -c copy -f data -`).
2. Parse it with pinned misb.js.
3. Assert all of the following:
   - no exceptions;
   - `checksum.valid !== false`;
   - tag 2 is within ±1 s of the sim clock;
   - decoded values match input telemetry within quantization tolerance for every emitted tag;
   - no unknown tags.

Keep the misb.js version pinned. Treat bumping it as a deliberate change: re-run the
conformance suite when it moves.

**Exit criteria:** all eight fixed with tests. A recorded stream passes the misb.js
conformance test in CI, and a scripted CIGI host (heading sweep plus an attached child
entity) renders correctly.

### Milestone 0 results

| ID  | Change | Regression tests (`CamSim.*`) |
| --- | ------ | ----------------------------- |
| 0.1 | `Geospatial/CigiFrames.h` converts CIGI heading/pitch/roll to Cesium East-South-Up; the camera, entities and first-person view use `SetEastSouthUpRotation`. Also fixed: dead-reckoned entities pitched nose-up *descended* (body Z sign), and the laser-designator overlay projected from the actor instead of the gimballed sensor. | `CigiFrames.Math`, `CigiFrames.HeadingNorthAwayFromOrigin` (real Cesium georeference: origin, 500 km away, lat 60, equator, southern hemisphere) |
| 0.2 | `Metadata/UtcClock` anchors the monotonic clock to UTC once; Tag 2 uses it. | `UtcClock.*` |
| 0.3 | All eight items (a–h) fixed. `klv_checksum` config key removed. Tags 21/23–25 are now omitted together when the boresight has no ground intersection (they carried stale/zero frame centres). `scripts/validate_klv.py` replaced by `scripts/klv_conformance/check.js`, which CI runs on exported packets (unit-tests job) and on the captured stream (`ci_validate.sh`). | `Phase26.*`, `KlvConformance.ExportPackets` + misb.js check |
| 0.4 | Receiver carries attach state, parent id and coordinate-system flags (casting to the CCL classes a 3.3 session actually builds). Attached entities and an attached camera platform follow their parent every tick; entity-relative HAT/HOT, LOS segment (incl. Destination Entity ID) and LOS vector requests are resolved to geodetic first. | `CigiEntityRelative.Math`, `CigiEntityRelative.ReceiverCarriesFlags` (real CCL host packets over UDP) |
| 0.5 | SOF echoes the full 32-bit host frame counter. | `CigiSender.SofEchoesFull32BitHostFrame` |
| 0.6 | `FCigiHostClock` stamps each update with host time (IG Control timestamp when valid, arrival time otherwise); `FGroundSpeedEstimator` holds the estimate between updates. Speed moved to Tag 56. | `GroundSpeed.*` |
| 0.7 | `FTerrainReadinessGate` holds capture until tilesets report ≥ 99% (re-arms on teleports > 5 km, 30 s timeout); `terrain_ready` in `/ready` and `camsim_health.json`; `terrain_gate:` config block. Also fixed: the heartbeat log reported tile progress ×100. | `TerrainReadinessGate.HoldsUntilLoaded` |
| 0.8 | `rc_max_rate` = bitrate, 0.5 s VBV buffer, `nal-hrd=cbr` for libx264. Without it a single noise frame hit 81 KB against a 37 KB budget. | `VideoEncoder.VbvCapsBurstsOnNoise` |

Every new test was checked to fail with its fix reverted. Full suite: 208 tests pass
(macOS, UE 5.7).

**Verified end to end on macOS (2026-09-26):** `scripts/ci_validate.sh --native` launches CamSim
headless, drives it with `send_cigi_test.py --sweep`, waits for `/ready`, captures 5 s of the
stream and checks it: H.264 + KLVA present, no decode errors, all 150 KLV packets conform to
misb.js, and the KLV sensor position (tags 13/14/15) matches the pose the host commanded. That
last check was added because the first live run passed while the camera ignored the host
entirely (see below). About 30 s warm; a cold start adds a few minutes of shader compiles. It
needs the Xcode Metal toolchain (`xcodebuild -downloadComponent MetalToolchain`).

Bugs the live run found:

- `send_cigi_test.py` defaulted `--entity-id` to 0, but `camera_entity_id` has been 1 since
  April (to match trillium-cigi), so its Entity Control spawned a stray entity 0 and the camera
  never moved. It also identified itself as CIGI 3.0 (IG Control minor version 0; CCL reads 3 as
  3.3) and sent 100 µs timestamp ticks instead of the ICD's 10 µs, which made the host-time
  ground speed (0.6) 10× too high.
- `deploy/camsim_config.yaml` still had `health_http_enabled: false`, overriding the code
  default, so `/live`, `/ready` and `/metrics` were off in the shipped config (and in Docker).
- UE's HTTP server binds to 127.0.0.1 unless configured, so the health server was unreachable from
  outside the host/container. `DefaultEngine.ini` now sets `[HTTPServer.Listeners]
  DefaultBindAddress=0.0.0.0`.
- `ci_validate.sh` could not fail: missing streams and decode errors were only warnings, and its
  capture had no `-map 0`, so the KLV stream was never recorded for the misb.js check. Every
  check is now a hard failure, and it no longer needs coreutils `timeout` (absent on macOS).
- `run.sh` printed the wrong log path for the macOS editor (UE logs to
  `~/Library/Logs/CamSimTest/`).

### Engine and dependency upgrade (2026-09-26)

| From | To |
| ---- | -- |
| UE 5.7 | UE 5.8.3 (`EngineAssociation` `"5.8"`, `BuildSettingsVersion.V7`, `Unreal5_8` include order) |
| Cesium for Unreal 2.23.0 | 2.29.1 (`CesiumForUnreal-58`). Camera registration moved from the deprecated `AddCamera`/`UpdateCamera`/`RemoveCamera` to `AdditionalCameras`. |
| glTFRuntime 20260113 | 20260826 (upstream UE 5.8 fixes) |
| FFmpeg n7.0 (libavcodec 61) | n8.1.3 (libavcodec 62). `FF_PROFILE_*` → `AV_PROFILE_*`. |

- UE 5.8 requires editor targets on an installed engine to match its build settings, so the
  targets must use V7.
- Cesium for Unreal is now installed into the engine (`$UE_ROOT/Engine/Plugins/Marketplace`) instead
  of the project's `Plugins/`, so UBT uses the release zip's prebuilt binaries. As a project plugin
  it was rebuilt from source (~20 min), which needed `repo_setup.sh` to patch Cesium: 2.29.1's
  `IonQuickAddPanel.cpp` captures a variable inside its own initializer, which Apple clang 21
  rejects. With the engine install, `repo_setup.sh` patches nothing. (Of the old patches, the
  `CesiumCartographicPolygon.cpp` include was already upstream, and on macOS V7 only warns about
  Cesium's unreachable code.) `repo_setup.sh` also replaces a Cesium install at the wrong version
  and fast-forwards glTFRuntime.
- V7's `-Wunreachable-code-loop-increment` found a real bug: a designer-placed `ACamSimCamera`
  made `ACamSimGameMode::BeginPlay` return before spawning `ACamSimEnvironment`.
- `encoder: videotoolbox` (macOS hardware H.264/HEVC). It is opt-in: on pure noise it overshoots
  `rc_max_rate` about 3.6× even with `qmax=51` and `constant_bit_rate`, so `auto` stays NVENC →
  libx264. An encoder that fails `avcodec_open2` now falls through to the next candidate.
- Fixed: `FVideoEncoder::Close()` flushed packets without rescaling timestamps or writing them to
  the local recording. It had never been hit before, because libx264 in zero-latency mode buffers
  nothing, while VideoToolbox holds about 10 frames.

Full suite: 209 tests pass (macOS, UE 5.8.3, FFmpeg 8.1.3); misb.js KLV check passes.

**Human follow-ups:**
- *(Deferred — there is no Linux CI runner for now; macOS is the reference platform.)* The Linux
  CI runner (`/opt/UE`) needs UE 5.8 installed, then `scripts/repo_setup.sh` and
  `scripts/build_thirdparty.sh` (FFmpeg 8 + fresh Linux libs) re-run before the UE5 jobs go green.
  `repo_setup.sh` writes Cesium into `/opt/UE/Engine/Plugins/Marketplace`, so run it as a user who
  can write there (or with sudo). Not yet verified on Linux: that packaging (`BuildCookRun`) uses
  the prebuilt `UnrealGame` libs and doesn't rebuild Cesium.
- Open `CamSimTest` once in the 5.8 editor and resave the maps, so assets stop loading through
  the 5.7 upgrade path.
- VideoToolbox adds about 10 frames (~330 ms at 30 fps) of pipeline latency. FFmpeg exposes no
  `MaxFrameDelayCount` option, so reducing it needs a patch or a direct VT session.

---

## Milestone 1: Engineering health

| ID  | Item                                                                                                                                                                                                                |
| --- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1.1 | ~~Remove `\|\| true` from the `integration-test` job so video/KLV validation can actually fail CI.~~ **Done 2026-09-26**, along with making `ci_validate.sh` strict (see Milestone 0 results). Until a Linux runner exists, run `scripts/ci_validate.sh --native` on macOS before merging. |
| 1.2 | Replace tautological tests (e.g. `Tests/Phase19OceanTest.cpp` "fields" tests that assert a value equals what was just assigned) with tests that go through `FCamSimConfig::Load()` using YAML and env-var fixtures. |
| 1.3 | Warn on YAML keys that are never read, so typos stop silently falling back to defaults. (A cheap pass over ryml nodes; no reflection rewrite.)                                                                      |
| 1.4 | Split `ACamSimCamera` (~1,700 lines) into: **sensor rig** (pose + gimbal), **capture pipeline** (render targets, readback), **telemetry/KLV assembly**, and **Cesium streaming controller**.                        |
| 1.5 | Sensor CPU path: reuse scratch buffers in blur, lens distortion, vibration, and Gaussian MTF (currently a full-frame allocation per effect per frame). Parallelize the AGC histogram build.                         |
| 1.6 | Smaller fixes: keep the gimbal slewing toward its target between host packets; have a hot-reload parse failure skip only the reload block instead of returning out of `Tick()`.                                     |
| 1.7 | Branch hygiene: delete the six merged `refactor/camsim-phase-*` branches on origin. (Stale local branches, including the abandoned ocean branch, were removed on 2026-09-26.) |
| 1.8 | ~~LOS responses report `Valid = false` when the path is clear.~~ **Not a bug.** CIGI 3.3 §4.2.4 defines Valid as "whether the Range parameter is valid. The range will be invalid if no intersection occurs", so a clear segment is correctly `Valid = 0, Visible = 1`. Real follow-up: a segment whose destination lies on the terrain surface can hit the surface at the destination itself and report Occluded; consider treating a hit within a small tolerance of the destination as Visible. |
| 1.9 | Extended HAT/HOT and LOS responses (intersection point, normal, `Response Coordinate System`) aren't implemented; extended requests get basic responses. |
| 1.10 | The remaining CCL packet processors `static_cast` to `…V3` classes while a 3.3 session builds `…V3_2`/`…V3_3` siblings. It works only because all fields live in the shared base classes; cast to the class CCL actually creates (done for entity control, HAT/HOT and LOS in 0.4). |
| 1.11 | KLV Tags 15/25 are defined as MSL but carry ellipsoid height (Cesium HAE). Convert with a geoid model (EGM96) and emit HAE in Tags 75/78. |
| 1.12 | `camsim_health.json` field `dropped` actually reports encoder watchdog reconnects. (`cigi_rx`, which reported encoded frames, was fixed in 0.7.) |
| 1.13 | An attached camera platform can lag its parent by one frame (actor tick order vs. the entity manager). Resolve both in one ordered pass. |

---

## Milestone 2: Core foundations (time, frames, protocols)

### 2.1 Sim clock

One authoritative simulation time (date + time of day) drives KLV timestamps, sun/moon
position, HUD time, annotations, and dead reckoning. It can be set from:

- CIGI IG Control timestamps and Celestial Sphere Control date
- DIS timestamps
- scenario files
- a lockstep API (2.4)

It supports real-time, faster/slower than real time, and **deterministic lockstep**
(needed for reproducible ML datasets).

It replaces two stopgaps from Milestone 0: `FUtcClock` (KLV Tag 2, wall-clock UTC) and
`FCigiHostClock` (host time per CIGI message, used for ground speed).

### 2.2 Coordinate-frame module

Started in Milestone 0: `Geospatial/CigiFrames.h` covers CIGI heading/pitch/roll, local
North-East-Up, Cesium East-South-Up and entity-relative (body-frame) offsets, with tests.
Remaining: DIS ECEF orientation, and moving the DIS adapter and ad-hoc flat-earth maths
(e.g. `ComputeGeometricLOS` fallback, LOS vector end point, dead-reckoning position) onto it.

All conversions live in `Geospatial/` with round-trip unit tests:

- CIGI north-referenced heading/pitch/roll, with entity-relative offsets
- DIS ECEF plus Euler angles
- East-South-Up
- UE world space

Nothing outside this module calls `SetActorRotation` with protocol angles.

### 2.3 Host adapter layer

Every protocol becomes an adapter that emits one canonical command model (geodetic pose, sim
time, entity IDs, view/sensor/environment commands). The renderer never sees protocol types.

```
CIGI 3.3/4.0 ─┐
DIS (listen) ─┤
MAVLink ──────┼─► Host adapters ─► Canonical commands ─► Entity / View / Env / Clock
Python/gRPC ──┤
Scenario file ┘
```

- **CIGI** stays the primary interface. It's what VRSG-style host integrators speak.
  Internals must not be tied to 3.3 packet layouts, so CIGI 4.0 can follow.
- **DIS** stays a passive "stealth viewer" (renders entities it hears on the network).
  - Define precedence and separate ID ranges when CIGI and DIS both describe the world.
  - Implement the full DIS dead-reckoning set (algorithms 1–9, including acceleration);
    today only algorithms 2 and 5 extrapolate.
- **HLA:** support it through an external HLA↔DIS gateway, not natively.

### 2.4 Control API (Python/gRPC)

Set up a scene, spawn entities, randomize, step the clock, and capture frames plus ground
truth. This becomes the main interface for ML dataset generation and batch runs, and
replaces ad-hoc batch-runner paths.

---

## Milestone 3: GPU-resident sensor pipeline

Current path:

1. Render an 8-bit, already tone-mapped image.
2. Read it back to the CPU.
3. Run about 2,000 lines of CPU effects.
4. Convert colour with `sws_scale`, then encode.

Target path:

1. **Capture in HDR scene-linear** (plus the thermal pass from Milestone 4), not the
   8-bit tone-mapped image.
2. **Sensor model as RDG compute shaders:**
   - optics: MTF/PSF, lens distortion, vignette, chromatic aberration
   - detector: noise, fixed-pattern noise, hot/dead pixels, rolling shutter, AC banding
   - electronics: AGC, quantization, polarity
3. Keep stateful control logic (AGC histograms, NUC timing) on the CPU as small parameter
   updates.
4. **Convert RGB/gray → NV12 on the GPU**, then either:
   - encode directly from GPU memory with NVENC (see UE AVCodecs / Pixel Streaming 2), or
   - read back only NV12 (1.5 bytes/px instead of 4).
5. Readback is a ring of render-thread-polled readbacks. (The per-tick
   `FlushRenderingCommands` is already gone; today only one readback is in flight at a
   time, gated on `EReadbackState::Idle`.)
6. Retire the parallel CPU pipeline (`Sensor/SensorPostProcess.cpp`) and the partial
   material-based GPU path. Keep one reference implementation, used for tests and as the
   fallback when no NVIDIA GPU is present (Mesa/llvmpipe).
7. Evaluate rendering the sensor through the primary view family under `-RenderOffscreen`
   instead of `SceneCapture2D`. That would allow TSR/temporal AA and full renderer
   features, and avoids paying for a second scene render if the main viewport also renders.

**Exit criteria:** 1080p30 EO and IR with all sensor effects enabled, below 50% of the frame
budget on the reference GPU. The pipeline benchmark is tracked in CI.

---

## Milestone 4: Physically based IR (flagship)

Today IR is the luma of the EO image pushed through a curve. So hot/cold relationships
follow visible albedo, night IR goes dark, and ATR models learn EO cues.

| ID  | Item                                                                                                                                                                                                             |
| --- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 4.1 | **Thermal material model:** a data asset keyed by physical material / class ID holding base temperature, emissivity, thermal inertia, and solar-loading response.                                                |
| 4.2 | **Terrain classification:** drape a land-cover raster (e.g. ESA WorldCover) as a Cesium raster overlay so terrain and photogrammetry get thermal classes.                                                        |
| 4.3 | **Entity thermal state:** engine, exhaust, and skin temperatures driven by entity state (running, speed, damage) through the component/CIGI interface.                                                           |
| 4.4 | **Thermal render pass:** compute in-band radiance (MWIR/LWIR) from temperature, emissivity, sky/solar terms, and path attenuation, independent of visible lighting. Feed it into the Milestone 3 detector model. |
| 4.5 | **Class-ID stencil reuse:** the same stencil gives semantic and instance segmentation for ML ground truth.                                                                                                       |
| 4.6 | **Validation:** compare against reference imagery / published contrast data (e.g. NETD-limited scenes, diurnal crossover).                                                                                       |

---

## Milestone 5: Drone-community interfaces

| ID  | Item                                                                                                             |
| --- | ---------------------------------------------------------------------------------------------------------------- |
| 5.1 | **MAVLink adapter:** PX4/ArduPilot software-in-the-loop sims drive vehicle pose and gimbal (Gimbal Protocol v2). |
| 5.2 | **RTSP/RTP output** alongside MPEG-TS multicast, for the autonomy/QGroundControl toolchain.                      |
| 5.3 | **ROS 2 bridge** (images + camera info + TF) via the control API.                                                |
| 5.4 | **CIGI 4.0** adapter.                                                                                            |

---

## Later / only when there's demand

- Mass/ECS entities with a timestamped interpolation buffer, for pattern-of-life scenes with
  thousands of entities. (One actor per entity is fine up to a few hundred.)
- Multiple sensors per platform and multiple platforms per instance, with sensors as
  components.
- SAR/radar, VR/dome output, scenario editor GUI.

## Parked

Existing features (ocean, weather FX, HUD, DIS, CoT, scenario engine, formation flying,
optical realism, etc.) stay in the tree and keep working. They are not being extended until
Milestones 0–4 land. Designs for those features remain in `docs/superpowers/specs/`.
