# Terrain Classification (ROADMAP 4B) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Terrain stops being one thermal class: every terrain pixel in IR gets a material from ESA WorldCover land cover (blended bilinearly across the four nearest 10 m texels and refined per pixel by the imagery's base colour), so night terrain has thermal texture, noon vegetation reads cool and built-up/bare ground hot, and inland water is water.

**Architecture:** An offline script (`scripts/landcover/fetch_worldcover.py`) cuts WorldCover 2021 v200 COGs into lossless 0.05° greyscale PNG tiles plus `index.json` (a San Francisco sample is committed via git LFS). At runtime `FLandCoverTileCache` decodes tiles (ImageWrapper, LRU) and `FLandCoverWindow` builds a camera-centred 2048² East/North window (10 m texels) on a task thread, swaps it in on the game thread and uploads it as an `R8_UINT` texture; the ref-counted GPU window travels to the render thread in the same render command as that frame's `FThermalFrameParams` (paired by window id). `ThermalCS` maps each terrain pixel's camera-relative world position into the window (East/North unit vectors at the window centre + the camera offset computed in doubles), blends the four texels' refined materials (temperature, emissivity, k_fast, S_abs,ref) and carries on with the unchanged 4A radiance maths; `CamSimThermalRef` mirrors it expression for expression.

**Tech Stack:** UE 5.8 C++ (RDG, global compute shaders, UE::Tasks, ImageWrapper, Json, UE Automation tests), HLSL (`/CamSim`, Metal + Vulkan portable), Cesium for Unreal (georeference transforms), Python 3.10+ (`rasterio` for the offline fetch only; numpy + pillow + pytest).

**Spec:** `docs/superpowers/specs/2026-10-01-terrain-classification-design.md` (builds on `docs/superpowers/specs/2026-10-01-thermal-core-design.md`; as-built 4A guide `docs/thermal.md`).

## Global Constraints

- Copyright header `// Copyright CamSim Contributors. All Rights Reserved.` on every new C++/HLSL file; Python files start with the repo's usual docstring (PEP 723 header for runnable scripts).
- UE naming (`A`/`U`/`F`/`E`/`I` prefixes), PascalCase, verb-first functions; `#include "CoreMinimal.h"` first, `.generated.h` last; forward declarations preferred in headers.
- `ThermalCS` mirrors `CamSimThermalRef::EvaluatePixel` expression for expression (same order, same constants, same float types); change both together. `CamSim.GPU.Thermal.MatchesCpu` and the new `CamSim.GPU.Thermal.LandCoverMatchesCpu` hold them to 1e-4 relative radiance (never loosen it).
- Rounding (where any) is `floor(x + 0.5)`, never HLSL `round`. Non-finite tests in HLSL are bit tests (`asuint`, `IsNonFinite`), never `x != x` (Metal fast-math folds it).
- Plain compute only: no float atomics, no wave intrinsics, no groupshared in `ThermalCS` (portable to Vulkan).
- Targets stay on `BuildSettingsVersion.V7`; no new modules or plugins (new code lives in the existing `CamSimShaders` and `CamSimTest` modules; `ImageWrapper`, `Json`, `RenderCore`, `Renderer`, `CesiumRuntime` are already dependencies).
- New config keys are documented in `docs/configuration.md` and `deploy/camsim_config.yaml` in the same task that adds them; `CamSim.Config.CanonicalConfigHasNoUnknownKeys` keeps passing.
- `thermal.land_cover.enabled: false` (or no window) gives 4A's thermal output bit for bit (`CamSim.Thermal.Reference.LandCoverOffIs4A`, `CamSim.Thermal.Builder.LandCoverDisabledIs4A`, the GPU "off" case); EO is unchanged (gate g); every existing `CamSim.*` and `CamSim.GPU.*` test keeps passing.
- No network at runtime: CamSim reads only `thermal.land_cover.dir`. The window build never runs on the game or render thread (task thread, < 100 ms target); only the 4 MB texture upload runs on the render thread, once per re-centre.
- Performance: `ThermalCS` ≤ 0.5 ms p95 at 1080p on the M1 Pro (gate f; land cover budget +0.1 ms over 4A's 0.141 ms), measured with the existing `FGPUStat` scope, never `RQT_AbsoluteTime`.
- Altitudes are WGS-84 ellipsoid heights; 1 UE unit = 1 cm; Cesium takes `FVector(Lon, Lat, Alt)` (longitude first).
- WorldCover data is CC BY 4.0: the attribution string travels in `index.json`, `ATTRIBUTION.txt`, `docs/thermal.md` and `README.md`.
- NullRHI tests via the headless command below (macOS needs `-DisablePython`); GPU tests (`CamSim.GPU.*`) via `scripts/run_gpu_tests.sh <filter>`.
- Keep `ROADMAP.md` current as tasks land; record any editor/human change in `ROADMAP.md` (4B needs none: the data is produced by a script and committed via LFS — say so explicitly).
- Commit messages end with the two lines (use the model that actually wrote the commit):
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
  ```

## Commands

```bash
# Build (editor target). --mode editor is required on macOS: run.sh switches to packaged mode
# when Saved/StagedBuilds/Mac/CamSimTest.app exists. Linux: same command (auto-discovers /opt/UE).
set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5

# NullRHI automation tests: run_tests <Filter>, e.g. run_tests CamSim.Thermal.LandCover
UE_BIN="/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"
run_tests() {
  rm -rf .cache/automation-report
  "$UE_BIN" "$PWD/unreal_project/CamSimTest/CamSimTest.uproject" -ExecCmds="Automation RunTests $1+Quit" \
    -TestExit="Automation Test Queue Empty" -ReportExportPath="$PWD/.cache/automation-report" \
    -unattended -nullrhi -nosound -nosplash -DisablePython -log -stdout -FullStdOutLogOutput > .cache/automation.log 2>&1
  python3 -c "import json;d=json.load(open('.cache/automation-report/index.json',encoding='utf-8-sig'));print('succeeded',d['succeeded'],'failed',d['failed']);[print('FAIL',t['fullTestPath'],[e['event']['message'] for e in t['entries'] if e['event']['type']=='Error']) for t in d['tests'] if t['state']=='Fail']"
}
# (Linux: UE_BIN=/opt/UE/Engine/Binaries/Linux/UnrealEditor, -DisablePython optional.)

# GPU tests (Metal; first run compiles shaders for minutes)
scripts/run_gpu_tests.sh CamSim.GPU.Thermal

# Python unit tests (what CI runs)
uv run --with pytest --with numpy --with pillow --with pycocotools pytest scripts/tests/ -v

# Live acceptance (about 9 launches; keep the host awake)
caffeinate -ims uv run scripts/thermal_check.py --band both
```

Every C++ "Run" step below means: build, then `run_tests <filter>` (or `scripts/run_gpu_tests.sh <filter>`). A step that expects a compile failure stops at the build.

## Review Focus

Input classes and failure modes the spec implies but its test list does not exercise, most likely first, each pinned by a test in its owning task:

1. **A clone without `git lfs pull`, or a damaged tile** (PNG files are LFS pointer text; one tile truncated): the tile is reported once by path with a "git lfs pull" hint, treated as no data (code 0 → `terrain_default`), never decoded again, and nothing crashes. → `CamSim.Thermal.LandCover.CorruptTileIsMissing` (Task 2) and `CamSim.Thermal.LandCover.CorruptTileInWindow` (Task 4).
2. **The camera flies out of the data's coverage** (the SF sample is only ~18 × 17 km; open ocean; any area nobody fetched): a window without a single nonzero code turns land cover off with one warning (4A output), does not rebuild every frame, and comes back by itself when the camera returns over data. → `CamSim.Thermal.LandCover.WindowWithoutDataIsOff` (Task 4).
3. **Hot reload of `thermal.land_cover.*` while a window is building** (`dir` or `window_texels` changed, land cover toggled): the in-flight result built under the old settings is discarded, never published with the wrong size or source, and the next build uses the new settings. → `CamSim.Thermal.LandCover.ReconfigureDiscardsInFlightBuild` (Task 4).
4. **A window swap between the game thread building a frame's parameters and the render thread using them** (re-centre lands mid-flight; the GPU upload hasn't run yet): a frame binds a window texture only when its id equals the parameters' `LandCoverWindowId` and the texture exists; otherwise that frame renders land cover off (never a texture from window N+1 with window N's mapping). → `CamSim.Thermal.LandCover.FramePairingById` (Task 10).
5. **Degenerate base colours in the refinement** (pure black shadow pixels with R+G+B = 0, saturated white, pure red; `veg_index_lo == veg_index_hi` reached via env overrides; a zero luma ramp): vegetation and concrete weights stay finite in [0, 1], the blend stays a convex combination of materials, and the radiance stays finite inside [B(150 K), B(1000 K)]. → `CamSim.Thermal.Reference.RefinementExtremesFinite` (Task 7).

## Decisions (where this plan completes or departs from the spec)

- **Tile geometry (spec correction):** a WorldCover cell is 1/12000° (3° COGs of 36000 × 36000 px, verified), so a 0.05° tile is exactly **600 × 600 cells** (≈ 9.3 m N–S × 7.3 m E–W at 37.8° N), not "~555 × 440 px". Tiles are exact 600 × 600 blocks of COG cells: no resampling, codes pass through.
- **Confirmed source URL:** `https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/2021/map/ESA_WorldCover_10m_2021_v200_{N|S}{lat:02d}{E|W}{lon:03d}_Map.tif`, named by the 3° tile's SW corner (SF: `N36W123`, HTTP 200, 87.6 MB, uint8, deflate, 1024² blocks, overviews 2–64, nodata 0, EPSG:4326, origin (−123, 39)). Read with HTTP range requests (`rasterio`), never downloaded whole.
- **Tile naming:** `<SW lat>_<SW lon>.png` with two decimals and explicit sign (`+37.75_-122.45.png`); `index.json` carries integer `lat_index = SW lat / 0.05`, `lon_index = SW lon / 0.05` so C++ never parses names. PNG row 0 = north edge.
- **Attribution string:** the index, `ATTRIBUTION.txt` and docs use ESA's full form "© ESA WorldCover project 2021 / Contains modified Copernicus Sentinel data (2021) processed by ESA WorldCover consortium" (the spec's string omits the "processed by…" clause).
- **Reference input (spec deviation):** `CamSimThermalRef::EvaluatePixel` takes the window codes (a pointer in `FPixelSample`) rather than "4 codes + bilinear weights", because the texel coordinates depend on the world position EvaluatePixel itself reconstructs. It stays a pure function; `SampleLandCover` (Pw → 4 codes + weights) and `BlendLandCover` are exposed and tested on their own, and the shader has the same split.
- **Refinement families are fixed by WorldCover code:** vegetation {10, 20, 30, 40, 90, 95, 100}, built-up {50}, bare {60}, none {0, 70, 80}. The asphalt/concrete luminance split applies to **built-up only**; bare's non-vegetation part stays `bare_soil`. (The spec says "in built-up and bare"; splitting bare too would make `bare_soil` unreachable for code 60 and contradict its own class table.) `built_up` is the material of code 50 when there is no base colour.
- **Soft luma ramp:** concrete weight = `saturate((BaseLum − asphalt_max_luma) / 0.04 + 0.5)` (ramp width `AsphaltRampLuma = 0.04`, a constant, not a key). `ExG` uses an epsilon of 1e-4 and degenerate threshold spans are guarded with `max(·, 1e-4)`.
- **Refinement is gated on base colour availability, not on the fast term:** at night the builder sets `KFastScale = 0` (no sun), but the GBuffer base colour is still valid, so `bLandCoverRefine = bBaseColorAvailable`.
- **Vegetation albedos are effective values:** the 4A model has no latent heat term, so tree/shrub/grass/crop/wetland albedos fold evapotranspiration into the absorbed solar (e.g. `tree_canopy` 0.40, `wetland` 0.40); high `convection_w_m2k` keeps canopies near air temperature (warmer than grass at night, cooler than asphalt at noon).
- **`snow_ice`** uses a new temperature source `snow` = min(model, 273.15 K) (`EThermalTemperatureSource::Snow`), the spec's "fixed ≤ 0 °C".
- **New built-in classes** are appended after 4A's six with fixed indices: 6 `tree_canopy`, 7 `shrubland`, 8 `grassland`, 9 `cropland`, 10 `built_up`, 11 `bare_soil`, 12 `snow_ice`, 13 `wetland` (14 built-ins; 18 left for `thermal.materials` additions under `MaxClasses = 32`).
- **Window texture** is `PF_R8_UINT` read as `Texture2D<uint>` (exact codes, no UNORM rounding). Null → land cover off and the engine's zero-uint dummy is bound.
- **East/North are computed every frame** on the game thread from the Cesium georeference at the window centre (`ComputeEastSouthUpToUnrealTransformation`), not stored with the window: Cesium origin shifting rotates the UE world axes between frames. `CamOffsetM` uses the same small-area formula as the resample, so the mapping is self-consistent; the tangent-plane vs small-area mismatch is ≤ E² tan φ / 2R (1.5 m at 5 km, 6 m at 10 km from the centre; under one texel).
- **Ownership:** `UCamSimCaptureComponent` owns the `FLandCoverWindow` next to `FThermalFrameBuilder` and updates it only on thermal IR ticks. The current window's CPU codes (4 MB) are kept alongside the GPU copy (diagnostics and tests).
- **Texel size** is fixed at 10 m (`FLandCoverWindow::TexelM`); `window_texels` is the configurable size. `recentre_fraction` is measured on the camera position (the KLV sensor position, as the spec's "nadir").
- **Frame stats** gain `land_cover_window` (the window id paired with the tick, 0 = none) so gate (l) can find re-centre frames.
- **Gate (l)** forces re-centres with `CAMSIM_THERMAL_LAND_COVER_RECENTRE_FRACTION=0.02` (409.6 m): at the spec's 1 km/min pan the default 25 % of 20.48 km would need over 5 minutes per re-centre. The spike is the max `wall_ms` within ±15 frames of a re-centre minus the pan's median.
- **Gate (k)** compares high-pass spatial std (Y minus its 33 × 33 box mean, central 60 %, entity boxes masked) of a land-cover night frame against a `CAMSIM_THERMAL_LAND_COVER_ENABLED=0` launch of the same pose: the AGC normalises contrast, and 4A's flat night is mostly vignetting the AGC stretches, which the high-pass removes.
- **Gates (i)/(j)** take the greenness mask from an EO capture (sensor 0) at the same static pose in the same launch, central 60 % only (EO and IR presets have different lens distortion) and eroded 2 px; non-vegetation excludes water-blue and deep-shadow pixels. Night (j) reuses the noon EO mask (the pose and land cover are static).
- **Pure geometry** lives in its own file (`Thermal/LandCoverGeometry.{h,cpp}`), separate from the async window (`Thermal/LandCoverWindow.{h,cpp}`), and the code tables in `Thermal/LandCoverClasses.{h,cpp}` (files the spec did not list).
- ROADMAP item 4.2 proposed draping the raster as a Cesium overlay; the spec's camera-centred window replaces that (photogrammetry tiles get classes the same way, through their world position).

---

## File Structure

| File | Responsibility |
|---|---|
| Create `scripts/landcover/__init__.py`, `scripts/landcover/fetch_worldcover.py` | offline COG → 0.05° PNG tiles + `index.json` + `ATTRIBUTION.txt` |
| Create `scripts/tests/test_fetch_worldcover.py` | tiling maths, naming, COG offsets, code passthrough |
| Create `unreal_project/CamSimTest/Content/NonUFS/LandCover/{index.json, ATTRIBUTION.txt, *.png}` | committed SF sample (20 tiles, PNGs in LFS) |
| Modify `.gitattributes` | LFS pattern for the land-cover PNGs |
| Create `Source/CamSimTest/Thermal/LandCoverTiles.h/.cpp` | `FLandCoverTile`, `FLandCoverIndex`, `FLandCoverTileCache` (index parse, PNG decode, LRU) |
| Create `Source/CamSimTest/Thermal/LandCoverGeometry.h/.cpp` | `CamSimLandCover::` radii, small-area mapping, global cell → tile, `Resample`, `NeedsRecentre`, pole rule |
| Create `Source/CamSimTest/Thermal/LandCoverWindow.h/.cpp` | `FLandCoverWindowData`, `FLandCoverGpuWindow`, `FLandCoverWindow` (async build, swap, GPU upload), `ShouldBindWindow` |
| Create `Source/CamSimTest/Thermal/LandCoverClasses.h/.cpp` | `ELandCoverFamily`, default code → material names, families, `FLandCoverClassTable`, class-spec validation |
| Modify `Source/CamSimTest/Thermal/ThermalTypes.h` | `EThermalTemperatureSource::Snow`, `FLandCoverClassSpec` |
| Modify `Source/CamSimTest/Thermal/ThermalMaterials.h/.cpp`, `ThermalModel.h/.cpp` | eight new built-ins + index constants; `snow` source |
| Modify `Source/CamSimTest/Config/CamSimConfig.h/.cpp` | `thermal.land_cover.*` keys, env vars, validation |
| Modify `Source/CamSimShaders/Public/ThermalFrameParams.h` | land-cover tables, mapping, refinement thresholds, special class indices |
| Modify `Source/CamSimTest/Thermal/ThermalReference.h/.cpp` | `SampleLandCover`, `RefinementWeights`, `RefinedClassData`, `BlendLandCover`, EvaluatePixel land-cover branch |
| Modify `Shaders/Private/CamSimThermalCommon.ush` | the shader mirror |
| Modify `Source/CamSimShaders/Public/ThermalPass.h`, `Private/ThermalPass.cpp` | `FThermalPassInputs::LandCover`, parameters, table packing, dummy |
| Modify `Source/CamSimTest/Thermal/ThermalFrameBuilder.h/.cpp` | `FThermalLandCoverInput`, class table, mapping fields |
| Modify `Source/CamSimTest/Thermal/ThermalFrameSources.h/.cpp` | `SetLandCover` (sanitiser), `LandCoverAxesWorld` (Cesium) |
| Modify `Source/CamSimTest/Camera/CamSimCaptureComponent.h/.cpp`, `CamSimCamera.cpp`, `CamSimFrameGrabExtension.h/.cpp`, `CamSimFrameStats.h/.cpp` | window per thermal tick, render handoff, texture registration, `land_cover_window` stat |
| Create `Tests/LandCoverTestTiles.h`, `LandCoverTilesTest.cpp`, `LandCoverGeometryTest.cpp`, `LandCoverWindowTest.cpp`, `ThermalLandCoverMaterialsTest.cpp`, `ThermalLandCoverConfigTest.cpp`, `ThermalLandCoverReferenceTest.cpp`, `ThermalLandCoverBuilderTest.cpp`; modify `ThermalTestScene.h`, `ThermalGpuTest.cpp`, `ThermalSourcesTest.cpp`, `RenderPathTest.cpp` | tests |
| Modify `scripts/thermal_check.py`, `scripts/tests/test_thermal_check.py` | gates (i)–(l), new views and runs |
| Modify `deploy/camsim_config.yaml`, `docs/configuration.md`, `docs/thermal.md`, `ROADMAP.md`, `CLAUDE.md`, `README.md` | docs |

All `Source/...`, `Shaders/...`, `Tests/...` paths are under `unreal_project/CamSimTest/` (`Tests/` = `Source/CamSimTest/Tests/`).

Task order and dependencies: 1 (data) → 2 (tiles) → 3 (geometry) → 4 (window) → 5 (materials) → 6 (config + code table) → 7 (params + reference) → 8 (shader + GPU) → 9 (builder + sources) → 10 (capture wiring) → 11 (acceptance) → 12 (docs). Tasks 5–6 do not depend on 2–4 and may run before them.

---

### Task 1: WorldCover fetch script, its tests, and the committed San Francisco sample

**Files:**
- Create: `scripts/landcover/__init__.py`, `scripts/landcover/fetch_worldcover.py`
- Test: `scripts/tests/test_fetch_worldcover.py`
- Create (generated, committed): `unreal_project/CamSimTest/Content/NonUFS/LandCover/index.json`, `ATTRIBUTION.txt`, 20 × `*.png`
- Modify: `.gitattributes`

**Interfaces:**
- Produces (Python, `scripts/landcover/fetch_worldcover.py`):
  ```python
  TILE_DEG = 0.05; CELLS_PER_DEG = 12000; TILE_PX = 600; TILES_PER_COG = 60
  COG_URL: str            # .format(name="N36W123")
  ATTRIBUTION: str; LICENCE = "CC BY 4.0"; FORMAT = "camsim-landcover-1"
  def tile_range(lo: float, hi: float) -> range
  def tiles_for_bbox(w: float, s: float, e: float, n: float) -> list[tuple[int, int]]   # (lat_index, lon_index)
  def tile_filename(i: int, j: int) -> str                                            # "+37.75_-122.45.png"
  def cog_name(i: int, j: int) -> str                                                 # "N36W123"
  def cog_offset(i: int, j: int) -> tuple[int, int]                                   # (row_off, col_off) cells
  def read_cog_window(url: str, row_off: int, col_off: int) -> np.ndarray | None      # None: COG absent (404)
  def write_tile(codes: np.ndarray, path: Path) -> None
  def build_index(bbox, written, missing) -> dict
  def fetch(bbox, out: Path, reader=read_cog_window) -> dict
  ```
- Produces (data contract read by Task 2): `index.json` = `{"format": "camsim-landcover-1", "source", "source_url", "licence": "CC BY 4.0", "attribution", "tile_deg": 0.05, "tile_px": 600, "bbox": [W, S, E, N], "tiles": [{"file", "lat_index", "lon_index"}...], "missing": [[i, j]...]}`; tile `(i, j)` covers lat `[0.05 i, 0.05 (i+1))`, lon `[0.05 j, 0.05 (j+1))`; 600 × 600 8-bit greyscale PNG, row 0 = north, value = WorldCover code.

- [ ] **Step 1: Write the failing tests** — `scripts/tests/test_fetch_worldcover.py`:

```python
"""Unit tests for scripts/landcover/fetch_worldcover.py (ROADMAP 4B): tiling maths, naming, code passthrough."""

import json

import numpy as np
import pytest
from PIL import Image

from landcover import fetch_worldcover as fw


def test_san_francisco_bbox_is_20_tiles():
    tiles = fw.tiles_for_bbox(-122.56, 37.69, -122.35, 37.84)
    assert len(tiles) == 20
    assert sorted({i for i, _ in tiles}) == [753, 754, 755, 756]
    assert sorted({j for _, j in tiles}) == [-2452, -2451, -2450, -2449, -2448]


def test_a_bbox_edge_on_a_tile_boundary_does_not_add_a_tile():
    assert list(fw.tile_range(37.70, 37.80)) == [754, 755]  # 37.80 starts tile 756
    assert list(fw.tile_range(-122.50, -122.45)) == [-2450]
    assert list(fw.tile_range(37.71, 37.72)) == [754]  # inside one tile


def test_invalid_bbox_raises():
    with pytest.raises(ValueError):
        fw.tiles_for_bbox(-122.0, 37.0, -123.0, 38.0)  # W > E
    with pytest.raises(ValueError):
        fw.tiles_for_bbox(-122.0, 37.0, -121.0, 91.0)  # N > 90


def test_tile_filename_is_the_signed_south_west_corner():
    assert fw.tile_filename(755, -2449) == "+37.75_-122.45.png"
    assert fw.tile_filename(0, -1) == "+0.00_-0.05.png"
    assert fw.tile_filename(-1, 3599) == "-0.05_+179.95.png"


def test_cog_name_and_offset():
    assert fw.cog_name(755, -2450) == "N36W123"
    assert fw.cog_offset(755, -2450) == (14400, 6000)  # verified against the live COG (rasterio window)
    assert fw.cog_name(-1, -1) == "S03W003"
    assert fw.cog_offset(-1, -1) == (0, 59 * 600)
    assert fw.cog_name(1799, 3599) == "N87E177"
    assert fw.cog_offset(1799, 3599) == (0, 59 * 600)
    assert fw.cog_name(-1800, -3600) == "S90W180"
    assert fw.cog_offset(-1800, -3600) == (59 * 600, 0)


def test_cog_offset_matches_the_cell_geometry():
    for i, j in [(755, -2450), (-17, 42), (1234, -3600), (-1800, 3599)]:
        row, col = fw.cog_offset(i, j)
        cog_top = (i // 60) * 3 + 3
        cog_left = (j // 60) * 3
        assert abs((cog_top - row / fw.CELLS_PER_DEG) - (i + 1) * fw.TILE_DEG) < 1e-9
        assert abs((cog_left + col / fw.CELLS_PER_DEG) - j * fw.TILE_DEG) < 1e-9


def test_fetch_passes_codes_through_losslessly(tmp_path):
    calls = []
    yy, xx = np.mgrid[0:600, 0:600]
    pattern = ((yy + 3 * xx) % 101).astype(np.uint8)

    def reader(url, row_off, col_off):
        calls.append((url, row_off, col_off))
        if "W123" in url and col_off == 6600:  # tile (755, -2449) "missing" (as a 404 would be)
            return None
        return pattern

    index = fw.fetch((-122.50, 37.75, -122.40, 37.80), tmp_path, reader=reader)
    assert [c[1:] for c in calls] == [(14400, 6000), (14400, 6600)]
    assert calls[0][0] == fw.COG_URL.format(name="N36W123")
    png = tmp_path / "+37.75_-122.50.png"
    assert png.exists() and not (tmp_path / "+37.75_-122.45.png").exists()
    img = Image.open(png)
    assert img.mode == "L" and img.size == (600, 600)
    assert np.array_equal(np.asarray(img), pattern)
    on_disk = json.loads((tmp_path / "index.json").read_text())
    assert on_disk == index
    assert index["format"] == "camsim-landcover-1"
    assert index["tile_deg"] == 0.05 and index["tile_px"] == 600
    assert index["licence"] == "CC BY 4.0" and "ESA WorldCover" in index["attribution"]
    assert index["tiles"] == [{"file": "+37.75_-122.50.png", "lat_index": 755, "lon_index": -2450}]
    assert index["missing"] == [[755, -2449]]
    assert "CC BY 4.0" in (tmp_path / "ATTRIBUTION.txt").read_text()


def test_write_tile_rejects_the_wrong_shape(tmp_path):
    with pytest.raises(ValueError):
        fw.write_tile(np.zeros((10, 10), np.uint8), tmp_path / "x.png")
```

- [ ] **Step 2: Run to verify failure** — Run: `uv run --with pytest --with numpy --with pillow --with pycocotools pytest scripts/tests/test_fetch_worldcover.py -v` → Expected: collection error, `ModuleNotFoundError: No module named 'landcover'`.

- [ ] **Step 3: Implement** — `scripts/landcover/__init__.py`:

```python
"""Land-cover data preparation for thermal IR (ROADMAP 4B)."""
```

`scripts/landcover/fetch_worldcover.py`:

```python
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy", "pillow", "rasterio"]
# ///
"""fetch_worldcover.py — cut ESA WorldCover 2021 v200 into CamSim land-cover tiles (ROADMAP 4B).

Reads only the windows it needs from ESA's 3 x 3 degree Cloud-Optimised GeoTIFFs (HTTP range
reads through rasterio; nothing is downloaded whole) and writes, for every 0.05 x 0.05 degree
tile that touches --bbox, an 8-bit greyscale PNG whose pixel values are the WorldCover class
codes, plus index.json and ATTRIBUTION.txt. A WorldCover cell is 1/12000 degree, so a tile is
exactly 600 x 600 cells: no resampling, the codes pass through losslessly. CamSim reads only
these local files (thermal.land_cover.dir); there is no network access at runtime.

Tile (lat_index i, lon_index j) covers latitudes [0.05 i, 0.05 (i + 1)) and longitudes
[0.05 j, 0.05 (j + 1)). Its file is named by its south-west corner (+37.75_-122.45.png);
PNG row 0 is the north edge, column 0 the west edge. A COG that doesn't exist (open ocean)
is listed under "missing" and CamSim treats it as no data (code 0).

Data: ESA WorldCover 10 m 2021 v200, CC BY 4.0. Attribution (keep it with the data):
  (c) ESA WorldCover project 2021 / Contains modified Copernicus Sentinel data (2021)
  processed by ESA WorldCover consortium

Usage:
    uv run scripts/landcover/fetch_worldcover.py --bbox W S E N [--out DIR]
    # the committed San Francisco sample:
    uv run scripts/landcover/fetch_worldcover.py --bbox -122.56 37.69 -122.35 37.84
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from collections.abc import Callable
from pathlib import Path

import numpy as np
from PIL import Image

REPO = Path(__file__).resolve().parents[2]
DEFAULT_OUT = REPO / "unreal_project/CamSimTest/Content/NonUFS/LandCover"
TILE_DEG = 0.05
CELLS_PER_DEG = 12000
TILE_PX = 600  # TILE_DEG * CELLS_PER_DEG
TILES_PER_COG = 60  # 3 degrees / TILE_DEG
COG_URL = (
    "https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/2021/map/"
    "ESA_WorldCover_10m_2021_v200_{name}_Map.tif"
)
SOURCE = "ESA WorldCover 10m 2021 v200"
LICENCE = "CC BY 4.0"
ATTRIBUTION = (
    "© ESA WorldCover project 2021 / Contains modified Copernicus Sentinel data (2021) "
    "processed by ESA WorldCover consortium"
)
FORMAT = "camsim-landcover-1"

Reader = Callable[[str, int, int], "np.ndarray | None"]


def tile_range(lo: float, hi: float) -> range:
    """Tile indices k whose [0.05 k, 0.05 (k + 1)) interval intersects [lo, hi)."""
    a = math.floor(round(lo / TILE_DEG, 9))
    b = math.ceil(round(hi / TILE_DEG, 9)) - 1
    return range(a, max(a, b) + 1)


def tiles_for_bbox(w: float, s: float, e: float, n: float) -> list[tuple[int, int]]:
    """(lat_index, lon_index) of every tile touching the bbox, south to north, west to east."""
    if not (w < e and s < n):
        raise ValueError(f"bbox must have W < E and S < N, got {w} {s} {e} {n}")
    if not (-180.0 <= w and e <= 180.0 and -90.0 <= s and n <= 90.0):
        raise ValueError(f"bbox outside [-180, 180] x [-90, 90]: {w} {s} {e} {n}")
    return [(i, j) for i in tile_range(s, n) for j in tile_range(w, e)]


def tile_filename(i: int, j: int) -> str:
    return f"{i * TILE_DEG:+.2f}_{j * TILE_DEG:+.2f}.png"


def cog_name(i: int, j: int) -> str:
    """The WorldCover COG holding tile (i, j), named by its 3-degree SW corner (N36W123)."""
    lat = (i // TILES_PER_COG) * 3
    lon = (j // TILES_PER_COG) * 3
    return f"{'N' if lat >= 0 else 'S'}{abs(lat):02d}{'E' if lon >= 0 else 'W'}{abs(lon):03d}"


def cog_offset(i: int, j: int) -> tuple[int, int]:
    """(row_off, col_off) in cells of tile (i, j) inside its COG (row 0 = the COG's north edge)."""
    row = (i // TILES_PER_COG) * TILES_PER_COG + TILES_PER_COG - 1 - i
    col = j - (j // TILES_PER_COG) * TILES_PER_COG
    return row * TILE_PX, col * TILE_PX


def read_cog_window(url: str, row_off: int, col_off: int) -> np.ndarray | None:
    """TILE_PX x TILE_PX uint8 codes from one COG (HTTP range reads), or None when the COG doesn't exist."""
    import rasterio
    from rasterio.errors import RasterioIOError
    from rasterio.windows import Window

    try:
        with rasterio.open(url) as ds:
            if ds.width != 3 * CELLS_PER_DEG or ds.height != 3 * CELLS_PER_DEG or ds.dtypes[0] != "uint8":
                raise SystemExit(f"{url}: unexpected layout {ds.width}x{ds.height} {ds.dtypes[0]}")
            return ds.read(1, window=Window(col_off, row_off, TILE_PX, TILE_PX))
    except RasterioIOError as e:
        if "404" in str(e):
            return None
        raise


def write_tile(codes: np.ndarray, path: Path) -> None:
    if codes.shape != (TILE_PX, TILE_PX) or codes.dtype != np.uint8:
        raise ValueError(f"tile must be {TILE_PX}x{TILE_PX} uint8, got {codes.shape} {codes.dtype}")
    Image.fromarray(codes).save(path, format="PNG", optimize=True)


def build_index(bbox, written: list[tuple[int, int]], missing: list[tuple[int, int]]) -> dict:
    return {
        "format": FORMAT,
        "source": SOURCE,
        "source_url": COG_URL,
        "licence": LICENCE,
        "attribution": ATTRIBUTION,
        "tile_deg": TILE_DEG,
        "tile_px": TILE_PX,
        "bbox": [float(v) for v in bbox],
        "tiles": [{"file": tile_filename(i, j), "lat_index": i, "lon_index": j} for i, j in written],
        "missing": [[i, j] for i, j in missing],
    }


def fetch(bbox, out: Path, reader: Reader = read_cog_window) -> dict:
    out.mkdir(parents=True, exist_ok=True)
    written: list[tuple[int, int]] = []
    missing: list[tuple[int, int]] = []
    for i, j in tiles_for_bbox(*bbox):
        row, col = cog_offset(i, j)
        codes = reader(COG_URL.format(name=cog_name(i, j)), row, col)
        if codes is None:
            missing.append((i, j))
            continue
        write_tile(np.ascontiguousarray(codes, dtype=np.uint8), out / tile_filename(i, j))
        written.append((i, j))
    index = build_index(bbox, written, missing)
    (out / "index.json").write_text(json.dumps(index, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    (out / "ATTRIBUTION.txt").write_text(
        f"{SOURCE}\n{ATTRIBUTION}\nLicence: {LICENCE} (https://creativecommons.org/licenses/by/4.0/)\n"
        "Source: https://esa-worldcover.org (tiles cut by scripts/landcover/fetch_worldcover.py)\n",
        encoding="utf-8",
    )
    return index


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bbox", nargs=4, type=float, metavar=("W", "S", "E", "N"), required=True)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    a = ap.parse_args(argv)
    index = fetch(tuple(a.bbox), a.out)
    print(f"{len(index['tiles'])} tiles written, {len(index['missing'])} missing -> {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Run to verify pass** — Run: `uv run --with pytest --with numpy --with pillow --with pycocotools pytest scripts/tests/test_fetch_worldcover.py -v` → Expected: 8 passed.

- [ ] **Step 5: Generate and commit the SF sample.** Add the LFS pattern to `.gitattributes` (after the `*.DAC` line):

```
unreal_project/CamSimTest/Content/NonUFS/LandCover/*.png filter=lfs diff=lfs merge=lfs -text
```

Then run (network, read-only; about 20 range-read sessions): `uv run scripts/landcover/fetch_worldcover.py --bbox -122.56 37.69 -122.35 37.84` → Expected: `20 tiles written, 0 missing -> .../Content/NonUFS/LandCover`. Check: `ls unreal_project/CamSimTest/Content/NonUFS/LandCover/*.png | wc -l` → 20; `du -sh unreal_project/CamSimTest/Content/NonUFS/LandCover` (record the size in the commit message; expect ≤ 3 MB); `python3 -c "from PIL import Image;import numpy as np;a=np.asarray(Image.open('unreal_project/CamSimTest/Content/NonUFS/LandCover/+37.75_-122.50.png'));print(a.shape, sorted(set(a.ravel())))"` → `(600, 600)` and codes ⊂ {0, 10, 20, 30, 40, 50, 60, 80, 90, 95, 100}.

- [ ] **Step 6: Commit**

```bash
git add .gitattributes scripts/landcover scripts/tests/test_fetch_worldcover.py unreal_project/CamSimTest/Content/NonUFS/LandCover
git lfs ls-files | grep -c 'NonUFS/LandCover/.*\.png'   # expect 20 (the PNGs went to LFS)
git commit -F - <<'EOF'
feat(thermal): WorldCover fetch script and San Francisco land-cover sample (ROADMAP 4B)

scripts/landcover/fetch_worldcover.py cuts ESA WorldCover 2021 v200 COGs (HTTP range reads)
into lossless 0.05 deg 600x600 greyscale PNG tiles + index.json. Sample: bbox -122.56 37.69
-122.35 37.84, 20 tiles in git LFS. Data CC BY 4.0, (c) ESA WorldCover project 2021 /
Contains modified Copernicus Sentinel data (2021) processed by ESA WorldCover consortium.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
EOF
```

---

### Task 2: Tile cache — index, PNG decode, LRU (`FLandCoverTileCache`)

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Thermal/LandCoverTiles.h`, `unreal_project/CamSimTest/Source/CamSimTest/Thermal/LandCoverTiles.cpp`
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Tests/LandCoverTestTiles.h` (test helpers, reused by Task 4)
- Test: `unreal_project/CamSimTest/Source/CamSimTest/Tests/LandCoverTilesTest.cpp`

**Interfaces:**
- Consumes: the Task 1 data contract (`index.json` format `camsim-landcover-1`, 600 × 600 8-bit grey PNGs).
- Produces:
  ```cpp
  struct FLandCoverTile { int32 LatIndex = 0; int32 LonIndex = 0; TArray<uint8> Codes; };   // TilePx^2, row 0 = north
  struct FLandCoverIndex { FString Attribution; FString Licence; TMap<FIntPoint, FString> Files; };  // key (LatIndex, LonIndex)
  class CAMSIMTEST_API FLandCoverTileCache
  {
  public:
      static constexpr int32  TilePx = 600;
      static constexpr double TileDeg = 0.05;
      static constexpr double CellsPerDeg = 12000.0;
      static bool ParseIndex(const FString& Json, FLandCoverIndex& Out, FString& OutError);
      static bool DecodeTilePng(IImageWrapperModule& Module, TConstArrayView<uint8> Png, TArray<uint8>& OutCodes, FString& OutError);
      static FString ResolveDir(const FString& Dir);           // relative -> under FPaths::ProjectDir()
      explicit FLandCoverTileCache(const FString& Dir, int32 MaxTiles = 64);   // game thread
      bool IsValid() const; const FString& GetDir() const; const FString& GetError() const;
      const FLandCoverIndex& GetIndex() const;
      bool HasTile(int32 LatIndex, int32 LonIndex) const;
      TSharedPtr<const FLandCoverTile, ESPMode::ThreadSafe> Get(int32 LatIndex, int32 LonIndex);   // any thread
      TArray<FString> TakeWarnings();                           // any thread
      int32 NumCached() const; int32 NumLoads() const;
  };
  ```
- Produces (tests): `CamSimLandCoverTest::{TempDir, EncodeGreyPng, PatternCodes, LfsPointerBytes, FTestTile, Uniform, WriteTileDir}` in `Tests/LandCoverTestTiles.h`.

- [ ] **Step 1: Write the test helpers** — `Tests/LandCoverTestTiles.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Thermal/LandCoverTiles.h"

/** Synthetic land-cover tile directories for CamSim.Thermal.LandCover.* (ROADMAP 4B). Test code only. */
namespace CamSimLandCoverTest
{
	inline constexpr int32 TilePx = FLandCoverTileCache::TilePx;

	/** An empty directory under Saved/Automation/LandCover (absolute). */
	inline FString TempDir(const TCHAR* Name)
	{
		const FString D = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("LandCover"), Name));
		IFileManager::Get().DeleteDirectory(*D, false, true);
		IFileManager::Get().MakeDirectory(*D, true);
		return D;
	}

	inline TArray64<uint8> EncodeGreyPng(const TArray<uint8>& Codes, int32 W, int32 H)
	{
		IImageWrapperModule& M = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		const TSharedPtr<IImageWrapper> Wr = M.CreateImageWrapper(EImageFormat::PNG);
		Wr->SetRaw(Codes.GetData(), Codes.Num(), W, H, ERGBFormat::Gray, 8);
		return Wr->GetCompressed();
	}

	/** TilePx^2 codes (row + 3 col) % 101: every value 0..100, no two neighbours equal along a row. */
	inline TArray<uint8> PatternCodes()
	{
		TArray<uint8> C;
		C.SetNumUninitialized(TilePx * TilePx);
		for (int32 Y = 0; Y < TilePx; ++Y)
			for (int32 X = 0; X < TilePx; ++X) C[Y * TilePx + X] = static_cast<uint8>((Y + 3 * X) % 101);
		return C;
	}

	/** What a PNG looks like after a clone without `git lfs pull`. */
	inline TArray<uint8> LfsPointerBytes()
	{
		const FTCHARToUTF8 S(TEXT("version https://git-lfs.github.com/spec/v1\noid sha256:0123456789abcdef\nsize 81234\n"));
		return TArray<uint8>(reinterpret_cast<const uint8*>(S.Get()), S.Length());
	}

	struct FTestTile
	{
		int32 LatIndex = 0;
		int32 LonIndex = 0;
		TArray<uint8> Codes;   // TilePx^2
	};

	inline FTestTile Uniform(int32 LatIndex, int32 LonIndex, uint8 Code)
	{
		FTestTile T;
		T.LatIndex = LatIndex;
		T.LonIndex = LonIndex;
		T.Codes.Init(Code, TilePx * TilePx);
		return T;
	}

	inline FString TileFile(int32 LatIndex, int32 LonIndex) { return FString::Printf(TEXT("t_%d_%d.png"), LatIndex, LonIndex); }

	/** PNGs + index.json (format camsim-landcover-1) in Dir. */
	inline void WriteTileDir(const FString& Dir, const TArray<FTestTile>& Tiles)
	{
		FString Entries;
		for (const FTestTile& T : Tiles)
		{
			const FString File = TileFile(T.LatIndex, T.LonIndex);
			FFileHelper::SaveArrayToFile(EncodeGreyPng(T.Codes, TilePx, TilePx), *FPaths::Combine(Dir, File));
			Entries += FString::Printf(TEXT("%s{\"file\":\"%s\",\"lat_index\":%d,\"lon_index\":%d}"),
				Entries.IsEmpty() ? TEXT("") : TEXT(","), *File, T.LatIndex, T.LonIndex);
		}
		FFileHelper::SaveStringToFile(FString::Printf(
			TEXT("{\"format\":\"camsim-landcover-1\",\"tile_deg\":0.05,\"tile_px\":600,\"licence\":\"CC BY 4.0\",")
			TEXT("\"attribution\":\"test tiles\",\"tiles\":[%s]}"), *Entries), *FPaths::Combine(Dir, TEXT("index.json")));
	}
}
```

- [ ] **Step 2: Write the failing tests** — `Tests/LandCoverTilesTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Thermal/LandCoverTiles.h"
#include "Tests/LandCoverTestTiles.h"

// CamSim.Thermal.LandCover.*: tile index parsing, PNG decode, LRU, the committed SF sample (ROADMAP 4B).

using namespace CamSimLandCoverTest;

namespace
{
	const TCHAR* GoodIndex =
		TEXT("{\"format\":\"camsim-landcover-1\",\"tile_deg\":0.05,\"tile_px\":600,\"licence\":\"CC BY 4.0\",")
		TEXT("\"attribution\":\"(c) ESA WorldCover project 2021\",\"tiles\":[")
		TEXT("{\"file\":\"+37.75_-122.50.png\",\"lat_index\":755,\"lon_index\":-2450},")
		TEXT("{\"file\":\"+37.75_-122.45.png\",\"lat_index\":755,\"lon_index\":-2449}]}");

	TConstArrayView<uint8> View(const TArray64<uint8>& A) { return TConstArrayView<uint8>(A.GetData(), static_cast<int32>(A.Num())); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverIndexParseTest, "CamSim.Thermal.LandCover.IndexParse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverIndexParseTest::RunTest(const FString& Parameters)
{
	FLandCoverIndex Index;
	FString Err;
	const FString Good(GoodIndex);
	TestTrue(TEXT("valid index parses"), FLandCoverTileCache::ParseIndex(Good, Index, Err));
	TestEqual(TEXT("two tiles"), Index.Files.Num(), 2);
	const FString* F = Index.Files.Find(FIntPoint(755, -2450));
	TestTrue(TEXT("keyed by (lat_index, lon_index)"), F != nullptr && *F == TEXT("+37.75_-122.50.png"));
	TestEqual(TEXT("licence"), Index.Licence, FString(TEXT("CC BY 4.0")));
	TestTrue(TEXT("attribution"), Index.Attribution.Contains(TEXT("ESA WorldCover")));
	auto Bad = [this](const FString& Json, const TCHAR* Why)
	{
		FLandCoverIndex I;
		FString E;
		TestFalse(Why, FLandCoverTileCache::ParseIndex(Json, I, E));
		TestFalse(*FString::Printf(TEXT("%s: has an error message"), Why), E.IsEmpty());
	};
	Bad(TEXT("not json"), TEXT("garbage"));
	Bad(Good.Replace(TEXT("camsim-landcover-1"), TEXT("camsim-landcover-9")), TEXT("wrong format"));
	Bad(Good.Replace(TEXT("\"tile_px\":600"), TEXT("\"tile_px\":512")), TEXT("wrong tile_px"));
	Bad(Good.Replace(TEXT("\"tile_deg\":0.05"), TEXT("\"tile_deg\":0.1")), TEXT("wrong tile_deg"));
	Bad(Good.Replace(TEXT("+37.75_-122.50.png"), TEXT("../evil.png")), TEXT("path in a file name"));
	Bad(Good.Replace(TEXT("\"lat_index\":755"), TEXT("\"lat_index\":755.5")), TEXT("fractional index"));
	Bad(Good.Replace(TEXT("\"lat_index\":755"), TEXT("\"lat_index\":4000")), TEXT("index out of range"));
	Bad(Good.Replace(TEXT("\"tiles\""), TEXT("\"tilez\"")), TEXT("no tiles array"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverPngDecodeTest, "CamSim.Thermal.LandCover.PngDecode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverPngDecodeTest::RunTest(const FString& Parameters)
{
	IImageWrapperModule& M = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	const TArray<uint8> Codes = PatternCodes();
	TArray<uint8> Out;
	FString Err;
	TestTrue(TEXT("decodes"), FLandCoverTileCache::DecodeTilePng(M, View(EncodeGreyPng(Codes, TilePx, TilePx)), Out, Err));
	TestTrue(TEXT("codes pass through losslessly"), Out == Codes);

	const TArray<uint8> Small(Codes.GetData(), 100 * 100);
	TestFalse(TEXT("wrong size rejected"), FLandCoverTileCache::DecodeTilePng(M, View(EncodeGreyPng(Small, 100, 100)), Out, Err));
	TestTrue(TEXT("expected size in the error"), Err.Contains(TEXT("600")));

	const TSharedPtr<IImageWrapper> W = M.CreateImageWrapper(EImageFormat::PNG);
	TArray<uint8> Rgba;
	Rgba.Init(10, TilePx * TilePx * 4);
	W->SetRaw(Rgba.GetData(), Rgba.Num(), TilePx, TilePx, ERGBFormat::RGBA, 8);
	TestFalse(TEXT("RGBA rejected"), FLandCoverTileCache::DecodeTilePng(M, View(W->GetCompressed()), Out, Err));

	TestFalse(TEXT("LFS pointer rejected"), FLandCoverTileCache::DecodeTilePng(M, LfsPointerBytes(), Out, Err));
	TestTrue(TEXT("LFS hint"), Err.Contains(TEXT("git lfs pull")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverCacheLruTest, "CamSim.Thermal.LandCover.CacheLruAndMissing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverCacheLruTest::RunTest(const FString& Parameters)
{
	const FString Dir = TempDir(TEXT("Lru"));
	TArray<FTestTile> Tiles;
	for (int32 K = 0; K < 6; ++K) Tiles.Add(Uniform(755, -2450 + K, static_cast<uint8>(10 * (K + 1))));
	WriteTileDir(Dir, Tiles);
	FLandCoverTileCache Cache(Dir, 4);
	if (!TestTrue(*FString::Printf(TEXT("valid (%s)"), *Cache.GetError()), Cache.IsValid())) return false;
	for (int32 K = 0; K < 6; ++K)
	{
		const auto T = Cache.Get(755, -2450 + K);
		if (TestTrue(*FString::Printf(TEXT("tile %d loads"), K), T.IsValid()))
		{
			TestEqual(TEXT("its code"), T->Codes[12345], static_cast<uint8>(10 * (K + 1)));
			TestEqual(TEXT("its indices"), FIntPoint(T->LatIndex, T->LonIndex), FIntPoint(755, -2450 + K));
		}
	}
	TestEqual(TEXT("LRU keeps 4"), Cache.NumCached(), 4);
	TestEqual(TEXT("6 decodes"), Cache.NumLoads(), 6);
	Cache.Get(755, -2445);   // most recent: a hit
	TestEqual(TEXT("a hit doesn't decode"), Cache.NumLoads(), 6);
	Cache.Get(755, -2450);   // evicted first: decoded again
	TestEqual(TEXT("an evicted tile decodes again"), Cache.NumLoads(), 7);
	TestFalse(TEXT("not in the index -> null"), Cache.Get(700, 0).IsValid());
	TestFalse(TEXT("HasTile"), Cache.HasTile(700, 0));
	TestEqual(TEXT("no warnings"), Cache.TakeWarnings().Num(), 0);

	FLandCoverTileCache Missing(Dir / TEXT("does_not_exist"));
	TestFalse(TEXT("missing directory -> invalid"), Missing.IsValid());
	TestTrue(TEXT("error names the index path"), Missing.GetError().Contains(TEXT("index.json")));
	TestFalse(TEXT("invalid cache returns null"), Missing.Get(755, -2450).IsValid());
	return true;
}

// Review Focus 1: a clone without `git lfs pull` (or a damaged tile) is reported once and treated as no data.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverCorruptTileTest, "CamSim.Thermal.LandCover.CorruptTileIsMissing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverCorruptTileTest::RunTest(const FString& Parameters)
{
	const FString Dir = TempDir(TEXT("Corrupt"));
	WriteTileDir(Dir, { Uniform(755, -2450, 10), Uniform(755, -2449, 50) });
	FFileHelper::SaveArrayToFile(LfsPointerBytes(), *FPaths::Combine(Dir, TileFile(755, -2450)));
	IFileManager::Get().Delete(*FPaths::Combine(Dir, TileFile(755, -2449)));
	FLandCoverTileCache Cache(Dir);
	TestTrue(TEXT("index still valid"), Cache.IsValid());
	TestFalse(TEXT("pointer file -> null"), Cache.Get(755, -2450).IsValid());
	TestFalse(TEXT("deleted file -> null"), Cache.Get(755, -2449).IsValid());
	const TArray<FString> W = Cache.TakeWarnings();
	TestEqual(TEXT("one warning per bad tile"), W.Num(), 2);
	TestTrue(TEXT("warning names the file and the fix"), W.Num() > 0 && W[0].Contains(TileFile(755, -2450)) && W[0].Contains(TEXT("git lfs pull")));
	Cache.Get(755, -2450);
	Cache.Get(755, -2449);
	TestEqual(TEXT("never retried, never re-warned"), Cache.TakeWarnings().Num(), 0);
	TestEqual(TEXT("no decode counted"), Cache.NumLoads(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverSampleTest, "CamSim.Thermal.LandCover.SanFranciscoSample",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverSampleTest::RunTest(const FString& Parameters)
{
	FLandCoverTileCache Cache(TEXT("Content/NonUFS/LandCover"));
	if (!TestTrue(*FString::Printf(TEXT("committed sample index loads (%s)"), *Cache.GetError()), Cache.IsValid())) return false;
	TestEqual(TEXT("20 tiles cover -122.56 37.69 -122.35 37.84"), Cache.GetIndex().Files.Num(), 20);
	TestTrue(TEXT("CC BY 4.0 attribution"), Cache.GetIndex().Attribution.Contains(TEXT("ESA WorldCover"))
		&& Cache.GetIndex().Licence == TEXT("CC BY 4.0"));
	const auto Presidio = Cache.Get(755, -2450);   // 37.75-37.80 N, 122.50-122.45 W: Presidio, Golden Gate Park, Richmond
	if (!TestTrue(TEXT("Presidio tile decodes (git lfs pull if this fails)"), Presidio.IsValid())) return false;
	int32 Hist[256] = {};
	for (const uint8 C : Presidio->Codes) ++Hist[C];
	TestTrue(TEXT("tree cover present"), Hist[10] > 1000);
	TestTrue(TEXT("built-up present"), Hist[50] > 1000);
	int32 Unknown = 0;
	for (int32 C = 0; C < 256; ++C)
	{
		const bool bWorldCover = C == 0 || C == 95 || (C % 10 == 0 && C <= 100);
		if (!bWorldCover) Unknown += Hist[C];
	}
	TestEqual(TEXT("only WorldCover codes"), Unknown, 0);
	TestEqual(TEXT("no warnings"), Cache.TakeWarnings().Num(), 0);
	return true;
}
```

- [ ] **Step 3: Run to verify failure** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5` → Expected: compile error, `Thermal/LandCoverTiles.h` not found.

- [ ] **Step 4: Implement** — `Thermal/LandCoverTiles.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

class IImageWrapperModule;

/** One 0.05 x 0.05 degree land-cover tile: WorldCover codes, row 0 = north edge, column 0 = west edge (ROADMAP 4B). */
struct FLandCoverTile
{
	int32 LatIndex = 0;     // south edge = 0.05 * LatIndex degrees
	int32 LonIndex = 0;     // west edge  = 0.05 * LonIndex degrees
	TArray<uint8> Codes;    // FLandCoverTileCache::TilePx^2
};

/** index.json of a land-cover directory (scripts/landcover/fetch_worldcover.py). */
struct FLandCoverIndex
{
	FString Attribution;
	FString Licence;
	TMap<FIntPoint, FString> Files;   // (LatIndex, LonIndex) -> file name inside the directory
};

/**
 * Local land-cover tiles (ROADMAP 4B): index.json plus 8-bit greyscale PNGs whose values are WorldCover codes, decoded
 * with ImageWrapper and kept in an LRU of MaxTiles. Get() is thread-safe (the window build calls it on a task thread).
 * A tile listed in the index whose file is missing, not a PNG (e.g. a git LFS pointer) or the wrong size is reported once
 * (TakeWarnings) and treated as no data from then on. No network: only Dir is read.
 */
class CAMSIMTEST_API FLandCoverTileCache
{
public:
	static constexpr int32  TilePx      = 600;       // 0.05 deg at 1/12000 deg per WorldCover cell
	static constexpr double TileDeg     = 0.05;
	static constexpr double CellsPerDeg = 12000.0;

	/** Parses index.json text. Wrong format / tile_deg / tile_px or a malformed entry fails with OutError. */
	static bool ParseIndex(const FString& Json, FLandCoverIndex& Out, FString& OutError);
	/** Decodes a PNG into TilePx^2 codes; false (with OutError) unless it is an 8-bit greyscale PNG of that size. */
	static bool DecodeTilePng(IImageWrapperModule& Module, TConstArrayView<uint8> Png, TArray<uint8>& OutCodes, FString& OutError);
	/** Absolute paths are kept; relative ones are taken from the project directory (Content/NonUFS/... stages as loose files). */
	static FString ResolveDir(const FString& Dir);

	/** Game thread (loads the ImageWrapper module). An unreadable or invalid index leaves the cache invalid (GetError). */
	explicit FLandCoverTileCache(const FString& Dir, int32 MaxTiles = 64);

	bool IsValid() const { return bValid; }
	const FString& GetDir() const { return Dir; }
	const FString& GetError() const { return Error; }
	const FLandCoverIndex& GetIndex() const { return Index; }
	bool HasTile(int32 LatIndex, int32 LonIndex) const { return Index.Files.Contains(FIntPoint(LatIndex, LonIndex)); }

	/** The tile, or null when it is not in the index or failed to load. Any thread. */
	TSharedPtr<const FLandCoverTile, ESPMode::ThreadSafe> Get(int32 LatIndex, int32 LonIndex);
	/** Warnings since the last call (each failed tile once). Any thread. */
	TArray<FString> TakeWarnings();
	int32 NumCached() const;
	/** Decodes so far (cache hits don't count). */
	int32 NumLoads() const;

private:
	struct FEntry
	{
		TSharedPtr<const FLandCoverTile, ESPMode::ThreadSafe> Tile;
		uint64 LastUse = 0;
	};

	FString Dir;
	FString Error;
	bool    bValid = false;
	int32   MaxTiles = 64;
	IImageWrapperModule* ImageWrapper = nullptr;
	FLandCoverIndex Index;

	mutable FCriticalSection Lock;   // guards everything below
	TMap<FIntPoint, FEntry> Cache;
	TSet<FIntPoint> Failed;
	TArray<FString> Warnings;
	uint64 UseCounter = 0;
	int32  Loads = 0;
};
```

`Thermal/LandCoverTiles.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Thermal/LandCoverTiles.h"
#include "Dom/JsonObject.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	const TCHAR* IndexFormat = TEXT("camsim-landcover-1");

	bool GetIndexField(const FJsonObject& O, const TCHAR* Key, int32 Lo, int32 Hi, int32& Out)
	{
		double V = 0.0;
		if (!O.TryGetNumberField(Key, V) || !FMath::IsFinite(V) || V != FMath::FloorToDouble(V) || V < Lo || V > Hi) return false;
		Out = static_cast<int32>(V);
		return true;
	}
}

FString FLandCoverTileCache::ResolveDir(const FString& InDir)
{
	const FString Trimmed = InDir.TrimStartAndEnd();
	if (Trimmed.IsEmpty()) return FString();
	return FPaths::IsRelative(Trimmed) ? FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), Trimmed)) : Trimmed;
}

bool FLandCoverTileCache::ParseIndex(const FString& Json, FLandCoverIndex& Out, FString& OutError)
{
	Out = FLandCoverIndex();
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		OutError = TEXT("index.json is not valid JSON");
		return false;
	}
	FString Format;
	if (!Root->TryGetStringField(TEXT("format"), Format) || Format != IndexFormat)
	{
		OutError = FString::Printf(TEXT("index.json format '%s' is not %s"), *Format, IndexFormat);
		return false;
	}
	double Deg = 0.0, Px = 0.0;
	if (!Root->TryGetNumberField(TEXT("tile_deg"), Deg) || FMath::Abs(Deg - TileDeg) > 1e-9
		|| !Root->TryGetNumberField(TEXT("tile_px"), Px) || Px != static_cast<double>(TilePx))
	{
		OutError = FString::Printf(TEXT("index.json tile_deg / tile_px must be %.2f / %d"), TileDeg, TilePx);
		return false;
	}
	Root->TryGetStringField(TEXT("attribution"), Out.Attribution);
	Root->TryGetStringField(TEXT("licence"), Out.Licence);
	const TArray<TSharedPtr<FJsonValue>>* Tiles = nullptr;
	if (!Root->TryGetArrayField(TEXT("tiles"), Tiles))
	{
		OutError = TEXT("index.json has no tiles array");
		return false;
	}
	for (int32 K = 0; K < Tiles->Num(); ++K)
	{
		const TSharedPtr<FJsonObject>* T = nullptr;
		FString File;
		int32 Lat = 0, Lon = 0;
		const bool bOk = (*Tiles)[K].IsValid() && (*Tiles)[K]->TryGetObject(T) && T && (*T).IsValid()
			&& (*T)->TryGetStringField(TEXT("file"), File) && File.EndsWith(TEXT(".png"))
			&& !File.Contains(TEXT("/")) && !File.Contains(TEXT("\\")) && !File.Contains(TEXT(".."))
			&& GetIndexField(**T, TEXT("lat_index"), -1800, 1799, Lat) && GetIndexField(**T, TEXT("lon_index"), -3600, 3599, Lon);
		if (!bOk)
		{
			OutError = FString::Printf(TEXT("index.json tiles[%d] is malformed"), K);
			return false;
		}
		Out.Files.Add(FIntPoint(Lat, Lon), File);
	}
	return true;
}

bool FLandCoverTileCache::DecodeTilePng(IImageWrapperModule& Module, TConstArrayView<uint8> Png, TArray<uint8>& OutCodes, FString& OutError)
{
	const TSharedPtr<IImageWrapper> W = Module.CreateImageWrapper(EImageFormat::PNG);
	if (!W.IsValid() || Png.Num() == 0 || !W->SetCompressed(Png.GetData(), Png.Num()))
	{
		OutError = TEXT("not a PNG (a git LFS pointer? run git lfs pull)");
		return false;
	}
	if (W->GetWidth() != TilePx || W->GetHeight() != TilePx || W->GetFormat() != ERGBFormat::Gray || W->GetBitDepth() != 8)
	{
		OutError = FString::Printf(TEXT("%lldx%lld, %d-bit: expected a %dx%d 8-bit greyscale PNG"),
			static_cast<long long>(W->GetWidth()), static_cast<long long>(W->GetHeight()), W->GetBitDepth(), TilePx, TilePx);
		return false;
	}
	TArray64<uint8> Raw;
	if (!W->GetRaw(ERGBFormat::Gray, 8, Raw) || Raw.Num() != static_cast<int64>(TilePx) * TilePx)
	{
		OutError = TEXT("PNG decode failed");
		return false;
	}
	OutCodes.SetNumUninitialized(TilePx * TilePx);
	FMemory::Memcpy(OutCodes.GetData(), Raw.GetData(), OutCodes.Num());
	return true;
}

FLandCoverTileCache::FLandCoverTileCache(const FString& InDir, int32 InMaxTiles)
	: Dir(ResolveDir(InDir))
	, MaxTiles(FMath::Max(InMaxTiles, 4))
{
	check(IsInGameThread());
	ImageWrapper = &FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	const FString IndexPath = FPaths::Combine(Dir, TEXT("index.json"));
	FString Json;
	if (Dir.IsEmpty() || !FFileHelper::LoadFileToString(Json, *IndexPath))
	{
		Error = FString::Printf(TEXT("no land-cover index at %s"), *IndexPath);
		return;
	}
	FString ParseError;
	if (!ParseIndex(Json, Index, ParseError))
	{
		Error = IndexPath + TEXT(": ") + ParseError;
		return;
	}
	bValid = true;
}

TSharedPtr<const FLandCoverTile, ESPMode::ThreadSafe> FLandCoverTileCache::Get(int32 LatIndex, int32 LonIndex)
{
	const FIntPoint Key(LatIndex, LonIndex);
	FScopeLock L(&Lock);
	if (FEntry* E = Cache.Find(Key))
	{
		E->LastUse = ++UseCounter;
		return E->Tile;
	}
	const FString* File = Index.Files.Find(Key);
	if (!bValid || !File || Failed.Contains(Key)) return nullptr;

	const FString Path = FPaths::Combine(Dir, *File);
	TArray<uint8> Png, Codes;
	FString Why;
	if (!FFileHelper::LoadFileToArray(Png, *Path, FILEREAD_Silent))
	{
		Why = TEXT("missing (git lfs pull?)");
	}
	else
	{
		DecodeTilePng(*ImageWrapper, Png, Codes, Why);
	}
	if (Codes.Num() != TilePx * TilePx)
	{
		Failed.Add(Key);
		Warnings.Add(FString::Printf(TEXT("land-cover tile %s: %s; treated as no data"), *Path, *Why));
		return nullptr;
	}
	++Loads;
	TSharedRef<FLandCoverTile, ESPMode::ThreadSafe> Tile = MakeShared<FLandCoverTile, ESPMode::ThreadSafe>();
	Tile->LatIndex = LatIndex;
	Tile->LonIndex = LonIndex;
	Tile->Codes = MoveTemp(Codes);
	if (Cache.Num() >= MaxTiles)
	{
		FIntPoint Oldest = FIntPoint::ZeroValue;
		uint64 OldestUse = MAX_uint64;
		for (const TPair<FIntPoint, FEntry>& P : Cache)
		{
			if (P.Value.LastUse < OldestUse) { OldestUse = P.Value.LastUse; Oldest = P.Key; }
		}
		Cache.Remove(Oldest);
	}
	Cache.Add(Key, FEntry{ Tile, ++UseCounter });
	return Tile;
}

TArray<FString> FLandCoverTileCache::TakeWarnings()
{
	FScopeLock L(&Lock);
	return MoveTemp(Warnings);
}

int32 FLandCoverTileCache::NumCached() const
{
	FScopeLock L(&Lock);
	return Cache.Num();
}

int32 FLandCoverTileCache::NumLoads() const
{
	FScopeLock L(&Lock);
	return Loads;
}
```

(`MoveTemp(Warnings)` leaves `Warnings` empty but valid — UE's moved-from `TArray` is empty.)

- [ ] **Step 5: Run to verify pass** — build, then `run_tests CamSim.Thermal.LandCover` → Expected: 5 tests succeeded, 0 failed (`IndexParse`, `PngDecode`, `CacheLruAndMissing`, `CorruptTileIsMissing`, `SanFranciscoSample`).

- [ ] **Step 6: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Thermal/LandCoverTiles.h unreal_project/CamSimTest/Source/CamSimTest/Thermal/LandCoverTiles.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/LandCoverTestTiles.h unreal_project/CamSimTest/Source/CamSimTest/Tests/LandCoverTilesTest.cpp
git commit -F - <<'EOF'
feat(thermal): land-cover tile cache (index.json, PNG decode, LRU) (ROADMAP 4B)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
EOF
```

---

### Task 3: Window geometry — small-area mapping, global cells, resample (`CamSimLandCover::`)

Pure functions only (no files, no threads): the resample maths the window build runs, tested against analytic patterns.

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Thermal/LandCoverGeometry.h`, `unreal_project/CamSimTest/Source/CamSimTest/Thermal/LandCoverGeometry.cpp`
- Test: `unreal_project/CamSimTest/Source/CamSimTest/Tests/LandCoverGeometryTest.cpp`

**Interfaces:**
- Consumes: `FLandCoverTileCache::{TilePx, CellsPerDeg}` (Task 2).
- Produces:
  ```cpp
  namespace CamSimLandCover
  {
      inline constexpr double MaxWindowLatDeg = 89.0;
      inline constexpr int32  GlobalRows = 2160000;   // 180 * 12000
      inline constexpr int32  GlobalCols = 4320000;   // 360 * 12000
      struct FWindowSpec { double CentreLatDeg = 0.0; double CentreLonDeg = 0.0; int32 Texels = 2048; float TexelM = 10.0f; };
      CAMSIMTEST_API double MeridionalRadiusM(double LatDeg);
      CAMSIMTEST_API double PrimeVerticalRadiusM(double LatDeg);
      CAMSIMTEST_API FVector2D GeodeticToWindowEN(const FWindowSpec& W, double LatDeg, double LonDeg);   // (E, N) metres
      CAMSIMTEST_API void WindowENToGeodetic(const FWindowSpec& W, double EastM, double NorthM, double& OutLatDeg, double& OutLonDeg);
      CAMSIMTEST_API FIntPoint GlobalCell(double LatDeg, double LonDeg);                                 // X = column from 180 W, Y = row from 90 N
      CAMSIMTEST_API void CellToTile(FIntPoint Cell, FIntPoint& OutTile, FIntPoint& OutInTile);           // OutTile = (LatIndex, LonIndex), OutInTile = (col, row)
      CAMSIMTEST_API bool IsWindowAllowed(double CentreLatDeg);
      CAMSIMTEST_API bool NeedsRecentre(const FWindowSpec& Current, double CamLatDeg, double CamLonDeg, float RecentreFraction);
      using FTileCodes = TFunctionRef<const uint8*(int32 LatIndex, int32 LonIndex)>;
      CAMSIMTEST_API int64 Resample(const FWindowSpec& W, FTileCodes Tiles, TArray<uint8>& OutCodes);    // returns nonzero texels
  }
  ```

- [ ] **Step 1: Write the failing tests** — `Tests/LandCoverGeometryTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Thermal/LandCoverGeometry.h"
#include "Thermal/LandCoverTiles.h"

// CamSim.Thermal.LandCover.*: window geometry and resample against analytic patterns (ROADMAP 4B).

using namespace CamSimLandCover;

namespace
{
	constexpr int32 Px = FLandCoverTileCache::TilePx;

	FWindowSpec Spec(double Lat, double Lon, int32 Texels, float TexelM = 10.0f)
	{
		FWindowSpec S;
		S.CentreLatDeg = Lat; S.CentreLonDeg = Lon; S.Texels = Texels; S.TexelM = TexelM;
		return S;
	}

	TArray<uint8> TileOf(TFunctionRef<uint8(int32 Col, int32 Row)> Code)
	{
		TArray<uint8> T;
		T.SetNumUninitialized(Px * Px);
		for (int32 R = 0; R < Px; ++R)
			for (int32 C = 0; C < Px; ++C) T[R * Px + C] = Code(C, R);
		return T;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverMappingTest, "CamSim.Thermal.LandCover.SmallAreaMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverMappingTest::RunTest(const FString& Parameters)
{
	TestNearlyEqual(TEXT("M at the equator = a (1 - e^2)"), MeridionalRadiusM(0.0), 6335439.327, 0.01);
	TestNearlyEqual(TEXT("N at the equator = a"), PrimeVerticalRadiusM(0.0), 6378137.0, 0.01);
	TestTrue(TEXT("M < N at 37.8"), MeridionalRadiusM(37.8) < PrimeVerticalRadiusM(37.8));
	const FWindowSpec W = Spec(37.7752, -122.4750, 2048);
	for (const FVector2D EN : { FVector2D(0, 0), FVector2D(5000, -3000), FVector2D(-10240, 10240) })
	{
		double Lat = 0.0, Lon = 0.0;
		WindowENToGeodetic(W, EN.X, EN.Y, Lat, Lon);
		const FVector2D Back = GeodeticToWindowEN(W, Lat, Lon);
		TestNearlyEqual(*FString::Printf(TEXT("E round trip %.0f"), EN.X), Back.X, EN.X, 1e-6);
		TestNearlyEqual(*FString::Printf(TEXT("N round trip %.0f"), EN.Y), Back.Y, EN.Y, 1e-6);
	}
	double Lat = 0.0, Lon = 0.0;
	WindowENToGeodetic(W, 0.0, 1000.0, Lat, Lon);
	TestTrue(TEXT("+N is north"), Lat > W.CentreLatDeg && Lon == W.CentreLonDeg);
	WindowENToGeodetic(W, 1000.0, 0.0, Lat, Lon);
	TestTrue(TEXT("+E is east"), Lon > W.CentreLonDeg && Lat == W.CentreLatDeg);
	const FWindowSpec Dateline = Spec(0.0, 179.99, 2048);
	TestTrue(TEXT("wraps across the antimeridian"), FMath::IsNearlyEqual(GeodeticToWindowEN(Dateline, 0.0, -179.99).X,
		FMath::DegreesToRadians(0.02) * PrimeVerticalRadiusM(0.0), 1e-3));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverCellTest, "CamSim.Thermal.LandCover.GlobalCellAndTile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverCellTest::RunTest(const FString& Parameters)
{
	FIntPoint Tile, In;
	CellToTile(GlobalCell(37.80 - 1e-9, -122.50 + 1e-9), Tile, In);
	TestEqual(TEXT("NW corner tile"), Tile, FIntPoint(755, -2450));
	TestEqual(TEXT("NW corner cell"), In, FIntPoint(0, 0));
	CellToTile(GlobalCell(37.75 + 1e-9, -122.45 - 1e-9), Tile, In);
	TestEqual(TEXT("SE corner tile"), Tile, FIntPoint(755, -2450));
	TestEqual(TEXT("SE corner cell"), In, FIntPoint(Px - 1, Px - 1));
	CellToTile(GlobalCell(10.0, 179.99999), Tile, In);
	TestEqual(TEXT("last column before 180"), Tile.Y, 3599);
	CellToTile(GlobalCell(10.0, -180.0), Tile, In);
	TestEqual(TEXT("first column at -180"), Tile.Y, -3600);
	CellToTile(GlobalCell(10.0, 180.0), Tile, In);
	TestEqual(TEXT("+180 wraps to -180"), Tile.Y, -3600);
	CellToTile(GlobalCell(-89.99999, 0.0), Tile, In);
	TestEqual(TEXT("south pole row clamps"), Tile.X, -1800);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverOrientationTest, "CamSim.Thermal.LandCover.ResampleOrientationAndCentre",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverOrientationTest::RunTest(const FString& Parameters)
{
	// Tile (755, -2450) split at its middle row (37.775 N) and middle column (122.475 W); the window is centred on the split.
	const TArray<uint8> NorthSouth = TileOf([](int32 C, int32 R) { return static_cast<uint8>(R < Px / 2 ? 10 : 50); });
	const TArray<uint8> WestEast   = TileOf([](int32 C, int32 R) { return static_cast<uint8>(C < Px / 2 ? 30 : 60); });
	const FWindowSpec W = Spec(37.775, -122.475, 64);
	TArray<uint8> Out;
	auto Only = [](const TArray<uint8>& T) { return [&T](int32 Lat, int32 Lon) -> const uint8* { return (Lat == 755 && Lon == -2450) ? T.GetData() : nullptr; }; };
	TestEqual(TEXT("every texel has data"), Resample(W, Only(NorthSouth), Out), static_cast<int64>(64 * 64));
	TestEqual(TEXT("size"), Out.Num(), 64 * 64);
	int32 Bad = 0;
	for (int32 Y = 0; Y < 64; ++Y)
		for (int32 X = 0; X < 64; ++X) Bad += Out[Y * 64 + X] != (Y < 32 ? 10 : 50);
	TestEqual(TEXT("row 0 is north: rows < 32 north of the split"), Bad, 0);
	Resample(W, Only(WestEast), Out);
	Bad = 0;
	for (int32 Y = 0; Y < 64; ++Y)
		for (int32 X = 0; X < 64; ++X) Bad += Out[Y * 64 + X] != (X < 32 ? 30 : 60);
	TestEqual(TEXT("column 0 is west: columns < 32 west of the split"), Bad, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverScaleTest, "CamSim.Thermal.LandCover.ResampleScale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverScaleTest::RunTest(const FString& Parameters)
{
	// Stripes every 60 cells (0.005 deg of longitude) everywhere: boundaries at lon = k * 0.005 deg.
	const TArray<uint8> Stripes = TileOf([](int32 C, int32 R) { return static_cast<uint8>((C / 60) % 2 ? 50 : 10); });
	const FWindowSpec W = Spec(37.7752, -122.4750, 512);
	TArray<uint8> Out;
	Resample(W, [&Stripes](int32, int32) -> const uint8* { return Stripes.GetData(); }, Out);
	const int32 Row = 256;
	TArray<double> Boundaries;   // E of every stripe boundary inside the window
	for (int32 K = FMath::FloorToInt32(-122.6 / 0.005); K <= FMath::CeilToInt32(-122.35 / 0.005); ++K)
	{
		const double E = GeodeticToWindowEN(W, W.CentreLatDeg, K * 0.005).X;
		if (E > -2555.0 && E < 2555.0) Boundaries.Add(E);
	}
	int32 Transitions = 0, Misplaced = 0;
	for (int32 X = 1; X < 512; ++X)
	{
		if (Out[Row * 512 + X] == Out[Row * 512 + X - 1]) continue;
		++Transitions;
		const double Lo = (X - 1 + 0.5 - 256) * 10.0, Hi = (X + 0.5 - 256) * 10.0;
		const bool bFound = Boundaries.ContainsByPredicate([Lo, Hi](double E) { return E >= Lo - 1e-6 && E <= Hi + 1e-6; });
		Misplaced += bFound ? 0 : 1;
	}
	TestEqual(TEXT("one transition per analytic boundary (scale)"), Transitions, Boundaries.Num());
	TestEqual(TEXT("each transition between the texels its boundary falls between"), Misplaced, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverMissingTest, "CamSim.Thermal.LandCover.ResampleMissingTilesAreZero",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverMissingTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Twenty = TileOf([](int32, int32) { return static_cast<uint8>(20); });
	const FWindowSpec W = Spec(37.80, -122.475, 64);   // straddles the 755 / 756 tile row boundary
	TArray<uint8> Out;
	const int64 NonZero = Resample(W, [&Twenty](int32 Lat, int32) -> const uint8* { return Lat == 756 ? Twenty.GetData() : nullptr; }, Out);
	TestEqual(TEXT("north half present, south half missing"), NonZero, static_cast<int64>(32 * 64));
	TestEqual(TEXT("north texel"), Out[10 * 64 + 5], static_cast<uint8>(20));
	TestEqual(TEXT("missing tile -> 0"), Out[50 * 64 + 5], static_cast<uint8>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverAntimeridianTest, "CamSim.Thermal.LandCover.ResampleAntimeridian",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverAntimeridianTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> West = TileOf([](int32, int32) { return static_cast<uint8>(10); });
	const TArray<uint8> East = TileOf([](int32, int32) { return static_cast<uint8>(50); });
	const FWindowSpec W = Spec(10.025, 179.999, 64);
	TArray<uint8> Out;
	const int64 NonZero = Resample(W, [&](int32 Lat, int32 Lon) -> const uint8*
	{
		if (Lat != 200) return nullptr;
		return Lon == 3599 ? West.GetData() : (Lon == -3600 ? East.GetData() : nullptr);
	}, Out);
	TestEqual(TEXT("both sides found"), NonZero, static_cast<int64>(64 * 64));
	const double E180 = GeodeticToWindowEN(W, W.CentreLatDeg, 180.0).X;
	int32 Bad = 0;
	for (int32 X = 0; X < 64; ++X)
	{
		const double E = (X + 0.5 - 32) * 10.0;
		Bad += Out[32 * 64 + X] != (E < E180 ? 10 : 50);
	}
	TestEqual(TEXT("west of 180 from tile 3599, east from tile -3600"), Bad, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverRecentreTest, "CamSim.Thermal.LandCover.RecentreAndPole",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverRecentreTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("north pole window refused"), IsWindowAllowed(89.5));
	TestFalse(TEXT("south pole window refused"), IsWindowAllowed(-89.5));
	TestTrue(TEXT("88.9 allowed"), IsWindowAllowed(88.9));
	const FWindowSpec W = Spec(37.7, -122.4, 2048);   // 20.48 km: 25 % = 5120 m
	auto At = [&W](double E, double N) { double Lat = 0.0, Lon = 0.0; WindowENToGeodetic(W, E, N, Lat, Lon); return FVector2D(Lat, Lon); };
	TestFalse(TEXT("5000 m north: stays"), NeedsRecentre(W, At(0, 5000).X, At(0, 5000).Y, 0.25f));
	TestTrue (TEXT("5200 m north: re-centre"), NeedsRecentre(W, At(0, 5200).X, At(0, 5200).Y, 0.25f));
	TestTrue (TEXT("5200 m east: re-centre"), NeedsRecentre(W, At(5200, 0).X, At(5200, 0).Y, 0.25f));
	TestTrue (TEXT("5200 m south-west: re-centre"), NeedsRecentre(W, At(-5200, -100).X, At(-5200, -100).Y, 0.25f));
	const FWindowSpec D = Spec(0.0, 179.99, 2048);
	TestFalse(TEXT("2.2 km across the antimeridian: stays"), NeedsRecentre(D, 0.0, -179.99, 0.25f));
	TestTrue (TEXT("... but re-centres at 5 %"), NeedsRecentre(D, 0.0, -179.99, 0.05f));
	return true;
}
```

- [ ] **Step 2: Run to verify failure** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5` → Expected: compile error, `Thermal/LandCoverGeometry.h` not found.

- [ ] **Step 3: Implement** — `Thermal/LandCoverGeometry.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Geometry of the camera-centred land-cover window (ROADMAP 4B). A window is Texels x Texels texels of TexelM metres on a
 * local East/North grid about its centre; texel (x, y) is at E = (x + 0.5 - Texels/2) TexelM, N = (Texels/2 - y - 0.5) TexelM
 * (row 0 = north). The small-area mapping lat = lat0 + N / M(lat0), lon = lon0 + E / (N(lat0) cos lat0) (radians) is used
 * both by the resample and by FThermalFrameBuilder's camera offset, so the two agree exactly. Pure functions; any thread.
 */
namespace CamSimLandCover
{
	inline constexpr double MaxWindowLatDeg = 89.0;   // windows within 1 deg of a pole fall back to terrain_default
	inline constexpr int32  GlobalRows = 2160000;     // 180 deg * 12000 WorldCover cells per degree
	inline constexpr int32  GlobalCols = 4320000;     // 360 deg * 12000

	struct FWindowSpec
	{
		double CentreLatDeg = 0.0;
		double CentreLonDeg = 0.0;
		int32  Texels = 2048;   // square, even
		float  TexelM = 10.0f;
	};

	/** WGS-84 meridional (M) and prime-vertical (N) radii of curvature, metres. */
	CAMSIMTEST_API double MeridionalRadiusM(double LatDeg);
	CAMSIMTEST_API double PrimeVerticalRadiusM(double LatDeg);
	/** (East, North) metres of a point from the window centre; the longitude difference wraps to (-180, 180]. */
	CAMSIMTEST_API FVector2D GeodeticToWindowEN(const FWindowSpec& W, double LatDeg, double LonDeg);
	/** Inverse of GeodeticToWindowEN (longitude not wrapped; GlobalCell wraps it). */
	CAMSIMTEST_API void WindowENToGeodetic(const FWindowSpec& W, double EastM, double NorthM, double& OutLatDeg, double& OutLonDeg);
	/** Global WorldCover cell: X = column from 180 W (wrapped), Y = row from 90 N (clamped). */
	CAMSIMTEST_API FIntPoint GlobalCell(double LatDeg, double LonDeg);
	/** Tile (LatIndex, LonIndex) holding a global cell, and the cell's (column, row) inside it. */
	CAMSIMTEST_API void CellToTile(FIntPoint Cell, FIntPoint& OutTile, FIntPoint& OutInTile);
	CAMSIMTEST_API bool IsWindowAllowed(double CentreLatDeg);
	/** The camera is more than RecentreFraction of the window size from its centre, East or North. */
	CAMSIMTEST_API bool NeedsRecentre(const FWindowSpec& Current, double CamLatDeg, double CamLonDeg, float RecentreFraction);

	/** A tile's TilePx^2 codes (row 0 = north), or null for no data. */
	using FTileCodes = TFunctionRef<const uint8*(int32 LatIndex, int32 LonIndex)>;
	/** Nearest-cell resample into Texels^2 codes (row 0 = north, column 0 = west); missing tiles give 0. Returns the
	 *  number of nonzero texels. Latitude depends only on the row and longitude only on the column, so each is computed
	 *  once per row / column. */
	CAMSIMTEST_API int64 Resample(const FWindowSpec& W, FTileCodes Tiles, TArray<uint8>& OutCodes);
}
```

`Thermal/LandCoverGeometry.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Thermal/LandCoverGeometry.h"
#include "Thermal/LandCoverTiles.h"

namespace CamSimLandCover
{
	namespace
	{
		constexpr double A = 6378137.0, E2 = 6.69437999014e-3;   // WGS-84
		constexpr int32  Px = FLandCoverTileCache::TilePx;
		constexpr double Cells = FLandCoverTileCache::CellsPerDeg;
		constexpr int32  TileRowsToEquator = GlobalRows / Px / 2;   // 1800
		constexpr int32  TileColsToMeridian = GlobalCols / Px / 2;  // 3600

		double W2(double LatDeg) { const double S = FMath::Sin(FMath::DegreesToRadians(LatDeg)); return 1.0 - E2 * S * S; }
	}

	double MeridionalRadiusM(double LatDeg) { const double W = W2(LatDeg); return A * (1.0 - E2) / (W * FMath::Sqrt(W)); }
	double PrimeVerticalRadiusM(double LatDeg) { return A / FMath::Sqrt(W2(LatDeg)); }

	FVector2D GeodeticToWindowEN(const FWindowSpec& W, double LatDeg, double LonDeg)
	{
		double DLon = FMath::Fmod(LonDeg - W.CentreLonDeg, 360.0);
		if (DLon > 180.0) DLon -= 360.0;
		else if (DLon <= -180.0) DLon += 360.0;
		const double CosPhi = FMath::Cos(FMath::DegreesToRadians(W.CentreLatDeg));
		return FVector2D(FMath::DegreesToRadians(DLon) * PrimeVerticalRadiusM(W.CentreLatDeg) * CosPhi,
			FMath::DegreesToRadians(LatDeg - W.CentreLatDeg) * MeridionalRadiusM(W.CentreLatDeg));
	}

	void WindowENToGeodetic(const FWindowSpec& W, double EastM, double NorthM, double& OutLatDeg, double& OutLonDeg)
	{
		const double CosPhi = FMath::Cos(FMath::DegreesToRadians(W.CentreLatDeg));
		OutLatDeg = W.CentreLatDeg + FMath::RadiansToDegrees(NorthM / MeridionalRadiusM(W.CentreLatDeg));
		OutLonDeg = W.CentreLonDeg + FMath::RadiansToDegrees(EastM / (PrimeVerticalRadiusM(W.CentreLatDeg) * CosPhi));
	}

	FIntPoint GlobalCell(double LatDeg, double LonDeg)
	{
		const int32 Row = FMath::Clamp(FMath::FloorToInt32((90.0 - LatDeg) * Cells), 0, GlobalRows - 1);
		int64 Col = FMath::FloorToInt64((LonDeg + 180.0) * Cells) % GlobalCols;
		if (Col < 0) Col += GlobalCols;
		return FIntPoint(static_cast<int32>(Col), Row);
	}

	void CellToTile(FIntPoint Cell, FIntPoint& OutTile, FIntPoint& OutInTile)
	{
		OutTile = FIntPoint(TileRowsToEquator - 1 - Cell.Y / Px, Cell.X / Px - TileColsToMeridian);
		OutInTile = FIntPoint(Cell.X % Px, Cell.Y % Px);
	}

	bool IsWindowAllowed(double CentreLatDeg) { return FMath::Abs(CentreLatDeg) <= MaxWindowLatDeg; }

	bool NeedsRecentre(const FWindowSpec& Current, double CamLatDeg, double CamLonDeg, float RecentreFraction)
	{
		const FVector2D EN = GeodeticToWindowEN(Current, CamLatDeg, CamLonDeg);
		const double Limit = static_cast<double>(RecentreFraction) * Current.Texels * Current.TexelM;
		return FMath::Abs(EN.X) > Limit || FMath::Abs(EN.Y) > Limit;
	}

	int64 Resample(const FWindowSpec& W, FTileCodes Tiles, TArray<uint8>& OutCodes)
	{
		const int32 N = W.Texels;
		OutCodes.SetNumZeroed(N * N);
		TArray<int32> ColCell, RowCell;
		ColCell.SetNumUninitialized(N);
		RowCell.SetNumUninitialized(N);
		for (int32 X = 0; X < N; ++X)
		{
			double Lat = 0.0, Lon = 0.0;
			WindowENToGeodetic(W, (X + 0.5 - 0.5 * N) * W.TexelM, 0.0, Lat, Lon);
			ColCell[X] = GlobalCell(W.CentreLatDeg, Lon).X;
		}
		for (int32 Y = 0; Y < N; ++Y)
		{
			double Lat = 0.0, Lon = 0.0;
			WindowENToGeodetic(W, 0.0, (0.5 * N - Y - 0.5) * W.TexelM, Lat, Lon);
			RowCell[Y] = GlobalCell(Lat, W.CentreLonDeg).Y;
		}
		int64 NonZero = 0;
		FIntPoint CachedTile(MAX_int32, MAX_int32);
		const uint8* Codes = nullptr;
		for (int32 Y = 0; Y < N; ++Y)
		{
			uint8* Dst = OutCodes.GetData() + static_cast<int64>(Y) * N;
			const int32 LatIndex = TileRowsToEquator - 1 - RowCell[Y] / Px;
			const int32 InRow = RowCell[Y] % Px;
			for (int32 X = 0; X < N; ++X)
			{
				const int32 LonIndex = ColCell[X] / Px - TileColsToMeridian;
				if (LatIndex != CachedTile.X || LonIndex != CachedTile.Y)
				{
					CachedTile = FIntPoint(LatIndex, LonIndex);
					Codes = Tiles(LatIndex, LonIndex);
				}
				if (Codes)
				{
					const uint8 C = Codes[InRow * Px + ColCell[X] % Px];
					Dst[X] = C;
					NonZero += C != 0 ? 1 : 0;
				}
			}
		}
		return NonZero;
	}
}
```

- [ ] **Step 4: Run to verify pass** — build, then `run_tests CamSim.Thermal.LandCover` → Expected: 12 succeeded (Task 2's 5 + `SmallAreaMapping`, `GlobalCellAndTile`, `ResampleOrientationAndCentre`, `ResampleScale`, `ResampleMissingTilesAreZero`, `ResampleAntimeridian`, `RecentreAndPole`), 0 failed.

- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Thermal/LandCoverGeometry.h unreal_project/CamSimTest/Source/CamSimTest/Thermal/LandCoverGeometry.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/LandCoverGeometryTest.cpp
git commit -F - <<'EOF'
feat(thermal): land-cover window geometry and nearest-cell resample (ROADMAP 4B)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
EOF
```

---

### Task 4: Async double-buffered window with GPU texture (`FLandCoverWindow`)

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Thermal/LandCoverWindow.h`, `unreal_project/CamSimTest/Source/CamSimTest/Thermal/LandCoverWindow.cpp`
- Test: `unreal_project/CamSimTest/Source/CamSimTest/Tests/LandCoverWindowTest.cpp`

**Interfaces:**
- Consumes: `FLandCoverTileCache` (Task 2), `CamSimLandCover::{FWindowSpec, Resample, NeedsRecentre, IsWindowAllowed}` (Task 3), `CamSimLandCoverTest::*` (Task 2 helpers).
- Produces:
  ```cpp
  struct FLandCoverWindowData { uint32 Id = 0; CamSimLandCover::FWindowSpec Spec; TArray<uint8> Codes; int64 NonZeroTexels = 0;
                                int32 TilesUsed = 0; int32 TilesMissing = 0; double BuildMs = 0.0; };
  struct FLandCoverGpuWindow { uint32 Id = 0; int32 Texels = 0; FTextureRHIRef Texture; };   // Texture: render thread only
  class CAMSIMTEST_API FLandCoverWindow
  {
  public:
      static constexpr float TexelM = 10.0f;
      struct FSettings { FString Dir; int32 Texels = 2048; float RecentreFraction = 0.25f; int32 MaxCachedTiles = 64;
                         bool bCreateGpuTexture = true; bool operator==(const FSettings&) const = default; };
      ~FLandCoverWindow();
      void Configure(const FSettings& S);                                   // game thread
      void Update(double CamLatDeg, double CamLonDeg);                      // game thread, never blocks
      TSharedPtr<const FLandCoverWindowData, ESPMode::ThreadSafe> GetCurrent() const;
      TSharedPtr<FLandCoverGpuWindow, ESPMode::ThreadSafe> GetCurrentGpu() const;
      bool IsAvailable() const; bool IsBuildInFlight() const;
      void FinishBuildForTest();
      TArray<FString> TakeWarnings();
      uint32 GetBuildsStarted() const;
  };
  ```

- [ ] **Step 1: Write the failing tests** — `Tests/LandCoverWindowTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Thermal/LandCoverWindow.h"
#include "Tests/LandCoverTestTiles.h"

// CamSim.Thermal.LandCover.*: the async, double-buffered window (ROADMAP 4B). GPU uploads are off (NullRHI).

using namespace CamSimLandCoverTest;

namespace
{
	constexpr double CamLat = 37.775, CamLon = -122.45;   // 122.45 W is the boundary between tile columns -2450 and -2449

	/** Rows 753..757 x columns -2452..-2447 (covers a 2048-texel window at the camera): code 10 west of 122.45 W, 50 east. */
	FString WriteSfDir(const TCHAR* Name)
	{
		const FString Dir = TempDir(Name);
		TArray<FTestTile> Tiles;
		for (int32 I = 753; I <= 757; ++I)
			for (int32 J = -2452; J <= -2447; ++J) Tiles.Add(Uniform(I, J, J <= -2450 ? 10 : 50));
		WriteTileDir(Dir, Tiles);
		return Dir;
	}

	FLandCoverWindow::FSettings Settings(const FString& Dir, int32 Texels)
	{
		FLandCoverWindow::FSettings S;
		S.Dir = Dir;
		S.Texels = Texels;
		S.RecentreFraction = 0.25f;
		S.bCreateGpuTexture = false;
		return S;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverWindowSwapTest, "CamSim.Thermal.LandCover.WindowBuildsAsyncAndSwaps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverWindowSwapTest::RunTest(const FString& Parameters)
{
	FLandCoverWindow W;
	W.Configure(Settings(WriteSfDir(TEXT("Swap")), 256));   // 2.56 km: re-centre beyond 640 m
	TestTrue(TEXT("index loaded"), W.IsAvailable());
	W.Update(CamLat, CamLon);
	TestTrue(TEXT("first build started"), W.IsBuildInFlight());
	TestFalse(TEXT("no window until it finishes"), W.GetCurrent().IsValid());
	W.FinishBuildForTest();
	const auto First = W.GetCurrent();
	if (!TestTrue(TEXT("first window published"), First.IsValid())) return false;
	TestEqual(TEXT("id 1"), First->Id, 1u);
	TestEqual(TEXT("size"), First->Codes.Num(), 256 * 256);
	TestEqual(TEXT("west of the boundary: tile code 10"), First->Codes[128 * 256 + 10], static_cast<uint8>(10));
	TestEqual(TEXT("east of the boundary: tile code 50"), First->Codes[128 * 256 + 245], static_cast<uint8>(50));
	TestTrue(TEXT("GPU handle paired"), W.GetCurrentGpu().IsValid() && W.GetCurrentGpu()->Id == 1u && W.GetCurrentGpu()->Texels == 256);
	W.Update(CamLat, CamLon);
	TestEqual(TEXT("same pose: no rebuild"), W.GetBuildsStarted(), 1u);
	double Lat = 0.0, Lon = 0.0;
	CamSimLandCover::WindowENToGeodetic(First->Spec, 0.0, 768.0, Lat, Lon);
	W.Update(Lat, Lon);
	TestTrue(TEXT("768 m north: rebuild started"), W.IsBuildInFlight() || W.GetBuildsStarted() == 2u);
	TestEqual(TEXT("the old window keeps rendering meanwhile"), W.GetCurrent()->Id, 1u);
	W.FinishBuildForTest();
	TestEqual(TEXT("swapped to id 2"), W.GetCurrent()->Id, 2u);
	TestNearlyEqual(TEXT("re-centred on the camera"), W.GetCurrent()->Spec.CentreLatDeg, Lat, 1e-12);
	W.Update(Lat, Lon);
	W.Update(Lat, Lon);
	TestEqual(TEXT("one build per re-centre"), W.GetBuildsStarted(), 2u);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverWindowUnavailableTest, "CamSim.Thermal.LandCover.MissingDirAndPole",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverWindowUnavailableTest::RunTest(const FString& Parameters)
{
	FLandCoverWindow Missing;
	Missing.Configure(Settings(TempDir(TEXT("Empty")) / TEXT("nope"), 256));
	Missing.Update(CamLat, CamLon);
	Missing.Update(CamLat, CamLon);
	TestFalse(TEXT("unavailable"), Missing.IsAvailable());
	TestEqual(TEXT("no build"), Missing.GetBuildsStarted(), 0u);
	TestEqual(TEXT("one warning"), Missing.TakeWarnings().Num(), 1);
	Missing.Update(CamLat, CamLon);
	TestEqual(TEXT("not repeated"), Missing.TakeWarnings().Num(), 0);

	FLandCoverWindow Pole;
	Pole.Configure(Settings(WriteSfDir(TEXT("Pole")), 256));
	Pole.Update(89.5, 0.0);
	Pole.Update(89.6, 0.0);
	TestEqual(TEXT("no window within 1 deg of a pole"), Pole.GetBuildsStarted(), 0u);
	TestEqual(TEXT("one pole warning"), Pole.TakeWarnings().Num(), 1);
	TestFalse(TEXT("no window"), Pole.GetCurrent().IsValid());
	return true;
}

// Review Focus 2: out of the data's coverage -> land cover off with one warning, no rebuild per frame, back when over data.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverWindowNoDataTest, "CamSim.Thermal.LandCover.WindowWithoutDataIsOff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverWindowNoDataTest::RunTest(const FString& Parameters)
{
	FLandCoverWindow W;
	W.Configure(Settings(WriteSfDir(TEXT("NoData")), 256));
	W.Update(0.025, 0.025);
	W.FinishBuildForTest();
	TestFalse(TEXT("all-zero window is not published"), W.GetCurrent().IsValid());
	TestFalse(TEXT("no GPU window either"), W.GetCurrentGpu().IsValid());
	const TArray<FString> Warn = W.TakeWarnings();
	TestTrue(TEXT("one no-data warning"), Warn.Num() == 1 && Warn[0].Contains(TEXT("no land-cover data")));
	W.Update(0.025, 0.025);
	W.Update(0.025, 0.025);
	TestEqual(TEXT("no rebuild per frame"), W.GetBuildsStarted(), 1u);
	W.Update(CamLat, CamLon);
	W.FinishBuildForTest();
	TestTrue(TEXT("back over data: published again"), W.GetCurrent().IsValid() && W.GetCurrent()->NonZeroTexels > 0);
	return true;
}

// Review Focus 3: a hot reload during a build never publishes the stale result.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverWindowReconfigureTest, "CamSim.Thermal.LandCover.ReconfigureDiscardsInFlightBuild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverWindowReconfigureTest::RunTest(const FString& Parameters)
{
	const FString Dir = WriteSfDir(TEXT("Reconfigure"));
	FLandCoverWindow W;
	W.Configure(Settings(Dir, 256));
	W.Update(CamLat, CamLon);
	TestTrue(TEXT("building"), W.IsBuildInFlight());
	W.Configure(Settings(Dir, 128));
	W.FinishBuildForTest();
	TestFalse(TEXT("the 256 build is discarded"), W.GetCurrent().IsValid());
	W.Update(CamLat, CamLon);
	W.FinishBuildForTest();
	if (!TestTrue(TEXT("new build published"), W.GetCurrent().IsValid())) return false;
	TestEqual(TEXT("with the new size"), W.GetCurrent()->Spec.Texels, 128);
	TestEqual(TEXT("codes match"), W.GetCurrent()->Codes.Num(), 128 * 128);
	W.Configure(Settings(Dir, 128));
	TestTrue(TEXT("identical settings keep the window"), W.GetCurrent().IsValid());
	return true;
}

// Review Focus 1 at window level: a bad tile becomes zeros, warned once by path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverWindowCorruptTest, "CamSim.Thermal.LandCover.CorruptTileInWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverWindowCorruptTest::RunTest(const FString& Parameters)
{
	const FString Dir = WriteSfDir(TEXT("CorruptWindow"));
	FFileHelper::SaveArrayToFile(LfsPointerBytes(), *FPaths::Combine(Dir, TileFile(755, -2450)));
	FLandCoverWindow W;
	W.Configure(Settings(Dir, 256));
	W.Update(CamLat, CamLon);
	W.FinishBuildForTest();
	if (!TestTrue(TEXT("published (east half has data)"), W.GetCurrent().IsValid())) return false;
	TestEqual(TEXT("bad tile -> 0"), W.GetCurrent()->Codes[128 * 256 + 10], static_cast<uint8>(0));
	TestEqual(TEXT("good tile kept"), W.GetCurrent()->Codes[128 * 256 + 245], static_cast<uint8>(50));
	const TArray<FString> Warn = W.TakeWarnings();
	TestEqual(TEXT("one warning for the tile"), Warn.FilterByPredicate([](const FString& S) { return S.Contains(TileFile(755, -2450)); }).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverWindowBuildTimeTest, "CamSim.Thermal.LandCover.WindowBuildTime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverWindowBuildTimeTest::RunTest(const FString& Parameters)
{
	FLandCoverWindow W;
	W.Configure(Settings(WriteSfDir(TEXT("BuildTime")), 2048));
	W.Update(CamLat, CamLon);
	W.FinishBuildForTest();
	if (!TestTrue(TEXT("published"), W.GetCurrent().IsValid())) return false;
	const double Ms = W.GetCurrent()->BuildMs;
	AddInfo(FString::Printf(TEXT("2048^2 window: %.1f ms (%d tiles; target < 100 ms)"), Ms, W.GetCurrent()->TilesUsed));
	if (Ms > 100.0) AddWarning(FString::Printf(TEXT("window build over the 100 ms target: %.1f ms"), Ms));
	TestTrue(TEXT("well under a second"), Ms < 1000.0);
	TestEqual(TEXT("every texel covered"), W.GetCurrent()->NonZeroTexels, static_cast<int64>(2048) * 2048);
	TestEqual(TEXT("no tile missing"), W.GetCurrent()->TilesMissing, 0);
	return true;
}
```

- [ ] **Step 2: Run to verify failure** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5` → Expected: compile error, `Thermal/LandCoverWindow.h` not found.

- [ ] **Step 3: Implement** — `Thermal/LandCoverWindow.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RHIResources.h"
#include "Tasks/Task.h"
#include "Thermal/LandCoverGeometry.h"

class FLandCoverTileCache;

/** One built window (CPU), immutable once published (ROADMAP 4B). */
struct FLandCoverWindowData
{
	uint32 Id = 0;                        // 1, 2, ... per FLandCoverWindow; 0 = none
	CamSimLandCover::FWindowSpec Spec;
	TArray<uint8> Codes;                  // Spec.Texels^2 WorldCover codes, row 0 = north
	int64  NonZeroTexels = 0;
	int32  TilesUsed = 0;
	int32  TilesMissing = 0;              // tiles the window touches that have no (usable) data
	double BuildMs = 0.0;
};

/**
 * The GPU side of one window (ROADMAP 4B): an R8_UINT texture created and filled on the render thread by the upload
 * command FLandCoverWindow::Update enqueues before any frame can use it. Ref-counted: the render thread keeps the window a
 * frame's thermal parameters were built against (paired by Id) until it receives the next frame's.
 */
struct FLandCoverGpuWindow
{
	uint32 Id = 0;
	int32  Texels = 0;
	FTextureRHIRef Texture;               // render thread only; null until uploaded (or with bCreateGpuTexture = false)
};

/**
 * Camera-centred land-cover window (ROADMAP 4B). Update (game thread, every thermal tick) harvests a finished build and
 * swaps it in, and starts a new build on a task thread when the camera is more than RecentreFraction of the window from its
 * centre. The previous window keeps rendering until the swap: nothing blocks the game or render thread except the 4 MB
 * texture upload, once per re-centre. No window (missing index, pole, no data in range) means land cover off (4A).
 */
class CAMSIMTEST_API FLandCoverWindow
{
public:
	static constexpr float TexelM = 10.0f;

	struct FSettings
	{
		FString Dir;                       // thermal.land_cover.dir
		int32   Texels = 2048;             // thermal.land_cover.window_texels
		float   RecentreFraction = 0.25f;  // thermal.land_cover.recentre_fraction
		int32   MaxCachedTiles = 64;
		bool    bCreateGpuTexture = true;  // false: CPU only (NullRHI tests)
		bool operator==(const FSettings&) const = default;
	};

	FLandCoverWindow() = default;
	~FLandCoverWindow();
	FLandCoverWindow(const FLandCoverWindow&) = delete;
	FLandCoverWindow& operator=(const FLandCoverWindow&) = delete;

	/** Game thread. New settings drop the windows and the tile cache; a build still in flight is discarded when it lands. */
	void Configure(const FSettings& S);
	/** Game thread, every thermal tick. Never blocks. */
	void Update(double CamLatDeg, double CamLonDeg);

	TSharedPtr<const FLandCoverWindowData, ESPMode::ThreadSafe> GetCurrent() const { return Current; }
	TSharedPtr<FLandCoverGpuWindow, ESPMode::ThreadSafe> GetCurrentGpu() const { return CurrentGpu; }
	bool IsAvailable() const;
	bool IsBuildInFlight() const { return bBuildInFlight; }
	/** Tests: wait for the in-flight build and harvest it as Update would. */
	void FinishBuildForTest();
	/** New warnings (each condition once per Configure / per data loss). */
	TArray<FString> TakeWarnings() { return MoveTemp(Warnings); }
	uint32 GetBuildsStarted() const { return BuildsStarted; }

private:
	using FBuildResult = TSharedPtr<FLandCoverWindowData, ESPMode::ThreadSafe>;

	void StartBuild(double CamLatDeg, double CamLonDeg);
	void Harvest();
	void Publish(const FBuildResult& Data);

	FSettings Settings;
	bool bConfigured = false;
	TSharedPtr<FLandCoverTileCache, ESPMode::ThreadSafe> Cache;
	TSharedPtr<const FLandCoverWindowData, ESPMode::ThreadSafe> Current;
	TSharedPtr<FLandCoverGpuWindow, ESPMode::ThreadSafe> CurrentGpu;
	TOptional<CamSimLandCover::FWindowSpec> LastSpec;   // the last window started (published or not): the recentre box
	UE::Tasks::TTask<FBuildResult> Build;
	bool   bBuildInFlight = false;
	uint32 Generation = 0;        // bumped by Configure
	uint32 BuildGeneration = 0;   // Generation when the in-flight build started
	uint32 NextId = 1;
	uint32 BuildsStarted = 0;
	TArray<FString> Warnings;
	bool bWarnedUnavailable = false;
	bool bWarnedPole = false;
	bool bWarnedNoData = false;
};
```

`Thermal/LandCoverWindow.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Thermal/LandCoverWindow.h"
#include "CamSimTest.h"
#include "HAL/PlatformTime.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Thermal/LandCoverTiles.h"

FLandCoverWindow::~FLandCoverWindow()
{
	if (bBuildInFlight) Build.Wait();
}

bool FLandCoverWindow::IsAvailable() const
{
	return Cache.IsValid() && Cache->IsValid();
}

void FLandCoverWindow::Configure(const FSettings& S)
{
	check(IsInGameThread());
	if (bConfigured && S == Settings) return;
	Settings = S;
	bConfigured = true;
	++Generation;
	Cache = MakeShared<FLandCoverTileCache, ESPMode::ThreadSafe>(S.Dir, S.MaxCachedTiles);
	Current.Reset();
	CurrentGpu.Reset();
	LastSpec.Reset();
	bWarnedUnavailable = bWarnedPole = bWarnedNoData = false;
}

void FLandCoverWindow::Update(double CamLatDeg, double CamLonDeg)
{
	check(bConfigured && IsInGameThread());
	if (bBuildInFlight && Build.IsCompleted()) Harvest();
	if (!IsAvailable())
	{
		if (!bWarnedUnavailable)
		{
			bWarnedUnavailable = true;
			Warnings.Add(FString::Printf(TEXT("land cover off: %s (terrain uses terrain_default)"), Cache.IsValid() ? *Cache->GetError() : TEXT("no cache")));
		}
		return;
	}
	Warnings.Append(Cache->TakeWarnings());
	if (!CamSimLandCover::IsWindowAllowed(CamLatDeg))
	{
		if (!bWarnedPole)
		{
			bWarnedPole = true;
			Warnings.Add(FString::Printf(TEXT("camera within 1 deg of a pole (lat %.3f): land cover off"), CamLatDeg));
		}
		Current.Reset();
		CurrentGpu.Reset();
		LastSpec.Reset();
		return;
	}
	bWarnedPole = false;
	if (bBuildInFlight) return;
	if (!LastSpec.IsSet() || CamSimLandCover::NeedsRecentre(*LastSpec, CamLatDeg, CamLonDeg, Settings.RecentreFraction))
	{
		StartBuild(CamLatDeg, CamLonDeg);
	}
}

void FLandCoverWindow::StartBuild(double CamLatDeg, double CamLonDeg)
{
	CamSimLandCover::FWindowSpec Spec;
	Spec.CentreLatDeg = CamLatDeg;
	Spec.CentreLonDeg = CamLonDeg;
	Spec.Texels = FMath::Max(2, Settings.Texels & ~1);
	Spec.TexelM = TexelM;
	LastSpec = Spec;   // the recentre box moves now: no second build for the same move
	const uint32 Id = NextId++;
	const TSharedPtr<FLandCoverTileCache, ESPMode::ThreadSafe> Tiles = Cache;
	BuildGeneration = Generation;
	bBuildInFlight = true;
	++BuildsStarted;
	Build = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Tiles, Spec, Id]() -> FBuildResult
	{
		const double T0 = FPlatformTime::Seconds();
		const FBuildResult Data = MakeShared<FLandCoverWindowData, ESPMode::ThreadSafe>();
		Data->Id = Id;
		Data->Spec = Spec;
		TMap<FIntPoint, TSharedPtr<const FLandCoverTile, ESPMode::ThreadSafe>> Pinned;   // alive for the whole resample
		Data->NonZeroTexels = CamSimLandCover::Resample(Spec, [&Pinned, &Tiles, &Data](int32 LatIndex, int32 LonIndex) -> const uint8*
		{
			const FIntPoint Key(LatIndex, LonIndex);
			if (const TSharedPtr<const FLandCoverTile, ESPMode::ThreadSafe>* P = Pinned.Find(Key))
			{
				return P->IsValid() ? (*P)->Codes.GetData() : nullptr;
			}
			const TSharedPtr<const FLandCoverTile, ESPMode::ThreadSafe> T = Tiles->Get(LatIndex, LonIndex);
			Pinned.Add(Key, T);
			++(T.IsValid() ? Data->TilesUsed : Data->TilesMissing);
			return T.IsValid() ? T->Codes.GetData() : nullptr;
		}, Data->Codes);
		Data->BuildMs = (FPlatformTime::Seconds() - T0) * 1000.0;
		return Data;
	});
}

void FLandCoverWindow::Harvest()
{
	bBuildInFlight = false;
	const FBuildResult Data = Build.GetResult();
	Build = UE::Tasks::TTask<FBuildResult>();
	if (Cache.IsValid()) Warnings.Append(Cache->TakeWarnings());   // tiles that failed during this build
	if (BuildGeneration != Generation || !Data.IsValid()) return;   // settings changed while it was building
	if (Data->NonZeroTexels == 0)
	{
		if (!bWarnedNoData)
		{
			bWarnedNoData = true;
			Warnings.Add(FString::Printf(TEXT("no land-cover data within %.1f km of (%.5f, %.5f) in %s: terrain uses terrain_default"),
				0.0005 * Data->Spec.Texels * Data->Spec.TexelM, Data->Spec.CentreLatDeg, Data->Spec.CentreLonDeg, *Cache->GetDir()));
		}
		Current.Reset();
		CurrentGpu.Reset();
		return;
	}
	bWarnedNoData = false;
	Publish(Data);
	UE_LOG(LogCamSim, Log, TEXT("LandCover: window #%u at (%.5f, %.5f), %d^2 x %.0f m, built in %.1f ms (%d tiles, %d without data)"),
		Data->Id, Data->Spec.CentreLatDeg, Data->Spec.CentreLonDeg, Data->Spec.Texels, Data->Spec.TexelM, Data->BuildMs,
		Data->TilesUsed, Data->TilesMissing);
}

void FLandCoverWindow::Publish(const FBuildResult& Data)
{
	const TSharedPtr<FLandCoverGpuWindow, ESPMode::ThreadSafe> Gpu = MakeShared<FLandCoverGpuWindow, ESPMode::ThreadSafe>();
	Gpu->Id = Data->Id;
	Gpu->Texels = Data->Spec.Texels;
	if (Settings.bCreateGpuTexture)
	{
		// Enqueued before the render command carrying any frame's parameters that reference this window (same game
		// thread, in order), so the texture exists by the time a frame binds it.
		const TSharedPtr<const FLandCoverWindowData, ESPMode::ThreadSafe> Src = Data;
		ENQUEUE_RENDER_COMMAND(CamSimLandCoverUpload)([Gpu, Src](FRHICommandListImmediate& RHICmdList)
		{
			const int32 N = Src->Spec.Texels;
			const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(TEXT("CamSimLandCover"), N, N, PF_R8_UINT)
				.SetFlags(ETextureCreateFlags::ShaderResource).SetInitialState(ERHIAccess::SRVMask);
			FTextureRHIRef Tex = RHICmdList.CreateTexture(Desc);
			RHICmdList.UpdateTexture2D(Tex, 0, FUpdateTextureRegion2D(0, 0, 0, 0, N, N), N, Src->Codes.GetData());
			Gpu->Texture = Tex;
		});
	}
	Current = Data;      // the previous window (and its texture) lives on while the render thread still holds it
	CurrentGpu = Gpu;
}

void FLandCoverWindow::FinishBuildForTest()
{
	if (!bBuildInFlight) return;
	Build.Wait();
	Harvest();
}
```

- [ ] **Step 4: Run to verify pass** — build, then `run_tests CamSim.Thermal.LandCover` → Expected: 18 succeeded, 0 failed (the 6 new: `WindowBuildsAsyncAndSwaps`, `MissingDirAndPole`, `WindowWithoutDataIsOff`, `ReconfigureDiscardsInFlightBuild`, `CorruptTileInWindow`, `WindowBuildTime`; note the `WindowBuildTime` info line — record the ms in the commit message).

- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Thermal/LandCoverWindow.h unreal_project/CamSimTest/Source/CamSimTest/Thermal/LandCoverWindow.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/LandCoverWindowTest.cpp
git commit -F - <<'EOF'
feat(thermal): async double-buffered land-cover window with R8_UINT texture (ROADMAP 4B)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
EOF
```

---

### Task 5: Land-cover thermal materials and the `snow` temperature source

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalTypes.h` (`EThermalTemperatureSource::Snow`)
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalMaterials.h`, `.cpp` (index constants, eight built-ins, `snow`)
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalModel.h`, `.cpp` (snow cap)
- Modify: `docs/configuration.md` (Thermal section: `thermal.materials.<name>` row, built-in classes table), `docs/thermal.md` ("Classes and keys" table), `deploy/camsim_config.yaml` (materials comment)
- Test: `unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalLandCoverMaterialsTest.cpp`

**Interfaces:**
- Consumes: `FThermalMaterialTable`, `FThermalModel`, `FThermalSite` (4A).
- Produces:
  ```cpp
  enum class EThermalTemperatureSource : uint8 { Model = 0, Water = 1, Snow = 2 };   // Snow: min(model, FThermalModel::SnowMaxK)
  // FThermalMaterialTable index constants (built-in order is fixed):
  static constexpr int32 TerrainDefault = 0, Water = 1, VehiclePaint = 2, Asphalt = 3, Vegetation = 4, Concrete = 5,
      TreeCanopy = 6, Shrubland = 7, Grassland = 8, Cropland = 9, BuiltUp = 10, BareSoil = 11, SnowIce = 12, Wetland = 13;
  static constexpr double FThermalModel::SnowMaxK = 273.15;
  ```

- [ ] **Step 1: Write the failing tests** — `Tests/ThermalLandCoverMaterialsTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Thermal/ThermalMaterials.h"
#include "Thermal/ThermalModel.h"

// CamSim.Thermal.Materials.*: the land-cover classes (ROADMAP 4B) and their diurnal contrasts at San Francisco.

namespace
{
	FThermalSite SanFrancisco(int32 DayOfYear)
	{
		FThermalSite S;
		S.Year = 2026; S.DayOfYear = DayOfYear; S.LatDeg = 37.80; S.LonDeg = -122.45;
		S.TairMeanK = 288.15; S.AirSwingK = 8.0; S.Cloud = 0.0;
		return S;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalLandCoverBuiltInsTest, "CamSim.Thermal.Materials.LandCoverBuiltIns",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalLandCoverBuiltInsTest::RunTest(const FString& Parameters)
{
	const TArray<FThermalMaterial>& B = FThermalMaterialTable::BuiltIns();
	TestEqual(TEXT("14 built-ins"), B.Num(), 14);
	struct FExpect { int32 Index; const TCHAR* Name; };
	const FExpect Expected[] = {
		{ FThermalMaterialTable::TerrainDefault, TEXT("terrain_default") }, { FThermalMaterialTable::Water, TEXT("water") },
		{ FThermalMaterialTable::VehiclePaint, TEXT("vehicle_paint") }, { FThermalMaterialTable::Asphalt, TEXT("asphalt") },
		{ FThermalMaterialTable::Vegetation, TEXT("vegetation") }, { FThermalMaterialTable::Concrete, TEXT("concrete") },
		{ FThermalMaterialTable::TreeCanopy, TEXT("tree_canopy") }, { FThermalMaterialTable::Shrubland, TEXT("shrubland") },
		{ FThermalMaterialTable::Grassland, TEXT("grassland") }, { FThermalMaterialTable::Cropland, TEXT("cropland") },
		{ FThermalMaterialTable::BuiltUp, TEXT("built_up") }, { FThermalMaterialTable::BareSoil, TEXT("bare_soil") },
		{ FThermalMaterialTable::SnowIce, TEXT("snow_ice") }, { FThermalMaterialTable::Wetland, TEXT("wetland") },
	};
	for (const FExpect& E : Expected)
	{
		if (TestTrue(*FString::Printf(TEXT("index %d exists"), E.Index), E.Index < B.Num()))
		{
			TestEqual(*FString::Printf(TEXT("index %d"), E.Index), B[E.Index].Name, FString(E.Name));
		}
	}
	const FThermalMaterialTable T;
	TestEqual(TEXT("Find bare_soil"), T.Find(TEXT("bare_soil")), FThermalMaterialTable::BareSoil);
	EThermalTemperatureSource Src = EThermalTemperatureSource::Model;
	TestTrue(TEXT("snow parses"), FThermalMaterialTable::ParseSource(TEXT("Snow"), Src) && Src == EThermalTemperatureSource::Snow);
	TestTrue(TEXT("snow_ice uses it"), B[FThermalMaterialTable::SnowIce].Source == EThermalTemperatureSource::Snow);
	TestTrue(TEXT("vegetation family keeps its heat in the air: tree h_c > grass h_c"),
		B[FThermalMaterialTable::TreeCanopy].ConvectionWm2K > B[FThermalMaterialTable::Grassland].ConvectionWm2K);
	FThermalMaterialSpec Lava;
	Lava.Name = TEXT("asphalt");
	Lava.Temperature = TEXT("lava");
	const TArray<FString> Errors = FThermalMaterialTable::Validate({ Lava });
	TestTrue(TEXT("error lists the three sources"), Errors.Num() == 1 && Errors[0].Contains(TEXT("model, water or snow")));
	for (const FThermalMaterial& M : B)
	{
		FThermalMaterialSpec S;
		S.Name = M.Name; S.Albedo = M.Albedo; S.Emissivity = M.Emissivity; S.ThermalInertia = M.ThermalInertia;
		S.ConvectionWm2K = M.ConvectionWm2K; S.KFast = M.KFast;
		TestEqual(*FString::Printf(TEXT("%s is within the config ranges"), *M.Name), FThermalMaterialTable::Validate({ S }).Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalSnowTest, "CamSim.Thermal.Materials.SnowNeverAboveFreezing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalSnowTest::RunTest(const FString& Parameters)
{
	const FThermalMaterialTable T;
	FThermalModel M;
	M.Update(SanFrancisco(172), T);   // June: the model alone would be well above 0 C
	double Hi = -1e9, Lo = 1e9;
	for (int32 Min = 0; Min < 1440; Min += 5)
	{
		const double K = M.TemperatureK(FThermalMaterialTable::SnowIce, Min * 60.0, 288.15);
		Hi = FMath::Max(Hi, K);
		Lo = FMath::Min(Lo, K);
	}
	TestTrue(*FString::Printf(TEXT("snow max %.2f K <= 273.15 K"), Hi), Hi <= FThermalModel::SnowMaxK + 1e-9);
	TestTrue(TEXT("terrain_default at noon is above freezing (the cap matters)"),
		M.TemperatureK(FThermalMaterialTable::TerrainDefault, 12.0 * 3600.0, 288.15) > FThermalModel::SnowMaxK);
	FThermalSite Cold = SanFrancisco(15);
	Cold.TairMeanK = 255.0;
	M.Update(Cold, T);
	const double Night = M.TemperatureK(FThermalMaterialTable::SnowIce, 2.0 * 3600.0, 271.0);
	TestTrue(*FString::Printf(TEXT("cold night: snow follows the model below 0 C (%.2f K)"), Night), Night < FThermalModel::SnowMaxK - 1.0);
	return true;
}

// The spec's success criteria (a) and (b) at class level: noon vegetation cooler than built-up/bare, night built-up warmer
// than open vegetation, canopy warmer than grass at night. 21 Dec, Presidio, clear sky.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalLandCoverContrastTest, "CamSim.Thermal.Materials.DiurnalContrastsAtSanFrancisco",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalLandCoverContrastTest::RunTest(const FString& Parameters)
{
	using T = FThermalMaterialTable;
	const T Table;
	FThermalModel M;
	M.Update(SanFrancisco(355), Table);
	const double Noon = 12.0 * 3600.0, Night = 2.0 * 3600.0;
	auto K = [&M](int32 C, double Sec) { return M.TemperatureK(C, Sec, 288.15); };
	FString Line;
	for (int32 C = 0; C < Table.Num(); ++C) Line += FString::Printf(TEXT(" %s %.1f/%.1f"), *Table.Get(C).Name, K(C, Noon), K(C, Night));
	AddInfo(TEXT("noon/02:00 K:") + Line);
	TestTrue(TEXT("noon: asphalt warmer than tree canopy by >= 1 K"), K(T::Asphalt, Noon) - K(T::TreeCanopy, Noon) >= 1.0);
	TestTrue(TEXT("noon: bare soil warmer than tree canopy by >= 1 K"), K(T::BareSoil, Noon) - K(T::TreeCanopy, Noon) >= 1.0);
	TestTrue(TEXT("noon: built-up warmer than tree canopy by >= 1 K"), K(T::BuiltUp, Noon) - K(T::TreeCanopy, Noon) >= 1.0);
	TestTrue(TEXT("night: asphalt warmer than grassland by >= 1 K"), K(T::Asphalt, Night) - K(T::Grassland, Night) >= 1.0);
	TestTrue(TEXT("night: concrete warmer than grassland by >= 1 K"), K(T::Concrete, Night) - K(T::Grassland, Night) >= 1.0);
	TestTrue(TEXT("night: built-up warmer than grassland by >= 1 K"), K(T::BuiltUp, Night) - K(T::Grassland, Night) >= 1.0);
	TestTrue(TEXT("night: tree canopy warmer than grassland by >= 0.5 K"), K(T::TreeCanopy, Night) - K(T::Grassland, Night) >= 0.5);
	return true;
}
```

- [ ] **Step 2: Run to verify failure** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5` → Expected: compile error, `TreeCanopy` / `SnowIce` / `EThermalTemperatureSource::Snow` / `SnowMaxK` are not members.

- [ ] **Step 3: Implement.** `Thermal/ThermalTypes.h` — the enum becomes:

```cpp
/** Where a thermal class's temperature comes from (ROADMAP 4A; Snow: 4B). */
enum class EThermalTemperatureSource : uint8
{
	Model = 0,   // closed-form diurnal response (FThermalModel)
	Water = 1,   // the water temperature (CIGI Maritime Surface / ocean.water_temperature_c) +- 0.5 K diurnal
	Snow  = 2,   // the model's temperature, capped at FThermalModel::SnowMaxK (snow and ice stay at or below 0 C)
};
```

and the `FThermalMaterialSpec::Temperature` comment becomes `// "" (keep), "model", "water", "snow"`.

`Thermal/ThermalMaterials.h` — replace the three index constants with:

```cpp
	static constexpr int32 MaxClasses     = 32;   // == FThermalFrameParams::MaxClasses (static_assert in ThermalFrameBuilder.cpp)
	// Built-in indices (order fixed). 0-5 are ROADMAP 4A's; 6-13 are the land-cover classes (ROADMAP 4B).
	static constexpr int32 TerrainDefault = 0;
	static constexpr int32 Water          = 1;
	static constexpr int32 VehiclePaint   = 2;
	static constexpr int32 Asphalt        = 3;
	static constexpr int32 Vegetation     = 4;
	static constexpr int32 Concrete       = 5;
	static constexpr int32 TreeCanopy     = 6;
	static constexpr int32 Shrubland      = 7;
	static constexpr int32 Grassland      = 8;
	static constexpr int32 Cropland       = 9;
	static constexpr int32 BuiltUp        = 10;
	static constexpr int32 BareSoil       = 11;
	static constexpr int32 SnowIce        = 12;
	static constexpr int32 Wetland        = 13;
```

(and the class comment's "The first three indices are fixed" becomes "Built-in indices are fixed (constants below)").

`Thermal/ThermalMaterials.cpp` — `BuiltIns()` becomes:

```cpp
const TArray<FThermalMaterial>& FThermalMaterialTable::BuiltIns()
{
	// k_fast per the 4A spec (asphalt 0.02, vegetation 0.008, metal paint 0.04). Index order is fixed (see the header).
	// ROADMAP 4B land-cover classes: vegetation albedos are effective values that fold evapotranspiration into the absorbed
	// solar (the model has no latent term); a high h_c keeps canopies near air temperature (cool at noon, warm at night).
	static const TArray<FThermalMaterial> B = {
		MakeMaterial(TEXT("terrain_default"), 0.20f, 0.95f, 1200.0f, 10.0f, 0.015f),
		MakeMaterial(TEXT("water"),           0.06f, 0.98f,    0.0f, 10.0f, 0.0f, EThermalTemperatureSource::Water),
		MakeMaterial(TEXT("vehicle_paint"),   0.30f, 0.90f,  600.0f, 12.0f, 0.04f),
		MakeMaterial(TEXT("asphalt"),         0.10f, 0.95f, 1500.0f, 10.0f, 0.02f),
		MakeMaterial(TEXT("vegetation"),      0.20f, 0.98f,  300.0f, 15.0f, 0.008f),
		MakeMaterial(TEXT("concrete"),        0.35f, 0.92f, 1800.0f, 10.0f, 0.015f),
		MakeMaterial(TEXT("tree_canopy"),     0.40f, 0.98f,  800.0f, 25.0f, 0.004f),
		MakeMaterial(TEXT("shrubland"),       0.30f, 0.97f,  600.0f, 15.0f, 0.008f),
		MakeMaterial(TEXT("grassland"),       0.30f, 0.97f,  300.0f,  8.0f, 0.010f),
		MakeMaterial(TEXT("cropland"),        0.30f, 0.97f,  700.0f, 12.0f, 0.010f),
		MakeMaterial(TEXT("built_up"),        0.20f, 0.93f, 1650.0f, 10.0f, 0.018f),
		MakeMaterial(TEXT("bare_soil"),       0.25f, 0.93f,  900.0f, 10.0f, 0.025f),
		MakeMaterial(TEXT("snow_ice"),        0.75f, 0.99f,  600.0f, 10.0f, 0.005f, EThermalTemperatureSource::Snow),
		MakeMaterial(TEXT("wetland"),         0.40f, 0.98f, 2500.0f, 15.0f, 0.004f),
	};
	return B;
}

bool FThermalMaterialTable::ParseSource(const FString& Name, EThermalTemperatureSource& Out)
{
	if (Name.Equals(TEXT("model"), ESearchCase::IgnoreCase)) { Out = EThermalTemperatureSource::Model; return true; }
	if (Name.Equals(TEXT("water"), ESearchCase::IgnoreCase)) { Out = EThermalTemperatureSource::Water; return true; }
	if (Name.Equals(TEXT("snow"),  ESearchCase::IgnoreCase)) { Out = EThermalTemperatureSource::Snow;  return true; }
	return false;
}
```

and in `ValidateOne` the temperature message becomes:

```cpp
			E.Add(FString::Printf(TEXT("%s.temperature '%s' must be model, water or snow"), *P, *S.Temperature));
```

`Thermal/ThermalModel.h` — add next to `WaterSwingK`:

```cpp
	static constexpr double SnowMaxK     = 273.15;   // snow-source classes never exceed 0 C (ROADMAP 4B)
```

and in `FClass` add `bool bSnow = false;` after `bWater`. `Thermal/ThermalModel.cpp` — in `Update`, after `Out.bWater = ...`:

```cpp
		Out.bSnow   = M.Source == EThermalTemperatureSource::Snow;
```

and the end of `TemperatureK` becomes:

```cpp
	const double T = Response(C.F, C.H, C.Inertia, LocalSolarSec);
	return C.bSnow ? FMath::Min(T, SnowMaxK) : T;
}
```

Docs, same commit: in `docs/configuration.md` "Thermal (`thermal:`)", the `thermal.materials.<name>` row lists the 14 built-ins and `temperature` `model`\|`water`\|`snow`; append to the built-in classes table (and to `docs/thermal.md` "Classes and keys", whose intro becomes "Built-in classes (index order fixed; 6-13 added by 4B land cover)"):

```markdown
| `tree_canopy` | 0.40\* | 0.98 | 800 | 25 | 0.004 | model |
| `shrubland` | 0.30\* | 0.97 | 600 | 15 | 0.008 | model |
| `grassland` | 0.30\* | 0.97 | 300 | 8 | 0.010 | model |
| `cropland` | 0.30\* | 0.97 | 700 | 12 | 0.010 | model |
| `built_up` | 0.20 | 0.93 | 1650 | 10 | 0.018 | model |
| `bare_soil` | 0.25 | 0.93 | 900 | 10 | 0.025 | model |
| `snow_ice` | 0.75 | 0.99 | 600 | 10 | 0.005 | snow (model, capped at 273.15 K) |
| `wetland` | 0.40\* | 0.98 | 2500 | 15 | 0.004 | model |

\* Effective albedo: folds evapotranspiration into the absorbed solar (the model has no latent heat term).
```

In `deploy/camsim_config.yaml`, the commented materials example line `#     temperature: model      # model | water` becomes `#     temperature: model      # model | water | snow`.

- [ ] **Step 4: Run to verify pass** — build, then `run_tests CamSim.Thermal` → Expected: 0 failed (the 3 new `CamSim.Thermal.Materials.*` pass; every 4A `CamSim.Thermal.*` test still passes — the four new classes are appended, so 4A indices are unchanged). Read the `DiurnalContrastsAtSanFrancisco` info line. If a contrast margin fails, retune the new class's values (inertia, h_c, effective albedo), never the margins, and say why in the commit message.

- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalTypes.h unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalMaterials.h \
  unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalMaterials.cpp unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalModel.h \
  unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalModel.cpp unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalLandCoverMaterialsTest.cpp \
  docs/configuration.md docs/thermal.md deploy/camsim_config.yaml
git commit -F - <<'EOF'
feat(thermal): land-cover thermal classes and the snow temperature source (ROADMAP 4B)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
EOF
```

---

### Task 6: `thermal.land_cover` config keys and the code → class table

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalTypes.h` (`FLandCoverClassSpec`)
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Thermal/LandCoverClasses.h`, `unreal_project/CamSimTest/Source/CamSimTest/Thermal/LandCoverClasses.cpp`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.h` (`FThermalConfig::FLandCoverConfig`), `CamSimConfig.cpp` (yaml, env, validation)
- Modify: `deploy/camsim_config.yaml`, `docs/configuration.md`
- Test: `unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalLandCoverConfigTest.cpp`

**Interfaces:**
- Consumes: `FThermalMaterialTable` with the Task 5 built-ins.
- Produces:
  ```cpp
  struct FLandCoverClassSpec { FString Key; int32 Code = -1; FString Material; bool operator==(const FLandCoverClassSpec&) const = default; };
  enum class ELandCoverFamily : uint8 { None = 0, Vegetation = 1, BuiltUp = 2, Bare = 3 };
  namespace CamSimLandCover {
      CAMSIMTEST_API const TCHAR* DefaultMaterialName(uint8 Code);
      CAMSIMTEST_API ELandCoverFamily FamilyForCode(uint8 Code);
      CAMSIMTEST_API TArray<FString> ValidateClassSpecs(const TArray<FLandCoverClassSpec>& Specs);
  }
  struct CAMSIMTEST_API FLandCoverClassTable { uint8 Class[256] = {}; uint8 Family[256] = {};
      TArray<FString> Build(const TArray<FLandCoverClassSpec>& Specs, const FThermalMaterialTable& Materials); };
  struct FCamSimConfig::FThermalConfig::FLandCoverConfig { bool bEnabled = true; FString Dir = TEXT("Content/NonUFS/LandCover");
      int32 WindowTexels = 2048; float RecentreFraction = 0.25f; float VegIndexLo = 0.05f; float VegIndexHi = 0.20f;
      float AsphaltMaxLuma = 0.12f; TArray<FLandCoverClassSpec> Classes; bool operator==(const FLandCoverConfig&) const = default; };
  FLandCoverConfig FCamSimConfig::FThermalConfig::LandCover;
  ```
  Env: `CAMSIM_THERMAL_LAND_COVER_{ENABLED, DIR, WINDOW_TEXELS, RECENTRE_FRACTION, VEG_INDEX_LO, VEG_INDEX_HI, ASPHALT_MAX_LUMA}`.

- [ ] **Step 1: Write the failing tests** — `Tests/ThermalLandCoverConfigTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"
#include "Thermal/LandCoverClasses.h"
#include "Thermal/ThermalMaterials.h"

#include <limits>

// CamSim.Thermal.Config.LandCover*: the thermal.land_cover section and the code -> class table (ROADMAP 4B).

namespace
{
	bool HasError(const TArray<FString>& Errors, const TCHAR* Needle)
	{
		return Errors.ContainsByPredicate([Needle](const FString& E) { return E.Contains(Needle); });
	}

	FLandCoverClassSpec Spec(int32 Code, const TCHAR* Material)
	{
		FLandCoverClassSpec S;
		S.Key = FString::FromInt(Code);
		S.Code = Code;
		S.Material = Material;
		return S;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalLandCoverConfigDefaultsTest, "CamSim.Thermal.Config.LandCoverDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalLandCoverConfigDefaultsTest::RunTest(const FString& Parameters)
{
	const FCamSimConfig::FThermalConfig::FLandCoverConfig L;
	TestTrue (TEXT("enabled"), L.bEnabled);
	TestEqual(TEXT("dir"), L.Dir, FString(TEXT("Content/NonUFS/LandCover")));
	TestEqual(TEXT("window"), L.WindowTexels, 2048);
	TestEqual(TEXT("recentre"), L.RecentreFraction, 0.25f);
	TestEqual(TEXT("veg lo"), L.VegIndexLo, 0.05f);
	TestEqual(TEXT("veg hi"), L.VegIndexHi, 0.20f);
	TestEqual(TEXT("asphalt luma"), L.AsphaltMaxLuma, 0.12f);
	TestEqual(TEXT("no class overrides"), L.Classes.Num(), 0);
	TestFalse(TEXT("defaults valid"), HasError(FCamSimConfig().Validate(), TEXT("land_cover")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalLandCoverConfigYamlTest, "CamSim.Thermal.Config.LandCoverYamlAndEnv",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalLandCoverConfigYamlTest::RunTest(const FString& Parameters)
{
	const FString Yaml = TEXT(
		"thermal:\n"
		"  land_cover:\n"
		"    enabled: false\n"
		"    dir: /data/landcover\n"
		"    window_texels: 1024\n"
		"    recentre_fraction: 0.1\n"
		"    veg_index_lo: 0.02\n"
		"    veg_index_hi: 0.3\n"
		"    asphalt_max_luma: 0.2\n"
		"    classes:\n"
		"      10: vegetation\n"
		"      50: concrete\n");
	FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(Yaml);
	const auto& L = Cfg.Thermal.LandCover;
	TestTrue (TEXT("parsed"), Cfg.bLoadedSuccessfully);
	TestEqual(TEXT("no unknown keys"), Cfg.UnknownYamlKeys.Num(), 0);
	TestFalse(TEXT("enabled"), L.bEnabled);
	TestEqual(TEXT("dir"), L.Dir, FString(TEXT("/data/landcover")));
	TestEqual(TEXT("window"), L.WindowTexels, 1024);
	TestEqual(TEXT("recentre"), L.RecentreFraction, 0.1f);
	TestEqual(TEXT("veg lo"), L.VegIndexLo, 0.02f);
	TestEqual(TEXT("veg hi"), L.VegIndexHi, 0.3f);
	TestEqual(TEXT("luma"), L.AsphaltMaxLuma, 0.2f);
	if (TestEqual(TEXT("two class specs"), L.Classes.Num(), 2))
	{
		TestEqual(TEXT("code"), L.Classes[0].Code, 10);
		TestEqual(TEXT("material"), L.Classes[0].Material, FString(TEXT("vegetation")));
		TestEqual(TEXT("second code"), L.Classes[1].Code, 50);
	}
	TestEqual(TEXT("a non-numeric key keeps Code -1"),
		FCamSimConfig::LoadFromYamlString(TEXT("thermal:\n  land_cover:\n    classes:\n      trees: vegetation\n")).Thermal.LandCover.Classes[0].Code, -1);
	TestTrue(TEXT("a typo is an unknown key"),
		FCamSimConfig::LoadFromYamlString(TEXT("thermal:\n  land_cover:\n    window_texel: 512\n")).UnknownYamlKeys
			.Contains(TEXT("thermal.land_cover.window_texel")));

	const TCHAR* Keys[] = { TEXT("CAMSIM_THERMAL_LAND_COVER_ENABLED"), TEXT("CAMSIM_THERMAL_LAND_COVER_DIR"),
		TEXT("CAMSIM_THERMAL_LAND_COVER_WINDOW_TEXELS"), TEXT("CAMSIM_THERMAL_LAND_COVER_RECENTRE_FRACTION"),
		TEXT("CAMSIM_THERMAL_LAND_COVER_VEG_INDEX_LO"), TEXT("CAMSIM_THERMAL_LAND_COVER_VEG_INDEX_HI"),
		TEXT("CAMSIM_THERMAL_LAND_COVER_ASPHALT_MAX_LUMA") };
	const TCHAR* Values[] = { TEXT("1"), TEXT("Content/Other"), TEXT("512"), TEXT("0.02"), TEXT("0.01"), TEXT("0.4"), TEXT("0.15") };
	for (int32 K = 0; K < UE_ARRAY_COUNT(Keys); ++K) FPlatformMisc::SetEnvironmentVar(Keys[K], Values[K]);
	Cfg = FCamSimConfig::LoadFromYamlString(Yaml);
	for (const TCHAR* K : Keys) FPlatformMisc::SetEnvironmentVar(K, TEXT(""));
	TestTrue (TEXT("env enabled"), Cfg.Thermal.LandCover.bEnabled);
	TestEqual(TEXT("env dir"), Cfg.Thermal.LandCover.Dir, FString(TEXT("Content/Other")));
	TestEqual(TEXT("env window"), Cfg.Thermal.LandCover.WindowTexels, 512);
	TestEqual(TEXT("env recentre"), Cfg.Thermal.LandCover.RecentreFraction, 0.02f);
	TestEqual(TEXT("env veg lo"), Cfg.Thermal.LandCover.VegIndexLo, 0.01f);
	TestEqual(TEXT("env veg hi"), Cfg.Thermal.LandCover.VegIndexHi, 0.4f);
	TestEqual(TEXT("env luma"), Cfg.Thermal.LandCover.AsphaltMaxLuma, 0.15f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalLandCoverConfigValidateTest, "CamSim.Thermal.Config.LandCoverValidate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalLandCoverConfigValidateTest::RunTest(const FString& Parameters)
{
	using FLc = FCamSimConfig::FThermalConfig::FLandCoverConfig;
	const float NaN = std::numeric_limits<float>::quiet_NaN();
	auto With = [](TFunction<void(FLc&)> Set) { FCamSimConfig C; Set(C.Thermal.LandCover); return C.Validate(); };
	TestTrue (TEXT("window 128"),     HasError(With([](FLc& L) { L.WindowTexels = 128; }), TEXT("thermal.land_cover.window_texels")));
	TestTrue (TEXT("window odd"),     HasError(With([](FLc& L) { L.WindowTexels = 1001; }), TEXT("thermal.land_cover.window_texels")));
	TestTrue (TEXT("recentre 0.5"),   HasError(With([](FLc& L) { L.RecentreFraction = 0.5f; }), TEXT("thermal.land_cover.recentre_fraction")));
	TestTrue (TEXT("recentre NaN"),   HasError(With([NaN](FLc& L) { L.RecentreFraction = NaN; }), TEXT("thermal.land_cover.recentre_fraction")));
	TestTrue (TEXT("lo >= hi"),       HasError(With([](FLc& L) { L.VegIndexLo = 0.3f; L.VegIndexHi = 0.3f; }), TEXT("thermal.land_cover.veg_index")));
	TestTrue (TEXT("hi NaN"),         HasError(With([NaN](FLc& L) { L.VegIndexHi = NaN; }), TEXT("thermal.land_cover.veg_index")));
	TestTrue (TEXT("luma 1.5"),       HasError(With([](FLc& L) { L.AsphaltMaxLuma = 1.5f; }), TEXT("thermal.land_cover.asphalt_max_luma")));
	TestTrue (TEXT("empty dir"),      HasError(With([](FLc& L) { L.Dir = TEXT("  "); }), TEXT("thermal.land_cover.dir")));
	TestFalse(TEXT("empty dir is fine when disabled"), HasError(With([](FLc& L) { L.bEnabled = false; L.Dir = TEXT(""); }), TEXT("land_cover")));
	TestTrue (TEXT("key 'trees'"),    HasError(With([](FLc& L) { FLandCoverClassSpec S; S.Key = TEXT("trees"); S.Material = TEXT("vegetation"); L.Classes.Add(S); }),
		TEXT("key 'trees' is not a WorldCover code")));
	TestTrue (TEXT("code 300"),       HasError(With([](FLc& L) { L.Classes.Add(Spec(300, TEXT("vegetation"))); }), TEXT("not a WorldCover code")));
	TestTrue (TEXT("bad name"),       HasError(With([](FLc& L) { L.Classes.Add(Spec(10, TEXT("Tree Canopy"))); }), TEXT("thermal.land_cover.classes.10")));
	TestTrue (TEXT("listed twice"),   HasError(With([](FLc& L) { L.Classes.Add(Spec(10, TEXT("vegetation"))); L.Classes.Add(Spec(10, TEXT("grassland"))); }), TEXT("listed twice")));
	TestFalse(TEXT("valid override"), HasError(With([](FLc& L) { L.Classes.Add(Spec(10, TEXT("vegetation"))); }), TEXT("land_cover")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalLandCoverClassTableTest, "CamSim.Thermal.Config.LandCoverClassTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalLandCoverClassTableTest::RunTest(const FString& Parameters)
{
	using T = FThermalMaterialTable;
	FThermalMaterialTable Materials;
	FLandCoverClassTable Table;
	TestEqual(TEXT("defaults: no warnings"), Table.Build({}, Materials).Num(), 0);
	const int32 Expected[][2] = { { 0, T::TerrainDefault }, { 10, T::TreeCanopy }, { 20, T::Shrubland }, { 30, T::Grassland },
		{ 40, T::Cropland }, { 50, T::BuiltUp }, { 60, T::BareSoil }, { 70, T::SnowIce }, { 80, T::Water }, { 90, T::Wetland },
		{ 95, T::Wetland }, { 100, T::Grassland }, { 200, T::TerrainDefault } };
	for (const auto& E : Expected) TestEqual(*FString::Printf(TEXT("code %d"), E[0]), static_cast<int32>(Table.Class[E[0]]), E[1]);
	TestEqual(TEXT("10 is vegetation family"), Table.Family[10], static_cast<uint8>(ELandCoverFamily::Vegetation));
	TestEqual(TEXT("95 is vegetation family"), Table.Family[95], static_cast<uint8>(ELandCoverFamily::Vegetation));
	TestEqual(TEXT("50 is built-up"), Table.Family[50], static_cast<uint8>(ELandCoverFamily::BuiltUp));
	TestEqual(TEXT("60 is bare"), Table.Family[60], static_cast<uint8>(ELandCoverFamily::Bare));
	TestEqual(TEXT("80 has no refinement"), Table.Family[80], static_cast<uint8>(ELandCoverFamily::None));
	TestEqual(TEXT("0 has no refinement"), Table.Family[0], static_cast<uint8>(ELandCoverFamily::None));

	TestEqual(TEXT("override: no warnings"), Table.Build({ Spec(10, TEXT("vegetation")) }, Materials).Num(), 0);
	TestEqual(TEXT("override applied"), static_cast<int32>(Table.Class[10]), T::Vegetation);
	TestEqual(TEXT("family stays by code"), Table.Family[10], static_cast<uint8>(ELandCoverFamily::Vegetation));
	const TArray<FString> W = Table.Build({ Spec(10, TEXT("lava")) }, Materials);
	TestTrue(TEXT("unknown material warned"), W.Num() == 1 && W[0].Contains(TEXT("lava")));
	TestEqual(TEXT("default kept"), static_cast<int32>(Table.Class[10]), T::TreeCanopy);

	FThermalMaterialSpec Gravel;
	Gravel.Name = TEXT("gravel");
	Materials.Build({ Gravel });
	Table.Build({ Spec(60, TEXT("gravel")) }, Materials);
	TestEqual(TEXT("user material reachable"), static_cast<int32>(Table.Class[60]), Materials.Find(TEXT("gravel")));
	int32 OutOfRange = 0;
	for (int32 C = 0; C < 256; ++C) OutOfRange += Table.Class[C] >= Materials.Num() ? 1 : 0;
	TestEqual(TEXT("every entry is a valid class index"), OutOfRange, 0);
	return true;
}
```

- [ ] **Step 2: Run to verify failure** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5` → Expected: compile error, `Thermal/LandCoverClasses.h` not found / `FLandCoverConfig` is not a member of `FThermalConfig`.

- [ ] **Step 3: Implement.** `Thermal/ThermalTypes.h` — append:

```cpp
/** thermal.land_cover.classes.<code>: the thermal material of one WorldCover code (ROADMAP 4B). */
struct FLandCoverClassSpec
{
	FString Key;          // the yaml key as written (for messages)
	int32   Code = -1;    // 0..255; -1 when the key is not a decimal code (FCamSimConfig::Validate reports it)
	FString Material;     // thermal class name (thermal.materials or a built-in)

	bool operator==(const FLandCoverClassSpec&) const = default;
};
```

`Thermal/LandCoverClasses.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Thermal/ThermalTypes.h"

class FThermalMaterialTable;

/** Base-colour refinement family of a WorldCover code (ROADMAP 4B). Values are FThermalFrameParams::LandCoverFamily* and the
 *  shader's LANDCOVER_FAMILY_* (static_assert in ThermalReference.cpp). */
enum class ELandCoverFamily : uint8
{
	None       = 0,   // 0 no data, 70 snow and ice, 80 water: no refinement
	Vegetation = 1,   // 10 tree, 20 shrub, 30 grass, 40 crop, 90/95 wetland/mangrove, 100 moss: blend toward bare_soil by (1 - v)
	BuiltUp    = 2,   // 50: blend toward vegetation by v; the rest splits asphalt / concrete by base luminance
	Bare       = 3,   // 60: blend toward vegetation by v
};

namespace CamSimLandCover
{
	/** The default thermal material of a WorldCover code (spec table); terrain_default for 0 and unknown codes. */
	CAMSIMTEST_API const TCHAR* DefaultMaterialName(uint8 Code);
	CAMSIMTEST_API ELandCoverFamily FamilyForCode(uint8 Code);
	/** Errors of thermal.land_cover.classes as FCamSimConfig::Validate reports them (codes, names, duplicates). */
	CAMSIMTEST_API TArray<FString> ValidateClassSpecs(const TArray<FLandCoverClassSpec>& Specs);
}

/** WorldCover code -> thermal class index and refinement family: defaults plus thermal.land_cover.classes. Game thread. */
struct CAMSIMTEST_API FLandCoverClassTable
{
	uint8 Class[256]  = {};
	uint8 Family[256] = {};

	/** Rebuilds both tables. A spec naming a material the table doesn't have keeps the default and returns a warning. */
	TArray<FString> Build(const TArray<FLandCoverClassSpec>& Specs, const FThermalMaterialTable& Materials);
};
```

`Thermal/LandCoverClasses.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Thermal/LandCoverClasses.h"
#include "Thermal/ThermalMaterials.h"

namespace CamSimLandCover
{
	const TCHAR* DefaultMaterialName(uint8 Code)
	{
		switch (Code)
		{
		case 10:  return TEXT("tree_canopy");
		case 20:  return TEXT("shrubland");
		case 30:  return TEXT("grassland");
		case 40:  return TEXT("cropland");
		case 50:  return TEXT("built_up");
		case 60:  return TEXT("bare_soil");
		case 70:  return TEXT("snow_ice");
		case 80:  return TEXT("water");
		case 90:  return TEXT("wetland");
		case 95:  return TEXT("wetland");
		case 100: return TEXT("grassland");
		default:  return TEXT("terrain_default");
		}
	}

	ELandCoverFamily FamilyForCode(uint8 Code)
	{
		switch (Code)
		{
		case 10: case 20: case 30: case 40: case 90: case 95: case 100: return ELandCoverFamily::Vegetation;
		case 50: return ELandCoverFamily::BuiltUp;
		case 60: return ELandCoverFamily::Bare;
		default: return ELandCoverFamily::None;
		}
	}

	TArray<FString> ValidateClassSpecs(const TArray<FLandCoverClassSpec>& Specs)
	{
		TArray<FString> Errors;
		TSet<int32> Seen;
		for (const FLandCoverClassSpec& S : Specs)
		{
			if (S.Code < 0 || S.Code > 255)
			{
				Errors.Add(FString::Printf(TEXT("thermal.land_cover.classes: key '%s' is not a WorldCover code 0..255"), *S.Key));
				continue;
			}
			bool bName = !S.Material.IsEmpty();
			for (const TCHAR C : S.Material)
			{
				bName &= (C >= TEXT('a') && C <= TEXT('z')) || (C >= TEXT('0') && C <= TEXT('9')) || C == TEXT('_');
			}
			if (!bName)
			{
				Errors.Add(FString::Printf(TEXT("thermal.land_cover.classes.%d: material '%s' must be lower-case letters, digits or '_'"), S.Code, *S.Material));
			}
			if (Seen.Contains(S.Code))
			{
				Errors.Add(FString::Printf(TEXT("thermal.land_cover.classes.%d is listed twice"), S.Code));
			}
			Seen.Add(S.Code);
		}
		return Errors;
	}
}

TArray<FString> FLandCoverClassTable::Build(const TArray<FLandCoverClassSpec>& Specs, const FThermalMaterialTable& Materials)
{
	TArray<FString> Warnings;
	for (int32 C = 0; C < 256; ++C)
	{
		const int32 I = Materials.Find(CamSimLandCover::DefaultMaterialName(static_cast<uint8>(C)));
		Class[C]  = static_cast<uint8>(I == INDEX_NONE ? FThermalMaterialTable::TerrainDefault : I);
		Family[C] = static_cast<uint8>(CamSimLandCover::FamilyForCode(static_cast<uint8>(C)));
	}
	for (const FLandCoverClassSpec& S : Specs)
	{
		if (S.Code < 0 || S.Code > 255) continue;   // reported by FCamSimConfig::Validate
		const int32 I = Materials.Find(S.Material);
		if (I == INDEX_NONE)
		{
			Warnings.Add(FString::Printf(TEXT("thermal.land_cover.classes.%d: '%s' is not a thermal class; keeping %s"),
				S.Code, *S.Material, CamSimLandCover::DefaultMaterialName(static_cast<uint8>(S.Code))));
			continue;
		}
		Class[S.Code] = static_cast<uint8>(I);
	}
	return Warnings;
}
```

`Config/CamSimConfig.h` — inside `FThermalConfig`, after `Materials`:

```cpp
		/** Terrain thermal classes from land cover (ROADMAP 4B, docs/thermal.md). Every key applies on hot reload. */
		struct FLandCoverConfig
		{
			bool    bEnabled         = true;
			FString Dir              = TEXT("Content/NonUFS/LandCover");   // index.json + tiles; relative to the project directory
			int32   WindowTexels     = 2048;    // camera-centred window, 10 m texels (2048 = 20.48 km)
			float   RecentreFraction = 0.25f;   // rebuild when the camera is this fraction of the window from its centre
			float   VegIndexLo       = 0.05f;   // base-colour excess green where the vegetation weight starts
			float   VegIndexHi       = 0.20f;   // ... and reaches 1
			float   AsphaltMaxLuma   = 0.12f;   // built-up: linear base luminance below which ground is asphalt (soft ramp)
			TArray<FLandCoverClassSpec> Classes;   // WorldCover code -> material overrides (yaml only)

			bool operator==(const FLandCoverConfig&) const = default;
		};
		FLandCoverConfig LandCover;
```

`Config/CamSimConfig.cpp` — add `#include "Thermal/LandCoverClasses.h"` next to the `ThermalMaterials.h` include. In the yaml block, inside `if (YamlHas(Root, "thermal"))`, after the `materials` block:

```cpp
			if (YamlHas(T, "land_cover"))   // ROADMAP 4B
			{
				ryml::ConstNodeRef L = T["land_cover"];
				FCamSimConfig::FThermalConfig::FLandCoverConfig& LC = Cfg.Thermal.LandCover;
				YamlBool  (L, "enabled",           LC.bEnabled);
				YamlString(L, "dir",               LC.Dir);
				YamlInt   (L, "window_texels",     LC.WindowTexels);
				YamlFloat (L, "recentre_fraction", LC.RecentreFraction);
				YamlFloat (L, "veg_index_lo",      LC.VegIndexLo);
				YamlFloat (L, "veg_index_hi",      LC.VegIndexHi);
				YamlFloat (L, "asphalt_max_luma",  LC.AsphaltMaxLuma);
				if (YamlHas(L, "classes"))
				{
					ryml::ConstNodeRef Cs = L["classes"];
					if (Cs.is_map())
					{
						YamlKeysAreData(Cs);   // WorldCover codes
						for (ryml::ConstNodeRef C : Cs)
						{
							FLandCoverClassSpec Spec;
							Spec.Key = RymlToFString(C.key());
							bool bDigits = Spec.Key.Len() > 0 && Spec.Key.Len() <= 3;
							for (const TCHAR Ch : Spec.Key) bDigits &= (Ch >= TEXT('0') && Ch <= TEXT('9'));
							Spec.Code = bDigits ? FCString::Atoi(*Spec.Key) : -1;
							if (C.has_val()) Spec.Material = RymlToFString(C.val());
							LC.Classes.Add(MoveTemp(Spec));
						}
					}
				}
			}
```

In the env block after the 4A thermal lines:

```cpp
	// Land cover (ROADMAP 4B)
	FCamSimConfig::FThermalConfig::FLandCoverConfig& LC = Cfg.Thermal.LandCover;
	LC.bEnabled         = GetEnvBool (TEXT("CAMSIM_THERMAL_LAND_COVER_ENABLED"),           LC.bEnabled);
	LC.Dir              = GetEnv     (TEXT("CAMSIM_THERMAL_LAND_COVER_DIR"),               LC.Dir);
	LC.WindowTexels     = GetEnvInt  (TEXT("CAMSIM_THERMAL_LAND_COVER_WINDOW_TEXELS"),     LC.WindowTexels);
	LC.RecentreFraction = GetEnvFloat(TEXT("CAMSIM_THERMAL_LAND_COVER_RECENTRE_FRACTION"), LC.RecentreFraction);
	LC.VegIndexLo       = GetEnvFloat(TEXT("CAMSIM_THERMAL_LAND_COVER_VEG_INDEX_LO"),      LC.VegIndexLo);
	LC.VegIndexHi       = GetEnvFloat(TEXT("CAMSIM_THERMAL_LAND_COVER_VEG_INDEX_HI"),      LC.VegIndexHi);
	LC.AsphaltMaxLuma   = GetEnvFloat(TEXT("CAMSIM_THERMAL_LAND_COVER_ASPHALT_MAX_LUMA"),  LC.AsphaltMaxLuma);
```

In `Validate`, after `Errors.Append(FThermalMaterialTable::Validate(Thermal.Materials));`:

```cpp
	// Land cover (ROADMAP 4B). Written !(x in range) so NaN is reported too.
	const FThermalConfig::FLandCoverConfig& LC = Thermal.LandCover;
	if (!(LC.WindowTexels >= 256 && LC.WindowTexels <= 8192 && LC.WindowTexels % 2 == 0))
		Errors.Add(FString::Printf(TEXT("thermal.land_cover.window_texels=%d must be even, in [256, 8192]"), LC.WindowTexels));
	if (!(LC.RecentreFraction >= 0.01f && LC.RecentreFraction <= 0.45f))
		Errors.Add(FString::Printf(TEXT("thermal.land_cover.recentre_fraction=%.3f out of range [0.01, 0.45]"), LC.RecentreFraction));
	if (!(LC.VegIndexLo >= -1.0f && LC.VegIndexHi <= 2.0f && LC.VegIndexLo < LC.VegIndexHi))
		Errors.Add(FString::Printf(TEXT("thermal.land_cover.veg_index_lo/hi=%.3f/%.3f must satisfy -1 <= lo < hi <= 2"), LC.VegIndexLo, LC.VegIndexHi));
	if (!(LC.AsphaltMaxLuma >= 0.0f && LC.AsphaltMaxLuma <= 1.0f))
		Errors.Add(FString::Printf(TEXT("thermal.land_cover.asphalt_max_luma=%.3f out of range [0, 1]"), LC.AsphaltMaxLuma));
	if (LC.bEnabled && LC.Dir.TrimStartAndEnd().IsEmpty())
		Errors.Add(TEXT("thermal.land_cover.dir is empty (set it, or thermal.land_cover.enabled: false)"));
	Errors.Append(CamSimLandCover::ValidateClassSpecs(LC.Classes));
```

`deploy/camsim_config.yaml` — after the commented `materials:` example in `thermal:`:

```yaml
  land_cover:                   # ROADMAP 4B: terrain thermal classes from ESA WorldCover (docs/thermal.md)
    enabled: true               # CAMSIM_THERMAL_LAND_COVER_ENABLED — false = 4A (every terrain pixel terrain_default)
    dir: Content/NonUFS/LandCover  # CAMSIM_THERMAL_LAND_COVER_DIR — index.json + tiles (scripts/landcover/fetch_worldcover.py), relative to the project
    window_texels: 2048         # CAMSIM_THERMAL_LAND_COVER_WINDOW_TEXELS — camera-centred window of 10 m texels (2048 = 20.48 km)
    recentre_fraction: 0.25     # CAMSIM_THERMAL_LAND_COVER_RECENTRE_FRACTION — rebuild when the camera is this fraction of the window off-centre
    veg_index_lo: 0.05          # CAMSIM_THERMAL_LAND_COVER_VEG_INDEX_LO — base-colour excess green where the vegetation weight starts
    veg_index_hi: 0.20          # CAMSIM_THERMAL_LAND_COVER_VEG_INDEX_HI — ... and reaches 1
    asphalt_max_luma: 0.12      # CAMSIM_THERMAL_LAND_COVER_ASPHALT_MAX_LUMA — built-up: base luminance below which ground is asphalt
    # classes:                  # WorldCover code -> thermal material (yaml only; defaults in docs/thermal.md)
    #   10: tree_canopy
```

`docs/configuration.md`, Thermal section — add `land_cover` to the yaml example and these rows to the key table:

```markdown
| `thermal.land_cover.enabled` | `CAMSIM_THERMAL_LAND_COVER_ENABLED` | `true` | ROADMAP 4B. Terrain pixels take their thermal class from land cover (ESA WorldCover). `false`: 4A output bit for bit (every terrain pixel `terrain_default`). Live. |
| `thermal.land_cover.dir` | `CAMSIM_THERMAL_LAND_COVER_DIR` | `Content/NonUFS/LandCover` | Directory with `index.json` and the tiles (`scripts/landcover/fetch_worldcover.py`); relative to the project directory (staged as loose files). Missing: one warning, land cover off. Must be non-empty when enabled. Live. |
| `thermal.land_cover.window_texels` | `CAMSIM_THERMAL_LAND_COVER_WINDOW_TEXELS` | `2048` | Side of the camera-centred window in 10 m texels (2048 = 20.48 km; 4 MB). Terrain outside it uses `terrain_default`. Even, `[256, 8192]`. Live. |
| `thermal.land_cover.recentre_fraction` | `CAMSIM_THERMAL_LAND_COVER_RECENTRE_FRACTION` | `0.25` | A new window is built (task thread) when the camera is this fraction of the window size from its centre, East or North. `[0.01, 0.45]`. Live. |
| `thermal.land_cover.veg_index_lo` / `veg_index_hi` | `CAMSIM_THERMAL_LAND_COVER_VEG_INDEX_LO` / `_HI` | `0.05` / `0.20` | Vegetation weight v = saturate((ExG − lo) / (hi − lo)), ExG = (2G − R − B) / (R + G + B) of the linear GBuffer base colour. `-1 <= lo < hi <= 2`. Live. |
| `thermal.land_cover.asphalt_max_luma` | `CAMSIM_THERMAL_LAND_COVER_ASPHALT_MAX_LUMA` | `0.12` | Built-up ground below this linear base luminance is asphalt, above it concrete (0.04-wide soft ramp). `[0, 1]`. Live. |
| `thermal.land_cover.classes.<code>` | *(yaml only)* | see `docs/thermal.md` | WorldCover code (0–255) → thermal material name; an unknown material keeps the default with one warning. |
```

- [ ] **Step 4: Run to verify pass** — build, then `run_tests CamSim.Thermal.Config` → Expected: 0 failed (4 new `LandCover*` tests pass), then `run_tests CamSim.Config` → Expected: 0 failed (`CanonicalConfigHasNoUnknownKeys` sees the new yaml block).

- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalTypes.h unreal_project/CamSimTest/Source/CamSimTest/Thermal/LandCoverClasses.h \
  unreal_project/CamSimTest/Source/CamSimTest/Thermal/LandCoverClasses.cpp unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.h \
  unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.cpp unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalLandCoverConfigTest.cpp \
  deploy/camsim_config.yaml docs/configuration.md
git commit -F - <<'EOF'
feat(config): thermal.land_cover keys and the WorldCover code -> class table (ROADMAP 4B)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
EOF
```

---

### Task 7: `FThermalFrameParams` land-cover fields and the CPU reference (`CamSimThermalRef`)

The reference is the spec of the per-pixel maths: window lookup from the camera-relative world position, bilinear blend of the four texels' materials (class data, not codes), base-colour refinement, and the unchanged 4A path when land cover is off.

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimShaders/Public/ThermalFrameParams.h`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalReference.h`, `unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalReference.cpp`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalTestScene.h` (land-cover scene helpers)
- Test: `unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalLandCoverReferenceTest.cpp`

**Interfaces:**
- Consumes: `ELandCoverFamily` (Task 6), 4A `CamSimThermalRef`.
- Produces (`FThermalFrameParams`, after the solar fast-term block):
  ```cpp
  static constexpr int32 NumLandCoverCodes = 256;
  static constexpr uint8 LandCoverFamilyNone = 0, LandCoverFamilyVegetation = 1, LandCoverFamilyBuiltUp = 2, LandCoverFamilyBare = 3;
  uint8     LandCoverClass[NumLandCoverCodes]  = {};
  uint8     LandCoverFamily[NumLandCoverCodes] = {};
  uint32    bLandCover = 0;            uint32 LandCoverWindowId = 0;
  FVector3f LandCoverEast  = FVector3f(1.0f, 0.0f, 0.0f);
  FVector3f LandCoverNorth = FVector3f(0.0f, -1.0f, 0.0f);
  FVector2f LandCoverCamOffsetM = FVector2f::ZeroVector;
  float     LandCoverTexelM = 10.0f;   uint32 LandCoverTexels = 2048;
  uint32    bLandCoverRefine = 0;
  float     VegIndexLo = 0.05f, VegIndexHi = 0.20f, AsphaltMaxLuma = 0.12f, AsphaltRampLuma = 0.04f;
  uint32    VegetationClass = 4, BareSoilClass = 11, AsphaltClass = 3, ConcreteClass = 5;
  ```
- Produces (`CamSimThermalRef`):
  ```cpp
  struct FLandCoverSample { bool bInside = false; uint8 Codes[4] = {}; float Weights[4] = {}; };   // (x0,y0) (x1,y0) (x0,y1) (x1,y1); y0 = northern row
  float     Lum709(const FVector3f& C);
  FVector3f ClipToWorld(const FThermalFrameParams& P, float Nx, float Ny, float Z);
  FVector4f ClassData(const FThermalFrameParams& P, uint32 Class);                    // (T, eps, k_fast, S_abs,ref), class clamped
  FLandCoverSample SampleLandCover(const FThermalFrameParams& P, const uint8* Codes, const FVector3f& Pw);
  FVector2f RefinementWeights(const FThermalFrameParams& P, const FVector3f& Base);   // (vegetation v, concrete c)
  FVector4f RefinedClassData(const FThermalFrameParams& P, uint8 Code, const FVector2f& Weights, bool bRefine);
  FVector4f BlendLandCover(const FThermalFrameParams& P, const FLandCoverSample& S, const FVector3f& Base, bool bRefine);
  // FPixelSample gains: const uint8* LandCover = nullptr;  FPixelResult gains: bool bLandCover = false;
  // FImages gains: const TArray<uint8>* LandCover = nullptr;   (P.LandCoverTexels^2, row 0 = north)
  ```
- Produces (tests, `Tests/ThermalTestScene.h`): `LcAsphalt..LcBuiltUp` (3..8), `MakeLandCoverParams(ViewRot, W, H, Texels, TexelM, YawDeg)`, `MakeLandCoverCodes(Texels)`, `GroundPoint(P, EastM, NorthM, ZCm)`, `MakeLandCoverScene(W, H, DW, DH, YawDeg)`, `FThermalTestScene::LandCover`, `FThermalTestScene::Images(bool bWithBase, bool bWithLandCover = true)`.

- [ ] **Step 1: Extend the test scene** — in `Tests/ThermalTestScene.h`, add to `FThermalTestScene` (after `Base`):

```cpp
		TArray<uint8> LandCover;       // P.LandCoverTexels^2 window codes (ROADMAP 4B); empty: none bound
```

change `Images` to:

```cpp
		CamSimThermalRef::FImages Images(bool bWithBase, bool bWithLandCover = true) const
		{
			CamSimThermalRef::FImages I;
			I.W = W; I.H = H; I.DepthW = DW; I.DepthH = DH;
			I.SceneColor = &Color; I.SceneDepth = &Depth; I.CustomDepth = &Custom; I.Stencil = &Stencil;
			I.BaseColor = bWithBase ? &Base : nullptr;
			I.LandCover = (bWithLandCover && LandCover.Num() > 0) ? &LandCover : nullptr;
			return I;
		}
```

and append to the namespace:

```cpp
	// ---- ROADMAP 4B land cover ----
	inline constexpr uint32 LcAsphalt = 3, LcVegetation = 4, LcConcrete = 5, LcTree = 6, LcBare = 7, LcBuiltUp = 8;

	/**
	 * MakeParams without the sea, plus land classes 3..8 with distinct values and a Texels^2 window of TexelM-metre texels whose
	 * axes are rotated YawDeg about +Z. Codes: 10 -> tree, 30 -> vegetation (both vegetation family), 50 -> built-up,
	 * 60 -> bare, 80 -> water (no family), everything else -> terrain.
	 */
	inline FThermalFrameParams MakeLandCoverParams(const FRotator& ViewRot, int32 W, int32 H, uint32 Texels, float TexelM, float YawDeg)
	{
		FThermalFrameParams P = MakeParams(ViewRot, W, H);
		P.bWater = 0;
		P.NumClasses = 9;
		auto Set = [&P](uint32 C, float T, float Eps, float K, float S) { P.ClassTempK[C] = T; P.ClassEmissivity[C] = Eps; P.ClassKFast[C] = K; P.ClassSAbsRef[C] = S; };
		Set(LcAsphalt,    310.0f, 0.95f, 0.020f, 420.0f);
		Set(LcVegetation, 296.0f, 0.98f, 0.008f, 380.0f);
		Set(LcConcrete,   304.0f, 0.92f, 0.015f, 300.0f);
		Set(LcTree,       293.0f, 0.98f, 0.004f, 280.0f);
		Set(LcBare,       312.0f, 0.93f, 0.025f, 350.0f);
		Set(LcBuiltUp,    307.0f, 0.93f, 0.018f, 380.0f);
		for (int32 C = 0; C < FThermalFrameParams::NumLandCoverCodes; ++C) { P.LandCoverClass[C] = 0; P.LandCoverFamily[C] = FThermalFrameParams::LandCoverFamilyNone; }
		P.LandCoverClass[10] = LcTree;       P.LandCoverFamily[10] = FThermalFrameParams::LandCoverFamilyVegetation;
		P.LandCoverClass[30] = LcVegetation; P.LandCoverFamily[30] = FThermalFrameParams::LandCoverFamilyVegetation;
		P.LandCoverClass[50] = LcBuiltUp;    P.LandCoverFamily[50] = FThermalFrameParams::LandCoverFamilyBuiltUp;
		P.LandCoverClass[60] = LcBare;       P.LandCoverFamily[60] = FThermalFrameParams::LandCoverFamilyBare;
		P.LandCoverClass[80] = 1;            // water class
		P.VegetationClass = LcVegetation; P.BareSoilClass = LcBare; P.AsphaltClass = LcAsphalt; P.ConcreteClass = LcConcrete;
		P.bLandCover = 1;
		P.LandCoverWindowId = 7;
		const float Yaw = FMath::DegreesToRadians(YawDeg);
		P.LandCoverEast  = FVector3f(FMath::Cos(Yaw), FMath::Sin(Yaw), 0.0f);
		P.LandCoverNorth = FVector3f(FMath::Sin(Yaw), -FMath::Cos(Yaw), 0.0f);   // UE +Y is south at the georeference origin
		P.LandCoverCamOffsetM = FVector2f(1.7f, -2.3f);
		P.LandCoverTexelM = TexelM;
		P.LandCoverTexels = Texels;
		P.bLandCoverRefine = 1;
		return P;
	}

	/** Quadrants: north-west 10, north-east 50, south-west 60, south-east 30; then row 0 (north edge) = 80, column 0 (west edge) = 0. */
	inline TArray<uint8> MakeLandCoverCodes(uint32 Texels)
	{
		TArray<uint8> C;
		C.SetNumUninitialized(Texels * Texels);
		const uint32 Half = Texels / 2;
		for (uint32 Y = 0; Y < Texels; ++Y)
		{
			for (uint32 X = 0; X < Texels; ++X)
			{
				uint8 V = Y < Half ? (X < Half ? 10 : 50) : (X < Half ? 60 : 30);
				if (Y == 0) V = 80;
				if (X == 0) V = 0;
				C[Y * Texels + X] = V;
			}
		}
		return C;
	}

	/** Camera-relative (translated) world position, cm, of window coordinates (EastM, NorthM) at height ZCm. */
	inline FVector3f GroundPoint(const FThermalFrameParams& P, float EastM, float NorthM, float ZCm)
	{
		return P.LandCoverEast * ((EastM - P.LandCoverCamOffsetM.X) * 100.0f) + P.LandCoverNorth * ((NorthM - P.LandCoverCamOffsetM.Y) * 100.0f)
			+ FVector3f(0.0f, 0.0f, ZCm);
	}

	/**
	 * Camera 7 m above flat terrain, pitched -50 deg. A 32^2 window of 0.4 m texels (MakeLandCoverCodes) whose centre is in view,
	 * its far edge crossed by the footprint (outside pixels exist). Base colour by U: green, green->grey ramp (intermediate v),
	 * dark grey (asphalt), bright grey (concrete). One visible entity (stencil 3). Sunlit colour (fast term active).
	 */
	inline FThermalTestScene MakeLandCoverScene(int32 W, int32 H, int32 DW, int32 DH, float YawDeg)
	{
		FThermalTestScene S;
		S.W = W; S.H = H; S.DW = DW; S.DH = DH;
		S.ViewRot = FRotator(-50.0, 0.0, 0.0);
		S.P = MakeLandCoverParams(S.ViewRot, W, H, 32, 0.4f, YawDeg);
		S.P.LandCoverCamOffsetM = FVector2f(-4.5f, 0.3f);
		S.LandCover = MakeLandCoverCodes(32);
		S.Depth.Init(0.0f, DW * DH);
		S.Custom.Init(0.0f, DW * DH);
		S.Stencil.Init(0, DW * DH);
		S.Base.Init(FLinearColor(0.2f, 0.2f, 0.2f, 1.0f), DW * DH);
		const FLinearColor Green(0.05f, 0.20f, 0.04f, 1.0f), Grey(0.2f, 0.2f, 0.2f, 1.0f);
		for (int32 Ty = 0; Ty < DH; ++Ty)
		{
			for (int32 Tx = 0; Tx < DW; ++Tx)
			{
				const int32 I = Ty * DW + Tx;
				const float U = (Tx + 0.5f) / DW, V = (Ty + 0.5f) / DH;
				const float Z = DeviceZForPlane(S.P, S.ViewRot, U, V, -700.0f);
				S.Depth[I] = Z;
				if (Z <= 0.0f) continue;
				if (U < 0.25f)      S.Base[I] = Green;
				else if (U < 0.5f)  S.Base[I] = FMath::Lerp(Green, Grey, V);
				else if (U < 0.75f) S.Base[I] = FLinearColor(0.06f, 0.06f, 0.06f, 1.0f);
				else                S.Base[I] = FLinearColor(0.40f, 0.40f, 0.42f, 1.0f);
				if (InBox(U, V, 0.45f, 0.55f, 0.80f, 0.90f)) { S.Stencil[I] = 3; S.Custom[I] = Z; }
			}
		}
		const float Lit = SunlitLuminance(S.P, 0.2f);
		S.Color.Init(FLinearColor(Lit, Lit, Lit, 1.0f), W * H);
		return S;
	}
```

- [ ] **Step 2: Write the failing tests** — `Tests/ThermalLandCoverReferenceTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Tests/ThermalTestScene.h"

#include <limits>

// CamSim.Thermal.Reference.LandCover* / Refinement*: land-cover lookup, bilinear material blend, base-colour refinement and the
// unchanged 4A path in the per-pixel CPU reference (ROADMAP 4B).

using namespace CamSimThermalTest;
using CamSimThermalRef::EPixelClass;
using CamSimThermalRef::FLandCoverSample;
using CamSimThermalRef::FPixelResult;
using CamSimThermalRef::FPixelSample;

namespace
{
	const FRotator Down(-50.0, 0.0, 0.0);

	TArray<uint8> Uniform(uint32 Texels, uint8 Code) { TArray<uint8> C; C.Init(Code, Texels * Texels); return C; }

	FVector4f Data(const FThermalFrameParams& P, uint32 C) { return CamSimThermalRef::ClassData(P, C); }

	bool Near4(const FVector4f& A, const FVector4f& B, float Tol)
	{
		return FMath::Abs(A.X - B.X) <= Tol && FMath::Abs(A.Y - B.Y) <= Tol && FMath::Abs(A.Z - B.Z) <= Tol && FMath::Abs(A.W - B.W) <= Tol;
	}

	/** Refined class data of a single-code window at its centre. */
	FVector4f Refined(const FThermalFrameParams& P, uint8 Code, const FVector3f& Base, bool bRefine = true)
	{
		const TArray<uint8> Codes = Uniform(P.LandCoverTexels, Code);
		const FLandCoverSample L = CamSimThermalRef::SampleLandCover(P, Codes.GetData(), GroundPoint(P, 0.0f, 0.0f, -700.0f));
		return CamSimThermalRef::BlendLandCover(P, L, Base, bRefine);
	}

	/** What Run samples for output pixel (X, Y). */
	FPixelSample SampleAt(const FThermalTestScene& S, int32 X, int32 Y, bool bBase, const uint8* LandCover)
	{
		FPixelSample Smp;
		Smp.U = (X + 0.5f) / S.W;
		Smp.V = (Y + 0.5f) / S.H;
		const int32 Tx = FMath::Clamp(FMath::FloorToInt32(Smp.U * S.DW), 0, S.DW - 1);
		const int32 Ty = FMath::Clamp(FMath::FloorToInt32(Smp.V * S.DH), 0, S.DH - 1);
		const int32 Ti = Ty * S.DW + Tx;
		Smp.DeviceZ = S.Depth[Ti];
		Smp.CustomZ = S.Custom[Ti];
		Smp.Stencil = S.Stencil[Ti];
		const FLinearColor& C = S.Color[Y * S.W + X];
		Smp.Color = FVector3f(C.R, C.G, C.B) * S.P.InputScale;
		if (bBase) { Smp.Base = FVector3f(S.Base[Ti].R, S.Base[Ti].G, S.Base[Ti].B); Smp.bHasBase = true; }
		Smp.LandCover = LandCover;
		return Smp;
	}

	/** ThermalReference.cpp's EvaluatePixel radiance as of 4A (git 3e8dcc0), frozen: land cover off must reproduce it bit for bit. */
	float Legacy4ARadiance(const FThermalFrameParams& P, const FPixelSample& S)
	{
		using namespace CamSimThermalRef;
		const float BAir = LutRadiance(P, P.TairK);
		if (!FMath::IsFinite(S.DeviceZ) || !FMath::IsFinite(S.Color.X) || !FMath::IsFinite(S.Color.Y) || !FMath::IsFinite(S.Color.Z)) return BAir;
		const float Nx = S.U * 2.0f - 1.0f, Ny = 1.0f - S.V * 2.0f;
		if (S.DeviceZ <= 0.0f)
		{
			const FVector3f Pn = ClipToWorld(P, Nx, Ny, 1.0f);
			const float Len = FMath::Sqrt(FVector3f::DotProduct(Pn, Pn));
			const float SinEl = (Len > 0.0f) ? FVector3f::DotProduct(Pn, P.Up) / Len : 1.0f;
			return LutRadiance(P, SkyTemperatureK(P, SinEl));
		}
		const FVector3f Pw = ClipToWorld(P, Nx, Ny, S.DeviceZ);
		const float Range = FMath::Sqrt(FVector3f::DotProduct(Pw, Pw));
		uint32 Class = P.TerrainClass;
		float Offset = 0.0f;
		const uint32 Stencil = S.Stencil & 0xFFu;
		if (Stencil > 0u && S.CustomZ >= S.DeviceZ * P.EntityDepthRatio)
		{
			Class = P.StencilClass[Stencil];
			Offset = P.StencilOffsetK[Stencil];
		}
		else if (P.bWater != 0u)
		{
			const float Vert = FVector3f::DotProduct(Pw, P.Up);
			const float D2 = FMath::Max(Range * Range - Vert * Vert, 0.0f);
			const float Height = P.CamHeightCm + Vert + D2 / (2.0f * P.SeaRadiusCm);
			if (Height < P.WaterBandCm) Class = P.WaterClass;
		}
		Class = FMath::Min(Class, FMath::Max(P.NumClasses, 1u) - 1u);
		float T = P.ClassTempK[Class] + Offset;
		if (S.bHasBase && P.KFastScale > 0.0f)
		{
			const FVector3f Base = (P.bBaseColorSrgb != 0u)
				? FVector3f(SrgbToLinear(S.Base.X), SrgbToLinear(S.Base.Y), SrgbToLinear(S.Base.Z)) : S.Base;
			const float BaseLum = Lum709(Base);
			const float E = FMath::Clamp(3.14159265f * Lum709(S.Color) / (FMath::Max(BaseLum, 0.03f) * FMath::Max(P.KLum, 1e-6f)), 0.0f, P.EClampWm2);
			const float SAbs = (1.0f - FMath::Clamp(BaseLum, 0.0f, 1.0f)) * E;
			T += P.KFastScale * P.ClassKFast[Class] * (SAbs - P.ClassSAbsRef[Class]);
		}
		const float Eps = P.ClassEmissivity[Class];
		const float LSurf = Eps * LutRadiance(P, T) + (1.0f - Eps) * P.SkyHemiRadiance;
		const float Tau = FMath::Exp(-P.BetaPerCm * Range);
		return Tau * LSurf + (1.0f - Tau) * BAir;
	}

	bool SameBits(float A, float B) { return FMemory::Memcmp(&A, &B, sizeof(float)) == 0; }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefLandCoverBlendTest, "CamSim.Thermal.Reference.LandCoverBilinearBlend",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefLandCoverBlendTest::RunTest(const FString& Parameters)
{
	FThermalFrameParams P = MakeLandCoverParams(Down, 64, 36, 8, 10.0f, 0.0f);
	P.bLandCoverRefine = 0;
	P.LandCoverCamOffsetM = FVector2f::ZeroVector;   // exact texel coordinates below
	TArray<uint8> Codes = Uniform(8, 30);   // columns 0..3 vegetation, 4..7 built-up
	for (int32 Y = 0; Y < 8; ++Y) for (int32 X = 4; X < 8; ++X) Codes[Y * 8 + X] = 50;
	auto At = [&](float E, float N) { return CamSimThermalRef::SampleLandCover(P, Codes.GetData(), GroundPoint(P, E, N, -700.0f)); };
	const FLandCoverSample Mid = At(0.0f, 0.0f);   // halfway between texel centres x = 3 (E = -5 m) and x = 4 (+5 m)
	if (!TestTrue(TEXT("inside"), Mid.bInside)) return false;
	TestNearlyEqual(TEXT("weights sum to 1"), Mid.Weights[0] + Mid.Weights[1] + Mid.Weights[2] + Mid.Weights[3], 1.0f, 1e-6f);
	const FVector4f Half = CamSimThermalRef::BlendLandCover(P, Mid, FVector3f(0.2f), false);
	TestNearlyEqual(TEXT("temperature: half vegetation, half built-up"), Half.X, 0.5f * 296.0f + 0.5f * 307.0f, 1e-3f);
	TestNearlyEqual(TEXT("emissivity blended, not the code"), Half.Y, 0.5f * 0.98f + 0.5f * 0.93f, 1e-5f);
	TestNearlyEqual(TEXT("k_fast blended"), Half.Z, 0.5f * 0.008f + 0.5f * 0.018f, 1e-6f);
	TestNearlyEqual(TEXT("S_abs,ref blended"), Half.W, 380.0f, 1e-3f);
	const FVector4f Quarter = CamSimThermalRef::BlendLandCover(P, At(-2.5f, 0.0f), FVector3f(0.2f), false);
	TestNearlyEqual(TEXT("a quarter texel toward vegetation"), Quarter.X, 0.75f * 296.0f + 0.25f * 307.0f, 1e-3f);
	const FLandCoverSample Centre = At(-5.0f, 5.0f);   // exactly texel (3, 3)'s centre
	TestNearlyEqual(TEXT("texel centre: weight 1 on it"), Centre.Weights[0], 1.0f, 1e-6f);
	TestEqual(TEXT("texel centre: its class"), CamSimThermalRef::BlendLandCover(P, Centre, FVector3f(0.2f), false).X, 296.0f);

	// Through EvaluatePixel: the terrain class data is exactly the blend at the pixel's world position.
	P.KFastScale = 0.0f;
	FPixelSample S;
	S.U = 0.5f; S.V = 0.6f;
	S.DeviceZ = DeviceZForPlane(P, Down, S.U, S.V, -700.0f);
	S.LandCover = Codes.GetData();
	const FVector3f Pw = CamSimThermalRef::ClipToWorld(P, S.U * 2.0f - 1.0f, 1.0f - S.V * 2.0f, S.DeviceZ);
	const FVector4f Expect = CamSimThermalRef::BlendLandCover(P, CamSimThermalRef::SampleLandCover(P, Codes.GetData(), Pw), FVector3f(0.0f), false);
	const FPixelResult R = CamSimThermalRef::EvaluatePixel(P, S);
	TestTrue(TEXT("terrain pixel used land cover"), R.bLandCover && R.Class == EPixelClass::Terrain);
	TestEqual(TEXT("its temperature is the blend's"), R.TempK, Expect.X);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefLandCoverOrientationTest, "CamSim.Thermal.Reference.LandCoverOrientation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefLandCoverOrientationTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Codes = MakeLandCoverCodes(8);   // NW 10, NE 50, SW 60, SE 30
	for (const float Yaw : { 0.0f, 30.0f, -117.0f })
	{
		const FThermalFrameParams P = MakeLandCoverParams(Down, 64, 36, 8, 10.0f, Yaw);
		struct FCase { float E, N; uint8 Code; const TCHAR* Name; };
		for (const FCase& C : { FCase{ 20.0f, 20.0f, 50, TEXT("north-east") }, FCase{ -20.0f, 20.0f, 10, TEXT("north-west") },
			FCase{ -20.0f, -20.0f, 60, TEXT("south-west") }, FCase{ 20.0f, -20.0f, 30, TEXT("south-east") } })
		{
			const FLandCoverSample L = CamSimThermalRef::SampleLandCover(P, Codes.GetData(), GroundPoint(P, C.E, C.N, -700.0f));
			const bool bAll = L.bInside && L.Codes[0] == C.Code && L.Codes[1] == C.Code && L.Codes[2] == C.Code && L.Codes[3] == C.Code;
			TestTrue(*FString::Printf(TEXT("yaw %.0f: %s quadrant"), Yaw, C.Name), bAll);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefRefineVegTest, "CamSim.Thermal.Reference.RefinementVegetationBothDirections",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefRefineVegTest::RunTest(const FString& Parameters)
{
	const FThermalFrameParams P = MakeLandCoverParams(Down, 64, 36, 8, 10.0f, 0.0f);
	const FVector3f Green(0.05f, 0.20f, 0.04f), Grey(0.2f), Half(0.2f, 0.24f, 0.2f);   // ExG ~1.07, 0, 0.125
	TestEqual(TEXT("green: v = 1"), CamSimThermalRef::RefinementWeights(P, Green).X, 1.0f);
	TestEqual(TEXT("grey: v = 0"), CamSimThermalRef::RefinementWeights(P, Grey).X, 0.0f);
	TestNearlyEqual(TEXT("ExG 0.125: v = 0.5"), CamSimThermalRef::RefinementWeights(P, Half).X, 0.5f, 1e-3f);
	TestTrue(TEXT("built-up + green (street trees, lawns) -> vegetation"), Near4(Refined(P, 50, Green), Data(P, LcVegetation), 1e-4f));
	TestTrue(TEXT("tree cover + grey (trail, clearing) -> bare soil"), Near4(Refined(P, 10, Grey), Data(P, LcBare), 1e-4f));
	TestTrue(TEXT("tree cover + green -> tree"), Near4(Refined(P, 10, Green), Data(P, LcTree), 1e-4f));
	TestTrue(TEXT("bare + green -> vegetation"), Near4(Refined(P, 60, Green), Data(P, LcVegetation), 1e-4f));
	TestTrue(TEXT("bare + grey -> bare"), Near4(Refined(P, 60, Grey), Data(P, LcBare), 1e-4f));
	const FVector4f Mix = Refined(P, 10, Half);
	TestNearlyEqual(TEXT("tree cover at v = 0.5: half tree, half bare"), Mix.X, 0.5f * 293.0f + 0.5f * 312.0f, 0.05f);
	TestTrue(TEXT("water code is never refined"), Near4(Refined(P, 80, Grey), Data(P, 1), 1e-4f));
	TestTrue(TEXT("no-data code is never refined"), Near4(Refined(P, 0, Green), Data(P, 0), 1e-4f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefRefineSplitTest, "CamSim.Thermal.Reference.RefinementAsphaltConcreteSplit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefRefineSplitTest::RunTest(const FString& Parameters)
{
	const FThermalFrameParams P = MakeLandCoverParams(Down, 64, 36, 8, 10.0f, 0.0f);
	TestTrue(TEXT("dark built-up -> asphalt"), Near4(Refined(P, 50, FVector3f(0.05f)), Data(P, LcAsphalt), 1e-4f));
	TestTrue(TEXT("bright built-up -> concrete"), Near4(Refined(P, 50, FVector3f(0.40f)), Data(P, LcConcrete), 1e-4f));
	TestNearlyEqual(TEXT("at asphalt_max_luma: half and half"), Refined(P, 50, FVector3f(P.AsphaltMaxLuma)).X, 0.5f * 310.0f + 0.5f * 304.0f, 0.01f);
	TestNearlyEqual(TEXT("concrete weight ramps over 0.04"), CamSimThermalRef::RefinementWeights(P, FVector3f(P.AsphaltMaxLuma + 0.01f)).Y, 0.75f, 1e-3f);
	TestTrue(TEXT("bare ground is not split into asphalt (decision: built-up only)"), Near4(Refined(P, 60, FVector3f(0.05f)), Data(P, LcBare), 1e-4f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefLandCoverNoBaseTest, "CamSim.Thermal.Reference.LandCoverWithoutBaseColour",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefLandCoverNoBaseTest::RunTest(const FString& Parameters)
{
	FThermalFrameParams P = MakeLandCoverParams(Down, 64, 36, 8, 10.0f, 0.0f);
	P.KFastScale = 0.0f;
	const TArray<uint8> BuiltUp = Uniform(8, 50);
	FPixelSample S;
	S.U = 0.5f; S.V = 0.6f;
	S.DeviceZ = DeviceZForPlane(P, Down, S.U, S.V, -700.0f);
	S.LandCover = BuiltUp.GetData();
	S.Base = FVector3f(0.05f, 0.20f, 0.04f);   // ignored: not bound
	S.bHasBase = false;
	TestNearlyEqual(TEXT("no base colour: WorldCover class alone (built_up)"), CamSimThermalRef::EvaluatePixel(P, S).TempK, P.ClassTempK[LcBuiltUp], 1e-3f);
	S.bHasBase = true;
	P.bLandCoverRefine = 0;
	TestNearlyEqual(TEXT("refinement off: built_up"), CamSimThermalRef::EvaluatePixel(P, S).TempK, P.ClassTempK[LcBuiltUp], 1e-3f);
	P.bLandCoverRefine = 1;
	TestNearlyEqual(TEXT("refinement on (green): vegetation, even with the fast term off (night)"),
		CamSimThermalRef::EvaluatePixel(P, S).TempK, P.ClassTempK[LcVegetation], 1e-3f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefLandCoverOffTest, "CamSim.Thermal.Reference.LandCoverOffIs4A",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefLandCoverOffTest::RunTest(const FString& Parameters)
{
	FThermalTestScene S = MakeScene(64, 36, 32, 18);   // sky, entities, water, terrain, shadow, NaN/Inf
	const TArray<uint8> Codes = MakeLandCoverCodes(32);
	FThermalFrameParams On = MakeLandCoverParams(S.ViewRot, 64, 36, 32, 10.0f, 30.0f);   // classes 0-2 = MakeParams'
	On.bWater = S.P.bWater;
	FThermalFrameParams Off = On;
	Off.bLandCover = 0;
	int32 Bad4A = 0, BadOff = 0, BadNonTerrain = 0, NonTerrain = 0, BadOutside = 0, Outside = 0;
	for (const bool bBase : { false, true })
	{
		for (int32 Y = 0; Y < S.H; ++Y)
		{
			for (int32 X = 0; X < S.W; ++X)
			{
				const FPixelSample None = SampleAt(S, X, Y, bBase, nullptr);
				const FPixelSample With = SampleAt(S, X, Y, bBase, Codes.GetData());
				Bad4A  += SameBits(CamSimThermalRef::EvaluatePixel(S.P, None).Radiance, Legacy4ARadiance(S.P, None)) ? 0 : 1;
				BadOff += SameBits(CamSimThermalRef::EvaluatePixel(Off, With).Radiance, Legacy4ARadiance(Off, With)) ? 0 : 1;
				const FPixelResult R = CamSimThermalRef::EvaluatePixel(On, With);
				if (R.Class != EPixelClass::Terrain) { ++NonTerrain; BadNonTerrain += SameBits(R.Radiance, Legacy4ARadiance(On, With)) ? 0 : 1; }
				else if (!R.bLandCover) { ++Outside; BadOutside += SameBits(R.Radiance, Legacy4ARadiance(On, With)) ? 0 : 1; }
			}
		}
	}
	TestEqual(TEXT("4A params, no window: bit for bit the 4A reference"), Bad4A, 0);
	TestEqual(TEXT("bLandCover = 0 with a window bound: bit for bit 4A"), BadOff, 0);
	TestTrue(TEXT("scene has sky / entity / water pixels"), NonTerrain > 100);
	TestEqual(TEXT("land cover on: sky, entity, water pixels unchanged"), BadNonTerrain, 0);
	TestTrue(TEXT("scene has terrain outside the window"), Outside > 0);
	TestEqual(TEXT("land cover on: terrain outside the window unchanged"), BadOutside, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefLandCoverEdgesTest, "CamSim.Thermal.Reference.LandCoverEdgesAndDegenerateAxes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefLandCoverEdgesTest::RunTest(const FString& Parameters)
{
	FThermalFrameParams P = MakeLandCoverParams(Down, 64, 36, 8, 10.0f, 0.0f);   // window E, N in [-40, 40] m; centres at +-35
	P.LandCoverCamOffsetM = FVector2f::ZeroVector;   // exact texel coordinates at the edges
	const TArray<uint8> Codes = MakeLandCoverCodes(8);
	auto At = [&](float E, float N) { return CamSimThermalRef::SampleLandCover(P, Codes.GetData(), GroundPoint(P, E, N, -700.0f)); };
	TestFalse(TEXT("west of the first texel centre: outside"), At(-35.5f, 0.0f).bInside);
	TestTrue (TEXT("first texel centre: inside"), At(-35.0f, 0.0f).bInside);
	const FLandCoverSample Last = At(35.0f, 0.0f);
	TestTrue (TEXT("last texel centre: inside"), Last.bInside);
	TestNearlyEqual(TEXT("... all weight on the last column"), Last.Weights[1] + Last.Weights[3], 1.0f, 1e-6f);
	TestFalse(TEXT("east of the last texel centre: outside"), At(35.5f, 0.0f).bInside);
	TestFalse(TEXT("north of the first row centre: outside"), At(0.0f, 35.5f).bInside);
	TestFalse(TEXT("null codes: off"), CamSimThermalRef::SampleLandCover(P, nullptr, GroundPoint(P, 0.0f, 0.0f, -700.0f)).bInside);
	FThermalFrameParams Q = P;
	Q.bLandCover = 0;
	TestFalse(TEXT("bLandCover 0: off"), CamSimThermalRef::SampleLandCover(Q, Codes.GetData(), GroundPoint(P, 0.0f, 0.0f, -700.0f)).bInside);
	Q = P;
	Q.LandCoverTexels = 1;
	TestFalse(TEXT("1-texel window: off"), CamSimThermalRef::SampleLandCover(Q, Codes.GetData(), FVector3f(0.0f, 0.0f, -700.0f)).bInside);
	Q = P;
	Q.LandCoverCamOffsetM.X = std::numeric_limits<float>::quiet_NaN();
	TestFalse(TEXT("NaN offset: outside, never NaN"), CamSimThermalRef::SampleLandCover(Q, Codes.GetData(), FVector3f(0.0f, 0.0f, -700.0f)).bInside);
	Q = P;
	Q.LandCoverEast = FVector3f(std::numeric_limits<float>::infinity(), 0.0f, 0.0f);
	TestFalse(TEXT("Inf axis: outside"), CamSimThermalRef::SampleLandCover(Q, Codes.GetData(), FVector3f(100.0f, 0.0f, -700.0f)).bInside);

	// A terrain pixel outside the window: TerrainClass, exactly as 4A.
	P.KFastScale = 0.0f;
	P.LandCoverCamOffsetM = FVector2f(1000.0f, 1000.0f);   // window 1 km away
	FPixelSample S;
	S.U = 0.5f; S.V = 0.6f;
	S.DeviceZ = DeviceZForPlane(P, Down, S.U, S.V, -700.0f);
	S.LandCover = Codes.GetData();
	const FPixelResult R = CamSimThermalRef::EvaluatePixel(P, S);
	TestFalse(TEXT("outside: no land cover"), R.bLandCover);
	TestEqual(TEXT("outside: terrain_default"), R.TempK, P.ClassTempK[P.TerrainClass]);
	return true;
}

// Review Focus 5: degenerate base colours and thresholds keep the blend convex and the radiance finite.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefRefineExtremesTest, "CamSim.Thermal.Reference.RefinementExtremesFinite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefRefineExtremesTest::RunTest(const FString& Parameters)
{
	FThermalFrameParams P = MakeLandCoverParams(Down, 64, 36, 8, 10.0f, 0.0f);
	const TArray<uint8> Codes = MakeLandCoverCodes(8);
	const FLandCoverSample Four = CamSimThermalRef::SampleLandCover(P, Codes.GetData(), GroundPoint(P, 0.0f, 0.0f, -700.0f));
	TestTrue(TEXT("window centre touches four quadrant codes"), Four.bInside && Four.Codes[0] != Four.Codes[3]);
	FVector4f Lo(1e9f), Hi(-1e9f);
	for (uint32 C = 0; C < P.NumClasses; ++C)
	{
		const FVector4f D = Data(P, C);
		Lo = FVector4f(FMath::Min(Lo.X, D.X), FMath::Min(Lo.Y, D.Y), FMath::Min(Lo.Z, D.Z), FMath::Min(Lo.W, D.W));
		Hi = FVector4f(FMath::Max(Hi.X, D.X), FMath::Max(Hi.Y, D.Y), FMath::Max(Hi.Z, D.Z), FMath::Max(Hi.W, D.W));
	}
	const float BMin = CamSimThermalRef::LutRadiance(P, FThermalFrameParams::LutMinK);
	const float BMax = CamSimThermalRef::LutRadiance(P, FThermalFrameParams::LutMaxK);
	int32 Bad = 0;
	for (const bool bDegenerate : { false, true })
	{
		P.VegIndexHi = bDegenerate ? P.VegIndexLo : 0.20f;
		P.AsphaltRampLuma = bDegenerate ? 0.0f : 0.04f;
		for (const uint32 bSrgb : { 0u, 1u })
		{
			P.bBaseColorSrgb = bSrgb;
			for (const FVector3f Base : { FVector3f(0.0f), FVector3f(1.0f), FVector3f(1.0f, 0.0f, 0.0f), FVector3f(0.0f, 1.0f, 0.0f), FVector3f(1e-7f) })
			{
				const FVector2f Wt = CamSimThermalRef::RefinementWeights(P, Base);
				Bad += (FMath::IsFinite(Wt.X) && FMath::IsFinite(Wt.Y) && Wt.X >= 0.0f && Wt.X <= 1.0f && Wt.Y >= 0.0f && Wt.Y <= 1.0f) ? 0 : 1;
				const FVector4f Cd = CamSimThermalRef::BlendLandCover(P, Four, Base, true);
				Bad += (Cd.X >= Lo.X - 1e-3f && Cd.X <= Hi.X + 1e-3f && Cd.Y >= Lo.Y - 1e-6f && Cd.Y <= Hi.Y + 1e-6f
					&& Cd.Z >= Lo.Z - 1e-6f && Cd.Z <= Hi.Z + 1e-6f && Cd.W >= Lo.W - 1e-3f && Cd.W <= Hi.W + 1e-3f) ? 0 : 1;
				FPixelSample S;
				S.U = 0.5f; S.V = 0.6f;
				S.DeviceZ = DeviceZForPlane(P, Down, S.U, S.V, -700.0f);
				S.LandCover = Codes.GetData();
				S.Base = Base;
				S.bHasBase = true;
				S.Color = FVector3f(SunlitLuminance(P, 0.2f));
				const float L = CamSimThermalRef::EvaluatePixel(P, S).Radiance;
				Bad += (FMath::IsFinite(L) && L >= BMin * 0.999f && L <= BMax * 1.001f) ? 0 : 1;
			}
		}
	}
	TestEqual(TEXT("weights in [0, 1], blend convex, radiance finite and in the LUT range"), Bad, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefLandCoverRunTest, "CamSim.Thermal.Reference.LandCoverRunCoverage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefLandCoverRunTest::RunTest(const FString& Parameters)
{
	for (const float Yaw : { 0.0f, 30.0f })
	{
		const FThermalTestScene S = MakeLandCoverScene(64, 36, 64, 36, Yaw);
		const TArray<FPixelResult> R = CamSimThermalRef::Run(S.Images(true), S.P);
		int32 Inside = 0, Outside = 0, Entity = 0, EntityWithLc = 0;
		TSet<int32> Temps;
		for (const FPixelResult& X : R)
		{
			if (X.Class == EPixelClass::Entity) { ++Entity; EntityWithLc += X.bLandCover ? 1 : 0; }
			else if (X.Class == EPixelClass::Terrain) { X.bLandCover ? ++Inside : ++Outside; if (X.bLandCover) Temps.Add(FMath::FloorToInt32(X.TempK * 10.0f)); }
		}
		TestTrue(*FString::Printf(TEXT("yaw %.0f: terrain inside the window"), Yaw), Inside > 500);
		TestTrue(*FString::Printf(TEXT("yaw %.0f: terrain outside the window"), Yaw), Outside > 0);
		TestTrue(*FString::Printf(TEXT("yaw %.0f: an entity"), Yaw), Entity > 0);
		TestEqual(*FString::Printf(TEXT("yaw %.0f: entities never use land cover"), Yaw), EntityWithLc, 0);
		TestTrue(*FString::Printf(TEXT("yaw %.0f: varied land temperatures (%d)"), Yaw, Temps.Num()), Temps.Num() >= 8);
	}
	return true;
}
```

- [ ] **Step 3: Run to verify failure** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5` → Expected: compile errors: `LandCoverClass` / `bLandCover` not members of `FThermalFrameParams`, `SampleLandCover` not a member of `CamSimThermalRef`.

- [ ] **Step 4: Implement the parameters** — `Source/CamSimShaders/Public/ThermalFrameParams.h`, after the "Solar fast term" block (before `InputScale`):

```cpp
	// Land cover (ROADMAP 4B). Terrain pixels inside the camera-centred window blend the class data of the four nearest
	// WorldCover texels (refined per pixel by the base colour); outside it, or with bLandCover = 0, they are TerrainClass (4A).
	static constexpr int32 NumLandCoverCodes = 256;
	static constexpr uint8 LandCoverFamilyNone = 0, LandCoverFamilyVegetation = 1, LandCoverFamilyBuiltUp = 2, LandCoverFamilyBare = 3;
	uint8     LandCoverClass[NumLandCoverCodes]  = {};   // WorldCover code -> class index
	uint8     LandCoverFamily[NumLandCoverCodes] = {};   // WorldCover code -> LandCoverFamily*
	uint32    bLandCover          = 0;
	uint32    LandCoverWindowId   = 0;                   // FLandCoverWindowData::Id these mapping values belong to (0 = none)
	FVector3f LandCoverEast       = FVector3f(1.0f, 0.0f, 0.0f);    // unit East at the window centre, (translated) world
	FVector3f LandCoverNorth      = FVector3f(0.0f, -1.0f, 0.0f);   // unit North (UE +Y is south at the georeference origin)
	FVector2f LandCoverCamOffsetM = FVector2f::ZeroVector;          // camera (East, North) from the window centre, m (CPU doubles)
	float     LandCoverTexelM     = 10.0f;
	uint32    LandCoverTexels     = 2048;
	uint32    bLandCoverRefine    = 0;                   // base-colour refinement (needs the base colour, not sunlight)
	float     VegIndexLo          = 0.05f;               // v = saturate((ExG - lo) / max(hi - lo, 1e-4))
	float     VegIndexHi          = 0.20f;
	float     AsphaltMaxLuma      = 0.12f;               // built-up concrete weight = saturate((BaseLum - max) / ramp + 0.5)
	float     AsphaltRampLuma     = 0.04f;
	uint32    VegetationClass     = 4;                   // refinement targets (FThermalMaterialTable indices)
	uint32    BareSoilClass       = 11;
	uint32    AsphaltClass        = 3;
	uint32    ConcreteClass       = 5;
```

- [ ] **Step 5: Implement the reference.** `Thermal/ThermalReference.h` — add to the header comment, after step 4:

```cpp
 *  4b. terrain inside the land-cover window (ROADMAP 4B): class data = bilinear blend of the four nearest texels' refined
 *      class data (SampleLandCover, RefinedClassData, BlendLandCover); outside it TerrainClass
```

add to `FPixelSample`:

```cpp
		const uint8* LandCover = nullptr;               // window codes (P.LandCoverTexels^2, row 0 = north); null: none bound
```

to `FPixelResult`:

```cpp
		bool bLandCover = false;                        // terrain whose class data came from land cover
```

to `FImages`:

```cpp
		const TArray<uint8>*        LandCover   = nullptr;   // P.LandCoverTexels^2 window codes; null: not bound
```

and these declarations next to `LutRadiance`:

```cpp
	/** The four window texels around a world position and their bilinear weights (ROADMAP 4B). */
	struct FLandCoverSample
	{
		bool  bInside = false;       // false: no window, land cover off, or outside the texel-centre grid -> TerrainClass
		uint8 Codes[4] = {};         // (x0, y0), (x1, y0), (x0, y1), (x1, y1); y0 is the northern row
		float Weights[4] = {};
	};

	float     Lum709(const FVector3f& C);
	FVector3f ClipToWorld(const FThermalFrameParams& P, float Nx, float Ny, float Z);
	/** (T_class, emissivity, k_fast, S_abs,ref) of a class; the index is clamped to NumClasses. */
	FVector4f ClassData(const FThermalFrameParams& P, uint32 Class);
	/** Pw: camera-relative world position (cm). (E, N) = (Pw . East, Pw . North) / 100 + CamOffset; texel centres on integers. */
	FLandCoverSample SampleLandCover(const FThermalFrameParams& P, const uint8* Codes, const FVector3f& Pw);
	/** (vegetation v, concrete c) of a linear base colour. */
	FVector2f RefinementWeights(const FThermalFrameParams& P, const FVector3f& Base);
	/** One code's class data; with bRefine the family's blend toward vegetation / bare soil / asphalt / concrete. */
	FVector4f RefinedClassData(const FThermalFrameParams& P, uint8 Code, const FVector2f& Weights, bool bRefine);
	/** Bilinear blend of the four texels' refined class data. */
	FVector4f BlendLandCover(const FThermalFrameParams& P, const FLandCoverSample& S, const FVector3f& Base, bool bRefine);
```

`Thermal/ThermalReference.cpp` — replace everything from the first line after `#include "Thermal/ThermalReference.h"` through the closing brace of the anonymous namespace with the block below (the anonymous namespace keeps only `PiF`; `Lum709` and `ClipToWorld` become public with the same bodies). `LutRadiance`, `SkyTemperatureK`, `SrgbToLinear`, `EvaluatePixel`, `Run` and `MakeClipToTranslatedWorld` follow it unchanged inside the same `namespace CamSimThermalRef` (EvaluatePixel and Run are then edited as shown after the block):

```cpp
#include "Thermal/LandCoverClasses.h"

static_assert(static_cast<uint8>(ELandCoverFamily::Vegetation) == FThermalFrameParams::LandCoverFamilyVegetation
	&& static_cast<uint8>(ELandCoverFamily::BuiltUp) == FThermalFrameParams::LandCoverFamilyBuiltUp
	&& static_cast<uint8>(ELandCoverFamily::Bare) == FThermalFrameParams::LandCoverFamilyBare
	&& static_cast<uint8>(ELandCoverFamily::None) == FThermalFrameParams::LandCoverFamilyNone,
	"land-cover families: ELandCoverFamily, FThermalFrameParams and LANDCOVER_FAMILY_* in CamSimThermalCommon.ush must agree");

namespace CamSimThermalRef
{
	namespace
	{
		constexpr float PiF = 3.14159265f;   // THERMAL_PI in CamSimThermalCommon.ush
	}

	float Lum709(const FVector3f& C) { return 0.2126f * C.X + 0.7152f * C.Y + 0.0722f * C.Z; }

	FVector3f ClipToWorld(const FThermalFrameParams& P, float Nx, float Ny, float Z)
	{
		const FVector4f H = P.ClipToTranslatedWorld.TransformFVector4(FVector4f(Nx, Ny, Z, 1.0f));
		return FVector3f(H.X / H.W, H.Y / H.W, H.Z / H.W);
	}

	FVector4f ClassData(const FThermalFrameParams& P, uint32 Class)
	{
		const uint32 C = FMath::Min(Class, FMath::Max(P.NumClasses, 1u) - 1u);
		return FVector4f(P.ClassTempK[C], P.ClassEmissivity[C], P.ClassKFast[C], P.ClassSAbsRef[C]);
	}

	FLandCoverSample SampleLandCover(const FThermalFrameParams& P, const uint8* Codes, const FVector3f& Pw)
	{
		FLandCoverSample S;
		if (!Codes || P.bLandCover == 0u || P.LandCoverTexels < 2u) return S;
		const float E = FVector3f::DotProduct(Pw, P.LandCoverEast) / 100.0f + P.LandCoverCamOffsetM.X;
		const float N = FVector3f::DotProduct(Pw, P.LandCoverNorth) / 100.0f + P.LandCoverCamOffsetM.Y;
		const float Half = static_cast<float>(P.LandCoverTexels) * 0.5f;
		const float X = E / P.LandCoverTexelM + Half - 0.5f;
		const float Y = Half - N / P.LandCoverTexelM - 0.5f;
		const float MaxI = static_cast<float>(P.LandCoverTexels) - 1.0f;
		if (!(X >= 0.0f && X <= MaxI && Y >= 0.0f && Y <= MaxI)) return S;   // NaN fails too
		const float X0 = FMath::Min(FMath::FloorToFloat(X), MaxI - 1.0f);
		const float Y0 = FMath::Min(FMath::FloorToFloat(Y), MaxI - 1.0f);
		const float Fx = X - X0, Fy = Y - Y0;
		const int32 I = static_cast<int32>(X0), J = static_cast<int32>(Y0), W = static_cast<int32>(P.LandCoverTexels);
		S.bInside = true;
		S.Codes[0] = Codes[J * W + I];
		S.Codes[1] = Codes[J * W + I + 1];
		S.Codes[2] = Codes[(J + 1) * W + I];
		S.Codes[3] = Codes[(J + 1) * W + I + 1];
		S.Weights[0] = (1.0f - Fx) * (1.0f - Fy);
		S.Weights[1] = Fx * (1.0f - Fy);
		S.Weights[2] = (1.0f - Fx) * Fy;
		S.Weights[3] = Fx * Fy;
		return S;
	}

	FVector2f RefinementWeights(const FThermalFrameParams& P, const FVector3f& Base)
	{
		const float Sum = Base.X + Base.Y + Base.Z;
		const float ExG = (2.0f * Base.Y - Base.X - Base.Z) / (Sum + 1e-4f);
		const float Veg = FMath::Clamp((ExG - P.VegIndexLo) / FMath::Max(P.VegIndexHi - P.VegIndexLo, 1e-4f), 0.0f, 1.0f);
		const float Concrete = FMath::Clamp((Lum709(Base) - P.AsphaltMaxLuma) / FMath::Max(P.AsphaltRampLuma, 1e-4f) + 0.5f, 0.0f, 1.0f);
		return FVector2f(Veg, Concrete);
	}

	FVector4f RefinedClassData(const FThermalFrameParams& P, uint8 Code, const FVector2f& Weights, bool bRefine)
	{
		const FVector4f Own = ClassData(P, P.LandCoverClass[Code]);
		if (!bRefine) return Own;
		const uint8 Family = P.LandCoverFamily[Code];
		const float V = Weights.X, C = Weights.Y;
		if (Family == FThermalFrameParams::LandCoverFamilyVegetation)
		{
			return Own * V + ClassData(P, P.BareSoilClass) * (1.0f - V);
		}
		if (Family == FThermalFrameParams::LandCoverFamilyBuiltUp)
		{
			return ClassData(P, P.VegetationClass) * V
				+ (ClassData(P, P.AsphaltClass) * (1.0f - C) + ClassData(P, P.ConcreteClass) * C) * (1.0f - V);
		}
		if (Family == FThermalFrameParams::LandCoverFamilyBare)
		{
			return ClassData(P, P.VegetationClass) * V + Own * (1.0f - V);
		}
		return Own;
	}

	FVector4f BlendLandCover(const FThermalFrameParams& P, const FLandCoverSample& S, const FVector3f& Base, bool bRefine)
	{
		const FVector2f Wt = bRefine ? RefinementWeights(P, Base) : FVector2f::ZeroVector;
		return RefinedClassData(P, S.Codes[0], Wt, bRefine) * S.Weights[0] + RefinedClassData(P, S.Codes[1], Wt, bRefine) * S.Weights[1]
			+ RefinedClassData(P, S.Codes[2], Wt, bRefine) * S.Weights[2] + RefinedClassData(P, S.Codes[3], Wt, bRefine) * S.Weights[3];
	}
```

In `EvaluatePixel`, everything up to the stencil/water classification is unchanged; from `Class = FMath::Min(...)` to the end it becomes:

```cpp
		Class = FMath::Min(Class, FMath::Max(P.NumClasses, 1u) - 1u);
		const FVector3f Base = (P.bBaseColorSrgb != 0u)
			? FVector3f(SrgbToLinear(S.Base.X), SrgbToLinear(S.Base.Y), SrgbToLinear(S.Base.Z)) : S.Base;
		FVector4f Cd = ClassData(P, Class);
		if (R.Class == EPixelClass::Terrain)
		{
			const FLandCoverSample L = SampleLandCover(P, S.LandCover, Pw);
			if (L.bInside)
			{
				Cd = BlendLandCover(P, L, Base, S.bHasBase && P.bLandCoverRefine != 0u);
				R.bLandCover = true;
			}
		}
		float T = Cd.X + Offset;
		if (S.bHasBase && P.KFastScale > 0.0f)
		{
			const float BaseLum = Lum709(Base);
			const float E = FMath::Clamp(PiF * Lum709(S.Color) / (FMath::Max(BaseLum, 0.03f) * FMath::Max(P.KLum, 1e-6f)), 0.0f, P.EClampWm2);
			const float SAbs = (1.0f - FMath::Clamp(BaseLum, 0.0f, 1.0f)) * E;
			T += P.KFastScale * Cd.Z * (SAbs - Cd.W);
		}
		const float Eps = Cd.Y;
		const float LSurf = Eps * LutRadiance(P, T) + (1.0f - Eps) * P.SkyHemiRadiance;
		const float Tau = FMath::Exp(-P.BetaPerCm * Range);
		R.TempK = T;
		R.Radiance = Tau * LSurf + (1.0f - Tau) * BAir;
		return R;
	}
```

In `Run`, after the existing `check`s:

```cpp
		check(!In.LandCover || In.LandCover->Num() == static_cast<int32>(P.LandCoverTexels * P.LandCoverTexels));
```

and inside the pixel loop, before `Out[...] = EvaluatePixel(P, S);`:

```cpp
				S.LandCover = In.LandCover ? In.LandCover->GetData() : nullptr;
```

- [ ] **Step 6: Run to verify pass** — build, then `run_tests CamSim.Thermal.Reference` → Expected: 0 failed (the 9 new tests and every 4A `CamSim.Thermal.Reference.*` test). Then `run_tests CamSim.Thermal` → 0 failed.

- [ ] **Step 7: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimShaders/Public/ThermalFrameParams.h unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalReference.h \
  unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalReference.cpp unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalTestScene.h \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalLandCoverReferenceTest.cpp
git commit -F - <<'EOF'
feat(thermal): land-cover lookup, bilinear material blend and base-colour refinement in the CPU reference (ROADMAP 4B)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
EOF
```

---

### Task 8: `ThermalCS` land-cover mirror, pass input, GPU tests

**Files:**
- Modify: `unreal_project/CamSimTest/Shaders/Private/CamSimThermalCommon.ush`
- Modify: `unreal_project/CamSimTest/Source/CamSimShaders/Public/ThermalPass.h`, `unreal_project/CamSimTest/Source/CamSimShaders/Private/ThermalPass.cpp`
- Test: `unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalGpuTest.cpp`

**Interfaces:**
- Consumes: the Task 7 parameters and reference (`SampleLandCover`, `RefinementWeights`, `RefinedClassData`, `BlendLandCover`, `EvaluatePixel`), `CamSimThermalTest::MakeLandCoverScene`.
- Produces: `FThermalPassInputs::LandCover` (`FRDGTextureRef`, `PF_R8_UINT`, `P.LandCoverTexels`²; null → off). Shader functions `LandCoverCoords`, `RefinementWeights`, `RefinedClassData`, `BlendLandCover` mirroring the reference, `#define LANDCOVER_FAMILY_{VEGETATION=1u, BUILT_UP=2u, BARE=3u}`.

- [ ] **Step 1: Write the failing GPU test.** In `Tests/ThermalGpuTest.cpp`, add an uploader to the anonymous namespace:

```cpp
	/** The land-cover window as the runtime binds it: PF_R8_UINT, one code per texel. */
	FTextureRHIRef UploadCodes(FRHICommandListImmediate& RHICmdList, const TArray<uint8>& Codes, int32 N)
	{
		const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(TEXT("CamSimTestLandCover"), N, N, PF_R8_UINT)
			.SetFlags(ETextureCreateFlags::ShaderResource).SetInitialState(ERHIAccess::SRVMask);
		FTextureRHIRef Tex = RHICmdList.CreateTexture(Desc);
		RHICmdList.UpdateTexture2D(Tex, 0, FUpdateTextureRegion2D(0, 0, 0, 0, N, N), N, Codes.GetData());
		return Tex;
	}
```

change `RunThermalOnGpu`'s signature to

```cpp
	TArray<float> RunThermalOnGpu(const CamSimThermalTest::FThermalTestScene& S, const FLayout& L, EBase BaseMode,
		const TArray<uint8>& BaseBytes, const TArray<uint8>* LandCover = nullptr)
```

and inside its render command, after `BaseTex` is created:

```cpp
			const int32 LcN = static_cast<int32>(S.P.LandCoverTexels);
			FTextureRHIRef LcTex = LandCover ? UploadCodes(RHICmdList, *LandCover, LcN) : FTextureRHIRef();
```

and after `In.DepthViewRect = ...;`:

```cpp
				In.LandCover = LandCover ? GraphBuilder.RegisterExternalTexture(CreateRenderTarget(LcTex, TEXT("CamSimTestLandCover"))) : nullptr;
```

Then add the test at the end of the file:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalGpuLandCoverTest, "CamSim.GPU.Thermal.LandCoverMatchesCpu",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalGpuLandCoverTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI) { AddInfo(TEXT("skipped: NullRHI (run scripts/run_gpu_tests.sh)")); return true; }
	constexpr float MirrorTol = 1e-4f;   // ThermalCS vs CamSimThermalRef (never loosen it)
	struct FCase { const TCHAR* Name; float Yaw; int32 DW, DH; FIntPoint ColorPad, DepthPad; bool bBase; bool bLandCover; };
	const FCase Cases[] = {
		{ TEXT("yaw 0, refined"),               0.0f,    64, 36, FIntPoint(0, 0), FIntPoint(0, 0), true,  true  },
		{ TEXT("yaw 30, refined"),              30.0f,   64, 36, FIntPoint(0, 0), FIntPoint(0, 0), true,  true  },
		{ TEXT("yaw 30, no base colour"),       30.0f,   64, 36, FIntPoint(0, 0), FIntPoint(0, 0), false, true  },
		{ TEXT("half-res depth, offset rects"), -117.0f, 32, 18, FIntPoint(3, 2), FIntPoint(4, 3), true,  true  },
		{ TEXT("land cover off, window bound"), 30.0f,   64, 36, FIntPoint(0, 0), FIntPoint(0, 0), true,  false },
	};
	for (const FCase& C : Cases)
	{
		CamSimThermalTest::FThermalTestScene S = CamSimThermalTest::MakeLandCoverScene(64, 36, C.DW, C.DH, C.Yaw);
		if (!C.bLandCover) S.P.bLandCover = 0;
		const TArray<CamSimThermalRef::FPixelResult> Ref = CamSimThermalRef::Run(S.Images(C.bBase), S.P);
		FLayout L;
		L.ColorMin = C.ColorPad;
		L.ColorExtent = FIntPoint(S.W, S.H) + C.ColorPad * 2;
		L.DepthMin = C.DepthPad;
		L.DepthExtent = FIntPoint(C.DW, C.DH) + C.DepthPad * 2;
		const EBase Base = C.bBase ? EBase::Float : EBase::None;
		const TArray<float> Gpu = RunThermalOnGpu(S, L, Base, TArray<uint8>(), &S.LandCover);
		if (!TestEqual(*FString::Printf(TEXT("%s: readback"), C.Name), Gpu.Num(), S.W * S.H)) continue;
		int32 Bad = 0, Land = 0, WorstI = 0;
		float WorstRel = 0.0f;
		for (int32 I = 0; I < Gpu.Num(); ++I)
		{
			const float R = Ref[I].Radiance;
			const float Rel = FMath::Abs(Gpu[I] - R) / FMath::Max(FMath::Abs(R), 1e-6f);
			Land += Ref[I].bLandCover ? 1 : 0;
			if (!(Rel <= MirrorTol)) ++Bad;
			if (!(Rel <= WorstRel)) { WorstRel = Rel; WorstI = I; }
		}
		AddInfo(FString::Printf(TEXT("%s: worst relative %.2e at pixel (%d, %d), %d land-cover pixels"), C.Name, WorstRel, WorstI % S.W, WorstI / S.W, Land));
		TestEqual(*FString::Printf(TEXT("%s: every pixel within %.0e of CamSimThermalRef"), C.Name, MirrorTol), Bad, 0);
		if (C.bLandCover)
		{
			TestTrue(*FString::Printf(TEXT("%s: land cover exercised"), C.Name), Land >= 300);
		}
		else
		{
			const TArray<float> Unbound = RunThermalOnGpu(S, L, Base, TArray<uint8>(), nullptr);
			TestTrue(TEXT("bLandCover = 0: bit for bit the same as no window bound"),
				Unbound.Num() == Gpu.Num() && FMemory::Memcmp(Unbound.GetData(), Gpu.GetData(), Gpu.Num() * sizeof(float)) == 0);
		}
	}
	return true;
}
```

- [ ] **Step 2: Run to verify failure** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5` → Expected: compile error, `LandCover` is not a member of `FThermalPassInputs`.

- [ ] **Step 3: Implement the pass input.** `Source/CamSimShaders/Public/ThermalPass.h` — add to `FThermalPassInputs` after `BaseColor`:

```cpp
	/**
	 * ROADMAP 4B: the land-cover window (PF_R8_UINT, Params.LandCoverTexels^2 WorldCover codes, row 0 = north). Null, another
	 * extent, or Params.bLandCover == 0: land cover off (terrain = TerrainClass, as 4A) and the zero-uint dummy is bound.
	 */
	FRDGTextureRef    LandCover     = nullptr;
```

`Source/CamSimShaders/Private/ThermalPass.cpp` — add below the existing `static_assert`s:

```cpp
static_assert(FThermalFrameParams::NumLandCoverCodes == 256, "LandCover*Packed[16] below and in CamSimThermalCommon.ush");

/** 16 bytes -> 4 little-endian uints (CamSimThermalCommon.ush PackedByte unpacks them). */
static FUintVector4 PackBytes16(const uint8* B)
{
	auto U = [B](int32 K) { return uint32(B[4 * K]) | (uint32(B[4 * K + 1]) << 8) | (uint32(B[4 * K + 2]) << 16) | (uint32(B[4 * K + 3]) << 24); };
	return FUintVector4(U(0), U(1), U(2), U(3));
}
```

add to `FCamSimThermalParameters` after `SHADER_PARAMETER(uint32, UseBaseColor)`:

```cpp
	SHADER_PARAMETER_ARRAY(FUintVector4, LandCoverClassPacked, [16])
	SHADER_PARAMETER_ARRAY(FUintVector4, LandCoverFamilyPacked, [16])
	SHADER_PARAMETER(uint32, UseLandCover)
	SHADER_PARAMETER(FVector3f, LandCoverEast)
	SHADER_PARAMETER(FVector3f, LandCoverNorth)
	SHADER_PARAMETER(FVector2f, LandCoverCamOffsetM)
	SHADER_PARAMETER(float, LandCoverTexelM)
	SHADER_PARAMETER(uint32, LandCoverTexels)
	SHADER_PARAMETER(uint32, bLandCoverRefine)
	SHADER_PARAMETER(float, VegIndexLo)
	SHADER_PARAMETER(float, VegIndexHi)
	SHADER_PARAMETER(float, AsphaltMaxLuma)
	SHADER_PARAMETER(float, AsphaltRampLuma)
	SHADER_PARAMETER(uint32, VegetationClass)
	SHADER_PARAMETER(uint32, BareSoilClass)
	SHADER_PARAMETER(uint32, AsphaltClass)
	SHADER_PARAMETER(uint32, ConcreteClass)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, LandCover)
```

and in `AddThermalPass`, after `Pass->UseBaseColor = ...;`:

```cpp
	for (int32 K = 0; K < FThermalFrameParams::NumLandCoverCodes / 16; ++K)
	{
		Pass->LandCoverClassPacked[K]  = PackBytes16(&P.LandCoverClass[16 * K]);
		Pass->LandCoverFamilyPacked[K] = PackBytes16(&P.LandCoverFamily[16 * K]);
	}
	const bool bLandCover = In.LandCover != nullptr && P.bLandCover != 0u && P.LandCoverTexels >= 2u
		&& In.LandCover->Desc.Extent == FIntPoint(static_cast<int32>(P.LandCoverTexels), static_cast<int32>(P.LandCoverTexels));
	Pass->UseLandCover        = bLandCover ? 1u : 0u;
	Pass->LandCoverEast       = P.LandCoverEast;
	Pass->LandCoverNorth      = P.LandCoverNorth;
	Pass->LandCoverCamOffsetM = P.LandCoverCamOffsetM;
	Pass->LandCoverTexelM     = P.LandCoverTexelM;
	Pass->LandCoverTexels     = P.LandCoverTexels;
	Pass->bLandCoverRefine    = P.bLandCoverRefine;
	Pass->VegIndexLo          = P.VegIndexLo;
	Pass->VegIndexHi          = P.VegIndexHi;
	Pass->AsphaltMaxLuma      = P.AsphaltMaxLuma;
	Pass->AsphaltRampLuma     = P.AsphaltRampLuma;
	Pass->VegetationClass     = P.VegetationClass;
	Pass->BareSoilClass       = P.BareSoilClass;
	Pass->AsphaltClass        = P.AsphaltClass;
	Pass->ConcreteClass       = P.ConcreteClass;
	Pass->LandCover           = bLandCover ? In.LandCover : GSystemTextures.GetZeroUIntDummy(GraphBuilder);
```

- [ ] **Step 4: Implement the shader mirror.** `Shaders/Private/CamSimThermalCommon.ush` — after `uint UseBaseColor; ...`:

```hlsl
// Land cover (ROADMAP 4B): CamSimThermalRef::{SampleLandCover, RefinementWeights, RefinedClassData, BlendLandCover}.
#define LANDCOVER_FAMILY_VEGETATION 1u   // FThermalFrameParams::LandCoverFamily* / ELandCoverFamily
#define LANDCOVER_FAMILY_BUILT_UP   2u
#define LANDCOVER_FAMILY_BARE       3u
uint4    LandCoverClassPacked[16];       // code c -> class: byte (c & 3) of component (c >> 2) & 3 of entry c >> 4
uint4    LandCoverFamilyPacked[16];      // same layout: 0 none, 1 vegetation, 2 built-up, 3 bare
uint     UseLandCover;                   // bLandCover and a window of the right size bound
float3   LandCoverEast;                  // unit East at the window centre (translated world)
float3   LandCoverNorth;
float2   LandCoverCamOffsetM;            // camera (E, N) from the window centre, m
float    LandCoverTexelM;
uint     LandCoverTexels;
uint     bLandCoverRefine;
float    VegIndexLo;
float    VegIndexHi;
float    AsphaltMaxLuma;
float    AsphaltRampLuma;
uint     VegetationClass;
uint     BareSoilClass;
uint     AsphaltClass;
uint     ConcreteClass;
Texture2D<uint> LandCover;               // window codes, row 0 = north (zero-uint dummy when UseLandCover == 0: never loaded then)
```

After `ClipToWorld` (before `struct FThermalSample`):

```hlsl
uint PackedByte(uint4 Entry, uint Code) { return (Entry[(Code >> 2u) & 3u] >> ((Code & 3u) * 8u)) & 0xFFu; }
uint LandCoverClassOf(uint Code)  { return PackedByte(LandCoverClassPacked[Code >> 4u], Code); }
uint LandCoverFamilyOf(uint Code) { return PackedByte(LandCoverFamilyPacked[Code >> 4u], Code); }

/** CamSimThermalRef::ClassData: (T, eps, k_fast, S_abs,ref), class clamped. */
float4 ClassDataOf(uint Class) { return ClassData[min(Class, max(NumClasses, 1u) - 1u)]; }

/** CamSimThermalRef::SampleLandCover: false outside the texel-centre grid; else the north-west texel I0 and the fractions F. */
bool LandCoverCoords(float3 Pw, out int2 I0, out float2 F)
{
	I0 = int2(0, 0);
	F = float2(0.0, 0.0);
	const float E = dot(Pw, LandCoverEast) / 100.0 + LandCoverCamOffsetM.x;
	const float N = dot(Pw, LandCoverNorth) / 100.0 + LandCoverCamOffsetM.y;
	const float Half = float(LandCoverTexels) * 0.5;
	const float X = E / LandCoverTexelM + Half - 0.5;
	const float Y = Half - N / LandCoverTexelM - 0.5;
	const float MaxI = float(LandCoverTexels) - 1.0;
	// Bit tests first: Metal fast-math may fold the NaN behaviour of the comparisons (CPU: !(X >= 0 && ...)).
	if (IsNonFinite(X) || IsNonFinite(Y) || X < 0.0 || X > MaxI || Y < 0.0 || Y > MaxI) return false;
	const float X0 = min(floor(X), MaxI - 1.0);
	const float Y0 = min(floor(Y), MaxI - 1.0);
	I0 = int2(int(X0), int(Y0));
	F = float2(X - X0, Y - Y0);
	return true;
}

/** CamSimThermalRef::RefinementWeights: (vegetation v, concrete c) of a linear base colour. */
float2 RefinementWeights(float3 Base)
{
	const float Sum = Base.x + Base.y + Base.z;
	const float ExG = (2.0 * Base.y - Base.x - Base.z) / (Sum + 1e-4);
	const float Veg = clamp((ExG - VegIndexLo) / max(VegIndexHi - VegIndexLo, 1e-4), 0.0, 1.0);
	const float Concrete = clamp((Lum709(Base) - AsphaltMaxLuma) / max(AsphaltRampLuma, 1e-4) + 0.5, 0.0, 1.0);
	return float2(Veg, Concrete);
}

/** CamSimThermalRef::RefinedClassData. */
float4 RefinedClassData(uint Code, float2 Wt, bool bRefine)
{
	const float4 Own = ClassDataOf(LandCoverClassOf(Code));
	if (!bRefine) return Own;
	const uint Family = LandCoverFamilyOf(Code);
	const float V = Wt.x;
	const float C = Wt.y;
	if (Family == LANDCOVER_FAMILY_VEGETATION)
	{
		return Own * V + ClassDataOf(BareSoilClass) * (1.0 - V);
	}
	if (Family == LANDCOVER_FAMILY_BUILT_UP)
	{
		return ClassDataOf(VegetationClass) * V + (ClassDataOf(AsphaltClass) * (1.0 - C) + ClassDataOf(ConcreteClass) * C) * (1.0 - V);
	}
	if (Family == LANDCOVER_FAMILY_BARE)
	{
		return ClassDataOf(VegetationClass) * V + Own * (1.0 - V);
	}
	return Own;
}

/** CamSimThermalRef::BlendLandCover: bilinear blend of the four texels' refined class data. */
float4 BlendLandCover(int2 I0, float2 F, float3 Base, bool bRefine)
{
	const float2 Wt = bRefine ? RefinementWeights(Base) : float2(0.0, 0.0);
	const uint C00 = LandCover.Load(int3(I0, 0)) & 0xFFu;
	const uint C10 = LandCover.Load(int3(I0 + int2(1, 0), 0)) & 0xFFu;
	const uint C01 = LandCover.Load(int3(I0 + int2(0, 1), 0)) & 0xFFu;
	const uint C11 = LandCover.Load(int3(I0 + int2(1, 1), 0)) & 0xFFu;
	return RefinedClassData(C00, Wt, bRefine) * ((1.0 - F.x) * (1.0 - F.y)) + RefinedClassData(C10, Wt, bRefine) * (F.x * (1.0 - F.y))
		+ RefinedClassData(C01, Wt, bRefine) * ((1.0 - F.x) * F.y) + RefinedClassData(C11, Wt, bRefine) * (F.x * F.y);
}
```

In `EvaluatePixel`, declare `bool bTerrain = true;` next to `float Offset = 0.0;`; set `bTerrain = false;` at the end of the entity branch; the water branch becomes

```hlsl
		if (Height < WaterBandCm)
		{
			Class = WaterClass;
			bTerrain = false;
		}
```

and everything from `Class = min(Class, ...)` to the end of the function becomes:

```hlsl
	Class = min(Class, max(NumClasses, 1u) - 1u);
	const float3 Base = (bBaseColorSrgb != 0u) ? float3(SrgbToLinear(S.Base.x), SrgbToLinear(S.Base.y), SrgbToLinear(S.Base.z)) : S.Base;
	float4 Cd = ClassData[Class];
	if (bTerrain && UseLandCover != 0u)
	{
		int2 I0;
		float2 F;
		if (LandCoverCoords(Pw, I0, F)) Cd = BlendLandCover(I0, F, Base, UseBaseColor != 0u && bLandCoverRefine != 0u);
	}
	float T = Cd.x + Offset;
	if (UseBaseColor != 0u && KFastScale > 0.0)
	{
		const float BaseLum = Lum709(Base);
		const float E = clamp(THERMAL_PI * Lum709(S.Color) / (max(BaseLum, 0.03) * max(KLum, 1e-6)), 0.0, EClampWm2);
		const float SAbs = (1.0 - clamp(BaseLum, 0.0, 1.0)) * E;
		T += KFastScale * Cd.z * (SAbs - Cd.w);
	}
	const float LSurf = Cd.y * LutRadiance(T) + (1.0 - Cd.y) * SkyHemiRadiance;
	const float Tau = exp(-BetaPerCm * Range);
	return Tau * LSurf + (1.0 - Tau) * BAir;
}
```

Update the file's header comment: "(ROADMAP 4A; 4B adds the land-cover lookup for terrain pixels)".

- [ ] **Step 5: Run to verify pass** — build, then `scripts/run_gpu_tests.sh CamSim.GPU.Thermal` → Expected: all pass (`MatchesCpu`, `SceneColorOutputMatchesCpu` unchanged; `LandCoverMatchesCpu` 5 cases within 1e-4, the off case bit-identical; note the worst relative errors from the info lines). Then `scripts/run_gpu_tests.sh CamSim.GPU` → all pass; `run_tests CamSim.Thermal` → 0 failed.

- [ ] **Step 6: Commit**

```bash
git add unreal_project/CamSimTest/Shaders/Private/CamSimThermalCommon.ush unreal_project/CamSimTest/Source/CamSimShaders/Public/ThermalPass.h \
  unreal_project/CamSimTest/Source/CamSimShaders/Private/ThermalPass.cpp unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalGpuTest.cpp
git commit -F - <<'EOF'
feat(thermal): ThermalCS land-cover lookup and blend, mirrored by CamSimThermalRef (ROADMAP 4B)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
EOF
```

---

### Task 9: Per-frame land-cover parameters (`FThermalFrameBuilder`) and the input sanitiser (`ThermalFrameSources`)

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalFrameBuilder.h`, `.cpp`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalFrameSources.h`, `.cpp`
- Test: `unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalLandCoverBuilderTest.cpp`, modify `unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalSourcesTest.cpp`

**Interfaces:**
- Consumes: `FLandCoverClassTable`, `CamSimLandCover::{FWindowSpec, GeodeticToWindowEN, IsWindowAllowed}`, `FLandCoverWindowData` (Tasks 3, 4, 6), the Task 7 parameters, `FCamSimConfig::FThermalConfig::LandCover`.
- Produces:
  ```cpp
  struct FThermalLandCoverInput { bool bValid = false; uint32 WindowId = 0; double CentreLatDeg = 0.0; double CentreLonDeg = 0.0;
      int32 Texels = 0; float TexelM = 10.0f; FVector EastWorld = FVector(1.0, 0.0, 0.0); FVector NorthWorld = FVector(0.0, -1.0, 0.0); };
  FThermalLandCoverInput FThermalFrameInputs::LandCover;
  static constexpr float FThermalFrameBuilder::AsphaltRampLuma = 0.04f;
  const FLandCoverClassTable& FThermalFrameBuilder::GetLandCoverTable() const;
  namespace CamSimThermal {
      CAMSIMTEST_API bool SetLandCover(FThermalFrameInputs& Out, const FLandCoverWindowData* Window, const FVector& EastWorld, const FVector& NorthWorld);
      CAMSIMTEST_API void LandCoverAxesWorld(const ACesiumGeoreference& Geo, double LatDeg, double LonDeg, FVector& OutEast, FVector& OutNorth);
  }
  ```

- [ ] **Step 1: Write the failing tests** — `Tests/ThermalLandCoverBuilderTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Thermal/LandCoverGeometry.h"
#include "Thermal/ThermalFrameBuilder.h"
#include "Thermal/ThermalMaterials.h"
#include "Time/SimClock.h"
#include "Tests/ThermalTestScene.h"

// CamSim.Thermal.Builder.LandCover*: land-cover fields of FThermalFrameParams (ROADMAP 4B).

namespace
{
	FThermalFrameInputs SanFrancisco()
	{
		FThermalFrameInputs In;
		In.UtcMicros = FSimClock::ToMicros(FDateTime(2026, 12, 21, 20, 10));
		In.CamLatDeg = 37.7989; In.CamLonDeg = -122.4662; In.CamAltHaeM = 342.0;
		In.AirTempC = 15.0; In.WaterTempC = 15.0;
		In.bHasSea = true; In.SeaLevelHaeM = -32.0; In.MaxWaveAmplitudeM = 0.4;
		In.SunIlluminanceLux = 100000.0;
		return In;
	}

	FThermalLandCoverInput Window(double Lat, double Lon)
	{
		FThermalLandCoverInput L;
		L.bValid = true; L.WindowId = 5; L.CentreLatDeg = Lat; L.CentreLonDeg = Lon; L.Texels = 2048; L.TexelM = 10.0f;
		L.EastWorld = FVector(0.6, 0.8, 0.0);
		L.NorthWorld = FVector(0.8, -0.6, 0.0);
		return L;
	}

	FThermalFrameBuilder MakeBuilder(const FCamSimConfig::FThermalConfig& Cfg = FCamSimConfig::FThermalConfig())
	{
		FThermalFrameBuilder B;
		B.Configure(Cfg, 3.0f, 5.0f);
		return B;
	}

	/** CamSimThermalRef radiance of MakeScene's pixels with land-cover codes bound, under P's thermal values. */
	TArray<float> RenderScene(const FThermalFrameParams& P)
	{
		CamSimThermalTest::FThermalTestScene S = CamSimThermalTest::MakeScene(64, 36, 64, 36);
		FThermalFrameParams Q = P;
		Q.ClipToTranslatedWorld = S.P.ClipToTranslatedWorld;
		Q.Up = S.P.Up;
		S.LandCover = CamSimThermalTest::MakeLandCoverCodes(Q.LandCoverTexels);
		TArray<float> Out;
		for (const CamSimThermalRef::FPixelResult& R : CamSimThermalRef::Run(S.Images(true), Q)) Out.Add(R.Radiance);
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalBuilderLandCoverMappingTest, "CamSim.Thermal.Builder.LandCoverMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalBuilderLandCoverMappingTest::RunTest(const FString& Parameters)
{
	FThermalFrameBuilder B = MakeBuilder();
	FThermalFrameInputs In = SanFrancisco();
	In.LandCover = Window(37.79, -122.475);
	FThermalFrameParams P;
	B.Build(In, P);
	TestEqual(TEXT("on"), P.bLandCover, 1u);
	TestEqual(TEXT("window id"), P.LandCoverWindowId, 5u);
	CamSimLandCover::FWindowSpec Spec;
	Spec.CentreLatDeg = 37.79; Spec.CentreLonDeg = -122.475; Spec.Texels = 2048; Spec.TexelM = 10.0f;
	const FVector2D Off = CamSimLandCover::GeodeticToWindowEN(Spec, In.CamLatDeg, In.CamLonDeg);
	TestNearlyEqual(TEXT("camera offset E (doubles, same mapping as the resample)"), P.LandCoverCamOffsetM.X, static_cast<float>(Off.X), 1e-3f);
	TestNearlyEqual(TEXT("camera offset N"), P.LandCoverCamOffsetM.Y, static_cast<float>(Off.Y), 1e-3f);
	TestTrue(TEXT("camera is north-east of the centre, ~0.8 / 1.0 km"), Off.X > 700.0 && Off.X < 850.0 && Off.Y > 950.0 && Off.Y < 1030.0);
	TestTrue(TEXT("axes copied"), P.LandCoverEast.Equals(FVector3f(0.6f, 0.8f, 0.0f), 1e-6f) && P.LandCoverNorth.Equals(FVector3f(0.8f, -0.6f, 0.0f), 1e-6f));
	TestEqual(TEXT("texels"), P.LandCoverTexels, 2048u);
	TestEqual(TEXT("texel size"), P.LandCoverTexelM, 10.0f);
	TestEqual(TEXT("code 10 -> tree_canopy"), static_cast<int32>(P.LandCoverClass[10]), FThermalMaterialTable::TreeCanopy);
	TestEqual(TEXT("code 50 family built-up"), P.LandCoverFamily[50], FThermalFrameParams::LandCoverFamilyBuiltUp);
	TestEqual(TEXT("refinement on with base colour"), P.bLandCoverRefine, 1u);
	TestEqual(TEXT("vegetation target"), P.VegetationClass, static_cast<uint32>(FThermalMaterialTable::Vegetation));
	TestEqual(TEXT("bare target"), P.BareSoilClass, static_cast<uint32>(FThermalMaterialTable::BareSoil));
	TestEqual(TEXT("asphalt target"), P.AsphaltClass, static_cast<uint32>(FThermalMaterialTable::Asphalt));
	TestEqual(TEXT("concrete target"), P.ConcreteClass, static_cast<uint32>(FThermalMaterialTable::Concrete));
	TestEqual(TEXT("thresholds from config"), P.VegIndexLo, FCamSimConfig::FThermalConfig::FLandCoverConfig().VegIndexLo);
	TestEqual(TEXT("ramp"), P.AsphaltRampLuma, FThermalFrameBuilder::AsphaltRampLuma);
	In.bBaseColorAvailable = false;
	B.Build(In, P);
	TestEqual(TEXT("no base colour: refinement off"), P.bLandCoverRefine, 0u);
	In.bBaseColorAvailable = true;
	In.SunIlluminanceLux = 0.0;   // night: fast term off, refinement stays on
	B.Build(In, P);
	TestTrue(TEXT("night: KFastScale 0 but refinement on"), P.KFastScale == 0.0f && P.bLandCoverRefine == 1u);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalBuilderLandCoverOffTest, "CamSim.Thermal.Builder.LandCoverDisabledIs4A",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalBuilderLandCoverOffTest::RunTest(const FString& Parameters)
{
	FThermalFrameParams NoWindow;
	{
		FThermalFrameBuilder B = MakeBuilder();
		B.Build(SanFrancisco(), NoWindow);
	}
	TestEqual(TEXT("no window: off"), NoWindow.bLandCover, 0u);
	const TArray<float> Reference = RenderScene(NoWindow);
	auto Check = [&](const FThermalFrameParams& P, const TCHAR* Why)
	{
		TestEqual(*FString::Printf(TEXT("%s: off"), Why), P.bLandCover, 0u);
		TestEqual(*FString::Printf(TEXT("%s: no window id"), Why), P.LandCoverWindowId, 0u);
		const TArray<float> R = RenderScene(P);
		TestTrue(*FString::Printf(TEXT("%s: bit for bit the no-window output"), Why),
			R.Num() == Reference.Num() && FMemory::Memcmp(R.GetData(), Reference.GetData(), R.Num() * sizeof(float)) == 0);
	};
	{
		FCamSimConfig::FThermalConfig Cfg;
		Cfg.LandCover.bEnabled = false;
		FThermalFrameBuilder B = MakeBuilder(Cfg);
		FThermalFrameInputs In = SanFrancisco();
		In.LandCover = Window(37.79, -122.475);
		FThermalFrameParams P;
		B.Build(In, P);
		Check(P, TEXT("thermal.land_cover.enabled false with a window"));
	}
	{
		FThermalFrameBuilder B = MakeBuilder();
		FThermalFrameInputs In = SanFrancisco();
		FThermalFrameParams P;
		In.LandCover = Window(37.79, -122.475);
		B.Build(In, P);                       // on ...
		In.LandCover.bValid = false;
		B.Build(In, P);                       // ... then the window goes away: every mapping field back to its default
		Check(P, TEXT("window lost, params reused"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalBuilderLandCoverInvalidTest, "CamSim.Thermal.Builder.LandCoverPoleAndInvalidWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalBuilderLandCoverInvalidTest::RunTest(const FString& Parameters)
{
	FThermalFrameBuilder B = MakeBuilder();
	auto Off = [&](TFunction<void(FThermalLandCoverInput&)> Break)
	{
		FThermalFrameInputs In = SanFrancisco();
		In.LandCover = Window(37.79, -122.475);
		Break(In.LandCover);
		FThermalFrameParams P;
		B.Build(In, P);
		return P.bLandCover == 0u;
	};
	TestTrue(TEXT("window at 89.5 N"), Off([](FThermalLandCoverInput& L) { L.CentreLatDeg = 89.5; }));
	TestTrue(TEXT("zero texels"), Off([](FThermalLandCoverInput& L) { L.Texels = 0; }));
	TestTrue(TEXT("zero texel size"), Off([](FThermalLandCoverInput& L) { L.TexelM = 0.0f; }));
	TestTrue(TEXT("window id 0"), Off([](FThermalLandCoverInput& L) { L.WindowId = 0; }));
	TestFalse(TEXT("a valid window is on"), Off([](FThermalLandCoverInput&) {}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalBuilderLandCoverClassesTest, "CamSim.Thermal.Builder.LandCoverClassesInRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalBuilderLandCoverClassesTest::RunTest(const FString& Parameters)
{
	FCamSimConfig::FThermalConfig Cfg;
	FThermalMaterialSpec Gravel;
	Gravel.Name = TEXT("gravel");
	Cfg.Materials.Add(Gravel);
	FLandCoverClassSpec ToGravel; ToGravel.Key = TEXT("60"); ToGravel.Code = 60; ToGravel.Material = TEXT("gravel");
	FLandCoverClassSpec ToLava;   ToLava.Key = TEXT("10");   ToLava.Code = 10;   ToLava.Material = TEXT("lava");
	Cfg.LandCover.Classes = { ToGravel, ToLava };
	FThermalFrameBuilder B = MakeBuilder(Cfg);
	FThermalFrameInputs In = SanFrancisco();
	In.LandCover = Window(37.79, -122.475);
	FThermalFrameParams P;
	TArray<FString> Warnings;
	B.Build(In, P, &Warnings);
	B.Build(In, P, &Warnings);
	TestEqual(TEXT("unknown class material warned once"), Warnings.FilterByPredicate([](const FString& W) { return W.Contains(TEXT("lava")); }).Num(), 1);
	TestEqual(TEXT("code 60 -> gravel"), static_cast<int32>(P.LandCoverClass[60]), B.GetMaterials().Find(TEXT("gravel")));
	TestEqual(TEXT("code 10 keeps tree_canopy"), static_cast<int32>(P.LandCoverClass[10]), FThermalMaterialTable::TreeCanopy);
	int32 Bad = 0;
	for (int32 C = 0; C < FThermalFrameParams::NumLandCoverCodes; ++C) Bad += P.LandCoverClass[C] >= P.NumClasses ? 1 : 0;
	Bad += (P.VegetationClass >= P.NumClasses || P.BareSoilClass >= P.NumClasses || P.AsphaltClass >= P.NumClasses || P.ConcreteClass >= P.NumClasses) ? 1 : 0;
	TestEqual(TEXT("no class index >= NumClasses reaches the shader"), Bad, 0);
	Cfg.LandCover.Classes = { ToGravel };
	ToGravel.Code = 50;
	ToGravel.Key = TEXT("50");
	Cfg.LandCover.Classes.Add(ToGravel);
	B.Configure(Cfg, 3.0f, 5.0f);   // hot reload of the classes
	B.Build(In, P);
	TestEqual(TEXT("hot reload: code 50 -> gravel"), static_cast<int32>(P.LandCoverClass[50]), B.GetMaterials().Find(TEXT("gravel")));
	TestEqual(TEXT("hot reload: code 10 back to tree_canopy"), static_cast<int32>(P.LandCoverClass[10]), FThermalMaterialTable::TreeCanopy);
	return true;
}
```

Append to `Tests/ThermalSourcesTest.cpp` (add `#include "Thermal/LandCoverWindow.h"` at the top):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalSourcesLandCoverTest, "CamSim.Thermal.Sources.LandCoverInputSanitized",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalSourcesLandCoverTest::RunTest(const FString& Parameters)
{
	const double NaN = std::numeric_limits<double>::quiet_NaN();
	FLandCoverWindowData W;
	W.Id = 3;
	W.Spec.CentreLatDeg = 37.79; W.Spec.CentreLonDeg = -122.475; W.Spec.Texels = 2048; W.Spec.TexelM = 10.0f;
	W.NonZeroTexels = 100;
	FThermalFrameInputs In;
	TestTrue(TEXT("valid window accepted"), CamSimThermal::SetLandCover(In, &W, FVector(2, 0, 0), FVector(0, -3, 0)));
	TestTrue(TEXT("bValid"), In.LandCover.bValid);
	TestEqual(TEXT("id"), In.LandCover.WindowId, 3u);
	TestEqual(TEXT("centre lat"), In.LandCover.CentreLatDeg, 37.79);
	TestEqual(TEXT("texels"), In.LandCover.Texels, 2048);
	TestTrue(TEXT("axes normalised"), In.LandCover.EastWorld.Equals(FVector(1, 0, 0), 1e-12) && In.LandCover.NorthWorld.Equals(FVector(0, -1, 0), 1e-12));
	auto Rejects = [&](const FLandCoverWindowData* Win, const FVector& E, const FVector& N)
	{
		FThermalFrameInputs X;
		const bool bOk = CamSimThermal::SetLandCover(X, Win, E, N);
		return !bOk && !X.LandCover.bValid;
	};
	TestTrue(TEXT("no window"), Rejects(nullptr, FVector(1, 0, 0), FVector(0, -1, 0)));
	FLandCoverWindowData Empty = W;
	Empty.NonZeroTexels = 0;
	TestTrue(TEXT("window without data"), Rejects(&Empty, FVector(1, 0, 0), FVector(0, -1, 0)));
	FLandCoverWindowData NoId = W;
	NoId.Id = 0;
	TestTrue(TEXT("window id 0"), Rejects(&NoId, FVector(1, 0, 0), FVector(0, -1, 0)));
	TestTrue(TEXT("NaN east"), Rejects(&W, FVector(NaN, 0, 0), FVector(0, -1, 0)));
	TestTrue(TEXT("zero north"), Rejects(&W, FVector(1, 0, 0), FVector::ZeroVector));
	TestTrue(TEXT("axes not at right angles"), Rejects(&W, FVector(1, 0, 0), FVector(1, 1, 0)));
	return true;
}
```

- [ ] **Step 2: Run to verify failure** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5` → Expected: compile errors: `FThermalLandCoverInput` undeclared, `SetLandCover` not a member of `CamSimThermal`.

- [ ] **Step 3: Implement the builder.** `Thermal/ThermalFrameBuilder.h` — add `#include "Thermal/LandCoverClasses.h"`, then before `FThermalFrameInputs`:

```cpp
/** The land-cover window one frame maps against (ROADMAP 4B); CamSimThermal::SetLandCover fills it from FLandCoverWindow. */
struct FThermalLandCoverInput
{
	bool    bValid = false;
	uint32  WindowId = 0;                              // FLandCoverWindowData::Id (pairs the params with the GPU window)
	double  CentreLatDeg = 0.0;
	double  CentreLonDeg = 0.0;
	int32   Texels = 0;
	float   TexelM = 10.0f;
	FVector EastWorld  = FVector(1.0, 0.0, 0.0);       // unit East at the window centre, UE world (= translated world direction)
	FVector NorthWorld = FVector(0.0, -1.0, 0.0);
};
```

add to `FThermalFrameInputs` (after `Entities`):

```cpp
	FThermalLandCoverInput LandCover;                  // ROADMAP 4B; invalid = land cover off this frame
```

add to `FThermalFrameBuilder` public constants and accessors:

```cpp
	static constexpr float  AsphaltRampLuma = 0.04f;   // built-up asphalt -> concrete ramp width (base luminance)
	const FLandCoverClassTable& GetLandCoverTable() const { return LandCoverTable; }
```

and the private member `FLandCoverClassTable LandCoverTable;`. Update the class comment: "... and the 256-entry stencil table rebuilt from the live entities; land-cover tables and the window mapping (ROADMAP 4B)."

`Thermal/ThermalFrameBuilder.cpp` — add `#include "Thermal/LandCoverGeometry.h"`; `Configure` becomes:

```cpp
void FThermalFrameBuilder::Configure(const FCamSimConfig::FThermalConfig& Cfg, float BandLoUm, float BandHiUm)
{
	if (!Band.IsBuilt() || Band.GetLoUm() != BandLoUm || Band.GetHiUm() != BandHiUm)
	{
		Band.Build(BandLoUm, BandHiUm);
	}
	const bool bMaterials = !bConfigured || !(Config.Materials == Cfg.Materials);
	if (bMaterials)
	{
		PendingWarnings.Append(Materials.Build(Cfg.Materials));
	}
	if (bMaterials || !(Config.LandCover.Classes == Cfg.LandCover.Classes))
	{
		PendingWarnings.Append(LandCoverTable.Build(Cfg.LandCover.Classes, Materials));
	}
	Config = Cfg;
	bConfigured = true;
}
```

and at the end of `Build` (after `Out.InputScale = 1.0f;`):

```cpp
	// Land cover (ROADMAP 4B). The tables and thresholds are filled every frame; without an enabled, valid window the mapping
	// fields are reset to their defaults and bLandCover = 0, so ThermalCS runs 4A's terrain path.
	FMemory::Memcpy(Out.LandCoverClass, LandCoverTable.Class, sizeof(Out.LandCoverClass));
	FMemory::Memcpy(Out.LandCoverFamily, LandCoverTable.Family, sizeof(Out.LandCoverFamily));
	Out.VegetationClass  = FThermalMaterialTable::Vegetation;
	Out.BareSoilClass    = FThermalMaterialTable::BareSoil;
	Out.AsphaltClass     = FThermalMaterialTable::Asphalt;
	Out.ConcreteClass    = FThermalMaterialTable::Concrete;
	Out.VegIndexLo       = Config.LandCover.VegIndexLo;
	Out.VegIndexHi       = Config.LandCover.VegIndexHi;
	Out.AsphaltMaxLuma   = Config.LandCover.AsphaltMaxLuma;
	Out.AsphaltRampLuma  = AsphaltRampLuma;
	Out.bLandCoverRefine = In.bBaseColorAvailable ? 1u : 0u;   // not the fast term: base colour is valid at night too
	const FThermalLandCoverInput& L = In.LandCover;
	if (Config.LandCover.bEnabled && L.bValid && L.WindowId != 0u && L.Texels >= 2 && L.TexelM > 0.0f
		&& CamSimLandCover::IsWindowAllowed(L.CentreLatDeg))
	{
		CamSimLandCover::FWindowSpec Spec;
		Spec.CentreLatDeg = L.CentreLatDeg;
		Spec.CentreLonDeg = L.CentreLonDeg;
		Spec.Texels = L.Texels;
		Spec.TexelM = L.TexelM;
		const FVector2D Off = CamSimLandCover::GeodeticToWindowEN(Spec, In.CamLatDeg, In.CamLonDeg);   // doubles
		Out.bLandCover          = 1u;
		Out.LandCoverWindowId   = L.WindowId;
		Out.LandCoverEast       = FVector3f(L.EastWorld);
		Out.LandCoverNorth      = FVector3f(L.NorthWorld);
		Out.LandCoverCamOffsetM = FVector2f(static_cast<float>(Off.X), static_cast<float>(Off.Y));
		Out.LandCoverTexelM     = L.TexelM;
		Out.LandCoverTexels     = static_cast<uint32>(L.Texels);
	}
	else
	{
		static const FThermalFrameParams Defaults;
		Out.bLandCover          = 0u;
		Out.LandCoverWindowId   = 0u;
		Out.LandCoverEast       = Defaults.LandCoverEast;
		Out.LandCoverNorth      = Defaults.LandCoverNorth;
		Out.LandCoverCamOffsetM = Defaults.LandCoverCamOffsetM;
		Out.LandCoverTexelM     = Defaults.LandCoverTexelM;
		Out.LandCoverTexels     = Defaults.LandCoverTexels;
	}
```

- [ ] **Step 4: Implement the sources helpers.** `Thermal/ThermalFrameSources.h` — forward declarations `struct FLandCoverWindowData;` and `class ACesiumGeoreference;`, and in `namespace CamSimThermal`:

```cpp
	/** The land-cover window this frame maps against (ROADMAP 4B). No window, a window without data or id, or axes that aren't
	 *  finite, non-zero and at right angles (|E.N| <= 1e-3 after normalising) leave Out.LandCover invalid (land cover off).
	 *  Returns whether it was accepted. */
	CAMSIMTEST_API bool SetLandCover(FThermalFrameInputs& Out, const FLandCoverWindowData* Window, const FVector& EastWorld, const FVector& NorthWorld);
	/** East and North (UE world) at a geodetic point from the Cesium georeference: the East-South-Up frame there, South negated.
	 *  Called every frame (Cesium origin shifting rotates the UE axes). Game thread. */
	CAMSIMTEST_API void LandCoverAxesWorld(const ACesiumGeoreference& Geo, double LatDeg, double LonDeg, FVector& OutEast, FVector& OutNorth);
```

`Thermal/ThermalFrameSources.cpp` — add `#include "Thermal/LandCoverWindow.h"` and `#include "CesiumGeoreference.h"`, and:

```cpp
bool CamSimThermal::SetLandCover(FThermalFrameInputs& Out, const FLandCoverWindowData* Window, const FVector& EastWorld, const FVector& NorthWorld)
{
	Out.LandCover = FThermalLandCoverInput();
	if (!Window || Window->Id == 0u || Window->NonZeroTexels <= 0) return false;
	auto Finite = [](const FVector& V) { return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z); };
	if (!Finite(EastWorld) || !Finite(NorthWorld)) return false;
	const FVector E = EastWorld.GetSafeNormal();
	const FVector N = NorthWorld.GetSafeNormal();
	if (E.IsZero() || N.IsZero() || FMath::Abs(FVector::DotProduct(E, N)) > 1e-3) return false;
	FThermalLandCoverInput& L = Out.LandCover;
	L.bValid       = true;
	L.WindowId     = Window->Id;
	L.CentreLatDeg = Window->Spec.CentreLatDeg;
	L.CentreLonDeg = Window->Spec.CentreLonDeg;
	L.Texels       = Window->Spec.Texels;
	L.TexelM       = Window->Spec.TexelM;
	L.EastWorld    = E;
	L.NorthWorld   = N;
	return true;
}

void CamSimThermal::LandCoverAxesWorld(const ACesiumGeoreference& Geo, double LatDeg, double LonDeg, FVector& OutEast, FVector& OutNorth)
{
	const FVector Centre = Geo.TransformLongitudeLatitudeHeightPositionToUnreal(FVector(LonDeg, LatDeg, 0.0));   // longitude first
	const FMatrix EsuToUnreal = Geo.ComputeEastSouthUpToUnrealTransformation(Centre);
	OutEast  = EsuToUnreal.TransformVector(FVector(1.0, 0.0, 0.0)).GetSafeNormal();
	OutNorth = -EsuToUnreal.TransformVector(FVector(0.0, 1.0, 0.0)).GetSafeNormal();
}
```

- [ ] **Step 5: Run to verify pass** — build, then `run_tests CamSim.Thermal.Builder` → Expected: 0 failed (4 new `LandCover*` + every 4A builder test, including `PerFrameCost` — read its info line: the land-cover block adds two 256-byte copies), then `run_tests CamSim.Thermal.Sources` → 0 failed.

- [ ] **Step 6: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalFrameBuilder.h unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalFrameBuilder.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalFrameSources.h unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalFrameSources.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalLandCoverBuilderTest.cpp unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalSourcesTest.cpp
git commit -F - <<'EOF'
feat(thermal): per-frame land-cover parameters and window input sanitiser (ROADMAP 4B)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
EOF
```

---

### Task 10: Wire the window into the capture (window per thermal tick, render handoff, texture binding, frame stat)

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Thermal/LandCoverWindow.h` (`CamSimLandCover::ShouldBindWindow`)
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCaptureComponent.h`, `.cpp`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimFrameGrabExtension.h`, `.cpp`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimFrameStats.h`, `.cpp`
- Test: `unreal_project/CamSimTest/Source/CamSimTest/Tests/LandCoverWindowTest.cpp` (pairing), `unreal_project/CamSimTest/Source/CamSimTest/Tests/RenderPathTest.cpp` (row field)

**Interfaces:**
- Consumes: `FLandCoverWindow` (Task 4), `CamSimThermal::{SetLandCover, LandCoverAxesWorld}` and `FThermalFrameInputs::LandCover` (Task 9), `FThermalPassInputs::LandCover` (Task 8), `FCamSimConfig::FThermalConfig::LandCover` (Task 6).
- Produces:
  ```cpp
  namespace CamSimLandCover { inline bool ShouldBindWindow(uint32 bLandCover, uint32 ParamsWindowId, uint32 GpuWindowId, bool bHasTexture); }
  void UCamSimCaptureComponent::SetThermalPose(double LatDeg, double LonDeg, double AltHaeM, const FVector& UpWorld, ACesiumGeoreference* Georeference = nullptr);
  uint32 UCamSimCaptureComponent::GetLandCoverWindowId() const;      // window paired with the last tick's thermal params, 0 = none
  void FCamSimFrameGrabExtension::SetThermalParams_RenderThread(TSharedPtr<const FThermalFrameParams, ESPMode::ThreadSafe> P,
      TSharedPtr<FLandCoverGpuWindow, ESPMode::ThreadSafe> LandCover = nullptr);
  uint32 FCamSimFrameStatsSample::LandCoverWindow = 0;                // JSON "land_cover_window"
  ```

- [ ] **Step 1: Write the failing tests.** Append to `Tests/LandCoverWindowTest.cpp`:

```cpp
// Review Focus 4: a frame binds a window texture only when it is the window its parameters were built against.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverPairingTest, "CamSim.Thermal.LandCover.FramePairingById",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverPairingTest::RunTest(const FString& Parameters)
{
	using CamSimLandCover::ShouldBindWindow;
	TestTrue (TEXT("same window, texture uploaded"), ShouldBindWindow(1u, 7u, 7u, true));
	TestFalse(TEXT("params from window 7, texture from window 8 (swap mid-flight)"), ShouldBindWindow(1u, 7u, 8u, true));
	TestFalse(TEXT("upload not run yet"), ShouldBindWindow(1u, 7u, 7u, false));
	TestFalse(TEXT("land cover off in the params"), ShouldBindWindow(0u, 7u, 7u, true));
	TestFalse(TEXT("no window id"), ShouldBindWindow(1u, 0u, 0u, true));

	// Every re-centre publishes a new id, so params built before a swap can never match the window published after it.
	FLandCoverWindow W;
	W.Configure(Settings(WriteSfDir(TEXT("Pairing")), 256));
	W.Update(CamLat, CamLon);
	W.FinishBuildForTest();
	if (!TestTrue(TEXT("first window"), W.GetCurrentGpu().IsValid())) return false;
	const uint32 First = W.GetCurrentGpu()->Id;
	double Lat = 0.0, Lon = 0.0;
	CamSimLandCover::WindowENToGeodetic(W.GetCurrent()->Spec, 0.0, 800.0, Lat, Lon);
	W.Update(Lat, Lon);
	W.FinishBuildForTest();
	const uint32 Second = W.GetCurrentGpu()->Id;
	TestNotEqual(TEXT("new id per window"), First, Second);
	TestEqual(TEXT("CPU and GPU halves share the id"), W.GetCurrent()->Id, Second);
	TestFalse(TEXT("old params never bind the new texture"), ShouldBindWindow(1u, First, Second, true));
	return true;
}
```

In `Tests/RenderPathTest.cpp` `FFrameStatsRowTest`: after `S.SensorGpuMs = 1.25; ...` add `S.LandCoverWindow = 3;`, after the `thermal_gpu_ms` check add

```cpp
	TestEqual(TEXT("land_cover_window"), static_cast<int32>(Obj->GetNumberField(TEXT("land_cover_window"))), 3);
```

and in the legacy part, after `S.ThermalGpuMs = -1.0f;` add `S.LandCoverWindow = 0;` and after the legacy `thermal_gpu_ms` check:

```cpp
	TestEqual(TEXT("legacy land_cover_window"), static_cast<int32>(Legacy->GetNumberField(TEXT("land_cover_window"))), 0);
```

- [ ] **Step 2: Run to verify failure** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5` → Expected: compile errors, `ShouldBindWindow` not a member of `CamSimLandCover`, `LandCoverWindow` not a member of `FCamSimFrameStatsSample`.

- [ ] **Step 3: Implement the pairing rule and the frame stat.** `Thermal/LandCoverWindow.h` — append:

```cpp
namespace CamSimLandCover
{
	/**
	 * Render thread (ROADMAP 4B): whether a frame may bind its land-cover texture. The parameters must ask for land cover,
	 * the GPU window must be the one they were built against (same id; a re-centre publishes a new id) and its upload must have
	 * run. Otherwise the frame renders land cover off: never a texture from one window with another window's mapping.
	 */
	inline bool ShouldBindWindow(uint32 bLandCover, uint32 ParamsWindowId, uint32 GpuWindowId, bool bHasTexture)
	{
		return bLandCover != 0u && ParamsWindowId != 0u && ParamsWindowId == GpuWindowId && bHasTexture;
	}
}
```

`Camera/CamSimFrameStats.h` — add after `ThermalGpuMs`:

```cpp
	uint32 LandCoverWindow    = 0;      // land-cover window id paired with this tick's thermal params, 0 = none (ROADMAP 4B)
```

`Camera/CamSimFrameStats.cpp` — the format string's last line and arguments become:

```cpp
		TEXT("\"sensor_gpu_ms\":%.3f,\"thermal_gpu_ms\":%.3f,\"land_cover_window\":%u,\"sensor_gain_ev\":%s,\"scene_median_log2\":%s}"),
		S.UtcSeconds, S.WallMs, S.GameMs, S.RenderMs, S.RhiMs, S.GpuMs,
		static_cast<unsigned long long>(S.FramesEmitted), static_cast<unsigned long long>(S.FramesDropped),
		S.MinLoadProgressPct, S.Sse, S.bCameraCut ? TEXT("true") : TEXT("false"), S.ViewFamilies,
		S.SensorGpuMs, S.ThermalGpuMs, S.LandCoverWindow, *Num(S.SensorGainEv), *Num(S.SceneMedianLog2));
```

- [ ] **Step 4: Run the unit tests** — build, then `run_tests CamSim.Thermal.LandCover` and `run_tests CamSim.Render.FrameStats` → Expected: 0 failed.

- [ ] **Step 5: Wire the capture component.** `Camera/CamSimCaptureComponent.h` — add `#include "Thermal/LandCoverWindow.h"` after `Thermal/ThermalFrameBuilder.h`, `class ACesiumGeoreference;` with the other forward declarations; replace the `SetThermalPose` declaration with

```cpp
	/** The thermal model's camera pose (ROADMAP 4A) and the georeference the land-cover axes come from (ROADMAP 4B). */
	void SetThermalPose(double LatDeg, double LonDeg, double AltHaeM, const FVector& UpWorld, ACesiumGeoreference* Georeference = nullptr);
	/** Land-cover window paired with the last tick's thermal parameters; 0 when land cover was off (frame stats). */
	uint32 GetLandCoverWindowId() const { return LandCoverWindowIdLastTick; }
```

and next to `ThermalBuilder`:

```cpp
	/** ROADMAP 4B: camera-centred land-cover window, updated on thermal IR ticks only (task-thread builds, never blocks). */
	FLandCoverWindow LandCover;
	TWeakObjectPtr<ACesiumGeoreference> ThermalGeoreference;
	uint32 LandCoverWindowIdLastTick = 0;
```

`Camera/CamSimCaptureComponent.cpp` — `SetThermalPose` gains `ThermalGeoreference = Georeference;`. In `UpdateSensorParams`, declare before `if (bThermal)`:

```cpp
	TSharedPtr<FLandCoverGpuWindow, ESPMode::ThreadSafe> LandCoverGpu;   // ROADMAP 4B: travels with this tick's thermal params
```

and inside `if (bThermal)`, between `GatherFrameInputs(...)` and `ThermalParams = MakeShared<...>();`:

```cpp
		// ROADMAP 4B: land cover. The window updates (and builds off-thread) only while thermal IR runs; its East/North axes are
		// recomputed every tick at the window centre (origin shifts rotate the UE axes).
		if (Cfg.Thermal.LandCover.bEnabled)
		{
			FLandCoverWindow::FSettings LS;
			LS.Dir              = Cfg.Thermal.LandCover.Dir;
			LS.Texels           = Cfg.Thermal.LandCover.WindowTexels;
			LS.RecentreFraction = Cfg.Thermal.LandCover.RecentreFraction;
			LandCover.Configure(LS);
			LandCover.Update(ThermalLat, ThermalLon);
			for (const FString& W : LandCover.TakeWarnings()) { UE_LOG(LogCamSim, Warning, TEXT("Thermal land cover: %s"), *W); }
			const TSharedPtr<const FLandCoverWindowData, ESPMode::ThreadSafe> Window = LandCover.GetCurrent();
			FVector East = FVector::ZeroVector, North = FVector::ZeroVector;
			if (Window.IsValid() && ThermalGeoreference.IsValid())
			{
				CamSimThermal::LandCoverAxesWorld(*ThermalGeoreference.Get(), Window->Spec.CentreLatDeg, Window->Spec.CentreLonDeg, East, North);
			}
			if (CamSimThermal::SetLandCover(TIn, Window.Get(), East, North))
			{
				LandCoverGpu = LandCover.GetCurrentGpu();
			}
		}
```

after `ThermalBuilder.Build(TIn, *ThermalParams, &Warnings);`:

```cpp
		LandCoverWindowIdLastTick = ThermalParams->bLandCover != 0u ? ThermalParams->LandCoverWindowId : 0u;
```

and after `bThermalActiveLastTick = bThermal;`:

```cpp
	if (!bThermal) LandCoverWindowIdLastTick = 0u;
```

The `camsim.Thermal.Log` statement becomes (one field added at the end; `thermal_check.py` keys its dedup on the text before ` gainEv=`, so it is unaffected):

```cpp
			UE_LOG(LogCamSim, Log, TEXT("Thermal: classes[K]%s terrain=%u water=%u Tair=%.1f K cloud=%.2f epsZ=%.3f KLum=%.1f "
				"KFast=%.2f EClamp=%.0f signalScale=%.4g gainEv=%.2f sunLux=%.0f entities=%d landCover=%u"),
				*Classes, T.TerrainClass, T.WaterClass, T.TairK, T.Cloud, T.SkyEpsZ, T.KLum, T.KFastScale, T.EClampWm2,
				ThermalBuilder.GetSignalScale(), SensorController.GetGainEv(), TIn.SunIlluminanceLux, TIn.Entities.Num(),
				T.bLandCover != 0u ? T.LandCoverWindowId : 0u);
```

The render command at the end of `UpdateSensorParams` becomes:

```cpp
	// One command: a frame never pairs IR thermal parameters with EO sensor parameters (or the reverse), nor thermal parameters
	// with another tick's land-cover window (ROADMAP 4B).
	TSharedPtr<FCamSimFrameGrabExtension, ESPMode::ThreadSafe> Ext = GrabExtension;
	TSharedPtr<const FThermalFrameParams, ESPMode::ThreadSafe> ConstThermal = ThermalParams;
	ENQUEUE_RENDER_COMMAND(CamSimSensorParams)([Ext, Params, ConstThermal, LandCoverGpu](FRHICommandListImmediate&)
	{
		Ext->SetParams_RenderThread(Params);
		Ext->SetThermalParams_RenderThread(ConstThermal, LandCoverGpu);
	});
```

`Camera/CamSimCamera.cpp` — the 4A thermal-pose block becomes:

```cpp
	// ROADMAP 4A: the thermal model's camera pose — the KLV's sensor position (Tags 13/14/75) and the geodetic up
	// (the georeference's East-South-Up Z axis at the sensor, in UE world space). ROADMAP 4B: the georeference also gives the
	// land-cover window's East/North axes.
	{
		FVector Up = FVector::UpVector;
		ACesiumGeoreference* Geo = GlobeAnchor ? GlobeAnchor->ResolveGeoreference() : nullptr;
		if (Geo)
		{
			// A degenerate result stays as is: GatherFrameInputs sanitises the up vector (+Z fallback).
			Up = Geo->ComputeEastSouthUpToUnrealTransformation(SceneCapture->GetComponentLocation())
				.TransformVector(FVector::UpVector).GetSafeNormal();
		}
		const FCamSimTelemetry& T = Telemetry.Get();
		CaptureComp->SetThermalPose(T.Latitude, T.Longitude, T.Altitude, Up, Geo);
	}
```

and with the other frame-stats fields: `S.LandCoverWindow = CaptureComp->GetLandCoverWindowId();`.

`Camera/CamSimFrameGrabExtension.h` — add `struct FLandCoverGpuWindow;` to the forward declarations; replace `SetThermalParams_RenderThread` with

```cpp
	void SetThermalParams_RenderThread(TSharedPtr<const FThermalFrameParams, ESPMode::ThreadSafe> P,
		TSharedPtr<FLandCoverGpuWindow, ESPMode::ThreadSafe> LandCover = nullptr)
	{
		ThermalParams = MoveTemp(P);
		ThermalLandCover = MoveTemp(LandCover);
	}
```

(keep the 4A doc comment and add: "LandCover (ROADMAP 4B) is the window those parameters were built against; ThermalCS binds its texture only when CamSimLandCover::ShouldBindWindow agrees.") and the member next to `ThermalParams`:

```cpp
	TSharedPtr<FLandCoverGpuWindow, ESPMode::ThreadSafe> ThermalLandCover;   // render thread; the window ThermalParams map against
```

`Camera/CamSimFrameGrabExtension.cpp` — add `#include "PooledRenderTarget.h"` and `#include "Thermal/LandCoverWindow.h"`; in `RunThermal_RenderThread`, after `Ti.OutputRect = SceneColor.ViewRect;`:

```cpp
	// ROADMAP 4B: the land-cover window these parameters were built against (ref-counted, set in the same render command).
	if (ThermalLandCover.IsValid() && CamSimLandCover::ShouldBindWindow(TP.bLandCover, TP.LandCoverWindowId, ThermalLandCover->Id,
		ThermalLandCover->Texture.IsValid()))
	{
		Ti.LandCover = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(ThermalLandCover->Texture, TEXT("CamSimLandCover")));
	}
```

- [ ] **Step 6: Run the full suites** — build, `run_tests CamSim` → Expected: 0 failed; `scripts/run_gpu_tests.sh CamSim.GPU` → all pass.

- [ ] **Step 7: Live smoke (IR night over the Presidio, land cover on then off).**

```bash
mkdir -p .cache/lc_smoke
CAMSIM_MULTICAST_ADDR=127.0.0.1 CAMSIM_SNAPSHOT_ENDPOINT_ENABLED=1 CAMSIM_FRAME_STATS_PATH="$PWD/.cache/lc_smoke/on.jsonl" \
  scripts/run.sh --headless --local --detach
uv run scripts/send_cigi_test.py --lat 37.7935 --lon -122.46 --alt 850 --pitch -90 --fov-h 40 --sensor-id 1 --time 0200 --duration 150 &
until curl -sf localhost:8080/ready >/dev/null; do sleep 2; done; sleep 60
curl -s localhost:8080/snapshot -o .cache/lc_smoke/ir_night_on.png
grep -m1 "LandCover: window #1" ~/Library/Logs/CamSimTest/CamSimTest.log
python3 -c "import json;r=[json.loads(l) for l in open('.cache/lc_smoke/on.jsonl') if l.startswith('{')];print('max window id',max(x['land_cover_window'] for x in r))"
scripts/stop.sh; wait
```

Expected: one `LandCover: window #1 at (37.79..., -122.46...), 2048^2 x 10 m, built in <N> ms (… tiles, … without data)` line, `max window id 1`, and `ir_night_on.png` showing terrain structure (tree stands, lawns, roads distinguishable; not one flat grey). Repeat with `CAMSIM_THERMAL_LAND_COVER_ENABLED=0` (`off.jsonl`, `ir_night_off.png`) → no `LandCover:` line and `max window id 0`; the off image looks like 4A. Linux: the log is under `unreal_project/CamSimTest/Saved/Logs/`. Put both PNG paths in the commit message.

- [ ] **Step 8: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Thermal/LandCoverWindow.h unreal_project/CamSimTest/Source/CamSimTest/Camera \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/LandCoverWindowTest.cpp unreal_project/CamSimTest/Source/CamSimTest/Tests/RenderPathTest.cpp
git commit -F - <<'EOF'
feat(thermal): land-cover window per thermal tick, paired with its params on the render thread (ROADMAP 4B)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
EOF
```

---

### Task 11: Acceptance gates (i)–(l) in `thermal_check.py` and the live run

**Files:**
- Modify: `scripts/thermal_check.py`
- Modify: `scripts/tests/test_thermal_check.py`
- Modify (only if tuning is needed, same commit): `deploy/camsim_config.yaml`, `docs/configuration.md`, `unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.h` (land-cover threshold defaults), `Thermal/ThermalMaterials.cpp` (class values), `ROADMAP.md` (why)

**Interfaces:**
- Consumes: frame-stats `land_cover_window` (Task 10), env `CAMSIM_THERMAL_LAND_COVER_ENABLED` / `_RECENTRE_FRACTION` (Task 6), the committed SF sample (Task 1); existing `thermal_check` helpers (`y_from_rgb`, `box_mask`, `scale_box`, `region_mean`, `overlay`, `load_view`, `load_rows`, `ViewData`, `View`, `RunSpec`, `MIN_PIXELS`).
- Produces (pure helpers): `exg(rgb)`, `central_mask(shape, frac=CENTRAL)`, `erode(mask, r)`, `greenness_masks(rgb, erode_px=2) -> (veg, non)`, `veg_contrast(y, veg, non) -> float`, `box_blur(y, k)`, `highpass_std(y, mask, k=HIGHPASS_K) -> float`, `pan_offset_m(t, leg_m=PAN_LEG_M, speed=PAN_SPEED_MPS) -> float`, `window_events(rows) -> list[int]`, `recentre_spike(rows, events, radius=RECENTRE_RADIUS) -> (spike_ms, median_ms)`, `entity_mask(vd) -> np.ndarray`, `run_group(label) -> str`, `run_band(label) -> str | None`; `View.sensor_id: int | None`; run groups `lcoff` and `pan`; gate rows `("i", band, "noon")`, `("j", band, "night")`, `("k", band, "night")`, `("l", "mwir", "noon")`.

- [ ] **Step 1: Write the failing tests** — in `scripts/tests/test_thermal_check.py`, replace `ALL_RUNS` and the two run-coverage tests:

```python
ALL_RUNS = {"bands", "hd", "eo", "lcoff", "pan"}


def test_expected_rows_cover_every_band_time_and_selected_run():
    rows = tc.expected_rows(["mwir", "lwir"], ALL_RUNS)
    assert len(rows) == 2 * 11 + 3
    assert ("e", "lwir", "noon") in rows and ("e", "lwir", "night") in rows
    for b in ("mwir", "lwir"):  # (h) static coast shimmer, both times; 4B land-cover gates
        assert ("h", b, "night") in rows and ("h", b, "noon") in rows
        assert ("i", b, "noon") in rows and ("j", b, "night") in rows and ("k", b, "night") in rows
    assert ("f", "mwir", "noon") in rows and ("g", "eo", "noon") in rows and ("l", "mwir", "noon") in rows
    assert rows[-1] == ("g", "eo", "noon")
    only_bands = tc.expected_rows(["mwir"], {"bands"})
    assert {r[0] for r in only_bands} == set("abcdehij")  # f, g, k, l not expected
    assert tc.expected_rows(["mwir"], {"eo"}) == [("g", "eo", "noon")]
    assert tc.expected_rows(["mwir"], {"lcoff"}) == []  # (k) needs the land-cover-on band run too
    assert tc.expected_rows(["mwir"], {"pan"}) == [("l", "mwir", "noon")]
```

and in `test_a_missing_row_fails` change the letter set to `set("abcdefghijkl")`. Append:

```python
def test_run_groups_and_bands():
    assert tc.run_group("mwir") == "bands" and tc.run_band("mwir") == "mwir"
    assert tc.run_group("lwir_lcoff") == "lcoff" and tc.run_band("lwir_lcoff") == "lwir"
    assert tc.run_group("mwir_pan") == "pan" and tc.run_band("mwir_pan") is None
    assert tc.run_group("mwir_1080p") == "hd" and tc.run_group("eo_thermal_off") == "eo"


def test_exg_and_greenness_masks():
    rgb = np.full((100, 200, 3), 128, np.uint8)
    rgb[:, :100] = (60, 140, 50)  # vegetation on the left
    rgb[40:60, 140:160] = (40, 60, 160)  # water-blue patch on the right
    rgb[70:80, 120:180] = (10, 10, 10)  # deep shadow
    assert tc.exg(rgb)[50, 50] > 0.3 and abs(tc.exg(rgb)[50, 150]) < 1e-6
    veg, non = tc.greenness_masks(rgb)
    assert not (veg & non).any()
    assert veg[50, 60] and not veg[50, 150]
    assert non[30, 130] and not non[50, 150] and not non[75, 150]  # blue and shadow excluded
    assert not veg[5, 60] and not non[5, 130]  # outside the central 60 %
    assert not veg[50, 99] and not non[50, 101]  # eroded at the boundary


def test_veg_contrast_sign_and_small_regions():
    y = np.full((50, 50), 100.0, np.float32)
    veg = np.zeros((50, 50), bool)
    non = np.zeros((50, 50), bool)
    veg[:, :25], non[:, 25:] = True, True
    y[:, :25] = 90.0
    assert tc.veg_contrast(y, veg, non) == pytest.approx(10.0)
    tiny = np.zeros((50, 50), bool)
    tiny[0, 0] = True
    assert math.isnan(tc.veg_contrast(y, tiny, non))


def test_box_blur_and_highpass_std():
    flat = np.full((80, 80), 50.0, np.float32)
    assert np.allclose(tc.box_blur(flat, 33), 50.0)
    mask = tc.central_mask((80, 80))
    assert tc.highpass_std(flat, mask) == pytest.approx(0.0, abs=1e-5)
    yy, xx = np.mgrid[0:80, 0:80]
    checker = flat + 10.0 * np.where(((yy // 4) + (xx // 4)) % 2 == 0, 1.0, -1.0)
    assert tc.highpass_std(checker, mask) == pytest.approx(10.0, rel=0.15)
    ramp = flat + 0.5 * xx  # a smooth gradient (vignetting-like) is not structure
    assert tc.highpass_std(ramp.astype(np.float32), mask) < 0.5


def test_pan_offset_is_a_triangle_at_the_pan_speed():
    period = 2.0 * tc.PAN_LEG_M / tc.PAN_SPEED_MPS
    assert tc.pan_offset_m(0.0) == pytest.approx(0.0)
    assert tc.pan_offset_m(period / 4) == pytest.approx(tc.PAN_LEG_M / 2)
    assert tc.pan_offset_m(period / 2) == pytest.approx(tc.PAN_LEG_M)
    assert tc.pan_offset_m(period) == pytest.approx(0.0, abs=1e-6)
    assert (tc.pan_offset_m(10.0) - tc.pan_offset_m(9.0)) == pytest.approx(tc.PAN_SPEED_MPS)


def test_window_events_and_recentre_spike():
    ids = [0, 0, 1, 1, 1, 2, 2, 3]
    rows = [{"land_cover_window": w, "wall_ms": 33.3} for w in ids]
    rows[5]["wall_ms"] = 45.3
    ev = tc.window_events(rows)
    assert ev == [5, 7]  # the first window is not a re-centre
    spike, base = tc.recentre_spike(rows, ev, radius=1)
    assert base == pytest.approx(33.3) and spike == pytest.approx(12.0)
    assert math.isnan(tc.recentre_spike(rows, [], radius=1)[0])
    assert tc.window_events([{"land_cover_window": 1}, {"land_cover_window": 0}, {"land_cover_window": 1}]) == []
```

- [ ] **Step 2: Run to verify failure** — Run: `uv run --with pytest --with numpy --with pillow --with pycocotools pytest scripts/tests/test_thermal_check.py -v` → Expected: failures (`AttributeError: module 'thermal_check' has no attribute 'run_group'` / `exg` / …; the expected-rows counts differ).

- [ ] **Step 3: Implement the pure helpers** — `scripts/thermal_check.py`, after the 4A thresholds:

```python
# ROADMAP 4B land-cover gates (spec "Testing", i-l)
VEG_NOON_DN = 3.0  # (i) noon: non-vegetation - vegetation >= +3 DN (white-hot: vegetation cooler)
VEG_NIGHT_DN = 2.0  # (j) night: non-vegetation (built-up / bare) - vegetation >= +2 DN
STRUCTURE_RATIO = 3.0  # (k) night high-pass std, land cover on / off
RECENTRE_SPIKE_MS = 5.0  # (l) max frame time near a re-centre - pan median
MIN_RECENTRES = 3
RECENTRE_RADIUS = 15  # frames either side of a re-centre
MIXED_CENTER = (37.7935, -122.4600)  # Presidio interior: forest, lawns, roads, buildings, no open water
MIXED_UP_M = 800.0
MIXED_FOV = 40.0
EXG_VEG = 0.06  # EO display RGB excess green above this: vegetation
EXG_NONVEG = 0.02  # ... below this: non-vegetation ground (neutral grey has ExG = 0)
CENTRAL = 0.6  # EO and IR presets differ in distortion: compare the central 60 % only
HIGHPASS_K = 33
PAN_UP_M = 600.0
PAN_LEG_M = 1200.0
PAN_SPEED_MPS = 1000.0 / 60.0  # the spec's 1 km/min
PAN_HOLD_S = 150.0
PAN_RECENTRE_FRACTION = "0.02"  # 409.6 m: re-centres every ~25 s at 1 km/min (the default 25 % needs > 5 min)
```

and with the other pure helpers:

```python
def exg(rgb: np.ndarray) -> np.ndarray:
    """Excess green of a display RGB image in chromatic coordinates: (2g - r - b) / (r + g + b)."""
    f = rgb.astype(np.float32) / 255.0
    return (2.0 * f[..., 1] - f[..., 0] - f[..., 2]) / (f.sum(axis=-1) + 1e-6)


def central_mask(shape: tuple[int, int], frac: float = CENTRAL) -> np.ndarray:
    h, w = shape
    y0, x0 = int(round(h * (1.0 - frac) / 2.0)), int(round(w * (1.0 - frac) / 2.0))
    m = np.zeros(shape, bool)
    m[y0 : h - y0, x0 : w - x0] = True
    return m


def erode(mask: np.ndarray, r: int) -> np.ndarray:
    """Binary erosion by a (2r + 1)^2 square; pixels within r of the border are cleared."""
    if r <= 0:
        return mask.copy()
    h, w = mask.shape
    inner = np.ones((h - 2 * r, w - 2 * r), bool)
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            inner &= mask[r + dy : h - r + dy, r + dx : w - r + dx]
    out = np.zeros_like(mask)
    out[r : h - r, r : w - r] = inner
    return out


def greenness_masks(rgb: np.ndarray, erode_px: int = 2) -> tuple[np.ndarray, np.ndarray]:
    """(vegetation, non-vegetation ground) from an EO frame: ExG > EXG_VEG / ExG < EXG_NONVEG; non-vegetation excludes
    water-blue (b > g, b > r) and deep shadow (Y < 30 DN). Central region only, eroded (EO/IR distortion differ)."""
    e = exg(rgb)
    f = rgb.astype(np.float32)
    blue = (f[..., 2] > f[..., 1]) & (f[..., 2] > f[..., 0])
    c = central_mask(rgb.shape[:2])
    veg = erode((e > EXG_VEG) & c, erode_px)
    non = erode((e < EXG_NONVEG) & ~blue & (y_from_rgb(rgb) >= 30.0) & c, erode_px)
    return veg, non


def veg_contrast(y: np.ndarray, veg: np.ndarray, non: np.ndarray) -> float:
    """Non-vegetation minus vegetation mean Y (white-hot: > 0 = vegetation cooler); nan if a region is too small."""
    if veg.sum() < MIN_PIXELS or non.sum() < MIN_PIXELS:
        return float("nan")
    return region_mean(y, non) - region_mean(y, veg)


def box_blur(y: np.ndarray, k: int) -> np.ndarray:
    """k x k box mean (k odd), edges replicated, through an integral image."""
    r = k // 2
    h, w = y.shape
    p = np.pad(y.astype(np.float64), r, mode="edge")
    s = np.pad(p.cumsum(0).cumsum(1), ((1, 0), (1, 0)))
    tot = s[k : k + h, k : k + w] - s[0:h, k : k + w] - s[k : k + h, 0:w] + s[0:h, 0:w]
    return (tot / (k * k)).astype(np.float32)


def highpass_std(y: np.ndarray, mask: np.ndarray, k: int = HIGHPASS_K) -> float:
    """Spatial std of Y minus its k x k box mean over mask: structure, not vignetting or AGC level."""
    if mask.sum() < MIN_PIXELS:
        return float("nan")
    hp = y.astype(np.float32) - box_blur(y, k)
    return float(hp[mask].std())


def pan_offset_m(t: float, leg_m: float = PAN_LEG_M, speed: float = PAN_SPEED_MPS) -> float:
    """East offset of the gate (l) pan: a triangle wave 0 -> leg -> 0 at `speed` m/s."""
    period = 2.0 * leg_m / speed
    ph = (t % period) / period
    return leg_m * (2.0 * ph if ph < 0.5 else 2.0 * (1.0 - ph))


def window_events(rows: list[dict]) -> list[int]:
    """Row indices where the land-cover window id changes from one nonzero id to another (re-centres)."""
    ev, prev = [], 0
    for i, r in enumerate(rows):
        w = int(r.get("land_cover_window") or 0)
        if w and prev and w != prev:
            ev.append(i)
        if w:
            prev = w
    return ev


def recentre_spike(rows: list[dict], events: list[int], radius: int = RECENTRE_RADIUS) -> tuple[float, float]:
    """(max wall_ms within `radius` rows of a re-centre minus the median wall_ms of all rows, that median)."""
    wall = [float(r["wall_ms"]) for r in rows]
    if not events or not wall:
        return float("nan"), float("nan")
    base = float(np.median(wall))
    near = [wall[j] for i in events for j in range(max(0, i - radius), min(len(wall), i + radius + 1))]
    return max(near) - base, base


def run_group(label: str) -> str:
    if label in BANDS:
        return "bands"
    if label == "mwir_1080p":
        return "hd"
    if label.startswith("eo_"):
        return "eo"
    if label.endswith("_lcoff"):
        return "lcoff"
    if label == "mwir_pan":
        return "pan"
    raise ValueError(label)


def run_band(label: str) -> str | None:
    if label in BANDS:
        return label
    return label[: -len("_lcoff")] if label.endswith("_lcoff") else None
```

`expected_rows` becomes (order kept: g last):

```python
def expected_rows(bands: list[str], runs: set[str]) -> list[Row]:
    """Every gate row the selected bands and runs must produce. a-e, h, i, j come from the band runs; k needs the band runs
    and their land-cover-off twins (lcoff); l the pan run; f the hd run; g the eo runs."""
    rows: list[Row] = []
    if "bands" in runs:
        for b in bands:
            rows += [
                ("a", b, "night"), ("b", b, "night"), ("c", b, "both"), ("d", b, "noon"),
                ("e", b, "night"), ("e", b, "noon"), ("h", b, "night"), ("h", b, "noon"),
                ("i", b, "noon"), ("j", b, "night"),
            ]
    if "bands" in runs and "lcoff" in runs:
        rows += [("k", b, "night") for b in bands]
    if "pan" in runs:
        rows.append(("l", "mwir", "noon"))
    if "hd" in runs:
        rows.append(("f", "mwir", "noon"))
    if "eo" in runs:
        rows.append(("g", "eo", "noon"))
    return rows
```

- [ ] **Step 4: Run the unit tests** — Run: `uv run --with pytest --with numpy --with pillow --with pycocotools pytest scripts/tests/test_thermal_check.py -v` → Expected: all pass.

- [ ] **Step 5: Wire the views, runs and checks.** In `scripts/thermal_check.py`:

`View` gains `sensor_id: int | None = None  # overrides the run's sensor (EO frame in an IR run)`. In `run_once`, `apply` becomes

```python
    def apply(pose, tod: dict, sensor: int | None = None):
        return dataclasses.replace(pose, sensor_id=spec.sensor_id if sensor is None else sensor, **tod)
```

and the tracker line becomes `tracker.pose_at = lambda t, v=v, tod=tod: apply(v.pose_at(t), tod, v.sensor_id)`.

In `_poses()` add:

```python
        "nadir_mixed": static(
            scenario.Pose(MIXED_CENTER[0], MIXED_CENTER[1], ground + MIXED_UP_M, gimbal_pitch=-90.0, fov_h=MIXED_FOV)
        ),
        "pan": lambda t: scenario.Pose(
            *sd.ne_to_latlon(MIXED_CENTER, 0.0, pan_offset_m(t)), ground + PAN_UP_M, gimbal_pitch=-90.0, fov_h=MIXED_FOV
        ),
```

(`sd.ne_to_latlon((lat, lon), north_m, east_m)` returns `(lat, lon)`, as `oblique_on` uses it.) In `build_runs`, `band_views` appends

```python
        v.append(View("nadir_mixed", p["nadir_mixed"], 10, "truck"))
        if tod == "noon":
            v.append(View("mixed_eo", p["nadir_mixed"], 3, None, sensor_id=0))  # greenness mask for (i) and (j)
```

and after the band runs:

```python
    if "lcoff" in wanted:
        for b in bands:
            runs.append(
                RunSpec(
                    f"{b}_lcoff",
                    {"CAMSIM_IR_PRESET": BANDS[b], "CAMSIM_THERMAL_LAND_COVER_ENABLED": "0"},
                    1,
                    ["night"],
                    lambda tod: [View("nadir_mixed", p["nadir_mixed"], 10, "truck")],
                )
            )
    if "pan" in wanted:
        runs.append(
            RunSpec(
                "mwir_pan",
                {"CAMSIM_IR_PRESET": "mwir_cooled", "CAMSIM_THERMAL_LAND_COVER_RECENTRE_FRACTION": PAN_RECENTRE_FRACTION},
                1,
                ["noon"],
                lambda tod: [View("pan", p["pan"], 0, None, hold_s=PAN_HOLD_S)],
                ml=False,
            )
        )
```

Add the land-cover checks:

```python
def entity_mask(vd: ViewData) -> np.ndarray:
    """Union of the view's COCO boxes (1.5x) over its frames: entities are not terrain."""
    m = np.zeros(vd.y.shape[1:], bool)
    for a in vd.anns:
        if a is not None:
            m |= box_mask(m.shape, scale_box(a["bbox"], 1.5))
    return m


def check_land_cover(
    out: Path, band: str, views: dict, off_views: dict, checks: list, info: list, shots: list
) -> None:
    """(i) noon and (j) night: non-vegetation minus vegetation IR mean (masks from the noon EO frame of the same pose);
    (k) night high-pass std with land cover on / off (ROADMAP 4B)."""
    eo = views.get(("noon", "mixed_eo"))
    rgb = None
    if eo is not None and Path(eo.rec["shot"]).exists():
        rgb = np.asarray(Image.open(eo.rec["shot"]).convert("RGB"))
    for tod, check, limit in (("noon", "i", VEG_NOON_DN), ("night", "j", VEG_NIGHT_DN)):
        vd = views.get((tod, "nadir_mixed"))
        if vd is None or rgb is None or rgb.shape[:2] != vd.y.shape[1:]:
            continue  # the row stays missing: the gate fails
        veg, non = greenness_masks(rgb)
        ent = entity_mask(vd)
        veg, non = veg & ~ent, non & ~ent
        img = vd.y.astype(np.float32).mean(axis=0)
        d = veg_contrast(img, veg, non)
        checks.append(
            {
                "check": check,
                "band": band,
                "time": tod,
                "value": d,
                "threshold": f">= +{limit:g} DN (non-vegetation - vegetation)",
                "pass": bool(d >= limit),  # NaN fails
                "detail": f"vegetation {int(veg.sum())} px, mean {region_mean(img, veg):.1f}; "
                f"non-vegetation {int(non.sum())} px, mean {region_mean(img, non):.1f}",
            }
        )
        shots.append(overlay(out, vd, veg, non, None, len(vd.y) // 2))
    on = views.get(("night", "nadir_mixed"))
    off = off_views.get(("night", "nadir_mixed"))
    if on is not None and off is not None and on.y.shape[1:] == off.y.shape[1:]:
        mask = central_mask(on.y.shape[1:]) & ~entity_mask(on) & ~entity_mask(off)
        s_on = highpass_std(on.y.astype(np.float32).mean(axis=0), mask)
        s_off = highpass_std(off.y.astype(np.float32).mean(axis=0), mask)
        ratio = s_on / max(s_off, 1e-6)
        checks.append(
            {
                "check": "k",
                "band": band,
                "time": "night",
                "value": ratio,
                "threshold": f">= {STRUCTURE_RATIO:g}x land cover off",
                "pass": bool(ratio >= STRUCTURE_RATIO),
                "detail": f"high-pass std {s_on:.2f} DN vs {s_off:.2f} DN (4A), {int(mask.sum())} px",
            }
        )
        shots.append(off.rec["shot"])
```

In `check_all`, inside `for band in bands:` after `check_band(...)`:

```python
        off_views = {}
        off_run = results.get(f"{band}_lcoff")
        if off_run:
            for rec in off_run["views"]:
                vd = load_view(out, off_run, rec)
                if vd is not None:
                    off_views[(rec["time"], rec["view"])] = vd
        check_land_cover(out, band, views, off_views, checks, info, shots)
```

and before the `(f)` block:

```python
    # (l) no frame-time spike when the land-cover window re-centres during a 1 km/min pan.
    run = results.get("mwir_pan")
    if run:
        w = run["views"][0]
        rows = [r for r in load_rows(out / "mwir_pan" / "frames.jsonl") if w["t0"] <= r["t"] <= w["t1"] and not r.get("cut")]
        ev = window_events(rows)
        spike, base = recentre_spike(rows, ev)
        quiet = [float(r["wall_ms"]) for i, r in enumerate(rows) if all(abs(i - e) > RECENTRE_RADIUS for e in ev)]
        checks.append(
            {
                "check": "l",
                "band": "mwir",
                "time": "noon",
                "value": spike,
                "threshold": f"<= {RECENTRE_SPIKE_MS:g} ms over the median, >= {MIN_RECENTRES} re-centres",
                "pass": bool(len(ev) >= MIN_RECENTRES and spike <= RECENTRE_SPIKE_MS),
                "detail": f"{len(ev)} re-centres in {len(rows)} frames; median wall {base:.2f} ms; "
                f"max away from re-centres {max(quiet, default=float('nan')):.2f} ms (tile streaming, info)",
            }
        )
```

In `main()`, `--runs` defaults to `"bands,hd,eo,lcoff,pan"` (help: `comma list of: bands, hd, eo, lcoff, pan`), and the run loop becomes:

```python
    for spec in build_runs(bands, {"bands", "hd", "eo", "lcoff", "pan"}):
        band = run_band(spec.label)
        if band is not None and band not in bands:
            continue
        group = run_group(spec.label)
        if group in wanted and not a.check_only:
            results[spec.label] = run_once(spec, out)
        elif (out / spec.label / "run.json").exists():
            results[spec.label] = json.loads((out / spec.label / "run.json").read_text())
```

Update the module docstring: add the `nadir_mixed` / `mixed_eo` views to "bands", the `lcoff` and `pan` launches, and gates (i)–(l) with the thresholds above; the report title becomes "Thermal check (ROADMAP 4A/4B)". Then re-run Step 4's pytest → all pass.

- [ ] **Step 6: Live acceptance** — `caffeinate -ims uv run scripts/thermal_check.py --band both` → Expected: every gate (a)–(l) passes, both bands. Then open `report.md`, the `nadir_mixed` overlays (red/cyan = vegetation / non-vegetation) and the shots: check that the EO `mixed_eo` shot contains no open water (else move `MIXED_CENTER` inland and re-run) and that class boundaries in the IR shots follow trees, lawns and roads rather than 10 m blocks (spec success (d); visual). If (i) or (j) fails, debug with superpowers:systematic-debugging first (are the masks right? does `camsim.Thermal.Log` show `landCover=<id>`?). Tuning order: `veg_index_lo/hi` and `asphalt_max_luma` (config defaults), then class values in `ThermalMaterials.cpp` (re-run `run_tests CamSim.Thermal.Materials`); never the gate thresholds. Any changed default goes into `CamSimConfig.h`, `deploy/camsim_config.yaml`, `docs/configuration.md` and the `LandCoverDefaults` test in the same commit, with a ROADMAP 4B note saying why. Record `thermal_gpu_ms` p95 at 1080p (gate f) against 4A's 0.141 ms (budget +0.1 ms).

- [ ] **Step 7: Commit**

```bash
git add scripts/thermal_check.py scripts/tests/test_thermal_check.py
# plus any tuned defaults/values and their docs, if Step 6 changed them
git commit -F - <<'EOF'
test(thermal): land-cover acceptance gates i-l in thermal_check.py (ROADMAP 4B)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
EOF
```

---

### Task 12: Docs — ROADMAP 4B, thermal guide, configuration, CLAUDE.md, README attribution

**Files:**
- Modify: `ROADMAP.md` (new "4B Terrain classification" section after 4A: status, what was built, results table from `report.md` for gates (i)–(l) plus (a)–(h) re-run, performance (window build ms from `WindowBuildTime` and the live log, `thermal_gpu_ms` delta), decisions worth keeping (the Decisions list's spec corrections), known issues, carry-overs; **"Editor / human changes: none"** stated explicitly; in 4A's "Carried over" mark the 4B line done; update item 4.2's note that the camera-centred window replaced the raster-overlay idea)
- Modify: `docs/thermal.md` (Classification step 4 → land cover; new "Land cover (ROADMAP 4B)" section: data pipeline and `fetch_worldcover.py` usage, the window and re-centre, the per-pixel lookup and bilinear blend, the refinement families and thresholds, the code → class table (spec table, with the built-up-only split), keys, how to fetch another area, edge cases (no data, pole, outside the window, LFS pointers), performance, limits, acceptance gates (i)–(l), and the attribution line; remove the 4A limit "One terrain class" and "Nothing selects a material per terrain location yet")
- Modify: `docs/configuration.md` (confirm the Task 6 rows; link `docs/thermal.md#land-cover-roadmap-4b`)
- Modify: `CLAUDE.md` (Commands row; Gotchas entry; architecture line for `Thermal/`; test counts)
- Modify: `README.md` (data attribution)

- [ ] **Step 1: Write the docs** from the measured results (numbers from `report.json`, the `LandCover: window` log lines, `WindowBuildTime`, the GPU test info lines and the last full test run; every number measured, none estimated). The fixed part of the new `docs/thermal.md` section (insert after "Classification"; replace its step 4 with "4. **Terrain**: the land-cover blend inside the window (below), `terrain_default` outside it or with land cover off"):

```markdown
## Land cover (ROADMAP 4B)

Terrain pixels take their thermal class from ESA WorldCover 2021 v200 (10 m, 11 classes), refined per pixel by the
imagery's base colour. Design: [`superpowers/specs/2026-10-01-terrain-classification-design.md`](superpowers/specs/2026-10-01-terrain-classification-design.md).

**Data.** `scripts/landcover/fetch_worldcover.py --bbox W S E N [--out DIR]` reads ESA's 3 x 3 degree COGs
(`https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/2021/map/ESA_WorldCover_10m_2021_v200_N36W123_Map.tif`, named by the
SW corner) with HTTP range requests and writes 0.05 degree tiles: 600 x 600 8-bit greyscale PNGs (a WorldCover cell is
1/12000 degree; codes pass through, no resampling) plus `index.json` and `ATTRIBUTION.txt`. The San Francisco sample
(-122.56 37.69 -122.35 37.84, 20 tiles) is committed in `Content/NonUFS/LandCover` (git LFS; staged as loose files). CamSim
reads only `thermal.land_cover.dir`; there is no network access at runtime.

**Window.** `FLandCoverWindow` keeps a camera-centred window of `window_texels`² texels of 10 m (2048 = 20.48 km) on a local
East/North grid, row 0 = north. When the camera is more than `recentre_fraction` of the window from its centre, a new window is
resampled on a task thread (nearest cell; missing tiles = code 0) and swapped in on the game thread; the old window renders
meanwhile. Its `R8_UINT` texture is uploaded on the render thread and travels with each frame's thermal parameters, paired by
window id. Within 1 degree of a pole, with no index, or with no data in range, land cover is off (one warning).

**Lookup.** For a terrain pixel (after the sky, entity and sea-band rules), `(E, N) = (P . East, P . North) / 100 + CamOffset`,
with P the camera-relative world position, East/North the unit vectors at the window centre (recomputed every frame from the
Cesium georeference) and CamOffset the camera's offset from the centre (CPU doubles, same small-area mapping as the resample).
The four nearest texels each map through the code -> class table, are refined, and their class data (temperature, emissivity,
k_fast, S_abs,ref) is blended bilinearly, so 10 m boundaries become ramps. Outside the window: `terrain_default`.

**Refinement** (needs the GBuffer base colour, not sunlight): `v = saturate((ExG - veg_index_lo) / (veg_index_hi - veg_index_lo))`,
`ExG = (2G - R - B) / (R + G + B)` of the linear base colour.

| Family (codes) | Refined material |
|---|---|
| Vegetation (10, 20, 30, 40, 90, 95, 100) | v x own class + (1 - v) x `bare_soil` (trails, clearings) |
| Built-up (50) | v x `vegetation` + (1 - v) x (`asphalt` below `asphalt_max_luma`, `concrete` above; 0.04 soft ramp) |
| Bare (60) | v x `vegetation` + (1 - v) x own class |
| None (0, 70, 80) | own class |

**Classes** (`thermal.land_cover.classes.<code>` overrides):

| WorldCover | Code | Default material |
|---|---|---|
| Tree cover | 10 | `tree_canopy` |
| Shrubland | 20 | `shrubland` |
| Grassland | 30 | `grassland` |
| Cropland | 40 | `cropland` |
| Built-up | 50 | `built_up` (asphalt / concrete split with base colour) |
| Bare / sparse vegetation | 60 | `bare_soil` |
| Snow and ice | 70 | `snow_ice` (capped at 0 C) |
| Permanent water bodies | 80 | `water` |
| Herbaceous wetland | 90 | `wetland` |
| Mangroves | 95 | `wetland` |
| Moss and lichen | 100 | `grassland` |
| No data / other | 0 | `terrain_default` |

Data attribution (CC BY 4.0): © ESA WorldCover project 2021 / Contains modified Copernicus Sentinel data (2021) processed by
ESA WorldCover consortium.
```

followed by measured "Performance" rows (window build ms, `thermal_gpu_ms` p95 at 1080p vs 4A's 0.141 ms, GPU vs CPU worst
error), "Limits" (no roads layer, one WorldCover epoch, imagery shadows bias the asphalt split, CPU copy of the window kept,
Linux/Vulkan unverified) and the gate (i)–(l) rows added to the "Acceptance" table. `CLAUDE.md` additions, exactly:

Commands table, after the `thermal_check.py` row (and amend that row to "Thermal IR acceptance (ROADMAP 4A/4B) … land-cover contrast, structure and re-centre hitch"):

```markdown
| `scripts/landcover/fetch_worldcover.py` | Cut ESA WorldCover 2021 COGs (HTTP range reads) into land-cover tiles for thermal IR (`--bbox W S E N`; SF sample committed) |
```

Gotchas, after the thermal entry:

```markdown
- **Land cover for thermal IR** (ROADMAP 4B, `thermal.land_cover`, guide `docs/thermal.md`): terrain pixels take their thermal class from ESA WorldCover tiles in `Content/NonUFS/LandCover` (git LFS — `git lfs pull` if `CamSim.Thermal.LandCover.SanFranciscoSample` fails; other areas: `scripts/landcover/fetch_worldcover.py`). `FLandCoverWindow` builds a camera-centred 2048² window of 10 m texels on a task thread and swaps it in; its GPU copy is paired with each frame's `FThermalFrameParams` by window id (`CamSimLandCover::ShouldBindWindow`). `ThermalCS` blends the four nearest texels' class data and refines it by the GBuffer base colour; `CamSimThermalRef` mirrors it (`CamSim.GPU.Thermal.LandCoverMatchesCpu`). East/North are recomputed every frame (origin shifts rotate UE axes). `thermal.land_cover.enabled: false` is 4A bit for bit. Data is CC BY 4.0: keep the attribution
```

Update the `Thermal/` line of the Architecture tree if present (add "land cover: tiles, window, code → class"), and the test counts (`Tests/` count and files) from the final `run_tests CamSim` result.

`README.md` — in "Links" (or a new "Data attribution" section right before it):

```markdown
## Data attribution

- Land cover for thermal IR (`unreal_project/CamSimTest/Content/NonUFS/LandCover`, ROADMAP 4B): © ESA WorldCover project 2021 / Contains modified Copernicus Sentinel data (2021) processed by ESA WorldCover consortium. Licence: [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). Source: [esa-worldcover.org](https://esa-worldcover.org).
```

- [ ] **Step 2: Verify** — `run_tests CamSim` → 0 failed (put the count in CLAUDE.md); `scripts/run_gpu_tests.sh` → all pass; `uv run --with pytest --with numpy --with pillow --with pycocotools pytest scripts/tests/ -v` → all pass; `scripts/ci_validate.sh --native` → passes (video/KLV unchanged).

- [ ] **Step 3: Commit**

```bash
git add ROADMAP.md docs/thermal.md docs/configuration.md CLAUDE.md README.md
git commit -F - <<'EOF'
docs(thermal): ROADMAP 4B results, land-cover guide, CLAUDE.md, WorldCover attribution

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
EOF
```

- [ ] **Step 4: Visual review hand-off** — send the user the MWIR/LWIR noon/night `nadir_mixed` shots with land cover on and off, the EO `mixed_eo` shot and the overlays; ROADMAP 4B stays "awaiting visual review" until they sign off (spec success (d): boundaries follow trees, lawns and roads).

---

## Spec coverage (self-review)

| Spec item | Task |
|---|---|
| Goal: night texture, noon vegetation cool / built-up hot, inland water | 5 (classes, contrasts test), 7–8 (blend, water code 80), 11 (gates i–k) |
| Success (a)–(c) | 5 (`DiurnalContrastsAtSanFrancisco`), 11 (gates i, j, k) |
| Success (d) boundaries follow imagery | 7 (refinement), 11 Step 6 + 12 Step 4 (visual review) |
| Success (e) no hitch on re-centre | 4 (task-thread build, swap), 10 (pairing), 11 (gate l) |
| Success (f) EO and `enabled: false` unchanged | 7 (`LandCoverOffIs4A`), 8 (GPU off case), 9 (`LandCoverDisabledIs4A`), 11 (gate g re-run) |
| Data pipeline: script, 0.05° lossless PNG tiles, `index.json`, licence, SF sample in LFS, local files only, NonUFS staging | 1, 2 |
| Window: 2048², 10 m, 25 % re-centre, task thread < 100 ms, atomic swap, old keeps rendering, small-area mapping, nearest cell, missing → 0 | 3, 4 |
| Lookup in ThermalCS: East/North at window centre + camera offset in doubles; terrain pixels only | 7, 8, 9, 10 |
| Soft boundaries: four texels, 256-entry code → material table, blend of material values | 6, 7, 8 |
| Imagery refinement: ExG vegetation index both directions, asphalt/concrete split, thresholds as keys, no-base-colour fallback | 6, 7, 8 |
| Classes table and new built-ins within `MaxClasses = 32`, overridable | 5, 6 |
| Interfaces: params fields, `FThermalPassInputs::LandCover`, reference as pure function, new files, config keys | 6, 7, 8, 2–4 |
| Edge cases: no tiles / missing dir, ocean, high altitude (outside window), antimeridian, poles, swap mid-flight | 2, 3, 4, 7, 9, 10 |
| Performance: +0.1 ms ThermalCS, window build < 100 ms off-thread, memory | 4 (`WindowBuildTime`), 11 (gate f record), 12 |
| Testing: `CamSim.Thermal.LandCover.*`, `Reference.*` additions, `GPU.Thermal` land-cover cases, `test_fetch_worldcover.py`, `thermal_check` (i)–(l) | 1–4, 7, 8, 11 |
| Risks: dry grass (prior keeps vegetation family), baked shadows (soft ramp), licence attribution | 6–7 (families, ramp), 1, 12 (attribution) |
