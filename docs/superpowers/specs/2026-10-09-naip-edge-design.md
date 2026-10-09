# NAIP edge: margin, Sentinel-2 colour match, feather (REALISM R1, chunk 1) — design

2026-10-09. Scene tooling only (`scripts/scene`); no CamSim runtime change, no editor work.

## Goal

Make the edge of a scene package's NAIP imagery hard to see, and move it away from the area being flown. Keep
NAIP's own look: Sentinel-2 is adjusted toward NAIP, never the reverse.

Success, on the Camp Pendleton package:

- Where NAIP and the Sentinel-2 composite cover the same land, the cell bias (per 2 km cell, |median(NAIP − matched
  Sentinel-2)|, held-out samples) has a median over cells ≤ 5 DN per band (today's overall bias: 31 / 25 / 25 DN for
  R / G / B). It is a bias, not a per-pixel error: per pixel ~15 DN remains from resolution and date alone.
- The edge sits ≥ `imagery_margin_km` outside the bbox wherever NAIP exists there.
- R0 gates 3 (byte-identical rebuild) and 4 (`verify --deep`) still pass.
- Before/after screenshots of the west (San Clemente) and south (Oceanside) edges, reviewed by a human.

## Findings this rests on (spike, 2026-10-09, Pendleton package)

- **NAIP needs no balancing across files.** Neighbouring quarter-quads are bit-identical where they overlap (113
  overlapping pairs, 0 DN median difference; six pairs checked at full resolution, 100 % equal, including pairs
  flown 18 days apart). USDA cuts them from one balanced state mosaic. Per-file gain/offset fitting would do
  nothing and is not built.
- **Inside NAIP files**, flight-line seamlines are faint on land (one visible, through a pond) and strong over sea
  (sun-glint stripes). At sea CamSim's opaque ocean covers them except near the shore (chunk 3's business).
- **The visible seam is NAIP → Sentinel-2** at the bbox edge: in tiles straddling it, and beyond it (the ring, z9–10).
  Sentinel-2 as decoded today (`S2_WHITE_DN` 3000, gamma 2.2) is brighter, hazier and less saturated than NAIP.
- **Mapping candidates** (5.2 M land pixel pairs at 10 m, half held out): today MAE 32 / 26 / 26 DN; per-band linear
  17 / 14 / 14; 3×4 colour matrix 16 / 13 / 13 but contrast falls ~20 % (regression to the mean); **per-band
  quantile match 17 / 14 / 14 with NAIP's mean and standard deviation matched exactly** — chosen. The remaining
  ~15 DN is per-pixel (resolution, date, registration), not colour.
- **A single bbox-wide match is not enough at the edge**: at San Clemente the step falls only from ~64 DN to ~35 DN.
  Hence a local offset field (section 2).
- **Pendleton fetches ~418 km² of NAIP outside the bbox already** (quarter-quads straddling it) and discards it.

## Design

### 1. Imagery margin

- New scene key `imagery_margin_km` (float, default 3, range 0–50, and ≤ `ring_km`). `ScenePlan.area("margin")` is the bbox grown by
  it (`ring_bounds`' rule).
- New region `margin` between `ring` and `bbox`: max zoom = the bbox's for imagery, the ring's for terrain. Terrain
  depth is unchanged (the highest-detail region a tile overlaps wins, so terrain still reaches bbox depth only
  inside the bbox).
- `naip_pc.area_kind = "margin"`: discovery searches the grown area. Other sources keep their area kinds.
- Margin 0 is today's package, byte for byte (the `margin` region equals `bbox`; its name is in the regions list, so
  the manifest differs, but every tile is identical).
- `plan` already prints tile counts and size; the margin's cost is visible before a fetch. Pendleton estimate:
  +36 % area at 3 km, much of it sea with no NAIP; the actual figure is reported.

### 2. Sentinel-2 colour match (`imagery/balance.json`)

Fitted once, at the start of `build`'s imagery stage, in the main process, from cached assets. Skipped (and the file
absent) when the package has no NAIP or `balance` is off.

- **Samples.** Lattice at 10 m over the union of NAIP footprints. At each node: NAIP RGB (`average` resampled from the
  nearest overview to 10 m), Sentinel-2 reflectance (raw DN × 1e-4, not decoded), WorldCover class. Kept: both valid,
  class ≠ 80 (permanent water) and ≠ 0. Node order is fixed (block-major; the statistics are order-independent), so the sample is
  deterministic.
- **Tone curve.** Per band, 257 quantiles (0, 1/256 … 1) of S2 reflectance and of NAIP DN over the kept samples of the even (fit) half of the lattice (odd (i + j) nodes are held out) →
  piecewise-linear map reflectance → DN. Stored as the two quantile arrays; strictly increasing x is enforced (ties
  dropped, first kept).
- **Offset field.** Grid of 2 km cells (in degrees: 2 km / M_PER_DEG on latitude, the same in longitude ÷ cos(bbox
  centre latitude)), aligned to the ring's south-west corner, covering the ring. Per cell with ≥ 500 kept samples (even half only):
  median(NAIP − tone(S2)) per band. Then a 3×3 median filter over fitted cells only. Unfitted cells: the value of
  the nearest fitted cell (Euclidean on the grid, as `scipy.ndimage.distance_transform_edt` picks it), times exp(−d / 10 km), d = distance to
  that cell's centre. Bilinear between cell centres at sample time.
- **Applied** to every Sentinel-2 (`wc_s2`) sample at every zoom: DN = clip(round(tone(r) + offset(lon, lat)), 0, 255)
  instead of `_s2_reflectance`. Parents still box-filter their children, so lower zooms inherit it.
- **File.** `imagery/balance.json`: format version, source ids, feather width, the quantile arrays, grid origin /
  cell size / shape, offsets. Values are quantised before writing (quantiles 0.01 raw DN = 1e-6 reflectance, DN
  0.1) and stored as floats; grid values are floats rounded to 1e-9 before writing, so float-summation order can't change the bytes across machines. It is a
  package file: in `hashes.txt`, covered by `verify`, and its sha256 joins every imagery leaf's inputs hash (a refit
  rebuilds every leaf, and parents follow). Workers always read the model back from this file.
- **Fit report** (log + `build.json`, never `balance.json`, whose bytes must not depend on float statistics): samples
  fitted / held out, cells fitted, held-out MAE, bias and cell bias (median and p90 over cells) per band before and
  after, fit seconds (reported separately from the per-tile build time). Held out = lattice nodes with odd (i + j).
- A rebuild with unchanged inputs skips the fit (a marker under `.state/`, as land cover does).

### 3. Feather at the NAIP edge

- In `leaf_rgb`, NAIP (`naip_pc` group) blends into the next group over `BALANCE["feather_m"]` = 200 m, measured inward
  from NAIP's valid-data edge: w = clip(dist / 200 m, 0, 1), out = w · NAIP + (1 − w) · next. Inside the NAIP group
  first-valid-wins is unchanged (files are identical in overlaps).
- Distance comes from NAIP's valid mask sampled on a lattice over the tile plus a margin of ≥ 200 m (the terrain
  layer's margin/`distance_transform_edt` pattern; equal degree spacing in latitude and longitude, as terrain, so the ramp is 200 m north-south and 200·cos(lat) m east-west), bilinearly
  upsampled to the tile's pixels. Lattice spacing: the leaf's pixel size, doubled while it stays ≤ 4 m (z17: 2.4 m;
  z13 and coarser: one pixel), so nodes align across neighbouring tiles.
- Leaves inside NAIP skip it: when every pixel is NAIP and the tile lies inside the union of NAIP footprints shrunk
  by 200 m + 500 m (footprints are dilated by one coarse overview pixel), the leaf is NAIP alone. Same bytes as the
  full path; only faster.
- Only the NAIP → next transition is feathered. Sentinel-2 → Blue Marble stays as it is.

### 4. Config

- `imagery_margin_km` (scene file), default 3.
- `balance` (scene file, bool), default true. Off: no `balance.json`, Sentinel-2 decoded as today, no feather.
  `balance = false` with `imagery_margin_km = 0` reproduces a current package byte for byte.
- Both land in the manifest (`layers.imagery`), so `layer_settings_hash` covers them.
- `docs/scene-packages.md` documents both keys, `balance.json`, the feather, and the known limits.

## Testing

pytest under `scripts/scene/tests`, synthetic rasters (`tests/rasters.py`), no network:

- Tone curve recovers a known monotonic map (gamma + offset) to ≤ 1 DN.
- Offset field recovers a known per-cell offset inside coverage; decays as exp(−d / 10 km) outside; ignores water
  (class 80) samples.
- Two fits from the same inputs write byte-identical `balance.json`; shuffled asset order gives the same bytes.
- Feather: weights are 0 at NAIP's edge, 1 at ≥ 200 m inside, continuous across leaf tile boundaries (adjacent
  leaves agree on their shared edge to ≤ 1 DN before JPEG).
- `balance = false`, margin 0: leaf bytes identical to the current code on the fixture package.
- Margin: `naip_pc` discovery area and the `margin` region grow by the margin; terrain tile set unchanged.
- No NAIP in the package: no `balance.json`, Sentinel-2 decoded as today.

## Acceptance (Pendleton, `sim`)

1. Colour gate: median |NAIP − matched S2| per 2 km cell ≤ 5 DN per band on land (from the fit report, held-out
   half).
2. Rebuild from the manifest: `hashes.txt` identical (gate 3); `verify --deep` all `ok` (gate 4).
3. Imagery build time: ≤ 10 % over today per built tile, same `-j` (the margin's extra tiles and the fit's seconds
   are reported separately).
4. Screenshots: west and south edges at z13–z15, before and after; plus a CamSim render looking across the bbox edge
   from altitude. Human review.

## Known limits (go into `REALISM.md` / `docs/scene-packages.md`)

- Sentinel-2 is 10 m: beyond NAIP the imagery is blurry whatever its colour. The margin moves that edge, it doesn't
  remove it.
- Flight-line seamlines inside NAIP files stay (faint on land; strong at sea, mostly under the ocean).
- The sea patchwork (NAIP glint stripes, Sentinel-2, Blue Marble) stays: chunk 3 (coastline).
- The colour match is fitted per package: two adjacent packages may differ slightly at their shared ring.

## Out of scope

Per-file NAIP balancing (measured unnecessary); spatially varying correction inside NAIP; Sentinel-2 → Blue Marble
balancing; NDVI (chunk 2); coastline / sea level (chunk 3).
