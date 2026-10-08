# Cesium for Unreal spike: local terrain/imagery, feature tilesets, cache (REALISM R1/R3/R5)

Research for `docs/realism-r0-spike.md` (2026-10-07). The spike confirmed §1 and §2 empirically.

Plugin: `/Users/Shared/Epic Games/UE_5.8/Engine/Plugins/Marketplace/CesiumForUnreal`, **v2.29.1**
(`CesiumForUnreal.uplugin` VersionName; matches `scripts/repo_setup.sh:20`). Paths below: `CR/` =
`Source/CesiumRuntime`, `TP/` = `Source/ThirdParty/include`, `LIB/` =
`Source/ThirdParty/lib/Darwin-universal-Release`. cesium-native ships as headers + static libs only,
so loader internals were checked via headers and `strings` on the libs (marked "lib strings").

## 1. file:// terrain and 3D Tiles

**Yes, both load from `file:///`.**

- Accessor chain (`CR/Private/CesiumRuntime.cpp:119-129`):
  `GunzipAssetAccessor( CachingAssetAccessor( UnrealAssetAccessor, SqliteCache ) )`.
- `UnrealAssetAccessor::get` (`CR/Private/UnrealAssetAccessor.cpp:179-181`) routes any URL starting
  with the literal prefix `"file:///"` (`:147-151`) to `getFromFile` → `FFileHelper::LoadFileToArray`
  on `GIOThreadPool` (`:361-376`, `:385-409`).
  - **Three slashes required**: `file://localhost/...` or `file://host/...` falls through to the HTTP
    module and fails. Use `file:///abs/path/layer.json`.
  - Path conversion: `Uri::uriPathToNativePath` (`:334-338`; `TP/CesiumUtility/Uri.h:319-331`):
    percent-decoded; POSIX on Linux/macOS, `file:///C:/...` on Windows. Absolute paths only (no
    relative file URLs). Spaces must be `%20`.
  - Response: status 200 or **404** if the read fails (resolved, not rejected, `:365-375`), **no headers,
    empty Content-Type** (`:304-314`). cesium-native detects content by magic/JSON keys, so a missing
    Content-Type is fine.
  - **gzip-compressed `.terrain` files work**: `GunzipAssetAccessor` wraps the whole chain including
    `file://` (`CesiumRuntime.cpp:123`; `TP/CesiumAsync/GunzipAssetAccessor.h:9-13`, sniffs gzip magic
    via `CesiumUtility/Gzip.h:14 isGzip`). Store tiles gzipped or raw; no Content-Encoding needed.
- `ETilesetSource::FromUrl` (`CR/Public/Cesium3DTileset.h:81-96`, `Url` at `:745`) accepts either a
  `tileset.json` or a quantized-mesh `layer.json`: cesium-native has a JSON-based dispatch
  (`LayerJsonTerrainLoader::createLoader(..., rapidjson::Document, ...)` and `TilesetJsonLoader::createLoader`
  in lib strings of `libCesium3DTilesSelection.a`, along with `"quantized-mesh-1.0"`, `"layer.json"`,
  `metadataAvailability`, `octvertexnormals`, `watermask`, and
  `"Could not deduce tiling scheme, projection, or bounding volume from layer.json."`).
  CamSim's `terrain.source: url` already does exactly `SetTilesetSource(FromUrl); SetUrl(...)`
  (`Geospatial/CamSimGeospatialProvider.cpp:167-171`), so R1 needs **no code** to try
  `CAMSIM_CESIUM_TERRAIN_URL=file:///data/pkg/terrain/layer.json`.
- Caveats for the R1 encoder: there is no server to negotiate extensions (the `Accept:
  ...;extensions=octvertexnormals-...` header is meaningless for files), so tiles must already contain
  the extensions layer.json lists. Provide `available` ranges (or `metadataAvailability` + metadata
  extension) so absent tiles are never requested; an unexpected miss is a 404 → failed tile.
  Set `"projection": "EPSG:4326"` (default geographic tiling, 2×1 roots).
- Docker: plain POSIX paths on a mounted volume work; the cook doesn't matter (runtime file reads).
  No HTTP sidecar needed unless the cache/offline argument (§5) applies.

## 2. Raster overlay classes and local tiles

Classes in 2.29.1 (`CR/Public/`): `CesiumIonRasterOverlay`, `CesiumBingMapsRasterOverlay`,
`CesiumTileMapServiceRasterOverlay` (TMS), `CesiumWebMapServiceRasterOverlay` (WMS),
`CesiumWebMapTileServiceRasterOverlay` (WMTS), `CesiumUrlTemplateRasterOverlay`,
`CesiumGoogleMapTilesRasterOverlay`, `CesiumAzureMapsRasterOverlay`,
`CesiumVectorTilesRasterOverlay`, `CesiumGeoJsonDocumentRasterOverlay`, `CesiumPolygonRasterOverlay`
(clipping), `CesiumDebugColorizeTilesRasterOverlay`. All go through the same accessor, so all can read
`file:///`.

- **URL template** (`CesiumUrlTemplateRasterOverlay.h:36-69`): `{x} {y} {z} {reverseX} {reverseY}
  {reverseZ}`, bbox placeholders, `{width}/{height}`. **`{y}` is 0 = south (TMS order); slippy/XYZ
  pyramids need `{reverseY}`.** Projection WebMercator (default) or Geographic (`:77`). Needs no
  metadata file. Limitation: the Unreal wrapper only exposes `bSpecifyTilingScheme` + one rectangle,
  and uses it as **both** coverage rectangle and tiling-scheme rectangle
  (`CR/Private/CesiumUrlTemplateRasterOverlay.cpp:40-56`). So a package-sized subset of a standard
  global XYZ pyramid can't be declared as "global tiling, coverage = bbox"; without it, coverage is the
  whole world and every tile outside the package is a 404. `MinimumLevel`/`MaximumLevel` are always
  passed (`:23-24`, default 0/25).
- **TMS** (`CesiumTileMapServiceRasterOverlay.h:21-59`): only `Url`, optional zoom levels, headers.
  The Unreal wrapper doesn't expose `fileExtension`/`coverageRectangle`/`tilingScheme`
  (`CesiumTileMapServiceRasterOverlay.cpp:15-34`; native options exist at
  `TP/CesiumRasterOverlays/TileMapServiceRasterOverlay.h:19-81`), so it **requires
  `tilemapresource.xml`** at `Url` (spike, 2026-10-07: for `file://` the `Url` must name the XML file itself; a directory URL fails with "Is a directory") (lib strings: `tilemapresource.xml`, "No response received from Tile
  Map Service.", "Tile map service XML document does not have an SRS / any tilesets",
  `global-geodetic`/`global-mercator`). gdal2tiles writes one; its `BoundingBox` gives a real coverage
  rectangle with global tiling, which is what a package subset needs. **Recommend TMS
  (gdal2tiles-style) for NAIP**, URL template only for full-world or self-describing pyramids.
- Layering: overlays bind to material layers via `MaterialLayerKey` (default `"Overlay0"`,
  `CR/Public/CesiumRasterOverlay.h:68`); the default material takes 3 overlays (Overlay0-2). A local NAIP
  overlay over an ion fallback overlay is plausible but behaviour at the coverage edge must be tested.

## 3. CamSim today

- Config (`Config/CamSimConfig.h:395-420`, YAML `Config/CamSimConfig.cpp:785-813`, env
  `:1110-1119`, `deploy/camsim_config.yaml:646-671`):
  - `cesium.ion_portal_url`, `ion_api_url`, `ion_token`
  - `cesium.terrain.source`: `cesium_ion` (default, `ion_asset_id: 1`) | `url` (`terrain.url`) | `flat`
    (hides the tileset + disables its collision); unknown → `cesium_ion` with a warning.
  - `cesium.imagery.source`: `cesium_ion` (default `ion_asset_id: 2`, Bing) | `wms` (`wms_url`,
    `wms_layers`, `wms_tile_width/height`) | `none`; plus `maximum_screen_space_error`,
    `maximum_texture_size`, `maximum_simultaneous_tile_loads`.
- `ApplyCesiumBackendConfig` (`Geospatial/CamSimGeospatialProvider.cpp:102-258`, called from
  `Camera/CamSimCamera.cpp:123` after `ApplyCesiumTilesetTuning` at `:122`):
  1. optional transient `UCesiumIonServer` (`:110-125`);
  2. iterates **all** `ACesium3DTileset` actors, picks the terrain via `SelectTerrainTileset` (first with
     ion asset 1, else index 0; `:16-26`) and **`Destroy()`s every other tileset** (`:132-150`);
  3. sets the terrain source + `RefreshTileset()` (`:160-191`);
  4. destroys all existing `UCesiumRasterOverlay` components on it and creates one `"CamSimImagery"`
     overlay (`NewObject<...>(Tileset)`, `RegisterComponent`, `Activate(false)`) (`:193-248`).
- Other tileset consumers: `ApplyCesiumTilesetTuning` tunes every tileset incl. `SetCreatePhysicsMeshes`
  (`Geospatial/CesiumTuning.cpp:32-74`); `UCamSimSubsystem::RefreshCachedTilesets` caches the list lazily
  once (`Subsystem/CamSimSubsystem.cpp:263-284`); the terrain gate takes the **min** load progress over
  cached tilesets (`Camera/CamSimStreamingController.cpp:105-115`); FOV-scaled culled SSE is written to
  all of them (`:167-173`).
- **(a) Local imagery**: add `imagery.source: tms | url_template` (+ `imagery.url`, `projection`,
  `min/max_level`, optional rectangle) and a branch at `CamSimGeospatialProvider.cpp:226` creating
  `UCesiumTileMapServiceRasterOverlay` (`Url`, `bSpecifyZoomLevels`) or
  `UCesiumUrlTemplateRasterOverlay` (`TemplateUrl`, `Projection`, ...), with the same SSE/texture/loads
  setters. Optional: a second overlay (ion fallback) on another `MaterialLayerKey`. Docs:
  `docs/configuration.md`, yaml, env vars, a config test.
- **(b) Feature tilesets**: spawn them *after* the destroy loop (or tag them, e.g. `Tags` "CamSimFeature",
  and skip tagged actors in the loop), so the Main.umap OSM-buildings removal still applies to anything
  not from the package: e.g. `cesium.features: [{url, ...}]` → `World->SpawnActor<ACesium3DTileset>`,
  `SetTilesetSource(FromUrl)`, `SetUrl`, `SetCreatePhysicsMeshes`, material, own SSE. Then: apply
  tuning to them (call order: tuning currently runs *before* backend config, so spawned tilesets miss
  it), set `Movable` mobility if origin shift is on (`CamSimCamera.cpp:104-113` only loops existing
  actors), call `RefreshCachedTilesets()` (otherwise the lazy cache may predate them or include
  destroyed ones), decide whether the terrain gate should wait on buildings, and make
  `SelectTerrainTileset` ignore them. Keep raster overlays off building tilesets (or decide otherwise).
  `SelectTerrainTileset` treats a URL tileset as `-1`, so a level that had a feature tileset first
  could be mistaken for terrain: spawning at runtime avoids it.

## 4. Feature tileset capabilities

- **Physics**: `CreatePhysicsMeshes` (default true, `Cesium3DTileset.h:819-822`),
  `EnableDoubleSidedCollisions` (`:832-836`), a `FBodyInstance` collision profile (`:677-685`). CamSim's
  traces (`ECC_Visibility`, `Entity/SurfaceProbe.cpp:32-33` complex; `CIGI/CigiQueryHandler.cpp:107,
  198, 279`) will hit buildings/bridges automatically. Extruded footprints are open at the bottom: keep
  normals outward, or enable double-sided collisions for LOS from inside.
- **Metadata at runtime (C++/BP)**: `UCesiumMetadataPickingBlueprintLibrary::GetPropertyTableValuesFromHit(
  const FHitResult&, int64 FeatureIDSetIndex)` (`CesiumMetadataPickingBlueprintLibrary.h:71-77`),
  `GetMetadataValuesForFace` (`:141`), `UCesiumFeatureIdSetBlueprintLibrary::GetFeatureIDFromHit`
  (`CesiumFeatureIdSet.h:214`), `UCesiumPrimitiveFeaturesBlueprintLibrary` (`CesiumPrimitiveFeatures.h:87-192`),
  `UCesiumPropertyTableBlueprintLibrary::GetMetadataValuesForFeature` (`CesiumPropertyTable.h:190`),
  `UCesiumModelMetadataBlueprintLibrary` (`CesiumModelMetadata.h:58-99`). EXT_mesh_features /
  EXT_structural_metadata. Picking needs the hit's face index: trace with `bTraceComplex = true` and
  `bReturnFaceIndex = true` (the docs list "hit's face index out-of-bounds" as a failure, `:60-65`).
  The telemetry trace is `bTraceComplex=false` (`Camera/CamSimTelemetryAssembler.cpp:156`).
- **Metadata on the GPU (materials)**: `UCesiumFeaturesMetadataComponent` encodes chosen feature ID sets /
  properties into textures; "Add Properties" and "Generate Material" are **editor-only**
  (`CesiumFeaturesMetadataComponent.h:14-62`, `#if WITH_EDITOR`). So a thermal-class-from-metadata
  material is human editor work (ROADMAP note), and ThermalCS would read it through the GBuffer or a
  custom stencil/attribute, not directly.
- **Custom material**: `Material`, `TranslucentMaterial`, `WaterMaterial` (`Cesium3DTileset.h:943-978`,
  setters `:1190-1204`); should be copies of `MI_CesiumThreeOverlaysAndClipping` (`:928-934`).
  `CustomDepthParameters` (`:982-986`) exists if buildings ever need a stencil (R3 says not).

## 5. Request cache (R5 cache warming)

- File: `FPaths::ProjectUserDir()/cesium-request-cache.sqlite` on desktop/Linux
  (`CR/Private/CesiumRuntime.cpp:75-100`; logs "Caching Cesium requests in ..."). With `-userdir=/var/lib/camsim`
  it's on the Docker volume.
- Limits (`CR/Public/CesiumRuntimeSettings.h:12`, `Config = Engine`, section
  `[/Script/CesiumRuntime.CesiumRuntimeSettings]`): **`MaxCacheItems = 4096`** (`:63-68`, an item
  *count*, not bytes) and `RequestsPerCachePrune = 10000` (`:52-57`), both restart-only and read once
  into statics (`CesiumRuntime.cpp:107-121`). **Not set in CamSim's `Config/`**, so the default 4096
  applies: after every 10,000 requests the DB is pruned back to 4096 items, far too few for a warmed
  mission corridor. R5 must raise `MaxCacheItems` (e.g. 10^6) in `DefaultEngine.ini`. No byte cap
  exists. `ClearRequestCache()` (`:95-99`) clears it.
- file:// responses carry no headers, and the caching layer (lib strings in `libCesiumAsync.a`:
  `Cache-Control`, `max-age`, `Expires`, `no-store`, `no-cache`, `must-revalidate`,
  `stale-while-revalidate`, `If-None-Match`, `Last-Modified`) caches only responses the HTTP headers
  allow, so local package files are effectively not cached (they don't need it). HTTP responses are
  cached per server headers, and expired entries are revalidated with the server (from cesium-native
  behaviour, headers/strings consistent; **verify empirically**). Consequence for R5: offline reuse of
  ion/CWT tiles depends on ion's `Cache-Control` lifetimes and on the ion endpoint call
  (`api.cesium.com/v1/assets/{id}/endpoint`, token) being cached too. The R5 gate "replayed mission
  runs with networking disabled" is the real test; a package-local terrain/imagery copy over `file://`
  avoids the question for the package area.
- Not the same as `MaximumCachedBytes` (`Cesium3DTileset.h:444`, in-memory loaded tiles, default 256 MB),
  which CamSim sets from `maximum_cached_bytes_mb` (`CesiumTuning.cpp:41-45`). Side note:
  `docs/configuration.md:266` calls it the "tile cache budget" and says `0` = "uncapped"; in code `0`
  leaves the plugin default (256 MB).
- Concurrency per tileset/overlay: `MaximumSimultaneousTileLoads` (`Cesium3DTileset.h:432`, default 20)
  and the overlay setter CamSim already uses; no global request cap.

## 6. Ellipsoid / geoid

- All plugin heights are ellipsoidal: `SampleHeightMostDetailed` returns metres above the ellipsoid,
  "not ... mean sea level" (`Cesium3DTileset.h:130-140`); the globe anchor says the same
  (`CesiumGlobeAnchorComponent.h:435, 476, 501`). No geoid/EGM support anywhere in `CR/Public`
  (grep for geoid/EGM finds only those warnings).
- The ellipsoid is a `UCesiumEllipsoid` on the georeference (`CesiumGeoreference.h:117-123`, default
  WGS84) and is passed to overlays/loaders (`options.ellipsoid`, `CesiumUrlTemplateRasterOverlay.cpp:29`).
  Keep it WGS84.
- So R1 must convert 3DEP (NAVD88, orthometric) to WGS-84 ellipsoid heights before quantized-mesh
  encoding (GEOID18 + NAD83→WGS84/ITRF shift), as REALISM principle 7 says. CWT is already
  ellipsoidal; the feathered seam at the package edge should then match within the datum residual.
  Buildings/bridges in 3D Tiles are ECEF, so their base heights must also be ellipsoidal (sample the
  R1 terrain, not a NAVD88 DEM).
- Quantized-mesh `.terrain` tiles carry their own min/max height and are decoded as ellipsoid heights;
  `EnableWaterMask` (`Cesium3DTileset.h:899-902`) uses the watermask extension if the tiles have one
  (CamSim draws its own ocean, so leave it off).
