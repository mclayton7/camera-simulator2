# Offline Scene Profile Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A CamSim run with `scene.dir` + `scene.offline: true` renders a scene package and makes no outbound request (closes REALISM R0 gate 5).

**Architecture:** Config gains `scene.dir` / `scene.offline` and `cesium.imagery.url`; a pure `CamSimScene` module resolves the package into terrain/imagery/land-cover settings at load time and adds offline validation. Cesium setup (origin-shift mobility, tuning, backend) moves from `ACamSimCamera::BeginPlay` to `ACamSimGameMode::StartPlay`, which runs before any actor's `BeginPlay`, so the level's ion tilesets never call `LoadTileset`. A new `tms` imagery source creates `UCesiumTileMapServiceRasterOverlay`.

**Tech Stack:** UE 5.8 C++ (CamSimTest module), Cesium for Unreal 2.29.1, UE Automation tests, rapidyaml config, Python gate tools (`scripts/scene/tools/`).

**Spec:** `docs/superpowers/specs/2026-10-09-offline-scene-profile-design.md`

## Global Constraints

- UE naming (`F`/`A`/`U`), PascalCase, verb-first functions; `#include "CoreMinimal.h"` first in new headers; copyright line `// Copyright CamSim Contributors. All Rights Reserved.` on every new source file.
- `file://` URLs must have three slashes and an absolute, percent-encoded path (Cesium's `UnrealAssetAccessor` only routes the literal prefix `file:///`; spaces must be `%20`).
- TMS `url` names `tilemapresource.xml` itself (a directory URL fails with "Is a directory").
- Cesium coordinates/behaviour are untouched; "one terrain tileset" stays: every other `ACesium3DTileset` is destroyed.
- Online behaviour without `scene.*` is unchanged except that the terrain tileset no longer loads twice.
- Every new config key is documented in `docs/configuration.md` and `deploy/camsim_config.yaml` with its `CAMSIM_*` env var.
- `manifest.json` `schema_version` supported: exactly `1`.

## Build and test commands (macOS, used by every task)

```bash
# build (incremental ~10 s; exit code is UBT's)
scripts/run.sh --build-only

# headless automation tests, filtered (replace the filter per task)
UE_BIN="/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd"
"$UE_BIN" "$PWD/unreal_project/CamSimTest/CamSimTest.uproject" \
  -ExecCmds="Automation RunTests CamSim.Scene+Quit" -TestExit="Automation Test Queue Empty" \
  -ReportExportPath="$PWD/.cache/automation-report" \
  -unattended -nullrhi -nosound -nosplash -DisablePython -log -stdout -FullStdOutLogOutput > .cache/automation.log 2>&1
python3 scripts/parse_automation_report.py .cache/automation-report/index.json
```

Run from the repo root. `-DisablePython` is required on macOS under `-nullrhi`.

## Review Focus

- **`scene.dir` with a trailing slash or `~`** — `"/pkg/"` must resolve like `"/pkg"`; `~/pkg` is relative (no shell expansion) and must give a clear "must be an absolute path" error, not a silent fallback to ion. Test: `CamSim.Scene.Resolve.NotAPackage` (relative) and `.FullPackage` (trailing slash).
- **Non-ASCII / reserved characters in the package path** (`+`, `#`, `%`, `é`, space) — the generated URL must decode back to the same path, or Cesium 404s every tile. Test: `CamSim.Scene.FileUrl.RoundTrip`.
- **YAML source spelled in upper case** (`source: TMS`) — env values are lower-cased but YAML values are not, so `TMS` would be "unknown imagery source". `scene.offline` must not accept what the backend then ignores; Validate compares the same exact strings the backend uses, so `TMS` is an offline error. Test: `CamSim.Scene.Offline.RejectsNetworkSources` (case `TMS`).
- **`scene.offline` set with the default (ion) config** — the commonest mistake; must exit non-zero listing both terrain and imagery reasons, not render ion. Test: `CamSim.Scene.Offline.RejectsNetworkSources` (default config gives exactly 2 errors) + acceptance step in Task 5.
- **`tms` without a URL in an online run** — must be a validation error, not an overlay with an empty URL. Test: `CamSim.Scene.Offline.TmsNeedsUrl`.

---

### Task 1: Config keys — `scene.*` and `cesium.imagery.url`

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.h` (imagery struct ~line 405; add `FSceneConfig` after `CesiumBackend`, ~line 420)
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.cpp` (YAML `cesium` block ~line 787; env ~line 1116)
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Tests/SceneConfigTest.cpp`

**Interfaces:**
- Produces: `FCamSimConfig::FSceneConfig Scene` with `FString Dir`, `bool bOffline`, `FString PackageName`, `TArray<FString> ResolveErrors`, `TArray<FString> ResolveWarnings`; `FCamSimConfig::FCesiumBackendConfig::FImageryConfig::Url` (FString).

- [ ] **Step 1: Write the failing test**

Create `Tests/SceneConfigTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"

// REALISM R0 offline profile: scene.* and cesium.imagery.url parse from YAML and env.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneConfigParseTest,
	"CamSim.Scene.Config.Parse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneConfigParseTest::RunTest(const FString& Parameters)
{
	{
		const FCamSimConfig Cfg;
		TestTrue(TEXT("scene.dir empty by default"), Cfg.Scene.Dir.IsEmpty());
		TestFalse(TEXT("scene.offline off by default"), Cfg.Scene.bOffline);
		TestTrue(TEXT("imagery.url empty by default"), Cfg.CesiumBackend.Imagery.Url.IsEmpty());
	}
	{
		const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT(
			"scene:\n"
			"  offline: true\n"
			"cesium:\n"
			"  imagery:\n"
			"    source: tms\n"
			"    url: \"file:///data/pkg/imagery/tilemapresource.xml\"\n"));
		TestTrue(TEXT("yaml scene.offline"), Cfg.Scene.bOffline);
		TestEqual(TEXT("yaml imagery.source"), Cfg.CesiumBackend.Imagery.Source, FString(TEXT("tms")));
		TestEqual(TEXT("yaml imagery.url"), Cfg.CesiumBackend.Imagery.Url,
			FString(TEXT("file:///data/pkg/imagery/tilemapresource.xml")));
		TestEqual(TEXT("no unknown keys"), Cfg.UnknownYamlKeys.Num(), 0);
	}
	{
		ON_SCOPE_EXIT
		{
			FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_SCENE_OFFLINE"), TEXT(""));
			FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_CESIUM_IMAGERY_URL"), TEXT(""));
		};
		FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_SCENE_OFFLINE"), TEXT("true"));
		FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_CESIUM_IMAGERY_URL"), TEXT("file:///x/tilemapresource.xml"));
		const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT("{}"));
		TestTrue(TEXT("env scene.offline"), Cfg.Scene.bOffline);
		TestEqual(TEXT("env imagery.url"), Cfg.CesiumBackend.Imagery.Url, FString(TEXT("file:///x/tilemapresource.xml")));
	}
	return true;
}
```

(`CAMSIM_SCENE_DIR` is tested in Task 2, where resolution needs a real package on disk.)

- [ ] **Step 2: Build to verify it fails**

Run: `scripts/run.sh --build-only`
Expected: compile error, `no member named 'Scene' in 'FCamSimConfig'` / `no member named 'Url'`.

- [ ] **Step 3: Implement**

In `CamSimConfig.h`, inside `FImageryConfig`, after `WmsTileHeight`:

```cpp
			// source: tms — URL of the pyramid's tilemapresource.xml (the file itself, not its
			// directory), e.g. file:///data/pkg/imagery/tilemapresource.xml. Env: CAMSIM_CESIUM_IMAGERY_URL
			FString Url           = TEXT("");
```

and change the `Source` comment to `// "cesium_ion" | "wms" | "tms" | "none"`.

After `} CesiumBackend;`:

```cpp
	/** Scene package (REALISM R0, docs/scene-packages.md). */
	struct FSceneConfig
	{
		// Absolute path to a scene package (manifest.json + terrain/ imagery/ landcover/).
		// Its layers replace cesium.terrain, cesium.imagery and thermal.land_cover.dir. Env: CAMSIM_SCENE_DIR
		FString Dir = TEXT("");
		// No network: terrain and imagery must be local file:/// sources, and any config error
		// exits at startup instead of running on defaults. Env: CAMSIM_SCENE_OFFLINE
		bool bOffline = false;

		// Filled by CamSimScene::ResolvePackage at load (not config keys).
		FString PackageName;
		TArray<FString> ResolveErrors;
		TArray<FString> ResolveWarnings;
	} Scene;
```

In `CamSimConfig.cpp`, in the `imagery` YAML block after `wms_tile_height`:

```cpp
				YamlString(Im, "url",             Cfg.CesiumBackend.Imagery.Url);
```

After the whole `cesium` block (before `// Phase 21: DIS protocol config`):

```cpp
		// REALISM R0: scene package
		if (YamlHas(Root, "scene"))
		{
			ryml::ConstNodeRef Sc = Root["scene"];
			YamlString(Sc, "dir",     Cfg.Scene.Dir);
			YamlBool  (Sc, "offline", Cfg.Scene.bOffline);
		}
```

In `ApplyEnvOverrides`, after the `CAMSIM_CESIUM_IMAGERY_WMS_TILE_HEIGHT` line:

```cpp
	Cfg.CesiumBackend.Imagery.Url        = GetEnv(TEXT("CAMSIM_CESIUM_IMAGERY_URL"), Cfg.CesiumBackend.Imagery.Url);
	Cfg.Scene.Dir      = GetEnv    (TEXT("CAMSIM_SCENE_DIR"),     Cfg.Scene.Dir);
	Cfg.Scene.bOffline = GetEnvBool(TEXT("CAMSIM_SCENE_OFFLINE"), Cfg.Scene.bOffline);
```

- [ ] **Step 4: Build and run the test**

Run: `scripts/run.sh --build-only`, then the test command with filter `CamSim.Scene.Config`.
Expected: `CamSim.Scene.Config.Parse` passes.

- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.h \
        unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/Tests/SceneConfigTest.cpp
git commit -m "feat(config): scene.dir / scene.offline and cesium.imagery.url keys"
```

---

### Task 2: Package resolution and `file:///` URLs (`CamSimScene`)

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Config/ScenePackage.h`
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Config/ScenePackage.cpp`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.cpp` (`LoadFromYaml`: both `ApplyEnvOverrides(Cfg); return Cfg;` sites, ~lines 320 and 918)
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Tests/ScenePackageTest.cpp`

**Interfaces:**
- Consumes: `FCamSimConfig::Scene` (Task 1), `FCesiumBackendConfig::Imagery.Url` (Task 1).
- Produces:
  - `FString CamSimScene::PathToFileUrl(const FString& AbsolutePath)`
  - `bool CamSimScene::FileUrlToPath(const FString& Url, FString& OutPath)`
  - `void CamSimScene::ResolvePackage(FCamSimConfig& Cfg)` — applied automatically by `FCamSimConfig::Load*`
  - `constexpr int32 CamSimScene::SupportedSchemaVersion = 1`

- [ ] **Step 1: Write the failing tests**

Create `Tests/ScenePackageTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Config/CamSimConfig.h"
#include "Config/ScenePackage.h"

namespace
{
	/** An empty directory under Saved/Automation/Scene (absolute). */
	FString SceneTempDir(const TCHAR* Name)
	{
		const FString D = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("Scene"), Name));
		IFileManager::Get().DeleteDirectory(*D, false, true);
		IFileManager::Get().MakeDirectory(*D, true);
		return D;
	}

	void WriteFile(const FString& Path, const FString& Text)
	{
		FFileHelper::SaveStringToFile(Text, *Path);
	}

	/** A package with the given layers' entry files (contents are never parsed by CamSim). */
	FString MakePackage(const TCHAR* Name, bool bTerrain, bool bImagery, bool bLandCover,
		const FString& Manifest = TEXT("{\"schema_version\": 1, \"name\": \"testpkg\"}"))
	{
		const FString D = SceneTempDir(Name);
		WriteFile(D / TEXT("manifest.json"), Manifest);
		if (bTerrain)   WriteFile(D / TEXT("terrain/layer.json"), TEXT("{}"));
		if (bImagery)   WriteFile(D / TEXT("imagery/tilemapresource.xml"), TEXT("<TileMap/>"));
		if (bLandCover) WriteFile(D / TEXT("landcover/index.json"), TEXT("{}"));
		return D;
	}

	FCamSimConfig ResolvedConfigFor(const FString& Dir)
	{
		FCamSimConfig Cfg;
		Cfg.Scene.Dir = Dir;
		CamSimScene::ResolvePackage(Cfg);
		return Cfg;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneFileUrlRoundTripTest,
	"CamSim.Scene.FileUrl.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneFileUrlRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace CamSimScene;
	TestEqual(TEXT("plain"), PathToFileUrl(TEXT("/data/pkg/terrain/layer.json")),
		FString(TEXT("file:///data/pkg/terrain/layer.json")));
	TestEqual(TEXT("space, plus, hash, percent"), PathToFileUrl(TEXT("/a b/c+d#e%f")),
		FString(TEXT("file:///a%20b/c%2Bd%23e%25f")));
	TestEqual(TEXT("UTF-8"), PathToFileUrl(TEXT("/caf\u00e9")), FString(TEXT("file:///caf%C3%A9")));
	TestEqual(TEXT("Windows drive"), PathToFileUrl(TEXT("C:\\x y\\l.json")), FString(TEXT("file:///C:/x%20y/l.json")));

	for (const TCHAR* P : { TEXT("/data/pkg/terrain/layer.json"), TEXT("/a b/c+d#e%f"), TEXT("/caf\u00e9"), TEXT("C:/x y/l.json") })
	{
		FString Back;
		TestTrue(FString::Printf(TEXT("decodes %s"), P), FileUrlToPath(PathToFileUrl(P), Back));
		TestEqual(FString::Printf(TEXT("round trip %s"), P), Back, FString(P));
	}

	FString Ignored;
	TestFalse(TEXT("https is not a file URL"), FileUrlToPath(TEXT("https://api.cesium.com/x"), Ignored));
	TestFalse(TEXT("file://host/ is not routed by Cesium"), FileUrlToPath(TEXT("file://host/x"), Ignored));
	TestFalse(TEXT("empty"), FileUrlToPath(TEXT(""), Ignored));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneResolveFullPackageTest,
	"CamSim.Scene.Resolve.FullPackage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneResolveFullPackageTest::RunTest(const FString& Parameters)
{
	const FString D = MakePackage(TEXT("full"), true, true, true);
	const FCamSimConfig Cfg = ResolvedConfigFor(D + TEXT("/"));   // trailing slash is accepted
	TestEqual(TEXT("no errors"), Cfg.Scene.ResolveErrors.Num(), 0);
	TestEqual(TEXT("no warnings"), Cfg.Scene.ResolveWarnings.Num(), 0);
	TestEqual(TEXT("package name"), Cfg.Scene.PackageName, FString(TEXT("testpkg")));
	TestEqual(TEXT("terrain source"), Cfg.CesiumBackend.Terrain.Source, FString(TEXT("url")));
	TestEqual(TEXT("terrain url"), Cfg.CesiumBackend.Terrain.Url,
		CamSimScene::PathToFileUrl(D / TEXT("terrain/layer.json")));
	TestEqual(TEXT("imagery source"), Cfg.CesiumBackend.Imagery.Source, FString(TEXT("tms")));
	TestEqual(TEXT("imagery url"), Cfg.CesiumBackend.Imagery.Url,
		CamSimScene::PathToFileUrl(D / TEXT("imagery/tilemapresource.xml")));
	TestEqual(TEXT("land cover dir"), Cfg.Thermal.LandCover.Dir, D / TEXT("landcover"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneResolveMissingLayersTest,
	"CamSim.Scene.Resolve.MissingLayers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneResolveMissingLayersTest::RunTest(const FString& Parameters)
{
	const FCamSimConfig Defaults;
	{
		const FCamSimConfig Cfg = ResolvedConfigFor(MakePackage(TEXT("terrain_only"), true, false, false));
		TestEqual(TEXT("terrain from package"), Cfg.CesiumBackend.Terrain.Source, FString(TEXT("url")));
		TestEqual(TEXT("imagery unchanged"), Cfg.CesiumBackend.Imagery.Source, Defaults.CesiumBackend.Imagery.Source);
		TestEqual(TEXT("land cover unchanged"), Cfg.Thermal.LandCover.Dir, Defaults.Thermal.LandCover.Dir);
	}
	{
		const FCamSimConfig Cfg = ResolvedConfigFor(MakePackage(TEXT("imagery_only"), false, true, false));
		TestEqual(TEXT("terrain unchanged"), Cfg.CesiumBackend.Terrain.Source, Defaults.CesiumBackend.Terrain.Source);
		TestEqual(TEXT("imagery from package"), Cfg.CesiumBackend.Imagery.Source, FString(TEXT("tms")));
	}
	{
		const FCamSimConfig Cfg = ResolvedConfigFor(MakePackage(TEXT("landcover_only"), false, false, true));
		TestEqual(TEXT("terrain unchanged"), Cfg.CesiumBackend.Terrain.Source, Defaults.CesiumBackend.Terrain.Source);
		TestTrue(TEXT("land cover from package"), Cfg.Thermal.LandCover.Dir.EndsWith(TEXT("landcover_only/landcover")));
		TestEqual(TEXT("no errors"), Cfg.Scene.ResolveErrors.Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneResolveNotAPackageTest,
	"CamSim.Scene.Resolve.NotAPackage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneResolveNotAPackageTest::RunTest(const FString& Parameters)
{
	const FCamSimConfig Defaults;
	auto ExpectError = [&](const TCHAR* What, const FString& Dir, const TCHAR* Needle)
	{
		const FCamSimConfig Cfg = ResolvedConfigFor(Dir);
		TestEqual(FString::Printf(TEXT("%s: one error"), What), Cfg.Scene.ResolveErrors.Num(), 1);
		if (Cfg.Scene.ResolveErrors.Num() == 1)
		{
			TestTrue(FString::Printf(TEXT("%s: message mentions '%s' (%s)"), What, Needle, *Cfg.Scene.ResolveErrors[0]),
				Cfg.Scene.ResolveErrors[0].Contains(Needle));
		}
		TestEqual(FString::Printf(TEXT("%s: terrain untouched"), What),
			Cfg.CesiumBackend.Terrain.Source, Defaults.CesiumBackend.Terrain.Source);
	};

	ExpectError(TEXT("relative"), TEXT("pkg/pendleton"), TEXT("absolute"));
	ExpectError(TEXT("tilde"), TEXT("~/pkg"), TEXT("absolute"));
	ExpectError(TEXT("no manifest"), SceneTempDir(TEXT("no_manifest")), TEXT("manifest.json"));
	ExpectError(TEXT("bad json"), MakePackage(TEXT("bad_json"), true, true, true, TEXT("{not json")), TEXT("JSON"));
	ExpectError(TEXT("schema 2"), MakePackage(TEXT("schema2"), true, true, true, TEXT("{\"schema_version\": 2}")),
		TEXT("schema_version"));
	ExpectError(TEXT("no schema"), MakePackage(TEXT("noschema"), true, true, true, TEXT("{\"name\": \"x\"}")),
		TEXT("schema_version"));

	const FCamSimConfig Empty = ResolvedConfigFor(TEXT(""));
	TestEqual(TEXT("empty dir: no-op"), Empty.Scene.ResolveErrors.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneResolveOverrideWarningTest,
	"CamSim.Scene.Resolve.OverrideWarning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneResolveOverrideWarningTest::RunTest(const FString& Parameters)
{
	const FString D = MakePackage(TEXT("override"), true, true, false);
	FCamSimConfig Cfg;
	Cfg.Scene.Dir = D;
	Cfg.CesiumBackend.Terrain.Source = TEXT("url");
	Cfg.CesiumBackend.Terrain.Url = TEXT("https://example.com/layer.json");
	CamSimScene::ResolvePackage(Cfg);
	TestEqual(TEXT("one warning (terrain)"), Cfg.Scene.ResolveWarnings.Num(), 1);
	if (Cfg.Scene.ResolveWarnings.Num() == 1)
	{
		TestTrue(TEXT("names the key"), Cfg.Scene.ResolveWarnings[0].Contains(TEXT("cesium.terrain")));
	}
	TestEqual(TEXT("package wins"), Cfg.CesiumBackend.Terrain.Url, CamSimScene::PathToFileUrl(D / TEXT("terrain/layer.json")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneResolveAtLoadTest,
	"CamSim.Scene.Resolve.AtLoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneResolveAtLoadTest::RunTest(const FString& Parameters)
{
	const FString D = MakePackage(TEXT("at load"), true, true, true);   // a space in the path
	{
		const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(FString::Printf(TEXT("scene:\n  dir: \"%s\"\n"), *D));
		TestEqual(TEXT("yaml: terrain url"), Cfg.CesiumBackend.Terrain.Url, CamSimScene::PathToFileUrl(D / TEXT("terrain/layer.json")));
		TestTrue(TEXT("yaml: space encoded"), Cfg.CesiumBackend.Terrain.Url.Contains(TEXT("at%20load")));
	}
	{
		ON_SCOPE_EXIT { FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_SCENE_DIR"), TEXT("")); };
		FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_SCENE_DIR"), *D);
		const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT("{}"));
		TestEqual(TEXT("env: imagery source"), Cfg.CesiumBackend.Imagery.Source, FString(TEXT("tms")));
		TestEqual(TEXT("env: package name"), Cfg.Scene.PackageName, FString(TEXT("testpkg")));
	}
	return true;
}
```

- [ ] **Step 2: Build to verify it fails**

Run: `scripts/run.sh --build-only`
Expected: compile error, `'Config/ScenePackage.h' file not found`.

- [ ] **Step 3: Implement `ScenePackage.h`**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FCamSimConfig;

/**
 * Scene packages (REALISM R0, docs/scene-packages.md) and the local file URLs Cesium reads them through.
 */
namespace CamSimScene
{
	/** manifest.json schema_version this build reads. */
	constexpr int32 SupportedSchemaVersion = 1;

	/**
	 * Absolute path -> file:/// URL. Backslashes become '/', and every byte outside
	 * [A-Za-z0-9-._~/:] is percent-encoded (UTF-8), as Cesium's UnrealAssetAccessor decodes it.
	 */
	FString PathToFileUrl(const FString& AbsolutePath);

	/**
	 * file:/// URL -> native path (percent-decoded; "/C:/x" -> "C:/x"). False for anything
	 * that doesn't start with "file:///" (Cesium sends file://host/ and http(s) to the network).
	 */
	bool FileUrlToPath(const FString& Url, FString& OutPath);

	/**
	 * Apply scene.dir to Cfg: each layer the package has (terrain/layer.json,
	 * imagery/tilemapresource.xml, landcover/index.json) replaces cesium.terrain, cesium.imagery
	 * and thermal.land_cover.dir. Fills Cfg.Scene.PackageName / ResolveErrors / ResolveWarnings
	 * (Validate() and ValidateWarnings() report them). No-op when scene.dir is empty.
	 */
	void ResolvePackage(FCamSimConfig& Cfg);
}
```

- [ ] **Step 4: Implement `ScenePackage.cpp`**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Config/ScenePackage.h"
#include "Config/CamSimConfig.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace CamSimScene
{
namespace
{
	bool IsUnreserved(uint8 C)
	{
		return (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9')
			|| C == '-' || C == '.' || C == '_' || C == '~' || C == '/' || C == ':';
	}

	int32 HexValue(ANSICHAR C)
	{
		if (C >= '0' && C <= '9') return C - '0';
		if (C >= 'a' && C <= 'f') return C - 'a' + 10;
		if (C >= 'A' && C <= 'F') return C - 'A' + 10;
		return -1;
	}
}

FString PathToFileUrl(const FString& AbsolutePath)
{
	const FString Path = AbsolutePath.Replace(TEXT("\\"), TEXT("/"));
	FString Out = Path.StartsWith(TEXT("/")) ? TEXT("file://") : TEXT("file:///");
	const FTCHARToUTF8 Utf8(*Path);
	for (int32 i = 0; i < Utf8.Length(); ++i)
	{
		const uint8 C = static_cast<uint8>(Utf8.Get()[i]);
		if (IsUnreserved(C))
		{
			Out.AppendChar(static_cast<TCHAR>(C));
		}
		else
		{
			Out += FString::Printf(TEXT("%%%02X"), C);
		}
	}
	return Out;
}

bool FileUrlToPath(const FString& Url, FString& OutPath)
{
	static const FString Prefix = TEXT("file:///");
	if (!Url.StartsWith(Prefix, ESearchCase::IgnoreCase))
	{
		return false;
	}
	const FTCHARToUTF8 Utf8(*Url.RightChop(Prefix.Len() - 1));   // keep the path's leading '/'
	const ANSICHAR* S = Utf8.Get();
	const int32 N = Utf8.Length();
	TArray<ANSICHAR> Bytes;
	Bytes.Reserve(N + 1);
	for (int32 i = 0; i < N; ++i)
	{
		const int32 Hi = (S[i] == '%' && i + 2 < N) ? HexValue(S[i + 1]) : -1;
		const int32 Lo = Hi >= 0 ? HexValue(S[i + 2]) : -1;
		if (Lo >= 0)
		{
			Bytes.Add(static_cast<ANSICHAR>(Hi * 16 + Lo));
			i += 2;
		}
		else
		{
			Bytes.Add(S[i]);
		}
	}
	Bytes.Add('\0');
	OutPath = UTF8_TO_TCHAR(Bytes.GetData());
	// "/C:/x" -> "C:/x"
	if (OutPath.Len() >= 3 && OutPath[0] == TEXT('/') && FChar::IsAlpha(OutPath[1]) && OutPath[2] == TEXT(':'))
	{
		OutPath.RightChopInline(1);
	}
	return true;
}

void ResolvePackage(FCamSimConfig& Cfg)
{
	FCamSimConfig::FSceneConfig& Scene = Cfg.Scene;
	Scene.PackageName.Reset();
	Scene.ResolveErrors.Reset();
	Scene.ResolveWarnings.Reset();

	FString Dir = Scene.Dir.TrimStartAndEnd();
	if (Dir.IsEmpty())
	{
		return;
	}
	FPaths::NormalizeDirectoryName(Dir);   // '/' separators, no trailing slash
	if (FPaths::IsRelative(Dir))
	{
		Scene.ResolveErrors.Add(FString::Printf(TEXT("scene.dir '%s' must be an absolute path"), *Scene.Dir));
		return;
	}

	FString ManifestText;
	if (!FFileHelper::LoadFileToString(ManifestText, *(Dir / TEXT("manifest.json"))))
	{
		Scene.ResolveErrors.Add(FString::Printf(TEXT("scene.dir '%s' has no manifest.json (not a scene package)"), *Dir));
		return;
	}
	TSharedPtr<FJsonObject> Manifest;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ManifestText), Manifest) || !Manifest.IsValid())
	{
		Scene.ResolveErrors.Add(FString::Printf(TEXT("scene.dir '%s': manifest.json is not valid JSON"), *Dir));
		return;
	}
	int32 Schema = 0;
	if (!Manifest->TryGetNumberField(TEXT("schema_version"), Schema) || Schema != SupportedSchemaVersion)
	{
		Scene.ResolveErrors.Add(FString::Printf(
			TEXT("scene.dir '%s': manifest.json schema_version %d is not supported (this build reads %d)"),
			*Dir, Schema, SupportedSchemaVersion));
		return;
	}
	Manifest->TryGetStringField(TEXT("name"), Scene.PackageName);

	const FCamSimConfig Defaults;
	FCamSimConfig::FCesiumBackendConfig& Cs = Cfg.CesiumBackend;

	const FString TerrainFile = Dir / TEXT("terrain/layer.json");
	if (FPaths::FileExists(TerrainFile))
	{
		if (Cs.Terrain.Source != Defaults.CesiumBackend.Terrain.Source || !Cs.Terrain.Url.IsEmpty())
		{
			Scene.ResolveWarnings.Add(FString::Printf(
				TEXT("scene.dir overrides cesium.terrain (source '%s', url '%s')"), *Cs.Terrain.Source, *Cs.Terrain.Url));
		}
		Cs.Terrain.Source = TEXT("url");
		Cs.Terrain.Url    = PathToFileUrl(TerrainFile);
	}

	const FString ImageryFile = Dir / TEXT("imagery/tilemapresource.xml");
	if (FPaths::FileExists(ImageryFile))
	{
		if (Cs.Imagery.Source != Defaults.CesiumBackend.Imagery.Source || !Cs.Imagery.Url.IsEmpty())
		{
			Scene.ResolveWarnings.Add(FString::Printf(
				TEXT("scene.dir overrides cesium.imagery (source '%s', url '%s')"), *Cs.Imagery.Source, *Cs.Imagery.Url));
		}
		Cs.Imagery.Source = TEXT("tms");
		Cs.Imagery.Url    = PathToFileUrl(ImageryFile);
	}

	if (FPaths::FileExists(Dir / TEXT("landcover/index.json")))
	{
		if (Cfg.Thermal.LandCover.Dir != Defaults.Thermal.LandCover.Dir)
		{
			Scene.ResolveWarnings.Add(FString::Printf(
				TEXT("scene.dir overrides thermal.land_cover.dir ('%s')"), *Cfg.Thermal.LandCover.Dir));
		}
		Cfg.Thermal.LandCover.Dir = Dir / TEXT("landcover");
	}
}
} // namespace CamSimScene
```

Note: `TryGetNumberField(..., int32&)` fails for a missing field, leaving `Schema` at 0, so "no schema" reports `schema_version 0`.

- [ ] **Step 5: Resolve at load**

In `CamSimConfig.cpp` add `#include "Config/ScenePackage.h"`. At both places in `LoadFromYaml` that do `ApplyEnvOverrides(Cfg);` followed by `return Cfg;` (the YAML parse-failure path and the end of the function), insert between them:

```cpp
	CamSimScene::ResolvePackage(Cfg);   // after env: CAMSIM_SCENE_DIR may set it
```

In `FCamSimConfig::Validate()`, before its final `return Errors;`:

```cpp
	Errors.Append(Scene.ResolveErrors);
```

In `FCamSimConfig::ValidateWarnings()`, before its final `return Warnings;`:

```cpp
	Warnings.Append(Scene.ResolveWarnings);
```

- [ ] **Step 6: Build and run the tests**

Run: `scripts/run.sh --build-only`, then the test command with filter `CamSim.Scene`.
Expected: `FileUrl.RoundTrip`, `Resolve.FullPackage`, `Resolve.MissingLayers`, `Resolve.NotAPackage`, `Resolve.OverrideWarning`, `Resolve.AtLoad`, `Config.Parse` all pass. Then run the full suite (filter `CamSim`) once: no regressions (`ConfigUnknownKeys` must not report `scene`).

- [ ] **Step 7: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Config/ScenePackage.h \
        unreal_project/CamSimTest/Source/CamSimTest/Config/ScenePackage.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/Tests/ScenePackageTest.cpp
git commit -m "feat(scene): resolve scene.dir into terrain, imagery and land cover"
```

---

### Task 3: Offline validation and fail-fast exit

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Config/ScenePackage.h` / `.cpp` (add `ValidateSources`)
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.cpp` (`Validate()`)
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.cpp` (validation block in `Initialize`, ~line 343)
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Tests/ScenePackageTest.cpp`

**Interfaces:**
- Consumes: `FileUrlToPath`, `ResolvePackage`, `FSceneConfig` (Tasks 1–2).
- Produces: `TArray<FString> CamSimScene::ValidateSources(const FCamSimConfig& Cfg)` — `tms` needs a URL (always); `scene.offline` rules (only when offline).

- [ ] **Step 1: Write the failing tests** (append to `ScenePackageTest.cpp`)

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneOfflineRejectsNetworkTest,
	"CamSim.Scene.Offline.RejectsNetworkSources",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneOfflineRejectsNetworkTest::RunTest(const FString& Parameters)
{
	using CamSimScene::ValidateSources;
	{
		FCamSimConfig Cfg;   // ion terrain + ion imagery
		TestEqual(TEXT("online default: no errors"), ValidateSources(Cfg).Num(), 0);
		Cfg.Scene.bOffline = true;
		const TArray<FString> E = ValidateSources(Cfg);
		TestEqual(TEXT("offline default: terrain + imagery"), E.Num(), 2);
		TestTrue(TEXT("names terrain"), E.Num() == 2 && E[0].Contains(TEXT("cesium.terrain.source")));
		TestTrue(TEXT("names imagery"), E.Num() == 2 && E[1].Contains(TEXT("cesium.imagery.source")));
	}
	const FString D = MakePackage(TEXT("offline_ok"), true, true, false);
	auto Offline = [&](TFunctionRef<void(FCamSimConfig&)> Edit)
	{
		FCamSimConfig Cfg = ResolvedConfigFor(D);
		Cfg.Scene.bOffline = true;
		Edit(Cfg);
		return ValidateSources(Cfg);
	};
	TestEqual(TEXT("package: ok"), Offline([](FCamSimConfig&) {}).Num(), 0);
	TestEqual(TEXT("flat + none: ok"), Offline([](FCamSimConfig& C) {
		C.CesiumBackend.Terrain.Source = TEXT("flat"); C.CesiumBackend.Imagery.Source = TEXT("none"); }).Num(), 0);
	TestEqual(TEXT("wms rejected"), Offline([](FCamSimConfig& C) { C.CesiumBackend.Imagery.Source = TEXT("wms"); }).Num(), 1);
	TestEqual(TEXT("upper-case TMS rejected (backend ignores it)"),
		Offline([](FCamSimConfig& C) { C.CesiumBackend.Imagery.Source = TEXT("TMS"); }).Num(), 1);
	TestEqual(TEXT("https terrain rejected"), Offline([](FCamSimConfig& C) {
		C.CesiumBackend.Terrain.Url = TEXT("https://example.com/layer.json"); }).Num(), 1);
	TestEqual(TEXT("file://host rejected"), Offline([](FCamSimConfig& C) {
		C.CesiumBackend.Terrain.Url = TEXT("file://host/layer.json"); }).Num(), 1);
	{
		const TArray<FString> E = Offline([&](FCamSimConfig& C) {
			C.CesiumBackend.Imagery.Url = CamSimScene::PathToFileUrl(D / TEXT("imagery/missing.xml")); });
		TestEqual(TEXT("missing file rejected"), E.Num(), 1);
		TestTrue(TEXT("says it does not exist"), E.Num() == 1 && E[0].Contains(TEXT("does not exist")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneTmsNeedsUrlTest,
	"CamSim.Scene.Offline.TmsNeedsUrl",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneTmsNeedsUrlTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	Cfg.CesiumBackend.Imagery.Source = TEXT("tms");
	const TArray<FString> E = Cfg.Validate();
	TestTrue(TEXT("online tms without url is a Validate() error"),
		E.ContainsByPredicate([](const FString& S) { return S.Contains(TEXT("cesium.imagery.url")); }));
	return true;
}
```

- [ ] **Step 2: Build to verify it fails**

Run: `scripts/run.sh --build-only`
Expected: compile error, `no member named 'ValidateSources' in namespace 'CamSimScene'`.

- [ ] **Step 3: Implement `ValidateSources`**

In `ScenePackage.h`, inside the namespace:

```cpp
	/**
	 * Terrain/imagery source checks: tms needs cesium.imagery.url; with scene.offline, terrain
	 * must be url (an existing file:/// target) or flat and imagery tms (an existing file:///
	 * target) or none. Compares the exact strings ApplyCesiumBackendConfig dispatches on.
	 */
	TArray<FString> ValidateSources(const FCamSimConfig& Cfg);
```

In `ScenePackage.cpp`, inside the namespace after `ResolvePackage`:

```cpp
TArray<FString> ValidateSources(const FCamSimConfig& Cfg)
{
	TArray<FString> Errors;
	const FCamSimConfig::FCesiumBackendConfig& Cs = Cfg.CesiumBackend;
	if (Cs.Imagery.Source == TEXT("tms") && Cs.Imagery.Url.IsEmpty())
	{
		Errors.Add(TEXT("cesium.imagery.source 'tms' needs cesium.imagery.url (the tilemapresource.xml URL)"));
	}
	if (!Cfg.Scene.bOffline)
	{
		return Errors;
	}

	auto CheckLocal = [&Errors](const TCHAR* Key, const FString& Url)
	{
		FString Path;
		if (!FileUrlToPath(Url, Path))
		{
			Errors.Add(FString::Printf(TEXT("scene.offline: %s '%s' is not a file:/// URL"), Key, *Url));
		}
		else if (!FPaths::FileExists(Path))
		{
			Errors.Add(FString::Printf(TEXT("scene.offline: %s file '%s' does not exist"), Key, *Path));
		}
	};

	if (Cs.Terrain.Source == TEXT("url"))
	{
		CheckLocal(TEXT("cesium.terrain.url"), Cs.Terrain.Url);
	}
	else if (Cs.Terrain.Source != TEXT("flat"))
	{
		Errors.Add(FString::Printf(
			TEXT("scene.offline: cesium.terrain.source '%s' needs the network (use url with a file:/// URL, or flat)"),
			*Cs.Terrain.Source));
	}

	if (Cs.Imagery.Source == TEXT("tms"))
	{
		if (!Cs.Imagery.Url.IsEmpty())
		{
			CheckLocal(TEXT("cesium.imagery.url"), Cs.Imagery.Url);
		}
	}
	else if (Cs.Imagery.Source != TEXT("none"))
	{
		Errors.Add(FString::Printf(
			TEXT("scene.offline: cesium.imagery.source '%s' needs the network (use tms with a file:/// URL, or none)"),
			*Cs.Imagery.Source));
	}
	return Errors;
}
```

In `FCamSimConfig::Validate()`, next to the `Errors.Append(Scene.ResolveErrors);` added in Task 2:

```cpp
	Errors.Append(CamSimScene::ValidateSources(*this));
```

- [ ] **Step 4: Exit on config errors when offline**

In `UCamSimSubsystem::Initialize`, replace the `if (ValidationErrors.Num() > 0) { ... }` block with:

```cpp
		if (ValidationErrors.Num() > 0)
		{
			UE_LOG(LogCamSim, Error, TEXT("UCamSimSubsystem: %d config validation error(s) — check config"),
				ValidationErrors.Num());
			Config.bLoadedSuccessfully = false;
			if (Config.Scene.bOffline)
			{
				// REALISM R0: an offline run must not fall back to defaults (Cesium ion).
				UE_LOG(LogCamSim, Error, TEXT("UCamSimSubsystem: scene.offline is set — exiting on config errors"));
				GLog->Flush();
				FPlatformMisc::RequestExitWithStatus(true, 1);
			}
		}
		if (!Config.Scene.Dir.IsEmpty() && Config.Scene.ResolveErrors.IsEmpty())
		{
			UE_LOG(LogCamSim, Log, TEXT("UCamSimSubsystem: scene package '%s' at %s (offline %s); terrain %s, imagery %s"),
				*Config.Scene.PackageName, *Config.Scene.Dir, Config.Scene.bOffline ? TEXT("on") : TEXT("off"),
				*Config.CesiumBackend.Terrain.Url, *Config.CesiumBackend.Imagery.Url);
		}
```

- [ ] **Step 5: Build and run the tests**

Run: `scripts/run.sh --build-only`, then the test command with filter `CamSim.Scene`, then the full `CamSim` suite once.
Expected: all `CamSim.Scene.*` pass; no regressions.

- [ ] **Step 6: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Config/ScenePackage.h \
        unreal_project/CamSimTest/Source/CamSimTest/Config/ScenePackage.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/Tests/ScenePackageTest.cpp
git commit -m "feat(scene): scene.offline requires local sources and exits on config errors"
```

---

### Task 4: TMS overlay and Cesium setup before `BeginPlay`

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Geospatial/CamSimGeospatialProvider.cpp` (`ApplyCesiumBackendConfig`)
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Geospatial/CesiumWorldSetup.h`
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Geospatial/CesiumWorldSetup.cpp`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.h` / `.cpp` (add `PrepareCesiumWorld`)
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/GameMode/CamSimGameMode.h` / `.cpp` (add `StartPlay`)
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp` (~lines 104–123)
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Geospatial/CesiumTuning.h` (comment "Called from ACamSimCamera::BeginPlay" → "Called from CamSim::Geospatial::SetUpCesiumWorld")
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Tests/CesiumBackendConfigTest.cpp`

**Interfaces:**
- Consumes: `Imagery.Url` (Task 1).
- Produces:
  - `UCesiumIonServer* CamSim::Geospatial::SetUpCesiumWorld(UWorld* World, const FCamSimConfig& Cfg)`
  - `class CamSim::Geospatial::FCesiumWorldSetupLatch { bool TryBegin(UWorld* World); }`
  - `void UCamSimSubsystem::PrepareCesiumWorld(UWorld* World)`
  - `void ACamSimGameMode::StartPlay() override`

- [ ] **Step 1: Write the failing tests** (append to `CesiumBackendConfigTest.cpp`)

Add includes at the top of the file:

```cpp
#include "Geospatial/CesiumWorldSetup.h"
#include "Cesium3DTileset.h"
#include "CesiumGeoreference.h"
#include "CesiumTileMapServiceRasterOverlay.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
```

Tests:

```cpp
// REALISM R0: configured before BeginPlay, the level's other tilesets are gone and the
// terrain has its final source before it ever loads. Tilesets are spawned FromEllipsoid so
// the test makes no request and touches no file.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCesiumBackendSetUpBeforeBeginPlayTest,
	"CamSim.CesiumBackend.SetUpBeforeBeginPlay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCesiumBackendSetUpBeforeBeginPlayTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	};
	World->SpawnActor<ACesiumGeoreference>();

	auto SpawnEllipsoidTileset = [World]()
	{
		ACesium3DTileset* T = World->SpawnActorDeferred<ACesium3DTileset>(
			ACesium3DTileset::StaticClass(), FTransform::Identity);
		T->SetTilesetSource(ETilesetSource::FromEllipsoid);
		T->FinishSpawning(FTransform::Identity);
		return T;
	};
	ACesium3DTileset* Terrain = SpawnEllipsoidTileset();
	SpawnEllipsoidTileset();   // stands in for Main.umap's OSM Buildings

	FCamSimConfig Cfg;
	Cfg.Render.OriginShiftDistanceM = 20000.0;
	Cfg.CesiumBackend.Terrain.Source = TEXT("url");
	Cfg.CesiumBackend.Terrain.Url    = TEXT("file:///nonexistent/terrain/layer.json");
	Cfg.CesiumBackend.Imagery.Source = TEXT("tms");
	Cfg.CesiumBackend.Imagery.Url    = TEXT("file:///nonexistent/imagery/tilemapresource.xml");

	TestFalse(TEXT("world has not begun play"), World->HasBegunPlay());
	CamSim::Geospatial::SetUpCesiumWorld(World, Cfg);

	TArray<ACesium3DTileset*> Left;
	for (TActorIterator<ACesium3DTileset> It(World); It; ++It) { Left.Add(*It); }
	TestEqual(TEXT("one tileset left"), Left.Num(), 1);
	TestTrue(TEXT("the first one is kept"), Left.Num() == 1 && Left[0] == Terrain);
	TestFalse(TEXT("terrain has not begun play"), Terrain->HasActorBegunPlay());
	TestTrue(TEXT("terrain source is FromUrl"), Terrain->GetTilesetSource() == ETilesetSource::FromUrl);
	TestEqual(TEXT("terrain url"), Terrain->GetUrl(), Cfg.CesiumBackend.Terrain.Url);
	TestTrue(TEXT("movable for origin shift"), Terrain->GetRootComponent()->Mobility == EComponentMobility::Movable);

	TArray<UCesiumTileMapServiceRasterOverlay*> Overlays;
	Terrain->GetComponents<UCesiumTileMapServiceRasterOverlay>(Overlays);
	TestEqual(TEXT("one TMS overlay"), Overlays.Num(), 1);
	TestTrue(TEXT("TMS url"), Overlays.Num() == 1 && Overlays[0]->Url == Cfg.CesiumBackend.Imagery.Url);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCesiumBackendSetupLatchTest,
	"CamSim.CesiumBackend.SetupLatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCesiumBackendSetupLatchTest::RunTest(const FString& Parameters)
{
	UWorld* A = UWorld::CreateWorld(EWorldType::Game, false);
	UWorld* B = UWorld::CreateWorld(EWorldType::Game, false);
	ON_SCOPE_EXIT { A->DestroyWorld(false); B->DestroyWorld(false); };

	CamSim::Geospatial::FCesiumWorldSetupLatch Latch;
	TestFalse(TEXT("null world"), Latch.TryBegin(nullptr));
	TestTrue(TEXT("first call for A"), Latch.TryBegin(A));
	TestFalse(TEXT("second call for A (game mode, then camera)"), Latch.TryBegin(A));
	TestTrue(TEXT("a new world runs again"), Latch.TryBegin(B));
	return true;
}
```

- [ ] **Step 2: Build to verify it fails**

Run: `scripts/run.sh --build-only`
Expected: compile error, `'Geospatial/CesiumWorldSetup.h' file not found`.

- [ ] **Step 3: TMS overlay and the refresh guard in `ApplyCesiumBackendConfig`**

In `CamSimGeospatialProvider.cpp` add `#include "CesiumTileMapServiceRasterOverlay.h"` after the WMS include. Replace

```cpp
		if (TerrainSrc != TEXT("flat"))
		{
			Tileset->RefreshTileset();
		}
```

with

```cpp
		// Before BeginPlay (ACamSimGameMode::StartPlay) nothing has loaded yet and the first
		// load uses the new source; a refresh would only load it twice.
		if (TerrainSrc != TEXT("flat") && Tileset->HasActorBegunPlay())
		{
			Tileset->RefreshTileset();
		}
```

and update the comment above it to `// Refresh after terrain property changes once the tileset has begun play (it may have loaded).`

Insert after the `wms` branch (before `else if (ImagerySrc != TEXT("none"))`):

```cpp
		else if (ImagerySrc == TEXT("tms"))
		{
			// Url names tilemapresource.xml itself; for file:// a directory URL fails.
			UCesiumTileMapServiceRasterOverlay* O =
				NewObject<UCesiumTileMapServiceRasterOverlay>(Tileset, TEXT("CamSimImagery"));
			O->Url = Config.Imagery.Url;
			O->SetMaximumScreenSpaceError(Config.Imagery.MaximumScreenSpaceError);
			if (Config.Imagery.MaximumTextureSize > 0)
			{
				O->SetMaximumTextureSize(Config.Imagery.MaximumTextureSize);
			}
			O->SetMaximumSimultaneousTileLoads(Config.Imagery.MaximumSimultaneousTileLoads);
			O->RegisterComponent();
			O->Activate(false);
		}
```

- [ ] **Step 4: `CesiumWorldSetup.h`**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtrTemplates.h"

class UCesiumIonServer;
class UWorld;
struct FCamSimConfig;

namespace CamSim::Geospatial
{
	/**
	 * Origin-shift mobility, tileset tuning and the Cesium backend (one terrain tileset,
	 * every other tileset destroyed), in that order. Call before the world's actors begin
	 * play: ACesium3DTileset::BeginPlay calls LoadTileset, so Main.umap's ion actors would
	 * otherwise request api.cesium.com before CamSim's config reaches them (REALISM R0).
	 * Returns the ion server override, or nullptr (see ApplyCesiumBackendConfig).
	 */
	UCesiumIonServer* SetUpCesiumWorld(UWorld* World, const FCamSimConfig& Cfg);

	/** Runs SetUpCesiumWorld once per world: the game mode's StartPlay and the camera's BeginPlay both ask. */
	class FCesiumWorldSetupLatch
	{
	public:
		/** True the first time it sees World; false for nullptr or a repeat. */
		bool TryBegin(UWorld* World)
		{
			if (World == nullptr || Prepared.Get() == World)
			{
				return false;
			}
			Prepared = World;
			return true;
		}

	private:
		TWeakObjectPtr<UWorld> Prepared;
	};
}
```

- [ ] **Step 5: `CesiumWorldSetup.cpp`**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Geospatial/CesiumWorldSetup.h"
#include "Config/CamSimConfig.h"
#include "Geospatial/CamSimGeospatialProvider.h"
#include "Geospatial/CesiumTuning.h"

#include "Cesium3DTileset.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace CamSim::Geospatial
{
UCesiumIonServer* SetUpCesiumWorld(UWorld* World, const FCamSimConfig& Cfg)
{
	if (!World)
	{
		return nullptr;
	}
	if (Cfg.Render.OriginShiftDistanceM > 0.0)
	{
		// ChangeCesiumGeoreference moves tilesets, so they must be Movable.
		for (TActorIterator<ACesium3DTileset> It(World); It; ++It)
		{
			if (USceneComponent* TilesetRoot = It->GetRootComponent())
			{
				TilesetRoot->SetMobility(EComponentMobility::Movable);
			}
		}
	}
	ApplyCesiumTilesetTuning(World, Cfg);
	return ::ApplyCesiumBackendConfig(World, Cfg.CesiumBackend);   // declared at global scope
}
} // namespace CamSim::Geospatial
```

- [ ] **Step 6: Subsystem, game mode, camera**

`CamSimSubsystem.h`: add `#include "Geospatial/CesiumWorldSetup.h"`; next to `StoreCesiumIonServer`:

```cpp
	/** REALISM R0: Cesium setup for World, once (ACamSimGameMode::StartPlay, else ACamSimCamera::BeginPlay). */
	void PrepareCesiumWorld(UWorld* World);
```

and in the private section: `CamSim::Geospatial::FCesiumWorldSetupLatch CesiumWorldSetup_;`

`CamSimSubsystem.cpp`:

```cpp
void UCamSimSubsystem::PrepareCesiumWorld(UWorld* World)
{
	if (!CesiumWorldSetup_.TryBegin(World))
	{
		return;
	}
	StoreCesiumIonServer(CamSim::Geospatial::SetUpCesiumWorld(World, Config));
	RefreshCachedTilesets();   // the destroyed tilesets must not stay in the cache
}
```

`CamSimGameMode.h`: add `virtual void StartPlay() override;` under `BeginPlay`.

`CamSimGameMode.cpp`: add `#include "Subsystem/CamSimSubsystem.h"` and `#include "Engine/GameInstance.h"`, then:

```cpp
void ACamSimGameMode::StartPlay()
{
	// Super::StartPlay dispatches BeginPlay to every actor, and ACesium3DTileset::BeginPlay
	// loads its tileset: configure (or destroy) the level's tilesets first, so Main.umap's
	// ion actors never send a request (REALISM R0 offline).
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		if (UCamSimSubsystem* Subsystem = GameInstance->GetSubsystem<UCamSimSubsystem>())
		{
			Subsystem->PrepareCesiumWorld(GetWorld());
		}
	}
	Super::StartPlay();
}
```

`CamSimCamera.cpp` (BeginPlay): delete the tileset mobility loop inside `if (Cfg.Render.OriginShiftDistanceM > 0.0)` (keep `OriginShift->SetDistance`, `SetMode` and the log line), and replace

```cpp
	// Tileset streaming parameters and the Cesium backend (ion server, terrain,
	// imagery).
	CamSim::Geospatial::ApplyCesiumTilesetTuning(GetWorld(), Cfg);
	Subsystem->StoreCesiumIonServer(ApplyCesiumBackendConfig(GetWorld(), Cfg.CesiumBackend));
```

with

```cpp
	// Tileset tuning and the Cesium backend. Normally done already by
	// ACamSimGameMode::StartPlay, before any tileset began play; a no-op then.
	Subsystem->PrepareCesiumWorld(GetWorld());
```

Remove includes from `CamSimCamera.cpp` that are now unused (`CesiumTuning.h`, `CamSimGeospatialProvider.h`) only if nothing else in the file uses them (grep first).

- [ ] **Step 7: Build and run the tests**

Run: `scripts/run.sh --build-only`, then the test command with filter `CamSim.CesiumBackend`, then the full `CamSim` suite.
Expected: `SetUpBeforeBeginPlay` and `SetupLatch` pass along with the existing `CesiumBackend.*`; no regressions. If spawning an `ACesium3DTileset` under NullRHI crashes or logs an error that fails the test, stop and report (do not convert the test to a GPU test silently).

- [ ] **Step 8: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Geospatial \
        unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.h \
        unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/GameMode \
        unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/Tests/CesiumBackendConfigTest.cpp
git commit -m "feat(cesium): tms imagery; configure tilesets before BeginPlay so level ion actors never load"
```

---

### Task 5: Gate tools and live acceptance (gate 5 offline)

**Files:**
- Modify: `scripts/scene/tools/camsim_session.py` (`env_for`, `camsim`)
- Modify: `scripts/scene/tools/render_check.py` (network-request check)
- Modify: `scripts/scene/tools/README.md` (drop the patch row)
- Delete: `scripts/scene/tools/tms_overlay.patch`, `scripts/scene/spike/tms_overlay_spike.patch`

**Interfaces:**
- Consumes: env vars `CAMSIM_SCENE_DIR`, `CAMSIM_SCENE_OFFLINE` (Tasks 1–3); the Task 4 build.

- [ ] **Step 1: Session environment**

In `camsim_session.py` replace the `env.update(...)` block in `env_for` with:

```python
    if mode == "package":
        env["CAMSIM_SCENE_DIR"] = str(pkg)
```

and in `camsim()`, after `cmd = [...]`:

```python
    if offline:
        env = dict(env, CAMSIM_SCENE_OFFLINE="1")
        cmd = ["sandbox-exec", "-f", str(Path(__file__).with_name("offline.sb")), *cmd]
```

(replacing the existing two-line `if offline:` block).

- [ ] **Step 2: Network-request check in `render_check.py`**

Add near the top (after imports):

```python
LOG = Path.home() / "Library" / "Logs" / "CamSimTest" / "CamSimTest.log"   # macOS editor log (run.sh)


def network_lines(log: Path) -> list[str]:
    """Log lines that show a request to Cesium ion / any http(s) endpoint (an offline run must have none)."""
    if not log.exists():
        return []
    text = log.read_text(errors="replace").splitlines()
    return [ln for ln in text if "cesium.com" in ln or "https://" in ln or "http://" in ln]
```

In `main()`, after the `with camsim(...)` block and before writing `result.json`:

```python
    res["network_lines"] = network_lines(LOG)[:20]
    if a.offline and res["network_lines"]:
        print("FAIL: offline run logged network use:", *res["network_lines"], sep="\n  ")
        (a.out / "result.json").write_text(json.dumps(res, indent=1))
        return 1
```

Before relying on the filter, check it against the 2026-10-09 gate-5 run's log if still present (the blocked `api.cesium.com` requests must match); if `http://` matches unrelated lines in a clean run (e.g. the health server's own URL), narrow the filter to the ones that are real outbound requests and note which in the commit message.

- [ ] **Step 3: Remove the spike patches**

```bash
git rm scripts/scene/tools/tms_overlay.patch scripts/scene/spike/tms_overlay_spike.patch
```

Delete the `tms_overlay.patch` row from `scripts/scene/tools/README.md`; grep `scripts/scene/spike/README.md` and `docs/` for `tms_overlay` and replace each mention with "`cesium.imagery.source: tms` (landed with the offline profile, 2026-10-09)".

- [ ] **Step 4: Acceptance — offline render**

Requires the Pendleton package at `.cache/scene-packages/pendleton` and a clean working tree build (`scripts/run.sh --build-only`).

```bash
uv run --project scripts/scene python scripts/scene/tools/render_check.py \
  .cache/gate5_offline package .cache/scene-packages/pendleton --offline
grep -n "scene package\|ApplyCesiumBackendConfig\|Loading tileset from URL" ~/Library/Logs/CamSimTest/CamSimTest.log | head
```

Expected: exit 0; `result.json` `network_lines` empty; every shot `terrain_ready: true`; the log shows `scene package 'pendleton'`, two "removing extra tileset" lines, and exactly one `Loading tileset from URL file:///…/terrain/layer.json` (one load, not two).

Then registration against the existing CWT shots and heights:

```bash
uv run --project scripts/scene python scripts/scene/tools/registration.py --help   # find the CWT shot dir argument
uv run --project scripts/scene python scripts/scene/tools/hot_check.py --help
```

Run both as `docs/scene-packages.md` "Measured" gate 5 did (same CWT reference shots). Expected: registration ≤ 1 px on every shot (2026-10-09: ≤ 0.16 px); heights 7/7 pass.

- [ ] **Step 5: Acceptance — fail fast and online unchanged**

```bash
CAMSIM_SCENE_OFFLINE=1 scripts/run.sh --headless --local; echo "exit $?"
grep -n "scene.offline" ~/Library/Logs/CamSimTest/CamSimTest.log | head
```

Expected: CamSim exits within seconds with a non-zero status (if `run.sh` masks the code, read it from the log's exit line instead); the log lists `cesium.terrain.source 'cesium_ion' needs the network` and the same for imagery, then `exiting on config errors`.

```bash
uv run --project scripts/scene python scripts/scene/tools/render_check.py .cache/gate5_online cwt
```

Expected: exit 0, every shot rendered from ion (online default unchanged by the StartPlay move).

- [ ] **Step 6: Commit**

```bash
git add scripts/scene/tools scripts/scene/spike
git commit -m "feat(scene): gate tools use scene.dir / scene.offline; offline render checks for network use"
```

---

### Task 6: Documentation and roadmap status

**Files:**
- Modify: `docs/configuration.md`, `deploy/camsim_config.yaml`, `docs/scene-packages.md`, `REALISM.md`, `ROADMAP.md`, `CLAUDE.md`, `FOR_LINUX.md`

- [ ] **Step 1: `deploy/camsim_config.yaml`**

Under `cesium.imagery`: change the `source` comment to `# cesium_ion | wms | tms | none` and add after `wms_tile_height`:

```yaml
    url: ""               # tms: URL of tilemapresource.xml itself, e.g. file:///data/pkg/imagery/tilemapresource.xml (CAMSIM_CESIUM_IMAGERY_URL)
```

After the `cesium:` block:

```yaml
# --- Scene package (REALISM R0, docs/scene-packages.md) ---
scene:
  dir: ""          # CAMSIM_SCENE_DIR — absolute path to a package; its terrain/imagery/landcover replace cesium.terrain, cesium.imagery, thermal.land_cover.dir
  offline: false   # CAMSIM_SCENE_OFFLINE — no network: terrain url/flat and imagery tms/none from file:///, and exit on any config error
```

- [ ] **Step 2: `docs/configuration.md`**

In the Cesium section's env table add `CAMSIM_CESIUM_IMAGERY_URL` (default empty, "TMS: URL of `tilemapresource.xml` (`file:///…` for local pyramids); used when `IMAGERY_SOURCE=tms`") and add `tms` to the `CAMSIM_CESIUM_IMAGERY_SOURCE` values. Add a "Scene package" section with `scene.dir` / `CAMSIM_SCENE_DIR` and `scene.offline` / `CAMSIM_SCENE_OFFLINE`: what each layer overrides (table from the spec §3), the override warning, the offline rules (spec §4), and that offline config errors exit with status 1. Note that Cesium is configured in `ACamSimGameMode::StartPlay`, before any actor's `BeginPlay`.

- [ ] **Step 3: `docs/scene-packages.md`**

Replace the "Set `CAMSIM_CESIUM_ION_TOKEN=""` … no offline config profile yet (both R1)" bullets with a "Running CamSim on a package" subsection:

```bash
CAMSIM_SCENE_DIR=/abs/path/to/pendleton CAMSIM_SCENE_OFFLINE=1 scripts/run.sh --headless
```

and a sentence that `Main.umap`'s ion actors are destroyed (or reconfigured) before they begin play, so an offline run sends no request. Update the gate 5 row of "Measured": replace "Pass except the offline log … closes with the editor follow-up" with "**Pass** (offline log closed 2026-10-09 by the offline profile: no network lines; registration ≤ X px, heights 7/7)" using Task 5's numbers.

- [ ] **Step 4: `REALISM.md`, `ROADMAP.md`, `FOR_LINUX.md`**

- REALISM.md R0: mark "Offline config profile" done (one line: `scene.dir` / `scene.offline`, `imagery.source: tms`, Cesium set up before `BeginPlay`); R1's "Offline: Main.umap's own ion actors …" bullet: no longer needed for offline runs; removing them stays optional cleanup (editor). R1's `imagery.source: tms` bullet: landed with R0.
- ROADMAP.md Realism track: gate 5 passed in full on 2026-10-09; the "Human follow-up (editor)" paragraph becomes optional cleanup ("CamSim now configures or destroys them before they begin play, so offline runs don't need it").
- FOR_LINUX.md "Optional: offline render on Linux": drop "It can't pass anywhere yet …"; say the Mac offline run passes and Linux needs an egress-blocking equivalent of `offline.sb` (e.g. `docker run --network none`).

- [ ] **Step 5: `CLAUDE.md`**

- "One terrain tileset" gotcha: add "Cesium is configured in `ACamSimGameMode::StartPlay` (`PrepareCesiumWorld`), before any actor's `BeginPlay`, because `ACesium3DTileset::BeginPlay` loads the tileset; anything that spawns or edits tilesets must run there too."
- Add a gotcha: "**Scene packages at runtime**: `scene.dir` (absolute) replaces terrain/imagery/land cover with the package's `file:///` layers (`Config/ScenePackage.h`); `scene.offline: true` rejects any network source and exits on config errors. TMS `url` names `tilemapresource.xml` itself."
- Update the test counts in "Architecture" and "Testing" (count with `grep -rho "IMPLEMENT_[A-Z_]*AUTOMATION_TEST" unreal_project/CamSimTest/Source/CamSimTest/Tests | wc -l` and `ls … /Tests/*.cpp | wc -l`).

- [ ] **Step 6: Commit**

```bash
git add docs deploy REALISM.md ROADMAP.md CLAUDE.md FOR_LINUX.md
git commit -m "docs: offline scene profile; R0 gate 5 passes in full"
```
