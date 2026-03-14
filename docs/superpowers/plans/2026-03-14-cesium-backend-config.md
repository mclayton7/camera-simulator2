# Cesium Backend Configuration Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expose Cesium ion server URL/token, terrain source, and imagery overlay as runtime config via a `cesium:` YAML block and `CAMSIM_CESIUM_*` env vars.

**Architecture:** `FCesiumBackendConfig` (nested in `FCamSimConfig`) is parsed from YAML + env vars. A free function `ApplyCesiumBackendConfig()` iterates all `ACesium3DTileset` actors at `BeginPlay` and applies ion server, terrain source, and raster overlay configuration. The resulting `UCesiumIonServer*` is stored in `FSubsystemImpl` via `TStrongObjectPtr` for GC safety.

**Tech Stack:** Unreal Engine 5.7 C++, CesiumForUnreal plugin, rapidyaml (bundled), UE5 Automation Tests

**Spec:** `docs/superpowers/specs/2026-03-14-cesium-backend-config-design.md`

---

## Chunk 1: Config Struct, YAML Parsing, Env Vars, and Tests

### Task 1: Add FCesiumBackendConfig struct to FCamSimConfig

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.h:391-394`

- [ ] **Step 1: Insert the struct after `FPhase19Config Phase19;` (line 391)**

Insert after line 391 in `CamSimConfig.h`:

```cpp
	/** Cesium backend: ion server, terrain source, imagery overlay */
	struct FCesiumBackendConfig
	{
		// UCesiumIonServer has two distinct URL fields:
		//   ServerUrl — portal/OAuth redirect URL (default: "https://ion.cesium.com")
		//   ApiUrl    — REST tile API endpoint    (default: "https://api.cesium.com")
		// For self-hosted ion these are typically different hosts.
		FString IonPortalUrl = TEXT("https://ion.cesium.com");  // → UCesiumIonServer::ServerUrl
		FString IonApiUrl    = TEXT("https://api.cesium.com");  // → UCesiumIonServer::ApiUrl
		// IonToken: never log at any verbosity level.
		// DefaultIonAccessTokenId not set — only needed for Editor sign-in UI, not headless use.
		FString IonToken = TEXT("");  // empty = use level asset default

		struct FTerrainConfig
		{
			FString Source     = TEXT("cesium_ion"); // "cesium_ion" | "url" | "flat"
			int32   IonAssetId = 1;                  // Cesium World Terrain
			FString Url        = TEXT("");
		} Terrain;

		struct FImageryConfig
		{
			FString Source        = TEXT("cesium_ion"); // "cesium_ion" | "wms" | "none"
			int32   IonAssetId    = 2;                  // Bing Maps Aerial
			FString WmsUrl        = TEXT("");
			FString WmsLayers     = TEXT("");
			int32   WmsTileWidth  = 256;
			int32   WmsTileHeight = 256;
		} Imagery;
	} CesiumBackend;
```

### Task 2: Write automation test for FCesiumBackendConfig defaults

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Tests/CesiumBackendConfigTest.cpp`

- [ ] **Step 1: Create the test file**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"

// -------------------------------------------------------------------------
// Cesium Backend Configuration — Automation Tests
// -------------------------------------------------------------------------

// 1. FCesiumBackendConfig defaults
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCesiumBackendConfigDefaultsTest,
	"CamSim.CesiumBackend.ConfigDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCesiumBackendConfigDefaultsTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;

	TestEqual(TEXT("IonPortalUrl default"), Cfg.CesiumBackend.IonPortalUrl, TEXT("https://ion.cesium.com"));
	TestEqual(TEXT("IonApiUrl default"),    Cfg.CesiumBackend.IonApiUrl,    TEXT("https://api.cesium.com"));
	TestTrue (TEXT("IonToken empty by default"), Cfg.CesiumBackend.IonToken.IsEmpty());

	TestEqual(TEXT("Terrain.Source default"),     Cfg.CesiumBackend.Terrain.Source,     TEXT("cesium_ion"));
	TestEqual(TEXT("Terrain.IonAssetId default"),  Cfg.CesiumBackend.Terrain.IonAssetId, 1);
	TestTrue (TEXT("Terrain.Url empty by default"), Cfg.CesiumBackend.Terrain.Url.IsEmpty());

	TestEqual(TEXT("Imagery.Source default"),       Cfg.CesiumBackend.Imagery.Source,       TEXT("cesium_ion"));
	TestEqual(TEXT("Imagery.IonAssetId default"),    Cfg.CesiumBackend.Imagery.IonAssetId,   2);
	TestTrue (TEXT("Imagery.WmsUrl empty by default"), Cfg.CesiumBackend.Imagery.WmsUrl.IsEmpty());
	TestEqual(TEXT("Imagery.WmsTileWidth default"),  Cfg.CesiumBackend.Imagery.WmsTileWidth,  256);
	TestEqual(TEXT("Imagery.WmsTileHeight default"), Cfg.CesiumBackend.Imagery.WmsTileHeight, 256);

	return true;
}

// 2. Terrain source strings are valid values
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCesiumBackendTerrainSourceValuesTest,
	"CamSim.CesiumBackend.TerrainSourceValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCesiumBackendTerrainSourceValuesTest::RunTest(const FString& Parameters)
{
	// Valid source strings — just check these are the known values
	const TArray<FString> ValidSources = { TEXT("cesium_ion"), TEXT("url"), TEXT("flat") };
	TestTrue(TEXT("cesium_ion in valid set"),
		ValidSources.Contains(TEXT("cesium_ion")));
	TestTrue(TEXT("url in valid set"),
		ValidSources.Contains(TEXT("url")));
	TestTrue(TEXT("flat in valid set"),
		ValidSources.Contains(TEXT("flat")));
	TestFalse(TEXT("gibberish not in valid set"),
		ValidSources.Contains(TEXT("gibberish")));
	return true;
}

// 3. Imagery source strings are valid values
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCesiumBackendImagerySourceValuesTest,
	"CamSim.CesiumBackend.ImagerySourceValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCesiumBackendImagerySourceValuesTest::RunTest(const FString& Parameters)
{
	const TArray<FString> ValidSources = { TEXT("cesium_ion"), TEXT("wms"), TEXT("none") };
	TestTrue(TEXT("cesium_ion in valid set"), ValidSources.Contains(TEXT("cesium_ion")));
	TestTrue(TEXT("wms in valid set"),        ValidSources.Contains(TEXT("wms")));
	TestTrue(TEXT("none in valid set"),       ValidSources.Contains(TEXT("none")));
	TestFalse(TEXT("gibberish not in valid set"), ValidSources.Contains(TEXT("gibberish")));
	return true;
}
```

### Task 3: Add YAML parsing for `cesium:` block

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.cpp:835`

The Phase 19 block ends at line 835 (`}`). Insert the `cesium:` block after the Phase 19 block and before the `// Phase 20: overlay HUD/OSD` comment (line 837):

- [ ] **Step 1: Insert YAML parsing block after line 835 in CamSimConfig.cpp**

```cpp
		// Cesium backend: ion server, terrain source, imagery overlay
		if (Root.has_child("cesium"))
		{
			ryml::ConstNodeRef Cs = Root["cesium"];
			YamlString(Cs, "ion_portal_url", Cfg.CesiumBackend.IonPortalUrl);
			YamlString(Cs, "ion_api_url",    Cfg.CesiumBackend.IonApiUrl);
			YamlString(Cs, "ion_token",      Cfg.CesiumBackend.IonToken);

			if (Cs.has_child("terrain"))
			{
				ryml::ConstNodeRef Tr = Cs["terrain"];
				YamlString(Tr, "source",       Cfg.CesiumBackend.Terrain.Source);
				YamlInt   (Tr, "ion_asset_id", Cfg.CesiumBackend.Terrain.IonAssetId);
				YamlString(Tr, "url",          Cfg.CesiumBackend.Terrain.Url);
			}

			if (Cs.has_child("imagery"))
			{
				ryml::ConstNodeRef Im = Cs["imagery"];
				YamlString(Im, "source",          Cfg.CesiumBackend.Imagery.Source);
				YamlInt   (Im, "ion_asset_id",    Cfg.CesiumBackend.Imagery.IonAssetId);
				YamlString(Im, "wms_url",         Cfg.CesiumBackend.Imagery.WmsUrl);
				YamlString(Im, "wms_layers",      Cfg.CesiumBackend.Imagery.WmsLayers);
				YamlInt   (Im, "wms_tile_width",  Cfg.CesiumBackend.Imagery.WmsTileWidth);
				YamlInt   (Im, "wms_tile_height", Cfg.CesiumBackend.Imagery.WmsTileHeight);
			}
		}
```

### Task 4: Add env var overrides for CesiumBackend

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.cpp:1097`

After the Phase 19 env vars (line 1097) and before `// Phase 20: overlay HUD/OSD env var overrides`:

- [ ] **Step 1: Insert env var overrides after line 1097 in CamSimConfig.cpp**

```cpp
	// Cesium backend env var overrides — IonToken is never logged
	Cfg.CesiumBackend.IonPortalUrl = GetEnv(TEXT("CAMSIM_CESIUM_ION_PORTAL_URL"), Cfg.CesiumBackend.IonPortalUrl);
	Cfg.CesiumBackend.IonApiUrl    = GetEnv(TEXT("CAMSIM_CESIUM_ION_API_URL"),    Cfg.CesiumBackend.IonApiUrl);
	Cfg.CesiumBackend.IonToken     = GetEnv(TEXT("CAMSIM_CESIUM_ION_TOKEN"),      Cfg.CesiumBackend.IonToken);
	Cfg.CesiumBackend.Terrain.Source     = GetEnv(TEXT("CAMSIM_CESIUM_TERRAIN_SOURCE"), Cfg.CesiumBackend.Terrain.Source).TrimStartAndEnd().ToLower();
	Cfg.CesiumBackend.Terrain.IonAssetId = GetEnvInt(TEXT("CAMSIM_CESIUM_TERRAIN_ION_ASSET_ID"), Cfg.CesiumBackend.Terrain.IonAssetId);
	Cfg.CesiumBackend.Terrain.Url        = GetEnv(TEXT("CAMSIM_CESIUM_TERRAIN_URL"), Cfg.CesiumBackend.Terrain.Url);
	Cfg.CesiumBackend.Imagery.Source     = GetEnv(TEXT("CAMSIM_CESIUM_IMAGERY_SOURCE"), Cfg.CesiumBackend.Imagery.Source).TrimStartAndEnd().ToLower();
	Cfg.CesiumBackend.Imagery.IonAssetId = GetEnvInt(TEXT("CAMSIM_CESIUM_IMAGERY_ION_ASSET_ID"), Cfg.CesiumBackend.Imagery.IonAssetId);
	Cfg.CesiumBackend.Imagery.WmsUrl     = GetEnv(TEXT("CAMSIM_CESIUM_IMAGERY_WMS_URL"),    Cfg.CesiumBackend.Imagery.WmsUrl);
	Cfg.CesiumBackend.Imagery.WmsLayers  = GetEnv(TEXT("CAMSIM_CESIUM_IMAGERY_WMS_LAYERS"), Cfg.CesiumBackend.Imagery.WmsLayers);
	Cfg.CesiumBackend.Imagery.WmsTileWidth  = GetEnvInt(TEXT("CAMSIM_CESIUM_IMAGERY_WMS_TILE_WIDTH"),  Cfg.CesiumBackend.Imagery.WmsTileWidth);
	Cfg.CesiumBackend.Imagery.WmsTileHeight = GetEnvInt(TEXT("CAMSIM_CESIUM_IMAGERY_WMS_TILE_HEIGHT"), Cfg.CesiumBackend.Imagery.WmsTileHeight);
```

### Task 5: Build and run tests (Chunk 1 verification)

- [ ] **Step 1: Build the project**

```bash
cd /Users/mclayton/developer/camsim/.claude/worktrees/shiny-munching-sketch
scripts/run.sh --build 2>&1 | tail -30
```

Expected: build succeeds (exit 0), no compile errors.

- [ ] **Step 2: Confirm test file is compiled**

No runtime test runner outside UE editor for this project. Verify the test file compiles with the build step. Automation tests run in-editor via `Ctrl+Alt+F11` or the Automation tab. For CI purposes, confirm build passes.

- [ ] **Step 3: Commit Chunk 1**

```bash
git add \
  unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.h \
  unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/CesiumBackendConfigTest.cpp
git commit -m "feat: add FCesiumBackendConfig struct, YAML parsing, and env var overrides"
```

---

## Chunk 2: Runtime Application (Subsystem Storage + Free Function + Camera Wiring)

### Task 6: Add StoreCesiumIonServer to UCamSimSubsystem

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.h:12-19`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.cpp:33-86`

- [ ] **Step 1: Add forward declaration and method to CamSimSubsystem.h**

After the existing forward declarations (after line 19, `class FCamSimParticleManager;`), add:

```cpp
class UCesiumIonServer;
```

After the `GetParticleManager()` accessor (line 59), add:

```cpp
	/** Store the transient UCesiumIonServer created by ApplyCesiumBackendConfig.
	 *  Passing nullptr clears the stored reference (no-op if already null).
	 *  Must be called on the game thread. */
	void StoreCesiumIonServer(UCesiumIonServer* Server);
```

- [ ] **Step 2: Add TStrongObjectPtr to FSubsystemImpl in CamSimSubsystem.cpp**

At the top of `CamSimSubsystem.cpp`, add includes after line 18 (`#include "Misc/Paths.h"`):

```cpp
#include "CesiumIonServer.h"
#include "UObject/StrongObjectPtr.h"
```

Inside `struct UCamSimSubsystem::FSubsystemImpl` (after line 43, `TUniquePtr<FCamSimParticleManager> ParticleManager;`), add:

```cpp
	// Transient UCesiumIonServer created when CesiumBackend config overrides defaults.
	// TStrongObjectPtr prevents GC of this UDataAsset-derived object from a plain C++ struct.
	TStrongObjectPtr<UCesiumIonServer> CesiumIonServerOverride;
```

In `~FSubsystemImpl()` (after line 72, `QueryHandler.Reset();`), add before the other resets:

```cpp
		CesiumIonServerOverride.Reset();
```

- [ ] **Step 3: Implement StoreCesiumIonServer in CamSimSubsystem.cpp**

Add after the closing brace of `UCamSimSubsystem::GetParticleManager()` accessor (find by searching for its definition):

```cpp
void UCamSimSubsystem::StoreCesiumIonServer(UCesiumIonServer* Server)
{
	if (Impl)
	{
		Impl->CesiumIonServerOverride.Reset();
		if (Server)
		{
			Impl->CesiumIonServerOverride = TStrongObjectPtr<UCesiumIonServer>(Server);
		}
	}
}
```

### Task 7: Declare and implement ApplyCesiumBackendConfig free function

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Geospatial/CamSimGeospatialProvider.h`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Geospatial/CamSimGeospatialProvider.cpp`

- [ ] **Step 1: Add forward declarations and function declaration to CamSimGeospatialProvider.h**

After the existing `class UWorld;` forward declaration (line 8), add:

```cpp
class UCesiumIonServer;
```

After the closing brace of `FCamSimGeospatialProvider` class (line 38), add:

```cpp
/**
 * Apply Cesium backend configuration (ion server, terrain source, imagery overlay)
 * to all ACesium3DTileset actors in the world.
 *
 * Must be called on the game thread.
 * Returns the created UCesiumIonServer* (nullptr if ion server step was skipped —
 * i.e. all three ion settings are at their defaults).
 * Caller is responsible for passing the result to UCamSimSubsystem::StoreCesiumIonServer()
 * to prevent GC.
 */
UCesiumIonServer* ApplyCesiumBackendConfig(
	UWorld* World, const FCamSimConfig::FCesiumBackendConfig& Config);
```

- [ ] **Step 2: Implement ApplyCesiumBackendConfig in CamSimGeospatialProvider.cpp**

Add new includes after the existing ones (after line 7, `#include "Engine/World.h"`):

```cpp
#include "Cesium3DTileset.h"
#include "CesiumIonServer.h"
#include "CesiumIonRasterOverlay.h"
#include "CesiumWebMapServiceRasterOverlay.h"
#include "EngineUtils.h"
```

Add the full implementation after the closing brace of `WorldToGeo` (after line 80):

```cpp
UCesiumIonServer* ApplyCesiumBackendConfig(
	UWorld* World, const FCamSimConfig::FCesiumBackendConfig& Config)
{
	if (!World)
	{
		return nullptr;
	}

	// --- Step 1: Ion server override ---
	// Skip if all three ion settings are at their default values.
	const bool bServerIsDefault =
		Config.IonPortalUrl == TEXT("https://ion.cesium.com") &&
		Config.IonApiUrl    == TEXT("https://api.cesium.com") &&
		Config.IonToken.IsEmpty();

	UCesiumIonServer* CustomServer = nullptr;
	if (!bServerIsDefault)
	{
		CustomServer = NewObject<UCesiumIonServer>(GetTransientPackage());
		CustomServer->ServerUrl             = Config.IonPortalUrl;
		CustomServer->ApiUrl                = Config.IonApiUrl;
		CustomServer->DefaultIonAccessToken = Config.IonToken;
		// Note: IonToken is intentionally not logged here or anywhere in this function.
	}

	// --- Steps 2 & 3: Terrain source + imagery overlay per tileset ---
	bool bFoundAnyTileset = false;
	for (TActorIterator<ACesium3DTileset> It(World); It; ++It)
	{
		bFoundAnyTileset = true;
		ACesium3DTileset* Tileset = *It;

		// Apply custom ion server to this tileset
		if (CustomServer)
		{
			Tileset->CesiumIonServer = CustomServer;
		}

		// Apply terrain source
		const FString& TerrainSrc = Config.Terrain.Source;
		if (TerrainSrc == TEXT("cesium_ion"))
		{
			Tileset->TilesetSource = ETilesetSource::FromCesiumIon;
			Tileset->IonAssetID    = Config.Terrain.IonAssetId;
		}
		else if (TerrainSrc == TEXT("url"))
		{
			Tileset->TilesetSource = ETilesetSource::FromUrl;
			Tileset->Url           = Config.Terrain.Url;
		}
		else if (TerrainSrc == TEXT("flat"))
		{
			Tileset->SetActorHiddenInGame(true);
			Tileset->SetActorEnableCollision(false);
		}
		else
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("ApplyCesiumBackendConfig: unknown terrain source '%s', using cesium_ion"),
				*TerrainSrc);
			Tileset->TilesetSource = ETilesetSource::FromCesiumIon;
			Tileset->IonAssetID    = Config.Terrain.IonAssetId;
		}

		// Refresh after direct property assignment (required; setters are not used here
		// to stay consistent with the existing tileset streaming-param loop in CamSimCamera.cpp)
		// Skip refresh for flat — the tileset is just hidden, no tile loading change needed.
		if (TerrainSrc != TEXT("flat"))
		{
			Tileset->RefreshTileset();
		}

		// Remove any existing raster overlay components (always, including flat)
		TArray<UCesiumRasterOverlay*> ExistingOverlays;
		Tileset->GetComponents<UCesiumRasterOverlay>(ExistingOverlays);
		for (UCesiumRasterOverlay* O : ExistingOverlays)
		{
			O->Deactivate();
			O->DestroyComponent();
		}

		// Add new imagery overlay (skip for flat terrain or source: none)
		const bool bTerrainIsFlat  = (TerrainSrc == TEXT("flat"));
		const FString& ImagerySrc  = Config.Imagery.Source;

		if (bTerrainIsFlat)
		{
			UE_LOG(LogCamSim, Log,
				TEXT("ApplyCesiumBackendConfig: imagery source ignored — terrain is flat"));
		}
		else if (ImagerySrc == TEXT("cesium_ion"))
		{
			UCesiumIonRasterOverlay* O =
				NewObject<UCesiumIonRasterOverlay>(Tileset, TEXT("CamSimImagery"));
			O->IonAssetID = Config.Imagery.IonAssetId;
			if (CustomServer) { O->CesiumIonServer = CustomServer; }
			O->RegisterComponent();
			O->Activate(false); // false = activate without resetting internal state
		}
		else if (ImagerySrc == TEXT("wms"))
		{
			UCesiumWebMapServiceRasterOverlay* O =
				NewObject<UCesiumWebMapServiceRasterOverlay>(Tileset, TEXT("CamSimImagery"));
			O->BaseUrl    = Config.Imagery.WmsUrl;
			O->Layers     = Config.Imagery.WmsLayers;
			O->TileWidth  = Config.Imagery.WmsTileWidth;
			O->TileHeight = Config.Imagery.WmsTileHeight;
			O->RegisterComponent();
			O->Activate(false);
		}
		else if (ImagerySrc != TEXT("none"))
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("ApplyCesiumBackendConfig: unknown imagery source '%s', treating as none"),
				*ImagerySrc);
		}
	}

	if (!bFoundAnyTileset && Config.Terrain.Source == TEXT("flat"))
	{
		UE_LOG(LogCamSim, Warning,
			TEXT("ApplyCesiumBackendConfig: terrain source is 'flat' but no ACesium3DTileset actors found in world"));
	}

	return CustomServer;
}
```

### Task 8: Wire up in ACamSimCamera::BeginPlay

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp:178`

- [ ] **Step 1: Add include for CamSimGeospatialProvider.h**

`CamSimCamera.cpp` already includes `"Geospatial/CamSimGeospatialProvider.h"` — confirm this is present. If not, add it with the other includes.

- [ ] **Step 2: Insert call after the tileset streaming-params loop (after line 178)**

After the closing brace of the `for (TActorIterator<ACesium3DTileset> It...` loop (line 178), add:

```cpp
	// Apply Cesium backend config (ion server, terrain source, imagery overlay).
	// Must run after the streaming-params loop above.
	UCesiumIonServer* CesiumServer = ApplyCesiumBackendConfig(GetWorld(), Cfg.CesiumBackend);
	Subsystem->StoreCesiumIonServer(CesiumServer);
```

Also add the include at the top of the file if `"Geospatial/CamSimGeospatialProvider.h"` is not already present:

```cpp
#include "Geospatial/CamSimGeospatialProvider.h"
```

And add the Cesium ion server forward declaration is handled via the include of `CamSimGeospatialProvider.h` (which forward-declares `UCesiumIonServer`).

### Task 9: Build Chunk 2

- [ ] **Step 1: Build the project**

```bash
cd /Users/mclayton/developer/camsim/.claude/worktrees/shiny-munching-sketch
scripts/run.sh --build 2>&1 | tail -40
```

Expected: build succeeds, no compile errors.

**Common compile issues to check:**
- `ETilesetSource::FromCesiumIon` / `FromUrl` — if enum names differ in the installed plugin, check `Cesium3DTileset.h` in the Plugins directory and use the correct enumerator names
- `UCesiumIonServer::ServerUrl` / `ApiUrl` — if field names differ, check `CesiumIonServer.h`
- `UCesiumIonRasterOverlay::IonAssetID`, `CesiumIonServer` — verify in `CesiumIonRasterOverlay.h`
- `UCesiumWebMapServiceRasterOverlay::BaseUrl`, `Layers`, `TileWidth`, `TileHeight` — verify in plugin header
- `ACesium3DTileset::CesiumIonServer` direct assignment — if only a setter exists, use `Tileset->SetCesiumIonServer(CustomServer)`
- `TStrongObjectPtr` — if not found, confirm `#include "UObject/StrongObjectPtr.h"` is present

If any field/method names are wrong, open the corresponding header in:
`unreal_project/CamSimTest/Plugins/CesiumForUnreal/Source/CesiumRuntime/Public/`
and use the correct names.

- [ ] **Step 2: Commit Chunk 2**

```bash
git add \
  unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.h \
  unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Geospatial/CamSimGeospatialProvider.h \
  unreal_project/CamSimTest/Source/CamSimTest/Geospatial/CamSimGeospatialProvider.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp
git commit -m "feat: implement ApplyCesiumBackendConfig and wire up in CamSimCamera"
```

---

## Chunk 3: Config File and Documentation Updates

### Task 10: Add cesium: block to camsim_config.yaml

**Files:**
- Modify: `deploy/camsim_config.yaml`

- [ ] **Step 1: Find the existing geospatial section in camsim_config.yaml**

Look for the `# --- Geospatial (Cesium) ---` comment block (near line 100) which has `terrain_provider`, `imagery_provider`, etc. Add the new `cesium:` block after the existing geospatial parameters.

- [ ] **Step 2: Add the cesium: block**

```yaml
# --- Cesium Backend ---
# Ion server, terrain source, and imagery overlay.
# Set ion_token to authenticate against Cesium ion (https://ion.cesium.com/tokens).
# For air-gapped deployments, set ion_portal_url and ion_api_url to your self-hosted server.

cesium:
  ion_portal_url: "https://ion.cesium.com"   # UCesiumIonServer::ServerUrl (OAuth/browser)
  ion_api_url:    "https://api.cesium.com"    # UCesiumIonServer::ApiUrl (REST tile API)
  ion_token: ""                               # leave empty to use level asset default

  terrain:
    source: cesium_ion    # cesium_ion | url | flat
    ion_asset_id: 1       # Cesium World Terrain (ignored unless source: cesium_ion)
    url: ""               # quantized-mesh endpoint (ignored unless source: url)

  imagery:
    source: cesium_ion    # cesium_ion | wms | none
    ion_asset_id: 2       # Bing Maps Aerial (ignored unless source: cesium_ion)
    wms_url: ""           # ignored unless source: wms
    wms_layers: ""        # ignored unless source: wms
    wms_tile_width: 256
    wms_tile_height: 256
```

### Task 11: Update docs/configuration.md with new env vars

**Files:**
- Modify: `docs/configuration.md`

- [ ] **Step 1: Add CAMSIM_CESIUM_* entries**

Find the Geospatial section in `docs/configuration.md`. Add after the existing geospatial env vars:

```markdown
### Cesium Backend

| Environment Variable | Default | Description |
|---|---|---|
| `CAMSIM_CESIUM_ION_PORTAL_URL` | `https://ion.cesium.com` | Ion portal URL (`UCesiumIonServer::ServerUrl`). Set for self-hosted ion. |
| `CAMSIM_CESIUM_ION_API_URL` | `https://api.cesium.com` | Ion REST API URL (`UCesiumIonServer::ApiUrl`). Set for self-hosted ion. |
| `CAMSIM_CESIUM_ION_TOKEN` | *(empty)* | Ion access token. **Never logged.** Leave empty to use level asset default. |
| `CAMSIM_CESIUM_TERRAIN_SOURCE` | `cesium_ion` | Terrain source: `cesium_ion`, `url`, or `flat`. |
| `CAMSIM_CESIUM_TERRAIN_ION_ASSET_ID` | `1` | Cesium ion asset ID for terrain (Cesium World Terrain = 1). |
| `CAMSIM_CESIUM_TERRAIN_URL` | *(empty)* | Quantized-mesh terrain URL (used when `TERRAIN_SOURCE=url`). |
| `CAMSIM_CESIUM_IMAGERY_SOURCE` | `cesium_ion` | Imagery overlay source: `cesium_ion`, `wms`, or `none`. |
| `CAMSIM_CESIUM_IMAGERY_ION_ASSET_ID` | `2` | Cesium ion asset ID for imagery (Bing Maps Aerial = 2). |
| `CAMSIM_CESIUM_IMAGERY_WMS_URL` | *(empty)* | WMS base URL (used when `IMAGERY_SOURCE=wms`). |
| `CAMSIM_CESIUM_IMAGERY_WMS_LAYERS` | *(empty)* | WMS layer name(s), comma-separated. |
| `CAMSIM_CESIUM_IMAGERY_WMS_TILE_WIDTH` | `256` | WMS tile width in pixels. |
| `CAMSIM_CESIUM_IMAGERY_WMS_TILE_HEIGHT` | `256` | WMS tile height in pixels. |
```

### Task 12: Final build and commit

- [ ] **Step 1: Final build**

```bash
cd /Users/mclayton/developer/camsim/.claude/worktrees/shiny-munching-sketch
scripts/run.sh --build 2>&1 | tail -20
```

Expected: clean build, exit 0.

- [ ] **Step 2: Commit Chunk 3**

```bash
git add deploy/camsim_config.yaml docs/configuration.md
git commit -m "docs: add cesium: block to config YAML and document CAMSIM_CESIUM_* env vars"
```

---

## Verification Checklist

After all chunks are implemented and built:

- [ ] **Default unchanged**: run with no `cesium:` block → tile streaming identical to before (check no new log warnings about Cesium backend)
- [ ] **Token override**: set `CAMSIM_CESIUM_ION_TOKEN=<token>` → tiles load authenticated from Cesium ion
- [ ] **Custom server**: set `CAMSIM_CESIUM_ION_PORTAL_URL` + `CAMSIM_CESIUM_ION_API_URL` → requests go to custom host (verify via network capture or ion server logs)
- [ ] **URL terrain**: `CAMSIM_CESIUM_TERRAIN_SOURCE=url CAMSIM_CESIUM_TERRAIN_URL=<url>` → terrain loads from custom endpoint
- [ ] **Flat terrain**: `CAMSIM_CESIUM_TERRAIN_SOURCE=flat` → no terrain geometry, no tile streaming
- [ ] **WMS imagery**: `CAMSIM_CESIUM_IMAGERY_SOURCE=wms CAMSIM_CESIUM_IMAGERY_WMS_URL=<url> CAMSIM_CESIUM_IMAGERY_WMS_LAYERS=<layer>` → custom imagery over terrain
- [ ] **No imagery**: `CAMSIM_CESIUM_IMAGERY_SOURCE=none` → terrain renders without overlay
- [ ] **Invalid source**: `CAMSIM_CESIUM_TERRAIN_SOURCE=typo` → warning in log, safe fallback to cesium_ion, no crash
- [ ] **Token not in logs**: `CAMSIM_CESIUM_ION_TOKEN=mysecrettoken scripts/run.sh` then `grep mysecrettoken` in log output → zero matches
