# R6 geo-registration and determinism Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Two acceptance checks (geo-registration with markers + a datum check; determinism of settled stills)
and the one C++ switch they need, measured on Linux / RTX 5080 and recorded.

**Architecture:** Pure Python libraries hold the geometry (ST 0601 camera, corner homography, mask centroid,
quantisation budget) and the comparison logic, unit-tested without CamSim; thin runner scripts drive CamSim through
the existing bench host (`scripts/bench/run_bench.py`, `scripts/scene/tools/camsim_session.py`). KLV is decoded by
misb.js (`check.js decode`). One test-only CVar pins the sensor's temporal noise.

**Tech Stack:** Python 3 (numpy, pyproj, pillow, pycocotools), Node (misb.js 0.1.30), UE 5.8 C++.

**Spec:** `docs/superpowers/specs/2026-10-15-r6-georeg-determinism-design.md`

## Global Constraints

- Gates are not loosened to pass; a fail is recorded and explained.
- `TMPDIR=/opt/mac/tmp` on every command; one CamSim at a time; nothing CPU/GPU heavy during a CamSim run.
- `run.sh` in the foreground or under `set -o pipefail`.
- Altitudes are WGS-84 ellipsoid heights everywhere (KLV Tag 75 is HAE; Tag 15 is MSL and is not used).
- Pixel coordinates: pixel-edge convention (pixel (i, j) covers [i, i+1) × [j, j+1)), as `docs/ground-truth.md`.
- Commit trailer: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_01TPvVBPPLAKNsjK5cEiaGJT`.

## Review Focus

1. ST 0601 rotation order and sign conventions (platform z-y-x then sensor relative z-y-x; elevation negative
   down): an axis or sign slip shows as a large error on the oblique views only. Pinned by a hand-worked
   oblique test in Task 3.
2. Tag 16/17 FOV vs the rendered FOV when the aspect is not 16:9 — the check uses Tag 17 for fy, not Tag 16 × H/W.
3. A marker partly outside the frame or hidden (roof edge, trees): the centroid is biased. The runner requires
   the mask not to touch the image edge and `visibility ≥ 0.95`, else the view fails "marker not clean".
4. Settling never converging (e.g. vegetation dissolve, ocean) must fail S1 with the pose named, not hang.
5. The CVar off must be bit-for-bit the old behaviour (FrameIndex = Serial): pinned by Task 1's test.

---

### Task 1: `camsim.Sensor.FixedNoiseFrame`

**Files:** Modify `Source/CamSimTest/Sensor/SensorController.h` (input field), `SensorController.cpp:162`,
`Camera/CamSimCaptureComponent.cpp` (CVar, `In.FixedNoiseFrame`); Test `Tests/SensorControllerTest.cpp`.

**Interfaces:** Produces `FSensorControllerInput::FixedNoiseFrame` (int64, -1 off) and the CVar
`camsim.Sensor.FixedNoiseFrame` (int32, default -1).

- [ ] Test `CamSim.Sensor.Controller.FixedNoiseFrame`: with `FixedNoiseFrame = -1`, `P.FrameIndex == Serial` for
  serials 1 and 2; with 7, `P.FrameIndex == 7` for both and `P.Serial` still the serial.
- [ ] Run it (headless NullRHI, `RunTests CamSim.Sensor.Controller`) → fails to compile (no field).
- [ ] Add the field; `P.FrameIndex = In.FixedNoiseFrame >= 0 ? uint32(In.FixedNoiseFrame) : In.Serial;`;
  the CVar in the capture component sets `In.FixedNoiseFrame`.
- [ ] Build (`run.sh --build-only`), run `CamSim.Sensor` → all pass.
- [ ] Commit `feat(sensor): camsim.Sensor.FixedNoiseFrame pins the temporal noise frame (R6 determinism)`.

### Task 2: `check.js decode`

**Files:** Modify `scripts/klv_conformance/check.js`; Test `scripts/klv_conformance/check.test.js` (new).

**Interfaces:** `node check.js decode <file.ts|klv.bin> <out.jsonl>`: one line per ST 0601 packet,
`{"tags": {"<n>": value, ...}}`, misb.js values (Tags 90/91 from the raw bytes); exit 2 on IO error.
Exported `decodePackets(buf) -> [{tags}]` for the test.

- [ ] Test: a packet built with misb.js's encoder (or a committed byte fixture from
  `CamSim.KlvConformance.ExportPackets` if the encoder can't) decodes to the expected tag values incl. 13, 14,
  75, 82.
- [ ] Implement; `npm test` passes; commit `feat(klv): check.js decode writes decoded ST 0601 packets as JSONL`.

### Task 3: geometry library `scripts/georeg/klv_camera.py`

**Interfaces (Produces):**
- `Camera.from_klv(tags: dict[int, float], width: int, height: int) -> Camera`
- `Camera.project(lat, lon, h) -> (x, y)` pixel-edge, pinhole; `Camera.project_distorted(lat, lon, h, k1, k2)`
- `Camera.ground_pixel_size(lat, lon, h) -> (mx, my)` metres per pixel along image x / y at that point
- `corner_homography(tags, width, height) -> callable(lat, lon) -> (x, y)`
- `mask_centroid(rle: dict) -> (x, y)` pixel-edge (pycocotools decode)
- `quantisation_budget(tags, width, height, point) -> dict[int, float]` px shift per tag LSB
- `LSB: dict[int, float]` per ST 0601 tag

- [ ] Tests (`scripts/tests/test_georeg_lib.py`): nadir camera over a point projects it to (W/2, H/2); a point
  100 m north at heading 0 nadir projects up the image by 100 / GSD; an oblique (heading 90, depression 45)
  hand-worked case; project/unproject round trip; distortion with k1 = 0 equals pinhole; homography exact on a
  synthetic planar footprint; RLE centroid of a 2 × 2 block at (10, 20) is (11, 21); budget of Tag 5 ≈ the
  analytic shift at range.
- [ ] Implement with pyproj (ECEF) and numpy; tests pass; commit
  `feat(georeg): ST 0601 camera model, corner homography and mask centroid`.

### Task 4: marker mesh and entity types

**Files:** Create `scripts/georeg/make_marker.py`, `entities/marker/plate.glb`, `entities/marker/LICENSE.md`;
modify `deploy/camsim_config.yaml` (types 9001, 9002), `docs/configuration.md` (entity types list).

- [ ] Test: `make_marker.build()` bytes are identical across two calls; GLB header valid; the POSITION accessor's
  min/max are (-1, -1, 0)…(1, 1, 0.05) in glTF axes as the config's rotation expects (origin at the bottom centre).
- [ ] Implement; write the GLB; add the types (`class_name: georeg_marker`; 9002 `scale: 5.0`); commit.

### Task 5: sites

**Files:** Create `scripts/georeg/pick_sites.py`, `scripts/georeg/sites_pendleton.json`.

- [ ] `pick_sites.py PKG` searches the package (3DEP 1 m terrain via the package's source cache, Overture snapshot,
  vegetation mix / land cover) for the three site kinds of spec §1.2 near the camp, prints candidates with their
  metrics; the chosen three are written to the JSON with the metrics as `why`.
- [ ] Unit test the slope/flatness metric on a synthetic plane; commit.

### Task 6: `scripts/georeg_check.py`

**Interfaces:** Consumes Tasks 2–5. `views(sites) -> list[View]` (pure: camera pose, aim, marker type), `run(...)`,
`evaluate(view, coco_frames, klv_packets, k1, k2) -> dict`, `gates(results) -> dict`.

- [ ] Tests: `views()` geometry (camera at the slant range and depression from the marker, heading toward it;
  corner view offsets the aim so the marker lands near (0.85 W, 0.85 H) under the pinhole model);
  `evaluate()` on synthetic COCO + KLV built from a known `Camera` gives 0 error; gate table logic (G2 only on
  flat/corner; marker not clean fails).
- [ ] Runner: session per run (`main`, `distorted`, `orbit`), per view: marker via Entity Control at 1 Hz with HOT
  height + normal, `wait_terrain`, 10 s, 3 s capture (check.js capture + COCO tail), decode, evaluate, overlays,
  `report.json`, exit code.
- [ ] Commit.

### Task 7: datum check

**Files:** Create `scripts/scene/tools/fetch_ngs.py`, `scripts/scene/tools/datum_check.py`,
`scripts/scene/tools/data/ngs_pendleton.json`; Test `scripts/scene/tests/test_datum_check.py`.

- [ ] Tests: parse a committed NDE JSON snippet and a CORS coord-file snippet; D1 and D2 against
  `pendleton-r5veg`'s manifest and the cached GEOID18 grid (skip when absent); a deliberately wrong grid sign
  fails D1.
- [ ] Implement; fetch and commit the data; run on the package; commit.

### Task 8: `scripts/determinism_check.py`

**Interfaces:** `POSES`, `datagram_digest(pose) -> str`, `settle(fetch, max_s, need=3) -> (hash, png, n)`,
`compare(a_dir, b_dir) -> dict`, `--against DIR` SSIM/mean-diff.

- [ ] Tests: settle with a fake fetch sequence (converges after noise; never converges → timeout result);
  compare on equal and on differing synthetic frames (count, max, bbox); digest ignores the frame counter.
- [ ] Runner (two launches with `-ExecCmds=camsim.Sensor.FixedNoiseFrame 0`, `CAMSIM_SIM_TIME_RATE=0`); commit.

### Task 9: measure

- [ ] `georeg_check.py --runs main,distorted,orbit` on `pendleton-r5veg`; `datum_check.py`; `determinism_check.py`.
- [ ] Every failure investigated (superpowers:systematic-debugging) and either fixed with a failing test first or
  recorded with its cause.

### Task 10: docs

- [ ] `docs/realism-validation.md`, REALISM R6, ROADMAP, `docs/testing.md`, CLAUDE.md (commands, CVar gotcha),
  `docs/configuration.md` (CVar). Commit.
