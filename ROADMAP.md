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
| 1.2 | ~~Replace tautological tests with tests through the loader.~~ **Done 2026-09-26.** Deleted 18 tests that asserted what they had just assigned or exercised only logic written inside the test (`Phase19.Fields/CigiWaveState/WakeLifecycle/SeaDomainDispatch`, `Phase27.GpuSensorBypass/HotReloadImmutableFields/TilePrefetchSlewDetection`, eight `Phase18` weather-zone/particle/crater tests, `CesiumBackend.Terrain/ImagerySourceValues`, `OpticalRealism.ConfigParse`). Added `Config.YamlSectionsApplied` (non-default values in every major section through `LoadFromYamlString`) and `Config.EnvOverridesYaml`, which found that boolean env vars parsed `true` as off (`Atoi`); they now accept true/yes/on. The `*.ConfigDefaults` tests stay: they pin documented defaults. **Untested product code** these fakes stood in for: weather-zone blending, cloud coverage → shadows, crater ring buffer, tile-prefetch slew detection, sea-domain vessel-motion dispatch, hot-reload immutable-field handling. |
| 1.3 | ~~Warn on YAML keys that are never read.~~ **Done 2026-09-26.** Every lookup goes through `YamlHas`, which records the node; after loading, unread map keys are logged and kept in `FCamSimConfig::UnknownYamlKeys`. Maps whose keys are data (entity type maps, preset names) and sections parsed by other modules (`entity_types`) are excluded. `FCamSimConfig::LoadFromYamlString()` now lets tests go through the real loader, which 1.2 needs. Tests: `Config.UnknownKeysReported`, `Config.CanonicalConfigHasNoUnknownKeys`. |
| 1.4 | ~~Split `ACamSimCamera`.~~ **Done 2026-09-26.** The actor (now ~470 lines with its header, down from ~2,140) orchestrates the tick and delegates to `FCamSimPlatformRig` (platform pose, attachment, first-person view; with `UCamSimGimbalComponent` this is the sensor rig), `UCamSimCaptureComponent` (render targets, readback state machine, sensor model dispatch, encoder thread, render settings), `FCamSimTelemetryAssembler` (KLV telemetry, boresight frame centre) and `FCamSimStreamingController` (Cesium streaming cameras, slew prefetch, adaptive SSE, terrain gate). Behaviour preserved — live KLV identical to before — except FOV telemetry now updates every frame (it used to change only on a pose update) and the first frame no longer counts as a gimbal slew (it triggered a spurious 30-frame prefetch boost at startup). Tests: `Camera.Streaming.LevelOfDetail`, `Camera.Telemetry.FlatEarthFrameCenter`. |
| 1.5 | ~~Sensor CPU path: reuse scratch buffers; parallelize the AGC histogram.~~ **Already done** (found 2026-09-26): scratch buffers landed in `d28c227` (2026-05-11) and the AGC histogram is built in parallel bands. The only per-frame allocations left are a small Gaussian kernel and 8×256 band histograms. On macOS (no NVIDIA GPU) this CPU path is the production path until Milestone 3. |
| 1.6 | ~~Keep the gimbal slewing between host packets; don't abort `Tick()` on a hot-reload parse failure.~~ **Done 2026-09-26.** The ArtPart target persists and `TickGimbal` advances toward it on ticks without a packet (a host at 10 Hz previously got a third of the commanded slew at 30 fps); a ViewControl snap cancels it. Test: `Camera.Gimbal.ArtPart_SlewContinuesBetweenPackets`. Still open: `PollHotReloadConfig`'s async stat task captures a raw `this`, which can outlive the camera actor. |
| 1.7 | Branch hygiene: delete the six merged `refactor/camsim-phase-*` branches on origin. (Stale local branches, including the abandoned ocean branch, were removed on 2026-09-26.) |
| 1.8 | ~~LOS responses report `Valid = false` when the path is clear.~~ **Not a bug.** CIGI 3.3 §4.2.4 defines Valid as "whether the Range parameter is valid. The range will be invalid if no intersection occurs", so a clear segment is correctly `Valid = 0, Visible = 1`. Real follow-up: a segment whose destination lies on the terrain surface can hit the surface at the destination itself and report Occluded; consider treating a hit within a small tolerance of the destination as Visible. |
| 1.9 | ~~Extended HAT/HOT and LOS responses aren't implemented.~~ **Done 2026-09-26.** HAT/HOT Request Type 2 gets a HAT/HOT Extended Response (103), and LOS Request Type 1 gets an LOS Extended Response (105), per ICD 4.2.3/4.2.5: surface normal azimuth/elevation from the trace hit, Range Valid, a clear segment reporting its destination point, and X/Y/Z offsets in the hit entity's body frame when the request's Response Coordinate System is Entity (`CamSimFrames::GeodeticToBodyOffset`). Material code is 0 (CamSim has no material data yet). Verified live against terrain with a scratch host (HOT ≈ −16 m HAE downtown San Francisco, a 60° LOS vector from 1,000 m hitting at 1,165 m). Tests: `CigiSender.ExtendedResponsesDecode` (decoded by a CCL 3.3 host), `CigiFrames.GeodeticInverse`. Found along the way: Cesium physics meshes were disabled, so no terrain query had ever hit (fixed, `create_physics_meshes`). Still open: queries only hit *loaded* tiles, so points outside the camera's view miss (a segment 2 km beyond the footprint reported visible) — HAT/HOT could use `ACesium3DTileset::SampleHeightMostDetailed`, which loads what it needs; periodic requests (Update Period > 0) and Host Frame Number LSN aren't implemented. |
| 1.10 | ~~CCL packet processors `static_cast` to the wrong versioned class.~~ **Done 2026-09-26.** CCL builds a different class per host minor version (e.g. `CigiRateCtrlV3` for 3.0/3.1, `…V3_2` for 3.2+) and stamps the packet with it, and the accessors live on those classes rather than the `CigiBase*` class. `VisitVersioned` in `CigiReceiver.cpp` now casts to the class CCL actually built for Entity, Rate, Component, IG Control, HAT/HOT and both LOS requests (0.4's fixed V3_x casts were wrong for 3.0 hosts). This also found that **Rate Control's Coordinate System was ignored**: rates were always integrated as body-frame, but World/Parent (the 3.2+ default) means North/East/Down and heading/pitch/roll rates. `CamSimFrames::IntegrateRates` handles both, and dead reckoning now uses the WGS-84 radii instead of a flat 111,320 m/°. Tests: `CigiFrames.RateControlFrames`, `CigiEntityRelative.ReceiverCarriesFlags` (Entity ID + Rate Control coordinate system from a CCL 3.3 host). |
| 1.11 | ~~KLV Tags 15/25 carry ellipsoid height but are defined as MSL.~~ **Done 2026-09-26.** `Geospatial/Geoid.h` samples NGA's 15′ EGM96 grid (`Content/NonUFS/Geoid/WW15MGH.DAC`, git LFS, staged as a loose file; regenerate with `scripts/make_egm96_dac.py`). Tags 15/25 are now MSL; ellipsoid heights go in new Tags 75/78. Without the grid, 15/25 are omitted rather than mislabelled. Cesium Native's `EarthGravitationalModel1996Grid` was not used: it clamps instead of wrapping at 360°→0°, so it is off within 0.25° west of the prime meridian (0.17 m at NGA's test point). `check.js` verifies 15/25/75/78 with its own geoid sampler. Tests: `Geoid.MatchesNgaReference` (NGA's reference points, ±0.1 m), KLV conformance packets. CIGI altitudes needed no change: CIGI 3.3 defines its "MSL" as the ellipsoid surface. |
| 1.12 | ~~`camsim_health.json` field `dropped` reports watchdog reconnects.~~ **Done 2026-09-26.** `dropped` is now the encoder queue's real drop count (`ACamSimCamera::GetDroppedFrameCount`; the old `DroppedFrameCount` member was never incremented), and reconnects have their own `watchdog_reconnects` field. Editor runs now write the file to `Saved/` instead of the shared engine `Binaries/` directory. Still open: in the opt-in per-category `frame_drops` block, `encoder_busy` and `socket_error` are never incremented (only `readback_timeout` is). |
| 1.13 | ~~An attached camera platform can lag its parent by one frame.~~ **Done 2026-09-26.** It was wider than attachment: the camera captured in `TG_PrePhysics`, before `FCamSimEntityManager` (a tickable object, run after `TG_PostPhysics`) applied that frame's CIGI entity states, so every entity in the image was a frame stale, and environment changes landed a frame late too. Now one ordered pass: the entity manager applies entity states, then the camera platform state (`ApplyHostPlatformState`), resolves attachments parent-first, then the camera's own attachment (`FollowAttachParent`); the camera ticks in `TG_PostUpdateWork` after `ACamSimEnvironment` and captures. Remaining lag: an entity attached to a camera platform that is itself attached. Not yet measured live: needs a moving parent entity in `send_cigi_test.py`. |

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

**Status (2026-09-26): core done.** `Time/SimClock.h` (`FSimClock`): a UTC epoch advancing
at a rate (1 = real time, 0 = frozen, >1 faster) from a monotonic source, with lockstep
`Step()` for 2.4. It starts at the wall-clock time (`start_datetime` / `start_hour` override;
`sim_time_rate`), and CIGI Celestial Sphere Control sets it: Date/Time Valid → date and time,
Ephemeris Model Enable → running or static (applied only when the packet changes, since hosts
may resend it every frame). It drives KLV Tag 2, CoT, the HUD date-time group, ground truth
and the sun (CesiumSunSky now updates continuously). `FUtcClock` is gone. `FCigiHostClock`
stays: it measures host-relative differences (ground speed, ordering), a different quantity
from IG sim time. Tests: `SimClock.*`, `CigiEntityRelative.EnvironmentPacketsFromCclHost`.

Bugs found and fixed along the way:
- Celestial Sphere Control's date was read from the wrong bytes (CIGI 3.3: `uint32`
  MMDDYYYY at bytes 8–11), and Atmosphere Control was read one field late (humidity is a
  byte, not a float), so the "visibility" driving fog was the host's horizontal wind speed.
  The raw environment parser also assumed big-endian, while CCL hosts send native
  (little-endian) order declared by the IG Control byte-swap magic. A real CCL host's
  Celestial/Atmosphere/Weather values all came out wrong (year 47605, every float 0).
  `send_cigi_test.py` shared the wrong layouts and is fixed too.
- CesiumSunSky interprets `SolarTime` in its `TimeZone` (default −5, with DST); CamSim fed it
  UTC hours, so the sun was 4–5 h off. Now `TimeZone = 0`, DST off.
- The fallback sun model (no CesiumSunSky) got UTC instead of local solar time and a
  compass azimuth 180° off; the telemetry sun elevation only updated on sky-light recaptures.

Scenario and time steps (done 2026-09-26): scenario time is sim time. Elapsed time,
`DeltaTime` and the pattern-of-life time of day (local solar time) come from the clock,
`scenario.start_hour` sets the clock when the scenario starts, and `scenario.time_scale`
multiplies its rate. The fixed 1/30 s steps are gone: waypoint pauses use the tick's
scenario delta, and IR thermal drift/NUC use the time between frames' sim timestamps (so
a frozen clock freezes drift). Test: `Phase23.ScenarioEngine.WaypointPauseUsesSimTime`.

Still open for 2.1:
- Lockstep needs a driver (the 2.4 control API) and must also gate the engine tick; other
  per-tick integrations (dead reckoning, gimbal slew, ocean waves) still use UE's
  `DeltaTime` rather than sim time.
- DIS PDU timestamps are parsed but unused (2.3).
- Night renders as daylight: the scene capture's auto exposure brightens a sun-below-horizon
  scene. Needs a sensor exposure model (Milestone 3).

### 2.2 Coordinate-frame module

Started in Milestone 0: `Geospatial/CigiFrames.h` covers CIGI heading/pitch/roll, local
North-East-Up, Cesium East-South-Up and entity-relative (body-frame) offsets, with tests.
Remaining: DIS ECEF orientation and velocity, and moving the DIS adapter and ad-hoc flat-earth
maths (e.g. `ComputeGeometricLOS` fallback, LOS vector end point) onto it. (Dead-reckoning
position moved onto it in 1.10.)

DIS is wrong in two ways today: `DisEntityAdapter` passes the Entity State PDU's psi/theta/phi
(Euler angles relative to ECEF) straight through as local heading/pitch/roll, and it treats the
Entity Linear Velocity (world ECEF for dead-reckoning algorithms 2–5) as body-frame. Both need
ECEF → local NED at the entity's position.

**Status (2026-09-26): done.** `Geospatial/EcefFrames.h` adds WGS-84 geodetic ↔ ECEF, the
NED ↔ ECEF rotation, ECEF vectors → NED, and DIS Euler angles ↔ CIGI heading/pitch/roll.
`CigiFrames.h` gains the inverses (`GeodeticDeltaToNeu`, `GeodeticToBodyOffset`, `NeuToAzEl`).
- DIS: orientation is converted from ECEF-referenced Euler angles to local heading/pitch/roll,
  and dead reckoning now follows IEEE 1278.1 per algorithm — world (ECEF → NED) velocity for
  2–5, body velocity for 6–9, body angular velocity only for the rotating ones (3, 4, 7, 8).
  Previously only 2 and 5 were handled, both as body-frame, and 5 (a fixed-orientation
  algorithm) rotated. Rate control now carries separate linear and angular frames.
- Found: **body-frame angular rates pitched and rolled the wrong way** (UE's axis-angle
  quaternions about X and Y run opposite to CIGI's), so every CIGI Local-frame rate-controlled
  entity pitched/rolled backwards. Fixed; `CigiFrames.BodyRateSigns` pins the signs.
- The flat-earth maths (LOS vector end point, frame-centre fallback, scenario linear motion,
  formation offsets, randomizer jitter) now goes through `OffsetGeodetic`; `GeoConstants.h`
  (111,320 m/°) is gone. The DIS adapter's private ECEF conversion moved into the module.
- Vessel wave motion tilted the hull about UE world axes (wrong away from the georeference
  origin); it now tilts about the hull's own axes. The ocean surface itself is still a flat
  UE-world plane (parked).
- Tests: `CigiFrames.DisEcef`, `CigiFrames.BodyRateSigns`, `Phase21.AdapterConvertsEcefFrames`.
- Not verified against a live DIS federate (none available here).

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

Design: `docs/superpowers/specs/2026-09-26-host-adapter-layer-design.md` (approved).
Progress:
- Phase 1 (done): canonical commands (`Sim/Commands.h`) and pure CIGI/DIS converters
  (`Hosts/CigiCommands`, `Hosts/DisCommands`) with tests.
- Phase 2 (done): entities are keyed per source (`FEntityKey`). The entity manager is an
  `ISimCommandSink` fed by `FCigiHostAdapter`, the DIS adapter (now emitting commands with its
  dead-reckoning motion model; its ID allocator and `dis.id_base_offset` are gone) and the
  scenario (its own namespace). `ACamSimEntity` and the particle manager consume commands.
  CIGI entity 1000 and DIS entity 1000 are now different entities (verified live); previously
  DIS silently overwrote CIGI and the scenario overwrote both. Rate limiting is per key. CIGI
  query responses report only CIGI entities.

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
5. Readback is a ring of render-thread-polled readbacks. (Done in 3A.1: a three-slot ring,
   `Camera/ReadbackRing.h`, delivers frames in capture order; 3B.1 puts NV12 through it.)
6. Retire the parallel CPU pipeline (`Sensor/SensorPostProcess.cpp`) and the partial
   material-based GPU path. Keep one reference implementation, used for tests and as the
   fallback when no NVIDIA GPU is present (Mesa/llvmpipe).
7. Evaluate rendering the sensor through the primary view family under `-RenderOffscreen`
   instead of `SceneCapture2D`. That would allow TSR/temporal AA and full renderer
   features, and avoids paying for a second scene render if the main viewport also renders.

**Exit criteria:** 1080p30 EO and IR with all sensor effects enabled, below 50% of the frame
budget on the reference GPU (RTX 5090). The pipeline benchmark is tracked in CI.

**Plan (2026-09-26).** Split into four sub-projects, each with its own spec and plan:

| #  | Sub-project                 | Covers                                                                                                                              |
| -- | --------------------------- | ----------------------------------------------------------------------------------------------------------------------------------- |
| 3A | Render path + measurement   | Benchmark/reference-shot harness; sensor rendered as the primary view (TSR, one scene render, Cesium LOD transitions, origin shift); hitch and SSE tuning. Item 7 above. |
| 3B | Sensor model on the GPU     | Items 1–3, 6 (item 5, the readback ring, done early as 3A.1): tonemapper replaced by RDG compute on HDR input (minimal core effects first, auto-fallback to CPU), NV12, readback ring. |
| 3C | GPU-texture encode          | Item 4: UE `AVCodecs` NVENC/VideoToolbox from GPU textures; FFmpeg for TS/KLV muxing only.                                          |
| 3D | CI performance gate         | Exit criterion: 3A's harness as a tracked CI threshold.                                                                             |

3A is first because the user-visible problems are tile pop-in and frame rate/hitches, and the
current path probably renders the scene twice (main viewport + `SceneCapture2D`). 3A targets
macOS (M1 Pro) only; Linux/5090 runs are deferred.

Design: `docs/superpowers/specs/2026-09-26-render-path-design.md` (approved); plan:
`docs/superpowers/plans/2026-09-26-render-path.md`.

**3A status (2026-09-27): implemented on macOS; awaiting visual review.** Measured with
`scripts/bench/` on an M1 Pro, 1280x720, warm cache (baseline → 3A final, per phase):

| Metric | Baseline (SceneCapture2D) | 3A (primary view) |
| --- | --- | --- |
| Scene renders per frame | 1.50 (the unused game viewport rendered too) | **1.00** |
| Pop-in (frames with tiles loading) | orbit 0.48, slew 0.09, far 0.87 | **0.18, 0.03, 0.53** |
| Frames > 66 ms (orbit + low pass) | 8 | **2** (hitch counts are noisy run to run) |
| Frame time p99 | 34.7–37.7 ms | 34.8–37.3 ms |
| GPU p50 | 14.7–16.5 ms | 16.8–18.9 ms at 100% TSR; **13.4–15.4 ms at 75%** |
| Output frame rate | 14.9 fps | 14.9 fps (unchanged: see below) |

- The sensor is the game viewport's view (TSR, full Lumen/VSM/exposure history); a scene view
  extension grabs the final image into the existing readback ring. `render.view_source:
  scene_capture` keeps the old path (now without the unused viewport render) until 3B.
- Cesium LOD crossfade on (0.5 s); only the slew-prefetch stand-in camera remains; origin shift
  every 20 km (`render.origin_shift_distance_m`); TSR history reset on pose jumps.
- 1080p runs at the engine's fixed 30 Hz on the M1 Pro at defaults: p50 33.3 ms, no frames
  dropped to 15 Hz, GPU 17–19 ms (about 55% of the budget), but p95 is 33.9–35.6 ms, just over
  the spec's literal 33.3 ms (fixed-step jitter). No dev profile was needed.
  75% TSR is visually indistinguishable and saves 20–30% GPU (documented, not default).
- SSE stays 16: 8 costs GPU and pop-in, 4 collapses (277 hitches in the slew phase).
- The per-render GPU cost of the primary view (~18 ms) is higher than a SceneCapture render
  (~10 ms); the baseline's cost was dominated by the wasted viewport render.

Exit criteria (macOS): one render per frame ✅; fewer hitches ✅; less pop-in ✅; SSE ≤ 16 ✅;
1080p30 ⚠️ (holds 30 Hz with GPU headroom, but p95 33.9–35.6 ms misses the literal ≤ 33.3 ms; a
fixed-step engine can't meet a p95 equal to its own step); tests ✅ (219 automation + bench pytest); **orbit GPU below baseline ❌ at
100% TSR (✅ at 75%)**; visual review by the user ✅ (2026-09-27, shots after the −1 EV exposure change); `ci_validate` smoke skipped
(CI deferred by the user). Deferred: RTX 5090 runs.

Findings for follow-up:
- ~~**Output is ~15 fps, not 30**~~ **Fixed in 3A.1** (pulled forward from 3B item 5): a
  three-slot readback ring (`Camera/ReadbackRing.h`) delivers frames in order, and
  `FEncoderThread` now paces start to start (it slept a full interval after each encode, capping
  output near 26 fps once input reached 30). Full bench on the M1 Pro: 29.7–29.9 fps in every
  phase at 720p and 1080p (was 14.8–15.0), 0 dropped, frame time p95 34–36 ms; stream 30.0 fps
  with 33.3 ms PTS spacing; KLV conformant. The CPU sensor model keeps up at 1080p with the
  default effects. The CPU sensor model now has ~33 ms per frame; if it falls behind, frames are
  skipped until 3B's GPU sensor model (decision 2026-09-27).
- ~~**Over-exposure**~~ **Addressed (2026-09-27)**: auto-exposure kept (user decision) with
  `render.exposure_compensation_ev` = −1 EV relative to UE's default bias (+1); mean shot luma
  ≈200 → ≈170. `r.EyeAdaptation.CachedLightingPreExposure=8` covers the physically bright sun
  (it removes Lumen's clipping warning but was not the cause). Remaining: a flat bluish aerial
  haze from the atmosphere — a separate follow-up if wanted.
- ~~Restarting within ~30 s fails to bind the health port (:8080, TIME_WAIT) while logging
  "listening".~~ **Fixed (2026-09-27)**: the bind failure is detected
  (`GetHttpRouter(..., bFailOnBindFailure)`); the server logs that it is not listening and
  retries every 2 s until the port frees (verified live: up 18 s after a restart into
  TIME_WAIT). Address reuse stays off (UE's option also sets SO_REUSEPORT).
- ~~A View Definition FOV and a Sensor Control gain in the same host frame fight (the preset wins
  every frame).~~ **Fixed (2026-09-27)**: a gain changes the FOV only when it selects a
  different preset, and View Definition is applied after Sensor Control. Verified live: a host
  sending Sensor Control every frame plus a 30° View Definition gets HFOV 30.0° in all 300 KLV
  packets.
- ~~`scripts/tests/test_send_cigi.py::test_pack_ig_control_header` is stale~~ **Fixed
  (2026-09-27)**.
- ~~Ten minor items from the 3A final review~~ **Fixed (2026-09-27)**: snapshot requests are
  answered 503 at shutdown and the PNG encode holds no module reference; non-positive camera-cut
  thresholds are a validation error and skipped; a stretched grab (view aspect ≠ capture) is
  logged once; the unread `GrabCount` is gone; a relative `frame_stats_path` is taken from the
  launch directory; `use_lod_transitions` applies only in the primary view; the bench tolerates
  a partial frame-stats line, checks only the local address for a busy port, and stops waiting
  if no pid file appears.

**3B.1 status (2026-09-27): GPU sensor pipeline implemented on macOS; visual review signed off by the user (2026-09-27).**
Spec: `docs/superpowers/specs/2026-09-27-gpu-sensor-model-design.md`; plan:
`docs/superpowers/plans/2026-09-27-gpu-sensor-3b1-pipeline.md`. With
`render.sensor_path: gpu` the sensor model replaces UE's tonemapper
(`EPostProcessingPass::ReplacingTonemapper`) as RDG compute on HDR scene-linear input: a histogram
accumulated in the same pass feeds a CPU AE/AGC controller, gain + BT.709 OETF (EO) or detector
signal + AGC/polarity (IR/NVG), then NV12 packing on the GPU and a 1.5 bytes/px readback through the
3A.1 ring. `auto` (the default) still selects legacy, because the default config enables effects
that arrive in 3B.2/3B.3. Chromatic aberration is one of them (3B.3): UE 5.8 applies it inside the
tonemapper the graph replaces, so `auto` treats `optical_realism.chromatic_aberration` as unported.
The other tonemapper-stage UE effects (vignette, film grain, colour grading, bloom dirt mask) don't
apply on the GPU path either. A GPU path that can't run falls back to legacy with an error log: a
capture width not a multiple of 4 or an odd height (NV12), NullRHI, no SM5 compute, or missing
sensor shaders.

Measured with `scripts/bench/` on an M1 Pro, warm cache, per phase (orbit / slew / low pass / far
origin); baselines `scripts/bench/baselines/macos-m1pro-3b1-{720p,1080p}.json`:

| Metric | 3A.1 baseline 720p | Legacy 720p (3B.1 build) | GPU 720p | GPU 1080p (3A.1 1080p baseline) |
| --- | --- | --- | --- | --- |
| Frame time p95 (ms) | 37.1 / 35.0 / 34.8 / 36.0 | 33.9 / 34.5 / 34.7 / 36.2 | 34.0 / 34.7 / 35.1 / 36.3 | 34.1 / 34.8 / 35.0 / 36.3 (34.1 / 34.7 / 34.8 / 35.9) |
| GPU frame p50 (ms) | 19.4 / 18.3 / 19.0 / 19.0 | 16.8 / 16.6 / 18.1 / 18.4 | 16.9 / 12.9 / 18.0 / 19.4 | 17.1 / 18.0 / 18.4 / 19.2 |
| Sensor graph GPU p50 / p95 (ms) | — | — (CPU model) | 0.14–0.17 / 0.16–0.18 | 0.33–0.37 / 0.36–0.40 |
| Emitted fps | 29.8–29.9 | 29.7–30.0 | 29.7–30.0 | 29.8–29.9 |
| Dropped frames | 0 | 0 | 0 | 0 |
| Render thread p50 (ms) | 5.7–6.1 | 5.5–6.0 | 2.7–3.1 | 2.8–3.2 |
| Game thread p50 (ms) | 3.8–4.6 | 3.1–4.1 | 3.4–4.5 | 3.4–4.5 |

Exposure (GPU path, 720p `*_sensor.png`, BT.709 luma of the decoded frame, 0–255): daylight EO
shots (nadir, slant, horizon, low oblique, far-origin slant/nadir) mean **105–114**, ≤ 0.02%
clipped; dawn 93, dusk 78 (twilight: both clamp slightly at `max_gain_ev`); `night_slant` EO
**26** (clamped at `max_gain_ev` −12.5); `night_slant_nvg` 67 at gain −8.8 (not clamped). 1080p
agrees within ±5. Calibration: scene medians are 2^10.9–2^12.4 in daylight, 2^9.4 dawn, 2^8.9
dusk, 2^6.6 night. The provisional run was dark (mean luma ~73) because EO's −1 EV
`render.exposure_compensation_ev` was added to the sensor AE target (median at 0.09); that bias
no longer applies to the GPU path (refinement 10), so EO `target_grey` 0.18 puts the median at
18% grey. EO `max_gain_ev` −12.5, NVG `highlight_percentile` 0.97 (0.99 held
night NVG at gain −10.7, luma 25). UE's pre-exposure equals 2^`AutoExposureBias` exactly under
manual exposure, so `UeExposureOffsetEv` stays 0. The legacy `/snapshot` is pre-CPU-sensor UE AE
output (daylight ~170, night 161: UE's AE brightens night), so it is not a like-for-like
exposure comparison.

Found and fixed during calibration: `sensor_gpu_ms` read 0 on every frame, because MetalRHI
resolves `RQT_AbsoluteTime` queries to the command buffer's end time truncated to whole seconds;
the graph is now timed through the GPU profiler (`RDG_EVENT_SCOPE_STAT` + `FGPUStat::OnTimingResults`,
`Camera/SensorGpuTimer.h`). The GPU-path stream is tagged BT.709 transfer (it applies the BT.709
OETF; legacy stays sRGB), verified with ffprobe.

Tests: 278 automation tests pass under NullRHI (276 + 2 with expected warnings; includes the 5
`CamSim.GPU.*`, skipped there; 274 before the final-review fixes); `scripts/run_gpu_tests.sh` 5/5 on Metal; bench/CIGI pytest 42/42;
KLV conformance export OK (misb.js 0.1.30); `scripts/ci_validate.sh --native` passes on both paths
(`CAMSIM_RENDER_SENSOR_PATH=gpu` and default/legacy: H.264 + KLV, no decode errors, 150/150 KLV
packets conformant in 5 s).

3B exit criteria (spec) after 3B.1, macOS:

| # | Criterion | 3B.1 |
| --- | --- | --- |
| 1 | `sensor_path=gpu` whole run; legacy CPU/SceneCapture/material paths deleted | ✅ gpu for every run; deletion is 3B.4 |
| 2 | 30 fps, 0 dropped per phase; frame p95 ≤ 3A.1 + 1 ms | ✅ 29.7–30.0 fps, 0 dropped; worst p95 delta +0.33 ms (1080p far origin) |
| 3 | `sensor_gpu_ms` p95 ≤ 4 ms at 1080p, all effects; controller < 0.2 ms; 1.5 B/px readback | ✅ 0.40 ms p95 (3B.1 core only; effects come in 3B.2/3B.3); controller not isolated (game thread +0.3–0.4 ms p50 vs legacy, an upper bound); NV12 readback ✅ |
| 4 | night EO < 40; daylight EO 90–170, < 1% clipped; cut converges in one frame | ✅ 26; 105–114, ≤ 0.02%. ◐ cut: the controller snaps on the first histogram rendered after the cut, which reaches it 1–3 frames later (stats readback latency), so the stream converges 1–3 frames after a cut, not in one; only the controller is tested (`CamSim.Sensor.Controller.CutAndModeSwitchSnap`) |
| 5 | NullRHI tests, `CamSim.GPU.Sensor.*` on Metal, `ci_validate --native` | ✅ |
| 6 | Visual review of EO/IR/NVG post-sensor shots | ✅ signed off 2026-09-27 (`scripts/bench/shots/macos/3b1/`) |
| 7 | Docs | ✅ this section, `docs/configuration.md`, CLAUDE.md |

Final-review fixes (2026-09-27): the sensor path is decided once by `UCamSimSubsystem` (the
capture and the encoder's transfer tag follow it) and falls back to legacy with an error when the
GPU path can't run (verified live: forced `gpu` at 1366×720 runs legacy, sRGB-tagged, no crash); a
hot reload can't change `capture_width`/`capture_height`/`render.sensor_path`; the NV12 width check
is a validation error only when the GPU path is wanted (legacy 1366×768 validates clean again);
built-in NVG defaults carry the red-heavy `signal_weight_*`; AE/AGC lag no longer depends on when
histograms arrive; legacy frame stats write `sensor_gain_ev`/`scene_median_log2` as `null`.

Refinements to the spec made while planning 3B.1:
1. UE exposure is manual but not constant: the controller sets UE's `AutoExposureBias` each tick
   so scene colour stays in fp16 range, and the shader divides out `View.OneOverPreExposure`.
2. The histogram is accumulated inside the Apply pass (no separate stats pass) and reaches the
   game thread through a mailbox fed by its own readback ring, not the NV12 ring.
3. `render.sensor_path: auto | gpu | legacy` (`CAMSIM_RENDER_SENSOR_PATH`, default `auto`); 3B.1's
   bench runs with `gpu`.
4. The path is fixed for the session; no hot switching (legacy is deleted in 3B.4).
5. `GET /snapshot/sensor` is a second route instead of `?stage=sensor`; in 3B.1's GPU path it
   equals `/snapshot` (both return the sensor image: the graph replaces the tonemapper, so there
   is no pre-sensor frame to grab).
6. Path reporting goes to `/metrics` (`camsim_sensor_path{path=...}`) and `camsim_health.json`;
   `/health` stays the liveness watchdog.
7. The encoder keeps YUV420P input: NV12 is de-interleaved on the CPU (no `sws_scale`).
   Codec-native NV12 / GPU textures are 3C.
8. Exposure keys are `min_gain_ev` / `max_gain_ev` (log2 of the gain on absolute scene-linear
   values); detector weights are per-mode `signal_weight_r/g/b`.
9. NV12 digital zoom is nearest-neighbour, as the BGRA path's.
10. `render.exposure_compensation_ev` applies only to UE's auto-exposure (legacy path); the GPU
    sensor AE target is `exposure.target_grey` alone. This overrides the spec's "keeps its meaning
    as a bias on the EO AE target": the −1 EV was a 3A correction for UE's own over-bright
    metering, meaningless for the sensor AE, and applying it silently halved the EO target.

Open points for the visual review: the NVG stream is monochrome (the green tint is applied only
to the viewport display; NV12 chroma is neutral for IR/NVG); IR at night is dark (mean luma 14,
22% black) because its AGC stretches the visible-light proxy between the 1st and 99th
percentiles and the bright clouds on the horizon set the top. Thermal radiance is Milestone 4.

Next: the 3B.2 plan (port the optics and detector effects so `auto` selects the GPU path with the
default config).

Terrain snap check (2026-09-27, Yosemite Valley, 3,200 m, 90° gimbal snaps and 18–60°/s pans):
- **Coarse tiles after a snap, fixed.** Off-screen tiles were kept at culled SSE 200 (FOV-scaled),
  so 30–80% of a newly snapped-to view stayed coarse for ~2 s. `culled_screen_space_error` now
  defaults to `maximum_screen_space_error` (16): 0% coarse, no settling, for ~60% more tiles
  (~930 MB; cache default raised to 2,048 MB) and no frame-time cost. SF bench vs 3A.1: pop-in
  orbit 0.16 → 0.001, slew 0.04 → 0, low pass 0.40 → 0.22; hitches > 66 ms low pass 3 → 11, far
  origin 6 → 12 (collision cooking for the extra tiles is the likely cause; hitch counts are noisy).
- **UE motion blur leaked in** with `optical_realism.enabled: false` (UE default on, 0.5): pans
  smeared. Now off unless optical realism enables it.
- **Encoder watchdog killed CamSim during a slow first tile load** (frames held by the terrain
  gate counted as a dead encoder). A stall now needs frames submitted and none written.
- Continuous pans at 18 and 60°/s kept tiles 100% loaded before and after the change.
- **The extra hitches were Cesium's LOD crossfade**, not collision cooking. With
  `use_lod_transitions` on, Cesium calls `UpdateFade` on every tile in the render set every frame
  (`Cesium3DTileset.cpp`, `updateTileFades`), and the render set includes the off-screen tiles
  now kept at full detail: game thread p50 3–4 → 8–12 ms (Insights: UpdateTileFades +2.0 ms,
  updateView +1.3 ms, ShowTilesToRender/SetCollisionEnabled +1.1 ms per frame). Crossfade length
  made no difference. `use_lod_transitions` now defaults to false: SF bench game thread p50
  2.4–3.2 ms, hitches > 66 ms orbit/slew/low pass/far origin 3/1/1/5 (3A.1: 4/0/3/6), frame p95
  33.8–35.0 ms. Cost: tile refinements switch instead of dithering in.

Carried into 3B.2 from the 3B.1 reviews (all before the first Linux/Vulkan run):
- Shader hardening: a NaN test that survives fast-math (`asuint` bit test, not `V != V`);
  `floor(x + 0.5)` instead of HLSL `round` in the NV12 packing (Y is at the 1 DN tolerance on
  Metal); clamp the bilinear scene and bloom UVs to their view rects.
- GPU tests: a size with partial thread groups (e.g. 68×34, or 1080p-shaped), and scene + bloom
  against the reference.
- Tests for the NV12 digital zoom (`ApplyDigitalZoomNv12`), the NV12 branches of
  `OfferSnapshot`/`SubmitFrameToEncoder`.
- Cleanup: the `SensorGraph.h` comments (`.a` is luma only for IR/NVG; doc block order around
  `IsSensorGraphSupported`), the stale `FSensorGpuTimer` comment/include, reset `LatestMs` to −1
  if the GPU profiler stops reporting.
- Docs: compare against `macos-m1pro-3a1-720p-exposure.json` too; note that the luma criteria are
  full-range RGB luma (the stream's limited-range Y reads ~38.7 on `night_slant`).
- Known, pre-existing: `FVideoEncoder` reads the subsystem's config while a hot reload
  move-assigns it (the capture size and sensor path are now pinned; other fields can still tear).
- ~~3B.4 deletes the unused BGRA render-target ring and colour readback pool on the GPU path.~~
  Done in 3B.2 (Task 2).

3B.2 progress (physical sensor model, `docs/superpowers/plans/2026-09-27-physical-sensor-3b2.md`):
- Task 1: NVG removed (EO and IR only).
- Task 2: the legacy CPU sensor path is gone — `FSensorPostProcess`/`IPixelPipeline`, the path
  selector and `render.sensor_path`, the 27A material path (`performance.gpu_sensor_*`), the HUD
  overlay (`Overlay/`, `overlay.*`), the drawn laser spot (`laser_designator.*`; DIS Designator
  PDUs are still tracked), the CPU precipitation overlay (`phase18.precipitation/rain_intensity/
  snow_intensity`, `randomization.randomize_weather/weather_probability`) and
  `render.exposure_compensation_ev` (UE auto-exposure, legacy only). The GPU sensor graph is the
  only path: `UCamSimSubsystem::IsSensorGraphAvailable()` is decided at startup; without it no
  frames are produced and `/ready` stays false. Streams are always tagged BT.709. The BGRA
  render-target ring, colour readback pool and backbuffer grab are gone (NV12 readback only).
- Task 3: primary view only — `render.view_source` / `EViewSource` / `CAMSIM_RENDER_VIEW_SOURCE`
  are gone (`scene_capture` reports as an unknown YAML key). Every `IsPrimary()` branch collapsed
  to its primary-view side (`FCamSimStreamingController::NumStreamingCameras` always 1,
  `CamSim::Geospatial::UseLodTransitions` follows `bUseLodTransitions` alone,
  `UCamSimSubsystem::CanRunSensorGraph` no longer checks the view source). The encoder takes NV12
  only: `FSensorFrame` is `{ TArray<uint8> Nv12 }` (no format enum), `FVideoEncoder` dropped
  `SwsCtx`/`RgbCompressedScratch`/`bSwsColorSpaceApplied` and the BGRA/sws_scale/IR-grayscale
  encode path, and `FMultiViewFrameSink::ApplyDigitalZoom` (BGRA) is gone (`ApplyDigitalZoomNv12`
  only). `Camera/CamSimPixelConvert.h` (BGRA readback conversion) and `swap_rb_readback` /
  `readback_format` / `CAMSIM_SWAP_RB_READBACK` / `CAMSIM_READBACK_FORMAT` are deleted
  (`readback_ready_polls` stays — the NV12 poll still uses it). `FrameGrabRequestQueue::DecidePoll`
  dropped its now-always-true `bNeedsGrab` parameter.
- Task 4: legacy sensor effect config keys and the quality-preset system removed —
  `FSensorModeConfig` trimmed to `SignalWeights`/`Exposure`/AGC only.
- Task 5: sensor-class presets — `CamSimSensorPresets::Apply` (`Sensor/SensorPresets.h/.cpp`)
  supplies `eo_hd_cmos`/`mwir_cooled`/`lwir_uncooled` optics/detector defaults;
  `FSensorModeConfig` gains `Preset`/`Seed`/`Optics`/`Detector` (`FSensorOpticsConfig`,
  `FSensorDetectorConfig`, `ESensorDetectorType`). `sensor_modes.<mode>.preset` (default
  `eo_hd_cmos`/`mwir_cooled`) selects a preset; `optics:`/`detector:` blocks override
  individual fields; `seed` sets the per-mode PCG noise stream. `exposure.max_gain_ev`
  renamed to `exposure.max_photon_gain_ev` (the photon/analog gain split lands in Task
  6; behaviour unchanged). `Validate()` reports an unknown preset, `full_well_e <= 0`,
  `adc_bits` outside `[8, 16]`, negative noise/`f_number`, defect fractions outside
  `[0, 0.01]`, and `|k1|`/`|k2| > 1`; an unknown preset keeps the mode's built-in preset
  defaults. `deploy/camsim_config.yaml` and `docs/configuration.md` document the
  preset table and override keys.
- Task 6: `FSensorFrameParams` carries the physical model's inputs (exposure: `PhotonGain`,
  `AnalogGain`, `DisplayGain`, `DisplayOffset`; detector fields, `Seed`, `FrameIndex`; optics
  fields, 0 until Task 10); `Gain`/`Offset` are gone. `FSensorController` splits the AE's total
  gain photon-first: the photon stage takes gain up to `max_photon_gain_ev`, analog gain the rest
  up to `detector.max_analog_gain_db` (none for a microbolometer; the IR presets set 0 dB). The AE
  state is per mode, and IR AGC now also runs the AE for its photon gain, stretching its band on
  the normalised signal (`DisplayGain`/`DisplayOffset`). `DarkE = dark_current_e_s /
  performance.render_frame_rate_hz`. The display path keeps 3B.1's output on
  `signal × PhotonGain × AnalogGain` until the detector lands (Tasks 7/9); the only visible change
  is EO at night, which analog gain now lifts past the old photon clamp.
- Task 7: CPU reference detector — `CamSimShaders/Public/SensorHash.h` (integer-exact PCG hash of
  (x, y, frame, seed, stream), 24-bit uniforms, Box-Muller Gaussian; to be mirrored in HLSL) and
  `CamSimSensorRef::DetectPixel`/`DetectImage`/`DisplayEo`/`DisplayIr`: photon detector (PRNU,
  shot, dark, DSNU, read noise, full-well clip, analog gain, ADC), microbolometer (temporal,
  pixel/column/row FPN), hot/dead defects. `CamSimSensorRef::Run` is now the full model
  (optics pass-through until Task 8); `RunDisplayOnly` keeps the 3B.1 path that the GPU graph
  still computes, and `CamSim.GPU.Sensor.*` compare against it until Task 9 ports the detector.
  Physics tests `CamSim.Sensor.Physics.*` (photon transfer, determinism, hash statistics,
  microbolometer FPN, defect fractions, clipping, analog gain).

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
