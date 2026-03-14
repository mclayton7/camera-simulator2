# Cesium Backend Configuration — Design Spec

**Date:** 2026-03-14
**Branch:** improve_rendering
**Status:** Approved

## Problem

CamSim has no runtime control over which Cesium tile or terrain servers are used. The ion token and server URL are baked into a level asset (`CesiumIonSaaS.uasset`), blocking air-gapped and multi-profile Docker deployments.

## Goals

- Configurable Cesium ion portal URL, API URL, and token
- Terrain source: Cesium ion | quantized-mesh URL | flat
- Imagery overlay: Cesium ion | WMS | none; terrain and imagery independent
- All settings overridable via environment variables
- No Unreal Editor changes required at deployment time

## Approach

Add a `FCesiumBackendConfig` struct **nested inside `FCamSimConfig`** (same pattern as `FPhase18Config`), parsed from a new `cesium:` YAML block. A new free function `ApplyCesiumBackendConfig()` declared in `CamSimGeospatialProvider.h` applies the config at `BeginPlay` by iterating all `ACesium3DTileset` actors. All such actors in this level are terrain (entity models use glTFRuntime), so no tag-based filtering is needed. If the world contains no tilesets and `flat` is requested, that is a no-op — a warning is logged.

## Cesium API Note

Exact Cesium for Unreal property and method names must be verified against the installed plugin version during implementation. The existing codebase (lines ~162-178 in `CamSimCamera.cpp`) uses **direct property assignment** for tileset streaming params — consistent with that pattern, this implementation also uses direct property assignment. After all terrain property changes, call `It->RefreshTileset()` once to reload with the new source. Overlay operations via `Activate(false)` trigger their own internal refresh and do not require a second `RefreshTileset()` call.

Key plugin names to verify at implementation time: `UCesiumIonServer::ServerUrl`, `UCesiumIonServer::ApiUrl`, `ACesium3DTileset::TilesetSource` / `IonAssetID` / `Url` (and whether `CesiumIonServer` is settable via direct assignment or requires a setter), `UCesiumWebMapServiceRasterOverlay::BaseUrl` / `Layers` / `TileWidth` / `TileHeight`.

## Config Struct

Nested inside `FCamSimConfig` body in `CamSimConfig.h`:

```cpp
struct FCesiumBackendConfig
{
    // UCesiumIonServer has two distinct URL fields:
    //   ServerUrl — portal/OAuth redirect URL (default: "https://ion.cesium.com")
    //   ApiUrl    — REST tile API endpoint    (default: "https://api.cesium.com")
    // For self-hosted ion these are typically different hosts.
    FString IonPortalUrl = TEXT("https://ion.cesium.com");  // → UCesiumIonServer::ServerUrl
    FString IonApiUrl    = TEXT("https://api.cesium.com");  // → UCesiumIonServer::ApiUrl
    // IonToken is the access token string. For headless/Docker use this is sufficient.
    // DefaultIonAccessTokenId (server-side record ID) is only needed for the Editor
    // sign-in UI flow, which is not used in CamSim.
    FString IonToken = TEXT("");  // empty = use level asset default; NEVER logged

    struct FTerrainConfig
    {
        FString Source     = TEXT("cesium_ion"); // "cesium_ion" | "url" | "flat"
        int32   IonAssetId = 1;                  // Cesium World Terrain
                                                 // int32: ion asset IDs currently fit in 32 bits;
                                                 // avoids needing a YamlInt64 helper
        FString Url        = TEXT("");
    } Terrain;

    struct FImageryConfig
    {
        FString Source        = TEXT("cesium_ion"); // "cesium_ion" | "wms" | "none"
        int32   IonAssetId    = 2;                  // Bing Maps Aerial (int32 sufficient)
        FString WmsUrl        = TEXT("");
        FString WmsLayers     = TEXT("");
        int32   WmsTileWidth  = 256;
        int32   WmsTileHeight = 256;
    } Imagery;
} CesiumBackend;
```

`WmsStyles` is omitted — `UCesiumWebMapServiceRasterOverlay` has no `Styles` field. Pass style parameters in the `WmsUrl` query string if needed.

## YAML Schema

New block in `deploy/camsim_config.yaml`:

```yaml
cesium:
  ion_portal_url: "https://ion.cesium.com"   # UCesiumIonServer::ServerUrl (OAuth/browser)
  ion_api_url:    "https://api.cesium.com"    # UCesiumIonServer::ApiUrl (REST tile API)
  ion_token: ""                               # leave empty to use level asset default

  terrain:
    source: cesium_ion    # cesium_ion | url | flat
    ion_asset_id: 1
    url: ""

  imagery:
    source: cesium_ion    # cesium_ion | wms | none
    ion_asset_id: 2
    wms_url: ""
    wms_layers: ""
    wms_tile_width: 256
    wms_tile_height: 256
```

## Environment Variable Overrides

Env var loading uses the existing `GetEnv` / `GetEnvInt` / `GetEnvFloat` helpers in `CamSimConfig.cpp`.

| Env var | Field |
|---|---|
| `CAMSIM_CESIUM_ION_PORTAL_URL` | `CesiumBackend.IonPortalUrl` |
| `CAMSIM_CESIUM_ION_API_URL` | `CesiumBackend.IonApiUrl` |
| `CAMSIM_CESIUM_ION_TOKEN` | `CesiumBackend.IonToken` |
| `CAMSIM_CESIUM_TERRAIN_SOURCE` | `CesiumBackend.Terrain.Source` |
| `CAMSIM_CESIUM_TERRAIN_ION_ASSET_ID` | `CesiumBackend.Terrain.IonAssetId` |
| `CAMSIM_CESIUM_TERRAIN_URL` | `CesiumBackend.Terrain.Url` |
| `CAMSIM_CESIUM_IMAGERY_SOURCE` | `CesiumBackend.Imagery.Source` |
| `CAMSIM_CESIUM_IMAGERY_ION_ASSET_ID` | `CesiumBackend.Imagery.IonAssetId` |
| `CAMSIM_CESIUM_IMAGERY_WMS_URL` | `CesiumBackend.Imagery.WmsUrl` |
| `CAMSIM_CESIUM_IMAGERY_WMS_LAYERS` | `CesiumBackend.Imagery.WmsLayers` |
| `CAMSIM_CESIUM_IMAGERY_WMS_TILE_WIDTH` | `CesiumBackend.Imagery.WmsTileWidth` |
| `CAMSIM_CESIUM_IMAGERY_WMS_TILE_HEIGHT` | `CesiumBackend.Imagery.WmsTileHeight` |

## Runtime Application

**Must be called on the game thread.** All Cesium actor mutations are game-thread-only. The call site in `ACamSimCamera::BeginPlay()` satisfies this.

### Signature

A **free function** in `CamSimGeospatialProvider.h` (not a member of the instance class `FCamSimGeospatialProvider`):

```cpp
// Returns the created UCesiumIonServer* (nullptr if ion server step was skipped).
// Caller must pass result to Subsystem->StoreCesiumIonServer() for GC rooting.
UCesiumIonServer* ApplyCesiumBackendConfig(UWorld* World, const FCesiumBackendConfig& Config);
```

Called from `ACamSimCamera::BeginPlay()` **after the closing brace of the existing `ACesium3DTileset` streaming-params for-loop** (currently lines ~162-178):

```cpp
UCesiumIonServer* Server = ApplyCesiumBackendConfig(GetWorld(), Cfg.CesiumBackend);
Subsystem->StoreCesiumIonServer(Server);
```

`StoreCesiumIonServer(UCesiumIonServer* Server)` is a new public method on `UCamSimSubsystem`. It stores the pointer (including `nullptr`) in a `TStrongObjectPtr<UCesiumIonServer>` inside `FSubsystemImpl`. Passing `nullptr` is a no-op (clears any existing stored pointer). `TStrongObjectPtr` is reset in `FSubsystemImpl::~FSubsystemImpl()`.

`CamSimSubsystem.h` adds: `class UCesiumIonServer;` forward declaration.
`CamSimSubsystem.cpp` adds: `#include "CesiumIonServer.h"` and `#include "UObject/StrongObjectPtr.h"`.

Note: `UCesiumIonServer` is a `UDataAsset` subclass; `NewObject<>(GetTransientPackage())` creates a valid transient runtime instance. GC is prevented by `TStrongObjectPtr`.

### Step 1 — Ion server

**Skip entirely** if all three are at their defaults:
- `IonPortalUrl == "https://ion.cesium.com"`
- `IonApiUrl == "https://api.cesium.com"`
- `IonToken.IsEmpty()`

Otherwise:

```cpp
UCesiumIonServer* CustomServer = NewObject<UCesiumIonServer>(GetTransientPackage());
CustomServer->ServerUrl             = Config.IonPortalUrl;
CustomServer->ApiUrl                = Config.IonApiUrl;
CustomServer->DefaultIonAccessToken = Config.IonToken;
for each ACesium3DTileset It:
    It->CesiumIonServer = CustomServer;  // direct property assignment; verify setter exists
return CustomServer;                     // nullptr if skipped
```

**Auth contract:** When a custom server is assigned, `ACesium3DTileset` uses the server's `DefaultIonAccessToken` for all requests — the tileset's own `IonAccessToken` is not consulted. When Step 1 is skipped, auth comes entirely from the editor-placed tileset configuration.

**Security:** `IonToken` must never appear in any `UE_LOG` call at any verbosity level, including in Steps 2–3 warning messages.

### Step 2 — Terrain source

Unknown `Source` → log warning (source string only, no token/URL values), fall back to `cesium_ion`.
`flat` with no tilesets found → log warning.

| Source | Action |
|---|---|
| `cesium_ion` | `It->TilesetSource = ETilesetSource::FromCesiumIon; It->IonAssetID = Config.Terrain.IonAssetId;` |
| `url` | `It->TilesetSource = ETilesetSource::FromUrl; It->Url = Config.Terrain.Url;` |
| `flat` | `It->SetActorHiddenInGame(true); It->SetActorEnableCollision(false);` |

After all terrain property changes, call `It->RefreshTileset()` once per tileset (required for direct property assignment; overlays manage their own refresh via `Activate(false)` and do not need a second call).

### Step 3 — Imagery overlay

**Removal** (always, on every tileset including flat — hidden tilesets are still iterable):

```cpp
TArray<UCesiumRasterOverlay*> Existing;
Tileset->GetComponents<UCesiumRasterOverlay>(Existing);
for (auto* O : Existing) { O->Deactivate(); O->DestroyComponent(); }
```

**Adding** (skip if `source: none` or terrain source is `flat`; when skipping due to flat, log `"imagery source ignored: terrain is flat"` at `Log` verbosity):

`cesium_ion`:
```cpp
auto* O = NewObject<UCesiumIonRasterOverlay>(Tileset, TEXT("CamSimImagery"));
O->IonAssetID = Config.Imagery.IonAssetId;
// Route auth through the ion server (not a per-overlay token field).
// When CustomServer is null (Step 1 skipped), O->CesiumIonServer stays at
// its component default, pointing to the level asset server.
if (CustomServer) { O->CesiumIonServer = CustomServer; }
O->RegisterComponent();
O->Activate(false); // false = activate without resetting internal state
```

`wms`:
```cpp
auto* O = NewObject<UCesiumWebMapServiceRasterOverlay>(Tileset, TEXT("CamSimImagery"));
O->BaseUrl    = Config.Imagery.WmsUrl;
O->Layers     = Config.Imagery.WmsLayers;
O->TileWidth  = Config.Imagery.WmsTileWidth;
O->TileHeight = Config.Imagery.WmsTileHeight;
O->RegisterComponent();
O->Activate(false);
```

Unknown `Imagery.Source` → log warning, treat as `none`.

## Files Modified

| File | Change |
|---|---|
| `Config/CamSimConfig.h` | Add `FCesiumBackendConfig` nested struct + `CesiumBackend` member inside `FCamSimConfig` |
| `Config/CamSimConfig.cpp` | Parse `cesium:` YAML block + env var overrides using existing `GetEnv`/`GetEnvInt` helpers; no logging of `IonToken` |
| `Geospatial/CamSimGeospatialProvider.h` | Declare free function `ApplyCesiumBackendConfig(UWorld*, const FCesiumBackendConfig&)` returning `UCesiumIonServer*`; forward-declare `struct FCesiumBackendConfig` and `class UCesiumIonServer` |
| `Geospatial/CamSimGeospatialProvider.cpp` | Implement; `#include "CesiumIonServer.h"`, `"CesiumIonRasterOverlay.h"`, `"CesiumWebMapServiceRasterOverlay.h"` |
| `Camera/CamSimCamera.cpp` | Call `ApplyCesiumBackendConfig()` + `Subsystem->StoreCesiumIonServer()` in `BeginPlay()` after existing tileset streaming-params loop |
| `Subsystem/CamSimSubsystem.h` | Add `StoreCesiumIonServer(UCesiumIonServer*)` public method; `class UCesiumIonServer;` forward declaration |
| `Subsystem/CamSimSubsystem.cpp` | Implement `StoreCesiumIonServer()`; `#include "CesiumIonServer.h"` and `#include "UObject/StrongObjectPtr.h"`; `TStrongObjectPtr<UCesiumIonServer>` in `FSubsystemImpl`; reset in `~FSubsystemImpl()` before other teardown |
| `deploy/camsim_config.yaml` | Add `cesium:` block with defaults |
| `docs/configuration.md` | Document `CAMSIM_CESIUM_*` env vars |

**Build.cs:** No changes — `CesiumRuntime` is already in `PublicDependencyModuleNames`.

## Verification

1. **Default unchanged**: no `cesium:` block → tile streaming identical to today
2. **Token override**: `CAMSIM_CESIUM_ION_TOKEN=<token>` → authenticated ion tiles
3. **Custom server**: `CAMSIM_CESIUM_ION_PORTAL_URL` + `CAMSIM_CESIUM_ION_API_URL` → requests go to custom host (verify via network capture)
4. **URL terrain**: `CAMSIM_CESIUM_TERRAIN_SOURCE=url` + `CAMSIM_CESIUM_TERRAIN_URL=<url>` → terrain from custom endpoint
5. **Flat terrain**: `CAMSIM_CESIUM_TERRAIN_SOURCE=flat` → no geometry, no tile streaming
6. **WMS imagery**: `CAMSIM_CESIUM_IMAGERY_SOURCE=wms` + URL + layers → imagery drapes over terrain
7. **No imagery**: `CAMSIM_CESIUM_IMAGERY_SOURCE=none` → terrain without overlay
8. **Invalid source**: unknown value → warning logged (no token/URL in message), safe fallback, no crash
9. **Token not in logs**: `CAMSIM_CESIUM_ION_TOKEN` set → grep full log confirms token never appears
