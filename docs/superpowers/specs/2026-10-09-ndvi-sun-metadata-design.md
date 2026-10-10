# NDVI layer and NAIP sun metadata (REALISM R1, chunk 2) — design

2026-10-09. Scene tooling only (`scripts/scene`); no CamSim runtime change, no editor work.

## Goal

Give R2 a better vegetation signal than 4B's GBuffer ExG (which comes from JPEG colour, needs the `veg_blur_m`
workaround for chroma blocks, and is a colour proxy), and give R4 a placement input, by packaging an NDVI layer next
to the imagery. Record each NAIP quarter-quad's acquisition date and a sun-position bound, so the baked-shadow limit
(REALISM R1) is documented with numbers. Whether `ThermalCS` uses NDVI, and the GPU window it would need, is R2's
decision; nothing here reads the layer at runtime.

Success, on the Camp Pendleton `sim` package:

- The package carries `ndvi/`, deterministic, covered by `hashes.txt` and by a `verify --deep` value check.
- NAIP and Sentinel-2 NDVI are on one scale (Sentinel-2's), with no visible step at the NAIP edge.
- Every NAIP asset in `manifest.json` has `acquired` and `sun_noon`.
- `ndvi = false` reproduces today's package byte for byte.

## Findings this rests on

- **NIR is already fetched.** The Planetary Computer NAIP COGs are 4-band (R, G, B, NIR; nodata 0 in all bands), and
  `naip_pc.open()` reads bands 1–3. The `wc_s2` composite is RGBNIR at 10 m (band order to confirm from the COG's
  band descriptions in the plan's first task). No new download.
- **NAIP has a date, not a time.** Pendleton's 44 quads were flown on 2022-04-25 (3), 05-12 (7) and 05-30 (34). Every
  STAC `datetime` is `T16:00:00Z`: a placeholder, so the true sun position isn't in the data. NAIP flies with the sun
  at ≥ 30° elevation, so the actual elevation is between 30° and the solar-noon value.
- **NAIP is one balanced state mosaic** (chunk 1: overlapping quads bit-identical), so one global NDVI fit is the
  starting point, with per-flight-day residuals reported rather than modelled.
- Tile inputs hashes use asset sha256s, not asset metadata, so adding metadata rebuilds no tile.

## Design

### 1. Layer layout and encoding

```
ndvi/tilemapresource.xml        # TMS 1.0, EPSG:4326, profile geodetic, format image/png
ndvi/fit.json                   # NAIP -> Sentinel-2 NDVI fit (section 3), when both are present
ndvi/{z}/{x}/{y}.png            # 256 px, 8-bit grayscale
```

- Same `WorldCRS84Quad` grid and TMS addressing (`y` from the south) as `imagery/`.
- Value `0` = nodata; `1..255` = NDVI −1…+1 linearly: `code = 1 + round((ndvi + 1) * 127)`, decoded
  `ndvi = (code - 1) / 127 - 1` (step ≈ 0.0079). Rounding is `floor(x + 0.5)`.
- PNG written by Pillow with fixed compression level and no metadata chunks, so bytes are deterministic.

### 2. Tile set

- A tile is built when it is at zoom ≤ min(`ndvi_max_zoom`, that tile's imagery depth) and overlaps the `ring`
  region (Sentinel-2 and NAIP exist only there; Blue Marble has no NIR).
- Tiles whose pixels are all nodata are not written. No complete-siblings rule: the layer is data, not a Cesium
  overlay. `tilemapresource.xml` lists zooms 0..max built.
- Leaves are the deepest tiles built; parents are every ancestor of a leaf.

### 3. Leaves and the fit

- **Per-pixel NDVI:** `(NIR - R) / (NIR + R)`, computed from raw values: NAIP DN bands 4 and 1; Sentinel-2
  reflectance B8 and B4. Pixels with `NIR + R = 0` are nodata.
- **Merge, feather and water clip** as imagery (chunk 1): NAIP first, Sentinel-2 behind it, first valid wins; NAIP fades
  into Sentinel-2 over `feather_m` (200 m) inward from its valid-data edge; with WorldCover present NAIP is used only
  within `naip_water_buffer_m` of land. The imagery leaf code is refactored so sampling, feather weights and the water
  distance are shared; imagery output bytes must not change (gate 6 covers it).
- **Fit (`ndvi/fit.json`):** NAIP NDVI → Sentinel-2 scale, `ndvi_s2 ≈ gain * ndvi_naip + offset`, least squares on the
  chunk-1 land lattice (10 m, WorldCover classes 0 and 80 excluded, even half fitted, odd half held out). Gain and
  offset quantised to 1e-6, written as canonical JSON with a `format` field; workers read it back from the file. The
  fit report (samples, held-out MAE, median bias before/after, residual median bias per NAIP acquisition date, fit
  seconds) goes to the log and `build.json` under `ndvi`, never into `fit.json`. Applied to every NAIP NDVI sample,
  then clamped to [−1, 1]. No NAIP/Sentinel-2 land overlap → no `fit.json`, NAIP NDVI unfitted, warning.
- `fit.json`'s sha256 joins every NDVI leaf's inputs hash (as `balance.json` for imagery). A rebuild with unchanged
  inputs skips the fit.

### 4. Parents

Each parent pixel is the mean of the valid decoded NDVI values among its 2 × 2 child pixels (a missing child tile
counts as four nodata pixels), re-encoded; nodata only if all four are nodata. Means of NDVI, not of bands.

### 5. Sun metadata

Written at `plan` into each NAIP asset's `metadata`:

- `acquired`: `YYYY-MM-DD` from the STAC `datetime` (the time is dropped).
- `sun_noon`: `{"elevation_deg": …, "azimuth_deg": …}` at local solar noon on that date at the footprint centroid,
  NOAA solar position equations implemented in `camsim_scene/sun.py` (no dependency), rounded to 0.01°.

`wc_s2` assets get `composite: "2021"` and no sun. `docs/scene-packages.md` states the bound: elevation in
[30°, `sun_noon.elevation_deg`], azimuth within about ±45° of `sun_noon.azimuth_deg` at Pendleton's latitude. Existing
packages pick the metadata up with `build --replan`; no tile is rebuilt. Nothing reads it at runtime (a CamSim warning
when the sim sun is far from the baked one is out of scope).

### 6. Config

```toml
ndvi = true            # sim default; preview default false (no NAIP)
ndvi_max_zoom = 15     # [10, 17]; z15 ≈ 2.4 m
```

- Stored in `manifest.layers["ndvi"]`; the layer has its own settings hash, so enabling it rebuilds no terrain or
  imagery tile.
- `ndvi = false`: no `ndvi/` directory, no `fit.json`; terrain, imagery, land cover and `hashes.txt` identical to a
  build without this change (the sun metadata changes `manifest.json` only).
- `ndvi = true` with no `naip_pc` or `wc_s2` in the imagery priorities: plan error.

### 7. verify

- NDVI files are in `hashes.txt` like every other file.
- `verify --deep` gains `ndvi_values`: sample leaf pixels (seeded, fixed count per leaf), recompute NDVI independently
  from the source COGs plus `fit.json` at the pixel centre, compare decoded values. Pixels inside NAIP's interior
  (beyond the feather and water buffer): p99 |Δ| ≤ 1 step. Sentinel-2-only pixels: p99 ≤ 1 step. Feather pixels are
  excluded.

## Testing (pytest, `scripts/scene/tests`, no network)

- Encoding: round trip of codes 1..255; NDVI −1, 0, +1 and the nodata code.
- `sun.py` against NOAA calculator reference values (three dates/locations, ≤ 0.05°).
- Fit: quantisation, identical `fit.json` from shuffled samples, no-overlap path.
- Leaf on synthetic NAIP + Sentinel-2 rasters: pure NAIP interior, Sentinel-2 only, feather midpoint, water clip.
- Parents: all-valid, partial nodata, missing child tile, all-nodata (not written).
- Config: defaults per profile, ranges, the no-source plan error.
- Off switch: `ndvi = false` leaves imagery leaf bytes unchanged for a synthetic scene.
- `-j 1` vs `-j 2` identical NDVI tiles on the synthetic scene.

## Acceptance (Pendleton, `sim`)

1. `verify --deep` clean, including `ndvi_values`.
2. Fit: held-out MAE reported; held-out median bias NAIP-fitted vs Sentinel-2 ≤ 0.02; per-date residual biases
   reported (> 0.03 on any date → a follow-up item, not a failure).
3. Plausibility: median NDVI per WorldCover class ordered tree cover (10) > grassland (30) / shrubland (20) >
   bare (60) / built-up (50); permanent water (80) < 0.
4. Seam: along the NAIP edge, median |NDVI(inside the feather) − NDVI(outside)| ≤ 0.03, from a lattice of
   pairs straddling the feather band.
5. Determinism: `-j 1` and `-j 16` builds byte-identical; a rebuild skips every NDVI tile.
6. Off switch: `ndvi = false` gives the chunk-1 package byte for byte (all files except `manifest.json`).
7. Reported, not gated: NDVI build wall time, tile count and size per zoom in `build.json` and "Measured".

A small tool `scripts/scene/tools/ndvi_check.py PKG --cache DIR` computes gates 3 and 4 and writes a false-colour
overview PNG for human review.

## Known limits (go into `docs/scene-packages.md`)

- NAIP NDVI is from uncalibrated DN; the fit puts it on Sentinel-2's scale on average, not per pixel.
- One global fit; per-flight-day differences are reported, not corrected.
- NDVI is leaf-on (NAIP flies in the growing season) and dated; Sentinel-2's composite is 2021.
- Sentinel-2 is 10 m outside NAIP; open sea beyond the composite is nodata.
- Sun position is a bound (date known, time not).

## Out of scope

Runtime use of NDVI (R2), a GPU NDVI window, a sun-mismatch warning in CamSim, per-date or gridded NDVI fits,
Sentinel-2-only NDVI for `preview`, 16-bit NDVI.
