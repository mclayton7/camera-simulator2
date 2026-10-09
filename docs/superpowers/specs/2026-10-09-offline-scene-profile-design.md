# Offline scene profile (REALISM R0) — design

2026-10-09. Closes the last open part of R0 gate 5: an offline run on a scene package makes no outbound request.
No editor work.

## Goal

A run with `scene.dir` pointing at a scene package and `scene.offline: true` renders the package and never
requests a network endpoint. Online runs without a package behave as today, minus one redundant terrain load.

Success: `scripts/scene/tools/render_check.py OUT package PKG --offline` on the Pendleton package shows no blocked
`api.cesium.com` request in the log, and gate 5's registration (< 1 px) and frame-centre height checks still pass.

## Root cause

`ACesium3DTileset::BeginPlay` calls `LoadTileset()`, which for an ion tileset requests the ion endpoint (and its
ion raster overlay follows when the tileset loads). CamSim applies its Cesium config in `ACamSimCamera::BeginPlay`,
after the level's tilesets have begun play, so `Main.umap`'s terrain (ion 1), Bing overlay (ion 2) and OSM
Buildings (ion 96188) have already sent their requests. `RefreshTileset()` then loads the terrain a second time.

## Design

### 1. Cesium setup before any `BeginPlay`

- New `ACamSimGameMode::StartPlay()` override: call `PrepareCesiumWorld()`, then `Super::StartPlay()` (which
  dispatches `BeginPlay` to every actor).
- `PrepareCesiumWorld(UWorld*, const FCamSimConfig&, UCamSimSubsystem*)` (Geospatial) runs, in this order, the work
  now in `ACamSimCamera::BeginPlay`: tileset mobility for origin shift, `ApplyCesiumTilesetTuning`,
  `ApplyCesiumBackendConfig`, `StoreCesiumIonServer`.
- Idempotent through a subsystem flag (reset per world). The camera still calls it, so tests and maps without
  `ACamSimGameMode` keep the current behaviour.
- Extra tilesets are destroyed before their `BeginPlay`, so they never call `LoadTileset`. The terrain tileset
  gets its final source before its first load.
- `ApplyCesiumBackendConfig` skips `RefreshTileset()` when the tileset has not begun play (`HasActorBegunPlay()`):
  the first load already uses the configured source.

### 2. Imagery source `tms`

- `cesium.imagery.source: tms` + new `cesium.imagery.url` (env `CAMSIM_CESIUM_IMAGERY_URL`). The URL names
  `tilemapresource.xml` (Cesium's TMS overlay needs the XML file itself for `file://`).
- Creates `UCesiumTileMapServiceRasterOverlay` named `CamSimImagery`, with the same max SSE, max texture size and
  max simultaneous tile loads as the other overlay sources.
- `wms_url` keeps meaning WMS only. `scripts/scene/tools/tms_overlay.patch` is deleted.

### 3. `scene.dir`

New top-level config block:

```yaml
scene:
  dir: ""          # CAMSIM_SCENE_DIR — absolute path to a scene package (docs/scene-packages.md); empty = none
  offline: false   # CAMSIM_SCENE_OFFLINE — no network: require local terrain/imagery, exit on config errors
```

Resolution happens after YAML and env are loaded and before validation, in a pure function
(`CamSimScene::ResolvePackage(Config, FileExists)` or similar) so it is testable without a world:

- `scene.dir` must be absolute and contain `manifest.json`. Its `name` and `schema_version` are logged;
  `schema_version` other than 1 is an error.
- Each layer present overrides config:

  | Package file | Sets |
  |---|---|
  | `terrain/layer.json` | `cesium.terrain.source: url`, `url: file:///…/terrain/layer.json` |
  | `imagery/tilemapresource.xml` | `cesium.imagery.source: tms`, `url: file:///…/imagery/tilemapresource.xml` |
  | `landcover/index.json` | `thermal.land_cover.dir: …/landcover` |

- A missing layer leaves that config as it was.
- Overriding a non-default `cesium.terrain.*` / `cesium.imagery.*` / `thermal.land_cover.dir` value adds a
  validation warning naming the key.
- `file:///` URLs are built by percent-encoding the path (space → `%20`); the path keeps its leading `/`, giving
  three slashes.

### 4. `scene.offline`

Validation rules, applied after `scene.dir` resolution, only when `scene.offline` is true:

- terrain: `url` with a `file:///` URL whose file exists, or `flat`;
- imagery: `tms` with a `file:///` URL whose file exists, or `none`;
- anything else (`cesium_ion`, `wms`, an `http(s)://` URL, a missing file) is an error naming the key.

With `scene.offline: true`, any config validation error (these or existing ones) logs every reason and exits
with a non-zero status (`FPlatformMisc::RequestExitWithStatus(false, 1)`) from subsystem initialisation, before
the world begins play. Without it, errors are logged and the run continues as today.

`scene.offline` without `scene.dir` is allowed (explicit `file:///` keys), and the same rules apply.

### 5. Tests, tools, docs

- Automation tests (`CamSim.Scene.*`, NullRHI):
  - package resolution on a temp-dir package: full package, each layer missing, no manifest, unknown
    schema version, relative path, a path containing a space;
  - offline validation: each rejected terrain/imagery source, `http://` URL, missing file, and `flat` + `none`
    accepted;
  - config parsing: `scene.*` and `cesium.imagery.url` from YAML and env;
  - `PrepareCesiumWorld` on a world whose actors have not begun play: one tileset left, with the configured
    source and URL, and a second call is a no-op.
- `scripts/scene/tools/camsim_session.py`: package mode sets `CAMSIM_SCENE_DIR`, and `--offline` also sets
  `CAMSIM_SCENE_OFFLINE=1`.
- Acceptance: `render_check.py --offline` and `hot_check.py` on Pendleton (above). Also check that an online
  default run (ion) still reaches `/ready` and renders.
- Docs: `docs/configuration.md` (new keys + env vars), `deploy/camsim_config.yaml`, `docs/scene-packages.md`
  (how to run a package; gate 5 row), `REALISM.md` and `ROADMAP.md` status. The `Main.umap` editor follow-up
  is no longer needed for offline runs (it stays optional cleanup).

## Out of scope

Feature tilesets (R3), NDVI (R1/R2), checking package hashes at startup (`camsim-scene verify` does that), and a
Linux equivalent of the macOS `sandbox-exec` offline check.
