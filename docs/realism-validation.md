# Realism validation (REALISM R6): geo-registration and determinism

R6 asks how real CamSim's output is (`REALISM.md` "R6 Realism validation"). Two of its four checks need no real
footage and are built: **geo-registration** (what CamSim draws is where its KLV says, and the datum conversion is
right) and **determinism** (the same package, seed and CIGI input give the same frames). The other two, appearance
and sim-to-real, wait on the choice of a real UAV EO/IR dataset (REALISM open question 3).

Design: `docs/superpowers/specs/2026-10-15-r6-georeg-determinism-design.md`.

## Geo-registration: `scripts/georeg_check.py`

```bash
TMPDIR=/opt/mac/tmp uv run --project scripts/scene --with pycocotools python scripts/georeg_check.py \
    --scene .cache/scene-packages/pendleton-r5veg --out .cache/r6/georeg [--runs main,distorted,pan] \
    [--views flat/nadir,flat/corner] [--check-only]
```

**Markers.** A plate 2 m × 2 m × 5 cm (`entities/marker/plate.glb`, written by `scripts/georeg/make_marker.py`, CC0)
is entity type 9001; 9002 is the same plate at scale 5 (10 m) for the 5 km views. The check places it with CIGI Entity
Control on the rendered surface. The height comes from a CIGI HOT request (extended), plus 1 cm, and the plate is tilted
to the HOT normal. Its truth point is the centre of its top face.

**Sites and views.** `scripts/georeg/sites_pendleton.json` is chosen by `scripts/georeg/pick_sites.py` from the
package's land cover, 3DEP 1/3″ DEM and Overture snapshot. It has three sites:
- **flat:** the MCAS Camp Pendleton airfield apron, all-paved land cover, 0.3° slope;
- **slope:** grass at 15° with 0.13 m relief;
- **roof:** a 66 m × 140 m building, 8.4 m tall.

Each site is seen from three views: nadir at 300 m (FOV 20°), 45° at 1 km (FOV 10°), and 10° at 5 km (FOV 5°, with the
10 m plate). A tenth view looks straight down at the flat site with the marker near the frame corner, at (0.85 W, 0.85 H).

**Measurement.** For each view, the check records 3 s of KLV (`check.js capture`, then `check.js decode`) and COCO
masks.
- **Measured pixel:** the mask centroid, averaged over the clean frames. The mask is the plate's silhouette: its top
  face and its visible side faces. Every prediction below is therefore the centroid of the projected box's silhouette,
  not of its top face; that difference is 0.37 px at 10°. A clean frame has visibility ≥ 0.95 and its
  mask doesn't touch the frame edge; at least 30 are needed.
- **Why the mean:** one binary mask is quantised to whole pixels, about 0.3 px of error on a 10 px plate. The stencil is
  rendered under TSR's sub-pixel jitter, which changes every frame, so the mean converges on the true centroid.

**Predictions.** Predictions (a) to (c) are built from the KLV alone, in Python (`scripts/georeg_lib.py`), from the
ST 0601 definitions:
- **(a) KLV pinhole camera:**
  - position from Tags 13, 14 and 75 (ellipsoid height);
  - platform attitude from Tags 5, 6 and 7 (or 90/91), then the sensor's relative angles from Tags 18, 19 and 20, both
    composed z-y-x in NED;
  - fx from Tag 16 and fy from Tag 17.
- **(b)** (a) with the lens model.
- **(c)** the homography from the four corner points (Tags 82–89) to the frame's corners. The plate's footprint is
  mapped through it, plus the parallax of its 6 cm top as (a) sees it.
- **(a′)** the same camera built from the *commanded* CIGI pose, with no KLV in the loop. Comparing (a) with (a′)
  separates KLV quantisation from rendering error.

**Gates.**
- **G1:** (a) within 1 px on every view.
- **G2:** (c) within 1 px on the flat site's four views (nadir, oblique, 5 km and corner). A four-corner homography
  assumes a plane, so on the slope and roof views (c) is reported but not gated.
- **G3:** every KLV corner point (Tags 82–89) and the frame centre (Tags 23/24) is checked on every view. Placed at the
  rendered surface height there (a CIGI HOT request after the capture), it must project through the commanded camera
  onto its image corner or centre within 1 px. That tests both the bearing and the range along the ray: the property
  CamSim controls. G2 also depends on the footprint's relief.

The report gives each error in metres on the ground. It also gives the **KLV quantisation budget**: how far one least
significant bit of each tag moves the point, in pixels.

**Informational runs.**
- **`distorted`:** the flat nadir and corner views with the lens set to `k1 = -0.2`. The run uses a temporary copy of the
  config, through `run.sh`'s `CAMSIM_CONFIG_SRC`.
- **`pan`:** the flat site's oblique camera, with the gimbal yaw swept ±3° over 6 s, recorded for 20 s. The marker
  crosses the frame at up to about 13 px per frame. KLV packets are matched to COCO frames by Tag 2 = `timestamp_us`,
  and the error of (a) is computed at lags from −3 to +3 frames. The best lag shows whether the metadata and the pixels
  are in step. A first version orbited the marker instead; because the camera stayed aimed at it, the marker never
  moved in the frame, so it could not see a lag.

## Datum: `scripts/scene/tools/datum_check.py`

```bash
TMPDIR=/opt/mac/tmp uv run --project scripts/scene python scripts/scene/tools/datum_check.py PKG [--json out.json]
```

No rendering. The reference data is `scripts/scene/tools/data/ngs_pendleton.json`, written by `fetch_ngs.py` from NGS:
- **Survey marks** within about 1° of Pendleton, each with a NAD83(2011) ellipsoid height, an adjusted first- or
  second-order NAVD88 height and its published GEOID18 height.
- **CORS stations,** each with its NGS-published NAD83(2011) and ITRF2014 positions at epoch 2010.0.

**Gates.**
- **D1:** the manifest's own `vgridshift` step must match each mark's published GEOID18 height within 5 mm.
- **D2:** the rest of the manifest's `to_itrf_3d` pipeline, a 14-parameter Helmert, must take each CORS NAD83(2011)
  position to its published ITRF2014 position within 1 cm, both horizontally and vertically.

## Determinism: `scripts/determinism_check.py`

```bash
TMPDIR=/opt/mac/tmp uv run --project scripts/scene python scripts/determinism_check.py \
    --scene .cache/scene-packages/pendleton-r5veg --out .cache/r6/determinism [--against OTHER] [--check-only]
```

**Runs.** Two launches each fly six poses in the same order: four in EO (`camp_nadir`, `trees_oblique`, `slant_5km`,
`coast_waves`) and two in MWIR (`camp_nadir_ir`, `trees_oblique_ir`).

**Pins.**
- **Clock:** `CAMSIM_SIM_TIME_RATE=0`, and the host sends the same date and time every frame.
- **Exposure:** manual by default; `--auto-exposure` keeps the sensor AE and IR AGC on. The EO exposure is fixed at
  −14 EV, the IR AGC is off and the thermal exposure is manual, through a temporary config passed with `run.sh`'s
  `CAMSIM_CONFIG_SRC`. With the clock frozen, the AE and AGC would hold their first snap:
  `FSensorController::Smoothing` holds on a zero sim-time step.
- **UE's temporal sequences:** `r.Test.FreezeTemporalSequences 1` (Development builds) freezes TSR's jitter and the
  view frame index.
- **Sensor noise:** `-ExecCmds=camsim.Sensor.FixedNoiseFrame 0`. This is a test-only CVar (default `-1`, off). At 0 or
  above, the sensor's temporal noise (shot, read and bolometer noise) is keyed to that frame index instead of the frame
  serial.

**Settling.** At each pose, after `/ready` terrain, the check takes a `/snapshot` every second until three frames in a
row have the same pixel hash, for at most 90 s.

**Gates.**
- **S1:** every pose settles in both runs.
- **S2:** the settled frames of run a and run b are pixel-identical.

`--against` compares the settled frames with another machine's (SSIM on luma, mean |diff|). It is informational until
a cross-GPU threshold is set. `--clouds-off` (diagnostic) adds `r.VolumetricCloud 0`: the level's volumetric clouds
drift on UE engine time, not the sim clock (see the results).

## Results (2026-10-15, Linux, RTX 5080, `pendleton-r5veg`)

### Geo-registration

`georeg_check.py --runs main,distorted,pan`. **G1 PASS, G3 PASS, G2 FAIL** (as specified, explained below).

| View | Clean frames | (a) KLV px / m | (a′) commanded px | (c) corners px / m | Corner rays px | Ground px (m) | Budget px |
|---|---|---|---|---|---|---|---|
| flat/nadir | 111 | 0.06 / 0.00 | 0.06 | 0.04 / 0.00 | 0.06 | 0.083 | 0.10 |
| flat/oblique | 111 | 0.75 / 0.14 | 0.04 | **1.94** / 0.28 | 0.04 | 0.137 × 0.193 | 1.57 |
| flat/slant | 112 | 0.41 / 0.29 | 0.07 | **12.99** / 12.39 | 0.01 | 0.341 × 1.969 | 1.38 |
| slope/nadir | 111 | 0.03 / 0.00 | 0.04 | 0.02 / 0.00 | 0.04 | 0.083 | 0.10 |
| slope/oblique | 111 | 0.67 / 0.13 | 0.04 | 41.04 / 5.61 (info) | 0.03 | 0.137 × 0.193 | 1.57 |
| slope/slant | 112 | 0.21 / 0.41 | 0.02 | 53.13 / 54.75 (info) | 0.01 | 0.341 × 1.969 | 1.38 |
| roof/nadir | 111 | 0.05 / 0.00 | 0.07 | 0.06 / 0.01 | 0.03 | 0.083 | 0.10 |
| roof/oblique | 112 | 0.36 / 0.06 | 0.05 | 40.39 / 7.81 (info) | 0.04 | 0.137 × 0.193 | 1.57 |
| roof/slant | 113 | 0.76 / 0.98 | 0.06 | 35.93 / 24.08 (info) | 0.02 | 0.341 × 1.968 | 1.38 |
| flat/corner | 111 | 0.06 / 0.00 | 0.04 | 0.08 / 0.01 | 0.05 | 0.082 | 0.53 |

- **Pixels are where the KLV says (G1).** The worst error is 0.76 px. The commanded-pose camera (a′) is within 0.07 px
  on every view, so the renderer matches the CIGI pose to 0.07 px. The rest of (a)'s error is KLV quantisation:
  Tag 75 (ellipsoid height) has a 0.30 m step, up to 1.57 px at 45° from 1 km; Tag 5 (heading) has a 0.0055° step, up
  to 1.38 px at 5 km. (A first evaluation predicted the top face only and read a further 0.1–0.35 px as render error.
  The mask includes the visible side faces; the final review caught it.)
- **Corner and frame-centre tags are right (G3, ≤ 0.06 px).** At the rendered surface height there, every one projects
  through the commanded camera onto its image corner or centre, so both bearing and range are right.
- **G2 fails on `flat/oblique` (1.94 px) and `flat/slant` (13.0 px), a property of four-corner footprints.** The site
  is flat, but the 1 km and 5 km footprints are not. Their corner points land on terrain 1–14 m apart in height (the
  rays are right, G3), and a homography assumes one plane. At 45°, 0.5 m of relief is about 2 px. A video player drawing
  a footprint, or projecting a point, from Tags 82–89 alone carries this error. On the slope, roof and 5 km views it is
  tens of pixels (informational).
- **Distorted run (`k1 = -0.2`, informational).** `flat/corner`: (b) 0.25 px, but (a) 2.29 px and (c) 2.17 px. KLV
  corners and the ST 0601 camera are pinhole, and the image is not (ROADMAP 3B.3). At the frame centre all three agree
  (0.06 px).
- **Pan: metadata in step with pixels.** 596 frames, with the marker moving up to 11 px per frame. Error of (a) by
  KLV-to-frame lag (median / p95 px): −3: 19.75 / 28.85, −2: 13.10 / 19.31, −1: 6.52 / 9.84, **0: 0.83 / 1.22**,
  +1: 6.48 / 9.80, +2: 13.17 / 19.32, +3: 19.94 / 28.89. The best lag is 0: each KLV packet describes the pose of the
  frame it is stamped with.

**Bug found and fixed: near-nadir views rendered along UE's vertical.** In the first full run `flat/corner` failed G1 at
1.70 px. The cause was a session whose georeference origin was 2.9 km away. `UCameraComponent::GetCameraView` takes
`FQuat::Rotator()`, whose gimbal-lock branch snaps any pitch within about 0.08° of −90° to exactly −90°. A nadir sensor
looks down its local vertical, which is off UE's −Z by distance / Earth radius (0.026° at 2.9 km). So the primary view
rendered along −Z: 0.14 m off on the ground from 300 m. The error grows with height and with distance from the origin
(about 1.4 m from 1 km up, 9 km from the origin), and only for views within 0.08° of UE's vertical.

`ACamSimCamera::CalcCamera` and the ground-truth projection now convert through the rotation matrix
(`CamSimRender::ViewRotationFromQuat`, test `CamSim.Render.ViewRotationKeepsNearNadirTilt`). In the same session order,
`flat/nadir` went from 1.63 to 0.06 px and `flat/corner` from 1.54 to 0.10 px.

### Datum

`datum_check.py pendleton-r5veg` against 34 NGS marks and 11 CORS stations (data retrieved 2026-10-15):

| Gate | Result |
|---|---|
| **D1 geoid:** our GEOID18 step vs the published GEOID18 height | **PASS:** max 4.19 mm, median 0.97 mm (limit 5 mm). The largest differences are at marks north of 33.8°N, outside the package; this fits PROJ's bilinear interpolation versus NGS's biquadratic. |
| **D2 frame:** NAD83(2011) → ITRF2014 vs the CORS published positions | **PASS:** max 0.21 mm horizontally, 0.95 mm vertically (limit 1 cm). |
| GEOID18's own fit (information) | Median 1.32 cm, max 6.61 cm. This is the model's error against levelling, not ours. |
| Vertical-offset lattice vs exact offset (information) | 0.06 mm at z12, z14 and z16 (2 marks in the bbox). |

### Determinism

`determinism_check.py` (manual exposure, all pins). **S1 FAIL and S2 FAIL on all six poses.** Settled stills are not
pixel-identical: no pose stops changing within 90 s, and the two runs differ by a fraction of a DN on average.

| Pose | Within a run: last two frames differ (a / b) | Across runs: mean \|diff\| | Pixels differing | 99th percentile | Max |
|---|---|---|---|---|---|
| camp_nadir | 13,156 / 16,057 px | 0.25 DN | 23.6 % | 3 DN | 90 |
| trees_oblique | 228 / 295 px | 0.12 DN | 12.5 % | 2 DN | 50 |
| slant_5km | 4,680 / 4,432 px | 0.11 DN | 10.9 % | 2 DN | 63 |
| coast_waves | 320,648 / 269,259 px | 0.29 DN | 25.7 % | 3 DN | 155 |
| camp_nadir_ir | 52,461 / 52,507 px | 0.38 DN | 11.6 % | 9 DN | 71 |
| trees_oblique_ir | 250,671 / 250,143 px | 3.83 DN | 34.4 % | 60 DN | 105 |

The input is the same in both runs by construction: each pose's CIGI datagram is a pure function of the fixed pose list
(its digest is in `report.json`). The variation comes from these sources, each measured on the way:

1. **Sensor temporal noise**, keyed to the frame serial: 34,000 px per frame at `camp_nadir`. It is removed by
   `camsim.Sensor.FixedNoiseFrame`, a new test-only CVar.
2. **UE's per-frame stochastic sampling** (TSR jitter, and Lumen and shadow sampling keyed to the view frame index): in
   shadowed areas. It is mostly removed by `r.Test.FreezeTemporalSequences 1`, leaving about 1,200–3,500 px per frame
   at ≤ 10 DN.
3. **Lumen's surface-cache refresh**, which runs on its own counter: shadowed and indirectly lit areas keep changing
   1–10 DN per frame after 120 s. UE's switches either stop the cache (wrong lighting) or loop a subset of it; neither
   is deterministic.
4. **Ocean ripples on UE's engine time** (`M_Ocean`'s `TimeS`, deliberately visual-only), not the sim clock:
   `coast_waves` changes in about 300,000 px per frame with the clock frozen. *Corrected below (later the same day):
   most of that was the volumetric clouds and their reflection on the sea; the ripples now run on the sim clock.*
5. **The sensor AE and IR AGC.** With auto exposure, the frozen-clock runs held their first snap: `camp_nadir`
   averaged 248 of 255, and the two runs differed by a uniform 2–5 DN. Manual exposure removes this.
6. **IR on vegetation (`trees_oblique_ir`), not isolated:** 250,000 px change per frame with the AGC off, and the runs
   differ by 3.8 DN on average.

With the auto-exposure run (`--auto-exposure`, same pins), S1 and S2 also fail, with the exposure differences above.

#### Follow-up: ripples on the sim clock, and the clouds (2026-10-15, later)

The ripples now run on the sim clock (`FOceanWaves::RipplePhase` → `MPC_Ocean.RipplePhase`; `CamSim.Ocean.Waves.RipplePhase`).
Re-run with the same pins (`.cache/r6/determinism-ripples`), the cross-run differences did **not** drop (camp_nadir 0.41,
trees_oblique 0.56, slant_5km 0.69, coast_waves 3.77, camp_nadir_ir 1.15, trees_oblique_ir 5.76 DN mean): the frames at
`coast_waves` still changed in ~300,000 px a second on a frozen clock. A probe of consecutive frames there (clock
frozen, all pins) found the cause: **the level's volumetric clouds**, whose material scrolls on UE engine time, and their
reflection on the sea (the moving bright patches on the water are the cloud blobs). With `r.VolumetricCloud 0` the same
probe changed in **51 px** a second: the ripples are frozen with the clock.

`determinism_check.py --clouds-off` (`.cache/r6/determinism-noclouds`, all other pins as above). **S1 and S2 still FAIL**,
but the cross-run differences drop 10–100×:

| Pose | Clouds on: mean \|diff\| / p99 / > 8 DN | Clouds off: mean \|diff\| / p99 / > 8 DN | Clouds off: pixels differing, max |
|---|---|---|---|
| camp_nadir | 0.43 / 3 / 0.03 % | **0.049** / 2 / 0.02 % | 21,966 px, 109 |
| trees_oblique | 0.22 / 2 / 0.01 % | **0.016** / 0 / 0 % | 7,958 px, 59 |
| slant_5km | 0.20 / 2 / 0.02 % | **0.013** / 0 / 0 % | 6,462 px, 41 |
| coast_waves | 0.49 / 3 / 0.04 % | **0.005** / 0 / 0 % | 1,999 px, 23 |
| camp_nadir_ir | 0.38 / 9 / 1.03 % | **0.013** / 0 / 0.02 % | 5,457 px, 94 |
| trees_oblique_ir | 3.83 / 60 / 10.4 % | **0.27** / 6 / 0.71 % | 28,217 px, 105 |

(Clouds-on columns: the first run, `.cache/r6/determinism`; the pixel and max columns there count any difference.)
So the clouds were most of sources 4 and 6 above, and much of 3: what is left is a few thousand pixels per pose,
mostly in shadow and on vegetation (Lumen's surface cache, source 3), and 0.27 DN on IR over trees.

