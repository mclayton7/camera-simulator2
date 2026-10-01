# Ground truth for ATR

CamSim writes per-frame labels for every tagged entity (DIS / CIGI / scenario vehicles) next to
the video: COCO JSON lines, optionally Pascal VOC XML and depth PNGs. Since ROADMAP 2.7 the 2D
labels are **measured from the rendered frame**, not projected from the model's bounds: the
box fits the vehicle's visible pixels in the encoded image (after the sensor's lens
distortion), and each annotation says how much of the vehicle is hidden (occlusion) and how
much is cut off by the frame edge (truncation). An oriented 2D box, the projected 3D box and a
COCO RLE mask come with it.

Configuration: `ml_training:` in [`configuration.md`](configuration.md#ml-training-data-ml_training).
Acceptance and numbers: ROADMAP 2.7, `scripts/gt_occlusion_check.py`.

## How it works

1. **Tagging (game thread).** With `ml_training.enabled` and `bounding_boxes` on, every
   primitive of every entity actor renders custom depth with a stencil value 1..255
   (`FStencilSlotAllocator`, lowest free first; a released value is reused only 4 frames
   later, so a frame still in the readback ring never maps it to the wrong entity).
   `r.CustomDepth=3` (custom depth with stencil). Cesium tiles and the ocean never write
   custom depth. The capture snapshot carries the stencil → entity table and each entity's
   oriented 3D box.
2. **Instance-ID pass (render thread).** On annotated frames only (`frame % annotation_interval_frames == 0`),
   `InstanceIdCS` runs in the sensor graph: for each **output** pixel it finds the source
   position exactly as `SensorCS`'s distortion resample does (they share `UndistortScale`),
   reads the custom stencil (the entity's whole silhouette: *amodal*) and compares custom depth
   with scene depth (the entity is *visible* there when nothing is nearer). The result,
   `visible | amodal << 8` per pixel, is read back with the frame's NV12.
3. **Mask analysis (task thread).** `FInstanceMaskAnalyzer` scans the ID image once: per entity
   the modal (visible) and amodal pixel counts, boxes, convex hulls → minimum-area rectangles,
   and the modal mask as COCO RLE. The 3D box is projected (pinhole + the forward lens
   distortion) for `box3d.corners_px` and `truncation`.

With ground truth off nothing is tagged and the pass doesn't run.

## Output files

Under `ml_training.output_dir`:

| File | When | Content |
|---|---|---|
| `camsim_coco.jsonl` | `coco_export` (default on) | One JSON object per annotated frame (below). |
| `annotations/frame_NNNNNNNN.xml` | `voc_export` (default off) | Pascal VOC, one file per annotated frame. |
| `depth/depth_NNNNNNNN.png` | `depth_map` (default on) | 16-bit depth, 0 m → 0, `depth_far_plane_m` → 65535. |

`NNNNNNNN` is the frame index (the COCO `frame_id`).

## COCO record

```json
{"frame_id": 873, "timestamp_us": 1782068442265248,
 "platform": {"lat": 37.79734248, "lon": -122.45971792, "alt_m": 202.0,
              "yaw_deg": 180.0, "pitch_deg": 0.0, "roll_deg": -0.0},
 "annotations": [{
   "entity_id": 1, "source": "dis", "source_id": "1.1.1",
   "category": {"id": 2001, "name": "truck"},
   "bbox": [541.0, 253.0, 181.0, 123.0], "area": 15262.0, "iscrowd": 0, "truncated": 0,
   "mask_source": "render", "visibility": 1.0,
   "bbox_amodal": [541.0, 253.0, 181.0, 123.0],
   "obb": [631.26, 316.58, 189.45, 105.2, -7.79],
   "obb_amodal": [631.26, 316.58, 189.45, 105.2, -7.79],
   "segmentation": {"size": [720, 1280], "counts": "ffl;>3F^e0[1_O3M1..."},
   "truncation": 0.0,
   "box3d": {"size_m": [7.569, 3.057, 3.275], "yaw_deg": 90.0, "pitch_deg": -12.23, "roll_deg": 3.411,
             "corners_px": [[728.6, 359.4], [728.8, 304.9], [547.7, 330.4], [545.9, 384.8],
                            [712.1, 304.2], [712.5, 249.7], [529.7, 275.7], [527.8, 330.2]]},
   "geo": {"lat": 37.79590095, "lon": -122.45972204, "alt_m": 13.065}}]}
```

Frame fields: `frame_id` (frame index), `timestamp_us` (sim time, µs since the Unix epoch, UTC),
`platform` (the sensor platform's geodetic pose: WGS-84 degrees, ellipsoid metres, CIGI
yaw/pitch/roll in degrees; the gimbal is not included).

### Image coordinates

Pixels are addressed by their top-left corner: pixel `(i, j)` covers `[i, i+1) × [j, j+1)`,
`x` to the right, `y` down, origin at the image's top-left corner. Every 2D quantity below is in
these **pixel-edge** coordinates of the encoded (distorted) output image, whose size is the
segmentation's `size` = `[H, W]`.

### Annotation fields

| Field | Units / format | Meaning |
|---|---|---|
| `entity_id` | uint32 | Session-unique, stable for the entity's lifetime, never reused. |
| `source`, `source_id` | string | `dis` / `cigi` / `scenario`; DIS `site.application.entity`, else the entity ID. |
| `category` | `{id, name}` | CamSim type ID and `class_name` (`truck`, `boat`, …). |
| `bbox` | `[x, y, w, h]` px | **Modal** box: the visible pixels. `x, y` = the top-left covered pixel, `w, h` = covered columns / rows, so a one-pixel mask is `[x, y, 1, 1]` and the box spans `[x, x+w) × [y, y+h)`. |
| `area` | px | Modal pixel count (the mask area, COCO convention), not `w·h`. |
| `iscrowd` | 0 | Always 0. |
| `mask_source` | `render` / `projection` | `render`: everything here measured from the frame. `projection`: the fallback (see Limitations) — `bbox` is the projected world-aligned bounds and `area = w·h`; `visibility`, `segmentation`, `obb*`, `bbox_amodal` are absent. |
| `visibility` | 0..1 | Visible / silhouette pixels **inside the frame**: modal count / amodal count. 1 = nothing in front of it. Anything rendered nearer counts as an occluder, water included. |
| `bbox_amodal` | `[x, y, w, h]` px | Box of the whole silhouette inside the frame, ignoring occluders (the custom-stencil coverage). |
| `obb`, `obb_amodal` | `[cx, cy, w, h, angle_deg]` px, deg | Minimum-area rectangle around the modal / amodal mask (rotating calipers over the pixel corners, so a 1×1 mask gives `w = h = 1`). `w ≥ h`; `angle_deg` is the rotation of the `w` axis from image +x toward +y (clockwise on screen), in [-90, 90) (a square: [-45, 45)). Corners: `c ± (w/2)(cos θ, sin θ) ± (h/2)(−sin θ, cos θ)`. |
| `segmentation` | COCO compressed RLE | Modal mask, `{"size": [H, W], "counts": "..."}`, column-major, pycocotools' string encoding. Absent with `ml_training.segmentation: false` (it is the bulk of each line). |
| `truncation` | 0..1 | Fraction of the projected 3D box's image outline outside the frame: 1 − area(hull of the 8 projected corners ∩ image) / area(hull). Absent when unknown (a corner behind the camera). Independent of `visibility`, which only counts pixels inside the frame. |
| `truncated` | 0 / 1 | `truncation > 0.01`; when `truncation` is unknown, 1 if the amodal mask touches the image edge. |
| `box3d.size_m` | `[L, W, H]` m | The model's own bounds (union of its primitives, actor-local) along body X (forward), Y (right), Z (up). |
| `box3d.yaw_deg`, `pitch_deg`, `roll_deg` | deg | The entity's attitude, CIGI convention: yaw = heading from true north, clockwise; pitch nose-up positive; roll right-wing-down positive. |
| `box3d.corners_px` | 8 × `[x, y]` px, or `null` | The box's corners projected into the image (pinhole + the forward lens distortion `r_d = r_u (1 + k1 r_u² + k2 r_u⁴)`), so they land on the distorted image as the masks do. Order: bottom face, then top face, each **rear-left, rear-right, front-right, front-left** (body frame). `null` when any corner is behind the camera. Corners may lie outside the image. |
| `geo` | `{lat, lon, alt_m}` | The entity origin at capture (WGS-84 degrees, ellipsoid metres); for a clamped vehicle its ground contact / waterline. |

An entity is written when it is in the camera frustum and has at least
`ml_training.min_visible_pixels` (default 1) visible pixels: fully hidden vehicles are not
labelled.

### Reading a record in Python

```python
import json
from pycocotools import mask as cocomask

with open("ml_output/camsim_coco.jsonl") as f:
    rec = json.loads(next(f))
for a in rec["annotations"]:
    if a.get("mask_source") != "render":
        continue  # projection fallback: box only
    seg = dict(a["segmentation"], counts=a["segmentation"]["counts"].encode())
    m = cocomask.decode(seg)  # H x W uint8, 1 = visible pixel of this entity
    assert int(m.sum()) == a["area"]
    x, y, w, h = a["bbox"]  # m[y:y+h, x:x+w] holds every visible pixel
    print(a["category"]["name"], a["visibility"], a.get("truncation"), a["obb"])
```

## Pascal VOC

One `annotations/frame_NNNNNNNN.xml` per annotated frame; per object `name`, `entity_id`,
`source`, `source_id`, `truncated` (as COCO), `occluded` (`visibility < 0.95`; only for
render-measured objects), `difficult` 0 and the **modal** box. Coordinates are 0-based pixel
edges, as CamSim always wrote them: `xmin`/`ymin` = the first covered pixel, `xmax`/`ymax` =
one past the last covered pixel (`xmax = x + w` of the COCO `bbox`).

## Configuration

| Key | Default | Env | Effect |
|---|---|---|---|
| `ml_training.enabled` | `false` | `CAMSIM_ML_ENABLED` | Master toggle (tagging, ID pass, writers). |
| `ml_training.bounding_boxes` | `true` | `CAMSIM_ML_BBOX_ENABLED` | Entity annotations (and so tagging and the ID pass). |
| `ml_training.annotation_interval_frames` | `1` | `CAMSIM_ML_INTERVAL_FRAMES` | Annotate every Nth frame; the ID pass runs only on those. |
| `ml_training.min_visible_pixels` | `1` | `CAMSIM_ML_MIN_VISIBLE_PIXELS` | Drop annotations with fewer visible pixels. |
| `ml_training.segmentation` | `true` | `CAMSIM_ML_SEGMENTATION_ENABLED` | Write the RLE `segmentation`. |

## Limitations

- **Vehicle-on-vehicle amodal masks.** Custom depth draws only the tagged vehicles, in one
  pass, so where one vehicle hides another the hidden one's amodal mask (and `bbox_amodal`,
  `obb_amodal`, `visibility`) loses the hidden part: `visibility` stays high for a truck behind
  another truck. Terrain, trees, buildings and water occlusion are measured correctly.
- **Edge accuracy under TSR.** Scene depth and stencil are jittered at render resolution while
  the output colour is TSR-resolved, so mask edges can be misregistered by up to ~0.5 render
  texel (≤ 1 output pixel). With FXAA (the default) there is no jitter.
- **Water is an occluder.** A hull below a wave crest is not visible even where clear water
  would show it faintly. The hull below the waterline is part of the amodal silhouette, so a
  boat seen from the side has `visibility` < 1 even on a flat sea (0.83 for the Mako 655 at
  ~3° depression, Beaufort 0); `bbox_amodal` includes the submerged hull. From above the deck
  hides it (`visibility` 1).
- **OBBs of side views.** `obb` is the minimum-area rectangle of the visible pixels, not the
  vehicle's axis: for a wedge-shaped profile (a boat from the side) it can tilt by several
  degrees from the hull line. Use `box3d` for the vehicle's orientation.
- **More than 255 tagged entities.** Entities beyond the 255 stencil values get no tag and fall
  back to `mask_source: "projection"` (the old loose box), as do entities that share a stencil
  value in one frame (never expected; logged). A frame whose ID readback failed also falls
  back to projection for every entity. If no tagged mesh draws in a frame (e.g. a
  model still loading), its entities have no IDs in the image and are dropped from that frame.
- **Depth map and KLV corners are still pinhole**: with `optics.k1`/`k2` ≠ 0 they drift from
  the distorted image toward the edges (ROADMAP 3B.3). The 2D labels and `box3d.corners_px`
  are distortion-aware.
- `truncation` clips the straight-edged hull of the 8 distorted corners: under strong
  distortion it is an approximation.
