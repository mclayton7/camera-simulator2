# REALISM R6, checks 1 and 2: geo-registration and determinism (design)

Date: 2026-10-15. Status: approved in chat (2026-10-15: choice "A" for geo-registration, "settled stills" for
determinism; the user then delegated the remaining choices: "take your recommendations").

R6 (`REALISM.md` "R6 Realism validation") has four checks. This spec covers the two that need no real
footage: **geo-registration** (what CamSim draws is where its KLV says, and the datum conversion is right) and
**determinism** (same package, seed and CIGI input give the same frames). Appearance and sim-to-real wait on the
choice of a real UAV EO/IR dataset (REALISM open question 3).

## 1. Geo-registration: `scripts/georeg_check.py`

### 1.1 Markers

- **Mesh:** `entities/marker/plate.glb`, a 2 m × 2 m × 0.05 m box with its origin at the centre of the bottom face,
  written by `scripts/georeg/make_marker.py` (pure Python glTF writer, deterministic bytes, committed output; CC0,
  `entities/marker/LICENSE.md`).
- **Entity types** in `deploy/camsim_config.yaml`: `9001` (`class_name: georeg_marker`, scale 1) and `9002`
  (`class_name: georeg_marker`, scale 5: a 10 m plate for the 5 km views, where a 2 m plate is ~1 px tall).
- **Placement:** CIGI Entity Control (CIGI entity, not DIS: no clamp), sent once a second while a view is held. The
  plate's origin goes at the rendered surface height from a CIGI HOT request (opcode 24 → 103) at its lat/lon, plus
  0.01 m. On the slope site the plate is pitched and rolled to the local slope, from HOT at ±5 m along each axis.
- **Truth point:** the centre of the plate's top face. Its four top corners (in the plate's attitude) are the
  points the predictions project; a prediction is the centroid of the projected quadrilateral.

### 1.2 Sites and views (Camp Pendleton, `pendleton-r5veg`)

Three sites, chosen from the package data during implementation and committed in
`scripts/georeg/sites_pendleton.json` (lat, lon, kind, and why it qualifies):

- **flat:** bare ground, terrain slope < 2° over 50 m (3DEP 1 m), no buildings or trees within 30 m;
- **slope:** 10°–20° over 50 m, no trees within 20 m;
- **roof:** a flat-roofed Overture building at least 20 m × 20 m and at least 6 m tall, marker at the roof centre
  (HOT returns material 230 there).

Views per site (the camera heading differs per site, 30° / 120° / 250°, so an axis swap shows):

| View | Depression | Slant range | FOV (h) | Marker |
|---|---|---|---|---|
| `nadir` | 90° | 300 m | 20° | 9001 |
| `oblique` | 45° | 1 km | 10° | 9001 |
| `slant` | 10° | 5 km | 5° | 9002 |

Plus **`corner`**: the flat site, nadir at 300 m, FOV 20°, with the aim point offset so the marker sits at about
(0.85 W, 0.85 H). It exercises the field of view and the corner tags. Ten views in all.

The camera is posed over CIGI as the bench does (`scenario.host_datagram`): the platform at the computed
position, platform pitch and roll 0, heading toward the aim point; the gimbal pitch is minus the depression.

### 1.3 One view

1. Fly to the view (the bench host, 30 Hz), place the marker, wait for `/ready` terrain, then 10 s.
2. Capture 3 s: KLV with `node check.js capture` (raw TS) and COCO frames from `ml/camsim_coco.jsonl` (ground
   truth on, interval 1, segmentation on, depth off).
3. **Measured pixel:** the centroid of the marker's decoded RLE mask in pixel-edge coordinates (pixel (i, j) has
   its centre at (i + 0.5, j + 0.5)), the median over the window's frames. At least 30 frames with the marker
   are required, else the view fails ("marker not seen").
4. **KLV:** decoded with misb.js by a new `check.js decode <file.ts> <out.jsonl>` (one JSON object per ST 0601
   packet, tag number → decoded value; Tags 90/91 decoded from the raw bytes as `check.js` already does). The view
   is static, so every packet in the window must carry the same pose (within one LSB per tag); the tool uses their
   median.

### 1.4 Predictions and gates

From the KLV alone, as a downstream user would:

- **(a) KLV pinhole.** Camera position from Tags 13, 14 and 75 (sensor ellipsoid height: no geoid in the loop).
  Attitude: platform heading, pitch, roll (Tags 5, 6, 7; 90/91 when present) then sensor relative azimuth,
  elevation, roll (18, 19, 20), as ST 0601 composes them (NED, z-y-x). Pinhole with fx = (W/2) / tan(HFOV/2)
  (Tag 16), fy = (H/2) / tan(VFOV/2) (Tag 17), principal point (W/2, H/2), W × H from the mask's `size`.
  Implemented in Python from the ST 0601 definitions, independently of CamSim's C++.
- **(b) (a) plus the lens model** (`r_d = r_u (1 + k1 r_u² + k2 r_u⁴)`, `k1`/`k2` from the run's config). Equal
  to (a) when `k1 = k2 = 0` (the shipped presets). Informational.
- **(c) KLV corners.** The homography from the four corner points (Tags 82–89; ST 0601 order: upper left, upper
  right, lower right, lower left) to the image corners (0, 0), (W, 0), (W, H), (0, H), applied to the marker's
  lat/lon (in a local east-north plane at the frame centre, Tags 23/24). This is the footprint a video player
  draws.

**Gates** (error = Euclidean distance between the measured pixel and the prediction, in image pixels; one image
pixel is one GSD on the ground along that image axis):

| Gate | Views | Pass |
|---|---|---|
| G1 (a) | all ten | ≤ 1.0 px |
| G2 (c) | `flat/*`, `corner` | ≤ 1.0 px |

(c) on the slope, roof and 5 km views is informational: a four-corner homography assumes a plane, so relief
(a 6 m roof seen at 45°) and the 5 km footprint's curvature move points by more than a pixel by construction.

The report also gives, per view: the error in metres along the ground (the error vector times the ground size of
a pixel at the marker, from (a)'s camera); and the **KLV quantisation budget**: the pixel shift caused by one LSB
of each tag used, from finite differences of (a). A gate failure whose error is within that budget is quantisation,
said so in the report; the gate is not loosened either way.

### 1.5 Informational runs

- **`distorted`:** the `flat/nadir` and `corner` views again with `k1 = -0.05`. Expected: (b) passes, while (a) and
  (c) miss toward the corner, because KLV corners and box3d are pinhole (ROADMAP 3B.3). Recorded, not gated.
- **`orbit`:** a 30 s orbit at 1 km slant and 45° around the flat marker at the normal sim-time rate. KLV packets
  are matched to COCO frames by Tag 2 (Precision Time Stamp) = `timestamp_us`. Per-frame error of (a): median, p95,
  and the KLV-to-frame lag (in frames, -3..3) that minimises the median. This shows whether metadata and pixels
  stay in step when the camera moves.

### 1.6 Datum: `scripts/scene/tools/datum_check.py PKG`

No rendering. The reference data is committed: `scripts/scene/tools/data/ngs_pendleton.json` (written by
`scripts/scene/tools/fetch_ngs.py`, which records the NGS API URLs and the retrieval date):

- **46 survey marks** within about 1° of Pendleton (32.8–33.9 N, 118.0–116.9 W) with a NAD83(2011) ellipsoid height,
  an adjusted first- or second-order NAVD88 height and the published GEOID18 height (NGS datasheet API, `nde/bounds`);
- **CORS stations** in the same area with both their NAD83(2011) and their ITRF2014 positions at epoch 2010.0 (NGS
  coordinate files `coord_14/<id>_14.coord.txt`), antenna reference points.

Gates:

| Gate | What | Pass |
|---|---|---|
| D1 geoid | the `vgridshift` step of the manifest's `to_itrf_3d` (the vendored, hash-checked grid) at each mark vs its published GEOID18 height | ≤ 5 mm at every mark |
| D2 frame | the manifest's NAD83(2011) → ITRF2014 Helmert (the rest of `to_itrf_3d`, at the manifest's epoch 2010.0) applied to each CORS NAD83(2011) position vs its published ITRF2014 position | ≤ 1 cm horizontal and ≤ 1 cm vertical at every station |

Informational: the GEOID18 model fit at the marks (published ellipsoid height − NAVD88 height − our N; NGS's
own residual, median 1.35 cm, max 6.4 cm); and the lattice error of the package's vertical offset
(`datum.offset_lattice` at each terrain zoom vs the exact offset at the marks inside the bbox).

## 2. Determinism: `scripts/determinism_check.py`

### 2.1 What varies today (spike, 2026-10-15, pendleton-r5veg, RTX 5080)

Two launches, a still nadir pose, clock frozen (`CAMSIM_SIM_TIME_RATE=0`), ten `/snapshot` frames each, 30 s after
`/ready`: every frame has a distinct hash, but only ~2,400 of 921,600 pixels (0.26 %) differ between any two frames,
by at most 20 DN, scattered over the whole image, and run a vs run b differ the same way (mean |diff| 0.004 DN).
That is the sensor's temporal noise, keyed to the frame serial (`FSensorFrameParams::FrameIndex`).

### 2.2 Pinning

- **Sensor noise:** a new CVar `camsim.Sensor.FixedNoiseFrame` (int32, default -1 = off). When ≥ 0, the frame
  index the sensor graph keys its temporal noise with (photon shot, read, bolometer temporal) is that value, on the
  GPU graph and in `CamSimSensorRef` alike (they share `FSensorFrameParams`). Fixed-pattern noise and defects are
  already frame-independent. Test-only; set with `-ExecCmds`. Off, behaviour is unchanged bit for bit.
- **Clock:** `CAMSIM_SIM_TIME_RATE=0`, and the host sends the same date and time every frame (Celestial Sphere
  Control), so the sun, the waves and the thermal state are fixed.
- **Anything else** that still varies (for example TSR's jitter sequence) is found by the check, not assumed:
  the implementation pins it if it can be pinned in a test-only way; otherwise the report names it and the gate
  fails until it is solved.

### 2.3 The check

- **Input:** six poses at Pendleton, flown in this order in each run: `camp_nadir` (EO, buildings), `trees_oblique`
  (EO, vegetation), `slant_5km` (EO, far tier and outside buildings), `coast_waves` (EO, ocean, Beaufort 3),
  `camp_nadir_ir` and `trees_oblique_ir` (MWIR). The CIGI datagram for each pose is a pure function of the pose
  (frame counter aside); the report records each pose's datagram SHA-256 (frame counter zeroed), so both runs
  provably got the same input. That pose list is the recording.
- **Runs:** two launches (`a`, `b`) of the same package with the same pins.
- **Settling:** per pose, after `/ready` terrain, take a `/snapshot` every second until three in a row have the same
  pixel hash (SHA-256 of the decoded RGB), up to 90 s. The settled frame is kept.
- **Gates:**
  - **S1** every pose settles within 90 s in both runs;
  - **S2** for every pose, run a's settled hash equals run b's.
- **On a mismatch:** the report gives the differing pixel count, max |diff|, its bounding box and a diff PNG.
- **Across GPUs:** `--against DIR` compares this run's settled frames with another machine's (SSIM on luma, mean
  |diff|). Informational until a second machine has run it. The threshold is set from the first Mac vs Linux
  comparison and recorded as a ROADMAP item.

## 3. Outputs and docs

- **Each script** writes `report.json` and a printed table. Exit status: 0 when every gate passes, 1 on a gate
  failure, 2 on a usage or runtime error.
- **`georeg_check.py`** also writes an overlay per view: a crop around the marker with the mask outline, the measured
  centroid, and predictions (a), (b) and (c).
- **Docs:** a new guide, `docs/realism-validation.md`, covering how to run each check and the measured results
  (2026-10-15, Linux, RTX 5080).
- **REALISM.md R6:** results for checks 1 and 2.
- **ROADMAP.md:** the checks done; the cross-GPU threshold; any failure found.
- **`docs/testing.md`:** which check to run for which change.
- **CLAUDE.md:** the commands table, and a gotcha for the CVar.

## 4. Tests

Python units under `scripts/tests/` (no CamSim):

- **KLV camera model:** round trips; a known nadir and a known oblique projection; the ST 0601 rotation order
  checked against a hand-worked example.
- **Corner homography:** exact on a planar synthetic footprint.
- **Mask centroid:** the pixel-edge convention on a synthetic RLE.
- **Quantisation budget:** on a synthetic camera.
- **Settling loop and hash comparison:** with fake snapshots.
- **Marker writer:** deterministic bytes, a valid glTF.
- **Datum check:** the parsers on committed snippets of an NGS JSON and a CORS coord file. D1 and D2 against
  `pendleton-r5veg`'s manifest when the grid is in the fetch cache (skipped otherwise).

C++: `CamSim.Sensor.FixedNoiseFrame` checks that `FSensorFrameParams::FrameIndex` follows the CVar when set and the
serial when not. Run `check.js decode` against `mpegts.test.js`'s fixture.

## 5. Out of scope

- Appearance and sim-to-real (they wait on the dataset).
- Lockstep rendering.
- Fixing ROADMAP 3B.3, the pinhole KLV corners under distortion. The distorted run only measures it.
- Geo-registration of the imagery itself: R2's `road_registration.py` covers it.
