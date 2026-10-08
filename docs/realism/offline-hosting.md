# Offline (air-gapped) hosting of scene packages: research notes (REALISM R0)

Date: 2026-10-07. Plugin: Cesium for Unreal 2.29.1 (cesium-native v0.64.0), installed at
`/Users/Shared/Epic Games/UE_5.8/Engine/Plugins/Marketplace/CesiumForUnreal` (below: `CFU/`).
Research for `docs/realism-r0-spike.md`. The spike's sandboxed run (ports 80/443 blocked) later confirmed §6:
CamSim rendered the package with no network; the only attempts were `Main.umap`'s ion actors at frame 0.
No Cesium credit overlay was visible in the spike's frames.

Labels: **[verified]** = checked in local source/libs or a primary page; **[unconfirmed]** = inference or
secondary source, needs a test or a check with the vendor.

## TL;DR

1. Cesium for Unreal 2.29.1 cannot read any tile archive (3TZ, .3dtiles SQLite, MBTiles, PMTiles,
   GeoPackage, zip). Its only sources are `file:///` (one file per tile, via UE's file manager) and HTTP(S).
   2.30.0 (2026-10-01) adds nothing here. **[verified]**
2. The lowest-risk way to ship millions of tiles as one file and keep `file:///`: a **read-only
   filesystem image (SquashFS or EROFS)**, loop-mounted on the host and bind-mounted read-only into the
   container. No code, no sidecar, no extraction. **[unconfirmed: not yet tried with CamSim]**
3. If the archive has to be read by user-space (no mount privileges): serve it over HTTP on localhost.
   Options: a ~150-line custom server over a zip/3TZ (serves terrain, imagery and 3D Tiles alike),
   `3d-tiles-tools serve` (3TZ/.3dtiles only, Node), `pmtiles serve` (imagery only in practice), or an
   in-process server in CamSim using the `FHttpServerModule` it already runs for `/health`.
4. Global base: **ETOPO 2022 30″ (ice surface) + its geoid layer** for terrain and **NASA Blue Marble
   Next Generation 500 m** for imagery, both US-government works with no use restrictions; optionally
   the **ESA WorldCover 2021 Sentinel-2 RGBNIR composite** (10 m, CC BY 4.0) for a z9–10 ring around
   the package.
5. Offline Cesium World Terrain needs a commercial licence (Cesium ion Self-Hosted or a data licence).
   Caching CWT/Bing for offline use is prohibited by the ion ToS §2.2.2, and Cesium says Bing can't be
   used offline at all. **[verified, ToS dated 2025-08-20/27]**
6. In a packaged or `-game` CamSim with only `file://`/localhost sources, nothing in the Cesium runtime
   contacts ion. What does go online today is CamSim's **own defaults** (ion asset 1 terrain, asset 2
   Bing imagery) and credit images with `http` URLs. **[verified in source; a network-isolated run is
   still needed]**

---

## 1. Archive formats Cesium for Unreal 2.29.1 can read directly

### Local evidence

- **Asset accessor chain (fixed, not pluggable):** `CFU/Source/CesiumRuntime/Private/CesiumRuntime.cpp:119-130`.
  `getAssetAccessor()` is a function-local static:
  `GunzipAssetAccessor( CachingAssetAccessor( UnrealAssetAccessor, SqliteCache ) )`. No API or setting
  lets a project swap in its own `IAssetAccessor`. `ACesium3DTileset` and the raster overlays all call
  `getAssetAccessor()` (`Cesium3DTileset.cpp:993, 2436`, `CesiumRasterOverlay.cpp:201`). A custom
  archive accessor would therefore mean patching the plugin, which CLAUDE.md rules out (the prebuilt
  engine plugin only).
- **`file:///` path** (`UnrealAssetAccessor.cpp:147-150, 334-410`): a URL beginning with `file:///` becomes
  a native path (`Uri::uriPathToNativePath`). `FCesiumReadFileWorker` on `GIOThreadPool` reads it with
  `FFileHelper::LoadFileToArray`. The result is status 200, or 404 if the read fails. There are no
  headers and the content type is empty. There is no archive lookup anywhere on this path.
- **Static-lib strings** (Linux-x86_64-Release and Darwin libs, all `lib*.a`, grep for `.3tz`, `3dtiles`
  (as a file extension), `mbtiles`, `pmtiles`, `geopackage`, `gpkg`, `ArchiveAsset`, `ZipAsset`,
  `minizip`, `libzip`, `unzip`): **no hits.** The only `3dtiles` hits are C++ namespace names (`Cesium3DTiles*`).
  The `zip`-related hits are `CesiumUtility::gzip/gunzip/isGzip` (`Gzip.cpp`), `GunzipAssetAccessor`, and
  MIME strings `application/gzip`, `application/zip` (content-type tables).
  `sqlite` appears only in `libCesiumAsync.a` (`SqliteCache`, `SqliteHelper`: the request cache) and in
  `libsqlite3.a` (bundled as `cesium_sqlite3`). Nothing reads tiles from a SQLite database.
- **Headers** (`CFU/Source/ThirdParty/include/CesiumAsync/`): `CachingAssetAccessor.h`, `GunzipAssetAccessor.h`,
  `CesiumIonAssetAccessor.h`, `SqliteCache.h`. No archive accessor exists.
- **Gzip:** `GunzipAssetAccessor` checks every response body for the gzip magic number (`isGzip`) and
  inflates it, whatever the headers say. Pre-gzipped `.terrain` files on disk work, and an HTTP server
  can return the stored bytes as they are, with or without `Content-Encoding: gzip`. **[verified by the
  spike for file://; HTTP is inferred]**

### Upstream changelogs

- **Cesium for Unreal 2.30.0 (2026-10-01; the task said 09-29):** adds Blueprint styling of 3D Tiles
  features and per-element styling of vector-tile overlays, and fixes an `FCesiumCamera` bug. It has no
  archive or offline items. 2.29.1 only updated cesium-native from 0.63 to 0.64.
  ([CHANGES.md](https://github.com/CesiumGS/cesium-unreal/blob/main/CHANGES.md)) **[verified]**
- **cesium-native up to v0.65.0 (2026-10-01) and unreleased:** no entries for 3tz, 3D Tiles archives,
  .3dtiles, pmtiles, mbtiles or GeoPackage. The related entries are `GunzipAssetAccessor` (0.25),
  `CurlAssetAccessor` (0.49) and `CesiumIonAssetAccessor` (0.50).
  ([CHANGES.md](https://github.com/CesiumGS/cesium-native/blob/main/CHANGES.md)) **[verified]**
- **CesiumJS** doesn't load 3TZ either. Cesium's forum advice is to unzip it, or to serve it
  ([forum](https://community.cesium.com/t/how-to-load-3tz/37419)).

**Conclusion:** no packed format is readable directly. The ways to get a single-file package are
(a) a filesystem image mounted under a `file:///` path, (b) a UE `.pak` mounted at runtime, or (c) a
localhost HTTP server.

### Option (b): UE `.pak` (needs a test before relying on it)

`LoadFileToArray` goes through `IFileManager`, which goes through the platform-file chain. When a
`FPakPlatformFile` is in that chain (it always is in packaged builds), files inside a mounted pak resolve
transparently. `FPakPlatformFile::Mount(PakFilename, Order, MountPath)` accepts any mount point
(`Engine/Source/Runtime/PakFile/Private/IPlatformFilePak.cpp:5998-6036`, `Pak->SetMountPoint(InPath)`),
and `UnrealPak` ships with the engine (`Engine/Binaries/Mac/UnrealPak`).

**[unconfirmed]**
- whether an absolute mount point such as `/data/scene/` resolves for a `file:///data/scene/...` URL;
- the size of the pak index for about 4 M entries (expect tens to low hundreds of MB);
- whether the editor `-game` build has the pak platform file active.

The approach also ties the package build to an Unreal install. For those reasons it is less attractive
than SquashFS.

## 2. Local tile servers (sidecar or in-process)

Requirements for a server: serve quantized-mesh (`layer.json` plus `.terrain` files, geographic TMS,
**2 × 1 root tiles**), imagery (TMS/XYZ JPEG) and 3D Tiles (`tileset.json` plus `.glb`/`.b3dm`, keyed by path).
It should not send long `Cache-Control` lifetimes. Otherwise `CachingAssetAccessor` copies every tile into
`cesium-request-cache.sqlite`, and with `MaxCacheItems` raised (R5) that duplicates the package.
Send no cache headers, or `no-store`.

| Server | Reads | Quantized-mesh | 3D Tiles | JPEG z/x/y | Licence | Status (GitHub) | Image |
|---|---|---|---|---|---|---|---|
| **nginx / Caddy / any static server** | directory (or a mounted SquashFS) | yes (serves bytes as stored; add `Content-Encoding: gzip` for `.terrain`, which is optional for Cesium) | yes | yes | BSD-2 / Apache-2.0 | mature | `nginx`, `caddy` |
| **3d-tiles-tools `serve`** (CesiumGS) | directory, **.3tz, .3dtiles** without extracting | **[unconfirmed]**: generic path lookup inside the package, so a zip holding `layer.json` + tiles probably works, but it's untested | yes | **[unconfirmed]** same | Apache-2.0 | active, pushed 2026-07-24, no GitHub releases (npm) | none official (Node) |
| **go-pmtiles `pmtiles serve`** (Protomaps) | PMTiles v3 | **poor fit**: the extension must match the tile type (`mvt/png/jpg/webp/avif`), and PMTiles' Hilbert tile ID assumes 2^z × 2^z per zoom, while geographic quantized-mesh has 2^(z+1) × 2^z. Storing it would need a custom id remap and the "Unknown" type 0x00 | no | yes (Web Mercator) | BSD-3-Clause | active, v1.31.2 2026-07-22 | `protomaps/go-pmtiles` |
| **martin** (MapLibre) | PostGIS, PMTiles, MBTiles | not documented **[unconfirmed]** | no | yes | Apache-2.0 / MIT | very active, v1.16.1 2026-09-09 | `ghcr.io/maplibre/martin` |
| **mbtileserver** (consbio) | MBTiles | no (png/jpg/webp/pbf only) | no | yes | ISC | slow: last release v0.11.0 2024-10, last push 2025-05 | `ghcr.io/consbio/mbtileserver` |
| **tileserver-gl** (MapTiler) | MBTiles/PMTiles + GL styles | no | no | yes | BSD-2 (repo shows NOASSERTION; multiple licences) | active, v5.6.0 2026-04 | `maptiler/tileserver-gl` |
| **cesium-terrain-server** (geo-data) | directory of `.terrain` | yes | no | no | Apache-2.0 per README (repo shows none) | unmaintained, last push 2022-11 | `geodata/cesium-terrain-server` |
| **Custom (Go `archive/zip` + `http.FileServer(http.FS(zr))`)** | one zip/3TZ (stored entries) | yes | yes | yes | ours | ~150 lines; static binary in a distroless image | ours |
| **In-process (CamSim `FHttpServerModule` on 127.0.0.1)** | anything we code (zip/3TZ/MBTiles via the bundled SQLite) | yes | yes | yes | ours | no extra container; same ticker caveats as `/health` | n/a |

Notes:
- **MBTiles** (SQLite `tiles(zoom_level, tile_column, tile_row, tile_data)`) can hold geographic
  quantized-mesh, because columns are plain integers. However, no common server serves arbitrary blobs
  with a `.terrain` route, and 3D Tiles don't fit it.
- **3TZ** is a ZIP plus an `@3dtilesIndex1@` entry: a sorted table of MD5 path hashes and offsets,
  24 bytes per entry, giving O(log n) lookup without reading the central directory.
  ([3d-tiles-tools](https://github.com/CesiumGS/3d-tiles-tools),
  [format note](https://docs.3dtiled.iconem.com/docs/references/3dtiles-gltf/3tz-3dtiles-packages)) A zip
  is path-keyed, so one zip or 3TZ can hold terrain, imagery and buildings unchanged. That makes it the
  natural single-file container when an HTTP server is used. A plain zip needs its central directory in
  memory: about 70–100 B per entry, roughly 0.3–0.4 GB for 4 M entries. **[estimate]**
- PMTiles is the best of these for imagery (deduplicates ocean/blank tiles, HTTP range reads, active
  tooling). Mixing formats per layer adds complexity, though, and every layer still needs a server.

## 3. Millions of small files vs one archive (quantitative)

The spike package measured tile sizes: imagery 2,293 JPEGs averaging **14.9 KB**, terrain 629 `.terrain`
averaging **7.3 KB**. The on-disk footprint on APFS is +12 % for imagery and +25 % for terrain (4 KB
block slack). For a **40,000 km²** package (20 terrain/km² to z16, 75 imagery/km² to z17):

| | Files | Payload |
|---|---|---|
| Terrain | ~0.8 M | ~5.9 GB |
| Imagery | ~3.0 M | ~45 GB |
| Total (+ directories, 3D Tiles) | **~4 M** | **~51 GB** |

- **ext4 inodes:** the `mke2fs.conf` default `inode_ratio` is 16384 bytes per inode, so about 61 M inodes per
  TB and about 6 M on a 100 GB volume. 4 M files fits, but a small dedicated volume, or one formatted with
  `-T largefile` (1 MB per inode), runs out of inodes before it runs out of space. With 4 KB blocks the
  slack is about 2 KB per file, roughly 8 GB per package.
- **exFAT/NTFS transfer media:** exFAT's default cluster on 32 GB–256 TB volumes is **128 KB**. Every 7–15 KB
  tile then takes 128 KB, so 4 M files need about **500 GB** of media for 51 GB of data. A single archive
  avoids this.
- **Copy and verification time [estimates]:** small-file copies are bound by metadata, at about
  1–10 ms per file on USB or network media (create, write, close, plus antivirus or cross-domain scanning
  per file). 4 M files take **1–11 h**. One 51 GB file at 200–400 MB/s takes **2–5 min**. Per-file
  hashing for a manifest adds the same per-file overhead again. Many cross-domain and transfer tools also
  scan or log each file.
- **Docker:** never bake tiles into an image. Layer extraction on overlay2 of millions of files is slow,
  and image size limits apply. A **read-only bind mount** of a host directory or mounted image has no
  per-file cost on Linux. Docker Desktop on macOS (virtiofs) is slow for many small-file opens, but that is
  dev only. Named volumes populated by `docker cp` pay the per-file cost again.
- **SquashFS/EROFS image:** one file, 51 GB or less (JPEG and gzip tiles barely compress further;
  use `-noD`/no compression to save CPU). Mount with `mount -o loop,ro`, or with `squashfuse` without
  root, then bind-mount it with `-v /mnt/pkg:/data/scene:ro`. Cesium sees ordinary files over `file:///`.
  It has random access, a dentry index, no extraction, and integrity from one hash of the image.
  This is the best fit for "one file on the media, millions of files at runtime". **[unconfirmed for
  CamSim; standard Linux practice]**

## 4. Global base layers (whole-Earth coverage outside the package)

Sizing: geographic quantized-mesh z0–8 is about 175 k tiles, and z0–10 about 2.8 M. Web Mercator imagery
z0–8 is about 87 k tiles, and z0–10 about 1.4 M. Ocean tiles deduplicate in a packed or image format.
Resolution needed: imagery z8 is about 611 m/px at the equator, z10 about 153 m/px. A z8 terrain tile is
about 0.7°, which at 65 × 65 samples gives about 1.2 km spacing. **Recommendation: global to z8, plus a z9–10
ring of a few hundred km around the package.** A high oblique UAV view only sees beyond the package at
coarse LOD anyway.

**One terrain tileset (CLAUDE.md gotcha):** the global base and the package's high-resolution terrain
must be merged into **one** `layer.json` pyramid, with `available` ranges for both. Imagery can be one
merged pyramid, or a base overlay plus a package overlay; the coverage-edge behaviour of the second case
still needs testing.

### DEMs

| Dataset | Resolution | Bathymetry | Vertical datum | Licence | Size | Access | Fit as base |
|---|---|---|---|---|---|---|---|
| **ETOPO 2022** (NOAA NCEI) | 15″, 30″, 60″ | **yes** (seamless land and ocean) | geoid (EGM2008); the **geoid-height grid ships alongside** | US government work, no copyright in the US (17 USC 105). NCEI asks for a citation, DOI 10.25921/fd45-gt74 (15″). **[licence statement not on the product page; standard NOAA terms]** | 60″ surface GeoTIFF **466 MB**; 30″ surface **1.59 GB**, 30″ geoid 1.45 GB (HTTP HEAD, verified); 15″ comes as 15° tiles | [product page](https://www.ncei.noaa.gov/products/etopo-global-relief-model), e.g. `https://www.ngdc.noaa.gov/mgg/global/relief/ETOPO2022/data/30s/30s_surface_elev_gtif/ETOPO_2022_v1_30s_N90W180_surface.tif` | **Best.** Global, includes seabed (useful for coastal and ocean HOT), "surface" variant has ice-sheet tops |
| GMTED2010 (USGS) | 7.5″, 15″, 30″ | no | EGM96 | public domain (USGS) | moderate (EarthExplorer tiles) | [USGS](https://www.usgs.gov/coastal-changes-and-impacts/gmted2010), EarthExplorer | Land only, 84N–56S at 7.5″/15″. Inferior to ETOPO for a base |
| Copernicus GLO-90 / GLO-30 | 3″ / 1″ | no | EGM2008 | free licence; redistribution allowed with the notice "© DLR e.V. 2010-2014 and © Airbus Defence and Space GmbH 2014-2018 provided under COPERNICUS by the European Union and ESA; all rights reserved". GLO-30 withholds a few countries' tiles | GLO-90 ~130 GB (third-party estimate) | [AWS registry](https://registry.opendata.aws/copernicus-dem/) `s3://copernicus-dem-90m` | **DSM**: canopy and building tops, which double up with 3D buildings and vegetation. Overkill for z≤10. Good for non-US packages |
| SRTM v3 (NASA) | 1″ / 3″ | no | EGM96 | public domain | ~ tens of GB | USGS EarthExplorer | 60N–56S only, voids, DSM-like (C-band) |

### Imagery

| Dataset | Resolution | Licence | Size | Access | Fit |
|---|---|---|---|---|---|
| **NASA Blue Marble Next Generation** (BMNG, Stöckli, 2004) | **500 m** (15″; 86400 × 43200 in 8 tiles of 21600²), also 2 km and 8 km; 12 monthly composites; `world.topo.bathy` has shaded relief and bathymetry | NASA imagery: no US copyright. Credit "NASA Earth Observatory / Reto Stöckli". **[the Visible Earth use-terms page wasn't readable; NASA media guidelines say NASA content is generally not copyrighted]** | ~285 MB per 21600² PNG tile × 8 ≈ **2.3 GB per month** (HTTP HEAD, verified) | e.g. `https://eoimages.gsfc.nasa.gov/images/imagerecords/73000/73751/world.topo.bathy.200407.3x21600x21600.A1.png` | **Best base to z8.** Seasonal choice per mission month. Stylised colour (relief shading baked in; use plain `world.200407` to avoid double shading) |
| **ESA WorldCover 2021 S2 RGBNIR composite** (VITO) | **10 m**, cloud-free median, 1° COGs with overviews | listed **CC BY 4.0** on the AWS registry. ESA's page says "free of charge for all users" and the WorldCover maps are CC BY 4.0. Attribution: "© ESA WorldCover project 2021 / Contains modified Copernicus Sentinel data (2021) processed by ESA WorldCover consortium". **[composite licence: confirm on the WorldCover licence page]** | 8.5 TB per year in full; read only the COG overviews for z9–10 (HTTP range reads, as `fetch_worldcover.py` already does) | [AWS](https://registry.opendata.aws/esa-worldcover-vito-composites) `s3://esa-worldcover-s2` | **Best ring source z9–12**, and natural-colour consistent with the land cover CamSim already uses. Land only (oceans have no tiles: fill with BMNG) |
| EOX Sentinel-2 cloudless | 10 m | current licence page: **CC BY-NC-SA 4.0** non-commercial, commercial = EOX licence; it no longer distinguishes 2016 (historically CC BY 4.0). No free bulk download (WMTS or buy) | n/a | [licence](https://cloudless.eox.at/documentation/license) | **Avoid**: the NC-SA and WMTS-harvest question isn't worth it when WorldCover S2 exists |
| NaturalEarth rasters (NE1/NE2/HYP) | 1:10m about 1′ (~1.8 km), 21600 × 10800 | public domain | ~300–400 MB | naturalearthdata.com | Cartographic look; only z≤6. BMNG is better |
| Landsat / Sentinel-2 Global Mosaic (Copernicus) | 30 m / 10 m | public domain / free Copernicus | very large | USGS / Copernicus Data Space | Possible, but WorldCover S2 is already cloud-free and simpler |

**Recommendation:** **ETOPO 2022 30″ ice-surface + ETOPO geoid grid** (convert to WGS-84 ellipsoid
heights at build time, per REALISM principle 7) and **BMNG 500 m** (choose the month) for z0–8 globally,
with **WorldCover S2 2021** in a ring for z9–10 and as the fill where NAIP ends. All three are public
domain or CC BY and can be redistributed with the ATTRIBUTION.txt the manifest generates.

## 5. Cesium ion Self-Hosted and offline licensing of CWT / Bing

- **Cesium ion Self-Hosted** is the commercial on-premises ion. It runs on Kubernetes (cloud, data centre,
  edge), "can support air-gapped solutions", tiles your own data, and has SAML and a REST API. Pricing is by
  contact with sales. ([product page](https://cesium.com/platform/cesium-ion/cesium-ion-self-hosted/))
  It would let CamSim keep `ETilesetSource::FromCesiumIon` against a local `UCesiumIonServer`
  (`cesium.ion_portal_url`/`ion_api_url`, which CamSim already exposes).
- **Curated data offline:** Cesium staff (Kevin Ring, 2024-01-08/09) say "We license it for offline use".
  CWT is "very large - several terabytes". CWT, OSM Buildings and Sentinel-2 imagery "can also be licensed
  from us for offline use", while **Google Photorealistic 3D Tiles and Bing Maps Aerial "can _not_ be used
  offline"**. ([forum](https://community.cesium.com/t/how-to-use-cesium-world-terrain-in-offline/29096))
  Whether CWT comes bundled with Self-Hosted or as an add-on is reported both ways: **ask sales**.
- **ion Terms of Service** (major update 2025-08-20, minor 2025-08-27,
  [ToS](https://cesium.com/legal/terms-of-service/)):
  - §2.2.2: you "may not copy, store, or redistribute any portion of Cesium Data Output … in, or for use in,
    an offline, disconnected, or local computer environment", except Clips under §2.3. Client and
    proxy caching is allowed only as "a general caching mechanism for performance" that "caches other
    internet traffic as well". **Pre-warming Cesium's SQLite cache with CWT or Bing for an air-gapped run
    is therefore not allowed.** This matters for R5's cache-warming plan.
  - §2.2(b): no display or use of Cesium Data Output after the Term.
  - §2.3 Clips (clip & ship): Clips are licensed during the Term to make "Value-Added Clips". Those carry a
    perpetual licence and may be distributed. The datasets that can be clipped, and the size limits, are in
    Appendix A and your Plan (not checked). Using a raw Clip offline is ambiguous. **[unconfirmed: check
    with Cesium]**
  - §2.4 / Appendix B-1: Bing terms apply on top (not read).
- **Implication:** an air-gapped CamSim either buys Self-Hosted plus a CWT licence, or uses open data
  (section 4 plus package terrain). The open-data path is consistent with REALISM's licence register.

## 6. Network access by Cesium for Unreal with file:// sources only

Checked in `CFU/Source/CesiumRuntime` (in packaged and `-game` builds; `CesiumEditor` is `"Type": "Editor"`
in the `.uplugin`, so it doesn't load in `-game` or packaged builds):

- **Hard-coded URLs in the runtime module:** only `https://ion.cesium.com` / `https://api.cesium.com`
  (`CesiumIonServer.h:78,91`, `.cpp:19-20,173-174`) and `https://dev.virtualearth.net`
  (`CesiumBingMapsRasterOverlay.cpp:42`). The ion URLs are used only by ion sources
  (`ETilesetSource::FromCesiumIon`, `UCesiumIonRasterOverlay`, ion GeoJSON/vector overlays, the geocoder).
  `UCesiumIonServer::ResolveApiUrl()`, which makes a network call, is `#if WITH_EDITOR`
  (`CesiumIonServer.cpp:84-196`; called from `Cesium3DTileset.cpp:1170-1172`).
  **Caveat:** CamSim's `run.sh` runs the *editor binary* with `-game`, so `WITH_EDITOR` is 1 there. It still
  only fires for an ion tileset whose `ApiUrl` is empty.
- **`ETilesetSource::FromUrl`** (`Cesium3DTileset.cpp:1153-1158`) passes the URL straight to cesium-native. No
  ion lookup, no token refresh. `CesiumIonAssetAccessor` (token refresh) is only used for ion assets.
- **No analytics or telemetry** in CesiumRuntime. The only other HTTP use is
  `ScreenCreditsWidget.cpp:235-256`, which downloads credit **images** whose `src` is not a `data:` URI
  (for example Bing/ion logos). Local sources' credits come from `layer.json` attribution or tileset
  copyright and are text unless we add images. The credits widget is added to the viewport
  (`CesiumCreditSystem.cpp:250,278`), and CamSim's viewport is the video. Check that `ACesiumCreditSystem`
  is hidden or removed in CamSim (CamSim code has no reference to it) **[unconfirmed]**.
- **`CachingAssetAccessor` + SQLite cache** is local only. file:// responses carry no headers and aren't cached.
- **The editor module** (`CesiumEditor`, editor only) resumes the ion session at startup
  (`CesiumIonServerManager.cpp:95-96`, `CesiumPanel.cpp:48`). It is irrelevant to the container, but the
  editor will try to reach ion when a technician opens the project air-gapped. That attempt should fail
  harmlessly.
- **What actually goes online in CamSim today:** the defaults `cesium.terrain.source: cesium_ion`
  (`ion_asset_id: 1`) and Bing imagery (`ion_asset_id: 2`)
  (`Config/CamSimConfig.h:389-405`). `ApplyCesiumBackendConfig` keeps `Main.umap`'s CWT tileset. An offline
  profile must set terrain/imagery `source` to url/tms (or `flat`) and should fail fast (no ion fallback)
  when `scene.dir` is set and `offline: true`.
- **Engine-level [unconfirmed]:** UE CrashReportClient upload and any engine analytics in a Development
  package. Verify with a **network-isolated acceptance run**: `docker run --network none` with
  `CAMSIM_MULTICAST_ADDR=127.0.0.1` and the CIGI test host inside the container, or an iptables egress
  drop, and log failed connections. This should become an R0 exit gate.

## Recommendations (ordered)

1. **Runtime access = `file:///` to a directory**, which is what the spike already proved. **Transport = one
   SquashFS (or EROFS) image per package**, loop-mounted read-only on the host and bind-mounted `:ro` into
   the container. This needs no code and no sidecar, and gives one hash per package. Prototype it in R0 and
   time a cold start against the plain directory.
2. Keep a **zip/3TZ + localhost HTTP** fallback for hosts that can't mount: a tiny Go server (preferred:
   static binary, zip `http.FS`, no cache headers), or a CamSim in-process `FHttpServerModule` route.
   `3d-tiles-tools serve` is fine for development. Avoid PMTiles for terrain (geographic 2:1 tiling doesn't map).
3. Package build: **global base z0–8** (ETOPO 2022 30″ to ellipsoid + BMNG 500 m) merged into the package's
   single terrain pyramid and its imagery pyramid; **z9–10 ring** from WorldCover S2 2021 (CC BY 4.0, confirm).
   Ocean tiles: deduplicate (hard links within SquashFS, which also deduplicates identical files itself).
4. **Offline mode in config:** a profile that disables every ion source, plus an R0 gate run with egress
   blocked. Hide Cesium on-screen credits; write ATTRIBUTION.txt instead.
5. Do **not** plan on caching CWT or Bing for offline (ToS §2.2.2). If CWT fidelity is needed air-gapped,
   it's a Cesium ion Self-Hosted plus data licence conversation with sales@cesium.com.
6. Revisit if cesium-native adds archive or `IAssetAccessor` injection (none as of v0.65.0). A plugin patch
   for a zip accessor is possible, but it conflicts with the "Cesium lives in the engine, prebuilt" rule.
