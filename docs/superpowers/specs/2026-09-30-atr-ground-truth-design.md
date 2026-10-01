# Ground Truth for ATR: Tight Boxes, Oriented Boxes, Occlusion — Design

**Date:** 2026-09-30
**Status:** Approved in conversation (the user took every recommendation and delegated the rest)
**Scope:** sub-project 2 of "boats and trucks for ATR". Wakes, DR acceleration, articulation,
the depth map's separate scene capture, and semantic segmentation images (ROADMAP 4.5) are out
of scope.

## Intent

CamSim's COCO sidecar is training and scoring data for an ATR. Today every label is the 2D
union of the entity's projected world-axis-aligned box: loose for any rotated vehicle, and
full-size even when a pier, trees, terrain or a wave crest hides most of it. That is label noise
and the main weakness left in the boat/truck pipeline.

Success: every annotation's box fits the vehicle's **rendered** pixels in the **encoded** frame;
each annotation says how much of the vehicle is visible (occlusion) and how much is cut off by the
frame edge (truncation); oriented boxes and a mask come with it.

What the user chose:

- Oriented boxes: **both** a rotated 2D rectangle in the image (DOTA / YOLO-OBB style) and the
  projected 3D box (8 image corners + size + attitude).
- 2D boxes: **both** modal (visible pixels; the main `bbox` / `obb`) and amodal (whole
  silhouette, ignoring occluders; `bbox_amodal` / `obb_amodal`).
- Masks: COCO RLE `segmentation` (modal) per annotation; no per-frame instance PNG yet.
- Everything else: the recommendations below.

Assumptions: COCO JSONL stays the primary output and only gains fields (`bbox`, `area` change
meaning — tighter, mask area — but keep their COCO semantics); VOC gets the modal box plus
`occluded`. "Occluded" means anything rendered in front, water included (a hull below a crest is
hidden even where clear water would show it faintly). The hull below the water surface at the
boat is not part of the silhouette at all (§2, final-review ruling), so a boat on a flat sea is
fully visible. macOS/Metal is the verified platform; the
shader stays Vulkan-portable (no float atomics, no wave intrinsics).

## Current state (verified 2026-09-30)

- `FCamSimEntityManager::GetEntitySnapshot` (`Entity/CamSimEntityManager.cpp:349`) projects the
  8 corners of `GetActorBounds` (world AABB) through a pinhole matrix built in
  `UCamSimCaptureComponent::BuildGroundTruthSnapshot`. `bVisible` = in frustum and
  `WasRecentlyRendered`; `bTruncated` = a corner fell outside the image.
- The snapshot rides in the readback-ring slot with its frame; the background task hands it to
  `FGroundTruthCollector::WriteAnnotationFrame` → COCO (`camsim_coco.jsonl`) / VOC writers.
- The sensor graph replaces the tonemapper in the primary view (`FCamSimFrameGrabExtension`,
  `EPostProcessingPass::ReplacingTonemapper`) and reads back NV12 per requested frame into the
  slot's `FRHIGPUBufferReadback`. It sees the view's scene textures (scene depth, custom
  depth/stencil) for the frame actually encoded.
- Output pixels come from the scene view rect via the optics distortion resample (`k1`, `k2`,
  Newton-inverted in `CamSimSensor.usf` / `CamSimOptics::UndistortRadius`). Labels are pinhole
  today, so they drift from the image when `k1`/`k2` ≠ 0 (3B.2 carry-over).
- Default AA is FXAA at 100 % (no jitter). The TSR render-quality option sets AA 4 and may lower
  `r.ScreenPercentage`, so scene textures can be smaller than the output and jittered.
- Depth comes from a separate `SCS_SceneDepth` scene capture (unchanged here).

## Architecture

```
game thread                         render thread (sensor graph)                 task thread
───────────                         ────────────────────────────                 ───────────
entity spawn: custom depth on,      InstanceIdCS* (output pixel space):          FInstanceMaskAnalyzer*:
  stencil = per-entity slot*          src = distortion inverse (as SensorCS)       per stencil slot: modal / amodal
                                      amodal = CustomStencil(src)                  pixel counts + AABBs, hulls → OBBs
capture: snapshot + slot→entity*      visible = amodal ∧ CustomDepth ≈ SceneDepth  (rotating calipers), modal RLE
  + 3D box (local bounds, pose)*      → uint16/pixel (visible:8 | amodal:8)       + truncation from the 3D box
  + projected 3D corners*           readback into the ring slot*                 → FEntityAnnotationData → COCO / VOC
```
(\* = new)

### Units

| Unit | Where | Does | Depends on |
|---|---|---|---|
| `FStencilSlotAllocator` | `Entity/` | hands out stencil values 1–255 to live entities, frees on destroy | nothing |
| Instance-ID pass | `CamSimShaders` (`AddInstanceIdPass`), `Shaders/Private/CamSimInstanceId.usf` | output-space ID image from custom stencil + depths | scene textures, distortion params |
| `FInstanceMaskAnalyzer` | `GroundTruth/` | pure C++: ID image + slot table → per-entity mask stats, boxes, OBBs, RLE | nothing (unit-testable) |
| `CamSimGroundTruth::ProjectBox3D` | `GroundTruth/FEntityProjection` | oriented 3D box → 8 output-pixel corners (pinhole + forward distortion), truncation | `CamSimOptics` |
| `CamSimGroundTruth::MinAreaRect`, `EncodeCocoRle` | `GroundTruth/` | geometry / encoding helpers | nothing |

## 1. Tagging entities (game thread)

- `FStencilSlotAllocator` (owned by `FCamSimEntityManager`): values 1..255, lowest free first,
  released when the entity is destroyed or purged. A value is reused only after a full frame ring
  has passed (≥ 4 frames) since release, so an in-flight frame never maps a reused value to the
  wrong entity: the allocator keeps a `ReleasedAtFrame` per value.
- On spawn (and on a model swap), every `UMeshComponent` on the entity actor gets
  `SetRenderCustomDepth(true)` and `SetCustomDepthStencilValue(Slot)` — meshes only: particle
  systems (rotor wash, smoke, wakes) never join the vehicle's mask. Entities with no free
  value (more than 255 live) get custom depth off and fall back to the projected-box path (§5);
  logged once per session.
- `DefaultEngine.ini`: `r.CustomDepth=3` (custom depth with stencil). Cesium tiles and the ocean
  never write custom depth.
- Only when `ml_training.enabled` and `bounding_boxes` **and** the instance-ID pass is available
  (`UCamSimSubsystem::IsGroundTruthMaskAvailable`, decided once at startup with the sensor graph):
  with ground truth off (or the pass unavailable) nothing is tagged and the pass doesn't run.

## 2. Instance-ID pass (render thread)

`AddInstanceIdPass(GraphBuilder, Inputs)` in `CamSimShaders`, run by
`FCamSimFrameGrabExtension::RunSensor_RenderThread` **only for frames whose grab request asks for
it** (the request gains `bInstanceIds`; the capture component sets it on annotated frames —
`FrameIdx % annotation_interval_frames == 0`).

- Inputs: `SceneDepth`, `CustomDepth`, `CustomStencil` (SRV) from `Inputs.SceneTextures`, the
  scene view rect, the output size, and the same distortion parameters `SensorCS` uses
  (`K1`, `K2`, `NewtonIterations`, principal point, focal scale).
- One thread per **output** pixel: compute the source UV exactly as `SensorCS`'s distortion
  resample (shared function moved to `CamSimSensorCommon.ush` so both shaders call one
  implementation), map to the scene-texture pixel (`floor`, point sample; scene textures may be
  smaller than the output under TSR), read:
  - `amodal = CustomStencil` (0 = no entity),
  - `visible = amodal` if `CustomDepth` is not behind `SceneDepth` (reversed Z:
    `CustomDeviceZ >= SceneDeviceZ * (1 - 1e-4)` — a relative tolerance so the entity's own depth
    always passes), else 0.
- Output: `uint16` per pixel (`visible | amodal << 8`), two pixels per `uint32` in a structured
  buffer (W·H·2 bytes: 4.1 MB at 1080p). Out-of-source-rect pixels (distortion pulling from
  outside the view) write 0.
- Readback: a second `FRHIGPUBufferReadback` per ring slot, issued in the same
  `AddReadbackBufferPass` sequence as the NV12 copy; the slot completes when both have landed.
- Budget: ≤ 0.15 ms GPU at 1080p on an M1 Pro (one load of three textures per pixel).

Water: Single Layer Water writes scene depth, so a hull behind a crest fails the depth test
(verified on Metal by the acceptance run, §7).

Submerged-hull cut (final-review ruling I2, as implemented): the custom-depth silhouette includes
the hull below the waterline, which no camera above the water can see — without a cut a side-view
boat on a flat sea had visibility 0.83. So an amodal pixel whose custom-depth point lies below its
**entity's water plane** is dropped from amodal **unless it is visible** (visible ⇒ amodal always
holds). The water plane of each tagged entity is the tangent plane of the sea surface (EGM96 geoid
+ CIGI tide + the active waves: `FOceanSurface::SurfaceHeightM`, the surface boats are placed on) at
the entity's position, normal = local up; computed per capture on the game thread in UE world
doubles (`CamSimGroundTruth::ComputeSeaSurfacePlane`), carried in the grab request as
`stencil → plane`, and moved into translated world with the view's pre-view translation on the
render thread (LWC: floats only after translation); the shader holds 256 planes, one per stencil
(`(0, 0, 0, 1)` = no cut). It reconstructs the point from the texel centre in the view rect and the
custom device Z through an explicit `ClipToTranslatedWorld` (the view's inverse translated
view-projection; reversed Z). Crests between the camera and the boat are above the water at the
boat, so they still occlude. Ocean off or no sea surface: no cut. A land vehicle's plane is the sea
surface under it, far below unless the land is below sea level (which the ocean mesh floods
anyway); lakes get no cut.

Deviation from the ruling as first written (a still-water plane, geoid + tide only, at the camera's
frame centre): measured live, it cut the crest occlusion along with the submerged hull. A boat
hidden by a crest sits in the trough behind it, below still water, so its hidden hull was dropped
as "submerged": Beaufort 6 crest view, 2.1 % of annotations below visibility 0.9 (brief bar
≥ 10 %), amodal height 44 px in troughs vs ~100 px level. With the per-entity sea-surface plane:
45.1 % below 0.9, min 0.185, calm Beaufort 0 still 1.000.

Jitter: with TSR on, scene textures are jittered by up to ½ render pixel, so mask edges can be off
by ≤ 1 output pixel. Documented, not corrected.

## 3. Mask analysis (task thread) — `FInstanceMaskAnalyzer`

Input: the ID image (W, H, `TArrayView<const uint16>`), the frame's slot table
(`stencil value → index into the snapshot`), the min-visible threshold. One pass over the
image accumulates, per stencil value: modal and amodal pixel counts and AABBs, and per row the
leftmost / rightmost modal and amodal x (enough for the convex hull: the hull of a pixel set is
the hull of each row's extreme pixels). Then per entity:

- `bbox` (modal) = `[minx, miny, maxx - minx + 1, maxy - miny + 1]` in pixel-edge coordinates
  (a one-pixel mask is `[x, y, 1, 1]`). `bbox_amodal` likewise from the amodal mask.
- `obb` / `obb_amodal`: a rectangle around the convex hull of the row extremes' pixel
  **corners** (each pixel contributes its 4 corners, so a 1×1 mask gives 1×1), oriented by the
  **projected vehicle axis** (final-review ruling I3): when `box3d.corners_px` is valid, the axis
  runs from the rear-face centre (mean of corners 0, 1, 4, 5) to the front-face centre (mean of
  2, 3, 6, 7) in the image; if it is at least 0.25 × the amodal mask's longer AABB side, the
  rectangle is the hull projected on that axis and its normal (`CamSimMask::RectAlongAxis`), the
  same axis for `obb` and `obb_amodal`. Otherwise (head-on, strongly foreshortened, no valid
  corners) the minimum-area rectangle (rotating calipers). Min-area alone tilted side views of
  wedge-shaped hulls by 7–9° and turned crest-split fragments into diagonal slivers.
  Format `[cx, cy, w, h, angle_deg]`: `w` is the longer side, `angle_deg` ∈ [-90, 90) is the
  rotation of the `w` axis from image +x toward +y (clockwise on screen). Corners are
  `c ± (w/2)(cosθ, sinθ) ± (h/2)(−sinθ, cosθ)`.
- `area` = modal pixel count (COCO convention: mask area).
- `segmentation` = COCO compressed RLE of the modal mask (`{"size":[H,W],"counts":"…"}`,
  column-major, pycocotools' string encoding), built from the run lengths per column within the
  amodal x-range (pixels outside it are known 0).
- `visibility` = modal / amodal pixels (in-frame); `occlusion` is not written separately.
- Annotations with modal pixels < `ml_training.min_visible_pixels` (default 1) are dropped
  (fully hidden vehicles are not labelled).
- Cost: one scan of W·H uint16 (~1–2 ms at 1080p) + per-entity work proportional to its bbox.

## 4. 3D box and truncation (game thread snapshot, task-thread finish)

At capture, per entity (in the snapshot, after the existing cone-cull):

- **Box:** the entity's local bounds — the union, in the actor's local frame, of the bounds of
  the meshes §1 tags that are visible and have a mesh asset (`ACamSimEntity::GetGroundTruthLocalBox`;
  final-review ruling I1: particles, lights and empty mesh slots never widen it), so it is the
  model's own box rotated with the vehicle, not a world AABB. The projected fallback box uses
  the same box.
- `box3d`: `{"size_m":[L,W,H], "yaw_deg", "pitch_deg", "roll_deg", "corners_px":[[x,y]×8]}`.
  Size along body X (fwd), Y (right), Z (up). Attitude is the entity's geo pose (CIGI convention:
  heading from true north, as `GetGeoPose`). Corner order: bottom face then top, each
  rear-left, rear-right, front-right, front-left (body frame). `corners_px` is `null` when any
  corner is behind the near plane.
- **Projection:** pinhole through the existing view-projection matrix, then the forward optics
  distortion `r_d = r_u (1 + k1 r_u² + k2 r_u⁴)` about the principal point (the inverse of
  what the shader undoes), so 3D corners land on the distorted image like the masks do.
- **Truncation** = 1 − area(projected 3D-box hull ∩ image) / area(projected 3D-box hull), via
  Sutherland–Hodgman clipping of the 2D hull of the 8 corners against the image rect (straight
  hull edges: under strong distortion this is an approximation). With a corner behind the near
  plane, `truncation` is omitted and `truncated` = 1 if the amodal mask touches the image edge —
  rare at sensor ranges. Otherwise `truncated` (int) stays, = `truncation > 0.01`.
- `visibility` (§3) is measured inside the frame only; truncation covers the part outside it, so
  the two are independent.
- The world-AABB `ScreenBBox` path stays as the fallback (§5) and as the frustum pre-filter.

## 5. Fallbacks

- Entity without a stencil value (> 255 live) or a frame without an ID image (readback failed):
  the old projected box is written with `"mask_source":"projection"` and no `visibility`,
  `segmentation`, `obb`, amodal fields. Normal annotations carry `"mask_source":"render"`.
- `bVisible` false (outside frustum) → never written, as today.

## 6. Output changes

COCO line per annotation (new or changed in **bold**):

```json
{"entity_id":1,"source":"dis","source_id":"1.1.1","category":{"id":2001,"name":"truck"},
 "bbox":[x,y,w,h], "area":N, "iscrowd":0, "truncated":0,
 "truncation":0.0, "visibility":0.83, "mask_source":"render",
 "bbox_amodal":[x,y,w,h], "obb":[cx,cy,w,h,a], "obb_amodal":[cx,cy,w,h,a],
 "segmentation":{"size":[H,W],"counts":"..."},
 "box3d":{"size_m":[L,W,H],"yaw_deg":..,"pitch_deg":..,"roll_deg":..,"corners_px":[[x,y],...]},
 "geo":{...}}
```

`bbox`, `area` change meaning (tight, mask area); all other fields are additions. VOC: modal
`bndbox`, `<truncated>` = `truncated`, `<occluded>` = `visibility < 0.95`.

Config (`ml_training:`): `min_visible_pixels` (default 1, `CAMSIM_ML_MIN_VISIBLE_PIXELS`),
`segmentation` (default true, `CAMSIM_ML_SEGMENTATION_ENABLED`; off drops the RLE, which is the
bulk of the line). Documented in `docs/configuration.md` and `deploy/camsim_config.yaml`.

`FEntityAnnotationData` gains the stencil value, the 3D box fields, and the analysis results
(filled on the task thread before the writers run).

## 7. Testing and acceptance

### Automation (NullRHI, `CamSim.GroundTruth.*`)

- `MinAreaRect`: axis rect, 30°-rotated rect (recovered to 0.5 px / 0.5°), single pixel, line,
  angle-range convention.
- `EncodeCocoRle`: known masks against strings produced by pycocotools (fixtures recorded in the
  test, with the Python snippet that made them in a comment).
- `FInstanceMaskAnalyzer`: synthetic ID images — two entities, one partly hidden (visibility
  exact), modal ⊂ amodal boxes, min-visible drop, unknown stencil value ignored, empty image.
- `ProjectBox3D`: centred box (no truncation), box half off the right edge (truncation ≈ 0.5),
  corner behind the camera (`null`), distortion k1 ≠ 0 moves corners outward as expected and
  round-trips with `UndistortRadius`.
- `FStencilSlotAllocator`: lowest-free order, exhaustion, delayed reuse.
- COCO / VOC writers: new fields present and well-formed JSON / XML; fallback annotation shape.

### GPU (Metal, `CamSim.GPU.GroundTruth.*`)

- `InstanceId.Synthetic`: run `AddInstanceIdPass` on synthetic stencil / depth textures —
  visible vs occluded pixels, out-of-rect zeros, packing.
- `InstanceId.DistortionMatchesSensor`: with k1, k2 ≠ 0, a single stencil texel lands on the same
  output pixel as `SensorCS` maps a single bright scene texel to.

### End-to-end acceptance — `scripts/gt_occlusion_check.py` (recorded in ROADMAP)

DIS truck + boat as in `dis_vehicle_check.py`, ground truth on; COCO checked frame by frame and
overlay PNGs (modal bbox, OBB, 3D box, mask tint) written for a human:

1. Nadir truck and boat: visibility ≥ 0.95, truncation 0, modal bbox area ≤ the old projected
   box area, OBB angle within 10° of the vehicle's heading mapped into the image.
2. Edge framing (vehicle centred on the frame's right edge): 0.3 ≤ truncation ≤ 0.7; modal bbox
   touches x = W.
3. Boat at Beaufort 6, grazing view (~3° depression, ~300 m): some frames with visibility < 0.9
   (crests hide the hull).
4. Truck behind terrain / trees: a low view across the Presidio loop; frames with visibility < 0.9
   reported (best effort — depends on the tiles; the overlays are the evidence).
5. Every `segmentation` decodes with pycocotools and its area equals `area`.
6. Frame-time impact: median frame time with ground truth on vs. off differs by < 2 ms.

## Documentation

- `docs/ground-truth.md` (new): every COCO field, conventions (OBB angle, corner order,
  RLE), visibility semantics and the vehicle-on-vehicle limitation (two vehicles' amodal masks
  still hide each other: custom depth draws only the tagged vehicles, in one pass), TSR edge error.
- ROADMAP: new "2.7 Ground truth for ATR" section (done/acceptance), mark the sub-project 2
  carry-over in 2.5 done, and the bbox part of the 3B.2 "distortion-aware ground truth" item
  (depth and KLV corners stay pinhole).
- CLAUDE.md: a Gotchas line (stencil values, `r.CustomDepth=3`, the pass runs only on annotated
  frames).

## Risks

- **Single Layer Water and scene depth** (§2): if SLW doesn't write depth on Metal, wave
  occlusion fails; detected by acceptance item 3 before anything else depends on it.
- **Custom stencil SRV on Metal**: reading the stencil view of the custom depth target from a
  compute shader; if unavailable in the post-process inputs, write the stencil into a colour
  target by a small pixel pass first.
- **Readback bandwidth**: 4 MB per annotated frame on top of 3 MB NV12. Unified memory on Apple;
  watch frame times (acceptance 6) and drop to annotating every Nth frame if needed.
- **255 entities**: fine for current scenarios; the fallback keeps output valid beyond that.
